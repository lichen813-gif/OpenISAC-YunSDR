#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace openisac {

std::vector<std::complex<float>> generate_zc_frequency(
    std::size_t fft_size,
    unsigned root);

std::vector<std::complex<float>> generate_zc_ofdm_symbol(
    std::size_t fft_size,
    std::size_t cp_length,
    unsigned root);

struct TimingEstimate {
    std::size_t offset = 0u;
    std::size_t search_begin = 0u;
    float peak_metric = 0.0f;
    std::vector<float> metrics;
};

TimingEstimate estimate_zc_timing(
    const std::vector<std::vector<std::complex<float>>>& receive_streams,
    const std::vector<std::complex<float>>& reference_symbol,
    std::size_t max_search_samples);

void estimate_zc_timing(
    const std::vector<std::vector<std::complex<float>>>& receive_streams,
    const std::vector<std::complex<float>>& reference_symbol,
    std::size_t max_search_samples,
    TimingEstimate& estimate);

// Searches an inclusive absolute candidate range. In locked operation a small
// window avoids repeating a full acquisition correlation on every frame.
void estimate_zc_timing_window(
    const std::vector<std::vector<std::complex<float>>>& receive_streams,
    const std::vector<std::complex<float>>& reference_symbol,
    std::size_t search_begin,
    std::size_t search_end,
    TimingEstimate& estimate);

// CFO is normalized to the OFDM subcarrier spacing (epsilon = f_offset / delta_f).
float estimate_cp_cfo_normalized(
    const std::vector<std::vector<std::complex<float>>>& receive_streams,
    std::size_t frame_offset,
    std::size_t fft_size,
    std::size_t cp_length,
    std::size_t symbol_count,
    std::size_t cp_skip_samples = 0u);

void apply_cfo_normalized_inplace(
    std::vector<std::vector<std::complex<float>>>& streams,
    float normalized_cfo,
    std::size_t fft_size);

}  // namespace openisac
