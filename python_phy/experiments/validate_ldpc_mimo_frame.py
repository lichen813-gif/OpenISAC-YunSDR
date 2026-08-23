#!/usr/bin/env python3
"""Validate the formal mixed-control LDPC 2x2 MIMO frame."""

from __future__ import annotations

import argparse
import csv
import json
import os
import sys
import time
from dataclasses import replace
from pathlib import Path

import numpy as np
import yaml

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
os.environ.setdefault("MPLCONFIGDIR", str(ROOT / ".matplotlib"))

import matplotlib  # noqa: E402

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

from openisac_phy import SimulationConfig, simulate_mimo_ofdm  # noqa: E402


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--config",
        type=Path,
        default=ROOT
        / "configs"
        / "mimo_2x2_spatial_multiplexing_ldpc_realtime_1024.yaml",
    )
    parser.add_argument("--frames", type=int, default=50)
    parser.add_argument("--snr-db", default="20,24,28,32,36,40")
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=ROOT.parent / "measurement" / "ldpc_mimo_frame",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    values = yaml.safe_load(args.config.read_text(encoding="utf-8")) or {}
    base = replace(SimulationConfig.from_mapping(values), frames=args.frames)
    snrs = [float(value) for value in args.snr_db.split(",") if value.strip()]
    if not snrs:
        raise ValueError("at least one SNR point is required")

    rows: list[dict[str, object]] = []
    started = time.perf_counter()
    for snr_db in snrs:
        point_started = time.perf_counter()
        result = simulate_mimo_ofdm(replace(base, snr_db=snr_db))
        elapsed = time.perf_counter() - point_started
        row = {"snr_db": snr_db, "wall_time_s": elapsed, **result}
        rows.append(row)
        print(
            f"SNR={snr_db:5.1f} dB "
            f"pre={float(result['pre_ldpc_ber']):.7g} "
            f"post={float(result['post_ldpc_ber']):.7g} "
            f"CRC={float(result['crc_failure_rate']):.3f} "
            f"header={float(result['control_header_failure_rate']):.3f} "
            f"EVM={float(result['evm_rms']):.5f} "
            f"time={elapsed:.2f}s"
        )

    highest = rows[-1]
    checks = {
        "formal_resource_count_is_14_blocks": int(highest["ldpc_payload_blocks"])
        == 14,
        "formal_user_payload_is_880_bytes": int(highest["ldpc_user_payload_bytes"])
        == 880,
        "control_region_is_128_re": int(highest["ldpc_control_re_count"]) == 128,
        "high_snr_post_ber_not_worse_than_pre_ber": float(
            highest["post_ldpc_ber"]
        )
        <= float(highest["pre_ldpc_ber"]),
        "high_snr_crc_failure_at_most_10_percent": float(
            highest["crc_failure_rate"]
        )
        <= 0.10,
        "high_snr_control_header_failure_at_most_1_percent": float(
            highest["control_header_failure_rate"]
        )
        <= 0.01,
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
                "source_config": str(args.config.resolve()),
                "frames_per_point": args.frames,
                "wall_time_s": time.perf_counter() - started,
                "checks": checks,
                "all_checks_passed": all(checks.values()),
                "results": rows,
            },
            indent=2,
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )

    snr_values = np.asarray([float(row["snr_db"]) for row in rows])
    pre = np.maximum(
        np.asarray([float(row["pre_ldpc_ber"]) for row in rows]), 1.0e-8
    )
    post = np.maximum(
        np.asarray([float(row["post_ldpc_ber"]) for row in rows]), 1.0e-8
    )
    crc = np.maximum(
        np.asarray([float(row["crc_failure_rate"]) for row in rows]), 1.0e-8
    )
    figure, axis = plt.subplots(figsize=(8.0, 5.0))
    axis.semilogy(snr_values, pre, "o-", label="Pre-LDPC BER")
    axis.semilogy(snr_values, post, "s-", label="Post-LDPC BER")
    axis.semilogy(snr_values, crc, "^-", label="CRC frame failure rate")
    axis.set_xlabel("SNR (dB)")
    axis.set_ylabel("Error rate")
    axis.set_title("2x2 MMSE / 64-QAM / LDPC(1008,504)")
    axis.grid(True, which="both", alpha=0.3)
    axis.legend()
    figure.tight_layout()
    plot_path = output_dir / "error_rates.png"
    figure.savefig(plot_path, dpi=160)
    plt.close(figure)

    print(f"Checks: {sum(checks.values())}/{len(checks)} passed")
    print(f"Wrote {csv_path}")
    print(f"Report {report_path}")
    print(f"Plot {plot_path}")
    if not all(checks.values()):
        raise SystemExit("one or more LDPC MIMO frame checks failed")


if __name__ == "__main__":
    main()
