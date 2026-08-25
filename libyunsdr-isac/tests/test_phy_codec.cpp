#include "libyunsdr_isac/openisac_phy_codec.hpp"

#include <algorithm>
#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_mode(libyunsdr_isac::PhyMode mode) {
    libyunsdr_isac::OpenIsacPhyCodecConfig config;
    config.mode = mode;
    config.pilot_pattern = libyunsdr_isac::PilotPattern::nr_dmrs;
    config.modulation = libyunsdr_isac::PhyModulation::qam64;
    config.maximum_timing_offset_samples = 64u;
    auto codec = libyunsdr_isac::make_openisac_phy_codec(config);
    const auto profile = codec->mode_profile();
    const auto frame_samples = codec->frame_samples_per_port();

    std::vector<std::uint8_t> payload(
        std::min<std::size_t>(257u, codec->maximum_payload_bytes()));
    for (std::size_t index = 0u; index < payload.size(); ++index) {
        payload[index] = static_cast<std::uint8_t>((index * 71u + 19u) & 0xFFu);
    }

    std::vector<std::vector<std::complex<float>>> tx(
        profile.tx_ports,
        std::vector<std::complex<float>>(frame_samples));
    libyunsdr_isac::MutableMultiChannelBuffer tx_view;
    tx_view.channel_count = profile.tx_ports;
    tx_view.samples_per_channel = frame_samples;
    for (std::size_t port = 0u; port < profile.tx_ports; ++port) {
        tx_view.channels[port] = tx[port].data();
    }
    const auto encoded = codec->encode(
        {payload.data(), payload.size()}, 91u, 0xC057u, tx_view);
    require(encoded == frame_samples, "codec encoded the wrong sample count");

    constexpr std::size_t timing_offset = 20u;
    constexpr std::size_t tail = 32u;
    std::vector<std::vector<std::complex<float>>> rx(
        profile.rx_ports,
        std::vector<std::complex<float>>(timing_offset + frame_samples + tail));
    for (std::size_t port = 0u; port < profile.rx_ports; ++port) {
        std::copy(tx[port].begin(), tx[port].end(),
                  rx[port].begin() + timing_offset);
    }
    libyunsdr_isac::ConstMultiChannelBuffer rx_view;
    rx_view.channel_count = profile.rx_ports;
    rx_view.samples_per_channel = rx.front().size();
    for (std::size_t port = 0u; port < profile.rx_ports; ++port) {
        rx_view.channels[port] = rx[port].data();
    }
    std::vector<std::uint8_t> decoded(codec->maximum_payload_bytes());
    const auto result = codec->decode(
        rx_view, 5000u, 0xC057u, {decoded.data(), decoded.size()});
    require(result.timing_ok && result.header_ok && result.crc_ok,
            "codec ideal waveform decode failed");
    require(result.sequence == 91u, "codec sequence mismatch");
    require(result.payload_bytes == payload.size(), "codec payload size mismatch");
    require(std::equal(payload.begin(), payload.end(), decoded.begin()),
            "codec payload content mismatch");
    libyunsdr_isac::PhyTelemetrySnapshot telemetry;
    require(codec->copy_telemetry(telemetry) && telemetry.valid,
            "codec telemetry snapshot is unavailable");
    require(telemetry.capture_timestamp == 5000u,
            "codec telemetry timestamp mismatch");
    require(!telemetry.receive_waveform_rx0.empty(),
            "codec telemetry waveform is empty");
    require(!telemetry.constellation_equalized.empty() &&
            telemetry.constellation_equalized.size() ==
                telemetry.constellation_ideal.size(),
            "codec telemetry constellation is invalid");
    require(telemetry.channel_frequency_response.size() == 1024u,
            "codec telemetry channel response is invalid");
}

}  // namespace

int main() {
    try {
        test_mode(libyunsdr_isac::PhyMode::siso);
        test_mode(libyunsdr_isac::PhyMode::spatial_2x2);
        test_mode(libyunsdr_isac::PhyMode::alamouti_stbc_2x2);
        std::cout << "OpenISAC SISO/2x2/STBC codec closure passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
