#include "openisac/cuda_mimo.hpp"
#include "openisac/ldpc_framing.hpp"

#include <cuda_runtime.h>
#include <cufft.h>

#include <cmath>
#include <complex>
#include <cfloat>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace openisac {
namespace {

constexpr std::size_t maximum_streams = 8u;

void require_cuda(cudaError_t status, const char* operation) {
    if (status != cudaSuccess) {
        throw std::runtime_error(
            std::string(operation) + ": " + cudaGetErrorString(status));
    }
}

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

__device__ unsigned gray_to_binary(unsigned gray) {
    unsigned binary = gray;
    while ((gray >>= 1u) != 0u) {
        binary ^= gray;
    }
    return binary;
}

__device__ float qam_normalization(unsigned bits_per_symbol) {
    switch (bits_per_symbol) {
        case 2u: return 1.4142135623730951f;
        case 4u: return 3.1622776601683793f;
        case 6u: return 6.4807406984078602f;
        case 8u: return 13.038404810405298f;
        default: return 1.0f;
    }
}

__device__ float pam_from_gray(unsigned gray, unsigned axis_bits) {
    const unsigned levels = 1u << axis_bits;
    return static_cast<float>(levels - 1u) -
        2.0f * static_cast<float>(gray_to_binary(gray));
}

__device__ void axis_max_log_llrs(
    float sample,
    unsigned axis_bits,
    unsigned bits_per_symbol,
    float inverse_variance,
    float* output) {
    const unsigned levels = 1u << axis_bits;
    const float normalization = qam_normalization(bits_per_symbol);
    for (unsigned bit = 0u; bit < axis_bits; ++bit) {
        float minimum_zero = FLT_MAX;
        float minimum_one = FLT_MAX;
        const unsigned mask = 1u << (axis_bits - 1u - bit);
        for (unsigned gray = 0u; gray < levels; ++gray) {
            const float reference =
                pam_from_gray(gray, axis_bits) / normalization;
            const float delta = sample - reference;
            const float distance = delta * delta;
            if ((gray & mask) == 0u) {
                minimum_zero = fminf(minimum_zero, distance);
            } else {
                minimum_one = fminf(minimum_one, distance);
            }
        }
        output[bit] = (minimum_one - minimum_zero) * inverse_variance;
    }
}

__device__ void qam_max_log_llrs(
    cufftComplex symbol,
    float noise_variance,
    unsigned bits_per_symbol,
    float* output) {
    const unsigned axis_bits = bits_per_symbol / 2u;
    const float inverse_variance =
        1.0f / fmaxf(noise_variance, 1.0e-12f);
    axis_max_log_llrs(
        symbol.x, axis_bits, bits_per_symbol, inverse_variance, output);
    axis_max_log_llrs(
        symbol.y, axis_bits, bits_per_symbol, inverse_variance,
        output + axis_bits);
}

__device__ bool factor_cholesky(
    const cufftComplex* gram,
    cufftComplex* lower,
    unsigned streams) {
    for (unsigned index = 0u; index < maximum_streams * maximum_streams;
         ++index) {
        lower[index] = {0.0f, 0.0f};
    }
    for (unsigned row = 0u; row < streams; ++row) {
        for (unsigned column = 0u; column <= row; ++column) {
            cufftComplex value = gram[row * maximum_streams + column];
            for (unsigned inner = 0u; inner < column; ++inner) {
                value = subtract(
                    value,
                    multiply(
                        lower[row * maximum_streams + inner],
                        conjugate(lower[column * maximum_streams + inner])));
            }
            if (row == column) {
                if (!isfinite(value.x) || value.x <= 1.0e-10f) {
                    return false;
                }
                lower[row * maximum_streams + column] =
                    {sqrtf(value.x), 0.0f};
            } else {
                lower[row * maximum_streams + column] = scale(
                    value,
                    1.0f / lower[column * maximum_streams + column].x);
            }
        }
    }
    return true;
}

__device__ void solve_cholesky(
    const cufftComplex* lower,
    const cufftComplex* right,
    cufftComplex* result,
    unsigned streams) {
    cufftComplex intermediate[maximum_streams];
    for (unsigned row = 0u; row < streams; ++row) {
        cufftComplex value = right[row];
        for (unsigned column = 0u; column < row; ++column) {
            value = subtract(
                value,
                multiply(
                    lower[row * maximum_streams + column],
                    intermediate[column]));
        }
        intermediate[row] = scale(
            value, 1.0f / lower[row * maximum_streams + row].x);
    }
    for (unsigned reverse = 0u; reverse < streams; ++reverse) {
        const unsigned row = streams - 1u - reverse;
        cufftComplex value = intermediate[row];
        for (unsigned column = row + 1u; column < streams; ++column) {
            value = subtract(
                value,
                multiply(
                    conjugate(lower[column * maximum_streams + row]),
                    result[column]));
        }
        result[row] = scale(
            value, 1.0f / lower[row * maximum_streams + row].x);
    }
}

__global__ void detect_mimo_batch(
    const cufftComplex* received,
    const cufftComplex* channels,
    cufftComplex* symbols,
    float* predicted_mse,
    float* soft_bits,
    const float* descramble_signs,
    unsigned char* status,
    unsigned streams,
    unsigned receive_ports,
    std::size_t batch_size,
    float noise_variance,
    bool mmse,
    unsigned bits_per_symbol,
    std::size_t ldpc_blocks) {
    const std::size_t batch =
        static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (batch >= batch_size) {
        return;
    }
    cufftComplex gram[maximum_streams * maximum_streams];
    cufftComplex lower[maximum_streams * maximum_streams];
    cufftComplex matched[maximum_streams];
    cufftComplex solved[maximum_streams];
    for (unsigned index = 0u; index < maximum_streams * maximum_streams;
         ++index) {
        gram[index] = {0.0f, 0.0f};
    }
    for (unsigned index = 0u; index < maximum_streams; ++index) {
        matched[index] = {0.0f, 0.0f};
        solved[index] = {0.0f, 0.0f};
    }
    const float transmit_scale = rsqrtf(static_cast<float>(streams));
    const std::size_t channel_offset =
        batch * static_cast<std::size_t>(receive_ports) * streams;
    const std::size_t receive_offset =
        batch * static_cast<std::size_t>(receive_ports);
    for (unsigned column = 0u; column < streams; ++column) {
        for (unsigned row = 0u; row < receive_ports; ++row) {
            const cufftComplex h = scale(
                channels[channel_offset + row * streams + column],
                transmit_scale);
            matched[column] = add(
                matched[column],
                multiply(conjugate(h), received[receive_offset + row]));
            for (unsigned other = 0u; other < streams; ++other) {
                const cufftComplex h_other = scale(
                    channels[channel_offset + row * streams + other],
                    transmit_scale);
                const unsigned index = column * maximum_streams + other;
                gram[index] = add(
                    gram[index], multiply(conjugate(h), h_other));
            }
        }
    }
    if (mmse) {
        for (unsigned stream = 0u; stream < streams; ++stream) {
            gram[stream * maximum_streams + stream].x += noise_variance;
        }
    }
    if (!factor_cholesky(gram, lower, streams)) {
        status[batch] = 0u;
        return;
    }
    solve_cholesky(lower, matched, solved, streams);
    const std::size_t output_offset = batch * streams;
    for (unsigned stream = 0u; stream < streams; ++stream) {
        symbols[output_offset + stream] = solved[stream];
        predicted_mse[output_offset + stream] = 0.0f;
    }
    if (noise_variance > 0.0f) {
        cufftComplex unit[maximum_streams];
        cufftComplex inverse_column[maximum_streams];
        for (unsigned stream = 0u; stream < streams; ++stream) {
            for (unsigned index = 0u; index < streams; ++index) {
                unit[index] = {0.0f, 0.0f};
                inverse_column[index] = {0.0f, 0.0f};
            }
            unit[stream] = {1.0f, 0.0f};
            solve_cholesky(lower, unit, inverse_column, streams);
            predicted_mse[output_offset + stream] = fmaxf(
                0.0f, noise_variance * inverse_column[stream].x);
        }
    }
    if (bits_per_symbol != 0u) {
        for (unsigned stream = 0u; stream < streams; ++stream) {
            float llrs[8];
            qam_max_log_llrs(
                solved[stream], predicted_mse[output_offset + stream],
                bits_per_symbol, llrs);
            for (unsigned bit = 0u; bit < bits_per_symbol; ++bit) {
                const std::size_t raw_index =
                    (output_offset + stream) * bits_per_symbol + bit;
                if (ldpc_blocks == 0u) {
                    soft_bits[raw_index] = llrs[bit];
                    continue;
                }
                if (raw_index >= ldpc_blocks * ldpc_codeword_bits) {
                    continue;
                }
                const std::size_t within_block =
                    raw_index % ldpc_codeword_bits;
                constexpr std::size_t columns =
                    ldpc_codeword_bits / ldpc_interleaver_rows;
                const std::size_t column =
                    within_block / ldpc_interleaver_rows;
                const std::size_t row =
                    within_block % ldpc_interleaver_rows;
                const std::size_t decoder_index =
                    (raw_index / ldpc_codeword_bits) * ldpc_codeword_bits +
                    row * columns + column;
                soft_bits[decoder_index] =
                    llrs[bit] * descramble_signs[decoder_index];
            }
        }
    }
    status[batch] = 1u;
}

unsigned grid_size(std::size_t values, unsigned block_size) {
    return static_cast<unsigned>((values + block_size - 1u) / block_size);
}

}  // namespace

