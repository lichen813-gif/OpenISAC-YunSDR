#include "libyunsdr_isac/phy_pipeline.hpp"

#include <array>
#include <complex>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace {

using namespace libyunsdr_isac;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class FakeVendorTransport final : public IVendorTransport {
public:
    DeviceIdentity open(const std::string& uri) override {
        ++open_calls;
        opened_uri = uri;
        return {"Y240", 0x26010001u, 0x1020u, 1u, 1u};
    }

    void close() noexcept override { ++close_calls; }

    void configure(
        const RadioSettings& radio,
        const ModeProfile& mode) override {
        ++configure_calls;
        configured_radio = radio;
        configured_mode = mode;
    }

    void start_streams(const ModeProfile& mode) override {
        ++start_calls;
        started_mode = mode;
    }

    void stop_streams() noexcept override { ++stop_calls; }

    VendorReadResult read(
        const MutableMultiChannelBuffer& buffers,
        std::uint32_t channel_mask,
        double) override {
        ++read_calls;
        last_rx_mask = channel_mask;
        for (std::size_t channel = 0u; channel < buffers.channel_count; ++channel) {
            for (std::size_t sample = 0u;
                 sample < buffers.samples_per_channel; ++sample) {
                buffers.channels[channel][sample] = {
                    static_cast<float>(channel + 1u),
                    static_cast<float>(sample)};
            }
        }
        const auto timestamp = next_rx_timestamp;
        if (advance_timestamp) {
            next_rx_timestamp += buffers.samples_per_channel;
        }
        return {buffers.samples_per_channel, timestamp};
    }

    std::size_t write(
        const ConstMultiChannelBuffer& buffers,
        std::uint32_t channel_mask,
        std::uint64_t timestamp,
        double) override {
        ++write_calls;
        last_tx_mask = channel_mask;
        last_tx_timestamp = timestamp;
        return buffers.samples_per_channel;
    }

    ChannelEventCounters read_events(const ModeProfile&) override {
        return events;
    }

    int open_calls = 0;
    int close_calls = 0;
    int configure_calls = 0;
    int start_calls = 0;
    int stop_calls = 0;
    int read_calls = 0;
    int write_calls = 0;
    std::string opened_uri;
    RadioSettings configured_radio{};
    ModeProfile configured_mode{};
    ModeProfile started_mode{};
    std::uint32_t last_rx_mask = 0u;
    std::uint32_t last_tx_mask = 0u;
    std::uint64_t last_tx_timestamp = 0u;
    std::uint64_t next_rx_timestamp = 100u;
    bool advance_timestamp = true;
    ChannelEventCounters events{};
};

void check_profiles() {
    const auto siso = make_mode_profile(PhyMode::siso);
    require(siso.tx_ports == 1u && siso.rx_ports == 1u,
            "SISO port mapping failed");
    require(siso.spatial_rank == 1u && siso.tx_channel_mask == 0x1u,
            "SISO rank/mask failed");

    const auto mimo = make_mode_profile(PhyMode::spatial_2x2);
    require(mimo.tx_ports == 2u && mimo.rx_ports == 2u,
            "2x2 port mapping failed");
    require(mimo.spatial_rank == 2u && mimo.tx_channel_mask == 0x3u,
            "2x2 rank/mask failed");

    const auto stbc = make_mode_profile(PhyMode::alamouti_stbc_2x2);
    require(stbc.tx_ports == 2u && stbc.rx_ports == 2u,
            "STBC port mapping failed");
    require(stbc.spatial_rank == 1u &&
                stbc.scheme == SpatialScheme::alamouti_stbc,
            "STBC scheme/rank failed");
}

void check_siso_lifecycle() {
    FakeVendorTransport vendor;
    TransceiverSession session(vendor);
    SessionConfig config;
    config.mode = PhyMode::siso;
    config.tx_lead_samples = 32u;

    const auto identity = session.open_and_configure(config);
    require(identity.model == "Y240", "device identity was not retained");
    require(vendor.opened_uri == "pcies:0.0", "wrong default URI");
    session.start();

    std::array<IqSample, 64u> rx{};
    std::array<IqSample, 64u> tx{};
    MutableMultiChannelBuffer rx_view;
    rx_view.channels[0] = rx.data();
    rx_view.channel_count = 1u;
    rx_view.samples_per_channel = rx.size();
    ConstMultiChannelBuffer tx_view;
    tx_view.channels[0] = tx.data();
    tx_view.channel_count = 1u;
    tx_view.samples_per_channel = tx.size();

    bool rejected_unprimed_tx = false;
    try {
        (void)session.transmit(tx_view, 0u, 0.1);
    } catch (const std::logic_error&) {
        rejected_unprimed_tx = true;
    }
    require(rejected_unprimed_tx, "TX was not gated behind RX priming");

    const auto first = session.receive(rx_view, 0.1);
    require(first.timestamp == 100u && first.timestamp_continuous,
            "first RX block metadata failed");
    require(vendor.last_rx_mask == 0x1u, "SISO RX mask failed");
    require(session.transmit(tx_view, 0u, 0.1) == tx.size(),
            "SISO TX sample count failed");
    require(vendor.last_tx_mask == 0x1u, "SISO TX mask failed");
    require(vendor.last_tx_timestamp == 196u,
            "derived timed TX timestamp failed");

    const auto second = session.receive(rx_view, 0.1);
    require(second.timestamp_continuous, "continuous timestamp was rejected");
    vendor.next_rx_timestamp = 1000u;
    const auto third = session.receive(rx_view, 0.1);
    require(!third.timestamp_continuous,
            "timestamp discontinuity was not detected");
    const auto events = session.poll_events();
    require(events.timestamp_discontinuities == 1u,
            "software timestamp event count failed");

    session.stop();
    session.close();
    require(vendor.stop_calls == 1 && vendor.close_calls == 1,
            "ordered shutdown failed");
}

void check_two_port_mode(PhyMode mode, unsigned expected_rank) {
    FakeVendorTransport vendor;
    TransceiverSession session(vendor);
    SessionConfig config;
    config.mode = mode;
    session.open_and_configure(config);
    session.start();

    std::array<IqSample, 32u> rx0{};
    std::array<IqSample, 32u> rx1{};
    std::array<IqSample, 32u> tx0{};
    std::array<IqSample, 32u> tx1{};
    MutableMultiChannelBuffer rx_view;
    rx_view.channels = {{rx0.data(), rx1.data()}};
    rx_view.channel_count = 2u;
    rx_view.samples_per_channel = rx0.size();
    ConstMultiChannelBuffer tx_view;
    tx_view.channels = {{tx0.data(), tx1.data()}};
    tx_view.channel_count = 2u;
    tx_view.samples_per_channel = tx0.size();

    (void)session.receive(rx_view, 0.1);
    (void)session.transmit(tx_view, session.next_transmit_timestamp(), 0.1);
    require(vendor.configured_mode.spatial_rank == expected_rank,
            "two-port rank configuration failed");
    require(vendor.last_rx_mask == 0x3u && vendor.last_tx_mask == 0x3u,
            "two-port channel masks failed");
    session.close();
}

}  // namespace

int main() {
    try {
        check_profiles();
        check_siso_lifecycle();
        check_two_port_mode(PhyMode::spatial_2x2, 2u);
        check_two_port_mode(PhyMode::alamouti_stbc_2x2, 1u);
        return 0;
    } catch (const std::exception&) {
        return 1;
    }
}
