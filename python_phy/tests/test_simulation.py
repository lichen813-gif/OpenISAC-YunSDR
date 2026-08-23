from dataclasses import replace

from openisac_phy import SimulationConfig, simulate_alamouti_ofdm
from openisac_phy.config import ChannelTap


def test_end_to_end_noiseless_crc_and_bits() -> None:
    cfg = SimulationConfig(
        modulation="256qam",
        fft_size=32,
        cp_length=8,
        frames=20,
        nr=2,
        channel="rayleigh",
        snr_db=float("inf"),
        seed=19,
    )
    result = simulate_alamouti_ofdm(cfg)
    assert result["bit_errors"] == 0
    assert result["block_errors"] == 0
    assert result["crc_failures"] == 0


def test_awgn_ber_improves_with_snr() -> None:
    common = dict(
        modulation="qpsk",
        fft_size=32,
        cp_length=8,
        frames=500,
        nr=1,
        channel="awgn",
        seed=23,
    )
    low = simulate_alamouti_ofdm(SimulationConfig(**common, snr_db=-2.0))
    high = simulate_alamouti_ofdm(SimulationConfig(**common, snr_db=12.0))
    assert high["ber"] < low["ber"]
    assert high["crc_failure_rate"] < low["crc_failure_rate"]


def test_constellation_artifacts_have_expected_shapes() -> None:
    cfg = SimulationConfig(
        modulation="64qam",
        fft_size=16,
        cp_length=4,
        frames=3,
        nr=2,
        channel="static",
        snr_db=20.0,
        seed=29,
    )
    result, artifacts = simulate_alamouti_ofdm(cfg, return_artifacts=True)
    assert result["nt"] == 2
    assert artifacts.tx_symbols.shape == (3, 16, 2)
    assert artifacts.equalized_symbols.shape == (3, 16, 2)
    assert artifacts.equivalent_noise_variance.shape == (3, 16)
    assert artifacts.channel.shape == (3, 16, 2, 2)
    assert artifacts.channel_impulse_response.shape == (3, 2, 2, 1)
    assert artifacts.pilot_symbols.shape == (3, 0, 2)
    assert artifacts.equalized_pilots.shape == (3, 0, 2)
    assert artifacts.data_indices.shape == (16,)
    assert artifacts.pilot_indices.shape == (0,)
    assert artifacts.tx_time.shape == (3, 2, 2, 20)
    assert artifacts.rx_time.shape == (3, 2, 2, 20)


def test_tdl_end_to_end_noiseless_and_frequency_selective() -> None:
    cfg = SimulationConfig(
        modulation="256qam",
        fft_size=64,
        cp_length=16,
        frames=20,
        nr=2,
        channel="tdl",
        channel_taps=(
            ChannelTap(0, 0.0, 0.0),
            ChannelTap(3, -4.0, 45.0),
            ChannelTap(9, -8.0, -80.0),
        ),
        snr_db=float("inf"),
        seed=31,
    )
    result, artifacts = simulate_alamouti_ofdm(cfg, return_artifacts=True)
    assert result["bit_errors"] == 0
    assert result["crc_failures"] == 0
    assert result["channel_path_count"] == 3
    assert result["channel_max_delay_samples"] == 9
    magnitude = abs(artifacts.channel[0, :, 0, 0])
    assert float(magnitude.max() - magnitude.min()) > 0.1


def test_tdl_delay_beyond_cp_is_rejected() -> None:
    cfg = SimulationConfig(
        fft_size=32,
        cp_length=4,
        frames=1,
        channel="tdl",
        channel_taps=(ChannelTap(0, 0.0), ChannelTap(5, -3.0)),
    )
    try:
        cfg.validate()
    except ValueError as error:
        assert "exceeds CP length" in str(error)
        return
    raise AssertionError("TDL delay greater than CP was accepted")


