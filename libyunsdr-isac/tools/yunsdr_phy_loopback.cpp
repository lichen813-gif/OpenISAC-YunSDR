#include "libyunsdr_isac/libyunsdr_transport.hpp"
#include "libyunsdr_isac/openisac_phy_codec.hpp"
#include "libyunsdr_isac/transceiver.hpp"

#include <algorithm>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Options {
    std::string device = "pcies:0.0";
    libyunsdr_isac::PhyMode mode = libyunsdr_isac::PhyMode::siso;
    libyunsdr_isac::PhyModulation modulation =
        libyunsdr_isac::PhyModulation::qam64;
    libyunsdr_isac::PilotPattern pilot =
        libyunsdr_isac::PilotPattern::nr_dmrs;
    unsigned frames = 3u;
    double tx_gain_db = 60.0;
    double rx_gain_db = 5.0;
    double frequency_mhz = 2400.0;
};

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--device" && index + 1 < argc) {
            options.device = argv[++index];
            if (options.device.empty()) {
                throw std::invalid_argument("device URI must not be empty");
            }
        } else if (argument == "--mode" && index + 1 < argc) {
            const std::string value = argv[++index];
            if (value == "siso") options.mode = libyunsdr_isac::PhyMode::siso;
            else if (value == "mimo2") {
                options.mode = libyunsdr_isac::PhyMode::spatial_2x2;
            } else if (value == "stbc") {
                options.mode = libyunsdr_isac::PhyMode::alamouti_stbc_2x2;
            } else throw std::invalid_argument("mode must be siso, mimo2 or stbc");
        } else if (argument == "--modulation" && index + 1 < argc) {
            const std::string value = argv[++index];
            if (value == "qpsk") options.modulation = libyunsdr_isac::PhyModulation::qpsk;
            else if (value == "16qam") options.modulation = libyunsdr_isac::PhyModulation::qam16;
            else if (value == "64qam") options.modulation = libyunsdr_isac::PhyModulation::qam64;
            else if (value == "256qam") options.modulation = libyunsdr_isac::PhyModulation::qam256;
            else throw std::invalid_argument("unsupported modulation");
        } else if (argument == "--pilot" && index + 1 < argc) {
            const std::string value = argv[++index];
            if (value == "fdm") options.pilot = libyunsdr_isac::PilotPattern::fdm;
            else if (value == "dmrs") options.pilot = libyunsdr_isac::PilotPattern::nr_dmrs;
            else throw std::invalid_argument("pilot must be fdm or dmrs");
        } else if (argument == "--frames" && index + 1 < argc) {
            options.frames = static_cast<unsigned>(std::stoul(argv[++index]));
            if (options.frames == 0u) throw std::invalid_argument("frames must be nonzero");
        } else if (argument == "--tx-gain" && index + 1 < argc) {
            options.tx_gain_db = std::stod(argv[++index]);
        } else if (argument == "--rx-gain" && index + 1 < argc) {
            options.rx_gain_db = std::stod(argv[++index]);
        } else if (argument == "--frequency-mhz" && index + 1 < argc) {
            options.frequency_mhz = std::stod(argv[++index]);
            if (options.frequency_mhz <= 0.0) {
                throw std::invalid_argument("frequency must be positive");
            }
        } else if (argument == "--help") {
            std::cout << "Usage: yunsdr_phy_loopback [--device pcies:0.0] "
                         "[--mode siso|mimo2|stbc] "
                         "[--modulation qpsk|16qam|64qam|256qam] "
                         "[--pilot fdm|dmrs] [--frames N] "
                         "[--tx-gain dB] [--rx-gain dB] "
                         "[--frequency-mhz MHz]\n";
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown or incomplete argument: " + argument);
        }
    }
    return options;
}

