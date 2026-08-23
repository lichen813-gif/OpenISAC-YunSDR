#!/usr/bin/env python3
"""Compare perfect, LS, DFT-LS and LMMSE CSI for 2x2 spatial MIMO."""

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


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="OpenISAC spatial-MIMO CSI comparison")
    parser.add_argument(
        "--config",
        type=Path,
        default=ROOT / "configs" / "mimo_2x2_spatial_multiplexing_tdl_lmmse_1024.yaml",
    )
    parser.add_argument("--snr-db", default="20,25,30,35,40")
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=ROOT.parent / "measurement" / "spatial_csi_comparison",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    with args.config.open("r", encoding="utf-8") as handle:
        values = yaml.safe_load(handle) or {}
    base = SimulationConfig.from_mapping(values)
    snrs = [float(item) for item in args.snr_db.split(",") if item.strip()]
    estimators = (
        ("Perfect CSI", "perfect"),
        ("LS-linear", "ls_linear"),
        ("DFT-LS", "ls_dft"),
        ("LMMSE", "lmmse"),
    )
    rows: list[dict[str, object]] = []
    for label, estimator in estimators:
        for snr_db in snrs:
            result = simulate_mimo_ofdm(
                replace(base, snr_db=snr_db, channel_estimation=estimator)
            )
            row = {"comparison_label": label, **result}
            rows.append(row)
            print(
                f"{label:11s} SNR={snr_db:5.1f} BER={result['ber']:.7g} "
                f"EVM={result['evm_rms']:.6g} NMSE={result['channel_estimation_nmse']:.6g} "
                f"goodput={result['goodput_bps'] / 1e6:.3f} Mb/s"
            )

    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    csv_path = output_dir / "results.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)
    report_path = output_dir / "report.json"
    report_path.write_text(
        json.dumps({"config": values, "results": rows}, indent=2, ensure_ascii=False),
        encoding="utf-8",
    )

    os.environ.setdefault("MPLCONFIGDIR", str(ROOT / ".matplotlib"))
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, axes = plt.subplots(4, 1, figsize=(8.4, 13.0), constrained_layout=True)
    markers = {"Perfect CSI": "o", "LS-linear": "s", "DFT-LS": "^", "LMMSE": "D"}
    for label, _estimator in estimators:
        selected = [row for row in rows if row["comparison_label"] == label]
        x = [float(row["snr_db"]) for row in selected]
        series = (
            [max(float(row["ber"]), 1.0e-8) for row in selected],
            [max(float(row["evm_rms"]), 1.0e-5) for row in selected],
            [max(float(row["channel_estimation_nmse"]), 1.0e-10) for row in selected],
            [float(row["goodput_bps"]) / 1e6 for row in selected],
        )
        for index, values_series in enumerate(series):
            plot = axes[index].semilogy if index < 3 else axes[index].plot
            plot(x, values_series, marker=markers[label], label=label)
    axes[0].set_title(
        f"1024/128 2x2 SM-MMSE · 64-QAM TDL · FDM pilots/4 · {base.frames} frames"
    )
    axes[0].set_ylabel("BER")
    axes[1].set_ylabel("RMS EVM")
    axes[2].set_ylabel("Channel NMSE")
    axes[3].set_ylabel("CRC goodput (Mb/s)")
    axes[3].set_xlabel("Total-power Es/N0 (dB)")
    for axis in axes:
        axis.grid(True, linestyle=":", alpha=0.6)
        axis.legend()
    figure_path = output_dir / "spatial_csi_estimators.png"
    fig.savefig(figure_path, dpi=180)
    plt.close(fig)
    print(f"Wrote {csv_path}")
    print(f"Report {report_path}")
    print(f"Figure {figure_path}")


if __name__ == "__main__":
    main()
