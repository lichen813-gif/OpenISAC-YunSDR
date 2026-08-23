#pragma once

#include "openisac/mimo2x2.hpp"

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace openisac {

struct FdmPilotChannelEstimatorWorkspace {
    using Point = std::pair<int, std::complex<float>>;
    std::array<std::array<std::vector<Point>, 2>, 2> estimates;
    std::size_t capacity_growths = 0u;
};

// Inputs use [time][fft][antenna] interleaving. The result uses [time][fft].
std::vector<Channel2x2> estimate_fdm_pilot_channel_linear_2x2(
    const std::vector<std::complex<float>>& receive_grid,
    const std::vector<std::complex<float>>& transmit_grid,
    const std::vector<std::uint16_t>& pilot_fft_indices,
    std::size_t time_symbols,
    std::size_t fft_size);

void estimate_fdm_pilot_channel_linear_2x2(
    const std::vector<std::complex<float>>& receive_grid,
    const std::vector<std::complex<float>>& transmit_grid,
    const std::vector<std::uint16_t>& pilot_fft_indices,
    std::size_t time_symbols,
    std::size_t fft_size,
    std::vector<Channel2x2>& output);

void estimate_fdm_pilot_channel_linear_2x2(
    const std::vector<std::complex<float>>& receive_grid,
    const std::vector<std::complex<float>>& transmit_grid,
    const std::vector<std::uint16_t>& pilot_fft_indices,
    std::size_t time_symbols,
    std::size_t fft_size,
    std::vector<Channel2x2>& output,
    FdmPilotChannelEstimatorWorkspace& workspace);

}  // namespace openisac
