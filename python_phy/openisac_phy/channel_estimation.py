"""Pilot-aided MIMO channel estimation for the Python PHY model."""

from __future__ import annotations

import numpy as np

from .stbc import TX_SCALE


def ls_alamouti_channel_at_pilots(
    rx_grid: np.ndarray,
    pilot_pairs: np.ndarray,
    pilot_indices: np.ndarray,
) -> np.ndarray:
    """Estimate both Tx channels independently on every pilot subcarrier.

    The known Alamouti pilot matrix is orthogonal across the two OFDM time
    slots, so its Hermitian transpose gives the LS solution. The returned
    array is ``[batch, pilot, rx, tx]`` and does not use the true channel.
    """

    rx_grid = np.asarray(rx_grid, dtype=np.complex128)
    pilot_pairs = np.asarray(pilot_pairs, dtype=np.complex128)
    pilot_indices = np.asarray(pilot_indices, dtype=np.int64)
    if rx_grid.ndim != 4 or rx_grid.shape[1] != 2:
        raise ValueError("rx_grid must have shape [batch, 2, subcarrier, rx]")
    if pilot_pairs.shape != (rx_grid.shape[0], pilot_indices.size, 2):
        raise ValueError(
            "pilot_pairs must have shape [batch, number_of_pilots, 2]"
        )
    if pilot_indices.size == 0:
        raise ValueError("LS channel estimation requires at least one pilot")
    if np.any((pilot_indices < 0) | (pilot_indices >= rx_grid.shape[2])):
        raise ValueError("pilot index is outside the resource grid")

    y0 = rx_grid[:, 0, pilot_indices, :]
    y1 = rx_grid[:, 1, pilot_indices, :]
    p0 = pilot_pairs[..., 0, None]
    p1 = pilot_pairs[..., 1, None]
    denominator = TX_SCALE**2 * (
        np.abs(p0) ** 2 + np.abs(p1) ** 2
    )
    if np.any(denominator <= 1.0e-15):
        raise ValueError("pilot pair has zero LS estimation energy")

    h0 = TX_SCALE * (np.conj(p0) * y0 - p1 * y1) / denominator
    h1 = TX_SCALE * (np.conj(p1) * y0 + p0 * y1) / denominator
    return np.stack((h0, h1), axis=-1)


