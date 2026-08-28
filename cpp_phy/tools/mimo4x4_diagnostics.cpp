#include "openisac/mimo_nxn_link.hpp"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

unsigned modulation_bits(const std::string& value) {
    if (value == "QPSK" || value == "qpsk") return 2u;
    if (value == "16QAM" || value == "16qam") return 4u;
    if (value == "64QAM" || value == "64qam") return 6u;
    if (value == "256QAM" || value == "256qam") return 8u;
    throw std::invalid_argument(
        "modulation must be QPSK, 16QAM, 64QAM or 256QAM");
}

void usage() {
    std::cout
        << "OpenISAC algorithm-only generic NxN OFDM diagnostic\n"
        << "  --streams N            spatial streams/ports: 1, 2, 4 or 8 (4)\n"
        << "  --frames N             OFDM frames (10)\n"
        << "  --snr DB               receive SNR dB (45)\n"
        << "  --modulation NAME      QPSK/16QAM/64QAM/256QAM (64QAM)\n"
        << "  --pilot-spacing N      aggregate FDM pilot spacing (2)\n"
        << "  --tx-correlation RHO   exponential Tx correlation (0.2)\n"
        << "  --rx-correlation RHO   exponential Rx correlation (0.2)\n"
        << "  --flat-channel         use a single zero-delay path for closure\n"
        << "  --seed N               spatial channel seed (311383)\n"
        << "  --output DIRECTORY     CSV output directory\n"
        << "  --check                fail unless uncoded BER<1% and EVM<8%\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        openisac::NxNOfdmSimulationConfig config;
        config.streams = 4u;
        std::filesystem::path output = "measurement/cpp_4x4_diagnostics";
        bool output_explicit = false;
        bool check = false;
        for (int index = 1; index < argc; ++index) {
            const std::string option = argv[index];
            auto value = [&]() -> std::string {
                if (++index >= argc) {
                    throw std::invalid_argument(option + " requires a value");
                }
                return argv[index];
            };
            if (option == "--streams") {
                config.streams = static_cast<std::size_t>(std::stoull(value()));
                if (config.streams != 1u && config.streams != 2u &&
                    config.streams != 4u && config.streams != 8u) {
                    throw std::invalid_argument("streams must be 1, 2, 4 or 8");
                }
            } else if (option == "--frames") {
                config.frames = static_cast<std::size_t>(std::stoull(value()));
            } else if (option == "--snr") {
                config.snr_db = std::stof(value());
            } else if (option == "--modulation") {
                config.bits_per_symbol = modulation_bits(value());
            } else if (option == "--pilot-spacing") {
                config.pilot_spacing = static_cast<std::size_t>(
                    std::stoull(value()));
            } else if (option == "--tx-correlation") {
                config.transmit_correlation = std::stof(value());
            } else if (option == "--rx-correlation") {
                config.receive_correlation = std::stof(value());
            } else if (option == "--flat-channel") {
                config.taps = {{0u, 0.0f, 0.0f}};
            } else if (option == "--seed") {
                config.channel_seed = static_cast<std::uint32_t>(
                    std::stoul(value()));
            } else if (option == "--output") {
                output = value();
                output_explicit = true;
            } else if (option == "--check") {
                check = true;
            } else if (option == "--help" || option == "-h") {
                usage();
                return 0;
            } else {
                throw std::invalid_argument("unknown option: " + option);
            }
        }
        if (!output_explicit && config.streams != 4u) {
            output = "measurement/cpp_" + std::to_string(config.streams) +
                "x" + std::to_string(config.streams) + "_diagnostics";
        }

        const auto result = openisac::simulate_nxn_ofdm_link(config);
        std::filesystem::create_directories(output);
        std::ofstream summary(output / "summary.csv");
        if (!summary) {
            throw std::runtime_error("cannot create NxN summary CSV");
        }
        summary << std::setprecision(12)
                << "metric,value\n"
                << "streams," << result.streams << '\n'
                << "fft_size," << config.fft_size << '\n'
                << "cp_length," << config.cp_length << '\n'
                << "frames," << config.frames << '\n'
                << "bits_per_symbol," << config.bits_per_symbol << '\n'
                << "pilot_spacing," << config.pilot_spacing << '\n'
                << "snr_db," << config.snr_db << '\n'
                << "tx_correlation," << config.transmit_correlation << '\n'
                << "rx_correlation," << config.receive_correlation << '\n'
                << "channel_taps," << config.taps.size() << '\n'
                << "pilot_subcarriers," << result.pilot_subcarriers << '\n'
                << "data_subcarriers," << result.data_subcarriers << '\n'
                << "detected_symbols," << result.detected_symbols << '\n'
                << "compared_bits," << result.compared_bits << '\n'
                << "bit_errors," << result.bit_errors << '\n'
                << "ber," << result.ber << '\n'
                << "evm_percent," << result.evm_percent << '\n'
                << "perfect_csi_evm_percent,"
                << result.perfect_csi_evm_percent << '\n'
                << "channel_nmse_db," << result.channel_nmse_db << '\n';

        std::ofstream constellation(output / "constellation.csv");
        if (!constellation) {
            throw std::runtime_error("cannot create NxN constellation CSV");
        }
        constellation << "symbol,layer,tx_i,tx_q,rx_i,rx_q\n";
        for (std::size_t index = 0u;
             index < result.equalized_symbols.size(); ++index) {
            const auto tx = result.transmitted_symbols[index];
            const auto rx = result.equalized_symbols[index];
            constellation << index / result.streams << ','
                          << index % result.streams << ','
                          << tx.real() << ',' << tx.imag() << ','
                          << rx.real() << ',' << rx.imag() << '\n';
        }

        std::cout << std::fixed << std::setprecision(4)
                  << config.streams << 'x' << config.streams << ' '
                  << config.bits_per_symbol << "-bit QAM, "
                  << config.frames << " OFDM frames\n"
                  << "pilots/data=" << result.pilot_subcarriers << '/'
                  << result.data_subcarriers
                  << ", symbols=" << result.detected_symbols << '\n'
                  << "BER=" << result.ber
                  << ", EVM=" << result.evm_percent << "%"
                  << ", perfect-CSI EVM="
                  << result.perfect_csi_evm_percent << "%"
                  << ", CSI NMSE=" << result.channel_nmse_db << " dB\n"
                  << "CSV: " << std::filesystem::absolute(output).string() << '\n';
        if (check && (result.ber >= 0.01f || result.evm_percent >= 8.0f)) {
            std::cerr << "FAIL: NxN diagnostic exceeds acceptance threshold\n";
            return 2;
        }
        if (check) {
            std::cout << "PASS: " << config.streams << 'x' << config.streams
                      << " OFDM/pilot/TDL/MMSE algorithm closure\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "NxN diagnostic error: " << error.what() << '\n';
        return 1;
    }
}
