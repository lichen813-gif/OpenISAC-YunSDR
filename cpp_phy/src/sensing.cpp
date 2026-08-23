#include "openisac/sensing.hpp"

#include "openisac/ofdm.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace openisac {
namespace {

constexpr float speed_of_light_mps = 299792458.0f;

bool is_power_of_two(std::size_t value) {
    return value != 0u && (value & (value - 1u)) == 0u;
}

void validate_config(const DynamicSensingConfig& config) {
    if (config.fft_size == 0u || config.data_symbols != 2u ||
        config.transmit_ports != 2u || config.receive_ports != 2u ||
        config.selected_transmit_port >= config.transmit_ports ||
        config.selected_receive_port >= config.receive_ports ||
        config.coherent_frames == 0u ||
        config.range_fft_size < config.fft_size ||
        config.doppler_fft_size < config.coherent_frames ||
        !is_power_of_two(config.fft_size) ||
        !is_power_of_two(config.range_fft_size) ||
        !is_power_of_two(config.doppler_fft_size) ||
        !std::isfinite(config.subcarrier_spacing_hz) ||
        config.subcarrier_spacing_hz <= 0.0f ||
        !std::isfinite(config.frame_period_seconds) ||
        config.frame_period_seconds <= 0.0f ||
        !std::isfinite(config.center_frequency_hz) ||
        config.center_frequency_hz <= 0.0f ||
        !std::isfinite(config.propagation_path_factor) ||
        config.propagation_path_factor <= 0.0f ||
        !std::isfinite(config.regularization) || config.regularization < 0.0f ||
        !std::isfinite(config.minimum_reference_power) ||
        config.minimum_reference_power <= 0.0f ||
        !std::isfinite(config.minimum_relative_determinant) ||
        config.minimum_relative_determinant < 0.0f ||
        config.minimum_relative_determinant >= 1.0f ||
        (config.enable_cfar_detection &&
         config.cfar_training_doppler == 0u &&
         config.cfar_training_range == 0u) ||
        !std::isfinite(config.cfar_false_alarm_probability) ||
        config.cfar_false_alarm_probability <= 0.0f ||
        config.cfar_false_alarm_probability >= 1.0f ||
        config.cfar_max_detections == 0u ||
        (config.maximum_range_bin != 0u &&
         (config.maximum_range_bin <= config.minimum_range_bin ||
          config.maximum_range_bin > config.range_fft_size)) ||
        config.minimum_range_bin >= config.range_fft_size) {
        throw std::invalid_argument("invalid dynamic sensing configuration");
    }
}

std::size_t grid_index(
    std::size_t symbol,
    std::size_t subcarrier,
    std::size_t port,
    std::size_t fft_size,
    std::size_t ports) {
    return (symbol * fft_size + subcarrier) * ports + port;
}

std::size_t centered_to_native(int centered, std::size_t fft_size) {
    return centered >= 0
        ? static_cast<std::size_t>(centered)
        : static_cast<std::size_t>(static_cast<int>(fft_size) + centered);
}

float hamming(std::size_t index, std::size_t count) {
    if (count <= 1u) {
        return 1.0f;
    }
    constexpr float two_pi = 6.28318530717958647692f;
    return 0.54f - 0.46f * std::cos(
        two_pi * static_cast<float>(index) /
        static_cast<float>(count - 1u));
}

struct CfarCandidate {
    std::size_t doppler = 0u;
    std::size_t range = 0u;
    float power = 0.0f;
    float noise_power = 0.0f;
    float threshold_power = 0.0f;
};

double rectangle_sum(
    const std::vector<double>& integral,
    std::size_t stride,
    std::size_t row_begin,
    std::size_t row_end,
    std::size_t column_begin,
    std::size_t column_end) {
    return integral[row_end * stride + column_end] -
        integral[row_begin * stride + column_end] -
        integral[row_end * stride + column_begin] +
        integral[row_begin * stride + column_begin];
}

}  // namespace

