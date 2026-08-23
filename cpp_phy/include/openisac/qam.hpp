#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <stdexcept>

namespace openisac {

class SquareQAM {
public:
    static bool supported(unsigned bits_per_symbol) noexcept {
        return bits_per_symbol == 2u || bits_per_symbol == 4u ||
               bits_per_symbol == 6u || bits_per_symbol == 8u;
    }

    static std::complex<float> modulate(unsigned label, unsigned bits_per_symbol) {
        validate(bits_per_symbol);
        const unsigned axis_bits = bits_per_symbol / 2u;
        const unsigned axis_mask = (1u << axis_bits) - 1u;
        if (label >= (1u << bits_per_symbol)) {
            throw std::invalid_argument("QAM label outside constellation");
        }
        const float scale = normalization(bits_per_symbol);
        return {
            pam_from_gray(label >> axis_bits, axis_bits) / scale,
            pam_from_gray(label & axis_mask, axis_bits) / scale,
        };
    }

    static unsigned demodulate(
        const std::complex<float>& sample, unsigned bits_per_symbol) {
        validate(bits_per_symbol);
        const unsigned axis_bits = bits_per_symbol / 2u;
        return (nearest_gray(sample.real(), axis_bits, bits_per_symbol) << axis_bits) |
               nearest_gray(sample.imag(), axis_bits, bits_per_symbol);
    }

    static std::array<float, 8> max_log_llrs(
        const std::complex<float>& sample,
        float noise_variance,
        unsigned bits_per_symbol) {
        validate(bits_per_symbol);
        std::array<float, 8> result{};
        const float scale = normalization(bits_per_symbol);
        const float inverse_variance =
            1.0f / std::max(noise_variance, 1.0e-15f);
        switch (bits_per_symbol) {
            case 2u:
                axis_max_log_llrs<1u>(
                    sample.real(), scale, inverse_variance, 0u, result);
                axis_max_log_llrs<1u>(
                    sample.imag(), scale, inverse_variance, 1u, result);
                break;
            case 4u:
                axis_max_log_llrs<2u>(
                    sample.real(), scale, inverse_variance, 0u, result);
                axis_max_log_llrs<2u>(
                    sample.imag(), scale, inverse_variance, 2u, result);
                break;
            case 6u:
                axis_max_log_llrs_64qam(
                    sample.real(), inverse_variance, 0u, result);
                axis_max_log_llrs_64qam(
                    sample.imag(), inverse_variance, 3u, result);
                break;
            case 8u:
                axis_max_log_llrs<4u>(
                    sample.real(), scale, inverse_variance, 0u, result);
                axis_max_log_llrs<4u>(
                    sample.imag(), scale, inverse_variance, 4u, result);
                break;
            default:
                break;
        }
        return result;
    }

private:
    static void validate(unsigned bits_per_symbol) {
        if (!supported(bits_per_symbol)) {
            throw std::invalid_argument("square QAM requires Qm=2,4,6,8");
        }
    }

    static unsigned gray_to_binary(unsigned gray) noexcept {
        unsigned binary = gray;
        while ((gray >>= 1u) != 0u) {
            binary ^= gray;
        }
        return binary;
    }

    static float pam_from_gray(unsigned gray, unsigned axis_bits) noexcept {
        const unsigned levels = 1u << axis_bits;
        return static_cast<float>(levels - 1u) -
               2.0f * static_cast<float>(gray_to_binary(gray));
    }

    template <unsigned AxisBits>
    static void axis_max_log_llrs(
        float sample,
        float scale,
        float inverse_variance,
        unsigned result_offset,
        std::array<float, 8>& result) noexcept {
        constexpr unsigned levels = 1u << AxisBits;
        std::array<float, levels> distances{};
        for (unsigned gray = 0u; gray < levels; ++gray) {
            const float reference = pam_from_gray(gray, AxisBits) / scale;
            const float delta = sample - reference;
            distances[gray] = delta * delta;
        }
        for (unsigned bit = 0u; bit < AxisBits; ++bit) {
            float minimum_zero = std::numeric_limits<float>::infinity();
            float minimum_one = std::numeric_limits<float>::infinity();
            const unsigned mask = 1u << (AxisBits - 1u - bit);
            for (unsigned gray = 0u; gray < levels; ++gray) {
                if ((gray & mask) == 0u) {
                    minimum_zero = std::min(minimum_zero, distances[gray]);
                } else {
                    minimum_one = std::min(minimum_one, distances[gray]);
                }
            }
            result[result_offset + bit] =
                (minimum_one - minimum_zero) * inverse_variance;
        }
    }

    static float squared_distance(float sample, float reference) noexcept {
        const float delta = sample - reference;
        return delta * delta;
    }

    static float minimum4(
        float first, float second, float third, float fourth) noexcept {
        return std::min(
            std::min(first, second), std::min(third, fourth));
    }

    static void axis_max_log_llrs_64qam(
        float sample,
        float inverse_variance,
        unsigned result_offset,
        std::array<float, 8>& result) noexcept {
        constexpr float inverse_scale = 0.1543033499620919f;  // 1/sqrt(42)
        const float d0 = squared_distance(sample, 7.0f * inverse_scale);
        const float d1 = squared_distance(sample, 5.0f * inverse_scale);
        const float d2 = squared_distance(sample, 1.0f * inverse_scale);
        const float d3 = squared_distance(sample, 3.0f * inverse_scale);
        const float d4 = squared_distance(sample, -7.0f * inverse_scale);
        const float d5 = squared_distance(sample, -5.0f * inverse_scale);
        const float d6 = squared_distance(sample, -1.0f * inverse_scale);
        const float d7 = squared_distance(sample, -3.0f * inverse_scale);
        result[result_offset] =
            (minimum4(d4, d5, d6, d7) - minimum4(d0, d1, d2, d3)) *
            inverse_variance;
        result[result_offset + 1u] =
            (minimum4(d2, d3, d6, d7) - minimum4(d0, d1, d4, d5)) *
            inverse_variance;
        result[result_offset + 2u] =
            (minimum4(d1, d3, d5, d7) - minimum4(d0, d2, d4, d6)) *
            inverse_variance;
    }

    static float normalization(unsigned bits_per_symbol) {
        switch (bits_per_symbol) {
            case 2u: return 1.4142135623730951f;
            case 4u: return 3.1622776601683793f;
            case 6u: return 6.4807406984078602f;
            case 8u: return 13.038404810405298f;
            default: return 1.0f;
        }
    }

    static unsigned nearest_gray(
        float value, unsigned axis_bits, unsigned bits_per_symbol) {
        const unsigned levels = 1u << axis_bits;
        const float scale = normalization(bits_per_symbol);
        unsigned best = 0u;
        float best_distance = std::numeric_limits<float>::infinity();
        for (unsigned gray = 0; gray < levels; ++gray) {
            const float delta = value - pam_from_gray(gray, axis_bits) / scale;
            const float distance = delta * delta;
            if (distance < best_distance) {
                best_distance = distance;
                best = gray;
            }
        }
        return best;
    }
};

}  // namespace openisac
