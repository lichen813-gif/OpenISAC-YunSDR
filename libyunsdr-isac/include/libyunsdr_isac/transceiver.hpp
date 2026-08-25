#pragma once

#include <array>
#include <atomic>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <mutex>

namespace libyunsdr_isac {

constexpr std::size_t maximum_phase1_ports = 2u;
using IqSample = std::complex<float>;

enum class PhyMode {
    siso,
    spatial_2x2,
    alamouti_stbc_2x2,
};

enum class SpatialScheme {
    spatial_multiplexing,
    alamouti_stbc,
};

enum class PilotPattern {
    fdm,
    nr_dmrs,
};

struct ModeProfile {
    PhyMode mode = PhyMode::siso;
    SpatialScheme scheme = SpatialScheme::spatial_multiplexing;
    std::size_t tx_ports = 1u;
    std::size_t rx_ports = 1u;
    unsigned spatial_rank = 1u;
    std::uint32_t tx_channel_mask = 0x1u;
    std::uint32_t rx_channel_mask = 0x1u;
};

ModeProfile make_mode_profile(PhyMode mode);
const char* phy_mode_name(PhyMode mode) noexcept;

struct RadioSettings {
    std::string uri = "pcies:0.0";
    double sample_rate_hz = 15.36e6;
    double center_frequency_hz = 2.45e9;
    double bandwidth_hz = 15.36e6;
    double tx_gain_db = 0.0;
    double rx_gain_db = 6.0;
    bool external_reference = false;
    bool fdd = true;
};

struct SessionConfig {
    RadioSettings radio{};
    PhyMode mode = PhyMode::siso;
    PilotPattern pilot_pattern = PilotPattern::fdm;
    std::uint32_t pilot_seed = 0xC057u;
    std::size_t dma_block_samples = 7680u;
    std::size_t tx_lead_samples = 0u;
    bool require_receive_before_transmit = true;
};

struct DeviceIdentity {
    std::string model;
    std::uint32_t firmware_version = 0u;
    std::uint32_t device_configuration = 0u;
    std::size_t subdevices = 0u;
    std::size_t rf_chips = 0u;
};

struct ChannelEventCounters {
    std::uint64_t tx_timeouts = 0u;
    std::uint64_t tx_underflows = 0u;
    std::uint64_t rx_timeouts = 0u;
    std::uint64_t rx_overflows = 0u;
    std::uint64_t timestamp_discontinuities = 0u;
};

struct MutableMultiChannelBuffer {
    std::array<IqSample*, maximum_phase1_ports> channels{{nullptr, nullptr}};
    std::size_t channel_count = 0u;
    std::size_t samples_per_channel = 0u;
};

struct ConstMultiChannelBuffer {
    std::array<const IqSample*, maximum_phase1_ports> channels{{nullptr, nullptr}};
    std::size_t channel_count = 0u;
    std::size_t samples_per_channel = 0u;
};

struct VendorReadResult {
    std::size_t samples_per_channel = 0u;
    std::uint64_t timestamp = 0u;
};

struct ReceiveResult {
    std::size_t samples_per_channel = 0u;
    std::uint64_t timestamp = 0u;
    bool timestamp_continuous = true;
};

// Private vendor implementations convert between complex<float> and the
// packed IQ16 wire format. Vendor headers do not cross this boundary.
class IVendorTransport {
public:
    virtual ~IVendorTransport() = default;

    virtual DeviceIdentity open(const std::string& uri) = 0;
    virtual void close() noexcept = 0;
    virtual void configure(
        const RadioSettings& radio,
        const ModeProfile& mode) = 0;
    virtual void start_streams(const ModeProfile& mode) = 0;
    virtual void stop_streams() noexcept = 0;

    virtual VendorReadResult read(
        const MutableMultiChannelBuffer& buffers,
        std::uint32_t channel_mask,
        double timeout_seconds) = 0;
    virtual std::size_t write(
        const ConstMultiChannelBuffer& buffers,
        std::uint32_t channel_mask,
        std::uint64_t timestamp,
        double timeout_seconds) = 0;
    virtual ChannelEventCounters read_events(const ModeProfile& mode) = 0;
};

enum class SessionState {
    closed,
    configured,
    streaming,
    stopped,
};

// Lifecycle shared by the C++ loopback tool and the future VLC/UDP hardware
// bridge. Configuration and start/stop are externally synchronized. The hot
// path permits one RX thread and one TX thread.
class TransceiverSession {
public:
    explicit TransceiverSession(IVendorTransport& transport) noexcept;
    ~TransceiverSession();

    TransceiverSession(const TransceiverSession&) = delete;
    TransceiverSession& operator=(const TransceiverSession&) = delete;

    DeviceIdentity open_and_configure(const SessionConfig& config);
    void start();
    ReceiveResult receive(
        const MutableMultiChannelBuffer& buffers,
        double timeout_seconds);
    std::size_t transmit(
        const ConstMultiChannelBuffer& buffers,
        std::uint64_t timestamp,
        double timeout_seconds);
    ChannelEventCounters poll_events();
    void stop() noexcept;
    void close() noexcept;

    SessionState state() const noexcept { return state_; }
    const SessionConfig& config() const noexcept { return config_; }
    const ModeProfile& mode_profile() const noexcept { return mode_profile_; }
    const DeviceIdentity& identity() const noexcept { return identity_; }
    bool receive_primed() const noexcept { return receive_primed_.load(); }
    std::uint64_t next_transmit_timestamp() const;

private:
    IVendorTransport& transport_;
    SessionState state_ = SessionState::closed;
    SessionConfig config_{};
    ModeProfile mode_profile_{};
    DeviceIdentity identity_{};
    std::atomic<bool> receive_primed_{false};
    mutable std::mutex receive_metadata_mutex_;
    bool have_last_receive_ = false;
    std::uint64_t last_receive_timestamp_ = 0u;
    std::size_t last_receive_samples_ = 0u;
    std::uint64_t timestamp_discontinuities_ = 0u;

    void validate_receive_buffer(const MutableMultiChannelBuffer& buffers) const;
    void validate_transmit_buffer(const ConstMultiChannelBuffer& buffers) const;
};

}  // namespace libyunsdr_isac
