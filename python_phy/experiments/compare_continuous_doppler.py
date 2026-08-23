#!/usr/bin/env python3
"""Expose OFDM ICI by comparing block and sample-continuous Doppler."""

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
    parser = argparse.ArgumentParser(
        description="Compare symbol-block and continuous Doppler for 2x2 SM"
    )
    parser.add_argument(
        "--config",
        type=Path,
        default=ROOT
        / "configs"
        / "mimo_2x2_spatial_multiplexing_continuous_doppler_1024.yaml",
    )
    parser.add_argument("--doppler-hz", default="0,100,300,500,1000")
    parser.add_argument("--frames", type=int, default=None)
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=ROOT.parent / "measurement" / "continuous_doppler_ici",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    with args.config.open("r", encoding="utf-8") as handle:
        values = yaml.safe_load(handle) or {}
    base = SimulationConfig.from_mapping(values)
    if args.frames is not None:
        base = replace(base, frames=args.frames)
    dopplers = [float(item) for item in args.doppler_hz.split(",") if item.strip()]
    if not dopplers:
        raise ValueError("at least one Doppler point is required")
    if 0.0 not in dopplers:
        raise ValueError("Doppler scan must include 0 Hz as the equivalence baseline")

    rows: list[dict[str, object]] = []
    for doppler_hz in dopplers:
        for model in ("symbol", "continuous"):
            result = simulate_mimo_ofdm(
                replace(base, doppler_hz=doppler_hz, doppler_model=model)
            )
            row = {"model": model, **result}
            rows.append(row)
            print(
                f"Doppler={doppler_hz:5.0f} Hz model={model:10s} "
                f"BER={result['ber']:.7g} EVM={result['evm_rms']:.6g} "
                f"intraNMSE={result['intrasymbol_channel_variation_nmse']:.6g} "
                f"phase={result['maximum_intrasymbol_phase_rotation_deg']:.2f} deg "
                f"CRC={result['crc_failure_rate']:.3f} "
                f"goodput={result['goodput_bps']/1e6:.3f} Mb/s"
            )

    by_key = {
        (float(row["maximum_doppler_hz"]), str(row["model"])): row
        for row in rows
    }
    zero_symbol = by_key[(0.0, "symbol")]
    zero_continuous = by_key[(0.0, "continuous")]
    high_doppler = max(dopplers)
    high_symbol = by_key[(high_doppler, "symbol")]
    high_continuous = by_key[(high_doppler, "continuous")]
    checks = {
        "zero_doppler_models_are_identical": float(zero_symbol["ber"])
        == float(zero_continuous["ber"])
        and float(zero_symbol["evm_rms"]) == float(zero_continuous["evm_rms"]),
        "continuous_model_reports_intrasymbol_variation": float(
            high_continuous["intrasymbol_channel_variation_nmse"]
        )
        > 0.05,
        "continuous_model_accumulates_over_20_degree_rotation": float(
            high_continuous["maximum_intrasymbol_phase_rotation_deg"]
        )
        > 20.0,
        "continuous_ici_more_than_doubles_evm": float(high_continuous["evm_rms"])
        > 2.0 * float(high_symbol["evm_rms"]),
        "continuous_ici_increases_ber_by_over_10x": float(high_continuous["ber"])
        > 10.0 * float(high_symbol["ber"]),
        "continuous_ici_reduces_crc_goodput_below_one_quarter": float(
            high_continuous["goodput_bps"]
        )
        < 0.25 * float(high_symbol["goodput_bps"]),
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
                "interpretation": (
                    "The symbol model updates taps only between OFDM symbols and cannot "
                    "create ICI. The continuous model rotates every active path at every "
                    "sample; its midpoint CSI removes the diagonal channel but not ICI."
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

    fig, axes = plt.subplots(2, 2, figsize=(11.0, 8.0), constrained_layout=True)
    for model, label, marker in (
        ("symbol", "Symbol-block channel", "o"),
        ("continuous", "Sample-continuous channel", "s"),
    ):
        selected = [row for row in rows if row["model"] == model]
        x = [float(row["maximum_doppler_hz"]) for row in selected]
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
        axes[1, 1].plot(
            x,
            [float(row["intrasymbol_channel_variation_nmse"]) for row in selected],
            marker=marker,
            label=label,
        )
    axes[0, 0].set_ylabel("BER")
    axes[0, 1].set_ylabel("RMS EVM")
    axes[1, 0].set_ylabel("CRC goodput (Mb/s)")
    axes[1, 1].set_ylabel("Intra-symbol channel variation NMSE")
    for axis in axes.flat:
        axis.set_xlabel("Maximum path Doppler (Hz)")
        axis.grid(True, linestyle=":", alpha=0.6)
        axis.legend(fontsize=8)
    axes[0, 0].set_title("2x2 SM MMSE · 64-QAM · 1024/128 · 40 dB")
    figure_path = output_dir / "continuous_doppler_ici.png"
    fig.savefig(figure_path, dpi=180)
    plt.close(fig)

    print(f"Checks: {sum(checks.values())}/{len(checks)} passed")
    print(f"Wrote {csv_path}")
    print(f"Report {report_path}")
    print(f"Figure {figure_path}")
    if not all(checks.values()):
        raise SystemExit("one or more continuous-Doppler checks failed")


if __name__ == "__main__":
    main()
