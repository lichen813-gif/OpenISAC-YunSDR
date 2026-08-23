#include "openisac/binary_io.hpp"
#include "openisac/channel_estimation.hpp"
#include "openisac/crc16.hpp"
#include "openisac/frame.hpp"
#include "openisac/link_adaptation.hpp"
#include "openisac/ldpc.hpp"
#include "openisac/ldpc_framing.hpp"
#include "openisac/mimo2x2.hpp"
#include "openisac/ofdm.hpp"
#include "openisac/preamble_sync.hpp"
#include "openisac/qam.hpp"
#include "openisac/sampling_offset.hpp"
#include "openisac/tdl_channel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

unsigned bit_errors(unsigned left, unsigned right, unsigned bits) {
    unsigned difference = (left ^ right) & ((1u << bits) - 1u);
    unsigned count = 0u;
    while (difference != 0u) {
        count += difference & 1u;
        difference >>= 1u;
    }
    return count;
}

void require_stream(const std::ofstream& stream, const std::filesystem::path& path) {
    if (!stream) {
        throw std::runtime_error("cannot write " + path.string());
    }
}

std::vector<openisac::TdlTap> parse_taps(const std::string& text) {
    std::vector<openisac::TdlTap> taps;
    std::string normalized = text;
    std::replace(normalized.begin(), normalized.end(), ';', ',');
    std::replace(normalized.begin(), normalized.end(), '+', ',');
    std::stringstream entries(normalized);
    std::string entry;
    while (std::getline(entries, entry, ',')) {
        std::stringstream fields(entry);
        std::string delay;
        std::string gain;
        std::string phase;
        if (!std::getline(fields, delay, ':') || !std::getline(fields, gain, ':')) {
            throw std::invalid_argument("TDL taps use delay:gain_db:phase_deg");
        }
        std::getline(fields, phase, ':');
        const long parsed_delay = std::stol(delay);
        if (parsed_delay < 0) {
            throw std::invalid_argument("TDL delay must be non-negative");
        }
        const auto duplicate = std::find_if(
            taps.begin(), taps.end(), [parsed_delay](const openisac::TdlTap& tap) {
                return tap.delay_samples == static_cast<std::size_t>(parsed_delay);
            });
        if (duplicate != taps.end()) {
            throw std::invalid_argument("TDL delays must be unique");
        }
        taps.push_back({
            static_cast<std::size_t>(parsed_delay),
            std::stof(gain),
            phase.empty() ? 0.0f : std::stof(phase)});
    }
    if (taps.empty()) {
        throw std::invalid_argument("at least one TDL tap is required");
    }
    return taps;
}