void estimate_dynamic_sensing_channel_2x2(
    const std::vector<std::complex<float>>& transmit_grid,
    const std::vector<std::complex<float>>& receive_grid,
    const DynamicSensingConfig& config,
    DynamicSensingChannelEstimate& estimate) {
    validate_config(config);
    const std::size_t expected =
        config.data_symbols * config.fft_size * config.transmit_ports;
    const std::size_t expected_receive =
        config.data_symbols * config.fft_size * config.receive_ports;
    if (transmit_grid.size() != expected ||
        receive_grid.size() != expected_receive) {
        throw std::invalid_argument("dynamic sensing grid dimensions do not match");
    }

    estimate.frequency_response.assign(config.fft_size, {});
    estimate.active_subcarrier_mask.assign(config.fft_size, 0u);
    estimate.direct_estimate_mask.assign(config.fft_size, 0u);
    estimate.active_subcarriers = 0u;
    estimate.directly_estimated_subcarriers = 0u;
    estimate.interpolated_subcarriers = 0u;
    estimate.ill_conditioned_subcarriers = 0u;

    const std::size_t selected = config.selected_transmit_port;
    const std::size_t other = 1u - selected;
    const float minimum_power = config.minimum_reference_power;
    for (std::size_t fft = 0u; fft < config.fft_size; ++fft) {
        float selected_power = 0.0f;
        float other_power = 0.0f;
        std::complex<float> cross{};
        std::complex<float> selected_projection{};
        std::complex<float> other_projection{};
        for (std::size_t symbol = 0u;
             symbol < config.data_symbols; ++symbol) {
            const auto x_selected = transmit_grid[grid_index(
                symbol, fft, selected, config.fft_size,
                config.transmit_ports)];
            const auto x_other = transmit_grid[grid_index(
                symbol, fft, other, config.fft_size,
                config.transmit_ports)];
            const auto received = receive_grid[grid_index(
                symbol, fft, config.selected_receive_port,
                config.fft_size, config.receive_ports)];
            selected_power += std::norm(x_selected);
            other_power += std::norm(x_other);
            cross += std::conj(x_selected) * x_other;
            selected_projection += std::conj(x_selected) * received;
            other_projection += std::conj(x_other) * received;
        }
        if (selected_power + other_power <= minimum_power) {
            continue;
        }
        estimate.active_subcarrier_mask[fft] = 1u;
        ++estimate.active_subcarriers;

        bool valid = false;
        std::complex<float> selected_channel{};
        if (selected_power > minimum_power && other_power <= minimum_power) {
            selected_channel = selected_projection / selected_power;
            valid = true;
        } else if (selected_power > minimum_power &&
                   other_power > minimum_power) {
            const float determinant =
                selected_power * other_power - std::norm(cross);
            const float determinant_scale = selected_power * other_power;
            if (determinant >
                config.minimum_relative_determinant * determinant_scale) {
                const float diagonal_loading = config.regularization *
                    (selected_power + other_power);
                const float loaded_selected = selected_power + diagonal_loading;
                const float loaded_other = other_power + diagonal_loading;
                const float loaded_determinant =
                    loaded_selected * loaded_other - std::norm(cross);
                if (loaded_determinant > minimum_power) {
                    selected_channel =
                        (loaded_other * selected_projection -
                         cross * other_projection) /
                        loaded_determinant;
                    valid = true;
                }
            }
            if (!valid) {
                ++estimate.ill_conditioned_subcarriers;
            }
        }
        if (valid && std::isfinite(selected_channel.real()) &&
            std::isfinite(selected_channel.imag())) {
            estimate.frequency_response[fft] = selected_channel;
            estimate.direct_estimate_mask[fft] = 1u;
            ++estimate.directly_estimated_subcarriers;
        }
    }

    if (estimate.directly_estimated_subcarriers == 0u) {
        throw std::runtime_error(
            "known-waveform sensing could not estimate the selected Tx port");
    }

    const int half = static_cast<int>(config.fft_size / 2u);
    estimate.interpolation_left.assign(config.fft_size, -1);
    estimate.interpolation_right.assign(config.fft_size, -1);
    int last = -1;
    for (std::size_t order = 0u; order < config.fft_size; ++order) {
        const int centered = static_cast<int>(order) - half;
        const std::size_t native = centered_to_native(centered, config.fft_size);
        if (estimate.direct_estimate_mask[native] != 0u) {
            last = static_cast<int>(order);
        }
        estimate.interpolation_left[order] = last;
    }
    last = -1;
    for (std::size_t reverse = config.fft_size; reverse > 0u; --reverse) {
        const std::size_t order = reverse - 1u;
        const int centered = static_cast<int>(order) - half;
        const std::size_t native = centered_to_native(centered, config.fft_size);
        if (estimate.direct_estimate_mask[native] != 0u) {
            last = static_cast<int>(order);
        }
        estimate.interpolation_right[order] = last;
    }

    for (std::size_t order = 0u; order < config.fft_size; ++order) {
        const int centered = static_cast<int>(order) - half;
        const std::size_t native = centered_to_native(centered, config.fft_size);
        if (estimate.active_subcarrier_mask[native] == 0u ||
            estimate.direct_estimate_mask[native] != 0u) {
            continue;
        }
        const int left = estimate.interpolation_left[order];
        const int right = estimate.interpolation_right[order];
        if (left < 0 && right < 0) {
            continue;
        }
        if (left < 0 || right < 0 || left == right) {
            const int nearest = left >= 0 ? left : right;
            const int nearest_centered = nearest - half;
            estimate.frequency_response[native] = estimate.frequency_response[
                centered_to_native(nearest_centered, config.fft_size)];
        } else {
            const float fraction = static_cast<float>(
                static_cast<int>(order) - left) /
                static_cast<float>(right - left);
            const auto left_value = estimate.frequency_response[
                centered_to_native(left - half, config.fft_size)];
            const auto right_value = estimate.frequency_response[
                centered_to_native(right - half, config.fft_size)];
            estimate.frequency_response[native] =
                left_value + fraction * (right_value - left_value);
        }
        ++estimate.interpolated_subcarriers;
    }
}

