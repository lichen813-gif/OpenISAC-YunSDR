#!/usr/bin/env python3
"""Analyze interleaved int16 IQ samples produced by YunSDR test programs."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import numpy as np


def db20(value: np.ndarray | float, floor: float = 1e-15):
    return 20.0 * np.log10(np.maximum(np.asarray(value), floor))


def max_pool_spectrum(freq_hz: np.ndarray, level_dbfs: np.ndarray, bins: int):
    if len(freq_hz) <= bins:
        return [[round(float(f / 1e6), 6), round(float(v), 3)] for f, v in zip(freq_hz, level_dbfs)]

    edges = np.linspace(0, len(freq_hz), bins + 1, dtype=int)
    result = []
    for start, stop in zip(edges[:-1], edges[1:]):
        local = level_dbfs[start:stop]
        index = start + int(np.argmax(local))
        result.append([round(float(freq_hz[index] / 1e6), 6), round(float(level_dbfs[index]), 3)])
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("iq_file", type=Path)
    parser.add_argument("--sample-rate", type=float, default=15.36e6)
    parser.add_argument("--expected-tone", type=float, default=1.0e6)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    raw = np.fromfile(args.iq_file, dtype="<i2")
    if raw.size < 4 or raw.size % 2:
        raise ValueError(f"invalid interleaved int16 IQ file: {args.iq_file}")

    i_data = raw[0::2].astype(np.float64)
    q_data = raw[1::2].astype(np.float64)
    samples = i_data + 1j * q_data
    count = samples.size

    dc = np.mean(samples)
    centered = samples - dc
    window = np.hanning(count)
    spectrum = np.fft.fftshift(np.fft.fft(centered * window))
    freq_hz = np.fft.fftshift(np.fft.fftfreq(count, d=1.0 / args.sample_rate))
    amplitude = np.abs(spectrum) / (np.sum(window) * 32768.0)
    level_dbfs = db20(amplitude)

    dc_guard_hz = max(args.sample_rate / count * 8.0, 5_000.0)
    valid_peak = np.abs(freq_hz) > dc_guard_hz
    peak_index = int(np.argmax(np.where(valid_peak, level_dbfs, -np.inf)))
    peak_hz = float(freq_hz[peak_index])

    # Three-bin parabolic interpolation gives a sub-bin frequency estimate.
    if 0 < peak_index < count - 1:
        y0, y1, y2 = level_dbfs[peak_index - 1 : peak_index + 2]
        denom = y0 - 2.0 * y1 + y2
        delta = 0.5 * (y0 - y2) / denom if abs(denom) > 1e-12 else 0.0
        delta = float(np.clip(delta, -0.5, 0.5))
    else:
        delta = 0.0
    bin_hz = args.sample_rate / count
    peak_interp_hz = peak_hz + delta * bin_hz

    n = np.arange(count, dtype=np.float64)
    reference = np.exp(1j * 2.0 * np.pi * peak_interp_hz * n / args.sample_rate)
    tone_amplitude = np.mean(centered * np.conj(reference))
    residual = centered - tone_amplitude * reference
    tone_power = float(abs(tone_amplitude) ** 2)
    residual_power = float(np.mean(np.abs(residual) ** 2))
    snr_db = 10.0 * math.log10(max(tone_power, 1e-30) / max(residual_power, 1e-30))

    exclusion_hz = max(25_000.0, bin_hz * 20.0)
    noise_mask = (
        (np.abs(freq_hz - peak_interp_hz) > exclusion_hz)
        & (np.abs(freq_hz) > dc_guard_hz)
    )
    noise_floor_dbfs = float(np.median(level_dbfs[noise_mask]))

    image_index = int(np.argmin(np.abs(freq_hz + peak_interp_hz)))
    image_level_dbfs = float(level_dbfs[image_index])
    peak_level_dbfs = float(level_dbfs[peak_index])
    image_rejection_db = peak_level_dbfs - image_level_dbfs

    peak_abs_component = int(max(np.max(np.abs(i_data)), np.max(np.abs(q_data))))
    clipped_components = int(np.count_nonzero((np.abs(i_data) >= 32760) | (np.abs(q_data) >= 32760)))

    zoom_mask = np.abs(freq_hz - args.expected_tone) <= 0.25e6
    payload = {
        "input_file": str(args.iq_file.resolve()),
        "sample_count": int(count),
        "sample_rate_hz": float(args.sample_rate),
        "bin_width_hz": float(bin_hz),
        "expected_tone_hz": float(args.expected_tone),
        "peak_bin_hz": peak_hz,
        "peak_estimated_hz": float(peak_interp_hz),
        "frequency_error_hz": float(peak_interp_hz - args.expected_tone),
        "peak_level_dbfs": peak_level_dbfs,
        "noise_floor_dbfs": noise_floor_dbfs,
        "tone_to_residual_db": float(snr_db),
        "image_level_dbfs": image_level_dbfs,
        "image_rejection_db": float(image_rejection_db),
        "dc_magnitude_dbfs": float(db20(abs(dc) / 32768.0)),
        "rms_magnitude_dbfs": float(db20(np.sqrt(np.mean(np.abs(samples) ** 2)) / 32768.0)),
        "max_component": peak_abs_component,
        "clipped_sample_count": clipped_components,
        "full_spectrum": max_pool_spectrum(freq_hz, level_dbfs, 2048),
        "tone_zoom": max_pool_spectrum(freq_hz[zoom_mask], level_dbfs[zoom_mask], 1001),
    }

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({key: value for key, value in payload.items() if key not in ("full_spectrum", "tone_zoom")}, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
