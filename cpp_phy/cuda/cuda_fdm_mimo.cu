#include "openisac/cuda_fdm_mimo.hpp"

#include <cuda_runtime.h>
#include <cufft.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace openisac {
namespace {

constexpr std::size_t maximum_ports = 8u;

void require_cuda(cudaError_t status, const char* operation) {
    if (status != cudaSuccess) {
        throw std::runtime_error(
            std::string(operation) + ": " + cudaGetErrorString(status));
    }
}

unsigned grid_size(std::size_t values, unsigned block_size) {
    return static_cast<unsigned>((values + block_size - 1u) / block_size);
}

struct TargetMap {
    std::uint16_t time = 0u;
    std::uint16_t fft = 0u;
    std::uint16_t left[maximum_ports]{};
    std::uint16_t right[maximum_ports]{};
    float fraction[maximum_ports]{};
};

__device__ cufftComplex add(cufftComplex left, cufftComplex right) {
    return {left.x + right.x, left.y + right.y};
}

__device__ cufftComplex subtract(cufftComplex left, cufftComplex right) {
    return {left.x - right.x, left.y - right.y};
}

__device__ cufftComplex multiply(cufftComplex left, cufftComplex right) {
    return {
        left.x * right.x - left.y * right.y,
        left.x * right.y + left.y * right.x};
}

__device__ cufftComplex conjugate(cufftComplex value) {
    return {value.x, -value.y};
}

__device__ cufftComplex scale(cufftComplex value, float factor) {
    return {value.x * factor, value.y * factor};
}

__device__ cufftComplex divide(cufftComplex numerator, cufftComplex denominator) {
    const float power = fmaxf(
        denominator.x * denominator.x + denominator.y * denominator.y,
        1.0e-20f);
    return {
        (numerator.x * denominator.x + numerator.y * denominator.y) / power,
        (numerator.y * denominator.x - numerator.x * denominator.y) / power};
}

__device__ int centered_index(std::uint16_t fft, std::size_t fft_size) {
    return fft < fft_size / 2u
        ? static_cast<int>(fft)
        : static_cast<int>(fft) - static_cast<int>(fft_size);
}

__device__ cufftComplex corrected_grid_value(
    const cufftComplex* frequency,
    std::size_t fft_size,
    std::size_t ports,
    std::size_t time,
    std::size_t rx,
    std::uint16_t fft,
    float intercept,
    float slope) {
    cufftComplex value = frequency[(time * ports + rx) * fft_size + fft];
    if (time == 0u) {
        return value;
    }
    const float phase = -(
        intercept + slope * static_cast<float>(centered_index(fft, fft_size)));
    const cufftComplex correction{cosf(phase), sinf(phase)};
    return multiply(value, correction);
}

__device__ cufftComplex physical_channel(
    const cufftComplex* frequency,
    const TargetMap& target,
    const std::uint16_t* pilot_fft,
    const cufftComplex* pilot_known,
    std::size_t pilot_count,
    std::size_t fft_size,
    std::size_t ports,
    std::size_t rx,
    std::size_t tx,
    std::size_t sample_time,
    float intercept,
    float slope) {
    const std::size_t left = target.left[tx];
    const std::size_t right = target.right[tx];
    const auto left_value = divide(
        corrected_grid_value(
            frequency, fft_size, ports, sample_time, rx, pilot_fft[left],
            intercept, slope),
        pilot_known[sample_time * pilot_count + left]);
    const auto right_value = divide(
        corrected_grid_value(
            frequency, fft_size, ports, sample_time, rx, pilot_fft[right],
            intercept, slope),
        pilot_known[sample_time * pilot_count + right]);
    return add(
        left_value,
        scale(subtract(right_value, left_value), target.fraction[tx]));
}

__global__ void gather_phase_kernel(
    const cufftComplex* frequency,
    const std::uint16_t* phase_fft,
    cufftComplex* sparse,
    std::size_t reference_count,
    std::size_t fft_size,
    std::size_t ports) {
    const std::size_t index = static_cast<std::size_t>(blockIdx.x) * blockDim.x +
        threadIdx.x;
    const std::size_t count = 2u * reference_count * ports;
    if (index >= count) return;
    const std::size_t rx = index % ports;
    const std::size_t reference = (index / ports) % reference_count;
    const std::size_t time = index / (ports * reference_count);
    sparse[index] = frequency[
        (time * ports + rx) * fft_size + phase_fft[reference]];
}

__global__ void pilot_noise_powers(
    const cufftComplex* frequency,
    const std::uint16_t* pilot_fft,
    const cufftComplex* pilot_known,
    float* powers,
    std::size_t pilot_count,
    std::size_t fft_size,
    std::size_t ports,
    float intercept,
    float slope) {
    const std::size_t index = static_cast<std::size_t>(blockIdx.x) * blockDim.x +
        threadIdx.x;
    const std::size_t count = pilot_count * ports;
    if (index >= count) return;
    const std::size_t pilot = index / ports;
    const std::size_t rx = index % ports;
    const auto first = corrected_grid_value(
        frequency, fft_size, ports, 0u, rx, pilot_fft[pilot], intercept, slope);
    const auto second = corrected_grid_value(
        frequency, fft_size, ports, 1u, rx, pilot_fft[pilot], intercept, slope);
    const auto predicted = multiply(
        divide(first, pilot_known[pilot]),
        pilot_known[pilot_count + pilot]);
    const auto residual = subtract(second, predicted);
    powers[index] = 0.5f *
        (residual.x * residual.x + residual.y * residual.y);
}

__device__ cufftComplex dft_4x2(std::size_t tx, std::size_t layer) {
    constexpr float half = 0.5f;
    if (layer == 0u) return {half, 0.0f};
    if (tx == 0u) return {half, 0.0f};
    if (tx == 1u) return {0.0f, half};
    if (tx == 2u) return {-half, 0.0f};
    return {0.0f, -half};
}

__global__ void prepare_payload_resources(
    const cufftComplex* frequency,
    const TargetMap* targets,
    const std::uint16_t* pilot_fft,
    const cufftComplex* pilot_known,
    cufftComplex* received,
    cufftComplex* channels,
    cufftComplex* history,
    std::size_t payload_count,
    std::size_t pilot_count,
    std::size_t fft_size,
    std::size_t ports,
    std::size_t rank,
    float intercept,
    float slope,
    bool average_symbols,
    bool reuse_history,
    float alpha) {
    const std::size_t resource =
        static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (resource >= payload_count) return;
    const TargetMap target = targets[resource];
    for (std::size_t rx = 0u; rx < ports; ++rx) {
        received[resource * ports + rx] = corrected_grid_value(
            frequency, fft_size, ports, target.time, rx, target.fft,
            intercept, slope);
        cufftComplex physical[4];
        for (std::size_t tx = 0u; tx < ports; ++tx) {
            auto estimate = physical_channel(
                frequency, target, pilot_fft, pilot_known, pilot_count,
                fft_size, ports, rx, tx, target.time, intercept, slope);
            if (average_symbols) {
                const auto first = physical_channel(
                    frequency, target, pilot_fft, pilot_known, pilot_count,
                    fft_size, ports, rx, tx, 0u, intercept, slope);
                const auto second = physical_channel(
                    frequency, target, pilot_fft, pilot_known, pilot_count,
                    fft_size, ports, rx, tx, 1u, intercept, slope);
                estimate = scale(add(first, second), 0.5f);
            }
            physical[tx] = estimate;
        }
        for (std::size_t layer = 0u; layer < rank; ++layer) {
            cufftComplex effective{0.0f, 0.0f};
            if (rank == 2u && ports == 4u) {
                for (std::size_t tx = 0u; tx < 4u; ++tx) {
                    effective = add(
                        effective, multiply(physical[tx], dft_4x2(tx, layer)));
                }
            } else {
                effective = physical[layer];
            }
            const std::size_t output =
                (resource * ports + rx) * rank + layer;
            if (reuse_history) {
                effective = add(
                    scale(effective, alpha),
                    scale(history[output], 1.0f - alpha));
            }
            history[output] = effective;
            channels[output] = effective;
        }
    }
}

__global__ void prepare_control_resources(
    const cufftComplex* frequency,
    const TargetMap* targets,
    const std::uint16_t* pilot_fft,
    const cufftComplex* pilot_known,
    cufftComplex* history,
    float* llrs,
    std::size_t control_count,
    std::size_t pilot_count,
    std::size_t fft_size,
    std::size_t ports,
    float intercept,
    float slope,
    float noise_variance,
    bool average_symbols,
    bool reuse_history,
    float alpha) {
    const std::size_t resource =
        static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (resource >= control_count) return;
    const TargetMap target = targets[resource];
    cufftComplex matched{0.0f, 0.0f};
    float power = 0.0f;
    for (std::size_t rx = 0u; rx < ports; ++rx) {
        auto channel = physical_channel(
            frequency, target, pilot_fft, pilot_known, pilot_count,
            fft_size, ports, rx, 0u, 0u, intercept, slope);
        if (average_symbols) {
            const auto first = channel;
            const auto second = physical_channel(
                frequency, target, pilot_fft, pilot_known, pilot_count,
                fft_size, ports, rx, 0u, 1u, intercept, slope);
            channel = scale(add(first, second), 0.5f);
        }
        const std::size_t history_index = resource * ports + rx;
        if (reuse_history) {
            channel = add(
                scale(channel, alpha),
                scale(history[history_index], 1.0f - alpha));
        }
        history[history_index] = channel;
        const auto sample = corrected_grid_value(
            frequency, fft_size, ports, 0u, rx, target.fft, intercept, slope);
        matched = add(matched, multiply(conjugate(channel), sample));
        power += channel.x * channel.x + channel.y * channel.y;
    }
    power = fmaxf(power, 1.0e-12f);
    const auto symbol = scale(matched, 1.0f / power);
    const float variance = fmaxf(noise_variance / power, 1.0e-12f);
    constexpr float qpsk_llr_scale = 2.8284271247461903f;
    llrs[resource * 2u] = qpsk_llr_scale * symbol.x / variance;
    llrs[resource * 2u + 1u] = qpsk_llr_scale * symbol.y / variance;
}

int centered_host(std::uint16_t fft, std::size_t fft_size) {
    return fft < fft_size / 2u
        ? static_cast<int>(fft)
        : static_cast<int>(fft) - static_cast<int>(fft_size);
}

}  // namespace

