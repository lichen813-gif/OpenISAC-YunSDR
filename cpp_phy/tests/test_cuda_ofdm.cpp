#include "openisac/cuda_ofdm.hpp"
#include "openisac/ofdm.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

namespace {

double elapsed_us(
    std::chrono::steady_clock::time_point begin,
    std::chrono::steady_clock::time_point end) {
    return std::chrono::duration<double, std::micro>(end - begin).count();
}

float maximum_error(
    const std::vector<std::complex<float>>& left,
    const std::vector<std::complex<float>>& right) {
    if (left.size() != right.size()) {
        throw std::runtime_error("CUDA/CPU vector size mismatch");
    }
    float result = 0.0f;
    for (std::size_t index = 0u; index < left.size(); ++index) {
        result = std::max(result, std::abs(left[index] - right[index]));
    }
    return result;
}

}  // namespace

int main() {
    try {
        if (!openisac::cuda_device_available()) {
            throw std::runtime_error("no CUDA device is available");
        }
        constexpr std::size_t fft_size = 1024u;
        constexpr std::size_t cp_length = 128u;
        constexpr std::size_t batch_size = 64u;
        constexpr std::size_t iterations = 40u;
        std::mt19937 random(0xC0DAu);
        std::normal_distribution<float> normal(0.0f, 0.3f);
        std::vector<std::complex<float>> frequency(batch_size * fft_size);
        for (auto& value : frequency) {
            value = {normal(random), normal(random)};
        }

        std::vector<std::complex<float>> cpu_time;
        cpu_time.reserve(batch_size * (fft_size + cp_length));
        for (std::size_t batch = 0u; batch < batch_size; ++batch) {
            const auto first = frequency.begin() +
                static_cast<std::ptrdiff_t>(batch * fft_size);
            std::vector<std::complex<float>> symbol(first, first + fft_size);
            const auto samples = openisac::ofdm_modulate(symbol, cp_length);
            cpu_time.insert(cpu_time.end(), samples.begin(), samples.end());
        }

        openisac::CudaOfdmBatch backend(fft_size, cp_length, batch_size);
        std::vector<std::complex<float>> cuda_time;
        std::vector<std::complex<float>> cuda_frequency;
        backend.modulate(frequency, cuda_time);
        backend.demodulate(cuda_time, cuda_frequency);
        const float modulation_error = maximum_error(cpu_time, cuda_time);
        const float round_trip_error = maximum_error(frequency, cuda_frequency);
        if (modulation_error > 5.0e-4f || round_trip_error > 5.0e-4f) {
            throw std::runtime_error("CUDA OFDM numerical tolerance exceeded");
        }

        const auto cpu_begin = std::chrono::steady_clock::now();
        std::vector<std::complex<float>> cpu_samples;
        std::vector<std::complex<float>> cpu_fft_scratch;
        for (std::size_t iteration = 0u; iteration < iterations; ++iteration) {
            for (std::size_t batch = 0u; batch < batch_size; ++batch) {
                const auto first = frequency.begin() +
                    static_cast<std::ptrdiff_t>(batch * fft_size);
                std::vector<std::complex<float>> symbol(first, first + fft_size);
                openisac::ofdm_modulate(
                    symbol, cp_length, cpu_samples, cpu_fft_scratch);
            }
        }
        const double cpu_us = elapsed_us(
            cpu_begin, std::chrono::steady_clock::now());

        const auto cuda_begin = std::chrono::steady_clock::now();
        for (std::size_t iteration = 0u; iteration < iterations; ++iteration) {
            backend.modulate(frequency, cuda_time);
        }
        const double cuda_us = elapsed_us(
            cuda_begin, std::chrono::steady_clock::now());
        const double transforms =
            static_cast<double>(iterations * batch_size);
        std::cout << "CUDA OFDM batch=" << batch_size
                  << " modulation_max_error=" << modulation_error
                  << " round_trip_max_error=" << round_trip_error << '\n'
                  << "CPU IFFT+CP=" << cpu_us / transforms
                  << " us/symbol; CUDA including H2D/D2H="
                  << cuda_us / transforms << " us/symbol; speedup="
                  << cpu_us / cuda_us << "x\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CUDA OFDM test failed: " << error.what() << '\n';
        return 1;
    }
}
