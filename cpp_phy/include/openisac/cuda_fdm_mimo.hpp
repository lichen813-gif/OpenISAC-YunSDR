#pragma once

#include "openisac/compute_backend.hpp"

#include <complex>
#include <cstddef>
#include <memory>
#include <vector>

namespace openisac {

// Retains the compact four-port FDM payload receive vectors and effective
// channel matrices on the CUDA device between control-header and payload
// decoding. The cuFFT grid is borrowed from CudaOfdmBatch.
class CudaFdmMimoFrame {
public:
    CudaFdmMimoFrame(
        std::size_t fft_size,
        std::size_t ports,
        std::size_t spatial_rank,
        std::size_t phase_reference_count,
        std::size_t pilot_count,
        std::size_t control_count,
        std::size_t payload_count);
    ~CudaFdmMimoFrame();

    CudaFdmMimoFrame(const CudaFdmMimoFrame&) = delete;
    CudaFdmMimoFrame& operator=(const CudaFdmMimoFrame&) = delete;
    CudaFdmMimoFrame(CudaFdmMimoFrame&&) noexcept;
    CudaFdmMimoFrame& operator=(CudaFdmMimoFrame&&) noexcept;

    void gather_phase_references(
        const void* device_frequency_batch,
        const std::vector<std::uint16_t>& phase_reference_fft_indices,
        std::vector<std::complex<float>>& sparse_grid);

    void compute_pilot_noise_powers(
        const void* device_frequency_batch,
        const FdmMimoFrameRequest& request,
        float phase_intercept_radians,
        float phase_slope_radians_per_subcarrier,
        std::vector<float>& powers);

    void prepare_payload_and_control(
        const void* device_frequency_batch,
        const FdmMimoFrameRequest& request,
        float phase_intercept_radians,
        float phase_slope_radians_per_subcarrier,
        float noise_variance,
        std::vector<float>& control_llrs);

    const void* device_payload_received() const noexcept;
    const void* device_payload_channels() const noexcept;
    std::size_t payload_count() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace openisac
