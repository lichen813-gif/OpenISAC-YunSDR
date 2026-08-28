#include "openisac/channel_estimation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace openisac {
namespace {

using Point = FdmPilotChannelEstimatorWorkspace::Point;

int centered_index(std::size_t native, std::size_t fft_size) {
    return native >= fft_size / 2u
               ? static_cast<int>(native) - static_cast<int>(fft_size)
               : static_cast<int>(native);
}

float dmrs_sign(
    std::uint32_t seed,
    int centered_subcarrier,
    std::size_t group) noexcept {
    std::uint64_t mixed = static_cast<std::uint64_t>(seed) ^
        (static_cast<std::uint64_t>(
             static_cast<std::int64_t>(centered_subcarrier)) *
         0x85EBCA77ull) ^
        (static_cast<std::uint64_t>(group) * 0xC2B2AE3Dull);
    mixed ^= mixed >> 16u;
    mixed *= 0x7FEB352Dull;
    mixed ^= mixed >> 15u;
    return (mixed & 1u) == 0u ? 1.0f : -1.0f;
}

void validate_active_grid(
    const std::vector<std::uint16_t>& active,
    std::size_t fft_size,
    bool require_pairs) {
    if (active.empty()) {
        throw std::invalid_argument("NR DM-RS requires occupied subcarriers");
    }
    int previous = -static_cast<int>(fft_size);
    std::size_t run_length = 0u;
    for (std::size_t index = 0u; index < active.size(); ++index) {
        if (active[index] >= fft_size) {
            throw std::invalid_argument("NR DM-RS subcarrier is outside FFT");
        }
        const int centered = centered_index(active[index], fft_size);
        if (index != 0u && centered <= previous) {
            throw std::invalid_argument(
                "NR DM-RS occupied subcarriers must be centered-frequency ordered");
        }
        if (index == 0u || centered != previous + 1) {
            if (require_pairs && run_length != 0u && (run_length & 1u) != 0u) {
                throw std::invalid_argument(
                    "NR DM-RS four-port contiguous runs must contain pairs");
            }
            run_length = 1u;
        } else {
            ++run_length;
        }
        previous = centered;
    }
    if (require_pairs && (run_length & 1u) != 0u) {
        throw std::invalid_argument(
            "NR DM-RS four-port contiguous runs must contain pairs");
    }
}

void periodic_linear_grid(
    const std::vector<Point>& sorted,
    int period,
    std::size_t time,
    std::vector<Channel2x2>& output,
    std::complex<float> Channel2x2::* component) {
    if (sorted.empty()) {
        throw std::invalid_argument("channel interpolation requires pilot estimates");
    }
    if (sorted.size() == 1u) {
        for (int target = -period / 2; target < period / 2; ++target) {
            const std::size_t native = target < 0
                ? static_cast<std::size_t>(target + period)
                : static_cast<std::size_t>(target);
            output[time * static_cast<std::size_t>(period) + native].*component =
                sorted.front().second;
        }
        return;
    }
    std::size_t upper = 0u;
    for (int target = -period / 2; target < period / 2; ++target) {
        while (upper < sorted.size() && sorted[upper].first <= target) {
            ++upper;
        }
        Point left;
        Point right;
        if (upper == 0u) {
            left = {sorted.back().first - period, sorted.back().second};
            right = sorted.front();
        } else if (upper == sorted.size()) {
            left = sorted.back();
            right = {sorted.front().first + period, sorted.front().second};
        } else {
            left = sorted[upper - 1u];
            right = sorted[upper];
        }
        const float fraction = static_cast<float>(target - left.first) /
                               static_cast<float>(right.first - left.first);
        const std::size_t native = target < 0
            ? static_cast<std::size_t>(target + period)
            : static_cast<std::size_t>(target);
        output[time * static_cast<std::size_t>(period) + native].*component =
            left.second + fraction * (right.second - left.second);
    }
}

void periodic_linear_grid_nxn(
    const std::vector<FdmPilotChannelEstimatorWorkspaceNxN::Point>& sorted,
    int period,
    std::size_t time,
    std::size_t receive_port,
    std::size_t transmit_port,
    std::vector<ChannelNxN>& output) {
    if (sorted.empty()) {
        throw std::invalid_argument("channel interpolation requires pilot estimates");
    }
    const std::size_t component =
        receive_port * maximum_spatial_streams + transmit_port;
    if (sorted.size() == 1u) {
        for (int target = -period / 2; target < period / 2; ++target) {
            const std::size_t native = target < 0
                ? static_cast<std::size_t>(target + period)
                : static_cast<std::size_t>(target);
            output[time * static_cast<std::size_t>(period) + native]
                .values[component] = sorted.front().second;
        }
        return;
    }
    std::size_t upper = 0u;
    for (int target = -period / 2; target < period / 2; ++target) {
        while (upper < sorted.size() && sorted[upper].first <= target) {
            ++upper;
        }
        FdmPilotChannelEstimatorWorkspaceNxN::Point left;
        FdmPilotChannelEstimatorWorkspaceNxN::Point right;
        if (upper == 0u) {
            left = {sorted.back().first - period, sorted.back().second};
            right = sorted.front();
        } else if (upper == sorted.size()) {
            left = sorted.back();
            right = {sorted.front().first + period, sorted.front().second};
        } else {
            left = sorted[upper - 1u];
            right = sorted[upper];
        }
        const float fraction = static_cast<float>(target - left.first) /
                               static_cast<float>(right.first - left.first);
        const std::size_t native = target < 0
            ? static_cast<std::size_t>(target + period)
            : static_cast<std::size_t>(target);
        output[time * static_cast<std::size_t>(period) + native]
            .values[component] =
                left.second + fraction * (right.second - left.second);
    }
}

void periodic_linear_grid_nxn_all(
    const std::array<
        std::array<
            std::vector<FdmPilotChannelEstimatorWorkspaceNxN::Point>,
            maximum_spatial_streams>,
        maximum_spatial_streams>& estimates,
    int period,
    std::size_t time,
    std::size_t ports,
    std::vector<ChannelNxN>& output) {
    std::array<std::size_t, maximum_spatial_streams> upper{};
    for (int target = -period / 2; target < period / 2; ++target) {
        const std::size_t native = target < 0
            ? static_cast<std::size_t>(target + period)
            : static_cast<std::size_t>(target);
        auto& channel = output[
            time * static_cast<std::size_t>(period) + native];
        for (std::size_t tx = 0u; tx < ports; ++tx) {
            const auto& reference = estimates[0u][tx];
            if (reference.empty()) {
                throw std::invalid_argument(
                    "channel interpolation requires pilot estimates");
            }
            if (reference.size() == 1u) {
                for (std::size_t rx = 0u; rx < ports; ++rx) {
                    channel.values[rx * maximum_spatial_streams + tx] =
                        estimates[rx][tx].front().second;
                }
                continue;
            }
            while (upper[tx] < reference.size() &&
                   reference[upper[tx]].first <= target) {
                ++upper[tx];
            }
            std::size_t left_index = 0u;
            std::size_t right_index = 0u;
            int left_frequency = 0;
            int right_frequency = 0;
            if (upper[tx] == 0u) {
                left_index = reference.size() - 1u;
                right_index = 0u;
                left_frequency = reference[left_index].first - period;
                right_frequency = reference[right_index].first;
            } else if (upper[tx] == reference.size()) {
                left_index = reference.size() - 1u;
                right_index = 0u;
                left_frequency = reference[left_index].first;
                right_frequency = reference[right_index].first + period;
            } else {
                left_index = upper[tx] - 1u;
                right_index = upper[tx];
                left_frequency = reference[left_index].first;
                right_frequency = reference[right_index].first;
            }
            const float fraction =
                static_cast<float>(target - left_frequency) /
                static_cast<float>(right_frequency - left_frequency);
            for (std::size_t rx = 0u; rx < ports; ++rx) {
                const auto left = estimates[rx][tx][left_index].second;
                const auto right = estimates[rx][tx][right_index].second;
                channel.values[rx * maximum_spatial_streams + tx] =
                    left + fraction * (right - left);
            }
        }
    }
}

}  // namespace

