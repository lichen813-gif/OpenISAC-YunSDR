#pragma once

#include "openisac/channel_estimation.hpp"
#include "openisac/compute_backend.hpp"
#include "openisac/dynamic_frame.hpp"
#include "openisac/link_adaptation.hpp"
#include "openisac/preamble_sync.hpp"
#include "openisac/tdl_channel.hpp"

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace openisac {

enum class SynchronizationMode : std::uint8_t {
    search,
    track,
    reacquire,
};

struct DynamicLinkSimulationConfig {
    PilotMode pilot_mode = PilotMode::fdm;
    float snr_db = 40.0f;
    std::size_t timing_offset_samples = 20u;
    float cfo_hz = 300.0f;
    float sfo_ppm = 20.0f;
    // Absolute simulator time used to rotate time-varying TDL paths.
    double channel_time_seconds = 0.0;
    // Optional Kronecker-correlated spatial model. The legacy deterministic
    // phase model remains available for historical regression vectors.
    bool enable_correlated_spatial_tdl = false;
    float transmit_spatial_correlation = 0.0f;
    float receive_spatial_correlation = 0.0f;
    std::uint32_t spatial_channel_seed = 0xC057u;
    unsigned random_seed = 0xC057u;
    std::uint32_t pilot_seed = 0xC057u;
    // 1.0 disables smoothing; lower values retain more previous-frame CSI.
    float csi_smoothing_alpha = 1.0f;
    // Simulator-only NMSE/EVM truth calculations. Disable for receiver
    // throughput measurements or hardware-facing operation.
    bool enable_truth_diagnostics = true;
    bool enable_continuous_tracking = true;
    std::size_t tracking_half_window_samples = 2u;
    // A tracking peak below both thresholds triggers full-search fallback.
    float tracking_min_metric = 0.05f;
    float tracking_metric_ratio = 0.5f;
    std::vector<TdlTap> taps{
        {0u, 0.0f, 0.0f},
        {3u, -4.0f, 45.0f},
        {9u, -8.0f, -80.0f},
    };
    PhyComputeBackend* compute_backend = nullptr;
};

enum class NoiseVarianceMode : std::uint8_t {
    fixed,
    pilot_residual,
};

// Hardware-facing receiver parameters. These values describe receiver policy
// and capture-buffer bounds; they do not contain simulated channel truth.
struct DynamicLinkReceiverConfig {
    PilotMode pilot_mode = PilotMode::fdm;
    NoiseVarianceMode noise_variance_mode = NoiseVarianceMode::pilot_residual;
    float fixed_noise_variance = 1.0e-4f;
    float minimum_noise_variance = 1.0e-8f;
    float maximum_noise_variance = 1.0f;
    // 1.0 disables temporal smoothing of the pilot-residual estimate.
    float noise_smoothing_alpha = 0.25f;
    // Largest preamble offset examined in a capture buffer.
    std::size_t maximum_timing_offset_samples = 64u;
    // Engineering channel-memory bound used by CP-based CFO estimation.
    std::size_t maximum_channel_delay_samples = 16u;
    // 1.0 disables smoothing; lower values retain more previous-frame CSI.
    float csi_smoothing_alpha = 1.0f;
    bool enable_continuous_tracking = true;
    std::size_t tracking_half_window_samples = 2u;
    float tracking_min_metric = 0.05f;
    float tracking_metric_ratio = 0.5f;
    // Optional accelerated implementation for frame-local OFDM/MIMO batches.
    // A null pointer selects the portable CPU implementation.
    PhyComputeBackend* compute_backend = nullptr;
};

DynamicLinkReceiverConfig make_dynamic_link_receiver_config(
    const DynamicLinkSimulationConfig& simulation_config,
    NoiseVarianceMode noise_variance_mode = NoiseVarianceMode::fixed);

struct DynamicLinkReceiverState {
    SynchronizationMode synchronization_state = SynchronizationMode::search;
    bool timing_valid = false;
    std::size_t predicted_timing_offset = 0u;
    float last_timing_metric = 0.0f;
    std::size_t synchronization_lock_age_frames = 0u;
    std::size_t consecutive_sync_failures = 0u;
    std::size_t full_search_count = 0u;
    std::size_t tracking_search_count = 0u;
    std::size_t reacquisition_count = 0u;
    bool csi_valid = false;
    std::vector<Channel2x2> filtered_channels;
    std::size_t csi_age_frames = 0u;
    std::size_t reset_count = 0u;
    bool noise_variance_valid = false;
    float filtered_noise_variance = 0.0f;
    std::size_t noise_variance_age_frames = 0u;

    void reset() noexcept;
};

// Reusable top-level buffers for continuous frame processing. This removes
// capacity growth from the simulation/receiver glue after one warm-up frame.
// Optional LdpcFrameDecoder storage is retained and measured separately.
struct DynamicLinkWorkspace {
    std::vector<std::uint8_t> payload;
    std::vector<std::complex<float>> preamble;
    std::array<std::vector<std::complex<float>>, 2> tx_time;
    std::array<std::vector<std::complex<float>>, 2> clean_rx;
    std::array<std::vector<std::complex<float>>, 2> transmitted_symbol;
    std::array<std::vector<std::complex<float>>, 2> received_symbol;
    std::vector<std::vector<std::complex<float>>> rx_stream;
    std::vector<std::vector<std::complex<float>>> resampled_stream;
    std::vector<std::complex<float>> frequency_scratch;
    std::vector<std::complex<float>> fft_scratch;
    std::vector<std::complex<float>> ofdm_samples;
    std::vector<std::complex<float>> rx_grid;
    std::vector<std::complex<float>> dmrs_rx_grid;
    TimingEstimate timing_estimate;
    std::vector<Channel2x2> channels;
    FdmPilotChannelEstimatorWorkspace channel_estimation;
    std::vector<ChannelNxN> dmrs_channels;
    FdmPilotChannelEstimatorWorkspaceNxN dmrs_channel_estimation;
    std::vector<float> control_llrs;
    std::vector<std::complex<float>> equalized;
    std::vector<float> variances;
    std::vector<Channel2x2> adaptation_channels;
    std::vector<std::array<float, 2>> adaptation_mse;
    std::vector<std::complex<float>> pilot_reference_grid;
    std::vector<std::complex<float>> dmrs_reference_grid;
    std::vector<float> noise_power_samples;
    std::vector<std::complex<float>> backend_time_batch;
    std::vector<std::complex<float>> backend_frequency_batch;
    std::vector<std::complex<float>> backend_received_batch;
    std::vector<std::complex<float>> backend_channel_batch;
    std::vector<std::complex<float>> backend_detected_batch;
    std::vector<float> backend_mse_batch;
    std::vector<float> backend_soft_bits;
    std::uint32_t pilot_reference_seed = 0u;
    bool pilot_reference_valid = false;
    std::uint32_t dmrs_reference_seed = 0u;
    bool dmrs_reference_valid = false;
    DynamicFrameDecodeWorkspace frame_decode;
    std::size_t capacity_growths = 0u;
    std::size_t frames_processed = 0u;

    void release() noexcept;
};

struct DynamicLinkTiming {
    double transmit_prepare_us = 0.0;
    double channel_impairments_us = 0.0;
    double synchronization_us = 0.0;
    double fft_csi_us = 0.0;
    double fft_grid_us = 0.0;
    double sfo_correction_us = 0.0;
    double channel_estimation_us = 0.0;
    double noise_estimation_us = 0.0;
    double csi_smoothing_us = 0.0;
    double detection_adaptation_us = 0.0;
    double control_header_us = 0.0;
    double soft_demapping_us = 0.0;
    double ldpc_crc_us = 0.0;
    double control_fec_us = 0.0;
    // Simulator-only truth NMSE/EVM calculations, excluded from receiver_total_us.
    double diagnostics_us = 0.0;
    // Wall time from synchronization start through decode, including diagnostics.
    double receiver_wall_us = 0.0;
    double receiver_total_us = 0.0;
    double simulation_total_us = 0.0;
};

struct DynamicLinkSimulationResult {
    PilotMode pilot_mode = PilotMode::fdm;
    std::size_t frame_symbols = formal_frame_symbols(PilotMode::fdm);
    LinkMode transmitted_mode{};
    LinkMode decoded_mode{};
    LinkDecision recommendation{};
    std::uint16_t sequence = 0u;
    std::size_t user_payload_bytes = 0u;
    // Present only after a successful LDPC decode. Callers must still check
    // crc_ok before forwarding these bytes to an application.
    std::vector<std::uint8_t> user_payload;
    bool timing_ok = false;
    bool header_ok = false;
    bool crc_ok = false;
    SynchronizationMode synchronization_mode_used = SynchronizationMode::search;
    SynchronizationMode receiver_synchronization_state = SynchronizationMode::search;
    bool tracking_fallback = false;
    std::size_t timing_candidates_evaluated = 0u;
    std::size_t synchronization_lock_age_frames = 0u;
    std::size_t syndrome_failures = 0u;
    std::size_t ldpc_worker_threads = 1u;
    std::size_t ldpc_capacity_growths_this_frame = 0u;
    float timing_metric = 0.0f;
    float cfo_error_hz = 0.0f;
    float residual_sfo_ppm = 0.0f;
    bool noise_variance_estimated = false;
    float raw_noise_variance = 0.0f;
    float noise_variance_used = 0.0f;
    std::size_t noise_variance_age_frames = 0u;
    float marker_metric = 0.0f;
    bool csi_smoothed = false;
    std::size_t csi_age_frames = 0u;
    float channel_nmse_db = 0.0f;
    float evm_percent = 0.0f;
    double goodput_bps = 0.0;
    DynamicLinkTiming timing{};
    std::size_t workspace_growths_this_frame = 0u;
    std::size_t workspace_total_growths = 0u;
};

struct PreparedDynamicLinkFrame {
    DynamicLinkSimulationResult result{};
    bool ready = false;
};

// Hardware-facing input: timestamp, agreed pilot seed and one or two complex
// IQ branches. Receiver thresholds/noise configuration are supplied separately.
struct DynamicLinkCaptureFrame {
    std::uint64_t capture_sequence = 0u;
    std::uint64_t timestamp = 0u;
    std::uint32_t pilot_seed = 0xC057u;
    std::vector<std::vector<std::complex<float>>> samples;
};

// Hardware-facing transmitter output. Unlike DynamicLinkIqFrame, this contains
// only the formal over-air waveform and never applies a simulated channel,
// noise, CFO, SFO or timing offset.
struct DynamicLinkTransmitFrame {
    LinkMode mode{};
    std::uint16_t sequence = 0u;
    std::uint32_t pilot_seed = 0xC057u;
    PilotMode pilot_mode = PilotMode::fdm;
    std::vector<std::vector<std::complex<float>>> samples;
    std::vector<std::complex<float>> transmit_reference_grid;
};

void generate_dynamic_tx_iq_frame(
    const std::vector<std::uint8_t>& user_payload,
    LinkMode mode,
    std::uint16_t sequence,
    const Ldpc5041008& codec,
    PilotMode pilot_mode,
    std::uint32_t pilot_seed,
    DynamicLinkTransmitFrame& tx_frame,
    DynamicLinkWorkspace& generation_workspace);

// Simulator capture with optional truth data. The receiver algorithm consumes
// the inherited hardware-facing fields and does not require truth fields.
struct DynamicLinkIqFrame : DynamicLinkCaptureFrame {
    LinkMode transmitted_mode{};
    std::uint16_t sequence = 0u;
    DynamicLinkSimulationConfig config{};
    std::vector<std::uint8_t> expected_payload;
    std::vector<std::complex<float>> truth_payload_symbols;
    // Simulator-only local Tx reference retained for sensing regression. A
    // hardware transmitter supplies the same formal frequency grid locally.
    std::vector<std::complex<float>> transmit_reference_grid;
    bool has_truth = true;
    double transmit_prepare_us = 0.0;
    double channel_impairments_us = 0.0;
};

void generate_dynamic_tdl_iq_frame(
    LinkMode mode,
    std::uint16_t sequence,
    const Ldpc5041008& codec,
    const DynamicLinkSimulationConfig& config,
    DynamicLinkIqFrame& iq_frame,
    DynamicLinkWorkspace& generation_workspace);

// Application-facing simulator entry. The supplied bytes are protected by the
// normal PHY CRC/LDPC chain and must fit the selected Rank/MCS frame capacity.
void generate_dynamic_tdl_iq_frame(
    const std::vector<std::uint8_t>& user_payload,
    LinkMode mode,
    std::uint16_t sequence,
    const Ldpc5041008& codec,
    const DynamicLinkSimulationConfig& config,
    DynamicLinkIqFrame& iq_frame,
    DynamicLinkWorkspace& generation_workspace);

void prepare_dynamic_iq_frame(
    const DynamicLinkIqFrame& iq_frame,
    PreparedDynamicLinkFrame& prepared,
    DynamicLinkReceiverState* receiver_state,
    DynamicLinkWorkspace& receiver_workspace);

void prepare_captured_iq_frame(
    const DynamicLinkCaptureFrame& capture,
    const DynamicLinkReceiverConfig& receiver_config,
    PreparedDynamicLinkFrame& prepared,
    DynamicLinkReceiverState* receiver_state,
    DynamicLinkWorkspace& receiver_workspace);

// Compatibility overload: converts simulator truth into a fixed-noise
// receiver configuration.
void prepare_captured_iq_frame(
    const DynamicLinkCaptureFrame& capture,
    const DynamicLinkSimulationConfig& receiver_config,
    PreparedDynamicLinkFrame& prepared,
    DynamicLinkReceiverState* receiver_state,
    DynamicLinkWorkspace& receiver_workspace);

void prepare_dynamic_tdl_frame(
    LinkMode mode,
    std::uint16_t sequence,
    const Ldpc5041008& codec,
    const DynamicLinkSimulationConfig& config,
    PreparedDynamicLinkFrame& prepared,
    DynamicLinkReceiverState* receiver_state,
    DynamicLinkWorkspace& workspace);

DynamicLinkSimulationResult finish_dynamic_tdl_frame(
    PreparedDynamicLinkFrame& prepared,
    const Ldpc5041008& codec,
    const DynamicLinkWorkspace& workspace,
    LdpcFrameDecoder* ldpc_frame_decoder = nullptr);

DynamicLinkSimulationResult simulate_dynamic_tdl_frame(
    LinkMode mode,
    std::uint16_t sequence,
    const Ldpc5041008& codec,
    const DynamicLinkSimulationConfig& config = {},
    DynamicLinkReceiverState* receiver_state = nullptr,
    DynamicLinkWorkspace* workspace = nullptr,
    LdpcFrameDecoder* ldpc_frame_decoder = nullptr);

}  // namespace openisac
