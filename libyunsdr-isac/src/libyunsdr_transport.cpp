#include "libyunsdr_isac/libyunsdr_transport.hpp"

#include "yunsdr_api_ss.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace libyunsdr_isac {
namespace {

void require_vendor(int rc, const char* operation) {
    if (rc < 0) {
        throw std::runtime_error(
            std::string("libyunsdr operation failed: ") + operation +
            " (rc=" + std::to_string(rc) + ")");
    }
}

std::uint32_t checked_u32(double value, const char* name) {
    if (!std::isfinite(value) || value < 0.0 ||
        value > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::invalid_argument(std::string(name) + " is outside uint32 range");
    }
    return static_cast<std::uint32_t>(std::llround(value));
}

std::uint64_t checked_u64(double value, const char* name) {
    if (!std::isfinite(value) || value < 0.0 ||
        value > static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
        throw std::invalid_argument(std::string(name) + " is outside uint64 range");
    }
    return static_cast<std::uint64_t>(std::llround(value));
}

std::uint32_t tx_attenuation_mdb(double gain_db) {
    if (gain_db < 0.0 || gain_db > 90.0) {
        throw std::invalid_argument("YunSDR TX gain must be in [0, 90] dB");
    }
    return checked_u32((90.0 - gain_db) * 1000.0, "TX attenuation");
}

class LibYunSdrTransport final : public IVendorTransport {
public:
    ~LibYunSdrTransport() override { close(); }

    DeviceIdentity open(const std::string& uri) override {
        if (device_ != nullptr) {
            throw std::logic_error("libyunsdr device is already open");
        }
        yunsdr_set_log_level(LOG_LVL_INFO);
        device_ = yunsdr_open_device(uri.c_str());
        if (device_ == nullptr) {
            throw std::runtime_error("yunsdr_open_device returned null for " + uri);
        }

        std::uint32_t model = 0u;
        std::uint32_t firmware = 0u;
        DEV_CFG configuration = NULLSubDevNULLRF;
        require_vendor(yunsdr_get_model_version(device_, &model),
                       "get model version");
        require_vendor(yunsdr_get_firmware_version(device_, &firmware),
                       "get firmware version");
        require_vendor(yunsdr_get_device_configuration(device_, &configuration),
                       "get device configuration");

        std::ostringstream model_name;
        model_name << "YunSDR model 0x" << std::hex << std::uppercase << model;
        return {model_name.str(), firmware,
                static_cast<std::uint32_t>(configuration),
                device_->nsubdev, device_->nchips};
    }

    void close() noexcept override {
        if (device_ != nullptr) {
            yunsdr_close_device(device_);
            device_ = nullptr;
        }
        rx_iq16_.clear();
        tx_iq16_.clear();
    }

    void configure(const RadioSettings& radio, const ModeProfile&) override {
        require_open();
        const auto sample_rate = checked_u32(radio.sample_rate_hz, "sample rate");
        const auto bandwidth = checked_u32(radio.bandwidth_hz, "bandwidth");
        const auto frequency = checked_u64(radio.center_frequency_hz, "frequency");
        const auto attenuation = tx_attenuation_mdb(radio.tx_gain_db);
        const auto rx_gain = static_cast<std::int32_t>(std::llround(radio.rx_gain_db));

        require_vendor(yunsdr_set_ref_clock(
                           device_, 0u,
                           radio.external_reference ? EXTERNAL_REFERENCE
                                                    : INTERNAL_REFERENCE),
                       "set reference clock");
        require_vendor(yunsdr_set_pps_select(
                           device_, 0u,
                           radio.external_reference ? PPS_EXTERNAL_EN
                                                    : PPS_INTERNAL_EN),
                       "set PPS source");
        require_vendor(yunsdr_set_rx_ant_enable(device_, 0u, 1u),
                       "enable RX antenna");
        require_vendor(yunsdr_set_trxsw_fpga_enable(device_, 0u, 0u),
                       "disable FPGA TRX switch control");
        require_vendor(yunsdr_tx_cyclic_enable(device_, 0u, 0u),
                       "disable cyclic TX");
        require_vendor(yunsdr_set_duplex_select(
                           device_, 0u, radio.fdd ? FDD : TDD),
                       "set duplex mode");

        const auto rf_chips = std::max<std::size_t>(1u, device_->nchips);
        for (std::size_t chip = 0u; chip < rf_chips; ++chip) {
            const auto id = static_cast<std::uint8_t>(chip);
            require_vendor(yunsdr_set_tx_lo_freq(device_, id, frequency),
                           "set TX LO");
            require_vendor(yunsdr_set_tx_sampling_freq(device_, id, sample_rate),
                           "set TX sample rate");
            require_vendor(yunsdr_set_tx_rf_bandwidth(device_, id, bandwidth),
                           "set TX bandwidth");
            require_vendor(yunsdr_set_tx1_attenuation(device_, id, attenuation),
                           "set TX1 attenuation");
            require_vendor(yunsdr_set_tx2_attenuation(device_, id, attenuation),
                           "set TX2 attenuation");

            require_vendor(yunsdr_set_rx_lo_freq(device_, id, frequency),
                           "set RX LO");
            require_vendor(yunsdr_set_rx_sampling_freq(device_, id, sample_rate),
                           "set RX sample rate");
            require_vendor(yunsdr_set_rx_rf_bandwidth(device_, id, bandwidth),
                           "set RX bandwidth");
            require_vendor(yunsdr_set_rx1_gain_control_mode(
                               device_, id, RF_GAIN_MGC),
                           "set RX1 manual gain");
            require_vendor(yunsdr_set_rx2_gain_control_mode(
                               device_, id, RF_GAIN_MGC),
                           "set RX2 manual gain");
            require_vendor(yunsdr_set_rx1_rf_gain(device_, id, rx_gain),
                           "set RX1 gain");
            require_vendor(yunsdr_set_rx2_rf_gain(device_, id, rx_gain),
                           "set RX2 gain");
        }
    }

