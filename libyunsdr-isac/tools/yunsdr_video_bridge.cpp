#include "libyunsdr_isac/libyunsdr_transport.hpp"
#include "libyunsdr_isac/openisac_phy_codec.hpp"
#include "libyunsdr_isac/transceiver.hpp"
#include "openisac/sensing.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t fragment_magic = 0x4356494Fu;
constexpr std::uint8_t fragment_version = 1u;
constexpr std::size_t fragment_header_bytes = 20u;
constexpr std::size_t dma_block_samples = 30720u;
constexpr std::size_t capture_tail_samples = 512u;

struct Options {
    std::string device = "pcies:0.0";
    libyunsdr_isac::PhyMode mode = libyunsdr_isac::PhyMode::siso;
    libyunsdr_isac::PhyModulation modulation =
        libyunsdr_isac::PhyModulation::qam64;
    libyunsdr_isac::PilotPattern pilot = libyunsdr_isac::PilotPattern::fdm;
    double frequency_mhz = 1500.0;
    double tx_gain_db = 60.0;
    double rx_gain_db = 20.0;
    unsigned retries = 8u;
    unsigned batch_packets = 8u;
    unsigned lead_blocks = 48u;
    unsigned self_test_packets = 0u;
    unsigned input_port = 50000u;
    unsigned output_port = 50001u;
    unsigned idle_exit_seconds = 0u;
    unsigned warmup_packets = 8u;
    std::string telemetry_directory;
    double telemetry_interval_seconds = 1.0;
    unsigned telemetry_waveform_points = 4096u;
    unsigned sensing_coherent_frames = 16u;
    unsigned sensing_range_bins = 128u;
};

Options parse_options(int argc, char** argv) {
    Options result;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        auto value = [&]() -> std::string {
            if (index + 1 >= argc) {
                throw std::invalid_argument("missing value after " + argument);
            }
            return argv[++index];
        };
        if (argument == "--device") {
            result.device = value();
            if (result.device.empty()) {
                throw std::invalid_argument("device URI must not be empty");
            }
        } else if (argument == "--mode") {
            const auto text = value();
            if (text == "siso") result.mode = libyunsdr_isac::PhyMode::siso;
            else if (text == "mimo2") result.mode = libyunsdr_isac::PhyMode::spatial_2x2;
            else if (text == "stbc") result.mode = libyunsdr_isac::PhyMode::alamouti_stbc_2x2;
            else throw std::invalid_argument("mode must be siso, mimo2 or stbc");
        } else if (argument == "--modulation") {
            const auto text = value();
            if (text == "qpsk") result.modulation = libyunsdr_isac::PhyModulation::qpsk;
            else if (text == "16qam") result.modulation = libyunsdr_isac::PhyModulation::qam16;
            else if (text == "64qam") result.modulation = libyunsdr_isac::PhyModulation::qam64;
            else if (text == "256qam") result.modulation = libyunsdr_isac::PhyModulation::qam256;
            else throw std::invalid_argument("unsupported modulation");
        } else if (argument == "--pilot") {
            const auto text = value();
            if (text == "fdm") result.pilot = libyunsdr_isac::PilotPattern::fdm;
            else if (text == "dmrs") result.pilot = libyunsdr_isac::PilotPattern::nr_dmrs;
            else throw std::invalid_argument("pilot must be fdm or dmrs");
        } else if (argument == "--frequency-mhz") result.frequency_mhz = std::stod(value());
        else if (argument == "--tx-gain") result.tx_gain_db = std::stod(value());
        else if (argument == "--rx-gain") result.rx_gain_db = std::stod(value());
        else if (argument == "--retries") result.retries = static_cast<unsigned>(std::stoul(value()));
        else if (argument == "--batch-packets") result.batch_packets = static_cast<unsigned>(std::stoul(value()));
        else if (argument == "--lead-blocks") result.lead_blocks = static_cast<unsigned>(std::stoul(value()));
        else if (argument == "--self-test") result.self_test_packets = static_cast<unsigned>(std::stoul(value()));
        else if (argument == "--input-port") result.input_port = static_cast<unsigned>(std::stoul(value()));
        else if (argument == "--output-port") result.output_port = static_cast<unsigned>(std::stoul(value()));
        else if (argument == "--idle-exit-seconds") result.idle_exit_seconds = static_cast<unsigned>(std::stoul(value()));
        else if (argument == "--warmup-packets") result.warmup_packets = static_cast<unsigned>(std::stoul(value()));
        else if (argument == "--telemetry-dir") result.telemetry_directory = value();
        else if (argument == "--telemetry-interval") result.telemetry_interval_seconds = std::stod(value());
        else if (argument == "--telemetry-points") result.telemetry_waveform_points = static_cast<unsigned>(std::stoul(value()));
        else if (argument == "--sensing-coherent") result.sensing_coherent_frames = static_cast<unsigned>(std::stoul(value()));
        else if (argument == "--sensing-range-bins") result.sensing_range_bins = static_cast<unsigned>(std::stoul(value()));
        else if (argument == "--help") {
            std::cout
                << "Usage: yunsdr_video_bridge [--device pcies:0.0] "
                   "[--mode siso|mimo2|stbc] "
                   "[--modulation qpsk|16qam|64qam|256qam] [--pilot fdm|dmrs]\n"
                   "  [--frequency-mhz 1500] [--tx-gain 60] [--rx-gain 20] "
                   "[--retries 8] [--batch-packets 8] [--lead-blocks 48]\n"
                   "  [--self-test packets] [--input-port 50000] "
                   "[--output-port 50001] [--idle-exit-seconds 0] "
                   "[--warmup-packets 8]\n"
                   "  [--telemetry-dir PATH] [--telemetry-interval 1.0] "
                   "[--telemetry-points 4096]\n"
                   "  [--sensing-coherent 16] [--sensing-range-bins 128]\n";
            std::exit(0);
        } else throw std::invalid_argument("unknown argument: " + argument);
    }
    if (result.frequency_mhz <= 0.0 || result.batch_packets == 0u ||
        result.batch_packets > 64u || result.lead_blocks == 0u ||
        result.lead_blocks > 64u || result.input_port > 65535u ||
        result.output_port > 65535u ||
        result.telemetry_interval_seconds < 0.2 ||
        result.telemetry_waveform_points == 0u ||
        result.sensing_coherent_frames < 16u ||
        (result.sensing_coherent_frames &
         (result.sensing_coherent_frames - 1u)) != 0u ||
        result.sensing_range_bins == 0u || result.sensing_range_bins > 512u) {
        throw std::invalid_argument("invalid video bridge option");
    }
    return result;
}

