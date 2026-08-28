#include "openisac/ldpc_frame_decoder.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace openisac {
namespace {

template <typename Value>
void resize_tracked(
    std::vector<Value>& values,
    std::size_t size,
    std::size_t& capacity_growths) {
    if (values.capacity() < size) {
        ++capacity_growths;
    }
    values.resize(size);
}

}  // namespace

struct LdpcFrameDecoder::Impl {
    explicit Impl(const Ldpc5041008& codec_value, std::size_t count)
        : codec(codec_value), configured_workers(count), workspaces(count),
          worker_results(count) {
        if (count == 0u || count > 19u) {
            throw std::invalid_argument("LDPC worker count must be in [1,19]");
        }
        // A single worker runs synchronously in the caller. Waking a helper
        // thread for a handful of short, high-SNR blocks costs more than the
        // normalized-min-sum work on some platforms (notably DGX/ARM).
        if (count == 1u) {
            return;
        }
        threads.reserve(count);
        try {
            for (std::size_t worker = 0u; worker < count; ++worker) {
                threads.emplace_back([this, worker] { worker_loop(worker); });
            }
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                stopping = true;
                ++generation;
            }
            start.notify_all();
            for (auto& thread : threads) {
                if (thread.joinable()) {
                    thread.join();
                }
            }
            throw;
        }
    }

    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            ++generation;
        }
        start.notify_all();
        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    std::size_t total_growths() const noexcept {
        std::size_t total = container_growths;
        for (const auto& workspace : workspaces) {
            total += workspace.capacity_growths;
        }
        return total;
    }

    void decode_block(std::size_t worker, std::size_t block) {
        auto& workspace = workspaces[worker];
        const float* const begin =
            input->data() + block * ldpc_encoded_bits;
        codec.decode_normalized_min_sum(
            begin, ldpc_encoded_bits, maximum_iterations, normalization,
            workspace, worker_results[worker]);
        const auto& decoded = worker_results[worker];
        std::copy(
            decoded.information_bits.begin(),
            decoded.information_bits.end(),
            decoded_information.begin() + static_cast<std::ptrdiff_t>(
                block * ldpc_information_bits));
        block_syndrome_weights[block] = decoded.syndrome_weight;
        block_iterations[block] = decoded.iterations;
    }

    void worker_loop(std::size_t worker) noexcept {
        std::size_t observed_generation = 0u;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                start.wait(lock, [&] {
                    return stopping || generation != observed_generation;
                });
                if (stopping) {
                    return;
                }
                observed_generation = generation;
            }
            // Static round-robin assignment avoids a lock/atomic operation per
            // short LDPC block and gives repeatable per-worker buffer warm-up.
            for (std::size_t block = worker; block < block_count;
                 block += configured_workers) {
                try {
                    decode_block(worker, block);
                } catch (...) {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (failure == nullptr) {
                        failure = std::current_exception();
                    }
                }
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++finished_workers;
                if (finished_workers == threads.size()) {
                    done.notify_one();
                }
            }
        }
    }

    const Ldpc5041008& codec;
    const std::size_t configured_workers;
    std::vector<LdpcDecodeWorkspace> workspaces;
    std::vector<LdpcDecodeResult> worker_results;
    std::vector<std::uint8_t> decoded_information;
    std::vector<std::size_t> block_syndrome_weights;
    std::vector<unsigned> block_iterations;
    std::vector<std::thread> threads;
    std::mutex call_mutex;
    std::mutex mutex;
    std::condition_variable start;
    std::condition_variable done;
    const std::vector<float>* input = nullptr;
    std::size_t block_count = 0u;
    std::size_t finished_workers = 0u;
    unsigned maximum_iterations = 0u;
    float normalization = 0.8f;
    std::size_t generation = 0u;
    std::size_t container_growths = 0u;
    bool stopping = false;
    std::exception_ptr failure;
};

LdpcFrameDecoder::LdpcFrameDecoder(
    const Ldpc5041008& codec,
    std::size_t worker_count)
    : impl_(new Impl(codec, worker_count)) {}

LdpcFrameDecoder::~LdpcFrameDecoder() = default;

std::size_t LdpcFrameDecoder::worker_count() const noexcept {
    return impl_->configured_workers;
}

void LdpcFrameDecoder::decode_blocks(
    const std::vector<float>& concatenated_llrs,
    std::size_t block_count,
    unsigned maximum_iterations,
    float normalization,
    LdpcFrameDecodeResult& result) {
    if (block_count == 0u || block_count > 19u ||
        concatenated_llrs.size() != block_count * ldpc_encoded_bits ||
        maximum_iterations == 0u || !std::isfinite(normalization) ||
        normalization <= 0.0f || normalization > 1.0f) {
        throw std::invalid_argument("invalid LDPC frame-decoder parameters");
    }
    std::lock_guard<std::mutex> call_lock(impl_->call_mutex);
    const std::size_t growths_before = impl_->total_growths();
    if (impl_->configured_workers == 1u) {
        resize_tracked(
            impl_->decoded_information,
            block_count * ldpc_information_bits, impl_->container_growths);
        resize_tracked(
            impl_->block_syndrome_weights,
            block_count, impl_->container_growths);
        resize_tracked(
            impl_->block_iterations,
            block_count, impl_->container_growths);
        impl_->input = &concatenated_llrs;
        impl_->block_count = block_count;
        impl_->maximum_iterations = maximum_iterations;
        impl_->normalization = normalization;
        for (std::size_t block = 0u; block < block_count; ++block) {
            impl_->decode_block(0u, block);
        }
    } else {
        std::unique_lock<std::mutex> lock(impl_->mutex);
        resize_tracked(
            impl_->decoded_information,
            block_count * ldpc_information_bits, impl_->container_growths);
        resize_tracked(
            impl_->block_syndrome_weights,
            block_count, impl_->container_growths);
        resize_tracked(
            impl_->block_iterations,
            block_count, impl_->container_growths);
        impl_->input = &concatenated_llrs;
        impl_->block_count = block_count;
        impl_->finished_workers = 0u;
        impl_->maximum_iterations = maximum_iterations;
        impl_->normalization = normalization;
        impl_->failure = nullptr;
        ++impl_->generation;
        impl_->start.notify_all();
        impl_->done.wait(lock, [&] {
            return impl_->finished_workers == impl_->threads.size();
        });
        if (impl_->failure != nullptr) {
            std::rethrow_exception(impl_->failure);
        }
    }

    result.syndrome_failures = 0u;
    result.maximum_iterations = 0u;
    result.information_bits.resize(block_count * ldpc_information_bits);
    std::copy(
        impl_->decoded_information.begin(), impl_->decoded_information.end(),
        result.information_bits.begin());
    for (std::size_t block = 0u; block < block_count; ++block) {
        result.syndrome_failures += impl_->block_syndrome_weights[block] != 0u;
        result.maximum_iterations =
            std::max(result.maximum_iterations, impl_->block_iterations[block]);
    }
    result.total_capacity_growths = impl_->total_growths();
    result.capacity_growths_this_frame =
        result.total_capacity_growths - growths_before;
}

}  // namespace openisac
