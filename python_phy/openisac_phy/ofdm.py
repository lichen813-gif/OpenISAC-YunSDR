"""Unitary CP-OFDM transforms used by the Python golden model."""

from __future__ import annotations

import math

import numpy as np


def modulate(resource_grid: np.ndarray, cp_length: int) -> np.ndarray:
    """IFFT the final axis and prepend a cyclic prefix."""

    resource_grid = np.asarray(resource_grid, dtype=np.complex128)
    fft_size = resource_grid.shape[-1]
    if fft_size <= 0 or not 0 <= cp_length <= fft_size:
        raise ValueError("invalid fft_size/cp_length")
    useful = np.fft.ifft(resource_grid, axis=-1) * math.sqrt(fft_size)
    if cp_length == 0:
        return useful
    return np.concatenate((useful[..., -cp_length:], useful), axis=-1)


def demodulate(samples: np.ndarray, fft_size: int, cp_length: int) -> np.ndarray:
    """Remove CP and FFT the final axis with unitary normalization."""

    samples = np.asarray(samples, dtype=np.complex128)
    if fft_size <= 0 or not 0 <= cp_length <= fft_size:
        raise ValueError("invalid fft_size/cp_length")
    if samples.shape[-1] != fft_size + cp_length:
        raise ValueError("sample length does not match fft_size + cp_length")
    useful = samples[..., cp_length:]
    return np.fft.fft(useful, axis=-1) / math.sqrt(fft_size)