const char* controller_reason_name(openisac::ControllerReason reason) {
    switch (reason) {
        case openisac::ControllerReason::hold: return "hold";
        case openisac::ControllerReason::quality_downshift: return "quality_downshift";
        case openisac::ControllerReason::crc_fast_downshift: return "crc_fast_downshift";
        case openisac::ControllerReason::outage_fast_downshift: return "outage_fast_downshift";
        case openisac::ControllerReason::upshift_hysteresis: return "upshift_hysteresis";
        case openisac::ControllerReason::confirmed_step_upshift: return "confirmed_step_upshift";
    }
    return "unknown";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::filesystem::path golden = OPENISAC_GOLDEN_DIR;
        const std::filesystem::path output =
            argc > 1 ? std::filesystem::path(argv[1])
                     : std::filesystem::path("measurement/cpp_windows_plot");
        const float snr_db = argc > 2 ? std::stof(argv[2]) : 40.0f;
        const std::size_t timing_offset =
            argc > 3 ? static_cast<std::size_t>(std::stoul(argv[3])) : 20u;
        const std::string tap_text = argc > 4 ? argv[4] : "0:0:0,3:-4:45,9:-8:-80";
        const float cfo_hz = argc > 5 ? std::stof(argv[5]) : 300.0f;
        const float sfo_ppm = argc > 6 ? std::stof(argv[6]) : 20.0f;
        const unsigned random_seed =
            argc > 7 ? static_cast<unsigned>(std::stoul(argv[7])) : 0xC057u;
        const auto taps = parse_taps(tap_text);
        if (!std::isfinite(snr_db) || !std::isfinite(cfo_hz) ||
            !std::isfinite(sfo_ppm) || std::abs(sfo_ppm) >= 10000.0f) {
            throw std::invalid_argument("SNR/CFO/SFO must be finite and SFO below 10000 ppm");
        }
        std::filesystem::create_directories(output);

        constexpr std::size_t fft_size = 1024u;
        constexpr std::size_t cp_length = 128u;
        constexpr std::size_t symbol_samples = fft_size + cp_length;
        constexpr std::size_t data_symbols = 2u;
        constexpr std::size_t frame_symbols = 3u;
        constexpr std::size_t antennas = 2u;
        constexpr unsigned bits_per_symbol = 6u;
        constexpr float subcarrier_spacing_hz = 15000.0f;

        const auto tx_grid = openisac::read_binary_vector<std::complex<float>>(
            openisac::join_path(golden.string(), "tx_grid_cf32.bin"));
        const auto ideal_labels = openisac::read_binary_vector<std::uint8_t>(
            openisac::join_path(golden.string(), "payload_labels_u8.bin"));
        if (tx_grid.size() != data_symbols * fft_size * antennas ||
            ideal_labels.size() != 2432u) {
            throw std::runtime_error("unexpected golden vector dimensions");
        }

        std::array<std::vector<std::complex<float>>, antennas> tx_time;
        for (auto& values : tx_time) {
            values.resize(frame_symbols * symbol_samples);
        }
        const auto preamble = openisac::generate_zc_ofdm_symbol(fft_size, cp_length, 29u);
        std::copy(preamble.begin(), preamble.end(), tx_time[0].begin());
        for (std::size_t symbol = 0u; symbol < data_symbols; ++symbol) {
            for (std::size_t tx = 0u; tx < antennas; ++tx) {
                std::vector<std::complex<float>> frequency(fft_size);
                for (std::size_t fft = 0u; fft < fft_size; ++fft) {
                    frequency[fft] = tx_grid[(symbol * fft_size + fft) * antennas + tx];
                }
                const auto samples = openisac::ofdm_modulate(frequency, cp_length);
                std::copy(
                    samples.begin(), samples.end(),
                    tx_time[tx].begin() +
                        static_cast<std::ptrdiff_t>((symbol + 1u) * symbol_samples));
            }
        }

        const auto impulse_response = openisac::build_deterministic_tdl_2x2(taps);
        const std::size_t maximum_delay = impulse_response[0][0].size() - 1u;
        if (maximum_delay >= cp_length) {
            throw std::invalid_argument("TDL maximum delay must be shorter than CP");
        }
        const float noise_variance = std::pow(10.0f, -snr_db / 10.0f);
        const float component_sigma = std::sqrt(noise_variance * 0.5f);
        std::mt19937 random(random_seed);
        std::normal_distribution<float> normal(0.0f, component_sigma);
        std::array<std::vector<std::complex<float>>, antennas> clean_rx;
        for (auto& values : clean_rx) {
            values.resize(frame_symbols * symbol_samples);
        }
        for (std::size_t symbol = 0u; symbol < frame_symbols; ++symbol) {
            std::array<std::vector<std::complex<float>>, 2> transmitted;
            for (std::size_t tx = 0u; tx < antennas; ++tx) {
                const auto begin = tx_time[tx].begin() +
                                   static_cast<std::ptrdiff_t>(symbol * symbol_samples);
                transmitted[tx].assign(begin, begin + symbol_samples);
            }
            const auto received =
                openisac::apply_tdl_2x2_symbol(transmitted, impulse_response);
            for (std::size_t rx = 0u; rx < antennas; ++rx) {
                std::copy(
                    received[rx].begin(), received[rx].end(),
                    clean_rx[rx].begin() +
                        static_cast<std::ptrdiff_t>(symbol * symbol_samples));
            }
        }

        const std::size_t maximum_search = timing_offset + 32u;
        const std::size_t stream_samples =
            timing_offset + frame_symbols * symbol_samples + 32u;
        std::vector<std::vector<std::complex<float>>> rx_stream(
            antennas, std::vector<std::complex<float>>(stream_samples));
        for (auto& values : rx_stream) {
            for (auto& value : values) {
                value = {normal(random), normal(random)};
            }
        }
        for (std::size_t rx = 0u; rx < antennas; ++rx) {
            for (std::size_t index = 0u; index < clean_rx[rx].size(); ++index) {
                rx_stream[rx][timing_offset + index] += clean_rx[rx][index];
            }
        }
        const float cfo_normalized = cfo_hz / subcarrier_spacing_hz;
        openisac::apply_cfo_normalized_inplace(rx_stream, cfo_normalized, fft_size);
        rx_stream = openisac::resample_sfo_cubic(rx_stream, sfo_ppm);

        const auto timing = openisac::estimate_zc_timing(
            rx_stream, preamble, maximum_search);
        if (timing.offset + frame_symbols * symbol_samples > stream_samples) {
            throw std::runtime_error("synchronized frame exceeds receive stream");
        }
        const float estimated_cfo_normalized = openisac::estimate_cp_cfo_normalized(
            rx_stream, timing.offset, fft_size, cp_length, frame_symbols, maximum_delay);
        openisac::apply_cfo_normalized_inplace(
            rx_stream, -estimated_cfo_normalized, fft_size);
        const float residual_cfo_normalized = openisac::estimate_cp_cfo_normalized(
            rx_stream, timing.offset, fft_size, cp_length, frame_symbols, maximum_delay);
        const auto demodulate_grid = [&](std::size_t frame_offset) {
            std::vector<std::complex<float>> grid(data_symbols * fft_size * antennas);
            for (std::size_t symbol = 0u; symbol < data_symbols; ++symbol) {
                for (std::size_t rx = 0u; rx < antennas; ++rx) {
                    const auto begin = rx_stream[rx].begin() +
                        static_cast<std::ptrdiff_t>(
                            frame_offset + (symbol + 1u) * symbol_samples);
                    const std::vector<std::complex<float>> samples(
                        begin, begin + symbol_samples);
                    const auto frequency =
                        openisac::ofdm_demodulate(samples, fft_size, cp_length);
                    for (std::size_t fft = 0u; fft < fft_size; ++fft) {
                        grid[(symbol * fft_size + fft) * antennas + rx] = frequency[fft];
                    }
                }
            }
            return grid;
        };
        const auto layout = openisac::build_formal_frame_layout({});
        const auto initial_grid = demodulate_grid(timing.offset);
        const auto sfo_estimate = openisac::estimate_sfo_phase_slope(
            initial_grid, layout.phase_reference_fft_indices,
            fft_size, antennas, symbol_samples);
        auto rx_grid = initial_grid;
        openisac::correct_second_symbol_phase_inplace(
            rx_grid, sfo_estimate, fft_size, antennas);
        const auto residual_sfo_estimate = openisac::estimate_sfo_phase_slope(
            rx_grid, layout.phase_reference_fft_indices,
            fft_size, antennas, symbol_samples);
        const auto estimated_channels =
            openisac::estimate_fdm_pilot_channel_linear_2x2(
                rx_grid, tx_grid, layout.pilot_fft_indices, data_symbols, fft_size);
        double channel_error_power = 0.0;
        double channel_reference_power = 0.0;
        for (std::size_t time = 0u; time < data_symbols; ++time) {
            for (const std::size_t fft : layout.data_fft_indices) {
                const auto truth = openisac::tdl_frequency_response(
                    impulse_response, fft, fft_size);
                const auto& estimate = estimated_channels[time * fft_size + fft];
                channel_error_power += std::norm(estimate.h00 - truth.h00) +
                                       std::norm(estimate.h01 - truth.h01) +
                                       std::norm(estimate.h10 - truth.h10) +
                                       std::norm(estimate.h11 - truth.h11);
                channel_reference_power += std::norm(truth.h00) + std::norm(truth.h01) +
                                           std::norm(truth.h10) + std::norm(truth.h11);
            }
        }
        const double channel_nmse = channel_error_power / channel_reference_power;
        std::vector<std::complex<float>> equalized(ideal_labels.size());
        std::size_t error_bits = 0u;
        std::size_t perfect_error_bits = 0u;
        double error_power = 0.0;
        double perfect_error_power = 0.0;
        double reference_power = 0.0;
        std::vector<openisac::Channel2x2> adaptation_channels;
        std::vector<std::array<float, 2>> adaptation_mse;
        adaptation_channels.reserve(layout.payload_time_indices.size());
        adaptation_mse.reserve(layout.payload_time_indices.size());
        for (std::size_t payload = 0u; payload < layout.payload_time_indices.size(); ++payload) {
            const std::size_t symbol = layout.payload_time_indices[payload];
            const std::size_t data_position = layout.payload_data_positions[payload];
            const std::size_t fft = layout.data_fft_indices[data_position];
            const std::array<std::complex<float>, 2> received{{
                rx_grid[(symbol * fft_size + fft) * antennas],
                rx_grid[(symbol * fft_size + fft) * antennas + 1u]}};
            const auto& channel = estimated_channels[symbol * fft_size + fft];
            const auto detected = openisac::detect_2x2(
                received, channel, noise_variance, openisac::LinearDetector::mmse);
            adaptation_channels.push_back(channel);
            adaptation_mse.push_back(detected.predicted_mse);
            const auto true_channel = openisac::tdl_frequency_response(
                impulse_response, fft, fft_size);
            const auto perfect_detected = openisac::detect_2x2(
                received, true_channel, noise_variance, openisac::LinearDetector::mmse);
            for (std::size_t layer = 0u; layer < antennas; ++layer) {
                const std::size_t index = payload * antennas + layer;
                equalized[index] = detected.symbols[layer];
                const unsigned hard = openisac::SquareQAM::demodulate(
                    detected.symbols[layer], bits_per_symbol);
                error_bits += bit_errors(ideal_labels[index], hard, bits_per_symbol);
                const auto ideal = openisac::SquareQAM::modulate(
                    ideal_labels[index], bits_per_symbol);
                const unsigned perfect_hard = openisac::SquareQAM::demodulate(
                    perfect_detected.symbols[layer], bits_per_symbol);
                perfect_error_bits += bit_errors(
                    ideal_labels[index], perfect_hard, bits_per_symbol);
                error_power += std::norm(detected.symbols[layer] - ideal);
                perfect_error_power += std::norm(perfect_detected.symbols[layer] - ideal);
                reference_power += std::norm(ideal);
            }
        }
        const double ber = static_cast<double>(error_bits) /
                           static_cast<double>(ideal_labels.size() * bits_per_symbol);
        const double evm_percent = 100.0 * std::sqrt(error_power / reference_power);
        const double perfect_ber = static_cast<double>(perfect_error_bits) /
                                   static_cast<double>(ideal_labels.size() * bits_per_symbol);
        const double perfect_evm_percent =
            100.0 * std::sqrt(perfect_error_power / reference_power);

        const auto packed_transmitted = openisac::read_binary_vector<std::uint8_t>(
            openisac::join_path(golden.string(), "transmitted_bits_packed_msb_u8.bin"));
        const auto transmitted_bits = openisac::unpack_msb_bits(
            packed_transmitted, layout.ldpc_blocks * openisac::ldpc_encoded_bits);
        const std::size_t coded_qam_symbols =
            transmitted_bits.size() / bits_per_symbol;
        std::vector<float> payload_llrs;
        payload_llrs.reserve(transmitted_bits.size());
        for (std::size_t symbol = 0u; symbol < coded_qam_symbols; ++symbol) {
            const std::size_t payload = symbol / antennas;
            const std::size_t layer = symbol % antennas;
            const auto llrs = openisac::SquareQAM::max_log_llrs(
                equalized[symbol], adaptation_mse[payload][layer], bits_per_symbol);
            for (unsigned bit = 0u; bit < bits_per_symbol; ++bit) {
                payload_llrs.push_back(llrs[bit]);
            }
        }
        std::size_t pre_ldpc_errors = 0u;
        for (std::size_t bit = 0u; bit < transmitted_bits.size(); ++bit) {
            pre_ldpc_errors += (payload_llrs[bit] < 0.0f ? 1u : 0u) != transmitted_bits[bit];
        }
        openisac::deinterleave_ldpc_blocks(payload_llrs);
        openisac::soft_descramble(payload_llrs);
        const std::string matrix_root = OPENISAC_MATRIX_DIR;
        const openisac::Ldpc5041008 ldpc_codec(
            openisac::join_path(matrix_root, "LDPC_504_1008.alist"),
            openisac::join_path(matrix_root, "LDPC_504_1008G.alist"));
        std::vector<std::uint8_t> decoded_information_bits;
        decoded_information_bits.reserve(layout.ldpc_blocks * openisac::ldpc_information_bits);
        std::size_t syndrome_failures = 0u;
        unsigned maximum_decoder_iterations = 0u;
        for (std::size_t block = 0u; block < layout.ldpc_blocks; ++block) {
            const auto begin = payload_llrs.begin() +
                static_cast<std::ptrdiff_t>(block * openisac::ldpc_encoded_bits);
            const std::vector<float> block_llrs(
                begin, begin + openisac::ldpc_encoded_bits);
            const auto decoded = ldpc_codec.decode_normalized_min_sum(
                block_llrs, 6u, 0.8f);
            syndrome_failures += decoded.syndrome_weight != 0u;
            maximum_decoder_iterations = std::max(
                maximum_decoder_iterations, decoded.iterations);
            decoded_information_bits.insert(
                decoded_information_bits.end(),
                decoded.information_bits.begin(), decoded.information_bits.end());
        }
        const auto reference_information_bits = openisac::unpack_msb_bits(
            openisac::read_binary_vector<std::uint8_t>(
                openisac::join_path(golden.string(), "information_bytes_u8.bin")),
            decoded_information_bits.size());
        std::size_t post_ldpc_errors = 0u;
        for (std::size_t bit = 0u; bit < decoded_information_bits.size(); ++bit) {
            post_ldpc_errors += decoded_information_bits[bit] != reference_information_bits[bit];
        }
        const auto decoded_information_bytes = openisac::pack_msb_bits(
            decoded_information_bits);
        const bool payload_crc_ok = openisac::check_crc16_ccitt_false(
            decoded_information_bytes);
        const double pre_ldpc_ber = static_cast<double>(pre_ldpc_errors) /
                                    static_cast<double>(transmitted_bits.size());
        const double post_ldpc_ber = static_cast<double>(post_ldpc_errors) /
                                     static_cast<double>(decoded_information_bits.size());
        const auto link_decision = openisac::recommend_rank_mcs(
            adaptation_channels, adaptation_mse, noise_variance,
            openisac::LinearDetector::mmse, openisac::Modulation::qam64);
        openisac::AdaptiveLinkController link_controller(
            {2u, openisac::Modulation::qam64}, 3u);
        const auto controller_update = link_controller.observe(
            link_decision.desired, false, link_decision.outage);

        const auto waveform_path = output / "waveform.csv";
        std::ofstream waveform(waveform_path);
        require_stream(waveform, waveform_path);
        waveform << "sample,symbol,sample_in_symbol,tx0_i,tx0_q,tx1_i,tx1_q,"
                    "rx0_i,rx0_q,rx1_i,rx1_q\n";
        waveform << std::setprecision(9);
        for (std::size_t index = 0u; index < stream_samples; ++index) {
            const bool in_frame =
                index >= timing_offset &&
                index < timing_offset + frame_symbols * symbol_samples;
            const std::size_t tx_index = in_frame ? index - timing_offset : 0u;
            const std::complex<float> tx0 = in_frame ? tx_time[0][tx_index] : std::complex<float>{};
            const std::complex<float> tx1 = in_frame ? tx_time[1][tx_index] : std::complex<float>{};
            waveform << index << ','
                     << (in_frame ? static_cast<long long>(tx_index / symbol_samples) : -1ll)
                     << ',' << (in_frame ? tx_index % symbol_samples : 0u) << ','
                     << tx0.real() << ',' << tx0.imag() << ','
                     << tx1.real() << ',' << tx1.imag() << ','
                     << rx_stream[0][index].real() << ',' << rx_stream[0][index].imag() << ','
                     << rx_stream[1][index].real() << ',' << rx_stream[1][index].imag() << '\n';
        }

        const auto channel_path = output / "channel.csv";
        std::ofstream channel_file(channel_path);
        require_stream(channel_file, channel_path);
        channel_file << "fft,h00_i,h00_q,h01_i,h01_q,h10_i,h10_q,h11_i,h11_q,"
                        "e00_i,e00_q,e01_i,e01_q,e10_i,e10_q,e11_i,e11_q\n";
        channel_file << std::setprecision(9);
        for (std::size_t fft = 0u; fft < fft_size; ++fft) {
            const auto channel = openisac::tdl_frequency_response(
                impulse_response, fft, fft_size);
            const auto& estimate = estimated_channels[fft];
            channel_file << fft << ','
                         << channel.h00.real() << ',' << channel.h00.imag() << ','
                         << channel.h01.real() << ',' << channel.h01.imag() << ','
                         << channel.h10.real() << ',' << channel.h10.imag() << ','
                         << channel.h11.real() << ',' << channel.h11.imag() << ','
                         << estimate.h00.real() << ',' << estimate.h00.imag() << ','
                         << estimate.h01.real() << ',' << estimate.h01.imag() << ','
                         << estimate.h10.real() << ',' << estimate.h10.imag() << ','
                         << estimate.h11.real() << ',' << estimate.h11.imag() << '\n';
        }

        const auto sync_path = output / "synchronization.csv";
        std::ofstream sync_file(sync_path);
        require_stream(sync_file, sync_path);
        sync_file << "candidate,metric\n";
        for (std::size_t candidate = 0u; candidate < timing.metrics.size(); ++candidate) {
            sync_file << candidate << ',' << timing.metrics[candidate] << '\n';
        }

        const auto constellation_path = output / "constellation.csv";
        std::ofstream constellation(constellation_path);
        require_stream(constellation, constellation_path);
        constellation << "index,layer,ideal_label,hard_label,ideal_i,ideal_q,equalized_i,equalized_q\n";
        constellation << std::setprecision(9);
        for (std::size_t index = 0u; index < equalized.size(); ++index) {
            const auto ideal = openisac::SquareQAM::modulate(
                ideal_labels[index], bits_per_symbol);
            const auto hard = openisac::SquareQAM::demodulate(
                equalized[index], bits_per_symbol);
            constellation << index << ',' << index % antennas << ','
                          << static_cast<unsigned>(ideal_labels[index]) << ',' << hard << ','
                          << ideal.real() << ',' << ideal.imag() << ','
                          << equalized[index].real() << ',' << equalized[index].imag() << '\n';
        }

        const auto metrics_path = output / "metrics.csv";
        std::ofstream metrics(metrics_path);
        require_stream(metrics, metrics_path);
        metrics << "metric,value\n"
                << "snr_db," << snr_db << '\n'
                << "random_seed," << random_seed << '\n'
                << "ber," << std::setprecision(12) << ber << '\n'
                << "evm_percent," << evm_percent << '\n'
                << "perfect_csi_ber," << perfect_ber << '\n'
                << "perfect_csi_evm_percent," << perfect_evm_percent << '\n'
                << "bit_errors," << error_bits << '\n'
                << "payload_bits," << ideal_labels.size() * bits_per_symbol << '\n'
                << "fft_size," << fft_size << '\n'
                << "cp_length," << cp_length << '\n';
        metrics << "frame_symbols," << frame_symbols << '\n'
                << "symbol_samples," << symbol_samples << '\n'
                << "timing_offset_true," << timing_offset << '\n'
                << "timing_offset_estimated," << timing.offset << '\n'
                << "timing_offset_after_sfo_correction," << timing.offset << '\n'
                << "timing_peak_metric," << timing.peak_metric << '\n'
                << "cfo_hz_true," << cfo_hz << '\n'
                << "cfo_hz_estimated," << estimated_cfo_normalized * subcarrier_spacing_hz << '\n'
                << "cfo_hz_residual," << residual_cfo_normalized * subcarrier_spacing_hz << '\n'
                << "sfo_ppm_true," << sfo_ppm << '\n'
                << "sfo_ppm_estimated," << sfo_estimate.sfo_ppm << '\n'
                << "sfo_ppm_combined_estimated," << sfo_estimate.sfo_ppm << '\n'
                << "sfo_ppm_residual," << residual_sfo_estimate.sfo_ppm << '\n'
                << "sfo_phase_reference_coherence," << sfo_estimate.coherence << '\n'
                << "tdl_path_count," << taps.size() << '\n'
                << "tdl_max_delay," << maximum_delay << '\n'
                << "channel_nmse_db," << 10.0 * std::log10(channel_nmse) << '\n'
                << "channel_rms_error_percent," << 100.0 * std::sqrt(channel_nmse) << '\n';
        metrics << "rank_mcs_desired_rank," << link_decision.desired.rank << '\n'
                << "rank_mcs_desired_modulation,"
                << openisac::modulation_name(link_decision.desired.modulation) << '\n'
                << "rank_mcs_selected_rank," << controller_update.selected.rank << '\n'
                << "rank_mcs_selected_modulation,"
                << openisac::modulation_name(controller_update.selected.modulation) << '\n'
                << "rank_mcs_controller_reason,"
                << controller_reason_name(controller_update.reason) << '\n'
                << "rank2_bottleneck_sinr_db," << link_decision.rank2_bottleneck_sinr_db << '\n'
                << "rank1_sinr_db," << link_decision.rank1_sinr_db << '\n'
                << "channel_minimum_eigenvalue_ratio,"
                << link_decision.minimum_eigenvalue_ratio << '\n'
                << "configured_64qam_supported,"
                << (link_decision.configured_mcs_supported ? 1 : 0) << '\n'
                << "link_outage," << (link_decision.outage ? 1 : 0) << '\n';
        metrics << "pre_ldpc_ber," << pre_ldpc_ber << '\n'
                << "post_ldpc_ber," << post_ldpc_ber << '\n'
                << "ldpc_syndrome_failures," << syndrome_failures << '\n'
                << "ldpc_blocks," << layout.ldpc_blocks << '\n'
                << "ldpc_maximum_iterations_used," << maximum_decoder_iterations << '\n'
                << "payload_crc_ok," << (payload_crc_ok ? 1 : 0) << '\n'
                << "frame_error," << (payload_crc_ok ? 0 : 1) << '\n';

        std::cout << std::fixed << std::setprecision(6)
                  << "C++ diagnostics written to " << output.string() << '\n'
                  << "SNR=" << snr_db << " dB, BER=" << ber
                  << ", EVM=" << evm_percent << "%"
                  << " (perfect CSI BER=" << perfect_ber
                  << ", EVM=" << perfect_evm_percent << "%)\n"
                  << "TDL paths=" << taps.size() << ", max delay=" << maximum_delay
                  << ", timing true/estimated=" << timing_offset << '/' << timing.offset
                  << ", peak=" << timing.peak_metric
                  << ", CFO true/estimated/residual=" << cfo_hz << '/'
                  << estimated_cfo_normalized * subcarrier_spacing_hz << '/'
                  << residual_cfo_normalized * subcarrier_spacing_hz << " Hz"
                  << ", SFO true/estimated/residual=" << sfo_ppm << '/'
                  << sfo_estimate.sfo_ppm << '/' << residual_sfo_estimate.sfo_ppm
                  << " ppm"
                  << ", LS CSI NMSE=" << 10.0 * std::log10(channel_nmse) << " dB\n"
                  << "Rank/MCS desired=" << link_decision.desired.rank << "x "
                  << openisac::modulation_name(link_decision.desired.modulation)
                  << ", selected=" << controller_update.selected.rank << "x "
                  << openisac::modulation_name(controller_update.selected.modulation)
                  << ", rank2 SINR=" << link_decision.rank2_bottleneck_sinr_db
                  << " dB, eigenvalue ratio=" << link_decision.minimum_eigenvalue_ratio
                  << ", reason=" << controller_reason_name(controller_update.reason) << '\n';
        std::cout << "LDPC pre/post BER=" << pre_ldpc_ber << '/' << post_ldpc_ber
                  << ", syndrome failures=" << syndrome_failures << '/' << layout.ldpc_blocks
                  << ", CRC=" << (payload_crc_ok ? "PASS" : "FAIL")
                  << ", max iterations=" << maximum_decoder_iterations << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
