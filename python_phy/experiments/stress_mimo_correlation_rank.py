#!/usr/bin/env python3
"""Stress full-layer spatial MIMO with antenna correlation and rank loss."""

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
        raise ValueError("at least one correlation point is required")
    return values


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="OpenISAC correlated/rank-limited MIMO")
    parser.add_argument(
        "--config",
        type=Path,
        default=ROOT / "configs" / "mimo_2x2_spatial_multiplexing_correlated_rayleigh_256.yaml",
    )
    parser.add_argument("--correlations", default="0,0.5,0.8,0.95,0.99,1")
    parser.add_argument("--frames", type=int, default=None)
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=ROOT.parent / "measurement" / "mimo_correlation_rank",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    with args.config.open("r", encoding="utf-8") as handle:
        values = yaml.safe_load(handle) or {}
    base = SimulationConfig.from_mapping(values)
    if args.frames is not None:
        base = replace(base, frames=args.frames)
    correlations = parse_numbers(args.correlations)

    correlation_rows: list[dict[str, object]] = []
    for detector in ("zf", "mmse"):
        for coefficient in correlations:
            result = simulate_mimo_ofdm(
                replace(
                    base,
                    detector=detector,
                    tx_correlation=coefficient,
                    rx_correlation=coefficient,
                    spatial_rank=0,
                )
            )
            row = {
                "sweep": "correlation",
                "requested_rank": 0,
                "rank_fraction": 1.0,
                **result,
            }
            correlation_rows.append(row)
            print(
                f"2x2 {detector.upper():4s} rho={coefficient:4.2f} "
                f"rank={result['mean_channel_rank']:.2f} "
                f"cond={result['mean_channel_condition_number']:.3g} "
                f"BER={result['ber']:.7g} EVM={result['evm_rms']:.6g} "
                f"goodput={result['goodput_bps']/1e6:.3f} Mb/s"
            )

    rank_rows: list[dict[str, object]] = []
    rank_sets = {2: (2, 1), 4: (4, 3, 2, 1), 8: (8, 6, 4, 2, 1)}
    for antennas, ranks in rank_sets.items():
        for requested_rank in ranks:
            result = simulate_mimo_ofdm(
                replace(
                    base,
                    nt=antennas,
                    nr=antennas,
                    layers=antennas,
                    detector="mmse",
                    tx_correlation=0.0,
                    rx_correlation=0.0,
                    spatial_rank=(0 if requested_rank == antennas else requested_rank),
                )
            )
            row = {
                "sweep": "rank",
                "requested_rank": requested_rank,
                "rank_fraction": requested_rank / antennas,
                **result,
            }
            rank_rows.append(row)
            print(
                f"{antennas}x{antennas} MMSE rank={requested_rank}/{antennas} "
                f"measured={result['mean_channel_rank']:.2f} "
                f"BER={result['ber']:.7g} EVM={result['evm_rms']:.6g} "
                f"goodput={result['goodput_bps']/1e6:.3f} Mb/s"
            )

    all_rows = correlation_rows + rank_rows
    correlation_lookup = {
        (str(row["detector"]), float(row["tx_correlation"])): row
        for row in correlation_rows
    }
    high_reference = max(value for value in correlations if value < 1.0)
    rank_one = [row for row in rank_rows if int(row["requested_rank"]) == 1]
    full_rank = {
        int(row["nt"]): row
        for row in rank_rows
        if int(row["requested_rank"]) == int(row["nt"])
    }
    checks = {
        "rho_one_is_rank_one": all(
            float(correlation_lookup[(detector, 1.0)]["mean_channel_rank"]) == 1.0
            for detector in ("zf", "mmse")
        ) if 1.0 in correlations else True,
        "high_correlation_worsens_condition_number": all(
            float(correlation_lookup[(detector, high_reference)]["mean_channel_condition_number"])
            > float(correlation_lookup[(detector, min(correlations))]["mean_channel_condition_number"])
            for detector in ("zf", "mmse")
        ),
        "mmse_evm_below_zf_at_high_correlation": (
            float(correlation_lookup[("mmse", high_reference)]["evm_rms"])
            < float(correlation_lookup[("zf", high_reference)]["evm_rms"])
        ),
        "requested_ranks_are_measured_exactly": all(
            float(row["mean_channel_rank"]) == float(row["requested_rank"])
            for row in rank_rows
        ),
        "rank_one_has_zero_crc_goodput": all(
            float(row["goodput_bps"]) == 0.0 for row in rank_one
        ),
        "full_rank_ber_is_below_rank_one": all(
            float(full_rank[int(row["nt"])]["ber"])
            < float(row["ber"])
            for row in rank_one
        ),
    }

    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    csv_path = output_dir / "results.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(all_rows[0].keys()))
        writer.writeheader()
        writer.writerows(all_rows)
    report_path = output_dir / "report.json"
    report_path.write_text(
        json.dumps(
            {
                "config": values,
                "frames_override": args.frames,
                "correlation_results": correlation_rows,
                "rank_results": rank_rows,
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
    for detector, marker in (("zf", "o"), ("mmse", "s")):
        selected = [row for row in correlation_rows if row["detector"] == detector]
        x = [float(row["tx_correlation"]) for row in selected]
        axes[0, 0].semilogy(x, [max(float(row["ber"]), 1e-7) for row in selected], marker=marker, label=detector.upper())
        axes[0, 1].semilogy(x, [float(row["evm_rms"]) for row in selected], marker=marker, label=detector.upper())
        axes[1, 0].semilogy(x, [float(row["mean_channel_condition_number"]) for row in selected], marker=marker, label=detector.upper())
        axes[1, 1].plot(x, [float(row["goodput_bps"])/1e6 for row in selected], marker=marker, label=detector.upper())
    axes[0, 0].set_title("2x2 Kronecker correlation · 64-QAM Rayleigh · perfect CSI")
    axes[0, 0].set_ylabel("BER")
    axes[0, 1].set_ylabel("RMS EVM")
    axes[1, 0].set_ylabel("Mean condition number")
    axes[1, 1].set_ylabel("CRC goodput (Mb/s)")
    for axis in axes.flat:
        axis.set_xlabel("Tx/Rx correlation coefficient")
        axis.grid(True, linestyle=":", alpha=0.6)
        axis.legend()
    correlation_figure = output_dir / "mimo_correlation_sweep.png"
    fig.savefig(correlation_figure, dpi=180)
    plt.close(fig)

    fig, axes = plt.subplots(1, 3, figsize=(13.0, 4.2), constrained_layout=True)
    for antennas, marker in ((2, "o"), (4, "s"), (8, "D")):
        selected = [row for row in rank_rows if int(row["nt"]) == antennas]
        selected.sort(key=lambda row: float(row["rank_fraction"]))
        x = [float(row["rank_fraction"]) for row in selected]
        axes[0].semilogy(x, [max(float(row["ber"]), 1e-7) for row in selected], marker=marker, label=f"{antennas}x{antennas}")
        axes[1].plot(x, [float(row["goodput_bps"])/1e6 for row in selected], marker=marker, label=f"{antennas}x{antennas}")
        axes[2].semilogy(x, [float(row["mean_channel_condition_number"]) for row in selected], marker=marker, label=f"{antennas}x{antennas}")
    axes[0].set_title("Exact-rank MMSE stress")
    axes[0].set_ylabel("BER")
    axes[1].set_ylabel("CRC goodput (Mb/s)")
    axes[2].set_ylabel("Mean condition number")
    for axis in axes:
        axis.set_xlabel("Available rank / transmitted layers")
        axis.grid(True, linestyle=":", alpha=0.6)
        axis.legend()
    rank_figure = output_dir / "mimo_rank_sweep.png"
    fig.savefig(rank_figure, dpi=180)
    plt.close(fig)

    print(f"Checks: {sum(checks.values())}/{len(checks)} passed")
    print(f"Wrote {csv_path}")
    print(f"Report {report_path}")
    print(f"Figures {correlation_figure}, {rank_figure}")
    if not all(checks.values()):
        raise SystemExit("one or more correlated-MIMO checks failed")


if __name__ == "__main__":
    main()
