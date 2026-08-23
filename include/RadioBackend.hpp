#ifndef RADIO_BACKEND_HPP
#define RADIO_BACKEND_HPP

// Radio I/O abstraction layer.
//
// IDevice / ITxStream / IRxStream are the backend-independent interfaces the
// engines drive. The current tree intentionally ships only SimBackend, backed
// by shared memory and ChannelSimulator. This boundary is retained for a future
// libyunsdr backend; the previous vendor-specific implementation and dependency
// have been removed.

#include <memory>
#include <string>

#include "RadioTypes.hpp"

namespace radio {

// ---------------------------------------------------------------------------
// Streams
// ---------------------------------------------------------------------------

class ITxStream {
public:
    virtual ~ITxStream() = default;

    virtual size_t num_channels() const = 0;
    virtual size_t max_num_samps() const = 0;

    // Single-channel hot path. `buff` may be null when `nsamps == 0` (e.g. an
    // end-of-burst marker).
    virtual size_t send(const sample_t* buff, size_t nsamps,
                        const TxMetadata& metadata, double timeout) = 0;

    // Multi-channel overload; defaults to the single-channel path (channel 0).
    virtual size_t send(const sample_t* const* buffs, size_t nsamps,
                        const TxMetadata& metadata, double timeout) {
        return send(buffs[0], nsamps, metadata, timeout);
    }

    virtual bool recv_async_msg(AsyncMetadata& metadata, double timeout) = 0;
};

class IRxStream {
public:
    virtual ~IRxStream() = default;

    virtual size_t num_channels() const = 0;
    virtual size_t max_num_samps() const = 0;

    // Single-channel hot path.
    virtual size_t recv(sample_t* buff, size_t nsamps, RxMetadata& metadata,
                        double timeout = 0.1, bool one_packet = false) = 0;

    // Multi-channel overload; defaults to the single-channel path (channel 0).
    virtual size_t recv(sample_t* const* buffs, size_t nsamps, RxMetadata& metadata,
                        double timeout, bool one_packet = false) {
        return recv(buffs[0], nsamps, metadata, timeout, one_packet);
    }

    virtual void issue_stream_cmd(const StreamCmd& cmd) = 0;
};

using ITxStreamPtr = std::shared_ptr<ITxStream>;
using IRxStreamPtr = std::shared_ptr<IRxStream>;

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------

// Backend feature flags. Engines branch on these instead of testing the concrete
// backend, so every "is this the simulator?" check becomes a capability query.
enum class Capability {
    TimedTx,        // honors tx_metadata time_spec / timed bursts
    FreeRunningClock, // device time advances independently of streamed samples
    AsyncTxEvents,  // produces TX async messages (underflow / seq error / ...)
    StreamRestart,  // RX stream can be stopped + timed-restarted
    HardwareGain,   // set_*_gain / get_*_gain_range act on a device
    RfDspTune,      // supports manual RF/DSP split retuning
};

class IDevice {
public:
    virtual ~IDevice() = default;

    virtual bool supports(Capability cap) const = 0;

    // Backend clock.
    virtual TimeSpec time_now() const = 0;
    virtual double master_clock_rate() const = 0;

    // Rates / bandwidth.
    virtual void set_tx_rate(double /*rate*/) {}
    virtual void set_rx_rate(double /*rate*/) {}
    virtual double get_tx_rate(size_t chan = 0) const = 0;
    virtual double get_rx_rate(size_t chan = 0) const { return get_tx_rate(chan); }
    virtual void set_tx_bandwidth(double /*bw*/, size_t /*chan*/ = 0) {}
    virtual void set_rx_bandwidth(double /*bw*/, size_t /*chan*/ = 0) {}
    virtual size_t get_tx_num_channels() const { return 1; }
    virtual size_t get_rx_num_channels() const { return 1; }

    // Gain. No-ops on backends without HardwareGain.
    virtual void set_tx_gain(double /*gain*/, size_t /*chan*/ = 0) {}
    virtual void set_rx_gain(double /*gain*/, size_t /*chan*/ = 0) {}
    virtual GainRange get_tx_gain_range(size_t /*chan*/ = 0) const { return GainRange{}; }
    virtual GainRange get_rx_gain_range(size_t /*chan*/ = 0) const { return GainRange{}; }

    // Tuning.
    virtual TuneResult set_tx_freq(const TuneRequest& req, size_t chan = 0) = 0;
    virtual TuneResult set_rx_freq(const TuneRequest& req, size_t chan = 0) = 0;
    // Receiver-side comm frequency correction used by the simulator's DSP path.
    // Other backends may retune via set_rx_freq instead.
    virtual void set_rx_freq_correction(double /*hz*/) {}
    virtual double rx_freq_correction() const { return 0.0; }

    // Streamers.
    virtual ITxStreamPtr get_tx_stream(const StreamArgs& args) = 0;
    virtual IRxStreamPtr get_rx_stream(const StreamArgs& args) = 0;

    // Liveness. The simulator reflects the hub's running flag so engines can
    // detect a paused or stopped hub.
    virtual bool running() const { return true; }
};

using IDevicePtr = std::shared_ptr<IDevice>;

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

// Everything a backend needs to materialize one device. Callers (BS/UE/sensing)
// populate the fields relevant to the selected backend; the rest are ignored.
struct DeviceConfig {
    std::string backend = "sim";

    std::string sim_session;      // ChannelSimulator session name
    double sim_tick_rate = 0.0;   // sample rate (Hz) reported as the radio clock
    double sim_center_freq = 0.0; // center freq used to synthesize tune results
    bool sim_predictive_delay = false; // validate simulator CFO/SRO consistency for UE prediction
};

// Create the configured device. The current implementation accepts only
// backend="sim"; this factory is the future libyunsdr insertion point.
IDevicePtr make_device(const DeviceConfig& cfg);

// Best-effort native RT-thread priority helper. Failure is intentionally benign
// so unprivileged simulator runs still work.
void set_thread_priority(float priority = 0.5f, bool realtime = true);

}  // namespace radio

#endif  // RADIO_BACKEND_HPP
