#pragma once

#include "openisac/dynamic_link.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace openisac {

struct DynamicLinkPipelineTiming {
    // Caller-side wall time. submit() includes waveform generation and channel
    // impairments; submit_iq()/submit_capture() contain receiver processing only.
    // receiver_front_us is the sum of the timed receiver algorithm stages.
    double producer_wall_us = 0.0;
    // Time spent waiting for one of the two reusable slots. This is zero when
    // the FEC consumer keeps pace with the receiver front end.
    double backpressure_wait_us = 0.0;
    // End-to-end wall time of the submit call, including slot backpressure.
    double submit_call_us = 0.0;
    double receiver_front_us = 0.0;
    double queue_wait_us = 0.0;
    double fec_wall_us = 0.0;
    double latency_us = 0.0;
    std::size_t buffer_slot = 0u;
};

struct DynamicLinkPipelineResult {
    std::uint64_t frame_id = 0u;
    std::uint64_t capture_sequence = 0u;
    std::uint64_t capture_timestamp = 0u;
    DynamicLinkSimulationResult link{};
    DynamicLinkPipelineTiming timing{};
};

// Two-slot, single-producer complete dynamic-link pipeline. submit() runs the
// simulator/transmitter, channel, synchronization, FFT/CSI, MIMO detection and
// soft demapping in the caller. A persistent consumer runs LDPC/CRC using a
// fixed worker pool. receive() returns results in submission order and releases
// the corresponding workspace.
class DynamicLinkPipeline {
public:
    DynamicLinkPipeline(
        const Ldpc5041008& codec,
        std::size_t ldpc_worker_count);
    ~DynamicLinkPipeline();

    DynamicLinkPipeline(const DynamicLinkPipeline&) = delete;
    DynamicLinkPipeline& operator=(const DynamicLinkPipeline&) = delete;
    DynamicLinkPipeline(DynamicLinkPipeline&&) = delete;
    DynamicLinkPipeline& operator=(DynamicLinkPipeline&&) = delete;

    void submit(
        std::uint64_t frame_id,
        LinkMode mode,
        std::uint16_t sequence,
        const DynamicLinkSimulationConfig& config = {});

    // Receiver-only entry. IQ generation or hardware capture is outside the
    // measured producer interval and the input may be released after return.
    void submit_iq(
        std::uint64_t frame_id,
        const DynamicLinkIqFrame& iq_frame);

    void submit_capture(
        std::uint64_t frame_id,
        const DynamicLinkCaptureFrame& capture,
        const DynamicLinkReceiverConfig& receiver_config);

    // Compatibility overload for existing simulation callers.
    void submit_capture(
        std::uint64_t frame_id,
        const DynamicLinkCaptureFrame& capture,
        const DynamicLinkSimulationConfig& receiver_config);

    DynamicLinkPipelineResult receive();

    std::size_t slot_count() const noexcept;
    const DynamicLinkReceiverState& receiver_state() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace openisac
