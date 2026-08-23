#pragma once

#include "openisac/mimo2x2.hpp"
#include "openisac/mimo_nxn.hpp"

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

struct FdmPilotChannelEstimatorWorkspaceNxN {
    using Point = std::pair<int, std::complex<float>>;
    std::array<
        std::array<std::vector<Point>, maximum_spatial_streams>,
        maximum_spatial_streams> estimates;
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

// Generic FDM-pilot estimator for square 1x1 through 8x8 links. Inputs use
// [time][fft][port] interleaving. Exactly one Tx port must be active on each
// pilot RE; pilot tones should be distributed across every active Tx port.
std::vector<ChannelNxN> estimate_fdm_pilot_channel_linear_nxn(
    const std::vector<std::complex<float>>& receive_grid,
    const std::vector<std::complex<float>>& transmit_grid,
    const std::vector<std::uint16_t>& pilot_fft_indices,
    std::size_t time_symbols,
    std::size_t fft_size,
    std::size_t ports);

void estimate_fdm_pilot_channel_linear_nxn(
    const std::vector<std::complex<float>>& receive_grid,
    const std::vector<std::complex<float>>& transmit_grid,
    const std::vector<std::uint16_t>& pilot_fft_indices,
    std::size_t time_symbols,
    std::size_t fft_size,
    std::size_t ports,
    std::vector<ChannelNxN>& output,
    FdmPilotChannelEstimatorWorkspaceNxN& workspace);

// Build two front-loaded NR-style DM-RS symbols. One/two ports use time-domain
// OCC on every occupied subcarrier. Four ports use two interleaved frequency
// combs, with two ports separated by time-domain OCC on each comb. Layout is
// [dmrs_symbol][fft][port].
void build_nr_dmrs_reference_grid(
    std::size_t fft_size,
    const std::vector<std::uint16_t>& active_fft_indices,
    std::size_t ports,
    std::uint32_t seed,
    std::vector<std::complex<float>>& reference_grid);

// Estimate a square 1x1, 2x2 or 4x4 channel from the two DM-RS symbols and
// replicate the estimate for each following data symbol. Inputs use
// [two DM-RS symbols][fft][port]; output uses [data symbol][fft].
void estimate_nr_dmrs_channel_linear_nxn(
    const std::vector<std::complex<float>>& receive_grid,
    const std::vector<std::complex<float>>& reference_grid,
    const std::vector<std::uint16_t>& active_fft_indices,
    std::size_t data_time_symbols,
    std::size_t fft_size,
    std::size_t ports,
    std::vector<ChannelNxN>& output,
    FdmPilotChannelEstimatorWorkspaceNxN& workspace);

}  // namespace openisac
