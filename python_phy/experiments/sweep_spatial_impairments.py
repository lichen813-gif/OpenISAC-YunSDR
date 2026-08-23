#!/usr/bin/env python3
"""Sweep SFO and symbol-rate Doppler boundaries for 2x2 spatial MIMO."""

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


def parse_numbers(text: str) -> list[float]:
    values = [float(item) for item in text.split(",") if item.strip()]
    if not values:
        raise ValueError("at least one sweep point is required")
    return values


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Sweep 2x2 SM SFO/Doppler boundaries")
    parser.add_argument(
        "--config",
        type=Path,
        default=ROOT / "configs" / "mimo_2x2_spatial_multiplexing_sync_stress_256.yaml",
    )
    parser.add_argument("--sfo-ppm", default="0,50,100,200,500,1000,2000")
    parser.add_argument("--doppler-hz", default="0,500,1000,3000,5000,10000")
    parser.add_argument("--frames", type=int, default=None)
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=ROOT.parent / "measurement" / "spatial_impairment_sweep",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    with args.config.open("r", encoding="utf-8") as handle:
        values = yaml.safe_load(handle) or {}
    base = SimulationConfig.from_mapping(values)
    if args.frames is not None:
        base = replace(base, frames=args.frames)
    sfo_values = parse_numbers(args.sfo_ppm)
    doppler_values = parse_numbers(args.doppler_hz)

    rows: list[dict[str, object]] = []
    for sweep, units, sweep_values in (
        ("sfo", "ppm", sfo_values),
        ("doppler", "Hz", doppler_values),
    ):
        for value in sweep_values:
            overrides = {"sfo_ppm": value} if sweep == "sfo" else {"doppler_hz": value}
            result = simulate_mimo_ofdm(replace(base, **overrides))
            row = {"sweep": sweep, "value": value, "units": units, **result}
            rows.append(row)
            print(
                f"{sweep:7s}={value:8g} {units:3s} BER={result['ber']:.7g} "
                f"EVM={result['evm_rms']:.6g} NMSE={result['channel_estimation_nmse']:.6g} "
                f"goodput={result['goodput_bps']/1e6:.3f} Mb/s"
            )

    sfo_rows = [row for row in rows if row["sweep"] == "sfo"]
    doppler_rows = [row for row in rows if row["sweep"] == "doppler"]
    sfo_by_value = {float(row["value"]): row for row in sfo_rows}
    baseline_sfo = sfo_by_value.get(0.0, sfo_rows[0])
    high_sfo = max(sfo_rows, key=lambda row: float(row["value"]))
    checks = {
        "all_synchronization_points_find_preamble": all(
            float(row["timing_success_rate"]) == 1.0 for row in rows
        ),
        "high_sfo_increases_channel_nmse_by_100x": (
            float(high_sfo["channel_estimation_nmse"])
            > 100.0 * float(baseline_sfo["channel_estimation_nmse"])
        ),
        "high_sfo_reduces_crc_goodput_by_90_percent": (
            float(high_sfo["goodput_bps"]) < 0.1 * float(baseline_sfo["goodput_bps"])
        ),
        "per_symbol_csi_keeps_doppler_estimation_nmse_below_0p001": max(
            float(row["channel_estimation_nmse"]) for row in doppler_rows
        ) < 0.001,
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
                "interpretation": {
                    "sfo": (
                        "This sweep deliberately disables the implemented spatial-MIMO SFO "
                        "tracker/resampler so it continues to expose the uncompensated limit."
                    ),
                    "doppler": (
                        "The current channel is constant inside each OFDM symbol and pilots "
                        "estimate every data symbol independently. This sweep does not model "
                        "intra-symbol Doppler ICI and is not a hardware mobility limit."
                    ),
                },
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

    fig, axes = plt.subplots(2, 3, figsize=(13.0, 7.8), constrained_layout=True)
    for row_index, (selected, x_label) in enumerate(
        ((sfo_rows, "SFO (ppm)"), (doppler_rows, "Maximum Doppler (Hz)"))
    ):
        x = [float(row["value"]) for row in selected]
        axes[row_index, 0].semilogy(
            x, [max(float(row["ber"]), 1.0e-7) for row in selected], marker="o"
        )
        axes[row_index, 0].set_ylabel("BER")
        axes[row_index, 1].semilogy(
            x,
            [max(float(row["channel_estimation_nmse"]), 1.0e-8) for row in selected],
            marker="s",
        )
        axes[row_index, 1].set_ylabel("Channel-estimation NMSE")
        axes[row_index, 2].plot(
            x, [float(row["goodput_bps"]) / 1e6 for row in selected], marker="D"
        )
        axes[row_index, 2].set_ylabel("CRC goodput (Mb/s)")
        for axis in axes[row_index]:
            axis.set_xlabel(x_label)
            axis.grid(True, linestyle=":", alpha=0.6)
    axes[0, 0].set_title("Uncompensated SFO boundary")
    axes[1, 0].set_title("Symbol-rate block-Doppler diagnostic")
    figure_path = output_dir / "spatial_impairment_sweep.png"
    fig.savefig(figure_path, dpi=180)
    plt.close(fig)

    print(f"Checks: {sum(checks.values())}/{len(checks)} passed")
    print(f"Wrote {csv_path}")
    print(f"Report {report_path}")
    print(f"Figure {figure_path}")
    if not all(checks.values()):
        raise SystemExit("one or more boundary checks failed")


if __name__ == "__main__":
    main()
