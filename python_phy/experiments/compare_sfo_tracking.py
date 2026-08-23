#!/usr/bin/env python3
"""Compare phase-reference SFO slope tracking under identical conditions."""

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

from openisac_phy import SimulationConfig, simulate_alamouti_ofdm  # noqa: E402


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Compare OpenISAC SFO tracking")
    parser.add_argument(
        "--config",
        type=Path,
        default=ROOT / "configs" / "mimo_2x2_alamouti_tdl_sfo_1024.yaml",
    )
    parser.add_argument("--snr-db", default="20,25,30")
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=ROOT.parent / "measurement" / "sfo_tracking_validation",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    with args.config.open("r", encoding="utf-8") as handle:
        values = yaml.safe_load(handle) or {}
    base = SimulationConfig.from_mapping(values)
    snrs = [float(item) for item in args.snr_db.split(",") if item.strip()]
    if not snrs:
        raise ValueError("at least one SNR point is required")

    rows: list[dict[str, object]] = []
    for index, snr_db in enumerate(snrs):
        for enabled in (False, True):
            cfg = replace(
                base,
                snr_db=snr_db,
                seed=base.seed + index,
                sfo_tracking_enable=enabled,
            )
            result = simulate_alamouti_ofdm(cfg)
            row = {"tracking": "on" if enabled else "off", **result}
            rows.append(row)
            print(
                f"SNR={snr_db:5.1f} tracking={row['tracking']:3s} "
                f"BER={result['ber']:.7g} EVM={result['evm_rms']:.6g} "
                f"NMSE={result['channel_estimation_nmse']:.6g} "
                f"SFO={result['estimated_sfo_ppm_mean']:.3f}/"
                f"{result['true_sfo_ppm']:.3f} ppm"
            )

    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    csv_path = output_dir / "results.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)
    (output_dir / "report.json").write_text(
        json.dumps({"config": values, "results": rows}, indent=2, ensure_ascii=False),
        encoding="utf-8",
    )

    os.environ.setdefault("MPLCONFIGDIR", str(ROOT / ".matplotlib"))
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, axes = plt.subplots(3, 1, figsize=(7.8, 10.2), constrained_layout=True)
    for tracking, label, marker in (("off", "SFO tracking off", "o"), ("on", "SFO tracking on", "s")):
        selected = [row for row in rows if row["tracking"] == tracking]
        x = [float(row["snr_db"]) for row in selected]
        axes[0].semilogy(
            x,
            [max(float(row["ber"]), 1.0e-8) for row in selected],
            marker=marker,
            label=label,
        )
        axes[1].plot(
            x,
            [float(row["evm_rms"]) for row in selected],
            marker=marker,
            label=label,
        )
    tracking_on = [row for row in rows if row["tracking"] == "on"]
    x_on = [float(row["snr_db"]) for row in tracking_on]
    axes[2].plot(
        x_on,
        [float(row["estimated_sfo_ppm_mean"]) for row in tracking_on],
        marker="s",
        label="Estimated SFO",
    )
    axes[2].axhline(base.sfo_ppm, linestyle="--", label="True SFO")
    axes[0].set_ylabel("BER")
    axes[1].set_ylabel("RMS EVM")
    axes[2].set_ylabel("SFO (ppm)")
    axes[2].set_xlabel("Es/N0 (dB)")
    axes[0].set_title(
        f"1024/128 2x{base.nr} Alamouti TDL · SFO={base.sfo_ppm:g} ppm"
    )
    for axes_item in axes:
        axes_item.grid(True, linestyle=":", alpha=0.6)
        axes_item.legend()
    figure_path = output_dir / "sfo_tracking_comparison.png"
    fig.savefig(figure_path, dpi=180)
    plt.close(fig)
    print(f"Wrote {csv_path}")
    print(f"Figure {figure_path}")


if __name__ == "__main__":
    main()
