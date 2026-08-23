#include "openisac/binary_io.hpp"
#include "openisac/dynamic_frame.hpp"
#include "openisac/frame.hpp"
#include "openisac/ldpc.hpp"
#include "openisac/sensing.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
constexpr float two_pi = 6.28318530717958647692f;

struct SensingFrame {
    std::vector<std::complex<float>> transmit_grid;
    std::vector<std::complex<float>> receive_grid;
};

std::vector<std::uint8_t> make_payload(
    const openisac::LinkMode mode,
    std::size_t frame) {
    openisac::FormalFrameProfile profile;
    profile.transmit_rank = mode.rank;
    profile.bits_per_symbol = openisac::modulation_bits(mode.modulation);
    const auto layout = openisac::build_formal_frame_layout(profile);
    std::vector<std::uint8_t> payload(layout.user_payload_bytes);
    for (std::size_t index = 0u; index < payload.size(); ++index) {
        payload[index] = static_cast<std::uint8_t>(
            (index * 43u + frame * 29u + 17u) & 0xFFu);
    }
    return payload;
}

SensingFrame make_sensing_frame(
    const openisac::Ldpc5041008& codec,
    const openisac::DynamicSensingConfig& config,
    std::size_t frame,
    std::mt19937& random) {
    const openisac::LinkMode mode{2u, openisac::Modulation::qam64};
    const auto encoded = openisac::encode_dynamic_frame(
        make_payload(mode, frame), mode, static_cast<std::uint16_t>(frame),
        codec, static_cast<std::uint32_t>(0xC600u + frame));
    SensingFrame output;
    output.transmit_grid = encoded.tx_grid;
    output.receive_grid.assign(
        config.data_symbols * config.fft_size * config.receive_ports,
        std::complex<float>{});
    std::normal_distribution<float> noise(0.0f, 1.0e-3f);
    struct Target {
        std::size_t range_bin;
        int doppler_bin;
        float amplitude;
        float phase;
    };
    constexpr Target targets[] = {
        {10u, 0, 4.0f, 0.11f},
        {5u, 1, 1.0f, 0.27f},
        {18u, -3, 0.65f, -0.41f},
    };
    for (std::size_t fft = 0u; fft < config.fft_size; ++fft) {
        const int centered = fft < config.fft_size / 2u
            ? static_cast<int>(fft)
            : static_cast<int>(fft) - static_cast<int>(config.fft_size);
        std::complex<float> h0{};
        std::complex<float> h1{};
        for (const auto& target : targets) {
            const float target_phase = -two_pi *
                static_cast<float>(centered) *
                static_cast<float>(target.range_bin) /
                static_cast<float>(config.range_fft_size) +
                two_pi * static_cast<float>(target.doppler_bin) *
                static_cast<float>(frame) /
                static_cast<float>(config.doppler_fft_size) + target.phase;
            h0 += std::polar(target.amplitude, target_phase);
            h1 += std::polar(
                0.35f * target.amplitude, target_phase + 0.31f);
        }
        for (std::size_t symbol = 0u; symbol < config.data_symbols; ++symbol) {
            const std::size_t base =
                (symbol * config.fft_size + fft) * 2u;
            const auto echo = encoded.tx_grid[base] * h0 +
                encoded.tx_grid[base + 1u] * h1;
            output.receive_grid[base] = echo + std::complex<float>{
                noise(random), noise(random)};
            output.receive_grid[base + 1u] = 0.7f * echo;
        }
    }
    return output;
}

double microseconds(Clock::duration duration) {
    return std::chrono::duration<double, std::micro>(duration).count();
}

bool contains_detection(
    const openisac::DynamicSensingResult& result,
    std::size_t range_bin,
    std::size_t doppler_bin) {
    return std::any_of(
        result.detections.begin(), result.detections.end(),
        [&](const openisac::DynamicSensingDetection& detection) {
            return detection.peak.range_bin == range_bin &&
                detection.peak.doppler_bin == doppler_bin;
        });
}

