#!/usr/bin/env python3
"""Deterministic SISO OFDM validation for OpenISAC square-QAM conventions.

This lightweight test complements the full ChannelSimulator/BS/UE run.  It
uses CP-OFDM, AWGN or a static tapped-delay-line channel, perfect one-tap
frequency-domain equalization, and reports uncoded BER/BLER.
"""

from __future__ import annotations

import argparse
import csv
import math
from pathlib import Path

import numpy as np


MODULATION_BITS = {"qpsk": 2, "16qam": 4, "64qam": 6, "256qam": 8}


def gray_to_binary(gray: np.ndarray) -> np.ndarray:
    binary = gray.copy()
    shifted = gray.copy()
    while np.any(shifted):
        shifted = shifted >> 1
        binary ^= shifted
    return binary


def qam_modulate(labels: np.ndarray, bits_per_symbol: int) -> np.ndarray:
    axis_bits = bits_per_symbol // 2
    mask = (1 << axis_bits) - 1
    i_gray = labels >> axis_bits
    q_gray = labels & mask
    levels = 1 << axis_bits
    norm = math.sqrt((2.0 / 3.0) * ((1 << bits_per_symbol) - 1))
    i_level = (levels - 1) - 2 * gray_to_binary(i_gray)
    q_level = (levels - 1) - 2 * gray_to_binary(q_gray)
    return (i_level + 1j * q_level) / norm


def qam_demodulate(samples: np.ndarray, bits_per_symbol: int) -> np.ndarray:
    order = 1 << bits_per_symbol
    references = qam_modulate(np.arange(order, dtype=np.int64), bits_per_symbol)
    distances = np.abs(samples.reshape(-1, 1) - references.reshape(1, -1)) ** 2
    return np.argmin(distances, axis=1).astype(np.int64)


def labels_to_bits(labels: np.ndarray, bits_per_symbol: int) -> np.ndarray:
    shifts = np.arange(bits_per_symbol - 1, -1, -1, dtype=np.int64)
    return ((labels.reshape(-1, 1) >> shifts) & 1).astype(np.uint8)


def channel_taps(profile: str) -> np.ndarray:
    if profile == "awgn":
        return np.asarray([1.0 + 0.0j], dtype=np.complex128)
    taps = np.zeros(10, dtype=np.complex128)
    for delay, gain_db, phase_deg in ((0, 0.0, 0.0), (3, -4.0, 45.0), (9, -8.0, -80.0)):
        taps[delay] = 10 ** (gain_db / 20.0) * np.exp(1j * np.deg2rad(phase_deg))
    taps /= np.sqrt(np.sum(np.abs(taps) ** 2))
    return taps


def simulate(
    modulation: str,
    profile: str,
    snr_db: float,
    frames: int,
    fft_size: int,
    cp_length: int,
    seed: int,
) -> dict[str, object]:
    bits_per_symbol = MODULATION_BITS[modulation]
    if cp_length < len(channel_taps(profile)) - 1:
        raise ValueError("CP must be at least the maximum channel delay")
    rng = np.random.default_rng(seed)
    labels = rng.integers(0, 1 << bits_per_symbol, size=(frames, fft_size), dtype=np.int64)
    freq_symbols = qam_modulate(labels, bits_per_symbol)
    time_symbols = np.fft.ifft(freq_symbols, axis=1) * math.sqrt(fft_size)
    with_cp = np.concatenate((time_symbols[:, -cp_length:], time_symbols), axis=1)
    tx = with_cp.reshape(-1)

    taps = channel_taps(profile)
    clean = np.convolve(tx, taps, mode="full")[: tx.size]
    noise_power = float(np.mean(np.abs(clean) ** 2)) * 10 ** (-snr_db / 10.0)
    noise = math.sqrt(noise_power / 2.0) * (
        rng.standard_normal(clean.size) + 1j * rng.standard_normal(clean.size)
    )
    rx = (clean + noise).reshape(frames, fft_size + cp_length)
    rx_freq = np.fft.fft(rx[:, cp_length:], axis=1) / math.sqrt(fft_size)
    channel_freq = np.fft.fft(taps, n=fft_size)
    equalized = rx_freq / np.where(np.abs(channel_freq) > 1e-7, channel_freq, 1e-7)
    decoded = qam_demodulate(equalized.reshape(-1), bits_per_symbol).reshape(labels.shape)

    tx_bits = labels_to_bits(labels.reshape(-1), bits_per_symbol).reshape(frames, -1)
    rx_bits = labels_to_bits(decoded.reshape(-1), bits_per_symbol).reshape(frames, -1)
    errors = tx_bits != rx_bits
    bit_errors = int(np.count_nonzero(errors))
    block_errors = int(np.count_nonzero(np.any(errors, axis=1)))
    total_bits = int(errors.size)
    return {
        "modulation": modulation,
        "channel": profile,
        "snr_db": snr_db,
        "frames": frames,
        "total_bits": total_bits,
        "bit_errors": bit_errors,
        "ber": bit_errors / total_bits,
        "block_errors": block_errors,
        "bler": block_errors / frames,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Validate SISO high-order QAM over CP-OFDM channels")
    parser.add_argument("--modulations", default="16qam,64qam")
    parser.add_argument("--channels", default="awgn,multipath")
    parser.add_argument("--snr-db", default="12,18,24,30,80")
    parser.add_argument("--frames", type=int, default=400)
    parser.add_argument("--fft-size", type=int, default=256)
    parser.add_argument("--cp-length", type=int, default=32)
    parser.add_argument("--seed", type=lambda value: int(value, 0), default=0x5A17)
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("measurement/siso_qam_validation/results.csv"),
    )
    parser.add_argument("--assert-regression", action="store_true")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    modulations = [item.strip().lower() for item in args.modulations.split(",") if item.strip()]
    channels = [item.strip().lower() for item in args.channels.split(",") if item.strip()]
    snrs = [float(item) for item in args.snr_db.split(",") if item.strip()]
    for modulation in modulations:
        if modulation not in MODULATION_BITS:
            raise ValueError(f"unsupported modulation: {modulation}")
    for channel in channels:
        if channel not in {"awgn", "multipath"}:
            raise ValueError(f"unsupported channel: {channel}")

    rows: list[dict[str, object]] = []
    for mod_index, modulation in enumerate(modulations):
        for channel_index, channel in enumerate(channels):
            for snr_index, snr_db in enumerate(snrs):
                row = simulate(
                    modulation,
                    channel,
                    snr_db,
                    args.frames,
                    args.fft_size,
                    args.cp_length,
                    args.seed + mod_index * 1000 + channel_index * 100 + snr_index,
                )
                rows.append(row)
                print(
                    f"{modulation:6s} {channel:9s} SNR={snr_db:5.1f} dB "
                    f"BER={float(row['ber']):.6g} BLER={float(row['bler']):.6g}"
                )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)

    if args.assert_regression:
        for modulation in modulations:
            for channel in channels:
                subset = [row for row in rows if row["modulation"] == modulation and row["channel"] == channel]
                ordered = sorted(subset, key=lambda row: float(row["snr_db"]))
                if float(ordered[-1]["ber"]) != 0.0:
                    raise AssertionError(f"{modulation}/{channel}: noiseless-limit BER is non-zero")
                # Monte-Carlo curves may wobble slightly, so only require that the
                # highest practical SNR improves over the lowest SNR.
                if len(ordered) > 1 and float(ordered[-2]["ber"]) >= float(ordered[0]["ber"]):
                    raise AssertionError(f"{modulation}/{channel}: BER did not improve with SNR")

    print(f"Wrote {args.output.resolve()}")


if __name__ == "__main__":
    main()
