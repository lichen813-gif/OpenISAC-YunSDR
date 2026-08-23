#include "openisac/binary_io.hpp"
#include "openisac/dynamic_link.hpp"
#include "openisac/dynamic_link_pipeline.hpp"
#include "openisac/frame.hpp"
#include "openisac/ldpc.hpp"
#include "openisac/rank4_time_link.hpp"
#include "openisac/rank4_time_pipeline.hpp"
#include "openisac/sensing.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#error The VLC video channel bridge currently targets Windows Winsock.
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::uint32_t fragment_magic = 0x4356494Fu;  // "OIVC" in LE
constexpr std::uint8_t fragment_version = 1u;
constexpr std::size_t fragment_header_bytes = 20u;
constexpr std::size_t maximum_udp_payload = 65507u;

std::atomic<bool> stop_requested{false};

BOOL WINAPI console_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT ||
        event == CTRL_CLOSE_EVENT) {
        stop_requested.store(true);
        return TRUE;
    }
    return FALSE;
}

struct SocketRuntime {
    SocketRuntime() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
    }
    ~SocketRuntime() { WSACleanup(); }
};

struct SocketHandle {
    SOCKET value = INVALID_SOCKET;
    ~SocketHandle() {
        if (value != INVALID_SOCKET) {
            closesocket(value);
        }
    }
    SocketHandle() = default;
    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;
};

struct Options {
    std::string listen_address = "127.0.0.1";
    unsigned listen_port = 50000u;
    std::string output_address = "127.0.0.1";
    unsigned output_port = 50001u;
    float snr_db = 45.0f;
    float cfo_hz = 300.0f;
    float sfo_ppm = 20.0f;
    std::size_t timing_offset = 20u;
    unsigned random_seed = 0xC057u;
    std::size_t ldpc_workers = 8u;
    std::size_t socket_buffer_bytes = 4u * 1024u * 1024u;
    std::size_t ingress_queue_packets = 8192u;
    std::size_t maximum_datagram_bytes = maximum_udp_payload;
    unsigned transmit_rank = 2u;
    openisac::Modulation modulation = openisac::Modulation::qam64;
    openisac::PilotMode pilot_mode = openisac::PilotMode::fdm;
    std::size_t fft_size = 1024u;
    std::size_t cp_length = 128u;
    float subcarrier_spacing_hz = 15000.0f;
    float center_frequency_hz = 5.8e9f;
    float transmit_spatial_correlation = 0.2f;
    float receive_spatial_correlation = 0.2f;
    float rank4_mmse_regularization_scale = 0.5f;
    std::uint32_t spatial_channel_seed = 0xC057u;
    std::string telemetry_directory;
    float telemetry_interval_seconds = 2.0f;
    std::size_t telemetry_waveform_points = 4096u;
    std::size_t sensing_coherent_frames = 128u;
    std::size_t sensing_export_range_bins = 128u;
    std::vector<openisac::TdlTap> taps{
        {0u, 0.0f, 0.0f},
        {3u, -4.0f, 45.0f},
        {9u, -8.0f, -80.0f},
    };
    bool self_test = false;
    std::size_t self_test_packets = 12u;
};

unsigned parse_unsigned(const std::string& text, const char* name) {
    std::size_t used = 0u;
    const unsigned long value = std::stoul(text, &used, 0);
    if (used != text.size() || value > std::numeric_limits<unsigned>::max()) {
        throw std::invalid_argument(std::string("invalid ") + name);
    }
    return static_cast<unsigned>(value);
}

std::size_t parse_size(const std::string& text, const char* name) {
    std::size_t used = 0u;
    const unsigned long long value = std::stoull(text, &used, 0);
    if (used != text.size() || value > std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument(std::string("invalid ") + name);
    }
    return static_cast<std::size_t>(value);
}

float parse_float(const std::string& text, const char* name) {
    std::size_t used = 0u;
    const float value = std::stof(text, &used);
    if (used != text.size() || !std::isfinite(value)) {
        throw std::invalid_argument(std::string("invalid ") + name);
    }
    return value;
}

openisac::Modulation parse_modulation(const std::string& text) {
    if (text == "qpsk" || text == "QPSK" || text == "4qam" || text == "4QAM") {
        return openisac::Modulation::qpsk;
    }
    if (text == "16qam" || text == "16QAM") {
        return openisac::Modulation::qam16;
    }
    if (text == "64qam" || text == "64QAM") {
        return openisac::Modulation::qam64;
    }
    if (text == "256qam" || text == "256QAM") {
        return openisac::Modulation::qam256;
    }
    throw std::invalid_argument("modulation must be QPSK, 16QAM, 64QAM or 256QAM");
}

openisac::PilotMode parse_pilot_mode(const std::string& text) {
    if (text == "fdm" || text == "FDM") {
        return openisac::PilotMode::fdm;
    }
    if (text == "nr-dmrs" || text == "NR-DMRS" ||
        text == "dmrs" || text == "DMRS") {
        return openisac::PilotMode::nr_dmrs;
    }
    throw std::invalid_argument("pilot mode must be fdm or nr-dmrs");
}

std::vector<openisac::TdlTap> parse_tdl(const std::string& text) {
    std::vector<openisac::TdlTap> taps;
    std::stringstream paths(text);
    std::string path;
    while (std::getline(paths, path, '+')) {
        std::stringstream fields(path);
        std::string delay;
        std::string power;
        std::string phase;
        std::string doppler;
        std::string extra;
        if (!std::getline(fields, delay, ':') ||
            !std::getline(fields, power, ':') ||
            !std::getline(fields, phase, ':')) {
            throw std::invalid_argument(
                "TDL must use delay:power_db:phase_deg[:doppler_hz] joined with +");
        }
        const bool has_doppler = static_cast<bool>(std::getline(fields, doppler, ':'));
        if (std::getline(fields, extra, ':')) {
            throw std::invalid_argument(
                "TDL must use delay:power_db:phase_deg[:doppler_hz] joined with +");
        }
        taps.push_back({
            parse_size(delay, "TDL delay"),
            parse_float(power, "TDL power"),
            parse_float(phase, "TDL phase"),
            has_doppler ? parse_float(doppler, "TDL Doppler") : 0.0f});
    }
    if (taps.empty()) {
        throw std::invalid_argument("TDL must contain at least one path");
    }
    return taps;
}

void print_usage() {
    std::cout
        << "OpenISAC Windows VLC/UDP channel simulator\n\n"
        << "Usage:\n"
        << "  openisac_phy_video_bridge.exe [options]\n\n"
        << "Options:\n"
        << "  --listen-address ADDR   UDP input bind address (127.0.0.1)\n"
        << "  --listen-port PORT      VLC sender input port (50000)\n"
        << "  --output-address ADDR   VLC receiver destination (127.0.0.1)\n"
        << "  --output-port PORT      VLC receiver destination port (50001)\n"
        << "  --snr DB                channel SNR (45)\n"
        << "  --cfo HZ                carrier offset (300)\n"
        << "  --sfo PPM               sample-rate offset (20)\n"
        << "  --timing SAMPLES        frame timing offset (20)\n"
        << "  --tdl SPEC              delay:power_db:phase_deg[:doppler_hz]+...\n"
        << "  --rank N                spatial rank: 1, 2 or 4 (2)\n"
        << "  --modulation NAME       QPSK, 16QAM, 64QAM or 256QAM (64QAM)\n"
        << "  --pilot-mode NAME       fdm or nr-dmrs (fdm)\n"
        << "  --fft N                 formal FFT size; currently 1024\n"
        << "  --cp N                  formal CP length; currently 128\n"
        << "  --subcarrier-spacing HZ formal spacing; currently 15000\n"
        << "  --center-frequency HZ   sensing display center frequency (5.8e9)\n"
        << "  --tx-correlation RHO    transmit spatial correlation, |rho|<1 (0.2)\n"
        << "  --rx-correlation RHO    receive spatial correlation, |rho|<1 (0.2)\n"
        << "  --mmse-scale VALUE      Rank-4 MMSE loading scale (0.5)\n"
        << "  --spatial-seed N        repeatable MIMO spatial channel seed\n"
        << "  --workers N             LDPC worker threads (8)\n"
        << "  --queue-packets N       UDP ingress queue capacity (8192)\n"
        << "  --seed N                deterministic channel base seed\n"
        << "  --telemetry-dir PATH    write live plot snapshots to PATH\n"
        << "  --telemetry-interval S  snapshot interval in seconds (2)\n"
        << "  --telemetry-points N    maximum time samples per snapshot (4096)\n"
        << "  --sensing-coherent N    coherent frames; 0 disables sensing (128)\n"
        << "  --sensing-range-bins N  exported non-negative range bins (128)\n"
        << "  --self-test [N]         run N packet PHY loopback test (12)\n"
        << "  --help                  show this message\n";
}

