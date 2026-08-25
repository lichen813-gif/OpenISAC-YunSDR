#include "openisac/binary_io.hpp"
#include "openisac/capture_io.hpp"
#include "openisac/channel_estimation.hpp"
#include "openisac/crc16.hpp"
#include "openisac/dynamic_frame.hpp"
#include "openisac/dynamic_frame_pipeline.hpp"
#include "openisac/dynamic_link.hpp"
#include "openisac/dynamic_link_pipeline.hpp"
#include "openisac/frame.hpp"
#include "openisac/link_adaptation.hpp"
#include "openisac/ldpc.hpp"
#include "openisac/ldpc_frame_decoder.hpp"
#include "openisac/ldpc_framing.hpp"
#include "openisac/mimo2x2.hpp"
#include "openisac/mimo_nxn.hpp"
#include "openisac/mimo_nxn_link.hpp"
#include "openisac/ofdm.hpp"
#include "openisac/preamble_sync.hpp"
#include "openisac/qam.hpp"
#include "openisac/rank4_formal_link.hpp"
#include "openisac/rank4_time_link.hpp"
#include "openisac/rank4_time_pipeline.hpp"
#include "openisac/sampling_offset.hpp"
#include "openisac/sensing.hpp"
#include "openisac/tdl_channel.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using openisac::AdaptiveLinkController;
using openisac::Channel2x2;
using openisac::ControllerReason;
using openisac::FormalFrameProfile;
using openisac::LinearDetector;
using openisac::LinkMode;
using openisac::MiniHeader;
using openisac::Modulation;
using openisac::SquareQAM;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void require_close(float actual, float expected, float tolerance, const std::string& message) {
    if (std::abs(actual - expected) > tolerance) {
        throw std::runtime_error(
            message + ": actual=" + std::to_string(actual) +
            ", expected=" + std::to_string(expected));
    }
}

void require_complex_close(
    const std::complex<float>& actual,
    const std::complex<float>& expected,
    float tolerance,
    const std::string& message) {
    if (std::abs(actual - expected) > tolerance) {
        throw std::runtime_error(message);
    }
}

