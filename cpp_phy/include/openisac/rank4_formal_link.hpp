#pragma once

#include "openisac/link_adaptation.hpp"
#include "openisac/ldpc.hpp"
#include "openisac/tdl_channel.hpp"

#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace openisac {

struct Rank4FormalSimulationConfig {
    Modulation modulation = Modulation::qam64;
    std::size_t frames = 4u;
    // Zero selects the maximum user payload supported by the Rank/MCS layout.
    std::size_t payload_bytes = 0u;
    float snr_db = 50.0f;
    float transmit_correlation = 0.2f;
    float receive_correlation = 0.2f;
    std::uint32_t channel_seed = 0x4C057u;
    std::uint32_t payload_seed = 0x52414E4Bu;
    std::uint32_t pilot_seed = 0xC057u;
    unsigned maximum_ldpc_iterations = 10u;
    std::vector<TdlTap> taps{
        {0u, 0.0f, 0.0f, 0.0f},
        {3u, -14.0f, 45.0f, 0.0f},
        {9u, -8.0f, -80.0f, 0.0f}};
};

struct Rank4FormalSimulationResult {
    std::size_t frames = 0u;
    std::size_t header_passes = 0u;
    std::size_t crc_passes = 0u;
    std::size_t payload_matches = 0u;
    std::size_t syndrome_failures = 0u;
    std::size_t user_payload_bytes_per_frame = 0u;
    std::size_t ldpc_blocks_per_frame = 0u;
    std::size_t pre_fec_bit_errors = 0u;
    std::size_t pre_fec_compared_bits = 0u;
    float pre_fec_ber = 0.0f;
    float evm_percent = 0.0f;
    float channel_nmse_db = 0.0f;
    std::vector<std::complex<float>> transmitted_symbols;
    std::vector<std::complex<float>> equalized_symbols;
};

// Frequency-domain acquisition closure for the formal Rank-4 frame. Each
// data symbol is OFDM modulated, passed through a correlated 4x4 TDL, channel
// estimated from receiver-known FDM pilots, MMSE detected, soft demapped and
// finally checked by the production LDPC/CRC implementation.
Rank4FormalSimulationResult simulate_rank4_formal_link(
    const Rank4FormalSimulationConfig& config,
    const Ldpc5041008& codec);

}  // namespace openisac
