#include "openisac/binary_io.hpp"
#include "openisac/ldpc_frame_decoder.hpp"
#include "openisac/rank4_time_link.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

openisac::Modulation modulation(const std::string& value) {
    if (value == "QPSK" || value == "qpsk") return openisac::Modulation::qpsk;
    if (value == "16QAM" || value == "16qam") return openisac::Modulation::qam16;
    if (value == "64QAM" || value == "64qam") return openisac::Modulation::qam64;
    if (value == "256QAM" || value == "256qam") return openisac::Modulation::qam256;
    throw std::invalid_argument(
        "modulation must be QPSK, 16QAM, 64QAM or 256QAM");
}

openisac::PilotMode pilot_mode(const std::string& value) {
    if (value == "fdm" || value == "FDM") return openisac::PilotMode::fdm;
    if (value == "nr-dmrs" || value == "NR-DMRS" ||
        value == "dmrs" || value == "DMRS") {
        return openisac::PilotMode::nr_dmrs;
    }
    throw std::invalid_argument("pilot mode must be fdm or nr-dmrs");
}

const char* synchronization_name(openisac::Rank4SynchronizationMode mode) {
    switch (mode) {
        case openisac::Rank4SynchronizationMode::search: return "SEARCH";
        case openisac::Rank4SynchronizationMode::track: return "TRACK";
        case openisac::Rank4SynchronizationMode::reacquire: return "REACQUIRE";
    }
    return "UNKNOWN";
}

