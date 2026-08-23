#!/usr/bin/env python3
"""Compare 2x2, 4x4 and 8x8 full-layer MMSE spatial multiplexing."""

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


def main() -> None:
    parser = argparse.ArgumentParser(description="OpenISAC spatial-MIMO size comparison")
    parser.add_argument(
        "--config",
        type=Path,
        default=ROOT / "configs" / "mimo_8x8_spatial_multiplexing_rayleigh_256.yaml",
    )
    parser.add_argument("--snr-db", default="30,40,50")
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=ROOT.parent / "measurement" / "mimo_size_comparison",
    )
    args = parser.parse_args()
    with args.config.open("r", encoding="utf-8") as handle:
        values = yaml.safe_load(handle) or {}
    base = SimulationConfig.from_mapping(values)
    snrs = [float(item) for item in args.snr_db.split(",") if item.strip()]
    rows: list[dict[str, object]] = []
    for antennas in (2, 4, 8):
        for snr_db in snrs:
            result = simulate_mimo_ofdm(
                replace(
                    base,
                    nt=antennas,
                    nr=antennas,
                    layers=antennas,
                    snr_db=snr_db,
                )
            )
            row = {"comparison_label": f"{antennas}x{antennas}", **result}
            rows.append(row)
            print(
                f"{antennas}x{antennas} SNR={snr_db:5.1f} BER={result['ber']:.7g} "
                f"EVM={result['evm_rms']:.6g} rate={result['net_payload_rate_bps']/1e6:.3f} "
                f"goodput={result['goodput_bps']/1e6:.3f} Mb/s"
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

    fig, axes = plt.subplots(3, 1, figsize=(8.2, 10.5), constrained_layout=True)
    for label, marker in (("2x2", "o"), ("4x4", "s"), ("8x8", "D")):
        selected = [row for row in rows if row["comparison_label"] == label]
        x = [float(row["snr_db"]) for row in selected]
        axes[0].semilogy(x, [max(float(row["ber"]), 1e-8) for row in selected], marker=marker, label=label)
        axes[1].semilogy(x, [float(row["evm_rms"]) for row in selected], marker=marker, label=label)
        axes[2].plot(x, [float(row["goodput_bps"])/1e6 for row in selected], marker=marker, label=label)
    axes[0].set_title("256/32 full-layer MMSE · 64-QAM Rayleigh · perfect CSI · total power=1")
    axes[0].set_ylabel("BER")
    axes[1].set_ylabel("RMS EVM")
    axes[2].set_ylabel("CRC goodput (Mb/s)")
    axes[2].set_xlabel("Total-power Es/N0 (dB)")
    for axis in axes:
        axis.grid(True, linestyle=":", alpha=0.6)
        axis.legend()
    figure_path = output_dir / "mimo_2x2_4x4_8x8.png"
    fig.savefig(figure_path, dpi=180)
    plt.close(fig)
    print(f"Wrote {csv_path}")
    print(f"Figure {figure_path}")


if __name__ == "__main__":
    main()
