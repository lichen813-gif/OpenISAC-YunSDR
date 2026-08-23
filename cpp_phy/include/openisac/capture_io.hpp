#pragma once

#include "openisac/dynamic_link.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace openisac {

class DynamicLinkPipeline;

// Cumulative counters for the capture boundary. source_sequence_gaps describe
// gaps already present at the producer input. consumer_sequence_gaps describe
// gaps visible after the ring, including frames rejected on overflow.
struct CaptureRingStatistics {
    std::uint64_t push_attempts = 0u;
    std::uint64_t frames_pushed = 0u;
    std::uint64_t frames_popped = 0u;
    std::uint64_t overflow_drops = 0u;
    std::uint64_t source_sequence_gaps = 0u;
    std::uint64_t consumer_sequence_gaps = 0u;
    std::uint64_t out_of_order_frames = 0u;
    std::uint64_t timestamp_regressions = 0u;
    std::size_t high_watermark = 0u;
};

// Thread-safe single-capture boundary with fixed slot and sample capacity.
// try_push() rejects the newest frame when full; it never overwrites a frame
// that may be in use by the receiver.
class CaptureRingBuffer {
public:
    CaptureRingBuffer(
        std::size_t capacity_frames,
        std::size_t antenna_count = 2u,
        std::size_t maximum_samples_per_antenna = 4096u);
    ~CaptureRingBuffer();

    CaptureRingBuffer(const CaptureRingBuffer&) = delete;
    CaptureRingBuffer& operator=(const CaptureRingBuffer&) = delete;
    CaptureRingBuffer(CaptureRingBuffer&&) = delete;
    CaptureRingBuffer& operator=(CaptureRingBuffer&&) = delete;

    bool try_push(const DynamicLinkCaptureFrame& frame);
    bool try_pop(DynamicLinkCaptureFrame& frame);

    std::size_t size() const noexcept;
    std::size_t capacity() const noexcept;
    std::size_t antenna_count() const noexcept;
    std::size_t maximum_samples_per_antenna() const noexcept;
    CaptureRingStatistics statistics() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Portable versioned file format: little-endian metadata followed by
// branch-major complex<float> IQ samples. read_next() returns false only at a
// clean end of file and throws for corrupt or truncated input.
class IqCaptureFileWriter {
public:
    explicit IqCaptureFileWriter(
        const std::string& path,
        std::size_t antenna_count = 2u);
    ~IqCaptureFileWriter();

    IqCaptureFileWriter(const IqCaptureFileWriter&) = delete;
    IqCaptureFileWriter& operator=(const IqCaptureFileWriter&) = delete;

    void append(const DynamicLinkCaptureFrame& frame);
    void flush();
    std::uint64_t frames_written() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class IqCaptureFileReader {
public:
    explicit IqCaptureFileReader(
        const std::string& path,
        std::size_t maximum_samples_per_antenna = 1u << 20u);
    ~IqCaptureFileReader();

    IqCaptureFileReader(const IqCaptureFileReader&) = delete;
    IqCaptureFileReader& operator=(const IqCaptureFileReader&) = delete;

    bool read_next(DynamicLinkCaptureFrame& frame);
    std::size_t antenna_count() const noexcept;
    std::uint64_t frames_read() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Pops one capture and submits it with capture_sequence as the pipeline ID.
// The scratch frame is reusable, so allocations disappear after warm-up.
bool submit_next_capture(
    CaptureRingBuffer& ring,
    DynamicLinkPipeline& pipeline,
    const DynamicLinkReceiverConfig& receiver_config,
    DynamicLinkCaptureFrame& scratch);

}  // namespace openisac