struct CudaFdmMimoFrame::Impl {
    std::size_t fft_size;
    std::size_t ports;
    std::size_t rank;
    std::size_t phase_count;
    std::size_t pilot_count;
    std::size_t control_count;
    std::size_t payload_count;
    std::uint16_t* phase_fft = nullptr;
    cufftComplex* sparse_phase = nullptr;
    std::uint16_t* pilot_fft = nullptr;
    cufftComplex* pilot_known = nullptr;
    float* noise_powers = nullptr;
    TargetMap* control_maps = nullptr;
    TargetMap* payload_maps = nullptr;
    cufftComplex* payload_received = nullptr;
    cufftComplex* payload_channels = nullptr;
    cufftComplex* payload_history = nullptr;
    cufftComplex* control_history = nullptr;
    float* control_llrs = nullptr;
    std::vector<std::uint16_t> cached_phase;
    std::vector<std::uint16_t> cached_pilots;
    std::vector<std::complex<float>> cached_known;
    std::vector<std::uint16_t> cached_control;
    std::vector<std::uint8_t> cached_payload_time;
    std::vector<std::uint16_t> cached_payload_fft;

    Impl(
        std::size_t fft,
        std::size_t port_count,
        std::size_t stream_count,
        std::size_t phase_reference_count,
        std::size_t pilots,
        std::size_t controls,
        std::size_t payloads)
        : fft_size(fft), ports(port_count), rank(stream_count),
          phase_count(phase_reference_count), pilot_count(pilots),
          control_count(controls), payload_count(payloads) {
        if (fft_size == 0u || ports != 4u ||
            (rank != 2u && rank != 4u) || phase_count < 2u ||
            pilot_count < ports || control_count == 0u || payload_count == 0u) {
            throw std::invalid_argument("invalid CUDA FDM MIMO frame shape");
        }
        try {
            require_cuda(cudaMalloc(reinterpret_cast<void**>(&phase_fft),
                phase_count * sizeof(std::uint16_t)), "cudaMalloc FDM phase indices");
            require_cuda(cudaMalloc(reinterpret_cast<void**>(&sparse_phase),
                2u * phase_count * ports * sizeof(cufftComplex)),
                "cudaMalloc FDM sparse phase");
            require_cuda(cudaMalloc(reinterpret_cast<void**>(&pilot_fft),
                pilot_count * sizeof(std::uint16_t)), "cudaMalloc FDM pilot indices");
            require_cuda(cudaMalloc(reinterpret_cast<void**>(&pilot_known),
                2u * pilot_count * sizeof(cufftComplex)),
                "cudaMalloc FDM known pilots");
            require_cuda(cudaMalloc(reinterpret_cast<void**>(&noise_powers),
                pilot_count * ports * sizeof(float)), "cudaMalloc FDM noise powers");
            require_cuda(cudaMalloc(reinterpret_cast<void**>(&control_maps),
                control_count * sizeof(TargetMap)), "cudaMalloc FDM control maps");
            require_cuda(cudaMalloc(reinterpret_cast<void**>(&payload_maps),
                payload_count * sizeof(TargetMap)), "cudaMalloc FDM payload maps");
            require_cuda(cudaMalloc(reinterpret_cast<void**>(&payload_received),
                payload_count * ports * sizeof(cufftComplex)),
                "cudaMalloc FDM payload received");
            const std::size_t channel_values = payload_count * ports * rank;
            require_cuda(cudaMalloc(reinterpret_cast<void**>(&payload_channels),
                channel_values * sizeof(cufftComplex)),
                "cudaMalloc FDM payload channels");
            require_cuda(cudaMalloc(reinterpret_cast<void**>(&payload_history),
                channel_values * sizeof(cufftComplex)),
                "cudaMalloc FDM payload history");
            require_cuda(cudaMalloc(reinterpret_cast<void**>(&control_history),
                control_count * ports * sizeof(cufftComplex)),
                "cudaMalloc FDM control history");
            require_cuda(cudaMalloc(reinterpret_cast<void**>(&control_llrs),
                control_count * 2u * sizeof(float)), "cudaMalloc FDM control LLRs");
        } catch (...) {
            release();
            throw;
        }
    }