struct DynamicSensingProcessor::Impl {
    explicit Impl(const DynamicSensingConfig& config_value)
        : config(config_value),
          frequency_history(
              config.coherent_frames * config.fft_size,
              std::complex<float>{}),
          range_history(
              config.coherent_frames * config.range_fft_size,
              std::complex<float>{}),
          clutter_mean(config.fft_size, std::complex<float>{}),
          range_scratch(config.range_fft_size, std::complex<float>{}),
          doppler_scratch(
              config.doppler_fft_size, std::complex<float>{}),
          cfar_integral(
              (config.doppler_fft_size + 1u) *
                  (config.range_fft_size + 1u),
              0.0) {
        validate_config(config);
        result.range_doppler_map.resize(
            config.doppler_fft_size * config.range_fft_size);
    }

    void clear_batch() noexcept {
        frame_count = 0u;
        direct_estimates = 0u;
        interpolated_estimates = 0u;
        batch_active_mask.clear();
    }

    void compute_result() {
        const std::size_t active_count = static_cast<std::size_t>(std::count(
            batch_active_mask.begin(), batch_active_mask.end(),
            static_cast<std::uint8_t>(1u)));
        if (active_count == 0u) {
            throw std::runtime_error("dynamic sensing batch has no active subcarriers");
        }

        std::fill(
            clutter_mean.begin(), clutter_mean.end(),
            std::complex<float>{});
        if (config.enable_static_clutter_suppression) {
            const float inverse_frames =
                1.0f / static_cast<float>(config.coherent_frames);
            for (std::size_t frame = 0u;
                 frame < config.coherent_frames; ++frame) {
                for (std::size_t fft = 0u; fft < config.fft_size; ++fft) {
                    clutter_mean[fft] +=
                        frequency_history[frame * config.fft_size + fft] *
                        inverse_frames;
                }
            }
        }

        for (std::size_t frame = 0u;
             frame < config.coherent_frames; ++frame) {
            std::fill(range_scratch.begin(), range_scratch.end(),
                      std::complex<float>{});
            std::size_t active_index = 0u;
            const int half = static_cast<int>(config.fft_size / 2u);
            for (std::size_t order = 0u; order < config.fft_size; ++order) {
                const int centered = static_cast<int>(order) - half;
                const std::size_t native = centered_to_native(
                    centered, config.fft_size);
                if (batch_active_mask[native] == 0u) {
                    continue;
                }
                const float window = config.enable_range_window
                    ? hamming(active_index, active_count)
                    : 1.0f;
                const std::size_t padded_native = centered >= 0
                    ? static_cast<std::size_t>(centered)
                    : static_cast<std::size_t>(
                        static_cast<int>(config.range_fft_size) + centered);
                range_scratch[padded_native] =
                    (frequency_history[frame * config.fft_size + native] -
                     clutter_mean[native]) *
                    window;
                ++active_index;
            }
            fft_inplace(range_scratch, true);
            std::copy(
                range_scratch.begin(), range_scratch.end(),
                range_history.begin() + static_cast<std::ptrdiff_t>(
                    frame * config.range_fft_size));
        }

        std::fill(
            result.range_doppler_map.begin(),
            result.range_doppler_map.end(), std::complex<float>{});
        for (std::size_t range = 0u;
             range < config.range_fft_size; ++range) {
            std::fill(doppler_scratch.begin(), doppler_scratch.end(),
                      std::complex<float>{});
            for (std::size_t frame = 0u;
                 frame < config.coherent_frames; ++frame) {
                const float window = config.enable_doppler_window
                    ? hamming(frame, config.coherent_frames)
                    : 1.0f;
                doppler_scratch[frame] =
                    range_history[frame * config.range_fft_size + range] *
                    window;
            }
            fft_inplace(doppler_scratch, false);
            for (std::size_t native = 0u;
                 native < config.doppler_fft_size; ++native) {
                const std::size_t shifted =
                    (native + config.doppler_fft_size / 2u) %
                    config.doppler_fft_size;
                result.range_doppler_map[
                    shifted * config.range_fft_size + range] =
                    doppler_scratch[native];
            }
        }

        const float range_spacing = speed_of_light_mps /
            (config.propagation_path_factor *
             static_cast<float>(config.range_fft_size) *
             config.subcarrier_spacing_hz);
        const float doppler_spacing = 1.0f /
            (static_cast<float>(config.doppler_fft_size) *
             config.frame_period_seconds);
        const float velocity_spacing =
            doppler_spacing * speed_of_light_mps /
            (config.propagation_path_factor * config.center_frequency_hz);
        result.range_bin_spacing_m = range_spacing;
        result.doppler_bin_spacing_hz = doppler_spacing;
        result.velocity_bin_spacing_mps = velocity_spacing;
        result.static_clutter_suppression_applied =
            config.enable_static_clutter_suppression;
        result.strongest_peak = DynamicSensingPeak{};
        float strongest_power = -1.0f;
        const std::size_t dc = config.doppler_fft_size / 2u;
        const std::size_t range_stop = config.maximum_range_bin == 0u
            ? config.range_fft_size / 2u
            : config.maximum_range_bin;
        for (std::size_t doppler = 0u;
             doppler < config.doppler_fft_size; ++doppler) {
            if (config.doppler_dc_exclusion_bins > 0u) {
                const std::size_t distance = doppler > dc
                    ? doppler - dc : dc - doppler;
                if (distance <= config.doppler_dc_exclusion_bins) {
                    continue;
                }
            }
            for (std::size_t range = config.minimum_range_bin;
                 range < range_stop; ++range) {
                const float power = std::norm(result.range_doppler_map[
                    doppler * config.range_fft_size + range]);
                if (power > strongest_power && std::isfinite(power)) {
                    strongest_power = power;
                    const int centered_doppler =
                        static_cast<int>(doppler) - static_cast<int>(dc);
                    result.strongest_peak.range_bin = range;
                    result.strongest_peak.doppler_bin = doppler;
                    result.strongest_peak.range_m =
                        static_cast<float>(range) * range_spacing;
                    result.strongest_peak.doppler_hz =
                        static_cast<float>(centered_doppler) * doppler_spacing;
                    result.strongest_peak.velocity_mps =
                        static_cast<float>(centered_doppler) * velocity_spacing;
                    result.strongest_peak.power = power;
                }
            }
        }
        result.ready = true;
        result.coherent_frames = config.coherent_frames;
        result.first_capture_sequence = batch_first_sequence;
        result.last_capture_sequence = last_sequence;
        result.first_timestamp = batch_first_timestamp;
        result.last_timestamp = last_timestamp;
        result.sequence_gap_resets = sequence_gap_resets;
        result.timestamp_regressions = timestamp_regressions;
        result.directly_estimated_subcarriers =
            direct_estimates / config.coherent_frames;
        result.interpolated_subcarriers =
            interpolated_estimates / config.coherent_frames;
        compute_cfar_detections();
    }

