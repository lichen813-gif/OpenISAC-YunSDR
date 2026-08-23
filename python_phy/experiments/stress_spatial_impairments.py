#!/usr/bin/env python3
"""Stress 2x2 spatial multiplexing with synchronization and oscillator errors."""

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


SCENARIOS: tuple[tuple[str, str, dict[str, object]], ...] = (
    ("baseline", "Baseline", {}),
    (
        "timing_cfo_no_sync",
        "Timing+CFO\n(no sync)",
        {
            "synchronization_enable": False,
            "timing_offset_samples": 12,
            "cfo_hz": 1000.0,
        },
    ),
    (
        "timing_cfo_sync",
        "Timing+CFO\n(sync)",
        {"timing_offset_samples": 12, "cfo_hz": 1000.0},
    ),
    ("doppler_500", "Doppler\n500 Hz", {"doppler_hz": 500.0}),
    ("sfo_50", "SFO\n50 ppm", {"sfo_ppm": 50.0}),
    (
        "combined",
        "Combined",
        {
            "timing_offset_samples": 12,
            "cfo_hz": 1000.0,
            "doppler_hz": 500.0,
            "sfo_ppm": 50.0,
        },
    ),
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="OpenISAC 2x2 SM impairment stress test")
    parser.add_argument(
        "--config",
        type=Path,
        default=ROOT / "configs" / "mimo_2x2_spatial_multiplexing_sync_stress_256.yaml",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=ROOT.parent / "measurement" / "spatial_impairment_stress",
    )
    parser.add_argument("--frames", type=int, default=None)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    with args.config.open("r", encoding="utf-8") as handle:
        values = yaml.safe_load(handle) or {}
    base = SimulationConfig.from_mapping(values)
    if args.frames is not None:
        base = replace(base, frames=args.frames)

    rows: list[dict[str, object]] = []
    for scenario_id, label, overrides in SCENARIOS:
        result = simulate_mimo_ofdm(replace(base, **overrides))
        row = {"scenario": scenario_id, "label": label.replace("\n", " "), **result}
        rows.append(row)
        print(
            f"{scenario_id:20s} BER={result['ber']:.7g} "
            f"EVM={result['evm_rms']:.6g} NMSE={result['channel_estimation_nmse']:.6g} "
            f"timing={result['timing_success_rate']:.3f} "
            f"CFOerr={result['mean_absolute_cfo_error_hz']:.2f} Hz "
            f"goodput={result['goodput_bps']/1e6:.3f} Mb/s"
        )

    by_id = {str(row["scenario"]): row for row in rows}
    baseline = by_id["baseline"]
    unsynchronized = by_id["timing_cfo_no_sync"]
    synchronized = by_id["timing_cfo_sync"]
    checks = {
        "baseline_ber_below_2_percent": float(baseline["ber"]) < 0.02,
        "zc_timing_success_is_100_percent": float(synchronized["timing_success_rate"]) == 1.0,
        "cfo_mean_absolute_error_below_25_hz": float(synchronized["mean_absolute_cfo_error_hz"]) < 25.0,
        "synchronization_reduces_ber": float(synchronized["ber"]) < float(unsynchronized["ber"]),
        "synchronization_recovers_at_least_90_percent_baseline_goodput": (
            float(synchronized["goodput_bps"]) >= 0.9 * float(baseline["goodput_bps"])
        ),
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
                "scenarios": rows,
                "checks": checks,
                "all_checks_passed": all(checks.values()),
                "known_limit": (
                    "This legacy stress configuration deliberately disables spatial-MIMO "
                    "phase-reference/SFO tracking to preserve the uncompensated boundary. "
                    "Use compare_spatial_sfo_resampling.py for the closed-loop result."
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

    labels = [label for _, label, _ in SCENARIOS]
    x = list(range(len(rows)))
    fig, axes = plt.subplots(2, 2, figsize=(12.0, 8.0), constrained_layout=True)
    axes[0, 0].bar(x, [max(float(row["ber"]), 1.0e-7) for row in rows])
    axes[0, 0].set_yscale("log")
    axes[0, 0].set_ylabel("BER")
    axes[0, 1].bar(x, [float(row["evm_rms"]) for row in rows])
    axes[0, 1].set_ylabel("RMS EVM")
    axes[1, 0].bar(x, [max(float(row["channel_estimation_nmse"]), 1.0e-8) for row in rows])
    axes[1, 0].set_yscale("log")
    axes[1, 0].set_ylabel("Channel-estimation NMSE")
    axes[1, 1].bar(x, [float(row["goodput_bps"]) / 1e6 for row in rows])
    axes[1, 1].set_ylabel("CRC goodput (Mb/s)")
    for axis in axes.flat:
        axis.set_xticks(x, labels, fontsize=8)
        axis.grid(True, axis="y", linestyle=":", alpha=0.6)
    axes[0, 0].set_title(
        "2x2 full-layer MMSE · 64-QAM · pilot LS CSI · ZC preamble · 35 dB"
    )
    figure_path = output_dir / "spatial_impairment_stress.png"
    fig.savefig(figure_path, dpi=180)
    plt.close(fig)

    print(f"Checks: {sum(checks.values())}/{len(checks)} passed")
    print(f"Wrote {csv_path}")
    print(f"Report {report_path}")
    print(f"Figure {figure_path}")
    if not all(checks.values()):
        raise SystemExit("one or more spatial impairment checks failed")


if __name__ == "__main__":
    main()
