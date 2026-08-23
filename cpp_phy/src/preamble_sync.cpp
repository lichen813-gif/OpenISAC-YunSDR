#include "openisac/preamble_sync.hpp"

#include "openisac/ofdm.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace openisac {

std::vector<std::complex<float>> generate_zc_frequency(
    std::size_t fft_size,
    unsigned root) {
    if (fft_size == 0u) {
        throw std::invalid_argument("ZC FFT size must be positive");
    }
    const unsigned normalized = root % static_cast<unsigned>(fft_size);
    if (normalized == 0u || std::gcd(normalized, static_cast<unsigned>(fft_size)) != 1u) {
        throw std::invalid_argument("ZC root must be non-zero and coprime to FFT size");
    }
    constexpr double pi = 3.14159265358979323846;
    const double delta = static_cast<double>(fft_size & 1u);
    std::vector<std::complex<float>> sequence(fft_size);
    for (std::size_t index = 0u; index < fft_size; ++index) {
        const double value = static_cast<double>(index);
        const double phase = -pi * static_cast<double>(normalized) *
                             value * (value + delta) / static_cast<double>(fft_size);
        sequence[index] = {
            static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase))};
    }
    return sequence;
}

std::vector<std::complex<float>> generate_zc_ofdm_symbol(
    std::size_t fft_size,
    std::size_t cp_length,
    unsigned root) {
    return ofdm_modulate(generate_zc_frequency(fft_size, root), cp_length);
}

TimingEstimate estimate_zc_timing(
    const std::vector<std::vector<std::complex<float>>>& receive_streams,
    const std::vector<std::complex<float>>& reference_symbol,
    std::size_t max_search_samples) {
    TimingEstimate estimate;
    estimate_zc_timing(
        receive_streams, reference_symbol, max_search_samples, estimate);
    return estimate;
}

void estimate_zc_timing(
    const std::vector<std::vector<std::complex<float>>>& receive_streams,
    const std::vector<std::complex<float>>& reference_symbol,
    std::size_t max_search_samples,
    TimingEstimate& estimate) {
    estimate_zc_timing_window(
        receive_streams, reference_symbol, 0u, max_search_samples, estimate);
}

void estimate_zc_timing_window(
    const std::vector<std::vector<std::complex<float>>>& receive_streams,
    const std::vector<std::complex<float>>& reference_symbol,
    std::size_t search_begin,
    std::size_t search_end,
    TimingEstimate& estimate) {
    if (receive_streams.empty() || reference_symbol.empty()) {
        throw std::invalid_argument("ZC timing requires receive branches and a reference");
    }
    if (search_begin > search_end) {
        throw std::invalid_argument("ZC timing search range is reversed");
    }
    const std::size_t required = search_end + reference_symbol.size();
    for (const auto& branch : receive_streams) {
        if (branch.size() < required) {
            throw std::invalid_argument("receive stream is shorter than ZC search window");
        }
    }
    double reference_energy = 0.0;
    for (const auto& value : reference_symbol) {
        reference_energy += std::norm(value);
    }
    if (reference_energy <= std::numeric_limits<double>::min()) {
        throw std::invalid_argument("ZC reference energy must be positive");
    }

    estimate.search_begin = search_begin;
    estimate.metrics.resize(search_end - search_begin + 1u);
    for (std::size_t candidate = search_begin; candidate <= search_end; ++candidate) {
        double combined = 0.0;
        for (const auto& branch : receive_streams) {
            std::complex<double> correlation{};
            double window_energy = 0.0;
            for (std::size_t sample = 0u; sample < reference_symbol.size(); ++sample) {
                const auto received = branch[candidate + sample];
                correlation += static_cast<std::complex<double>>(received) *
                               std::conj(static_cast<std::complex<double>>(reference_symbol[sample]));
                window_energy += std::norm(received);
            }
            const double denominator = std::max(reference_energy * window_energy, 1.0e-30);
            combined += std::norm(correlation) / denominator;
        }
        estimate.metrics[candidate - search_begin] = static_cast<float>(
            combined / static_cast<double>(receive_streams.size()));
    }
    const auto peak = std::max_element(estimate.metrics.begin(), estimate.metrics.end());
    estimate.offset = search_begin +
        static_cast<std::size_t>(peak - estimate.metrics.begin());
    estimate.peak_metric = *peak;
}

float estimate_cp_cfo_normalized(
    const std::vector<std::vector<std::complex<float>>>& receive_streams,
    std::size_t frame_offset,
    std::size_t fft_size,
    std::size_t cp_length,
    std::size_t symbol_count,
    std::size_t cp_skip_samples) {
    if (receive_streams.empty() || fft_size == 0u || cp_length == 0u ||
        symbol_count == 0u || cp_skip_samples >= cp_length) {
        throw std::invalid_argument("invalid CP CFO estimator dimensions");
    }
    const std::size_t symbol_samples = fft_size + cp_length;
    const std::size_t required = frame_offset + symbol_count * symbol_samples;
    for (const auto& branch : receive_streams) {
        if (branch.size() < required) {
            throw std::invalid_argument("receive stream is shorter than CP CFO window");
        }
    }
    std::complex<double> correlation{};
    for (const auto& branch : receive_streams) {
        for (std::size_t symbol = 0u; symbol < symbol_count; ++symbol) {
            const std::size_t start = frame_offset + symbol * symbol_samples;
            for (std::size_t cp = cp_skip_samples; cp < cp_length; ++cp) {
                correlation +=
                    std::conj(static_cast<std::complex<double>>(branch[start + cp])) *
                    static_cast<std::complex<double>>(branch[start + fft_size + cp]);
            }
        }
    }
    if (std::norm(correlation) <= std::numeric_limits<double>::min()) {
        throw std::runtime_error("CP CFO correlation has zero magnitude");
    }
    constexpr double two_pi = 6.28318530717958647692;
    return static_cast<float>(std::arg(correlation) / two_pi);
}

void apply_cfo_normalized_inplace(
    std::vector<std::vector<std::complex<float>>>& streams,
    float normalized_cfo,
    std::size_t fft_size) {
    if (fft_size == 0u || !std::isfinite(normalized_cfo)) {
        throw std::invalid_argument("invalid CFO correction parameters");
    }
    constexpr double two_pi = 6.28318530717958647692;
    const double radians_per_sample =
        two_pi * static_cast<double>(normalized_cfo) / static_cast<double>(fft_size);
    const std::complex<double> phase_step{
        std::cos(radians_per_sample), std::sin(radians_per_sample)};
    for (auto& branch : streams) {
        std::complex<double> oscillator{1.0, 0.0};
        for (std::size_t sample = 0u; sample < branch.size(); ++sample) {
            branch[sample] *= std::complex<float>{
                static_cast<float>(oscillator.real()),
                static_cast<float>(oscillator.imag())};
            oscillator *= phase_step;
        }
    }
}

}  // namespace openisac
