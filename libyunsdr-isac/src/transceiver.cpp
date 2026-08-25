#include "libyunsdr_isac/transceiver.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace libyunsdr_isac {
namespace {

void validate_radio_settings(const SessionConfig& config) {
    const auto& radio = config.radio;
    if (radio.uri.empty() || !std::isfinite(radio.sample_rate_hz) ||
        radio.sample_rate_hz <= 0.0 ||
        !std::isfinite(radio.center_frequency_hz) ||
        radio.center_frequency_hz <= 0.0 ||
        !std::isfinite(radio.bandwidth_hz) || radio.bandwidth_hz <= 0.0 ||
        !std::isfinite(radio.tx_gain_db) ||
        !std::isfinite(radio.rx_gain_db) ||
        config.dma_block_samples == 0u) {
        throw std::invalid_argument("invalid YunSDR session configuration");
    }
}

void require_timeout(double timeout_seconds) {
    if (!std::isfinite(timeout_seconds) || timeout_seconds < 0.0) {
        throw std::invalid_argument("timeout must be finite and non-negative");
    }
}

}  // namespace

ModeProfile make_mode_profile(PhyMode mode) {
    switch (mode) {
        case PhyMode::siso:
            return {mode, SpatialScheme::spatial_multiplexing,
                    1u, 1u, 1u, 0x1u, 0x1u};
        case PhyMode::spatial_2x2:
            return {mode, SpatialScheme::spatial_multiplexing,
                    2u, 2u, 2u, 0x3u, 0x3u};
        case PhyMode::alamouti_stbc_2x2:
            return {mode, SpatialScheme::alamouti_stbc,
                    2u, 2u, 1u, 0x3u, 0x3u};
    }
    throw std::invalid_argument("unsupported phase-1 PHY mode");
}

const char* phy_mode_name(PhyMode mode) noexcept {
    switch (mode) {
        case PhyMode::siso: return "siso";
        case PhyMode::spatial_2x2: return "2x2-spatial";
        case PhyMode::alamouti_stbc_2x2: return "2x2-stbc";
    }
    return "unknown";
}

TransceiverSession::TransceiverSession(IVendorTransport& transport) noexcept
    : transport_(transport) {}

TransceiverSession::~TransceiverSession() {
    close();
}

DeviceIdentity TransceiverSession::open_and_configure(
    const SessionConfig& config) {
    if (state_ != SessionState::closed) {
        throw std::logic_error("YunSDR session is already open");
    }
    validate_radio_settings(config);

    config_ = config;
    mode_profile_ = make_mode_profile(config.mode);
    try {
        identity_ = transport_.open(config.radio.uri);
        transport_.configure(config.radio, mode_profile_);
    } catch (...) {
        transport_.close();
        identity_ = {};
        throw;
    }

    state_ = SessionState::configured;
    receive_primed_.store(false);
    {
        const std::lock_guard<std::mutex> lock(receive_metadata_mutex_);
        have_last_receive_ = false;
        timestamp_discontinuities_ = 0u;
    }
    return identity_;
}

void TransceiverSession::start() {
    if (state_ != SessionState::configured &&
        state_ != SessionState::stopped) {
        throw std::logic_error("YunSDR session must be configured before start");
    }
    transport_.start_streams(mode_profile_);
    state_ = SessionState::streaming;
    receive_primed_.store(false);
    {
        const std::lock_guard<std::mutex> lock(receive_metadata_mutex_);
        have_last_receive_ = false;
    }
}

void TransceiverSession::validate_receive_buffer(
    const MutableMultiChannelBuffer& buffers) const {
    if (buffers.channel_count != mode_profile_.rx_ports ||
        buffers.samples_per_channel == 0u) {
        throw std::invalid_argument("RX buffer does not match PHY port profile");
    }
    for (std::size_t channel = 0u; channel < buffers.channel_count; ++channel) {
        if (buffers.channels[channel] == nullptr) {
            throw std::invalid_argument("RX buffer contains a null channel");
        }
    }
}

