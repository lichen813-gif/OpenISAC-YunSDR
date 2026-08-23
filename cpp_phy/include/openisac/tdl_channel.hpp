#pragma once

#include "openisac/mimo2x2.hpp"

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace openisac {

struct TdlTap {
    std::size_t delay_samples = 0u;
    float gain_db = 0.0f;
    float phase_degrees = 0.0f;
    // Complex-baseband path Doppler. Positive values advance phase with time.
    float doppler_hz = 0.0f;
};

using ImpulseResponse2x2 =
    std::array<std::array<std::vector<std::complex<float>>, 2>, 2>;

struct TdlSpatialCorrelationConfig {
    // Real-valued two-element correlation coefficients. Negative correlation
    // is supported; magnitudes must remain below one.
    float transmit_correlation = 0.0f;
    float receive_correlation = 0.0f;
    std::uint32_t random_seed = 0xC057u;
};

ImpulseResponse2x2 build_deterministic_tdl_2x2(const std::vector<TdlTap>& taps);

// Kronecker model H_l = R_rx^(1/2) W_l R_tx^(1/2). W_l uses a local,
// implementation-independent PRNG so identical seeds reproduce across the
// supported Windows and Linux C++ builds.
ImpulseResponse2x2 build_correlated_tdl_2x2(
    const std::vector<TdlTap>& taps,
    const TdlSpatialCorrelationConfig& correlation);

// Evaluate a time-varying TDL snapshot without changing path delay or power.
std::vector<TdlTap> evaluate_tdl_taps(
    const std::vector<TdlTap>& taps,
    double time_seconds);

std::array<std::vector<std::complex<float>>, 2> apply_tdl_2x2_symbol(
    const std::array<std::vector<std::complex<float>>, 2>& transmitted,
    const ImpulseResponse2x2& impulse_response);

void apply_tdl_2x2_symbol(
    const std::array<std::vector<std::complex<float>>, 2>& transmitted,
    const ImpulseResponse2x2& impulse_response,
    std::array<std::vector<std::complex<float>>, 2>& received);

Channel2x2 tdl_frequency_response(
    const ImpulseResponse2x2& impulse_response,
    std::size_t fft_index,
    std::size_t fft_size);

}  // namespace openisac
