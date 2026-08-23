#include "openisac/rank4_formal_link.hpp"

#include "openisac/channel_estimation.hpp"
#include "openisac/dynamic_frame.hpp"
#include "openisac/frame.hpp"
#include "openisac/ldpc.hpp"
#include "openisac/mimo_nxn.hpp"
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

constexpr std::size_t ports = 4u;
constexpr std::size_t data_symbols = 2u;

std::size_t count_bit_errors(unsigned left, unsigned right, unsigned bits) {
    unsigned difference = left ^ right;
    std::size_t result = 0u;
    for (unsigned bit = 0u; bit < bits; ++bit) {
        result += difference & 1u;
        difference >>= 1u;
    }
    return result;
}

float ratio_db(double numerator, double denominator) {
    if (!(denominator > 0.0)) {
        throw std::runtime_error("Rank-4 formal metric has zero denominator");
    }
    return 10.0f * std::log10(static_cast<float>(
        std::max(numerator / denominator, 1.0e-20)));
}

void validate(const Rank4FormalSimulationConfig& config) {
    if (config.frames == 0u || !std::isfinite(config.snr_db) ||
        !std::isfinite(config.transmit_correlation) ||
        !std::isfinite(config.receive_correlation) ||
        std::abs(config.transmit_correlation) >= 1.0f ||
        std::abs(config.receive_correlation) >= 1.0f ||
        config.maximum_ldpc_iterations == 0u || config.taps.empty()) {
        throw std::invalid_argument("invalid Rank-4 formal simulation config");
    }
    for (const auto& tap : config.taps) {
        if (tap.delay_samples >= 128u) {
            throw std::invalid_argument("Rank-4 TDL delay must fit inside CP");
        }
    }
}

std::complex<float> mrc_detect_tx0(
    const std::array<std::complex<float>, maximum_spatial_streams>& received,
    const ChannelNxN& channel,
    float noise_variance,
    float& equivalent_variance) {
    float power = 0.0f;
    std::complex<float> matched{};
    for (std::size_t rx = 0u; rx < ports; ++rx) {
        const auto h = channel.values[rx * maximum_spatial_streams];
        power += std::norm(h);
        matched += std::conj(h) * received[rx];
    }
    power = std::max(power, 1.0e-12f);
    equivalent_variance = noise_variance / power;
    return matched / power;
}

}  // namespace

