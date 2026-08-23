import numpy as np

from openisac_phy.phase_reference import (
    correct_differential_phase,
    map_phase_references,
)


def test_repeated_phase_reference_recovers_differential_phase() -> None:
    rng = np.random.default_rng(301)
    tx_grid = np.zeros((8, 2, 16, 2), dtype=np.complex128)
    reference_indices = np.asarray([2, 7, 13])
    mapped = map_phase_references(tx_grid, reference_indices)
    channel = (
        rng.standard_normal((8, 16, 3))
        + 1j * rng.standard_normal((8, 16, 3))
    ) / np.sqrt(2.0)
    rx_grid = mapped[..., 0, None] * channel[:, None, :, :]
    phase = np.linspace(-0.4, 0.5, 8)
    rx_grid[:, 1] *= np.exp(1j * phase)[:, None, None]

    corrected, estimate = correct_differential_phase(
        rx_grid, reference_indices, minimum_coherence=0.9
    )

    np.testing.assert_allclose(estimate.differential_phase_rad, phase, atol=1.0e-12)
    np.testing.assert_allclose(estimate.coherence, 1.0, atol=1.0e-12)
    assert np.all(estimate.applied)
    np.testing.assert_allclose(corrected[:, 1], corrected[:, 0], atol=1.0e-12)


def test_phase_references_fit_and_correct_subcarrier_phase_slope() -> None:
    rng = np.random.default_rng(303)
    fft_size = 16
    centered = np.asarray([-6, -2, 3, 7])
    reference_indices = np.mod(centered, fft_size)
    tx_grid = np.zeros((5, 2, fft_size, 2), dtype=np.complex128)
    mapped = map_phase_references(tx_grid, reference_indices)
    channel = (
        rng.standard_normal((5, fft_size, 2))
        + 1j * rng.standard_normal((5, fft_size, 2))
    ) / np.sqrt(2.0)
    rx_grid = mapped[..., 0, None] * channel[:, None, :, :]
    intercept = np.linspace(-0.3, 0.4, 5)
    slope = np.linspace(-0.05, 0.06, 5)
    all_indices = np.arange(fft_size)
    all_centered = np.where(all_indices < fft_size // 2, all_indices, all_indices - fft_size)
    rx_grid[:, 1] *= np.exp(
        1j * (intercept[:, None] + slope[:, None] * all_centered[None, :])
    )[:, :, None]

    corrected, estimate = correct_differential_phase(
        rx_grid,
        reference_indices,
        minimum_coherence=0.9,
        phase_reference_centered_subcarriers=centered,
        slope_tracking=True,
        samples_per_symbol=20,
    )

    np.testing.assert_allclose(estimate.differential_phase_rad, intercept, atol=1.0e-12)
    np.testing.assert_allclose(
        estimate.phase_slope_rad_per_subcarrier, slope, atol=1.0e-12
    )
    np.testing.assert_allclose(estimate.coherence, 1.0, atol=1.0e-12)
    np.testing.assert_allclose(
        corrected[:, 1, reference_indices],
        corrected[:, 0, reference_indices],
        atol=1.0e-12,
    )