Options parse_options(int argc, char** argv) {
    Options result;
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        auto value = [&](const char* name) -> std::string {
            if (++index >= argc) {
                throw std::invalid_argument(std::string(name) + " needs a value");
            }
            return argv[index];
        };
        if (option == "--help" || option == "-h") {
            print_usage();
            std::exit(0);
        } else if (option == "--listen-address") {
            result.listen_address = value("--listen-address");
        } else if (option == "--listen-port") {
            result.listen_port = parse_unsigned(value("--listen-port"), "listen port");
        } else if (option == "--output-address") {
            result.output_address = value("--output-address");
        } else if (option == "--output-port") {
            result.output_port = parse_unsigned(value("--output-port"), "output port");
        } else if (option == "--snr") {
            result.snr_db = parse_float(value("--snr"), "SNR");
        } else if (option == "--cfo") {
            result.cfo_hz = parse_float(value("--cfo"), "CFO");
        } else if (option == "--sfo") {
            result.sfo_ppm = parse_float(value("--sfo"), "SFO");
        } else if (option == "--timing") {
            result.timing_offset = parse_size(value("--timing"), "timing offset");
        } else if (option == "--tdl") {
            result.taps = parse_tdl(value("--tdl"));
        } else if (option == "--rank") {
            result.transmit_rank = parse_unsigned(value("--rank"), "rank");
        } else if (option == "--modulation") {
            result.modulation = parse_modulation(value("--modulation"));
        } else if (option == "--pilot-mode") {
            result.pilot_mode = parse_pilot_mode(value("--pilot-mode"));
        } else if (option == "--fft") {
            result.fft_size = parse_size(value("--fft"), "FFT size");
        } else if (option == "--cp") {
            result.cp_length = parse_size(value("--cp"), "CP length");
        } else if (option == "--subcarrier-spacing") {
            result.subcarrier_spacing_hz =
                parse_float(value("--subcarrier-spacing"), "subcarrier spacing");
        } else if (option == "--center-frequency") {
            result.center_frequency_hz =
                parse_float(value("--center-frequency"), "center frequency");
        } else if (option == "--tx-correlation") {
            result.transmit_spatial_correlation =
                parse_float(value("--tx-correlation"), "Tx correlation");
        } else if (option == "--rx-correlation") {
            result.receive_spatial_correlation =
                parse_float(value("--rx-correlation"), "Rx correlation");
        } else if (option == "--mmse-scale") {
            result.rank4_mmse_regularization_scale =
                parse_float(value("--mmse-scale"), "MMSE scale");
        } else if (option == "--spatial-seed") {
            result.spatial_channel_seed =
                parse_unsigned(value("--spatial-seed"), "spatial seed");
        } else if (option == "--workers") {
            result.ldpc_workers = parse_size(value("--workers"), "worker count");
        } else if (option == "--queue-packets") {
            result.ingress_queue_packets =
                parse_size(value("--queue-packets"), "queue packet count");
        } else if (option == "--seed") {
            result.random_seed = parse_unsigned(value("--seed"), "seed");
        } else if (option == "--telemetry-dir") {
            result.telemetry_directory = value("--telemetry-dir");
        } else if (option == "--telemetry-interval") {
            result.telemetry_interval_seconds =
                parse_float(value("--telemetry-interval"), "telemetry interval");
        } else if (option == "--telemetry-points") {
            result.telemetry_waveform_points =
                parse_size(value("--telemetry-points"), "telemetry points");
        } else if (option == "--sensing-coherent") {
            result.sensing_coherent_frames =
                parse_size(value("--sensing-coherent"), "sensing coherent frames");
        } else if (option == "--sensing-range-bins") {
            result.sensing_export_range_bins =
                parse_size(value("--sensing-range-bins"), "sensing range bins");
        } else if (option == "--self-test") {
            result.self_test = true;
            if (index + 1 < argc && argv[index + 1][0] != '-') {
                result.self_test_packets = parse_size(argv[++index], "self-test packets");
            }
        } else {
            throw std::invalid_argument("unknown option: " + option);
        }
    }
    if (result.fft_size != 1024u || result.cp_length != 128u ||
        std::abs(result.subcarrier_spacing_hz - 15000.0f) > 0.1f) {
        throw std::invalid_argument(
            "formal video PHY currently requires FFT=1024, CP=128 and spacing=15000 Hz");
    }
    if (result.listen_port == 0u || result.listen_port > 65535u ||
        result.output_port == 0u || result.output_port > 65535u ||
        result.ldpc_workers == 0u || result.ldpc_workers > 19u ||
        result.ingress_queue_packets == 0u || result.ingress_queue_packets > 65536u ||
        result.timing_offset > 128u || result.self_test_packets == 0u ||
        (result.transmit_rank != 1u && result.transmit_rank != 2u &&
         result.transmit_rank != 4u) ||
        result.center_frequency_hz <= 0.0f ||
        std::abs(result.transmit_spatial_correlation) >= 1.0f ||
        std::abs(result.receive_spatial_correlation) >= 1.0f ||
        !std::isfinite(result.rank4_mmse_regularization_scale) ||
        result.rank4_mmse_regularization_scale <= 0.0f ||
        result.rank4_mmse_regularization_scale > 16.0f ||
        result.telemetry_interval_seconds < 0.2f ||
        result.telemetry_waveform_points == 0u ||
        result.sensing_coherent_frames > 512u ||
        (result.sensing_coherent_frames != 0u &&
         (result.sensing_coherent_frames < 16u ||
          (result.sensing_coherent_frames & (result.sensing_coherent_frames - 1u)) != 0u)) ||
        result.sensing_export_range_bins == 0u ||
        result.sensing_export_range_bins > result.fft_size / 2u) {
        throw std::invalid_argument("port/worker/timing/test count is out of range");
    }
    return result;
}

void put_u16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value & 0xFFu);
    bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
}

void put_u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned byte = 0u; byte < 4u; ++byte) {
        bytes[offset + byte] = static_cast<std::uint8_t>(value >> (8u * byte));
    }
}

std::uint16_t get_u16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(bytes[offset + 1u] << 8u);
}

std::uint32_t get_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint32_t result = 0u;
    for (unsigned byte = 0u; byte < 4u; ++byte) {
        result |= static_cast<std::uint32_t>(bytes[offset + byte]) << (8u * byte);
    }
    return result;
}

struct BridgeCounters {
    std::uint64_t ingress_socket_packets = 0u;
    std::uint64_t ingress_queue_drops = 0u;
    std::size_t ingress_queue_depth = 0u;
    std::size_t ingress_queue_high_watermark = 0u;
    std::uint64_t udp_packets_in = 0u;
    std::uint64_t udp_bytes_in = 0u;
    std::uint64_t phy_frames = 0u;
    std::uint64_t phy_crc_failures = 0u;
    std::uint64_t malformed_fragments = 0u;
    std::uint64_t udp_packets_out = 0u;
    std::uint64_t udp_bytes_out = 0u;
    std::uint64_t dropped_packets = 0u;
    double phy_latency_us = 0.0;
};

openisac::DynamicSensingConfig make_sensing_config(const Options& options) {
    openisac::DynamicSensingConfig config;
    config.fft_size = options.fft_size;
    config.transmit_ports = options.transmit_rank == 4u ? 4u : 2u;
    config.receive_ports = options.transmit_rank == 4u ? 4u : 2u;
    config.coherent_frames = options.sensing_coherent_frames;
    config.range_fft_size = options.fft_size;
    config.doppler_fft_size = options.sensing_coherent_frames;
    config.subcarrier_spacing_hz = options.subcarrier_spacing_hz;
    config.center_frequency_hz = options.center_frequency_hz;
    config.frame_period_seconds = static_cast<float>(
        openisac::formal_frame_period_seconds(options.pilot_mode));
    config.maximum_range_bin = options.sensing_export_range_bins;
    config.enable_static_clutter_suppression = true;
    config.doppler_dc_exclusion_bins = 1u;
    config.enable_cfar_detection = true;
    config.cfar_false_alarm_probability = 1.0e-7f;
    return config;
}

double frame_period_seconds(const Options& options) {
    return openisac::formal_frame_period_seconds(options.pilot_mode);
}

std::uint64_t frame_timestamp_ns(
    std::uint64_t frame_id,
    const Options& options) {
    return static_cast<std::uint64_t>(std::llround(
        static_cast<double>(frame_id) * frame_period_seconds(options) * 1.0e9));
}

