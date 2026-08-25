#include "openisac/dynamic_frame.hpp"

#include "openisac/crc16.hpp"
#include "openisac/ldpc_frame_decoder.hpp"
#include "openisac/ldpc_framing.hpp"
#include "openisac/mimo_nxn.hpp"
#include "openisac/qam.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

namespace openisac {
namespace {

FormalFrameProfile profile_for_mode(LinkMode mode) {
    FormalFrameProfile profile;
    profile.bits_per_symbol = modulation_bits(mode.modulation);
    profile.transmit_rank = mode.rank;
    profile.scheme = mode.scheme;
    if (physical_transmit_ports(mode) == 4u) {
        profile.pilot_spacing = 2u;
    }
    return profile;
}

const FormalFrameLayout& cached_layout_for_mode(LinkMode mode) {
    const unsigned bits = modulation_bits(mode.modulation);
    const unsigned physical_ports = physical_transmit_ports(mode);
    const bool spatial =
        mode.scheme == TransmissionScheme::spatial_multiplexing &&
        ((physical_ports == 1u && mode.rank == 1u) ||
         (physical_ports == 2u && (mode.rank == 1u || mode.rank == 2u)) ||
         (physical_ports == 4u && (mode.rank == 2u || mode.rank == 4u)));
    const bool stbc =
        mode.scheme == TransmissionScheme::alamouti_stbc &&
        physical_ports == 2u && mode.rank == 1u;
    if ((!spatial && !stbc) ||
        (bits != 2u && bits != 4u && bits != 6u && bits != 8u)) {
        throw std::invalid_argument("unsupported cached PHY mode/MCS layout");
    }
    static const std::array<FormalFrameLayout, 20> layouts = [] {
        std::array<FormalFrameLayout, 20> result;
        constexpr std::array<Modulation, 4> modulations{{
            Modulation::qpsk, Modulation::qam16,
            Modulation::qam64, Modulation::qam256}};
        constexpr std::array<unsigned, 3> ranks{{1u, 2u, 4u}};
        for (std::size_t rank_index = 0u; rank_index < ranks.size(); ++rank_index) {
            for (std::size_t modulation = 0u;
                 modulation < modulations.size(); ++modulation) {
                result[rank_index * modulations.size() + modulation] =
                    build_formal_frame_layout(
                        profile_for_mode({ranks[rank_index], modulations[modulation]}));
            }
        }
        for (std::size_t modulation = 0u;
             modulation < modulations.size(); ++modulation) {
            result[12u + modulation] = build_formal_frame_layout(profile_for_mode({
                1u, modulations[modulation], TransmissionScheme::alamouti_stbc}));
        }
        for (std::size_t modulation = 0u;
             modulation < modulations.size(); ++modulation) {
            result[16u + modulation] = build_formal_frame_layout(profile_for_mode({
                2u, modulations[modulation],
                TransmissionScheme::spatial_multiplexing, 4u}));
        }
        return result;
    }();
    const std::size_t modulation_index = bits / 2u - 1u;
    if (stbc) {
        return layouts[12u + modulation_index];
    }
    if (physical_ports == 4u && mode.rank == 2u) {
        return layouts[16u + modulation_index];
    }
    const std::size_t rank_index = mode.rank == 1u ? 0u :
        (mode.rank == 2u ? 1u : 2u);
    return layouts[rank_index * 4u + modulation_index];
}

float pilot_sign(
    std::uint32_t seed,
    int centered_subcarrier,
    unsigned slot) noexcept {
    std::uint64_t mixed = static_cast<std::uint64_t>(seed) ^
        (static_cast<std::uint64_t>(static_cast<std::int64_t>(centered_subcarrier)) *
         0x85EBCA77ull) ^
        (static_cast<std::uint64_t>(slot) * 0xC2B2AE3Dull);
    mixed ^= mixed >> 16u;
    mixed *= 0x7FEB352Dull;
    mixed ^= mixed >> 15u;
    return (mixed & 1u) == 0u ? 1.0f : -1.0f;
}

std::vector<std::uint8_t> append_crc(const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> result = payload;
    const std::uint16_t crc = crc16_ccitt_false(payload);
    result.push_back(static_cast<std::uint8_t>(crc >> 8u));
    result.push_back(static_cast<std::uint8_t>(crc & 0xFFu));
    return result;
}

Modulation modulation_from_bits(unsigned bits) {
    switch (bits) {
        case 2u: return Modulation::qpsk;
        case 4u: return Modulation::qam16;
        case 6u: return Modulation::qam64;
        case 8u: return Modulation::qam256;
        default: throw std::invalid_argument("unsupported modulation order");
    }
}

}  // namespace

EncodedDynamicFrame encode_dynamic_frame(
    const std::vector<std::uint8_t>& user_payload,
    LinkMode mode,
    std::uint16_t sequence,
    const Ldpc5041008& codec,
    std::uint32_t pilot_seed) {
    EncodedDynamicFrame result;
    result.profile = profile_for_mode(mode);
    result.layout = cached_layout_for_mode(mode);
    if (user_payload.size() + 2u > result.layout.information_bytes) {
        throw std::invalid_argument("user payload exceeds selected Rank/MCS capacity");
    }
    result.information_bytes = append_crc(user_payload);
    const std::size_t blocks = (result.information_bytes.size() + 62u) / 63u;
    if (blocks == 0u || blocks > result.layout.ldpc_blocks) {
        throw std::invalid_argument("dynamic frame requires one or more valid LDPC blocks");
    }
    result.header = {
        1u,
        static_cast<std::uint8_t>(
            modulation_flag(result.profile.bits_per_symbol) |
            transmission_mode_flag(mode)),
        static_cast<std::uint16_t>(result.information_bytes.size()),
        static_cast<std::uint8_t>(blocks),
        sequence,
    };
    result.control_labels = control_qpsk_labels(result.header);

    std::vector<std::uint8_t> padded_information(blocks * 63u, 0u);
    std::copy(
        result.information_bytes.begin(), result.information_bytes.end(),
        padded_information.begin());
    std::vector<std::uint8_t> coded_bits;
    coded_bits.reserve(blocks * ldpc_encoded_bits);
    for (std::size_t block = 0u; block < blocks; ++block) {
        const auto begin = padded_information.begin() +
            static_cast<std::ptrdiff_t>(block * 63u);
        const std::vector<std::uint8_t> block_bytes(begin, begin + 63u);
        const auto information_bits = unpack_msb_bits(block_bytes, ldpc_information_bits);
        const auto codeword = codec.encode(information_bits);
        coded_bits.insert(coded_bits.end(), codeword.begin(), codeword.end());
    }
    scramble_bits(coded_bits);
    interleave_ldpc_blocks(coded_bits);
    result.transmitted_bits = coded_bits;

    result.payload_labels.assign(result.layout.payload_layer_symbols, 0u);
    const unsigned bits = result.profile.bits_per_symbol;
    for (std::size_t symbol = 0u; symbol < coded_bits.size() / bits; ++symbol) {
        unsigned label = 0u;
        for (unsigned bit = 0u; bit < bits; ++bit) {
            label = (label << 1u) | coded_bits[symbol * bits + bit];
        }
        result.payload_labels[symbol] = static_cast<std::uint8_t>(label);
    }
    result.payload_symbols.resize(result.payload_labels.size());
    for (std::size_t index = 0u; index < result.payload_labels.size(); ++index) {
        result.payload_symbols[index] =
            SquareQAM::modulate(result.payload_labels[index], bits);
    }

    const std::size_t physical_ports = physical_transmit_ports(mode);
    result.physical_ports = physical_ports;
    result.tx_grid.assign(2u * result.profile.fft_size * physical_ports, {});
    if (mode.scheme == TransmissionScheme::alamouti_stbc) {
        const std::size_t pairs = result.layout.payload_time_indices.size() / 2u;
        if (pairs * 2u != result.layout.payload_time_indices.size()) {
            throw std::runtime_error("STBC payload layout must contain two equal time slots");
        }
        constexpr float stbc_scale = 0.70710678118654752440f;
        for (std::size_t pair = 0u; pair < pairs; ++pair) {
            const std::size_t second = pair + pairs;
            if (result.layout.payload_time_indices[pair] != 0u ||
                result.layout.payload_time_indices[second] != 1u ||
                result.layout.payload_data_positions[pair] !=
                    result.layout.payload_data_positions[second]) {
                throw std::runtime_error("STBC payload positions are not time-paired");
            }
            const std::size_t data = result.layout.payload_data_positions[pair];
            const std::size_t fft = result.layout.data_fft_indices[data];
            const auto first = result.payload_symbols[pair];
            const auto second_symbol = result.payload_symbols[second];
            result.tx_grid[fft * physical_ports] = first * stbc_scale;
            result.tx_grid[fft * physical_ports + 1u] =
                second_symbol * stbc_scale;
            result.tx_grid[(result.profile.fft_size + fft) * physical_ports] =
                -std::conj(second_symbol) * stbc_scale;
            result.tx_grid[(result.profile.fft_size + fft) * physical_ports + 1u] =
                std::conj(first) * stbc_scale;
        }
    } else if (physical_ports == 4u && mode.rank == 2u) {
        const float payload_scale = 0.70710678118654752440f;
        for (std::size_t payload = 0u;
             payload < result.layout.payload_time_indices.size(); ++payload) {
            const std::size_t time = result.layout.payload_time_indices[payload];
            const std::size_t data = result.layout.payload_data_positions[payload];
            const std::size_t fft = result.layout.data_fft_indices[data];
            for (std::size_t tx = 0u; tx < physical_ports; ++tx) {
                for (std::size_t layer = 0u; layer < mode.rank; ++layer) {
                    result.tx_grid[
                        (time * result.profile.fft_size + fft) * physical_ports + tx] +=
                        fixed_dft_precoder_4x2(tx, layer) *
                        result.payload_symbols[payload * mode.rank + layer] *
                        payload_scale;
                }
            }
        }
    } else {
        const float payload_scale =
            1.0f / std::sqrt(static_cast<float>(mode.rank));
        for (std::size_t payload = 0u;
             payload < result.layout.payload_time_indices.size(); ++payload) {
            const std::size_t time = result.layout.payload_time_indices[payload];
            const std::size_t data = result.layout.payload_data_positions[payload];
            const std::size_t fft = result.layout.data_fft_indices[data];
            for (std::size_t layer = 0u; layer < mode.rank; ++layer) {
                result.tx_grid[
                    (time * result.profile.fft_size + fft) * physical_ports + layer] =
                    result.payload_symbols[payload * mode.rank + layer] *
                    payload_scale;
            }
        }
    }
    for (std::size_t control = 0u;
         control < result.layout.control_data_positions.size(); ++control) {
        const std::size_t data = result.layout.control_data_positions[control];
        const std::size_t fft = result.layout.data_fft_indices[data];
        result.tx_grid[fft * physical_ports] =
            SquareQAM::modulate(result.control_labels[control], 2u);
    }
    for (std::size_t pilot = 0u;
         pilot < result.layout.pilot_fft_indices.size(); ++pilot) {
        const std::size_t fft = result.layout.pilot_fft_indices[pilot];
        const int centered = fft < result.profile.fft_size / 2u
            ? static_cast<int>(fft)
            : static_cast<int>(fft) - static_cast<int>(result.profile.fft_size);
        const std::size_t tx = pilot % physical_ports;
        for (unsigned time = 0u; time < 2u; ++time) {
            result.tx_grid[(time * result.profile.fft_size + fft) * physical_ports + tx] =
                {pilot_sign(pilot_seed, centered, time), 0.0f};
        }
    }
    for (const std::size_t fft : result.layout.phase_reference_fft_indices) {
        for (unsigned time = 0u; time < 2u; ++time) {
            result.tx_grid[(time * result.profile.fft_size + fft) * physical_ports] =
                {1.0f, 0.0f};
        }
    }
    return result;
}

void build_dynamic_pilot_reference_grid(
    std::uint32_t pilot_seed,
    std::vector<std::complex<float>>& reference_grid) {
    build_dynamic_pilot_reference_grid(
        pilot_seed, {2u, Modulation::qam64}, reference_grid);
}

void build_dynamic_pilot_reference_grid(
    std::uint32_t pilot_seed,
    LinkMode mode,
    std::vector<std::complex<float>>& reference_grid) {
    const FormalFrameProfile profile = profile_for_mode(mode);
    const auto layout = build_formal_frame_layout(profile);
    const std::size_t physical_ports = physical_transmit_ports(mode);
    constexpr unsigned data_symbols = 2u;
    reference_grid.assign(
        static_cast<std::size_t>(data_symbols) * profile.fft_size *
            physical_ports,
        {});
    for (std::size_t pilot = 0u;
         pilot < layout.pilot_fft_indices.size(); ++pilot) {
        const std::size_t fft = layout.pilot_fft_indices[pilot];
        const int centered = fft < profile.fft_size / 2u
            ? static_cast<int>(fft)
            : static_cast<int>(fft) - static_cast<int>(profile.fft_size);
        const std::size_t tx = pilot % physical_ports;
        for (unsigned time = 0u; time < data_symbols; ++time) {
            reference_grid[
                (static_cast<std::size_t>(time) * profile.fft_size + fft) *
                    physical_ports + tx] =
                {pilot_sign(pilot_seed, centered, time), 0.0f};
        }
    }
    for (const std::size_t fft : layout.phase_reference_fft_indices) {
        for (unsigned time = 0u; time < data_symbols; ++time) {
            reference_grid[
                (static_cast<std::size_t>(time) * profile.fft_size + fft) *
                    physical_ports] = {1.0f, 0.0f};
        }
    }
}

namespace {

void prepare_dynamic_frame_impl(
    const MiniHeader& header,
    float marker_metric,
    const LinkMode* configured_mode,
    const std::vector<std::complex<float>>& equalized_payload_symbols,
    const std::vector<float>& effective_noise_variances,
    PreparedDynamicFrame& prepared) {
    prepared.header = header;
    prepared.marker_metric = marker_metric;
    const unsigned bits = bits_per_symbol_from_flags(prepared.header.flags);
    const LinkMode header_mode{
        transmit_rank_from_flags(prepared.header.flags),
        modulation_from_bits(bits),
        transmission_scheme_from_flags(prepared.header.flags)};
    if (configured_mode != nullptr) {
        if (configured_mode->rank != header_mode.rank ||
            configured_mode->modulation != header_mode.modulation ||
            configured_mode->scheme != header_mode.scheme) {
            throw std::runtime_error(
                "configured physical-port profile disagrees with frame header");
        }
        prepared.mode = *configured_mode;
    } else {
        prepared.mode = header_mode;
    }
    const auto& layout = cached_layout_for_mode(prepared.mode);
    if (prepared.header.payload_blocks == 0u ||
        prepared.header.payload_blocks > layout.ldpc_blocks) {
        throw std::runtime_error("control header exceeds Rank/MCS frame capacity");
    }
    if (equalized_payload_symbols.size() != layout.payload_layer_symbols ||
        effective_noise_variances.size() != layout.payload_layer_symbols) {
        throw std::invalid_argument("equalized payload shape does not match decoded Rank/MCS");
    }
    using Clock = std::chrono::steady_clock;
    const auto demapping_start = Clock::now();
    auto& llrs = prepared.llrs;
    auto& interleaved_block = prepared.interleaved_block;
    llrs.resize(static_cast<std::size_t>(prepared.header.payload_blocks) *
                ldpc_codeword_bits);
    interleaved_block.resize(ldpc_codeword_bits);
    constexpr std::size_t interleaver_columns =
        ldpc_codeword_bits / ldpc_interleaver_rows;
    const std::size_t symbols_per_block = ldpc_codeword_bits / bits;
    for (std::size_t block = 0u; block < prepared.header.payload_blocks; ++block) {
        for (std::size_t local_symbol = 0u;
             local_symbol < symbols_per_block; ++local_symbol) {
            const std::size_t symbol = block * symbols_per_block + local_symbol;
            const auto values = SquareQAM::max_log_llrs(
                equalized_payload_symbols[symbol],
                effective_noise_variances[symbol], bits);
            for (unsigned bit = 0u; bit < bits; ++bit) {
                interleaved_block[local_symbol * bits + bit] = values[bit];
            }
        }
        float* const output = llrs.data() + block * ldpc_codeword_bits;
        for (std::size_t row = 0u; row < ldpc_interleaver_rows; ++row) {
            for (std::size_t column = 0u;
                 column < interleaver_columns; ++column) {
                const std::size_t source = row * interleaver_columns + column;
                const std::size_t destination =
                    column * ldpc_interleaver_rows + row;
                output[source] = interleaved_block[destination];
            }
        }
    }
    soft_descramble(llrs);
    const auto demapping_done = Clock::now();
    prepared.soft_demapping_us = std::chrono::duration<double, std::micro>(
        demapping_done - demapping_start).count();
}

DecodedDynamicFrame decode_prepared_dynamic_frame_impl(
    const PreparedDynamicFrame& prepared,
    const Ldpc5041008& codec,
    unsigned maximum_ldpc_iterations,
    float ldpc_normalization,
    LdpcFrameDecoder* frame_decoder) {
    DecodedDynamicFrame result;
    result.header = prepared.header;
    result.mode = prepared.mode;
    result.marker_metric = prepared.marker_metric;
    result.soft_demapping_us = prepared.soft_demapping_us;
    const auto ldpc_start = std::chrono::steady_clock::now();
    std::vector<std::uint8_t> information_bits;
    if (frame_decoder != nullptr) {
        LdpcFrameDecodeResult decoded;
        frame_decoder->decode_blocks(
            prepared.llrs, result.header.payload_blocks, maximum_ldpc_iterations,
            ldpc_normalization, decoded);
        result.syndrome_failures = decoded.syndrome_failures;
        result.maximum_decoder_iterations = decoded.maximum_iterations;
        result.ldpc_worker_threads = frame_decoder->worker_count();
        result.ldpc_capacity_growths_this_frame =
            decoded.capacity_growths_this_frame;
        information_bits = std::move(decoded.information_bits);
    } else {
        information_bits.reserve(
            static_cast<std::size_t>(result.header.payload_blocks) *
            ldpc_information_bits);
        for (std::size_t block = 0u; block < result.header.payload_blocks; ++block) {
            const auto begin = prepared.llrs.begin() +
                static_cast<std::ptrdiff_t>(block * ldpc_encoded_bits);
            const std::vector<float> block_llrs(begin, begin + ldpc_encoded_bits);
            const auto decoded = codec.decode_normalized_min_sum(
                block_llrs, maximum_ldpc_iterations, ldpc_normalization);
            result.syndrome_failures += decoded.syndrome_weight != 0u;
            result.maximum_decoder_iterations =
                std::max(result.maximum_decoder_iterations, decoded.iterations);
            information_bits.insert(
                information_bits.end(), decoded.information_bits.begin(),
                decoded.information_bits.end());
        }
    }
    result.information_bytes = pack_msb_bits(information_bits);
    result.information_bytes.resize(result.header.payload_len);
    result.crc_ok = check_crc16_ccitt_false(result.information_bytes);
    if (result.information_bytes.size() >= 2u) {
        result.user_payload.assign(
            result.information_bytes.begin(), result.information_bytes.end() - 2);
    }
    const auto ldpc_done = std::chrono::steady_clock::now();
    result.ldpc_crc_us = std::chrono::duration<double, std::micro>(
        ldpc_done - ldpc_start).count();
    return result;
}

}  // namespace

void prepare_dynamic_frame_llrs(
    const std::vector<float>& control_llrs,
    const std::vector<std::complex<float>>& equalized_payload_symbols,
    const std::vector<float>& effective_noise_variances,
    PreparedDynamicFrame& prepared) {
    float marker_metric = 0.0f;
    const auto header = decode_control_qpsk_llrs(control_llrs, &marker_metric);
    prepare_dynamic_frame_impl(
        header, marker_metric, nullptr, equalized_payload_symbols,
        effective_noise_variances, prepared);
}

void prepare_dynamic_frame_payload_llrs(
    const MiniHeader& decoded_header,
    float marker_metric,
    const std::vector<std::complex<float>>& equalized_payload_symbols,
    const std::vector<float>& effective_noise_variances,
    PreparedDynamicFrame& prepared) {
    prepare_dynamic_frame_impl(
        decoded_header, marker_metric, nullptr, equalized_payload_symbols,
        effective_noise_variances, prepared);
}

void prepare_dynamic_frame_payload_llrs(
    const MiniHeader& decoded_header,
    float marker_metric,
    const LinkMode& configured_mode,
    const std::vector<std::complex<float>>& equalized_payload_symbols,
    const std::vector<float>& effective_noise_variances,
    PreparedDynamicFrame& prepared) {
    prepare_dynamic_frame_impl(
        decoded_header, marker_metric, &configured_mode,
        equalized_payload_symbols, effective_noise_variances, prepared);
}

DecodedDynamicFrame decode_prepared_dynamic_frame(
    const PreparedDynamicFrame& prepared,
    const Ldpc5041008& codec,
    unsigned maximum_ldpc_iterations,
    float ldpc_normalization,
    LdpcFrameDecoder* frame_decoder) {
    return decode_prepared_dynamic_frame_impl(
        prepared, codec, maximum_ldpc_iterations,
        ldpc_normalization, frame_decoder);
}

DecodedDynamicFrame decode_dynamic_frame(
    const std::vector<std::uint8_t>& control_labels,
    const std::vector<std::complex<float>>& equalized_payload_symbols,
    const std::vector<float>& effective_noise_variances,
    const Ldpc5041008& codec,
    unsigned maximum_ldpc_iterations,
    float ldpc_normalization,
    LdpcFrameDecoder* frame_decoder,
    DynamicFrameDecodeWorkspace* workspace) {
    float marker_metric = 0.0f;
    const auto header = decode_control_qpsk_labels(control_labels, &marker_metric);
    DynamicFrameDecodeWorkspace local_workspace;
    auto& prepared = workspace == nullptr ? local_workspace : *workspace;
    prepare_dynamic_frame_impl(
        header, marker_metric, nullptr, equalized_payload_symbols,
        effective_noise_variances, prepared);
    return decode_prepared_dynamic_frame_impl(
        prepared, codec, maximum_ldpc_iterations,
        ldpc_normalization, frame_decoder);
}

DecodedDynamicFrame decode_dynamic_frame_llrs(
    const std::vector<float>& control_llrs,
    const std::vector<std::complex<float>>& equalized_payload_symbols,
    const std::vector<float>& effective_noise_variances,
    const Ldpc5041008& codec,
    unsigned maximum_ldpc_iterations,
    float ldpc_normalization,
    LdpcFrameDecoder* frame_decoder,
    DynamicFrameDecodeWorkspace* workspace) {
    DynamicFrameDecodeWorkspace local_workspace;
    auto& prepared = workspace == nullptr ? local_workspace : *workspace;
    prepare_dynamic_frame_llrs(
        control_llrs, equalized_payload_symbols,
        effective_noise_variances, prepared);
    return decode_prepared_dynamic_frame_impl(
        prepared, codec, maximum_ldpc_iterations,
        ldpc_normalization, frame_decoder);
}

}  // namespace openisac
