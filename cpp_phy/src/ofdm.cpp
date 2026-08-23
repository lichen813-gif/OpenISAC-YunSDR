#include "openisac/ofdm.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace openisac {
namespace {

constexpr std::size_t fixed_fft_size = 1024u;
constexpr std::size_t fixed_fft_twiddles = fixed_fft_size - 1u;

struct Fft1024Plan {
    std::array<std::uint16_t, fixed_fft_size> bit_reversed{};
    std::array<std::complex<float>, fixed_fft_twiddles> forward{};
    std::array<std::complex<float>, fixed_fft_twiddles> inverse{};

    Fft1024Plan() {
        for (std::size_t index = 0u; index < fixed_fft_size; ++index) {
            std::size_t value = index;
            std::size_t reversed = 0u;
            for (unsigned bit = 0u; bit < 10u; ++bit) {
                reversed = (reversed << 1u) | (value & 1u);
                value >>= 1u;
            }
            bit_reversed[index] = static_cast<std::uint16_t>(reversed);
        }
        constexpr float two_pi = 6.28318530717958647692f;
        std::size_t twiddle_index = 0u;
        for (std::size_t length = 2u; length <= fixed_fft_size; length <<= 1u) {
            const std::size_t half = length >> 1u;
            for (std::size_t offset = 0u; offset < half; ++offset) {
                const float angle = -two_pi * static_cast<float>(offset) /
                                    static_cast<float>(length);
                forward[twiddle_index] = {std::cos(angle), std::sin(angle)};
                inverse[twiddle_index] = std::conj(forward[twiddle_index]);
                ++twiddle_index;
            }
        }
    }
};

void fft_1024_inplace(
    std::vector<std::complex<float>>& values,
    bool inverse_transform) {
    static const Fft1024Plan plan;
    for (std::size_t index = 1u; index < fixed_fft_size; ++index) {
        const std::size_t reversed = plan.bit_reversed[index];
        if (index < reversed) {
            std::swap(values[index], values[reversed]);
        }
    }
    const auto& twiddles = inverse_transform ? plan.inverse : plan.forward;
    std::size_t stage_twiddle = 0u;
    for (std::size_t length = 2u; length <= fixed_fft_size; length <<= 1u) {
        const std::size_t half = length >> 1u;
        for (std::size_t start = 0u; start < fixed_fft_size; start += length) {
            for (std::size_t offset = 0u; offset < half; ++offset) {
                const auto even = values[start + offset];
                const auto odd = values[start + offset + half] *
                                 twiddles[stage_twiddle + offset];
                values[start + offset] = even + odd;
                values[start + offset + half] = even - odd;
            }
        }
        stage_twiddle += half;
    }
    constexpr float scale = 0.03125f;
    for (auto& value : values) {
        value *= scale;
    }
}

}  // namespace

void fft_inplace(std::vector<std::complex<float>>& values, bool inverse) {
    const std::size_t size = values.size();
    if (size == 0u || (size & (size - 1u)) != 0u) {
        throw std::invalid_argument("FFT length must be a non-zero power of two");
    }
    if (size == fixed_fft_size) {
        fft_1024_inplace(values, inverse);
        return;
    }

    for (std::size_t index = 1u, reverse = 0u; index < size; ++index) {
        std::size_t bit = size >> 1u;
        while ((reverse & bit) != 0u) {
            reverse ^= bit;
            bit >>= 1u;
        }
        reverse ^= bit;
        if (index < reverse) {
            std::swap(values[index], values[reverse]);
        }
    }

    constexpr float pi = 3.14159265358979323846f;
    for (std::size_t length = 2u; length <= size; length <<= 1u) {
        const float angle = (inverse ? 2.0f : -2.0f) * pi /
                            static_cast<float>(length);
        const std::complex<float> step{std::cos(angle), std::sin(angle)};
        for (std::size_t start = 0u; start < size; start += length) {
            std::complex<float> twiddle{1.0f, 0.0f};
            const std::size_t half = length >> 1u;
            for (std::size_t offset = 0u; offset < half; ++offset) {
                const auto even = values[start + offset];
                const auto odd = values[start + offset + half] * twiddle;
                values[start + offset] = even + odd;
                values[start + offset + half] = even - odd;
                twiddle *= step;
            }
        }
    }

    const float scale = 1.0f / std::sqrt(static_cast<float>(size));
    for (auto& value : values) {
        value *= scale;
    }
}

std::vector<std::complex<float>> ofdm_modulate(
    const std::vector<std::complex<float>>& frequency,
    std::size_t cp_length) {
    std::vector<std::complex<float>> samples;
    std::vector<std::complex<float>> scratch;
    ofdm_modulate(frequency, cp_length, samples, scratch);
    return samples;
}

void ofdm_modulate(
    const std::vector<std::complex<float>>& frequency,
    std::size_t cp_length,
    std::vector<std::complex<float>>& samples_output,
    std::vector<std::complex<float>>& fft_scratch) {
    if (cp_length > frequency.size()) {
        throw std::invalid_argument("CP length must not exceed FFT length");
    }
    if (static_cast<const void*>(&frequency) ==
            static_cast<const void*>(&samples_output) ||
        static_cast<const void*>(&frequency) ==
            static_cast<const void*>(&fft_scratch) ||
        static_cast<const void*>(&samples_output) ==
            static_cast<const void*>(&fft_scratch)) {
        throw std::invalid_argument("OFDM modulation buffers must not alias");
    }
    fft_scratch.assign(frequency.begin(), frequency.end());
    fft_inplace(fft_scratch, true);
    samples_output.resize(fft_scratch.size() + cp_length);
    std::copy(
        fft_scratch.end() - static_cast<std::ptrdiff_t>(cp_length),
        fft_scratch.end(), samples_output.begin());
    std::copy(
        fft_scratch.begin(), fft_scratch.end(),
        samples_output.begin() + static_cast<std::ptrdiff_t>(cp_length));
}

std::vector<std::complex<float>> ofdm_demodulate(
    const std::vector<std::complex<float>>& samples,
    std::size_t fft_size,
    std::size_t cp_length) {
    std::vector<std::complex<float>> frequency;
    ofdm_demodulate(samples, fft_size, cp_length, frequency);
    return frequency;
}

void ofdm_demodulate(
    const std::vector<std::complex<float>>& samples,
    std::size_t fft_size,
    std::size_t cp_length,
    std::vector<std::complex<float>>& frequency_output) {
    if (samples.size() != fft_size + cp_length) {
        throw std::invalid_argument("OFDM sample count does not match FFT plus CP");
    }
    if (static_cast<const void*>(&samples) ==
        static_cast<const void*>(&frequency_output)) {
        throw std::invalid_argument("OFDM demodulation input/output must not alias");
    }
    frequency_output.resize(fft_size);
    std::copy(
        samples.begin() + static_cast<std::ptrdiff_t>(cp_length), samples.end(),
        frequency_output.begin());
    fft_inplace(frequency_output, false);
}

}  // namespace openisac
