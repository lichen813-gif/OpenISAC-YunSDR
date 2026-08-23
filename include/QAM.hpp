#ifndef OPENISAC_QAM_HPP
#define OPENISAC_QAM_HPP

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

/**
 * Gray-coded, unit-average-power square QAM used by the SISO data plane.
 *
 * Bit ordering is MSB first.  The first Qm/2 bits select the I-axis Gray
 * label and the remaining Qm/2 bits select the Q-axis Gray label.  Qm=2 is
 * deliberately identical to OpenISAC's legacy QPSK mapping:
 *   00=(+,+), 01=(+,-), 10=(-,+), 11=(-,-), divided by sqrt(2).
 */
class SquareQAM {
public:
    static bool supported_bits_per_symbol(size_t bits_per_symbol) {
        return bits_per_symbol == 2 || bits_per_symbol == 4 ||
               bits_per_symbol == 6 || bits_per_symbol == 8;
    }

    static size_t constellation_size(size_t bits_per_symbol) {
        validate(bits_per_symbol);
        return size_t{1} << bits_per_symbol;
    }

    static std::complex<float> modulate(unsigned symbol, size_t bits_per_symbol) {
        validate(bits_per_symbol);
        const unsigned axis_bits = static_cast<unsigned>(bits_per_symbol / 2);
        const unsigned axis_mask = (1u << axis_bits) - 1u;
        const unsigned i_gray = (symbol >> axis_bits) & axis_mask;
        const unsigned q_gray = symbol & axis_mask;
        const float norm = normalization(bits_per_symbol);
        return {pam_from_gray(i_gray, axis_bits) / norm,
                pam_from_gray(q_gray, axis_bits) / norm};
    }

    static unsigned demodulate(std::complex<float> sample, size_t bits_per_symbol) {
        validate(bits_per_symbol);
        const unsigned axis_bits = static_cast<unsigned>(bits_per_symbol / 2);
        const unsigned i_gray = nearest_gray(sample.real(), axis_bits, bits_per_symbol);
        const unsigned q_gray = nearest_gray(sample.imag(), axis_bits, bits_per_symbol);
        return (i_gray << axis_bits) | q_gray;
    }

    static std::complex<float> remodulate(
        std::complex<float> sample, size_t bits_per_symbol) {
        return modulate(demodulate(sample, bits_per_symbol), bits_per_symbol);
    }

    /**
     * Max-log LLRs, positive for bit 0 and negative for bit 1.
     * noise_variance is E[|n|^2] for the complex equalized sample.
     */
    static void compute_llrs(std::complex<float> sample,
                             float noise_variance,
                             size_t bits_per_symbol,
                             float* llrs) {
        validate(bits_per_symbol);
        const unsigned axis_bits = static_cast<unsigned>(bits_per_symbol / 2);
        const unsigned levels = 1u << axis_bits;
        const float inv_noise = 1.0f / std::max(noise_variance, 1.0e-9f);
        const float norm = normalization(bits_per_symbol);
        const std::array<float, 2> axes{{sample.real(), sample.imag()}};

        for (unsigned axis = 0; axis < 2; ++axis) {
            for (unsigned bit = 0; bit < axis_bits; ++bit) {
                float min_zero = std::numeric_limits<float>::infinity();
                float min_one = std::numeric_limits<float>::infinity();
                const unsigned mask = 1u << (axis_bits - 1u - bit);
                for (unsigned gray = 0; gray < levels; ++gray) {
                    const float ref = pam_from_gray(gray, axis_bits) / norm;
                    const float delta = axes[axis] - ref;
                    const float distance = delta * delta;
                    if ((gray & mask) == 0) {
                        min_zero = std::min(min_zero, distance);
                    } else {
                        min_one = std::min(min_one, distance);
                    }
                }
                llrs[axis * axis_bits + bit] =
                    (min_one - min_zero) * inv_noise;
            }
        }
    }

private:
    static void validate(size_t bits_per_symbol) {
        if (!supported_bits_per_symbol(bits_per_symbol)) {
            throw std::invalid_argument("SquareQAM supports Qm=2,4,6,8 only");
        }
    }

    static unsigned gray_to_binary(unsigned gray) {
        unsigned binary = gray;
        while ((gray >>= 1u) != 0u) {
            binary ^= gray;
        }
        return binary;
    }

    static float pam_from_gray(unsigned gray, unsigned axis_bits) {
        const unsigned levels = 1u << axis_bits;
        const unsigned binary = gray_to_binary(gray);
        // Positive outer level for an all-zero label preserves legacy QPSK.
        return static_cast<float>(levels - 1u) - 2.0f * static_cast<float>(binary);
    }

    static float normalization(size_t bits_per_symbol) {
        const float order = static_cast<float>(size_t{1} << bits_per_symbol);
        return std::sqrt((2.0f / 3.0f) * (order - 1.0f));
    }

    static unsigned nearest_gray(float value,
                                 unsigned axis_bits,
                                 size_t bits_per_symbol) {
        const unsigned levels = 1u << axis_bits;
        const float norm = normalization(bits_per_symbol);
        float best_distance = std::numeric_limits<float>::infinity();
        unsigned best_gray = 0;
        for (unsigned gray = 0; gray < levels; ++gray) {
            const float delta = value - pam_from_gray(gray, axis_bits) / norm;
            const float distance = delta * delta;
            if (distance < best_distance) {
                best_distance = distance;
                best_gray = gray;
            }
        }
        return best_gray;
    }
};

#endif  // OPENISAC_QAM_HPP
