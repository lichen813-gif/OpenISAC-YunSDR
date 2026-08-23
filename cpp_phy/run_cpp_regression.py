"""Run the Windows C++ PHY over repeatable SNR/offset seeds and aggregate FER."""

from __future__ import annotations

import csv
import os
import subprocess
import sys
from collections import defaultdict
from pathlib import Path


def read_metrics(path: Path) -> dict[str, str]:
    with path.open(newline="", encoding="utf-8") as stream:
        return {row["metric"]: row["value"] for row in csv.DictReader(stream)}


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    executable = Path(sys.argv[1]) if len(sys.argv) > 1 else (
        root / "cpp_phy" / "build" / "ninja-vs2019" / "openisac_phy_diagnostics.exe"
    )
    output_root = Path(sys.argv[2]) if len(sys.argv) > 2 else (
        root / "measurement" / "cpp_windows_regression"
    )
    if not executable.is_file():
        raise FileNotFoundError(f"C++ diagnostics executable not found: {executable}")
    output_root.mkdir(parents=True, exist_ok=True)

    snrs = (28, 32, 36, 40, 42)
    seeds = (101, 202, 303, 404, 505)
    timings = (0, 7, 20, 37, 55)
    rows: list[dict[str, str]] = []
    for snr in snrs:
        for index, seed in enumerate(seeds):
            case = output_root / f"snr{snr}_seed{seed}"
            command = [
                str(executable), str(case), str(snr), str(timings[index]),
                "0:0:0,3:-4:45,9:-8:-80", "300", "20", str(seed),
            ]
            completed = subprocess.run(command, text=True, capture_output=True)
            if completed.returncode:
                sys.stderr.write(completed.stdout + completed.stderr)
                return completed.returncode
            metrics = read_metrics(case / "metrics.csv")
            metrics["case"] = case.name
            rows.append(metrics)
            print(
                f"{case.name}: pre/post BER={float(metrics['pre_ldpc_ber']):.4g}/"
                f"{float(metrics['post_ldpc_ber']):.4g}, FER={metrics['frame_error']}, "
                f"EVM={float(metrics['evm_percent']):.3f}%"
            )

    keys = ["case"] + sorted(key for key in rows[0] if key != "case")
    with (output_root / "frames.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=keys)
        writer.writeheader()
        writer.writerows(rows)

    grouped: dict[int, list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        grouped[int(float(row["snr_db"]))].append(row)
    summary_rows: list[dict[str, float | int]] = []
    for snr, group in sorted(grouped.items()):
        count = len(group)
        summary_rows.append({
            "snr_db": snr,
            "frames": count,
            "fer": sum(int(row["frame_error"]) for row in group) / count,
            "mean_pre_ldpc_ber": sum(float(row["pre_ldpc_ber"]) for row in group) / count,
            "mean_post_ldpc_ber": sum(float(row["post_ldpc_ber"]) for row in group) / count,
            "mean_evm_percent": sum(float(row["evm_percent"]) for row in group) / count,
            "timing_failures": sum(
                row["timing_offset_true"] != row["timing_offset_estimated"] for row in group
            ),
            "mean_abs_cfo_error_hz": sum(
                abs(float(row["cfo_hz_estimated"]) - float(row["cfo_hz_true"]))
                for row in group
            ) / count,
            "mean_abs_residual_sfo_ppm": sum(
                abs(float(row["sfo_ppm_residual"])) for row in group
            ) / count,
        })
    with (output_root / "summary.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(summary_rows[0]))
        writer.writeheader()
        writer.writerows(summary_rows)
    matplotlib_cache = output_root / "matplotlib-cache"
    matplotlib_cache.mkdir(exist_ok=True)
    os.environ.setdefault("MPLCONFIGDIR", str(matplotlib_cache))
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    snr_axis = [float(row["snr_db"]) for row in summary_rows]
    figure, axes = plt.subplots(1, 3, figsize=(14, 4.3), constrained_layout=True)
    axes[0].semilogy(
        snr_axis,
        [max(float(row["mean_pre_ldpc_ber"]), 1e-7) for row in summary_rows],
        "o-", label="Pre-LDPC BER",
    )
    axes[0].semilogy(
        snr_axis,
        [max(float(row["mean_post_ldpc_ber"]), 1e-7) for row in summary_rows],
        "s-", label="Post-LDPC BER",
    )
    axes[0].set_ylabel("BER")
    axes[0].legend()
    axes[1].plot(snr_axis, [float(row["fer"]) for row in summary_rows], "o-")
    axes[1].set_ylabel("Frame error rate")
    axes[1].set_ylim(-0.03, 1.03)
    axes[2].plot(
        snr_axis, [float(row["mean_evm_percent"]) for row in summary_rows], "o-"
    )
    axes[2].set_ylabel("EVM (%)")
    for axis in axes:
        axis.set_xlabel("SNR (dB)")
        axis.grid(True, ls=":", alpha=0.6)
    figure.suptitle("Windows C++ 2x2 OFDM regression: TDL + 300 Hz CFO + 20 ppm SFO")
    figure.savefig(output_root / "ber_fer_evm.png", dpi=160)
    plt.close(figure)
    print(f"Regression summary: {output_root / 'summary.csv'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