void export_result(
    const std::filesystem::path& directory,
    const openisac::DynamicSensingResult& result,
    const openisac::DynamicSensingConfig& config) {
    std::filesystem::create_directories(directory);
    std::ofstream map_file(directory / "range_doppler.csv");
    map_file << "doppler_bin,range_bin,range_m,velocity_mps,power_db,relative_power_db\n";
    float peak_power = 0.0f;
    for (const auto& value : result.range_doppler_map) {
        peak_power = std::max(peak_power, std::norm(value));
    }
    peak_power = std::max(peak_power, 1.0e-30f);
    for (std::size_t doppler = 0u; doppler < config.doppler_fft_size; ++doppler) {
        const int centered = static_cast<int>(doppler) -
            static_cast<int>(config.doppler_fft_size / 2u);
        const float velocity = static_cast<float>(centered) *
            result.velocity_bin_spacing_mps;
        for (std::size_t range = 0u; range < config.range_fft_size; ++range) {
            const float power = std::max(
                std::norm(result.range_doppler_map[
                    doppler * config.range_fft_size + range]),
                1.0e-30f);
            map_file << doppler << ',' << range << ','
                     << static_cast<float>(range) * result.range_bin_spacing_m
                     << ',' << velocity << ',' << 10.0f * std::log10(power)
                     << ',' << 10.0f * std::log10(power / peak_power) << '\n';
        }
    }
    if (!map_file) {
        throw std::runtime_error("failed to write sensing range-Doppler CSV");
    }

    std::ofstream detection_file(directory / "detections.csv");
    detection_file << "range_bin,doppler_bin,range_m,doppler_hz,velocity_mps,power_db,margin_db\n";
    for (const auto& detection : result.detections) {
        detection_file << detection.peak.range_bin << ','
                       << detection.peak.doppler_bin << ','
                       << detection.peak.range_m << ','
                       << detection.peak.doppler_hz << ','
                       << detection.peak.velocity_mps << ','
                       << 10.0f * std::log10(std::max(
                              detection.peak.power, 1.0e-30f)) << ','
                       << detection.power_over_threshold_db << '\n';
    }
    if (!detection_file) {
        throw std::runtime_error("failed to write sensing detection CSV");
    }

    std::ofstream summary_file(directory / "summary.csv");
    summary_file << "metric,value\n"
                 << "fft_size," << config.fft_size << '\n'
                 << "coherent_frames," << config.coherent_frames << '\n'
                 << "range_fft_size," << config.range_fft_size << '\n'
                 << "doppler_fft_size," << config.doppler_fft_size << '\n'
                 << "range_bin_spacing_m," << result.range_bin_spacing_m << '\n'
                 << "velocity_bin_spacing_mps," << result.velocity_bin_spacing_mps << '\n'
                 << "clutter_suppression," <<
                    (result.static_clutter_suppression_applied ? 1 : 0) << '\n'
                 << "cfar_cells_tested," << result.cfar_cells_tested << '\n'
                 << "detections," << result.detections.size() << '\n';
    if (!summary_file) {
        throw std::runtime_error("failed to write sensing summary CSV");
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::size_t batches = argc > 1
        ? std::max<std::size_t>(1u, std::strtoull(argv[1], nullptr, 10))
        : 20u;
    const std::string matrix_root = OPENISAC_MATRIX_DIR;
    const openisac::Ldpc5041008 codec(
        openisac::join_path(matrix_root, "LDPC_504_1008.alist"),
        openisac::join_path(matrix_root, "LDPC_504_1008G.alist"));
    openisac::DynamicSensingConfig config;
    config.enable_static_clutter_suppression = true;
    std::mt19937 random(0x6600u);
    std::vector<SensingFrame> corpus;
    corpus.reserve(config.coherent_frames);
    for (std::size_t frame = 0u; frame < config.coherent_frames; ++frame) {
        corpus.push_back(make_sensing_frame(codec, config, frame, random));
    }

    openisac::DynamicSensingProcessor processor(config);
    double nonfinal_us = 0.0;
    double final_us = 0.0;
    double total_us = 0.0;
    std::size_t nonfinal_calls = 0u;
    for (std::size_t batch = 0u; batch < batches; ++batch) {
        const auto batch_start = Clock::now();
        for (std::size_t frame = 0u; frame < config.coherent_frames; ++frame) {
            const std::uint64_t sequence =
                static_cast<std::uint64_t>(batch * config.coherent_frames + frame);
            const auto start = Clock::now();
            const bool ready = processor.push_frame(
                sequence, sequence * 225000u,
                corpus[frame].transmit_grid, corpus[frame].receive_grid);
            const double elapsed = microseconds(Clock::now() - start);
            if (frame + 1u == config.coherent_frames) {
                final_us += elapsed;
                if (!ready) {
                    std::cerr << "ERROR: coherent batch did not complete\n";
                    return 2;
                }
            } else {
                nonfinal_us += elapsed;
                ++nonfinal_calls;
                if (ready) {
                    std::cerr << "ERROR: coherent batch completed early\n";
                    return 2;
                }
            }
        }
        total_us += microseconds(Clock::now() - batch_start);
    }

    const auto& result = processor.last_result();
    const std::size_t expected_doppler = config.doppler_fft_size / 2u + 1u;
    const bool peak_ok = result.strongest_peak.range_bin == 5u &&
        result.strongest_peak.doppler_bin == expected_doppler &&
        contains_detection(result, 5u, expected_doppler) &&
        contains_detection(
            result, 18u, config.doppler_fft_size / 2u - 3u) &&
        !contains_detection(result, 10u, config.doppler_fft_size / 2u);
    std::cout << std::fixed << std::setprecision(3)
              << "Current-PHY sensing benchmark (R2/64-QAM, 1024 FFT, 64 frames)\n"
              << "batches=" << batches << '\n'
              << "ordinary_push_mean_us=" << nonfinal_us /
                    static_cast<double>(nonfinal_calls) << '\n'
              << "final_push_with_range_doppler_mean_us=" << final_us /
                    static_cast<double>(batches) << '\n'
              << "batch_mean_us=" << total_us /
                    static_cast<double>(batches) << '\n'
              << "amortized_per_frame_us=" << total_us /
                    static_cast<double>(batches * config.coherent_frames) << '\n'
              << "direct_subcarriers="
              << result.directly_estimated_subcarriers << '\n'
              << "interpolated_subcarriers="
              << result.interpolated_subcarriers << '\n'
              << "cfar_cells_tested=" << result.cfar_cells_tested << '\n'
              << "detections=" << result.detections.size() << '\n'
              << "peak_range_bin=" << result.strongest_peak.range_bin
              << " peak_range_m=" << result.strongest_peak.range_m << '\n'
              << "peak_doppler_bin=" << result.strongest_peak.doppler_bin
              << " peak_velocity_mps="
              << result.strongest_peak.velocity_mps << '\n'
              << "multi_target_clutter_cfar_check="
              << (peak_ok ? "PASS" : "FAIL") << '\n';
    if (argc > 2 && argv[2][0] != '\0') {
        export_result(argv[2], result, config);
        std::cout << "export_directory=" << argv[2] << '\n';
    }
    return peak_ok ? 0 : 3;
}
