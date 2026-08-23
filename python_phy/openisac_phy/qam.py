"""Square Gray-QAM matching ``include/QAM.hpp``."""

from __future__ import annotations

import math

import numpy as np


SUPPORTED_BITS_PER_SYMBOL = (2, 4, 6, 8)


def _validate(bits_per_symbol: int) -> None:
    if bits_per_symbol not in SUPPORTED_BITS_PER_SYMBOL:
        raise ValueError("square QAM supports bits_per_symbol=2,4,6,8 only")


def gray_to_binary(gray: np.ndarray) -> np.ndarray:
    gray = np.asarray(gray, dtype=np.int64)
    binary = gray.copy()
    shifted = gray.copy()
    while np.any(shifted):
        shifted >>= 1
        binary ^= shifted
    return binary


def labels_to_bits(labels: np.ndarray, bits_per_symbol: int) -> np.ndarray:
    """Expand integer labels to MSB-first bits on a new final axis."""

    _validate(bits_per_symbol)
    labels = np.asarray(labels, dtype=np.int64)
    if np.any(labels < 0) or np.any(labels >= (1 << bits_per_symbol)):
        raise ValueError("QAM label outside constellation")
    shifts = np.arange(bits_per_symbol - 1, -1, -1, dtype=np.int64)
    return ((labels[..., None] >> shifts) & 1).astype(np.uint8)


def bits_to_labels(bits: np.ndarray, bits_per_symbol: int) -> np.ndarray:
    """Pack MSB-first bits from the final axis into integer labels."""

    _validate(bits_per_symbol)
    bits = np.asarray(bits, dtype=np.uint8)
    if bits.shape[-1] % bits_per_symbol != 0:
        raise ValueError("final bit dimension is not divisible by bits_per_symbol")
    if np.any(bits > 1):
        raise ValueError("bits must contain only 0 and 1")
    grouped = bits.reshape(*bits.shape[:-1], -1, bits_per_symbol)
    weights = 1 << np.arange(bits_per_symbol - 1, -1, -1, dtype=np.int64)
    return np.sum(grouped * weights, axis=-1, dtype=np.int64)


def modulate(labels: np.ndarray, bits_per_symbol: int) -> np.ndarray:
    """Map integer labels to unit-average-power Gray-coded square QAM."""

    _validate(bits_per_symbol)
    labels = np.asarray(labels, dtype=np.int64)
    if np.any(labels < 0) or np.any(labels >= (1 << bits_per_symbol)):
        raise ValueError("QAM label outside constellation")
    axis_bits = bits_per_symbol // 2
    mask = (1 << axis_bits) - 1
    levels = 1 << axis_bits
    i_gray = labels >> axis_bits
    q_gray = labels & mask
    i_level = (levels - 1) - 2 * gray_to_binary(i_gray)
    q_level = (levels - 1) - 2 * gray_to_binary(q_gray)
    norm = math.sqrt((2.0 / 3.0) * ((1 << bits_per_symbol) - 1))
    return ((i_level + 1j * q_level) / norm).astype(np.complex128)


def hard_demodulate(samples: np.ndarray, bits_per_symbol: int) -> np.ndarray:
    """Nearest-neighbour hard decision, returning integer QAM labels."""

    _validate(bits_per_symbol)
    samples = np.asarray(samples, dtype=np.complex128)
    axis_bits = bits_per_symbol // 2
    levels = 1 << axis_bits
    gray = np.arange(levels, dtype=np.int64)
    norm = math.sqrt((2.0 / 3.0) * ((1 << bits_per_symbol) - 1))
    references = ((levels - 1) - 2 * gray_to_binary(gray)) / norm
    i_gray = np.argmin((samples.real[..., None] - references) ** 2, axis=-1)
    q_gray = np.argmin((samples.imag[..., None] - references) ** 2, axis=-1)
    return ((i_gray << axis_bits) | q_gray).astype(np.int64)


def max_log_llrs(
    samples: np.ndarray,
    noise_variance: float | np.ndarray,
    bits_per_symbol: int,
) -> np.ndarray:
    """Return max-log LLRs; positive means bit 0, matching C++ ``SquareQAM``."""

    _validate(bits_per_symbol)
    samples = np.asarray(samples, dtype=np.complex128)
    references = modulate(np.arange(1 << bits_per_symbol), bits_per_symbol)
    reference_bits = labels_to_bits(np.arange(1 << bits_per_symbol), bits_per_symbol)
    distances = np.abs(samples[..., None] - references) ** 2
    llrs = []
    for bit in range(bits_per_symbol):
        min_zero = np.min(distances[..., reference_bits[:, bit] == 0], axis=-1)
        min_one = np.min(distances[..., reference_bits[:, bit] == 1], axis=-1)
        llrs.append(min_one - min_zero)
    result = np.stack(llrs, axis=-1)
    variance = np.maximum(np.asarray(noise_variance, dtype=np.float64), 1.0e-15)
    return result / variance[..., None]
