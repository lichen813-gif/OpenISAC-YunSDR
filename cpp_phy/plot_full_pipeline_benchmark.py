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


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Plot C++ complete dynamic-link two-buffer benchmark"
    )
    parser.add_argument("frames_csv", type=Path)
    parser.add_argument("--output-dir", type=Path, default=None)
    args = parser.parse_args()
    output_dir = args.output_dir or args.frames_csv.parent
    with args.frames_csv.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    with (args.frames_csv.parent / "summary.csv").open(
        newline="", encoding="utf-8"
    ) as stream:
        summary = next(csv.DictReader(stream))
    if not rows:
        raise RuntimeError("complete pipeline timing CSV is empty")

    def column(name: str) -> np.ndarray:
        return np.asarray([float(row[name]) for row in rows])

    serial_interval = float(summary["serial_simulator_interval_us"])
    pipeline_interval = float(summary["pipeline_simulator_interval_us"])
    figure, axes = plt.subplots(1, 2, figsize=(12.8, 5.2))
    axes[0].bar(
        ["Serial simulator", "Two-buffer\nsimulator"],
        [serial_interval, pipeline_interval],
        color=["#778DA9", "#2A9D8F"],
    )
    axes[0].set_ylabel("Measured wall interval (us/frame)")
    axes[0].set_title(
        f"Complete-link simulator: {float(summary['simulator_speedup']):.2f}x"
    )
    axes[0].grid(True, axis="y", alpha=0.3)

    axes[1].boxplot(
        [
            column("pipeline_receiver_front_us"),
            column("pipeline_fec_wall_us"),
            column("pipeline_queue_wait_us"),
            column("pipeline_latency_us"),
        ],
        tick_labels=["RX front", "FEC", "Queue", "Full latency"],
        showfliers=False,
        whis=(1, 99),
    )
    axes[1].set_ylabel("Per-frame time (us)")
    axes[1].set_title("Complete two-slot timing (1st-99th percentile)")
    axes[1].grid(True, axis="y", alpha=0.3)
    figure.suptitle(
        "VS2019 C++ Rank-2/64-QAM complete dynamic-link pipeline\n"
        "Wall interval includes transmitter/channel simulation; RX front does not"
    )
    figure.tight_layout()
    output = output_dir / "full_pipeline_timing.png"
    figure.savefig(output, dpi=160)
    plt.close(figure)
    print(f"Plot: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
