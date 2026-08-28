#include "openisac/cuda_backend.hpp"

#include "openisac/cuda_fdm_mimo.hpp"
#include "openisac/cuda_mimo.hpp"
#include "openisac/cuda_ofdm.hpp"
#include "openisac/sampling_offset.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <tuple>

namespace openisac {
namespace {

class CudaComputeBackend final : public PhyComputeBackend {
public:
    CudaComputeBackend() {
        if (!cuda_device_available()) {
            throw std::runtime_error("no CUDA device is available");
        }
    }

    std::string_view name() const noexcept override { return "cuda"; }

    void set_timing_enabled(bool enabled) noexcept override {
        timing_enabled_ = enabled;
        if (!enabled) timing_ = {};
    }

    ComputeBackendTiming timing() const noexcept override { return timing_; }

    void ofdm_modulate_batch(
        std::size_t fft_size,
        std::size_t cp_length,
        std::size_t batch_size,
        const std::vector<std::complex<float>>& frequency,
        std::vector<std::complex<float>>& time_with_cp) override {
        auto& backend = ofdm_backend(fft_size, cp_length, batch_size);
        backend.set_timing_enabled(timing_enabled_);
        backend.modulate(frequency, time_with_cp);
        timing_.ofdm = backend.timing();
    }

    void ofdm_demodulate_batch(
        std::size_t fft_size,
        std::size_t cp_length,
        std::size_t batch_size,
        const std::vector<std::complex<float>>& time_with_cp,
        std::vector<std::complex<float>>& frequency) override {
        auto& backend = ofdm_backend(fft_size, cp_length, batch_size);
        backend.set_timing_enabled(timing_enabled_);
        backend.demodulate(time_with_cp, frequency);
        timing_.ofdm = backend.timing();
    }

    bool prepare_fdm_mimo_frame(
        const FdmMimoFrameRequest& request,
        FdmMimoFrameFrontend& frontend) override {
        if (request.ports != 4u ||
            (request.spatial_rank != 2u && request.spatial_rank != 4u) ||
            request.time_with_cp == nullptr ||
            request.phase_reference_fft_indices == nullptr ||
            request.pilot_fft_indices == nullptr ||
            request.pilot_reference_grid == nullptr ||
            request.control_fft_indices == nullptr ||
            request.payload_time_indices == nullptr ||
            request.payload_fft_indices == nullptr) {
            return false;
        }
        const std::size_t batch_size = 2u * request.ports;
        auto& ofdm = ofdm_backend(
            request.fft_size, request.cp_length, batch_size);
        ofdm.set_timing_enabled(timing_enabled_);
        ofdm.demodulate_device(*request.time_with_cp);
        timing_.ofdm = ofdm.timing();

        auto& frame = fdm_backend(request);
        std::vector<std::complex<float>> sparse_phase;
        frame.gather_phase_references(
            ofdm.device_frequency_data(),
            *request.phase_reference_fft_indices, sparse_phase);
        const auto phase = estimate_sfo_phase_slope_sparse(
            sparse_phase, *request.phase_reference_fft_indices,
            request.fft_size, request.ports, request.samples_per_symbol);
        std::vector<float> noise_powers;
        frame.compute_pilot_noise_powers(
            ofdm.device_frequency_data(), request,
            phase.intercept_radians, phase.slope_radians_per_subcarrier,
            noise_powers);
        const auto middle = noise_powers.begin() +
            static_cast<std::ptrdiff_t>(noise_powers.size() / 2u);
        std::nth_element(noise_powers.begin(), middle, noise_powers.end());
        constexpr float exponential_median_ratio = 0.6931471805599453f;
        const float noise_variance = std::clamp(
            *middle / exponential_median_ratio, 1.0e-8f, 1.0f);
        frame.prepare_payload_and_control(
            ofdm.device_frequency_data(), request,
            phase.intercept_radians, phase.slope_radians_per_subcarrier,
            noise_variance, frontend.control_llrs);
        frontend.estimated_sfo_ppm = phase.sfo_ppm;
        frontend.residual_sfo_ppm = 0.0f;
        frontend.phase_intercept_radians = phase.intercept_radians;
        frontend.phase_slope_radians_per_subcarrier =
            phase.slope_radians_per_subcarrier;
        frontend.noise_variance = noise_variance;
        prepared_fdm_ = &frame;
        prepared_rank_ = request.spatial_rank;
        return true;
    }

