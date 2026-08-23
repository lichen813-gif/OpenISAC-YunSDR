import numpy as np

from openisac_phy.channel_estimation import (
    estimate_spatial_channel_from_fdm_pilots,
    interpolate_channel_dft_ls,
    interpolate_channel_linear,
    interpolate_channel_lmmse,
    ls_alamouti_channel_at_pilots,
)
from openisac_phy.resource_grid import deterministic_spatial_pilots
from openisac_phy.stbc import alamouti_encode_grid


def test_alamouti_ls_recovers_both_tx_channels_at_pilots() -> None:
    rng = np.random.default_rng(181)
    batches, pilots, nr = 6, 7, 3
    pilot_pairs = np.where(
        rng.integers(0, 2, size=(batches, pilots, 2)) == 0, 1.0, -1.0
    ).astype(np.complex128)
    tx_grid = alamouti_encode_grid(pilot_pairs)
    true_channel = (
        rng.standard_normal((batches, pilots, nr, 2))
        + 1j * rng.standard_normal((batches, pilots, nr, 2))
    ) / np.sqrt(2.0)
    rx_grid = np.einsum("btkx,bkrx->btkr", tx_grid, true_channel)

    estimated = ls_alamouti_channel_at_pilots(
        rx_grid, pilot_pairs, np.arange(pilots)
    )

    np.testing.assert_allclose(estimated, true_channel, atol=1.0e-12)


def test_linear_interpolation_preserves_flat_channel() -> None:
    rng = np.random.default_rng(183)
    flat = (
        rng.standard_normal((4, 1, 2, 2))
        + 1j * rng.standard_normal((4, 1, 2, 2))
    )
    pilot_channel = np.repeat(flat, 5, axis=1)
    estimated = interpolate_channel_linear(
        pilot_channel,
        np.asarray([-24, -12, 0, 12, 24]),
        fft_size=64,
    )

    np.testing.assert_allclose(estimated, np.repeat(flat, 64, axis=1), atol=1.0e-12)


def _channel_from_taps(taps: np.ndarray, fft_size: int) -> np.ndarray:
    return np.fft.fft(taps, n=fft_size, axis=1)


def test_dft_ls_exactly_reconstructs_finite_delay_channel() -> None:
    rng = np.random.default_rng(185)
    fft_size, channel_length = 64, 10
    taps = (
        rng.standard_normal((8, channel_length, 2, 2))
        + 1j * rng.standard_normal((8, channel_length, 2, 2))
    ) / np.sqrt(2.0 * channel_length)
    true_channel = _channel_from_taps(taps, fft_size)
    pilot_centered = np.arange(-32, 32, 2)
    pilot_indices = np.mod(pilot_centered, fft_size)

    estimated = interpolate_channel_dft_ls(
        true_channel[:, pilot_indices],
        pilot_centered,
        fft_size,
        channel_length,
    )

    np.testing.assert_allclose(estimated, true_channel, atol=1.0e-12)


def test_lmmse_reduces_noisy_interpolation_nmse() -> None:
    rng = np.random.default_rng(187)
    batches, fft_size, channel_length = 300, 64, 10
    taps = (
        rng.standard_normal((batches, channel_length, 1, 1))
        + 1j * rng.standard_normal((batches, channel_length, 1, 1))
    ) / np.sqrt(2.0 * channel_length)
    true_channel = _channel_from_taps(taps, fft_size)
    pilot_centered = np.arange(-32, 32, 4)
    pilot_indices = np.mod(pilot_centered, fft_size)
    noise_variance = 0.1
    sigma = np.sqrt(noise_variance / 2.0)
    noisy_pilots = true_channel[:, pilot_indices] + sigma * (
        rng.standard_normal((batches, pilot_centered.size, 1, 1))
        + 1j * rng.standard_normal((batches, pilot_centered.size, 1, 1))
    )

    linear = interpolate_channel_linear(noisy_pilots, pilot_centered, fft_size)
    lmmse = interpolate_channel_lmmse(
        noisy_pilots,
        pilot_centered,
        fft_size,
        channel_length,
        noise_variance,
    )
    linear_nmse = np.mean(np.abs(linear - true_channel) ** 2)
    lmmse_nmse = np.mean(np.abs(lmmse - true_channel) ** 2)

    assert lmmse_nmse < linear_nmse


def test_spatial_fdm_pilots_recover_flat_channel_each_symbol() -> None:
    rng = np.random.default_rng(191)
    batches, fft_size, nr = 4, 64, 2
    pilot_centered = np.arange(-28, 29, 4)
    pilot_indices = np.mod(pilot_centered, fft_size)
    pilot_grid, assignments = deterministic_spatial_pilots(
        batches, pilot_centered, 192
    )
    tx_grid = np.zeros((batches, 2, fft_size, 2), dtype=np.complex128)
    tx_grid[:, :, pilot_indices, :] = pilot_grid
    flat = (
        rng.standard_normal((batches, 2, 1, nr, 2))
        + 1j * rng.standard_normal((batches, 2, 1, nr, 2))
    ) / np.sqrt(2.0)
    true_channel = np.broadcast_to(flat, (batches, 2, fft_size, nr, 2)).copy()
    rx_grid = np.einsum("btkx,btkrx->btkr", tx_grid, true_channel)

    estimated, raw, recovered_assignments = estimate_spatial_channel_from_fdm_pilots(
        rx_grid,
        pilot_grid,
        pilot_indices,
        pilot_centered,
        fft_size,
        "ls_linear",
        10,
        0.0,
    )

    np.testing.assert_array_equal(recovered_assignments, assignments)
    np.testing.assert_allclose(estimated, true_channel, atol=1.0e-12)
    assert raw.shape == (batches, 2, pilot_centered.size, nr)
