#include "openisac/mimo2x2.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace openisac {

Detection2x2 detect_2x2(
    const std::array<std::complex<float>, 2>& received,
    const Channel2x2& channel,
    float noise_variance,
    LinearDetector detector) {
    if (!std::isfinite(noise_variance) || noise_variance < 0.0f) {
        throw std::invalid_argument("noise variance must be finite and non-negative");
    }
    constexpr float inverse_sqrt_two = 0.7071067811865475244f;
    const float regularization = detector == LinearDetector::mmse
        ? noise_variance
        : 0.0f;
    const float a = 0.5f * (
        std::norm(channel.h00) + std::norm(channel.h10)) + regularization;
    const float d = 0.5f * (
        std::norm(channel.h01) + std::norm(channel.h11)) + regularization;
    const std::complex<float> b = 0.5f * (
        std::conj(channel.h00) * channel.h01 +
        std::conj(channel.h10) * channel.h11);
    const float determinant = a * d - std::norm(b);
    if (std::abs(determinant) < 1.0e-15f) {
        throw std::runtime_error("singular 2x2 detector matrix");
    }
    const float reciprocal = 1.0f / determinant;
    const std::complex<float> matched0 = inverse_sqrt_two * (
        std::conj(channel.h00) * received[0] +
        std::conj(channel.h10) * received[1]);
    const std::complex<float> matched1 = inverse_sqrt_two * (
        std::conj(channel.h01) * received[0] +
        std::conj(channel.h11) * received[1]);
    Detection2x2 result;
    result.symbols[0] = (d * matched0 - b * matched1) * reciprocal;
    result.symbols[1] =
        (-std::conj(b) * matched0 + a * matched1) * reciprocal;
    // For both ZF and MMSE, the post-detection MSE diagonal is the noise
    // variance times the inverse Gram diagonal. For MMSE the regularization is
    // included in the Gram matrix above.
    result.predicted_mse[0] = std::max(
        0.0f, noise_variance * d * reciprocal);
    result.predicted_mse[1] = std::max(
        0.0f, noise_variance * a * reciprocal);
    return result;
}

}  // namespace openisac