const char* modulation_name(libyunsdr_isac::PhyModulation modulation) {
    switch (modulation) {
        case libyunsdr_isac::PhyModulation::qpsk: return "QPSK";
        case libyunsdr_isac::PhyModulation::qam16: return "16QAM";
        case libyunsdr_isac::PhyModulation::qam64: return "64QAM";
        case libyunsdr_isac::PhyModulation::qam256: return "256QAM";
    }
    return "unknown";
}

float condition_number_2x2(
    const libyunsdr_isac::PhyChannel2x2Sample& channel) {
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

void put_u16(std::vector<std::uint8_t>& bytes, std::size_t offset,
             std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value & 0xFFu);
    bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
}

void put_u32(std::vector<std::uint8_t>& bytes, std::size_t offset,
             std::uint32_t value) {
    for (unsigned byte = 0u; byte < 4u; ++byte) {
        bytes[offset + byte] = static_cast<std::uint8_t>(value >> (8u * byte));
    }
}

std::uint16_t get_u16(const std::vector<std::uint8_t>& bytes,
                      std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
        (static_cast<std::uint16_t>(bytes[offset + 1u]) << 8u);
}

std::uint32_t get_u32(const std::vector<std::uint8_t>& bytes,
                      std::size_t offset) {
    std::uint32_t value = 0u;
    for (unsigned byte = 0u; byte < 4u; ++byte) {
        value |= static_cast<std::uint32_t>(bytes[offset + byte]) << (8u * byte);
    }
    return value;
}

template <typename View, typename Storage>
void point_view(View& view, Storage& storage, std::size_t samples) {
    view.channel_count = storage.size();
    view.samples_per_channel = samples;
    for (std::size_t port = 0u; port < storage.size(); ++port) {
        view.channels[port] = storage[port].data();
    }
}

struct FragmentPlan {
    std::size_t packet = 0u;
    std::size_t fragment = 0u;
    std::vector<std::uint8_t> payload;
};

struct PendingSensingFrame {
    std::uint64_t timestamp = 0u;
    std::vector<libyunsdr_isac::PhyChannel2x2Sample> channels;
};