float condition_number_2x2(const openisac::Channel2x2& channel) {
    const double first = std::norm(channel.h00) + std::norm(channel.h10);
    const double second = std::norm(channel.h01) + std::norm(channel.h11);
    const auto cross = std::conj(channel.h00) * channel.h01 +
        std::conj(channel.h10) * channel.h11;
    const double discriminant = std::sqrt(std::max(
        0.0, (first - second) * (first - second) +
            4.0 * static_cast<double>(std::norm(cross))));
    const double largest = 0.5 * (first + second + discriminant);
    const double smallest = 0.5 * (first + second - discriminant);
    if (!(largest > 0.0) ||
        smallest <= std::max(1.0e-12, largest * 1.0e-10)) {
        return 1.0e6f;
    }
    return static_cast<float>(std::sqrt(largest / smallest));
}

class VideoPhyChannel {
public:
    VideoPhyChannel(const Options& options, const openisac::Ldpc5041008& codec)
        : options_(options), codec_(codec),
          mode_{options.transmit_rank, options.modulation} {
        openisac::FormalFrameProfile profile;
        profile.transmit_rank = mode_.rank;
        profile.bits_per_symbol = openisac::modulation_bits(mode_.modulation);
        if (mode_.rank == 4u) {
            profile.pilot_spacing = 2u;
            rank4_pipeline_ = std::make_unique<openisac::Rank4TimePipeline>(
                codec_, options_.ldpc_workers);
        } else {
            dynamic_pipeline_ = std::make_unique<openisac::DynamicLinkPipeline>(
                codec_, options_.ldpc_workers);
        }
        const auto layout = openisac::build_formal_frame_layout(profile);
        phy_payload_capacity_ = layout.user_payload_bytes;
        data_fft_indices_ = layout.data_fft_indices;
        if (phy_payload_capacity_ <= fragment_header_bytes) {
            throw std::runtime_error("PHY payload is too small for video fragments");
        }
        fragment_payload_capacity_ = phy_payload_capacity_ - fragment_header_bytes;
        if (!options_.telemetry_directory.empty() &&
            options_.sensing_coherent_frames != 0u) {
            sensing_ = std::make_unique<openisac::DynamicSensingProcessor>(
                make_sensing_config(options_));
        }
    }

    std::optional<std::vector<std::uint8_t>> process(
        const std::vector<std::uint8_t>& datagram,
        std::uint32_t packet_sequence) {
        ++counters_.udp_packets_in;
        counters_.udp_bytes_in += datagram.size();
        if (datagram.empty() || datagram.size() > options_.maximum_datagram_bytes ||
            datagram.size() > maximum_udp_payload) {
            ++counters_.dropped_packets;
            return std::nullopt;
        }
        const std::size_t fragment_count =
            (datagram.size() + fragment_payload_capacity_ - 1u) /
            fragment_payload_capacity_;
        if (fragment_count > std::numeric_limits<std::uint16_t>::max()) {
            ++counters_.dropped_packets;
            return std::nullopt;
        }

        struct CompletedPhyFrame {
            bool crc_ok = false;
            std::vector<std::uint8_t> payload;
            double latency_us = 0.0;
        };
        auto receive_frame = [&]() {
            CompletedPhyFrame completed;
            if (mode_.rank == 4u) {
                auto result = rank4_pipeline_->receive();
                if (!result.link.sensing_channel_frequency_response.empty()) {
                    try {
                        process_rank4_telemetry(result);
                    } catch (const std::exception& error) {
                        std::cerr << "WARNING: Rank-4 live telemetry snapshot failed: "
                                  << error.what() << '\n';
                        if (sensing_ != nullptr) {
                            sensing_->reset();
                        }
                        rank4_sensing_batch_in_flight_ = false;
                        last_telemetry_ = std::chrono::steady_clock::now();
                    }
                }
                completed.crc_ok = result.link.crc_ok;
                completed.payload = std::move(result.link.user_payload);
                completed.latency_us = result.timing.latency_us;
            } else {
                auto result = dynamic_pipeline_->receive();
                completed.crc_ok = result.link.crc_ok;
                completed.payload = std::move(result.link.user_payload);
                completed.latency_us = result.timing.latency_us;
            }
            return completed;
        };
        const std::size_t pipeline_slots = mode_.rank == 4u
            ? rank4_pipeline_->slot_count()
            : dynamic_pipeline_->slot_count();
        std::size_t in_flight = 0u;
        std::vector<CompletedPhyFrame> results;
        results.reserve(fragment_count);
        for (std::size_t fragment = 0u; fragment < fragment_count; ++fragment) {
            if (in_flight >= pipeline_slots) {
                results.push_back(receive_frame());
                --in_flight;
            }
            const std::size_t offset = fragment * fragment_payload_capacity_;
            const std::size_t count = std::min(
                fragment_payload_capacity_, datagram.size() - offset);
            std::vector<std::uint8_t> phy_payload(fragment_header_bytes + count, 0u);
            put_u32(phy_payload, 0u, fragment_magic);
            phy_payload[4u] = fragment_version;
            phy_payload[5u] = 0u;
            put_u16(phy_payload, 6u, static_cast<std::uint16_t>(fragment_header_bytes));
            put_u32(phy_payload, 8u, packet_sequence);
            put_u16(phy_payload, 12u, static_cast<std::uint16_t>(fragment));
            put_u16(phy_payload, 14u, static_cast<std::uint16_t>(fragment_count));
            put_u16(phy_payload, 16u, static_cast<std::uint16_t>(datagram.size()));
            put_u16(phy_payload, 18u, static_cast<std::uint16_t>(count));
            std::copy(
                datagram.begin() + static_cast<std::ptrdiff_t>(offset),
                datagram.begin() + static_cast<std::ptrdiff_t>(offset + count),
                phy_payload.begin() + static_cast<std::ptrdiff_t>(fragment_header_bytes));

            const bool time_varying_channel = std::any_of(
                options_.taps.begin(), options_.taps.end(),
                [](const openisac::TdlTap& tap) {
                    return std::abs(tap.doppler_hz) > 1.0e-6f;
                });
            if (mode_.rank == 4u) {
                openisac::Rank4TimeSimulationConfig config;
                config.pilot_mode = options_.pilot_mode;
                config.modulation = mode_.modulation;
                config.snr_db = options_.snr_db;
                config.timing_offset_samples = options_.timing_offset;
                config.cfo_hz = options_.cfo_hz;
                config.sfo_ppm = options_.sfo_ppm;
                config.transmit_correlation =
                    options_.transmit_spatial_correlation;
                config.receive_correlation =
                    options_.receive_spatial_correlation;
                config.csi_smoothing_alpha =
                    time_varying_channel ? 1.0f : 0.35f;
                config.mmse_regularization_scale =
                    options_.rank4_mmse_regularization_scale;
                config.channel_time_seconds =
                    static_cast<double>(frame_id_) * frame_period_seconds(options_);
                config.channel_seed = options_.spatial_channel_seed;
                config.random_seed = options_.random_seed +
                    static_cast<unsigned>(frame_id_ * 2654435761u);
                config.pilot_seed = 0xC057u;
                config.taps = options_.taps;
                const bool telemetry_requested = telemetry_due();
                if (telemetry_requested && sensing_ != nullptr &&
                    !rank4_sensing_batch_in_flight_) {
                    sensing_->reset();
                    rank4_sensing_batch_in_flight_ = true;
                    rank4_sensing_capture_remaining_ =
                        options_.sensing_coherent_frames;
                    std::cout << "Rank-4 sensing capture started: "
                              << rank4_sensing_capture_remaining_
                              << " coherent frames\n";
                }
                const bool telemetry_capture = sensing_ != nullptr
                    ? rank4_sensing_capture_remaining_ > 0u
                    : telemetry_requested;
                if (telemetry_capture && sensing_ != nullptr) {
                    --rank4_sensing_capture_remaining_;
                }
                config.enable_sensing_snapshot = telemetry_capture;
                config.diagnostic_waveform_points =
                    options_.telemetry_waveform_points;
                rank4_pipeline_->submit_payload(
                    frame_id_, phy_payload,
                    static_cast<std::uint16_t>(frame_id_), config);
            } else {
                openisac::DynamicLinkSimulationConfig config;
                config.pilot_mode = options_.pilot_mode;
                config.snr_db = options_.snr_db;
                config.timing_offset_samples = options_.timing_offset;
                config.cfo_hz = options_.cfo_hz;
                config.sfo_ppm = options_.sfo_ppm;
                config.random_seed = options_.random_seed +
                    static_cast<unsigned>(frame_id_ * 2654435761u);
                config.pilot_seed = 0xC057u;
                config.enable_correlated_spatial_tdl = true;
                config.transmit_spatial_correlation =
                    options_.transmit_spatial_correlation;
                config.receive_spatial_correlation =
                    options_.receive_spatial_correlation;
                config.spatial_channel_seed = options_.spatial_channel_seed;
                // A moving path invalidates stale CSI; current-frame CSI is
                // both cheaper and more robust than temporal smoothing.
                config.csi_smoothing_alpha =
                    time_varying_channel ? 1.0f : 0.35f;
                config.channel_time_seconds =
                    static_cast<double>(frame_id_) * frame_period_seconds(options_);
                const bool telemetry_requested = telemetry_due();
                const bool sensing_batch_active = sensing_ != nullptr &&
                    sensing_->frames_accumulated() > 0u;
                const bool telemetry_capture =
                    telemetry_requested || sensing_batch_active;
                if (telemetry_requested && sensing_ != nullptr &&
                    !sensing_batch_active) {
                    sensing_->reset();
                    telemetry_receiver_state_.reset();
                }
                const bool final_sensing_frame = sensing_ != nullptr &&
                    telemetry_capture &&
                    sensing_->frames_accumulated() + 1u >=
                        options_.sensing_coherent_frames;
                config.enable_truth_diagnostics = telemetry_capture &&
                    (sensing_ == nullptr || final_sensing_frame);
                config.taps = options_.taps;

                openisac::DynamicLinkIqFrame iq;
                openisac::generate_dynamic_tdl_iq_frame(
                    phy_payload, mode_, static_cast<std::uint16_t>(frame_id_),
                    codec_, config, iq, generation_workspace_);
                if (telemetry_capture) {
                    try {
                        process_telemetry(iq);
                    } catch (const std::exception& error) {
                        std::cerr << "WARNING: live telemetry snapshot failed: "
                                  << error.what() << '\n';
                        if (sensing_ != nullptr) {
                            sensing_->reset();
                        }
                        last_telemetry_ = std::chrono::steady_clock::now();
                    }
                }
                dynamic_pipeline_->submit_iq(frame_id_, iq);
            }
            ++frame_id_;
            ++in_flight;
            ++counters_.phy_frames;
        }
        while (in_flight > 0u) {
            results.push_back(receive_frame());
            --in_flight;
        }

        std::vector<std::uint8_t> reconstructed(datagram.size(), 0u);
        std::vector<bool> received(fragment_count, false);
        bool valid = true;
        for (const auto& result : results) {
            counters_.phy_latency_us += result.latency_us;
            if (!result.crc_ok) {
                ++counters_.phy_crc_failures;
                valid = false;
                continue;
            }
            const auto& payload = result.payload;
            if (payload.size() < fragment_header_bytes ||
                get_u32(payload, 0u) != fragment_magic ||
                payload[4u] != fragment_version ||
                get_u16(payload, 6u) != fragment_header_bytes ||
                get_u32(payload, 8u) != packet_sequence ||
                get_u16(payload, 14u) != fragment_count ||
                get_u16(payload, 16u) != datagram.size()) {
                ++counters_.malformed_fragments;
                valid = false;
                continue;
            }
            const std::size_t fragment = get_u16(payload, 12u);
            const std::size_t count = get_u16(payload, 18u);
            const std::size_t offset = fragment * fragment_payload_capacity_;
            if (fragment >= fragment_count || received[fragment] ||
                payload.size() != fragment_header_bytes + count ||
                offset + count > reconstructed.size()) {
                ++counters_.malformed_fragments;
                valid = false;
                continue;
            }
            std::copy(
                payload.begin() + static_cast<std::ptrdiff_t>(fragment_header_bytes),
                payload.end(),
                reconstructed.begin() + static_cast<std::ptrdiff_t>(offset));
            received[fragment] = true;
        }
        valid = valid && std::all_of(received.begin(), received.end(),
                                     [](bool value) { return value; });
        if (!valid) {
            ++counters_.dropped_packets;
            return std::nullopt;
        }
        ++counters_.udp_packets_out;
        counters_.udp_bytes_out += reconstructed.size();
        return reconstructed;
    }

