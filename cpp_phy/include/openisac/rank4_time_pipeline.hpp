#pragma once

#include "openisac/rank4_time_link.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace openisac {

struct Rank4TimePipelineTiming {
    double producer_wall_us = 0.0;
    double backpressure_wait_us = 0.0;
    double submit_call_us = 0.0;
    double receiver_front_us = 0.0;
    double queue_wait_us = 0.0;
    double fec_wall_us = 0.0;
    double latency_us = 0.0;
    std::size_t buffer_slot = 0u;
};

struct Rank4TimePipelineResult {
    std::uint64_t frame_id = 0u;
    Rank4TimeSimulationResult link{};
    Rank4TimePipelineTiming timing{};
};

// Two-slot, single-producer Rank-4 simulator/receiver pipeline. The producer
// completes waveform simulation, synchronization, CSI, 4x4 MMSE and soft
// demapping. A persistent consumer performs LDPC/CRC with a fixed worker pool.
class Rank4TimePipeline {
public:
    Rank4TimePipeline(
        const Ldpc5041008& codec,
        std::size_t ldpc_worker_count);
    ~Rank4TimePipeline();

    Rank4TimePipeline(const Rank4TimePipeline&) = delete;
    Rank4TimePipeline& operator=(const Rank4TimePipeline&) = delete;
    Rank4TimePipeline(Rank4TimePipeline&&) = delete;
    Rank4TimePipeline& operator=(Rank4TimePipeline&&) = delete;

    void submit(
        std::uint64_t frame_id,
        std::uint16_t sequence,
        const Rank4TimeSimulationConfig& config = {});

    void submit_payload(
        std::uint64_t frame_id,
        const std::vector<std::uint8_t>& payload,
        std::uint16_t sequence,
        const Rank4TimeSimulationConfig& config = {});

    Rank4TimePipelineResult receive();

    std::size_t slot_count() const noexcept;
    const Rank4TimeReceiverState& receiver_state() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace openisac
