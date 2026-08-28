#include "openisac/rank4_time_link.hpp"

#include "openisac/channel_estimation.hpp"
#include "openisac/dynamic_frame.hpp"
#include "openisac/frame.hpp"
#include "openisac/ldpc_frame_decoder.hpp"
#include "openisac/ofdm.hpp"
#include "openisac/preamble_sync.hpp"
#include "openisac/qam.hpp"
#include "openisac/sampling_offset.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

namespace openisac {
namespace {

constexpr std::size_t fft_size = 1024u;
constexpr std::size_t cp_length = 128u;
constexpr std::size_t symbol_samples = fft_size + cp_length;
constexpr std::size_t data_symbols = 2u;
constexpr std::size_t ports = 4u;
constexpr float subcarrier_spacing_hz = 15000.0f;
using Clock = std::chrono::steady_clock;

double elapsed_us(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::micro>(end - begin).count();
}

float ratio_db(double numerator, double denominator) {
    if (!(denominator > 0.0)) {
        throw std::runtime_error("Rank-4 time metric has zero denominator");
    }
    return 10.0f * std::log10(static_cast<float>(
        std::max(numerator / denominator, 1.0e-20)));
}

std::size_t count_bit_errors(unsigned left, unsigned right, unsigned bits) {
    unsigned difference = left ^ right;
    std::size_t result = 0u;
    for (unsigned bit = 0u; bit < bits; ++bit) {
        result += difference & 1u;
        difference >>= 1u;
    }
    return result;
}

std::size_t maximum_delay(const std::vector<TdlTap>& taps) {
    std::size_t result = 0u;
    for (const auto& tap : taps) {
        result = std::max(result, tap.delay_samples);
    }
    return result;
}

void validate(const Rank4TimeSimulationConfig& config) {
    if ((config.spatial_rank != 2u && config.spatial_rank != 4u) ||
        !std::isfinite(config.snr_db) || !std::isfinite(config.cfo_hz) ||
        !std::isfinite(config.sfo_ppm) || std::abs(config.sfo_ppm) >= 10000.0f ||
        !std::isfinite(config.transmit_correlation) ||
        !std::isfinite(config.receive_correlation) ||
        std::abs(config.transmit_correlation) >= 1.0f ||
        std::abs(config.receive_correlation) >= 1.0f ||
        !std::isfinite(config.csi_smoothing_alpha) ||
        config.csi_smoothing_alpha <= 0.0f ||
        config.csi_smoothing_alpha > 1.0f ||
        !std::isfinite(config.mmse_regularization_scale) ||
        config.mmse_regularization_scale <= 0.0f ||
        config.mmse_regularization_scale > 16.0f ||
        !std::isfinite(config.channel_time_seconds) ||
        config.diagnostic_waveform_points == 0u ||
        config.tracking_half_window_samples > cp_length ||
        !std::isfinite(config.tracking_min_metric) ||
        config.tracking_min_metric < 0.0f || config.tracking_min_metric > 1.0f ||
        !std::isfinite(config.tracking_metric_ratio) ||
        config.tracking_metric_ratio < 0.0f ||
        config.tracking_metric_ratio > 1.0f ||
        config.maximum_ldpc_iterations == 0u || config.taps.empty() ||
        maximum_delay(config.taps) >= cp_length) {
        throw std::invalid_argument("invalid Rank-4 time-link configuration");
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

float estimate_pilot_noise(
    const std::vector<std::complex<float>>& receive_grid,
    const std::vector<std::complex<float>>& reference_grid,
    const std::vector<std::uint16_t>& pilot_indices,
    std::vector<float>& powers) {
    powers.clear();
    powers.reserve(pilot_indices.size() * ports);
    for (const auto fft : pilot_indices) {
        std::size_t tx = ports;
        for (std::size_t candidate = 0u; candidate < ports; ++candidate) {
            if (std::norm(reference_grid[fft * ports + candidate]) > 0.0f) {
                tx = candidate;
                break;
            }
        }
        if (tx == ports) {
            continue;
        }
        const auto first_reference = reference_grid[fft * ports + tx];
        const auto second_reference =
            reference_grid[(fft_size + fft) * ports + tx];
        for (std::size_t rx = 0u; rx < ports; ++rx) {
            const auto first_received = receive_grid[fft * ports + rx];
            const auto second_received =
                receive_grid[(fft_size + fft) * ports + rx];
            const auto predicted =
                first_received / first_reference * second_reference;
            powers.push_back(0.5f * std::norm(second_received - predicted));
        }
    }
    if (powers.empty()) {
        throw std::runtime_error("Rank-4 pilot noise estimator has no samples");
    }
    const auto middle = powers.begin() +
        static_cast<std::ptrdiff_t>(powers.size() / 2u);
    std::nth_element(powers.begin(), middle, powers.end());
    constexpr float exponential_median_ratio = 0.6931471805599453f;
    return std::clamp(*middle / exponential_median_ratio, 1.0e-8f, 1.0f);
}

float estimate_channel_residual_noise(
    const std::vector<std::complex<float>>& receive_grid,
    const std::vector<std::complex<float>>& reference_grid,
    const std::vector<std::uint16_t>& pilot_indices,
    const std::vector<ChannelNxN>& channels,
    std::vector<float>& powers) {
    powers.clear();
    powers.reserve(data_symbols * pilot_indices.size() * ports);
    for (std::size_t time = 0u; time < data_symbols; ++time) {
        for (const auto fft : pilot_indices) {
            for (std::size_t rx = 0u; rx < ports; ++rx) {
                std::complex<float> predicted{};
                for (std::size_t tx = 0u; tx < ports; ++tx) {
                    predicted += channels[time * fft_size + fft].values[
                        rx * maximum_spatial_streams + tx] *
                        reference_grid[
                            (time * fft_size + fft) * ports + tx];
                }
                const auto received = receive_grid[
                    (time * fft_size + fft) * ports + rx];
                powers.push_back(std::norm(received - predicted));
            }
        }
    }
    if (powers.empty()) {
        throw std::runtime_error("Rank-4 channel-residual noise estimator has no samples");
    }
    const auto middle = powers.begin() +
        static_cast<std::ptrdiff_t>(powers.size() / 2u);
    std::nth_element(powers.begin(), middle, powers.end());
    constexpr float exponential_median_ratio = 0.6931471805599453f;
    return std::clamp(*middle / exponential_median_ratio, 1.0e-8f, 1.0f);
}

std::size_t workspace_capacity(const Rank4TimeWorkspace& workspace) {
    std::size_t total = workspace.preamble.capacity() +
        workspace.timing.metrics.capacity() + workspace.rx_grid.capacity() +
        workspace.dmrs_rx_grid.capacity() +
        workspace.ofdm_samples.capacity() +
        workspace.frequency_scratch.capacity() +
        workspace.pilot_reference_grid.capacity() +
        workspace.dmrs_reference_grid.capacity() +
        workspace.channels.capacity() +
        workspace.noise_power_samples.capacity() +
        workspace.control_llrs.capacity() + workspace.equalized.capacity() +
        workspace.variances.capacity() + workspace.frame_decode.llrs.capacity() +
        workspace.frame_decode.interleaved_block.capacity() +
        workspace.backend_control_fft_indices.capacity() +
        workspace.backend_payload_fft_indices.capacity();
    for (const auto& receive : workspace.channel_estimation.estimates) {
        for (const auto& transmit : receive) {
            total += transmit.capacity();
        }
    }
    return total;
}

}  // namespace

void Rank4TimeReceiverState::reset() noexcept {
    synchronization_state = Rank4SynchronizationMode::reacquire;
    timing_valid = false;
    predicted_timing_offset = 0u;
    last_timing_metric = 0.0f;
    synchronization_lock_age_frames = 0u;
    csi_valid = false;
    filtered_channels.clear();
    csi_age_frames = 0u;
    ++reset_count;
}

void Rank4TimeWorkspace::release() noexcept {
    *this = Rank4TimeWorkspace{};
}

void prepare_rank4_time_frame(
    std::uint16_t sequence,
    const Rank4TimeSimulationConfig& config,
    const Ldpc5041008& codec,
    PreparedRank4TimeFrame& prepared,
    Rank4TimeReceiverState* receiver_state,
    Rank4TimeWorkspace& workspace) {
    FormalFrameProfile profile;
    profile.transmit_rank = config.spatial_rank;
    profile.bits_per_symbol = modulation_bits(config.modulation);
    profile.pilot_spacing = 2u;
    const auto layout = build_formal_frame_layout(profile);
    const std::size_t payload_bytes = config.payload_bytes == 0u
        ? layout.user_payload_bytes : config.payload_bytes;
    if (payload_bytes == 0u || payload_bytes > layout.user_payload_bytes) {
        throw std::invalid_argument("Rank-4 time payload exceeds frame capacity");
    }
    std::mt19937 random(config.random_seed + sequence * 0x9E37u);
    std::uniform_int_distribution<unsigned> byte_value(0u, 255u);
    std::vector<std::uint8_t> payload(payload_bytes);
    for (auto& value : payload) {
        value = static_cast<std::uint8_t>(byte_value(random));
    }
    prepare_rank4_time_payload_frame(
        payload, sequence, config, codec, prepared, receiver_state, workspace);
}

void prepare_rank4_time_payload_frame(
    const std::vector<std::uint8_t>& payload,
    std::uint16_t sequence,
    const Rank4TimeSimulationConfig& config,
    const Ldpc5041008& codec,
    PreparedRank4TimeFrame& prepared,
    Rank4TimeReceiverState* receiver_state,
    Rank4TimeWorkspace& buffers) {
    validate(config);
    prepared.ready = false;
    const std::size_t capacity_before = workspace_capacity(buffers);
    const auto simulation_start = Clock::now();
    const LinkMode mode{
        config.spatial_rank, config.modulation,
        TransmissionScheme::spatial_multiplexing, ports};
    FormalFrameProfile profile;
    profile.transmit_rank = config.spatial_rank;
    profile.bits_per_symbol = modulation_bits(config.modulation);
    profile.pilot_spacing = 2u;
    const auto capacity = build_formal_frame_layout(profile);
    if (payload.empty() || payload.size() > capacity.user_payload_bytes) {
        throw std::invalid_argument("Rank-4 time payload exceeds frame capacity");
    }
    const auto encoded = encode_dynamic_frame(
        payload, mode, sequence, codec, config.pilot_seed);
    const std::size_t frame_symbol_count = formal_frame_symbols(config.pilot_mode);
    const std::size_t data_symbol_offset = config.pilot_mode == PilotMode::nr_dmrs
        ? 3u : 1u;
    std::mt19937 random(config.random_seed + sequence * 0x9E37u);

    TdlSpatialCorrelationNxNConfig spatial;
    spatial.streams = ports;
    spatial.transmit_correlation = config.transmit_correlation;
    spatial.receive_correlation = config.receive_correlation;
    spatial.random_seed = config.channel_seed;
    const auto evaluated_taps = evaluate_tdl_taps(
        config.taps, config.channel_time_seconds);
    const auto impulse = build_correlated_tdl_nxn(evaluated_taps, spatial);

    std::array<std::vector<std::complex<float>>, maximum_spatial_streams>
        tx_frame;
    for (std::size_t tx = 0u; tx < ports; ++tx) {
        tx_frame[tx].assign(frame_symbol_count * symbol_samples, {});
    }
    if (buffers.preamble.size() != symbol_samples) {
        buffers.preamble = generate_zc_ofdm_symbol(
            fft_size, cp_length, 29u);
    }
    std::copy(
        buffers.preamble.begin(), buffers.preamble.end(),
        tx_frame[0].begin());
    if (config.pilot_mode == PilotMode::nr_dmrs) {
        build_nr_dmrs_reference_grid(
            fft_size, encoded.layout.active_fft_indices, ports,
            config.pilot_seed, buffers.dmrs_reference_grid);
        for (std::size_t time = 0u; time < nr_dmrs_symbols; ++time) {
            for (std::size_t tx = 0u; tx < ports; ++tx) {
                std::vector<std::complex<float>> frequency(fft_size);
                for (std::size_t fft = 0u; fft < fft_size; ++fft) {
                    frequency[fft] = buffers.dmrs_reference_grid[
                        (time * fft_size + fft) * ports + tx];
                }
                const auto samples = ofdm_modulate(frequency, cp_length);
                std::copy(
                    samples.begin(), samples.end(),
                    tx_frame[tx].begin() + static_cast<std::ptrdiff_t>(
                        (time + 1u) * symbol_samples));
            }
        }
    }
    for (std::size_t time = 0u; time < data_symbols; ++time) {
        for (std::size_t tx = 0u; tx < ports; ++tx) {
            std::vector<std::complex<float>> frequency(fft_size);
            for (std::size_t fft = 0u; fft < fft_size; ++fft) {
                frequency[fft] = encoded.tx_grid[
                    (time * fft_size + fft) * ports + tx];
            }
            const auto samples = ofdm_modulate(frequency, cp_length);
            std::copy(
                samples.begin(), samples.end(),
                tx_frame[tx].begin() + static_cast<std::ptrdiff_t>(
                    (time + data_symbol_offset) * symbol_samples));
        }
    }

    std::array<std::vector<std::complex<float>>, maximum_spatial_streams>
        clean_rx;
    for (std::size_t rx = 0u; rx < ports; ++rx) {
        clean_rx[rx].assign(frame_symbol_count * symbol_samples, {});
    }
    std::array<std::vector<std::complex<float>>, maximum_spatial_streams>
        tx_symbol;
    std::array<std::vector<std::complex<float>>, maximum_spatial_streams>
        rx_symbol;
    for (std::size_t symbol = 0u; symbol < frame_symbol_count; ++symbol) {
        for (std::size_t tx = 0u; tx < ports; ++tx) {
            const auto begin = tx_frame[tx].begin() +
                static_cast<std::ptrdiff_t>(symbol * symbol_samples);
            tx_symbol[tx].assign(
                begin, begin + static_cast<std::ptrdiff_t>(symbol_samples));
        }
        apply_tdl_nxn_symbol(tx_symbol, impulse, rx_symbol);
        for (std::size_t rx = 0u; rx < ports; ++rx) {
            std::copy(
                rx_symbol[rx].begin(), rx_symbol[rx].end(),
                clean_rx[rx].begin() +
                    static_cast<std::ptrdiff_t>(symbol * symbol_samples));
        }
    }

    const float true_noise_variance = std::pow(10.0f, -config.snr_db / 10.0f);
    std::normal_distribution<float> noise(
        0.0f, std::sqrt(true_noise_variance * 0.5f));
    constexpr std::size_t tail_samples = 32u;
    const std::size_t stream_samples = config.timing_offset_samples +
        frame_symbol_count * symbol_samples + tail_samples;
    std::vector<std::vector<std::complex<float>>> receive_streams(
        ports, std::vector<std::complex<float>>(stream_samples));
    for (auto& branch : receive_streams) {
        for (auto& sample : branch) {
            sample = {noise(random), noise(random)};
        }
    }
    for (std::size_t rx = 0u; rx < ports; ++rx) {
        for (std::size_t sample = 0u; sample < clean_rx[rx].size(); ++sample) {
            receive_streams[rx][config.timing_offset_samples + sample] +=
                clean_rx[rx][sample];
        }
    }
    apply_cfo_normalized_inplace(
        receive_streams, config.cfo_hz / subcarrier_spacing_hz, fft_size);
    auto received = resample_sfo_cubic(receive_streams, config.sfo_ppm);

    const auto receiver_start = Clock::now();
    Rank4TimeSimulationResult result;
    result.pilot_mode = config.pilot_mode;
    result.spatial_rank = config.spatial_rank;
    result.frame_symbols = frame_symbol_count;
    result.sequence = sequence;
    result.payload_bytes = payload.size();
    if (config.enable_sensing_snapshot) {
        const std::size_t waveform_points = std::min(
            config.diagnostic_waveform_points, received[0].size());
        result.receive_waveform_rx0.assign(
            received[0].begin(),
            received[0].begin() +
                static_cast<std::ptrdiff_t>(waveform_points));
    }
    const std::size_t maximum_search = std::min(
        config.timing_offset_samples + tail_samples,
        received[0].size() - frame_symbol_count * symbol_samples);
    auto& timing = buffers.timing;
    const bool can_track = receiver_state != nullptr &&
        receiver_state->timing_valid &&
        receiver_state->synchronization_state ==
            Rank4SynchronizationMode::track;
    if (can_track) {
        const std::size_t predicted = std::min(
            receiver_state->predicted_timing_offset, maximum_search);
        const std::size_t begin = predicted > config.tracking_half_window_samples
            ? predicted - config.tracking_half_window_samples : 0u;
        const std::size_t end = std::min(
            maximum_search,
            predicted + config.tracking_half_window_samples);
        estimate_zc_timing_window(
            received, buffers.preamble, begin, end, timing);
        result.synchronization_mode_used = Rank4SynchronizationMode::track;
        ++receiver_state->tracking_search_count;
        const float required = std::max(
            config.tracking_min_metric,
            receiver_state->last_timing_metric * config.tracking_metric_ratio);
        if (timing.peak_metric < required) {
            estimate_zc_timing(
                received, buffers.preamble, maximum_search, timing);
            result.synchronization_mode_used =
                Rank4SynchronizationMode::reacquire;
            result.tracking_fallback = true;
            ++receiver_state->full_search_count;
            ++receiver_state->reacquisition_count;
        }
    } else {
        result.synchronization_mode_used = receiver_state != nullptr &&
            receiver_state->synchronization_state ==
                Rank4SynchronizationMode::reacquire
            ? Rank4SynchronizationMode::reacquire
            : Rank4SynchronizationMode::search;
        estimate_zc_timing(
            received, buffers.preamble, maximum_search, timing);
        if (receiver_state != nullptr) {
            ++receiver_state->full_search_count;
        }
    }
    result.timing_offset = timing.offset;
    result.timing_metric = timing.peak_metric;
    result.timing_candidates_evaluated = timing.metrics.size();
    result.timing_ok = timing.offset >= config.timing_offset_samples -
            std::min(config.timing_offset_samples, std::size_t{1u}) &&
        timing.offset <= config.timing_offset_samples + 1u;
    const float normalized_cfo = estimate_cp_cfo_normalized(
        received, timing.offset, fft_size, cp_length, frame_symbol_count,
        maximum_delay(config.taps));
    result.estimated_cfo_hz = normalized_cfo * subcarrier_spacing_hz;
    result.cfo_error_hz = result.estimated_cfo_hz - config.cfo_hz;
    apply_cfo_normalized_inplace(received, -normalized_cfo, fft_size);
    const auto synchronization_done = Clock::now();

    if (!buffers.pilot_reference_valid ||
        buffers.pilot_reference_seed != config.pilot_seed ||
        buffers.pilot_reference_modulation != config.modulation) {
        build_dynamic_pilot_reference_grid(
            config.pilot_seed, mode, buffers.pilot_reference_grid);
        buffers.pilot_reference_seed = config.pilot_seed;
        buffers.pilot_reference_modulation = config.modulation;
        buffers.pilot_reference_valid = true;
    }
    FdmMimoFrameFrontend device_fdm_frontend;
    bool device_fdm_prepared = false;

    buffers.rx_grid.resize(data_symbols * fft_size * ports);
    if (config.pilot_mode == PilotMode::nr_dmrs) {
        buffers.dmrs_rx_grid.resize(nr_dmrs_symbols * fft_size * ports);
    } else {
        buffers.dmrs_rx_grid.clear();
    }
    buffers.ofdm_samples.resize(symbol_samples);
    buffers.frequency_scratch.resize(fft_size);
    if (config.compute_backend != nullptr) {
        const std::size_t batch_symbols =
            (data_symbols + (config.pilot_mode == PilotMode::nr_dmrs
                ? nr_dmrs_symbols : 0u)) * ports;
        buffers.backend_time_batch.resize(batch_symbols * symbol_samples);
        std::size_t batch = 0u;
        auto pack_symbol = [&](std::size_t frame_symbol, std::size_t rx) {
            const auto begin = received[rx].begin() +
                static_cast<std::ptrdiff_t>(
                    timing.offset + frame_symbol * symbol_samples);
            std::copy(
                begin, begin + static_cast<std::ptrdiff_t>(symbol_samples),
                buffers.backend_time_batch.begin() +
                    static_cast<std::ptrdiff_t>(batch * symbol_samples));
            ++batch;
        };
        if (config.pilot_mode == PilotMode::nr_dmrs) {
            for (std::size_t time = 0u; time < nr_dmrs_symbols; ++time) {
                for (std::size_t rx = 0u; rx < ports; ++rx) {
                    pack_symbol(time + 1u, rx);
                }
            }
        }
        for (std::size_t time = 0u; time < data_symbols; ++time) {
            for (std::size_t rx = 0u; rx < ports; ++rx) {
                pack_symbol(time + data_symbol_offset, rx);
            }
        }
        if (config.pilot_mode == PilotMode::fdm &&
            !config.enable_truth_diagnostics &&
            !config.enable_sensing_snapshot) {
            buffers.backend_control_fft_indices.resize(
                encoded.layout.control_data_positions.size());
            for (std::size_t index = 0u;
                 index < encoded.layout.control_data_positions.size(); ++index) {
                buffers.backend_control_fft_indices[index] =
                    encoded.layout.data_fft_indices[
                        encoded.layout.control_data_positions[index]];
            }
            buffers.backend_payload_fft_indices.resize(
                encoded.layout.payload_data_positions.size());
            for (std::size_t index = 0u;
                 index < encoded.layout.payload_data_positions.size(); ++index) {
                buffers.backend_payload_fft_indices[index] =
                    encoded.layout.data_fft_indices[
                        encoded.layout.payload_data_positions[index]];
            }
            FdmMimoFrameRequest request;
            request.fft_size = fft_size;
            request.cp_length = cp_length;
            request.samples_per_symbol = symbol_samples;
            request.ports = ports;
            request.spatial_rank = config.spatial_rank;
            request.time_with_cp = &buffers.backend_time_batch;
            request.phase_reference_fft_indices =
                &encoded.layout.phase_reference_fft_indices;
            request.pilot_fft_indices = &encoded.layout.pilot_fft_indices;
            request.pilot_reference_grid = &buffers.pilot_reference_grid;
            request.control_fft_indices =
                &buffers.backend_control_fft_indices;
            request.payload_time_indices =
                &encoded.layout.payload_time_indices;
            request.payload_fft_indices =
                &buffers.backend_payload_fft_indices;
            request.average_intra_frame_csi = config.average_intra_frame_csi;
            request.reuse_csi_history = receiver_state != nullptr &&
                receiver_state->csi_valid &&
                receiver_state->filtered_channels.empty();
            request.csi_smoothing_alpha = config.csi_smoothing_alpha;
            device_fdm_prepared =
                config.compute_backend->prepare_fdm_mimo_frame(
                    request, device_fdm_frontend);
        }
        if (!device_fdm_prepared) {
            config.compute_backend->ofdm_demodulate_batch(
                fft_size, cp_length, batch_symbols,
                buffers.backend_time_batch, buffers.backend_frequency_batch);
        }
        batch = 0u;
        auto unpack_symbol = [&](
            std::vector<std::complex<float>>& grid,
            std::size_t time,
            std::size_t rx) {
            for (std::size_t fft = 0u; fft < fft_size; ++fft) {
                grid[(time * fft_size + fft) * ports + rx] =
                    buffers.backend_frequency_batch[batch * fft_size + fft];
            }
            ++batch;
        };
        if (!device_fdm_prepared && config.pilot_mode == PilotMode::nr_dmrs) {
            for (std::size_t time = 0u; time < nr_dmrs_symbols; ++time) {
                for (std::size_t rx = 0u; rx < ports; ++rx) {
                    unpack_symbol(buffers.dmrs_rx_grid, time, rx);
                }
            }
        }
        if (!device_fdm_prepared) {
            for (std::size_t time = 0u; time < data_symbols; ++time) {
                for (std::size_t rx = 0u; rx < ports; ++rx) {
                    unpack_symbol(buffers.rx_grid, time, rx);
                }
            }
        }
    } else {
        if (config.pilot_mode == PilotMode::nr_dmrs) {
            for (std::size_t time = 0u; time < nr_dmrs_symbols; ++time) {
                for (std::size_t rx = 0u; rx < ports; ++rx) {
                    const auto begin = received[rx].begin() +
                        static_cast<std::ptrdiff_t>(
                            timing.offset + (time + 1u) * symbol_samples);
                    std::copy(
                        begin, begin + static_cast<std::ptrdiff_t>(symbol_samples),
                        buffers.ofdm_samples.begin());
                    ofdm_demodulate(
                        buffers.ofdm_samples, fft_size, cp_length,
                        buffers.frequency_scratch);
                    for (std::size_t fft = 0u; fft < fft_size; ++fft) {
                        buffers.dmrs_rx_grid[
                            (time * fft_size + fft) * ports + rx] =
                            buffers.frequency_scratch[fft];
                    }
                }
            }
        }
        for (std::size_t time = 0u; time < data_symbols; ++time) {
            for (std::size_t rx = 0u; rx < ports; ++rx) {
                const auto begin = received[rx].begin() +
                    static_cast<std::ptrdiff_t>(
                        timing.offset +
                        (time + data_symbol_offset) * symbol_samples);
                std::copy(
                    begin, begin + static_cast<std::ptrdiff_t>(symbol_samples),
                    buffers.ofdm_samples.begin());
                ofdm_demodulate(
                    buffers.ofdm_samples, fft_size, cp_length,
                    buffers.frequency_scratch);
                for (std::size_t fft = 0u; fft < fft_size; ++fft) {
                    buffers.rx_grid[(time * fft_size + fft) * ports + rx] =
                        buffers.frequency_scratch[fft];
                }
            }
        }
    }

    if (device_fdm_prepared) {
        result.estimated_sfo_ppm = device_fdm_frontend.estimated_sfo_ppm;
        result.residual_sfo_ppm = device_fdm_frontend.residual_sfo_ppm;
    } else {
        const auto sfo = estimate_sfo_phase_slope(
            buffers.rx_grid, encoded.layout.phase_reference_fft_indices,
            fft_size, ports, symbol_samples);
        result.estimated_sfo_ppm = sfo.sfo_ppm;
        if (config.pilot_mode == PilotMode::nr_dmrs) {
            correct_symbol_phase_inplace(
                buffers.dmrs_rx_grid, sfo, fft_size, ports, 1u, 1.0f);
            correct_symbol_phase_inplace(
                buffers.rx_grid, sfo, fft_size, ports, 0u, 2.0f);
            correct_symbol_phase_inplace(
                buffers.rx_grid, sfo, fft_size, ports, 1u, 3.0f);
        } else {
            correct_second_symbol_phase_inplace(
                buffers.rx_grid, sfo, fft_size, ports);
        }
        result.residual_sfo_ppm = estimate_sfo_phase_slope(
            buffers.rx_grid, encoded.layout.phase_reference_fft_indices,
            fft_size, ports, symbol_samples).sfo_ppm;
    }
    const auto fft_sfo_done = Clock::now();

    auto channel_estimation_done = Clock::now();
    const std::vector<ChannelNxN>* channel_view = &buffers.channels;
    if (device_fdm_prepared) {
        result.noise_variance = device_fdm_frontend.noise_variance;
        buffers.control_llrs = std::move(device_fdm_frontend.control_llrs);
        channel_estimation_done = Clock::now();
        if (receiver_state != nullptr) {
            receiver_state->csi_valid = true;
            receiver_state->filtered_channels.clear();
            ++receiver_state->csi_age_frames;
        }
    } else {
    if (config.pilot_mode == PilotMode::nr_dmrs &&
        (!buffers.dmrs_reference_valid ||
         buffers.dmrs_reference_seed != config.pilot_seed)) {
        build_nr_dmrs_reference_grid(
            fft_size, encoded.layout.active_fft_indices, ports,
            config.pilot_seed, buffers.dmrs_reference_grid);
        buffers.dmrs_reference_seed = config.pilot_seed;
        buffers.dmrs_reference_valid = true;
    }
    if (config.pilot_mode == PilotMode::nr_dmrs) {
        estimate_nr_dmrs_channel_linear_nxn(
            buffers.dmrs_rx_grid, buffers.dmrs_reference_grid,
            encoded.layout.active_fft_indices, data_symbols, fft_size, ports,
            buffers.channels, buffers.channel_estimation);
        for (std::size_t time = 0u; time < data_symbols; ++time) {
            const auto phase_tracking =
                estimate_reference_residual_phase_slope_nxn(
                    buffers.rx_grid, buffers.pilot_reference_grid,
                    encoded.layout.pilot_fft_indices, buffers.channels,
                    time, fft_size, ports);
            correct_symbol_phase_inplace(
                buffers.rx_grid, phase_tracking, fft_size, ports, time, 1.0f);
        }
        // The two data symbols retain the same sparse phase/pilot references.
        // Their differential residual cancels the channel and therefore does
        // not misinterpret DM-RS interpolation error as thermal noise.
        result.noise_variance = estimate_pilot_noise(
            buffers.rx_grid, buffers.pilot_reference_grid,
            encoded.layout.pilot_fft_indices,
            buffers.noise_power_samples);
    } else {
        result.noise_variance = estimate_pilot_noise(
            buffers.rx_grid, buffers.pilot_reference_grid,
            encoded.layout.pilot_fft_indices, buffers.noise_power_samples);
        estimate_fdm_pilot_channel_linear_nxn(
            buffers.rx_grid, buffers.pilot_reference_grid,
            encoded.layout.pilot_fft_indices, data_symbols, fft_size, ports,
            buffers.channels, buffers.channel_estimation);
    }
    if (config.average_intra_frame_csi) {
        for (std::size_t fft = 0u; fft < fft_size; ++fft) {
            for (std::size_t rx = 0u; rx < ports; ++rx) {
                for (std::size_t tx = 0u; tx < ports; ++tx) {
                    const std::size_t component =
                        rx * maximum_spatial_streams + tx;
                    const auto average = 0.5f * (
                        buffers.channels[fft].values[component] +
                        buffers.channels[fft_size + fft].values[component]);
                    buffers.channels[fft].values[component] = average;
                    buffers.channels[fft_size + fft].values[component] = average;
                }
            }
        }
    }
    channel_estimation_done = Clock::now();
    if (receiver_state != nullptr) {
        if (receiver_state->csi_valid &&
            receiver_state->filtered_channels.size() == buffers.channels.size()) {
            const float alpha = config.csi_smoothing_alpha;
            const float history = 1.0f - alpha;
            for (std::size_t index = 0u;
                 index < buffers.channels.size(); ++index) {
                receiver_state->filtered_channels[index].streams = ports;
                for (std::size_t rx = 0u; rx < ports; ++rx) {
                    for (std::size_t tx = 0u; tx < ports; ++tx) {
                        const std::size_t component =
                            rx * maximum_spatial_streams + tx;
                        receiver_state->filtered_channels[index].values[component] =
                            alpha * buffers.channels[index].values[component] +
                            history * receiver_state->filtered_channels[index]
                                .values[component];
                    }
                }
            }
        } else {
            receiver_state->filtered_channels = buffers.channels;
        }
        receiver_state->csi_valid = true;
        ++receiver_state->csi_age_frames;
        channel_view = &receiver_state->filtered_channels;
    }
    const auto& used_channels = *channel_view;

    if (config.enable_sensing_snapshot) {
        const std::size_t links = ports * ports;
        result.sensing_channel_frequency_response.assign(
            links * fft_size, std::complex<float>{});
        for (std::size_t rx = 0u; rx < ports; ++rx) {
            for (std::size_t tx = 0u; tx < ports; ++tx) {
                const std::size_t component =
                    rx * maximum_spatial_streams + tx;
                const std::size_t link = rx * ports + tx;
                for (std::size_t fft = 0u; fft < fft_size; ++fft) {
                    result.sensing_channel_frequency_response[
                        link * fft_size + fft] = 0.5f * (
                            buffers.channels[fft].values[component] +
                            buffers.channels[fft_size + fft].values[component]);
                }
            }
        }
        result.sensing_active_subcarrier_mask.assign(fft_size, 0u);
        for (const auto fft : encoded.layout.data_fft_indices) {
            result.sensing_active_subcarrier_mask[fft] = 1u;
        }
        for (const auto fft : encoded.layout.pilot_fft_indices) {
            result.sensing_active_subcarrier_mask[fft] = 1u;
        }
        for (const auto fft : encoded.layout.phase_reference_fft_indices) {
            result.sensing_active_subcarrier_mask[fft] = 1u;
        }
    }

    buffers.control_llrs.clear();
    buffers.control_llrs.reserve(encoded.control_labels.size() * 2u);
    for (const auto data : encoded.layout.control_data_positions) {
        const std::size_t fft = encoded.layout.data_fft_indices[data];
        std::array<std::complex<float>, maximum_spatial_streams> samples{};
        for (std::size_t rx = 0u; rx < ports; ++rx) {
            samples[rx] = buffers.rx_grid[fft * ports + rx];
        }
        float variance = 0.0f;
        const auto symbol = mrc_detect_tx0(
            samples, used_channels[fft], result.noise_variance, variance);
        const auto llrs = SquareQAM::max_log_llrs(symbol, variance, 2u);
        buffers.control_llrs.push_back(llrs[0]);
        buffers.control_llrs.push_back(llrs[1]);
    }
    }
    const auto& used_channels = *channel_view;
    MiniHeader decoded_header;
    float marker_metric = 0.0f;
    try {
        decoded_header = decode_control_qpsk_llrs(
            buffers.control_llrs, &marker_metric);
        result.header_ok =
            transmit_rank_from_flags(decoded_header.flags) ==
                config.spatial_rank &&
            bits_per_symbol_from_flags(decoded_header.flags) ==
                encoded.profile.bits_per_symbol &&
            decoded_header.sequence == sequence &&
            decoded_header.payload_len == encoded.header.payload_len;
    } catch (const std::exception&) {
        result.header_ok = false;
    }

    const bool retain_equalized = config.compute_backend == nullptr ||
        config.enable_truth_diagnostics || config.enable_sensing_snapshot;
    if (retain_equalized) {
        buffers.equalized.resize(encoded.layout.payload_layer_symbols);
        buffers.variances.resize(encoded.layout.payload_layer_symbols);
    } else {
        buffers.equalized.clear();
        buffers.variances.clear();
    }
    const std::size_t payload_resources =
        encoded.layout.payload_time_indices.size();
    if (config.compute_backend != nullptr) {
        if (device_fdm_prepared) {
            config.compute_backend->detect_prepared_fdm_mimo(
                encoded.profile.bits_per_symbol,
                decoded_header.payload_blocks, result.noise_variance,
                config.mmse_regularization_scale, retain_equalized,
                buffers.backend_detected_batch, buffers.backend_mse_batch,
                buffers.frame_decode.llrs);
        } else {
            buffers.backend_received_batch.resize(payload_resources * ports);
            buffers.backend_channel_batch.resize(
                payload_resources * ports * config.spatial_rank);
            for (std::size_t payload_index = 0u;
                 payload_index < payload_resources; ++payload_index) {
                const std::size_t time =
                    encoded.layout.payload_time_indices[payload_index];
                const std::size_t data =
                    encoded.layout.payload_data_positions[payload_index];
                const std::size_t fft = encoded.layout.data_fft_indices[data];
                for (std::size_t rx = 0u; rx < ports; ++rx) {
                    buffers.backend_received_batch[payload_index * ports + rx] =
                        buffers.rx_grid[(time * fft_size + fft) * ports + rx];
                }
                const auto& physical_channel =
                    used_channels[time * fft_size + fft];
                const ChannelNxN effective_channel = config.spatial_rank == 2u
                    ? apply_fixed_dft_precoder_4x2(physical_channel)
                    : physical_channel;
                const std::size_t channel_offset =
                    payload_index * ports * config.spatial_rank;
                for (std::size_t rx = 0u; rx < ports; ++rx) {
                    for (std::size_t layer = 0u;
                         layer < config.spatial_rank; ++layer) {
                        buffers.backend_channel_batch[
                            channel_offset + rx * config.spatial_rank + layer] =
                            effective_channel.values[
                                rx * maximum_spatial_streams + layer];
                    }
                }
            }
            config.compute_backend->detect_mimo_batch(
                config.spatial_rank, ports, payload_resources,
                buffers.backend_received_batch, buffers.backend_channel_batch,
                result.noise_variance * config.mmse_regularization_scale,
                LinearDetector::mmse, encoded.profile.bits_per_symbol,
                decoded_header.payload_blocks,
                retain_equalized,
                buffers.backend_detected_batch, buffers.backend_mse_batch,
                buffers.frame_decode.llrs);
        }
    }
    for (std::size_t payload_index = 0u;
         payload_index < payload_resources;
         ++payload_index) {
        if (config.compute_backend != nullptr) {
            if (retain_equalized) {
                for (std::size_t layer = 0u;
                     layer < config.spatial_rank; ++layer) {
                    const std::size_t index =
                        payload_index * config.spatial_rank + layer;
                    buffers.equalized[index] =
                        buffers.backend_detected_batch[index];
                    buffers.variances[index] = std::max(
                        buffers.backend_mse_batch[index], 1.0e-12f);
                }
            }
        } else {
            const std::size_t time =
                encoded.layout.payload_time_indices[payload_index];
            const std::size_t data =
                encoded.layout.payload_data_positions[payload_index];
            const std::size_t fft = encoded.layout.data_fft_indices[data];
            std::array<std::complex<float>, maximum_spatial_streams> samples{};
            for (std::size_t rx = 0u; rx < ports; ++rx) {
                samples[rx] = buffers.rx_grid[
                    (time * fft_size + fft) * ports + rx];
            }
            const auto& physical_channel =
                used_channels[time * fft_size + fft];
            const ChannelNxN effective_channel = config.spatial_rank == 2u
                ? apply_fixed_dft_precoder_4x2(physical_channel)
                : physical_channel;
            const auto detected = detect_nxn(
                samples, effective_channel,
                result.noise_variance * config.mmse_regularization_scale,
                LinearDetector::mmse);
            for (std::size_t layer = 0u;
                 layer < config.spatial_rank; ++layer) {
                const std::size_t index =
                    payload_index * config.spatial_rank + layer;
                buffers.equalized[index] = detected.symbols[layer];
                buffers.variances[index] = std::max(
                    detected.predicted_mse[layer], 1.0e-12f);
            }
        }
    }
    const auto detection_done = Clock::now();
    if (result.header_ok) {
        try {
            if (config.compute_backend != nullptr) {
                prepare_dynamic_frame_decoder_llrs(
                    decoded_header, marker_metric, mode,
                    buffers.frame_decode);
            } else {
                prepare_dynamic_frame_payload_llrs(
                    decoded_header, marker_metric, mode, buffers.equalized,
                    buffers.variances, buffers.frame_decode);
            }
        } catch (const std::exception&) {
            result.header_ok = false;
        }
    }
    const auto soft_demapping_done = Clock::now();
    // Tracking is a synchronization decision. It must be committed before the
    // background FEC of this frame completes so the next frame can use the
    // narrow timing window. CRC remains the data-delivery decision.
    const bool front_lock_ok = result.timing_ok && result.header_ok;
    if (receiver_state != nullptr) {
        if (front_lock_ok) {
            receiver_state->synchronization_state =
                Rank4SynchronizationMode::track;
            receiver_state->timing_valid = true;
            receiver_state->predicted_timing_offset = timing.offset;
            receiver_state->last_timing_metric = timing.peak_metric;
            ++receiver_state->synchronization_lock_age_frames;
        } else {
            receiver_state->reset();
        }
        result.synchronization_lock_age_frames =
            receiver_state->synchronization_lock_age_frames;
    } else {
        result.synchronization_lock_age_frames = front_lock_ok ? 1u : 0u;
    }
    result.synchronization_us = elapsed_us(
        receiver_start, synchronization_done);
    result.fft_sfo_us = elapsed_us(
        synchronization_done, fft_sfo_done);
    result.channel_estimation_us = elapsed_us(
        fft_sfo_done, channel_estimation_done);
    result.detection_us = elapsed_us(
        channel_estimation_done, detection_done);
    result.soft_demapping_us = elapsed_us(
        detection_done, soft_demapping_done);
    if (config.compute_backend != nullptr) {
        const auto backend_timing = config.compute_backend->timing();
        result.backend_ofdm_h2d_us = backend_timing.ofdm.h2d_us;
        result.backend_ofdm_kernel_us = backend_timing.ofdm.kernel_us;
        result.backend_ofdm_d2h_us = backend_timing.ofdm.d2h_us;
        result.backend_mimo_h2d_us = backend_timing.mimo.h2d_us;
        result.backend_mimo_kernel_us = backend_timing.mimo.kernel_us;
        result.backend_mimo_d2h_us = backend_timing.mimo.d2h_us;
    }
    result.ldpc_crc_us = 0.0;
    result.receiver_us = elapsed_us(receiver_start, soft_demapping_done);
    ++buffers.frames_processed;
    result.workspace_growths_this_frame =
        workspace_capacity(buffers) > capacity_before ? 1u : 0u;

    // Simulator truth metrics are deliberately outside receiver_us. Ordinary
    // GPU video frames omit them so only decoded LLRs cross the PCIe boundary.
    const bool diagnostics_enabled = config.enable_truth_diagnostics ||
        config.enable_sensing_snapshot;
    if (diagnostics_enabled) {
        result.transmitted_symbols = encoded.payload_symbols;
        result.equalized_symbols = buffers.equalized;
        double error_power = 0.0;
        double reference_power = 0.0;
        for (std::size_t index = 0u; index < buffers.equalized.size(); ++index) {
            error_power += std::norm(
                buffers.equalized[index] - encoded.payload_symbols[index]);
            reference_power += std::norm(encoded.payload_symbols[index]);
        }
        const unsigned payload_bits = encoded.profile.bits_per_symbol;
        const std::size_t coded_symbols =
            encoded.transmitted_bits.size() / payload_bits;
        for (std::size_t symbol = 0u; symbol < coded_symbols; ++symbol) {
            const unsigned recovered = SquareQAM::demodulate(
                buffers.equalized[symbol], payload_bits);
            result.pre_fec_bit_errors += count_bit_errors(
                encoded.payload_labels[symbol], recovered, payload_bits);
        }
        result.pre_fec_compared_bits = encoded.transmitted_bits.size();
        result.pre_fec_ber = result.pre_fec_compared_bits == 0u ? 0.0f :
            static_cast<float>(result.pre_fec_bit_errors) /
            static_cast<float>(result.pre_fec_compared_bits);
        result.evm_percent = reference_power > 0.0
            ? 100.0f * std::sqrt(
                static_cast<float>(error_power / reference_power))
            : 0.0f;
    }

    double channel_error = 0.0;
    double channel_reference = 0.0;
    if (diagnostics_enabled && front_lock_ok) {
        for (std::size_t time = 0u; time < data_symbols; ++time) {
            for (const auto fft : encoded.layout.data_fft_indices) {
                const auto truth = tdl_frequency_response_nxn(
                    impulse, fft, fft_size);
                const auto& estimate = used_channels[time * fft_size + fft];
                for (std::size_t rx = 0u; rx < ports; ++rx) {
                    for (std::size_t tx = 0u; tx < ports; ++tx) {
                        const std::size_t component =
                            rx * maximum_spatial_streams + tx;
                        channel_error += std::norm(
                            estimate.values[component] - truth.values[component]);
                        channel_reference += std::norm(truth.values[component]);
                    }
                }
            }
        }
        result.channel_nmse_db = ratio_db(
            channel_error, channel_reference);
    }
    result.simulation_us = elapsed_us(simulation_start, Clock::now());
    prepared.result = std::move(result);
    prepared.expected_payload = payload;
    prepared.maximum_ldpc_iterations = config.maximum_ldpc_iterations;
    prepared.ready = true;
}

Rank4TimeSimulationResult finish_rank4_time_frame(
    PreparedRank4TimeFrame& prepared,
    const Ldpc5041008& codec,
    Rank4TimeWorkspace& workspace,
    LdpcFrameDecoder* ldpc_frame_decoder) {
    if (!prepared.ready) {
        throw std::logic_error("Rank-4 time frame was not prepared");
    }
    auto result = std::move(prepared.result);
    const auto fec_start = Clock::now();
    if (result.header_ok) {
        try {
            const auto decoded = decode_prepared_dynamic_frame(
                workspace.frame_decode, codec,
                prepared.maximum_ldpc_iterations, 0.8f,
                ldpc_frame_decoder);
            result.syndrome_failures = decoded.syndrome_failures;
            result.crc_ok = decoded.crc_ok;
            result.payload_match =
                decoded.user_payload == prepared.expected_payload;
            if (result.crc_ok) {
                result.user_payload = decoded.user_payload;
            } else {
                result.user_payload.clear();
            }
            result.ldpc_worker_threads = decoded.ldpc_worker_threads;
            result.maximum_ldpc_iterations_used =
                decoded.maximum_decoder_iterations;
            result.ldpc_capacity_growths_this_frame =
                decoded.ldpc_capacity_growths_this_frame;
        } catch (const std::exception&) {
            result.crc_ok = false;
            result.payload_match = false;
            result.user_payload.clear();
        }
    }
    const auto fec_done = Clock::now();
    result.ldpc_crc_us = elapsed_us(fec_start, fec_done);
    result.receiver_us += result.ldpc_crc_us;
    result.simulation_us += result.ldpc_crc_us;
    prepared.ready = false;
    return result;
}

Rank4TimeSimulationResult simulate_rank4_time_frame(
    std::uint16_t sequence,
    const Rank4TimeSimulationConfig& config,
    const Ldpc5041008& codec,
    Rank4TimeReceiverState* receiver_state,
    Rank4TimeWorkspace* workspace,
    LdpcFrameDecoder* ldpc_frame_decoder) {
    Rank4TimeWorkspace local_workspace;
    auto& buffers = workspace == nullptr ? local_workspace : *workspace;
    PreparedRank4TimeFrame prepared;
    prepare_rank4_time_frame(
        sequence, config, codec, prepared, receiver_state, buffers);
    return finish_rank4_time_frame(
        prepared, codec, buffers, ldpc_frame_decoder);
}

Rank4TimeSimulationResult simulate_rank4_time_payload_frame(
    const std::vector<std::uint8_t>& payload,
    std::uint16_t sequence,
    const Rank4TimeSimulationConfig& config,
    const Ldpc5041008& codec,
    Rank4TimeReceiverState* receiver_state,
    Rank4TimeWorkspace* workspace,
    LdpcFrameDecoder* ldpc_frame_decoder) {
    Rank4TimeWorkspace local_workspace;
    auto& buffers = workspace == nullptr ? local_workspace : *workspace;
    PreparedRank4TimeFrame prepared;
    prepare_rank4_time_payload_frame(
        payload, sequence, config, codec, prepared, receiver_state, buffers);
    return finish_rank4_time_frame(
        prepared, codec, buffers, ldpc_frame_decoder);
}

}  // namespace openisac