    std::size_t phy_payload_capacity() const noexcept { return phy_payload_capacity_; }
    std::size_t fragment_payload_capacity() const noexcept {
        return fragment_payload_capacity_;
    }
    const BridgeCounters& counters() const noexcept { return counters_; }
    void update_ingress_statistics(
        std::uint64_t socket_packets,
        std::uint64_t queue_drops,
        std::size_t queue_depth,
        std::size_t queue_high_watermark) noexcept {
        counters_.ingress_socket_packets = socket_packets;
        counters_.ingress_queue_drops = queue_drops;
        counters_.ingress_queue_depth = queue_depth;
        counters_.ingress_queue_high_watermark = queue_high_watermark;
    }

private:
    bool telemetry_due() const {
        return !options_.telemetry_directory.empty() &&
            (last_telemetry_.time_since_epoch().count() == 0 ||
             std::chrono::duration<float>(
                 std::chrono::steady_clock::now() - last_telemetry_).count() >=
                 options_.telemetry_interval_seconds);
    }

    void process_rank4_telemetry(
        const openisac::Rank4TimePipelineResult& pipeline_result) {
        const openisac::DynamicSensingResult* sensing_result = nullptr;
        if (sensing_ != nullptr) {
            const bool ready = sensing_->push_channel_frame(
                pipeline_result.frame_id,
                frame_timestamp_ns(pipeline_result.frame_id, options_),
                pipeline_result.link.sensing_channel_frequency_response,
                pipeline_result.link.sensing_active_subcarrier_mask);
            if (!ready) {
                return;
            }
            std::cout << "Rank-4 sensing capture complete: frame "
                      << pipeline_result.frame_id << '\n';
            rank4_sensing_batch_in_flight_ = false;
            sensing_result = &sensing_->last_result();
        }
        write_rank4_telemetry(
            pipeline_result.frame_id, pipeline_result.link, sensing_result);
    }

