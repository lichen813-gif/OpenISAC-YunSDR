#pragma once

#include "openisac/frame.hpp"
#include "openisac/ldpc.hpp"
#include "openisac/link_adaptation.hpp"

#include <complex>
#include <cstdint>
#include <vector>

namespace openisac {

class LdpcFrameDecoder;

struct PreparedDynamicFrame {
    MiniHeader header{};
    LinkMode mode{};
    float marker_metric = 0.0f;
    std::vector<float> llrs;
    std::vector<float> interleaved_block;
    double soft_demapping_us = 0.0;
};

using DynamicFrameDecodeWorkspace = PreparedDynamicFrame;

struct EncodedDynamicFrame {
    FormalFrameProfile profile{};
    FormalFrameLayout layout{};
    MiniHeader header{};
    std::vector<std::uint8_t> information_bytes;
    std::vector<std::uint8_t> transmitted_bits;
    std::vector<std::uint8_t> control_labels;
    std::vector<std::uint8_t> payload_labels;
    std::vector<std::complex<float>> payload_symbols;
    // [two OFDM data symbols][1024 subcarriers][two physical Tx ports]
    std::vector<std::complex<float>> tx_grid;
};

struct DecodedDynamicFrame {
    MiniHeader header{};
    LinkMode mode{};
    std::vector<std::uint8_t> information_bytes;
    std::vector<std::uint8_t> user_payload;
    bool crc_ok = false;
    std::size_t syndrome_failures = 0u;
    unsigned maximum_decoder_iterations = 0u;
    std::size_t ldpc_worker_threads = 1u;
    std::size_t ldpc_capacity_growths_this_frame = 0u;
    float marker_metric = 0.0f;
    // Payload soft demapping, deinterleaving and descrambling.
    double soft_demapping_us = 0.0;
    // LDPC decoding, bit packing and CRC verification.
    double ldpc_crc_us = 0.0;
};

EncodedDynamicFrame encode_dynamic_frame(
    const std::vector<std::uint8_t>& user_payload,
    LinkMode mode,
    std::uint16_t sequence,
    const Ldpc5041008& codec,
    std::uint32_t pilot_seed = 0xC057u);

// Build only the receiver-known FDM pilot and phase-reference REs for the two
// physical ports. Payload and control REs remain zero.
void build_dynamic_pilot_reference_grid(
    std::uint32_t pilot_seed,
    std::vector<std::complex<float>>& reference_grid);

void prepare_dynamic_frame_llrs(
    const std::vector<float>& control_llrs,
    const std::vector<std::complex<float>>& equalized_payload_symbols,
    const std::vector<float>& effective_noise_variances,
    PreparedDynamicFrame& prepared);

// Receiver fast path when the robust control region was already decoded
// before Rank/MCS-dependent MIMO payload detection.
void prepare_dynamic_frame_payload_llrs(
    const MiniHeader& decoded_header,
    float marker_metric,
    const std::vector<std::complex<float>>& equalized_payload_symbols,
    const std::vector<float>& effective_noise_variances,
    PreparedDynamicFrame& prepared);

DecodedDynamicFrame decode_prepared_dynamic_frame(
    const PreparedDynamicFrame& prepared,
    const Ldpc5041008& codec,
    unsigned maximum_ldpc_iterations = 6u,
    float ldpc_normalization = 0.8f,
    LdpcFrameDecoder* frame_decoder = nullptr);

DecodedDynamicFrame decode_dynamic_frame(
    const std::vector<std::uint8_t>& control_labels,
    const std::vector<std::complex<float>>& equalized_payload_symbols,
    const std::vector<float>& effective_noise_variances,
    const Ldpc5041008& codec,
    unsigned maximum_ldpc_iterations = 6u,
    float ldpc_normalization = 0.8f,
    LdpcFrameDecoder* frame_decoder = nullptr,
    DynamicFrameDecodeWorkspace* workspace = nullptr);

DecodedDynamicFrame decode_dynamic_frame_llrs(
    const std::vector<float>& control_llrs,
    const std::vector<std::complex<float>>& equalized_payload_symbols,
    const std::vector<float>& effective_noise_variances,
    const Ldpc5041008& codec,
    unsigned maximum_ldpc_iterations = 6u,
    float ldpc_normalization = 0.8f,
    LdpcFrameDecoder* frame_decoder = nullptr,
    DynamicFrameDecodeWorkspace* workspace = nullptr);

}  // namespace openisac
