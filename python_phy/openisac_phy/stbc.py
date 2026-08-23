"""Alamouti space-time block coding on an OFDM resource grid."""

from __future__ import annotations

import math

import numpy as np


TX_SCALE = 1.0 / math.sqrt(2.0)


def alamouti_encode_grid(symbol_pairs: np.ndarray) -> np.ndarray:
    """Encode ``[batch, subcarrier, 2]`` symbols to ``[batch, time, subcarrier, tx]``.

    The two time slots are ``[[s0, s1], [-conj(s1), conj(s0)]]``. A
    1/sqrt(2) factor keeps total radiated power equal to the SISO reference.
    """

    symbol_pairs = np.asarray(symbol_pairs, dtype=np.complex128)
    if symbol_pairs.ndim != 3 or symbol_pairs.shape[-1] != 2:
        raise ValueError("symbol_pairs must have shape [batch, subcarrier, 2]")
    s0 = symbol_pairs[..., 0]
    s1 = symbol_pairs[..., 1]
    grid = np.empty((symbol_pairs.shape[0], 2, symbol_pairs.shape[1], 2), dtype=np.complex128)
    grid[:, 0, :, 0] = s0
    grid[:, 0, :, 1] = s1
    grid[:, 1, :, 0] = -np.conj(s1)
    grid[:, 1, :, 1] = np.conj(s0)
    return TX_SCALE * grid


def alamouti_combine_grid(
    rx_grid: np.ndarray,
    channel: np.ndarray,
    noise_variance: float | np.ndarray,
) -> tuple[np.ndarray, np.ndarray]:
    """Combine a time-paired Alamouti grid using perfect or estimated CSI.

    Args:
        rx_grid: ``[batch, 2, subcarrier, nr]``.
        channel: ``[batch, subcarrier, nr, 2]``, constant over the pair.
        noise_variance: E[|n|^2] before combining, scalar or broadcastable to
            ``[batch, subcarrier]``.

    Returns:
        Equalized symbols ``[batch, subcarrier, 2]`` and their equivalent
        complex noise variance ``[batch, subcarrier]``.
    """

    rx_grid = np.asarray(rx_grid, dtype=np.complex128)
    channel = np.asarray(channel, dtype=np.complex128)
    if rx_grid.ndim != 4 or rx_grid.shape[1] != 2:
        raise ValueError("rx_grid must have shape [batch, 2, subcarrier, nr]")
    expected = (rx_grid.shape[0], rx_grid.shape[2], rx_grid.shape[3], 2)
    if channel.shape != expected:
        raise ValueError(f"channel must have shape {expected}, got {channel.shape}")

    y0 = rx_grid[:, 0]
    y1 = rx_grid[:, 1]
    h0 = channel[..., 0]
    h1 = channel[..., 1]
    gain = np.sum(np.abs(h0) ** 2 + np.abs(h1) ** 2, axis=-1)
    if np.any(gain <= 1.0e-15):
        raise ValueError("channel has zero Alamouti combining gain")

    z0 = np.sum(np.conj(h0) * y0 + h1 * np.conj(y1), axis=-1)
    z1 = np.sum(np.conj(h1) * y0 - h0 * np.conj(y1), axis=-1)
    denominator = TX_SCALE * gain
    symbols = np.stack((z0 / denominator, z1 / denominator), axis=-1)

    variance = np.asarray(noise_variance, dtype=np.float64)
    equivalent_variance = np.broadcast_to(variance, gain.shape) / (TX_SCALE**2 * gain)
    return symbols, equivalent_variance

