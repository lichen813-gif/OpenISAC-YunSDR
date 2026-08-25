#include "openisac/frame.hpp"

#include "openisac/crc16.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace openisac {
namespace {

std::size_t rounded_position(std::size_t index, std::size_t count, std::size_t last) {
    if (count <= 1u) {
        return 0u;
    }
    return static_cast<std::size_t>(std::llround(
        static_cast<double>(index * last) / static_cast<double>(count - 1u)));
}

std::uint8_t gf_multiply(
    std::uint8_t left,
    std::uint8_t right,
    const std::array<std::uint8_t, 127>& exponent,
    const std::array<std::int16_t, 128>& logarithm) {
    if (left == 0u || right == 0u) {
        return 0u;
    }
    const int power =
        (static_cast<int>(logarithm[left]) + static_cast<int>(logarithm[right])) % 127;
    return exponent[static_cast<std::size_t>(power)];
}

std::array<std::uint8_t, 64> bch_generator() {
    std::array<std::uint8_t, 127> exponent{};
    std::array<std::int16_t, 128> logarithm{};
    logarithm.fill(-1);
    unsigned value = 1u;
    for (std::size_t index = 0; index < exponent.size(); ++index) {
        exponent[index] = static_cast<std::uint8_t>(value);
        logarithm[value] = static_cast<std::int16_t>(index);
        unsigned shifted = value << 1u;
        if ((shifted & 0x80u) != 0u) {
            shifted ^= 0x83u;
        }
        value = shifted & 0x7Fu;
    }
    std::array<bool, 127> seen{};
    std::vector<unsigned> roots;
    for (unsigned root = 1u; root <= 20u; ++root) {
        unsigned power = root % 127u;
        for (unsigned iteration = 0u; iteration < 7u; ++iteration) {
            if (!seen[power]) {
                seen[power] = true;
                roots.push_back(power);
            }
            power = (power * 2u) % 127u;
        }
    }
    std::sort(roots.begin(), roots.end());
    std::vector<std::uint8_t> polynomial{1u};
    for (const unsigned power : roots) {
        const std::uint8_t root = exponent[power];
        std::vector<std::uint8_t> next(polynomial.size() + 1u, 0u);
        for (std::size_t index = 0; index < polynomial.size(); ++index) {
            next[index] ^= gf_multiply(polynomial[index], root, exponent, logarithm);
            next[index + 1u] ^= polynomial[index];
        }
        polynomial = std::move(next);
    }
    if (polynomial.size() != 64u) {
        throw std::runtime_error("invalid BCH generator size");
    }
    std::array<std::uint8_t, 64> generator{};
    std::copy(polynomial.begin(), polynomial.end(), generator.begin());
    return generator;
}

std::array<std::uint8_t, 127> bch_encode(std::uint64_t word) {
    const auto generator = bch_generator();
    std::array<std::uint8_t, 64> message{};
    for (std::size_t index = 0; index < message.size(); ++index) {
        message[index] = static_cast<std::uint8_t>(
            (word >> (63u - static_cast<unsigned>(index))) & 1u);
    }
    std::array<std::uint8_t, 127> work{};
    std::copy(message.begin(), message.end(), work.begin() + 63);
    for (int position = 126; position >= 63; --position) {
        if (work[static_cast<std::size_t>(position)] == 0u) {
            continue;
        }
        const std::size_t shift = static_cast<std::size_t>(position - 63);
        for (std::size_t index = 0; index < generator.size(); ++index) {
            work[shift + index] ^= generator[index];
        }
    }
    std::array<std::uint8_t, 127> result{};
    std::copy(work.begin(), work.begin() + 63, result.begin());
    std::copy(message.begin(), message.end(), result.begin() + 63);
    return result;
}

struct GfTables {
    std::array<std::uint8_t, 127> exponent{};
    std::array<std::int16_t, 128> logarithm{};
};

GfTables gf_tables() {
    GfTables tables;
    tables.logarithm.fill(-1);
    unsigned value = 1u;
    for (std::size_t index = 0u; index < tables.exponent.size(); ++index) {
        tables.exponent[index] = static_cast<std::uint8_t>(value);
        tables.logarithm[value] = static_cast<std::int16_t>(index);
        unsigned shifted = value << 1u;
        if ((shifted & 0x80u) != 0u) {
            shifted ^= 0x83u;
        }
        value = shifted & 0x7Fu;
    }
    return tables;
}

std::uint8_t gf_divide(
    std::uint8_t numerator,
    std::uint8_t denominator,
    const GfTables& tables) {
    if (numerator == 0u) {
        return 0u;
    }
    if (denominator == 0u) {
        throw std::runtime_error("BCH GF divide by zero");
    }
    int power = static_cast<int>(tables.logarithm[numerator]) -
                static_cast<int>(tables.logarithm[denominator]);
    power %= 127;
    if (power < 0) {
        power += 127;
    }
    return tables.exponent[static_cast<std::size_t>(power)];
}

std::array<std::uint8_t, 20> bch_syndromes(
    const std::array<std::uint8_t, 127>& bits,
    const GfTables& tables) {
    std::array<std::uint8_t, 20> result{};
    for (unsigned syndrome = 1u; syndrome <= 20u; ++syndrome) {
        std::uint8_t accumulator = 0u;
        for (std::size_t position = 0u; position < bits.size(); ++position) {
            if (bits[position] != 0u) {
                accumulator ^= tables.exponent[
                    (syndrome * static_cast<unsigned>(position)) % 127u];
            }
        }
        result[syndrome - 1u] = accumulator;
    }
    return result;
}

std::uint64_t bch_decode(std::array<std::uint8_t, 127> bits) {
    const auto tables = gf_tables();
    auto syndromes = bch_syndromes(bits, tables);
    if (std::any_of(syndromes.begin(), syndromes.end(),
                    [](std::uint8_t value) { return value != 0u; })) {
        std::array<std::uint8_t, 21> locator{};
        std::array<std::uint8_t, 21> previous{};
        locator[0] = 1u;
        previous[0] = 1u;
        unsigned degree = 0u;
        unsigned shift = 1u;
        std::uint8_t scale = 1u;
        for (unsigned index = 0u; index < 20u; ++index) {
            std::uint8_t discrepancy = syndromes[index];
            for (unsigned coefficient = 1u; coefficient <= degree; ++coefficient) {
                if (locator[coefficient] != 0u &&
                    syndromes[index - coefficient] != 0u) {
                    discrepancy ^= gf_multiply(
                        locator[coefficient], syndromes[index - coefficient],
                        tables.exponent, tables.logarithm);
                }
            }
            if (discrepancy == 0u) {
                ++shift;
                continue;
            }
            const auto saved = locator;
            const std::uint8_t factor = gf_divide(discrepancy, scale, tables);
            for (unsigned coefficient = 0u;
                 coefficient + shift < locator.size(); ++coefficient) {
                if (previous[coefficient] != 0u) {
                    locator[coefficient + shift] ^= gf_multiply(
                        factor, previous[coefficient],
                        tables.exponent, tables.logarithm);
                }
            }
            if (2u * degree <= index) {
                degree = index + 1u - degree;
                previous = saved;
                scale = discrepancy;
                shift = 1u;
            } else {
                ++shift;
            }
        }
        if (degree > 10u) {
            throw std::runtime_error("BCH error count exceeds correction capability");
        }
        std::vector<std::size_t> error_positions;
        for (std::size_t position = 0u; position < bits.size(); ++position) {
            const std::uint8_t x = tables.exponent[
                (127u - static_cast<unsigned>(position % 127u)) % 127u];
            std::uint8_t value = locator[0];
            std::uint8_t x_power = 1u;
            for (unsigned coefficient = 1u; coefficient <= degree; ++coefficient) {
                x_power = gf_multiply(
                    x_power, x, tables.exponent, tables.logarithm);
                if (locator[coefficient] != 0u) {
                    value ^= gf_multiply(
                        locator[coefficient], x_power,
                        tables.exponent, tables.logarithm);
                }
            }
            if (value == 0u) {
                error_positions.push_back(position);
            }
        }
        if (error_positions.size() != degree) {
            throw std::runtime_error("BCH error locator has inconsistent degree");
        }
        for (const auto position : error_positions) {
            bits[position] ^= 1u;
        }
        syndromes = bch_syndromes(bits, tables);
        if (std::any_of(syndromes.begin(), syndromes.end(),
                        [](std::uint8_t value) { return value != 0u; })) {
            throw std::runtime_error("BCH correction left a non-zero syndrome");
        }
    }
    std::uint64_t word = 0u;
    for (std::size_t index = 63u; index < bits.size(); ++index) {
        word = (word << 1u) | bits[index];
    }
    return word;
}

}  // namespace

