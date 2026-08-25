#pragma once

#include "libyunsdr_isac/transceiver.hpp"

#include <cstddef>
#include <cstdint>
#include <complex>
#include <vector>

namespace libyunsdr_isac {

struct ByteView {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0u;
};

struct MutableByteView {
    std::uint8_t* data = nullptr;
    std::size_t capacity = 0u;
};

struct PhyDecodeResult {
    bool timing_ok = false;
    bool header_ok = false;
    bool crc_ok = false;
    std::uint16_t sequence = 0u;
    std::size_t payload_bytes = 0u;
    float timing_metric = 0.0f;
    float cfo_hz = 0.0f;
    float evm_percent = 0.0f;
};

struct PhyChannel2x2Sample {
    std::complex<float> h00{};
    std::complex<float> h01{};
    std::complex<float> h10{};
    std::complex<float> h11{};
};

// Low-rate copy of the most recent hardware receiver diagnostics. The codec
// retains the working buffers; callers request this snapshot only at the GUI
// refresh rate so normal frame processing does not allocate for every plot.
struct PhyTelemetrySnapshot {
    bool valid = false;
    PhyDecodeResult decode{};
    std::uint64_t capture_timestamp = 0u;
    std::size_t timing_offset_samples = 0u;
    float residual_sfo_ppm = 0.0f;
    float noise_variance = 0.0f;
    std::vector<std::complex<float>> receive_waveform_rx0;
    std::vector<std::complex<float>> constellation_ideal;
    std::vector<std::complex<float>> constellation_equalized;
    std::vector<PhyChannel2x2Sample> channel_frequency_response;
};

// OpenISAC owns this implementation. encode() must produce the formal
// over-the-air frame without applying TDL/AWGN/CFO/SFO; decode() consumes IQ
// captured from hardware. Callers preallocate all buffers before streaming.
class IPhyFrameCodec {
public:
    virtual ~IPhyFrameCodec() = default;

    virtual ModeProfile mode_profile() const noexcept = 0;
    virtual PilotPattern pilot_pattern() const noexcept = 0;
    virtual std::size_t frame_samples_per_port() const noexcept = 0;
    virtual std::size_t maximum_payload_bytes() const noexcept = 0;

    virtual std::size_t encode(
        ByteView payload,
        std::uint16_t sequence,
        std::uint32_t pilot_seed,
        const MutableMultiChannelBuffer& output) = 0;
    virtual PhyDecodeResult decode(
        const ConstMultiChannelBuffer& capture,
        std::uint64_t capture_timestamp,
        std::uint32_t pilot_seed,
        MutableByteView payload_output) = 0;
    virtual bool copy_telemetry(PhyTelemetrySnapshot& output) const = 0;
};

class IVideoPacketSource {
public:
    virtual ~IVideoPacketSource() = default;
    virtual std::size_t receive_packet(
        MutableByteView output,
        double timeout_seconds) = 0;
};

class IVideoPacketSink {
public:
    virtual ~IVideoPacketSink() = default;
    virtual void send_packet(ByteView packet) = 0;
};

struct VideoFlowConfig {
    std::size_t tx_queue_frames = 64u;
    std::size_t rx_queue_frames = 64u;
    std::size_t maximum_udp_datagram_bytes = 65507u;
    double io_timeout_seconds = 0.1;
};

}  // namespace libyunsdr_isac