    void write_rank4_telemetry(
        std::uint64_t capture_frame_id,
        const openisac::Rank4TimeSimulationResult& diagnostic,
        const openisac::DynamicSensingResult* sensing_result) {
        const std::filesystem::path directory(options_.telemetry_directory);
        std::filesystem::create_directories(directory);

        std::ofstream waveform(directory / "waveform.csv");
        waveform << "sample,tx0_i,tx0_q,rx0_i,rx0_q\n"
                 << std::setprecision(9);
        for (std::size_t sample = 0u;
             sample < diagnostic.receive_waveform_rx0.size(); ++sample) {
            const auto value = diagnostic.receive_waveform_rx0[sample];
            waveform << sample << ",0,0," << value.real() << ','
                     << value.imag() << '\n';
        }

        std::ofstream constellation(directory / "constellation.csv");
        constellation << "index,layer,ideal_i,ideal_q,equalized_i,equalized_q\n"
                      << std::setprecision(9);
        const std::size_t information_bytes = diagnostic.payload_bytes + 2u;
        const std::size_t payload_blocks = (information_bytes + 62u) / 63u;
        const std::size_t coded_symbols = payload_blocks * 1008u /
            openisac::modulation_bits(mode_.modulation);
        const std::size_t available_symbols = std::min(
            diagnostic.transmitted_symbols.size(),
            diagnostic.equalized_symbols.size());
        const std::size_t symbols = std::min(available_symbols, coded_symbols);
        const std::size_t padding_symbols = available_symbols - symbols;
        for (std::size_t index = 0u; index < symbols; ++index) {
            const auto ideal = diagnostic.transmitted_symbols[index];
            const auto equalized = diagnostic.equalized_symbols[index];
            constellation << index << ',' << index % 4u << ','
                          << ideal.real() << ',' << ideal.imag() << ','
                          << equalized.real() << ',' << equalized.imag() << '\n';
        }

        std::ofstream channel(directory / "channel.csv");
        channel << "fft";
        for (std::size_t rx = 0u; rx < 4u; ++rx) {
            for (std::size_t tx = 0u; tx < 4u; ++tx) {
                channel << ",h" << rx << tx << "_i,h" << rx << tx << "_q";
            }
        }
        channel << '\n' << std::setprecision(9);
        for (std::size_t fft = 0u; fft < options_.fft_size; ++fft) {
            channel << fft;
            for (std::size_t link = 0u; link < 16u; ++link) {
                const auto value = diagnostic.sensing_channel_frequency_response[
                    link * options_.fft_size + fft];
                channel << ',' << value.real() << ',' << value.imag();
            }
            channel << '\n';
        }
        waveform.close();
        constellation.close();
        channel.close();
        if (!waveform || !constellation || !channel) {
            throw std::runtime_error(
                "cannot write Rank-4 telemetry waveform/channel data");
        }

        std::size_t reported_sensing_detections = 0u;
        constexpr float sensing_display_floor_db = -30.0f;
        if (sensing_result != nullptr) {
            const auto& sensing_config = sensing_->config();
            const std::size_t range_bins = std::min(
                options_.sensing_export_range_bins,
                sensing_config.range_fft_size / 2u);
            float maximum_power = 0.0f;
            for (std::size_t doppler = 0u;
                 doppler < sensing_config.doppler_fft_size; ++doppler) {
                for (std::size_t range = 0u; range < range_bins; ++range) {
                    maximum_power = std::max(
                        maximum_power,
                        std::norm(sensing_result->range_doppler_map[
                            doppler * sensing_config.range_fft_size + range]));
                }
            }
            std::ofstream range_doppler(directory / "sensing_range_doppler.csv");
            range_doppler
                << "doppler_bin,range_bin,range_m,velocity_mps,relative_power_db\n"
                << std::setprecision(9);
            const std::size_t dc = sensing_config.doppler_fft_size / 2u;
            for (std::size_t doppler = 0u;
                 doppler < sensing_config.doppler_fft_size; ++doppler) {
                const int centered = static_cast<int>(doppler) -
                    static_cast<int>(dc);
                for (std::size_t range = 0u; range < range_bins; ++range) {
                    const float power = std::norm(
                        sensing_result->range_doppler_map[
                            doppler * sensing_config.range_fft_size + range]);
                    const float relative_db = 10.0f * std::log10(
                        std::max(power, 1.0e-30f) /
                        std::max(maximum_power, 1.0e-30f));
                    range_doppler << doppler << ',' << range << ','
                                  << range * sensing_result->range_bin_spacing_m
                                  << ','
                                  << centered *
                                      sensing_result->velocity_bin_spacing_mps
                                  << ',' << relative_db << '\n';
                }
            }
            std::ofstream detections(directory / "sensing_detections.csv");
            detections
                << "range_bin,doppler_bin,range_m,doppler_hz,velocity_mps,"
                   "relative_power_db,power_over_threshold_db\n"
                << std::setprecision(9);
            for (const auto& detection : sensing_result->detections) {
                const float relative_db = 10.0f * std::log10(
                    std::max(detection.peak.power, 1.0e-30f) /
                    std::max(maximum_power, 1.0e-30f));
                if (relative_db < sensing_display_floor_db) {
                    continue;
                }
                detections << detection.peak.range_bin << ','
                           << detection.peak.doppler_bin << ','
                           << detection.peak.range_m << ','
                           << detection.peak.doppler_hz << ','
                           << detection.peak.velocity_mps << ','
                           << relative_db << ','
                           << detection.power_over_threshold_db << '\n';
                ++reported_sensing_detections;
            }
            range_doppler.close();
            detections.close();
            if (!range_doppler || !detections) {
                throw std::runtime_error(
                    "cannot write Rank-4 sensing telemetry data");
            }
        }

        std::vector<float> condition_numbers;
        condition_numbers.reserve(options_.fft_size);
        if (diagnostic.sensing_active_subcarrier_mask.size() ==
                options_.fft_size &&
            diagnostic.sensing_channel_frequency_response.size() ==
                16u * options_.fft_size) {
            for (std::size_t fft = 0u; fft < options_.fft_size; ++fft) {
                if (diagnostic.sensing_active_subcarrier_mask[fft] == 0u) {
                    continue;
                }
                openisac::ChannelNxN channel;
                channel.streams = 4u;
                for (std::size_t rx = 0u; rx < 4u; ++rx) {
                    for (std::size_t tx = 0u; tx < 4u; ++tx) {
                        const std::size_t link = rx * 4u + tx;
                        channel.values[
                            rx * openisac::maximum_spatial_streams + tx] =
                            diagnostic.sensing_channel_frequency_response[
                                link * options_.fft_size + fft];
                    }
                }
                condition_numbers.push_back(
                    openisac::condition_number_nxn(channel));
            }
        }
        std::sort(condition_numbers.begin(), condition_numbers.end());
        const float condition_median = condition_numbers.empty()
            ? 0.0f : condition_numbers[condition_numbers.size() / 2u];
        const float condition_p90 = condition_numbers.empty()
            ? 0.0f : condition_numbers[std::min(
                condition_numbers.size() - 1u,
                condition_numbers.size() * 9u / 10u)];
        const std::size_t ill_conditioned_subcarriers =
            static_cast<std::size_t>(std::count_if(
                condition_numbers.begin(), condition_numbers.end(),
                [](float value) { return value > 10.0f; }));
        const double ill_conditioned_percent = condition_numbers.empty()
            ? 0.0 : 100.0 * static_cast<double>(ill_conditioned_subcarriers) /
                static_cast<double>(condition_numbers.size());

        const auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const double fer_percent = counters_.phy_frames > 0u
            ? 100.0 * static_cast<double>(counters_.phy_crc_failures) /
                  static_cast<double>(counters_.phy_frames)
            : 0.0;
        std::ofstream status(directory / "status.csv");
        status << "metric,value\n" << std::setprecision(12)
               << "snapshot_epoch_ms," << epoch_ms << '\n'
               << "frame_id," << capture_frame_id << '\n'
               << "rank,4\n"
               << "modulation," << openisac::modulation_name(mode_.modulation) << '\n'
               << "pilot_mode," << openisac::pilot_mode_name(options_.pilot_mode) << '\n'
               << "frame_symbols," << openisac::formal_frame_symbols(options_.pilot_mode) << '\n'
               << "frame_period_us,"
               << frame_period_seconds(options_) * 1.0e6 << '\n'
               << "fft_size," << options_.fft_size << '\n'
               << "cp_length," << options_.cp_length << '\n'
               << "subcarrier_spacing_hz," << options_.subcarrier_spacing_hz << '\n'
               << "sample_rate_hz,"
               << options_.fft_size * options_.subcarrier_spacing_hz << '\n'
               << "center_frequency_hz," << options_.center_frequency_hz << '\n'
               << "tx_spatial_correlation,"
               << options_.transmit_spatial_correlation << '\n'
               << "rx_spatial_correlation,"
               << options_.receive_spatial_correlation << '\n'
               << "spatial_channel_seed," << options_.spatial_channel_seed << '\n'
               << "channel_condition_median," << condition_median << '\n'
               << "channel_condition_p90," << condition_p90 << '\n'
               << "ill_conditioned_subcarriers,"
               << ill_conditioned_subcarriers << '\n'
               << "ill_conditioned_subcarrier_percent,"
               << ill_conditioned_percent << '\n'
               << "sensing_link_count,16\n"
               << "snr_db," << options_.snr_db << '\n'
               << "constellation_valid_symbols," << symbols << '\n'
               << "constellation_padding_symbols_excluded,"
               << padding_symbols << '\n'
               << "evm_percent," << diagnostic.evm_percent << '\n'
               << "channel_nmse_db," << diagnostic.channel_nmse_db << '\n'
               << "cfo_true_hz," << options_.cfo_hz << '\n'
               << "cfo_estimated_hz," << diagnostic.estimated_cfo_hz << '\n'
               << "cfo_error_hz," << diagnostic.cfo_error_hz << '\n'
               << "sfo_true_ppm," << options_.sfo_ppm << '\n'
               << "sfo_residual_ppm," << diagnostic.residual_sfo_ppm << '\n'
               << "timing_true_samples," << options_.timing_offset << '\n'
               << "timing_estimated_samples," << diagnostic.timing_offset << '\n'
               << "timing_metric," << diagnostic.timing_metric << '\n'
               << "noise_variance," << diagnostic.noise_variance << '\n'
               << "mmse_regularization_scale,"
               << options_.rank4_mmse_regularization_scale << '\n'
               << "ingress_socket_packets," << counters_.ingress_socket_packets << '\n'
               << "ingress_queue_drops," << counters_.ingress_queue_drops << '\n'
               << "ingress_queue_depth," << counters_.ingress_queue_depth << '\n'
               << "ingress_queue_high_watermark,"
               << counters_.ingress_queue_high_watermark << '\n'
               << "udp_packets_in," << counters_.udp_packets_in << '\n'
               << "udp_packets_out," << counters_.udp_packets_out << '\n'
               << "udp_dropped," << counters_.dropped_packets << '\n'
               << "phy_frames," << counters_.phy_frames << '\n'
               << "fer_percent," << fer_percent << '\n';
        if (sensing_result != nullptr) {
            status << "sensing_ready,1\n"
                   << "sensing_coherent_frames," << sensing_result->coherent_frames << '\n'
                   << "sensing_range_spacing_m,"
                   << sensing_result->range_bin_spacing_m << '\n'
                   << "sensing_doppler_spacing_hz,"
                   << sensing_result->doppler_bin_spacing_hz << '\n'
                   << "sensing_velocity_spacing_mps,"
                   << sensing_result->velocity_bin_spacing_mps << '\n'
                   << "sensing_peak_range_m,"
                   << sensing_result->strongest_peak.range_m << '\n'
                   << "sensing_peak_doppler_hz,"
                   << sensing_result->strongest_peak.doppler_hz << '\n'
                   << "sensing_peak_velocity_mps,"
                   << sensing_result->strongest_peak.velocity_mps << '\n'
                   << "sensing_detection_count,"
                   << reported_sensing_detections << '\n'
                   << "sensing_raw_cfar_detection_count,"
                   << sensing_result->detections.size() << '\n'
                   << "sensing_display_floor_db," << sensing_display_floor_db << '\n'
                   << "sensing_cfar_cells," << sensing_result->cfar_cells_tested << '\n';
        } else {
            status << "sensing_ready,0\n";
        }
        status.close();
        if (!status) {
            throw std::runtime_error("cannot write Rank-4 telemetry status data");
        }
        last_telemetry_ = std::chrono::steady_clock::now();
    }

