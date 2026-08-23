"""Constellation plotting for simulation artifacts."""

from __future__ import annotations

import os
from pathlib import Path

import numpy as np

from .qam import modulate


def _sample_points(
    equalized_symbols: np.ndarray, max_points: int, seed: int
) -> np.ndarray:
    if max_points <= 0:
        raise ValueError("max_points must be positive")
    samples = np.asarray(equalized_symbols, dtype=np.complex128).reshape(-1)
    if samples.size > max_points:
        rng = np.random.default_rng(seed)
        samples = samples[rng.choice(samples.size, size=max_points, replace=False)]
    return samples


def draw_constellation(
    ax: object,
    equalized_symbols: np.ndarray,
    bits_per_symbol: int,
    *,
    title: str,
    max_points: int = 20000,
    seed: int = 0xC057,
) -> None:
    """Draw a constellation on an existing Matplotlib axes."""

    samples = _sample_points(equalized_symbols, max_points, seed)
    references = modulate(np.arange(1 << bits_per_symbol), bits_per_symbol)
    ax.clear()
    ax.scatter(
        samples.real,
        samples.imag,
        s=7,
        alpha=0.22,
        marker=".",
        color="#1677b8",
        linewidths=0,
        label=f"Equalized samples ({samples.size})",
        rasterized=True,
    )
    ax.scatter(
        references.real,
        references.imag,
        s=70,
        marker="x",
        color="#d1495b",
        linewidths=1.5,
        label=f"Ideal {1 << bits_per_symbol}-QAM references",
    )
    extent = max(
        1.0,
        float(np.quantile(np.abs(np.concatenate((samples.real, samples.imag))), 0.997)) * 1.12,
        float(np.max(np.abs(np.concatenate((references.real, references.imag))))) * 1.18,
    )
    ax.set_xlim(-extent, extent)
    ax.set_ylim(-extent, extent)
    ax.set_aspect("equal", adjustable="box")
    ax.axhline(0.0, color="#666666", linewidth=0.7, alpha=0.55)
    ax.axvline(0.0, color="#666666", linewidth=0.7, alpha=0.55)
    ax.grid(True, linestyle=":", linewidth=0.7, alpha=0.55)
    ax.set_xlabel("In-phase (normalized)")
    ax.set_ylabel("Quadrature (normalized)")
    ax.set_title(title)
    ax.legend(loc="upper right")


def draw_time_waveform(
    ax: object,
    tx_time: np.ndarray,
    rx_time: np.ndarray,
    *,
    signal: str = "Tx0",
    component: str = "i/q",
    max_samples: int | None = 0,
    preamble_symbols: int = 0,
) -> None:
    """Draw one transmitted or received channel from the first frame."""

    tx_time = np.asarray(tx_time, dtype=np.complex128)
    rx_time = np.asarray(rx_time, dtype=np.complex128)
    if tx_time.ndim != 4 or rx_time.ndim != 4:
        raise ValueError("time arrays must have shape [batch, ofdm_symbol, antenna, sample]")
    if max_samples is not None and max_samples < 0:
        raise ValueError("max_samples must be zero/all or positive")
    if signal.startswith("Tx"):
        index = int(signal[2:])
        if not 0 <= index < tx_time.shape[2]:
            raise ValueError(f"invalid transmit channel: {signal}")
        samples = tx_time[0, :, index, :].reshape(-1)
    elif signal.startswith("Rx"):
        index = int(signal[2:])
        if not 0 <= index < rx_time.shape[2]:
            raise ValueError(f"invalid receive channel: {signal}")
        samples = rx_time[0, :, index, :].reshape(-1)
    else:
        raise ValueError(f"invalid waveform signal: {signal}")
    if max_samples not in (None, 0):
        samples = samples[:max_samples]
    sample_index = np.arange(samples.size)
    component = component.lower()

    ax.clear()
    if component == "real":
        ax.plot(sample_index, samples.real, linewidth=1.0, color="#1677b8", label="Real (I)")
    elif component == "imag":
        ax.plot(sample_index, samples.imag, linewidth=1.0, color="#d1495b", label="Imag (Q)")
    elif component == "magnitude":
        ax.plot(sample_index, np.abs(samples), linewidth=1.0, color="#398c5a", label="Magnitude")
    elif component == "i/q":
        ax.plot(sample_index, samples.real, linewidth=1.0, color="#1677b8", label="Real (I)")
        ax.plot(sample_index, samples.imag, linewidth=1.0, color="#d1495b", label="Imag (Q)")
    else:
        raise ValueError(f"invalid waveform component: {component}")

    samples_per_symbol = tx_time.shape[-1]
    for boundary in range(samples_per_symbol, samples.size, samples_per_symbol):
        ax.axvline(boundary, color="#666666", linewidth=0.8, linestyle="--", alpha=0.65)
    if preamble_symbols > 0:
        preamble_stop = min(samples.size, preamble_symbols * samples_per_symbol)
        ax.axvspan(0, preamble_stop, color="#f2c14e", alpha=0.11)
        if preamble_stop > 0:
            ax.text(
                preamble_stop / 2,
                0.96,
                "ZC preamble",
                transform=ax.get_xaxis_transform(),
                ha="center",
                va="top",
                color="#8a6500",
                fontsize=9,
            )
    ax.set_xlim(0, max(1, samples.size - 1))
    ax.set_xlabel("Time sample index")
    ax.set_ylabel("Normalized amplitude")
    ax.set_title(
        f"Time-domain waveform · {signal} · {component.upper()} · "
        f"{samples.size} samples"
    )
    ax.grid(True, linestyle=":", linewidth=0.7, alpha=0.55)
    ax.legend(loc="upper right")