void test_crc() {
    const std::string text = "123456789";
    const auto crc = openisac::crc16_ccitt_false(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    require(crc == 0x29B1u, "CRC-16/CCITT-FALSE standard vector failed");
}

void test_qam() {
    for (const unsigned bits : {2u, 4u, 6u, 8u}) {
        const unsigned order = 1u << bits;
        double power = 0.0;
        for (unsigned label = 0; label < order; ++label) {
            const auto point = SquareQAM::modulate(label, bits);
            power += std::norm(point);
            require(
                SquareQAM::demodulate(point, bits) == label,
                "QAM hard round trip failed");
            const auto llrs = SquareQAM::max_log_llrs(point, 0.01f, bits);
            for (unsigned bit = 0; bit < bits; ++bit) {
                const bool expected =
                    ((label >> (bits - 1u - bit)) & 1u) != 0u;
                require(
                    (llrs[bit] < 0.0f) == expected,
                    "QAM LLR sign failed");
            }
        }
        require_close(
            static_cast<float>(power / static_cast<double>(order)),
            1.0f,
            1.0e-6f,
            "QAM average power failed");
        for (unsigned sample_index = 0u; sample_index < 16u; ++sample_index) {
            const std::complex<float> sample{
                -1.3f + 0.17f * static_cast<float>(sample_index),
                1.1f - 0.13f * static_cast<float>(sample_index)};
            constexpr float variance = 0.037f;
            const auto optimized = SquareQAM::max_log_llrs(sample, variance, bits);
            for (unsigned bit = 0u; bit < bits; ++bit) {
                float minimum_zero = std::numeric_limits<float>::infinity();
                float minimum_one = std::numeric_limits<float>::infinity();
                const unsigned mask = 1u << (bits - 1u - bit);
                for (unsigned label = 0u; label < order; ++label) {
                    const float distance = std::norm(
                        sample - SquareQAM::modulate(label, bits));
                    if ((label & mask) == 0u) {
                        minimum_zero = std::min(minimum_zero, distance);
                    } else {
                        minimum_one = std::min(minimum_one, distance);
                    }
                }
                require_close(
                    optimized[bit],
                    (minimum_one - minimum_zero) / variance,
                    2.0e-4f,
                    "optimized QAM LLR disagrees with exhaustive reference");
            }
        }
    }
}

void test_ofdm_round_trip() {
    std::vector<std::complex<float>> frequency(1024u);
    for (std::size_t index = 0; index < frequency.size(); ++index) {
        frequency[index] = {
            std::sin(static_cast<float>(index) * 0.17f),
            std::cos(static_cast<float>(index) * 0.11f)};
    }
    const auto samples = openisac::ofdm_modulate(frequency, 128u);
    require(samples.size() == 1152u, "OFDM symbol size changed");
    for (std::size_t index = 0; index < 128u; ++index) {
        require_complex_close(
            samples[index], samples[1024u + index], 1.0e-6f, "cyclic prefix failed");
    }
    const auto recovered = openisac::ofdm_demodulate(samples, 1024u, 128u);
    for (std::size_t index = 0; index < frequency.size(); ++index) {
        require_complex_close(recovered[index], frequency[index], 3.0e-5f, "FFT round trip failed");
    }
}

void test_preamble_and_tdl() {
    const auto reference = openisac::generate_zc_ofdm_symbol(1024u, 128u, 29u);
    require(reference.size() == 1152u, "ZC preamble length changed");
    std::vector<std::vector<std::complex<float>>> streams(
        2u, std::vector<std::complex<float>>(32u + reference.size()));
    for (std::size_t index = 0u; index < reference.size(); ++index) {
        streams[0][17u + index] = reference[index] * std::complex<float>{0.8f, 0.3f};
        streams[1][17u + index] = reference[index] * std::complex<float>{-0.2f, 0.9f};
    }
    const auto timing = openisac::estimate_zc_timing(streams, reference, 32u);
    require(timing.offset == 17u, "ZC timing estimate failed");
    require(timing.peak_metric > 0.999f, "ZC normalized peak metric failed");
    openisac::TimingEstimate tracked_timing;
    openisac::estimate_zc_timing_window(
        streams, reference, 15u, 19u, tracked_timing);
    require(tracked_timing.offset == timing.offset &&
                tracked_timing.search_begin == 15u &&
                tracked_timing.metrics.size() == 5u,
            "windowed ZC timing estimate failed");

    std::vector<std::vector<std::complex<float>>> cfo_streams(
        2u, std::vector<std::complex<float>>(17u + 2u * reference.size()));
    for (std::size_t symbol = 0u; symbol < 2u; ++symbol) {
        for (std::size_t index = 0u; index < reference.size(); ++index) {
            cfo_streams[0][17u + symbol * reference.size() + index] =
                reference[index] * std::complex<float>{0.8f, 0.3f};
            cfo_streams[1][17u + symbol * reference.size() + index] =
                reference[index] * std::complex<float>{-0.2f, 0.9f};
        }
    }
    constexpr float cfo = 0.08f;
    openisac::apply_cfo_normalized_inplace(cfo_streams, cfo, 1024u);
    const float estimated_cfo = openisac::estimate_cp_cfo_normalized(
        cfo_streams, 17u, 1024u, 128u, 2u);
    require_close(estimated_cfo, cfo, 2.0e-6f, "CP CFO estimate failed");
    openisac::apply_cfo_normalized_inplace(cfo_streams, -estimated_cfo, 1024u);
    const float residual_cfo = openisac::estimate_cp_cfo_normalized(
        cfo_streams, 17u, 1024u, 128u, 2u);
    require_close(residual_cfo, 0.0f, 2.0e-6f, "CP CFO correction failed");

    const std::vector<openisac::TdlTap> taps{
        {0u, 0.0f, 0.0f}, {3u, -4.0f, 45.0f}, {9u, -8.0f, -80.0f}};
    const auto moving_snapshot = openisac::evaluate_tdl_taps(
        std::vector<openisac::TdlTap>{{7u, -3.0f, 10.0f, 2.0f}}, 0.125);
    require_close(
        moving_snapshot[0].phase_degrees, 100.0f, 1.0e-6f,
        "TDL Doppler phase evolution failed");

    std::vector<openisac::TdlTap> correlation_taps(512u);
    for (std::size_t index = 0u; index < correlation_taps.size(); ++index) {
        correlation_taps[index] = {index, 0.0f, 0.0f, 0.0f};
    }
    openisac::TdlSpatialCorrelationConfig spatial;
    spatial.transmit_correlation = 0.65f;
    spatial.receive_correlation = 0.35f;
    spatial.random_seed = 0x12345678u;
    const auto correlated = openisac::build_correlated_tdl_2x2(
        correlation_taps, spatial);
    const auto repeated = openisac::build_correlated_tdl_2x2(
        correlation_taps, spatial);
    spatial.random_seed ^= 0x01020304u;
    const auto changed = openisac::build_correlated_tdl_2x2(
        correlation_taps, spatial);
    double total_correlated_power = 0.0;
    double repeat_error = 0.0;
    double changed_error = 0.0;
    std::complex<double> transmit_cross{};
    std::complex<double> receive_cross{};
    double transmit_first_power = 0.0;
    double transmit_second_power = 0.0;
    double receive_first_power = 0.0;
    double receive_second_power = 0.0;
    for (std::size_t rx = 0u; rx < 2u; ++rx) {
        for (std::size_t tx = 0u; tx < 2u; ++tx) {
            for (std::size_t delay = 0u; delay < correlation_taps.size(); ++delay) {
                total_correlated_power += std::norm(correlated[rx][tx][delay]);
                repeat_error += std::norm(
                    correlated[rx][tx][delay] - repeated[rx][tx][delay]);
                changed_error += std::norm(
                    correlated[rx][tx][delay] - changed[rx][tx][delay]);
            }
        }
    }
    for (std::size_t delay = 0u; delay < correlation_taps.size(); ++delay) {
        for (std::size_t rx = 0u; rx < 2u; ++rx) {
            transmit_cross += std::conj(
                static_cast<std::complex<double>>(correlated[rx][0][delay])) *
                static_cast<std::complex<double>>(correlated[rx][1][delay]);
            transmit_first_power += std::norm(correlated[rx][0][delay]);
            transmit_second_power += std::norm(correlated[rx][1][delay]);
        }
        for (std::size_t tx = 0u; tx < 2u; ++tx) {
            receive_cross += std::conj(
                static_cast<std::complex<double>>(correlated[0][tx][delay])) *
                static_cast<std::complex<double>>(correlated[1][tx][delay]);
            receive_first_power += std::norm(correlated[0][tx][delay]);
            receive_second_power += std::norm(correlated[1][tx][delay]);
        }
    }
    const float measured_transmit_correlation = static_cast<float>(
        transmit_cross.real() /
        std::sqrt(transmit_first_power * transmit_second_power));
    const float measured_receive_correlation = static_cast<float>(
        receive_cross.real() /
        std::sqrt(receive_first_power * receive_second_power));
    require_close(
        static_cast<float>(total_correlated_power), 4.0f, 2.0e-5f,
        "correlated TDL average-link normalization failed");
    require(repeat_error == 0.0, "correlated TDL seed is not repeatable");
    require(changed_error > 0.1, "correlated TDL seed does not change the channel");
    require_close(
        measured_transmit_correlation, 0.65f, 0.06f,
        "correlated TDL transmit coefficient failed");
    require_close(
        measured_receive_correlation, 0.35f, 0.06f,
        "correlated TDL receive coefficient failed");
    const auto impulse = openisac::build_deterministic_tdl_2x2(taps);
    for (std::size_t rx = 0u; rx < 2u; ++rx) {
        for (std::size_t tx = 0u; tx < 2u; ++tx) {
            double power = 0.0;
            for (const auto& value : impulse[rx][tx]) {
                power += std::norm(value);
            }
            require_close(static_cast<float>(power), 1.0f, 1.0e-6f, "TDL link power failed");
        }
    }
    std::array<std::vector<std::complex<float>>, 2> transmitted{
        std::vector<std::complex<float>>(16u), std::vector<std::complex<float>>(16u)};
    transmitted[0][0] = {1.0f, 0.0f};
    const auto received = openisac::apply_tdl_2x2_symbol(transmitted, impulse);
    require_complex_close(received[0][3], impulse[0][0][3], 1.0e-6f, "TDL convolution failed");
}

void test_fdm_pilot_channel_estimation() {
    constexpr std::size_t fft_size = 16u;
    constexpr std::size_t time_symbols = 2u;
    const std::vector<std::uint16_t> pilots{12u, 13u, 14u, 15u, 1u, 2u, 3u, 4u};
    std::vector<std::complex<float>> transmitted(time_symbols * fft_size * 2u);
    std::vector<std::complex<float>> received(time_symbols * fft_size * 2u);
    const Channel2x2 expected{{0.9f, 0.2f}, {0.1f, -0.3f}, {-0.2f, 0.1f}, {0.7f, 0.4f}};
    for (std::size_t time = 0u; time < time_symbols; ++time) {
        for (std::size_t index = 0u; index < pilots.size(); ++index) {
            const std::size_t fft = pilots[index];
            const std::size_t tx = index % 2u;
            const std::complex<float> known = ((index + time) & 1u) != 0u
                                                  ? std::complex<float>{-1.0f, 0.0f}
                                                  : std::complex<float>{1.0f, 0.0f};
            transmitted[(time * fft_size + fft) * 2u + tx] = known;
            received[(time * fft_size + fft) * 2u] =
                known * (tx == 0u ? expected.h00 : expected.h01);
            received[(time * fft_size + fft) * 2u + 1u] =
                known * (tx == 0u ? expected.h10 : expected.h11);
        }
    }
    const auto estimated = openisac::estimate_fdm_pilot_channel_linear_2x2(
        received, transmitted, pilots, time_symbols, fft_size);
    for (const auto& channel : estimated) {
        require_complex_close(channel.h00, expected.h00, 1.0e-6f, "pilot LS H00 failed");
        require_complex_close(channel.h01, expected.h01, 1.0e-6f, "pilot LS H01 failed");
        require_complex_close(channel.h10, expected.h10, 1.0e-6f, "pilot LS H10 failed");
        require_complex_close(channel.h11, expected.h11, 1.0e-6f, "pilot LS H11 failed");
    }
}

void test_nr_dmrs_channel_estimation() {
    openisac::FormalFrameProfile profile;
    profile.transmit_rank = 4u;
    profile.pilot_spacing = 2u;
    const auto layout = openisac::build_formal_frame_layout(profile);
    for (const std::size_t ports : {1u, 2u, 4u}) {
        std::vector<std::complex<float>> reference;
        openisac::build_nr_dmrs_reference_grid(
            profile.fft_size, layout.active_fft_indices, ports,
            0xD4A5u, reference);
        std::vector<std::complex<float>> received(reference.size());
        std::array<std::array<std::complex<float>, 4>, 4> truth{};
        for (std::size_t rx = 0u; rx < ports; ++rx) {
            for (std::size_t tx = 0u; tx < ports; ++tx) {
                truth[rx][tx] = {
                    0.25f + 0.11f * static_cast<float>(rx + 1u) +
                        0.07f * static_cast<float>(tx),
                    -0.18f + 0.05f * static_cast<float>(rx) -
                        0.03f * static_cast<float>(tx)};
            }
        }
        for (std::size_t time = 0u; time < 2u; ++time) {
            for (const auto fft : layout.active_fft_indices) {
                float total_power = 0.0f;
                for (std::size_t tx = 0u; tx < ports; ++tx) {
                    total_power += std::norm(reference[
                        (time * profile.fft_size + fft) * ports + tx]);
                }
                require_close(total_power, 1.0f, 1.0e-6f,
                              "NR DM-RS total power normalization failed");
                for (std::size_t rx = 0u; rx < ports; ++rx) {
                    std::complex<float> sample{};
                    for (std::size_t tx = 0u; tx < ports; ++tx) {
                        sample += truth[rx][tx] * reference[
                            (time * profile.fft_size + fft) * ports + tx];
                    }
                    received[(time * profile.fft_size + fft) * ports + rx] =
                        sample;
                }
            }
        }
        std::vector<openisac::ChannelNxN> estimated;
        openisac::FdmPilotChannelEstimatorWorkspaceNxN workspace;
        openisac::estimate_nr_dmrs_channel_linear_nxn(
            received, reference, layout.active_fft_indices, 2u,
            profile.fft_size, ports, estimated, workspace);
        for (std::size_t time = 0u; time < 2u; ++time) {
            for (const auto fft : layout.active_fft_indices) {
                const auto& channel = estimated[time * profile.fft_size + fft];
                for (std::size_t rx = 0u; rx < ports; ++rx) {
                    for (std::size_t tx = 0u; tx < ports; ++tx) {
                        require_complex_close(
                            channel.values[
                                rx * openisac::maximum_spatial_streams + tx],
                            truth[rx][tx], 2.0e-5f,
                            "NR DM-RS CDM/OCC channel estimate failed");
                    }
                }
            }
        }
    }
}

void test_nr_dmrs_rank_compatibility() {
    require(
        std::abs(openisac::formal_frame_period_seconds(
                     openisac::PilotMode::fdm) - 225.0e-6) < 1.0e-12 &&
            std::abs(openisac::formal_frame_period_seconds(
                         openisac::PilotMode::nr_dmrs) - 375.0e-6) < 1.0e-12,
        "pilot-mode frame periods are inconsistent with FFT/CP sampling");
    const std::string matrix_root = OPENISAC_MATRIX_DIR;
    const openisac::Ldpc5041008 codec(
        openisac::join_path(matrix_root, "LDPC_504_1008.alist"),
        openisac::join_path(matrix_root, "LDPC_504_1008G.alist"));
    for (const unsigned rank : {1u, 2u}) {
        openisac::DynamicLinkSimulationConfig config;
        config.pilot_mode = openisac::PilotMode::nr_dmrs;
        config.snr_db = 50.0f;
        config.cfo_hz = 300.0f;
        config.sfo_ppm = 20.0f;
        config.enable_correlated_spatial_tdl = true;
        config.transmit_spatial_correlation = 0.2f;
        config.receive_spatial_correlation = 0.2f;
        config.random_seed = 0xD100u + rank;
        const auto result = openisac::simulate_dynamic_tdl_frame(
            {rank, Modulation::qam64}, static_cast<std::uint16_t>(100u + rank),
            codec, config);
        require(result.timing_ok && result.header_ok && result.crc_ok &&
                    result.pilot_mode == openisac::PilotMode::nr_dmrs &&
                    result.frame_symbols == 5u,
                "SISO/2x2 NR DM-RS full-link compatibility failed");
    }

    openisac::Rank4TimeSimulationConfig rank4;
    rank4.pilot_mode = openisac::PilotMode::nr_dmrs;
    rank4.snr_db = 50.0f;
    rank4.cfo_hz = 300.0f;
    rank4.sfo_ppm = 20.0f;
    rank4.random_seed = 0xD400u;
    const auto result = openisac::simulate_rank4_time_frame(404u, rank4, codec);
    require(result.timing_ok && result.header_ok && result.crc_ok &&
                result.payload_match &&
                result.pilot_mode == openisac::PilotMode::nr_dmrs &&
                result.frame_symbols == 5u,
            "4x4 NR DM-RS full-link compatibility failed");
}

void test_sampling_offset() {
    constexpr std::size_t sample_count = 4096u;
    constexpr float tone_radians = 0.19f;
    std::vector<std::vector<std::complex<float>>> tone(
        1u, std::vector<std::complex<float>>(sample_count));
    for (std::size_t sample = 0u; sample < sample_count; ++sample) {
        tone[0][sample] = std::polar(1.0f, tone_radians * static_cast<float>(sample));
    }
    const auto shifted = openisac::resample_sfo_cubic(tone, 100.0f);
    const float expected_step = tone_radians / 1.0001f;
    const float measured_step = std::arg(
        std::conj(shifted[0][2000u]) * shifted[0][2001u]);
    require_close(measured_step, expected_step, 2.0e-5f, "cubic SFO tone failed");
    require_close(openisac::inverse_sfo_ppm(100.0f), -99.99f, 0.02f, "inverse SFO failed");

    constexpr std::size_t fft_size = 64u;
    constexpr std::size_t antennas = 2u;
    constexpr std::size_t symbol_samples = 80u;
    constexpr float known_sfo = 20.0f;
    constexpr double two_pi = 6.28318530717958647692;
    const double slope = -static_cast<double>(known_sfo) * 1.0e-6 * two_pi *
                         static_cast<double>(symbol_samples) /
                         static_cast<double>(fft_size);
    const std::vector<std::uint16_t> references{4u, 12u, 20u, 28u, 36u, 44u, 52u, 60u};
    std::vector<std::complex<float>> grid(2u * fft_size * antennas);
    for (const auto fft : references) {
        const double centered = fft < fft_size / 2u ? static_cast<double>(fft)
                                                    : static_cast<double>(fft) - fft_size;
        for (std::size_t rx = 0u; rx < antennas; ++rx) {
            const std::complex<float> channel = rx == 0u
                ? std::complex<float>{0.7f, 0.2f}
                : std::complex<float>{-0.3f, 0.8f};
            grid[fft * antennas + rx] = channel;
            grid[(fft_size + fft) * antennas + rx] =
                channel * std::polar(1.0f, static_cast<float>(0.13 + slope * centered));
        }
    }
    const auto estimate = openisac::estimate_sfo_phase_slope(
        grid, references, fft_size, antennas, symbol_samples);
    require_close(estimate.sfo_ppm, known_sfo, 0.01f, "phase-slope SFO estimate failed");
    require(estimate.coherence > 0.999f, "phase-slope coherence failed");
    openisac::correct_second_symbol_phase_inplace(grid, estimate, fft_size, antennas);
    const auto residual = openisac::estimate_sfo_phase_slope(
        grid, references, fft_size, antennas, symbol_samples);
    require_close(residual.sfo_ppm, 0.0f, 0.01f, "phase-slope correction failed");
}

void test_formal_golden_vectors() {
    const std::string root = OPENISAC_GOLDEN_DIR;
    const auto golden_data = openisac::read_binary_vector<std::int16_t>(
        openisac::join_path(root, "data_fft_indices_i16.bin"));
    const auto golden_pilots = openisac::read_binary_vector<std::int16_t>(
        openisac::join_path(root, "pilot_fft_indices_i16.bin"));
    const auto golden_control_positions =
        openisac::read_binary_vector<std::int16_t>(
            openisac::join_path(root, "control_data_positions_i16.bin"));
    const auto golden_payload_time = openisac::read_binary_vector<std::uint8_t>(
        openisac::join_path(root, "payload_time_indices_u8.bin"));
    const auto golden_payload_positions =
        openisac::read_binary_vector<std::int16_t>(
            openisac::join_path(root, "payload_data_positions_i16.bin"));

    const FormalFrameProfile profile{};
    const auto layout = openisac::build_formal_frame_layout(profile);
    require(layout.data_fft_indices.size() == 672u, "formal data count changed");
    require(layout.pilot_fft_indices.size() == 216u, "formal pilot count changed");
    require(
        layout.phase_reference_fft_indices.size() == 8u,
        "formal phase-reference count changed");
    require(layout.payload_time_indices.size() == 1216u, "payload RE count changed");
    require(layout.ldpc_blocks == 14u, "formal LDPC block count changed");
    require(layout.user_payload_bytes == 880u, "formal user payload changed");
    require(layout.padding_qam_symbols == 80u, "formal padding changed");
    require(
        std::equal(layout.data_fft_indices.begin(), layout.data_fft_indices.end(),
                   golden_data.begin(), golden_data.end()),
        "data FFT indices disagree with Python golden vector");
    require(
        std::equal(layout.pilot_fft_indices.begin(), layout.pilot_fft_indices.end(),
                   golden_pilots.begin(), golden_pilots.end()),
        "pilot FFT indices disagree with Python golden vector");
    require(
        std::equal(
            layout.control_data_positions.begin(),
            layout.control_data_positions.end(),
            golden_control_positions.begin(),
            golden_control_positions.end()),
        "control positions disagree with Python golden vector");
    require(
        layout.payload_time_indices == golden_payload_time,
        "payload time indices disagree with Python golden vector");
    require(
        std::equal(
            layout.payload_data_positions.begin(),
            layout.payload_data_positions.end(),
            golden_payload_positions.begin(),
            golden_payload_positions.end()),
        "payload frequency positions disagree with Python golden vector");

    const auto information = openisac::read_binary_vector<std::uint8_t>(
        openisac::join_path(root, "information_bytes_u8.bin"));
    require(information.size() == 882u, "information byte count changed");
    require(openisac::check_crc16_ccitt_false(information), "golden payload CRC failed");
    for (std::size_t index = 0; index < 880u; ++index) {
        require(
            information[index] == static_cast<std::uint8_t>((index * 37u + 11u) & 0xFFu),
            "deterministic golden payload changed");
    }

    const auto packed_codewords = openisac::read_binary_vector<std::uint8_t>(
        openisac::join_path(root, "codewords_packed_msb_u8.bin"));
    const auto packed_transmitted = openisac::read_binary_vector<std::uint8_t>(
        openisac::join_path(root, "transmitted_bits_packed_msb_u8.bin"));
    auto codeword_bits = openisac::unpack_msb_bits(
        packed_codewords, layout.ldpc_blocks * openisac::ldpc_codeword_bits);
    require(
        openisac::pack_msb_bits(codeword_bits) == packed_codewords,
        "MSB-first bit pack/unpack disagrees with Python");
    openisac::scramble_bits(codeword_bits);
    openisac::interleave_ldpc_blocks(codeword_bits);
    require(
        openisac::pack_msb_bits(codeword_bits) == packed_transmitted,
        "LDPC scrambling/interleaving disagrees with Python");
    openisac::deinterleave_ldpc_blocks(codeword_bits);
    openisac::scramble_bits(codeword_bits);
    require(
        openisac::pack_msb_bits(codeword_bits) == packed_codewords,
        "LDPC framing inverse round trip failed");

    const std::string matrix_root = OPENISAC_MATRIX_DIR;
    const openisac::Ldpc5041008 codec(
        openisac::join_path(matrix_root, "LDPC_504_1008.alist"),
        openisac::join_path(matrix_root, "LDPC_504_1008G.alist"));
    const auto golden_codeword_bits = openisac::unpack_msb_bits(
        packed_codewords, layout.ldpc_blocks * openisac::ldpc_encoded_bits);
    for (std::size_t block = 0u; block < layout.ldpc_blocks; ++block) {
        const auto byte_begin = information.begin() +
            static_cast<std::ptrdiff_t>(block * 63u);
        const std::vector<std::uint8_t> block_bytes(byte_begin, byte_begin + 63u);
        const auto information_bits = openisac::unpack_msb_bits(
            block_bytes, openisac::ldpc_information_bits);
        const auto encoded = codec.encode(information_bits);
        const auto golden_begin = golden_codeword_bits.begin() +
            static_cast<std::ptrdiff_t>(block * openisac::ldpc_encoded_bits);
        require(
            std::equal(encoded.begin(), encoded.end(), golden_begin),
            "C++ LDPC encoder disagrees with Python golden codeword");
        require(codec.syndrome_weight(encoded) == 0u, "encoded LDPC syndrome failed");
        std::vector<float> noiseless_llrs(encoded.size());
        for (std::size_t bit = 0u; bit < encoded.size(); ++bit) {
            noiseless_llrs[bit] = encoded[bit] == 0u ? 8.0f : -8.0f;
        }
        const auto decoded = codec.decode_normalized_min_sum(noiseless_llrs, 6u, 0.8f);
        require(decoded.syndrome_weight == 0u, "noiseless LDPC decode syndrome failed");
        require(
            decoded.information_bits == information_bits,
            "noiseless LDPC decoder failed to recover information bits");
    }

    const auto golden_control = openisac::read_binary_vector<std::uint8_t>(
        openisac::join_path(root, "control_labels_u8.bin"));
    const MiniHeader header{
        1u,
        static_cast<std::uint8_t>(
            openisac::modulation_flag(6u) | openisac::transmit_rank_flag(2u)),
        882u,
        14u,
        0x1234u,
    };
    require(
        openisac::control_qpsk_labels(header) == golden_control,
        "marker/BCH mini-header labels disagree with Python golden vector");
    float marker_metric = 0.0f;
    const auto decoded_header =
        openisac::decode_control_qpsk_labels(golden_control, &marker_metric);
    require(marker_metric > 0.999f, "noiseless control marker metric failed");
    require(decoded_header.version == header.version &&
                decoded_header.flags == header.flags &&
                decoded_header.payload_len == header.payload_len &&
                decoded_header.payload_blocks == header.payload_blocks &&
                decoded_header.sequence == header.sequence,
            "C++ BCH/control-header decode round trip failed");
    std::vector<float> control_llrs;
    control_llrs.reserve(golden_control.size() * 2u);
    for (const auto label : golden_control) {
        control_llrs.push_back((label & 0x02u) == 0u ? 7.0f : -7.0f);
        control_llrs.push_back((label & 0x01u) == 0u ? 7.0f : -7.0f);
    }
    float soft_marker_metric = 0.0f;
    const auto soft_header =
        openisac::decode_control_qpsk_llrs(control_llrs, &soft_marker_metric);
    require(soft_marker_metric > 0.999f &&
                soft_header.sequence == header.sequence &&
                soft_header.flags == header.flags,
            "soft-LLR control-header decode failed");
    auto reliability_weighted_llrs = control_llrs;
    for (std::size_t bit = 0u; bit < 128u; ++bit) {
        const float magnitude = bit < 40u ? 0.1f : 5.0f;
        reliability_weighted_llrs[bit] =
            std::copysign(magnitude, control_llrs[bit]);
    }
    for (std::size_t bit = 0u; bit < 40u; ++bit) {
        reliability_weighted_llrs[bit] = -reliability_weighted_llrs[bit];
    }
    const auto reliability_header = openisac::decode_control_qpsk_llrs(
        reliability_weighted_llrs, &soft_marker_metric);
    require(soft_marker_metric > 0.95f &&
                reliability_header.sequence == header.sequence,
            "soft marker failed to down-weight unreliable hard errors");
    std::vector<std::uint8_t> reliability_hard_labels(golden_control.size());
    for (std::size_t label = 0u; label < reliability_hard_labels.size(); ++label) {
        reliability_hard_labels[label] = static_cast<std::uint8_t>(
            (reliability_weighted_llrs[label * 2u] < 0.0f ? 2u : 0u) |
            (reliability_weighted_llrs[label * 2u + 1u] < 0.0f ? 1u : 0u));
    }
    bool hard_marker_rejected = false;
    try {
        static_cast<void>(
            openisac::decode_control_qpsk_labels(reliability_hard_labels));
    } catch (const std::exception&) {
        hard_marker_rejected = true;
    }
    require(hard_marker_rejected,
            "hard marker unexpectedly accepted 40/128 marker-bit errors");
    auto corrected_control = golden_control;
    for (std::size_t bit = 0u; bit < 10u; ++bit) {
        const std::size_t label_index = 64u + bit / 2u;
        corrected_control[label_index] ^= static_cast<std::uint8_t>(
            1u << (1u - static_cast<unsigned>(bit % 2u)));
    }
    const auto corrected_header =
        openisac::decode_control_qpsk_labels(corrected_control);
    require(corrected_header.sequence == header.sequence &&
                corrected_header.flags == header.flags,
            "BCH failed to correct ten control-bit errors");

    const auto payload_labels = openisac::read_binary_vector<std::uint8_t>(
        openisac::join_path(root, "payload_labels_u8.bin"));
    const auto payload_symbols =
        openisac::read_binary_vector<std::complex<float>>(
            openisac::join_path(root, "payload_symbols_cf32.bin"));
    require(payload_labels.size() == 2432u, "payload label capacity changed");
    require(payload_symbols.size() == payload_labels.size(), "payload symbol shape changed");
    for (std::size_t index = 0; index < payload_labels.size(); ++index) {
        require_complex_close(
            SquareQAM::modulate(payload_labels[index], 6u),
            payload_symbols[index],
            1.0e-6f,
            "64-QAM payload symbol disagrees with Python");
    }

    const auto tx_grid = openisac::read_binary_vector<std::complex<float>>(
        openisac::join_path(root, "tx_grid_cf32.bin"));
    require(tx_grid.size() == 2u * 1024u * 2u, "TX grid shape changed");
    const auto tx_time = openisac::read_binary_vector<std::complex<float>>(
        openisac::join_path(root, "tx_time_cf32.bin"));
    require(tx_time.size() == 3u * 2u * 1152u, "time-domain frame shape changed");
    const auto zc_preamble = openisac::generate_zc_ofdm_symbol(1024u, 128u, 29u);
    for (std::size_t sample = 0u; sample < 1152u; ++sample) {
        require_complex_close(
            zc_preamble[sample], tx_time[sample], 2.0e-5f,
            "ZC preamble sample disagrees with Python");
        require_complex_close(
            tx_time[1152u + sample], {}, 1.0e-7f,
            "Tx1 must be silent during ZC preamble");
    }
    for (std::size_t time = 0u; time < 2u; ++time) {
        for (std::size_t tx = 0u; tx < 2u; ++tx) {
            std::vector<std::complex<float>> frequency(1024u);
            for (std::size_t fft = 0u; fft < 1024u; ++fft) {
                frequency[fft] = tx_grid[(time * 1024u + fft) * 2u + tx];
            }
            const auto samples = openisac::ofdm_modulate(frequency, 128u);
            const std::size_t golden_start = ((time + 1u) * 2u + tx) * 1152u;
            for (std::size_t sample = 0u; sample < 1152u; ++sample) {
                require_complex_close(
                    samples[sample],
                    tx_time[golden_start + sample],
                    2.0e-5f,
                    "data OFDM time sample disagrees with Python");
            }
        }
    }
    constexpr float inverse_sqrt_two = 0.7071067811865475244f;
    for (std::size_t payload = 0; payload < 1216u; ++payload) {
        const std::size_t time = layout.payload_time_indices[payload];
        const std::size_t data_position = layout.payload_data_positions[payload];
        const std::size_t fft = layout.data_fft_indices[data_position];
        for (std::size_t tx = 0; tx < 2u; ++tx) {
            const std::size_t grid_index = (time * 1024u + fft) * 2u + tx;
            require_complex_close(
                tx_grid[grid_index],
                payload_symbols[payload * 2u + tx] * inverse_sqrt_two,
                2.0e-6f,
                "payload grid mapping disagrees with Python");
        }
    }
    for (std::size_t control = 0; control < 128u; ++control) {
        const std::size_t fft =
            layout.data_fft_indices[layout.control_data_positions[control]];
        const std::size_t tx0 = fft * 2u;
        require_complex_close(
            tx_grid[tx0],
            SquareQAM::modulate(golden_control[control], 2u),
            1.0e-6f,
            "control grid mapping disagrees with Python");
        require_complex_close(
            tx_grid[tx0 + 1u], {}, 1.0e-7f, "Tx1 must be silent in control region");
    }
}

void test_ldpc_reusable_and_parallel_decoder() {
    const std::string matrix_root = OPENISAC_MATRIX_DIR;
    const openisac::Ldpc5041008 codec(
        openisac::join_path(matrix_root, "LDPC_504_1008.alist"),
        openisac::join_path(matrix_root, "LDPC_504_1008G.alist"));
    constexpr std::size_t blocks = 14u;
    std::vector<float> frame_llrs;
    std::vector<std::uint8_t> expected_information;
    frame_llrs.reserve(blocks * openisac::ldpc_encoded_bits);
    expected_information.reserve(blocks * openisac::ldpc_information_bits);
    for (std::size_t block = 0u; block < blocks; ++block) {
        std::vector<std::uint8_t> information(openisac::ldpc_information_bits);
        for (std::size_t bit = 0u; bit < information.size(); ++bit) {
            information[bit] = static_cast<std::uint8_t>(
                ((bit * 17u + block * 29u + 3u) >> 2u) & 1u);
        }
        const auto encoded = codec.encode(information);
        expected_information.insert(
            expected_information.end(), information.begin(), information.end());
        for (const auto bit : encoded) {
            frame_llrs.push_back(bit == 0u ? 8.0f : -8.0f);
        }
    }

    openisac::LdpcDecodeWorkspace workspace;
    openisac::LdpcDecodeResult first;
    const std::vector<float> first_block(
        frame_llrs.begin(), frame_llrs.begin() + openisac::ldpc_encoded_bits);
    codec.decode_normalized_min_sum(first_block, 6u, 0.8f, workspace, first);
    require(first.syndrome_weight == 0u &&
                std::equal(first.information_bits.begin(), first.information_bits.end(),
                           expected_information.begin()),
            "reusable LDPC workspace changed noiseless decode");
    const auto warm_growths = workspace.capacity_growths;
    codec.decode_normalized_min_sum(first_block, 6u, 0.8f, workspace, first);
    require(workspace.capacity_growths == warm_growths,
            "reusable LDPC workspace grew after warm-up");

    openisac::LdpcFrameDecoder decoder(codec, 4u);
    openisac::LdpcFrameDecodeResult parallel;
    decoder.decode_blocks(frame_llrs, blocks, 6u, 0.8f, parallel);
    require(parallel.syndrome_failures == 0u &&
                parallel.information_bits == expected_information,
            "parallel LDPC decoder disagrees with sequential information bits");
    require(parallel.maximum_iterations == 1u &&
                parallel.capacity_growths_this_frame > 0u,
            "parallel LDPC decoder did not report first-use state");
    decoder.decode_blocks(frame_llrs, blocks, 6u, 0.8f, parallel);
    require(parallel.capacity_growths_this_frame == 0u,
            "parallel LDPC worker buffers grew after warm-up");

    bool rejected_nan = false;
    try {
        decoder.decode_blocks(
            frame_llrs, blocks, 6u,
            std::numeric_limits<float>::quiet_NaN(), parallel);
    } catch (const std::invalid_argument&) {
        rejected_nan = true;
    }
    require(rejected_nan, "parallel LDPC decoder accepted NaN normalization");
}

void test_mimo() {
    const Channel2x2 identity{{1.0f, 0.0f}, {}, {}, {1.0f, 0.0f}};
    const std::array<std::complex<float>, 2> transmitted{{
        {0.3f, -0.7f}, {-0.2f, 0.5f}}};
    constexpr float inverse_sqrt_two = 0.7071067811865475244f;
    const std::array<std::complex<float>, 2> received{{
        transmitted[0] * inverse_sqrt_two,
        transmitted[1] * inverse_sqrt_two,
    }};
    const auto zf = openisac::detect_2x2(
        received, identity, 0.0f, LinearDetector::zf);
    require_complex_close(zf.symbols[0], transmitted[0], 1.0e-6f, "ZF layer 0 failed");
    require_complex_close(zf.symbols[1], transmitted[1], 1.0e-6f, "ZF layer 1 failed");
    require_close(zf.predicted_mse[0], 0.0f, 1.0e-7f, "ZF MSE failed");

    const auto mmse = openisac::detect_2x2(
        received, identity, 0.1f, LinearDetector::mmse);
    require_complex_close(
        mmse.symbols[0], transmitted[0] * (5.0f / 6.0f), 1.0e-6f, "MMSE bias failed");
    require_close(mmse.predicted_mse[0], 1.0f / 6.0f, 1.0e-6f, "MMSE MSE failed");
}

void test_scalable_mimo_detector() {
    constexpr std::size_t nmax = openisac::maximum_spatial_streams;
    for (const std::size_t streams : {4u, 8u}) {
        openisac::ChannelNxN channel;
        channel.streams = streams;
        for (std::size_t row = 0u; row < streams; ++row) {
            for (std::size_t column = 0u; column < streams; ++column) {
                if (row == column) {
                    channel.values[row * nmax + column] = {1.0f, 0.0f};
                } else {
                    const float sign = ((row + column) & 1u) != 0u ? -1.0f : 1.0f;
                    channel.values[row * nmax + column] =
                        {0.025f * sign, 0.015f * static_cast<float>(row) -
                                           0.01f * static_cast<float>(column)};
                }
            }
        }
        std::array<std::complex<float>, nmax> transmitted{};
        for (std::size_t stream = 0u; stream < streams; ++stream) {
            transmitted[stream] = SquareQAM::modulate(
                static_cast<unsigned>((stream * 13u + 7u) & 0x3Fu), 6u);
        }
        std::array<std::complex<float>, nmax> received{};
        const float scale = 1.0f / std::sqrt(static_cast<float>(streams));
        for (std::size_t row = 0u; row < streams; ++row) {
            for (std::size_t column = 0u; column < streams; ++column) {
                received[row] += channel.values[row * nmax + column] *
                                 transmitted[column] * scale;
            }
        }
        const auto detected = openisac::detect_nxn(
            received, channel, 0.0f, LinearDetector::zf);
        require(detected.streams == streams, "generic MIMO output rank failed");
        for (std::size_t stream = 0u; stream < streams; ++stream) {
            require_complex_close(
                detected.symbols[stream], transmitted[stream], 2.0e-4f,
                "generic 4x4/8x8 ZF recovery failed");
        }
    }

    openisac::ChannelNxN identity;
    identity.streams = 2u;
    identity.values[0u] = {1.0f, 0.0f};
    identity.values[nmax + 1u] = {1.0f, 0.0f};
    std::array<std::complex<float>, nmax> received{};
    received[0] = {0.3f * 0.70710678118f, -0.7f * 0.70710678118f};
    received[1] = {-0.2f * 0.70710678118f, 0.5f * 0.70710678118f};
    const auto mmse = openisac::detect_nxn(
        received, identity, 0.1f, LinearDetector::mmse);
    require_complex_close(
        mmse.symbols[0], std::complex<float>{0.3f, -0.7f} * (5.0f / 6.0f),
        1.0e-6f, "generic MMSE disagrees with specialized 2x2 detector");
    require_close(mmse.predicted_mse[0], 1.0f / 6.0f, 1.0e-6f,
                  "generic MMSE predicted MSE failed");

    require_close(
        openisac::condition_number_nxn(identity), 1.0f, 1.0e-5f,
        "generic identity condition number failed");
    openisac::ChannelNxN diagonal;
    diagonal.streams = 4u;
    diagonal.values[0u] = {4.0f, 0.0f};
    diagonal.values[nmax + 1u] = {2.0f, 0.0f};
    diagonal.values[2u * nmax + 2u] = {1.0f, 0.0f};
    diagonal.values[3u * nmax + 3u] = {0.5f, 0.0f};
    require_close(
        openisac::condition_number_nxn(diagonal), 8.0f, 1.0e-4f,
        "generic diagonal condition number failed");

    openisac::ChannelNxN physical_identity;
    physical_identity.streams = 4u;
    physical_identity.receive_ports = 4u;
    for (std::size_t port = 0u; port < 4u; ++port) {
        physical_identity.values[port * nmax + port] = {1.0f, 0.0f};
    }
    const auto effective =
        openisac::apply_fixed_dft_precoder_4x2(physical_identity);
    require(effective.streams == 2u && effective.receive_ports == 4u,
            "4x2 precoder produced the wrong rectangular channel shape");
    require_close(openisac::condition_number_nxn(effective), 1.0f, 1.0e-5f,
                  "semi-unitary 4x2 precoder condition number failed");
    const std::array<std::complex<float>, 2> rank2_symbols{{
        {0.3f, -0.7f}, {-0.2f, 0.5f}}};
    std::array<std::complex<float>, nmax> four_receive{};
    constexpr float rank2_scale = 0.70710678118654752440f;
    for (std::size_t rx = 0u; rx < 4u; ++rx) {
        for (std::size_t layer = 0u; layer < 2u; ++layer) {
            four_receive[rx] += effective.values[rx * nmax + layer] *
                rank2_symbols[layer] * rank2_scale;
        }
    }
    const auto rank2_detected = openisac::detect_nxn(
        four_receive, effective, 0.0f, LinearDetector::zf);
    for (std::size_t layer = 0u; layer < 2u; ++layer) {
        require_complex_close(
            rank2_detected.symbols[layer], rank2_symbols[layer], 1.0e-6f,
            "4Rx Rank-2 rectangular ZF recovery failed");
    }

}

void test_rank4_ofdm_algorithm_closure() {
    openisac::NxNOfdmSimulationConfig config;
    config.streams = 4u;
    config.frames = 2u;
    config.bits_per_symbol = 6u;
    config.snr_db = 45.0f;
    config.pilot_spacing = 2u;
    config.transmit_correlation = 0.2f;
    config.receive_correlation = 0.2f;
    config.channel_seed = 311383u;
    const auto result = openisac::simulate_nxn_ofdm_link(config);
    require(result.streams == 4u, "Rank-4 algorithm closure stream count failed");
    require(result.pilot_subcarriers == 448u,
            "Rank-4 dense FDM pilot allocation failed");
    require(result.data_subcarriers == 448u,
            "Rank-4 data allocation failed");
    require(result.detected_symbols == 3584u,
            "Rank-4 detected symbol count failed");
    require(result.ber < 0.01f,
            "Rank-4 uncoded BER exceeds first-stage threshold");
    require(result.evm_percent < 8.0f,
            "Rank-4 estimated-CSI EVM exceeds first-stage threshold");
    require(result.channel_nmse_db < -38.0f,
            "Rank-4 FDM pilot CSI NMSE exceeds first-stage threshold");
}

void test_rank4_formal_ldpc_crc_closure() {
    const std::string matrix_root = OPENISAC_MATRIX_DIR;
    const openisac::Ldpc5041008 codec(
        openisac::join_path(matrix_root, "LDPC_504_1008.alist"),
        openisac::join_path(matrix_root, "LDPC_504_1008G.alist"));
    openisac::Rank4FormalSimulationConfig config;
    config.frames = 1u;
    config.snr_db = 50.0f;
    config.modulation = Modulation::qam64;
    const auto result = openisac::simulate_rank4_formal_link(config, codec);
    require(result.user_payload_bytes_per_frame == 1132u,
            "Rank-4 formal payload capacity changed");
    require(result.ldpc_blocks_per_frame == 18u,
            "Rank-4 formal LDPC capacity changed");
    require(result.header_passes == 1u,
            "Rank-4 formal soft control failed");
    require(result.crc_passes == 1u && result.payload_matches == 1u,
            "Rank-4 formal LDPC/CRC closure failed");
    require(result.syndrome_failures == 0u,
            "Rank-4 formal LDPC syndrome failed");
    require(result.pre_fec_ber < 0.01f,
            "Rank-4 formal pre-FEC BER exceeds threshold");
    require(result.evm_percent < 8.0f,
            "Rank-4 formal EVM exceeds threshold");
    require(result.channel_nmse_db < -38.0f,
            "Rank-4 formal channel NMSE exceeds threshold");
}

void test_rank4_time_synchronization_closure() {
    const std::string matrix_root = OPENISAC_MATRIX_DIR;
    const openisac::Ldpc5041008 codec(
        openisac::join_path(matrix_root, "LDPC_504_1008.alist"),
        openisac::join_path(matrix_root, "LDPC_504_1008G.alist"));
    openisac::Rank4TimeSimulationConfig config;
    config.snr_db = 50.0f;
    config.modulation = Modulation::qam64;
    config.sfo_ppm = 20.0f;
    config.cfo_hz = 300.0f;
    config.csi_smoothing_alpha = 1.0f;
    openisac::Rank4TimeReceiverState state;
    openisac::Rank4TimeWorkspace workspace;
    openisac::LdpcFrameDecoder decoder(codec, 4u);

    auto baseline_config = config;
    baseline_config.snr_db = 45.0f;
    baseline_config.transmit_correlation = 0.02f;
    baseline_config.receive_correlation = 0.02f;
    baseline_config.average_intra_frame_csi = false;
    baseline_config.mmse_regularization_scale = 1.0f;
    baseline_config.random_seed = 0x80A0u;
    auto optimized_config = baseline_config;
    optimized_config.average_intra_frame_csi = true;
    optimized_config.mmse_regularization_scale = 0.5f;
    const auto baseline = openisac::simulate_rank4_time_frame(
        49000u, baseline_config, codec);
    const auto optimized = openisac::simulate_rank4_time_frame(
        49000u, optimized_config, codec);
    require(baseline.crc_ok && optimized.crc_ok &&
                optimized.evm_percent < baseline.evm_percent &&
                optimized.pre_fec_ber <= baseline.pre_fec_ber,
            "Rank-4 low-complexity EVM optimization regressed");

    auto rank2_config = config;
    rank2_config.spatial_rank = 2u;
    rank2_config.snr_db = 45.0f;
    rank2_config.transmit_correlation = 0.2f;
    rank2_config.receive_correlation = 0.2f;
    for (const auto pilot_mode : {
             openisac::PilotMode::fdm,
             openisac::PilotMode::nr_dmrs}) {
        rank2_config.pilot_mode = pilot_mode;
        rank2_config.random_seed += 0x101u;
        const auto rank2 = openisac::simulate_rank4_time_frame(
            static_cast<std::uint16_t>(49100u +
                (pilot_mode == openisac::PilotMode::nr_dmrs)),
            rank2_config, codec);
        require(rank2.spatial_rank == 2u && rank2.timing_ok &&
                    rank2.header_ok && rank2.crc_ok && rank2.payload_match &&
                    rank2.syndrome_failures == 0u,
                "4Tx/4Rx Rank-2 time-domain LDPC/CRC closure failed");
        require(rank2.evm_percent < 8.0f && rank2.pre_fec_ber < 0.01f,
                "4Tx/4Rx Rank-2 EVM/BER exceeded threshold");
    }

    for (std::size_t frame = 0u; frame < 3u; ++frame) {
        config.timing_offset_samples = 20u + frame;
        config.random_seed = static_cast<std::uint32_t>(0x8100u + frame * 31u);
        const auto result = openisac::simulate_rank4_time_frame(
            static_cast<std::uint16_t>(frame), config, codec, &state,
            &workspace, &decoder);
        require(result.timing_ok && result.header_ok && result.crc_ok &&
                    result.payload_match && result.syndrome_failures == 0u,
                "Rank-4 time-domain synchronization/LDPC closure failed");
        require(std::abs(result.cfo_error_hz) < 10.0f &&
                    std::abs(result.residual_sfo_ppm) < 0.1f,
                "Rank-4 CFO/SFO correction exceeded threshold");
        require(result.evm_percent < 8.0f,
                "Rank-4 synchronized EVM exceeded threshold");
        require(result.pre_fec_ber < 0.01f,
                "Rank-4 synchronized pre-FEC BER exceeded threshold");
        require(result.ldpc_worker_threads == 4u,
                "Rank-4 did not use persistent LDPC worker pool");
        if (frame > 0u) {
            require(result.workspace_growths_this_frame == 0u &&
                        result.ldpc_capacity_growths_this_frame == 0u,
                    "warmed Rank-4 receive path grew buffers");
        }
        if (frame == 0u) {
            require(result.synchronization_mode_used ==
                        openisac::Rank4SynchronizationMode::search &&
                        result.timing_candidates_evaluated > 5u,
                    "Rank-4 first frame did not perform full search");
        } else {
            require(result.synchronization_mode_used ==
                        openisac::Rank4SynchronizationMode::track &&
                        !result.tracking_fallback &&
                        result.timing_candidates_evaluated <= 5u,
                    "Rank-4 locked frame did not use tracking window");
        }
    }
    config.timing_offset_samples = 31u;
    config.random_seed = 0x8200u;
    const auto jump = openisac::simulate_rank4_time_frame(
        10u, config, codec, &state, &workspace, &decoder);
    require(jump.timing_ok && jump.header_ok && jump.crc_ok &&
                jump.payload_match && jump.tracking_fallback &&
                jump.synchronization_mode_used ==
                    openisac::Rank4SynchronizationMode::reacquire &&
                state.reacquisition_count == 1u,
            "Rank-4 timing jump did not reacquire and preserve payload");

    std::vector<std::uint8_t> video_payload(777u);
    for (std::size_t index = 0u; index < video_payload.size(); ++index) {
        video_payload[index] = static_cast<std::uint8_t>(
            (index * 37u + 19u) & 0xFFu);
    }
    config.timing_offset_samples = 31u;
    config.random_seed = 0x8280u;
    const auto explicit_payload = openisac::simulate_rank4_time_payload_frame(
        video_payload, 11u, config, codec, &state, &workspace, &decoder);
    require(explicit_payload.crc_ok && explicit_payload.payload_match &&
                explicit_payload.user_payload == video_payload,
            "Rank-4 explicit video payload did not round-trip byte-for-byte");
}

void test_rank4_time_double_buffer_pipeline() {
    const std::string matrix_root = OPENISAC_MATRIX_DIR;
    const openisac::Ldpc5041008 codec(
        openisac::join_path(matrix_root, "LDPC_504_1008.alist"),
        openisac::join_path(matrix_root, "LDPC_504_1008G.alist"));
    openisac::Rank4TimeSimulationConfig config;
    config.snr_db = 50.0f;
    config.modulation = Modulation::qam64;
    config.sfo_ppm = 20.0f;
    config.cfo_hz = 300.0f;
    config.csi_smoothing_alpha = 1.0f;
    openisac::Rank4TimePipeline pipeline(codec, 4u);
    for (std::size_t warm = 0u; warm < pipeline.slot_count(); ++warm) {
        config.random_seed = static_cast<std::uint32_t>(0x8300u + warm * 17u);
        pipeline.submit(
            warm, static_cast<std::uint16_t>(50000u + warm), config);
    }
    for (std::size_t warm = 0u; warm < pipeline.slot_count(); ++warm) {
        const auto result = pipeline.receive();
        require(result.link.crc_ok && result.link.payload_match,
                "Rank-4 pipeline warm-up failed");
    }

    constexpr std::size_t frame_count = 4u;
    std::vector<openisac::Rank4TimePipelineResult> results;
    results.reserve(frame_count);
    for (std::size_t frame = 0u; frame < frame_count; ++frame) {
        if (frame >= pipeline.slot_count()) {
            results.push_back(pipeline.receive());
        }
        config.random_seed = static_cast<std::uint32_t>(0x8400u + frame * 19u);
        pipeline.submit(
            frame, static_cast<std::uint16_t>(frame), config);
    }
    while (results.size() < frame_count) {
        results.push_back(pipeline.receive());
    }
    for (std::size_t frame = 0u; frame < frame_count; ++frame) {
        const auto& result = results[frame];
        require(result.frame_id == frame && result.link.timing_ok &&
                    result.link.header_ok && result.link.crc_ok &&
                    result.link.payload_match &&
                    result.link.syndrome_failures == 0u,
                "Rank-4 pipeline order/CRC closure failed");
        require(result.link.ldpc_worker_threads == 4u &&
                    result.link.workspace_growths_this_frame == 0u &&
                    result.link.ldpc_capacity_growths_this_frame == 0u,
                "warmed Rank-4 pipeline grew buffers or lost workers");
        require(result.timing.receiver_front_us > 0.0 &&
                    result.timing.fec_wall_us > 0.0 &&
                    result.timing.latency_us >= result.timing.fec_wall_us,
                "Rank-4 pipeline timing was not populated");
    }

    std::vector<std::uint8_t> explicit_payload(991u);
    for (std::size_t index = 0u; index < explicit_payload.size(); ++index) {
        explicit_payload[index] = static_cast<std::uint8_t>(
            (index * 53u + 7u) & 0xFFu);
    }
    config.random_seed = 0x8500u;
    pipeline.submit_payload(100u, explicit_payload, 100u, config);
    const auto explicit_result = pipeline.receive();
    require(explicit_result.frame_id == 100u &&
                explicit_result.link.crc_ok &&
                explicit_result.link.payload_match &&
                explicit_result.link.user_payload == explicit_payload,
            "Rank-4 double-buffer pipeline lost explicit video payload bytes");
}

void test_rank4_time_sensing_frontend() {
    const std::string matrix_root = OPENISAC_MATRIX_DIR;
    const openisac::Ldpc5041008 codec(
        openisac::join_path(matrix_root, "LDPC_504_1008.alist"),
        openisac::join_path(matrix_root, "LDPC_504_1008G.alist"));
    openisac::Rank4TimeSimulationConfig link_config;
    link_config.snr_db = 50.0f;
    link_config.modulation = Modulation::qam64;
    link_config.sfo_ppm = 20.0f;
    link_config.cfo_hz = 300.0f;
    link_config.csi_smoothing_alpha = 1.0f;
    link_config.enable_sensing_snapshot = true;
    link_config.taps = {
        {0u, 0.0f, 0.0f, 0.0f},
        {5u, -3.0f, 27.0f, 69.444444f},
    };
    openisac::DynamicSensingConfig sensing_config;
    sensing_config.transmit_ports = 4u;
    sensing_config.receive_ports = 4u;
    sensing_config.coherent_frames = 64u;
    sensing_config.doppler_fft_size = 64u;
    sensing_config.enable_static_clutter_suppression = true;
    sensing_config.cfar_false_alarm_probability = 1.0e-6f;
    openisac::DynamicSensingProcessor sensing(sensing_config);
    openisac::Rank4TimeReceiverState state;
    openisac::Rank4TimeWorkspace workspace;
    openisac::LdpcFrameDecoder decoder(codec, 4u);
    std::size_t crc_successes = 0u;
    for (std::size_t frame = 0u;
         frame < sensing_config.coherent_frames; ++frame) {
        link_config.channel_time_seconds = frame * 225.0e-6;
        link_config.random_seed = static_cast<std::uint32_t>(
            0x8680u + frame * 31u);
        const auto result = openisac::simulate_rank4_time_frame(
            static_cast<std::uint16_t>(frame), link_config, codec,
            &state, &workspace, &decoder);
        require(result.sensing_channel_frequency_response.size() ==
                    16u * 1024u &&
                    result.sensing_active_subcarrier_mask.size() == 1024u,
                "Rank-4 sensing snapshot dimensions changed");
        const bool ready = sensing.push_channel_frame(
            frame, frame * 225'000u,
            result.sensing_channel_frequency_response,
            result.sensing_active_subcarrier_mask);
        require(ready == (frame + 1u == sensing_config.coherent_frames),
                "Rank-4 sensing batch completed at the wrong frame");
        if (result.crc_ok && result.payload_match) {
            ++crc_successes;
        }
    }
    const auto& result = sensing.last_result();
    const std::size_t expected_doppler =
        sensing_config.doppler_fft_size / 2u + 1u;
    const auto range_error = result.strongest_peak.range_bin > 5u
        ? result.strongest_peak.range_bin - 5u
        : 5u - result.strongest_peak.range_bin;
    const auto doppler_error =
        result.strongest_peak.doppler_bin > expected_doppler
        ? result.strongest_peak.doppler_bin - expected_doppler
        : expected_doppler - result.strongest_peak.doppler_bin;
    const bool cfar_found = std::any_of(
        result.detections.begin(), result.detections.end(),
        [&](const openisac::DynamicSensingDetection& detection) {
            const auto range_delta = detection.peak.range_bin > 5u
                ? detection.peak.range_bin - 5u
                : 5u - detection.peak.range_bin;
            const auto doppler_delta =
                detection.peak.doppler_bin > expected_doppler
                ? detection.peak.doppler_bin - expected_doppler
                : expected_doppler - detection.peak.doppler_bin;
            return range_delta <= 1u && doppler_delta <= 1u;
        });
    require(crc_successes == sensing_config.coherent_frames &&
                result.ready && range_error <= 1u && doppler_error <= 1u &&
                cfar_found,
            "Rank-4 communication/range-Doppler/CFAR closure failed");
}

void test_dynamic_rank_mcs_frames() {
    const std::string matrix_root = OPENISAC_MATRIX_DIR;
    const openisac::Ldpc5041008 codec(
        openisac::join_path(matrix_root, "LDPC_504_1008.alist"),
        openisac::join_path(matrix_root, "LDPC_504_1008G.alist"));
    const std::array<LinkMode, 20> modes{{
        {1u, Modulation::qpsk}, {1u, Modulation::qam16},
        {1u, Modulation::qam64}, {1u, Modulation::qam256},
        {2u, Modulation::qpsk}, {2u, Modulation::qam16},
        {2u, Modulation::qam64}, {2u, Modulation::qam256},
        {4u, Modulation::qpsk}, {4u, Modulation::qam16},
        {4u, Modulation::qam64}, {4u, Modulation::qam256},
        {1u, Modulation::qpsk, openisac::TransmissionScheme::alamouti_stbc},
        {1u, Modulation::qam16, openisac::TransmissionScheme::alamouti_stbc},
        {1u, Modulation::qam64, openisac::TransmissionScheme::alamouti_stbc},
        {1u, Modulation::qam256, openisac::TransmissionScheme::alamouti_stbc},
        {2u, Modulation::qpsk,
         openisac::TransmissionScheme::spatial_multiplexing, 4u},
        {2u, Modulation::qam16,
         openisac::TransmissionScheme::spatial_multiplexing, 4u},
        {2u, Modulation::qam64,
         openisac::TransmissionScheme::spatial_multiplexing, 4u},
        {2u, Modulation::qam256,
         openisac::TransmissionScheme::spatial_multiplexing, 4u},
    }};
    require(openisac::transmit_rank_flag(4u) == 0x02u &&
                openisac::transmit_rank_from_flags(0x02u) == 4u &&
                openisac::transmission_mode_flag({
                    1u, Modulation::qpsk,
                    openisac::TransmissionScheme::alamouti_stbc}) == 0x03u &&
                openisac::transmit_rank_from_flags(0x03u) == 1u &&
                openisac::transmission_scheme_from_flags(0x03u) ==
                    openisac::TransmissionScheme::alamouti_stbc,
            "Rank-4/STBC control flag mapping failed");
    std::uint16_t sequence = 100u;
    for (const auto mode : modes) {
        FormalFrameProfile profile;
        profile.transmit_rank = mode.rank;
        profile.bits_per_symbol = openisac::modulation_bits(mode.modulation);
        profile.scheme = mode.scheme;
        if (openisac::physical_transmit_ports(mode) == 4u) {
            profile.pilot_spacing = 2u;
        }
        const auto expected_layout = openisac::build_formal_frame_layout(profile);
        std::vector<std::uint8_t> payload(expected_layout.user_payload_bytes);
        for (std::size_t index = 0u; index < payload.size(); ++index) {
            payload[index] = static_cast<std::uint8_t>(
                (index * 37u + sequence * 11u) & 0xFFu);
        }
        const auto encoded = openisac::encode_dynamic_frame(
            payload, mode, sequence, codec);
        require(encoded.layout.ldpc_blocks == expected_layout.ldpc_blocks,
                "dynamic frame LDPC capacity changed");
        require(encoded.header.payload_blocks == expected_layout.ldpc_blocks,
                "dynamic frame header block count failed");
        require(openisac::bits_per_symbol_from_flags(encoded.header.flags) ==
                    profile.bits_per_symbol &&
                    openisac::transmit_rank_from_flags(encoded.header.flags) == mode.rank &&
                    openisac::transmission_scheme_from_flags(encoded.header.flags) ==
                        mode.scheme,
                "dynamic frame transmission-mode/MCS flags failed");
        require(encoded.payload_symbols.size() == expected_layout.payload_layer_symbols,
                "dynamic frame payload symbol capacity failed");
        const std::size_t physical_ports =
            openisac::physical_transmit_ports(mode);
        require(encoded.physical_ports == physical_ports,
                "dynamic frame physical port count failed");
        require(encoded.tx_grid.size() == 2u * 1024u * physical_ports,
                "dynamic frame resource grid shape failed");
        if (mode.rank == 4u) {
            require(encoded.layout.data_fft_indices.size() == 448u &&
                        encoded.layout.pilot_fft_indices.size() == 440u &&
                        encoded.layout.phase_reference_fft_indices.size() == 8u &&
                        encoded.layout.payload_layer_symbols == 3072u,
                    "Rank-4 formal resource allocation changed");
            std::vector<std::complex<float>> reference_grid;
            openisac::build_dynamic_pilot_reference_grid(
                0xC057u, mode, reference_grid);
            require(reference_grid.size() == encoded.tx_grid.size(),
                    "Rank-4 receiver pilot reference shape failed");
            for (const auto fft : encoded.layout.pilot_fft_indices) {
                for (std::size_t time = 0u; time < 2u; ++time) {
                    for (std::size_t port = 0u; port < physical_ports; ++port) {
                        const std::size_t index =
                            (time * 1024u + fft) * physical_ports + port;
                        require_complex_close(
                            reference_grid[index], encoded.tx_grid[index], 1.0e-7f,
                            "Rank-4 receiver pilot reference mismatch");
                    }
                }
            }
        }
        if (mode.rank == 1u &&
            mode.scheme == openisac::TransmissionScheme::spatial_multiplexing) {
            for (std::size_t payload_index = 0u;
                 payload_index < encoded.layout.payload_time_indices.size();
                 ++payload_index) {
                const std::size_t time = encoded.layout.payload_time_indices[payload_index];
                const std::size_t data = encoded.layout.payload_data_positions[payload_index];
                const std::size_t fft = encoded.layout.data_fft_indices[data];
                require_complex_close(
                    encoded.tx_grid[(time * 1024u + fft) * 2u + 1u], {}, 1.0e-7f,
                    "Rank-1 payload must leave Tx1 silent");
            }
        }
        if (mode.scheme == openisac::TransmissionScheme::alamouti_stbc) {
            const std::size_t pairs = encoded.layout.payload_time_indices.size() / 2u;
            require(encoded.layout.payload_time_indices.size() == pairs * 2u &&
                        encoded.layout.payload_layer_symbols ==
                            encoded.layout.payload_time_indices.size(),
                    "STBC layout must contain equal paired slots");
            constexpr float scale = 0.70710678118654752440f;
            for (std::size_t pair = 0u; pair < pairs; ++pair) {
                const std::size_t second = pair + pairs;
                const std::size_t data = encoded.layout.payload_data_positions[pair];
                const std::size_t fft = encoded.layout.data_fft_indices[data];
                require(encoded.layout.payload_time_indices[pair] == 0u &&
                            encoded.layout.payload_time_indices[second] == 1u &&
                            encoded.layout.payload_data_positions[second] == data,
                        "STBC payload resource pairs changed");
                require_complex_close(
                    encoded.tx_grid[fft * 2u],
                    encoded.payload_symbols[pair] * scale, 1.0e-7f,
                    "STBC first-slot Tx0 mapping failed");
                require_complex_close(
                    encoded.tx_grid[fft * 2u + 1u],
                    encoded.payload_symbols[second] * scale, 1.0e-7f,
                    "STBC first-slot Tx1 mapping failed");
                require_complex_close(
                    encoded.tx_grid[(1024u + fft) * 2u],
                    -std::conj(encoded.payload_symbols[second]) * scale, 1.0e-7f,
                    "STBC second-slot Tx0 mapping failed");
                require_complex_close(
                    encoded.tx_grid[(1024u + fft) * 2u + 1u],
                    std::conj(encoded.payload_symbols[pair]) * scale, 1.0e-7f,
                    "STBC second-slot Tx1 mapping failed");
            }
        }
        if (physical_ports == 4u && mode.rank == 2u) {
            constexpr float scale = 0.70710678118654752440f;
            for (std::size_t payload = 0u;
                 payload < encoded.layout.payload_time_indices.size(); ++payload) {
                const std::size_t time =
                    encoded.layout.payload_time_indices[payload];
                const std::size_t data =
                    encoded.layout.payload_data_positions[payload];
                const std::size_t fft = encoded.layout.data_fft_indices[data];
                for (std::size_t tx = 0u; tx < 4u; ++tx) {
                    std::complex<float> expected{};
                    for (std::size_t layer = 0u; layer < 2u; ++layer) {
                        expected += openisac::fixed_dft_precoder_4x2(tx, layer) *
                            encoded.payload_symbols[payload * 2u + layer] * scale;
                    }
                    require_complex_close(
                        encoded.tx_grid[
                            (time * 1024u + fft) * physical_ports + tx],
                        expected, 1.0e-7f,
                        "four-port Rank-2 DFT precoder mapping failed");
                }
            }
        }
        const std::vector<float> variances(encoded.payload_symbols.size(), 1.0e-3f);
        openisac::DecodedDynamicFrame decoded;
        if (physical_ports == 4u && mode.rank == 2u) {
            openisac::PreparedDynamicFrame prepared;
            openisac::prepare_dynamic_frame_payload_llrs(
                encoded.header, 1.0f, mode, encoded.payload_symbols,
                variances, prepared);
            decoded = openisac::decode_prepared_dynamic_frame(prepared, codec);
        } else {
            decoded = openisac::decode_dynamic_frame(
                encoded.control_labels, encoded.payload_symbols,
                variances, codec);
        }
        require(decoded.mode == mode, "dynamic receiver ignored control Rank/MCS");
        require(decoded.header.sequence == sequence,
                "dynamic receiver sequence mismatch");
        require(decoded.syndrome_failures == 0u && decoded.crc_ok,
                "dynamic frame noiseless LDPC/CRC failed");
        require(decoded.user_payload == payload,
                "dynamic frame payload round trip failed");
        ++sequence;
    }
}

void test_link_adaptation() {
    const Channel2x2 identity{{1.0f, 0.0f}, {}, {}, {1.0f, 0.0f}};
    std::vector<Channel2x2> channels(16u, identity);
    std::vector<std::array<float, 2>> mse(16u, {{0.009f, 0.009f}});
    const auto decision = openisac::recommend_rank_mcs(
        channels, mse, 0.01f, LinearDetector::mmse, Modulation::qam64);
    require(decision.desired == LinkMode{2u, Modulation::qam64}, "rank/MCS decision failed");
    require(decision.configured_mcs_supported, "configured 64-QAM should be supported");

    const Channel2x2 weak_second_mode{
        {1.0f, 0.0f}, {}, {}, {0.17320508f, 0.0f}};
    const std::vector<Channel2x2> weak_channels(16u, weak_second_mode);
    const float rank2_mse_15db = 1.0f / (1.0f + std::pow(10.0f, 1.5f));
    const std::vector<std::array<float, 2>> weak_mse(
        16u, {{rank2_mse_15db, rank2_mse_15db}});
    const auto prefer_rank1 = openisac::recommend_rank_mcs(
        weak_channels, weak_mse, 1.0e-4f,
        LinearDetector::mmse, Modulation::qam64);
    require(prefer_rank1.desired == LinkMode{1u, Modulation::qam256},
            "Rank selection must compare actual Rank-times-Qm capacity");
    const float rank2_mse_22db = 1.0f / (1.0f + std::pow(10.0f, 2.2f));
    const std::vector<std::array<float, 2>> strong_mse(
        16u, {{rank2_mse_22db, rank2_mse_22db}});
    const auto prefer_rank2 = openisac::recommend_rank_mcs(
        weak_channels, strong_mse, 1.0e-4f,
        LinearDetector::mmse, Modulation::qam64);
    require(prefer_rank2.desired == LinkMode{2u, Modulation::qam64},
            "high-SINR weak spatial mode should recover Rank-2 throughput");

    AdaptiveLinkController controller({1u, Modulation::qpsk}, 3u);
    const LinkMode desired{2u, Modulation::qam64};
    require(
        controller.observe(desired).reason == ControllerReason::upshift_hysteresis,
        "first upshift must wait");
    require(
        controller.observe(desired).reason == ControllerReason::upshift_hysteresis,
        "second upshift must wait");
    const auto third = controller.observe(desired);
    require(
        third.reason == ControllerReason::confirmed_step_upshift,
        "third upshift must execute");
    require(third.selected == LinkMode{1u, Modulation::qam16}, "upshift step changed");

    AdaptiveLinkController fast({2u, Modulation::qam256}, 3u);
    const auto down = fast.observe({2u, Modulation::qam256}, true, false);
    require(
        down.selected == LinkMode{2u, Modulation::qam64},
        "CRC fast downshift failed");
}

void test_cross_frame_rank_mcs_loop() {
    const std::string matrix_root = OPENISAC_MATRIX_DIR;
    const openisac::Ldpc5041008 codec(
        openisac::join_path(matrix_root, "LDPC_504_1008.alist"),
        openisac::join_path(matrix_root, "LDPC_504_1008G.alist"));
    AdaptiveLinkController controller({1u, Modulation::qpsk}, 3u);
    const std::array<LinkMode, 10> expected_modes{{
        {1u, Modulation::qpsk}, {1u, Modulation::qpsk},
        {1u, Modulation::qpsk}, {1u, Modulation::qam16},
        {1u, Modulation::qam16}, {1u, Modulation::qam16},
        {1u, Modulation::qam64}, {1u, Modulation::qam64},
        {1u, Modulation::qam64}, {2u, Modulation::qam64},
    }};
    const std::vector<std::uint8_t> payload(60u, 0xA5u);
    for (std::size_t frame = 0u; frame < expected_modes.size(); ++frame) {
        require(controller.current() == expected_modes[frame],
                "cross-frame controller selected an unexpected mode");
        const auto encoded = openisac::encode_dynamic_frame(
            payload, controller.current(), static_cast<std::uint16_t>(frame), codec);
        const auto received_header =
            openisac::decode_control_qpsk_labels(encoded.control_labels);
        require(openisac::transmit_rank_from_flags(received_header.flags) ==
                    expected_modes[frame].rank &&
                    openisac::bits_per_symbol_from_flags(received_header.flags) ==
                    openisac::modulation_bits(expected_modes[frame].modulation),
                "next-frame encoder did not apply selected Rank/MCS");
        const std::vector<float> variances(encoded.payload_symbols.size(), 1.0e-3f);
        const auto decoded = openisac::decode_dynamic_frame(
            encoded.control_labels, encoded.payload_symbols, variances, codec);
        require(decoded.crc_ok && decoded.user_payload == payload,
                "cross-frame payload feedback decode failed");
        if (frame + 1u < expected_modes.size()) {
            controller.observe({2u, Modulation::qam64}, !decoded.crc_ok, false);
        }
    }
}

void test_dynamic_tdl_receive_chain() {
    const std::string matrix_root = OPENISAC_MATRIX_DIR;
    const openisac::Ldpc5041008 codec(
        openisac::join_path(matrix_root, "LDPC_504_1008.alist"),
        openisac::join_path(matrix_root, "LDPC_504_1008G.alist"));
    openisac::DynamicLinkSimulationConfig config;
    config.snr_db = 45.0f;
    config.random_seed = 0x5100u;
    for (const auto mode : {
             LinkMode{1u, Modulation::qam256},
             LinkMode{2u, Modulation::qam64},
             LinkMode{1u, Modulation::qpsk,
                      openisac::TransmissionScheme::alamouti_stbc},
             LinkMode{1u, Modulation::qam16,
                      openisac::TransmissionScheme::alamouti_stbc},
             LinkMode{1u, Modulation::qam64,
                      openisac::TransmissionScheme::alamouti_stbc},
             LinkMode{1u, Modulation::qam256,
                      openisac::TransmissionScheme::alamouti_stbc}}) {
        const auto result = openisac::simulate_dynamic_tdl_frame(
            mode, 0x3210u, codec, config, nullptr, nullptr);
        require(result.timing_ok && result.header_ok,
                "dynamic TDL synchronization/control decode failed");
        require(result.decoded_mode == mode,
                "dynamic TDL receiver ignored decoded Rank/MCS");
        require(result.crc_ok && result.syndrome_failures == 0u,
                "dynamic TDL LDPC/CRC receive chain failed");
        require(std::abs(result.cfo_error_hz) < 20.0f,
                "dynamic TDL CFO estimate left excessive error");
    }

    std::vector<std::uint8_t> application_payload(777u);
    for (std::size_t index = 0u; index < application_payload.size(); ++index) {
        application_payload[index] = static_cast<std::uint8_t>(
            (index * 53u + 19u) & 0xFFu);
    }
    openisac::DynamicLinkWorkspace generation_workspace;
    openisac::DynamicLinkWorkspace receiver_workspace;
    openisac::DynamicLinkReceiverState receiver_state;
    openisac::DynamicLinkIqFrame iq_frame;
    openisac::PreparedDynamicLinkFrame prepared;
    config.random_seed = 0x51A5u;
    openisac::generate_dynamic_tdl_iq_frame(
        application_payload, {2u, Modulation::qam64}, 0x4321u, codec,
        config, iq_frame, generation_workspace);
    openisac::prepare_dynamic_iq_frame(
        iq_frame, prepared, &receiver_state, receiver_workspace);
    const auto application_result = openisac::finish_dynamic_tdl_frame(
        prepared, codec, receiver_workspace);
    require(application_result.crc_ok &&
                application_result.user_payload == application_payload,
            "application payload did not survive the dynamic PHY byte-for-byte");
}

void test_cross_frame_csi_smoothing() {
    const std::string matrix_root = OPENISAC_MATRIX_DIR;
    const openisac::Ldpc5041008 codec(
        openisac::join_path(matrix_root, "LDPC_504_1008.alist"),
        openisac::join_path(matrix_root, "LDPC_504_1008G.alist"));
    openisac::DynamicLinkReceiverState state;
    double raw_nmse_linear = 0.0;
    double filtered_nmse_linear = 0.0;
    constexpr std::size_t frames = 12u;
    for (std::size_t frame = 0u; frame < frames; ++frame) {
        openisac::DynamicLinkSimulationConfig config;
        config.snr_db = 32.0f;
        config.csi_smoothing_alpha = 0.25f;
        config.random_seed = static_cast<unsigned>(0x7200u + frame * 19u);
        const auto raw = openisac::simulate_dynamic_tdl_frame(
            {1u, Modulation::qam16}, static_cast<std::uint16_t>(frame),
            codec, config, nullptr, nullptr);
        const auto filtered = openisac::simulate_dynamic_tdl_frame(
            {1u, Modulation::qam16}, static_cast<std::uint16_t>(frame),
            codec, config, &state, nullptr);
        require(filtered.timing_ok && filtered.header_ok,
                "cross-frame CSI smoothing lost synchronization/control");
        if (frame > 0u) {
            require(filtered.csi_smoothed,
                    "persistent receiver failed to apply CSI smoothing");
            raw_nmse_linear += std::pow(10.0, raw.channel_nmse_db / 10.0);
            filtered_nmse_linear +=
                std::pow(10.0, filtered.channel_nmse_db / 10.0);
        }
    }
    require(state.csi_valid && state.csi_age_frames == frames,
            "persistent CSI state age is incorrect");
    require(filtered_nmse_linear < raw_nmse_linear,
            "cross-frame CSI smoothing did not reduce mean linear NMSE");
    const auto resets_before = state.reset_count;
    openisac::DynamicLinkSimulationConfig outage_config;
    outage_config.snr_db = -20.0f;
    outage_config.csi_smoothing_alpha = 0.25f;
    outage_config.random_seed = 0xDEADu;
    const auto outage = openisac::simulate_dynamic_tdl_frame(
        {1u, Modulation::qpsk}, 0xFFFFu, codec, outage_config, &state, nullptr);
    require(!outage.timing_ok || !outage.header_ok,
            "forced outage unexpectedly retained synchronization/control");
    require(!state.csi_valid && state.csi_age_frames == 0u &&
                state.filtered_channels.empty() &&
                state.reset_count == resets_before + 1u,
            "synchronization/control failure did not clear CSI history");
}

void test_continuous_synchronization_tracking() {
    const std::string matrix_root = OPENISAC_MATRIX_DIR;
    const openisac::Ldpc5041008 codec(
        openisac::join_path(matrix_root, "LDPC_504_1008.alist"),
        openisac::join_path(matrix_root, "LDPC_504_1008G.alist"));
    openisac::DynamicLinkReceiverState state;
    openisac::DynamicLinkWorkspace workspace;
    openisac::LdpcFrameDecoder decoder(codec, 4u);
    openisac::DynamicLinkSimulationConfig config;
    config.snr_db = 45.0f;
    config.tracking_half_window_samples = 2u;
    config.tracking_metric_ratio = 0.9f;

    for (std::size_t frame = 0u; frame < 5u; ++frame) {
        config.timing_offset_samples = 20u + frame / 2u;
        config.random_seed = static_cast<unsigned>(0xA100u + frame * 31u);
        const auto result = openisac::simulate_dynamic_tdl_frame(
            {2u, Modulation::qam64}, static_cast<std::uint16_t>(frame),
            codec, config, &state, &workspace, &decoder);
        require(result.timing_ok && result.header_ok && result.crc_ok,
                "continuous synchronization lost a slowly drifting frame");
        if (frame == 0u) {
            require(
                result.synchronization_mode_used ==
                    openisac::SynchronizationMode::search &&
                    result.timing_candidates_evaluated > 5u,
                "first continuous frame did not use full acquisition");
        } else {
            require(
                result.synchronization_mode_used ==
                    openisac::SynchronizationMode::track &&
                    !result.tracking_fallback &&
                    result.timing_candidates_evaluated <= 5u,
                "locked continuous frame did not use the small timing window");
        }
    }
    require(
        state.synchronization_state == openisac::SynchronizationMode::track &&
            state.synchronization_lock_age_frames == 5u &&
            state.full_search_count == 1u && state.tracking_search_count == 4u,
        "continuous synchronization state counters are incorrect");

    // A timing jump larger than the tracking window must invoke full-search
    // fallback in the same frame and preserve payload delivery.
    config.timing_offset_samples = 31u;
    config.random_seed = 0xA200u;
    const auto jump = openisac::simulate_dynamic_tdl_frame(
        {2u, Modulation::qam64}, 10u, codec, config,
        &state, &workspace, &decoder);
    require(jump.timing_ok && jump.header_ok && jump.crc_ok &&
                jump.tracking_fallback &&
                jump.synchronization_mode_used ==
                    openisac::SynchronizationMode::reacquire &&
                state.reacquisition_count == 1u,
            "tracking-window miss did not fall back to full acquisition");

    const auto resets_before = state.reset_count;
    config.snr_db = -20.0f;
    config.random_seed = 0xA300u;
    const auto outage = openisac::simulate_dynamic_tdl_frame(
        {1u, Modulation::qpsk}, 11u, codec, config,
        &state, &workspace, &decoder);
    require((!outage.timing_ok || !outage.header_ok) &&
                state.synchronization_state ==
                    openisac::SynchronizationMode::reacquire &&
                !state.timing_valid && state.reset_count == resets_before + 1u,
            "continuous receiver did not enter reacquisition after outage");

    config.snr_db = 45.0f;
    config.random_seed = 0xA400u;
    const auto recovered = openisac::simulate_dynamic_tdl_frame(
        {2u, Modulation::qam64}, 12u, codec, config,
        &state, &workspace, &decoder);
    require(recovered.timing_ok && recovered.header_ok && recovered.crc_ok &&
                recovered.synchronization_mode_used ==
                    openisac::SynchronizationMode::reacquire &&
                state.synchronization_state ==
                    openisac::SynchronizationMode::track &&
                state.timing_valid &&
                state.synchronization_lock_age_frames == 1u,
            "continuous receiver failed to reacquire after outage");
}

void test_dynamic_workspace_and_timing() {
    const std::string matrix_root = OPENISAC_MATRIX_DIR;
    const openisac::Ldpc5041008 codec(
        openisac::join_path(matrix_root, "LDPC_504_1008.alist"),
        openisac::join_path(matrix_root, "LDPC_504_1008G.alist"));
    openisac::DynamicLinkSimulationConfig config;
    config.snr_db = 45.0f;
    config.random_seed = 0x8300u;
    openisac::DynamicLinkWorkspace workspace;
    openisac::DynamicLinkWorkspace parallel_workspace;
    openisac::LdpcFrameDecoder frame_decoder(codec, 4u);
    const auto warm = openisac::simulate_dynamic_tdl_frame(
        {2u, Modulation::qam256}, 0u, codec, config,
        nullptr, &workspace);
    require(warm.crc_ok && warm.workspace_growths_this_frame > 0u,
            "workspace warm-up did not allocate and decode correctly");
    require(warm.timing.receiver_total_us > 0.0 &&
                warm.timing.simulation_total_us >= warm.timing.receiver_total_us &&
                warm.timing.synchronization_us > 0.0 &&
                warm.timing.fft_csi_us > 0.0 &&
                warm.timing.detection_adaptation_us > 0.0 &&
                warm.timing.control_header_us > 0.0 &&
                warm.timing.soft_demapping_us > 0.0 &&
                warm.timing.ldpc_crc_us > 0.0 &&
                warm.timing.control_fec_us > 0.0,
            "dynamic-link stage timing was not populated");
    require(std::abs(
                warm.timing.control_fec_us - warm.timing.control_header_us -
                warm.timing.soft_demapping_us - warm.timing.ldpc_crc_us) < 1.0,
            "control/FEC substage timing does not match its total");
    const auto parallel_warm = openisac::simulate_dynamic_tdl_frame(
        {2u, Modulation::qam256}, 0u, codec, config,
        nullptr, &parallel_workspace, &frame_decoder);
    require(parallel_warm.crc_ok && parallel_warm.ldpc_worker_threads == 4u &&
                parallel_warm.ldpc_capacity_growths_this_frame > 0u,
            "parallel dynamic-link warm-up failed");

    std::size_t sequence = 1u;
    for (const auto mode : {
             LinkMode{1u, Modulation::qpsk},
             LinkMode{1u, Modulation::qam256},
             LinkMode{2u, Modulation::qam64},
             LinkMode{2u, Modulation::qam256}}) {
        config.random_seed = static_cast<unsigned>(0x8300u + sequence * 23u);
        const auto baseline = openisac::simulate_dynamic_tdl_frame(
            mode, static_cast<std::uint16_t>(sequence), codec, config,
            nullptr, nullptr);
        const auto reused = openisac::simulate_dynamic_tdl_frame(
            mode, static_cast<std::uint16_t>(sequence), codec, config,
            nullptr, &workspace);
        const auto parallel = openisac::simulate_dynamic_tdl_frame(
            mode, static_cast<std::uint16_t>(sequence), codec, config,
            nullptr, &parallel_workspace, &frame_decoder);
        require(baseline.timing_ok == reused.timing_ok &&
                    baseline.header_ok == reused.header_ok &&
                    baseline.crc_ok == reused.crc_ok &&
                    baseline.decoded_mode == reused.decoded_mode &&
                    std::abs(baseline.channel_nmse_db - reused.channel_nmse_db) < 1.0e-5f &&
                    std::abs(baseline.evm_percent - reused.evm_percent) < 1.0e-5f,
                "reused workspace changed PHY results");
        if (!(parallel.timing_ok == reused.timing_ok &&
              parallel.header_ok == reused.header_ok &&
              parallel.crc_ok == reused.crc_ok &&
              parallel.decoded_mode == reused.decoded_mode &&
              parallel.syndrome_failures == reused.syndrome_failures &&
              parallel.ldpc_worker_threads == 4u &&
              parallel.ldpc_capacity_growths_this_frame == 0u &&
              parallel.workspace_growths_this_frame == 0u)) {
            throw std::runtime_error(
                "parallel dynamic-link mismatch: rank=" +
                std::to_string(mode.rank) + " qm=" +
                std::to_string(openisac::modulation_bits(mode.modulation)) +
                " timing=" + std::to_string(parallel.timing_ok) + "/" +
                std::to_string(reused.timing_ok) + " header=" +
                std::to_string(parallel.header_ok) + "/" +
                std::to_string(reused.header_ok) + " crc=" +
                std::to_string(parallel.crc_ok) + "/" +
                std::to_string(reused.crc_ok) + " syndrome=" +
                std::to_string(parallel.syndrome_failures) + "/" +
                std::to_string(reused.syndrome_failures) + " workers=" +
                std::to_string(parallel.ldpc_worker_threads) + " ldpc_growth=" +
                std::to_string(parallel.ldpc_capacity_growths_this_frame) +
                " workspace_growth=" +
                std::to_string(parallel.workspace_growths_this_frame));
        }
        if (reused.workspace_growths_this_frame != 0u) {
            throw std::runtime_error(
                "warmed workspace grew " +
                std::to_string(reused.workspace_growths_this_frame) +
                " capacities for rank " + std::to_string(mode.rank) +
                " modulation bits " +
                std::to_string(openisac::modulation_bits(mode.modulation)));
        }
        ++sequence;
    }
    require(workspace.frames_processed == sequence,
            "workspace frame counter is incorrect");
    workspace.release();
    require(workspace.frames_processed == 0u &&
                workspace.capacity_growths == 0u &&
                workspace.rx_grid.empty() && workspace.rx_stream.empty(),
            "workspace release did not return retained buffers");
}

void test_dynamic_frame_double_buffer_pipeline() {
    const std::string matrix_root = OPENISAC_MATRIX_DIR;
    const openisac::Ldpc5041008 codec(
        openisac::join_path(matrix_root, "LDPC_504_1008.alist"),
        openisac::join_path(matrix_root, "LDPC_504_1008G.alist"));
    const LinkMode mode{2u, Modulation::qam64};
    FormalFrameProfile profile;
    profile.transmit_rank = mode.rank;
    profile.bits_per_symbol = openisac::modulation_bits(mode.modulation);
    const auto layout = openisac::build_formal_frame_layout(profile);
    constexpr std::size_t frame_count = 12u;
    std::vector<std::vector<std::uint8_t>> expected(frame_count);
    std::vector<openisac::DynamicFramePipelineResult> results;
    results.reserve(frame_count);
    openisac::DynamicFramePipeline pipeline(codec, 4u);
    require(pipeline.slot_count() == 2u,
            "dynamic-frame pipeline did not create exactly two slots");

    for (std::size_t frame = 0u; frame < frame_count; ++frame) {
        if (frame >= pipeline.slot_count()) {
            results.push_back(pipeline.receive());
        }
        auto& payload = expected[frame];
        payload.resize(layout.user_payload_bytes);
        for (std::size_t index = 0u; index < payload.size(); ++index) {
            payload[index] = static_cast<std::uint8_t>(
                (index * 29u + frame * 17u + 3u) & 0xFFu);
        }
        const auto encoded = openisac::encode_dynamic_frame(
            payload, mode, static_cast<std::uint16_t>(frame), codec,
            static_cast<std::uint32_t>(0xA100u + frame));
        std::vector<float> control_llrs;
        control_llrs.reserve(encoded.control_labels.size() * 2u);
        for (const auto label : encoded.control_labels) {
            control_llrs.push_back((label & 0x02u) == 0u ? 12.0f : -12.0f);
            control_llrs.push_back((label & 0x01u) == 0u ? 12.0f : -12.0f);
        }
        const std::vector<float> variances(
            encoded.payload_symbols.size(), 0.01f);
        pipeline.submit(
            frame, control_llrs, encoded.payload_symbols, variances);
    }
    while (results.size() < frame_count) {
        results.push_back(pipeline.receive());
    }
    for (std::size_t frame = 0u; frame < frame_count; ++frame) {
        const auto& output = results[frame];
        require(output.frame_id == frame && output.decoded.crc_ok &&
                    output.decoded.mode == mode &&
                    output.decoded.header.sequence == frame &&
                    output.decoded.user_payload == expected[frame],
                "double-buffer pipeline changed frame order or decoded content");
        require(output.timing.producer_us > 0.0 &&
                    output.timing.queue_wait_us >= 0.0 &&
                    output.timing.fec_us > 0.0 &&
                    output.timing.latency_us >= output.timing.producer_us &&
                    output.timing.buffer_slot < 2u,
                "double-buffer pipeline timing was not populated");
        if (frame >= 2u) {
            require(output.timing.capacity_growths_this_frame == 0u,
                    "warmed pipeline slot grew its prepared-frame buffers");
        }
    }

}

void test_dynamic_link_double_buffer_pipeline() {
    const std::string matrix_root = OPENISAC_MATRIX_DIR;
    const openisac::Ldpc5041008 codec(
        openisac::join_path(matrix_root, "LDPC_504_1008.alist"),
        openisac::join_path(matrix_root, "LDPC_504_1008G.alist"));
    constexpr std::size_t frame_count = 12u;
    const LinkMode mode{2u, Modulation::qam64};
    openisac::DynamicLinkSimulationConfig config;
    config.snr_db = 45.0f;
    config.csi_smoothing_alpha = 0.35f;
    config.tracking_half_window_samples = 2u;

    std::vector<openisac::DynamicLinkSimulationResult> baseline;
    baseline.reserve(frame_count);
    openisac::DynamicLinkReceiverState baseline_state;
    openisac::DynamicLinkWorkspace baseline_workspace;
    openisac::LdpcFrameDecoder baseline_decoder(codec, 4u);
    for (std::size_t frame = 0u; frame < frame_count; ++frame) {
        config.timing_offset_samples = 20u;
        config.random_seed = static_cast<unsigned>(0xB100u + frame * 37u);
        baseline.push_back(openisac::simulate_dynamic_tdl_frame(
            mode, static_cast<std::uint16_t>(frame), codec, config,
            &baseline_state, &baseline_workspace, &baseline_decoder));
    }

    openisac::DynamicLinkPipeline pipeline(codec, 4u);
    require(pipeline.slot_count() == 2u,
            "dynamic-link pipeline did not create exactly two slots");
    std::vector<openisac::DynamicLinkPipelineResult> results;
    results.reserve(frame_count);
    for (std::size_t frame = 0u; frame < frame_count; ++frame) {
        if (frame >= pipeline.slot_count()) {
            results.push_back(pipeline.receive());
        }
        config.timing_offset_samples = 20u;
        config.random_seed = static_cast<unsigned>(0xB100u + frame * 37u);
        pipeline.submit(
            frame, mode, static_cast<std::uint16_t>(frame), config);
    }
    while (results.size() < frame_count) {
        results.push_back(pipeline.receive());
    }

    for (std::size_t frame = 0u; frame < frame_count; ++frame) {
        const auto& expected = baseline[frame];
        const auto& output = results[frame];
        const auto& actual = output.link;
        require(output.frame_id == frame && actual.timing_ok == expected.timing_ok &&
                    actual.header_ok == expected.header_ok &&
                    actual.crc_ok == expected.crc_ok &&
                    actual.decoded_mode == expected.decoded_mode &&
                    actual.syndrome_failures == expected.syndrome_failures &&
                    actual.synchronization_mode_used ==
                        expected.synchronization_mode_used &&
                    actual.timing_candidates_evaluated ==
                        expected.timing_candidates_evaluated &&
                    actual.synchronization_lock_age_frames ==
                        expected.synchronization_lock_age_frames,
                "complete dynamic-link pipeline changed ordered PHY results");
        require(output.timing.producer_wall_us > 0.0 &&
                    output.timing.receiver_front_us > 0.0 &&
                    output.timing.backpressure_wait_us >= 0.0 &&
                    output.timing.submit_call_us >=
                        output.timing.producer_wall_us &&
                    output.timing.queue_wait_us >= 0.0 &&
                    output.timing.fec_wall_us > 0.0 &&
                    output.timing.latency_us >=
                        output.timing.producer_wall_us &&
                    output.timing.buffer_slot < 2u,
                "complete dynamic-link pipeline timing was not populated");
        if (frame >= 2u) {
            if (actual.workspace_growths_this_frame != 0u ||
                actual.ldpc_capacity_growths_this_frame != 0u) {
                throw std::runtime_error(
                    "warmed complete-link pipeline grew buffers: frame=" +
                    std::to_string(frame) + " slot=" +
                    std::to_string(output.timing.buffer_slot) +
                    " workspace=" +
                    std::to_string(actual.workspace_growths_this_frame) +
                    " ldpc=" +
                    std::to_string(actual.ldpc_capacity_growths_this_frame));
            }
        }
    }
    require(
        pipeline.receiver_state().synchronization_lock_age_frames ==
                baseline_state.synchronization_lock_age_frames &&
            pipeline.receiver_state().full_search_count ==
                baseline_state.full_search_count &&
            pipeline.receiver_state().tracking_search_count ==
                baseline_state.tracking_search_count &&
            pipeline.receiver_state().csi_age_frames ==
                baseline_state.csi_age_frames,
        "complete-link pipeline changed continuous receiver state");
}

void test_dynamic_iq_receiver_pipeline() {
    const std::string matrix_root = OPENISAC_MATRIX_DIR;
    const openisac::Ldpc5041008 codec(
        openisac::join_path(matrix_root, "LDPC_504_1008.alist"),
        openisac::join_path(matrix_root, "LDPC_504_1008G.alist"));
    constexpr std::size_t frame_count = 12u;
    const LinkMode mode{2u, Modulation::qam64};
    openisac::DynamicLinkSimulationConfig config;
    config.snr_db = 45.0f;
    config.csi_smoothing_alpha = 0.35f;
    openisac::DynamicLinkWorkspace generation_workspace;
    std::vector<openisac::DynamicLinkIqFrame> iq_frames(frame_count);
    for (std::size_t frame = 0u; frame < frame_count; ++frame) {
        config.random_seed = static_cast<unsigned>(0xD100u + frame * 41u);
        openisac::generate_dynamic_tdl_iq_frame(
            mode, static_cast<std::uint16_t>(frame), codec, config,
            iq_frames[frame], generation_workspace);
        require(iq_frames[frame].samples.size() == 2u &&
                    !iq_frames[frame].samples[0].empty() &&
                    iq_frames[frame].samples[0].size() ==
                        iq_frames[frame].samples[1].size(),
                "generated dynamic IQ frame has invalid branch storage");
        // The receiver must derive Rank/MCS from the robust control region,
        // not from simulator truth metadata carried beside captured IQ.
        iq_frames[frame].transmitted_mode = {1u, Modulation::qpsk};
    }

    std::vector<openisac::DynamicLinkSimulationResult> baseline;
    baseline.reserve(frame_count);
    openisac::DynamicLinkReceiverState baseline_state;
    openisac::DynamicLinkWorkspace baseline_workspace;
    openisac::LdpcFrameDecoder baseline_decoder(codec, 4u);
    for (const auto& iq_frame : iq_frames) {
        openisac::PreparedDynamicLinkFrame prepared;
        openisac::prepare_dynamic_iq_frame(
            iq_frame, prepared, &baseline_state, baseline_workspace);
        baseline.push_back(openisac::finish_dynamic_tdl_frame(
            prepared, codec, baseline_workspace, &baseline_decoder));
    }

    openisac::DynamicLinkPipeline pipeline(codec, 4u);
    std::vector<openisac::DynamicLinkPipelineResult> results;
    results.reserve(frame_count);
    for (std::size_t frame = 0u; frame < frame_count; ++frame) {
        if (frame >= pipeline.slot_count()) {
            results.push_back(pipeline.receive());
        }
        pipeline.submit_capture(frame, iq_frames[frame], config);
    }
    while (results.size() < frame_count) {
        results.push_back(pipeline.receive());
    }
    for (std::size_t frame = 0u; frame < frame_count; ++frame) {
        const auto& expected = baseline[frame];
        const auto& item = results[frame];
        require(item.frame_id == frame && item.link.timing_ok &&
                    item.link.header_ok && item.link.crc_ok &&
                    item.link.decoded_mode == mode &&
                    item.link.decoded_mode == expected.decoded_mode &&
                    item.link.syndrome_failures == expected.syndrome_failures &&
                    item.link.synchronization_mode_used ==
                        expected.synchronization_mode_used &&
                    item.link.timing_candidates_evaluated ==
                        expected.timing_candidates_evaluated &&
                    item.link.synchronization_lock_age_frames ==
                        expected.synchronization_lock_age_frames,
                "receiver-only IQ pipeline changed ordered PHY results");
        if (frame >= 2u) {
            require(item.link.workspace_growths_this_frame == 0u &&
                        item.link.ldpc_capacity_growths_this_frame == 0u,
                    "warmed receiver-only IQ pipeline grew its buffers");
        }
    }

    auto automatic_receiver = openisac::make_dynamic_link_receiver_config(
        config, openisac::NoiseVarianceMode::pilot_residual);
    require(automatic_receiver.maximum_timing_offset_samples == 52u &&
                automatic_receiver.maximum_channel_delay_samples == 9u,
            "simulation-to-receiver configuration lost timing/delay bounds");
    openisac::DynamicLinkReceiverState automatic_state;
    openisac::DynamicLinkWorkspace automatic_workspace;
    openisac::LdpcFrameDecoder automatic_decoder(codec, 4u);
    double estimated_noise_sum = 0.0;
    for (std::size_t frame = 0u; frame < frame_count; ++frame) {
        openisac::PreparedDynamicLinkFrame prepared;
        openisac::prepare_captured_iq_frame(
            iq_frames[frame], automatic_receiver, prepared,
            &automatic_state, automatic_workspace);
        const auto result = openisac::finish_dynamic_tdl_frame(
            prepared, codec, automatic_workspace, &automatic_decoder);
        require(result.timing_ok && result.header_ok && result.crc_ok &&
                    result.noise_variance_estimated &&
                    result.raw_noise_variance > 0.0f &&
                    result.noise_variance_used > 0.0f &&
                    result.noise_variance_age_frames == frame + 1u,
                "pilot-residual receiver configuration failed");
        estimated_noise_sum += result.raw_noise_variance;
    }
    const double true_noise_variance = std::pow(10.0, -config.snr_db / 10.0);
    const double mean_estimated_noise = estimated_noise_sum / frame_count;
    require(mean_estimated_noise > 0.2 * true_noise_variance &&
                mean_estimated_noise < 5.0 * true_noise_variance,
            "pilot-residual noise estimate is outside engineering tolerance");

    auto bounded_receiver = automatic_receiver;
    bounded_receiver.maximum_timing_offset_samples = 10u;
    openisac::PreparedDynamicLinkFrame bounded_prepared;
    openisac::DynamicLinkWorkspace bounded_workspace;
    openisac::prepare_captured_iq_frame(
        iq_frames.front(), bounded_receiver, bounded_prepared,
        nullptr, bounded_workspace);
    require(bounded_prepared.result.timing_candidates_evaluated == 11u,
            "receiver timing-search boundary was not enforced");

    bool rejected_delay = false;
    try {
        auto invalid_receiver = automatic_receiver;
        invalid_receiver.maximum_channel_delay_samples = 128u;
        openisac::PreparedDynamicLinkFrame invalid_prepared;
        openisac::prepare_captured_iq_frame(
            iq_frames.front(), invalid_receiver, invalid_prepared,
            nullptr, bounded_workspace);
    } catch (const std::invalid_argument&) {
        rejected_delay = true;
    }
    require(rejected_delay,
            "receiver accepted a maximum channel delay outside the CP");
}

void test_hardware_tx_waveform_receiver_closure() {
    using openisac::LinkMode;
    using openisac::Modulation;
    using openisac::PilotMode;
    using openisac::TransmissionScheme;

    const std::string matrix_root = OPENISAC_MATRIX_DIR;
    const openisac::Ldpc5041008 codec(
        openisac::join_path(matrix_root, "LDPC_504_1008.alist"),
        openisac::join_path(matrix_root, "LDPC_504_1008G.alist"));
    const std::array<LinkMode, 3> modes{{
        {1u, Modulation::qam64, TransmissionScheme::spatial_multiplexing, 1u},
        {2u, Modulation::qam64, TransmissionScheme::spatial_multiplexing, 2u},
        {1u, Modulation::qam64, TransmissionScheme::alamouti_stbc, 2u},
    }};
    const std::array<PilotMode, 2> pilots{{PilotMode::fdm, PilotMode::nr_dmrs}};

    for (const auto mode : modes) {
        const std::size_t physical_ports =
            openisac::physical_transmit_ports(mode);
        openisac::FormalFrameProfile profile;
        profile.transmit_rank = mode.rank;
        profile.bits_per_symbol = openisac::modulation_bits(mode.modulation);
        profile.scheme = mode.scheme;
        const auto layout = openisac::build_formal_frame_layout(profile);
        const std::size_t payload_bytes =
            std::min<std::size_t>(257u, layout.user_payload_bytes);
        std::vector<std::uint8_t> payload(payload_bytes);
        for (std::size_t index = 0u; index < payload.size(); ++index) {
            payload[index] = static_cast<std::uint8_t>(
                (index * 53u + mode.rank * 17u + physical_ports) & 0xFFu);
        }

        for (const auto pilot : pilots) {
            openisac::DynamicLinkWorkspace generation_workspace;
            openisac::DynamicLinkTransmitFrame tx;
            openisac::generate_dynamic_tx_iq_frame(
                payload, mode, 73u, codec, pilot, 0xC057u, tx,
                generation_workspace);
            require(
                tx.samples.size() == physical_ports &&
                    !tx.samples.empty() && !tx.samples[0].empty(),
                "hardware TX waveform has the wrong port or sample count");

            constexpr std::size_t timing_offset = 20u;
            constexpr std::size_t tail = 32u;
            openisac::DynamicLinkCaptureFrame capture;
            capture.capture_sequence = 73u;
            capture.timestamp = 5000u;
            capture.pilot_seed = tx.pilot_seed;
            capture.samples.resize(physical_ports);
            for (std::size_t port = 0u; port < physical_ports; ++port) {
                capture.samples[port].assign(
                    timing_offset + tx.samples[port].size() + tail,
                    std::complex<float>{});
                std::copy(
                    tx.samples[port].begin(), tx.samples[port].end(),
                    capture.samples[port].begin() +
                        static_cast<std::ptrdiff_t>(timing_offset));
            }

            openisac::DynamicLinkReceiverConfig receiver_config;
            receiver_config.pilot_mode = pilot;
            receiver_config.noise_variance_mode =
                openisac::NoiseVarianceMode::fixed;
            receiver_config.fixed_noise_variance = 1.0e-6f;
            receiver_config.maximum_timing_offset_samples = 64u;
            receiver_config.maximum_channel_delay_samples = 16u;
            openisac::DynamicLinkReceiverState receiver_state;
            openisac::DynamicLinkWorkspace receiver_workspace;
            openisac::PreparedDynamicLinkFrame prepared;
            openisac::prepare_captured_iq_frame(
                capture, receiver_config, prepared, &receiver_state,
                receiver_workspace);
            const auto result = openisac::finish_dynamic_tdl_frame(
                prepared, codec, receiver_workspace);
            require(
                result.timing_ok && result.header_ok && result.crc_ok &&
                    result.user_payload == payload,
                "hardware TX waveform did not close through receiver-only PHY");
        }
    }
}

void test_dynamic_sensing_current_phy() {
    const std::string matrix_root = OPENISAC_MATRIX_DIR;
    const openisac::Ldpc5041008 codec(
        openisac::join_path(matrix_root, "LDPC_504_1008.alist"),
        openisac::join_path(matrix_root, "LDPC_504_1008G.alist"));
    openisac::DynamicSensingConfig sensing_config;
    sensing_config.regularization = 0.0f;
    constexpr std::size_t target_range_bin = 5u;
    constexpr int target_doppler_bin = 1;
    constexpr float two_pi = 6.28318530717958647692f;

    auto make_payload = [](const LinkMode mode, std::size_t frame) {
        FormalFrameProfile profile;
        profile.transmit_rank = mode.rank;
        profile.bits_per_symbol = openisac::modulation_bits(mode.modulation);
        const auto layout = openisac::build_formal_frame_layout(profile);
        std::vector<std::uint8_t> payload(layout.user_payload_bytes);
        for (std::size_t index = 0u; index < payload.size(); ++index) {
            payload[index] = static_cast<std::uint8_t>(
                (index * 43u + frame * 29u + 17u) & 0xFFu);
        }
        return payload;
    };

    auto synthesize_receive_grid = [&](const openisac::EncodedDynamicFrame& encoded,
                                       std::size_t frame,
                                       float noise_sigma,
                                       std::mt19937& random) {
        std::vector<std::complex<float>> receive_grid(
            sensing_config.data_symbols * sensing_config.fft_size *
                sensing_config.receive_ports,
            std::complex<float>{});
        std::normal_distribution<float> normal(0.0f, noise_sigma);
        const float doppler_phase = two_pi *
            static_cast<float>(target_doppler_bin) *
            static_cast<float>(frame) /
            static_cast<float>(sensing_config.doppler_fft_size);
        for (std::size_t fft = 0u; fft < sensing_config.fft_size; ++fft) {
            const int centered = fft < sensing_config.fft_size / 2u
                ? static_cast<int>(fft)
                : static_cast<int>(fft) -
                    static_cast<int>(sensing_config.fft_size);
            const float delay_phase = -two_pi *
                static_cast<float>(centered) *
                static_cast<float>(target_range_bin) /
                static_cast<float>(sensing_config.range_fft_size);
            const auto h0 = std::polar(1.0f, delay_phase + doppler_phase);
            const auto h1 = std::polar(
                0.45f, delay_phase + doppler_phase + 0.37f);
            for (std::size_t symbol = 0u;
                 symbol < sensing_config.data_symbols; ++symbol) {
                const std::size_t tx_base =
                    (symbol * sensing_config.fft_size + fft) * 2u;
                const std::size_t rx_base =
                    (symbol * sensing_config.fft_size + fft) * 2u;
                const auto echo =
                    encoded.tx_grid[tx_base] * h0 +
                    encoded.tx_grid[tx_base + 1u] * h1;
                receive_grid[rx_base] = echo + std::complex<float>{
                    normal(random), normal(random)};
                receive_grid[rx_base + 1u] = 0.7f * echo;
            }
        }
        return receive_grid;
    };

    // First verify that the known-waveform LS estimator works with the latest
    // Rank-2/256-QAM grid, including non-unit data symbols and FDM pilot holes.
    const LinkMode qam256_mode{2u, Modulation::qam256};
    const auto qam256_payload = make_payload(qam256_mode, 0u);
    const auto qam256_frame = openisac::encode_dynamic_frame(
        qam256_payload, qam256_mode, 300u, codec, 0xB500u);
    std::mt19937 noiseless_random(0x5500u);
    const auto qam256_receive = synthesize_receive_grid(
        qam256_frame, 0u, 0.0f, noiseless_random);
    openisac::DynamicSensingChannelEstimate channel_estimate;
    openisac::estimate_dynamic_sensing_channel_2x2(
        qam256_frame.tx_grid, qam256_receive,
        sensing_config, channel_estimate);
    double error_power = 0.0;
    double reference_power = 0.0;
    for (std::size_t fft = 0u; fft < sensing_config.fft_size; ++fft) {
        if (channel_estimate.direct_estimate_mask[fft] == 0u) {
            continue;
        }
        const int centered = fft < sensing_config.fft_size / 2u
            ? static_cast<int>(fft)
            : static_cast<int>(fft) -
                static_cast<int>(sensing_config.fft_size);
        const auto expected = std::polar(
            1.0f,
            -two_pi * static_cast<float>(centered) *
                static_cast<float>(target_range_bin) /
                static_cast<float>(sensing_config.range_fft_size));
        error_power += std::norm(
            channel_estimate.frequency_response[fft] - expected);
        reference_power += std::norm(expected);
    }
    require(channel_estimate.active_subcarriers > 800u &&
                channel_estimate.directly_estimated_subcarriers > 500u &&
                channel_estimate.interpolated_subcarriers > 0u &&
                error_power / reference_power < 1.0e-7,
            "Rank-2/256-QAM known-waveform sensing channel estimate failed");

    const LinkMode qam64_mode{2u, Modulation::qam64};
    openisac::DynamicSensingProcessor sensing(sensing_config);
    std::mt19937 noisy_random(0x6600u);
    std::vector<std::complex<float>> final_transmit_grid;
    std::vector<std::complex<float>> final_receive_grid;
    for (std::size_t frame = 0u;
         frame < sensing_config.coherent_frames; ++frame) {
        const auto payload = make_payload(qam64_mode, frame);
        const auto encoded = openisac::encode_dynamic_frame(
            payload, qam64_mode, static_cast<std::uint16_t>(frame), codec,
            static_cast<std::uint32_t>(0xC600u + frame));
        auto receive_grid = synthesize_receive_grid(
            encoded, frame, 1.0e-3f, noisy_random);
        const bool ready = sensing.push_frame(
            1000u + frame, 10'000'000u + 225'000u * frame,
            encoded.tx_grid, receive_grid);
        require(ready == (frame + 1u == sensing_config.coherent_frames),
                "dynamic sensing coherent batch completed at the wrong frame");
        if (frame + 1u == sensing_config.coherent_frames) {
            final_transmit_grid = encoded.tx_grid;
            final_receive_grid = std::move(receive_grid);
        }
    }
    const auto& result = sensing.last_result();
    const auto range_error = result.strongest_peak.range_bin > target_range_bin
        ? result.strongest_peak.range_bin - target_range_bin
        : target_range_bin - result.strongest_peak.range_bin;
    const std::size_t expected_doppler =
        sensing_config.doppler_fft_size / 2u + target_doppler_bin;
    const auto doppler_error = result.strongest_peak.doppler_bin > expected_doppler
        ? result.strongest_peak.doppler_bin - expected_doppler
        : expected_doppler - result.strongest_peak.doppler_bin;
    require(result.ready && result.coherent_frames == 64u &&
                result.first_capture_sequence == 1000u &&
                result.last_capture_sequence == 1063u &&
                result.range_doppler_map.size() == 64u * 1024u &&
                range_error <= 1u && doppler_error <= 1u &&
                std::abs(result.strongest_peak.range_m -
                         target_range_bin * result.range_bin_spacing_m) <=
                    result.range_bin_spacing_m &&
                std::abs(result.strongest_peak.velocity_mps -
                         target_doppler_bin *
                             result.velocity_bin_spacing_mps) <=
                    result.velocity_bin_spacing_mps,
            "current-PHY range-Doppler target estimate failed");
    const auto has_detection = [](
        const openisac::DynamicSensingResult& sensing_result,
        std::size_t expected_range,
        std::size_t expected_doppler,
        std::size_t tolerance) {
        return std::any_of(
            sensing_result.detections.begin(),
            sensing_result.detections.end(),
            [&](const openisac::DynamicSensingDetection& detection) {
                const auto range_delta = detection.peak.range_bin > expected_range
                    ? detection.peak.range_bin - expected_range
                    : expected_range - detection.peak.range_bin;
                const auto doppler_delta =
                    detection.peak.doppler_bin > expected_doppler
                    ? detection.peak.doppler_bin - expected_doppler
                    : expected_doppler - detection.peak.doppler_bin;
                return range_delta <= tolerance &&
                    doppler_delta <= tolerance;
            });
    };
    require(result.cfar_cells_tested > 20'000u &&
                has_detection(result, target_range_bin, expected_doppler, 1u),
            "CA-CFAR did not detect the current-PHY target");

    // Verify two moving targets in the presence of a much stronger exact-DC
    // reflector. Slow-time mean removal must reject the static component while
    // preserving the +1-bin low-speed target and a separated -3-bin target.
    struct TargetSpec {
        std::size_t range_bin;
        int doppler_bin;
        float amplitude;
        float phase;
    };
    const std::array<TargetSpec, 3u> targets{{
        {10u, 0, 4.0f, 0.11f},
        {5u, 1, 1.0f, 0.27f},
        {18u, -3, 0.65f, -0.41f},
    }};
    auto multi_config = sensing_config;
    multi_config.enable_static_clutter_suppression = true;
    multi_config.cfar_false_alarm_probability = 1.0e-5f;
    openisac::DynamicSensingProcessor clutter_suppressed(multi_config);
    auto static_config = multi_config;
    static_config.enable_static_clutter_suppression = false;
    openisac::DynamicSensingProcessor static_preserved(static_config);
    std::mt19937 multi_random(0x7700u);
    for (std::size_t frame = 0u;
         frame < multi_config.coherent_frames; ++frame) {
        const auto payload = make_payload(qam64_mode, 100u + frame);
        const auto encoded = openisac::encode_dynamic_frame(
            payload, qam64_mode, static_cast<std::uint16_t>(500u + frame),
            codec, static_cast<std::uint32_t>(0xD700u + frame));
        std::vector<std::complex<float>> receive_grid(
            multi_config.data_symbols * multi_config.fft_size *
                multi_config.receive_ports,
            std::complex<float>{});
        std::normal_distribution<float> noise(0.0f, 1.0e-3f);
        for (std::size_t fft = 0u; fft < multi_config.fft_size; ++fft) {
            const int centered = fft < multi_config.fft_size / 2u
                ? static_cast<int>(fft)
                : static_cast<int>(fft) -
                    static_cast<int>(multi_config.fft_size);
            std::complex<float> h0{};
            std::complex<float> h1{};
            for (const auto& target : targets) {
                const float phase = -two_pi *
                    static_cast<float>(centered) *
                    static_cast<float>(target.range_bin) /
                    static_cast<float>(multi_config.range_fft_size) +
                    two_pi * static_cast<float>(target.doppler_bin) *
                    static_cast<float>(frame) /
                    static_cast<float>(multi_config.doppler_fft_size) +
                    target.phase;
                h0 += std::polar(target.amplitude, phase);
                h1 += std::polar(0.35f * target.amplitude, phase + 0.31f);
            }
            for (std::size_t symbol = 0u;
                 symbol < multi_config.data_symbols; ++symbol) {
                const std::size_t base =
                    (symbol * multi_config.fft_size + fft) * 2u;
                const auto echo = encoded.tx_grid[base] * h0 +
                    encoded.tx_grid[base + 1u] * h1;
                receive_grid[base] = echo + std::complex<float>{
                    noise(multi_random), noise(multi_random)};
                receive_grid[base + 1u] = 0.7f * echo;
            }
        }
        const auto sequence = static_cast<std::uint64_t>(2000u + frame);
        const bool suppressed_ready = clutter_suppressed.push_frame(
            sequence, sequence * 225'000u,
            encoded.tx_grid, receive_grid);
        const bool static_ready = static_preserved.push_frame(
            sequence, sequence * 225'000u,
            encoded.tx_grid, receive_grid);
        require(suppressed_ready ==
                    (frame + 1u == multi_config.coherent_frames) &&
                    static_ready ==
                    (frame + 1u == multi_config.coherent_frames),
                "multi-target sensing batch completion failed");
    }
    const auto& suppressed_result = clutter_suppressed.last_result();
    const auto& static_result = static_preserved.last_result();
    const std::size_t dc_bin = multi_config.doppler_fft_size / 2u;
    require(static_result.strongest_peak.range_bin == 10u &&
                static_result.strongest_peak.doppler_bin == dc_bin &&
                has_detection(static_result, 10u, dc_bin, 1u),
            "static target was not preserved when clutter suppression was off");
    require(suppressed_result.static_clutter_suppression_applied &&
                has_detection(suppressed_result, 5u, dc_bin + 1u, 1u) &&
                has_detection(suppressed_result, 18u, dc_bin - 3u, 1u) &&
                !has_detection(suppressed_result, 10u, dc_bin, 1u),
            "static clutter suppression or two-target CA-CFAR failed");

    auto gap_config = sensing_config;
    gap_config.coherent_frames = 4u;
    gap_config.doppler_fft_size = 4u;
    openisac::DynamicSensingProcessor gap_processor(gap_config);
    require(!gap_processor.push_frame(
                0u, 0u, final_transmit_grid, final_receive_grid) &&
                !gap_processor.push_frame(
                    2u, 450'000u,
                    final_transmit_grid, final_receive_grid) &&
                gap_processor.frames_accumulated() == 1u,
            "sensing sequence gap did not reset the coherent batch");
    require(!gap_processor.push_frame(
                3u, 675'000u,
                final_transmit_grid, final_receive_grid) &&
                !gap_processor.push_frame(
                    4u, 900'000u,
                    final_transmit_grid, final_receive_grid) &&
                gap_processor.push_frame(
                    5u, 1'125'000u,
                    final_transmit_grid, final_receive_grid),
            "sensing gap recovery did not complete a new batch");
    require(gap_processor.last_result().sequence_gap_resets == 1u &&
                gap_processor.last_result().first_capture_sequence == 2u &&
                gap_processor.last_result().last_capture_sequence == 5u,
            "sensing gap recovery metadata failed");
}

void test_dynamic_sensing_time_domain_frontend() {
    const std::string matrix_root = OPENISAC_MATRIX_DIR;
    const openisac::Ldpc5041008 codec(
        openisac::join_path(matrix_root, "LDPC_504_1008.alist"),
        openisac::join_path(matrix_root, "LDPC_504_1008G.alist"));
    const LinkMode mode{2u, Modulation::qam64};
    openisac::DynamicLinkSimulationConfig link_config;
    link_config.snr_db = 48.0f;
    link_config.timing_offset_samples = 20u;
    link_config.cfo_hz = 300.0f;
    link_config.sfo_ppm = 20.0f;
    link_config.enable_truth_diagnostics = false;
    link_config.csi_smoothing_alpha = 1.0f;

    openisac::DynamicSensingConfig sensing_config;
    sensing_config.enable_static_clutter_suppression = true;
    openisac::DynamicSensingProcessor sensing(sensing_config);
    openisac::DynamicLinkWorkspace generation_workspace;
    openisac::DynamicLinkWorkspace receiver_workspace;
    openisac::DynamicLinkReceiverState receiver_state;
    std::size_t communication_crc_successes = 0u;
    constexpr std::size_t target_range_bin = 5u;
    constexpr int target_doppler_bin = 1;
    for (std::size_t frame = 0u;
         frame < sensing_config.coherent_frames; ++frame) {
        link_config.taps = {
            {0u, 0.0f, 0.0f},
            {target_range_bin, -3.0f,
             360.0f * static_cast<float>(target_doppler_bin) *
                 static_cast<float>(frame) /
                 static_cast<float>(sensing_config.coherent_frames)},
        };
        link_config.random_seed = static_cast<unsigned>(0xE800u + 37u * frame);
        link_config.pilot_seed = static_cast<std::uint32_t>(0xE900u + frame);
        openisac::DynamicLinkIqFrame iq_frame;
        openisac::generate_dynamic_tdl_iq_frame(
            mode, static_cast<std::uint16_t>(3000u + frame), codec,
            link_config, iq_frame, generation_workspace);
        require(iq_frame.transmit_reference_grid.size() == 2u * 1024u * 2u,
                "time-domain simulator did not retain the sensing Tx grid");

        openisac::PreparedDynamicLinkFrame prepared;
        openisac::prepare_dynamic_iq_frame(
            iq_frame, prepared, &receiver_state, receiver_workspace);
        require(prepared.ready && prepared.result.timing_ok &&
                    prepared.result.header_ok &&
                    receiver_workspace.rx_grid.size() == 2u * 1024u * 2u,
                "shared time-domain synchronization/FFT frontend failed");
        const bool ready = sensing.push_frame(
            iq_frame.capture_sequence,
            static_cast<std::uint64_t>(frame) * 225'000u,
            iq_frame.transmit_reference_grid,
            receiver_workspace.rx_grid);
        require(ready == (frame + 1u == sensing_config.coherent_frames),
                "time-domain sensing batch completed at the wrong frame");
        const auto link_result = openisac::finish_dynamic_tdl_frame(
            prepared, codec, receiver_workspace);
        if (link_result.crc_ok) {
            ++communication_crc_successes;
        }
    }

    const auto& result = sensing.last_result();
    const std::size_t expected_doppler =
        sensing_config.doppler_fft_size / 2u + target_doppler_bin;
    const auto range_error = result.strongest_peak.range_bin > target_range_bin
        ? result.strongest_peak.range_bin - target_range_bin
        : target_range_bin - result.strongest_peak.range_bin;
    const auto doppler_error =
        result.strongest_peak.doppler_bin > expected_doppler
        ? result.strongest_peak.doppler_bin - expected_doppler
        : expected_doppler - result.strongest_peak.doppler_bin;
    const bool cfar_found = std::any_of(
        result.detections.begin(), result.detections.end(),
        [&](const openisac::DynamicSensingDetection& detection) {
            const auto range_delta = detection.peak.range_bin > target_range_bin
                ? detection.peak.range_bin - target_range_bin
                : target_range_bin - detection.peak.range_bin;
            const auto doppler_delta =
                detection.peak.doppler_bin > expected_doppler
                ? detection.peak.doppler_bin - expected_doppler
                : expected_doppler - detection.peak.doppler_bin;
            return range_delta <= 1u && doppler_delta <= 1u;
        });
    require(communication_crc_successes == sensing_config.coherent_frames &&
                range_error <= 1u && doppler_error <= 1u && cfar_found,
            "shared time-domain communication/sensing frontend regression failed");
}

void test_capture_ring_and_file_iq() {
    auto make_capture = [](std::uint64_t sequence, std::uint64_t timestamp) {
        openisac::DynamicLinkCaptureFrame frame;
        frame.capture_sequence = sequence;
        frame.timestamp = timestamp;
        frame.pilot_seed = 0x12345678u;
        frame.samples.resize(2u);
        for (std::size_t antenna = 0u; antenna < 2u; ++antenna) {
            frame.samples[antenna].resize(4u);
            for (std::size_t sample = 0u; sample < 4u; ++sample) {
                frame.samples[antenna][sample] = {
                    static_cast<float>(sequence + antenna + sample),
                    -static_cast<float>(timestamp + antenna + sample)};
            }
        }
        return frame;
    };

    openisac::CaptureRingBuffer bounded_ring(2u, 2u, 4u);
    require(bounded_ring.try_push(make_capture(10u, 100u)) &&
                bounded_ring.try_push(make_capture(12u, 200u)) &&
                !bounded_ring.try_push(make_capture(13u, 150u)),
            "capture ring overflow policy failed");
    openisac::DynamicLinkCaptureFrame popped;
    require(bounded_ring.try_pop(popped) && popped.capture_sequence == 10u,
            "capture ring changed FIFO order");
    require(bounded_ring.try_push(make_capture(14u, 300u)),
            "capture ring did not recover after overflow");
    require(bounded_ring.try_pop(popped) && popped.capture_sequence == 12u &&
                bounded_ring.try_pop(popped) && popped.capture_sequence == 14u &&
                !bounded_ring.try_pop(popped),
            "capture ring pop sequence failed");
    require(bounded_ring.try_push(make_capture(14u, 300u)) &&
                bounded_ring.try_pop(popped),
            "capture ring duplicate-sequence exercise failed");
    const auto bounded_stats = bounded_ring.statistics();
    require(bounded_stats.push_attempts == 5u &&
                bounded_stats.frames_pushed == 4u &&
                bounded_stats.frames_popped == 4u &&
                bounded_stats.overflow_drops == 1u &&
                bounded_stats.source_sequence_gaps == 1u &&
                bounded_stats.consumer_sequence_gaps == 2u &&
                bounded_stats.out_of_order_frames == 1u &&
                bounded_stats.timestamp_regressions == 1u &&
                bounded_stats.high_watermark == 2u,
            "capture ring statistics are inconsistent");

    bool rejected_oversize = false;
    try {
        auto oversized = make_capture(20u, 400u);
        oversized.samples[0].push_back({});
        oversized.samples[1].push_back({});
        (void)bounded_ring.try_push(oversized);
    } catch (const std::invalid_argument&) {
        rejected_oversize = true;
    }
    require(rejected_oversize,
            "capture ring accepted a frame larger than its fixed slot");

    constexpr std::size_t concurrent_frames = 5000u;
    openisac::CaptureRingBuffer concurrent_ring(8u, 2u, 4u);
    std::atomic<bool> producer_done{false};
    std::thread producer([&] {
        for (std::size_t index = 0u; index < concurrent_frames; ++index) {
            (void)concurrent_ring.try_push(make_capture(index, 1000u + index));
            if ((index & 31u) == 0u) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });
    bool sequence_valid = false;
    std::uint64_t previous_sequence = 0u;
    std::size_t concurrent_pops = 0u;
    while (!producer_done.load(std::memory_order_acquire) ||
           concurrent_ring.size() != 0u) {
        if (!concurrent_ring.try_pop(popped)) {
            std::this_thread::yield();
            continue;
        }
        require(!sequence_valid || popped.capture_sequence > previous_sequence,
                "concurrent capture ring changed sequence order");
        require(popped.samples.size() == 2u &&
                    popped.samples[1].size() == 4u &&
                    popped.samples[1][3u].real() ==
                        static_cast<float>(popped.capture_sequence + 4u),
                "concurrent capture ring corrupted IQ samples");
        sequence_valid = true;
        previous_sequence = popped.capture_sequence;
        ++concurrent_pops;
    }
    producer.join();
    const auto concurrent_stats = concurrent_ring.statistics();
    require(concurrent_stats.push_attempts == concurrent_frames &&
                concurrent_stats.frames_pushed == concurrent_pops &&
                concurrent_stats.frames_popped == concurrent_pops &&
                concurrent_stats.frames_pushed +
                    concurrent_stats.overflow_drops == concurrent_frames &&
                concurrent_stats.source_sequence_gaps == 0u &&
                concurrent_stats.out_of_order_frames == 0u &&
                concurrent_stats.timestamp_regressions == 0u &&
                concurrent_stats.high_watermark <= concurrent_ring.capacity(),
            "concurrent capture ring counters are inconsistent");

    const std::string matrix_root = OPENISAC_MATRIX_DIR;
    const openisac::Ldpc5041008 codec(
        openisac::join_path(matrix_root, "LDPC_504_1008.alist"),
        openisac::join_path(matrix_root, "LDPC_504_1008G.alist"));
    constexpr std::size_t frame_count = 4u;
    const LinkMode mode{2u, Modulation::qam64};
    openisac::DynamicLinkSimulationConfig config;
    config.snr_db = 45.0f;
    config.enable_truth_diagnostics = false;
    openisac::DynamicLinkWorkspace generation_workspace;
    std::vector<openisac::DynamicLinkIqFrame> generated(frame_count);
    for (std::size_t index = 0u; index < frame_count; ++index) {
        config.random_seed = static_cast<unsigned>(0xE200u + 31u * index);
        openisac::generate_dynamic_tdl_iq_frame(
            mode, static_cast<std::uint16_t>(index), codec, config,
            generated[index], generation_workspace);
        generated[index].capture_sequence = 1000u + index;
        generated[index].timestamp = 5'000'000u + 225'000u * index;
    }

    const auto unique_suffix = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    const auto file_path = std::filesystem::temp_directory_path() /
        ("openisac_capture_" + std::to_string(unique_suffix) + ".oiq");
    struct FileCleanup {
        std::filesystem::path path;
        ~FileCleanup() {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    } cleanup{file_path};

    {
        openisac::IqCaptureFileWriter writer(file_path.string(), 2u);
        for (const auto& frame : generated) {
            writer.append(frame);
        }
        writer.flush();
        require(writer.frames_written() == frame_count,
                "IQ file writer lost a frame");
    }

    const auto receiver_config = openisac::make_dynamic_link_receiver_config(
        config, openisac::NoiseVarianceMode::pilot_residual);
    openisac::CaptureRingBuffer playback_ring(2u, 2u, 4096u);
    openisac::DynamicLinkPipeline pipeline(codec, 4u);
    openisac::DynamicLinkCaptureFrame capture;
    openisac::DynamicLinkCaptureFrame pipeline_scratch;
    std::vector<openisac::DynamicLinkPipelineResult> results;
    results.reserve(frame_count);
    std::size_t submitted = 0u;
    {
        openisac::IqCaptureFileReader reader(file_path.string(), 4096u);
        require(reader.antenna_count() == 2u,
                "IQ file reader changed the antenna count");
        while (reader.read_next(capture)) {
            if (submitted >= pipeline.slot_count()) {
                results.push_back(pipeline.receive());
            }
            require(playback_ring.try_push(capture),
                    "file playback ring unexpectedly overflowed");
            require(openisac::submit_next_capture(
                        playback_ring, pipeline, receiver_config,
                        pipeline_scratch),
                    "file playback did not reach the receiver pipeline");
            ++submitted;
        }
        require(reader.frames_read() == frame_count,
                "IQ file reader lost a frame");
    }
    while (results.size() < frame_count) {
        results.push_back(pipeline.receive());
    }
    for (std::size_t index = 0u; index < frame_count; ++index) {
        require(results[index].frame_id == generated[index].capture_sequence &&
                    results[index].capture_sequence ==
                        generated[index].capture_sequence &&
                    results[index].capture_timestamp ==
                        generated[index].timestamp &&
                    results[index].link.timing_ok &&
                    results[index].link.header_ok &&
                    results[index].link.crc_ok,
                "file IQ playback changed receiver output or metadata");
    }
    const auto playback_stats = playback_ring.statistics();
    require(playback_stats.frames_pushed == frame_count &&
                playback_stats.frames_popped == frame_count &&
                playback_stats.overflow_drops == 0u &&
                playback_stats.source_sequence_gaps == 0u &&
                playback_stats.consumer_sequence_gaps == 0u,
            "file IQ playback reported a false discontinuity");

    const auto complete_size = std::filesystem::file_size(file_path);
    require(complete_size > 0u, "IQ file test produced an empty file");
    std::filesystem::resize_file(file_path, complete_size - 1u);
    bool rejected_truncation = false;
    try {
        openisac::IqCaptureFileReader truncated_reader(
            file_path.string(), 4096u);
        while (truncated_reader.read_next(capture)) {
        }
    } catch (const std::runtime_error&) {
        rejected_truncation = true;
    }
    require(rejected_truncation,
            "IQ file reader accepted a truncated capture");
}

}  // namespace

int main() {
    try {
        std::cout << std::unitbuf;
        test_crc();
        std::cout << "PASS CRC\n";
        test_qam();
        std::cout << "PASS QAM\n";
        test_ofdm_round_trip();
        std::cout << "PASS FFT/IFFT and CP\n";
        test_preamble_and_tdl();
        std::cout << "PASS ZC timing and 2x2 TDL\n";
        test_fdm_pilot_channel_estimation();
        std::cout << "PASS FDM pilot LS channel estimation\n";
        test_nr_dmrs_channel_estimation();
        std::cout << "PASS 1/2/4-port NR DM-RS CDM/OCC channel estimation\n";
        test_sampling_offset();
        std::cout << "PASS cubic SFO resampler and phase-slope tracker\n";
        test_formal_golden_vectors();
        std::cout << "PASS formal frame golden vectors\n";
        test_ldpc_reusable_and_parallel_decoder();
        std::cout << "PASS reusable and 4-thread LDPC frame decoder\n";
        test_dynamic_rank_mcs_frames();
        std::cout << "PASS dynamic Rank-1/2/4 QPSK/16/64/256-QAM frames\n";
        test_mimo();
        std::cout << "PASS 2x2 ZF/MMSE\n";
        test_scalable_mimo_detector();
        std::cout << "PASS fixed-storage 4x4/8x8 ZF/MMSE\n";
        test_rank4_ofdm_algorithm_closure();
        std::cout << "PASS Rank-4 high-order OFDM/pilot/TDL/MMSE closure\n";
        test_rank4_formal_ldpc_crc_closure();
        std::cout << "PASS Rank-4 formal soft-control/LDPC/CRC closure\n";
        test_rank4_time_synchronization_closure();
        std::cout << "PASS Rank-4 ZC/CFO/SFO continuous time-domain closure\n";
        test_nr_dmrs_rank_compatibility();
        std::cout << "PASS SISO/2x2/4x4 NR DM-RS compatibility\n";
        test_rank4_time_double_buffer_pipeline();
        std::cout << "PASS two-slot Rank-4 front-end/FEC pipeline\n";
        test_rank4_time_sensing_frontend();
        std::cout << "PASS Rank-4 communication/range-Doppler/CFAR closure\n";
        test_link_adaptation();
        std::cout << "PASS Rank/MCS controller\n";
        test_cross_frame_rank_mcs_loop();
        std::cout << "PASS cross-frame Rank/MCS encode-feedback loop\n";
        test_dynamic_tdl_receive_chain();
        std::cout << "PASS dynamic Rank/MCS TDL synchronization and receive chain\n";
        test_cross_frame_csi_smoothing();
        std::cout << "PASS soft control and cross-frame CSI smoothing\n";
        test_continuous_synchronization_tracking();
        std::cout << "PASS SEARCH/TRACK/REACQUIRE continuous synchronization\n";
        test_dynamic_workspace_and_timing();
        std::cout << "PASS reusable dynamic workspace and stage timing\n";
        test_dynamic_frame_double_buffer_pipeline();
        std::cout << "PASS two-slot soft-demapping/FEC pipeline\n";
        test_dynamic_link_double_buffer_pipeline();
        std::cout << "PASS two-slot complete dynamic-link pipeline\n";
        test_dynamic_iq_receiver_pipeline();
        std::cout << "PASS pre-generated IQ receiver-only pipeline\n";
        test_hardware_tx_waveform_receiver_closure();
        std::cout << "PASS SISO/2x2/STBC hardware TX/RX waveform closure\n";
        test_dynamic_sensing_current_phy();
        std::cout << "PASS current-PHY high-order MIMO range-Doppler sensing\n";
        test_dynamic_sensing_time_domain_frontend();
        std::cout << "PASS shared time-domain communication/sensing frontend\n";
        test_capture_ring_and_file_iq();
        std::cout << "PASS capture ring, metadata and file IQ playback\n";
        std::cout << "All Windows C++ PHY tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