class HardwareVideoLink {
public:
    explicit HardwareVideoLink(const Options& options)
        : options_(options) {
        libyunsdr_isac::OpenIsacPhyCodecConfig phy;
        phy.mode = options.mode;
        phy.modulation = options.modulation;
        phy.pilot_pattern = options.pilot;
        phy.maximum_timing_offset_samples = capture_tail_samples;
        codec_ = libyunsdr_isac::make_openisac_phy_codec(phy);
        profile_ = codec_->mode_profile();
        if (codec_->maximum_payload_bytes() <= fragment_header_bytes) {
            throw std::runtime_error("selected PHY payload is too small for video");
        }
        fragment_data_bytes_ =
            codec_->maximum_payload_bytes() - fragment_header_bytes;

        transport_ = libyunsdr_isac::make_libyunsdr_transport();
        session_.reset(new libyunsdr_isac::TransceiverSession(*transport_));
        libyunsdr_isac::SessionConfig radio;
        radio.mode = options.mode;
        radio.pilot_pattern = options.pilot;
        radio.dma_block_samples = dma_block_samples;
        radio.tx_lead_samples = options.lead_blocks * dma_block_samples;
        radio.radio.uri = options.device;
        radio.radio.sample_rate_hz = 15.36e6;
        radio.radio.center_frequency_hz = options.frequency_mhz * 1.0e6;
        radio.radio.bandwidth_hz = 15.36e6;
        radio.radio.tx_gain_db = options.tx_gain_db;
        radio.radio.rx_gain_db = options.rx_gain_db;
        const auto identity = session_->open_and_configure(radio);
        std::cout << "Device: " << identity.model
                  << ", URI=" << options.device
                  << ", mode=" << libyunsdr_isac::phy_mode_name(options.mode)
                  << ", frequency=" << options.frequency_mhz << " MHz"
                  << ", TX gain=" << options.tx_gain_db
                  << ", RX gain=" << options.rx_gain_db << '\n';
        std::cout << "PHY payload=" << codec_->maximum_payload_bytes()
                  << " bytes, fragment data=" << fragment_data_bytes_
                  << " bytes, frame samples="
                  << codec_->frame_samples_per_port() << '\n';
        session_->start();

        if (!options_.telemetry_directory.empty()) {
            openisac::DynamicSensingConfig sensing_config;
            sensing_config.fft_size = 1024u;
            sensing_config.transmit_ports = profile_.tx_ports;
            sensing_config.receive_ports = profile_.rx_ports;
            sensing_config.coherent_frames = options_.sensing_coherent_frames;
            sensing_config.range_fft_size = 1024u;
            sensing_config.doppler_fft_size = options_.sensing_coherent_frames;
            sensing_config.subcarrier_spacing_hz = 15000.0f;
            sensing_config.frame_period_seconds = static_cast<float>(
                codec_->frame_samples_per_port() / 15.36e6);
            sensing_config.center_frequency_hz = static_cast<float>(
                options_.frequency_mhz * 1.0e6);
            sensing_config.minimum_range_bin = 0u;
            sensing_config.maximum_range_bin = options_.sensing_range_bins;
            sensing_config.enable_static_clutter_suppression = false;
            sensing_config.doppler_dc_exclusion_bins = 0u;
            sensing_config.enable_cfar_detection = true;
            sensing_config.cfar_false_alarm_probability = 1.0e-7f;
            sensing_.reset(new openisac::DynamicSensingProcessor(sensing_config));
            std::filesystem::create_directories(options_.telemetry_directory);
            std::cout << "Telemetry: " << options_.telemetry_directory
                      << ", interval=" << options_.telemetry_interval_seconds
                      << " s, sensing CPI=" << options_.sensing_coherent_frames
                      << " frames\n";
        }

        wait_storage_.assign(
            profile_.rx_ports,
            std::vector<std::complex<float>>(dma_block_samples));
        libyunsdr_isac::MutableMultiChannelBuffer wait;
        point_view(wait, wait_storage_, dma_block_samples);
        for (unsigned block = 0u; block < 9u; ++block) {
            session_->receive(wait, 1.0);
        }
    }

    ~HardwareVideoLink() {
        if (session_) session_->close();
    }

    bool transfer_batch(
        const std::vector<std::vector<std::uint8_t>>& packets,
        std::vector<std::vector<std::uint8_t>>& recovered) {
        if (packets.empty()) {
            recovered.clear();
            return true;
        }
        input_packet_count_ += packets.size();
        std::vector<std::uint32_t> packet_sequences(packets.size());
        std::vector<FragmentPlan> fragments;
        for (std::size_t packet = 0u; packet < packets.size(); ++packet) {
            if (packets[packet].empty() || packets[packet].size() > 65507u) {
                throw std::invalid_argument("UDP packet has invalid size");
            }
            packet_sequences[packet] = next_packet_sequence_++;
            const std::size_t count =
                (packets[packet].size() + fragment_data_bytes_ - 1u) /
                fragment_data_bytes_;
            if (count > std::numeric_limits<std::uint16_t>::max()) {
                throw std::runtime_error("UDP packet needs too many PHY fragments");
            }
            for (std::size_t fragment = 0u; fragment < count; ++fragment) {
                const std::size_t offset = fragment * fragment_data_bytes_;
                const std::size_t bytes = std::min(
                    fragment_data_bytes_, packets[packet].size() - offset);
                FragmentPlan plan;
                plan.packet = packet;
                plan.fragment = fragment;
                plan.payload.assign(fragment_header_bytes + bytes, 0u);
                put_u32(plan.payload, 0u, fragment_magic);
                plan.payload[4u] = fragment_version;
                put_u16(plan.payload, 6u,
                        static_cast<std::uint16_t>(fragment_header_bytes));
                put_u32(plan.payload, 8u, packet_sequences[packet]);
                put_u16(plan.payload, 12u, static_cast<std::uint16_t>(fragment));
                put_u16(plan.payload, 14u, static_cast<std::uint16_t>(count));
                put_u32(plan.payload, 16u,
                        static_cast<std::uint32_t>(packets[packet].size()));
                std::copy_n(packets[packet].begin() + offset, bytes,
                            plan.payload.begin() + fragment_header_bytes);
                fragments.push_back(std::move(plan));
            }
        }

        for (unsigned attempt = 0u; attempt <= options_.retries; ++attempt) {
            if (transfer_fragments(fragments, packet_sequences, recovered)) {
                retry_count_ += attempt;
                packet_count_ += packets.size();
                commit_sensing_batch();
                write_telemetry_if_due();
                return true;
            }
            ++failed_attempts_;
            if (attempt < options_.retries) {
                std::cout << "Batch decode failed; retry " << (attempt + 1u)
                          << '/' << options_.retries
                          << " (" << last_failure_ << ")\n";
            }
        }
        dropped_packets_ += packets.size();
        return false;
    }