    void process_telemetry(const openisac::DynamicLinkIqFrame& iq) {
        openisac::PreparedDynamicLinkFrame prepared;
        openisac::prepare_dynamic_iq_frame(
            iq, prepared, &telemetry_receiver_state_, telemetry_workspace_);
        const openisac::DynamicSensingResult* sensing_result = nullptr;
        if (sensing_ != nullptr) {
            const bool ready = sensing_->push_frame(
                frame_id_, frame_timestamp_ns(frame_id_, options_),
                iq.transmit_reference_grid, telemetry_workspace_.rx_grid);
            if (!ready) {
                return;
            }
            sensing_result = &sensing_->last_result();
        }
        write_telemetry(iq, prepared, sensing_result);
    }

    void write_telemetry(
        const openisac::DynamicLinkIqFrame& iq,
        const openisac::PreparedDynamicLinkFrame& prepared,
        const openisac::DynamicSensingResult* sensing_result) {
        const auto& diagnostic = prepared.result;
        const std::filesystem::path directory(options_.telemetry_directory);
        std::filesystem::create_directories(directory);

        std::ofstream waveform(directory / "waveform.csv");
        waveform << "sample,tx0_i,tx0_q,rx0_i,rx0_q\n" << std::setprecision(9);
        const std::size_t available = iq.samples.empty() ? 0u : iq.samples[0].size();
        const std::size_t count = std::min(available, options_.telemetry_waveform_points);
        for (std::size_t sample = 0u; sample < count; ++sample) {
            std::complex<float> tx{};
            if (sample >= options_.timing_offset) {
                const std::size_t tx_index = sample - options_.timing_offset;
                if (tx_index < generation_workspace_.tx_time[0].size()) {
                    tx = generation_workspace_.tx_time[0][tx_index];
                }
            }
            const auto rx = iq.samples[0][sample];
            waveform << sample << ',' << tx.real() << ',' << tx.imag() << ','
                     << rx.real() << ',' << rx.imag() << '\n';
        }

        std::ofstream constellation(directory / "constellation.csv");
        constellation << "index,layer,ideal_i,ideal_q,equalized_i,equalized_q\n"
                      << std::setprecision(9);
        const std::size_t available_symbols = std::min(
            iq.truth_payload_symbols.size(), telemetry_workspace_.equalized.size());
        const std::size_t information_bytes = iq.expected_payload.size() + 2u;
        const std::size_t payload_blocks =
            (information_bytes + 62u) / 63u;
        const std::size_t coded_symbols =
            payload_blocks * 1008u /
            openisac::modulation_bits(mode_.modulation);
        // The remaining formal-frame RE are deterministic label-zero padding.
        // Excluding them prevents a misleading pile-up at one outer QAM point.
        const std::size_t symbols = std::min(available_symbols, coded_symbols);
        const std::size_t padding_symbols = available_symbols - symbols;
        for (std::size_t index = 0u; index < symbols; ++index) {
            const auto ideal = iq.truth_payload_symbols[index];
            const auto equalized = telemetry_workspace_.equalized[index];
            constellation << index << ',' << index % mode_.rank << ','
                          << ideal.real() << ',' << ideal.imag() << ','
                          << equalized.real() << ',' << equalized.imag() << '\n';
        }

        const auto& channel_view = telemetry_receiver_state_.csi_valid
            ? telemetry_receiver_state_.filtered_channels
            : telemetry_workspace_.channels;
        std::ofstream channel(directory / "channel.csv");
        channel << "fft,h00_i,h00_q,h01_i,h01_q,h10_i,h10_q,h11_i,h11_q\n"
                << std::setprecision(9);
        for (std::size_t fft = 0u; fft < options_.fft_size; ++fft) {
            const auto& first = channel_view[fft];
            const auto& second = channel_view[options_.fft_size + fft];
            const openisac::Channel2x2 average{
                0.5f * (first.h00 + second.h00),
                0.5f * (first.h01 + second.h01),
                0.5f * (first.h10 + second.h10),
                0.5f * (first.h11 + second.h11)};
            channel << fft << ',' << average.h00.real() << ',' << average.h00.imag()
                    << ',' << average.h01.real() << ',' << average.h01.imag()
                    << ',' << average.h10.real() << ',' << average.h10.imag()
                    << ',' << average.h11.real() << ',' << average.h11.imag() << '\n';
        }
        std::vector<float> condition_numbers;
        condition_numbers.reserve(data_fft_indices_.size());
        for (const auto fft : data_fft_indices_) {
            const auto& first = channel_view[fft];
            const auto& second = channel_view[options_.fft_size + fft];
            condition_numbers.push_back(condition_number_2x2({
                0.5f * (first.h00 + second.h00),
                0.5f * (first.h01 + second.h01),
                0.5f * (first.h10 + second.h10),
                0.5f * (first.h11 + second.h11)}));
        }
        std::sort(condition_numbers.begin(), condition_numbers.end());
        const float condition_median = condition_numbers.empty()
            ? 0.0f : condition_numbers[condition_numbers.size() / 2u];
        const float condition_p90 = condition_numbers.empty()
            ? 0.0f : condition_numbers[std::min(
                condition_numbers.size() - 1u,
                condition_numbers.size() * 9u / 10u)];
        const std::size_t ill_conditioned_subcarriers =
            static_cast<std::size_t>(std::count_if(
                condition_numbers.begin(), condition_numbers.end(),
                [](float value) { return value > 10.0f; }));
        const double ill_conditioned_percent = condition_numbers.empty()
            ? 0.0 : 100.0 * static_cast<double>(ill_conditioned_subcarriers) /
                static_cast<double>(condition_numbers.size());
        waveform.close();
        constellation.close();
        channel.close();
        if (!waveform || !constellation || !channel) {
            throw std::runtime_error("cannot write telemetry waveform/channel data");
        }

        std::size_t reported_sensing_detections = 0u;
        constexpr float sensing_display_floor_db = -30.0f;
        if (sensing_result != nullptr) {
            const auto& sensing_config = sensing_->config();
            const std::size_t range_bins = std::min(
                options_.sensing_export_range_bins,
                sensing_config.range_fft_size / 2u);
            float maximum_power = 0.0f;
            for (std::size_t doppler = 0u;
                 doppler < sensing_config.doppler_fft_size; ++doppler) {
                for (std::size_t range = 0u; range < range_bins; ++range) {
                    maximum_power = std::max(
                        maximum_power,
                        std::norm(sensing_result->range_doppler_map[
                            doppler * sensing_config.range_fft_size + range]));
                }
            }
            std::ofstream range_doppler(directory / "sensing_range_doppler.csv");
            range_doppler
                << "doppler_bin,range_bin,range_m,velocity_mps,relative_power_db\n"
                << std::setprecision(9);
            const std::size_t dc = sensing_config.doppler_fft_size / 2u;
            for (std::size_t doppler = 0u;
                 doppler < sensing_config.doppler_fft_size; ++doppler) {
                const int centered = static_cast<int>(doppler) -
                    static_cast<int>(dc);
                for (std::size_t range = 0u; range < range_bins; ++range) {
                    const float power = std::norm(sensing_result->range_doppler_map[
                        doppler * sensing_config.range_fft_size + range]);
                    const float relative_db = 10.0f * std::log10(
                        std::max(power, 1.0e-30f) /
                        std::max(maximum_power, 1.0e-30f));
                    range_doppler << doppler << ',' << range << ','
                                  << range * sensing_result->range_bin_spacing_m << ','
                                  << centered * sensing_result->velocity_bin_spacing_mps
                                  << ',' << relative_db << '\n';
                }
            }
            std::ofstream detections(directory / "sensing_detections.csv");
            detections
                << "range_bin,doppler_bin,range_m,doppler_hz,velocity_mps,"
                   "relative_power_db,power_over_threshold_db\n"
                << std::setprecision(9);
            for (const auto& detection : sensing_result->detections) {
                const float relative_db = 10.0f * std::log10(
                    std::max(detection.peak.power, 1.0e-30f) /
                    std::max(maximum_power, 1.0e-30f));
                if (relative_db < sensing_display_floor_db) {
                    continue;
                }
                detections << detection.peak.range_bin << ','
                           << detection.peak.doppler_bin << ','
                           << detection.peak.range_m << ','
                           << detection.peak.doppler_hz << ','
                           << detection.peak.velocity_mps << ','
                           << relative_db << ','
                           << detection.power_over_threshold_db << '\n';
                ++reported_sensing_detections;
            }
            range_doppler.close();
            detections.close();
            if (!range_doppler || !detections) {
                throw std::runtime_error("cannot write sensing telemetry data");
            }
        }

        const auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const double fer_percent = counters_.phy_frames > 0u
            ? 100.0 * static_cast<double>(counters_.phy_crc_failures) /
                  static_cast<double>(counters_.phy_frames)
            : 0.0;
        std::ofstream status(directory / "status.csv");
        status << "metric,value\n" << std::setprecision(12)
               << "snapshot_epoch_ms," << epoch_ms << '\n'
               << "frame_id," << frame_id_ << '\n'
               << "rank," << mode_.rank << '\n'
               << "modulation," << openisac::modulation_name(mode_.modulation) << '\n'
               << "pilot_mode," << openisac::pilot_mode_name(options_.pilot_mode) << '\n'
               << "frame_symbols," << openisac::formal_frame_symbols(options_.pilot_mode) << '\n'
               << "frame_period_us,"
               << frame_period_seconds(options_) * 1.0e6 << '\n'
               << "fft_size," << options_.fft_size << '\n'
               << "cp_length," << options_.cp_length << '\n'
               << "subcarrier_spacing_hz," << options_.subcarrier_spacing_hz << '\n'
               << "sample_rate_hz,"
               << options_.fft_size * options_.subcarrier_spacing_hz << '\n'
               << "center_frequency_hz," << options_.center_frequency_hz << '\n'
               << "tx_spatial_correlation,"
               << options_.transmit_spatial_correlation << '\n'
               << "rx_spatial_correlation,"
               << options_.receive_spatial_correlation << '\n'
               << "spatial_channel_seed," << options_.spatial_channel_seed << '\n'
               << "channel_condition_median," << condition_median << '\n'
               << "channel_condition_p90," << condition_p90 << '\n'
               << "ill_conditioned_subcarriers,"
               << ill_conditioned_subcarriers << '\n'
               << "ill_conditioned_subcarrier_percent,"
               << ill_conditioned_percent << '\n'
               << "snr_db," << options_.snr_db << '\n'
               << "constellation_valid_symbols," << symbols << '\n'
               << "constellation_padding_symbols_excluded,"
               << padding_symbols << '\n'
               << "evm_percent," << diagnostic.evm_percent << '\n'
               << "channel_nmse_db," << diagnostic.channel_nmse_db << '\n'
               << "cfo_true_hz," << options_.cfo_hz << '\n'
               << "cfo_estimated_hz,"
               << options_.cfo_hz + diagnostic.cfo_error_hz << '\n'
               << "cfo_error_hz," << diagnostic.cfo_error_hz << '\n'
               << "sfo_true_ppm," << options_.sfo_ppm << '\n'
               << "sfo_residual_ppm," << diagnostic.residual_sfo_ppm << '\n'
               << "timing_true_samples," << options_.timing_offset << '\n'
               << "timing_estimated_samples,"
               << telemetry_workspace_.timing_estimate.offset << '\n'
               << "timing_metric," << diagnostic.timing_metric << '\n'
               << "noise_variance," << diagnostic.noise_variance_used << '\n'
               << "ingress_socket_packets," << counters_.ingress_socket_packets << '\n'
               << "ingress_queue_drops," << counters_.ingress_queue_drops << '\n'
               << "ingress_queue_depth," << counters_.ingress_queue_depth << '\n'
               << "ingress_queue_high_watermark,"
               << counters_.ingress_queue_high_watermark << '\n'
               << "udp_packets_in," << counters_.udp_packets_in << '\n'
               << "udp_packets_out," << counters_.udp_packets_out << '\n'
               << "udp_dropped," << counters_.dropped_packets << '\n'
               << "phy_frames," << counters_.phy_frames << '\n'
               << "fer_percent," << fer_percent << '\n';
        if (sensing_result != nullptr) {
            status << "sensing_ready,1\n"
                   << "sensing_coherent_frames," << sensing_result->coherent_frames << '\n'
                   << "sensing_range_spacing_m,"
                   << sensing_result->range_bin_spacing_m << '\n'
                   << "sensing_doppler_spacing_hz,"
                   << sensing_result->doppler_bin_spacing_hz << '\n'
                   << "sensing_velocity_spacing_mps,"
                   << sensing_result->velocity_bin_spacing_mps << '\n'
                   << "sensing_peak_range_m,"
                   << sensing_result->strongest_peak.range_m << '\n'
                   << "sensing_peak_doppler_hz,"
                   << sensing_result->strongest_peak.doppler_hz << '\n'
                   << "sensing_peak_velocity_mps,"
                   << sensing_result->strongest_peak.velocity_mps << '\n'
                   << "sensing_detection_count,"
                   << reported_sensing_detections << '\n'
                   << "sensing_raw_cfar_detection_count,"
                   << sensing_result->detections.size() << '\n'
                   << "sensing_display_floor_db," << sensing_display_floor_db << '\n'
                   << "sensing_cfar_cells," << sensing_result->cfar_cells_tested << '\n';
        } else {
            status << "sensing_ready,0\n";
        }
        status.close();
        if (!status) {
            throw std::runtime_error("cannot write telemetry status data");
        }
        last_telemetry_ = std::chrono::steady_clock::now();
    }

