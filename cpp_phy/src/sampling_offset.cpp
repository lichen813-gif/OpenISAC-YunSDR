#include "openisac/sampling_offset.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace openisac {

std::vector<std::vector<std::complex<float>>> resample_sfo_cubic(
    const std::vector<std::vector<std::complex<float>>>& streams,
    float sfo_ppm) {
    std::vector<std::vector<std::complex<float>>> output;
    resample_sfo_cubic(streams, sfo_ppm, output);
    return output;
}

void resample_sfo_cubic(
    const std::vector<std::vector<std::complex<float>>>& streams,
    float sfo_ppm,
    std::vector<std::vector<std::complex<float>>>& output) {
    if (streams.empty() || !std::isfinite(sfo_ppm)) {
        throw std::invalid_argument("SFO resampler requires finite ppm and receive streams");
    }
    if (static_cast<const void*>(&streams) == static_cast<const void*>(&output)) {
        throw std::invalid_argument("SFO resampler input/output must not alias");
    }
    const std::size_t sample_count = streams.front().size();
    for (const auto& branch : streams) {
        if (branch.size() != sample_count) {
            throw std::invalid_argument("SFO stream branches must have equal length");
        }
    }
    const double ratio = 1.0 + static_cast<double>(sfo_ppm) * 1.0e-6;
    if (ratio <= 0.0) {
        throw std::invalid_argument("SFO ppm produces a non-positive sample rate");
    }
    output.resize(streams.size());
    for (auto& branch : output) {
        branch.resize(sample_count);
    }
    if (sfo_ppm == 0.0f) {
        output = streams;
        return;
    }
    for (std::size_t target = 0u; target < sample_count; ++target) {
        const double source = static_cast<double>(target) / ratio;
        const auto center = static_cast<long long>(std::floor(source));
        const double fraction = source - static_cast<double>(center);
        const double weights[4]{
            -fraction * (1.0 - fraction) * (2.0 - fraction) / 6.0,
            (1.0 + fraction) * (1.0 - fraction) * (2.0 - fraction) / 2.0,
            (1.0 + fraction) * fraction * (2.0 - fraction) / 2.0,
            -(1.0 + fraction) * fraction * (1.0 - fraction) / 6.0};
        for (std::size_t branch = 0u; branch < streams.size(); ++branch) {
            std::complex<double> interpolated{};
            for (int tap = 0; tap < 4; ++tap) {
                const long long index = center + static_cast<long long>(tap) - 1ll;
                if (index >= 0ll && index < static_cast<long long>(sample_count)) {
                    interpolated += static_cast<std::complex<double>>(
                                        streams[branch][static_cast<std::size_t>(index)]) *
                                    weights[tap];
                }
            }
            output[branch][target] = static_cast<std::complex<float>>(interpolated);
        }
    }
}

float inverse_sfo_ppm(float sfo_ppm) {
    if (!std::isfinite(sfo_ppm)) {
        throw std::invalid_argument("SFO ppm must be finite");
    }
    const double ratio = 1.0 + static_cast<double>(sfo_ppm) * 1.0e-6;
    if (ratio <= 0.0) {
        throw std::invalid_argument("SFO ppm produces a non-positive sample rate");
    }
    return static_cast<float>((1.0 / ratio - 1.0) * 1.0e6);
}

