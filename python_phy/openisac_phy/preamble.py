"""OpenISAC-compatible Zadoff-Chu OFDM synchronization preamble."""

from __future__ import annotations

import math

import numpy as np

from . import ofdm


def generate_zc_frequency(fft_size: int, zc_root: int) -> np.ndarray:
    """Generate the frequency-domain ZC sequence used by OpenISAC.

    This matches ``generate_zc_freq`` in the C++ design.  A root coprime to
    the FFT size is required so that the sequence retains its CAZAC property.
    """

    if fft_size <= 0:
        raise ValueError("fft_size must be positive")
    root = int(zc_root) % fft_size
    if root == 0 or math.gcd(root, fft_size) != 1:
        raise ValueError("zc_root must be non-zero and coprime to fft_size")
    index = np.arange(fft_size, dtype=np.float64)
    delta = fft_size & 1
    phase = -np.pi * root * index * (index + delta) / fft_size
    return np.exp(1j * phase)


def generate_zc_ofdm_symbol(
    fft_size: int,
    cp_length: int,
    zc_root: int,
) -> tuple[np.ndarray, np.ndarray]:
    """Return the frequency-domain ZC and its unit-power CP-OFDM symbol."""

    frequency = generate_zc_frequency(fft_size, zc_root)
    time = ofdm.modulate(frequency, cp_length)
    return frequency, time


def build_zc_preamble(
    frames: int,
    nt: int,
    fft_size: int,
    cp_length: int,
    zc_root: int,
) -> tuple[np.ndarray, np.ndarray]:
    """Map one ZC OFDM preamble to Tx0 and silence all other antennas.

    The time-domain return shape is ``[frame, 1, tx, sample]``.  Tx0-only
    transmission provides an unambiguous frame marker while the Alamouti comb
    pilots in the two following symbols continue to estimate both MIMO links.
    """

    if frames <= 0 or nt <= 0:
        raise ValueError("frames and nt must be positive")
    frequency, time = generate_zc_ofdm_symbol(fft_size, cp_length, zc_root)
    preamble = np.zeros(
        (frames, 1, nt, fft_size + cp_length), dtype=np.complex128
    )
    preamble[:, 0, 0, :] = time
    return frequency, preamble