    void compute_cfar_detections() {
        result.detections.clear();
        result.cfar_cells_tested = 0u;
        if (!config.enable_cfar_detection) {
            return;
        }

        const std::size_t rows = config.doppler_fft_size;
        const std::size_t columns = config.range_fft_size;
        const std::size_t stride = columns + 1u;
        std::fill(cfar_integral.begin(), cfar_integral.end(), 0.0);
        for (std::size_t row = 0u; row < rows; ++row) {
            double row_sum = 0.0;
            for (std::size_t column = 0u; column < columns; ++column) {
                row_sum += static_cast<double>(std::norm(
                    result.range_doppler_map[row * columns + column]));
                cfar_integral[(row + 1u) * stride + column + 1u] =
                    cfar_integral[row * stride + column + 1u] + row_sum;
            }
        }

        const std::size_t outer_d =
            config.cfar_training_doppler + config.cfar_guard_doppler;
        const std::size_t outer_r =
            config.cfar_training_range + config.cfar_guard_range;
        if (rows <= 2u * outer_d || columns <= 2u * outer_r) {
            return;
        }
        const std::size_t outer_cells =
            (2u * outer_d + 1u) * (2u * outer_r + 1u);
        const std::size_t guard_cells =
            (2u * config.cfar_guard_doppler + 1u) *
            (2u * config.cfar_guard_range + 1u);
        const std::size_t training_cells = outer_cells - guard_cells;
        if (training_cells == 0u) {
            return;
        }
        const double training = static_cast<double>(training_cells);
        const double alpha = training * (
            std::pow(
                static_cast<double>(config.cfar_false_alarm_probability),
                -1.0 / training) - 1.0);
        const std::size_t first_range = std::max(
            config.minimum_range_bin, outer_r);
        const std::size_t configured_range_stop =
            config.maximum_range_bin == 0u
            ? columns / 2u : config.maximum_range_bin;
        const std::size_t range_stop = std::min(
            columns - outer_r, configured_range_stop);
        const std::size_t dc = rows / 2u;
        cfar_candidates.clear();
        for (std::size_t doppler = outer_d;
             doppler < rows - outer_d; ++doppler) {
            if (config.doppler_dc_exclusion_bins > 0u) {
                const std::size_t dc_distance = doppler > dc
                    ? doppler - dc : dc - doppler;
                if (dc_distance <= config.doppler_dc_exclusion_bins) {
                    continue;
                }
            }
            for (std::size_t range = first_range;
                 range < range_stop; ++range) {
                const double outer_sum = rectangle_sum(
                    cfar_integral, stride,
                    doppler - outer_d, doppler + outer_d + 1u,
                    range - outer_r, range + outer_r + 1u);
                const double guard_sum = rectangle_sum(
                    cfar_integral, stride,
                    doppler - config.cfar_guard_doppler,
                    doppler + config.cfar_guard_doppler + 1u,
                    range - config.cfar_guard_range,
                    range + config.cfar_guard_range + 1u);
                const double noise = std::max(
                    0.0, (outer_sum - guard_sum) / training);
                const double threshold = alpha * noise;
                const float power = std::norm(
                    result.range_doppler_map[doppler * columns + range]);
                ++result.cfar_cells_tested;
                if (static_cast<double>(power) > threshold &&
                    std::isfinite(power)) {
                    cfar_candidates.push_back(CfarCandidate{
                        doppler, range, power,
                        static_cast<float>(noise),
                        static_cast<float>(threshold)});
                }
            }
        }
        std::sort(
            cfar_candidates.begin(), cfar_candidates.end(),
            [](const CfarCandidate& left, const CfarCandidate& right) {
                return left.power > right.power;
            });
        for (const auto& candidate : cfar_candidates) {
            bool suppressed = false;
            for (const auto& accepted : result.detections) {
                const std::size_t doppler_distance =
                    candidate.doppler > accepted.peak.doppler_bin
                    ? candidate.doppler - accepted.peak.doppler_bin
                    : accepted.peak.doppler_bin - candidate.doppler;
                const std::size_t range_distance =
                    candidate.range > accepted.peak.range_bin
                    ? candidate.range - accepted.peak.range_bin
                    : accepted.peak.range_bin - candidate.range;
                if (doppler_distance <= config.cfar_suppression_doppler &&
                    range_distance <= config.cfar_suppression_range) {
                    suppressed = true;
                    break;
                }
            }
            if (suppressed) {
                continue;
            }
            DynamicSensingDetection detection;
            detection.peak.range_bin = candidate.range;
            detection.peak.doppler_bin = candidate.doppler;
            detection.peak.range_m = static_cast<float>(candidate.range) *
                result.range_bin_spacing_m;
            const int centered_doppler =
                static_cast<int>(candidate.doppler) -
                static_cast<int>(config.doppler_fft_size / 2u);
            detection.peak.doppler_hz =
                static_cast<float>(centered_doppler) *
                result.doppler_bin_spacing_hz;
            detection.peak.velocity_mps =
                static_cast<float>(centered_doppler) *
                result.velocity_bin_spacing_mps;
            detection.peak.power = candidate.power;
            detection.noise_power = candidate.noise_power;
            detection.threshold_power = candidate.threshold_power;
            detection.power_over_threshold_db = 10.0f * std::log10(
                std::max(candidate.power, std::numeric_limits<float>::min()) /
                std::max(
                    candidate.threshold_power,
                    std::numeric_limits<float>::min()));
            result.detections.push_back(detection);
            if (result.detections.size() >= config.cfar_max_detections) {
                break;
            }
        }
    }

