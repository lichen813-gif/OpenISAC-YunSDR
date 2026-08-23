"""Plot diagnostics calculated by the Windows C++ PHY executable."""

from __future__ import annotations

import argparse
import csv
import math
import os
import tempfile
from pathlib import Path

os.environ.setdefault("MPLCONFIGDIR", str(Path(tempfile.gettempdir()) / "openisac-mpl"))
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


def read_rows(path: Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as stream:
        return list(csv.DictReader(stream))


def read_metrics(path: Path) -> dict[str, float | str]:
    metrics: dict[str, float | str] = {}
    for row in read_rows(path):
        try:
            metrics[row["metric"]] = float(row["value"])
        except ValueError:
            metrics[row["metric"]] = row["value"]
    return metrics


def plot_waveform(
    rows: list[dict[str, str]], metrics: dict[str, float | str], output: Path
) -> None:
    sample = [int(row["sample"]) for row in rows]
    fig, axes = plt.subplots(2, 1, figsize=(13, 7), sharex=True, constrained_layout=True)
    for axis, prefix, title in (
        (axes[0], "tx0", "C++ transmitted time waveform - Tx0"),
        (axes[1], "rx0", "C++ channel output time waveform - Rx0"),
    ):
        axis.plot(sample, [float(row[f"{prefix}_i"]) for row in rows], lw=0.75, label="I")
        axis.plot(sample, [float(row[f"{prefix}_q"]) for row in rows], lw=0.75, label="Q")
        timing = int(metrics["timing_offset_estimated"])
        symbol_samples = int(metrics["symbol_samples"])
        frame_symbols = int(metrics["frame_symbols"])
        for boundary in range(frame_symbols + 1):
            axis.axvline(
                timing + boundary * symbol_samples,
                color="0.35",
                ls="--",
                lw=0.8,
                label="Detected OFDM boundary" if boundary == 0 else None,
            )
        axis.axvspan(timing, timing + symbol_samples, color="gold", alpha=0.08)
        axis.set_ylabel("Normalized amplitude")
        axis.set_title(title)
        axis.grid(True, ls=":", alpha=0.55)
        axis.legend(loc="upper right")
    axes[1].set_xlabel("Receive-stream sample index (ZC preamble + two data symbols)")
    fig.savefig(output, dpi=150)
    plt.close(fig)


def plot_channel_and_sync(
    channel_rows: list[dict[str, str]],
    sync_rows: list[dict[str, str]],
    metrics: dict[str, float | str],
    output: Path,
) -> None:
    fig, axes = plt.subplots(2, 1, figsize=(12, 7), constrained_layout=True)
    candidates = [int(row["candidate"]) for row in sync_rows]
    values = [float(row["metric"]) for row in sync_rows]
    axes[0].plot(candidates, values, lw=1.3, label="Normalized ZC metric")
    axes[0].axvline(
        metrics["timing_offset_true"], color="tab:green", ls=":", label="True timing"
    )
    axes[0].axvline(
        metrics["timing_offset_estimated"],
        color="tab:red",
        ls="--",
        label="Estimated timing",
    )
    axes[0].set_xlabel("Timing candidate (samples)")
    axes[0].set_ylabel("Normalized metric")
    axes[0].set_title(
        f"ZC timing search - peak {metrics['timing_peak_metric']:.3f}"
    )
    axes[0].grid(True, ls=":", alpha=0.55)
    axes[0].legend(loc="upper right")

    fft = [int(row["fft"]) for row in channel_rows]
    for link in ("h00", "h01", "h10", "h11"):
        magnitude_db = [
            20.0
            * math.log10(
                max(math.hypot(float(row[f"{link}_i"]), float(row[f"{link}_q"])), 1e-12)
            )
            for row in channel_rows
        ]
        axes[1].plot(fft, magnitude_db, lw=0.9, label=link.upper())
    estimated_h00_db = [
        20.0
        * math.log10(
            max(math.hypot(float(row["e00_i"]), float(row["e00_q"])), 1e-12)
        )
        for row in channel_rows
    ]
    axes[1].plot(
        fft, estimated_h00_db, color="black", ls="--", lw=0.8, label="H00 LS estimate"
    )
    axes[1].set_xlabel("FFT subcarrier index")
    axes[1].set_ylabel("Channel magnitude (dB)")
    axes[1].set_title(
        f"2x2 TDL frequency response - {int(metrics['tdl_path_count'])} paths, "
        f"maximum delay {int(metrics['tdl_max_delay'])} samples | "
        f"LS NMSE {metrics['channel_nmse_db']:.1f} dB"
    )
    axes[1].grid(True, ls=":", alpha=0.55)
    axes[1].legend(ncol=4, loc="upper right")
    fig.savefig(output, dpi=150)
    plt.close(fig)


def plot_constellation(
    rows: list[dict[str, str]], metrics: dict[str, float | str], output: Path
) -> None:
    fig, axes = plt.subplots(1, 2, figsize=(12, 5.7), constrained_layout=True)
    for layer, axis in enumerate(axes):
        selected = [row for row in rows if int(row["layer"]) == layer]
        equalized_i = [float(row["equalized_i"]) for row in selected]
        equalized_q = [float(row["equalized_q"]) for row in selected]
        ideal = sorted(
            {(float(row["ideal_i"]), float(row["ideal_q"])) for row in selected}
        )
        axis.scatter(equalized_i, equalized_q, s=7, alpha=0.28, linewidths=0, label="MMSE")
        axis.scatter(
            [point[0] for point in ideal],
            [point[1] for point in ideal],
            s=42,
            marker="x",
            linewidths=1.2,
            label="Ideal 64-QAM",
        )
        axis.axhline(0.0, color="0.4", lw=0.7)
        axis.axvline(0.0, color="0.4", lw=0.7)
        axis.set_xlim(-1.35, 1.35)
        axis.set_ylim(-1.35, 1.35)
        axis.set_aspect("equal", adjustable="box")
        axis.set_xlabel("In-phase")
        axis.set_ylabel("Quadrature")
        axis.set_title(f"2x2 spatial multiplexing - layer {layer}")
        axis.grid(True, ls=":", alpha=0.55)
        axis.legend(loc="upper right")
    fig.suptitle(
        f"C++ MMSE with pilot LS CSI | SNR {metrics['snr_db']:.1f} dB | "
        f"BER {metrics['ber']:.3g} | EVM {metrics['evm_percent']:.2f}% | "
        f"post-LDPC BER {metrics['post_ldpc_ber']:.3g} | "
        f"CRC {'PASS' if metrics['payload_crc_ok'] else 'FAIL'}"
    )
    fig.savefig(output, dpi=150)
    plt.close(fig)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "input_dir", nargs="?", type=Path, default=Path("measurement/cpp_windows_plot")
    )
    args = parser.parse_args()
    input_dir = args.input_dir.resolve()
    waveform = read_rows(input_dir / "waveform.csv")
    constellation = read_rows(input_dir / "constellation.csv")
    channel = read_rows(input_dir / "channel.csv")
    synchronization = read_rows(input_dir / "synchronization.csv")
    metrics = read_metrics(input_dir / "metrics.csv")
    plot_waveform(waveform, metrics, input_dir / "time_waveform.png")
    plot_constellation(constellation, metrics, input_dir / "constellation.png")
    plot_channel_and_sync(
        channel, synchronization, metrics, input_dir / "channel_synchronization.png"
    )
    print(f"Plots written to {input_dir}")


if __name__ == "__main__":
    main()
