#include "openisac/cuda_ofdm.hpp"

#include <cuda_runtime.h>
#include <cufft.h>

#include <cmath>
#include <complex>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace openisac {
namespace {

void require_cuda(cudaError_t status, const char* operation) {
    if (status != cudaSuccess) {
        throw std::runtime_error(
            std::string(operation) + ": " + cudaGetErrorString(status));
    }
}

void require_cufft(cufftResult status, const char* operation) {
    if (status != CUFFT_SUCCESS) {
        throw std::runtime_error(
            std::string(operation) + " failed with cuFFT status " +
            std::to_string(static_cast<int>(status)));
    }
}

__global__ void scale_values(cufftComplex* values, std::size_t count, float scale) {
    const std::size_t index = static_cast<std::size_t>(blockIdx.x) * blockDim.x +
        threadIdx.x;
    if (index < count) {
        values[index].x *= scale;
        values[index].y *= scale;
    }
}

__global__ void scale_and_add_cp(
    const cufftComplex* frequency,
    cufftComplex* time_with_cp,
    std::size_t fft_size,
    std::size_t cp_length,
    std::size_t total_output,
    float scale) {
    const std::size_t index = static_cast<std::size_t>(blockIdx.x) * blockDim.x +
        threadIdx.x;
    if (index >= total_output) {
        return;
    }
    const std::size_t symbol_size = fft_size + cp_length;
    const std::size_t batch = index / symbol_size;
    const std::size_t offset = index % symbol_size;
    const std::size_t source = offset < cp_length
        ? fft_size - cp_length + offset
        : offset - cp_length;
    const cufftComplex value = frequency[batch * fft_size + source];
    time_with_cp[index] = {value.x * scale, value.y * scale};
}

unsigned grid_size(std::size_t values, unsigned block_size) {
    return static_cast<unsigned>((values + block_size - 1u) / block_size);
}

}  // namespace

struct CudaOfdmBatch::Impl {
    std::size_t fft_size = 0u;
    std::size_t cp_length = 0u;
    std::size_t batch_size = 0u;
    cufftHandle plan = 0;
    cufftComplex* fft_buffer = nullptr;
    cufftComplex* time_buffer = nullptr;
    cudaEvent_t timing_events[4]{};
    ComputeOperationTiming timing{};
    bool timing_enabled = false;

    Impl(std::size_t fft, std::size_t cp, std::size_t batch)
        : fft_size(fft), cp_length(cp), batch_size(batch) {
        if (fft == 0u || (fft & (fft - 1u)) != 0u || cp > fft ||
            batch == 0u || fft > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
            batch > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            throw std::invalid_argument("invalid CUDA OFDM batch shape");
        }
        static_assert(
            sizeof(std::complex<float>) == sizeof(cufftComplex),
            "std::complex<float> must match cufftComplex storage");
        const std::size_t fft_bytes =
            fft_size * batch_size * sizeof(cufftComplex);
        const std::size_t time_bytes =
            (fft_size + cp_length) * batch_size * sizeof(cufftComplex);
        require_cuda(
            cudaMalloc(reinterpret_cast<void**>(&fft_buffer), fft_bytes),
            "cudaMalloc FFT buffer");
        try {
            require_cuda(
                cudaMalloc(reinterpret_cast<void**>(&time_buffer), time_bytes),
                "cudaMalloc CP buffer");
            require_cufft(
                cufftPlan1d(
                    &plan, static_cast<int>(fft_size), CUFFT_C2C,
                    static_cast<int>(batch_size)),
                "cufftPlan1d");
            for (auto& event : timing_events) {
                require_cuda(cudaEventCreate(&event), "cudaEventCreate OFDM timing");
            }
        } catch (...) {
            for (auto& event : timing_events) {
                if (event != nullptr) cudaEventDestroy(event);
            }
            if (plan != 0) cufftDestroy(plan);
            if (time_buffer != nullptr) cudaFree(time_buffer);
            cudaFree(fft_buffer);
            fft_buffer = nullptr;
            time_buffer = nullptr;
            throw;
        }
    }

    ~Impl() {
        for (auto& event : timing_events) {
            if (event != nullptr) cudaEventDestroy(event);
        }
        if (plan != 0) cufftDestroy(plan);
        if (time_buffer != nullptr) cudaFree(time_buffer);
        if (fft_buffer != nullptr) cudaFree(fft_buffer);
    }
};

