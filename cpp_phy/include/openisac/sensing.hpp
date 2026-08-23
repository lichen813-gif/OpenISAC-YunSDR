#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace openisac {

// Sensing profile aligned with the current formal communication PHY:
// 1024 subcarriers, two data OFDM symbols, 2x2 or 4x4 physical ports and one
// channel snapshot per 225-us frame. The hardware timestamp unit is deliberately
// left to the capture adapter; metric axes use frame_period_seconds.
struct DynamicSensingConfig {
    std::size_t fft_size = 1024u;
    std::size_t data_symbols = 2u;
    std::size_t transmit_ports = 2u;
    std::size_t receive_ports = 2u;
    std::size_t selected_transmit_port = 0u;
    std::size_t selected_receive_port = 0u;
    std::size_t coherent_frames = 64u;
    std::size_t range_fft_size = 1024u;
    std::size_t doppler_fft_size = 64u;
    float subcarrier_spacing_hz = 15000.0f;
    float frame_period_seconds = 225.0e-6f;
    float center_frequency_hz = 5.8e9f;
    // 2 for monostatic round-trip range/velocity, 1 for one-way equivalent.
    float propagation_path_factor = 2.0f;
    float regularization = 1.0e-6f;
    float minimum_reference_power = 1.0e-8f;
    float minimum_relative_determinant = 1.0e-4f;
    std::size_t minimum_range_bin = 1u;
    // Zero selects the non-negative-delay half of the circular range IFFT.
    std::size_t maximum_range_bin = 0u;
    std::size_t doppler_dc_exclusion_bins = 0u;
    // Remove the coherent slow-time mean before the Doppler FFT. This rejects
    // exact-DC leakage/static clutter but intentionally suppresses static targets.
    bool enable_static_clutter_suppression = false;
    bool enable_cfar_detection = true;
    std::size_t cfar_training_doppler = 3u;
    std::size_t cfar_training_range = 4u;
    std::size_t cfar_guard_doppler = 1u;
    std::size_t cfar_guard_range = 1u;
    float cfar_false_alarm_probability = 1.0e-5f;
    std::size_t cfar_suppression_doppler = 2u;
    std::size_t cfar_suppression_range = 2u;
    std::size_t cfar_max_detections = 32u;
    bool enable_range_window = true;
    bool enable_doppler_window = true;
    bool reset_on_sequence_gap = true;
};

struct DynamicSensingChannelEstimate {
    std::vector<std::complex<float>> frequency_response;
    std::vector<std::uint8_t> active_subcarrier_mask;
    std::vector<std::uint8_t> direct_estimate_mask;
    std::size_t active_subcarriers = 0u;
    std::size_t directly_estimated_subcarriers = 0u;
    std::size_t interpolated_subcarriers = 0u;
    std::size_t ill_conditioned_subcarriers = 0u;
    // Reused interpolation workspaces. Callers should treat these as internal
    // scratch storage; keeping them here avoids two allocations per frame.
    std::vector<int> interpolation_left;
    std::vector<int> interpolation_right;
};

// Recover one selected Tx->Rx channel from the two known transmitted data
// symbols. Full-rank RE use a 2x2 regularized LS solve; single-port pilot/control
// RE use direct division; remaining active holes are linearly interpolated.
// This supports the current QPSK/16/64/256-QAM Rank-1/2 waveform and does not
// assume unit-magnitude QPSK symbols.
void estimate_dynamic_sensing_channel_2x2(
    const std::vector<std::complex<float>>& transmit_grid,
    const std::vector<std::complex<float>>& receive_grid,
    const DynamicSensingConfig& config,
    DynamicSensingChannelEstimate& estimate);

struct DynamicSensingPeak {
    std::size_t range_bin = 0u;
    std::size_t doppler_bin = 0u;
    float range_m = 0.0f;
    float doppler_hz = 0.0f;
    float velocity_mps = 0.0f;
    float power = 0.0f;
};

struct DynamicSensingDetection {
    DynamicSensingPeak peak{};
    float noise_power = 0.0f;
    float threshold_power = 0.0f;
    float power_over_threshold_db = 0.0f;
};

struct DynamicSensingResult {
    bool ready = false;
    std::size_t coherent_frames = 0u;
    std::uint64_t first_capture_sequence = 0u;
    std::uint64_t last_capture_sequence = 0u;
    std::uint64_t first_timestamp = 0u;
    std::uint64_t last_timestamp = 0u;
    std::size_t sequence_gap_resets = 0u;
    std::size_t timestamp_regressions = 0u;
    std::size_t directly_estimated_subcarriers = 0u;
    std::size_t interpolated_subcarriers = 0u;
    float range_bin_spacing_m = 0.0f;
    float doppler_bin_spacing_hz = 0.0f;
    float velocity_bin_spacing_mps = 0.0f;
    bool static_clutter_suppression_applied = false;
    std::size_t cfar_cells_tested = 0u;
    // [fft-shifted Doppler row][range bin].
    std::vector<std::complex<float>> range_doppler_map;
    DynamicSensingPeak strongest_peak{};
    std::vector<DynamicSensingDetection> detections;
};

class DynamicSensingProcessor {
public:
    explicit DynamicSensingProcessor(const DynamicSensingConfig& config = {});
    ~DynamicSensingProcessor();

    DynamicSensingProcessor(const DynamicSensingProcessor&) = delete;
    DynamicSensingProcessor& operator=(const DynamicSensingProcessor&) = delete;
    DynamicSensingProcessor(DynamicSensingProcessor&&) = delete;
    DynamicSensingProcessor& operator=(DynamicSensingProcessor&&) = delete;

    // Returns true when one coherent range-Doppler result has completed.
    bool push_frame(
        std::uint64_t capture_sequence,
        std::uint64_t timestamp,
        const std::vector<std::complex<float>>& transmit_grid,
        const std::vector<std::complex<float>>& receive_grid);

    // Push already-estimated MIMO channel responses. frequency_response uses
    // [link = rx*transmit_ports+tx][fft] layout. The processor performs a
    // range/Doppler transform per link and noncoherently sums link powers, so
    // Rank-4 sensing does not require cross-RF-chain phase calibration.
    bool push_channel_frame(
        std::uint64_t capture_sequence,
        std::uint64_t timestamp,
        const std::vector<std::complex<float>>& frequency_response,
        const std::vector<std::uint8_t>& active_subcarrier_mask);

    const DynamicSensingResult& last_result() const noexcept;
    std::size_t frames_accumulated() const noexcept;
    const DynamicSensingConfig& config() const noexcept;
    void reset() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace openisac
