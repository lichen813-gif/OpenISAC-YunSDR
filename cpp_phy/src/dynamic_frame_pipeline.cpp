#include "openisac/dynamic_frame_pipeline.hpp"

#include "openisac/ldpc_frame_decoder.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace openisac {
namespace {

using Clock = std::chrono::steady_clock;

double elapsed_us(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::micro>(end - begin).count();
}

enum class SlotState {
    free,
    preparing,
    queued,
    processing,
    complete,
};

struct PipelineSlot {
    PreparedDynamicFrame prepared;
    DynamicFramePipelineResult output;
    Clock::time_point producer_start{};
    Clock::time_point submitted{};
    std::exception_ptr failure;
    SlotState state = SlotState::free;
};

}  // namespace

struct DynamicFramePipeline::Impl {
    Impl(
        const Ldpc5041008& codec_value,
        std::size_t ldpc_worker_count,
        unsigned iterations,
        float normalization_value)
        : codec(codec_value), decoder(codec_value, ldpc_worker_count),
          maximum_iterations(iterations), normalization(normalization_value),
          consumer([this] { consumer_loop(); }) {
        free_slots.push_back(0u);
        free_slots.push_back(1u);
    }

    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        pending_ready.notify_all();
        slot_released.notify_all();
        completed_ready.notify_all();
        if (consumer.joinable()) {
            consumer.join();
        }
    }

    void consumer_loop() noexcept {
        for (;;) {
            std::size_t slot_index = 0u;
            {
                std::unique_lock<std::mutex> lock(mutex);
                pending_ready.wait(lock, [&] {
                    return stopping || !pending_slots.empty();
                });
                if (stopping && pending_slots.empty()) {
                    return;
                }
                slot_index = pending_slots.front();
                pending_slots.pop_front();
                slots[slot_index].state = SlotState::processing;
            }

            auto& slot = slots[slot_index];
            const auto fec_start = Clock::now();
            slot.output.timing.queue_wait_us =
                elapsed_us(slot.submitted, fec_start);
            try {
                slot.output.decoded = decode_prepared_dynamic_frame(
                    slot.prepared, codec, maximum_iterations,
                    normalization, &decoder);
            } catch (...) {
                slot.failure = std::current_exception();
            }
            const auto fec_done = Clock::now();
            slot.output.timing.fec_us = elapsed_us(fec_start, fec_done);
            slot.output.timing.latency_us =
                elapsed_us(slot.producer_start, fec_done);
            {
                std::lock_guard<std::mutex> lock(mutex);
                slot.state = SlotState::complete;
                completed_slots.push_back(slot_index);
            }
            completed_ready.notify_one();
        }
    }

    const Ldpc5041008& codec;
    LdpcFrameDecoder decoder;
    unsigned maximum_iterations;
    float normalization;
    std::array<PipelineSlot, 2> slots;
    std::deque<std::size_t> free_slots;
    std::deque<std::size_t> pending_slots;
    std::deque<std::size_t> completed_slots;
    std::mutex producer_mutex;
    std::mutex receive_mutex;
    std::mutex mutex;
    std::condition_variable slot_released;
    std::condition_variable pending_ready;
    std::condition_variable completed_ready;
    bool stopping = false;
    std::thread consumer;
};

DynamicFramePipeline::DynamicFramePipeline(
    const Ldpc5041008& codec,
    std::size_t ldpc_worker_count,
    unsigned maximum_ldpc_iterations,
    float ldpc_normalization)
    : impl_(new Impl(
          codec, ldpc_worker_count,
          maximum_ldpc_iterations, ldpc_normalization)) {}

DynamicFramePipeline::~DynamicFramePipeline() = default;

void DynamicFramePipeline::submit(
    std::uint64_t frame_id,
    const std::vector<float>& control_llrs,
    const std::vector<std::complex<float>>& equalized_payload_symbols,
    const std::vector<float>& effective_noise_variances) {
    std::lock_guard<std::mutex> producer_call_lock(impl_->producer_mutex);
    std::size_t slot_index = 0u;
    {
        std::unique_lock<std::mutex> lock(impl_->mutex);
        impl_->slot_released.wait(lock, [&] {
            return impl_->stopping || !impl_->free_slots.empty();
        });
        if (impl_->stopping) {
            throw std::runtime_error("dynamic-frame pipeline is stopping");
        }
        slot_index = impl_->free_slots.front();
        impl_->free_slots.pop_front();
        impl_->slots[slot_index].state = SlotState::preparing;
    }

    auto& slot = impl_->slots[slot_index];
    slot.failure = nullptr;
    slot.output = DynamicFramePipelineResult{};
    slot.output.frame_id = frame_id;
    slot.output.timing.buffer_slot = slot_index;
    const std::size_t llr_capacity = slot.prepared.llrs.capacity();
    const std::size_t interleaver_capacity =
        slot.prepared.interleaved_block.capacity();
    slot.producer_start = Clock::now();
    try {
        prepare_dynamic_frame_llrs(
            control_llrs, equalized_payload_symbols,
            effective_noise_variances, slot.prepared);
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            slot.state = SlotState::free;
            impl_->free_slots.push_back(slot_index);
        }
        impl_->slot_released.notify_one();
        throw;
    }
    slot.submitted = Clock::now();
    slot.output.timing.producer_us =
        elapsed_us(slot.producer_start, slot.submitted);
    slot.output.timing.capacity_growths_this_frame =
        static_cast<std::size_t>(slot.prepared.llrs.capacity() > llr_capacity) +
        static_cast<std::size_t>(
            slot.prepared.interleaved_block.capacity() > interleaver_capacity);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        slot.state = SlotState::queued;
        impl_->pending_slots.push_back(slot_index);
    }
    impl_->pending_ready.notify_one();
}

DynamicFramePipelineResult DynamicFramePipeline::receive() {
    std::lock_guard<std::mutex> receive_call_lock(impl_->receive_mutex);
    std::size_t slot_index = 0u;
    {
        std::unique_lock<std::mutex> lock(impl_->mutex);
        impl_->completed_ready.wait(lock, [&] {
            return impl_->stopping || !impl_->completed_slots.empty();
        });
        if (impl_->completed_slots.empty()) {
            throw std::runtime_error("dynamic-frame pipeline stopped without a result");
        }
        slot_index = impl_->completed_slots.front();
        impl_->completed_slots.pop_front();
    }
    auto& slot = impl_->slots[slot_index];
    const auto failure = slot.failure;
    DynamicFramePipelineResult output = std::move(slot.output);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        slot.state = SlotState::free;
        impl_->free_slots.push_back(slot_index);
    }
    impl_->slot_released.notify_one();
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
    return output;
}

std::size_t DynamicFramePipeline::slot_count() const noexcept {
    return 2u;
}

}  // namespace openisac
