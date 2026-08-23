#pragma once

#include "openisac/channel_estimation.hpp"
#include "openisac/dynamic_frame.hpp"
#include "openisac/link_adaptation.hpp"
#include "openisac/ldpc.hpp"
#include "openisac/mimo_nxn.hpp"
#include "openisac/preamble_sync.hpp"
#include "openisac/tdl_channel.hpp"

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace openisac {

class LdpcFrameDecoder;

enum class Rank4SynchronizationMode : std::uint8_t {
    search,
    track,
    reacquire,
};

struct Rank4TimeSimulationConfig {
    PilotMode pilot_mode = PilotMode::fdm;
    Modulation modulation = Modulation::qam64;
    // Zero selects the Rank-4/MCS maximum payload.
    std::size_t payload_bytes = 0u;
    float snr_db = 50.0f;
    std::size_t timing_offset_samples = 20u;
    float cfo_hz = 300.0f;
    float sfo_ppm = 20.0f;
    float transmit_correlation = 0.2f;
    float receive_correlation = 0.2f;
    float csi_smoothing_alpha = 1.0f;
    // Average the two phase-aligned OFDM-symbol channel estimates. This is
    // appropriate when the channel is effectively constant inside one frame.
    bool average_intra_frame_csi = true;
    // Multiplier for the robust pilot-residual variance used by the MMSE
    // detector. Kept explicit so fixed-seed engineering sweeps are repeatable.
    float mmse_regularization_scale = 0.5f;
    // Simulated start time of this frame. TDL Doppler phases are evaluated at
    // this time so coherent sensing sees continuous slow-time phase.
    double channel_time_seconds = 0.0;
    // Retain the raw 4x4 channel snapshot and Rx0 waveform for low-rate live
    // telemetry/sensing. Keep disabled on ordinary communication frames.
    bool enable_sensing_snapshot = false;
    std::size_t diagnostic_waveform_points = 4096u;
    std::size_t tracking_half_window_samples = 2u;
    float tracking_min_metric = 0.05f;
    float tracking_metric_ratio = 0.5f;
    std::uint32_t channel_seed = 0x4C057u;
    std::uint32_t random_seed = 0x54494D45u;
    std::uint32_t pilot_seed = 0xC057u;
    unsigned maximum_ldpc_iterations = 10u;
    std::vector<TdlTap> taps{
        {0u, 0.0f, 0.0f, 0.0f},
        {3u, -14.0f, 45.0f, 0.0f},
        {9u, -8.0f, -80.0f, 0.0f}};
};

struct Rank4TimeReceiverState {
    Rank4SynchronizationMode synchronization_state =
        Rank4SynchronizationMode::search;
    bool timing_valid = false;
    std::size_t predicted_timing_offset = 0u;
    float last_timing_metric = 0.0f;
    std::size_t synchronization_lock_age_frames = 0u;
    std::size_t full_search_count = 0u;
    std::size_t tracking_search_count = 0u;
    std::size_t reacquisition_count = 0u;
    bool csi_valid = false;
    std::vector<ChannelNxN> filtered_channels;
    std::size_t csi_age_frames = 0u;
    std::size_t reset_count = 0u;

    void reset() noexcept;
};

// Reusable Rank-4 receive-side buffers. A caller that processes a continuous
// stream should retain one workspace per receive pipeline slot.
struct Rank4TimeWorkspace {
    std::vector<std::complex<float>> preamble;
    TimingEstimate timing;
    std::vector<std::complex<float>> rx_grid;
    std::vector<std::complex<float>> dmrs_rx_grid;
    std::vector<std::complex<float>> ofdm_samples;
    std::vector<std::complex<float>> frequency_scratch;
    std::vector<std::complex<float>> pilot_reference_grid;
    std::vector<std::complex<float>> dmrs_reference_grid;
    std::uint32_t pilot_reference_seed = 0u;
    Modulation pilot_reference_modulation = Modulation::qpsk;
    bool pilot_reference_valid = false;
    std::uint32_t dmrs_reference_seed = 0u;
    bool dmrs_reference_valid = false;
    std::vector<ChannelNxN> channels;
    FdmPilotChannelEstimatorWorkspaceNxN channel_estimation;
    std::vector<float> noise_power_samples;
    std::vector<float> control_llrs;
    std::vector<std::complex<float>> equalized;
    std::vector<float> variances;
    DynamicFrameDecodeWorkspace frame_decode;
    std::size_t frames_processed = 0u;

    void release() noexcept;
};

struct Rank4TimeSimulationResult {
    PilotMode pilot_mode = PilotMode::fdm;
    std::size_t frame_symbols = formal_frame_symbols(PilotMode::fdm);
    std::uint16_t sequence = 0u;
    std::size_t payload_bytes = 0u;
    bool timing_ok = false;
    bool header_ok = false;
    bool crc_ok = false;
    bool payload_match = false;
    std::size_t syndrome_failures = 0u;
    std::size_t pre_fec_bit_errors = 0u;
    std::size_t pre_fec_compared_bits = 0u;
    float pre_fec_ber = 0.0f;
    std::size_t timing_offset = 0u;
    float timing_metric = 0.0f;
    Rank4SynchronizationMode synchronization_mode_used =
        Rank4SynchronizationMode::search;
    bool tracking_fallback = false;
    std::size_t timing_candidates_evaluated = 0u;
    std::size_t synchronization_lock_age_frames = 0u;
    float estimated_cfo_hz = 0.0f;
    float cfo_error_hz = 0.0f;
    float estimated_sfo_ppm = 0.0f;
    float residual_sfo_ppm = 0.0f;
    float noise_variance = 0.0f;
    float evm_percent = 0.0f;
    float channel_nmse_db = 0.0f;
    double synchronization_us = 0.0;
    double fft_sfo_us = 0.0;
    double channel_estimation_us = 0.0;
    double detection_us = 0.0;
    double soft_demapping_us = 0.0;
    double ldpc_crc_us = 0.0;
    double receiver_us = 0.0;
    double simulation_us = 0.0;
    std::size_t ldpc_worker_threads = 1u;
    unsigned maximum_ldpc_iterations_used = 0u;
    std::size_t ldpc_capacity_growths_this_frame = 0u;
    std::size_t workspace_growths_this_frame = 0u;
    std::vector<std::complex<float>> transmitted_symbols;
    std::vector<std::complex<float>> equalized_symbols;
    // Optional Rank-4 telemetry. Channel layout is
    // [link = rx*4+tx][fft], averaged over the two data symbols.
    std::vector<std::complex<float>> sensing_channel_frequency_response;
    std::vector<std::uint8_t> sensing_active_subcarrier_mask;
    std::vector<std::complex<float>> receive_waveform_rx0;
    // Populated only when CRC passes. Video/network callers must still check
    // crc_ok before forwarding the bytes.
    std::vector<std::uint8_t> user_payload;
};

// Front-end result retained in one pipeline slot. The front end completes
// synchronization, channel estimation, detection and soft demapping; finish
// performs only LDPC/packing/CRC and may run on a background thread.
struct PreparedRank4TimeFrame {
    Rank4TimeSimulationResult result{};
    std::vector<std::uint8_t> expected_payload;
    unsigned maximum_ldpc_iterations = 10u;
    bool ready = false;
};

void prepare_rank4_time_frame(
    std::uint16_t sequence,
    const Rank4TimeSimulationConfig& config,
    const Ldpc5041008& codec,
    PreparedRank4TimeFrame& prepared,
    Rank4TimeReceiverState* receiver_state,
    Rank4TimeWorkspace& workspace);

void prepare_rank4_time_payload_frame(
    const std::vector<std::uint8_t>& payload,
    std::uint16_t sequence,
    const Rank4TimeSimulationConfig& config,
    const Ldpc5041008& codec,
    PreparedRank4TimeFrame& prepared,
    Rank4TimeReceiverState* receiver_state,
    Rank4TimeWorkspace& workspace);

Rank4TimeSimulationResult finish_rank4_time_frame(
    PreparedRank4TimeFrame& prepared,
    const Ldpc5041008& codec,
    Rank4TimeWorkspace& workspace,
    LdpcFrameDecoder* ldpc_frame_decoder = nullptr);

// Rank-4 waveform closure: one port-0 ZC preamble, optional two-symbol
// front-loaded NR-style DM-RS, then two formal data symbols on four Tx/Rx.
Rank4TimeSimulationResult simulate_rank4_time_frame(
    std::uint16_t sequence,
    const Rank4TimeSimulationConfig& config,
    const Ldpc5041008& codec,
    Rank4TimeReceiverState* receiver_state = nullptr,
    Rank4TimeWorkspace* workspace = nullptr,
    LdpcFrameDecoder* ldpc_frame_decoder = nullptr);

Rank4TimeSimulationResult simulate_rank4_time_payload_frame(
    const std::vector<std::uint8_t>& payload,
    std::uint16_t sequence,
    const Rank4TimeSimulationConfig& config,
    const Ldpc5041008& codec,
    Rank4TimeReceiverState* receiver_state = nullptr,
    Rank4TimeWorkspace* workspace = nullptr,
    LdpcFrameDecoder* ldpc_frame_decoder = nullptr);

}  // namespace openisac
