from __future__ import annotations

import argparse
import csv
import os
import tempfile
from pathlib import Path

_cache = Path(tempfile.gettempdir()) / "openisac-matplotlib"
_cache.mkdir(parents=True, exist_ok=True)
os.environ.setdefault("MPLCONFIGDIR", str(_cache))

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402


def read_metrics(path: Path) -> dict[str, float]:
    with path.open("r", encoding="utf-8", newline="") as handle:
        rows = csv.DictReader(handle)
        return {row["metric"]: float(row["value"]) for row in rows}


def read_points(path: Path) -> list[list[tuple[float, float, float, float]]]:
    layers: list[list[tuple[float, float, float, float]]] = [[], [], [], []]
    with path.open("r", encoding="utf-8", newline="") as handle:
        for row in csv.DictReader(handle):
            layer = int(row["layer"])
            if 0 <= layer < len(layers):
                layers[layer].append(
                    (
                        float(row["tx_i"]),
                        float(row["tx_q"]),
                        float(row["rx_i"]),
                        float(row["rx_q"]),
                    )
                )
    return layers


def main() -> None:
    parser = argparse.ArgumentParser(description="Plot OpenISAC Rank-4 diagnostics")
    parser.add_argument(
        "directory", nargs="?", default="measurement/cpp_4x4_diagnostics"
    )
    args = parser.parse_args()
    directory = Path(args.directory)
    metrics = read_metrics(directory / "summary.csv")
    layers = read_points(directory / "constellation.csv")

    figure, axes = plt.subplots(2, 2, figsize=(10, 9), constrained_layout=True)
    for layer, axis in enumerate(axes.flat):
        points = layers[layer]
        stride = max(1, len(points) // 3000)
        selected = points[::stride]
        axis.scatter(
            [point[2] for point in selected],
            [point[3] for point in selected],
            s=4,
            alpha=0.38,
            color="#00a6d6",
            edgecolors="none",
            label="MMSE",
        )
        ideal = sorted({(point[0], point[1]) for point in points})
        axis.scatter(
            [point[0] for point in ideal],
            [point[1] for point in ideal],
            s=26,
            marker="x",
            linewidths=1.2,
            color="#f0a202",
            label="Ideal",
        )
        axis.set_title(f"Rank-4 Layer {layer}")
        axis.set_xlabel("I")
        axis.set_ylabel("Q")
        axis.grid(True, alpha=0.25)
        axis.set_aspect("equal", adjustable="box")
        axis.legend(loc="upper right")

    figure.suptitle(
        "OpenISAC 4x4 MIMO OFDM | "
        f"EVM {metrics['evm_percent']:.2f}% | "
        f"BER {metrics.get('pre_fec_ber', metrics.get('ber', 0.0)):.4g} | "
        f"CSI NMSE {metrics['channel_nmse_db']:.2f} dB | "
        f"SNR {metrics['snr_db']:.1f} dB",
        fontsize=13,
    )
    output = directory / "rank4_constellation.png"
    figure.savefig(output, dpi=170)
    plt.close(figure)
    print(f"Saved {output.resolve()}")


if __name__ == "__main__":
    main()
