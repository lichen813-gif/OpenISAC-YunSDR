#!/usr/bin/env python3
"""Validate the low-complexity receiver for the intended operating region."""

from __future__ import annotations

import argparse
import csv
import json
import sys
from dataclasses import replace
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from openisac_phy import SimulationConfig, simulate_mimo_ofdm  # noqa: E402


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate the <=20 ppm, low-Doppler engineering profile"
    )
    parser.add_argument(
        "--config",
        type=Path,
        default=ROOT
        / "configs"
        / "mimo_2x2_spatial_multiplexing_realtime_1024.yaml",
    )
    parser.add_argument("--frames", type=int, default=None)
    parser.add_argument("--doppler-hz", default="0,25,50,100,200")
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=ROOT.parent / "measurement" / "engineering_operating_region",
    )
    return parser.parse_args()


def compact_row(case: str, result: dict[str, object]) -> dict[str, object]:
    return {"case": case, **result}


def print_row(row: dict[str, object]) -> None:
    print(
        f"{str(row['case']):18s} "
        f"BER={float(row['ber']):.7g} "
        f"EVM={float(row['evm_rms']):.6g} "
        f"CRC={float(row['crc_failure_rate']):.3f} "
        f"goodput={float(row['goodput_bps']) / 1e6:.3f} Mb/s "
        f"SFOerr={float(row['mean_absolute_sfo_error_ppm']):.2f} ppm "
        f"resamp={float(row['resampler_tap_mac_per_second']) / 1e6:.1f} Mtap-MAC/s"
    )


def main() -> None:
    args = parse_args()
    values = yaml.safe_load(args.config.read_text(encoding="utf-8")) or {}
    base = SimulationConfig.from_mapping(values)
    if args.frames is not None:
        base = replace(base, frames=args.frames)

    # The field limit is 20 ppm.  Keep only frequency-domain phase-slope
    # correction in the main path and compare it with the old sinc resampler.
    low_complexity = replace(
        base,
        sfo_ppm=20.0,
        phase_reference_count=8,
        sfo_tracking_enable=True,
        sfo_resampling_enable=False,
        doppler_hz=0.0,
        doppler_model="continuous",
    )
    rows: list[dict[str, object]] = []
    for count in (8, 16, 32):
        result = simulate_mimo_ofdm(
            replace(low_complexity, phase_reference_count=count)
        )
        row = compact_row(f"phase_only_ref{count}", result)
        rows.append(row)
        print_row(row)

    resampled = simulate_mimo_ofdm(
        replace(
            low_complexity,
            sfo_resampling_enable=True,
            sfo_resampling_interpolator="sinc8",
        )
    )
    row = compact_row("sinc8_ref8", resampled)
    rows.append(row)
    print_row(row)

    dopplers = [float(value) for value in args.doppler_hz.split(",") if value]
    if 0.0 not in dopplers:
        raise ValueError("Doppler scan must include 0 Hz")
    for doppler_hz in dopplers:
        result = simulate_mimo_ofdm(
            replace(low_complexity, doppler_hz=doppler_hz)
        )
        row = compact_row(f"doppler_{doppler_hz:g}Hz", result)
        rows.append(row)
        print_row(row)

    by_case = {str(row["case"]): row for row in rows}
    phase8 = by_case["phase_only_ref8"]
    sinc8 = by_case["sinc8_ref8"]
    doppler0 = by_case["doppler_0Hz"]
    doppler100 = by_case.get("doppler_100Hz")
    checks = {
        "phase_only_removes_resampler_mac_load": float(
            phase8["resampler_tap_mac_per_second"]
        )
        == 0.0,
        "eight_refs_within_20_percent_of_sinc8_evm": float(phase8["evm_rms"])
        <= 1.2 * float(sinc8["evm_rms"]),
        "eight_refs_preserve_95_percent_of_sinc8_goodput": float(
            phase8["goodput_bps"]
        )
        >= 0.95 * float(sinc8["goodput_bps"]),
        "100_hz_preserves_90_percent_of_zero_doppler_goodput": doppler100
        is not None
        and float(doppler100["goodput_bps"])
        >= 0.9 * float(doppler0["goodput_bps"]),
    }

    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    csv_path = output_dir / "results.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)
    report_path = output_dir / "report.json"
    report_path.write_text(
        json.dumps(
            {
                "source_config": str(args.config),
                "frames": base.frames,
                "results": rows,
                "checks": checks,
                "all_checks_passed": all(checks.values()),
            },
            indent=2,
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    print(f"Checks: {sum(checks.values())}/{len(checks)} passed")
    print(f"Wrote {csv_path}")
    print(f"Report {report_path}")
    if not all(checks.values()):
        raise SystemExit("one or more engineering operating-region checks failed")


if __name__ == "__main__":
    main()