PhaseSlopeEstimate estimate_sfo_phase_slope(
    const std::vector<std::complex<float>>& receive_grid,
    const std::vector<std::uint16_t>& phase_reference_fft_indices,
    std::size_t fft_size,
    std::size_t receive_antennas,
    std::size_t samples_per_symbol) {
    if (fft_size == 0u || receive_antennas == 0u || samples_per_symbol == 0u ||
        phase_reference_fft_indices.size() < 2u ||
        receive_grid.size() != 2u * fft_size * receive_antennas) {
        throw std::invalid_argument("invalid phase-slope estimator dimensions");
    }
    struct Reference {
        double subcarrier;
        std::complex<double> correlation;
    };
    std::vector<Reference> references;
    references.reserve(phase_reference_fft_indices.size());
    for (const auto fft_index : phase_reference_fft_indices) {
        if (fft_index >= fft_size) {
            throw std::invalid_argument("phase reference lies outside the FFT");
        }
        std::complex<double> correlation{};
        for (std::size_t rx = 0u; rx < receive_antennas; ++rx) {
            const auto first = receive_grid[fft_index * receive_antennas + rx];
            const auto second = receive_grid[
                (fft_size + fft_index) * receive_antennas + rx];
            correlation += std::conj(static_cast<std::complex<double>>(first)) *
                           static_cast<std::complex<double>>(second);
        }
        const double centered = fft_index < fft_size / 2u
                                    ? static_cast<double>(fft_index)
                                    : static_cast<double>(fft_index) -
                                          static_cast<double>(fft_size);
        references.push_back({centered, correlation});
    }
    std::sort(references.begin(), references.end(), [](const auto& left, const auto& right) {
        return left.subcarrier < right.subcarrier;
    });

    std::vector<double> phases(references.size());
    phases[0] = std::arg(references[0].correlation);
    constexpr double two_pi = 6.28318530717958647692;
    for (std::size_t index = 1u; index < references.size(); ++index) {
        double phase = std::arg(references[index].correlation);
        while (phase - phases[index - 1u] > 0.5 * two_pi) {
            phase -= two_pi;
        }
        while (phase - phases[index - 1u] < -0.5 * two_pi) {
            phase += two_pi;
        }
        phases[index] = phase;
    }

    double sum_w = 0.0;
    double sum_x = 0.0;
    double sum_y = 0.0;
    double sum_xx = 0.0;
    double sum_xy = 0.0;
    for (std::size_t index = 0u; index < references.size(); ++index) {
        const double weight = std::max(std::abs(references[index].correlation), 1.0e-15);
        const double x = references[index].subcarrier;
        const double y = phases[index];
        sum_w += weight;
        sum_x += weight * x;
        sum_y += weight * y;
        sum_xx += weight * x * x;
        sum_xy += weight * x * y;
    }
    const double denominator = sum_w * sum_xx - sum_x * sum_x;
    if (std::abs(denominator) <= std::numeric_limits<double>::epsilon()) {
        throw std::runtime_error("phase references cannot determine an SFO slope");
    }
    const double slope = (sum_w * sum_xy - sum_x * sum_y) / denominator;
    const double intercept = (sum_y - slope * sum_x) / sum_w;
    std::complex<double> aligned{};
    double magnitude_sum = 0.0;
    for (const auto& reference : references) {
        const double fitted = intercept + slope * reference.subcarrier;
        aligned += reference.correlation * std::polar(1.0, -fitted);
        magnitude_sum += std::abs(reference.correlation);
    }
    PhaseSlopeEstimate estimate;
    estimate.intercept_radians = static_cast<float>(intercept);
    estimate.slope_radians_per_subcarrier = static_cast<float>(slope);
    estimate.sfo_ppm = static_cast<float>(
        -slope * static_cast<double>(fft_size) /
        (two_pi * static_cast<double>(samples_per_symbol)) * 1.0e6);
    estimate.coherence = static_cast<float>(
        std::abs(aligned) / std::max(magnitude_sum, 1.0e-30));
    return estimate;
}

PhaseSlopeEstimate estimate_sfo_phase_slope_sparse(
    const std::vector<std::complex<float>>& sparse_grid,
    const std::vector<std::uint16_t>& phase_reference_fft_indices,
    std::size_t fft_size,
    std::size_t receive_antennas,
    std::size_t samples_per_symbol) {
    const std::size_t references_count = phase_reference_fft_indices.size();
    if (fft_size == 0u || receive_antennas == 0u || samples_per_symbol == 0u ||
        references_count < 2u ||
        sparse_grid.size() != 2u * references_count * receive_antennas) {
        throw std::invalid_argument("invalid sparse phase-slope dimensions");
    }
    struct Reference {
        double subcarrier;
        std::complex<double> correlation;
    };
    std::vector<Reference> references;
    references.reserve(references_count);
    for (std::size_t reference = 0u; reference < references_count; ++reference) {
        const auto fft_index = phase_reference_fft_indices[reference];
        if (fft_index >= fft_size) {
            throw std::invalid_argument("phase reference lies outside the FFT");
        }
        std::complex<double> correlation{};
        for (std::size_t rx = 0u; rx < receive_antennas; ++rx) {
            const auto first = sparse_grid[reference * receive_antennas + rx];
            const auto second = sparse_grid[
                (references_count + reference) * receive_antennas + rx];
            correlation += std::conj(static_cast<std::complex<double>>(first)) *
                static_cast<std::complex<double>>(second);
        }
        const double centered = fft_index < fft_size / 2u
            ? static_cast<double>(fft_index)
            : static_cast<double>(fft_index) - static_cast<double>(fft_size);
        references.push_back({centered, correlation});
    }
    std::sort(references.begin(), references.end(), [](const auto& left,
                                                        const auto& right) {
        return left.subcarrier < right.subcarrier;
    });
    constexpr double two_pi = 6.28318530717958647692;
    std::vector<double> phases(references.size());
    phases.front() = std::arg(references.front().correlation);
    for (std::size_t index = 1u; index < references.size(); ++index) {
        double phase = std::arg(references[index].correlation);
        while (phase - phases[index - 1u] > 0.5 * two_pi) phase -= two_pi;
        while (phase - phases[index - 1u] < -0.5 * two_pi) phase += two_pi;
        phases[index] = phase;
    }
    double sum_w = 0.0;
    double sum_x = 0.0;
    double sum_y = 0.0;
    double sum_xx = 0.0;
    double sum_xy = 0.0;
    for (std::size_t index = 0u; index < references.size(); ++index) {
        const double weight = std::max(
            std::abs(references[index].correlation), 1.0e-15);
        const double x = references[index].subcarrier;
        const double y = phases[index];
        sum_w += weight;
        sum_x += weight * x;
        sum_y += weight * y;
        sum_xx += weight * x * x;
        sum_xy += weight * x * y;
    }
    const double denominator = sum_w * sum_xx - sum_x * sum_x;
    if (std::abs(denominator) <= std::numeric_limits<double>::epsilon()) {
        throw std::runtime_error("phase references cannot determine an SFO slope");
    }
    const double slope = (sum_w * sum_xy - sum_x * sum_y) / denominator;
    const double intercept = (sum_y - slope * sum_x) / sum_w;
    std::complex<double> aligned{};
    double magnitude_sum = 0.0;
    for (const auto& reference : references) {
        const double fitted = intercept + slope * reference.subcarrier;
        aligned += reference.correlation * std::polar(1.0, -fitted);
        magnitude_sum += std::abs(reference.correlation);
    }
    PhaseSlopeEstimate estimate;
    estimate.intercept_radians = static_cast<float>(intercept);
    estimate.slope_radians_per_subcarrier = static_cast<float>(slope);
    estimate.sfo_ppm = static_cast<float>(
        -slope * static_cast<double>(fft_size) /
        (two_pi * static_cast<double>(samples_per_symbol)) * 1.0e6);
    estimate.coherence = static_cast<float>(
        std::abs(aligned) / std::max(magnitude_sum, 1.0e-30));
    return estimate;
}