std::vector<Channel2x2> estimate_fdm_pilot_channel_linear_2x2(
    const std::vector<std::complex<float>>& receive_grid,
    const std::vector<std::complex<float>>& transmit_grid,
    const std::vector<std::uint16_t>& pilot_fft_indices,
    std::size_t time_symbols,
    std::size_t fft_size) {
    std::vector<Channel2x2> output;
    estimate_fdm_pilot_channel_linear_2x2(
        receive_grid, transmit_grid, pilot_fft_indices,
        time_symbols, fft_size, output);
    return output;
}

void estimate_fdm_pilot_channel_linear_2x2(
    const std::vector<std::complex<float>>& receive_grid,
    const std::vector<std::complex<float>>& transmit_grid,
    const std::vector<std::uint16_t>& pilot_fft_indices,
    std::size_t time_symbols,
    std::size_t fft_size,
    std::vector<Channel2x2>& output) {
    FdmPilotChannelEstimatorWorkspace workspace;
    estimate_fdm_pilot_channel_linear_2x2(
        receive_grid, transmit_grid, pilot_fft_indices,
        time_symbols, fft_size, output, workspace);
}

void estimate_fdm_pilot_channel_linear_2x2(
    const std::vector<std::complex<float>>& receive_grid,
    const std::vector<std::complex<float>>& transmit_grid,
    const std::vector<std::uint16_t>& pilot_fft_indices,
    std::size_t time_symbols,
    std::size_t fft_size,
    std::vector<Channel2x2>& output,
    FdmPilotChannelEstimatorWorkspace& workspace) {
    const std::size_t expected = time_symbols * fft_size * 2u;
    if (time_symbols == 0u || fft_size == 0u || (fft_size & 1u) != 0u ||
        receive_grid.size() != expected || transmit_grid.size() != expected ||
        pilot_fft_indices.empty()) {
        throw std::invalid_argument("invalid FDM pilot channel-estimation dimensions");
    }
    output.resize(time_symbols * fft_size);
    for (std::size_t time = 0u; time < time_symbols; ++time) {
        auto& estimates = workspace.estimates;
        const std::size_t points_per_port = pilot_fft_indices.size() / 2u + 1u;
        for (auto& receive : estimates) {
            for (auto& transmit : receive) {
                transmit.clear();
                if (transmit.capacity() < points_per_port) {
                    ++workspace.capacity_growths;
                    transmit.reserve(points_per_port);
                }
            }
        }
        for (const std::size_t pilot : pilot_fft_indices) {
            if (pilot >= fft_size) {
                throw std::invalid_argument("pilot index is outside FFT grid");
            }
            int active_tx = -1;
            std::complex<float> known{};
            for (std::size_t tx = 0u; tx < 2u; ++tx) {
                const auto value = transmit_grid[(time * fft_size + pilot) * 2u + tx];
                if (std::abs(value) > 1.0e-7f) {
                    if (active_tx >= 0) {
                        throw std::invalid_argument("FDM pilot tone has multiple active Tx ports");
                    }
                    active_tx = static_cast<int>(tx);
                    known = value;
                }
            }
            if (active_tx < 0) {
                throw std::invalid_argument("FDM pilot tone has no active Tx port");
            }
            const int centered = centered_index(pilot, fft_size);
            for (std::size_t rx = 0u; rx < 2u; ++rx) {
                const auto received = receive_grid[(time * fft_size + pilot) * 2u + rx];
                estimates[rx][static_cast<std::size_t>(active_tx)].push_back(
                    {centered, received / known});
            }
        }
        for (auto& receive : estimates) {
            for (auto& transmit : receive) {
                const auto ordered = [](const Point& left, const Point& right) {
                    return left.first < right.first;
                };
                if (!std::is_sorted(transmit.begin(), transmit.end(), ordered)) {
                    std::sort(transmit.begin(), transmit.end(), ordered);
                }
                if (transmit.empty()) {
                    throw std::invalid_argument("FDM pilot allocation misses one Tx port");
                }
            }
        }
        const int period = static_cast<int>(fft_size);
        periodic_linear_grid(
            estimates[0][0], period, time, output, &Channel2x2::h00);
        periodic_linear_grid(
            estimates[0][1], period, time, output, &Channel2x2::h01);
        periodic_linear_grid(
            estimates[1][0], period, time, output, &Channel2x2::h10);
        periodic_linear_grid(
            estimates[1][1], period, time, output, &Channel2x2::h11);
    }
}