    DynamicSensingConfig config;
    DynamicSensingChannelEstimate channel_estimate;
    std::vector<std::complex<float>> frequency_history;
    std::vector<std::complex<float>> range_history;
    std::vector<std::complex<float>> clutter_mean;
    std::vector<std::complex<float>> range_scratch;
    std::vector<std::complex<float>> doppler_scratch;
    std::vector<double> cfar_integral;
    std::vector<CfarCandidate> cfar_candidates;
    std::vector<std::uint8_t> batch_active_mask;
    DynamicSensingResult result{};
    std::size_t frame_count = 0u;
    std::size_t direct_estimates = 0u;
    std::size_t interpolated_estimates = 0u;
    bool sequence_valid = false;
    std::uint64_t last_sequence = 0u;
    bool timestamp_valid = false;
    std::uint64_t last_timestamp = 0u;
    std::uint64_t batch_first_sequence = 0u;
    std::uint64_t batch_first_timestamp = 0u;
    std::size_t sequence_gap_resets = 0u;
    std::size_t timestamp_regressions = 0u;
};

DynamicSensingProcessor::DynamicSensingProcessor(
    const DynamicSensingConfig& config)
    : impl_(new Impl(config)) {}

DynamicSensingProcessor::~DynamicSensingProcessor() = default;

