#!/usr/bin/env python3
"""Compare spatial-MIMO SFO phase tracking with closed-loop resampling."""

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


VARIANTS = (
    ("common_phase", "Common phase only", False, False),
    ("phase_slope", "Phase-slope correction", True, False),
    ("closed_loop", "Closed-loop resampling", True, True),
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compare 2x2 spatial-MIMO SFO compensation stages"
    )
    parser.add_argument(
        "--config",
        type=Path,
        default=ROOT
        / "configs"
        / "mimo_2x2_spatial_multiplexing_sfo_closed_loop_1024.yaml",
    )
    parser.add_argument("--snr-db", default="30,35,40")
    parser.add_argument("--frames", type=int, default=None)
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=ROOT.parent / "measurement" / "spatial_sfo_closed_loop",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    with args.config.open("r", encoding="utf-8") as handle:
        values = yaml.safe_load(handle) or {}
    base = SimulationConfig.from_mapping(values)
    if args.frames is not None:
        base = replace(base, frames=args.frames)
    snrs = [float(item) for item in args.snr_db.split(",") if item.strip()]
    if not snrs:
        raise ValueError("at least one SNR point is required")

    rows: list[dict[str, object]] = []
    for snr_index, snr_db in enumerate(snrs):
        for variant, label, slope_tracking, resampling in VARIANTS:
            cfg = replace(
                base,
                snr_db=snr_db,
                seed=base.seed + snr_index,
                sfo_tracking_enable=slope_tracking,
                sfo_resampling_enable=resampling,
            )
            result = simulate_mimo_ofdm(cfg)
            row = {"variant": variant, "label": label, **result}
            rows.append(row)
            print(
                f"SNR={snr_db:4.0f} {variant:12s} "
                f"BER={result['ber']:.7g} EVM={result['evm_rms']:.6g} "
                f"CRC={result['crc_failure_rate']:.3f} "
                f"SFO={result['estimated_sfo_ppm_mean']:.2f}/"
                f"{result['true_sfo_ppm']:.2f} ppm "
                f"residual={result['mean_absolute_residual_sfo_ppm']:.2f} ppm "
                f"goodput={result['goodput_bps']/1e6:.3f} Mb/s"
            )

    highest_snr = max(snrs)
    high = {
        str(row["variant"]): row
        for row in rows
        if float(row["snr_db"]) == highest_snr
    }
    phase_slope = high["phase_slope"]
    closed_loop = high["closed_loop"]
    checks = {
        "phase_reference_estimates_sfo_within_50_ppm": abs(
            float(closed_loop["estimated_sfo_ppm_mean"]) - base.sfo_ppm
        )
        < 50.0,
        "closed_loop_applies_at_least_85_percent_high_confidence_frames": float(
            closed_loop["sfo_resampling_application_rate"]
        )
        >= 0.85,
        "closed_loop_reduces_ber_by_at_least_half": float(closed_loop["ber"])
        < 0.5 * float(phase_slope["ber"]),
        "closed_loop_reduces_evm_by_at_least_40_percent": float(
            closed_loop["evm_rms"]
        )
        < 0.6 * float(phase_slope["evm_rms"]),
        "closed_loop_residual_is_below_100_ppm": float(
            closed_loop["mean_absolute_residual_sfo_ppm"]
        )
        < 100.0,
        "closed_loop_improves_crc_goodput": float(closed_loop["goodput_bps"])
        > float(phase_slope["goodput_bps"]),
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

    fig, axes = plt.subplots(2, 2, figsize=(11.0, 8.0), constrained_layout=True)
    styles = (("common_phase", "o"), ("phase_slope", "s"), ("closed_loop", "^"))
    for variant, marker in styles:
        selected = [row for row in rows if row["variant"] == variant]
        x = [float(row["snr_db"]) for row in selected]
        label = str(selected[0]["label"])
        axes[0, 0].semilogy(
            x,
            [max(float(row["ber"]), 1.0e-8) for row in selected],
            marker=marker,
            label=label,
        )
        axes[0, 1].plot(
            x,
            [float(row["evm_rms"]) for row in selected],
            marker=marker,
            label=label,
        )
        axes[1, 0].plot(
            x,
            [float(row["goodput_bps"]) / 1e6 for row in selected],
            marker=marker,
            label=label,
        )
    closed_rows = [row for row in rows if row["variant"] == "closed_loop"]
    x_closed = [float(row["snr_db"]) for row in closed_rows]
    axes[1, 1].plot(
        x_closed,
        [float(row["estimated_sfo_ppm_mean"]) for row in closed_rows],
        marker="o",
        label="Initial estimate",
    )
    axes[1, 1].plot(
        x_closed,
        [float(row["mean_absolute_residual_sfo_ppm"]) for row in closed_rows],
        marker="s",
        label="Mean absolute residual",
    )
    axes[1, 1].axhline(base.sfo_ppm, linestyle="--", label="True SFO")
    axes[0, 0].set_ylabel("BER")
    axes[0, 1].set_ylabel("RMS EVM")
    axes[1, 0].set_ylabel("CRC goodput (Mb/s)")
    axes[1, 1].set_ylabel("SFO (ppm)")
    for axis in axes.flat:
        axis.set_xlabel("Es/N0 (dB)")
        axis.grid(True, linestyle=":", alpha=0.6)
        axis.legend(fontsize=8)
    axes[0, 0].set_title(
        f"2x2 SM MMSE · 64-QAM · 1024/128 · SFO={base.sfo_ppm:g} ppm"
    )
    figure_path = output_dir / "spatial_sfo_closed_loop.png"
    fig.savefig(figure_path, dpi=180)
    plt.close(fig)

    print(f"Checks: {sum(checks.values())}/{len(checks)} passed")
    print(f"Wrote {csv_path}")
    print(f"Report {report_path}")
    print(f"Figure {figure_path}")
    if not all(checks.values()):
        raise SystemExit("one or more spatial SFO checks failed")


if __name__ == "__main__":
    main()
