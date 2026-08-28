#include "openisac/cuda_mimo.hpp"
#include "openisac/ldpc_framing.hpp"
#include "openisac/mimo_nxn.hpp"
#include "openisac/qam.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

namespace {

struct Inputs {
    std::vector<std::complex<float>> received;
    std::vector<std::complex<float>> channels;
    std::vector<openisac::ChannelNxN> cpu_channels;
    std::vector<std::array<std::complex<float>,
                           openisac::maximum_spatial_streams>> cpu_received;
};

Inputs make_inputs(std::size_t streams, std::size_t batch_size) {
    std::mt19937 random(static_cast<unsigned>(0x4D494D4Fu + streams));
    std::normal_distribution<float> channel_random(0.0f, 0.12f);
    std::normal_distribution<float> symbol_random(0.0f, 0.5f);
    Inputs inputs;
    inputs.received.resize(batch_size * streams);
    inputs.channels.resize(batch_size * streams * streams);
    inputs.cpu_channels.resize(batch_size);
    inputs.cpu_received.resize(batch_size);
    const float transmit_scale = 1.0f / std::sqrt(static_cast<float>(streams));
    for (std::size_t batch = 0u; batch < batch_size; ++batch) {
        std::array<std::complex<float>, openisac::maximum_spatial_streams>
            transmitted{};
        for (std::size_t stream = 0u; stream < streams; ++stream) {
            transmitted[stream] = {
                symbol_random(random), symbol_random(random)};
        }
        auto& cpu_channel = inputs.cpu_channels[batch];
        cpu_channel.streams = streams;
        cpu_channel.receive_ports = streams;
        for (std::size_t row = 0u; row < streams; ++row) {
            for (std::size_t column = 0u; column < streams; ++column) {
                std::complex<float> h{
                    channel_random(random), channel_random(random)};
                if (row == column) {
                    h += std::complex<float>{1.5f, 0.0f};
                }
                inputs.channels[
                    (batch * streams + row) * streams + column] = h;
                cpu_channel.values[
                    row * openisac::maximum_spatial_streams + column] = h;
                inputs.received[batch * streams + row] +=
                    h * transmit_scale * transmitted[column];
            }
            inputs.cpu_received[batch][row] =
                inputs.received[batch * streams + row];
        }
    }
    return inputs;
}

double elapsed_us(
    std::chrono::steady_clock::time_point begin,
    std::chrono::steady_clock::time_point end) {
    return std::chrono::duration<double, std::micro>(end - begin).count();
}

void verify_shape(std::size_t streams) {
    constexpr std::size_t batch_size = 256u;
    constexpr float noise_variance = 1.0e-3f;
    const Inputs inputs = make_inputs(streams, batch_size);
    openisac::CudaMimoBatch backend(streams, streams, batch_size);
    std::vector<std::complex<float>> cuda_symbols;
    std::vector<float> cuda_mse;
    std::vector<float> cuda_soft_bits;
    constexpr unsigned bits_per_symbol = 6u;
    backend.detect_soft(
        inputs.received, inputs.channels, noise_variance,
        openisac::LinearDetector::mmse, bits_per_symbol, 0u, true,
        cuda_symbols, cuda_mse, cuda_soft_bits);
    float symbol_error = 0.0f;
    float mse_error = 0.0f;
    float soft_bit_error = 0.0f;
    for (std::size_t batch = 0u; batch < batch_size; ++batch) {
        const auto cpu = openisac::detect_nxn(
            inputs.cpu_received[batch], inputs.cpu_channels[batch],
            noise_variance, openisac::LinearDetector::mmse);
        for (std::size_t stream = 0u; stream < streams; ++stream) {
            const std::size_t index = batch * streams + stream;
            symbol_error = std::max(
                symbol_error, std::abs(cpu.symbols[stream] - cuda_symbols[index]));
            mse_error = std::max(
                mse_error,
                std::abs(cpu.predicted_mse[stream] - cuda_mse[index]));
            const auto cpu_soft_bits = openisac::SquareQAM::max_log_llrs(
                cpu.symbols[stream],
                std::max(cpu.predicted_mse[stream], 1.0e-12f),
                bits_per_symbol);
            for (unsigned bit = 0u; bit < bits_per_symbol; ++bit) {
                soft_bit_error = std::max(
                    soft_bit_error,
                    std::abs(
                        cpu_soft_bits[bit] -
                        cuda_soft_bits[index * bits_per_symbol + bit]));
            }
        }
    }
    std::cout << streams << "x" << streams
              << " CUDA MMSE symbol_max_error=" << symbol_error
              << " mse_max_error=" << mse_error
              << " soft_bit_max_error=" << soft_bit_error << '\n';
    if (symbol_error > 5.0e-4f || mse_error > 5.0e-5f ||
        soft_bit_error > 2.0e-2f) {
        throw std::runtime_error("CUDA MIMO numerical tolerance exceeded");
    }

    std::vector<std::complex<float>> soft_only_symbols{{1.0f, 1.0f}};
    std::vector<float> soft_only_mse{1.0f};
    std::vector<float> soft_only_bits;
    backend.detect_soft(
        inputs.received, inputs.channels, noise_variance,
        openisac::LinearDetector::mmse, bits_per_symbol, 0u, false,
        soft_only_symbols, soft_only_mse, soft_only_bits);
    if (!soft_only_symbols.empty() || !soft_only_mse.empty() ||
        soft_only_bits.size() != cuda_soft_bits.size()) {
        throw std::runtime_error("CUDA MIMO soft-only output shape mismatch");
    }
    float soft_only_error = 0.0f;
    for (std::size_t index = 0u; index < soft_only_bits.size(); ++index) {
        soft_only_error = std::max(
            soft_only_error,
            std::abs(soft_only_bits[index] - cuda_soft_bits[index]));
    }
    if (soft_only_error > 1.0e-6f) {
        throw std::runtime_error("CUDA MIMO soft-only output mismatch");
    }

    constexpr std::size_t ldpc_blocks = 1u;
    std::vector<std::complex<float>> decoder_symbols;
    std::vector<float> decoder_mse;
    std::vector<float> decoder_llrs;
    backend.detect_soft(
        inputs.received, inputs.channels, noise_variance,
        openisac::LinearDetector::mmse, bits_per_symbol, ldpc_blocks, false,
        decoder_symbols, decoder_mse, decoder_llrs);
    std::vector<float> expected_decoder_llrs(
        cuda_soft_bits.begin(),
        cuda_soft_bits.begin() + openisac::ldpc_codeword_bits);
    openisac::deinterleave_ldpc_blocks(expected_decoder_llrs);
    openisac::soft_descramble(expected_decoder_llrs);
    float decoder_llr_error = 0.0f;
    for (std::size_t index = 0u; index < decoder_llrs.size(); ++index) {
        decoder_llr_error = std::max(
            decoder_llr_error,
            std::abs(decoder_llrs[index] - expected_decoder_llrs[index]));
    }
    std::cout << streams << "x" << streams
              << " CUDA decoder_llr_max_error=" << decoder_llr_error << '\n';
    if (!decoder_symbols.empty() || !decoder_mse.empty() ||
        decoder_llrs.size() != openisac::ldpc_codeword_bits ||
        decoder_llr_error > 1.0e-6f) {
        throw std::runtime_error("CUDA decoder-order LLR mismatch");
    }
}

void benchmark_8x8() {
    constexpr std::size_t streams = 8u;
    constexpr std::size_t batch_size = 4096u;
    constexpr std::size_t iterations = 10u;
    constexpr float noise_variance = 1.0e-3f;
    const Inputs inputs = make_inputs(streams, batch_size);
    openisac::CudaMimoBatch backend(streams, streams, batch_size);
    std::vector<std::complex<float>> cuda_symbols;
    std::vector<float> cuda_mse;
    backend.detect(
        inputs.received, inputs.channels, noise_variance,
        openisac::LinearDetector::mmse, cuda_symbols, cuda_mse);

    volatile float sink = 0.0f;
    const auto cpu_begin = std::chrono::steady_clock::now();
    for (std::size_t iteration = 0u; iteration < iterations; ++iteration) {
        for (std::size_t batch = 0u; batch < batch_size; ++batch) {
            const auto result = openisac::detect_nxn(
                inputs.cpu_received[batch], inputs.cpu_channels[batch],
                noise_variance, openisac::LinearDetector::mmse);
            sink += result.symbols[0].real();
        }
    }
    const double cpu_us = elapsed_us(
        cpu_begin, std::chrono::steady_clock::now());
    const auto cuda_begin = std::chrono::steady_clock::now();
    for (std::size_t iteration = 0u; iteration < iterations; ++iteration) {
        backend.detect(
            inputs.received, inputs.channels, noise_variance,
            openisac::LinearDetector::mmse, cuda_symbols, cuda_mse);
        sink += cuda_symbols[0].real();
    }
    const double cuda_us = elapsed_us(
        cuda_begin, std::chrono::steady_clock::now());
    const double detections = static_cast<double>(iterations * batch_size);
    std::cout << "8x8 batch=" << batch_size
              << " CPU=" << cpu_us * 1000.0 / detections << " ns/RE"
              << " CUDA including H2D/D2H="
              << cuda_us * 1000.0 / detections << " ns/RE"
              << " speedup=" << cpu_us / cuda_us << "x sink=" << sink << '\n';
}

}  // namespace

int main() {
    try {
        for (const std::size_t streams : {1u, 2u, 4u, 8u}) {
            verify_shape(streams);
        }
        benchmark_8x8();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CUDA MIMO test failed: " << error.what() << '\n';
        return 1;
    }
}