    ~Impl() { release(); }

    void release() noexcept {
        if (control_llrs != nullptr) cudaFree(control_llrs);
        if (control_history != nullptr) cudaFree(control_history);
        if (payload_history != nullptr) cudaFree(payload_history);
        if (payload_channels != nullptr) cudaFree(payload_channels);
        if (payload_received != nullptr) cudaFree(payload_received);
        if (payload_maps != nullptr) cudaFree(payload_maps);
        if (control_maps != nullptr) cudaFree(control_maps);
        if (noise_powers != nullptr) cudaFree(noise_powers);
        if (pilot_known != nullptr) cudaFree(pilot_known);
        if (pilot_fft != nullptr) cudaFree(pilot_fft);
        if (sparse_phase != nullptr) cudaFree(sparse_phase);
        if (phase_fft != nullptr) cudaFree(phase_fft);
    }

    std::vector<TargetMap> build_maps(
        const std::vector<std::uint8_t>& times,
        const std::vector<std::uint16_t>& targets) const {
        if (times.size() != targets.size()) {
            throw std::invalid_argument("FDM target time/FFT vectors differ");
        }
        std::vector<std::vector<std::pair<int, std::uint16_t>>> by_tx(ports);
        for (std::size_t pilot = 0u; pilot < cached_pilots.size(); ++pilot) {
            by_tx[pilot % ports].push_back({
                centered_host(cached_pilots[pilot], fft_size),
                static_cast<std::uint16_t>(pilot)});
        }
        for (auto& points : by_tx) {
            std::sort(points.begin(), points.end());
            if (points.empty()) {
                throw std::invalid_argument("FDM pilot map misses a Tx port");
            }
        }
        std::vector<TargetMap> maps(targets.size());
        for (std::size_t resource = 0u; resource < targets.size(); ++resource) {
            auto& map = maps[resource];
            map.time = times[resource];
            map.fft = targets[resource];
            const int target = centered_host(targets[resource], fft_size);
            for (std::size_t tx = 0u; tx < ports; ++tx) {
                const auto& points = by_tx[tx];
                const auto upper = std::upper_bound(
                    points.begin(), points.end(),
                    std::make_pair(target, std::uint16_t{0xFFFFu}));
                std::size_t left = 0u;
                std::size_t right = 0u;
                int left_frequency = 0;
                int right_frequency = 0;
                if (upper == points.begin()) {
                    left = points.size() - 1u;
                    right = 0u;
                    left_frequency = points[left].first - static_cast<int>(fft_size);
                    right_frequency = points[right].first;
                } else if (upper == points.end()) {
                    left = points.size() - 1u;
                    right = 0u;
                    left_frequency = points[left].first;
                    right_frequency = points[right].first + static_cast<int>(fft_size);
                } else {
                    right = static_cast<std::size_t>(upper - points.begin());
                    left = right - 1u;
                    left_frequency = points[left].first;
                    right_frequency = points[right].first;
                }
                map.left[tx] = points[left].second;
                map.right[tx] = points[right].second;
                map.fraction[tx] =
                    static_cast<float>(target - left_frequency) /
                    static_cast<float>(right_frequency - left_frequency);
            }
        }
        return maps;
    }