    void print_counters() {
        const auto events = session_->poll_events();
        std::cout << "Packets=" << packet_count_
                  << ", retries=" << retry_count_
                  << ", failed attempts=" << failed_attempts_
                  << ", dropped=" << dropped_packets_
                  << ", TX timeout=" << events.tx_timeouts
                  << ", TX underflow=" << events.tx_underflows
                  << ", RX overflow=" << events.rx_overflows
                  << ", timestamp discontinuity="
                  << events.timestamp_discontinuities << '\n';
    }

    void drain_receive_block() {
        libyunsdr_isac::MutableMultiChannelBuffer wait;
        point_view(wait, wait_storage_, dma_block_samples);
        session_->receive(wait, 1.0);
    }

private:
    void commit_sensing_batch() {
        if (!sensing_ || pending_sensing_frames_.empty()) return;
        sensing_->reset();
        sensing_ready_ = false;
        const std::size_t fft_size = 1024u;
        const std::size_t links = profile_.tx_ports * profile_.rx_ports;
        for (const auto& frame : pending_sensing_frames_) {
            if (frame.channels.size() != fft_size) continue;
            std::vector<std::complex<float>> response(links * fft_size);
            std::vector<std::uint8_t> active(fft_size, 0u);
            for (std::size_t fft = 0u; fft < fft_size; ++fft) {
                const auto& channel = frame.channels[fft];
                response[fft] = channel.h00;
                if (links == 4u) {
                    response[fft_size + fft] = channel.h01;
                    response[2u * fft_size + fft] = channel.h10;
                    response[3u * fft_size + fft] = channel.h11;
                }
                const float power = std::norm(channel.h00) +
                    (links == 4u ? std::norm(channel.h01) +
                        std::norm(channel.h10) + std::norm(channel.h11) : 0.0f);
                if (power > 1.0e-10f) active[fft] = 1u;
            }
            if (sensing_->push_channel_frame(
                    sensing_sequence_++, frame.timestamp, response, active)) {
                sensing_ready_ = true;
            }
        }
    }

