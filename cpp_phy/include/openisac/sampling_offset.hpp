#pragma once

#include "openisac/mimo_nxn.hpp"

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

// Sparse equivalent used by device-resident FFT paths. sparse_grid layout is
// [two symbols][phase reference][receive antenna].
PhaseSlopeEstimate estimate_sfo_phase_slope_sparse(
    const std::vector<std::complex<float>>& sparse_grid,
    const std::vector<std::uint16_t>& phase_reference_fft_indices,
    std::size_t fft_size,
    std::size_t receive_antennas,
    std::size_t samples_per_symbol);

// Fit a common and frequency-linear phase directly from one complex
// correlation per reference tone. This compact form lets a device-resident
// receiver return pilot correlations instead of complete OFDM grids.
PhaseSlopeEstimate estimate_phase_slope_from_correlations(
    const std::vector<std::complex<double>>& correlations,
    const std::vector<std::uint16_t>& reference_fft_indices,
    std::size_t fft_size);

// Fit the residual common phase and frequency-linear phase between a known
// sparse reference grid and a previously estimated NxN channel.  This is used
// to track the age of front-loaded DM-RS without repeating a full CSI solve.
PhaseSlopeEstimate estimate_reference_residual_phase_slope_nxn(
    const std::vector<std::complex<float>>& receive_grid,
    const std::vector<std::complex<float>>& reference_grid,
    const std::vector<std::uint16_t>& pilot_fft_indices,
    const std::vector<ChannelNxN>& channels,
    std::size_t symbol_index,
    std::size_t fft_size,
    std::size_t ports);

void correct_second_symbol_phase_inplace(
    std::vector<std::complex<float>>& receive_grid,
    const PhaseSlopeEstimate& estimate,
    std::size_t fft_size,
    std::size_t receive_antennas);

// Align one OFDM symbol to an earlier reference. symbol_intervals is the
// number of OFDM-symbol spacings between the target and reference symbols.
void correct_symbol_phase_inplace(
    std::vector<std::complex<float>>& receive_grid,
    const PhaseSlopeEstimate& one_symbol_estimate,
    std::size_t fft_size,
    std::size_t receive_antennas,
    std::size_t symbol_index,
    float symbol_intervals);

}  // namespace openisac
