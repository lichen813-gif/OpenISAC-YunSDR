from dataclasses import replace

import numpy as np

from openisac_phy.config import SimulationConfig
from openisac_phy.mimo import (
    detect_spatial_multiplexing,
    spatial_multiplexing_encode_grid,
)
from openisac_phy.simulation import simulate_alamouti_ofdm


def test_spatial_mapping_keeps_total_transmit_power_one() -> None:
    rng = np.random.default_rng(10)
    symbols = (
        rng.standard_normal((1000, 2, 4, 2))
        + 1j * rng.standard_normal((1000, 2, 4, 2))
    ) / np.sqrt(2.0)
    grid = spatial_multiplexing_encode_grid(symbols, np.array([1, 2, 4, 5]), 8)

    active_power = np.sum(np.abs(grid[:, :, [1, 2, 4, 5], :]) ** 2, axis=-1)
    assert np.isclose(np.mean(active_power), 1.0, atol=0.03)


def test_zf_and_mmse_exactly_recover_identity_channel_without_noise() -> None:
    rng = np.random.default_rng(12)
    symbols = rng.standard_normal((3, 2, 5, 2)) + 1j * rng.standard_normal(
        (3, 2, 5, 2)
    )
    channel = np.broadcast_to(np.eye(2), (3, 2, 5, 2, 2)).copy()
    received = np.squeeze(
        (channel / np.sqrt(2.0)) @ symbols[..., None], axis=-1
    )

    for detector in ("zf", "mmse"):
        recovered, predicted_mse = detect_spatial_multiplexing(
            received, channel, 0.0, detector
        )
        np.testing.assert_allclose(recovered, symbols, atol=1.0e-12)
        np.testing.assert_allclose(predicted_mse, 0.0, atol=1.0e-12)


def test_spatial_multiplexing_end_to_end_noiseless_rayleigh() -> None:
    result = simulate_alamouti_ofdm(
        SimulationConfig(
            modulation="256qam",
            fft_size=64,
            cp_length=16,
            dc_null=True,
            frames=10,
            nr=2,
            mode="spatial_multiplexing",
            layers=2,
            detector="zf",
            channel="rayleigh",
            snr_db=float("inf"),
            seed=301,
            channel_seed=401,
        )
    )

    assert result["mode"] == "spatial_multiplexing"
    assert result["bit_errors"] == 0
    assert result["crc_failures"] == 0
    assert result["evm_rms"] < 1.0e-11
    assert result["mean_channel_rank"] == 2.0


def test_spatial_multiplexing_has_twice_stbc_gross_rate() -> None:
    base = SimulationConfig(
        modulation="64qam",
        fft_size=64,
        cp_length=16,
        dc_null=True,
        frames=2,
        nr=2,
        channel="rayleigh",
        snr_db=20.0,
    )
    stbc = simulate_alamouti_ofdm(base)
    spatial = simulate_alamouti_ofdm(
        replace(base, mode="spatial_multiplexing", layers=2, detector="mmse")
    )

    assert spatial["gross_phy_rate_bps"] == 2.0 * stbc["gross_phy_rate_bps"]
    assert spatial["gross_spectral_efficiency_bps_hz"] == 2.0 * stbc[
        "gross_spectral_efficiency_bps_hz"
    ]


def test_mmse_improves_evm_and_ber_over_zf_at_moderate_snr() -> None:
    base = SimulationConfig(
        modulation="64qam",
        fft_size=64,
        cp_length=16,
        dc_null=True,
        frames=300,
        nr=2,
        mode="spatial_multiplexing",
        layers=2,
        channel="rayleigh",
        snr_db=15.0,
        seed=77,
        channel_seed=701,
        noise_seed=702,
    )
    zf = simulate_alamouti_ofdm(replace(base, detector="zf"))
    mmse = simulate_alamouti_ofdm(replace(base, detector="mmse"))

    assert mmse["ber"] < zf["ber"]
    assert mmse["evm_rms"] < zf["evm_rms"]


