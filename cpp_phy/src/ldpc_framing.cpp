#include "openisac/ldpc_framing.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace openisac {
namespace {

std::vector<std::uint8_t> scrambler_sequence(
    std::size_t size,
    std::uint8_t initial_state) {
    std::vector<std::uint8_t> sequence(size);
    std::uint8_t lfsr = initial_state;
    for (std::size_t index = 0; index < size; ++index) {
        const auto bit = static_cast<std::uint8_t>(
            ((lfsr >> 7u) ^ (lfsr >> 3u) ^ (lfsr >> 2u) ^ (lfsr >> 1u)) & 1u);
        sequence[index] = bit;
        lfsr = static_cast<std::uint8_t>((static_cast<unsigned>(lfsr) << 1u) | bit);
    }
    return sequence;
}

template <typename T>
void permute_blocks(std::vector<T>& values, bool inverse) {
    if ((values.size() % ldpc_codeword_bits) != 0u) {
        throw std::invalid_argument(
            "LDPC interleaver input must be a multiple of 1008 elements");
    }
    constexpr std::size_t columns = ldpc_codeword_bits / ldpc_interleaver_rows;
    std::vector<T> scratch(ldpc_codeword_bits);
    for (std::size_t block = 0; block < values.size() / ldpc_codeword_bits; ++block) {
        T* const target = values.data() + block * ldpc_codeword_bits;
        for (std::size_t row = 0; row < ldpc_interleaver_rows; ++row) {
            for (std::size_t column = 0; column < columns; ++column) {
                const std::size_t source = row * columns + column;
                const std::size_t destination = column * ldpc_interleaver_rows + row;
                if (inverse) {
                    scratch[source] = target[destination];
                } else {
                    scratch[destination] = target[source];
                }
            }
        }
        std::copy(scratch.begin(), scratch.end(), target);
    }
}

}  // namespace

std::vector<std::uint8_t> unpack_msb_bits(
    const std::vector<std::uint8_t>& packed,
    std::size_t bit_count) {
    if (bit_count > packed.size() * 8u) {
        throw std::invalid_argument("packed input is shorter than requested bit count");
    }
    std::vector<std::uint8_t> bits(bit_count);
    for (std::size_t index = 0; index < bit_count; ++index) {
        bits[index] = static_cast<std::uint8_t>(
            (packed[index / 8u] >> (7u - index % 8u)) & 1u);
    }
    return bits;
}

std::vector<std::uint8_t> pack_msb_bits(const std::vector<std::uint8_t>& bits) {
    std::vector<std::uint8_t> packed((bits.size() + 7u) / 8u, 0u);
    for (std::size_t index = 0; index < bits.size(); ++index) {
        if (bits[index] > 1u) {
            throw std::invalid_argument("bit vector must contain only zero and one");
        }
        packed[index / 8u] |= static_cast<std::uint8_t>(
            bits[index] << (7u - index % 8u));
    }
    return packed;
}

void scramble_bits(std::vector<std::uint8_t>& bits, std::uint8_t initial_state) {
    const auto sequence = scrambler_sequence(bits.size(), initial_state);
    for (std::size_t index = 0; index < bits.size(); ++index) {
        if (bits[index] > 1u) {
            throw std::invalid_argument("bit vector must contain only zero and one");
        }
        bits[index] ^= sequence[index];
    }
}

void soft_descramble(std::vector<float>& llrs, std::uint8_t initial_state) {
    std::uint8_t lfsr = initial_state;
    for (std::size_t index = 0; index < llrs.size(); ++index) {
        const auto bit = static_cast<std::uint8_t>(
            ((lfsr >> 7u) ^ (lfsr >> 3u) ^ (lfsr >> 2u) ^ (lfsr >> 1u)) & 1u);
        if (bit != 0u) {
            llrs[index] = -llrs[index];
        }
        lfsr = static_cast<std::uint8_t>(
            (static_cast<unsigned>(lfsr) << 1u) | bit);
    }
}

void interleave_ldpc_blocks(std::vector<std::uint8_t>& bits) {
    permute_blocks(bits, false);
}

void deinterleave_ldpc_blocks(std::vector<std::uint8_t>& bits) {
    permute_blocks(bits, true);
}

void deinterleave_ldpc_blocks(std::vector<float>& llrs) {
    permute_blocks(llrs, true);
}

}  // namespace openisac
