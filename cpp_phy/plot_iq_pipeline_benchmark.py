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
        description="Plot C++ pre-generated IQ receiver pipeline benchmark"
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
        raise RuntimeError("IQ receiver timing CSV is empty")

    def column(name: str) -> np.ndarray:
        return np.asarray([float(row[name]) for row in rows])

    serial_interval = float(summary["serial_rx_interval_us"])
    pipeline_interval = float(summary["pipeline_rx_interval_us"])
    figure, axes = plt.subplots(1, 3, figsize=(17.2, 5.5))
    axes[0].bar(
        ["Serial RX", "Two-buffer RX"],
        [serial_interval, pipeline_interval],
        color=["#778DA9", "#2A9D8F"],
    )
    axes[0].axhline(225.0, color="#D62828", linestyle=":", label="225 us target")
    axes[0].set_ylabel("Measured RX interval (us/frame)")
    axes[0].set_title(
        f"Hardware-format IQ: {float(summary['rx_speedup']):.2f}x speedup"
    )
    axes[0].grid(True, axis="y", alpha=0.3)
    axes[0].legend()

    axes[1].boxplot(
        [
            column("pipeline_submit_call_us"),
            column("pipeline_rx_submit_us"),
            column("pipeline_fec_us"),
            column("pipeline_latency_us"),
        ],
        tick_labels=["Submit call", "RX front", "FEC", "RX latency"],
        showfliers=False,
        whis=(0.1, 99.9),
    )
    axes[1].axhline(225.0, color="#D62828", linestyle=":", label="225 us target")
    axes[1].set_ylabel("Per-frame time (us)")
    axes[1].set_title(
        "Tail timing (0.1st-99.9th percentile)\n"
        f"submit P99.9 {float(summary['submit_call_p999_us']):.1f} us; "
        f"miss {100.0 * float(summary['submit_deadline_miss_rate']):.2f}%"
    )
    axes[1].grid(True, axis="y", alpha=0.3)
    axes[1].legend()

    stage_names = [
        "Sync", "FFT", "SFO", "Noise", "CSI est.", "CSI smooth",
        "Detect", "Control", "Demap",
    ]
    stage_means = [
        float(summary["synchronization_mean_us"]),
        float(summary["fft_grid_mean_us"]),
        float(summary["sfo_correction_mean_us"]),
        float(summary["noise_estimation_mean_us"]),
        float(summary["channel_estimation_mean_us"]),
        float(summary["csi_smoothing_mean_us"]),
        float(summary["detection_adaptation_mean_us"]),
        float(summary["control_header_mean_us"]),
        float(summary["soft_demapping_mean_us"]),
    ]
    axes[2].bar(stage_names, stage_means, color="#457B9D")
    axes[2].set_ylabel("Mean time (us/frame)")
    axes[2].set_title("RX front-end stage breakdown")
    axes[2].tick_params(axis="x", rotation=20)
    axes[2].grid(True, axis="y", alpha=0.3)
    figure.suptitle(
        "VS2019 C++ Rank-2/64-QAM hardware-format IQ receiver pipeline\n"
        f"{summary['frames']} frames; {summary['scheduling_profile']} scheduling; "
        "transmitter, TDL/AWGN and truth diagnostics excluded"
    )
    figure.tight_layout()
    output = output_dir / "iq_pipeline_timing.png"
    figure.savefig(output, dpi=160)
    plt.close(figure)
    print(f"Plot: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
