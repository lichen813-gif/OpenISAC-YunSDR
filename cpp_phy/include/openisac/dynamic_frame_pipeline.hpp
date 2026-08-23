#pragma once

#include "openisac/dynamic_frame.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace openisac {

struct DynamicFramePipelineTiming {
    double producer_us = 0.0;
    double queue_wait_us = 0.0;
    double fec_us = 0.0;
    double latency_us = 0.0;
    std::size_t buffer_slot = 0u;
    std::size_t capacity_growths_this_frame = 0u;
};

struct DynamicFramePipelineResult {
    std::uint64_t frame_id = 0u;
    DecodedDynamicFrame decoded{};
    DynamicFramePipelineTiming timing{};
};

// Two-slot, single-producer pipeline. submit() performs control decoding and
// soft demapping in the caller, while a persistent consumer thread performs
// LDPC/CRC using its own fixed LdpcFrameDecoder worker pool. receive() returns
// results in submission order and releases the corresponding slot.
class DynamicFramePipeline {
public:
    DynamicFramePipeline(
        const Ldpc5041008& codec,
        std::size_t ldpc_worker_count,
        unsigned maximum_ldpc_iterations = 6u,
        float ldpc_normalization = 0.8f);
    ~DynamicFramePipeline();

    DynamicFramePipeline(const DynamicFramePipeline&) = delete;
    DynamicFramePipeline& operator=(const DynamicFramePipeline&) = delete;
    DynamicFramePipeline(DynamicFramePipeline&&) = delete;
    DynamicFramePipeline& operator=(DynamicFramePipeline&&) = delete;

    void submit(
        std::uint64_t frame_id,
        const std::vector<float>& control_llrs,
        const std::vector<std::complex<float>>& equalized_payload_symbols,
        const std::vector<float>& effective_noise_variances);

    DynamicFramePipelineResult receive();

    std::size_t slot_count() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace openisac
