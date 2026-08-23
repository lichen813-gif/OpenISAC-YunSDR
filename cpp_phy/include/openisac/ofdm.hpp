#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace openisac {

// Unitary radix-2 transform: both directions use 1/sqrt(N) normalization.
void fft_inplace(std::vector<std::complex<float>>& values, bool inverse);

std::vector<std::complex<float>> ofdm_modulate(
    const std::vector<std::complex<float>>& frequency,
    std::size_t cp_length);

void ofdm_modulate(
    const std::vector<std::complex<float>>& frequency,
    std::size_t cp_length,
    std::vector<std::complex<float>>& samples_output,
    std::vector<std::complex<float>>& fft_scratch);

std::vector<std::complex<float>> ofdm_demodulate(
    const std::vector<std::complex<float>>& samples,
    std::size_t fft_size,
    std::size_t cp_length);

// Reuses output capacity for frame-by-frame real-time processing.
void ofdm_demodulate(
    const std::vector<std::complex<float>>& samples,
    std::size_t fft_size,
    std::size_t cp_length,
    std::vector<std::complex<float>>& frequency_output);

}  // namespace openisac