struct CudaMimoBatch::Impl {
    std::size_t streams = 0u;
    std::size_t receive_ports = 0u;
    std::size_t batch_size = 0u;
    cufftComplex* received = nullptr;
    cufftComplex* channels = nullptr;
    cufftComplex* symbols = nullptr;
    float* predicted_mse = nullptr;
    float* soft_bits = nullptr;
    float* descramble_signs = nullptr;
    unsigned char* status = nullptr;
    std::vector<unsigned char> host_status;
    cudaEvent_t timing_events[4]{};
    ComputeOperationTiming timing{};
    bool timing_enabled = false;

    Impl(std::size_t stream_count, std::size_t receive_count, std::size_t batch)
        : streams(stream_count),
          receive_ports(receive_count),
          batch_size(batch),
          host_status(batch) {
        if (streams < 1u || streams > maximum_streams ||
            receive_ports < streams || receive_ports > maximum_streams ||
            batch_size == 0u) {
            throw std::invalid_argument("invalid CUDA MIMO batch shape");
        }
        static_assert(
            sizeof(std::complex<float>) == sizeof(cufftComplex),
            "std::complex<float> must match cufftComplex storage");
        try {
            require_cuda(
                cudaMalloc(
                    reinterpret_cast<void**>(&received),
                    batch_size * receive_ports * sizeof(cufftComplex)),
                "cudaMalloc MIMO received");
            require_cuda(
                cudaMalloc(
                    reinterpret_cast<void**>(&channels),
                    batch_size * receive_ports * streams * sizeof(cufftComplex)),
                "cudaMalloc MIMO channels");
            require_cuda(
                cudaMalloc(
                    reinterpret_cast<void**>(&symbols),
                    batch_size * streams * sizeof(cufftComplex)),
                "cudaMalloc MIMO symbols");
            require_cuda(
                cudaMalloc(
                    reinterpret_cast<void**>(&predicted_mse),
                    batch_size * streams * sizeof(float)),
                "cudaMalloc MIMO MSE");
            require_cuda(
                cudaMalloc(
                    reinterpret_cast<void**>(&soft_bits),
                    batch_size * streams * 8u * sizeof(float)),
                "cudaMalloc MIMO soft bits");
            const std::size_t maximum_soft_bits = batch_size * streams * 8u;
            require_cuda(
                cudaMalloc(
                    reinterpret_cast<void**>(&descramble_signs),
                    maximum_soft_bits * sizeof(float)),
                "cudaMalloc MIMO descramble signs");
            std::vector<float> host_descramble_signs(maximum_soft_bits);
            unsigned char lfsr = 0x5Au;
            for (std::size_t index = 0u; index < maximum_soft_bits; ++index) {
                const unsigned char bit = static_cast<unsigned char>(
                    ((lfsr >> 7u) ^ (lfsr >> 3u) ^
                     (lfsr >> 2u) ^ (lfsr >> 1u)) & 1u);
                host_descramble_signs[index] = bit == 0u ? 1.0f : -1.0f;
                lfsr = static_cast<unsigned char>(
                    (static_cast<unsigned>(lfsr) << 1u) | bit);
            }
            require_cuda(
                cudaMemcpy(
                    descramble_signs, host_descramble_signs.data(),
                    maximum_soft_bits * sizeof(float), cudaMemcpyHostToDevice),
                "cudaMemcpy MIMO descramble signs");
            require_cuda(
                cudaMalloc(
                    reinterpret_cast<void**>(&status),
                    batch_size * sizeof(unsigned char)),
                "cudaMalloc MIMO status");
            for (auto& event : timing_events) {
                require_cuda(cudaEventCreate(&event), "cudaEventCreate MIMO timing");
            }
        } catch (...) {
            for (auto& event : timing_events) {
                if (event != nullptr) cudaEventDestroy(event);
            }
            if (status != nullptr) cudaFree(status);
            if (descramble_signs != nullptr) cudaFree(descramble_signs);
            if (soft_bits != nullptr) cudaFree(soft_bits);
            if (predicted_mse != nullptr) cudaFree(predicted_mse);
            if (symbols != nullptr) cudaFree(symbols);
            if (channels != nullptr) cudaFree(channels);
            if (received != nullptr) cudaFree(received);
            throw;
        }
    }