def draw_channel_response(
    ax: object,
    channel_grid: np.ndarray,
    *,
    link: str = "Tx0→Rx0",
    pilot_indices: np.ndarray | None = None,
    phase_reference_indices: np.ndarray | None = None,
    reference_channel_grid: np.ndarray | None = None,
    channel_estimation_label: str = "estimated",
) -> None:
    """Draw an estimated channel and, when supplied, its perfect reference."""

    channel_grid = np.asarray(channel_grid, dtype=np.complex128)
    if channel_grid.ndim != 4:
        raise ValueError("channel_grid must have shape [batch, subcarrier, rx, tx]")
    if reference_channel_grid is not None:
        reference_channel_grid = np.asarray(reference_channel_grid, dtype=np.complex128)
        if reference_channel_grid.shape != channel_grid.shape:
            raise ValueError("reference channel must have the same shape as channel_grid")
    normalized = link.replace("→", "-").replace("->", "-")
    try:
        tx_text, rx_text = normalized.split("-", maxsplit=1)
        tx = int(tx_text.strip()[2:])
        rx = int(rx_text.strip()[2:])
    except (ValueError, IndexError) as error:
        raise ValueError(f"invalid MIMO link: {link}") from error
    if not 0 <= tx < channel_grid.shape[3] or not 0 <= rx < channel_grid.shape[2]:
        raise ValueError(f"MIMO link outside channel dimensions: {link}")

    response = np.fft.fftshift(channel_grid[0, :, rx, tx])
    magnitude_db = 20.0 * np.log10(np.maximum(np.abs(response), 1.0e-12))
    fft_size = response.size
    subcarriers = np.arange(fft_size) - fft_size // 2
    ax.clear()
    channel_label = (
        f"{channel_estimation_label} estimate"
        if reference_channel_grid is not None
        else "Channel"
    )
    ax.plot(subcarriers, magnitude_db, linewidth=1.3, color="#7c4d9e", label=channel_label)
    plotted_magnitudes = [magnitude_db]
    if reference_channel_grid is not None:
        reference_response = np.fft.fftshift(reference_channel_grid[0, :, rx, tx])
        reference_db = 20.0 * np.log10(
            np.maximum(np.abs(reference_response), 1.0e-12)
        )
        plotted_magnitudes.append(reference_db)
        ax.plot(
            subcarriers,
            reference_db,
            linewidth=1.0,
            linestyle="--",
            color="#4f697c",
            alpha=0.9,
            label="Perfect reference",
        )
    if pilot_indices is not None:
        pilot_indices = np.asarray(pilot_indices, dtype=np.int64)
        if pilot_indices.size:
            pilot_centered = ((pilot_indices + fft_size // 2) % fft_size) - fft_size // 2
            pilot_magnitude = 20.0 * np.log10(
                np.maximum(np.abs(channel_grid[0, pilot_indices, rx, tx]), 1.0e-12)
            )
            ax.scatter(
                pilot_centered,
                pilot_magnitude,
                s=28,
                marker="o",
                facecolors="none",
                edgecolors="#d4771e",
                linewidths=1.2,
                label="Pilot subcarriers",
                zorder=3,
            )
    if phase_reference_indices is not None:
        phase_reference_indices = np.asarray(phase_reference_indices, dtype=np.int64)
        if phase_reference_indices.size:
            reference_centered = (
                (phase_reference_indices + fft_size // 2) % fft_size
            ) - fft_size // 2
            reference_magnitude = 20.0 * np.log10(
                np.maximum(
                    np.abs(channel_grid[0, phase_reference_indices, rx, tx]),
                    1.0e-12,
                )
            )
            ax.scatter(
                reference_centered,
                reference_magnitude,
                s=34,
                marker="s",
                facecolors="none",
                edgecolors="#238b68",
                linewidths=1.3,
                label="Phase references",
                zorder=3,
            )
    ax.set_xlim(int(subcarriers[0]), int(subcarriers[-1]))
    all_magnitudes = np.concatenate(plotted_magnitudes)
    padding = max(1.0, float(np.ptp(all_magnitudes)) * 0.08)
    ax.set_ylim(
        float(np.min(all_magnitudes) - padding),
        float(np.max(all_magnitudes) + padding),
    )
    ax.set_xlabel("Subcarrier index (FFT-shifted)")
    ax.set_ylabel("Magnitude (dB)")
    estimate_label = (
        f"{channel_estimation_label} vs perfect"
        if reference_channel_grid is not None
        else "Perfect-CSI"
    )
    ax.set_title(f"{estimate_label} channel frequency response · {link}")
    ax.grid(True, linestyle=":", linewidth=0.7, alpha=0.55)
    ax.legend(loc="upper right")


def plot_phy_overview(
    equalized_symbols: np.ndarray,
    bits_per_symbol: int,
    tx_time: np.ndarray,
    rx_time: np.ndarray,
    channel_grid: np.ndarray,
    *,
    title: str,
    signal: str = "Tx0",
    component: str = "i/q",
    channel_link: str = "Tx0→Rx0",
    pilot_indices: np.ndarray | None = None,
    phase_reference_indices: np.ndarray | None = None,
    reference_channel_grid: np.ndarray | None = None,
    channel_estimation_label: str = "estimated",
    output: Path | None = None,
    show: bool = False,
    max_points: int = 20000,
    max_samples: int | None = 0,
    preamble_symbols: int = 0,
    seed: int = 0xC057,
) -> Path | None:
    """Plot constellation and time-domain waveform in a stacked overview."""

    config_dir = Path(__file__).resolve().parents[1] / ".matplotlib"
    config_dir.mkdir(parents=True, exist_ok=True)
    os.environ.setdefault("MPLCONFIGDIR", str(config_dir))
    import matplotlib

    if not show:
        matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, (constellation_ax, waveform_ax, channel_ax) = plt.subplots(
        3,
        1,
        figsize=(8.4, 12.0),
        gridspec_kw={"height_ratios": (1.55, 0.8, 0.7)},
        constrained_layout=True,
    )
    draw_constellation(
        constellation_ax,
        equalized_symbols,
        bits_per_symbol,
        title=title,
        max_points=max_points,
        seed=seed,
    )
    draw_time_waveform(
        waveform_ax,
        tx_time,
        rx_time,
        signal=signal,
        component=component,
        max_samples=max_samples,
        preamble_symbols=preamble_symbols,
    )
    draw_channel_response(
        channel_ax,
        channel_grid,
        link=channel_link,
        pilot_indices=pilot_indices,
        phase_reference_indices=phase_reference_indices,
        reference_channel_grid=reference_channel_grid,
        channel_estimation_label=channel_estimation_label,
    )
    saved: Path | None = None
    if output is not None:
        saved = Path(output).resolve()
        saved.parent.mkdir(parents=True, exist_ok=True)
        fig.savefig(saved, dpi=180)
    if show:
        plt.show()
    else:
        plt.close(fig)
    return saved


def plot_constellation(
    equalized_symbols: np.ndarray,
    bits_per_symbol: int,
    *,
    title: str,
    output: Path | None = None,
    show: bool = False,
    max_points: int = 20000,
    seed: int = 0xC057,
) -> Path | None:
    """Plot equalized samples and ideal QAM references.

    ``show=True`` opens an interactive Matplotlib window. ``output`` saves a
    PNG suitable for reports and regression evidence. Point subsampling is
    deterministic so repeated runs produce the same picture.
    """

    config_dir = Path(__file__).resolve().parents[1] / ".matplotlib"
    config_dir.mkdir(parents=True, exist_ok=True)
    os.environ.setdefault("MPLCONFIGDIR", str(config_dir))
    import matplotlib

    if not show:
        matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots(figsize=(7.2, 7.2), constrained_layout=True)
    draw_constellation(
        ax,
        equalized_symbols,
        bits_per_symbol,
        title=title,
        max_points=max_points,
        seed=seed,
    )

    saved: Path | None = None
    if output is not None:
        saved = Path(output).resolve()
        saved.parent.mkdir(parents=True, exist_ok=True)
        fig.savefig(saved, dpi=180)
    if show:
        plt.show()
    else:
        plt.close(fig)
    return saved
