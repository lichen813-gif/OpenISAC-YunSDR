from __future__ import annotations

import argparse
import os
from pathlib import Path

os.environ.setdefault("MPLBACKEND", "Agg")

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Plot the C++ current-PHY range-Doppler result"
    )
    parser.add_argument("input_dir", type=Path)
    parser.add_argument("--max-range-m", type=float, default=300.0)
    args = parser.parse_args()

    map_path = args.input_dir / "range_doppler.csv"
    detections_path = args.input_dir / "detections.csv"
    raw = np.genfromtxt(map_path, delimiter=",", names=True, dtype=np.float64)
    doppler_bins = int(np.max(raw["doppler_bin"])) + 1
    range_bins = int(np.max(raw["range_bin"])) + 1
    relative_db = raw["relative_power_db"].reshape(doppler_bins, range_bins)
    range_axis = raw["range_m"].reshape(doppler_bins, range_bins)[0]
    velocity_axis = raw["velocity_mps"].reshape(doppler_bins, range_bins)[:, 0]
    view_bins = max(1, int(np.searchsorted(range_axis, args.max_range_m, side="right")))

    detections = np.genfromtxt(
        detections_path, delimiter=",", names=True, dtype=np.float64
    )
    if detections.size == 0:
        detections = np.empty(0, dtype=detections.dtype)
    detections = np.atleast_1d(detections)

    fig, axes = plt.subplots(2, 2, figsize=(13, 8), constrained_layout=True)
    image = axes[0, 0].imshow(
        relative_db[:, :view_bins],
        origin="lower",
        aspect="auto",
        extent=(range_axis[0], range_axis[view_bins - 1], velocity_axis[0], velocity_axis[-1]),
        vmin=-60.0,
        vmax=0.0,
        cmap="turbo",
    )
    visible = detections["range_m"] <= args.max_range_m
    axes[0, 0].scatter(
        detections["range_m"][visible],
        detections["velocity_mps"][visible],
        facecolors="none",
        edgecolors="white",
        s=70,
        linewidths=1.3,
        label="CA-CFAR",
    )
    axes[0, 0].set_title("C++ current-PHY range-Doppler map")
    axes[0, 0].set_xlabel("Range (m)")
    axes[0, 0].set_ylabel("Velocity (m/s)")
    axes[0, 0].legend(loc="upper right")
    fig.colorbar(image, ax=axes[0, 0], label="Relative power (dB)")

    axes[0, 1].plot(range_axis[:view_bins], np.max(relative_db[:, :view_bins], axis=0))
    axes[0, 1].set_title("Maximum range profile")
    axes[0, 1].set_xlabel("Range (m)")
    axes[0, 1].set_ylabel("Relative power (dB)")
    axes[0, 1].grid(True, alpha=0.3)

    axes[1, 0].plot(velocity_axis, np.max(relative_db[:, :view_bins], axis=1))
    axes[1, 0].set_title("Maximum Doppler profile")
    axes[1, 0].set_xlabel("Velocity (m/s)")
    axes[1, 0].set_ylabel("Relative power (dB)")
    axes[1, 0].grid(True, alpha=0.3)

    axes[1, 1].axis("off")
    lines = ["Detected targets (static clutter suppression enabled)"]
    for target in detections[:12]:
        lines.append(
            f"R={target['range_m']:.2f} m, v={target['velocity_mps']:.3f} m/s, "
            f"margin={target['margin_db']:.1f} dB"
        )
    axes[1, 1].text(0.02, 0.98, "\n".join(lines), va="top", family="monospace")

    output = args.input_dir / "range_doppler.png"
    fig.savefig(output, dpi=160)
    plt.close(fig)
    print(output.resolve())


if __name__ == "__main__":
    main()
