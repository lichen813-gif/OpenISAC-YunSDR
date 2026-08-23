#!/usr/bin/env python3
"""Validate the low-complexity C++ SFO interpolator against the reference."""

from __future__ import annotations

import argparse
import csv
import json
import os
import sys
from dataclasses import replace
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from openisac_phy import SimulationConfig, simulate_mimo_ofdm  # noqa: E402


METHODS = (
    ("cubic", 4, "4-point cubic"),
    ("sinc8", 8, "8-tap real-time FIR"),
    ("sinc24", 24, "24-tap reference FIR"),
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compare real-time and reference SFO interpolators"
    )
    parser.add_argument(
        "--config",
        type=Path,
        default=ROOT
        / "configs"
        / "mimo_2x2_spatial_multiplexing_realtime_1024.yaml",
    )
    parser.add_argument("--frames", type=int, default=None)
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=ROOT.parent / "measurement" / "realtime_sfo_interpolator",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    with args.config.open("r", encoding="utf-8") as handle:
        values = yaml.safe_load(handle) or {}
    base = SimulationConfig.from_mapping(values)
    if args.frames is not None:
        base = replace(base, frames=args.frames)

    rows: list[dict[str, object]] = []
    for method, taps, label in METHODS:
        result = simulate_mimo_ofdm(
            replace(base, sfo_resampling_interpolator=method)
        )
        row = {
            "interpolator": method,
            "tap_count": taps,
            "label": label,
            **result,
        }
        rows.append(row)
        print(
            f"{method:6s} taps={taps:2d} BER={result['ber']:.7g} "
            f"EVM={result['evm_rms']:.6g} CRC={result['crc_failure_rate']:.3f} "
            f"residual={result['mean_absolute_residual_sfo_ppm']:.2f} ppm "
            f"goodput={result['goodput_bps']/1e6:.3f} Mb/s"
        )

    by_method = {str(row["interpolator"]): row for row in rows}
    realtime = by_method["sinc8"]
    reference = by_method["sinc24"]
    cubic = by_method["cubic"]
    checks = {
        "sinc8_reduces_taps_by_two_thirds": int(realtime["tap_count"]) * 3
        == int(reference["tap_count"]),
        "sinc8_ber_is_within_10_percent_of_reference": float(realtime["ber"])
        <= 1.10 * float(reference["ber"]),
        "sinc8_evm_is_within_3_percent_of_reference": float(realtime["evm_rms"])
        <= 1.03 * float(reference["evm_rms"]),
        "sinc8_preserves_at_least_95_percent_reference_goodput": float(
            realtime["goodput_bps"]
        )
        >= 0.95 * float(reference["goodput_bps"]),
        "sinc8_residual_is_below_10_ppm": float(
            realtime["mean_absolute_residual_sfo_ppm"]
        )
        < 10.0,
        "sinc8_outperforms_four_point_cubic_evm": float(realtime["evm_rms"])
        < float(cubic["evm_rms"]),
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
                "config": values,
                "frames_override": args.frames,
                "results": rows,
                "checks": checks,
                "all_checks_passed": all(checks.values()),
                "realtime_scope": (
                    "sinc8 is the C++ receiver baseline for the validated 50-ppm "
                    "operating profile. sinc24 remains the offline/extreme-offset reference."
                ),
            },
            indent=2,
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )

    os.environ.setdefault("MPLCONFIGDIR", str(ROOT / ".matplotlib"))
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    labels = [str(row["interpolator"]) for row in rows]
    x = list(range(len(rows)))
    fig, axes = plt.subplots(2, 2, figsize=(10.5, 7.5), constrained_layout=True)
    axes[0, 0].bar(x, [float(row["evm_rms"]) for row in rows])
    axes[0, 0].set_ylabel("RMS EVM")
    axes[0, 1].bar(x, [float(row["ber"]) for row in rows])
    axes[0, 1].set_yscale("log")
    axes[0, 1].set_ylabel("BER")
    axes[1, 0].bar(x, [float(row["goodput_bps"]) / 1e6 for row in rows])
    axes[1, 0].set_ylabel("CRC goodput (Mb/s)")
    axes[1, 1].bar(x, [int(row["tap_count"]) for row in rows])
    axes[1, 1].set_ylabel("Interpolation taps per output sample")
    for axis in axes.flat:
        axis.set_xticks(x, labels)
        axis.grid(True, axis="y", linestyle=":", alpha=0.6)
    axes[0, 0].set_title(
        f"2x2 SM · 64-QAM · 1024/128 · SFO={base.sfo_ppm:g} ppm · 40 dB"
    )
    figure_path = output_dir / "realtime_sfo_interpolator.png"
    fig.savefig(figure_path, dpi=180)
    plt.close(fig)

    print(f"Checks: {sum(checks.values())}/{len(checks)} passed")
    print(f"Wrote {csv_path}")
    print(f"Report {report_path}")
    print(f"Figure {figure_path}")
    if not all(checks.values()):
        raise SystemExit("one or more real-time SFO checks failed")


if __name__ == "__main__":
    main()