bool cuda_device_available() noexcept {
    int count = 0;
    return cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

CudaOfdmBatch::CudaOfdmBatch(
    std::size_t fft_size,
    std::size_t cp_length,
    std::size_t batch_size)
    : impl_(new Impl(fft_size, cp_length, batch_size)) {}

CudaOfdmBatch::~CudaOfdmBatch() = default;
CudaOfdmBatch::CudaOfdmBatch(CudaOfdmBatch&&) noexcept = default;
CudaOfdmBatch& CudaOfdmBatch::operator=(CudaOfdmBatch&&) noexcept = default;

std::size_t CudaOfdmBatch::fft_size() const noexcept { return impl_->fft_size; }
std::size_t CudaOfdmBatch::cp_length() const noexcept { return impl_->cp_length; }
std::size_t CudaOfdmBatch::batch_size() const noexcept { return impl_->batch_size; }
void CudaOfdmBatch::set_timing_enabled(bool enabled) noexcept {
    impl_->timing_enabled = enabled;
    if (!enabled) impl_->timing = {};
}
ComputeOperationTiming CudaOfdmBatch::timing() const noexcept {
    return impl_->timing;
}

void CudaOfdmBatch::demodulate(
    const std::vector<std::complex<float>>& time_with_cp,
    std::vector<std::complex<float>>& frequency) {
    demodulate_device(time_with_cp);
    const std::size_t values = impl_->fft_size * impl_->batch_size;
    if (impl_->timing_enabled) {
        require_cuda(cudaEventRecord(impl_->timing_events[2]), "record OFDM D2H start");
    }
    frequency.resize(values);
    require_cuda(
        cudaMemcpy(
            frequency.data(), impl_->fft_buffer,
            values * sizeof(cufftComplex), cudaMemcpyDeviceToHost),
        "cudaMemcpy FFT output");
    if (!impl_->timing_enabled) return;
    require_cuda(cudaEventRecord(impl_->timing_events[3]), "record OFDM timing end");
    require_cuda(cudaEventSynchronize(impl_->timing_events[3]), "sync OFDM timing");
    float milliseconds = 0.0f;
    require_cuda(
        cudaEventElapsedTime(
            &milliseconds, impl_->timing_events[0], impl_->timing_events[1]),
        "elapsed OFDM H2D");
    impl_->timing.h2d_us = static_cast<double>(milliseconds) * 1000.0;
    require_cuda(
        cudaEventElapsedTime(
            &milliseconds, impl_->timing_events[1], impl_->timing_events[2]),
        "elapsed OFDM kernel");
    impl_->timing.kernel_us = static_cast<double>(milliseconds) * 1000.0;
    require_cuda(
        cudaEventElapsedTime(
            &milliseconds, impl_->timing_events[2], impl_->timing_events[3]),
        "elapsed OFDM D2H");
    impl_->timing.d2h_us = static_cast<double>(milliseconds) * 1000.0;
}

void CudaOfdmBatch::demodulate_device(
    const std::vector<std::complex<float>>& time_with_cp) {
    const std::size_t symbol_size = impl_->fft_size + impl_->cp_length;
    if (time_with_cp.size() != impl_->batch_size * symbol_size) {
        throw std::invalid_argument("CUDA OFDM demodulation input shape mismatch");
    }
    if (impl_->timing_enabled) {
        require_cuda(cudaEventRecord(impl_->timing_events[0]), "record OFDM H2D start");
    }
    const std::size_t row_bytes = impl_->fft_size * sizeof(cufftComplex);
    require_cuda(
        cudaMemcpy2D(
            impl_->fft_buffer, row_bytes,
            time_with_cp.data() + impl_->cp_length,
            symbol_size * sizeof(std::complex<float>), row_bytes,
            impl_->batch_size, cudaMemcpyHostToDevice),
        "cudaMemcpy2D CP removal");
    if (impl_->timing_enabled) {
        require_cuda(cudaEventRecord(impl_->timing_events[1]), "record OFDM kernel start");
    }
    require_cufft(
        cufftExecC2C(
            impl_->plan, impl_->fft_buffer, impl_->fft_buffer, CUFFT_FORWARD),
        "cufftExecC2C forward");
    const std::size_t values = impl_->fft_size * impl_->batch_size;
    constexpr unsigned block = 256u;
    scale_values<<<grid_size(values, block), block>>>(
        impl_->fft_buffer, values,
        1.0f / std::sqrt(static_cast<float>(impl_->fft_size)));
    require_cuda(cudaGetLastError(), "CUDA FFT scale kernel");
    if (!impl_->timing_enabled) return;
    require_cuda(cudaEventRecord(impl_->timing_events[2]), "record OFDM device end");
    require_cuda(cudaEventSynchronize(impl_->timing_events[2]), "sync OFDM device timing");
    float milliseconds = 0.0f;
    require_cuda(
        cudaEventElapsedTime(
            &milliseconds, impl_->timing_events[0], impl_->timing_events[1]),
        "elapsed OFDM H2D");
    impl_->timing.h2d_us = static_cast<double>(milliseconds) * 1000.0;
    require_cuda(
        cudaEventElapsedTime(
            &milliseconds, impl_->timing_events[1], impl_->timing_events[2]),
        "elapsed OFDM kernel");
    impl_->timing.kernel_us = static_cast<double>(milliseconds) * 1000.0;
    impl_->timing.d2h_us = 0.0;
}

const void* CudaOfdmBatch::device_frequency_data() const noexcept {
    return impl_->fft_buffer;
}

void CudaOfdmBatch::modulate(
    const std::vector<std::complex<float>>& frequency,
    std::vector<std::complex<float>>& time_with_cp) {
    const std::size_t values = impl_->fft_size * impl_->batch_size;
    if (frequency.size() != values) {
        throw std::invalid_argument("CUDA OFDM modulation input shape mismatch");
    }
    if (impl_->timing_enabled) {
        require_cuda(cudaEventRecord(impl_->timing_events[0]), "record IFFT H2D start");
    }
    require_cuda(
        cudaMemcpy(
            impl_->fft_buffer, frequency.data(),
            values * sizeof(cufftComplex), cudaMemcpyHostToDevice),
        "cudaMemcpy IFFT input");
    if (impl_->timing_enabled) {
        require_cuda(cudaEventRecord(impl_->timing_events[1]), "record IFFT kernel start");
    }
    require_cufft(
        cufftExecC2C(
            impl_->plan, impl_->fft_buffer, impl_->fft_buffer, CUFFT_INVERSE),
        "cufftExecC2C inverse");
    const std::size_t output_values =
        (impl_->fft_size + impl_->cp_length) * impl_->batch_size;
    constexpr unsigned block = 256u;
    scale_and_add_cp<<<grid_size(output_values, block), block>>>(
        impl_->fft_buffer, impl_->time_buffer, impl_->fft_size,
        impl_->cp_length, output_values,
        1.0f / std::sqrt(static_cast<float>(impl_->fft_size)));
    require_cuda(cudaGetLastError(), "CUDA IFFT CP kernel");
    if (impl_->timing_enabled) {
        require_cuda(cudaEventRecord(impl_->timing_events[2]), "record IFFT D2H start");
    }
    time_with_cp.resize(output_values);
    require_cuda(
        cudaMemcpy(
            time_with_cp.data(), impl_->time_buffer,
            output_values * sizeof(cufftComplex), cudaMemcpyDeviceToHost),
        "cudaMemcpy IFFT output");
    if (!impl_->timing_enabled) return;
    require_cuda(cudaEventRecord(impl_->timing_events[3]), "record IFFT timing end");
    require_cuda(cudaEventSynchronize(impl_->timing_events[3]), "sync IFFT timing");
    float milliseconds = 0.0f;
    require_cuda(
        cudaEventElapsedTime(
            &milliseconds, impl_->timing_events[0], impl_->timing_events[1]),
        "elapsed IFFT H2D");
    impl_->timing.h2d_us = static_cast<double>(milliseconds) * 1000.0;
    require_cuda(
        cudaEventElapsedTime(
            &milliseconds, impl_->timing_events[1], impl_->timing_events[2]),
        "elapsed IFFT kernel");
    impl_->timing.kernel_us = static_cast<double>(milliseconds) * 1000.0;
    require_cuda(
        cudaEventElapsedTime(
            &milliseconds, impl_->timing_events[2], impl_->timing_events[3]),
        "elapsed IFFT D2H");
    impl_->timing.d2h_us = static_cast<double>(milliseconds) * 1000.0;
}

}  // namespace openisac
