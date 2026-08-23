"""Alamouti space-frequency block coding on adjacent OFDM subcarriers."""

from __future__ import annotations

import numpy as np

from .stbc import TX_SCALE


def alamouti_sfbc_encode_grid(
    symbol_pairs: np.ndarray,
    data_indices: np.ndarray,
    fft_size: int,
) -> np.ndarray:
    """Map ``[batch, data_subcarrier, 2]`` symbols to a two-symbol SFBC grid.

    For each OFDM symbol and adjacent frequency pair ``(k0, k1)`` the two-Tx
    matrix is ``[[s0, s1], [-conj(s1), conj(s0)]]``.  The final dimension of
    ``symbol_pairs`` therefore remains the pair of information symbols carried
    by one adjacent-frequency Alamouti word.
    """

    symbols = np.asarray(symbol_pairs, dtype=np.complex128)
    indices = np.asarray(data_indices, dtype=np.int64)
    if symbols.ndim != 3 or symbols.shape[-1] != 2:
        raise ValueError("symbol_pairs must have shape [batch, data_subcarrier, 2]")
    if indices.ndim != 1 or symbols.shape[1] != indices.size:
        raise ValueError("data_indices must match the data-subcarrier dimension")
    if indices.size % 2:
        raise ValueError("SFBC requires an even number of data subcarriers")
    if fft_size <= 0 or np.any(indices < 0) or np.any(indices >= fft_size):
        raise ValueError("data_indices must be valid native FFT indices")

    grid = np.zeros((symbols.shape[0], 2, fft_size, 2), dtype=np.complex128)
    for offset in range(0, indices.size, 2):
        k0 = indices[offset]
        k1 = indices[offset + 1]
        # Native FFT indices wrap at DC, so adjacency is checked modulo N.
        if k1 != (k0 + 1) % fft_size:
            raise ValueError("each SFBC frequency pair must use adjacent subcarriers")
        s0 = symbols[:, offset, :]
        s1 = symbols[:, offset + 1, :]
        grid[:, :, k0, 0] = s0
        grid[:, :, k0, 1] = s1
        grid[:, :, k1, 0] = -np.conj(s1)
        grid[:, :, k1, 1] = np.conj(s0)
    return TX_SCALE * grid


def alamouti_sfbc_combine_grid(
    rx_grid: np.ndarray,
    channel_by_symbol: np.ndarray,
    data_indices: np.ndarray,
    noise_variance: float | np.ndarray,
) -> tuple[np.ndarray, np.ndarray]:
    """Combine adjacent-frequency Alamouti words for both OFDM symbols.

    The conventional orthogonal combiner uses the first tone's channel for
    both tones in a pair.  Its resulting degradation therefore measures the
    expected SFBC sensitivity when the channel is not flat across adjacent
    subcarriers.
    """

    received = np.asarray(rx_grid, dtype=np.complex128)
    channel = np.asarray(channel_by_symbol, dtype=np.complex128)
    indices = np.asarray(data_indices, dtype=np.int64)
    if received.ndim != 4 or received.shape[1] != 2:
        raise ValueError("rx_grid must have shape [batch, 2, subcarrier, nr]")
    expected = (*received.shape, 2)
    if channel.shape != expected:
        raise ValueError(f"channel_by_symbol must have shape {expected}, got {channel.shape}")
    if indices.ndim != 1 or indices.size % 2:
        raise ValueError("data_indices must contain adjacent frequency pairs")

    output = np.empty((received.shape[0], indices.size, 2), dtype=np.complex128)
    variance_output = np.empty((received.shape[0], indices.size, 2), dtype=np.float64)
    input_variance = np.asarray(noise_variance, dtype=np.float64)
    for offset in range(0, indices.size, 2):
        k0 = indices[offset]
        k1 = indices[offset + 1]
        if k1 != (k0 + 1) % received.shape[2]:
            raise ValueError("each SFBC frequency pair must use adjacent subcarriers")
        y0 = received[:, :, k0, :]
        y1 = received[:, :, k1, :]
        h0 = channel[:, :, k0, :, 0]
        h1 = channel[:, :, k0, :, 1]
        gain = np.sum(np.abs(h0) ** 2 + np.abs(h1) ** 2, axis=-1)
        if np.any(gain <= 1.0e-15):
            raise ValueError("channel has zero SFBC combining gain")
        z0 = np.sum(np.conj(h0) * y0 + h1 * np.conj(y1), axis=-1)
        z1 = np.sum(np.conj(h1) * y0 - h0 * np.conj(y1), axis=-1)
        decoded = np.stack((z0, z1), axis=-1) / (TX_SCALE * gain[..., None])
        # Each data tone carries one Alamouti word during each time symbol.
        output[:, offset, :] = decoded[:, :, 0]
        output[:, offset + 1, :] = decoded[:, :, 1]
        equivalent = np.broadcast_to(input_variance, gain.shape) / (TX_SCALE**2 * gain)
        variance_output[:, offset, :] = equivalent
        variance_output[:, offset + 1, :] = equivalent
    return output, variance_output
