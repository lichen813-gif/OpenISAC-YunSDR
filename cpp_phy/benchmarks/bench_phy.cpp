#include "openisac/binary_io.hpp"
#include "openisac/ldpc.hpp"
#include "openisac/mimo2x2.hpp"
#include "openisac/mimo_nxn.hpp"
#include "openisac/ofdm.hpp"
#include "openisac/qam.hpp"

#include <array>
#include <chrono>
#include <complex>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

int main() {
    using Clock = std::chrono::steady_clock;
    using openisac::Channel2x2;
    using openisac::LinearDetector;
    using openisac::SquareQAM;

    const Channel2x2 channel{
        {0.91f, 0.12f}, {0.21f, -0.18f},
        {-0.14f, 0.31f}, {0.77f, 0.09f},
    };
    std::array<std::complex<float>, 2> received{{
        {0.4f, -0.2f}, {-0.1f, 0.8f}}};
    constexpr std::size_t detection_iterations = 2'000'000u;
    float sink = 0.0f;
    const auto detection_start = Clock::now();
    for (std::size_t index = 0; index < detection_iterations; ++index) {
        received[0] += std::complex<float>{1.0e-8f, -1.0e-8f};
        const auto detected = openisac::detect_2x2(
            received, channel, 0.001f, LinearDetector::mmse);
        sink += detected.symbols[index & 1u].real();
    }
    const auto detection_end = Clock::now();
    const auto benchmark_nxn = [&](std::size_t streams, std::size_t iterations) {
        openisac::ChannelNxN nxn_channel;
        nxn_channel.streams = streams;
        std::array<std::complex<float>, openisac::maximum_spatial_streams> nxn_received{};
        for (std::size_t row = 0u; row < streams; ++row) {
            nxn_received[row] = {
                0.05f * static_cast<float>(row + 1u),
                -0.03f * static_cast<float>(row + 2u)};
            for (std::size_t column = 0u; column < streams; ++column) {
                nxn_channel.values[
                    row * openisac::maximum_spatial_streams + column] =
                    row == column
                        ? std::complex<float>{1.0f, 0.0f}
                        : std::complex<float>{
                              0.015f * static_cast<float>(row + 1u),
                              -0.01f * static_cast<float>(column + 1u)};
            }
        }
        const auto start = Clock::now();
        for (std::size_t index = 0u; index < iterations; ++index) {
            nxn_received[0] += std::complex<float>{1.0e-8f, -1.0e-8f};
            const auto detected = openisac::detect_nxn(
                nxn_received, nxn_channel, 0.001f, LinearDetector::mmse);
            sink += detected.symbols[index % streams].real();
        }
        const auto end = Clock::now();
        return std::chrono::duration<double>(end - start).count() * 1.0e9 /
               static_cast<double>(iterations);
    };
    const double detector_4x4_ns = benchmark_nxn(4u, 300'000u);
    const double detector_8x8_ns = benchmark_nxn(8u, 100'000u);
    constexpr std::size_t llr_iterations = 200'000u;
    const auto llr_start = Clock::now();
    for (std::size_t index = 0; index < llr_iterations; ++index) {
        const auto llrs = SquareQAM::max_log_llrs(
            received[index & 1u], 0.01f, 6u);
        sink += llrs[index % 6u];
    }
    const auto llr_end = Clock::now();

    std::vector<std::complex<float>> ofdm_samples(1152u);
    for (std::size_t index = 0; index < ofdm_samples.size(); ++index) {
        ofdm_samples[index] = {
            static_cast<float>(index % 17u) * 0.01f,
            static_cast<float>(index % 23u) * -0.01f};
    }
    constexpr std::size_t fft_iterations = 20'000u;
    const auto fft_start = Clock::now();
    for (std::size_t index = 0; index < fft_iterations; ++index) {
        const auto frequency = openisac::ofdm_demodulate(ofdm_samples, 1024u, 128u);
        sink += frequency[index & 1023u].real();
    }
    const auto fft_end = Clock::now();

    const std::string matrix_root = OPENISAC_MATRIX_DIR;
    const openisac::Ldpc5041008 codec(
        openisac::join_path(matrix_root, "LDPC_504_1008.alist"),
        openisac::join_path(matrix_root, "LDPC_504_1008G.alist"));
    std::vector<std::uint8_t> information(openisac::ldpc_information_bits);
    for (std::size_t bit = 0u; bit < information.size(); ++bit) {
        information[bit] = static_cast<std::uint8_t>((bit * 17u + 3u) & 1u);
    }
    const auto codeword = codec.encode(information);
    std::vector<float> ldpc_llrs(codeword.size());
    for (std::size_t bit = 0u; bit < codeword.size(); ++bit) {
        ldpc_llrs[bit] = codeword[bit] == 0u ? 8.0f : -8.0f;
    }
    constexpr std::size_t ldpc_iterations = 2000u;
    const auto ldpc_start = Clock::now();
    for (std::size_t iteration = 0u; iteration < ldpc_iterations; ++iteration) {
        const auto decoded = codec.decode_normalized_min_sum(ldpc_llrs, 6u, 0.8f);
        sink += static_cast<float>(decoded.iterations + decoded.syndrome_weight);
    }
    const auto ldpc_end = Clock::now();

    const double detection_seconds =
        std::chrono::duration<double>(detection_end - detection_start).count();
    const double llr_seconds =
        std::chrono::duration<double>(llr_end - llr_start).count();
    const double fft_seconds =
        std::chrono::duration<double>(fft_end - fft_start).count();
    const double ldpc_seconds =
        std::chrono::duration<double>(ldpc_end - ldpc_start).count();
    const double detector_ns =
        detection_seconds * 1.0e9 / static_cast<double>(detection_iterations);
    const double llr_ns =
        llr_seconds * 1.0e9 / static_cast<double>(llr_iterations);
    const double fft_ns =
        fft_seconds * 1.0e9 / static_cast<double>(fft_iterations);
    const double ldpc_us =
        ldpc_seconds * 1.0e6 / static_cast<double>(ldpc_iterations);
    const double payload_re_per_second = 1.0e9 / (detector_ns + llr_ns);
    const double detector_demapper_frame_rate = payload_re_per_second / 1216.0;
    const double receive_frame_ns = 1216.0 * (detector_ns + llr_ns) + 4.0 * fft_ns;
    const double receive_frame_rate = 1.0e9 / receive_frame_ns;
    std::cout << std::fixed << std::setprecision(2)
              << "2x2 MMSE: " << detector_ns << " ns/RE\n"
              << "4x4 MMSE Cholesky: " << detector_4x4_ns << " ns/RE\n"
              << "8x8 MMSE Cholesky: " << detector_8x8_ns << " ns/RE\n"
              << "64-QAM max-log: " << llr_ns << " ns/RE\n"
              << "1024-point FFT with CP removal: " << fft_ns << " ns/symbol\n"
              << "LDPC(1008,504) high-SNR decode: " << ldpc_us << " us/block\n"
              << "14-block LDPC estimate: " << ldpc_us * 14.0 << " us/frame\n"
              << "detector/demapper estimate: " << detector_demapper_frame_rate << " frame/s\n"
              << "plus four data FFTs estimate: " << receive_frame_rate << " frame/s\n"
              << "sink=" << sink << '\n';
    return receive_frame_rate > 0.0 ? 0 : 1;
}