template <typename Buffer>
void set_channels(Buffer& view,
                  std::vector<std::vector<std::complex<float>>>& storage) {
    view.channel_count = storage.size();
    view.samples_per_channel = storage.front().size();
    for (std::size_t port = 0u; port < storage.size(); ++port) {
        view.channels[port] = storage[port].data();
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        libyunsdr_isac::OpenIsacPhyCodecConfig phy_config;
        phy_config.mode = options.mode;
        phy_config.modulation = options.modulation;
        phy_config.pilot_pattern = options.pilot;
        phy_config.maximum_timing_offset_samples = 512u;
        auto codec = libyunsdr_isac::make_openisac_phy_codec(phy_config);
        const auto profile = codec->mode_profile();

        constexpr std::size_t block_samples = 30720u;
        // Eight complete 2 ms DMA blocks leave enough scheduling margin for
        // Windows host jitter. Four blocks occasionally produced TX timeouts.
        constexpr std::size_t lead_blocks = 8u;
        if (codec->frame_samples_per_port() > block_samples) {
            throw std::runtime_error("PHY frame does not fit one DMA block");
        }

        auto transport = libyunsdr_isac::make_libyunsdr_transport();
        libyunsdr_isac::TransceiverSession session(*transport);
        libyunsdr_isac::SessionConfig session_config;
        session_config.mode = options.mode;
        session_config.pilot_pattern = options.pilot;
        session_config.dma_block_samples = block_samples;
        session_config.tx_lead_samples = lead_blocks * block_samples;
        session_config.radio.uri = options.device;
        session_config.radio.sample_rate_hz = 15.36e6;
        session_config.radio.center_frequency_hz =
            options.frequency_mhz * 1.0e6;
        session_config.radio.bandwidth_hz = 15.36e6;
        session_config.radio.tx_gain_db = options.tx_gain_db;
        session_config.radio.rx_gain_db = options.rx_gain_db;

        const auto identity = session.open_and_configure(session_config);
        std::cout << "Device: " << identity.model
                  << ", firmware=0x" << std::hex << identity.firmware_version
                  << std::dec << ", RF chips=" << identity.rf_chips << '\n';
        std::cout << "Device URI: " << options.device << '\n';
        std::cout << "Mode: " << libyunsdr_isac::phy_mode_name(options.mode)
                  << ", TX gain=" << options.tx_gain_db
                  << ", RX gain=" << options.rx_gain_db
                  << ", frequency=" << options.frequency_mhz << " MHz"
                  << ", rate=15.36 Msps\n";
        session.start();

        std::vector<std::vector<std::complex<float>>> rx(
            profile.rx_ports,
            std::vector<std::complex<float>>(block_samples));
        std::vector<std::vector<std::complex<float>>> tx(
            profile.tx_ports,
            std::vector<std::complex<float>>(block_samples));
        std::vector<std::vector<std::complex<float>>> encoded(
            profile.tx_ports,
            std::vector<std::complex<float>>(codec->frame_samples_per_port()));
        libyunsdr_isac::MutableMultiChannelBuffer rx_view;
        libyunsdr_isac::ConstMultiChannelBuffer tx_view;
        libyunsdr_isac::MutableMultiChannelBuffer encoded_view;
        set_channels(rx_view, rx);
        set_channels(tx_view, tx);
        set_channels(encoded_view, encoded);

        for (unsigned index = 0u; index < 9u; ++index) {
            const auto received = session.receive(rx_view, 1.0);
            if (received.samples_per_channel != block_samples) {
                throw std::runtime_error("RX prime returned a short DMA block");
            }
        }

        unsigned passed = 0u;
        for (unsigned frame = 0u; frame < options.frames; ++frame) {
            std::vector<std::uint8_t> payload(
                std::min<std::size_t>(200u, codec->maximum_payload_bytes()));
            for (std::size_t index = 0u; index < payload.size(); ++index) {
                payload[index] = static_cast<std::uint8_t>(
                    (index * 73u + frame * 29u + 7u) & 0xFFu);
            }
            for (auto& branch : tx) std::fill(branch.begin(), branch.end(), 0.0f);
            const auto frame_samples = codec->encode(
                {payload.data(), payload.size()},
                static_cast<std::uint16_t>(frame + 1u), 0xC057u, encoded_view);
            for (std::size_t port = 0u; port < profile.tx_ports; ++port) {
                std::copy_n(encoded[port].begin(), frame_samples, tx[port].begin());
            }

            const auto anchor = session.receive(rx_view, 1.0);
            const auto tx_timestamp = session.next_transmit_timestamp();
            const auto written = session.transmit(tx_view, tx_timestamp, 1.0);
            if (written != block_samples) {
                throw std::runtime_error("TX returned a short DMA block");
            }

            libyunsdr_isac::ReceiveResult capture;
            for (std::size_t block = 0u; block <= lead_blocks; ++block) {
                capture = session.receive(rx_view, 1.0);
            }
            if (capture.timestamp != tx_timestamp) {
                std::cout << "Timestamp warning: expected " << tx_timestamp
                          << ", captured " << capture.timestamp << '\n';
            }

            libyunsdr_isac::ConstMultiChannelBuffer capture_view;
            set_channels(capture_view, rx);
            double received_power = 0.0;
            float received_peak = 0.0f;
            std::size_t clipped_components = 0u;
            for (const auto& branch : rx) {
                for (const auto sample : branch) {
                    received_power += std::norm(sample);
                    received_peak = std::max(received_peak, std::abs(sample));
                    clipped_components +=
                        std::abs(sample.real()) >= 0.9999f ? 1u : 0u;
                    clipped_components +=
                        std::abs(sample.imag()) >= 0.9999f ? 1u : 0u;
                }
            }
            const auto complex_samples = rx.size() * rx.front().size();
            const float received_rms = static_cast<float>(std::sqrt(
                received_power / static_cast<double>(complex_samples)));
            const double clipping_percent = 100.0 * clipped_components /
                static_cast<double>(2u * complex_samples);
            std::vector<std::uint8_t> decoded(codec->maximum_payload_bytes());
            const auto result = codec->decode(
                capture_view, capture.timestamp, 0xC057u,
                {decoded.data(), decoded.size()});
            const bool payload_ok = result.payload_bytes == payload.size() &&
                std::equal(payload.begin(), payload.end(), decoded.begin());
            const bool okay = result.timing_ok && result.header_ok &&
                result.crc_ok && payload_ok;
            std::cout << "Frame " << (frame + 1u)
                      << ": timing=" << result.timing_ok
                      << " metric=" << result.timing_metric
                      << " header=" << result.header_ok
                      << " crc=" << result.crc_ok
                      << " payload=" << payload_ok
                      << " evm=" << result.evm_percent << "%"
                      << " rx_peak=" << received_peak
                      << " rx_rms=" << received_rms
                      << " clipped=" << clipping_percent << "%"
                      << " anchor_ts=" << anchor.timestamp
                      << " tx_ts=" << tx_timestamp << '\n';
            if (okay) ++passed;
        }

        const auto events = session.poll_events();
        session.close();
        std::cout << "Result: " << passed << '/' << options.frames
                  << " frames passed; TX timeout=" << events.tx_timeouts
                  << ", TX underflow=" << events.tx_underflows
                  << ", RX overflow=" << events.rx_overflows
                  << ", timestamp discontinuity="
                  << events.timestamp_discontinuities << '\n';
        return passed == options.frames ? 0 : 3;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
