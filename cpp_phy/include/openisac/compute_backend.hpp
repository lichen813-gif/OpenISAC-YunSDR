#pragma once

#include "openisac/mimo2x2.hpp"

#include <complex>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace openisac {

struct ComputeOperationTiming {
    double h2d_us = 0.0;
    double kernel_us = 0.0;
    double d2h_us = 0.0;
};

struct ComputeBackendTiming {
    ComputeOperationTiming ofdm{};
    ComputeOperationTiming mimo{};
};

// Optional device-resident four-port FDM receive request. The vectors are
// borrowed only for the duration of prepare_fdm_mimo_frame().
struct FdmMimoFrameRequest {
    std::size_t fft_size = 0u;
    std::size_t cp_length = 0u;
    std::size_t samples_per_symbol = 0u;
    std::size_t ports = 0u;
    unsigned spatial_rank = 0u;
    const std::vector<std::complex<float>>* time_with_cp = nullptr;
    const std::vector<std::uint16_t>* phase_reference_fft_indices = nullptr;
    const std::vector<std::uint16_t>* pilot_fft_indices = nullptr;
    const std::vector<std::complex<float>>* pilot_reference_grid = nullptr;
    const std::vector<std::uint16_t>* control_fft_indices = nullptr;
    const std::vector<std::uint8_t>* payload_time_indices = nullptr;
    const std::vector<std::uint16_t>* payload_fft_indices = nullptr;
    bool average_intra_frame_csi = true;
    bool reuse_csi_history = false;
    float csi_smoothing_alpha = 1.0f;
};

struct FdmMimoFrameFrontend {
    float estimated_sfo_ppm = 0.0f;
    float residual_sfo_ppm = 0.0f;
    float phase_intercept_radians = 0.0f;
    float phase_slope_radians_per_subcarrier = 0.0f;
    float noise_variance = 0.0f;
    std::vector<float> control_llrs;
};

// Optional frame-level compute backend. Inputs are contiguous batches so a
// GPU implementation performs one transfer and one launch per PHY stage, not
// per resource element. A null backend keeps the established CPU path.
class PhyComputeBackend {
public:
    virtual ~PhyComputeBackend() = default;
    virtual std::string_view name() const noexcept = 0;

    virtual void set_timing_enabled(bool) noexcept {}
    virtual ComputeBackendTiming timing() const noexcept { return {}; }

    virtual void ofdm_modulate_batch(
        std::size_t fft_size,
        std::size_t cp_length,
        std::size_t batch_size,
        const std::vector<std::complex<float>>& frequency,
        std::vector<std::complex<float>>& time_with_cp) = 0;

    virtual void ofdm_demodulate_batch(
        std::size_t fft_size,
        std::size_t cp_length,
        std::size_t batch_size,
        const std::vector<std::complex<float>>& time_with_cp,
        std::vector<std::complex<float>>& frequency) = 0;

    // Returns true when the backend retained the FDM FFT grid, channel
    // estimates and compact payload inputs on the device. A false return keeps
    // the established portable CPU/host-staged path.
    virtual bool prepare_fdm_mimo_frame(
        const FdmMimoFrameRequest& request,
        FdmMimoFrameFrontend& frontend) {
        (void)request;
        (void)frontend;
        return false;
    }

    virtual void detect_prepared_fdm_mimo(
        unsigned bits_per_symbol,
        std::size_t ldpc_blocks,
        float noise_variance,
        float mmse_regularization_scale,
        bool return_equalized,
        std::vector<std::complex<float>>& symbols,
        std::vector<float>& predicted_mse,
        std::vector<float>& soft_bits) {
        (void)bits_per_symbol;
        (void)ldpc_blocks;
        (void)noise_variance;
        (void)mmse_regularization_scale;
        (void)return_equalized;
        (void)symbols;
        (void)predicted_mse;
        (void)soft_bits;
        throw std::runtime_error(
            "compute backend has no prepared FDM MIMO frame");
    }

    // received: [batch][receive_port]
    // channels: [batch][receive_port][stream], compact row-major
    // symbols/MSE: [batch][stream]. With ldpc_blocks=0, soft_bits uses raw
    // [batch][stream][bit] ordering. A nonzero ldpc_blocks requests exactly
    // blocks*1008 decoder-order LLRs after 21x48 deinterleaving and soft
    // descrambling, allowing the GPU to feed LDPC without a CPU LLR pass.
    virtual void detect_mimo_batch(
        std::size_t streams,
        std::size_t receive_ports,
        std::size_t batch_size,
        const std::vector<std::complex<float>>& received,
        const std::vector<std::complex<float>>& channels,
        float noise_variance,
        LinearDetector detector,
        unsigned bits_per_symbol,
        std::size_t ldpc_blocks,
        bool return_equalized,
        std::vector<std::complex<float>>& symbols,
        std::vector<float>& predicted_mse,
        std::vector<float>& soft_bits) = 0;
};

}  // namespace openisac
