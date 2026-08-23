#pragma once

#include "openisac/mimo2x2.hpp"

#include <array>
#include <complex>
#include <cstddef>

namespace openisac {

constexpr std::size_t maximum_spatial_streams = 8u;

struct ChannelNxN {
    std::size_t streams = 0u;
    // Row-major H[receive][transmit]; only streams x streams entries are active.
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

}  // namespace openisac
