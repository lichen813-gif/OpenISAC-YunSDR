#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace openisac {

std::vector<std::vector<std::complex<float>>> resample_sfo_cubic(
    const std::vector<std::vector<std::complex<float>>>& streams,
    float sfo_ppm);

void resample_sfo_cubic(
    const std::vector<std::vector<std::complex<float>>>& streams,
    float sfo_ppm,
    std::vector<std::vector<std::complex<float>>>& output);

float inverse_sfo_ppm(float sfo_ppm);

struct PhaseSlopeEstimate {
    float intercept_radians = 0.0f;
    float slope_radians_per_subcarrier = 0.0f;
    float sfo_ppm = 0.0f;
    float coherence = 0.0f;
};

PhaseSlopeEstimate estimate_sfo_phase_slope(
    const std::vector<std::complex<float>>& receive_grid,
    const std::vector<std::uint16_t>& phase_reference_fft_indices,
    std::size_t fft_size,
    std::size_t receive_antennas,
    std::size_t samples_per_symbol);

void correct_second_symbol_phase_inplace(
    std::vector<std::complex<float>>& receive_grid,
    const PhaseSlopeEstimate& estimate,
    std::size_t fft_size,
    std::size_t receive_antennas);

}  // namespace openisac