def test_end_to_end_cp_sync_corrects_timing_and_cfo() -> None:
    cfg = SimulationConfig(
        modulation="64qam",
        fft_size=64,
        cp_length=16,
        frames=20,
        nr=2,
        channel="tdl",
        channel_taps=(
            ChannelTap(0, 0.0, 0.0),
            ChannelTap(3, -4.0, 45.0),
            ChannelTap(9, -8.0, -80.0),
        ),
        snr_db=float("inf"),
        synchronization_enable=True,
        timing_offset_samples=5,
        timing_search_samples=16,
        cfo_hz=1200.0,
        seed=37,
    )
    result, artifacts = simulate_alamouti_ofdm(cfg, return_artifacts=True)

    assert result["bit_errors"] == 0
    assert result["crc_failures"] == 0
    assert result["timing_success_rate"] == 1.0
    assert result["mean_absolute_cfo_error_hz"] < 1.0e-8
    assert artifacts.synchronization_metric.shape == (20, 17)
    assert artifacts.rx_stream.shape == (20, 2, 181)


def test_end_to_end_zc_preamble_sync_corrects_tdl_timing_and_cfo() -> None:
    cfg = SimulationConfig(
        modulation="64qam",
        fft_size=64,
        cp_length=16,
        frames=20,
        nr=2,
        channel="tdl",
        channel_taps=(
            ChannelTap(0, 0.0, 0.0),
            ChannelTap(3, -4.0, 45.0),
            ChannelTap(9, -8.0, -80.0),
        ),
        snr_db=float("inf"),
        preamble_enable=True,
        zc_root=29,
        synchronization_enable=True,
        timing_offset_samples=5,
        timing_search_samples=16,
        cfo_hz=1200.0,
        seed=407,
    )
    result, artifacts = simulate_alamouti_ofdm(cfg, return_artifacts=True)

    assert result["bit_errors"] == 0
    assert result["crc_failures"] == 0
    assert result["timing_success_rate"] == 1.0
    assert result["mean_absolute_cfo_error_hz"] < 1.0e-8
    assert result["synchronization_method"] == "zc_matched_filter_cp_cfo"
    assert result["frame_ofdm_symbols"] == 3
    assert result["samples_per_frame"] == 240
    assert artifacts.preamble_frequency.shape == (64,)
    assert artifacts.tx_time.shape == (20, 3, 2, 80)
    assert artifacts.rx_time.shape == (20, 3, 2, 80)
    assert artifacts.rx_stream.shape == (20, 2, 261)


def test_pilot_phase_tracking_improves_noisy_high_order_tdl_case() -> None:
    base = SimulationConfig(
        modulation="256qam",
        fft_size=64,
        cp_length=16,
        dc_null=True,
        pilot_spacing=8,
        frames=500,
        nr=2,
        channel="tdl",
        channel_taps=(
            ChannelTap(0, 0.0, 0.0),
            ChannelTap(3, -4.0, 45.0),
            ChannelTap(9, -8.0, -80.0),
        ),
        snr_db=25.0,
        synchronization_enable=True,
        timing_offset_samples=0,
        timing_search_samples=0,
        cfo_hz=3000.0,
        seed=23063,
    )
    without_tracking = simulate_alamouti_ofdm(
        replace(base, pilot_phase_tracking_enable=False)
    )
    with_tracking = simulate_alamouti_ofdm(
        replace(base, pilot_phase_tracking_enable=True)
    )

    assert with_tracking["ber"] < without_tracking["ber"]
    assert with_tracking["evm_rms"] < without_tracking["evm_rms"]
    assert with_tracking["mean_pilot_phase_coherence"] > 0.99


def test_ls_channel_estimation_flat_noiseless_zero_errors() -> None:
    cfg = SimulationConfig(
        modulation="256qam",
        fft_size=64,
        cp_length=16,
        dc_null=True,
        pilot_spacing=8,
        frames=20,
        nr=2,
        channel="static",
        snr_db=float("inf"),
        channel_estimation="ls_linear",
        seed=43,
    )
    result = simulate_alamouti_ofdm(cfg)

    assert result["bit_errors"] == 0
    assert result["crc_failures"] == 0
    assert result["channel_estimation_nmse"] < 1.0e-28
    assert result["pilot_channel_nmse"] < 1.0e-28


