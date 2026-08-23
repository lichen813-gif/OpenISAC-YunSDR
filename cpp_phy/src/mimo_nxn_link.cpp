#include "openisac/mimo_nxn_link.hpp"

#include "openisac/channel_estimation.hpp"
#include "openisac/ofdm.hpp"
#include "openisac/qam.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

namespace openisac {
namespace {

std::uint16_t native_index(int centered, std::size_t fft_size) {
    const int period = static_cast<int>(fft_size);
    return static_cast<std::uint16_t>((centered % period + period) % period);
}

std::size_t bit_errors(unsigned left, unsigned right, unsigned bits) noexcept {
    unsigned difference = left ^ right;
    std::size_t count = 0u;
    for (unsigned bit = 0u; bit < bits; ++bit) {
        count += static_cast<std::size_t>(difference & 1u);
        difference >>= 1u;
    }
    return count;
}

float ratio_db(double numerator, double denominator) {
    if (!(denominator > 0.0)) {
        throw std::runtime_error("MIMO diagnostic metric has zero denominator");
    }
    return 10.0f * std::log10(static_cast<float>(
        std::max(numerator / denominator, 1.0e-20)));
}

void validate(const NxNOfdmSimulationConfig& config) {
    if (config.streams == 0u || config.streams > maximum_spatial_streams ||
        config.fft_size == 0u ||
        (config.fft_size & (config.fft_size - 1u)) != 0u ||
        config.fft_size > 65535u || config.cp_length >= config.fft_size ||
        config.guard_left + config.guard_right + 1u >= config.fft_size ||
        config.pilot_spacing == 0u || config.frames == 0u ||
        !SquareQAM::supported(config.bits_per_symbol) ||
        !std::isfinite(config.snr_db) ||
        !std::isfinite(config.transmit_correlation) ||
        !std::isfinite(config.receive_correlation) ||
        std::abs(config.transmit_correlation) >= 1.0f ||
        std::abs(config.receive_correlation) >= 1.0f || config.taps.empty()) {
        throw std::invalid_argument("invalid generic MIMO OFDM simulation config");
    }
    std::size_t maximum_delay = 0u;
    for (const auto& tap : config.taps) {
        maximum_delay = std::max(maximum_delay, tap.delay_samples);
    }
    if (maximum_delay >= config.cp_length) {
        throw std::invalid_argument("generic MIMO TDL delay must fit inside CP");
    }
}

}  // namespace

NxNOfdmSimulationResult simulate_nxn_ofdm_link(
    const NxNOfdmSimulationConfig& config) {
    validate(config);
    const std::size_t streams = config.streams;
    const int first = -static_cast<int>(config.fft_size / 2u) +
                      static_cast<int>(config.guard_left);
    const int last = static_cast<int>(config.fft_size / 2u) - 1 -
                     static_cast<int>(config.guard_right);
    std::vector<std::uint16_t> pilot_indices;
    std::vector<std::uint16_t> data_indices;
    for (int centered = first; centered <= last; ++centered) {
        if (centered == 0) {
            continue;
        }
        const int spacing = static_cast<int>(config.pilot_spacing);
        if ((centered % spacing + spacing) % spacing == 0) {
            pilot_indices.push_back(native_index(centered, config.fft_size));
        } else {
            data_indices.push_back(native_index(centered, config.fft_size));
        }
    }
    if (pilot_indices.size() < streams || data_indices.empty()) {
        throw std::invalid_argument("generic MIMO grid has insufficient pilots or data");
    }

    TdlSpatialCorrelationNxNConfig spatial;
    spatial.streams = streams;
    spatial.transmit_correlation = config.transmit_correlation;
    spatial.receive_correlation = config.receive_correlation;
    spatial.random_seed = config.channel_seed;
    const auto impulse = build_correlated_tdl_nxn(config.taps, spatial);
    const float noise_variance = std::pow(10.0f, -config.snr_db / 10.0f);
    const float symbol_scale = 1.0f / std::sqrt(static_cast<float>(streams));
    std::mt19937 random(config.data_seed);
    std::uniform_int_distribution<unsigned> labels(
        0u, (1u << config.bits_per_symbol) - 1u);
    std::normal_distribution<float> noise(
        0.0f, std::sqrt(noise_variance * 0.5f));

    NxNOfdmSimulationResult result;
    result.streams = streams;
    result.pilot_subcarriers = pilot_indices.size();
    result.data_subcarriers = data_indices.size();
    result.transmitted_symbols.reserve(
        config.frames * data_indices.size() * streams);
    result.equalized_symbols.reserve(
        config.frames * data_indices.size() * streams);
    double error_power = 0.0;
    double perfect_error_power = 0.0;
    double reference_power = 0.0;
    double channel_error_power = 0.0;
    double channel_reference_power = 0.0;

    std::array<std::vector<std::complex<float>>, maximum_spatial_streams>
        transmit_frequency;
    std::array<std::vector<std::complex<float>>, maximum_spatial_streams>
        transmit_time;
    std::array<std::vector<std::complex<float>>, maximum_spatial_streams>
        receive_time;
    std::array<std::vector<std::complex<float>>, maximum_spatial_streams>
        receive_frequency;
    std::vector<std::complex<float>> transmit_grid;
    std::vector<std::complex<float>> receive_grid;
    std::vector<unsigned> frame_labels(data_indices.size() * streams);
    FdmPilotChannelEstimatorWorkspaceNxN estimator_workspace;
    std::vector<ChannelNxN> estimated_channels;

    for (std::size_t frame = 0u; frame < config.frames; ++frame) {
        transmit_grid.assign(config.fft_size * streams, {});
        for (std::size_t port = 0u; port < streams; ++port) {
            transmit_frequency[port].assign(config.fft_size, {});
        }
        for (std::size_t pilot = 0u; pilot < pilot_indices.size(); ++pilot) {
            const std::size_t port = pilot % streams;
            const float sign = ((pilot / streams + frame + port) & 1u) == 0u
                ? 1.0f : -1.0f;
            const std::size_t fft = pilot_indices[pilot];
            transmit_frequency[port][fft] = {sign, 0.0f};
            transmit_grid[fft * streams + port] = {sign, 0.0f};
        }
        for (std::size_t data = 0u; data < data_indices.size(); ++data) {
            const std::size_t fft = data_indices[data];
            for (std::size_t port = 0u; port < streams; ++port) {
                const unsigned label = labels(random);
                frame_labels[data * streams + port] = label;
                const auto symbol = SquareQAM::modulate(
                    label, config.bits_per_symbol);
                transmit_frequency[port][fft] = symbol * symbol_scale;
                transmit_grid[fft * streams + port] = symbol * symbol_scale;
                result.transmitted_symbols.push_back(symbol);
            }
        }
        for (std::size_t port = 0u; port < streams; ++port) {
            transmit_time[port] = ofdm_modulate(
                transmit_frequency[port], config.cp_length);
        }
        apply_tdl_nxn_symbol(transmit_time, impulse, receive_time);
        for (std::size_t port = 0u; port < streams; ++port) {
            for (auto& sample : receive_time[port]) {
                sample += std::complex<float>{noise(random), noise(random)};
            }
            receive_frequency[port] = ofdm_demodulate(
                receive_time[port], config.fft_size, config.cp_length);
        }
        receive_grid.assign(config.fft_size * streams, {});
        for (std::size_t fft = 0u; fft < config.fft_size; ++fft) {
            for (std::size_t port = 0u; port < streams; ++port) {
                receive_grid[fft * streams + port] =
                    receive_frequency[port][fft];
            }
        }
        estimate_fdm_pilot_channel_linear_nxn(
            receive_grid, transmit_grid, pilot_indices, 1u,
            config.fft_size, streams, estimated_channels,
            estimator_workspace);

        for (std::size_t data = 0u; data < data_indices.size(); ++data) {
            const std::size_t fft = data_indices[data];
            std::array<std::complex<float>, maximum_spatial_streams> received{};
            for (std::size_t port = 0u; port < streams; ++port) {
                received[port] = receive_frequency[port][fft];
            }
            const auto truth = tdl_frequency_response_nxn(
                impulse, fft, config.fft_size);
            const auto detected = detect_nxn(
                received, estimated_channels[fft], noise_variance,
                LinearDetector::mmse);
            const auto perfect = detect_nxn(
                received, truth, noise_variance, LinearDetector::mmse);
            for (std::size_t rx = 0u; rx < streams; ++rx) {
                for (std::size_t tx = 0u; tx < streams; ++tx) {
                    const std::size_t component =
                        rx * maximum_spatial_streams + tx;
                    channel_error_power += std::norm(
                        estimated_channels[fft].values[component] -
                        truth.values[component]);
                    channel_reference_power += std::norm(
                        truth.values[component]);
                }
            }
            for (std::size_t port = 0u; port < streams; ++port) {
                const unsigned label = frame_labels[data * streams + port];
                const auto reference = SquareQAM::modulate(
                    label, config.bits_per_symbol);
                result.equalized_symbols.push_back(detected.symbols[port]);
                error_power += std::norm(detected.symbols[port] - reference);
                perfect_error_power += std::norm(
                    perfect.symbols[port] - reference);
                reference_power += std::norm(reference);
                const unsigned recovered = SquareQAM::demodulate(
                    detected.symbols[port], config.bits_per_symbol);
                result.bit_errors += bit_errors(
                    label, recovered, config.bits_per_symbol);
            }
        }
    }
    result.detected_symbols = result.equalized_symbols.size();
    result.compared_bits = result.detected_symbols * config.bits_per_symbol;
    result.ber = result.compared_bits == 0u ? 0.0f :
        static_cast<float>(result.bit_errors) /
        static_cast<float>(result.compared_bits);
    result.evm_percent = 100.0f * std::sqrt(
        static_cast<float>(error_power / reference_power));
    result.perfect_csi_evm_percent = 100.0f * std::sqrt(
        static_cast<float>(perfect_error_power / reference_power));
    result.channel_nmse_db = ratio_db(
        channel_error_power, channel_reference_power);
    return result;
}

}  // namespace openisac