PhaseSlopeEstimate estimate_phase_slope_from_correlations(
    const std::vector<std::complex<double>>& correlations,
    const std::vector<std::uint16_t>& reference_fft_indices,
    std::size_t fft_size) {
    if (fft_size == 0u || reference_fft_indices.size() < 2u ||
        correlations.size() != reference_fft_indices.size()) {
        throw std::invalid_argument(
            "invalid correlation phase-slope dimensions");
    }
    struct Reference {
        double subcarrier;
        std::complex<double> correlation;
    };
    std::vector<Reference> references;
    references.reserve(reference_fft_indices.size());
    for (std::size_t index = 0u;
         index < reference_fft_indices.size(); ++index) {
        const auto fft = reference_fft_indices[index];
        if (fft >= fft_size) {
            throw std::invalid_argument("phase-tracking pilot lies outside FFT");
        }
        const double centered = fft < fft_size / 2u
            ? static_cast<double>(fft)
            : static_cast<double>(fft) - static_cast<double>(fft_size);
        references.push_back({centered, correlations[index]});
    }
    std::sort(references.begin(), references.end(), [](const auto& left,
                                                        const auto& right) {
        return left.subcarrier < right.subcarrier;
    });
    constexpr double two_pi = 6.28318530717958647692;
    std::vector<double> phases(references.size());
    phases.front() = std::arg(references.front().correlation);
    for (std::size_t index = 1u; index < references.size(); ++index) {
        double phase = std::arg(references[index].correlation);
        while (phase - phases[index - 1u] > 0.5 * two_pi) phase -= two_pi;
        while (phase - phases[index - 1u] < -0.5 * two_pi) phase += two_pi;
        phases[index] = phase;
    }
    double sum_w = 0.0;
    double sum_x = 0.0;
    double sum_y = 0.0;
    double sum_xx = 0.0;
    double sum_xy = 0.0;
    for (std::size_t index = 0u; index < references.size(); ++index) {
        const double weight = std::max(
            std::abs(references[index].correlation), 1.0e-15);
        const double x = references[index].subcarrier;
        const double y = phases[index];
        sum_w += weight;
        sum_x += weight * x;
        sum_y += weight * y;
        sum_xx += weight * x * x;
        sum_xy += weight * x * y;
    }
    const double denominator = sum_w * sum_xx - sum_x * sum_x;
    if (std::abs(denominator) <= std::numeric_limits<double>::epsilon()) {
        throw std::runtime_error("reference pilots cannot determine phase slope");
    }
    PhaseSlopeEstimate estimate;
    estimate.slope_radians_per_subcarrier = static_cast<float>(
        (sum_w * sum_xy - sum_x * sum_y) / denominator);
    estimate.intercept_radians = static_cast<float>(
        (sum_y - estimate.slope_radians_per_subcarrier * sum_x) / sum_w);
    std::complex<double> aligned{};
    double magnitude_sum = 0.0;
    for (const auto& reference : references) {
        const double fitted = estimate.intercept_radians +
            estimate.slope_radians_per_subcarrier * reference.subcarrier;
        aligned += reference.correlation * std::polar(1.0, -fitted);
        magnitude_sum += std::abs(reference.correlation);
    }
    estimate.coherence = static_cast<float>(
        std::abs(aligned) / std::max(magnitude_sum, 1.0e-30));
    return estimate;
}