    void detect_prepared_fdm_mimo(
        unsigned bits_per_symbol,
        std::size_t ldpc_blocks,
        float noise_variance,
        float mmse_regularization_scale,
        bool return_equalized,
        std::vector<std::complex<float>>& symbols,
        std::vector<float>& predicted_mse,
        std::vector<float>& soft_bits) override {
        if (prepared_fdm_ == nullptr) {
            throw std::runtime_error("no CUDA FDM frame is prepared");
        }
        auto& mimo = mimo_backend(
            prepared_rank_, 4u, prepared_fdm_->payload_count());
        mimo.set_timing_enabled(timing_enabled_);
        mimo.detect_soft_device(
            prepared_fdm_->device_payload_received(),
            prepared_fdm_->device_payload_channels(),
            noise_variance * mmse_regularization_scale,
            LinearDetector::mmse, bits_per_symbol, ldpc_blocks,
            return_equalized, symbols, predicted_mse, soft_bits);
        timing_.mimo = mimo.timing();
        prepared_fdm_ = nullptr;
    }

    void detect_mimo_batch(
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
        std::vector<float>& soft_bits) override {
        auto& backend = mimo_backend(streams, receive_ports, batch_size);
        backend.set_timing_enabled(timing_enabled_);
        backend.detect_soft(
            received, channels, noise_variance, detector,
            bits_per_symbol, ldpc_blocks, return_equalized,
            symbols, predicted_mse, soft_bits);
        timing_.mimo = backend.timing();
    }

private:
    using OfdmKey = std::tuple<std::size_t, std::size_t, std::size_t>;
    using MimoKey = std::tuple<std::size_t, std::size_t, std::size_t>;
    using FdmKey = std::tuple<
        std::size_t, std::size_t, std::size_t, std::size_t,
        std::size_t, std::size_t, std::size_t>;

    CudaOfdmBatch& ofdm_backend(
        std::size_t fft_size,
        std::size_t cp_length,
        std::size_t batch_size) {
        const OfdmKey key{fft_size, cp_length, batch_size};
        auto& backend = ofdm_backends_[key];
        if (!backend) {
            backend = std::make_unique<CudaOfdmBatch>(
                fft_size, cp_length, batch_size);
        }
        return *backend;
    }

    CudaMimoBatch& mimo_backend(
        std::size_t streams,
        std::size_t receive_ports,
        std::size_t batch_size) {
        const MimoKey key{streams, receive_ports, batch_size};
        auto& backend = mimo_backends_[key];
        if (!backend) {
            backend = std::make_unique<CudaMimoBatch>(
                streams, receive_ports, batch_size);
        }
        return *backend;
    }

    CudaFdmMimoFrame& fdm_backend(const FdmMimoFrameRequest& request) {
        const FdmKey key{
            request.fft_size, request.ports, request.spatial_rank,
            request.phase_reference_fft_indices->size(),
            request.pilot_fft_indices->size(),
            request.control_fft_indices->size(),
            request.payload_time_indices->size()};
        auto& backend = fdm_backends_[key];
        if (!backend) {
            backend = std::make_unique<CudaFdmMimoFrame>(
                request.fft_size, request.ports, request.spatial_rank,
                request.phase_reference_fft_indices->size(),
                request.pilot_fft_indices->size(),
                request.control_fft_indices->size(),
                request.payload_time_indices->size());
        }
        return *backend;
    }

    std::map<OfdmKey, std::unique_ptr<CudaOfdmBatch>> ofdm_backends_;
    std::map<MimoKey, std::unique_ptr<CudaMimoBatch>> mimo_backends_;
    std::map<FdmKey, std::unique_ptr<CudaFdmMimoFrame>> fdm_backends_;
    ComputeBackendTiming timing_{};
    bool timing_enabled_ = false;
    CudaFdmMimoFrame* prepared_fdm_ = nullptr;
    std::size_t prepared_rank_ = 0u;
};

}  // namespace

std::unique_ptr<PhyComputeBackend> make_cuda_compute_backend() {
    return std::make_unique<CudaComputeBackend>();
}

}  // namespace openisac