const char* pilot_mode_name(PilotMode mode) noexcept {
    switch (mode) {
        case PilotMode::fdm: return "fdm";
        case PilotMode::nr_dmrs: return "nr-dmrs";
    }
    return "unknown";
}

FormalFrameLayout build_formal_frame_layout(const FormalFrameProfile& profile) {
    if (profile.fft_size != 1024u || profile.control_re_count != 128u ||
        (profile.transmit_rank != 1u && profile.transmit_rank != 2u &&
         profile.transmit_rank != 4u) ||
        (profile.scheme == TransmissionScheme::alamouti_stbc &&
         profile.transmit_rank != 1u) ||
        (profile.transmit_rank == 4u && profile.pilot_spacing != 2u) ||
        1008u % profile.bits_per_symbol != 0u) {
        throw std::invalid_argument("unsupported formal-frame profile");
    }
    std::vector<int> active;
    const int first = -static_cast<int>(profile.fft_size / 2u) +
                      static_cast<int>(profile.guard_left);
    const int last = static_cast<int>(profile.fft_size / 2u) - 1 -
                     static_cast<int>(profile.guard_right);
    for (int centered = first; centered <= last; ++centered) {
        if (centered != 0) {
            active.push_back(centered);
        }
    }
    std::vector<int> all_pilots;
    for (const int centered : active) {
        const int spacing = static_cast<int>(profile.pilot_spacing);
        if ((centered % spacing + spacing) % spacing == 0) {
            all_pilots.push_back(centered);
        }
    }
    std::set<int> pilot_set(all_pilots.begin(), all_pilots.end());
    std::set<std::size_t> phase_positions;
    for (std::size_t index = 0; index < profile.phase_reference_count; ++index) {
        phase_positions.insert(
            rounded_position(index, profile.phase_reference_count, all_pilots.size() - 1u));
    }

    FormalFrameLayout layout;
    auto native_index = [&profile](int centered) {
        const int fft = static_cast<int>(profile.fft_size);
        return static_cast<std::uint16_t>((centered % fft + fft) % fft);
    };
    for (const int centered : active) {
        layout.active_fft_indices.push_back(native_index(centered));
    }
    for (const int centered : active) {
        if (pilot_set.count(centered) == 0u) {
            layout.data_fft_indices.push_back(native_index(centered));
        }
    }
    for (std::size_t index = 0; index < all_pilots.size(); ++index) {
        if (phase_positions.count(index) != 0u) {
            layout.phase_reference_fft_indices.push_back(native_index(all_pilots[index]));
        } else {
            layout.pilot_fft_indices.push_back(native_index(all_pilots[index]));
        }
    }
    for (std::size_t index = 0; index < profile.control_re_count; ++index) {
        layout.control_data_positions.push_back(static_cast<std::uint16_t>(
            rounded_position(
                index, profile.control_re_count, layout.data_fft_indices.size() - 1u)));
    }
    const std::set<std::uint16_t> control_set(
        layout.control_data_positions.begin(), layout.control_data_positions.end());
    for (std::uint8_t time = 0u; time < 2u; ++time) {
        for (std::size_t data = 0; data < layout.data_fft_indices.size(); ++data) {
            const bool control_position =
                control_set.count(static_cast<std::uint16_t>(data)) != 0u;
            if ((time == 0u ||
                 profile.scheme == TransmissionScheme::alamouti_stbc) &&
                control_position) {
                continue;
            }
            layout.payload_time_indices.push_back(time);
            layout.payload_data_positions.push_back(static_cast<std::uint16_t>(data));
        }
    }
    layout.payload_layer_symbols =
        layout.payload_time_indices.size() * profile.transmit_rank;
    const std::size_t symbols_per_block = 1008u / profile.bits_per_symbol;
    layout.ldpc_blocks = layout.payload_layer_symbols / symbols_per_block;
    layout.coded_qam_symbols = layout.ldpc_blocks * symbols_per_block;
    layout.padding_qam_symbols =
        layout.payload_layer_symbols - layout.coded_qam_symbols;
    layout.information_bytes = layout.ldpc_blocks * 63u;
    layout.user_payload_bytes = layout.information_bytes - 2u;
    return layout;
}