PhaseSlopeEstimate estimate_reference_residual_phase_slope_nxn(
    const std::vector<std::complex<float>>& receive_grid,
    const std::vector<std::complex<float>>& reference_grid,
    const std::vector<std::uint16_t>& pilot_fft_indices,
    const std::vector<ChannelNxN>& channels,
    std::size_t symbol_index,
    std::size_t fft_size,
    std::size_t ports) {
    if (fft_size == 0u || ports == 0u ||
        ports > maximum_spatial_streams || pilot_fft_indices.size() < 2u ||
        receive_grid.size() != reference_grid.size() ||
        receive_grid.size() % (fft_size * ports) != 0u ||
        channels.size() * ports != receive_grid.size() ||
        symbol_index >= receive_grid.size() / (fft_size * ports)) {
        throw std::invalid_argument(
            "invalid reference residual phase-slope dimensions");
    }
    std::vector<std::complex<double>> correlations;
    correlations.reserve(pilot_fft_indices.size());
    for (const auto fft : pilot_fft_indices) {
        if (fft >= fft_size) {
            throw std::invalid_argument("phase-tracking pilot lies outside FFT");
        }
        const auto& channel = channels[symbol_index * fft_size + fft];
        std::complex<double> correlation{};
        for (std::size_t rx = 0u; rx < ports; ++rx) {
            std::complex<float> predicted{};
            for (std::size_t tx = 0u; tx < ports; ++tx) {
                predicted += channel.values[
                    rx * maximum_spatial_streams + tx] * reference_grid[
                    (symbol_index * fft_size + fft) * ports + tx];
            }
            const auto received = receive_grid[
                (symbol_index * fft_size + fft) * ports + rx];
            correlation += std::conj(
                static_cast<std::complex<double>>(predicted)) *
                static_cast<std::complex<double>>(received);
        }
        correlations.push_back(correlation);
    }
    return estimate_phase_slope_from_correlations(
        correlations, pilot_fft_indices, fft_size);
}

void correct_second_symbol_phase_inplace(
    std::vector<std::complex<float>>& receive_grid,
    const PhaseSlopeEstimate& estimate,
    std::size_t fft_size,
    std::size_t receive_antennas) {
    if (fft_size == 0u || receive_antennas == 0u ||
        receive_grid.size() != 2u * fft_size * receive_antennas) {
        throw std::invalid_argument("invalid differential-phase correction dimensions");
    }
    correct_symbol_phase_inplace(
        receive_grid, estimate, fft_size, receive_antennas, 1u, 1.0f);
}

void correct_symbol_phase_inplace(
    std::vector<std::complex<float>>& receive_grid,
    const PhaseSlopeEstimate& estimate,
    std::size_t fft_size,
    std::size_t receive_antennas,
    std::size_t symbol_index,
    float symbol_intervals) {
    if (fft_size == 0u || receive_antennas == 0u ||
        receive_grid.size() % (fft_size * receive_antennas) != 0u ||
        symbol_index >= receive_grid.size() / (fft_size * receive_antennas) ||
        !std::isfinite(symbol_intervals)) {
        throw std::invalid_argument("invalid OFDM-symbol phase correction dimensions");
    }
    const float intercept = estimate.intercept_radians * symbol_intervals;
    const float slope =
        estimate.slope_radians_per_subcarrier * symbol_intervals;
    const std::complex<float> phase_step = std::polar(1.0f, -slope);
    std::complex<float> correction = std::polar(1.0f, -intercept);
    const std::size_t centered_wrap = fft_size / 2u;
    for (std::size_t fft = 0u; fft < fft_size; ++fft) {
        if (fft == centered_wrap) {
            const float wrapped_phase = intercept -
                slope * static_cast<float>(centered_wrap);
            correction = std::polar(1.0f, -wrapped_phase);
        }
        for (std::size_t rx = 0u; rx < receive_antennas; ++rx) {
            receive_grid[
                (symbol_index * fft_size + fft) * receive_antennas + rx] *=
                correction;
        }
        correction *= phase_step;
    }
}

}  // namespace openisac