    void update_topology(const FdmMimoFrameRequest& request) {
        const auto& phases = *request.phase_reference_fft_indices;
        const auto& pilots = *request.pilot_fft_indices;
        const auto& reference = *request.pilot_reference_grid;
        const auto& controls = *request.control_fft_indices;
        const auto& payload_times = *request.payload_time_indices;
        const auto& payload_ffts = *request.payload_fft_indices;
        if (phases.size() != phase_count || pilots.size() != pilot_count ||
            controls.size() != control_count ||
            payload_times.size() != payload_count ||
            payload_ffts.size() != payload_count ||
            reference.size() != 2u * fft_size * ports) {
            throw std::invalid_argument("CUDA FDM topology shape mismatch");
        }
        std::vector<std::complex<float>> known(2u * pilot_count);
        for (std::size_t pilot = 0u; pilot < pilot_count; ++pilot) {
            const std::size_t tx = pilot % ports;
            for (std::size_t time = 0u; time < 2u; ++time) {
                const auto value = reference[
                    (time * fft_size + pilots[pilot]) * ports + tx];
                known[time * pilot_count + pilot] = value;
            }
        }
        const bool phase_changed = phases != cached_phase;
        const bool pilot_changed = pilots != cached_pilots || known != cached_known;
        const bool targets_changed = controls != cached_control ||
            payload_times != cached_payload_time || payload_ffts != cached_payload_fft;
        if (phase_changed) {
            cached_phase = phases;
            require_cuda(cudaMemcpy(phase_fft, phases.data(),
                phase_count * sizeof(std::uint16_t), cudaMemcpyHostToDevice),
                "cudaMemcpy FDM phase indices");
        }
        if (pilot_changed) {
            cached_pilots = pilots;
            cached_known = known;
            require_cuda(cudaMemcpy(pilot_fft, pilots.data(),
                pilot_count * sizeof(std::uint16_t), cudaMemcpyHostToDevice),
                "cudaMemcpy FDM pilot indices");
            require_cuda(cudaMemcpy(pilot_known, known.data(),
                known.size() * sizeof(cufftComplex), cudaMemcpyHostToDevice),
                "cudaMemcpy FDM known pilots");
        }
        if (pilot_changed || targets_changed) {
            cached_control = controls;
            cached_payload_time = payload_times;
            cached_payload_fft = payload_ffts;
            std::vector<std::uint8_t> control_times(control_count, 0u);
            const auto control = build_maps(control_times, controls);
            const auto payload = build_maps(payload_times, payload_ffts);
            require_cuda(cudaMemcpy(control_maps, control.data(),
                control.size() * sizeof(TargetMap), cudaMemcpyHostToDevice),
                "cudaMemcpy FDM control maps");
            require_cuda(cudaMemcpy(payload_maps, payload.data(),
                payload.size() * sizeof(TargetMap), cudaMemcpyHostToDevice),
                "cudaMemcpy FDM payload maps");
        }
    }
};