    const Options& options_;
    const openisac::Ldpc5041008& codec_;
    const openisac::LinkMode mode_;
    openisac::DynamicLinkWorkspace generation_workspace_;
    openisac::DynamicLinkWorkspace telemetry_workspace_;
    openisac::DynamicLinkReceiverState telemetry_receiver_state_;
    std::unique_ptr<openisac::DynamicSensingProcessor> sensing_;
    std::unique_ptr<openisac::DynamicLinkPipeline> dynamic_pipeline_;
    std::unique_ptr<openisac::Rank4TimePipeline> rank4_pipeline_;
    std::size_t phy_payload_capacity_ = 0u;
    std::size_t fragment_payload_capacity_ = 0u;
    std::vector<std::uint16_t> data_fft_indices_;
    std::uint64_t frame_id_ = 0u;
    std::size_t rank4_sensing_capture_remaining_ = 0u;
    bool rank4_sensing_batch_in_flight_ = false;
    BridgeCounters counters_;
    std::chrono::steady_clock::time_point last_telemetry_{};
};

sockaddr_in make_address(const std::string& address, unsigned port) {
    sockaddr_in result{};
    result.sin_family = AF_INET;
    result.sin_port = htons(static_cast<u_short>(port));
    if (inet_pton(AF_INET, address.c_str(), &result.sin_addr) != 1) {
        throw std::invalid_argument("invalid IPv4 address: " + address);
    }
    return result;
}

void print_statistics(const BridgeCounters& value, double seconds) {
    const double input_mbps = seconds > 0.0
        ? static_cast<double>(value.udp_bytes_in) * 8.0 / seconds / 1.0e6
        : 0.0;
    const double output_mbps = seconds > 0.0
        ? static_cast<double>(value.udp_bytes_out) * 8.0 / seconds / 1.0e6
        : 0.0;
    const double crc_percent = value.phy_frames > 0u
        ? 100.0 * static_cast<double>(value.phy_crc_failures) /
              static_cast<double>(value.phy_frames)
        : 0.0;
    const double latency_ms = value.phy_frames > 0u
        ? value.phy_latency_us / static_cast<double>(value.phy_frames) / 1000.0
        : 0.0;
    std::cout << std::fixed << std::setprecision(3)
              << "Ingress socket/qdrop/depth/high "
              << value.ingress_socket_packets << '/'
              << value.ingress_queue_drops << '/'
              << value.ingress_queue_depth << '/'
              << value.ingress_queue_high_watermark << "; "
              << "UDP in/out/drop " << value.udp_packets_in << '/'
              << value.udp_packets_out << '/' << value.dropped_packets
              << "; bitrate " << input_mbps << '/' << output_mbps << " Mbit/s"
              << "; PHY frames " << value.phy_frames
              << "; FER " << crc_percent << "%"
              << "; PHY latency " << latency_ms << " ms/frame\n";
}