Rank4FormalSimulationResult simulate_rank4_formal_link(
    const Rank4FormalSimulationConfig& config,
    const Ldpc5041008& codec) {
    validate(config);
    const LinkMode mode{4u, config.modulation};
    const unsigned bits = modulation_bits(config.modulation);
    const float noise_variance = std::pow(10.0f, -config.snr_db / 10.0f);
    std::normal_distribution<float> noise(
        0.0f, std::sqrt(noise_variance * 0.5f));
    std::mt19937 random(config.payload_seed);
    std::uniform_int_distribution<unsigned> byte_value(0u, 255u);

    TdlSpatialCorrelationNxNConfig spatial;
    spatial.streams = ports;
    spatial.transmit_correlation = config.transmit_correlation;
    spatial.receive_correlation = config.receive_correlation;
    spatial.random_seed = config.channel_seed;
    const auto impulse = build_correlated_tdl_nxn(config.taps, spatial);
    Rank4FormalSimulationResult result;
    result.frames = config.frames;
    double error_power = 0.0;
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
    std::vector<std::complex<float>> receive_grid;
    std::vector<ChannelNxN> estimated_channels;
    FdmPilotChannelEstimatorWorkspaceNxN estimator_workspace;
    DynamicFrameDecodeWorkspace decode_workspace;

    for (std::size_t frame = 0u; frame < config.frames; ++frame) {
        // Determine the selected layout before filling a deterministic payload.
        FormalFrameProfile profile;
        profile.transmit_rank = ports;
        profile.bits_per_symbol = bits;
        profile.pilot_spacing = 2u;
        const auto layout = build_formal_frame_layout(profile);
        const std::size_t payload_bytes = config.payload_bytes == 0u
            ? layout.user_payload_bytes : config.payload_bytes;
        if (payload_bytes == 0u || payload_bytes > layout.user_payload_bytes) {
            throw std::invalid_argument("Rank-4 payload exceeds frame capacity");
        }
        std::vector<std::uint8_t> payload(payload_bytes);
        for (auto& value : payload) {
            value = static_cast<std::uint8_t>(byte_value(random));
        }
        const auto encoded = encode_dynamic_frame(
            payload, mode, static_cast<std::uint16_t>(frame), codec,
            config.pilot_seed);
        result.user_payload_bytes_per_frame = payload_bytes;
        result.ldpc_blocks_per_frame = encoded.header.payload_blocks;

        receive_grid.assign(
            data_symbols * encoded.profile.fft_size * ports, {});
        for (std::size_t time = 0u; time < data_symbols; ++time) {
            for (std::size_t tx = 0u; tx < ports; ++tx) {
                transmit_frequency[tx].resize(encoded.profile.fft_size);
                for (std::size_t fft = 0u; fft < encoded.profile.fft_size; ++fft) {
                    transmit_frequency[tx][fft] = encoded.tx_grid[
                        (time * encoded.profile.fft_size + fft) * ports + tx];
                }
                transmit_time[tx] = ofdm_modulate(
                    transmit_frequency[tx], encoded.profile.cp_length);
            }
            apply_tdl_nxn_symbol(transmit_time, impulse, receive_time);
            for (std::size_t rx = 0u; rx < ports; ++rx) {
                for (auto& sample : receive_time[rx]) {
                    sample += std::complex<float>{noise(random), noise(random)};
                }
                receive_frequency[rx] = ofdm_demodulate(
                    receive_time[rx], encoded.profile.fft_size,
                    encoded.profile.cp_length);
                for (std::size_t fft = 0u; fft < encoded.profile.fft_size; ++fft) {
                    receive_grid[
                        (time * encoded.profile.fft_size + fft) * ports + rx] =
                        receive_frequency[rx][fft];
                }
            }
        }

        estimate_fdm_pilot_channel_linear_nxn(
            receive_grid, encoded.tx_grid, encoded.layout.pilot_fft_indices,
            data_symbols, encoded.profile.fft_size, ports,
            estimated_channels, estimator_workspace);

        std::vector<float> control_llrs;
        control_llrs.reserve(encoded.control_labels.size() * 2u);
        for (const auto data : encoded.layout.control_data_positions) {
            const std::size_t fft = encoded.layout.data_fft_indices[data];
            std::array<std::complex<float>, maximum_spatial_streams> received{};
            for (std::size_t rx = 0u; rx < ports; ++rx) {
                received[rx] = receive_grid[fft * ports + rx];
            }
            float variance = 0.0f;
            const auto symbol = mrc_detect_tx0(
                received, estimated_channels[fft], noise_variance, variance);
            const auto llrs = SquareQAM::max_log_llrs(symbol, variance, 2u);
            control_llrs.push_back(llrs[0]);
            control_llrs.push_back(llrs[1]);
        }

        try {
            const auto header = decode_control_qpsk_llrs(control_llrs);
            if (header.flags == encoded.header.flags &&
                header.payload_len == encoded.header.payload_len &&
                header.payload_blocks == encoded.header.payload_blocks &&
                header.sequence == encoded.header.sequence) {
                ++result.header_passes;
            }
        } catch (const std::exception&) {
            // The production decoder below will report this frame as failed.
        }

        std::vector<std::complex<float>> equalized(
            encoded.layout.payload_layer_symbols);
        std::vector<float> variances(encoded.layout.payload_layer_symbols);
        for (std::size_t payload_index = 0u;
             payload_index < encoded.layout.payload_time_indices.size();
             ++payload_index) {
            const std::size_t time =
                encoded.layout.payload_time_indices[payload_index];
            const std::size_t data =
                encoded.layout.payload_data_positions[payload_index];
            const std::size_t fft = encoded.layout.data_fft_indices[data];
            std::array<std::complex<float>, maximum_spatial_streams> received{};
            for (std::size_t rx = 0u; rx < ports; ++rx) {
                received[rx] = receive_grid[
                    (time * encoded.profile.fft_size + fft) * ports + rx];
            }
            const auto detected = detect_nxn(
                received,
                estimated_channels[time * encoded.profile.fft_size + fft],
                noise_variance, LinearDetector::mmse);
            for (std::size_t layer = 0u; layer < ports; ++layer) {
                const std::size_t index = payload_index * ports + layer;
                equalized[index] = detected.symbols[layer];
                variances[index] = std::max(
                    detected.predicted_mse[layer], 1.0e-12f);
                error_power += std::norm(
                    equalized[index] - encoded.payload_symbols[index]);
                reference_power += std::norm(encoded.payload_symbols[index]);
                result.transmitted_symbols.push_back(
                    encoded.payload_symbols[index]);
                result.equalized_symbols.push_back(equalized[index]);
            }
        }

        const std::size_t coded_symbols = encoded.transmitted_bits.size() / bits;
        for (std::size_t symbol = 0u; symbol < coded_symbols; ++symbol) {
            const unsigned recovered = SquareQAM::demodulate(
                equalized[symbol], bits);
            result.pre_fec_bit_errors += count_bit_errors(
                encoded.payload_labels[symbol], recovered, bits);
        }
        result.pre_fec_compared_bits += encoded.transmitted_bits.size();

        for (std::size_t time = 0u; time < data_symbols; ++time) {
            for (const auto fft : encoded.layout.data_fft_indices) {
                const auto truth = tdl_frequency_response_nxn(
                    impulse, fft, encoded.profile.fft_size);
                const auto& estimate = estimated_channels[
                    time * encoded.profile.fft_size + fft];
                for (std::size_t rx = 0u; rx < ports; ++rx) {
                    for (std::size_t tx = 0u; tx < ports; ++tx) {
                        const std::size_t component =
                            rx * maximum_spatial_streams + tx;
                        channel_error_power += std::norm(
                            estimate.values[component] - truth.values[component]);
                        channel_reference_power += std::norm(
                            truth.values[component]);
                    }
                }
            }
        }

        try {
            const auto decoded = decode_dynamic_frame_llrs(
                control_llrs, equalized, variances, codec,
                config.maximum_ldpc_iterations, 0.8f, nullptr,
                &decode_workspace);
            result.syndrome_failures += decoded.syndrome_failures;
            result.crc_passes += decoded.crc_ok;
            result.payload_matches += decoded.user_payload == payload;
        } catch (const std::exception&) {
            ++result.syndrome_failures;
        }
    }

    result.pre_fec_ber = result.pre_fec_compared_bits == 0u ? 0.0f :
        static_cast<float>(result.pre_fec_bit_errors) /
        static_cast<float>(result.pre_fec_compared_bits);
    result.evm_percent = 100.0f * std::sqrt(
        static_cast<float>(error_power / reference_power));
    result.channel_nmse_db = ratio_db(
        channel_error_power, channel_reference_power);
    return result;
}

}  // namespace openisac
