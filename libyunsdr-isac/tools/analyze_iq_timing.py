#!/usr/bin/env python3
"""Find and characterize the first magnitude threshold crossing in int16 IQ data."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np


def first_run(mask: np.ndarray, length: int) -> int | None:
    if mask.size < length:
        return None
    runs = np.convolve(mask.astype(np.int32), np.ones(length, dtype=np.int32), mode="valid")
    found = np.flatnonzero(runs == length)
    return int(found[0]) if found.size else None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("iq_file", type=Path)
    parser.add_argument("--sample-rate", type=float, default=15.36e6)
    parser.add_argument("--threshold", type=float, default=500.0)
    parser.add_argument("--plot-samples", type=int, default=256)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    raw = np.fromfile(args.iq_file, dtype="<i2")
    if raw.size < 4 or raw.size % 2:
        raise ValueError("input must be interleaved little-endian int16 IQ")

    i_data = raw[0::2].astype(np.float64)
    q_data = raw[1::2].astype(np.float64)
    iq = i_data + 1j * q_data
    magnitude = np.abs(iq)
    crossings = np.flatnonzero(magnitude > args.threshold)
    first = int(crossings[0]) if crossings.size else None
    stable = first_run(magnitude > args.threshold, 16)

    if first is None:
        raise RuntimeError("no sample exceeded the requested threshold")

    stop = min(len(iq), max(args.plot_samples, first + 160))
    time_us = np.arange(stop, dtype=np.float64) / args.sample_rate * 1e6
    samples = [
        [int(index), round(float(time_us[index]), 6), int(i_data[index]), int(q_data[index]), round(float(magnitude[index]), 3)]
        for index in range(stop)
    ]

    after = magnitude[first : min(len(magnitude), first + 1024)]
    before = magnitude[:first]
    payload = {
        "input_file": str(args.iq_file.resolve()),
        "sample_rate_hz": float(args.sample_rate),
        "sample_period_ns": float(1e9 / args.sample_rate),
        "threshold": float(args.threshold),
        "first_crossing_index": first,
        "first_crossing_time_us": float(first / args.sample_rate * 1e6),
        "first_crossing_i": int(i_data[first]),
        "first_crossing_q": int(q_data[first]),
        "first_crossing_magnitude": float(magnitude[first]),
        "previous_index": first - 1,
        "previous_i": int(i_data[first - 1]),
        "previous_q": int(q_data[first - 1]),
        "previous_magnitude": float(magnitude[first - 1]),
        "maximum_before_crossing": float(np.max(before)) if before.size else 0.0,
        "stable_16_sample_start_index": stable,
        "median_magnitude_next_1024": float(np.median(after)),
        "minimum_magnitude_next_16": float(np.min(magnitude[first : first + 16])),
        "samples": samples,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({k: v for k, v in payload.items() if k != "samples"}, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
