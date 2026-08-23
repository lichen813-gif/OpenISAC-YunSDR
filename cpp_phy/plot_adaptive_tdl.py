from __future__ import annotations

import argparse
import csv
import os
import tempfile
from collections import Counter, defaultdict
from pathlib import Path

os.environ.setdefault("MPLCONFIGDIR", str(Path(tempfile.gettempdir()) / "openisac-mpl"))
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np


def main() -> int:
    parser = argparse.ArgumentParser(description="Plot C++ adaptive TDL regression")
    parser.add_argument("csv_path", type=Path)
    parser.add_argument("--output-dir", type=Path, default=None)
    args = parser.parse_args()
    output_dir = args.output_dir or args.csv_path.parent
    output_dir.mkdir(parents=True, exist_ok=True)

    with args.csv_path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    grouped: dict[float, list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        grouped[float(row["snr_db"])].append(row)

    summary_rows: list[dict[str, object]] = []
    for snr in sorted(grouped):
        values = grouped[snr]
        count = len(values)
        adaptive_crc = np.asarray([int(row["adaptive_crc_ok"]) for row in values])
        fixed_crc = np.asarray([int(row["fixed_crc_ok"]) for row in values])
        smoothed_crc = np.asarray([int(row["smoothed_crc_ok"]) for row in values])
        fixed_header = np.asarray([int(row["fixed_header_ok"]) for row in values])
        smoothed_header = np.asarray(
            [int(row["smoothed_header_ok"]) for row in values]
        )
        steady_start = max(0, int(np.ceil(count * 0.60)))
        adaptive_goodput_values = np.asarray(
            [float(row["adaptive_goodput_bps"]) for row in values]
        ) / 1e6
        fixed_goodput_values = np.asarray(
            [float(row["fixed_goodput_bps"]) for row in values]
        ) / 1e6
        smoothed_goodput_values = np.asarray(
            [float(row["smoothed_goodput_bps"]) for row in values]
        ) / 1e6
        fixed_nmse_linear = np.power(
            10.0,
            np.asarray([float(row["fixed_channel_nmse_db"]) for row in values]) / 10.0,
        )
        smoothed_nmse_linear = np.power(
            10.0,
            np.asarray(
                [float(row["smoothed_channel_nmse_db"]) for row in values]
            ) / 10.0,
        )
        modes = Counter(row["adaptive_mode"] for row in values)
        summary_rows.append(
            {
                "snr_db": snr,
                "frames": count,
                "adaptive_fer": float(1.0 - adaptive_crc.mean()),
                "fixed_fer": float(1.0 - fixed_crc.mean()),
                "smoothed_fixed_fer": float(1.0 - smoothed_crc.mean()),
                "fixed_header_success": float(fixed_header.mean()),
                "smoothed_header_success": float(smoothed_header.mean()),
                "steady_window_frames": count - steady_start,
                "adaptive_steady_fer": float(1.0 - adaptive_crc[steady_start:].mean()),
                "fixed_steady_fer": float(1.0 - fixed_crc[steady_start:].mean()),
                "smoothed_fixed_steady_fer": float(
                    1.0 - smoothed_crc[steady_start:].mean()
                ),
                "adaptive_goodput_mbps": float(
                    adaptive_goodput_values.mean()
                ),
                "fixed_goodput_mbps": float(
                    fixed_goodput_values.mean()
                ),
                "smoothed_fixed_goodput_mbps": float(
                    smoothed_goodput_values.mean()
                ),
                "adaptive_steady_goodput_mbps": float(
                    adaptive_goodput_values[steady_start:].mean()
                ),
                "fixed_steady_goodput_mbps": float(
                    fixed_goodput_values[steady_start:].mean()
                ),
                "smoothed_fixed_steady_goodput_mbps": float(
                    smoothed_goodput_values[steady_start:].mean()
                ),
                "adaptive_mean_evm_percent": float(
                    np.mean([float(row["adaptive_evm_percent"]) for row in values])
                ),
                "fixed_mean_evm_percent": float(
                    np.mean([float(row["fixed_evm_percent"]) for row in values])
                ),
                "smoothed_mean_evm_percent": float(
                    np.mean(
                        [float(row["smoothed_evm_percent"]) for row in values]
                    )
                ),
                "fixed_mean_nmse_db": float(10.0 * np.log10(fixed_nmse_linear.mean())),
                "smoothed_mean_nmse_db": float(
                    10.0 * np.log10(smoothed_nmse_linear.mean())
                ),
                "adaptive_modes": ";".join(
                    f"{mode}:{modes[mode]}" for mode in sorted(modes)
                ),
            }
        )

    summary_path = output_dir / "summary.csv"
    with summary_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(summary_rows[0]))
        writer.writeheader()
        writer.writerows(summary_rows)

    snr = np.asarray([float(row["snr_db"]) for row in summary_rows])
    adaptive_fer = np.asarray(
        [float(row["adaptive_steady_fer"]) for row in summary_rows]
    )
    fixed_fer = np.asarray([float(row["fixed_steady_fer"]) for row in summary_rows])
    smoothed_fer = np.asarray(
        [float(row["smoothed_fixed_steady_fer"]) for row in summary_rows]
    )
    adaptive_goodput = np.asarray(
        [float(row["adaptive_goodput_mbps"]) for row in summary_rows]
    )
    fixed_goodput = np.asarray(
        [float(row["fixed_goodput_mbps"]) for row in summary_rows]
    )
    adaptive_steady_goodput = np.asarray(
        [float(row["adaptive_steady_goodput_mbps"]) for row in summary_rows]
    )
    fixed_steady_goodput = np.asarray(
        [float(row["fixed_steady_goodput_mbps"]) for row in summary_rows]
    )
    smoothed_steady_goodput = np.asarray(
        [float(row["smoothed_fixed_steady_goodput_mbps"]) for row in summary_rows]
    )

    figure, axes = plt.subplots(2, 1, figsize=(9.2, 8.0), sharex=True)
    axes[0].semilogy(snr, np.maximum(adaptive_fer, 1e-3), "o-", label="Adaptive")
    axes[0].semilogy(snr, np.maximum(fixed_fer, 1e-3), "s--", label="Fixed R2 64-QAM")
    axes[0].semilogy(
        snr, np.maximum(smoothed_fer, 1e-3), "^--",
        label="Fixed R2 64-QAM + CSI smoothing",
    )
    axes[0].set_ylabel("Steady-state frame error rate")
    axes[0].grid(True, which="both", alpha=0.3)
    axes[0].legend()
    axes[1].plot(snr, adaptive_steady_goodput, "o-", label="Adaptive steady-state")
    axes[1].plot(snr, fixed_steady_goodput, "s--", label="Fixed R2 64-QAM")
    axes[1].plot(
        snr, smoothed_steady_goodput, "^--",
        label="Fixed R2 64-QAM + CSI smoothing",
    )
    axes[1].plot(
        snr, adaptive_goodput, "o:", alpha=0.65,
        label="Adaptive incl. initial convergence",
    )
    axes[1].set_xlabel("SNR (dB)")
    axes[1].set_ylabel("CRC goodput (Mbit/s)")
    axes[1].grid(True, alpha=0.3)
    axes[1].legend()
    figure.suptitle("C++ dynamic Rank/MCS over synchronized 2x2 TDL")
    figure.tight_layout()
    figure.savefig(output_dir / "adaptive_vs_fixed.png", dpi=160)
    plt.close(figure)

    fixed_header_success = np.asarray(
        [float(row["fixed_header_success"]) for row in summary_rows]
    )
    smoothed_header_success = np.asarray(
        [float(row["smoothed_header_success"]) for row in summary_rows]
    )
    fixed_nmse = np.asarray(
        [float(row["fixed_mean_nmse_db"]) for row in summary_rows]
    )
    smoothed_nmse = np.asarray(
        [float(row["smoothed_mean_nmse_db"]) for row in summary_rows]
    )
    fixed_evm = np.asarray(
        [float(row["fixed_mean_evm_percent"]) for row in summary_rows]
    )
    smoothed_evm = np.asarray(
        [float(row["smoothed_mean_evm_percent"]) for row in summary_rows]
    )
    comparison, compare_axes = plt.subplots(3, 1, figsize=(9.2, 9.6), sharex=True)
    compare_axes[0].plot(snr, fixed_nmse, "s--", label="Raw LS-CSI")
    compare_axes[0].plot(snr, smoothed_nmse, "^-", label="EMA CSI, alpha=0.25")
    compare_axes[0].set_ylabel("Mean CSI NMSE (dB)")
    compare_axes[0].grid(True, alpha=0.3)
    compare_axes[0].legend()
    compare_axes[1].plot(snr, fixed_evm, "s--", label="Raw LS-CSI")
    compare_axes[1].plot(snr, smoothed_evm, "^-", label="EMA CSI, alpha=0.25")
    compare_axes[1].set_ylabel("Mean EVM (%)")
    compare_axes[1].grid(True, alpha=0.3)
    compare_axes[1].legend()
    compare_axes[2].plot(snr, fixed_header_success, "s--", label="Raw LS-CSI")
    compare_axes[2].plot(
        snr, smoothed_header_success, "^-", label="EMA CSI, alpha=0.25"
    )
    compare_axes[2].set_xlabel("SNR (dB)")
    compare_axes[2].set_ylabel("Soft-header success rate")
    compare_axes[2].set_ylim(-0.03, 1.03)
    compare_axes[2].grid(True, alpha=0.3)
    compare_axes[2].legend()
    comparison.suptitle("Paired C++ cross-frame CSI smoothing regression")
    comparison.tight_layout()
    comparison.savefig(output_dir / "csi_smoothing.png", dpi=160)
    plt.close(comparison)
    print(f"Summary: {summary_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
