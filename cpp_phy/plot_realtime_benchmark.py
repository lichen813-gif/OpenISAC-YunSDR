from __future__ import annotations

import argparse
import csv
import os
import tempfile
from pathlib import Path

os.environ.setdefault("MPLCONFIGDIR", str(Path(tempfile.gettempdir()) / "openisac-mpl"))
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np


VARIANT_ORDER = [
    "local_legacy",
    "reused_legacy",
    "pool_1",
    "pool_2",
    "pool_4",
    "pool_8",
    "pool_8_track",
]
VARIANT_LABELS = {
    "local_legacy": "Local\nlegacy",
    "reused_legacy": "Reused\nlegacy",
    "pool_1": "Pool 1",
    "pool_2": "Pool 2",
    "pool_4": "Pool 4",
    "pool_8": "Pool 8",
    "pool_8_track": "Pool 8\ntrack",
}


def stats(values: np.ndarray) -> tuple[float, float, float, float]:
    return (
        float(values.mean()),
        float(np.median(values)),
        float(np.percentile(values, 95)),
        float(np.percentile(values, 99)),
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Plot C++ LDPC worker-pool benchmark")
    parser.add_argument("csv_path", type=Path)
    parser.add_argument("--output-dir", type=Path, default=None)
    args = parser.parse_args()
    output_dir = args.output_dir or args.csv_path.parent
    output_dir.mkdir(parents=True, exist_ok=True)
    with args.csv_path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    if not rows:
        raise RuntimeError("timing CSV is empty")

    grouped = {
        variant: [row for row in rows if row["variant"] == variant]
        for variant in VARIANT_ORDER
    }
    if any(not values for values in grouped.values()):
        raise RuntimeError("timing CSV is missing one or more benchmark variants")

    def column(variant: str, name: str) -> np.ndarray:
        return np.asarray([float(row[name]) for row in grouped[variant]])

    receiver_stats = {
        variant: stats(column(variant, "receiver_us")) for variant in VARIANT_ORDER
    }
    simulation_stats = {
        variant: stats(column(variant, "simulation_us")) for variant in VARIANT_ORDER
    }
    local_median = receiver_stats["local_legacy"][1]
    fastest = min(VARIANT_ORDER, key=lambda item: receiver_stats[item][1])
    summary: dict[str, float | int | str] = {
        "frames_per_variant": len(grouped["local_legacy"]),
        "frame_interval_us": 225.0,
        "fastest_variant": fastest,
        "fastest_receiver_median_us": receiver_stats[fastest][1],
        "fastest_deadline_ratio": receiver_stats[fastest][1] / 225.0,
        "fastest_speedup_vs_local": local_median / receiver_stats[fastest][1],
    }
    for variant in VARIANT_ORDER:
        mean_us, median_us, p95_us, p99_us = receiver_stats[variant]
        summary.update(
            {
                f"{variant}_receiver_mean_us": mean_us,
                f"{variant}_receiver_median_us": median_us,
                f"{variant}_receiver_p95_us": p95_us,
                f"{variant}_receiver_p99_us": p99_us,
                f"{variant}_receiver_speedup_vs_local": local_median / median_us,
                f"{variant}_simulation_median_us": simulation_stats[variant][1],
                f"{variant}_control_fec_mean_us": float(
                    column(variant, "control_fec_us").mean()
                ),
                f"{variant}_soft_demapping_mean_us": float(
                    column(variant, "soft_demapping_us").mean()
                ),
                f"{variant}_ldpc_crc_mean_us": float(
                    column(variant, "ldpc_crc_us").mean()
                ),
                f"{variant}_pipeline_interval_median_us": float(
                    np.median(column(variant, "pipeline_interval_us"))
                ),
                f"{variant}_pipeline_interval_p99_us": float(
                    np.percentile(column(variant, "pipeline_interval_us"), 99)
                ),
                f"{variant}_pipeline_throughput_fps": float(
                    1.0e6 / np.median(column(variant, "pipeline_interval_us"))
                ),
                f"{variant}_synchronization_mean_us": float(
                    column(variant, "sync_us").mean()
                ),
                f"{variant}_timing_candidates_mean": float(
                    column(variant, "timing_candidates").mean()
                ),
                f"{variant}_crc_success_rate": float(
                    column(variant, "crc_ok").mean()
                ),
                f"{variant}_workspace_growths_after_warmup": int(
                    column(variant, "workspace_growths").sum()
                ),
                f"{variant}_ldpc_growths_after_warmup": int(
                    column(variant, "ldpc_growths").sum()
                ),
            }
        )
    summary_path = output_dir / "summary.csv"
    with summary_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(summary))
        writer.writeheader()
        writer.writerow(summary)

    figure, axes = plt.subplots(1, 2, figsize=(14.0, 5.3))
    positions = np.arange(len(VARIANT_ORDER))
    width = 0.24
    for offset, stat_index, label in (
        (-width, 0, "Mean"),
        (0.0, 1, "Median"),
        (width, 2, "P95"),
    ):
        axes[0].bar(
            positions + offset,
            [receiver_stats[variant][stat_index] for variant in VARIANT_ORDER],
            width,
            label=label,
        )
    axes[0].set_xticks(positions, [VARIANT_LABELS[item] for item in VARIANT_ORDER])
    axes[0].set_ylabel("Receiver algorithm time (us/frame)")
    axes[0].axhline(
        225.0, color="red", linestyle=":", linewidth=1.5,
        label="225 us frame interval",
    )
    axes[0].grid(True, axis="y", alpha=0.3)
    axes[0].legend(ncols=2)

    stage_specs = [
        ("sync_us", "Sync"),
        ("fft_csi_us", "FFT + CSI"),
        ("detection_us", "Detection"),
        ("control_header_us", "Control header"),
        ("soft_demapping_us", "Soft demapping"),
        ("ldpc_crc_us", "LDPC + CRC"),
    ]
    bottom = np.zeros(len(VARIANT_ORDER))
    for column_name, label in stage_specs:
        values = np.asarray(
            [column(variant, column_name).mean() for variant in VARIANT_ORDER]
        )
        axes[1].bar(positions, values, bottom=bottom, label=label)
        bottom += values
    axes[1].set_xticks(positions, [VARIANT_LABELS[item] for item in VARIANT_ORDER])
    axes[1].set_ylabel("Mean receiver stage time (us/frame)")
    axes[1].grid(True, axis="y", alpha=0.3)
    axes[1].legend()
    axes[1].set_title(
        f"Fastest: {VARIANT_LABELS[fastest].replace(chr(10), ' ')}; "
        f"{local_median / receiver_stats[fastest][1]:.2f}x vs local"
    )
    figure.suptitle(
        "VS2019 C++ Rank-2/64-QAM TDL: LDPC pool and continuous tracking"
    )
    figure.tight_layout()
    figure.savefig(output_dir / "realtime_timing.png", dpi=160)
    plt.close(figure)
    print(f"Summary: {summary_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
