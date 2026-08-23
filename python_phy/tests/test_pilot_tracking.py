import numpy as np

from openisac_phy.pilot_tracking import (
    correct_common_phase,
    phase_difference_to_cfo_hz,
)


def test_comb_pilots_remove_known_common_phase() -> None:
    rng = np.random.default_rng(151)
    batches, subcarriers, nr, nt = 5, 16, 2, 2
    tx_grid = (
        rng.standard_normal((batches, 2, subcarriers, nt))
        + 1j * rng.standard_normal((batches, 2, subcarriers, nt))
    )
    channel = (
        rng.standard_normal((batches, subcarriers, nr, nt))
        + 1j * rng.standard_normal((batches, subcarriers, nr, nt))
    ) / np.sqrt(2.0)
    clean = np.einsum("btkx,bkrx->btkr", tx_grid, channel)
    applied_phase = np.asarray(
        [[0.15, -0.21], [0.33, 0.48], [-0.27, 0.09], [0.01, 0.18], [-0.4, -0.12]]
    )
    impaired = clean * np.exp(1j * applied_phase)[:, :, None, None]

    corrected, estimate = correct_common_phase(
        impaired, tx_grid, channel, np.arange(0, subcarriers, 2)
    )

    np.testing.assert_allclose(estimate.phase_rad, applied_phase, atol=1.0e-12)
    np.testing.assert_allclose(estimate.coherence, 1.0, atol=1.0e-12)
    np.testing.assert_allclose(corrected, clean, atol=1.0e-12)


def test_pilot_phase_difference_reports_residual_cfo() -> None:
    sample_rate_hz = 960_000.0
    samples_per_symbol = 80
    expected_cfo_hz = np.asarray([120.0, -75.0])
    delta = 2.0 * np.pi * expected_cfo_hz * samples_per_symbol / sample_rate_hz
    phases = np.stack((np.asarray([0.2, -0.1]), np.asarray([0.2, -0.1]) + delta), axis=1)
    estimated = phase_difference_to_cfo_hz(
        phases,
        sample_rate_hz=sample_rate_hz,
        samples_per_symbol=samples_per_symbol,
    )
    np.testing.assert_allclose(estimated, expected_cfo_hz, atol=1.0e-12)


def test_low_coherence_phase_estimate_is_not_applied() -> None:
    rng = np.random.default_rng(157)
    tx_grid = rng.standard_normal((2, 2, 8, 2)) + 1j * rng.standard_normal((2, 2, 8, 2))
    channel = rng.standard_normal((2, 8, 1, 2)) + 1j * rng.standard_normal((2, 8, 1, 2))
    unrelated_rx = rng.standard_normal((2, 2, 8, 1)) + 1j * rng.standard_normal((2, 2, 8, 1))

    corrected, estimate = correct_common_phase(
        unrelated_rx,
        tx_grid,
        channel,
        np.arange(8),
        minimum_coherence=1.0,
    )

    assert not np.any(estimate.applied)
    np.testing.assert_array_equal(corrected, unrelated_rx)