void TransceiverSession::validate_transmit_buffer(
    const ConstMultiChannelBuffer& buffers) const {
    if (buffers.channel_count != mode_profile_.tx_ports ||
        buffers.samples_per_channel == 0u) {
        throw std::invalid_argument("TX buffer does not match PHY port profile");
    }
    for (std::size_t channel = 0u; channel < buffers.channel_count; ++channel) {
        if (buffers.channels[channel] == nullptr) {
            throw std::invalid_argument("TX buffer contains a null channel");
        }
    }
}

ReceiveResult TransceiverSession::receive(
    const MutableMultiChannelBuffer& buffers,
    double timeout_seconds) {
    if (state_ != SessionState::streaming) {
        throw std::logic_error("RX requires a streaming YunSDR session");
    }
    validate_receive_buffer(buffers);
    require_timeout(timeout_seconds);

    const auto result = transport_.read(
        buffers, mode_profile_.rx_channel_mask, timeout_seconds);
    if (result.samples_per_channel > buffers.samples_per_channel) {
        throw std::runtime_error("vendor RX returned more samples than requested");
    }

    ReceiveResult output{
        result.samples_per_channel, result.timestamp, true};
    if (result.samples_per_channel != 0u) {
        {
            const std::lock_guard<std::mutex> lock(receive_metadata_mutex_);
            if (have_last_receive_) {
                const auto expected = last_receive_timestamp_ +
                    static_cast<std::uint64_t>(last_receive_samples_);
                output.timestamp_continuous = result.timestamp == expected;
                if (!output.timestamp_continuous) {
                    ++timestamp_discontinuities_;
                }
            }
            last_receive_timestamp_ = result.timestamp;
            last_receive_samples_ = result.samples_per_channel;
            have_last_receive_ = true;
        }
        receive_primed_.store(true);
    }
    return output;
}

std::uint64_t TransceiverSession::next_transmit_timestamp() const {
    if (!receive_primed_.load()) {
        throw std::logic_error("RX must be primed before deriving a TX timestamp");
    }
    const std::lock_guard<std::mutex> lock(receive_metadata_mutex_);
    if (!have_last_receive_) {
        throw std::logic_error("RX timestamp is not available");
    }
    const auto base = last_receive_timestamp_ +
        static_cast<std::uint64_t>(last_receive_samples_);
    if (config_.tx_lead_samples >
        std::numeric_limits<std::uint64_t>::max() - base) {
        throw std::overflow_error("TX timestamp overflow");
    }
    return base + static_cast<std::uint64_t>(config_.tx_lead_samples);
}

std::size_t TransceiverSession::transmit(
    const ConstMultiChannelBuffer& buffers,
    std::uint64_t timestamp,
    double timeout_seconds) {
    if (state_ != SessionState::streaming) {
        throw std::logic_error("TX requires a streaming YunSDR session");
    }
    validate_transmit_buffer(buffers);
    require_timeout(timeout_seconds);
    if (config_.require_receive_before_transmit && !receive_primed_.load()) {
        throw std::logic_error("RX must be primed before TX");
    }
    if (timestamp == 0u && config_.require_receive_before_transmit) {
        timestamp = next_transmit_timestamp();
    }

    const auto written = transport_.write(
        buffers, mode_profile_.tx_channel_mask, timestamp, timeout_seconds);
    if (written > buffers.samples_per_channel) {
        throw std::runtime_error("vendor TX wrote more samples than requested");
    }
    return written;
}

ChannelEventCounters TransceiverSession::poll_events() {
    if (state_ == SessionState::closed) {
        throw std::logic_error("cannot read events from a closed YunSDR session");
    }
    auto events = transport_.read_events(mode_profile_);
    {
        const std::lock_guard<std::mutex> lock(receive_metadata_mutex_);
        events.timestamp_discontinuities += timestamp_discontinuities_;
    }
    return events;
}

void TransceiverSession::stop() noexcept {
    if (state_ == SessionState::streaming) {
        transport_.stop_streams();
        state_ = SessionState::stopped;
    }
}

void TransceiverSession::close() noexcept {
    stop();
    if (state_ != SessionState::closed) {
        transport_.close();
        state_ = SessionState::closed;
        identity_ = {};
        receive_primed_.store(false);
        const std::lock_guard<std::mutex> lock(receive_metadata_mutex_);
        have_last_receive_ = false;
    }
}

}  // namespace libyunsdr_isac
