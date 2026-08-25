#pragma once

#include "openisac/mimo2x2.hpp"

#include <array>
#include <complex>
#include <cstddef>

namespace openisac {

constexpr std::size_t maximum_spatial_streams = 8u;

struct ChannelNxN {
    std::size_t streams = 0u;
    // Zero keeps the legacy square-channel behavior. A non-zero value enables
    // rectangular receive_ports x streams detection.
    std::size_t receive_ports = 0u;
    // Row-major H[receive][transmit].
    std::array<std::complex<float>,
               maximum_spatial_streams * maximum_spatial_streams> values{};
};

struct DetectionNxN {
    std::size_t streams = 0u;
    std::array<std::complex<float>, maximum_spatial_streams> symbols{};
    std::array<float, maximum_spatial_streams> predicted_mse{};
};

DetectionNxN detect_nxn(
    const std::array<std::complex<float>, maximum_spatial_streams>& received,
    const ChannelNxN& channel,
    float noise_variance,
    LinearDetector detector);

// Low-rate diagnostic condition number based on the singular values of H.
// This is intended for telemetry/link adaptation, not the per-RE detector.
float condition_number_nxn(const ChannelNxN& channel);

// Fixed semi-unitary 4x2 DFT precoder. Every physical Tx port carries both
// layers while the two precoder columns remain orthonormal.
std::complex<float> fixed_dft_precoder_4x2(
    std::size_t transmit_port,
    std::size_t layer);

ChannelNxN apply_fixed_dft_precoder_4x2(const ChannelNxN& physical_channel);

}  // namespace openisac
