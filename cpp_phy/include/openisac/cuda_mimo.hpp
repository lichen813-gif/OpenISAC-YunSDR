#pragma once

#include "openisac/compute_backend.hpp"
#include "openisac/mimo2x2.hpp"

#include <complex>
#include <cstddef>
#include <memory>
#include <vector>

namespace openisac {

// Fixed-shape batched linear detector for 1..8 spatial streams. Channel
// matrices are contiguous row-major [batch][receive_port][stream].
class CudaMimoBatch {
public:
    CudaMimoBatch(
        std::size_t streams,
        std::size_t receive_ports,
        std::size_t batch_size);
    ~CudaMimoBatch();

    CudaMimoBatch(const CudaMimoBatch&) = delete;
    CudaMimoBatch& operator=(const CudaMimoBatch&) = delete;
    CudaMimoBatch(CudaMimoBatch&&) noexcept;
    CudaMimoBatch& operator=(CudaMimoBatch&&) noexcept;

    std::size_t streams() const noexcept;
    std::size_t receive_ports() const noexcept;
    std::size_t batch_size() const noexcept;
    void set_timing_enabled(bool enabled) noexcept;
    ComputeOperationTiming timing() const noexcept;

    void detect(
        const std::vector<std::complex<float>>& received,
        const std::vector<std::complex<float>>& channels,
        float noise_variance,
        LinearDetector detector,
        std::vector<std::complex<float>>& symbols,
        std::vector<float>& predicted_mse);

    // Fuses max-log square-QAM soft demapping into the detector kernel.
    // ldpc_blocks=0 returns [batch][stream][bit]. A nonzero value returns
    // blocks*1008 decoder-order LLRs after deinterleaving and descrambling.
    void detect_soft(
        const std::vector<std::complex<float>>& received,
        const std::vector<std::complex<float>>& channels,
        float noise_variance,
        LinearDetector detector,
        unsigned bits_per_symbol,
        std::size_t ldpc_blocks,
        bool return_equalized,
        std::vector<std::complex<float>>& symbols,
        std::vector<float>& predicted_mse,
        std::vector<float>& soft_bits);

    // Uses compact [batch][rx] and [batch][rx][stream] inputs already resident
    // on the current CUDA device. Output semantics match detect_soft().
    void detect_soft_device(
        const void* device_received,
        const void* device_channels,
        float noise_variance,
        LinearDetector detector,
        unsigned bits_per_symbol,
        std::size_t ldpc_blocks,
        bool return_equalized,
        std::vector<std::complex<float>>& symbols,
        std::vector<float>& predicted_mse,
        std::vector<float>& soft_bits);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace openisac