def test_denser_pilots_reduce_tdl_linear_interpolation_nmse() -> None:
    base = SimulationConfig(
        modulation="qpsk",
        fft_size=64,
        cp_length=16,
        dc_null=True,
        frames=20,
        nr=2,
        channel="tdl",
        channel_taps=(
            ChannelTap(0, 0.0, 0.0),
            ChannelTap(3, -4.0, 45.0),
            ChannelTap(9, -8.0, -80.0),
        ),
        snr_db=float("inf"),
        channel_estimation="ls_linear",
        seed=47,
    )
    dense = simulate_alamouti_ofdm(replace(base, pilot_spacing=2))
    sparse = simulate_alamouti_ofdm(replace(base, pilot_spacing=8))

    assert dense["pilot_channel_nmse"] < 1.0e-28
    assert sparse["pilot_channel_nmse"] < 1.0e-28
    assert dense["channel_estimation_nmse"] < sparse["channel_estimation_nmse"]


def test_dft_ls_removes_noiseless_tdl_interpolation_floor() -> None:
    base = SimulationConfig(
        modulation="64qam",
        fft_size=64,
        cp_length=16,
        dc_null=True,
        pilot_spacing=2,
        frames=50,
        nr=2,
        channel="tdl",
        channel_taps=(
            ChannelTap(0, 0.0, 0.0),
            ChannelTap(3, -4.0, 45.0),
            ChannelTap(9, -8.0, -80.0),
        ),
        snr_db=float("inf"),
        channel_estimation_taps=10,
        seed=53,
    )
    linear = simulate_alamouti_ofdm(replace(base, channel_estimation="ls_linear"))
    dft_ls = simulate_alamouti_ofdm(replace(base, channel_estimation="ls_dft"))

    assert linear["bit_errors"] > 0
    assert dft_ls["bit_errors"] == 0
    assert dft_ls["channel_estimation_nmse"] < 1.0e-28


def test_lmmse_reduces_noisy_tdl_nmse_and_ber_vs_linear() -> None:
    base = SimulationConfig(
        modulation="64qam",
        fft_size=64,
        cp_length=16,
        dc_null=True,
        pilot_spacing=4,
        frames=300,
        nr=2,
        channel="tdl",
        channel_taps=(
            ChannelTap(0, 0.0, 0.0),
            ChannelTap(3, -4.0, 45.0),
            ChannelTap(9, -8.0, -80.0),
        ),
        snr_db=20.0,
        channel_estimation_taps=10,
        seed=277,
    )
    linear = simulate_alamouti_ofdm(replace(base, channel_estimation="ls_linear"))
    lmmse = simulate_alamouti_ofdm(replace(base, channel_estimation="lmmse"))

    assert lmmse["channel_estimation_nmse"] < linear["channel_estimation_nmse"]
    assert lmmse["ber"] < linear["ber"]


def test_phase_references_remove_noiseless_slot_phase_jump_without_perfect_csi() -> None:
    base = SimulationConfig(
        modulation="64qam",
        fft_size=64,
        cp_length=16,
        dc_null=True,
        pilot_spacing=2,
        frames=50,
        nr=2,
        channel="tdl",
        channel_taps=(
            ChannelTap(0, 0.0, 0.0),
            ChannelTap(3, -4.0, 45.0),
            ChannelTap(9, -8.0, -80.0),
        ),
        snr_db=float("inf"),
        channel_estimation="ls_dft",
        channel_estimation_taps=10,
        slot_phase_offset_deg=30.0,
        seed=311,
    )
    without_reference = simulate_alamouti_ofdm(
        replace(base, phase_reference_tracking_enable=False)
    )
    with_reference = simulate_alamouti_ofdm(
        replace(
            base,
            phase_reference_tracking_enable=True,
            phase_reference_count=2,
        )
    )

    assert without_reference["bit_errors"] > 0
    assert with_reference["bit_errors"] == 0
    assert abs(with_reference["estimated_differential_phase_deg_mean"] - 30.0) < 1.0e-10
    assert with_reference["channel_estimation_nmse"] < 1.0e-28