void usage() {
    std::cout
        << "OpenISAC Rank-4 time-domain synchronization diagnostic\n"
        << "  --frames N             frames (4)\n"
        << "  --snr DB               SNR dB (50)\n"
        << "  --modulation NAME      QPSK/16QAM/64QAM/256QAM (64QAM)\n"
        << "  --pilot-mode NAME      fdm or nr-dmrs (fdm)\n"
        << "  --payload-bytes N      zero selects full capacity (0)\n"
        << "  --timing N             first-frame timing offset samples (20)\n"
        << "  --timing-drift N       added samples per frame (0)\n"
        << "  --cfo HZ               carrier offset Hz (300)\n"
        << "  --sfo PPM              sampling offset ppm (20)\n"
        << "  --tx-correlation RHO   Tx correlation (0.2)\n"
        << "  --rx-correlation RHO   Rx correlation (0.2)\n"
        << "  --mmse-scale VALUE     pilot-residual MMSE loading scale (0.5)\n"
        << "  --intra-frame-average N average two-symbol CSI, 0/1 (1)\n"
        << "  --ldpc-threads N       persistent LDPC workers (12)\n"
        << "  --output DIRECTORY     output directory\n"
        << "  --check                require all timing/header/CRC/payload checks\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        openisac::Rank4TimeSimulationConfig config;
        std::size_t frames = 4u;
        std::size_t timing_drift = 0u;
        std::size_t ldpc_threads = 12u;
        std::filesystem::path output =
            "measurement/cpp_4x4_time_diagnostics";
        bool check = false;
        for (int index = 1; index < argc; ++index) {
            const std::string option = argv[index];
            auto value = [&]() -> std::string {
                if (++index >= argc) {
                    throw std::invalid_argument(option + " requires a value");
                }
                return argv[index];
            };
            if (option == "--frames") {
                frames = static_cast<std::size_t>(std::stoull(value()));
            } else if (option == "--snr") {
                config.snr_db = std::stof(value());
            } else if (option == "--modulation") {
                config.modulation = modulation(value());
            } else if (option == "--pilot-mode") {
                config.pilot_mode = pilot_mode(value());
            } else if (option == "--payload-bytes") {
                config.payload_bytes = static_cast<std::size_t>(
                    std::stoull(value()));
            } else if (option == "--timing") {
                config.timing_offset_samples = static_cast<std::size_t>(
                    std::stoull(value()));
            } else if (option == "--timing-drift") {
                timing_drift = static_cast<std::size_t>(std::stoull(value()));
            } else if (option == "--cfo") {
                config.cfo_hz = std::stof(value());
            } else if (option == "--sfo") {
                config.sfo_ppm = std::stof(value());
            } else if (option == "--tx-correlation") {
                config.transmit_correlation = std::stof(value());
            } else if (option == "--rx-correlation") {
                config.receive_correlation = std::stof(value());
            } else if (option == "--mmse-scale") {
                config.mmse_regularization_scale = std::stof(value());
            } else if (option == "--intra-frame-average") {
                const auto enabled = std::stoul(value());
                if (enabled > 1u) {
                    throw std::invalid_argument(
                        "--intra-frame-average must be 0 or 1");
                }
                config.average_intra_frame_csi = enabled != 0u;
            } else if (option == "--ldpc-threads") {
                ldpc_threads = static_cast<std::size_t>(std::stoull(value()));
            } else if (option == "--output") {
                output = value();
            } else if (option == "--check") {
                check = true;
            } else if (option == "--help" || option == "-h") {
                usage();
                return 0;
            } else {
                throw std::invalid_argument("unknown option: " + option);
            }
        }
        if (frames == 0u) {
            throw std::invalid_argument("frames must be positive");
        }

        const std::string matrix_root = OPENISAC_MATRIX_DIR;
        const openisac::Ldpc5041008 codec(
            openisac::join_path(matrix_root, "LDPC_504_1008.alist"),
            openisac::join_path(matrix_root, "LDPC_504_1008G.alist"));
        openisac::LdpcFrameDecoder ldpc_decoder(codec, ldpc_threads);
        std::filesystem::create_directories(output);
        std::ofstream frame_csv(output / "frames.csv");
        std::ofstream constellation(output / "constellation.csv");
        if (!frame_csv || !constellation) {
            throw std::runtime_error("cannot create Rank-4 time CSV output");
        }
        frame_csv
            << "frame,sync_mode,timing_offset,timing_metric,timing_ok,header_ok,"
               "crc_ok,payload_match,cfo_est_hz,cfo_error_hz,sfo_est_ppm,"
               "residual_sfo_ppm,noise_variance,evm_percent,channel_nmse_db,"
               "pre_fec_ber,receiver_us,simulation_us,timing_candidates\n";
        constellation << "symbol,layer,tx_i,tx_q,rx_i,rx_q\n";
        frame_csv << std::setprecision(12);
        openisac::Rank4TimeReceiverState state;
        openisac::Rank4TimeWorkspace workspace;
        std::size_t passes = 0u;
        double evm_sum = 0.0;
        double nmse_sum = 0.0;
        double receiver_sum = 0.0;
        double simulation_sum = 0.0;
        double synchronization_sum = 0.0;
        double fft_sfo_sum = 0.0;
        double channel_estimation_sum = 0.0;
        double detection_sum = 0.0;
        double soft_demapping_sum = 0.0;
        double ldpc_crc_sum = 0.0;
        std::vector<double> receiver_times;
        receiver_times.reserve(frames);
        std::size_t warmed_workspace_growth_frames = 0u;
        std::size_t warmed_ldpc_growth_frames = 0u;
        unsigned maximum_ldpc_iterations_used = 0u;
        std::size_t total_bit_errors = 0u;
        std::size_t total_compared_bits = 0u;
        const std::size_t initial_timing = config.timing_offset_samples;
        std::size_t point_offset = 0u;
        for (std::size_t frame = 0u; frame < frames; ++frame) {
            config.timing_offset_samples = initial_timing + frame * timing_drift;
            config.random_seed = static_cast<std::uint32_t>(0x7100u + frame * 41u);
            const auto result = openisac::simulate_rank4_time_frame(
                static_cast<std::uint16_t>(frame), config, codec, &state,
                &workspace, &ldpc_decoder);
            const bool passed = result.timing_ok && result.header_ok &&
                result.crc_ok && result.payload_match;
            passes += passed;
            evm_sum += result.evm_percent;
            nmse_sum += result.channel_nmse_db;
            receiver_sum += result.receiver_us;
            receiver_times.push_back(result.receiver_us);
            simulation_sum += result.simulation_us;
            synchronization_sum += result.synchronization_us;
            fft_sfo_sum += result.fft_sfo_us;
            channel_estimation_sum += result.channel_estimation_us;
            detection_sum += result.detection_us;
            soft_demapping_sum += result.soft_demapping_us;
            ldpc_crc_sum += result.ldpc_crc_us;
            maximum_ldpc_iterations_used = std::max(
                maximum_ldpc_iterations_used,
                result.maximum_ldpc_iterations_used);
            if (frame > 0u) {
                warmed_workspace_growth_frames +=
                    result.workspace_growths_this_frame != 0u;
                warmed_ldpc_growth_frames +=
                    result.ldpc_capacity_growths_this_frame != 0u;
            }
            total_bit_errors += result.pre_fec_bit_errors;
            total_compared_bits += result.pre_fec_compared_bits;
            frame_csv << frame << ','
                      << synchronization_name(result.synchronization_mode_used)
                      << ',' << result.timing_offset << ','
                      << result.timing_metric << ',' << result.timing_ok << ','
                      << result.header_ok << ',' << result.crc_ok << ','
                      << result.payload_match << ',' << result.estimated_cfo_hz
                      << ',' << result.cfo_error_hz << ','
                      << result.estimated_sfo_ppm << ','
                      << result.residual_sfo_ppm << ','
                      << result.noise_variance << ',' << result.evm_percent
                      << ',' << result.channel_nmse_db << ','
                      << result.pre_fec_ber << ',' << result.receiver_us << ','
                      << result.simulation_us
                      << ',' << result.timing_candidates_evaluated << '\n';
            for (std::size_t point = 0u;
                 point < result.equalized_symbols.size(); ++point) {
                const auto tx = result.transmitted_symbols[point];
                const auto rx = result.equalized_symbols[point];
                constellation << point_offset + point / 4u << ','
                              << point % 4u << ',' << tx.real() << ','
                              << tx.imag() << ',' << rx.real() << ','
                              << rx.imag() << '\n';
            }
            point_offset += result.equalized_symbols.size() / 4u;
            std::cout << "frame " << frame << ' '
                      << synchronization_name(result.synchronization_mode_used)
                      << " timing=" << result.timing_offset
                      << " CFOerr=" << std::fixed << std::setprecision(2)
                      << result.cfo_error_hz << " Hz EVM="
                      << result.evm_percent << "% CRC=" << result.crc_ok
                      << '\n';
        }
        std::sort(receiver_times.begin(), receiver_times.end());
        const auto lower_percentile = [&](double fraction) {
            const std::size_t index = static_cast<std::size_t>(
                fraction * static_cast<double>(receiver_times.size() - 1u));
            return receiver_times[index];
        };
        const double receiver_p50 = lower_percentile(0.50);
        const double receiver_p99 = lower_percentile(0.99);
        const double receiver_max = receiver_times.back();
        std::ofstream summary(output / "summary.csv");
        if (!summary) {
            throw std::runtime_error("cannot create Rank-4 time summary");
        }
        summary << std::setprecision(12)
                << "metric,value\n"
                << "frames," << frames << '\n'
                << "passes," << passes << '\n'
                << "snr_db," << config.snr_db << '\n'
                << "pilot_mode," << openisac::pilot_mode_name(config.pilot_mode) << '\n'
                << "frame_symbols," << openisac::formal_frame_symbols(config.pilot_mode) << '\n'
                << "frame_period_us,"
                << openisac::formal_frame_period_seconds(config.pilot_mode) * 1.0e6 << '\n'
                << "cfo_hz," << config.cfo_hz << '\n'
                << "sfo_ppm," << config.sfo_ppm << '\n'
                << "evm_percent," << evm_sum / frames << '\n'
                << "ber,"
                << static_cast<double>(total_bit_errors) /
                    static_cast<double>(total_compared_bits) << '\n'
                << "channel_nmse_db," << nmse_sum / frames << '\n'
                << "receiver_us," << receiver_sum / frames << '\n'
                << "receiver_p50_us," << receiver_p50 << '\n'
                << "receiver_p99_us," << receiver_p99 << '\n'
                << "receiver_max_us," << receiver_max << '\n'
                << "simulation_us," << simulation_sum / frames << '\n'
                << "synchronization_us," << synchronization_sum / frames << '\n'
                << "fft_sfo_us," << fft_sfo_sum / frames << '\n'
                << "channel_estimation_us,"
                << channel_estimation_sum / frames << '\n'
                << "detection_us," << detection_sum / frames << '\n'
                << "soft_demapping_us," << soft_demapping_sum / frames << '\n'
                << "ldpc_crc_us," << ldpc_crc_sum / frames << '\n'
                << "ldpc_threads," << ldpc_threads << '\n'
                << "maximum_ldpc_iterations_used,"
                << maximum_ldpc_iterations_used << '\n'
                << "warmed_workspace_growth_frames,"
                << warmed_workspace_growth_frames << '\n'
                << "warmed_ldpc_growth_frames,"
                << warmed_ldpc_growth_frames << '\n'
                << "full_searches," << state.full_search_count << '\n'
                << "tracking_searches," << state.tracking_search_count << '\n'
                << "reacquisitions," << state.reacquisition_count << '\n';

        std::cout << std::fixed << std::setprecision(3)
                  << "Rank-4 time closure: " << passes << '/' << frames
                  << " passed, mean EVM=" << evm_sum / frames
                  << "%, mean CSI NMSE=" << nmse_sum / frames << " dB\n"
                  << "mean receiver=" << receiver_sum / frames
                  << " us, P50/P99/max=" << receiver_p50 << '/'
                  << receiver_p99 << '/' << receiver_max
                  << " us, complete simulation=" << simulation_sum / frames
                  << " us\nmean stages sync/FFT+SFO/CSI/detect/demap/FEC="
                  << synchronization_sum / frames << '/'
                  << fft_sfo_sum / frames << '/'
                  << channel_estimation_sum / frames << '/'
                  << detection_sum / frames << '/'
                  << soft_demapping_sum / frames << '/'
                  << ldpc_crc_sum / frames << " us\nCSV: "
                  << std::filesystem::absolute(output).string() << '\n';
        if (check && passes != frames) {
            std::cerr << "FAIL: Rank-4 time-domain acceptance failed\n";
            return 2;
        }
        if (check &&
            (warmed_workspace_growth_frames != 0u ||
             warmed_ldpc_growth_frames != 0u)) {
            std::cerr << "FAIL: warmed Rank-4 receive path grew buffers\n";
            return 3;
        }
        if (check) {
            std::cout << "PASS: Rank-4 ZC/CFO/SFO/MMSE/LDPC/CRC closure\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Rank-4 time diagnostic error: " << error.what() << '\n';
        return 1;
    }
}