    void start_streams(const ModeProfile&) override {
        require_open();
        require_vendor(yunsdr_enable_timestamp(device_, 0u, 0u),
                       "disable timestamp");
        std::this_thread::sleep_for(std::chrono::seconds(1));
        require_vendor(yunsdr_enable_timestamp(device_, 0u, 1u),
                       "enable timestamp");
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    void stop_streams() noexcept override {
        if (device_ != nullptr) {
            yunsdr_enable_timestamp(device_, 0u, 0u);
        }
    }

    VendorReadResult read(
        const MutableMultiChannelBuffer& buffers,
        std::uint32_t channel_mask,
        double) override {
        require_open();
        if (buffers.samples_per_channel >
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
            throw std::invalid_argument("RX request is too large for libyunsdr");
        }
        rx_iq16_.resize(buffers.channel_count);
        std::array<void*, maximum_phase1_ports> pointers{{nullptr, nullptr}};
        for (std::size_t channel = 0u; channel < buffers.channel_count; ++channel) {
            rx_iq16_[channel].resize(buffers.samples_per_channel * 2u);
            pointers[channel] = rx_iq16_[channel].data();
        }
        std::uint64_t timestamp = 0u;
        const auto count = static_cast<std::uint32_t>(buffers.samples_per_channel);
        require_vendor(yunsdr_read_samples_multiport(
                           device_, pointers.data(), count, channel_mask, &timestamp),
                       "read samples multiport");
        constexpr float scale = 1.0f / 32768.0f;
        for (std::size_t channel = 0u; channel < buffers.channel_count; ++channel) {
            for (std::size_t index = 0u; index < buffers.samples_per_channel; ++index) {
                buffers.channels[channel][index] = {
                    static_cast<float>(rx_iq16_[channel][2u * index]) * scale,
                    static_cast<float>(rx_iq16_[channel][2u * index + 1u]) * scale};
            }
        }
        return {buffers.samples_per_channel, timestamp};
    }

    std::size_t write(
        const ConstMultiChannelBuffer& buffers,
        std::uint32_t channel_mask,
        std::uint64_t timestamp,
        double) override {
        require_open();
        if (buffers.samples_per_channel >
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
            throw std::invalid_argument("TX request is too large for libyunsdr");
        }
        tx_iq16_.resize(buffers.channel_count);
        std::array<const void*, maximum_phase1_ports> pointers{{nullptr, nullptr}};
        for (std::size_t channel = 0u; channel < buffers.channel_count; ++channel) {
            auto& packed = tx_iq16_[channel];
            packed.resize(buffers.samples_per_channel * 2u);
            for (std::size_t index = 0u; index < buffers.samples_per_channel; ++index) {
                const auto sample = buffers.channels[channel][index];
                packed[2u * index] = quantize(sample.real());
                packed[2u * index + 1u] = quantize(sample.imag());
            }
            pointers[channel] = packed.data();
        }
        const auto count = static_cast<std::uint32_t>(buffers.samples_per_channel);
        require_vendor(yunsdr_write_samples_multiport(
                           device_, pointers.data(), count, channel_mask,
                           timestamp, 0u),
                       "write samples multiport");
        return buffers.samples_per_channel;
    }

    ChannelEventCounters read_events(const ModeProfile& mode) override {
        require_open();
        ChannelEventCounters result;
        for (std::size_t channel = 0u; channel < mode.tx_ports; ++channel) {
            result.tx_timeouts += event_count(
                TX_CHANNEL_TIMEOUT, static_cast<std::uint8_t>(channel + 1u));
            result.tx_underflows += event_count(
                TX_CHANNEL_UNDERFLOW, static_cast<std::uint8_t>(channel + 1u));
        }
        for (std::size_t channel = 0u; channel < mode.rx_ports; ++channel) {
            result.rx_timeouts += event_count(
                RX_CHANNEL_TIMEOUT, static_cast<std::uint8_t>(channel + 1u));
            result.rx_overflows += event_count(
                RX_CHANNEL_OVERFLOW, static_cast<std::uint8_t>(channel + 1u));
        }
        return result;
    }

private:
    YUNSDR_DESCRIPTOR* device_ = nullptr;
    std::vector<std::vector<std::int16_t>> rx_iq16_;
    std::vector<std::vector<std::int16_t>> tx_iq16_;

    void require_open() const {
        if (device_ == nullptr) {
            throw std::logic_error("libyunsdr device is not open");
        }
    }

    static std::int16_t quantize(float value) noexcept {
        const float bounded = std::max(-1.0f, std::min(0.9999695f, value));
        return static_cast<std::int16_t>(std::lrint(bounded * 32768.0f));
    }

    std::uint32_t event_count(CHANNEL_EVENT event, std::uint8_t channel) {
        std::uint32_t count = 0u;
        require_vendor(yunsdr_get_channel_event(device_, event, channel, &count),
                       "get channel event");
        return count;
    }
};

}  // namespace

std::unique_ptr<IVendorTransport> make_libyunsdr_transport() {
    return std::unique_ptr<IVendorTransport>(new LibYunSdrTransport());
}

}  // namespace libyunsdr_isac
