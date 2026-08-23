#!/usr/bin/env python3
"""Measure conventional Alamouti degradation under symbol-rate Doppler."""

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
    parser = argparse.ArgumentParser(description="OpenISAC Doppler/STBC comparison")
    parser.add_argument(
        "--config",
        type=Path,
        default=ROOT / "configs" / "mimo_2x2_alamouti_tdl_doppler_1024.yaml",
    )
    parser.add_argument("--doppler-hz", default="0,100,300,500,1000")
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=ROOT.parent / "measurement" / "doppler_validation",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    with args.config.open("r", encoding="utf-8") as handle:
        values = yaml.safe_load(handle) or {}
    base = SimulationConfig.from_mapping(values)
    dopplers = [float(item) for item in args.doppler_hz.split(",") if item.strip()]
    if not dopplers:
        raise ValueError("at least one Doppler point is required")

    rows = []
    for doppler_hz in dopplers:
        result = simulate_alamouti_ofdm(replace(base, doppler_hz=doppler_hz))
        rows.append(result)
        print(
            f"Doppler={doppler_hz:7.1f} Hz BER={result['ber']:.7g} "
            f"EVM={result['evm_rms']:.6g} "
            f"STBC-var={result['stbc_channel_variation_nmse']:.6g} "
            f"phase={result['mean_interslot_channel_phase_deg']:.3f} deg"
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

    x = [float(row["maximum_doppler_hz"]) for row in rows]
    fig, axes = plt.subplots(3, 1, figsize=(7.8, 10.2), constrained_layout=True)
    axes[0].semilogy(x, [max(float(row["ber"]), 1.0e-8) for row in rows], marker="o")
    axes[1].plot(x, [float(row["evm_rms"]) for row in rows], marker="s")
    axes[2].plot(
        x,
        [float(row["stbc_channel_variation_nmse"]) for row in rows],
        marker="^",
    )
    axes[0].set_title(
        f"1024/128 2x{base.nr} Alamouti TDL · Es/N0={base.snr_db:g} dB"
    )
    axes[0].set_ylabel("BER")
    axes[1].set_ylabel("RMS EVM")
    axes[2].set_ylabel("Slot H variation NMSE")
    axes[2].set_xlabel("Maximum path Doppler (Hz)")
    for item in axes:
        item.grid(True, linestyle=":", alpha=0.6)
    figure_path = output_dir / "doppler_stbc_degradation.png"
    fig.savefig(figure_path, dpi=180)
    plt.close(fig)
    print(f"Wrote {csv_path}")
    print(f"Figure {figure_path}")


if __name__ == "__main__":
    main()
