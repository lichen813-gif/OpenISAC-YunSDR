#include "openisac/dynamic_link.hpp"

#include "openisac/channel_estimation.hpp"
#include "openisac/mimo2x2.hpp"
#include "openisac/ofdm.hpp"
#include "openisac/preamble_sync.hpp"
#include "openisac/qam.hpp"
#include "openisac/sampling_offset.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <optional>
#include <random>
#include <stdexcept>

namespace openisac {
namespace {

constexpr std::size_t fft_size = 1024u;
constexpr std::size_t cp_length = 128u;
constexpr std::size_t symbol_samples = fft_size + cp_length;
constexpr std::size_t data_symbols = 2u;
constexpr std::size_t maximum_dynamic_ports = 2u;
constexpr float subcarrier_spacing_hz = 15000.0f;

using Clock = std::chrono::steady_clock;

double elapsed_us(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::micro>(end - begin).count();
}

template <typename Value>
void resize_tracked(
    std::vector<Value>& values,
    std::size_t size,
    std::size_t& capacity_growths) {
    if (values.capacity() < size) {
        ++capacity_growths;
    }
    values.resize(size);
}

template <typename Value>
void resize_branches_tracked(
    std::vector<std::vector<Value>>& values,
    std::size_t branches,
    std::size_t size,
    std::size_t& capacity_growths) {
    if (values.capacity() < branches) {
        ++capacity_growths;
    }
    values.resize(branches);
    for (auto& branch : values) {
        resize_tracked(branch, size, capacity_growths);
    }
}

void fill_deterministic_payload(
    std::vector<std::uint8_t>& payload,
    std::uint16_t sequence) {
    for (std::size_t index = 0u; index < payload.size(); ++index) {
        payload[index] = static_cast<std::uint8_t>(
            (index * 37u + static_cast<std::size_t>(sequence) * 11u + 5u) & 0xFFu);
    }
}

std::complex<float> mrc_detect_tx0(
    const std::array<std::complex<float>, 2>& received,
    const Channel2x2& channel,
    float& equivalent_variance,
    float noise_variance) {
    const float power = std::max(
        std::norm(channel.h00) + std::norm(channel.h10), 1.0e-12f);
    equivalent_variance = noise_variance / power;
    return (std::conj(channel.h00) * received[0] +
            std::conj(channel.h10) * received[1]) / power;
}

struct AlamoutiDetection2x2 {
    std::array<std::complex<float>, 2> symbols{};
    float equivalent_variance = 0.0f;
};

AlamoutiDetection2x2 detect_alamouti_2x2(
    const std::array<std::complex<float>, 2>& received_first,
    const std::array<std::complex<float>, 2>& received_second,
    const Channel2x2& channel,
    float noise_variance) {
    constexpr float transmit_scale = 0.70710678118654752440f;
    const std::array<std::complex<float>, 2> first_channel{{
        channel.h00, channel.h10}};
    const std::array<std::complex<float>, 2> second_channel{{
        channel.h01, channel.h11}};
    std::complex<float> first{};
    std::complex<float> second{};
    float gain = 0.0f;
    for (std::size_t receive = 0u; receive < 2u; ++receive) {
        first += std::conj(first_channel[receive]) * received_first[receive] +
                 second_channel[receive] * std::conj(received_second[receive]);
        second += std::conj(second_channel[receive]) * received_first[receive] -
                  first_channel[receive] * std::conj(received_second[receive]);
        gain += std::norm(first_channel[receive]) +
                std::norm(second_channel[receive]);
    }
    gain = std::max(gain, 1.0e-12f);
    const float denominator = transmit_scale * gain;
    AlamoutiDetection2x2 result;
    result.symbols = {{first / denominator, second / denominator}};
    result.equivalent_variance =
        noise_variance / (transmit_scale * transmit_scale * gain);
    return result;
}

Modulation modulation_from_bits(unsigned bits) {
    switch (bits) {
        case 2u: return Modulation::qpsk;
        case 4u: return Modulation::qam16;
        case 6u: return Modulation::qam64;
        case 8u: return Modulation::qam256;
        default: throw std::invalid_argument("unsupported modulation order");
    }
}

const FormalFrameLayout& cached_formal_frame_layout(LinkMode mode) {
    const unsigned bits_per_symbol = modulation_bits(mode.modulation);
    const unsigned physical_ports = physical_transmit_ports(mode);
    const bool spatial =
        mode.scheme == TransmissionScheme::spatial_multiplexing &&
        ((physical_ports == 1u && mode.rank == 1u) ||
         (physical_ports == 2u && (mode.rank == 1u || mode.rank == 2u)));
    const bool stbc =
        mode.scheme == TransmissionScheme::alamouti_stbc &&
        physical_transmit_ports(mode) == 2u && mode.rank == 1u;
    if ((!spatial && !stbc) ||
        (bits_per_symbol != 2u && bits_per_symbol != 4u &&
         bits_per_symbol != 6u && bits_per_symbol != 8u)) {
        throw std::invalid_argument("unsupported cached PHY mode/MCS layout");
    }
    static const std::array<FormalFrameLayout, 12> layouts = [] {
        std::array<FormalFrameLayout, 12> result;
        constexpr std::array<unsigned, 4> modulation_bits{{2u, 4u, 6u, 8u}};
        for (unsigned rank = 1u; rank <= 2u; ++rank) {
            for (std::size_t modulation = 0u;
                 modulation < modulation_bits.size(); ++modulation) {
                FormalFrameProfile profile;
                profile.transmit_rank = rank;
                profile.bits_per_symbol = modulation_bits[modulation];
                result[(rank - 1u) * modulation_bits.size() + modulation] =
                    build_formal_frame_layout(profile);
            }
        }
        for (std::size_t modulation = 0u;
             modulation < modulation_bits.size(); ++modulation) {
            FormalFrameProfile profile;
            profile.transmit_rank = 1u;
            profile.bits_per_symbol = modulation_bits[modulation];
            profile.scheme = TransmissionScheme::alamouti_stbc;
            result[8u + modulation] = build_formal_frame_layout(profile);
        }
        return result;
    }();
    const std::size_t modulation = bits_per_symbol / 2u - 1u;
    if (stbc) {
        return layouts[8u + modulation];
    }
    return layouts[(mode.rank - 1u) * 4u + modulation];
}

std::size_t maximum_tap_delay(const std::vector<TdlTap>& taps) {
    std::size_t maximum = 0u;
    for (const auto& tap : taps) {
        maximum = std::max(maximum, tap.delay_samples);
    }
    return maximum;
}

void validate_receiver_config(const DynamicLinkReceiverConfig& config) {
    if (!std::isfinite(config.fixed_noise_variance) ||
        config.fixed_noise_variance < 0.0f ||
        !std::isfinite(config.minimum_noise_variance) ||
        config.minimum_noise_variance <= 0.0f ||
        !std::isfinite(config.maximum_noise_variance) ||
        config.maximum_noise_variance < config.minimum_noise_variance ||
        !std::isfinite(config.noise_smoothing_alpha) ||
        config.noise_smoothing_alpha <= 0.0f ||
        config.noise_smoothing_alpha > 1.0f ||
        config.maximum_channel_delay_samples >= cp_length ||
        !std::isfinite(config.csi_smoothing_alpha) ||
        config.csi_smoothing_alpha <= 0.0f ||
        config.csi_smoothing_alpha > 1.0f ||
        config.tracking_half_window_samples > cp_length ||
        !std::isfinite(config.tracking_min_metric) ||
        config.tracking_min_metric < 0.0f ||
        config.tracking_min_metric > 1.0f ||
        !std::isfinite(config.tracking_metric_ratio) ||
        config.tracking_metric_ratio < 0.0f ||
        config.tracking_metric_ratio > 1.0f) {
        throw std::invalid_argument("invalid dynamic-link receiver configuration");
    }
}

float estimate_pilot_residual_noise_variance(
    const std::vector<std::complex<float>>& receive_grid,
    const std::vector<std::complex<float>>& pilot_reference_grid,
    const std::vector<std::uint16_t>& pilot_fft_indices,
    std::size_t ports,
    std::vector<float>& power_samples,
    std::size_t& capacity_growths) {
    const std::size_t expected_samples = pilot_fft_indices.size() * ports;
    if (power_samples.capacity() < expected_samples) {
        power_samples.reserve(expected_samples);
        ++capacity_growths;
    }
    power_samples.clear();
    for (const auto fft : pilot_fft_indices) {
        std::size_t tx = ports;
        for (std::size_t candidate = 0u; candidate < ports; ++candidate) {
            if (std::norm(
                    pilot_reference_grid[fft * ports + candidate]) > 0.0f) {
                tx = candidate;
                break;
            }
        }
        if (tx == ports) {
            continue;
        }
        const auto first_reference = pilot_reference_grid[fft * ports + tx];
        const auto second_reference = pilot_reference_grid[
            (fft_size + fft) * ports + tx];
        for (std::size_t rx = 0u; rx < ports; ++rx) {
            const auto first_received = receive_grid[fft * ports + rx];
            const auto second_received = receive_grid[
                (fft_size + fft) * ports + rx];
            const auto predicted =
                first_received / first_reference * second_reference;
            const auto residual = second_received - predicted;
            power_samples.push_back(0.5f * std::norm(residual));
        }
    }
    if (power_samples.empty()) {
        throw std::runtime_error("pilot-residual noise estimator has no samples");
    }
    const auto middle = power_samples.begin() +
        static_cast<std::ptrdiff_t>(power_samples.size() / 2u);
    std::nth_element(power_samples.begin(), middle, power_samples.end());
    constexpr float exponential_median_ratio = 0.6931471805599453f;
    return *middle / exponential_median_ratio;
}

float estimate_channel_residual_noise_variance(
    const std::vector<std::complex<float>>& receive_grid,
    const std::vector<std::complex<float>>& pilot_reference_grid,
    const std::vector<std::uint16_t>& pilot_fft_indices,
    const std::vector<Channel2x2>& channels,
    std::vector<float>& power_samples,
    std::size_t& capacity_growths) {
    const std::size_t expected_samples =
        data_symbols * pilot_fft_indices.size() * maximum_dynamic_ports;
    if (power_samples.capacity() < expected_samples) {
        power_samples.reserve(expected_samples);
        ++capacity_growths;
    }
    power_samples.clear();
    for (std::size_t time = 0u; time < data_symbols; ++time) {
        for (const auto fft : pilot_fft_indices) {
            std::array<std::complex<float>, maximum_dynamic_ports> transmitted{};
            for (std::size_t tx = 0u; tx < maximum_dynamic_ports; ++tx) {
                transmitted[tx] = pilot_reference_grid[
                    (time * fft_size + fft) * maximum_dynamic_ports + tx];
            }
            const auto& channel = channels[time * fft_size + fft];
            const std::array<std::complex<float>, maximum_dynamic_ports> predicted{{
                channel.h00 * transmitted[0] + channel.h01 * transmitted[1],
                channel.h10 * transmitted[0] + channel.h11 * transmitted[1]}};
            for (std::size_t rx = 0u; rx < maximum_dynamic_ports; ++rx) {
                const auto received = receive_grid[
                    (time * fft_size + fft) * maximum_dynamic_ports + rx];
                power_samples.push_back(std::norm(received - predicted[rx]));
            }
        }
    }
    if (power_samples.empty()) {
        throw std::runtime_error("channel-residual noise estimator has no samples");
    }
    const auto middle = power_samples.begin() +
        static_cast<std::ptrdiff_t>(power_samples.size() / 2u);
    std::nth_element(power_samples.begin(), middle, power_samples.end());
    constexpr float exponential_median_ratio = 0.6931471805599453f;
    return *middle / exponential_median_ratio;
}

ImpulseResponse2x2 build_simulation_impulse(
    const DynamicLinkSimulationConfig& config) {
    const auto evaluated = evaluate_tdl_taps(
        config.taps, config.channel_time_seconds);
    if (!config.enable_correlated_spatial_tdl) {
        return build_deterministic_tdl_2x2(evaluated);
    }
    TdlSpatialCorrelationConfig spatial;
    spatial.transmit_correlation = config.transmit_spatial_correlation;
    spatial.receive_correlation = config.receive_spatial_correlation;
    spatial.random_seed = config.spatial_channel_seed;
    return build_correlated_tdl_2x2(evaluated, spatial);
}

}  // namespace

DynamicLinkReceiverConfig make_dynamic_link_receiver_config(
    const DynamicLinkSimulationConfig& simulation_config,
    NoiseVarianceMode noise_variance_mode) {
    DynamicLinkReceiverConfig receiver;
    receiver.compute_backend = simulation_config.compute_backend;
    receiver.pilot_mode = simulation_config.pilot_mode;
    receiver.noise_variance_mode = noise_variance_mode;
    receiver.fixed_noise_variance =
        std::pow(10.0f, -simulation_config.snr_db / 10.0f);
    receiver.maximum_timing_offset_samples =
        simulation_config.timing_offset_samples + 32u;
    receiver.maximum_channel_delay_samples =
        maximum_tap_delay(simulation_config.taps);
    receiver.csi_smoothing_alpha = simulation_config.csi_smoothing_alpha;
    receiver.enable_continuous_tracking =
        simulation_config.enable_continuous_tracking;
    receiver.tracking_half_window_samples =
        simulation_config.tracking_half_window_samples;
    receiver.tracking_min_metric = simulation_config.tracking_min_metric;
    receiver.tracking_metric_ratio = simulation_config.tracking_metric_ratio;
    validate_receiver_config(receiver);
    return receiver;
}

void DynamicLinkReceiverState::reset() noexcept {
    synchronization_state = SynchronizationMode::reacquire;
    timing_valid = false;
    predicted_timing_offset = 0u;
    last_timing_metric = 0.0f;
    synchronization_lock_age_frames = 0u;
    ++consecutive_sync_failures;
    csi_valid = false;
    filtered_channels.clear();
    csi_age_frames = 0u;
    noise_variance_valid = false;
    filtered_noise_variance = 0.0f;
    noise_variance_age_frames = 0u;
    ++reset_count;
}

void DynamicLinkWorkspace::release() noexcept {
    *this = DynamicLinkWorkspace{};
}

void generate_dynamic_tx_iq_frame(
    const std::vector<std::uint8_t>& user_payload,
    LinkMode mode,
    std::uint16_t sequence,
    const Ldpc5041008& codec,
    PilotMode pilot_mode,
    std::uint32_t pilot_seed,
    DynamicLinkTransmitFrame& tx_frame,
    DynamicLinkWorkspace& generation_workspace) {
    auto& buffers = generation_workspace;
    const std::size_t physical_ports = physical_transmit_ports(mode);
    if (physical_ports == 0u || physical_ports > maximum_dynamic_ports ||
        (physical_ports == 1u &&
         (mode.rank != 1u ||
          mode.scheme != TransmissionScheme::spatial_multiplexing))) {
        throw std::invalid_argument(
            "dynamic TX supports 1x1 Rank-1, 2x2 Rank-1/2 or 2x2 STBC");
    }
    const auto capacity = cached_formal_frame_layout(mode).user_payload_bytes;
    if (user_payload.empty() || user_payload.size() > capacity) {
        throw std::invalid_argument(
            "user payload is empty or exceeds selected Rank/MCS capacity");
    }

    auto encoded = encode_dynamic_frame(
        user_payload, mode, sequence, codec, pilot_seed);
    const std::size_t frame_symbol_count = formal_frame_symbols(pilot_mode);
    const std::size_t data_symbol_offset = pilot_mode == PilotMode::nr_dmrs
        ? 3u : 1u;

    for (auto& branch : buffers.tx_time) {
        resize_tracked(
            branch, frame_symbol_count * symbol_samples,
            buffers.capacity_growths);
        std::fill(branch.begin(), branch.end(), std::complex<float>{});
    }
    if (buffers.preamble.size() != symbol_samples) {
        if (buffers.preamble.capacity() < symbol_samples) {
            ++buffers.capacity_growths;
        }
        buffers.preamble = generate_zc_ofdm_symbol(fft_size, cp_length, 29u);
    }
    std::copy(
        buffers.preamble.begin(), buffers.preamble.end(),
        buffers.tx_time[0].begin());
    resize_tracked(
        buffers.frequency_scratch, fft_size, buffers.capacity_growths);
    resize_tracked(buffers.fft_scratch, fft_size, buffers.capacity_growths);
    resize_tracked(
        buffers.ofdm_samples, symbol_samples, buffers.capacity_growths);

    if (pilot_mode == PilotMode::nr_dmrs) {
        build_nr_dmrs_reference_grid(
            fft_size, encoded.layout.active_fft_indices, physical_ports,
            pilot_seed, buffers.dmrs_reference_grid);
        for (std::size_t symbol = 0u; symbol < nr_dmrs_symbols; ++symbol) {
            for (std::size_t tx = 0u; tx < physical_ports; ++tx) {
                for (std::size_t fft = 0u; fft < fft_size; ++fft) {
                    buffers.frequency_scratch[fft] =
                        buffers.dmrs_reference_grid[
                            (symbol * fft_size + fft) * physical_ports + tx];
                }
                ofdm_modulate(
                    buffers.frequency_scratch, cp_length,
                    buffers.ofdm_samples, buffers.fft_scratch);
                std::copy(
                    buffers.ofdm_samples.begin(), buffers.ofdm_samples.end(),
                    buffers.tx_time[tx].begin() +
                        static_cast<std::ptrdiff_t>(
                            (symbol + 1u) * symbol_samples));
            }
        }
    }
    for (std::size_t symbol = 0u; symbol < data_symbols; ++symbol) {
        for (std::size_t tx = 0u; tx < physical_ports; ++tx) {
            for (std::size_t fft = 0u; fft < fft_size; ++fft) {
                buffers.frequency_scratch[fft] = encoded.tx_grid[
                    (symbol * fft_size + fft) * physical_ports + tx];
            }
            ofdm_modulate(
                buffers.frequency_scratch, cp_length,
                buffers.ofdm_samples, buffers.fft_scratch);
            std::copy(
                buffers.ofdm_samples.begin(), buffers.ofdm_samples.end(),
                buffers.tx_time[tx].begin() +
                    static_cast<std::ptrdiff_t>(
                        (symbol + data_symbol_offset) * symbol_samples));
        }
    }

    tx_frame.mode = mode;
    tx_frame.sequence = sequence;
    tx_frame.pilot_seed = pilot_seed;
    tx_frame.pilot_mode = pilot_mode;
    tx_frame.samples.resize(physical_ports);
    for (std::size_t tx = 0u; tx < physical_ports; ++tx) {
        tx_frame.samples[tx] = buffers.tx_time[tx];
    }
    tx_frame.transmit_reference_grid = std::move(encoded.tx_grid);
}

namespace {

void generate_dynamic_tdl_iq_frame_impl(
    const std::vector<std::uint8_t>* application_payload,
    LinkMode mode,
    std::uint16_t sequence,
    const Ldpc5041008& codec,
    const DynamicLinkSimulationConfig& config,
    DynamicLinkIqFrame& iq_frame,
    DynamicLinkWorkspace& generation_workspace) {
    const auto simulation_start = Clock::now();
    auto& buffers = generation_workspace;
    if (!std::isfinite(config.snr_db) || !std::isfinite(config.cfo_hz) ||
        !std::isfinite(config.sfo_ppm) || std::abs(config.sfo_ppm) >= 10000.0f ||
        !std::isfinite(config.channel_time_seconds) ||
        !std::isfinite(config.transmit_spatial_correlation) ||
        std::abs(config.transmit_spatial_correlation) >= 1.0f ||
        !std::isfinite(config.receive_spatial_correlation) ||
        std::abs(config.receive_spatial_correlation) >= 1.0f ||
        !std::isfinite(config.csi_smoothing_alpha) ||
        config.csi_smoothing_alpha <= 0.0f || config.csi_smoothing_alpha > 1.0f ||
        config.tracking_half_window_samples > cp_length ||
        !std::isfinite(config.tracking_min_metric) ||
        config.tracking_min_metric < 0.0f || config.tracking_min_metric > 1.0f ||
        !std::isfinite(config.tracking_metric_ratio) ||
        config.tracking_metric_ratio < 0.0f || config.tracking_metric_ratio > 1.0f) {
        throw std::invalid_argument("invalid dynamic-link SNR/CFO/SFO");
    }
    const std::size_t physical_ports = physical_transmit_ports(mode);
    if (physical_ports == 0u || physical_ports > maximum_dynamic_ports ||
        (physical_ports == 1u &&
         (mode.rank != 1u ||
          mode.scheme != TransmissionScheme::spatial_multiplexing))) {
        throw std::invalid_argument(
            "dynamic time link supports 1x1 Rank-1, 2x2 Rank-1/2 or 2x2 STBC");
    }
    const auto impulse = build_simulation_impulse(config);
    const std::size_t maximum_delay = impulse[0][0].size() - 1u;
    if (maximum_delay >= cp_length) {
        throw std::invalid_argument("TDL maximum delay must be shorter than CP");
    }
    const auto capacity = cached_formal_frame_layout(mode).user_payload_bytes;
    if (application_payload != nullptr && application_payload->size() > capacity) {
        throw std::invalid_argument(
            "user payload exceeds selected Rank/MCS capacity");
    }
    const std::size_t payload_size = application_payload == nullptr
        ? capacity
        : application_payload->size();
    resize_tracked(buffers.payload, payload_size, buffers.capacity_growths);
    if (application_payload == nullptr) {
        fill_deterministic_payload(buffers.payload, sequence);
    } else {
        std::copy(
            application_payload->begin(), application_payload->end(),
            buffers.payload.begin());
    }
    auto encoded = encode_dynamic_frame(
        buffers.payload, mode, sequence, codec, config.pilot_seed);

    const std::size_t frame_symbol_count = formal_frame_symbols(config.pilot_mode);
    const std::size_t data_symbol_offset = config.pilot_mode == PilotMode::nr_dmrs
        ? 3u : 1u;

    for (auto& branch : buffers.tx_time) {
        resize_tracked(
            branch, frame_symbol_count * symbol_samples, buffers.capacity_growths);
        std::fill(branch.begin(), branch.end(), std::complex<float>{});
    }
    if (buffers.preamble.size() != symbol_samples) {
        if (buffers.preamble.capacity() < symbol_samples) {
            ++buffers.capacity_growths;
        }
        buffers.preamble = generate_zc_ofdm_symbol(fft_size, cp_length, 29u);
    }
    std::copy(
        buffers.preamble.begin(), buffers.preamble.end(),
        buffers.tx_time[0].begin());
    resize_tracked(buffers.frequency_scratch, fft_size, buffers.capacity_growths);
    resize_tracked(buffers.fft_scratch, fft_size, buffers.capacity_growths);
    resize_tracked(buffers.ofdm_samples, symbol_samples, buffers.capacity_growths);
    if (config.pilot_mode == PilotMode::nr_dmrs) {
        build_nr_dmrs_reference_grid(
            fft_size, encoded.layout.active_fft_indices, physical_ports,
            config.pilot_seed, buffers.dmrs_reference_grid);
        for (std::size_t symbol = 0u; symbol < nr_dmrs_symbols; ++symbol) {
            for (std::size_t tx = 0u; tx < physical_ports; ++tx) {
                for (std::size_t fft = 0u; fft < fft_size; ++fft) {
                    buffers.frequency_scratch[fft] =
                        buffers.dmrs_reference_grid[
                            (symbol * fft_size + fft) * physical_ports + tx];
                }
                ofdm_modulate(
                    buffers.frequency_scratch, cp_length,
                    buffers.ofdm_samples, buffers.fft_scratch);
                std::copy(
                    buffers.ofdm_samples.begin(), buffers.ofdm_samples.end(),
                    buffers.tx_time[tx].begin() +
                        static_cast<std::ptrdiff_t>((symbol + 1u) * symbol_samples));
            }
        }
    }
    for (std::size_t symbol = 0u; symbol < data_symbols; ++symbol) {
        for (std::size_t tx = 0u; tx < physical_ports; ++tx) {
            for (std::size_t fft = 0u; fft < fft_size; ++fft) {
                buffers.frequency_scratch[fft] = encoded.tx_grid[
                    (symbol * fft_size + fft) * physical_ports + tx];
            }
            ofdm_modulate(
                buffers.frequency_scratch, cp_length,
                buffers.ofdm_samples, buffers.fft_scratch);
            std::copy(
                buffers.ofdm_samples.begin(), buffers.ofdm_samples.end(),
                buffers.tx_time[tx].begin() +
                    static_cast<std::ptrdiff_t>(
                        (symbol + data_symbol_offset) * symbol_samples));
        }
    }
    const auto transmit_done = Clock::now();

    for (auto& branch : buffers.clean_rx) {
        resize_tracked(
            branch, frame_symbol_count * symbol_samples, buffers.capacity_growths);
        std::fill(branch.begin(), branch.end(), std::complex<float>{});
    }
    for (auto& branch : buffers.transmitted_symbol) {
        resize_tracked(branch, symbol_samples, buffers.capacity_growths);
    }
    for (auto& branch : buffers.received_symbol) {
        resize_tracked(branch, symbol_samples, buffers.capacity_growths);
    }
    for (std::size_t symbol = 0u; symbol < frame_symbol_count; ++symbol) {
        for (std::size_t tx = 0u; tx < maximum_dynamic_ports; ++tx) {
            const auto begin = buffers.tx_time[tx].begin() +
                static_cast<std::ptrdiff_t>(symbol * symbol_samples);
            std::copy(
                begin, begin + static_cast<std::ptrdiff_t>(symbol_samples),
                buffers.transmitted_symbol[tx].begin());
        }
        apply_tdl_2x2_symbol(
            buffers.transmitted_symbol, impulse, buffers.received_symbol);
        for (std::size_t rx = 0u; rx < physical_ports; ++rx) {
            std::copy(
                buffers.received_symbol[rx].begin(), buffers.received_symbol[rx].end(),
                buffers.clean_rx[rx].begin() +
                    static_cast<std::ptrdiff_t>(symbol * symbol_samples));
        }
    }

    const float noise_variance = std::pow(10.0f, -config.snr_db / 10.0f);
    const float sigma = std::sqrt(noise_variance * 0.5f);
    std::mt19937 random(config.random_seed);
    std::normal_distribution<float> normal(0.0f, sigma);
    const std::size_t tail = 32u;
    const std::size_t stream_samples =
        config.timing_offset_samples + frame_symbol_count * symbol_samples + tail;
    resize_branches_tracked(
        buffers.rx_stream, physical_ports, stream_samples, buffers.capacity_growths);
    for (auto& branch : buffers.rx_stream) {
        for (auto& sample : branch) {
            sample = {normal(random), normal(random)};
        }
    }
    for (std::size_t rx = 0u; rx < physical_ports; ++rx) {
        for (std::size_t sample = 0u; sample < buffers.clean_rx[rx].size(); ++sample) {
            buffers.rx_stream[rx][config.timing_offset_samples + sample] +=
                buffers.clean_rx[rx][sample];
        }
    }
    const float normalized_cfo = config.cfo_hz / subcarrier_spacing_hz;
    apply_cfo_normalized_inplace(buffers.rx_stream, normalized_cfo, fft_size);
    resize_branches_tracked(
        buffers.resampled_stream, physical_ports, stream_samples,
        buffers.capacity_growths);
    resample_sfo_cubic(
        buffers.rx_stream, config.sfo_ppm, buffers.resampled_stream);
    const auto channel_done = Clock::now();

    iq_frame.transmitted_mode = mode;
    iq_frame.sequence = sequence;
    iq_frame.capture_sequence = sequence;
    iq_frame.timestamp = sequence;
    iq_frame.pilot_seed = config.pilot_seed;
    iq_frame.config = config;
    iq_frame.expected_payload = buffers.payload;
    iq_frame.truth_payload_symbols = std::move(encoded.payload_symbols);
    iq_frame.transmit_reference_grid = std::move(encoded.tx_grid);
    iq_frame.has_truth = true;
    iq_frame.samples = buffers.resampled_stream;
    iq_frame.transmit_prepare_us = elapsed_us(simulation_start, transmit_done);
    iq_frame.channel_impairments_us = elapsed_us(transmit_done, channel_done);
}

}  // namespace

void generate_dynamic_tdl_iq_frame(
    LinkMode mode,
    std::uint16_t sequence,
    const Ldpc5041008& codec,
    const DynamicLinkSimulationConfig& config,
    DynamicLinkIqFrame& iq_frame,
    DynamicLinkWorkspace& generation_workspace) {
    generate_dynamic_tdl_iq_frame_impl(
        nullptr, mode, sequence, codec, config, iq_frame,
        generation_workspace);
}

void generate_dynamic_tdl_iq_frame(
    const std::vector<std::uint8_t>& user_payload,
    LinkMode mode,
    std::uint16_t sequence,
    const Ldpc5041008& codec,
    const DynamicLinkSimulationConfig& config,
    DynamicLinkIqFrame& iq_frame,
    DynamicLinkWorkspace& generation_workspace) {
    generate_dynamic_tdl_iq_frame_impl(
        &user_payload, mode, sequence, codec, config, iq_frame,
        generation_workspace);
}

namespace {

void prepare_dynamic_iq_frame_impl(
    const DynamicLinkCaptureFrame& capture,
    const DynamicLinkReceiverConfig& config,
    const DynamicLinkIqFrame* truth,
    PreparedDynamicLinkFrame& prepared,
    DynamicLinkReceiverState* receiver_state,
    DynamicLinkWorkspace& receiver_workspace) {
    const auto receiver_call_start = Clock::now();
    auto& buffers = receiver_workspace;
    const std::size_t growths_before = buffers.capacity_growths;
    const bool has_truth = truth != nullptr && truth->has_truth;
    validate_receiver_config(config);
    const std::size_t physical_ports = capture.samples.size();
    if (physical_ports == 0u || physical_ports > maximum_dynamic_ports ||
        capture.samples[0].empty()) {
        throw std::invalid_argument(
            "dynamic IQ frame must contain one or two receive branches");
    }
    for (std::size_t rx = 1u; rx < physical_ports; ++rx) {
        if (capture.samples[rx].size() != capture.samples[0].size()) {
            throw std::invalid_argument(
                "dynamic IQ receive branches must have equal length");
        }
    }
    std::optional<ImpulseResponse2x2> truth_impulse;
    if (has_truth && truth->config.enable_truth_diagnostics) {
        truth_impulse.emplace(build_simulation_impulse(truth->config));
    }
    const std::size_t stream_samples = capture.samples[0].size();
    const std::size_t frame_symbol_count = formal_frame_symbols(config.pilot_mode);
    const std::size_t data_symbol_offset = config.pilot_mode == PilotMode::nr_dmrs
        ? 3u : 1u;
    const std::size_t expected_payload_size = has_truth
        ? truth->expected_payload.size()
        : 0u;
    resize_tracked(
        buffers.payload, expected_payload_size,
        buffers.capacity_growths);
    if (has_truth) {
        std::copy(
            truth->expected_payload.begin(), truth->expected_payload.end(),
            buffers.payload.begin());
    }
    if (buffers.preamble.size() != symbol_samples) {
        if (buffers.preamble.capacity() < symbol_samples) {
            ++buffers.capacity_growths;
        }
        buffers.preamble = generate_zc_ofdm_symbol(fft_size, cp_length, 29u);
    }
    resize_tracked(buffers.frequency_scratch, fft_size, buffers.capacity_growths);
    resize_tracked(buffers.fft_scratch, fft_size, buffers.capacity_growths);
    resize_tracked(buffers.ofdm_samples, symbol_samples, buffers.capacity_growths);
    resize_branches_tracked(
        buffers.resampled_stream, physical_ports, stream_samples,
        buffers.capacity_growths);
    for (std::size_t rx = 0u; rx < physical_ports; ++rx) {
        std::copy(
            capture.samples[rx].begin(), capture.samples[rx].end(),
            buffers.resampled_stream[rx].begin());
    }
    const auto receiver_start = Clock::now();
    const std::size_t minimum_frame_samples = frame_symbol_count * symbol_samples;
    if (stream_samples < minimum_frame_samples) {
        throw std::invalid_argument("dynamic IQ capture is shorter than one frame");
    }
    const std::size_t maximum_search = std::min(
        config.maximum_timing_offset_samples,
        stream_samples - minimum_frame_samples);
    resize_tracked(
        buffers.timing_estimate.metrics, maximum_search + 1u,
        buffers.capacity_growths);
    SynchronizationMode synchronization_mode_used = SynchronizationMode::search;
    bool tracking_fallback = false;
    const bool can_track =
        receiver_state != nullptr && config.enable_continuous_tracking &&
        receiver_state->timing_valid &&
        receiver_state->synchronization_state == SynchronizationMode::track;
    if (can_track) {
        const std::size_t predicted = std::min(
            receiver_state->predicted_timing_offset, maximum_search);
        const std::size_t half_window = config.tracking_half_window_samples;
        const std::size_t search_begin = predicted > half_window
            ? predicted - half_window
            : 0u;
        const std::size_t search_end = std::min(
            maximum_search, predicted + half_window);
        estimate_zc_timing_window(
            buffers.resampled_stream, buffers.preamble,
            search_begin, search_end, buffers.timing_estimate);
        synchronization_mode_used = SynchronizationMode::track;
        ++receiver_state->tracking_search_count;
        const float required_metric = std::max(
            config.tracking_min_metric,
            receiver_state->last_timing_metric * config.tracking_metric_ratio);
        if (buffers.timing_estimate.peak_metric < required_metric) {
            estimate_zc_timing(
                buffers.resampled_stream, buffers.preamble, maximum_search,
                buffers.timing_estimate);
            synchronization_mode_used = SynchronizationMode::reacquire;
            tracking_fallback = true;
            ++receiver_state->full_search_count;
        }
    } else {
        if (receiver_state != nullptr &&
            receiver_state->synchronization_state == SynchronizationMode::reacquire) {
            synchronization_mode_used = SynchronizationMode::reacquire;
        }
        estimate_zc_timing(
            buffers.resampled_stream, buffers.preamble, maximum_search,
            buffers.timing_estimate);
        if (receiver_state != nullptr) {
            ++receiver_state->full_search_count;
        }
    }
    const auto& timing = buffers.timing_estimate;
    if (timing.offset + frame_symbol_count * symbol_samples > stream_samples) {
        throw std::runtime_error("synchronized dynamic frame exceeds stream");
    }
    const float estimated_cfo = estimate_cp_cfo_normalized(
        buffers.resampled_stream, timing.offset, fft_size, cp_length,
        frame_symbol_count, config.maximum_channel_delay_samples);
    apply_cfo_normalized_inplace(
        buffers.resampled_stream, -estimated_cfo, fft_size);
    const auto synchronization_done = Clock::now();

    resize_tracked(
        buffers.rx_grid, data_symbols * fft_size * physical_ports,
        buffers.capacity_growths);
    if (config.pilot_mode == PilotMode::nr_dmrs) {
        resize_tracked(
            buffers.dmrs_rx_grid, nr_dmrs_symbols * fft_size * physical_ports,
            buffers.capacity_growths);
    } else {
        buffers.dmrs_rx_grid.clear();
    }
    if (config.compute_backend != nullptr) {
        const std::size_t batch_symbols =
            (data_symbols + (config.pilot_mode == PilotMode::nr_dmrs
                ? nr_dmrs_symbols : 0u)) * physical_ports;
        resize_tracked(
            buffers.backend_time_batch, batch_symbols * symbol_samples,
            buffers.capacity_growths);
        std::size_t batch = 0u;
        auto pack_symbol = [&](std::size_t frame_symbol, std::size_t rx) {
            const auto begin = buffers.resampled_stream[rx].begin() +
                static_cast<std::ptrdiff_t>(
                    timing.offset + frame_symbol * symbol_samples);
            std::copy(
                begin, begin + static_cast<std::ptrdiff_t>(symbol_samples),
                buffers.backend_time_batch.begin() +
                    static_cast<std::ptrdiff_t>(batch * symbol_samples));
            ++batch;
        };
        if (config.pilot_mode == PilotMode::nr_dmrs) {
            for (std::size_t symbol = 0u; symbol < nr_dmrs_symbols; ++symbol) {
                for (std::size_t rx = 0u; rx < physical_ports; ++rx) {
                    pack_symbol(symbol + 1u, rx);
                }
            }
        }
        for (std::size_t symbol = 0u; symbol < data_symbols; ++symbol) {
            for (std::size_t rx = 0u; rx < physical_ports; ++rx) {
                pack_symbol(symbol + data_symbol_offset, rx);
            }
        }
        config.compute_backend->ofdm_demodulate_batch(
            fft_size, cp_length, batch_symbols,
            buffers.backend_time_batch, buffers.backend_frequency_batch);
        batch = 0u;
        auto unpack_symbol = [&](
            std::vector<std::complex<float>>& grid,
            std::size_t symbol,
            std::size_t rx) {
            for (std::size_t fft = 0u; fft < fft_size; ++fft) {
                grid[(symbol * fft_size + fft) * physical_ports + rx] =
                    buffers.backend_frequency_batch[batch * fft_size + fft];
            }
            ++batch;
        };
        if (config.pilot_mode == PilotMode::nr_dmrs) {
            for (std::size_t symbol = 0u; symbol < nr_dmrs_symbols; ++symbol) {
                for (std::size_t rx = 0u; rx < physical_ports; ++rx) {
                    unpack_symbol(buffers.dmrs_rx_grid, symbol, rx);
                }
            }
        }
        for (std::size_t symbol = 0u; symbol < data_symbols; ++symbol) {
            for (std::size_t rx = 0u; rx < physical_ports; ++rx) {
                unpack_symbol(buffers.rx_grid, symbol, rx);
            }
        }
    } else {
        if (config.pilot_mode == PilotMode::nr_dmrs) {
            for (std::size_t symbol = 0u; symbol < nr_dmrs_symbols; ++symbol) {
                for (std::size_t rx = 0u; rx < physical_ports; ++rx) {
                    const auto begin = buffers.resampled_stream[rx].begin() +
                        static_cast<std::ptrdiff_t>(
                            timing.offset + (symbol + 1u) * symbol_samples);
                    std::copy(
                        begin, begin + static_cast<std::ptrdiff_t>(symbol_samples),
                        buffers.ofdm_samples.begin());
                    ofdm_demodulate(
                        buffers.ofdm_samples, fft_size, cp_length,
                        buffers.frequency_scratch);
                    for (std::size_t fft = 0u; fft < fft_size; ++fft) {
                        buffers.dmrs_rx_grid[
                            (symbol * fft_size + fft) * physical_ports + rx] =
                            buffers.frequency_scratch[fft];
                    }
                }
            }
        }
        for (std::size_t symbol = 0u; symbol < data_symbols; ++symbol) {
            for (std::size_t rx = 0u; rx < physical_ports; ++rx) {
                const auto begin = buffers.resampled_stream[rx].begin() +
                    static_cast<std::ptrdiff_t>(
                        timing.offset +
                        (symbol + data_symbol_offset) * symbol_samples);
                std::copy(
                    begin, begin + static_cast<std::ptrdiff_t>(symbol_samples),
                    buffers.ofdm_samples.begin());
                ofdm_demodulate(
                    buffers.ofdm_samples, fft_size, cp_length,
                    buffers.frequency_scratch);
                for (std::size_t fft = 0u; fft < fft_size; ++fft) {
                    buffers.rx_grid[
                        (symbol * fft_size + fft) * physical_ports + rx] =
                        buffers.frequency_scratch[fft];
                }
            }
        }
    }
    const auto fft_grid_done = Clock::now();
    const LinkMode reference_mode{
        physical_ports == 1u ? 1u : 2u,
        Modulation::qam64,
        TransmissionScheme::spatial_multiplexing,
        static_cast<unsigned>(physical_ports)};
    const auto& reference_layout = cached_formal_frame_layout(reference_mode);
    if (!buffers.pilot_reference_valid ||
        buffers.pilot_reference_seed != capture.pilot_seed ||
        buffers.pilot_reference_grid.size() !=
            data_symbols * fft_size * physical_ports) {
        const auto capacity_before = buffers.pilot_reference_grid.capacity();
        build_dynamic_pilot_reference_grid(
            capture.pilot_seed, reference_mode,
            buffers.pilot_reference_grid);
        if (buffers.pilot_reference_grid.capacity() > capacity_before) {
            ++buffers.capacity_growths;
        }
        buffers.pilot_reference_seed = capture.pilot_seed;
        buffers.pilot_reference_valid = true;
    }
    if (config.pilot_mode == PilotMode::nr_dmrs &&
        (!buffers.dmrs_reference_valid ||
         buffers.dmrs_reference_seed != capture.pilot_seed ||
         buffers.dmrs_reference_grid.size() !=
             nr_dmrs_symbols * fft_size * physical_ports)) {
        const auto capacity_before = buffers.dmrs_reference_grid.capacity();
        build_nr_dmrs_reference_grid(
            fft_size, reference_layout.active_fft_indices, physical_ports,
            capture.pilot_seed, buffers.dmrs_reference_grid);
        if (buffers.dmrs_reference_grid.capacity() > capacity_before) {
            ++buffers.capacity_growths;
        }
        buffers.dmrs_reference_seed = capture.pilot_seed;
        buffers.dmrs_reference_valid = true;
    }
    const auto sfo = estimate_sfo_phase_slope(
        buffers.rx_grid, reference_layout.phase_reference_fft_indices,
        fft_size, physical_ports, symbol_samples);
    if (config.pilot_mode == PilotMode::nr_dmrs) {
        correct_symbol_phase_inplace(
            buffers.dmrs_rx_grid, sfo, fft_size, physical_ports, 1u, 1.0f);
        correct_symbol_phase_inplace(
            buffers.rx_grid, sfo, fft_size, physical_ports, 0u, 2.0f);
        correct_symbol_phase_inplace(
            buffers.rx_grid, sfo, fft_size, physical_ports, 1u, 3.0f);
    } else {
        correct_second_symbol_phase_inplace(
            buffers.rx_grid, sfo, fft_size, physical_ports);
    }
    const auto residual_sfo = estimate_sfo_phase_slope(
        buffers.rx_grid, reference_layout.phase_reference_fft_indices,
        fft_size, physical_ports, symbol_samples);
    const auto sfo_done = Clock::now();

    resize_tracked(
        buffers.channels, data_symbols * fft_size, buffers.capacity_growths);
    if (config.pilot_mode == PilotMode::nr_dmrs) {
        const std::size_t estimator_growths_before =
            buffers.dmrs_channel_estimation.capacity_growths;
        estimate_nr_dmrs_channel_linear_nxn(
            buffers.dmrs_rx_grid, buffers.dmrs_reference_grid,
            reference_layout.active_fft_indices, data_symbols, fft_size,
            physical_ports, buffers.dmrs_channels,
            buffers.dmrs_channel_estimation);
        for (std::size_t time = 0u; time < data_symbols; ++time) {
            const auto phase_tracking =
                estimate_reference_residual_phase_slope_nxn(
                    buffers.rx_grid, buffers.pilot_reference_grid,
                    reference_layout.pilot_fft_indices,
                    buffers.dmrs_channels, time, fft_size, physical_ports);
            correct_symbol_phase_inplace(
                buffers.rx_grid, phase_tracking, fft_size, physical_ports,
                time, 1.0f);
        }
        buffers.capacity_growths +=
            buffers.dmrs_channel_estimation.capacity_growths -
            estimator_growths_before;
        for (std::size_t index = 0u; index < buffers.channels.size(); ++index) {
            const auto& source = buffers.dmrs_channels[index];
            auto& target = buffers.channels[index];
            target.h00 = source.values[0u * maximum_spatial_streams + 0u];
            target.h01 = physical_ports == 2u
                ? source.values[0u * maximum_spatial_streams + 1u]
                : std::complex<float>{};
            target.h10 = physical_ports == 2u
                ? source.values[1u * maximum_spatial_streams + 0u]
                : std::complex<float>{};
            target.h11 = physical_ports == 2u
                ? source.values[1u * maximum_spatial_streams + 1u]
                : std::complex<float>{};
        }
    } else if (physical_ports == 1u) {
        const std::size_t estimator_growths_before =
            buffers.dmrs_channel_estimation.capacity_growths;
        estimate_fdm_pilot_channel_linear_nxn(
            buffers.rx_grid, buffers.pilot_reference_grid,
            reference_layout.pilot_fft_indices,
            data_symbols, fft_size, physical_ports,
            buffers.dmrs_channels, buffers.dmrs_channel_estimation);
        buffers.capacity_growths +=
            buffers.dmrs_channel_estimation.capacity_growths -
            estimator_growths_before;
        for (std::size_t index = 0u; index < buffers.channels.size(); ++index) {
            buffers.channels[index] = Channel2x2{};
            buffers.channels[index].h00 =
                buffers.dmrs_channels[index].values[0u];
        }
    } else {
        const std::size_t estimator_growths_before =
            buffers.channel_estimation.capacity_growths;
        estimate_fdm_pilot_channel_linear_2x2(
            buffers.rx_grid, buffers.pilot_reference_grid,
            reference_layout.pilot_fft_indices,
            data_symbols, fft_size, buffers.channels,
            buffers.channel_estimation);
        buffers.capacity_growths +=
            buffers.channel_estimation.capacity_growths -
            estimator_growths_before;
    }
    const auto channel_estimation_done = Clock::now();

    float raw_noise_variance = config.fixed_noise_variance;
    bool noise_variance_estimated = false;
    if (config.noise_variance_mode == NoiseVarianceMode::pilot_residual) {
        // Both pilot modes retain identical sparse references in the two data
        // symbols. Differential pilot noise estimation cancels the channel and
        // avoids treating front-loaded DM-RS interpolation error as AWGN.
        raw_noise_variance = estimate_pilot_residual_noise_variance(
            buffers.rx_grid, buffers.pilot_reference_grid,
            reference_layout.pilot_fft_indices,
            physical_ports,
            buffers.noise_power_samples, buffers.capacity_growths);
        noise_variance_estimated = true;
    }
    raw_noise_variance = std::clamp(
        raw_noise_variance,
        config.minimum_noise_variance,
        config.maximum_noise_variance);
    float noise_variance = raw_noise_variance;
    std::size_t noise_variance_age_frames = 1u;
    if (receiver_state != nullptr && noise_variance_estimated) {
        if (receiver_state->noise_variance_valid) {
            const float alpha = config.noise_smoothing_alpha;
            receiver_state->filtered_noise_variance =
                alpha * raw_noise_variance +
                (1.0f - alpha) * receiver_state->filtered_noise_variance;
        } else {
            receiver_state->filtered_noise_variance = raw_noise_variance;
        }
        receiver_state->noise_variance_valid = true;
        ++receiver_state->noise_variance_age_frames;
        noise_variance = std::clamp(
            receiver_state->filtered_noise_variance,
            config.minimum_noise_variance,
            config.maximum_noise_variance);
        noise_variance_age_frames =
            receiver_state->noise_variance_age_frames;
    } else if (receiver_state != nullptr) {
        receiver_state->noise_variance_valid = false;
        receiver_state->filtered_noise_variance = noise_variance;
        receiver_state->noise_variance_age_frames = 0u;
    }
    const auto noise_estimation_done = Clock::now();
    const std::vector<Channel2x2>* channel_view = &buffers.channels;
    bool csi_smoothed = false;
    if (receiver_state != nullptr) {
        if (receiver_state->csi_valid &&
            receiver_state->filtered_channels.size() == buffers.channels.size()) {
            const float alpha = config.csi_smoothing_alpha;
            const float history = 1.0f - alpha;
            for (std::size_t index = 0u; index < buffers.channels.size(); ++index) {
                const auto& current = buffers.channels[index];
                auto& filtered = receiver_state->filtered_channels[index];
                filtered.h00 = alpha * current.h00 + history * filtered.h00;
                filtered.h01 = alpha * current.h01 + history * filtered.h01;
                filtered.h10 = alpha * current.h10 + history * filtered.h10;
                filtered.h11 = alpha * current.h11 + history * filtered.h11;
            }
            csi_smoothed = alpha < 1.0f;
        } else {
            receiver_state->filtered_channels = buffers.channels;
        }
        channel_view = &receiver_state->filtered_channels;
        receiver_state->csi_valid = true;
        ++receiver_state->csi_age_frames;
    }
    const auto& channels = *channel_view;
    const auto fft_csi_done = Clock::now();

    const auto channel_diagnostics_start = Clock::now();
    double channel_error = 0.0;
    double channel_reference = 0.0;
    if (truth_impulse.has_value()) {
        for (std::size_t time = 0u; time < data_symbols; ++time) {
            for (const auto fft : reference_layout.data_fft_indices) {
                const auto truth_channel =
                    tdl_frequency_response(*truth_impulse, fft, fft_size);
                const auto& estimate = channels[time * fft_size + fft];
                channel_error += std::norm(estimate.h00 - truth_channel.h00);
                channel_reference += std::norm(truth_channel.h00);
                if (physical_ports == 2u) {
                    channel_error +=
                        std::norm(estimate.h01 - truth_channel.h01) +
                        std::norm(estimate.h10 - truth_channel.h10) +
                        std::norm(estimate.h11 - truth_channel.h11);
                    channel_reference +=
                        std::norm(truth_channel.h01) +
                        std::norm(truth_channel.h10) +
                        std::norm(truth_channel.h11);
                }
            }
        }
    }
    const auto detection_start = Clock::now();

    resize_tracked(
        buffers.control_llrs,
        reference_layout.control_data_positions.size() * 2u,
        buffers.capacity_growths);
    std::size_t control_llr_index = 0u;
    for (std::size_t control = 0u;
         control < reference_layout.control_data_positions.size(); ++control) {
        const std::size_t data = reference_layout.control_data_positions[control];
        const std::size_t fft = reference_layout.data_fft_indices[data];
        const std::array<std::complex<float>, 2> received{{
            buffers.rx_grid[fft * physical_ports],
            physical_ports == 2u
                ? buffers.rx_grid[fft * physical_ports + 1u]
                : std::complex<float>{}}};
        float variance = 0.0f;
        const auto symbol = mrc_detect_tx0(
            received, channels[fft], variance, noise_variance);
        const auto llrs = SquareQAM::max_log_llrs(symbol, variance, 2u);
        buffers.control_llrs[control_llr_index++] = llrs[0];
        buffers.control_llrs[control_llr_index++] = llrs[1];
    }

    float preliminary_marker_metric = 0.0f;
    MiniHeader preliminary_header;
    LinkMode received_mode{
        1u, Modulation::qpsk,
        TransmissionScheme::spatial_multiplexing,
        static_cast<unsigned>(physical_ports)};
    bool preliminary_header_ok = true;
    try {
        preliminary_header = decode_control_qpsk_llrs(
            buffers.control_llrs, &preliminary_marker_metric);
        received_mode = {
            transmit_rank_from_flags(preliminary_header.flags),
            modulation_from_bits(
                bits_per_symbol_from_flags(preliminary_header.flags)),
            transmission_scheme_from_flags(preliminary_header.flags),
            static_cast<unsigned>(physical_ports)};
    } catch (const std::exception&) {
        preliminary_header_ok = false;
    }
    const auto& received_layout = cached_formal_frame_layout(received_mode);
    if (preliminary_header.payload_blocks == 0u ||
        preliminary_header.payload_blocks > received_layout.ldpc_blocks ||
        preliminary_header.payload_len < 2u ||
        preliminary_header.payload_len > received_layout.information_bytes) {
        preliminary_header_ok = false;
    }

    resize_tracked(
        buffers.equalized, received_layout.payload_layer_symbols,
        buffers.capacity_growths);
    resize_tracked(
        buffers.variances, received_layout.payload_layer_symbols,
        buffers.capacity_growths);
    LinkDecision recommendation;
    if (received_mode.scheme == TransmissionScheme::alamouti_stbc) {
        const std::size_t pairs =
            received_layout.payload_time_indices.size() / 2u;
        if (pairs * 2u != received_layout.payload_time_indices.size()) {
            throw std::runtime_error("received STBC layout is not time-paired");
        }
        double variance_sum = 0.0;
        for (std::size_t pair = 0u; pair < pairs; ++pair) {
            const std::size_t second = pair + pairs;
            if (received_layout.payload_time_indices[pair] != 0u ||
                received_layout.payload_time_indices[second] != 1u ||
                received_layout.payload_data_positions[pair] !=
                    received_layout.payload_data_positions[second]) {
                throw std::runtime_error("received STBC resource pairs are inconsistent");
            }
            const std::size_t data =
                received_layout.payload_data_positions[pair];
            const std::size_t fft = received_layout.data_fft_indices[data];
            const std::array<std::complex<float>, 2> received_first{{
                buffers.rx_grid[fft * physical_ports],
                buffers.rx_grid[fft * physical_ports + 1u]}};
            const std::array<std::complex<float>, 2> received_second{{
                buffers.rx_grid[(fft_size + fft) * physical_ports],
                buffers.rx_grid[(fft_size + fft) * physical_ports + 1u]}};
            const auto& first_channel = channels[fft];
            const auto& second_channel = channels[fft_size + fft];
            const Channel2x2 average_channel{
                0.5f * (first_channel.h00 + second_channel.h00),
                0.5f * (first_channel.h01 + second_channel.h01),
                0.5f * (first_channel.h10 + second_channel.h10),
                0.5f * (first_channel.h11 + second_channel.h11)};
            const auto detected = detect_alamouti_2x2(
                received_first, received_second, average_channel,
                noise_variance);
            buffers.equalized[pair] = detected.symbols[0];
            buffers.equalized[second] = detected.symbols[1];
            buffers.variances[pair] = detected.equivalent_variance;
            buffers.variances[second] = detected.equivalent_variance;
            variance_sum += 2.0 * detected.equivalent_variance;
        }
        recommendation.desired = received_mode;
        recommendation.configured_mcs_supported = true;
        const double mean_variance = variance_sum /
            static_cast<double>(std::max<std::size_t>(1u, pairs * 2u));
        recommendation.rank1_sinr_db = static_cast<float>(
            10.0 * std::log10(1.0 / std::max(mean_variance, 1.0e-15)));
    } else if (physical_ports == 1u) {
        double variance_sum = 0.0;
        for (std::size_t payload_index = 0u;
             payload_index < received_layout.payload_time_indices.size();
             ++payload_index) {
            const std::size_t time =
                received_layout.payload_time_indices[payload_index];
            const std::size_t data =
                received_layout.payload_data_positions[payload_index];
            const std::size_t fft = received_layout.data_fft_indices[data];
            const auto received =
                buffers.rx_grid[time * fft_size + fft];
            const auto channel = channels[time * fft_size + fft].h00;
            const float power = std::max(std::norm(channel), 1.0e-12f);
            buffers.equalized[payload_index] =
                std::conj(channel) * received / power;
            buffers.variances[payload_index] = noise_variance / power;
            variance_sum += buffers.variances[payload_index];
        }
        recommendation.desired = received_mode;
        recommendation.configured_mcs_supported = true;
        const double mean_variance = variance_sum /
            static_cast<double>(std::max<std::size_t>(
                1u, received_layout.payload_time_indices.size()));
        recommendation.rank1_sinr_db = static_cast<float>(
            10.0 * std::log10(1.0 / std::max(mean_variance, 1.0e-15)));
    } else {
        const std::size_t payload_resources =
            received_layout.payload_time_indices.size();
        resize_tracked(
            buffers.adaptation_channels,
            payload_resources, buffers.capacity_growths);
        resize_tracked(
            buffers.adaptation_mse,
            payload_resources, buffers.capacity_growths);
        if (config.compute_backend != nullptr) {
            resize_tracked(
                buffers.backend_received_batch, payload_resources * 2u,
                buffers.capacity_growths);
            resize_tracked(
                buffers.backend_channel_batch, payload_resources * 4u,
                buffers.capacity_growths);
            for (std::size_t payload_index = 0u;
                 payload_index < payload_resources; ++payload_index) {
                const std::size_t time =
                    received_layout.payload_time_indices[payload_index];
                const std::size_t data =
                    received_layout.payload_data_positions[payload_index];
                const std::size_t fft = received_layout.data_fft_indices[data];
                const std::size_t grid_index = time * fft_size + fft;
                buffers.backend_received_batch[payload_index * 2u] =
                    buffers.rx_grid[grid_index * physical_ports];
                buffers.backend_received_batch[payload_index * 2u + 1u] =
                    buffers.rx_grid[grid_index * physical_ports + 1u];
                const auto& channel = channels[grid_index];
                const std::size_t channel_offset = payload_index * 4u;
                buffers.backend_channel_batch[channel_offset] = channel.h00;
                buffers.backend_channel_batch[channel_offset + 1u] = channel.h01;
                buffers.backend_channel_batch[channel_offset + 2u] = channel.h10;
                buffers.backend_channel_batch[channel_offset + 3u] = channel.h11;
            }
            config.compute_backend->detect_mimo_batch(
                2u, 2u, payload_resources,
                buffers.backend_received_batch, buffers.backend_channel_batch,
                noise_variance, LinearDetector::mmse,
                0u, 0u, true,
                buffers.backend_detected_batch, buffers.backend_mse_batch,
                buffers.backend_soft_bits);
        }
        std::size_t adaptation_index = 0u;
        for (std::size_t payload_index = 0u;
             payload_index < payload_resources;
             ++payload_index) {
            const std::size_t time =
                received_layout.payload_time_indices[payload_index];
            const std::size_t data =
                received_layout.payload_data_positions[payload_index];
            const std::size_t fft = received_layout.data_fft_indices[data];
            const std::array<std::complex<float>, 2> received{{
                buffers.rx_grid[(time * fft_size + fft) * physical_ports],
                buffers.rx_grid[
                    (time * fft_size + fft) * physical_ports + 1u]}};
            const auto& channel = channels[time * fft_size + fft];
            Detection2x2 rank2_probe;
            if (config.compute_backend != nullptr) {
                rank2_probe.symbols[0] =
                    buffers.backend_detected_batch[payload_index * 2u];
                rank2_probe.symbols[1] =
                    buffers.backend_detected_batch[payload_index * 2u + 1u];
                rank2_probe.predicted_mse[0] =
                    buffers.backend_mse_batch[payload_index * 2u];
                rank2_probe.predicted_mse[1] =
                    buffers.backend_mse_batch[payload_index * 2u + 1u];
            } else {
                rank2_probe = detect_2x2(
                    received, channel, noise_variance, LinearDetector::mmse);
            }
            buffers.adaptation_channels[adaptation_index] = channel;
            buffers.adaptation_mse[adaptation_index] = rank2_probe.predicted_mse;
            ++adaptation_index;
            if (received_mode.rank == 1u) {
                float variance = 0.0f;
                buffers.equalized[payload_index] = mrc_detect_tx0(
                    received, channel, variance, noise_variance);
                buffers.variances[payload_index] = variance;
            } else {
                for (std::size_t layer = 0u; layer < 2u; ++layer) {
                    const std::size_t index = payload_index * 2u + layer;
                    buffers.equalized[index] = rank2_probe.symbols[layer];
                    buffers.variances[index] = rank2_probe.predicted_mse[layer];
                }
            }
        }
        recommendation = recommend_rank_mcs(
            buffers.adaptation_channels, buffers.adaptation_mse, noise_variance,
            LinearDetector::mmse, received_mode.modulation,
            0.01f, 4.0f, 2.0f);
    }
    const auto detection_done = Clock::now();

    const auto evm_diagnostics_start = Clock::now();
    double error_power = 0.0;
    double reference_power = 0.0;
    bool evm_available =
        truth_impulse.has_value() &&
        truth->truth_payload_symbols.size() == buffers.equalized.size();
    if (evm_available) {
        for (std::size_t index = 0u; index < buffers.equalized.size(); ++index) {
            error_power += std::norm(
                buffers.equalized[index] - truth->truth_payload_symbols[index]);
            reference_power += std::norm(truth->truth_payload_symbols[index]);
        }
    } else if (!has_truth && preliminary_header_ok &&
               !buffers.equalized.empty()) {
        // Hardware captures have no transmitted truth grid. Report the
        // conventional decision-directed EVM against the nearest decoded QAM
        // point so gain scans can identify compression and poor equalization.
        const unsigned bits = modulation_bits(received_mode.modulation);
        if (SquareQAM::supported(bits)) {
            evm_available = true;
            for (const auto sample : buffers.equalized) {
                const auto reference = SquareQAM::modulate(
                    SquareQAM::demodulate(sample, bits), bits);
                error_power += std::norm(sample - reference);
                reference_power += std::norm(reference);
            }
        }
    }

    DynamicLinkSimulationResult result;
    result.pilot_mode = config.pilot_mode;
    result.frame_symbols = frame_symbol_count;
    result.transmitted_mode = has_truth
        ? truth->transmitted_mode
        : LinkMode{};
    result.sequence = preliminary_header.sequence;
    result.user_payload_bytes = buffers.payload.size();
    result.timing_ok = has_truth
        ? timing.offset == truth->config.timing_offset_samples
        : timing.peak_metric >= config.tracking_min_metric;
    result.synchronization_mode_used = synchronization_mode_used;
    result.tracking_fallback = tracking_fallback;
    result.timing_candidates_evaluated = timing.metrics.size();
    result.timing_metric = timing.peak_metric;
    result.cfo_error_hz = has_truth
        ? (estimated_cfo - truth->config.cfo_hz / subcarrier_spacing_hz) *
              subcarrier_spacing_hz
        : 0.0f;
    result.residual_sfo_ppm = residual_sfo.sfo_ppm;
    result.noise_variance_estimated = noise_variance_estimated;
    result.raw_noise_variance = raw_noise_variance;
    result.noise_variance_used = noise_variance;
    result.noise_variance_age_frames = noise_variance_age_frames;
    result.channel_nmse_db = truth_impulse.has_value()
        ? static_cast<float>(10.0 * std::log10(
              std::max(channel_error / channel_reference, 1.0e-30)))
        : 0.0f;
    result.evm_percent = evm_available
        ? static_cast<float>(100.0 * std::sqrt(
              error_power / std::max(reference_power, 1.0e-30)))
        : 0.0f;
    result.csi_smoothed = csi_smoothed;
    result.csi_age_frames = receiver_state == nullptr
        ? 1u
        : receiver_state->csi_age_frames;
    result.recommendation = recommendation;
    const auto decode_start = Clock::now();
    const std::size_t llr_capacity_before = buffers.frame_decode.llrs.capacity();
    const std::size_t interleaver_capacity_before =
        buffers.frame_decode.interleaved_block.capacity();
    double soft_demapping_us = 0.0;
    try {
        if (!preliminary_header_ok) {
            throw std::runtime_error("preliminary control header is invalid");
        }
        prepare_dynamic_frame_payload_llrs(
            preliminary_header, preliminary_marker_metric,
            received_mode, buffers.equalized, buffers.variances,
            buffers.frame_decode);
        result.header_ok = true;
        result.decoded_mode = buffers.frame_decode.mode;
        result.user_payload_bytes =
            buffers.frame_decode.header.payload_len >= 2u
            ? buffers.frame_decode.header.payload_len - 2u
            : 0u;
        result.marker_metric = buffers.frame_decode.marker_metric;
        soft_demapping_us = buffers.frame_decode.soft_demapping_us;
    } catch (const std::exception&) {
        result.header_ok = false;
        result.crc_ok = false;
    }
    if (buffers.frame_decode.llrs.capacity() > llr_capacity_before) {
        ++buffers.capacity_growths;
    }
    if (buffers.frame_decode.interleaved_block.capacity() >
        interleaver_capacity_before) {
        ++buffers.capacity_growths;
    }
    if (receiver_state != nullptr) {
        if (!result.timing_ok || !result.header_ok) {
            receiver_state->reset();
            result.csi_age_frames = 0u;
        } else {
            if (synchronization_mode_used == SynchronizationMode::reacquire) {
                ++receiver_state->reacquisition_count;
            }
            receiver_state->synchronization_state = SynchronizationMode::track;
            receiver_state->timing_valid = true;
            receiver_state->predicted_timing_offset = timing.offset;
            receiver_state->last_timing_metric = timing.peak_metric;
            receiver_state->synchronization_lock_age_frames =
                synchronization_mode_used == SynchronizationMode::track
                ? receiver_state->synchronization_lock_age_frames + 1u
                : 1u;
            receiver_state->consecutive_sync_failures = 0u;
        }
        result.receiver_synchronization_state =
            receiver_state->synchronization_state;
        result.synchronization_lock_age_frames =
            receiver_state->synchronization_lock_age_frames;
    }
    const auto decode_done = Clock::now();
    ++buffers.frames_processed;
    result.workspace_growths_this_frame =
        buffers.capacity_growths - growths_before;
    result.workspace_total_growths = buffers.capacity_growths;
    result.timing.transmit_prepare_us = has_truth
        ? truth->transmit_prepare_us
        : 0.0;
    result.timing.channel_impairments_us = has_truth
        ? truth->channel_impairments_us
        : 0.0;
    result.timing.synchronization_us =
        elapsed_us(receiver_start, synchronization_done);
    result.timing.fft_csi_us =
        elapsed_us(synchronization_done, fft_csi_done);
    result.timing.fft_grid_us =
        elapsed_us(synchronization_done, fft_grid_done);
    result.timing.sfo_correction_us =
        elapsed_us(fft_grid_done, sfo_done);
    result.timing.channel_estimation_us =
        elapsed_us(sfo_done, channel_estimation_done);
    result.timing.noise_estimation_us =
        elapsed_us(channel_estimation_done, noise_estimation_done);
    result.timing.csi_smoothing_us =
        elapsed_us(noise_estimation_done, fft_csi_done);
    result.timing.detection_adaptation_us =
        elapsed_us(detection_start, detection_done);
    result.timing.control_fec_us = elapsed_us(decode_start, decode_done);
    result.timing.soft_demapping_us = soft_demapping_us;
    result.timing.ldpc_crc_us = 0.0;
    result.timing.control_header_us = std::max(
        0.0, result.timing.control_fec_us - soft_demapping_us);
    result.timing.diagnostics_us =
        elapsed_us(channel_diagnostics_start, detection_start) +
        elapsed_us(evm_diagnostics_start, decode_start);
    result.timing.receiver_wall_us = elapsed_us(receiver_start, decode_done);
    result.timing.receiver_total_us =
        result.timing.synchronization_us + result.timing.fft_csi_us +
        result.timing.detection_adaptation_us + result.timing.control_fec_us;
    result.timing.simulation_total_us =
        result.timing.transmit_prepare_us +
        result.timing.channel_impairments_us +
        elapsed_us(receiver_call_start, decode_done);
    prepared.result = result;
    prepared.ready = true;
}

}  // namespace

void prepare_dynamic_iq_frame(
    const DynamicLinkIqFrame& iq_frame,
    PreparedDynamicLinkFrame& prepared,
    DynamicLinkReceiverState* receiver_state,
    DynamicLinkWorkspace& receiver_workspace) {
    const auto receiver_config = make_dynamic_link_receiver_config(
        iq_frame.config, NoiseVarianceMode::fixed);
    prepare_dynamic_iq_frame_impl(
        iq_frame, receiver_config, &iq_frame, prepared,
        receiver_state, receiver_workspace);
}

void prepare_captured_iq_frame(
    const DynamicLinkCaptureFrame& capture,
    const DynamicLinkReceiverConfig& receiver_config,
    PreparedDynamicLinkFrame& prepared,
    DynamicLinkReceiverState* receiver_state,
    DynamicLinkWorkspace& receiver_workspace) {
    prepare_dynamic_iq_frame_impl(
        capture, receiver_config, nullptr, prepared,
        receiver_state, receiver_workspace);
}

void prepare_captured_iq_frame(
    const DynamicLinkCaptureFrame& capture,
    const DynamicLinkSimulationConfig& receiver_config,
    PreparedDynamicLinkFrame& prepared,
    DynamicLinkReceiverState* receiver_state,
    DynamicLinkWorkspace& receiver_workspace) {
    prepare_captured_iq_frame(
        capture, make_dynamic_link_receiver_config(receiver_config), prepared,
        receiver_state, receiver_workspace);
}

void prepare_dynamic_tdl_frame(
    LinkMode mode,
    std::uint16_t sequence,
    const Ldpc5041008& codec,
    const DynamicLinkSimulationConfig& config,
    PreparedDynamicLinkFrame& prepared,
    DynamicLinkReceiverState* receiver_state,
    DynamicLinkWorkspace& workspace) {
    DynamicLinkIqFrame iq_frame;
    generate_dynamic_tdl_iq_frame(
        mode, sequence, codec, config, iq_frame, workspace);
    prepare_dynamic_iq_frame(
        iq_frame, prepared, receiver_state, workspace);
}

DynamicLinkSimulationResult finish_dynamic_tdl_frame(
    PreparedDynamicLinkFrame& prepared,
    const Ldpc5041008& codec,
    const DynamicLinkWorkspace& workspace,
    LdpcFrameDecoder* ldpc_frame_decoder) {
    if (!prepared.ready) {
        throw std::logic_error("dynamic link frame was not prepared");
    }
    auto result = prepared.result;
    const auto fec_start = Clock::now();
    if (result.header_ok) {
        try {
            const auto decoded = decode_prepared_dynamic_frame(
                workspace.frame_decode, codec, 6u, 0.8f, ldpc_frame_decoder);
            result.decoded_mode = decoded.mode;
            const bool truth_matches = workspace.payload.empty() ||
                decoded.user_payload == workspace.payload;
            result.crc_ok = decoded.crc_ok && truth_matches;
            if (result.crc_ok) {
                result.user_payload = decoded.user_payload;
            } else {
                result.user_payload.clear();
            }
            result.syndrome_failures = decoded.syndrome_failures;
            result.ldpc_worker_threads = decoded.ldpc_worker_threads;
            result.ldpc_capacity_growths_this_frame =
                decoded.ldpc_capacity_growths_this_frame;
        } catch (const std::exception&) {
            result.crc_ok = false;
        }
    }
    const auto fec_done = Clock::now();
    const double fec_us = elapsed_us(fec_start, fec_done);
    result.timing.ldpc_crc_us = fec_us;
    result.timing.control_fec_us += fec_us;
    result.timing.receiver_wall_us += fec_us;
    result.timing.receiver_total_us += fec_us;
    result.timing.simulation_total_us += fec_us;
    result.goodput_bps = result.crc_ok
        ? static_cast<double>(result.user_payload_bytes * 8u) /
            formal_frame_period_seconds(result.pilot_mode)
        : 0.0;
    prepared.ready = false;
    return result;
}

DynamicLinkSimulationResult simulate_dynamic_tdl_frame(
    LinkMode mode,
    std::uint16_t sequence,
    const Ldpc5041008& codec,
    const DynamicLinkSimulationConfig& config,
    DynamicLinkReceiverState* receiver_state,
    DynamicLinkWorkspace* workspace,
    LdpcFrameDecoder* ldpc_frame_decoder) {
    DynamicLinkWorkspace local_workspace;
    auto& buffers = workspace == nullptr ? local_workspace : *workspace;
    PreparedDynamicLinkFrame prepared;
    prepare_dynamic_tdl_frame(
        mode, sequence, codec, config, prepared, receiver_state, buffers);
    return finish_dynamic_tdl_frame(
        prepared, codec, buffers, ldpc_frame_decoder);
}

}  // namespace openisac