def test_spatial_dft_ls_tdl_noiseless_end_to_end() -> None:
    result = simulate_alamouti_ofdm(
        SimulationConfig(
            modulation="64qam",
            fft_size=64,
            cp_length=16,
            dc_null=True,
            pilot_spacing=2,
            frames=8,
            nr=2,
            mode="spatial_multiplexing",
            layers=2,
            detector="zf",
            channel="tdl",
            channel_estimation="ls_dft",
            channel_estimation_taps=10,
            snr_db=float("inf"),
            seed=811,
        )
    )

    assert result["bit_errors"] == 0
    assert result["crc_failures"] == 0
    assert result["channel_estimation_nmse"] < 1.0e-25
    assert result["pilot_channel_nmse"] < 1.0e-25


def test_spatial_pilots_alternate_tx_and_keep_total_power_one() -> None:
    from openisac_phy.resource_grid import deterministic_spatial_pilots

    pilots, assignments = deterministic_spatial_pilots(
        5, np.arange(-16, 17, 4), 901
    )

    np.testing.assert_array_equal(assignments, np.arange(assignments.size) % 2)
    np.testing.assert_allclose(np.sum(np.abs(pilots) ** 2, axis=-1), 1.0)
    assert np.all(np.count_nonzero(pilots, axis=-1) == 1)


def test_four_and_eight_layer_identity_detection() -> None:
    rng = np.random.default_rng(1001)
    for layers in (4, 8):
        symbols = rng.standard_normal((2, 2, 3, layers)) + 1j * rng.standard_normal(
            (2, 2, 3, layers)
        )
        channel = np.broadcast_to(
            np.eye(layers), (2, 2, 3, layers, layers)
        ).copy()
        received = np.squeeze(
            (channel / np.sqrt(layers)) @ symbols[..., None], axis=-1
        )
        for detector in ("zf", "mmse"):
            recovered, _ = detect_spatial_multiplexing(
                received, channel, 0.0, detector
            )
            np.testing.assert_allclose(recovered, symbols, atol=1.0e-11)


def test_four_by_four_and_eight_by_eight_noiseless_end_to_end() -> None:
    for antennas in (4, 8):
        result = simulate_alamouti_ofdm(
            SimulationConfig(
                modulation="qpsk",
                fft_size=32,
                cp_length=8,
                dc_null=True,
                frames=2,
                nt=antennas,
                nr=antennas,
                mode="spatial_multiplexing",
                layers=antennas,
                detector="zf",
                channel="rayleigh",
                snr_db=float("inf"),
                seed=1100 + antennas,
                channel_seed=1200 + antennas,
            )
        )
        assert result["bit_errors"] == 0
        assert result["crc_failures"] == 0
        assert result["mean_channel_rank"] == antennas


def test_spatial_multiplexing_zc_sync_recovers_timing_and_cfo() -> None:
    base = SimulationConfig(
        modulation="64qam",
        fft_size=64,
        cp_length=16,
        dc_null=True,
        pilot_spacing=2,
        preamble_enable=True,
        synchronization_enable=True,
        timing_search_samples=16,
        channel_estimation="ls_linear",
        frames=20,
        nt=2,
        nr=2,
        mode="spatial_multiplexing",
        layers=2,
        detector="mmse",
        channel="rayleigh",
        snr_db=40.0,
        seed=1301,
        channel_seed=1302,
        noise_seed=1303,
    )
    baseline = simulate_alamouti_ofdm(base)
    synchronized = simulate_alamouti_ofdm(
        replace(base, timing_offset_samples=7, cfo_hz=1200.0)
    )
    unsynchronized = simulate_alamouti_ofdm(
        replace(
            base,
            synchronization_enable=False,
            timing_offset_samples=7,
            cfo_hz=1200.0,
        )
    )

    assert synchronized["timing_success_rate"] == 1.0
    assert synchronized["mean_absolute_cfo_error_hz"] < 25.0
    assert synchronized["ber"] < 0.02
    assert synchronized["ber"] < unsynchronized["ber"]
    assert synchronized["goodput_bps"] >= 0.9 * baseline["goodput_bps"]