def interpolate_channel_linear(
    pilot_channel: np.ndarray,
    pilot_centered_subcarriers: np.ndarray,
    fft_size: int,
) -> np.ndarray:
    """Linearly interpolate complex LS estimates over the full FFT grid.

    Interpolation is performed in FFT-shifted frequency order, independently
    on real and imaginary parts. The pilot sequence is extended by one FFT
    period at each edge so interpolation respects the periodic OFDM spectrum.
    The output uses native FFT indexing and has shape
    ``[batch, subcarrier, rx, tx]``.
    """

    pilot_channel = np.asarray(pilot_channel, dtype=np.complex128)
    pilot_centered = np.asarray(pilot_centered_subcarriers, dtype=np.int64)
    if pilot_channel.ndim != 4:
        raise ValueError("pilot_channel must have shape [batch, pilot, rx, tx]")
    if fft_size <= 0 or fft_size % 2 != 0:
        raise ValueError("fft_size must be a positive even integer")
    if pilot_centered.shape != (pilot_channel.shape[1],):
        raise ValueError("one centered subcarrier index is required per pilot")
    if pilot_centered.size == 0:
        raise ValueError("at least one pilot is required for interpolation")
    if np.any(np.diff(pilot_centered) <= 0):
        raise ValueError("pilot centered subcarriers must be strictly increasing")
    if np.any((pilot_centered < -fft_size // 2) | (pilot_centered >= fft_size // 2)):
        raise ValueError("pilot centered subcarrier is outside the FFT grid")

    centered_axis = np.arange(-fft_size // 2, fft_size // 2, dtype=np.int64)
    centered_channel = np.empty(
        (pilot_channel.shape[0], fft_size, pilot_channel.shape[2], pilot_channel.shape[3]),
        dtype=np.complex128,
    )
    for batch in range(pilot_channel.shape[0]):
        for rx in range(pilot_channel.shape[2]):
            for tx in range(pilot_channel.shape[3]):
                values = pilot_channel[batch, :, rx, tx]
                extended_subcarriers = np.concatenate(
                    (
                        pilot_centered[-1:] - fft_size,
                        pilot_centered,
                        pilot_centered[:1] + fft_size,
                    )
                )
                extended_values = np.concatenate((values[-1:], values, values[:1]))
                centered_channel[batch, :, rx, tx] = np.interp(
                    centered_axis, extended_subcarriers, extended_values.real
                ) + 1j * np.interp(
                    centered_axis, extended_subcarriers, extended_values.imag
                )

    native_indices = np.mod(centered_axis, fft_size)
    native_channel = np.empty_like(centered_channel)
    native_channel[:, native_indices, :, :] = centered_channel
    return native_channel


def _frequency_basis(
    subcarriers: np.ndarray,
    fft_size: int,
    channel_length: int,
) -> np.ndarray:
    delays = np.arange(channel_length, dtype=np.float64)
    return np.exp(
        -1j
        * 2.0
        * np.pi
        * np.asarray(subcarriers, dtype=np.float64)[:, None]
        * delays[None, :]
        / fft_size
    )


def interpolate_channel_dft_ls(
    pilot_channel: np.ndarray,
    pilot_centered_subcarriers: np.ndarray,
    fft_size: int,
    channel_length: int,
) -> np.ndarray:
    """Fit a finite impulse response to pilot LS estimates and reconstruct H."""

    pilot_channel = np.asarray(pilot_channel, dtype=np.complex128)
    pilot_centered = np.asarray(pilot_centered_subcarriers, dtype=np.int64)
    if pilot_channel.ndim != 4:
        raise ValueError("pilot_channel must have shape [batch, pilot, rx, tx]")
    if pilot_centered.shape != (pilot_channel.shape[1],):
        raise ValueError("one centered subcarrier index is required per pilot")
    if not 1 <= channel_length <= fft_size:
        raise ValueError("channel_length must be in [1, fft_size]")
    if pilot_centered.size < channel_length:
        raise ValueError("DFT-LS requires at least channel_length pilot tones")

    centered_axis = np.arange(-fft_size // 2, fft_size // 2, dtype=np.int64)
    pilot_basis = _frequency_basis(pilot_centered, fft_size, channel_length)
    full_basis = _frequency_basis(centered_axis, fft_size, channel_length)
    basis_pinv = np.linalg.pinv(pilot_basis, rcond=1.0e-12)
    impulse_response = np.einsum(
        "lp,bprt->blrt", basis_pinv, pilot_channel, optimize=True
    )
    centered_channel = np.einsum(
        "kl,blrt->bkrt", full_basis, impulse_response, optimize=True
    )
    native_indices = np.mod(centered_axis, fft_size)
    native_channel = np.empty_like(centered_channel)
    native_channel[:, native_indices, :, :] = centered_channel
    return native_channel


def interpolate_channel_lmmse(
    pilot_channel: np.ndarray,
    pilot_centered_subcarriers: np.ndarray,
    fft_size: int,
    channel_length: int,
    noise_variance: float,
) -> np.ndarray:
    """LMMSE-interpolate pilot LS estimates using a uniform finite-delay PDP."""

    pilot_channel = np.asarray(pilot_channel, dtype=np.complex128)
    pilot_centered = np.asarray(pilot_centered_subcarriers, dtype=np.int64)
    if pilot_channel.ndim != 4:
        raise ValueError("pilot_channel must have shape [batch, pilot, rx, tx]")
    if pilot_centered.shape != (pilot_channel.shape[1],):
        raise ValueError("one centered subcarrier index is required per pilot")
    if pilot_centered.size == 0:
        raise ValueError("LMMSE interpolation requires pilot tones")
    if not 1 <= channel_length <= fft_size:
        raise ValueError("channel_length must be in [1, fft_size]")
    if not np.isfinite(noise_variance) or noise_variance < 0:
        raise ValueError("noise_variance must be non-negative and finite")

    centered_axis = np.arange(-fft_size // 2, fft_size // 2, dtype=np.int64)
    pilot_basis = _frequency_basis(pilot_centered, fft_size, channel_length)
    full_basis = _frequency_basis(centered_axis, fft_size, channel_length)
    # Unit total channel power and a uniform power-delay profile provide a
    # deterministic baseline prior. A configured/estimated PDP can replace it
    # later without changing the interpolation interface.
    tap_covariance = np.eye(channel_length, dtype=np.complex128) / channel_length
    cross_covariance = full_basis @ tap_covariance @ np.conj(pilot_basis.T)
    pilot_covariance = pilot_basis @ tap_covariance @ np.conj(pilot_basis.T)
    regularized = pilot_covariance + noise_variance * np.eye(
        pilot_centered.size, dtype=np.complex128
    )
    weights = cross_covariance @ np.linalg.pinv(
        regularized, rcond=1.0e-12, hermitian=True
    )
    centered_channel = np.einsum(
        "kp,bprt->bkrt", weights, pilot_channel, optimize=True
    )
    native_indices = np.mod(centered_axis, fft_size)
    native_channel = np.empty_like(centered_channel)
    native_channel[:, native_indices, :, :] = centered_channel
    return native_channel


def estimate_spatial_channel_from_fdm_pilots(
    rx_grid: np.ndarray,
    pilot_grid: np.ndarray,
    pilot_indices: np.ndarray,
    pilot_centered_subcarriers: np.ndarray,
    fft_size: int,
    method: str,
    channel_length: int,
    noise_variance: float,
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Estimate per-symbol MIMO CSI from frequency-orthogonal pilots.

    Each pilot tone must contain exactly one non-zero transmit antenna. The
    returned channel is ``[batch,time,subcarrier,rx,tx]``; raw LS estimates
    are ``[batch,time,pilot,rx]`` with a matching Tx assignment vector.
    """

    received = np.asarray(rx_grid, dtype=np.complex128)
    pilots = np.asarray(pilot_grid, dtype=np.complex128)
    indices = np.asarray(pilot_indices, dtype=np.int64)
    centered = np.asarray(pilot_centered_subcarriers, dtype=np.int64)
    if received.ndim != 4 or received.shape[1] != 2:
        raise ValueError("rx_grid must have shape [batch,2,subcarrier,rx]")
    if pilots.ndim != 4 or pilots.shape[:3] != (
        received.shape[0],
        2,
        indices.size,
    ) or pilots.shape[-1] < 2:
        raise ValueError(
            "pilot_grid must have shape [batch,2,pilot,nt>=2]"
        )
    if centered.shape != indices.shape or indices.size == 0:
        raise ValueError("pilot indices and centered positions must be non-empty and match")
    if method not in {"ls_linear", "ls_dft", "lmmse"}:
        raise ValueError("unsupported spatial channel-estimation method")

    active = np.any(np.abs(pilots) > 1.0e-15, axis=(0, 1))
    if np.any(np.sum(active, axis=-1) != 1):
        raise ValueError("each spatial pilot tone must be assigned to exactly one Tx")
    assignments = np.argmax(active, axis=-1)
    raw = np.empty(
        (received.shape[0], 2, indices.size, received.shape[-1]),
        dtype=np.complex128,
    )
    nt = pilots.shape[-1]
    full = np.empty(
        (received.shape[0], 2, fft_size, received.shape[-1], nt),
        dtype=np.complex128,
    )
    received_pilots = received[:, :, indices, :]
    for tx in range(nt):
        mask = assignments == tx
        if not np.any(mask):
            raise ValueError(f"spatial pilots contain no tones for Tx{tx}")
        known = pilots[:, :, mask, tx]
        estimates = received_pilots[:, :, mask, :] / known[..., None]
        raw[:, :, mask, :] = estimates
        flattened = estimates.reshape(
            received.shape[0] * 2, np.count_nonzero(mask), received.shape[-1], 1
        )
        tx_centered = centered[mask]
        if method == "ls_linear":
            interpolated = interpolate_channel_linear(
                flattened, tx_centered, fft_size
            )
        elif method == "ls_dft":
            interpolated = interpolate_channel_dft_ls(
                flattened, tx_centered, fft_size, channel_length
            )
        else:
            interpolated = interpolate_channel_lmmse(
                flattened,
                tx_centered,
                fft_size,
                channel_length,
                noise_variance,
            )
        full[..., tx] = interpolated[..., 0].reshape(
            received.shape[0], 2, fft_size, received.shape[-1]
        )
    return full, raw, assignments
