#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace openisac {

constexpr std::size_t ldpc_codeword_bits = 1008u;
constexpr std::size_t ldpc_interleaver_rows = 21u;

std::vector<std::uint8_t> unpack_msb_bits(
    const std::vector<std::uint8_t>& packed,
    std::size_t bit_count);

std::vector<std::uint8_t> pack_msb_bits(
    const std::vector<std::uint8_t>& bits);

void scramble_bits(std::vector<std::uint8_t>& bits, std::uint8_t initial_state = 0x5Au);

void soft_descramble(std::vector<float>& llrs, std::uint8_t initial_state = 0x5Au);

void interleave_ldpc_blocks(std::vector<std::uint8_t>& bits);

void deinterleave_ldpc_blocks(std::vector<std::uint8_t>& bits);

void deinterleave_ldpc_blocks(std::vector<float>& llrs);

}  // namespace openisac