std::vector<ChannelNxN> estimate_fdm_pilot_channel_linear_nxn(
    const std::vector<std::complex<float>>& receive_grid,
    const std::vector<std::complex<float>>& transmit_grid,
    const std::vector<std::uint16_t>& pilot_fft_indices,
    std::size_t time_symbols,
    std::size_t fft_size,
    std::size_t ports) {
    std::vector<ChannelNxN> output;
    FdmPilotChannelEstimatorWorkspaceNxN workspace;
    estimate_fdm_pilot_channel_linear_nxn(
        receive_grid, transmit_grid, pilot_fft_indices, time_symbols,
        fft_size, ports, output, workspace);
    return output;
}

void estimate_fdm_pilot_channel_linear_nxn(
    const std::vector<std::complex<float>>& receive_grid,
    const std::vector<std::complex<float>>& transmit_grid,
    const std::vector<std::uint16_t>& pilot_fft_indices,
    std::size_t time_symbols,
    std::size_t fft_size,
    std::size_t ports,
    std::vector<ChannelNxN>& output,
    FdmPilotChannelEstimatorWorkspaceNxN& workspace) {
    if (time_symbols == 0u || fft_size == 0u || (fft_size & 1u) != 0u ||
        ports == 0u || ports > maximum_spatial_streams ||
        pilot_fft_indices.empty() ||
        receive_grid.size() != time_symbols * fft_size * ports ||
        transmit_grid.size() != time_symbols * fft_size * ports) {
        throw std::invalid_argument(
            "invalid generic FDM pilot channel-estimation dimensions");
    }
    // Reuse the large generic NxN channel buffer across continuous frames.
    // ChannelNxN reserves 8x8 storage, while this estimator overwrites every
    // active [rx][tx] component below. Re-value-initializing the entire vector
    // would clear roughly 1 MiB per 4-port frame, including 48 inactive matrix
    // entries per RE, and is a measurable receive-front-end cost.
    output.resize(time_symbols * fft_size);
    for (auto& channel : output) {
        channel.streams = ports;
    }
    for (std::size_t time = 0u; time < time_symbols; ++time) {
        auto& estimates = workspace.estimates;
        const std::size_t points_per_port =
            pilot_fft_indices.size() / ports + 1u;
        for (std::size_t rx = 0u; rx < ports; ++rx) {
            for (std::size_t tx = 0u; tx < ports; ++tx) {
                auto& points = estimates[rx][tx];
                points.clear();
                if (points.capacity() < points_per_port) {
                    ++workspace.capacity_growths;
                    points.reserve(points_per_port);
                }
            }
        }
        for (const std::size_t pilot : pilot_fft_indices) {
            if (pilot >= fft_size) {
                throw std::invalid_argument("pilot index is outside FFT grid");
            }
            std::size_t active_tx = ports;
            std::complex<float> known{};
            for (std::size_t tx = 0u; tx < ports; ++tx) {
                const auto value = transmit_grid[
                    (time * fft_size + pilot) * ports + tx];
                if (std::abs(value) > 1.0e-7f) {
                    if (active_tx != ports) {
                        throw std::invalid_argument(
                            "FDM pilot tone has multiple active Tx ports");
                    }
                    active_tx = tx;
                    known = value;
                }
            }
            if (active_tx == ports) {
                throw std::invalid_argument("FDM pilot tone has no active Tx port");
            }
            const int centered = centered_index(pilot, fft_size);
            for (std::size_t rx = 0u; rx < ports; ++rx) {
                const auto received = receive_grid[
                    (time * fft_size + pilot) * ports + rx];
                estimates[rx][active_tx].push_back(
                    {centered, received / known});
            }
        }
        const auto ordered = [](
            const FdmPilotChannelEstimatorWorkspaceNxN::Point& left,
            const FdmPilotChannelEstimatorWorkspaceNxN::Point& right) {
            return left.first < right.first;
        };
        for (std::size_t rx = 0u; rx < ports; ++rx) {
            for (std::size_t tx = 0u; tx < ports; ++tx) {
                auto& points = estimates[rx][tx];
                if (!std::is_sorted(points.begin(), points.end(), ordered)) {
                    std::sort(points.begin(), points.end(), ordered);
                }
                if (points.empty()) {
                    throw std::invalid_argument(
                        "FDM pilot allocation misses one Tx port");
                }
            }
        }
        periodic_linear_grid_nxn_all(
            estimates, static_cast<int>(fft_size), time, ports, output);
    }
}