    ~Impl() {
        for (auto& event : timing_events) {
            if (event != nullptr) cudaEventDestroy(event);
        }
        if (status != nullptr) cudaFree(status);
        if (descramble_signs != nullptr) cudaFree(descramble_signs);
        if (soft_bits != nullptr) cudaFree(soft_bits);
        if (predicted_mse != nullptr) cudaFree(predicted_mse);
        if (symbols != nullptr) cudaFree(symbols);
        if (channels != nullptr) cudaFree(channels);
        if (received != nullptr) cudaFree(received);
    }
};

CudaMimoBatch::CudaMimoBatch(
    std::size_t streams,
    std::size_t receive_ports,
    std::size_t batch_size)
    : impl_(new Impl(streams, receive_ports, batch_size)) {}

CudaMimoBatch::~CudaMimoBatch() = default;
CudaMimoBatch::CudaMimoBatch(CudaMimoBatch&&) noexcept = default;
CudaMimoBatch& CudaMimoBatch::operator=(CudaMimoBatch&&) noexcept = default;

std::size_t CudaMimoBatch::streams() const noexcept { return impl_->streams; }
std::size_t CudaMimoBatch::receive_ports() const noexcept {
    return impl_->receive_ports;
}
std::size_t CudaMimoBatch::batch_size() const noexcept {
    return impl_->batch_size;
}
void CudaMimoBatch::set_timing_enabled(bool enabled) noexcept {
    impl_->timing_enabled = enabled;
    if (!enabled) impl_->timing = {};
}
ComputeOperationTiming CudaMimoBatch::timing() const noexcept {
    return impl_->timing;
}

