"""Sampling-frequency-offset impairment and diagnostic conversion helpers."""

from __future__ import annotations

import numpy as np


def apply_sampling_frequency_offset(
    stream: np.ndarray,
    sfo_ppm: float | np.ndarray,
    method: str = "sinc24",
) -> np.ndarray:
    """Resample a continuous complex stream with a receiver clock offset.

    ``sfo_ppm = (F_rx - F_tx) / F_tx * 1e6``.  Receiver sample ``m`` observes
    the transmitted waveform at source index ``m / (1 + sfo)``.  A
    ``method='sinc24'`` is the high-accuracy impairment/reference model.
    ``method='sinc8'`` is the real-time receiver baseline and maps directly
    to an eight-tap polyphase fractional-delay FIR in C++.
    ``method='cubic'`` is a four-point Lagrange interpolator intended for the
    real-time C++ receiver: fixed four-sample support, no coefficient table,
    and no large resampling buffer.
    """

    stream = np.asarray(stream, dtype=np.complex128)
    if stream.ndim != 3:
        raise ValueError("stream must have shape [batch, rx, sample]")
    if method not in {"sinc24", "sinc8", "cubic"}:
        raise ValueError(
            "sampling-offset interpolation must be sinc24, sinc8 or cubic"
        )
    offsets = np.asarray(sfo_ppm, dtype=np.float64)
    if offsets.ndim == 0:
        offsets = np.full(stream.shape[0], float(offsets), dtype=np.float64)
    if offsets.shape != (stream.shape[0],):
        raise ValueError("sfo_ppm must be scalar or have one value per batch")
    if np.any(~np.isfinite(offsets)):
        raise ValueError("sfo_ppm must be finite")
    ratios = 1.0 + offsets * 1.0e-6
    if np.any(ratios <= 0.0):
        raise ValueError("sfo_ppm produces a non-positive receive sample rate")
    if np.all(offsets == 0.0):
        return stream.copy()

    sample_count = stream.shape[-1]
    resampled = np.empty_like(stream)
    for batch in range(stream.shape[0]):
        source_positions = np.arange(sample_count, dtype=np.float64) / ratios[batch]
        center = np.floor(source_positions).astype(np.int64)
        if method == "cubic":
            fraction = source_positions - center
            source_indices = center[:, None] + np.array(
                [-1, 0, 1, 2], dtype=np.int64
            )[None, :]
            weights = np.column_stack(
                (
                    -fraction * (1.0 - fraction) * (2.0 - fraction) / 6.0,
                    (1.0 + fraction)
                    * (1.0 - fraction)
                    * (2.0 - fraction)
                    / 2.0,
                    (1.0 + fraction)
                    * fraction
                    * (2.0 - fraction)
                    / 2.0,
                    -(1.0 + fraction)
                    * fraction
                    * (1.0 - fraction)
                    / 6.0,
                )
            )
        else:
            half_length = 12 if method == "sinc24" else 4
            tap_offsets = np.arange(
                -half_length + 1, half_length + 1, dtype=np.int64
            )
            source_indices = center[:, None] + tap_offsets[None, :]
            distance = source_positions[:, None] - source_indices
            weights = np.sinc(distance) * np.sinc(distance / half_length)
        valid = (source_indices >= 0) & (source_indices < sample_count)
        clipped_indices = np.clip(source_indices, 0, sample_count - 1)
        weights *= valid
        for rx in range(stream.shape[1]):
            samples = stream[batch, rx]
            resampled[batch, rx] = np.sum(
                samples[clipped_indices] * weights, axis=1
            )
    return resampled


def inverse_sampling_offset_ppm(sfo_ppm: float | np.ndarray) -> np.ndarray:
    """Return the exact resampling offset that inverts a measured clock ratio."""

    offsets = np.asarray(sfo_ppm, dtype=np.float64)
    if np.any(~np.isfinite(offsets)):
        raise ValueError("sfo_ppm must be finite")
    ratio = 1.0 + offsets * 1.0e-6
    if np.any(ratio <= 0.0):
        raise ValueError("sfo_ppm produces a non-positive receive sample rate")
    return (1.0 / ratio - 1.0) * 1.0e6


def phase_slope_to_sfo_ppm(
    phase_slope_rad_per_subcarrier: np.ndarray,
    fft_size: int,
    samples_per_symbol: int,
) -> np.ndarray:
    """Convert inter-symbol phase slope to the configured SFO convention."""

    if fft_size <= 0 or samples_per_symbol <= 0:
        raise ValueError("FFT and samples_per_symbol must be positive")
    slope = np.asarray(phase_slope_rad_per_subcarrier, dtype=np.float64)
    return -slope * fft_size / (2.0 * np.pi * samples_per_symbol) * 1.0e6