void build_nr_dmrs_reference_grid(
    std::size_t fft_size,
    const std::vector<std::uint16_t>& active_fft_indices,
    std::size_t ports,
    std::uint32_t seed,
    std::vector<std::complex<float>>& reference_grid) {
    if (fft_size == 0u || (ports != 1u && ports != 2u && ports != 4u)) {
        throw std::invalid_argument("NR DM-RS supports 1, 2 or 4 ports");
    }
    validate_active_grid(active_fft_indices, fft_size, ports == 4u);
    reference_grid.assign(2u * fft_size * ports, {});
    if (ports <= 2u) {
        const float scale = ports == 1u
            ? 1.0f : 0.7071067811865475f;
        for (std::size_t index = 0u; index < active_fft_indices.size(); ++index) {
            const auto fft = active_fft_indices[index];
            const float base = dmrs_sign(
                seed, centered_index(fft, fft_size), index);
            for (std::size_t time = 0u; time < 2u; ++time) {
                for (std::size_t tx = 0u; tx < ports; ++tx) {
                    const float code = tx == 0u || time == 0u ? 1.0f : -1.0f;
                    reference_grid[(time * fft_size + fft) * ports + tx] =
                        {base * scale * code, 0.0f};
                }
            }
        }
        return;
    }

    // Four ports use two interleaved frequency combs.  Each comb carries two
    // ports separated by a two-symbol OCC.  Unlike a four-way frequency/time
    // Walsh block, this does not assume that adjacent subcarriers see exactly
    // the same channel, which is important for frequency-selective TDL paths.
    constexpr float scale = 0.7071067811865475f;
    for (std::size_t index = 0u; index < active_fft_indices.size(); ++index) {
        const auto fft = active_fft_indices[index];
        const std::size_t first_tx = (index & 1u) * 2u;
        const float base = dmrs_sign(
            seed, centered_index(fft, fft_size), index);
        for (std::size_t time = 0u; time < 2u; ++time) {
            reference_grid[(time * fft_size + fft) * ports + first_tx] =
                {base * scale, 0.0f};
            reference_grid[
                (time * fft_size + fft) * ports + first_tx + 1u] =
                {base * scale * (time == 0u ? 1.0f : -1.0f), 0.0f};
        }
    }
}

