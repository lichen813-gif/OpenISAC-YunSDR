#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace openisac {

constexpr std::size_t ldpc_information_bits = 504u;
constexpr std::size_t ldpc_parity_checks = 504u;
constexpr std::size_t ldpc_encoded_bits = 1008u;

struct LdpcDecodeResult {
    std::vector<std::uint8_t> information_bits;
    std::vector<std::uint8_t> codeword_bits;
    std::size_t syndrome_weight = 0u;
    unsigned iterations = 0u;
};

struct LdpcDecodeWorkspace {
    std::vector<float> input_llrs;
    std::vector<float> beliefs;
    std::vector<float> messages;
    std::size_t capacity_growths = 0u;

    void release() noexcept;
};

class Ldpc5041008 {
public:
    Ldpc5041008(const std::string& parity_check_alist, const std::string& generator_alist);

    std::vector<std::uint8_t> encode(
        const std::vector<std::uint8_t>& information_bits) const;

    std::size_t syndrome_weight(const std::vector<std::uint8_t>& codeword_bits) const;

    LdpcDecodeResult decode_normalized_min_sum(
        const std::vector<float>& llrs,
        unsigned maximum_iterations = 6u,
        float normalization = 0.8f) const;

    void decode_normalized_min_sum(
        const std::vector<float>& llrs,
        unsigned maximum_iterations,
        float normalization,
        LdpcDecodeWorkspace& workspace,
        LdpcDecodeResult& result) const;

    const std::vector<std::uint16_t>& systematic_positions() const noexcept {
        return systematic_positions_;
    }

private:
    std::vector<std::vector<std::uint16_t>> check_rows_;
    std::vector<std::vector<std::uint16_t>> generator_rows_;
    std::vector<std::uint16_t> systematic_positions_;
    std::vector<std::size_t> message_offsets_;
};

}  // namespace openisac