    void write_telemetry_if_due() {
        if (options_.telemetry_directory.empty() || !latest_telemetry_.valid) return;
        const auto now = std::chrono::steady_clock::now();
        if (last_telemetry_.time_since_epoch().count() != 0 &&
            std::chrono::duration<double>(now - last_telemetry_).count() <
                options_.telemetry_interval_seconds) {
            return;
        }
        const std::filesystem::path directory(options_.telemetry_directory);
        std::filesystem::create_directories(directory);
        const std::size_t points = std::min<std::size_t>(
            options_.telemetry_waveform_points,
            latest_telemetry_.receive_waveform_rx0.size());

        std::ofstream waveform(directory / "waveform.csv");
        waveform << "sample,tx0_i,tx0_q,rx0_i,rx0_q\n" << std::setprecision(9);
        for (std::size_t sample = 0u; sample < points; ++sample) {
            const auto value = latest_telemetry_.receive_waveform_rx0[sample];
            waveform << sample << ",0,0," << value.real() << ','
                     << value.imag() << '\n';
        }

        std::ofstream constellation(directory / "constellation.csv");
        constellation
            << "index,layer,ideal_i,ideal_q,equalized_i,equalized_q\n"
            << std::setprecision(9);
        const std::size_t symbols = std::min(
            latest_telemetry_.constellation_ideal.size(),
            latest_telemetry_.constellation_equalized.size());
        for (std::size_t index = 0u; index < symbols; ++index) {
            const auto ideal = latest_telemetry_.constellation_ideal[index];
            const auto equalized = latest_telemetry_.constellation_equalized[index];
            constellation << index << ',' << index % profile_.spatial_rank << ','
                          << ideal.real() << ',' << ideal.imag() << ','
                          << equalized.real() << ',' << equalized.imag() << '\n';
        }

        std::ofstream channel(directory / "channel.csv");
        channel << "fft,h00_i,h00_q,h01_i,h01_q,h10_i,h10_q,h11_i,h11_q\n"
                << std::setprecision(9);
        for (std::size_t fft = 0u;
             fft < latest_telemetry_.channel_frequency_response.size(); ++fft) {
            const auto& value = latest_telemetry_.channel_frequency_response[fft];
            channel << fft << ',' << value.h00.real() << ',' << value.h00.imag()
                    << ',' << value.h01.real() << ',' << value.h01.imag()
                    << ',' << value.h10.real() << ',' << value.h10.imag()
                    << ',' << value.h11.real() << ',' << value.h11.imag() << '\n';
        }
        waveform.close();
        constellation.close();
        channel.close();
        if (!waveform || !constellation || !channel) {
            throw std::runtime_error("cannot write hardware telemetry CSV files");
        }

        std::vector<float> conditions;
        if (profile_.tx_ports == 1u) {
            conditions.push_back(1.0f);
        } else {
            for (const auto& value : latest_telemetry_.channel_frequency_response) {
                const float power = std::norm(value.h00) + std::norm(value.h01) +
                    std::norm(value.h10) + std::norm(value.h11);
                if (power > 1.0e-10f) conditions.push_back(condition_number_2x2(value));
            }
        }
        std::sort(conditions.begin(), conditions.end());
        const float condition_median = conditions.empty()
            ? 0.0f : conditions[conditions.size() / 2u];
        const float condition_p90 = conditions.empty()
            ? 0.0f : conditions[std::min(
                conditions.size() - 1u, conditions.size() * 9u / 10u)];
        const std::size_t ill = static_cast<std::size_t>(std::count_if(
            conditions.begin(), conditions.end(),
            [](float value) { return value > 10.0f; }));
        const double ill_percent = conditions.empty() ? 0.0 :
            100.0 * static_cast<double>(ill) / conditions.size();

        const openisac::DynamicSensingResult* sensing_result = nullptr;
        std::size_t sensing_detections = 0u;
        if (sensing_ && sensing_ready_) {
            sensing_result = &sensing_->last_result();
            const auto& config = sensing_->config();
            const std::size_t range_bins = std::min<std::size_t>(
                options_.sensing_range_bins, config.range_fft_size / 2u);
            float maximum_power = 0.0f;
            for (std::size_t doppler = 0u; doppler < config.doppler_fft_size; ++doppler) {
                for (std::size_t range = 0u; range < range_bins; ++range) {
                    maximum_power = std::max(maximum_power, std::norm(
                        sensing_result->range_doppler_map[
                            doppler * config.range_fft_size + range]));
                }
            }
            std::ofstream range_doppler(directory / "sensing_range_doppler.csv");
            range_doppler
                << "doppler_bin,range_bin,range_m,velocity_mps,relative_power_db\n"
                << std::setprecision(9);
            const std::size_t dc = config.doppler_fft_size / 2u;
            for (std::size_t doppler = 0u; doppler < config.doppler_fft_size; ++doppler) {
                const int centered = static_cast<int>(doppler) - static_cast<int>(dc);
                for (std::size_t range = 0u; range < range_bins; ++range) {
                    const float power = std::norm(sensing_result->range_doppler_map[
                        doppler * config.range_fft_size + range]);
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
                if (relative_db < -30.0f) continue;
                detections << detection.peak.range_bin << ','
                           << detection.peak.doppler_bin << ','
                           << detection.peak.range_m << ','
                           << detection.peak.doppler_hz << ','
                           << detection.peak.velocity_mps << ','
                           << relative_db << ','
                           << detection.power_over_threshold_db << '\n';
                ++sensing_detections;
            }
        }

        const auto events = session_->poll_events();
        const auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const double fer_percent = phy_frames_ == 0u ? 0.0 :
            100.0 * static_cast<double>(phy_crc_failures_) / phy_frames_;
        const bool stbc = options_.mode ==
            libyunsdr_isac::PhyMode::alamouti_stbc_2x2;
        std::ofstream status(directory / "status.csv");
        status << "metric,value\n" << std::setprecision(12)
               << "snapshot_epoch_ms," << epoch_ms << '\n'
               << "hardware_mode,1\n"
               << "frame_id," << ++telemetry_frame_id_ << '\n'
               << "rank," << profile_.spatial_rank << '\n'
               << "mimo_mode," << (stbc ? "alamouti_stbc" : "spatial_multiplexing") << '\n'
               << "tx_ports," << profile_.tx_ports << '\n'
               << "rx_ports," << profile_.rx_ports << '\n'
               << "modulation," << modulation_name(options_.modulation) << '\n'
               << "pilot_mode," << (options_.pilot == libyunsdr_isac::PilotPattern::fdm ? "fdm" : "nr-dmrs") << '\n'
               << "frame_symbols," << (options_.pilot == libyunsdr_isac::PilotPattern::fdm ? 3u : 5u) << '\n'
               << "frame_period_us," << codec_->frame_samples_per_port() / 15.36 << '\n'
               << "fft_size,1024\ncp_length,128\nsubcarrier_spacing_hz,15000\n"
               << "sample_rate_hz,15360000\n"
               << "center_frequency_hz," << options_.frequency_mhz * 1.0e6 << '\n'
               << "tx_spatial_correlation,0\nrx_spatial_correlation,0\n"
               << "channel_condition_median," << condition_median << '\n'
               << "channel_condition_p90," << condition_p90 << '\n'
               << "ill_conditioned_subcarriers," << ill << '\n'
               << "ill_conditioned_subcarrier_percent," << ill_percent << '\n'
               << "snr_db,0\n"
               << "constellation_valid_symbols," << symbols << '\n'
               << "constellation_padding_symbols_excluded,0\n"
               << "evm_percent," << latest_telemetry_.decode.evm_percent << '\n'
               << "channel_nmse_db,0\n"
               << "cfo_true_hz,0\n"
               << "cfo_estimated_hz," << latest_telemetry_.decode.cfo_hz << '\n'
               << "cfo_error_hz,0\n"
               << "sfo_true_ppm,0\n"
               << "sfo_residual_ppm," << latest_telemetry_.residual_sfo_ppm << '\n'
               << "timing_true_samples,0\n"
               << "timing_estimated_samples," << latest_telemetry_.timing_offset_samples << '\n'
               << "timing_metric," << latest_telemetry_.decode.timing_metric << '\n'
               << "noise_variance," << latest_telemetry_.noise_variance << '\n'
               << "ingress_socket_packets," << input_packet_count_ << '\n'
               << "ingress_queue_drops,0\ningress_queue_depth,0\ningress_queue_high_watermark,0\n"
               << "udp_packets_in," << input_packet_count_ << '\n'
               << "udp_packets_out," << packet_count_ << '\n'
               << "udp_dropped," << dropped_packets_ << '\n'
               << "phy_frames," << phy_frames_ << '\n'
               << "fer_percent," << fer_percent << '\n'
               << "tx_timeouts," << events.tx_timeouts << '\n'
               << "tx_underflows," << events.tx_underflows << '\n'
               << "rx_overflows," << events.rx_overflows << '\n'
               << "timestamp_discontinuities," << events.timestamp_discontinuities << '\n';
        if (sensing_result != nullptr) {
            status << "sensing_ready,1\n"
                   << "sensing_coherent_frames," << sensing_result->coherent_frames << '\n'
                   << "sensing_range_spacing_m," << sensing_result->range_bin_spacing_m << '\n'
                   << "sensing_doppler_spacing_hz," << sensing_result->doppler_bin_spacing_hz << '\n'
                   << "sensing_velocity_spacing_mps," << sensing_result->velocity_bin_spacing_mps << '\n'
                   << "sensing_peak_range_m," << sensing_result->strongest_peak.range_m << '\n'
                   << "sensing_peak_doppler_hz," << sensing_result->strongest_peak.doppler_hz << '\n'
                   << "sensing_peak_velocity_mps," << sensing_result->strongest_peak.velocity_mps << '\n'
                   << "sensing_detection_count," << sensing_detections << '\n'
                   << "sensing_raw_cfar_detection_count," << sensing_result->detections.size() << '\n'
                   << "sensing_cfar_cells," << sensing_result->cfar_cells_tested << '\n';
        } else {
            status << "sensing_ready,0\n";
        }
        status.close();
        if (!status) throw std::runtime_error("cannot write hardware telemetry status");
        last_telemetry_ = now;
    }

    bool transfer_fragments(
        const std::vector<FragmentPlan>& fragments,
        const std::vector<std::uint32_t>& packet_sequences,
        std::vector<std::vector<std::uint8_t>>& recovered) {
        pending_sensing_frames_.clear();
        const std::size_t frame_samples = codec_->frame_samples_per_port();
        const std::size_t waveform_samples = fragments.size() * frame_samples;
        const std::size_t tx_samples = std::max(
            dma_block_samples,
            ((waveform_samples + dma_block_samples - 1u) /
             dma_block_samples) * dma_block_samples);
        tx_storage_.assign(
            profile_.tx_ports,
            std::vector<std::complex<float>>(tx_samples));
        encoded_storage_.assign(
            profile_.tx_ports,
            std::vector<std::complex<float>>(frame_samples));
        libyunsdr_isac::MutableMultiChannelBuffer encoded;
        point_view(encoded, encoded_storage_, frame_samples);
        for (std::size_t index = 0u; index < fragments.size(); ++index) {
            codec_->encode(
                {fragments[index].payload.data(), fragments[index].payload.size()},
                next_phy_sequence_++, 0xC057u, encoded);
            for (std::size_t port = 0u; port < profile_.tx_ports; ++port) {
                std::copy(encoded_storage_[port].begin(),
                          encoded_storage_[port].end(),
                          tx_storage_[port].begin() + index * frame_samples);
            }
        }

        libyunsdr_isac::MutableMultiChannelBuffer wait;
        point_view(wait, wait_storage_, dma_block_samples);
        session_->receive(wait, 1.0);
        const auto tx_timestamp = session_->next_transmit_timestamp();
        libyunsdr_isac::ConstMultiChannelBuffer tx;
        point_view(tx, tx_storage_, tx_samples);
        session_->transmit(tx, tx_timestamp, 1.0);
        for (std::size_t block = 0u; block < options_.lead_blocks; ++block) {
            session_->receive(wait, 1.0);
        }

        // The PCIE backend is most reliable when every RX request uses the
        // same DMA block size. Variable-sized reads caused timestamp jumps in
        // continuous VLC traffic even though a single self-test batch passed.
        const std::size_t tx_blocks = tx_samples / dma_block_samples;
        const std::size_t capture_samples =
            (tx_blocks + 1u) * dma_block_samples;
        capture_storage_.assign(
            profile_.rx_ports,
            std::vector<std::complex<float>>(capture_samples));
        std::uint64_t capture_timestamp = 0u;
        for (std::size_t block = 0u; block <= tx_blocks; ++block) {
            const auto received = session_->receive(wait, 1.0);
            if (block == 0u) capture_timestamp = received.timestamp;
            for (std::size_t port = 0u; port < profile_.rx_ports; ++port) {
                std::copy(wait_storage_[port].begin(), wait_storage_[port].end(),
                          capture_storage_[port].begin() +
                              block * dma_block_samples);
            }
        }
        if (capture_timestamp != tx_timestamp) {
            last_failure_ = "capture timestamp mismatch";
            return false;
        }

        recovered.assign(packet_sequences.size(), {});
        std::vector<std::size_t> received_fragments(packet_sequences.size(), 0u);
        std::vector<std::uint8_t> payload(codec_->maximum_payload_bytes());
        for (std::size_t index = 0u; index < fragments.size(); ++index) {
            libyunsdr_isac::ConstMultiChannelBuffer slice;
            slice.channel_count = profile_.rx_ports;
            slice.samples_per_channel = frame_samples + capture_tail_samples;
            for (std::size_t port = 0u; port < profile_.rx_ports; ++port) {
                slice.channels[port] =
                    capture_storage_[port].data() + index * frame_samples;
            }
            const auto decoded = codec_->decode(
                slice, capture_timestamp + index * frame_samples, 0xC057u,
                {payload.data(), payload.size()});
            ++phy_frames_;
            if (!decoded.crc_ok) ++phy_crc_failures_;
            if (!decoded.timing_ok || !decoded.header_ok || !decoded.crc_ok) {
                last_failure_ = "fragment " + std::to_string(index) +
                    " timing=" + std::to_string(decoded.timing_ok) +
                    " header=" + std::to_string(decoded.header_ok) +
                    " crc=" + std::to_string(decoded.crc_ok);
                return false;
            }
            libyunsdr_isac::PhyTelemetrySnapshot snapshot;
            if (codec_->copy_telemetry(snapshot)) {
                latest_telemetry_ = snapshot;
                pending_sensing_frames_.push_back({
                    snapshot.capture_timestamp,
                    std::move(snapshot.channel_frequency_response)});
            }
            std::vector<std::uint8_t> bytes(
                payload.begin(), payload.begin() + decoded.payload_bytes);
            if (bytes.size() < fragment_header_bytes ||
                get_u32(bytes, 0u) != fragment_magic ||
                bytes[4u] != fragment_version ||
                get_u16(bytes, 6u) != fragment_header_bytes) {
                return false;
            }
            const auto packet_sequence = get_u32(bytes, 8u);
            const auto fragment_index = get_u16(bytes, 12u);
            const auto fragment_count = get_u16(bytes, 14u);
            const auto packet_bytes = get_u32(bytes, 16u);
            const auto packet = fragments[index].packet;
            if (packet_sequence != packet_sequences[packet] ||
                fragment_index != fragments[index].fragment ||
                packet_bytes > 65507u || fragment_count == 0u) {
                return false;
            }
            if (recovered[packet].empty()) recovered[packet].resize(packet_bytes);
            const std::size_t offset = fragment_index * fragment_data_bytes_;
            const std::size_t data_bytes = bytes.size() - fragment_header_bytes;
            if (offset + data_bytes > recovered[packet].size()) return false;
            std::copy(bytes.begin() + fragment_header_bytes, bytes.end(),
                      recovered[packet].begin() + offset);
            ++received_fragments[packet];
            if (received_fragments[packet] > fragment_count) return false;
        }
        for (std::size_t packet = 0u; packet < recovered.size(); ++packet) {
            const std::size_t expected =
                (recovered[packet].size() + fragment_data_bytes_ - 1u) /
                fragment_data_bytes_;
            if (received_fragments[packet] != expected) return false;
        }
        return true;
    }

    Options options_;
    std::unique_ptr<libyunsdr_isac::IPhyFrameCodec> codec_;
    libyunsdr_isac::ModeProfile profile_;
    std::unique_ptr<libyunsdr_isac::IVendorTransport> transport_;
    std::unique_ptr<libyunsdr_isac::TransceiverSession> session_;
    std::size_t fragment_data_bytes_ = 0u;
    std::uint32_t next_packet_sequence_ = 1u;
    std::uint16_t next_phy_sequence_ = 1u;
    std::vector<std::vector<std::complex<float>>> wait_storage_;
    std::vector<std::vector<std::complex<float>>> tx_storage_;
    std::vector<std::vector<std::complex<float>>> encoded_storage_;
    std::vector<std::vector<std::complex<float>>> capture_storage_;
    std::uint64_t packet_count_ = 0u;
    std::uint64_t retry_count_ = 0u;
    std::uint64_t failed_attempts_ = 0u;
    std::uint64_t dropped_packets_ = 0u;
    std::uint64_t input_packet_count_ = 0u;
    std::uint64_t phy_frames_ = 0u;
    std::uint64_t phy_crc_failures_ = 0u;
    std::uint64_t telemetry_frame_id_ = 0u;
    std::uint64_t sensing_sequence_ = 0u;
    libyunsdr_isac::PhyTelemetrySnapshot latest_telemetry_{};
    std::vector<PendingSensingFrame> pending_sensing_frames_;
    std::unique_ptr<openisac::DynamicSensingProcessor> sensing_;
    bool sensing_ready_ = false;
    std::chrono::steady_clock::time_point last_telemetry_{};
    std::string last_failure_ = "unknown";
};

class SocketRuntime {
public:
    SocketRuntime() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
    }
    ~SocketRuntime() { WSACleanup(); }
};