CudaFdmMimoFrame::CudaFdmMimoFrame(
    std::size_t fft_size,
    std::size_t ports,
    std::size_t spatial_rank,
    std::size_t phase_reference_count,
    std::size_t pilot_count,
    std::size_t control_count,
    std::size_t payload_count)
    : impl_(new Impl(
          fft_size, ports, spatial_rank, phase_reference_count, pilot_count,
          control_count, payload_count)) {}

CudaFdmMimoFrame::~CudaFdmMimoFrame() = default;
CudaFdmMimoFrame::CudaFdmMimoFrame(CudaFdmMimoFrame&&) noexcept = default;
CudaFdmMimoFrame& CudaFdmMimoFrame::operator=(CudaFdmMimoFrame&&) noexcept = default;

void CudaFdmMimoFrame::gather_phase_references(
    const void* device_frequency_batch,
    const std::vector<std::uint16_t>& phase_reference_fft_indices,
    std::vector<std::complex<float>>& sparse_grid) {
    if (device_frequency_batch == nullptr ||
        phase_reference_fft_indices.size() != impl_->phase_count) {
        throw std::invalid_argument("CUDA FDM phase-reference shape mismatch");
    }
    if (phase_reference_fft_indices != impl_->cached_phase) {
        impl_->cached_phase = phase_reference_fft_indices;
        require_cuda(cudaMemcpy(
            impl_->phase_fft, phase_reference_fft_indices.data(),
            impl_->phase_count * sizeof(std::uint16_t), cudaMemcpyHostToDevice),
            "cudaMemcpy FDM phase indices");
    }
    const std::size_t count = 2u * impl_->phase_count * impl_->ports;
    constexpr unsigned block = 128u;
    gather_phase_kernel<<<grid_size(count, block), block>>>(
        static_cast<const cufftComplex*>(device_frequency_batch),
        impl_->phase_fft, impl_->sparse_phase, impl_->phase_count,
        impl_->fft_size, impl_->ports);
    require_cuda(cudaGetLastError(), "CUDA FDM phase gather kernel");
    sparse_grid.resize(count);
    require_cuda(cudaMemcpy(
        sparse_grid.data(), impl_->sparse_phase,
        count * sizeof(cufftComplex), cudaMemcpyDeviceToHost),
        "cudaMemcpy FDM sparse phase");
}