std::uint8_t modulation_flag(unsigned bits_per_symbol) {
    switch (bits_per_symbol) {
        case 2u: return 0x00u;
        case 4u: return 0x04u;
        case 6u: return 0x08u;
        case 8u: return 0x0Cu;
        default: throw std::invalid_argument("unsupported modulation flag");
    }
}

unsigned bits_per_symbol_from_flags(std::uint8_t flags) {
    switch (flags & 0x0Cu) {
        case 0x00u: return 2u;
        case 0x04u: return 4u;
        case 0x08u: return 6u;
        case 0x0Cu: return 8u;
        default: throw std::invalid_argument("unsupported modulation flag");
    }
}

std::uint8_t transmit_rank_flag(unsigned transmit_rank) {
    if (transmit_rank == 1u) {
        return 0x00u;
    }
    if (transmit_rank == 2u) {
        return 0x01u;
    }
    if (transmit_rank == 4u) {
        return 0x02u;
    }
    throw std::invalid_argument("transmit rank must be 1, 2 or 4");
}

std::uint8_t transmission_mode_flag(const LinkMode& mode) {
    const unsigned ports = physical_transmit_ports(mode);
    if (mode.scheme == TransmissionScheme::alamouti_stbc) {
        if (mode.rank != 1u || ports != 2u) {
            throw std::invalid_argument(
                "Alamouti STBC requires one stream and two transmit ports");
        }
        return 0x03u;
    }
    const bool supported =
        (ports == 1u && mode.rank == 1u) ||
        (ports == 2u && (mode.rank == 1u || mode.rank == 2u)) ||
        (ports == 4u && (mode.rank == 2u || mode.rank == 4u));
    if (!supported) {
        throw std::invalid_argument("unsupported spatial Rank/Tx-port profile");
    }
    return transmit_rank_flag(mode.rank);
}