bool DynamicSensingProcessor::push_frame(
    std::uint64_t capture_sequence,
    std::uint64_t timestamp,
    const std::vector<std::complex<float>>& transmit_grid,
    const std::vector<std::complex<float>>& receive_grid) {
    bool reset_batch = false;
    if (impl_->sequence_valid && capture_sequence != impl_->last_sequence + 1u) {
        if (impl_->config.reset_on_sequence_gap) {
            ++impl_->sequence_gap_resets;
            reset_batch = true;
        }
    }
    if (impl_->timestamp_valid && timestamp < impl_->last_timestamp) {
        ++impl_->timestamp_regressions;
        reset_batch = true;
    }
    if (reset_batch) {
        impl_->clear_batch();
    }
    impl_->sequence_valid = true;
    impl_->last_sequence = capture_sequence;
    impl_->timestamp_valid = true;
    impl_->last_timestamp = timestamp;

    estimate_dynamic_sensing_channel_2x2(
        transmit_grid, receive_grid, impl_->config,
        impl_->channel_estimate);
    if (impl_->frame_count > 0u &&
        impl_->batch_active_mask !=
            impl_->channel_estimate.active_subcarrier_mask) {
        impl_->clear_batch();
    }
    if (impl_->frame_count == 0u) {
        impl_->batch_first_sequence = capture_sequence;
        impl_->batch_first_timestamp = timestamp;
        impl_->batch_active_mask =
            impl_->channel_estimate.active_subcarrier_mask;
    }
    std::copy(
        impl_->channel_estimate.frequency_response.begin(),
        impl_->channel_estimate.frequency_response.end(),
        impl_->frequency_history.begin() + static_cast<std::ptrdiff_t>(
            impl_->frame_count * impl_->config.fft_size));
    impl_->direct_estimates +=
        impl_->channel_estimate.directly_estimated_subcarriers;
    impl_->interpolated_estimates +=
        impl_->channel_estimate.interpolated_subcarriers;
    ++impl_->frame_count;
    if (impl_->frame_count < impl_->config.coherent_frames) {
        return false;
    }
    impl_->compute_result();
    impl_->clear_batch();
    return true;
}

const DynamicSensingResult&
DynamicSensingProcessor::last_result() const noexcept {
    return impl_->result;
}

std::size_t DynamicSensingProcessor::frames_accumulated() const noexcept {
    return impl_->frame_count;
}

const DynamicSensingConfig& DynamicSensingProcessor::config() const noexcept {
    return impl_->config;
}

void DynamicSensingProcessor::reset() noexcept {
    impl_->clear_batch();
    impl_->sequence_valid = false;
    impl_->timestamp_valid = false;
    impl_->last_sequence = 0u;
    impl_->last_timestamp = 0u;
    impl_->sequence_gap_resets = 0u;
    impl_->timestamp_regressions = 0u;
    impl_->result = DynamicSensingResult{};
    impl_->result.range_doppler_map.resize(
        impl_->config.doppler_fft_size * impl_->config.range_fft_size);
}

}  // namespace openisac
