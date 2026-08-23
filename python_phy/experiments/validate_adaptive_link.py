#!/usr/bin/env python3
"""Validate delayed rank/MCS feedback against fixed rank-2 64-QAM."""

from __future__ import annotations

import argparse
import csv
import json
import os
import sys
import time
from collections import Counter
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
from openisac_phy.adaptive_link import (  # noqa: E402
    AdaptiveLinkController,
    LinkMode,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--config",
        type=Path,
        default=ROOT
        / "configs"
        / "mimo_2x2_spatial_multiplexing_ldpc_realtime_1024.yaml",
    )
    parser.add_argument("--frames", type=int, default=64)
    parser.add_argument("--coherence-frames", type=int, default=8)
    parser.add_argument("--upshift-confirmation", type=int, default=3)
    parser.add_argument("--snr-db", type=float, default=40.0)
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=ROOT.parent / "measurement" / "adaptive_link",
    )
    return parser.parse_args()


def desired_mode(result: dict[str, object]) -> LinkMode:
    rank = 2 if float(result["recommended_rank2_rate"]) >= 0.5 else 1
    counts = dict(result["recommended_mcs_counts"])
    modulation = max(counts, key=lambda name: int(counts[name]))
    return LinkMode(rank, modulation)


def run_one(
    base: SimulationConfig,
    mode: LinkMode,
    frame: int,
    channel_seed: int,
) -> dict[str, object]:
    return simulate_mimo_ofdm(
        replace(
            base,
            frames=1,
            modulation=mode.modulation,
            ldpc_transmit_rank=mode.rank,
            seed=base.seed + frame,
            channel_seed=channel_seed,
            noise_seed=(base.noise_seed or base.seed + 20000) + frame,
        )
    )