unsigned transmit_rank_from_flags(std::uint8_t flags) noexcept {
    switch (flags & 0x03u) {
        case 0x00u: return 1u;
        case 0x01u: return 2u;
        case 0x02u: return 4u;
        case 0x03u: return 1u;
    }
    return 0u;
}

TransmissionScheme transmission_scheme_from_flags(std::uint8_t flags) noexcept {
    return (flags & 0x03u) == 0x03u
        ? TransmissionScheme::alamouti_stbc
        : TransmissionScheme::spatial_multiplexing;
}

std::uint64_t pack_mini_header(const MiniHeader& header) {
    if (header.version != 1u || (header.flags & 0xF0u) != 0u) {
        throw std::invalid_argument("unsupported mini-header version or flags");
    }
    const std::size_t expected_blocks =
        (static_cast<std::size_t>(header.payload_len) + 62u) / 63u;
    const std::uint8_t expected_field = static_cast<std::uint8_t>(
        std::min<std::size_t>(expected_blocks, 0xFFu));
    if (header.payload_blocks != expected_field) {
        throw std::invalid_argument("payload block field does not match length");
    }
    const std::uint64_t prefix =
        (static_cast<std::uint64_t>(header.version & 0x0Fu) << 44u) |
        (static_cast<std::uint64_t>(header.flags & 0x0Fu) << 40u) |
        (static_cast<std::uint64_t>(header.payload_len) << 24u) |
        (static_cast<std::uint64_t>(header.payload_blocks) << 16u) |
        header.sequence;
    std::array<std::uint8_t, 6> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>(
            (prefix >> (8u * static_cast<unsigned>(5u - index))) & 0xFFu);
    }
    const std::uint16_t crc = crc16_ccitt_false(bytes.data(), bytes.size());
    return (prefix << 16u) | crc;
}

MiniHeader unpack_mini_header(std::uint64_t word) {
    const std::uint64_t prefix = word >> 16u;
    std::array<std::uint8_t, 6> bytes{};
    for (std::size_t index = 0u; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>(
            (prefix >> (8u * static_cast<unsigned>(5u - index))) & 0xFFu);
    }
    if (crc16_ccitt_false(bytes.data(), bytes.size()) !=
        static_cast<std::uint16_t>(word & 0xFFFFu)) {
        throw std::runtime_error("mini-header CRC mismatch");
    }
    const MiniHeader header{
        static_cast<std::uint8_t>((prefix >> 44u) & 0x0Fu),
        static_cast<std::uint8_t>((prefix >> 40u) & 0x0Fu),
        static_cast<std::uint16_t>((prefix >> 24u) & 0xFFFFu),
        static_cast<std::uint8_t>((prefix >> 16u) & 0xFFu),
        static_cast<std::uint16_t>(prefix & 0xFFFFu),
    };
    if (header.version != 1u || (header.flags & 0xF0u) != 0u) {
        throw std::runtime_error("unsupported mini-header version or flags");
    }
    const std::size_t expected_blocks =
        (static_cast<std::size_t>(header.payload_len) + 62u) / 63u;
    if (header.payload_blocks !=
        static_cast<std::uint8_t>(std::min<std::size_t>(expected_blocks, 0xFFu))) {
        throw std::runtime_error("mini-header block count mismatch");
    }
    return header;
}