void CudaMimoBatch::detect(
    const std::vector<std::complex<float>>& received,
    const std::vector<std::complex<float>>& channels,
    float noise_variance,
    LinearDetector detector,
    std::vector<std::complex<float>>& symbols,
    std::vector<float>& predicted_mse) {
    std::vector<float> unused_soft_bits;
    detect_soft(
        received, channels, noise_variance, detector, 0u, 0u, true,
        symbols, predicted_mse, unused_soft_bits);
}

void CudaMimoBatch::detect_soft(
    const std::vector<std::complex<float>>& received,
    const std::vector<std::complex<float>>& channels,
    float noise_variance,
    LinearDetector detector,
    unsigned bits_per_symbol,
    std::size_t ldpc_blocks,
    bool return_equalized,
    std::vector<std::complex<float>>& symbols,
    std::vector<float>& predicted_mse,
    std::vector<float>& soft_bits) {
    if (received.size() != impl_->batch_size * impl_->receive_ports ||
        channels.size() !=
            impl_->batch_size * impl_->receive_ports * impl_->streams) {
        throw std::invalid_argument("CUDA MIMO input shape mismatch");
    }
    if (!std::isfinite(noise_variance) || noise_variance < 0.0f) {
        throw std::invalid_argument(
            "CUDA MIMO noise variance must be finite and non-negative");
    }
    if (bits_per_symbol != 0u && bits_per_symbol != 2u &&
        bits_per_symbol != 4u && bits_per_symbol != 6u &&
        bits_per_symbol != 8u) {
        throw std::invalid_argument(
            "CUDA MIMO soft demapper requires Qm=2,4,6,8");
    }
    const std::size_t output_values = impl_->batch_size * impl_->streams;
    if ((ldpc_blocks != 0u && bits_per_symbol == 0u) ||
        ldpc_blocks * ldpc_codeword_bits > output_values * bits_per_symbol) {
        throw std::invalid_argument(
            "CUDA MIMO LDPC soft-bit shape exceeds detector output");
    }
    if (impl_->timing_enabled) {
        require_cuda(cudaEventRecord(impl_->timing_events[0]), "record MIMO H2D start");
    }
    require_cuda(
        cudaMemcpy(
            impl_->received, received.data(),
            received.size() * sizeof(cufftComplex), cudaMemcpyHostToDevice),
        "cudaMemcpy MIMO received");
    require_cuda(
        cudaMemcpy(
            impl_->channels, channels.data(),
            channels.size() * sizeof(cufftComplex), cudaMemcpyHostToDevice),
        "cudaMemcpy MIMO channels");
    if (impl_->timing_enabled) {
        require_cuda(cudaEventRecord(impl_->timing_events[1]), "record MIMO kernel start");
    }
    constexpr unsigned block = 128u;
    detect_mimo_batch<<<grid_size(impl_->batch_size, block), block>>>(
        impl_->received, impl_->channels, impl_->symbols,
        impl_->predicted_mse, impl_->soft_bits, impl_->descramble_signs,
        impl_->status,
        static_cast<unsigned>(impl_->streams),
        static_cast<unsigned>(impl_->receive_ports), impl_->batch_size,
        noise_variance, detector == LinearDetector::mmse, bits_per_symbol,
        ldpc_blocks);
    require_cuda(cudaGetLastError(), "CUDA MIMO detector kernel");
    if (impl_->timing_enabled) {
        require_cuda(cudaEventRecord(impl_->timing_events[2]), "record MIMO D2H start");
    }
    if (return_equalized) {
        symbols.resize(output_values);
        predicted_mse.resize(output_values);
        require_cuda(
            cudaMemcpy(
                symbols.data(), impl_->symbols,
                symbols.size() * sizeof(cufftComplex), cudaMemcpyDeviceToHost),
            "cudaMemcpy MIMO symbols");
        require_cuda(
            cudaMemcpy(
                predicted_mse.data(), impl_->predicted_mse,
                predicted_mse.size() * sizeof(float), cudaMemcpyDeviceToHost),
            "cudaMemcpy MIMO MSE");
    } else {
        symbols.clear();
        predicted_mse.clear();
    }
    if (bits_per_symbol != 0u) {
        soft_bits.resize(ldpc_blocks == 0u
            ? output_values * bits_per_symbol
            : ldpc_blocks * ldpc_codeword_bits);
        require_cuda(
            cudaMemcpy(
                soft_bits.data(), impl_->soft_bits,
                soft_bits.size() * sizeof(float), cudaMemcpyDeviceToHost),
            "cudaMemcpy MIMO soft bits");
    } else {
        soft_bits.clear();
    }
    require_cuda(
        cudaMemcpy(
            impl_->host_status.data(), impl_->status,
            impl_->batch_size * sizeof(unsigned char), cudaMemcpyDeviceToHost),
        "cudaMemcpy MIMO status");
    if (impl_->timing_enabled) {
        require_cuda(cudaEventRecord(impl_->timing_events[3]), "record MIMO timing end");
        require_cuda(cudaEventSynchronize(impl_->timing_events[3]), "sync MIMO timing");
    }
    float milliseconds = 0.0f;
    if (impl_->timing_enabled) {
    require_cuda(
        cudaEventElapsedTime(
            &milliseconds, impl_->timing_events[0], impl_->timing_events[1]),
        "elapsed MIMO H2D");
    impl_->timing.h2d_us = static_cast<double>(milliseconds) * 1000.0;
    require_cuda(
        cudaEventElapsedTime(
            &milliseconds, impl_->timing_events[1], impl_->timing_events[2]),
        "elapsed MIMO kernel");
    impl_->timing.kernel_us = static_cast<double>(milliseconds) * 1000.0;
    require_cuda(
        cudaEventElapsedTime(
            &milliseconds, impl_->timing_events[2], impl_->timing_events[3]),
        "elapsed MIMO D2H");
    impl_->timing.d2h_us = static_cast<double>(milliseconds) * 1000.0;
    }
    for (const unsigned char value : impl_->host_status) {
        if (value == 0u) {
            throw std::runtime_error(
                "singular or non-positive CUDA MIMO Gram matrix");
        }
    }
}