void CudaFdmMimoFrame::compute_pilot_noise_powers(
    const void* device_frequency_batch,
    const FdmMimoFrameRequest& request,
    float phase_intercept_radians,
    float phase_slope_radians_per_subcarrier,
    std::vector<float>& powers) {
    impl_->update_topology(request);
    const std::size_t count = impl_->pilot_count * impl_->ports;
    constexpr unsigned block = 128u;
    pilot_noise_powers<<<grid_size(count, block), block>>>(
        static_cast<const cufftComplex*>(device_frequency_batch),
        impl_->pilot_fft, impl_->pilot_known, impl_->noise_powers,
        impl_->pilot_count, impl_->fft_size, impl_->ports,
        phase_intercept_radians, phase_slope_radians_per_subcarrier);
    require_cuda(cudaGetLastError(), "CUDA FDM pilot-noise kernel");
    powers.resize(count);
    require_cuda(cudaMemcpy(
        powers.data(), impl_->noise_powers, count * sizeof(float),
        cudaMemcpyDeviceToHost), "cudaMemcpy FDM noise powers");
}

void CudaFdmMimoFrame::prepare_payload_and_control(
    const void* device_frequency_batch,
    const FdmMimoFrameRequest& request,
    float phase_intercept_radians,
    float phase_slope_radians_per_subcarrier,
    float noise_variance,
    std::vector<float>& control_llrs) {
    impl_->update_topology(request);
    constexpr unsigned block = 128u;
    prepare_payload_resources<<<grid_size(impl_->payload_count, block), block>>>(
        static_cast<const cufftComplex*>(device_frequency_batch),
        impl_->payload_maps, impl_->pilot_fft, impl_->pilot_known,
        impl_->payload_received, impl_->payload_channels, impl_->payload_history,
        impl_->payload_count, impl_->pilot_count, impl_->fft_size, impl_->ports,
        impl_->rank, phase_intercept_radians,
        phase_slope_radians_per_subcarrier,
        request.average_intra_frame_csi, request.reuse_csi_history,
        request.csi_smoothing_alpha);
    require_cuda(cudaGetLastError(), "CUDA FDM payload preparation kernel");
    prepare_control_resources<<<grid_size(impl_->control_count, block), block>>>(
        static_cast<const cufftComplex*>(device_frequency_batch),
        impl_->control_maps, impl_->pilot_fft, impl_->pilot_known,
        impl_->control_history, impl_->control_llrs, impl_->control_count,
        impl_->pilot_count, impl_->fft_size, impl_->ports,
        phase_intercept_radians, phase_slope_radians_per_subcarrier,
        noise_variance, request.average_intra_frame_csi,
        request.reuse_csi_history, request.csi_smoothing_alpha);
    require_cuda(cudaGetLastError(), "CUDA FDM control preparation kernel");
    control_llrs.resize(impl_->control_count * 2u);
    require_cuda(cudaMemcpy(
        control_llrs.data(), impl_->control_llrs,
        control_llrs.size() * sizeof(float), cudaMemcpyDeviceToHost),
        "cudaMemcpy FDM control LLRs");
}

const void* CudaFdmMimoFrame::device_payload_received() const noexcept {
    return impl_->payload_received;
}

const void* CudaFdmMimoFrame::device_payload_channels() const noexcept {
    return impl_->payload_channels;
}

std::size_t CudaFdmMimoFrame::payload_count() const noexcept {
    return impl_->payload_count;
}

}  // namespace openisac
