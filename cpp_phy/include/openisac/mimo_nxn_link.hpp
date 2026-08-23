#pragma once

#include "openisac/mimo_nxn.hpp"
#include "openisac/tdl_channel.hpp"

#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace openisac {

struct NxNOfdmSimulationConfig {
    std::size_t streams = 4u;
    std::size_t fft_size = 1024u;
    std::size_t cp_length = 128u;
    std::size_t guard_left = 64u;
    std::size_t guard_right = 63u;
    // Rank-4 uses aggregate spacing 2, hence spacing 8 per Tx port. This
    // controls interpolation error while retaining more payload RE than Rank-2.
    std::size_t pilot_spacing = 2u;
    unsigned bits_per_symbol = 6u;
    std::size_t frames = 10u;
    float snr_db = 45.0f;
    float transmit_correlation = 0.2f;
    float receive_correlation = 0.2f;
    std::uint32_t channel_seed = 0x4C057u;
    std::uint32_t data_seed = 0x4D494D4Fu;
    std::vector<TdlTap> taps{
        {0u, 0.0f, 0.0f, 0.0f},
        {3u, -14.0f, 45.0f, 0.0f},
        {9u, -8.0f, -80.0f, 0.0f}};
};

struct NxNOfdmSimulationResult {
    std::size_t streams = 0u;
    std::size_t pilot_subcarriers = 0u;
    std::size_t data_subcarriers = 0u;
    std::size_t detected_symbols = 0u;
    std::size_t bit_errors = 0u;
    std::size_t compared_bits = 0u;
    float ber = 0.0f;
    float evm_percent = 0.0f;
    float perfect_csi_evm_percent = 0.0f;
    float channel_nmse_db = 0.0f;
    // Interleaved [symbol][stream] points retained for diagnostic plotting.
    std::vector<std::complex<float>> transmitted_symbols;
    std::vector<std::complex<float>> equalized_symbols;
};

// Algorithm-only square-MIMO OFDM closure. It intentionally excludes the
// current 2x2 formal header/LDPC/preamble so the 4x4 pilot, channel and MMSE
// design can be validated before changing the established video PHY.
NxNOfdmSimulationResult simulate_nxn_ofdm_link(
    const NxNOfdmSimulationConfig& config);

}  // namespace openisac