void estimate_nr_dmrs_channel_linear_nxn(
    const std::vector<std::complex<float>>& receive_grid,
    const std::vector<std::complex<float>>& reference_grid,
    const std::vector<std::uint16_t>& active_fft_indices,
    std::size_t data_time_symbols,
    std::size_t fft_size,
    std::size_t ports,
    std::vector<ChannelNxN>& output,
    FdmPilotChannelEstimatorWorkspaceNxN& workspace) {
    if (data_time_symbols == 0u || fft_size == 0u ||
        (ports != 1u && ports != 2u && ports != 4u) ||
        receive_grid.size() != 2u * fft_size * ports ||
        reference_grid.size() != receive_grid.size()) {
        throw std::invalid_argument("invalid NR DM-RS estimator dimensions");
    }
    validate_active_grid(active_fft_indices, fft_size, ports == 4u);
    auto& estimates = workspace.estimates;
    const std::size_t points_per_port = active_fft_indices.size() + 1u;
    for (std::size_t rx = 0u; rx < ports; ++rx) {
        for (std::size_t tx = 0u; tx < ports; ++tx) {
            auto& points = estimates[rx][tx];
            points.clear();
            if (points.capacity() < points_per_port) {
                points.reserve(points_per_port);
                ++workspace.capacity_growths;
            }
        }
    }

    const std::size_t group_width = 1u;
    for (std::size_t index = 0u; index < active_fft_indices.size();
         index += group_width) {
        for (std::size_t rx = 0u; rx < ports; ++rx) {
            for (std::size_t tx = 0u; tx < ports; ++tx) {
                std::complex<float> matched{};
                float reference_power = 0.0f;
                for (std::size_t time = 0u; time < 2u; ++time) {
                    for (std::size_t frequency = 0u;
                         frequency < group_width; ++frequency) {
                        const auto fft = active_fft_indices[index + frequency];
                        const auto known = reference_grid[
                            (time * fft_size + fft) * ports + tx];
                        const auto received = receive_grid[
                            (time * fft_size + fft) * ports + rx];
                        matched += std::conj(known) * received;
                        reference_power += std::norm(known);
                    }
                }
                if (!(reference_power > 0.0f)) {
                    // In four-port mode this tone belongs to the other
                    // frequency comb.  The missing point is interpolated from
                    // this port's neighboring comb tones below.
                    continue;
                }
                const auto estimate = matched / reference_power;
                for (std::size_t frequency = 0u;
                     frequency < group_width; ++frequency) {
                    const auto fft = active_fft_indices[index + frequency];
                    estimates[rx][tx].push_back(
                        {centered_index(fft, fft_size), estimate});
                }
            }
        }
    }

    // The interpolation below overwrites every active channel component.
    // Preserve capacity and inactive storage instead of clearing the generic
    // 8x8 matrix payload on every continuous 1/2/4-port frame.
    output.resize(data_time_symbols * fft_size);
    for (auto& channel : output) {
        channel.streams = ports;
    }
    for (std::size_t rx = 0u; rx < ports; ++rx) {
        for (std::size_t tx = 0u; tx < ports; ++tx) {
            if (estimates[rx][tx].empty()) {
                throw std::runtime_error("NR DM-RS allocation misses one port");
            }
            for (std::size_t time = 0u; time < data_time_symbols; ++time) {
                periodic_linear_grid_nxn(
                    estimates[rx][tx], static_cast<int>(fft_size), time,
                    rx, tx, output);
            }
        }
    }
}

}  // namespace openisac