std::vector<std::uint8_t> marker_qpsk_labels() {
    std::vector<std::uint8_t> labels(64u);
    for (std::uint32_t index = 0u; index < 64u; ++index) {
        std::uint32_t value = 0x4F504953u ^ (index * 0x9E3779B9u);
        value ^= value >> 16u;
        value *= 0x7FEB352Du;
        value ^= value >> 15u;
        value *= 0x846CA68Bu;
        value ^= value >> 16u;
        labels[index] = static_cast<std::uint8_t>((value >> 5u) & 0x03u);
    }
    return labels;
}

std::vector<std::uint8_t> control_qpsk_labels(const MiniHeader& header) {
    const auto code = bch_encode(pack_mini_header(header));
    std::array<std::uint8_t, 128> padded{};
    std::copy(code.begin(), code.end(), padded.begin());
    auto labels = marker_qpsk_labels();
    labels.reserve(128u);
    for (std::size_t index = 0; index < padded.size(); index += 2u) {
        labels.push_back(static_cast<std::uint8_t>(
            (padded[index] << 1u) | padded[index + 1u]));
    }
    return labels;
}

MiniHeader decode_control_qpsk_labels(
    const std::vector<std::uint8_t>& labels,
    float* marker_metric) {
    if (labels.size() != 128u ||
        std::any_of(labels.begin(), labels.end(),
                    [](std::uint8_t label) { return label > 3u; })) {
        throw std::invalid_argument("control region requires 128 QPSK labels");
    }
    const auto expected_marker = marker_qpsk_labels();
    unsigned matching_bits = 0u;
    for (std::size_t index = 0u; index < expected_marker.size(); ++index) {
        const std::uint8_t difference =
            static_cast<std::uint8_t>(labels[index] ^ expected_marker[index]);
        matching_bits += 2u - static_cast<unsigned>(difference & 1u) -
                         static_cast<unsigned>((difference >> 1u) & 1u);
    }
    const float metric =
        2.0f * static_cast<float>(matching_bits) / 128.0f - 1.0f;
    if (marker_metric != nullptr) {
        *marker_metric = metric;
    }
    if (metric < 0.5f) {
        throw std::runtime_error("LDPC packet marker was not detected");
    }
    std::array<std::uint8_t, 127> code{};
    for (std::size_t bit = 0u; bit < code.size(); ++bit) {
        const std::uint8_t label = labels[64u + bit / 2u];
        code[bit] = static_cast<std::uint8_t>(
            (label >> (1u - static_cast<unsigned>(bit % 2u))) & 1u);
    }
    return unpack_mini_header(bch_decode(code));
}

MiniHeader decode_control_qpsk_llrs(
    const std::vector<float>& llrs,
    float* marker_metric) {
    if (llrs.size() != 256u ||
        std::any_of(llrs.begin(), llrs.end(),
                    [](float value) { return !std::isfinite(value); })) {
        throw std::invalid_argument("control region requires 256 finite QPSK LLRs");
    }
    const auto marker = marker_qpsk_labels();
    double correlation = 0.0;
    double energy = 0.0;
    for (std::size_t bit = 0u; bit < 128u; ++bit) {
        const std::uint8_t label = marker[bit / 2u];
        const bool expected =
            ((label >> (1u - static_cast<unsigned>(bit % 2u))) & 1u) != 0u;
        correlation += expected ? -llrs[bit] : llrs[bit];
        energy += std::abs(llrs[bit]);
    }
    const float metric = energy <= 1.0e-9
        ? 0.0f
        : static_cast<float>(correlation / energy);
    if (marker_metric != nullptr) {
        *marker_metric = metric;
    }
    if (metric < 0.5f) {
        throw std::runtime_error("LDPC packet marker was not detected");
    }
    std::array<std::uint8_t, 127> code{};
    for (std::size_t bit = 0u; bit < code.size(); ++bit) {
        code[bit] = llrs[128u + bit] < 0.0f ? 1u : 0u;
    }
    return unpack_mini_header(bch_decode(code));
}

}  // namespace openisac
