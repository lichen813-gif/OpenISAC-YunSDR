#include "rank8_frame.hpp"
#include "openisac/crc16.hpp"
#include "openisac/ldpc_framing.hpp"
#include "openisac/qam.hpp"

#include <algorithm>
#include <stdexcept>

namespace studio_rank8 {
using namespace openisac;
namespace {
using Complex = std::complex<float>;
constexpr std::size_t fft = 1024, ports = 8;
constexpr std::uint32_t pilot_seed = 0xC057u;
constexpr std::uint8_t version = 2u, rank8_flag = 2u;

std::vector<std::uint8_t> header_bytes(const MiniHeader& head) {
    std::vector<std::uint8_t> bytes{
        static_cast<std::uint8_t>(head.version << 4u | head.flags),
        static_cast<std::uint8_t>(head.payload_len >> 8u),
        static_cast<std::uint8_t>(head.payload_len), head.payload_blocks,
        static_cast<std::uint8_t>(head.sequence >> 8u),
        static_cast<std::uint8_t>(head.sequence)};
    const auto crc = crc16_ccitt_false(bytes);
    bytes.push_back(static_cast<std::uint8_t>(crc >> 8u));
    bytes.push_back(static_cast<std::uint8_t>(crc));
    return bytes;
}

std::size_t blocks_for(std::size_t bytes) { return (bytes+62u)/63u; }

std::size_t index(std::size_t time, std::size_t k, std::size_t port) {
    return (time*fft+k)*ports+port;
}

Complex dmrs_symbol(std::size_t k) {
    const auto label = (k ^ (k >> 3u) ^ (k >> 7u)) & 3u;
    return SquareQAM::modulate(static_cast<unsigned>(label), 2);
}

int centered(std::size_t k) {
    return static_cast<int>(k)-(k >= fft/2 ? static_cast<int>(fft) : 0);
}
}  // namespace

FormalFrameLayout layout(Modulation modulation, unsigned edge) {
    // Reuse the mature Rank-4 grid geometry, including the control and phase
    // references. Version 2 maps eight layers on each payload resource.
    FormalFrameProfile profile;
    profile.guard_left = fft/2-edge;
    profile.guard_right = fft/2-1-edge;
    profile.transmit_rank = 4;
    profile.pilot_spacing = 2;
    profile.bits_per_symbol = modulation_bits(modulation);
    auto shape = build_formal_frame_layout(profile);
    shape.payload_layer_symbols = shape.payload_time_indices.size()*ports;
    const auto per_block = ldpc_encoded_bits/profile.bits_per_symbol;
    shape.ldpc_blocks = shape.payload_layer_symbols/per_block;
    shape.coded_qam_symbols = shape.ldpc_blocks*per_block;
    shape.padding_qam_symbols = shape.payload_layer_symbols-shape.coded_qam_symbols;
    shape.information_bytes = shape.ldpc_blocks*63u;
    shape.user_payload_bytes = shape.information_bytes-2u;
    return shape;
}

void pilots(bool dmrs, std::vector<Complex>& grid, unsigned edge) {
    const auto shape = layout(Modulation::qpsk, edge);
    grid.assign(2*fft*ports, {});
    if (dmrs) {
        for (const auto k : shape.active_fft_indices) {
            const auto tx = 2*(k%4);
            const auto pilot = dmrs_symbol(k);
            grid[index(0, k, tx)] = grid[index(1, k, tx)] = pilot;
            grid[index(0, k, tx+1)] = pilot;
            grid[index(1, k, tx+1)] = -pilot;
        }
        return;
    }
    std::vector<Complex> legacy;
    build_dynamic_pilot_reference_grid(pilot_seed, {4u, Modulation::qpsk}, legacy);
    for (unsigned time = 0; time < 2; ++time) {
        for (std::size_t i = 0; i < shape.pilot_fft_indices.size(); ++i) {
            const auto k = shape.pilot_fft_indices[i];
            grid[index(time, k, i%ports)] = legacy[(time*fft+k)*4+i%4];
        }
        for (const auto k : shape.phase_reference_fft_indices)
            grid[index(time, k, 0)] = 1;
    }
}

void estimate_dmrs(const std::vector<Complex>& received, const std::vector<Complex>& reference,
                   const FormalFrameLayout& shape, std::vector<ChannelNxN>& channels) {
    channels.assign(2*fft, {});
    for (auto& channel : channels)
        channel.streams = ports;
    for (std::size_t tx = 0; tx < ports; ++tx) {
        std::vector<std::uint16_t> tones;
        for (const auto k : shape.active_fft_indices)
            if (k%4 == tx/2)
                tones.push_back(k);
        const float sign = tx%2 == 0 ? 1.0f : -1.0f;
        std::size_t right = 0;
        for (const auto k : shape.active_fft_indices) {
            while (right+1 < tones.size() && centered(tones[right]) < centered(k))
                ++right;
            const auto lo = tones[right ? right-1 : 0], hi = tones[right];
            const auto width = centered(hi)-centered(lo);
            const float alpha = width ? std::clamp(static_cast<float>(centered(k)-centered(lo))/width, 0.0f, 1.0f) : 0;
            for (std::size_t rx = 0; rx < ports; ++rx) {
                auto channel_at = [&](std::size_t bin) {
                    return (received[index(0, bin, rx)]+sign*received[index(1, bin, rx)])/
                           (2.0f*reference[index(0, bin, tx)]);
                };
                const auto value = (1-alpha)*channel_at(lo) + alpha*channel_at(hi);
                channels[k].values[rx*ports+tx] = channels[fft+k].values[rx*ports+tx] = value;
            }
        }
    }
}

MiniHeader header(const std::vector<float>& llrs, bool dmrs) {
    // Half the control RE carry a version-specific marker; the other half
    // carry the 64-bit CRC-protected header twice. Combine in the soft domain.
    const auto marker = marker_qpsk_labels();
    unsigned matches = 0;
    for (std::size_t i = 0; i < marker.size(); ++i) {
        const auto label = (llrs[2*i] < 0 ? 2u : 0u) | (llrs[2*i+1] < 0 ? 1u : 0u);
        matches += (label == (marker[i]^3u));
    }
    if (matches < 48)
        throw std::runtime_error("OpenISAC v2 control marker mismatch");
    std::vector<std::uint8_t> bits(64);
    for (unsigned i = 0; i < 64; ++i)
        bits[i] = llrs[128+i]+llrs[192+i] < 0;
    const auto bytes = pack_msb_bits(bits);
    const MiniHeader head{static_cast<std::uint8_t>(bytes[0] >> 4u),
                          static_cast<std::uint8_t>(bytes[0]&15u),
                          static_cast<std::uint16_t>(bytes[1] << 8u | bytes[2]), bytes[3],
                          static_cast<std::uint16_t>(bytes[4] << 8u | bytes[5])};
    if (!check_crc16_ccitt_false(bytes) || head.version != version ||
        (head.flags & 3u) != (rank8_flag | static_cast<unsigned>(dmrs)) ||
        head.payload_len < 3 || head.payload_blocks != blocks_for(head.payload_len) ||
        head.payload_blocks > layout(static_cast<Modulation>(bits_per_symbol_from_flags(head.flags)/2-1)).ldpc_blocks)
        throw std::runtime_error("OpenISAC v2 mini-header CRC or profile mismatch");
    return head;
}

EncodedDynamicFrame encode(const std::vector<std::uint8_t>& payload, LinkMode mode,
                            std::uint16_t sequence, bool dmrs, const Ldpc5041008& codec) {
    EncodedDynamicFrame frame;
    frame.layout = layout(mode.modulation);
    const auto& shape = frame.layout;
    if (payload.empty() || payload.size() > shape.user_payload_bytes)
        throw std::invalid_argument("version 2 payload does not fit selected modulation");
    const auto bits = modulation_bits(mode.modulation);
    const auto blocks = blocks_for(payload.size()+2u);
    frame.header = {version, static_cast<std::uint8_t>(rank8_flag | static_cast<unsigned>(dmrs) |
                     modulation_flag(bits)), static_cast<std::uint16_t>(payload.size()+2u),
                     static_cast<std::uint8_t>(blocks), sequence};
    auto info = payload;
    const auto crc = crc16_ccitt_false(info);
    info.push_back(static_cast<std::uint8_t>(crc >> 8u));
    info.push_back(static_cast<std::uint8_t>(crc));
    info.resize(blocks*63u);
    const auto packed = unpack_msb_bits(info, info.size()*8u);
    std::vector<std::uint8_t> encoded;
    encoded.reserve(blocks*ldpc_encoded_bits);
    for (std::size_t offset = 0; offset < packed.size(); offset += ldpc_information_bits) {
        const auto block = codec.encode({packed.begin()+offset, packed.begin()+offset+ldpc_information_bits});
        encoded.insert(encoded.end(), block.begin(), block.end());
    }
    scramble_bits(encoded);
    interleave_ldpc_blocks(encoded);
    frame.payload_symbols.assign(shape.payload_layer_symbols, SquareQAM::modulate(0, bits));
    for (std::size_t i = 0; i < encoded.size()/bits; ++i) {
        unsigned label = 0;
        for (unsigned b = 0; b < bits; ++b)
            label = (label << 1u) | encoded[i*bits+b];
        frame.payload_symbols[i] = SquareQAM::modulate(label, bits);
    }
    pilots(false, frame.tx_grid);
    const auto marker = marker_qpsk_labels();
    const auto head = unpack_msb_bits(header_bytes(frame.header), 64);
    for (std::size_t i = 0; i < shape.control_data_positions.size(); ++i) {
        const auto k = shape.data_fft_indices[shape.control_data_positions[i]];
        const auto j = (2*i)%64;
        const auto label = i < 64 ? marker[i]^3u : (head[j] << 1u | head[j+1]);
        frame.tx_grid[index(0, k, 0)] = SquareQAM::modulate(label, 2);
    }
    constexpr float scale = 0.3535533905932738f;
    for (std::size_t i = 0; i < shape.payload_time_indices.size(); ++i) {
        const auto k = shape.data_fft_indices[shape.payload_data_positions[i]];
        for (std::size_t layer = 0; layer < ports; ++layer)
            frame.tx_grid[index(shape.payload_time_indices[i], k, layer)] =
                scale*frame.payload_symbols[i*ports+layer];
    }
    return frame;
}

DecodedDynamicFrame decode_payload(const MiniHeader& control, LinkMode mode, const FormalFrameLayout& shape,
                            const std::vector<Complex>& equalized,
                            const std::vector<float>& variance, const Ldpc5041008& codec) {
    const auto bits = modulation_bits(mode.modulation);
    if (control.payload_len < 3 || control.payload_blocks != blocks_for(control.payload_len) ||
        control.payload_blocks > shape.ldpc_blocks)
        throw std::runtime_error("OpenISAC payload block count exceeds the selected profile capacity");
    const auto coded = control.payload_blocks*ldpc_encoded_bits;
    std::vector<float> llrs(coded);
    for (std::size_t s = 0; s < coded/bits; ++s) {
        const auto llr = SquareQAM::max_log_llrs(equalized[s], variance[s], bits);
        std::copy_n(llr.begin(), bits, llrs.begin()+s*bits);
    }
    deinterleave_ldpc_blocks(llrs);
    soft_descramble(llrs);
    std::vector<std::uint8_t> info;
    info.reserve(control.payload_blocks*ldpc_information_bits);
    DecodedDynamicFrame result;
    for (std::size_t offset = 0; offset < coded; offset += ldpc_encoded_bits) {
        const auto block = codec.decode_normalized_min_sum(
            {llrs.begin()+offset, llrs.begin()+offset+ldpc_encoded_bits}, 10);
        result.syndrome_failures += block.syndrome_weight != 0u;
        info.insert(info.end(), block.information_bits.begin(), block.information_bits.end());
    }
    auto bytes = pack_msb_bits(info);
    bytes.resize(control.payload_len);
    result.crc_ok = result.syndrome_failures == 0 && check_crc16_ccitt_false(bytes);
    if (result.crc_ok)
        result.user_payload.assign(bytes.begin(), bytes.end()-2);
    return result;
}

}  // namespace studio_rank8
