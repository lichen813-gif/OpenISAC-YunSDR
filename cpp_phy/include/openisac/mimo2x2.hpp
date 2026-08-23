#pragma once

#include <array>
#include <complex>

namespace openisac {

enum class LinearDetector { zf, mmse };

struct Channel2x2 {
    std::complex<float> h00{};
    std::complex<float> h01{};
    std::complex<float> h10{};
    std::complex<float> h11{};
};

struct Detection2x2 {
    std::array<std::complex<float>, 2> symbols{};
    std::array<float, 2> predicted_mse{};
};

Detection2x2 detect_2x2(
    const std::array<std::complex<float>, 2>& received,
    const Channel2x2& channel,
    float noise_variance,
    LinearDetector detector);

}  // namespace openisac
