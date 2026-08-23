#!/usr/bin/env python3
"""Compare linear LS, DFT-LS and LMMSE under identical channel conditions."""

from __future__ import annotations

import argparse
import csv
import json
import sys
from dataclasses import replace
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from openisac_phy import SimulationConfig, simulate_alamouti_ofdm  # noqa: E402
from openisac_phy.config import ChannelTap  # noqa: E402


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Compare OpenISAC channel estimators")
    parser.add_argument("--frames", type=int, default=1000)
    parser.add_argument("--snr-db", default="15,20,25,30")
    parser.add_argument(
        "--output",
        type=Path,
        default=ROOT.parent / "measurement" / "channel_estimator_comparison" / "results.csv",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    snrs = [float(item) for item in args.snr_db.split(",") if item.strip()]
    if args.frames <= 0 or not snrs:
        raise ValueError("positive frames and at least one SNR point are required")
    base = SimulationConfig(
        modulation="64qam",
        fft_size=64,
        cp_length=16,
        dc_null=True,
        pilot_spacing=4,
        frames=args.frames,
        nr=2,
        channel="tdl",
        channel_taps=(
            ChannelTap(0, 0.0, 0.0),
            ChannelTap(3, -4.0, 45.0),
            ChannelTap(9, -8.0, -80.0),
        ),
        synchronization_enable=False,
        pilot_phase_tracking_enable=False,
        timing_offset_samples=0,
        timing_search_samples=0,
        cfo_hz=0.0,
        channel_estimation_taps=10,
        seed=0x6A31,
    )
    rows: list[dict[str, object]] = []
    for snr_db in snrs:
        for mode in ("ls_linear", "ls_dft", "lmmse"):
            result = simulate_alamouti_ofdm(
                replace(base, snr_db=snr_db, channel_estimation=mode)
            )
            row = {
                "mode": mode,
                "snr_db": snr_db,
                "frames": args.frames,
                "ber": result["ber"],
                "bler": result["bler"],
                "crc_failure_rate": result["crc_failure_rate"],
                "evm_rms": result["evm_rms"],
                "channel_estimation_nmse": result["channel_estimation_nmse"],
                "pilot_channel_nmse": result["pilot_channel_nmse"],
            }
            rows.append(row)
            print(
                f"{mode:9s} SNR={snr_db:5.1f} dB "
                f"BER={row['ber']:.8g} EVM={row['evm_rms']:.6g} "
                f"NMSE={row['channel_estimation_nmse']:.6g}"
            )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)
    args.output.with_suffix(".json").write_text(
        json.dumps(rows, indent=2, ensure_ascii=False), encoding="utf-8"
    )
    print(f"Wrote {args.output.resolve()}")


if __name__ == "__main__":
    main()