void run_self_test(HardwareVideoLink& link, const Options& options) {
    unsigned completed = 0u;
    while (completed < options.self_test_packets) {
        const unsigned count = std::min(
            options.batch_packets, options.self_test_packets - completed);
        std::vector<std::vector<std::uint8_t>> packets(count);
        for (unsigned packet = 0u; packet < count; ++packet) {
            const std::size_t bytes = 1316u + ((completed + packet) % 5u) * 37u;
            packets[packet].resize(bytes);
            for (std::size_t index = 0u; index < bytes; ++index) {
                packets[packet][index] = static_cast<std::uint8_t>(
                    (index * 67u + (completed + packet) * 31u + 9u) & 0xFFu);
            }
        }
        std::vector<std::vector<std::uint8_t>> recovered;
        if (!link.transfer_batch(packets, recovered) || recovered != packets) {
            throw std::runtime_error("hardware video self-test payload mismatch");
        }
        completed += count;
        std::cout << "Self-test packets " << completed << '/'
                  << options.self_test_packets << " passed\n";
    }
    link.print_counters();
}

void run_udp(HardwareVideoLink& link, const Options& options) {
    SocketRuntime sockets;
    const SOCKET input = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    const SOCKET output = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (input == INVALID_SOCKET || output == INVALID_SOCKET) {
        throw std::runtime_error("cannot create UDP sockets");
    }
    int receive_buffer_bytes = 8 * 1024 * 1024;
    setsockopt(input, SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char*>(&receive_buffer_bytes),
               sizeof(receive_buffer_bytes));
    sockaddr_in bind_address{};
    bind_address.sin_family = AF_INET;
    bind_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind_address.sin_port = htons(static_cast<u_short>(options.input_port));
    if (bind(input, reinterpret_cast<const sockaddr*>(&bind_address),
             sizeof(bind_address)) == SOCKET_ERROR) {
        closesocket(input);
        closesocket(output);
        throw std::runtime_error("cannot bind UDP input port");
    }
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(static_cast<u_short>(options.output_port));
    inet_pton(AF_INET, "127.0.0.1", &destination.sin_addr);
    // Keep the socket nonblocking so the same thread can continuously drain
    // RX DMA while VLC is idle. Otherwise the device timestamps fall hundreds
    // of 2 ms blocks behind during VLC startup and scheduled TX becomes late.
    u_long nonblocking = 1u;
    if (ioctlsocket(input, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        closesocket(input);
        closesocket(output);
        throw std::runtime_error("cannot make UDP input nonblocking");
    }
    std::cout << "UDP bridge: 127.0.0.1:" << options.input_port
              << " -> PHY -> 127.0.0.1:" << options.output_port << '\n';

    if (options.warmup_packets != 0u) {
        std::vector<std::vector<std::uint8_t>> warmup(
            options.warmup_packets, std::vector<std::uint8_t>(1316u));
        for (std::size_t packet = 0u; packet < warmup.size(); ++packet) {
            for (std::size_t index = 0u; index < warmup[packet].size(); ++index) {
                warmup[packet][index] = static_cast<std::uint8_t>(
                    (index * 43u + packet * 17u + 5u) & 0xFFu);
            }
        }
        std::vector<std::vector<std::uint8_t>> recovered;
        if (!link.transfer_batch(warmup, recovered) || recovered != warmup) {
            throw std::runtime_error("hardware warmup batch failed");
        }
        std::cout << "Hardware warmup: " << options.warmup_packets
                  << " packets passed.\n";
    }

    std::array<std::uint8_t, 65507u> buffer{};
    std::uint64_t forwarded = 0u;
    std::uint64_t next_report = 100u;
    bool received_any = false;
    auto last_input = std::chrono::steady_clock::now();
    for (;;) {
        std::vector<std::vector<std::uint8_t>> packets;
        const int first = recvfrom(
            input, reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size()), 0, nullptr, nullptr);
        if (first == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) {
                link.drain_receive_block();
                if (received_any && options.idle_exit_seconds != 0u &&
                    std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - last_input).count() >=
                        options.idle_exit_seconds) {
                    break;
                }
                continue;
            }
            throw std::runtime_error("UDP receive failed");
        }
        received_any = true;
        last_input = std::chrono::steady_clock::now();
        packets.emplace_back(buffer.begin(), buffer.begin() + first);
        while (packets.size() < options.batch_packets) {
            const int bytes = recvfrom(
                input, reinterpret_cast<char*>(buffer.data()),
                static_cast<int>(buffer.size()), 0, nullptr, nullptr);
            if (bytes == SOCKET_ERROR) break;
            packets.emplace_back(buffer.begin(), buffer.begin() + bytes);
            last_input = std::chrono::steady_clock::now();
        }

        std::vector<std::vector<std::uint8_t>> recovered;
        if (link.transfer_batch(packets, recovered)) {
            for (const auto& packet : recovered) {
                sendto(output, reinterpret_cast<const char*>(packet.data()),
                       static_cast<int>(packet.size()), 0,
                       reinterpret_cast<const sockaddr*>(&destination),
                       sizeof(destination));
                ++forwarded;
            }
        }
        if (forwarded >= next_report) {
            std::cout << "Forwarded UDP packets: " << forwarded << '\n';
            link.print_counters();
            while (next_report <= forwarded) next_report += 100u;
        }
    }
    std::cout << "UDP input idle; bridge exiting cleanly.\n";
    link.print_counters();
    closesocket(input);
    closesocket(output);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        std::cout.setf(std::ios::unitbuf);
        std::cerr.setf(std::ios::unitbuf);
        const auto options = parse_options(argc, argv);
        HardwareVideoLink link(options);
        if (options.self_test_packets != 0u) run_self_test(link, options);
        else run_udp(link, options);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
