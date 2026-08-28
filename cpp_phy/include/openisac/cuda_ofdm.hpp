#pragma once

#include "openisac/compute_backend.hpp"

#include <complex>
#include <cstddef>
#include <memory>
#include <vector>

namespace openisac {

bool cuda_device_available() noexcept;

// Fixed-shape batched OFDM transform with retained CUDA buffers and cuFFT plan.
// Both directions use the same unitary 1/sqrt(N) normalization as the CPU PHY.
class CudaOfdmBatch {
public:
    CudaOfdmBatch(
        std::size_t fft_size,
        std::size_t cp_length,
        std::size_t batch_size);
    ~CudaOfdmBatch();

    CudaOfdmBatch(const CudaOfdmBatch&) = delete;
    CudaOfdmBatch& operator=(const CudaOfdmBatch&) = delete;
    CudaOfdmBatch(CudaOfdmBatch&&) noexcept;
    CudaOfdmBatch& operator=(CudaOfdmBatch&&) noexcept;

    std::size_t fft_size() const noexcept;
    std::size_t cp_length() const noexcept;
    std::size_t batch_size() const noexcept;
    void set_timing_enabled(bool enabled) noexcept;
    ComputeOperationTiming timing() const noexcept;

    // Input/output layout is batch-major. Each time-domain symbol includes CP.
    void demodulate(
        const std::vector<std::complex<float>>& time_with_cp,
        std::vector<std::complex<float>>& frequency);

    // Runs CP removal, cuFFT and normalization but retains the result on the
    // device for a following CUDA receive stage.
    void demodulate_device(
        const std::vector<std::complex<float>>& time_with_cp);
    const void* device_frequency_data() const noexcept;

    void modulate(
        const std::vector<std::complex<float>>& frequency,
        std::vector<std::complex<float>>& time_with_cp);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace openisac