int run_self_test(const Options& options, VideoPhyChannel& channel) {
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t packet = 0u; packet < options.self_test_packets; ++packet) {
        const std::size_t sizes[] = {188u, 376u, 1316u, 2000u, 4096u};
        const std::size_t size = sizes[packet % (sizeof(sizes) / sizeof(sizes[0]))];
        std::vector<std::uint8_t> input(size);
        for (std::size_t index = 0u; index < size; ++index) {
            input[index] = static_cast<std::uint8_t>(
                (packet * 73u + index * 37u + 11u) & 0xFFu);
        }
        const auto output = channel.process(input, static_cast<std::uint32_t>(packet));
        if (!output.has_value() || *output != input) {
            throw std::runtime_error(
                "self-test packet " + std::to_string(packet) + " did not round-trip");
        }
    }
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    print_statistics(channel.counters(), seconds);
    std::cout << "PASS: all " << options.self_test_packets
              << " UDP datagrams survived fragmentation, Rank-"
              << options.transmit_rank << '/'
              << openisac::modulation_name(options.modulation) << " PHY, "
                  "TDL/AWGN/CFO/SFO, LDPC/CRC and reassembly byte-for-byte.\n";
    return 0;
}

int run_live(const Options& options, VideoPhyChannel& channel) {
    SocketRuntime sockets;
    SocketHandle input;
    SocketHandle output;
    input.value = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    output.value = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (input.value == INVALID_SOCKET || output.value == INVALID_SOCKET) {
        throw std::runtime_error("cannot create UDP socket");
    }
    const int receive_buffer = static_cast<int>(std::min<std::size_t>(
        options.socket_buffer_bytes, static_cast<std::size_t>(INT_MAX)));
    setsockopt(input.value, SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char*>(&receive_buffer), sizeof(receive_buffer));
    const auto listen = make_address(options.listen_address, options.listen_port);
    if (bind(input.value, reinterpret_cast<const sockaddr*>(&listen), sizeof(listen)) ==
        SOCKET_ERROR) {
        throw std::runtime_error(
            "cannot bind UDP input; Winsock error " + std::to_string(WSAGetLastError()));
    }
    const auto destination = make_address(options.output_address, options.output_port);
    stop_requested.store(false);
    SetConsoleCtrlHandler(console_handler, TRUE);

    std::cout << "Listening for VLC MPEG-TS/UDP on " << options.listen_address << ':'
              << options.listen_port << "; forwarding decoded packets to "
              << options.output_address << ':' << options.output_port << "\n"
              << "PHY: " << (options.transmit_rank == 4u ? "4x4" : "2x2")
              << " Rank-" << options.transmit_rank << '/'
              << openisac::modulation_name(options.modulation) << ", pilots "
              << openisac::pilot_mode_name(options.pilot_mode)
              << ", frame "
              << openisac::formal_frame_symbols(options.pilot_mode)
              << " symbols, FFT/CP "
              << options.fft_size << '/' << options.cp_length << ", payload "
              << channel.phy_payload_capacity() << " bytes, fragment data "
              << channel.fragment_payload_capacity() << " bytes, SNR "
              << options.snr_db << " dB, CFO " << options.cfo_hz << " Hz, SFO "
              << options.sfo_ppm << " ppm. Press Ctrl+C to stop.\n";

    struct QueuedDatagram {
        std::uint32_t sequence = 0u;
        std::vector<std::uint8_t> bytes;
    };
    std::deque<QueuedDatagram> ingress_queue;
    std::mutex ingress_mutex;
    std::condition_variable ingress_ready;
    std::uint64_t ingress_socket_packets = 0u;
    std::uint64_t ingress_queue_drops = 0u;
    std::size_t ingress_high_watermark = 0u;
    std::exception_ptr ingress_failure;
    std::thread ingress_thread([&] {
        try {
            std::vector<std::uint8_t> receive_buffer_bytes(maximum_udp_payload);
            std::uint32_t packet_sequence = 0u;
            while (!stop_requested.load()) {
                fd_set read_set;
                FD_ZERO(&read_set);
                FD_SET(input.value, &read_set);
                timeval timeout{};
                timeout.tv_sec = 0;
                timeout.tv_usec = 100000;
                const int ready = select(0, &read_set, nullptr, nullptr, &timeout);
                if (ready == SOCKET_ERROR) {
                    throw std::runtime_error(
                        "UDP select failed; Winsock error " +
                        std::to_string(WSAGetLastError()));
                }
                if (ready == 0) {
                    continue;
                }
                const int received = recvfrom(
                    input.value,
                    reinterpret_cast<char*>(receive_buffer_bytes.data()),
                    static_cast<int>(receive_buffer_bytes.size()), 0, nullptr, nullptr);
                if (received == SOCKET_ERROR) {
                    throw std::runtime_error(
                        "UDP receive failed; Winsock error " +
                        std::to_string(WSAGetLastError()));
                }
                QueuedDatagram datagram;
                datagram.sequence = packet_sequence++;
                datagram.bytes.assign(
                    receive_buffer_bytes.begin(),
                    receive_buffer_bytes.begin() + received);
                {
                    std::lock_guard<std::mutex> lock(ingress_mutex);
                    ++ingress_socket_packets;
                    if (ingress_queue.size() >= options.ingress_queue_packets) {
                        ++ingress_queue_drops;
                    } else {
                        ingress_queue.push_back(std::move(datagram));
                        ingress_high_watermark =
                            std::max(ingress_high_watermark, ingress_queue.size());
                    }
                }
                ingress_ready.notify_one();
            }
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(ingress_mutex);
                ingress_failure = std::current_exception();
            }
            stop_requested.store(true);
            ingress_ready.notify_all();
        }
    });

    const auto start = std::chrono::steady_clock::now();
    auto last_report = start;
    std::exception_ptr processing_failure;
    try {
        while (!stop_requested.load()) {
            QueuedDatagram datagram;
            bool has_datagram = false;
            std::uint64_t socket_packets = 0u;
            std::uint64_t queue_drops = 0u;
            std::size_t queue_depth = 0u;
            std::size_t queue_high = 0u;
            {
                std::unique_lock<std::mutex> lock(ingress_mutex);
                ingress_ready.wait_for(lock, std::chrono::milliseconds(250), [&] {
                    return stop_requested.load() || !ingress_queue.empty() ||
                        ingress_failure != nullptr;
                });
                if (!ingress_queue.empty()) {
                    datagram = std::move(ingress_queue.front());
                    ingress_queue.pop_front();
                    has_datagram = true;
                }
                socket_packets = ingress_socket_packets;
                queue_drops = ingress_queue_drops;
                queue_depth = ingress_queue.size();
                queue_high = ingress_high_watermark;
            }
            channel.update_ingress_statistics(
                socket_packets, queue_drops, queue_depth, queue_high);
            if (has_datagram) {
                const auto decoded = channel.process(datagram.bytes, datagram.sequence);
                if (decoded.has_value()) {
                    const int sent = sendto(
                        output.value, reinterpret_cast<const char*>(decoded->data()),
                        static_cast<int>(decoded->size()), 0,
                        reinterpret_cast<const sockaddr*>(&destination),
                        sizeof(destination));
                    if (sent != static_cast<int>(decoded->size())) {
                        throw std::runtime_error(
                            "UDP output failed; Winsock error " +
                            std::to_string(WSAGetLastError()));
                    }
                }
            }
            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(now - last_report).count() >= 2.0) {
                print_statistics(
                    channel.counters(),
                    std::chrono::duration<double>(now - start).count());
                last_report = now;
            }
        }
    } catch (...) {
        processing_failure = std::current_exception();
        stop_requested.store(true);
        ingress_ready.notify_all();
    }
    if (ingress_thread.joinable()) {
        ingress_thread.join();
    }
    if (processing_failure != nullptr) {
        std::rethrow_exception(processing_failure);
    }
    if (ingress_failure != nullptr) {
        std::rethrow_exception(ingress_failure);
    }
    print_statistics(
        channel.counters(),
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        std::cout << std::unitbuf;
        const Options options = parse_options(argc, argv);
        const std::string matrix_root = OPENISAC_MATRIX_DIR;
        const openisac::Ldpc5041008 codec(
            openisac::join_path(matrix_root, "LDPC_504_1008.alist"),
            openisac::join_path(matrix_root, "LDPC_504_1008G.alist"));
        VideoPhyChannel channel(options, codec);
        return options.self_test
            ? run_self_test(options, channel)
            : run_live(options, channel);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
