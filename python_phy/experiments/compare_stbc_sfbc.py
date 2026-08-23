#!/usr/bin/env python3
"""Compare time-paired STBC and adjacent-frequency SFBC under Doppler."""

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
    parser = argparse.ArgumentParser(description="OpenISAC STBC/SFBC Doppler comparison")
    parser.add_argument(
        "--config",
        type=Path,
        default=ROOT / "configs" / "mimo_2x2_alamouti_sfbc_tdl_doppler_1024.yaml",
    )
    parser.add_argument("--doppler-hz", default="0,100,300,500,1000")
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=ROOT.parent / "measurement" / "stbc_sfbc_validation",
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
    for pairing in ("time", "frequency"):
        for doppler_hz in dopplers:
            result = simulate_alamouti_ofdm(
                replace(base, pairing=pairing, doppler_hz=doppler_hz)
            )
            rows.append(result)
            print(
                f"{result['mode'].upper():4s} Doppler={doppler_hz:7.1f} Hz "
                f"BER={result['ber']:.7g} EVM={result['evm_rms']:.6g} "
                f"pair-var={result['alamouti_pair_channel_variation_nmse']:.6g}"
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
    styles = {"time": ("STBC", "o"), "frequency": ("SFBC", "s")}
    for pairing, (label, marker) in styles.items():
        selected = [row for row in rows if row["pairing"] == pairing]
        x = [float(row["maximum_doppler_hz"]) for row in selected]
        axes[0].semilogy(
            x,
            [max(float(row["ber"]), 1.0e-9) for row in selected],
            marker=marker,
            label=label,
        )
        axes[1].plot(
            x,
            [float(row["evm_rms"]) for row in selected],
            marker=marker,
            label=label,
        )
        axes[2].plot(
            x,
            [float(row["alamouti_pair_channel_variation_nmse"]) for row in selected],
            marker=marker,
            label=label,
        )
    axes[0].set_title(
        f"1024/128 2x{base.nr} Alamouti TDL · 64-QAM · Es/N0={base.snr_db:g} dB"
    )
    axes[0].set_ylabel("BER")
    axes[1].set_ylabel("RMS EVM")
    axes[2].set_ylabel("Alamouti pair H variation NMSE")
    axes[2].set_xlabel("Maximum path Doppler (Hz)")
    for axis in axes:
        axis.grid(True, linestyle=":", alpha=0.6)
        axis.legend()
    figure_path = output_dir / "stbc_vs_sfbc_doppler.png"
    fig.savefig(figure_path, dpi=180)
    plt.close(fig)
    print(f"Wrote {csv_path}")
    print(f"Figure {figure_path}")


if __name__ == "__main__":
    main()