def test_phase_references_improve_noisy_lmmse_slot_phase_case() -> None:
    base = SimulationConfig(
        modulation="64qam",
        fft_size=64,
        cp_length=16,
        dc_null=True,
        pilot_spacing=4,
        frames=300,
        nr=2,
        channel="tdl",
        channel_taps=(
            ChannelTap(0, 0.0, 0.0),
            ChannelTap(3, -4.0, 45.0),
            ChannelTap(9, -8.0, -80.0),
        ),
        snr_db=20.0,
        channel_estimation="lmmse",
        channel_estimation_taps=10,
        slot_phase_offset_deg=30.0,
        pilot_phase_min_coherence=0.9,
        seed=313,
    )
    without_reference = simulate_alamouti_ofdm(
        replace(base, phase_reference_tracking_enable=False)
    )
    with_reference = simulate_alamouti_ofdm(
        replace(
            base,
            phase_reference_tracking_enable=True,
            phase_reference_count=2,
        )
    )

    assert with_reference["ber"] < without_reference["ber"]
    assert with_reference["channel_estimation_nmse"] < without_reference[
        "channel_estimation_nmse"
    ]
    assert with_reference["mean_phase_reference_coherence"] > 0.98


def test_phase_slope_tracking_improves_time_resampled_sfo_case() -> None:
    base = SimulationConfig(
        modulation="64qam",
        fft_size=64,
        cp_length=16,
        dc_null=True,
        pilot_spacing=2,
        preamble_enable=True,
        synchronization_enable=True,
        phase_reference_tracking_enable=True,
        phase_reference_count=16,
        pilot_phase_min_coherence=0.8,
        sfo_ppm=200.0,
        frames=100,
        nr=2,
        channel="tdl",
        channel_taps=(
            ChannelTap(0, 0.0, 0.0),
            ChannelTap(3, -4.0, 45.0),
            ChannelTap(9, -8.0, -80.0),
        ),
        snr_db=30.0,
        channel_estimation="lmmse",
        channel_estimation_taps=10,
        seed=431,
    )
    without_slope = simulate_alamouti_ofdm(
        replace(base, sfo_tracking_enable=False)
    )
    with_slope = simulate_alamouti_ofdm(
        replace(base, sfo_tracking_enable=True)
    )

    assert with_slope["ber"] < without_slope["ber"]
    assert with_slope["evm_rms"] < without_slope["evm_rms"]
    assert with_slope["channel_estimation_nmse"] < without_slope[
        "channel_estimation_nmse"
    ]
    assert abs(with_slope["estimated_sfo_ppm_mean"] - 200.0) < 40.0
    assert with_slope["mean_phase_reference_coherence"] > 0.99


def test_doppler_creates_alamouti_block_mismatch_and_errors() -> None:
    base = SimulationConfig(
        modulation="64qam",
        fft_size=64,
        cp_length=16,
        dc_null=True,
        pilot_spacing=2,
        frames=100,
        nr=2,
        channel="tdl",
        snr_db=float("inf"),
        channel_estimation="ls_dft",
        channel_estimation_taps=10,
        seed=501,
    )
    static = simulate_alamouti_ofdm(replace(base, doppler_hz=0.0))
    varying = simulate_alamouti_ofdm(replace(base, doppler_hz=500.0))

    assert static["bit_errors"] == 0
    assert static["stbc_channel_variation_nmse"] == 0.0
    assert varying["bit_errors"] > 0
    assert varying["stbc_channel_variation_nmse"] > 0.03
    assert varying["evm_rms"] > static["evm_rms"]
