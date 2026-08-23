#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace openisac {

struct FormalFrameProfile {
    std::size_t fft_size = 1024u;
    std::size_t cp_length = 128u;
    std::size_t guard_left = 64u;
    std::size_t guard_right = 63u;
    std::size_t pilot_spacing = 4u;
    std::size_t phase_reference_count = 8u;
    std::size_t control_re_count = 128u;
    unsigned bits_per_symbol = 6u;
    unsigned transmit_rank = 2u;
};

struct FormalFrameLayout {
    std::vector<std::uint16_t> data_fft_indices;
    std::vector<std::uint16_t> pilot_fft_indices;
    std::vector<std::uint16_t> phase_reference_fft_indices;
    std::vector<std::uint16_t> control_data_positions;
    std::vector<std::uint8_t> payload_time_indices;
    std::vector<std::uint16_t> payload_data_positions;
    std::size_t payload_layer_symbols = 0u;
    std::size_t ldpc_blocks = 0u;
    std::size_t coded_qam_symbols = 0u;
    std::size_t padding_qam_symbols = 0u;
    std::size_t information_bytes = 0u;
    std::size_t user_payload_bytes = 0u;
};

struct MiniHeader {
    std::uint8_t version = 1u;
    std::uint8_t flags = 0u;
    std::uint16_t payload_len = 0u;
    std::uint8_t payload_blocks = 0u;
    std::uint16_t sequence = 0u;
};

FormalFrameLayout build_formal_frame_layout(const FormalFrameProfile& profile);
std::uint8_t modulation_flag(unsigned bits_per_symbol);
unsigned bits_per_symbol_from_flags(std::uint8_t flags);
std::uint8_t transmit_rank_flag(unsigned transmit_rank);
unsigned transmit_rank_from_flags(std::uint8_t flags) noexcept;
std::uint64_t pack_mini_header(const MiniHeader& header);
MiniHeader unpack_mini_header(std::uint64_t word);
std::vector<std::uint8_t> marker_qpsk_labels();
std::vector<std::uint8_t> control_qpsk_labels(const MiniHeader& header);
MiniHeader decode_control_qpsk_labels(
    const std::vector<std::uint8_t>& labels,
    float* marker_metric = nullptr);
MiniHeader decode_control_qpsk_llrs(
    const std::vector<float>& llrs,
    float* marker_metric = nullptr);

}  // namespace openisac
