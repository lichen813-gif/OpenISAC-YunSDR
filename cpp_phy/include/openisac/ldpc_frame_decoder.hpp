#pragma once

#include "openisac/ldpc.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace openisac {

struct LdpcFrameDecodeResult {
    std::vector<std::uint8_t> information_bits;
    std::size_t syndrome_failures = 0u;
    unsigned maximum_iterations = 0u;
    std::size_t capacity_growths_this_frame = 0u;
    std::size_t total_capacity_growths = 0u;
};

// Persistent fixed-size worker pool. One synchronous decode_blocks call may be
// active at a time; the immutable codec is safely shared by all workers.
class LdpcFrameDecoder {
public:
    explicit LdpcFrameDecoder(
        const Ldpc5041008& codec,
        std::size_t worker_count);
    ~LdpcFrameDecoder();

    LdpcFrameDecoder(const LdpcFrameDecoder&) = delete;
    LdpcFrameDecoder& operator=(const LdpcFrameDecoder&) = delete;
    LdpcFrameDecoder(LdpcFrameDecoder&&) = delete;
    LdpcFrameDecoder& operator=(LdpcFrameDecoder&&) = delete;

    std::size_t worker_count() const noexcept;

    void decode_blocks(
        const std::vector<float>& concatenated_llrs,
        std::size_t block_count,
        unsigned maximum_iterations,
        float normalization,
        LdpcFrameDecodeResult& result);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace openisac