def main() -> None:
    args = parse_args()
    if args.frames <= 0 or args.coherence_frames <= 0:
        raise ValueError("frame and coherence counts must be positive")
    values = yaml.safe_load(args.config.read_text(encoding="utf-8")) or {}
    base = replace(
        SimulationConfig.from_mapping(values),
        frames=1,
        snr_db=args.snr_db,
    )
    if base.fec_mode != "ldpc_1008_504":
        raise ValueError("adaptive validation requires the formal LDPC frame")

    fixed_mode = LinkMode(2, "64qam")
    controller = AdaptiveLinkController(
        initial_mode=fixed_mode,
        upshift_confirmation_frames=args.upshift_confirmation,
    )
    rows: list[dict[str, object]] = []
    started = time.perf_counter()
    base_channel_seed = base.channel_seed or base.seed + 10000

    for frame in range(args.frames):
        channel_block = frame // args.coherence_frames
        channel_seed = base_channel_seed + channel_block
        used_mode = controller.current
        adaptive = run_one(base, used_mode, frame, channel_seed)
        fixed = (
            adaptive
            if used_mode == fixed_mode
            else run_one(base, fixed_mode, frame, channel_seed)
        )
        desired = desired_mode(adaptive)
        adaptive_crc_failed = int(adaptive["crc_failures"]) != 0
        update = controller.observe(
            desired,
            crc_failed=adaptive_crc_failed,
            outage=bool(float(adaptive["link_adaptation_outage_rate"]) > 0.0),
        )
        row = {
            "frame": frame,
            "channel_block": channel_block,
            "channel_seed": channel_seed,
            "used_rank": used_mode.rank,
            "used_modulation": used_mode.modulation,
            "desired_rank": desired.rank,
            "desired_modulation": desired.modulation,
            "next_rank": update.selected.rank,
            "next_modulation": update.selected.modulation,
            "controller_reason": update.reason,
            "pending_upshift_count": update.pending_upshift_count,
            "adaptive_crc_failed": adaptive_crc_failed,
            "adaptive_header_failed": int(adaptive["control_header_failures"]) != 0,
            "adaptive_post_ldpc_ber": float(adaptive["post_ldpc_ber"]),
            "adaptive_evm_rms": float(adaptive["evm_rms"]),
            "adaptive_payload_bytes": int(adaptive["ldpc_user_payload_bytes"]),
            "adaptive_goodput_bps": float(adaptive["goodput_bps"]),
            "fixed_crc_failed": int(fixed["crc_failures"]) != 0,
            "fixed_post_ldpc_ber": float(fixed["post_ldpc_ber"]),
            "fixed_goodput_bps": float(fixed["goodput_bps"]),
            "rank2_bottleneck_sinr_db": float(
                adaptive["mean_rank2_bottleneck_sinr_db"]
            ),
            "minimum_eigenvalue_ratio": float(
                adaptive["mean_minimum_eigenvalue_ratio"]
            ),
        }
        rows.append(row)
        print(
            f"frame={frame:03d} block={channel_block:02d} "
            f"use=R{used_mode.rank}/{used_mode.modulation:7s} "
            f"want=R{desired.rank}/{desired.modulation:7s} "
            f"next=R{update.selected.rank}/{update.selected.modulation:7s} "
            f"CRC={int(adaptive_crc_failed)} fixed={int(row['fixed_crc_failed'])} "
            f"reason={update.reason}"
        )

    frame_duration_s = float(
        simulate_mimo_ofdm(
            replace(base, modulation="qpsk", ldpc_transmit_rank=1)
        )["frame_duration_s"]
    )
    adaptive_delivered_bits = sum(
        int(row["adaptive_payload_bytes"]) * 8
        for row in rows
        if not bool(row["adaptive_crc_failed"])
    )
    fixed_payload_bytes = replace(
        base, modulation="64qam", ldpc_transmit_rank=2
    ).payload_bytes
    fixed_delivered_bits = sum(
        fixed_payload_bytes * 8
        for row in rows
        if not bool(row["fixed_crc_failed"])
    )
    total_duration_s = args.frames * frame_duration_s
    adaptive_goodput = adaptive_delivered_bits / total_duration_s
    fixed_goodput = fixed_delivered_bits / total_duration_s
    adaptive_fer = float(
        np.mean([bool(row["adaptive_crc_failed"]) for row in rows])
    )
    fixed_fer = float(np.mean([bool(row["fixed_crc_failed"]) for row in rows]))
    mode_counts = Counter(
        f"R{row['used_rank']}/{row['used_modulation']}" for row in rows
    )
    reasons = Counter(str(row["controller_reason"]) for row in rows)
    summary = {
        "frames": args.frames,
        "coherence_frames": args.coherence_frames,
        "feedback_delay_frames": 1,
        "upshift_confirmation_frames": args.upshift_confirmation,
        "snr_db": args.snr_db,
        "rank1_implementation": "Tx0 single stream with 2Rx MRC",
        "adaptive_crc_failure_rate": adaptive_fer,
        "fixed_rank2_64qam_crc_failure_rate": fixed_fer,
        "adaptive_goodput_bps": adaptive_goodput,
        "fixed_rank2_64qam_goodput_bps": fixed_goodput,
        "adaptive_to_fixed_goodput_ratio": adaptive_goodput
        / max(fixed_goodput, 1.0e-30),
        "mode_counts": dict(mode_counts),
        "controller_reason_counts": dict(reasons),
        "wall_time_s": time.perf_counter() - started,
    }
    checks = {
        "all_control_headers_decode": not any(
            bool(row["adaptive_header_failed"]) for row in rows
        ),
        "controller_uses_rank1_and_rank2": any(
            int(row["used_rank"]) == 1 for row in rows
        )
        and any(int(row["used_rank"]) == 2 for row in rows),
        "controller_uses_multiple_mcs": len(
            {str(row["used_modulation"]) for row in rows}
        )
        >= 2,
        "adaptive_fer_not_worse_than_fixed": adaptive_fer <= fixed_fer,
        "adaptive_goodput_at_least_80_percent_of_fixed": adaptive_goodput
        >= 0.8 * fixed_goodput,
    }

    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    csv_path = output_dir / "frames.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)
    report_path = output_dir / "report.json"
    report_path.write_text(
        json.dumps(
            {
                "source_config": str(args.config.resolve()),
                "summary": summary,
                "checks": checks,
                "all_checks_passed": all(checks.values()),
            },
            indent=2,
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )

    frames = np.arange(args.frames)
    adaptive_cumulative = np.cumsum(
        [
            int(row["adaptive_payload_bytes"]) * 8
            if not bool(row["adaptive_crc_failed"])
            else 0
            for row in rows
        ]
    ) / ((frames + 1) * frame_duration_s)
    fixed_cumulative = np.cumsum(
        [
            fixed_payload_bytes * 8 if not bool(row["fixed_crc_failed"]) else 0
            for row in rows
        ]
    ) / ((frames + 1) * frame_duration_s)
    mode_efficiency = np.asarray(
        [
            LinkMode(
                int(row["used_rank"]), str(row["used_modulation"])
            ).nominal_bits_per_re
            for row in rows
        ]
    )
    figure, axes = plt.subplots(3, 1, figsize=(10.0, 8.0), sharex=True)
    axes[0].step(frames, mode_efficiency, where="post", label="Adaptive mode")
    axes[0].set_ylabel("Rank x Qm")
    axes[0].grid(True, alpha=0.3)
    axes[1].scatter(
        frames,
        [int(bool(row["adaptive_crc_failed"])) for row in rows],
        marker="o",
        label="Adaptive",
    )
    axes[1].scatter(
        frames,
        [int(bool(row["fixed_crc_failed"])) + 0.05 for row in rows],
        marker="x",
        label="Fixed R2/64-QAM",
    )
    axes[1].set_ylabel("CRC fail")
    axes[1].legend()
    axes[1].grid(True, alpha=0.3)
    axes[2].plot(frames, adaptive_cumulative / 1.0e6, label="Adaptive")
    axes[2].plot(frames, fixed_cumulative / 1.0e6, label="Fixed R2/64-QAM")
    axes[2].set_xlabel("Frame")
    axes[2].set_ylabel("Cumulative goodput (Mb/s)")
    axes[2].legend()
    axes[2].grid(True, alpha=0.3)
    figure.suptitle("Delayed rank/MCS closed loop")
    figure.tight_layout()
    plot_path = output_dir / "closed_loop.png"
    figure.savefig(plot_path, dpi=160)
    plt.close(figure)

    print(json.dumps(summary, indent=2, ensure_ascii=False))
    print(f"Checks: {sum(checks.values())}/{len(checks)} passed")
    print(f"Frames {csv_path}")
    print(f"Report {report_path}")
    print(f"Plot {plot_path}")
    if not all(checks.values()):
        raise SystemExit("one or more adaptive-link checks failed")


if __name__ == "__main__":
    main()