def test_antenna_correlation_worsens_conditioning_and_mmse_beats_zf_evm() -> None:
    base = SimulationConfig(
        modulation="64qam",
        fft_size=64,
        cp_length=16,
        dc_null=True,
        frames=100,
        nt=2,
        nr=2,
        mode="spatial_multiplexing",
        layers=2,
        channel="rayleigh",
        snr_db=25.0,
        seed=3301,
        channel_seed=3302,
        noise_seed=3303,
    )
    independent = simulate_alamouti_ofdm(replace(base, detector="mmse"))
    correlated_mmse = simulate_alamouti_ofdm(
        replace(
            base,
            detector="mmse",
            tx_correlation=0.95,
            rx_correlation=0.95,
        )
    )
    correlated_zf = simulate_alamouti_ofdm(
        replace(
            base,
            detector="zf",
            tx_correlation=0.95,
            rx_correlation=0.95,
        )
    )

    assert correlated_mmse["mean_channel_condition_number"] > 5.0 * independent[
        "mean_channel_condition_number"
    ]
    assert correlated_mmse["ber"] > independent["ber"]
    assert correlated_mmse["evm_rms"] < correlated_zf["evm_rms"]


def test_exact_rank_one_channel_is_reported_and_cannot_carry_two_layers() -> None:
    result = simulate_alamouti_ofdm(
        SimulationConfig(
            modulation="64qam",
            fft_size=64,
            cp_length=16,
            dc_null=True,
            frames=100,
            nt=2,
            nr=2,
            mode="spatial_multiplexing",
            layers=2,
            detector="mmse",
            channel="rayleigh",
            spatial_rank=1,
            snr_db=40.0,
            seed=3301,
            channel_seed=3302,
            noise_seed=3303,
        )
    )

    assert result["mean_channel_rank"] == 1.0
    assert result["rank_deficient_rate"] == 1.0
    assert result["ber"] > 0.25
    assert result["crc_failure_rate"] == 1.0


def test_spatial_sfo_closed_loop_resampling_restores_64qam_link() -> None:
    base = SimulationConfig(
        modulation="64qam",
        fft_size=256,
        cp_length=32,
        guard_left=16,
        guard_right=15,
        dc_null=True,
        pilot_spacing=4,
        preamble_enable=True,
        synchronization_enable=True,
        phase_reference_tracking_enable=True,
        phase_reference_count=16,
        pilot_phase_min_coherence=0.8,
        sfo_tracking_enable=True,
        sfo_resampling_interpolator="sinc24",
        sfo_ppm=500.0,
        channel_estimation="ls_linear",
        frames=40,
        nt=2,
        nr=2,
        mode="spatial_multiplexing",
        layers=2,
        detector="mmse",
        channel="rayleigh",
        snr_db=40.0,
        seed=23063,
    )
    phase_only = simulate_alamouti_ofdm(base)
    closed_loop = simulate_alamouti_ofdm(
        replace(base, sfo_resampling_enable=True)
    )

    assert closed_loop["sfo_resampling_application_rate"] == 1.0
    assert closed_loop["ber"] < 0.6 * phase_only["ber"]
    assert closed_loop["evm_rms"] < 0.85 * phase_only["evm_rms"]
    assert closed_loop["mean_absolute_residual_sfo_ppm"] < 100.0


def test_continuous_doppler_exposes_intrasymbol_ici_hidden_by_block_model() -> None:
    base = SimulationConfig(
        modulation="64qam",
        fft_size=64,
        cp_length=16,
        dc_null=True,
        frames=100,
        nt=2,
        nr=2,
        mode="spatial_multiplexing",
        layers=2,
        detector="mmse",
        channel="rayleigh",
        channel_estimation="perfect",
        snr_db=float("inf"),
        doppler_hz=3000.0,
        seed=8801,
    )
    block = simulate_alamouti_ofdm(replace(base, doppler_model="symbol"))
    continuous = simulate_alamouti_ofdm(
        replace(base, doppler_model="continuous")
    )

    assert block["bit_errors"] == 0
    assert block["intrasymbol_channel_variation_nmse"] == 0.0
    assert continuous["ber"] > 0.1
    assert continuous["evm_rms"] > 0.2
    assert continuous["intrasymbol_channel_variation_nmse"] > 0.5
    assert continuous["maximum_intrasymbol_phase_rotation_deg"] > 60.0