void CudaMimoBatch::detect_soft_device(
    const void* device_received,
    const void* device_channels,
    float noise_variance,
    LinearDetector detector,
    unsigned bits_per_symbol,
    std::size_t ldpc_blocks,
    bool return_equalized,
    std::vector<std::complex<float>>& symbols,
    std::vector<float>& predicted_mse,
    std::vector<float>& soft_bits) {
    if (device_received == nullptr || device_channels == nullptr) {
        throw std::invalid_argument("CUDA device MIMO inputs are null");
    }
    if (!std::isfinite(noise_variance) || noise_variance < 0.0f) {
        throw std::invalid_argument(
            "CUDA MIMO noise variance must be finite and non-negative");
    }
    if (bits_per_symbol != 0u && bits_per_symbol != 2u &&
        bits_per_symbol != 4u && bits_per_symbol != 6u &&
        bits_per_symbol != 8u) {
        throw std::invalid_argument(
            "CUDA MIMO soft demapper requires Qm=2,4,6,8");
    }
    const std::size_t output_values = impl_->batch_size * impl_->streams;
    if ((ldpc_blocks != 0u && bits_per_symbol == 0u) ||
        ldpc_blocks * ldpc_codeword_bits > output_values * bits_per_symbol) {
        throw std::invalid_argument(
            "CUDA MIMO LDPC soft-bit shape exceeds detector output");
    }
    if (impl_->timing_enabled) {
        require_cuda(cudaEventRecord(impl_->timing_events[0]),
            "record prepared MIMO start");
        require_cuda(cudaEventRecord(impl_->timing_events[1]),
            "record prepared MIMO kernel start");
    }
    constexpr unsigned block = 128u;
    detect_mimo_batch<<<grid_size(impl_->batch_size, block), block>>>(
        static_cast<const cufftComplex*>(device_received),
        static_cast<const cufftComplex*>(device_channels),
        impl_->symbols, impl_->predicted_mse, impl_->soft_bits,
        impl_->descramble_signs, impl_->status,
        static_cast<unsigned>(impl_->streams),
        static_cast<unsigned>(impl_->receive_ports), impl_->batch_size,
        noise_variance, detector == LinearDetector::mmse, bits_per_symbol,
        ldpc_blocks);
    require_cuda(cudaGetLastError(), "CUDA prepared MIMO detector kernel");
    if (impl_->timing_enabled) {
        require_cuda(cudaEventRecord(impl_->timing_events[2]),
            "record prepared MIMO D2H start");
    }
    if (return_equalized) {
        symbols.resize(output_values);
        predicted_mse.resize(output_values);
        require_cuda(cudaMemcpy(
            symbols.data(), impl_->symbols,
            symbols.size() * sizeof(cufftComplex), cudaMemcpyDeviceToHost),
            "cudaMemcpy prepared MIMO symbols");
        require_cuda(cudaMemcpy(
            predicted_mse.data(), impl_->predicted_mse,
            predicted_mse.size() * sizeof(float), cudaMemcpyDeviceToHost),
            "cudaMemcpy prepared MIMO MSE");
    } else {
        symbols.clear();
        predicted_mse.clear();
    }
    if (bits_per_symbol != 0u) {
        soft_bits.resize(ldpc_blocks == 0u
            ? output_values * bits_per_symbol
            : ldpc_blocks * ldpc_codeword_bits);
        require_cuda(cudaMemcpy(
            soft_bits.data(), impl_->soft_bits,
            soft_bits.size() * sizeof(float), cudaMemcpyDeviceToHost),
            "cudaMemcpy prepared MIMO soft bits");
    } else {
        soft_bits.clear();
    }
    require_cuda(cudaMemcpy(
        impl_->host_status.data(), impl_->status,
        impl_->batch_size * sizeof(unsigned char), cudaMemcpyDeviceToHost),
        "cudaMemcpy prepared MIMO status");
    if (impl_->timing_enabled) {
        require_cuda(cudaEventRecord(impl_->timing_events[3]),
            "record prepared MIMO timing end");
        require_cuda(cudaEventSynchronize(impl_->timing_events[3]),
            "sync prepared MIMO timing");
        float milliseconds = 0.0f;
        require_cuda(cudaEventElapsedTime(
            &milliseconds, impl_->timing_events[0], impl_->timing_events[1]),
            "elapsed prepared MIMO H2D");
        impl_->timing.h2d_us = static_cast<double>(milliseconds) * 1000.0;
        require_cuda(cudaEventElapsedTime(
            &milliseconds, impl_->timing_events[1], impl_->timing_events[2]),
            "elapsed prepared MIMO kernel");
        impl_->timing.kernel_us = static_cast<double>(milliseconds) * 1000.0;
        require_cuda(cudaEventElapsedTime(
            &milliseconds, impl_->timing_events[2], impl_->timing_events[3]),
            "elapsed prepared MIMO D2H");
        impl_->timing.d2h_us = static_cast<double>(milliseconds) * 1000.0;
    }
    for (const unsigned char value : impl_->host_status) {
        if (value == 0u) {
            throw std::runtime_error(
                "singular or non-positive CUDA MIMO Gram matrix");
        }
    }
}

}  // namespace openisac
