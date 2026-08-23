#!/usr/bin/env python3
"""Compare STBC, SFBC and 2x2 ZF/MMSE spatial multiplexing."""

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
    parser = argparse.ArgumentParser(description="OpenISAC MIMO mode comparison")
    parser.add_argument(
        "--config",
        type=Path,
        default=ROOT / "configs" / "mimo_2x2_spatial_multiplexing_rayleigh_1024.yaml",
    )
    parser.add_argument("--snr-db", default="5,10,15,20,25,30")
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=ROOT.parent / "measurement" / "mimo_mode_comparison",
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

    modes = (
        (
            "STBC",
            {"mode": "stbc", "layers": 1, "pairing": "time", "detector": "mmse"},
        ),
        (
            "SFBC",
            {"mode": "sfbc", "layers": 1, "pairing": "frequency", "detector": "mmse"},
        ),
        (
            "SM-ZF",
            {
                "mode": "spatial_multiplexing",
                "layers": 2,
                "pairing": "time",
                "detector": "zf",
            },
        ),
        (
            "SM-MMSE",
            {
                "mode": "spatial_multiplexing",
                "layers": 2,
                "pairing": "time",
                "detector": "mmse",
            },
        ),
    )
    rows: list[dict[str, object]] = []
    for label, overrides in modes:
        for snr_db in snrs:
            result = simulate_mimo_ofdm(replace(base, snr_db=snr_db, **overrides))
            row = {"comparison_label": label, **result}
            rows.append(row)
            print(
                f"{label:7s} SNR={snr_db:5.1f} dB BER={result['ber']:.7g} "
                f"EVM={result['evm_rms']:.6g} "
                f"rate={result['net_payload_rate_bps'] / 1e6:.3f} Mb/s "
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

    fig, axes = plt.subplots(3, 1, figsize=(8.4, 10.8), constrained_layout=True)
    markers = {"STBC": "o", "SFBC": "s", "SM-ZF": "^", "SM-MMSE": "D"}
    for label, _overrides in modes:
        selected = [row for row in rows if row["comparison_label"] == label]
        x = [float(row["snr_db"]) for row in selected]
        axes[0].semilogy(
            x,
            [max(float(row["ber"]), 1.0e-8) for row in selected],
            marker=markers[label],
            label=label,
        )
        axes[1].semilogy(
            x,
            [max(float(row["evm_rms"]), 1.0e-4) for row in selected],
            marker=markers[label],
            label=label,
        )
        axes[2].plot(
            x,
            [float(row["goodput_bps"]) / 1e6 for row in selected],
            marker=markers[label],
            label=label,
        )
        axes[2].plot(
            x,
            [float(row["net_payload_rate_bps"]) / 1e6 for row in selected],
            linestyle=":",
            alpha=0.45,
        )
    axes[0].set_title(
        f"1024/128 2x2 Rayleigh · 64-QAM · perfect CSI · {base.frames} frames/point"
    )
    axes[0].set_ylabel("BER")
    axes[1].set_ylabel("RMS EVM")
    axes[2].set_ylabel("CRC goodput (Mb/s)")
    axes[2].set_xlabel("Total-power Es/N0 (dB)")
    for axis in axes:
        axis.grid(True, linestyle=":", alpha=0.6)
        axis.legend()
    figure_path = output_dir / "stbc_sfbc_sm_zf_mmse.png"
    fig.savefig(figure_path, dpi=180)
    plt.close(fig)
    print(f"Wrote {csv_path}")
    print(f"Report {report_path}")
    print(f"Figure {figure_path}")


if __name__ == "__main__":
    main()
