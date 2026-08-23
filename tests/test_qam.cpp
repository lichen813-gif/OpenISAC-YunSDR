#include "QAM.hpp"

#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

uint64_t bit_errors(unsigned lhs, unsigned rhs, size_t bits) {
    unsigned value = (lhs ^ rhs) & ((1u << bits) - 1u);
    uint64_t count = 0;
    while (value != 0u) {
        count += value & 1u;
        value >>= 1u;
    }
    return count;
}

void test_legacy_qpsk_mapping() {
    constexpr float a = 0.7071067811865476f;
    const std::array<std::complex<float>, 4> expected{{
        {a, a}, {a, -a}, {-a, a}, {-a, -a}}};
    for (unsigned symbol = 0; symbol < expected.size(); ++symbol) {
        require(std::abs(SquareQAM::modulate(symbol, 2) - expected[symbol]) < 1e-6f,
                "QPSK compatibility mapping changed");
    }
}

void test_exhaustive_round_trip(size_t bits_per_symbol) {
    const unsigned order = 1u << bits_per_symbol;
    double average_power = 0.0;
    for (unsigned symbol = 0; symbol < order; ++symbol) {
        const auto point = SquareQAM::modulate(symbol, bits_per_symbol);
        average_power += std::norm(point);
        require(SquareQAM::demodulate(point, bits_per_symbol) == symbol,
                "noiseless QAM hard-decision round trip failed");
        std::array<float, 8> llrs{};
        SquareQAM::compute_llrs(point, 0.01f, bits_per_symbol, llrs.data());
        for (size_t bit = 0; bit < bits_per_symbol; ++bit) {
            const unsigned expected_bit =
                (symbol >> (bits_per_symbol - 1 - bit)) & 1u;
            require((llrs[bit] < 0.0f) == (expected_bit != 0u),
                    "QAM LLR sign disagrees with mapped label");
        }
    }
    average_power /= static_cast<double>(order);
    require(std::abs(average_power - 1.0) < 1e-6,
            "QAM constellation is not unit-average-power normalized");
}

double run_awgn(size_t bits_per_symbol, double snr_db, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<unsigned> labels(
        0u, (1u << bits_per_symbol) - 1u);
    std::normal_distribution<float> gaussian(0.0f, 1.0f);
    const float noise_variance = static_cast<float>(std::pow(10.0, -snr_db / 10.0));
    const float sigma = std::sqrt(noise_variance / 2.0f);
    const std::complex<float> channel(0.8f, 0.35f);
    constexpr size_t kSymbols = 200000;
    uint64_t errors = 0;
    for (size_t i = 0; i < kSymbols; ++i) {
        const unsigned tx = labels(rng);
        const std::complex<float> noise(
            sigma * gaussian(rng), sigma * gaussian(rng));
        const std::complex<float> rx =
            (channel * SquareQAM::modulate(tx, bits_per_symbol) + noise) / channel;
        const unsigned decoded = SquareQAM::demodulate(rx, bits_per_symbol);
        errors += bit_errors(tx, decoded, bits_per_symbol);
    }
    return static_cast<double>(errors) /
           static_cast<double>(kSymbols * bits_per_symbol);
}

}  // namespace

int main() {
    try {
        test_legacy_qpsk_mapping();
        for (const size_t bits : {size_t{2}, size_t{4}, size_t{6}, size_t{8}}) {
            test_exhaustive_round_trip(bits);
        }
        const double ber16 = run_awgn(4, 18.0, 0x1600u);
        const double ber64 = run_awgn(6, 26.0, 0x6400u);
        require(ber16 < 2.0e-3, "16-QAM AWGN BER regression");
        require(ber64 < 2.0e-3, "64-QAM AWGN BER regression");
        std::cout << "QAM tests passed: 16QAM_BER=" << ber16
                  << ", 64QAM_BER=" << ber64 << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "QAM test failed: " << error.what() << '\n';
        return 1;
    }
}
