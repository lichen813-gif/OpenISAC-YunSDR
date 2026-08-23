#include "openisac/binary_io.hpp"
#include "openisac/ldpc.hpp"
#include "openisac/rank4_formal_link.hpp"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

openisac::Modulation modulation(const std::string& value) {
    if (value == "QPSK" || value == "qpsk") return openisac::Modulation::qpsk;
    if (value == "16QAM" || value == "16qam") return openisac::Modulation::qam16;
    if (value == "64QAM" || value == "64qam") return openisac::Modulation::qam64;
    if (value == "256QAM" || value == "256qam") return openisac::Modulation::qam256;
    throw std::invalid_argument(
        "modulation must be QPSK, 16QAM, 64QAM or 256QAM");
}

void usage() {
    std::cout
        << "OpenISAC formal Rank-4 LDPC/CRC channel diagnostic\n"
        << "  --frames N             formal frames (4)\n"
        << "  --snr DB               receive SNR dB (50)\n"
        << "  --modulation NAME      QPSK/16QAM/64QAM/256QAM (64QAM)\n"
        << "  --payload-bytes N      0 selects full frame capacity (0)\n"
        << "  --tx-correlation RHO   exponential Tx correlation (0.2)\n"
        << "  --rx-correlation RHO   exponential Rx correlation (0.2)\n"
        << "  --output DIRECTORY     CSV output directory\n"
        << "  --check                require every header, CRC and payload to pass\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        openisac::Rank4FormalSimulationConfig config;
        std::filesystem::path output =
            "measurement/cpp_4x4_formal_diagnostics";
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
                config.frames = static_cast<std::size_t>(std::stoull(value()));
            } else if (option == "--snr") {
                config.snr_db = std::stof(value());
            } else if (option == "--modulation") {
                config.modulation = modulation(value());
            } else if (option == "--payload-bytes") {
                config.payload_bytes = static_cast<std::size_t>(
                    std::stoull(value()));
            } else if (option == "--tx-correlation") {
                config.transmit_correlation = std::stof(value());
            } else if (option == "--rx-correlation") {
                config.receive_correlation = std::stof(value());
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

        const std::string matrix_root = OPENISAC_MATRIX_DIR;
        const openisac::Ldpc5041008 codec(
            openisac::join_path(matrix_root, "LDPC_504_1008.alist"),
            openisac::join_path(matrix_root, "LDPC_504_1008G.alist"));
        const auto result = openisac::simulate_rank4_formal_link(config, codec);
        std::filesystem::create_directories(output);
        std::ofstream summary(output / "summary.csv");
        if (!summary) {
            throw std::runtime_error("cannot create Rank-4 formal summary CSV");
        }
        summary << std::setprecision(12)
                << "metric,value\n"
                << "frames," << result.frames << '\n'
                << "snr_db," << config.snr_db << '\n'
                << "bits_per_symbol,"
                << openisac::modulation_bits(config.modulation) << '\n'
                << "payload_bytes_per_frame,"
                << result.user_payload_bytes_per_frame << '\n'
                << "ldpc_blocks_per_frame,"
                << result.ldpc_blocks_per_frame << '\n'
                << "header_passes," << result.header_passes << '\n'
                << "crc_passes," << result.crc_passes << '\n'
                << "payload_matches," << result.payload_matches << '\n'
                << "syndrome_failures," << result.syndrome_failures << '\n'
                << "pre_fec_compared_bits,"
                << result.pre_fec_compared_bits << '\n'
                << "pre_fec_bit_errors," << result.pre_fec_bit_errors << '\n'
                << "pre_fec_ber," << result.pre_fec_ber << '\n'
                << "evm_percent," << result.evm_percent << '\n'
                << "channel_nmse_db," << result.channel_nmse_db << '\n';

        std::ofstream constellation(output / "constellation.csv");
        if (!constellation) {
            throw std::runtime_error(
                "cannot create Rank-4 formal constellation CSV");
        }
        constellation << "symbol,layer,tx_i,tx_q,rx_i,rx_q\n";
        for (std::size_t index = 0u;
             index < result.equalized_symbols.size(); ++index) {
            const auto tx = result.transmitted_symbols[index];
            const auto rx = result.equalized_symbols[index];
            constellation << index / 4u << ',' << index % 4u << ','
                          << tx.real() << ',' << tx.imag() << ','
                          << rx.real() << ',' << rx.imag() << '\n';
        }

        std::cout << std::fixed << std::setprecision(4)
                  << "Rank-4 formal frame: " << result.frames << " frames, "
                  << result.user_payload_bytes_per_frame << " bytes/frame, "
                  << result.ldpc_blocks_per_frame << " LDPC blocks/frame\n"
                  << "header/CRC/payload=" << result.header_passes << '/'
                  << result.crc_passes << '/' << result.payload_matches
                  << " of " << result.frames
                  << ", syndrome failures=" << result.syndrome_failures << '\n'
                  << "pre-FEC BER=" << result.pre_fec_ber
                  << ", EVM=" << result.evm_percent << "%"
                  << ", CSI NMSE=" << result.channel_nmse_db << " dB\n"
                  << "CSV: " << std::filesystem::absolute(output).string()
                  << '\n';
        if (check &&
            (result.header_passes != result.frames ||
             result.crc_passes != result.frames ||
             result.payload_matches != result.frames)) {
            std::cerr << "FAIL: Rank-4 formal frame acceptance failed\n";
            return 2;
        }
        if (check) {
            std::cout
                << "PASS: Rank-4 header/pilots/MMSE/LDPC/CRC channel closure\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Rank-4 formal diagnostic error: " << error.what() << '\n';
        return 1;
    }
}
