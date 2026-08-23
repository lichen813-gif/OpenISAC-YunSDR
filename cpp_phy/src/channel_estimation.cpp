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

}  // namespace openisac
