"""OpenISAC C++ video-PHY live monitor using only Tkinter and NumPy."""

from __future__ import annotations

import argparse
import csv
import math
import time
import tkinter as tk
from pathlib import Path

import numpy as np


COLORS = {
    "background": "#0b1220",
    "panel": "#111b2e",
    "grid": "#30405f",
    "text": "#dbeafe",
    "muted": "#8ea6c9",
    "cyan": "#22d3ee",
    "green": "#4ade80",
    "yellow": "#facc15",
    "red": "#fb7185",
    "purple": "#c084fc",
}


def read_rows(path: Path, allow_empty: bool = False) -> list[dict[str, str]]:
    last_error: Exception | None = None
    for _ in range(4):
        try:
            with path.open("r", encoding="utf-8", newline="") as stream:
                rows = list(csv.DictReader(stream))
            if rows or allow_empty:
                return rows
        except (OSError, csv.Error, ValueError) as error:
            last_error = error
        time.sleep(0.04)
    if last_error is not None:
        raise last_error
    raise RuntimeError(f"empty telemetry file: {path}")


def read_snapshot(directory: Path) -> dict[str, object]:
    status_rows = read_rows(directory / "status.csv")
    status: dict[str, str] = {row["metric"]: row["value"] for row in status_rows}
    waveform = read_rows(directory / "waveform.csv")
    constellation = read_rows(directory / "constellation.csv")
    channel = read_rows(directory / "channel.csv")
    sensing_range_doppler: list[dict[str, str]] = []
    sensing_detections: list[dict[str, str]] = []
    if status.get("sensing_ready") == "1":
        sensing_range_doppler = read_rows(
            directory / "sensing_range_doppler.csv")
        sensing_detections = read_rows(
            directory / "sensing_detections.csv", allow_empty=True)
    return {
        "status": status,
        "waveform": waveform,
        "constellation": constellation,
        "channel": channel,
        "sensing_range_doppler": sensing_range_doppler,
        "sensing_detections": sensing_detections,
    }


def number(status: dict[str, str], name: str, default: float = 0.0) -> float:
    try:
        return float(status[name])
    except (KeyError, ValueError):
        return default


class Plot(tk.Canvas):
    def __init__(self, parent: tk.Misc, title: str) -> None:
        super().__init__(
            parent,
            background=COLORS["panel"],
            highlightthickness=1,
            highlightbackground="#243553",
        )
        self.title = title

    def axes(self, x_label: str = "", y_label: str = "") -> tuple[float, float, float, float]:
        self.delete("all")
        width = max(self.winfo_width(), 320)
        height = max(self.winfo_height(), 220)
        left, top, right, bottom = 54.0, 34.0, width - 18.0, height - 36.0
        self.create_text(
            12, 10, text=self.title, anchor="nw", fill=COLORS["text"],
            font=("Microsoft YaHei UI", 11, "bold")
        )
        for step in range(6):
            x = left + (right - left) * step / 5.0
            y = top + (bottom - top) * step / 5.0
            self.create_line(x, top, x, bottom, fill=COLORS["grid"], dash=(2, 5))
            self.create_line(left, y, right, y, fill=COLORS["grid"], dash=(2, 5))
        self.create_rectangle(left, top, right, bottom, outline="#526784")
        if x_label:
            self.create_text((left + right) / 2, height - 8, text=x_label,
                             fill=COLORS["muted"], font=("Microsoft YaHei UI", 8))
        if y_label:
            self.create_text(8, (top + bottom) / 2, text=y_label, anchor="w",
                             fill=COLORS["muted"], font=("Microsoft YaHei UI", 8))
        return left, top, right, bottom

    def line_plot(
        self,
        series: list[tuple[np.ndarray, np.ndarray, str, str]],
        x_label: str,
        y_label: str,
        y_limits: tuple[float, float] | None = None,
    ) -> None:
        left, top, right, bottom = self.axes(x_label, y_label)
        valid = [(x, y, color, label) for x, y, color, label in series if len(x) and len(y)]
        if not valid:
            return
        x_min = min(float(np.nanmin(x)) for x, _, _, _ in valid)
        x_max = max(float(np.nanmax(x)) for x, _, _, _ in valid)
        if y_limits is None:
            y_min = min(float(np.nanmin(y)) for _, y, _, _ in valid)
            y_max = max(float(np.nanmax(y)) for _, y, _, _ in valid)
            margin = max((y_max - y_min) * 0.08, 1e-6)
            y_min, y_max = y_min - margin, y_max + margin
        else:
            y_min, y_max = y_limits
        x_span = max(x_max - x_min, 1e-12)
        y_span = max(y_max - y_min, 1e-12)
        for x_values, y_values, color, label in valid:
            stride = max(1, len(x_values) // 1300)
            points: list[float] = []
            for x_value, y_value in zip(x_values[::stride], y_values[::stride]):
                points.extend([
                    left + (float(x_value) - x_min) / x_span * (right - left),
                    bottom - (float(y_value) - y_min) / y_span * (bottom - top),
                ])
            if len(points) >= 4:
                self.create_line(*points, fill=color, width=1.25)
        self.create_text(left, bottom + 4, text=f"{x_min:.3g}", anchor="nw",
                         fill=COLORS["muted"], font=("Consolas", 8))
        self.create_text(right, bottom + 4, text=f"{x_max:.3g}", anchor="ne",
                         fill=COLORS["muted"], font=("Consolas", 8))
        self.create_text(left - 4, top, text=f"{y_max:.3g}", anchor="ne",
                         fill=COLORS["muted"], font=("Consolas", 8))
        self.create_text(left - 4, bottom, text=f"{y_min:.3g}", anchor="se",
                         fill=COLORS["muted"], font=("Consolas", 8))
        legend_x = right - 8
        for index, (_, _, color, label) in enumerate(reversed(valid)):
            self.create_text(legend_x, top + 12 + 16 * index, text=f"— {label}", anchor="ne",
                             fill=color, font=("Microsoft YaHei UI", 8, "bold"))

    def constellation(
        self,
        ideal_i: np.ndarray,
        ideal_q: np.ndarray,
        equalized_i: np.ndarray,
        equalized_q: np.ndarray,
    ) -> None:
        left, top, right, bottom = self.axes("同相 I", "正交 Q")
        limit = max(
            1.15,
            float(np.max(np.abs(equalized_i))) * 1.05 if len(equalized_i) else 1.15,
            float(np.max(np.abs(equalized_q))) * 1.05 if len(equalized_q) else 1.15,
        )
        def point(x: float, y: float) -> tuple[float, float]:
            return (
                left + (x + limit) / (2.0 * limit) * (right - left),
                bottom - (y + limit) / (2.0 * limit) * (bottom - top),
            )
        zero_x, zero_y = point(0.0, 0.0)
        self.create_line(zero_x, top, zero_x, bottom, fill="#617391")
        self.create_line(left, zero_y, right, zero_y, fill="#617391")
        stride = max(1, len(equalized_i) // 1600)
        for x, y in zip(equalized_i[::stride], equalized_q[::stride]):
            px, py = point(float(x), float(y))
            self.create_oval(px - 1.5, py - 1.5, px + 1.5, py + 1.5,
                             fill=COLORS["cyan"], outline="")
        ideals = sorted(set(zip(np.round(ideal_i, 6), np.round(ideal_q, 6))))
        for x, y in ideals:
            px, py = point(float(x), float(y))
            self.create_line(px - 4, py - 4, px + 4, py + 4, fill=COLORS["yellow"], width=2)
            self.create_line(px - 4, py + 4, px + 4, py - 4, fill=COLORS["yellow"], width=2)

    def range_doppler(
        self,
        rows: list[dict[str, str]],
        detections: list[dict[str, str]],
    ) -> None:
        left, top, right, bottom = self.axes("距离 m", "速度 m/s")
        if not rows:
            self.create_text(
                (left + right) / 2, (top + bottom) / 2,
                text="等待相干积累…", fill=COLORS["muted"])
            return
        ranges = sorted({float(row["range_m"]) for row in rows})
        velocities = sorted({float(row["velocity_mps"]) for row in rows})
        range_index = {value: index for index, value in enumerate(ranges)}
        velocity_index = {value: index for index, value in enumerate(velocities)}
        power = np.full((len(velocities), len(ranges)), -80.0, dtype=float)
        for row in rows:
            power[velocity_index[float(row["velocity_mps"])]][
                range_index[float(row["range_m"])]] = float(row["relative_power_db"])
        range_step = max(1, int(math.ceil(len(ranges) / 96)))
        velocity_step = max(1, int(math.ceil(len(velocities) / 64)))
        sampled_ranges = ranges[::range_step]
        sampled_velocities = velocities[::velocity_step]
        sampled_power = power[::velocity_step, ::range_step]

        def heat_color(value_db: float) -> str:
            value = max(0.0, min(1.0, (value_db + 55.0) / 55.0))
            anchors = (
                (11, 18, 32), (28, 61, 115), (6, 182, 212),
                (250, 204, 21), (251, 113, 133),
            )
            scaled = value * (len(anchors) - 1)
            index = min(int(scaled), len(anchors) - 2)
            fraction = scaled - index
            rgb = tuple(int(anchors[index][component] * (1.0 - fraction) +
                            anchors[index + 1][component] * fraction)
                        for component in range(3))
            return f"#{rgb[0]:02x}{rgb[1]:02x}{rgb[2]:02x}"

        columns = len(sampled_ranges)
        lines = len(sampled_velocities)
        cell_width = (right - left) / max(columns, 1)
        cell_height = (bottom - top) / max(lines, 1)
        for velocity in range(lines):
            for distance in range(columns):
                x0 = left + distance * cell_width
                y0 = bottom - (velocity + 1) * cell_height
                self.create_rectangle(
                    x0, y0, x0 + cell_width + 0.5, y0 + cell_height + 0.5,
                    fill=heat_color(float(sampled_power[velocity, distance])),
                    outline="")
        range_min, range_max = sampled_ranges[0], sampled_ranges[-1]
        velocity_min, velocity_max = sampled_velocities[0], sampled_velocities[-1]
        for detection in detections:
            distance = float(detection["range_m"])
            velocity = float(detection["velocity_mps"])
            if range_min <= distance <= range_max and velocity_min <= velocity <= velocity_max:
                x = left + (distance - range_min) / max(range_max - range_min, 1e-9) * (right - left)
                y = bottom - (velocity - velocity_min) / max(
                    velocity_max - velocity_min, 1e-9) * (bottom - top)
                self.create_oval(x - 5, y - 5, x + 5, y + 5,
                                 outline="#ffffff", width=2)
        self.create_rectangle(left, top, right, bottom, outline="#8ea6c9")
        self.create_text(left, bottom + 4, text=f"{range_min:.1f}", anchor="nw",
                         fill=COLORS["muted"], font=("Consolas", 8))
        self.create_text(right, bottom + 4, text=f"{range_max:.1f}", anchor="ne",
                         fill=COLORS["muted"], font=("Consolas", 8))
        self.create_text(left - 4, top, text=f"{velocity_max:.1f}", anchor="ne",
                         fill=COLORS["muted"], font=("Consolas", 8))
        self.create_text(left - 4, bottom, text=f"{velocity_min:.1f}", anchor="se",
                         fill=COLORS["muted"], font=("Consolas", 8))
        self.create_text(right - 5, top + 8, text="CFAR ○", anchor="ne",
                         fill="#ffffff", font=("Microsoft YaHei UI", 8, "bold"))


class LiveMonitor:
    def __init__(self, directory: Path, refresh_seconds: float) -> None:
        self.directory = directory
        self.refresh_ms = max(200, int(refresh_seconds * 1000.0))
        self.root = tk.Tk()
        self.root.title("OpenISAC 实时物理层监视器")
        self.root.geometry("1720x920")
        self.root.minsize(1280, 720)
        self.root.configure(background=COLORS["background"])
        self.root.attributes("-topmost", True)
        self.root.after(1800, lambda: self.root.attributes("-topmost", False))

        self.header = tk.Label(
            self.root, text="等待 C++ 物理层遥测数据…", anchor="w",
            background=COLORS["background"], foreground=COLORS["text"],
            font=("Microsoft YaHei UI", 12, "bold"), padx=14, pady=9,
        )
        self.header.pack(fill="x")
        body = tk.Frame(self.root, background=COLORS["background"])
        body.pack(fill="both", expand=True, padx=10, pady=(0, 10))
        for row in range(2):
            body.rowconfigure(row, weight=1)
        for column in range(4):
            body.columnconfigure(column, weight=1)
        self.layers = [Plot(body, f"星座图 · Layer {layer}") for layer in range(4)]
        self.waveform = Plot(body, "接收时域波形")
        self.channel = Plot(body, "MIMO 信道估计幅频响应")
        self.sensing = Plot(body, "动态感知 · 距离–速度图 / CFAR")
        self.metrics = tk.Label(
            body, justify="left", anchor="nw", background=COLORS["panel"],
            foreground=COLORS["text"], font=("Consolas", 10), padx=18, pady=14,
            relief="solid", borderwidth=1,
        )
        widgets = [(plot, 0, layer) for layer, plot in enumerate(self.layers)]
        widgets += [
            (self.waveform, 1, 0), (self.channel, 1, 1),
            (self.sensing, 1, 2), (self.metrics, 1, 3),
        ]
        for widget, row, column in widgets:
            widget.grid(row=row, column=column, sticky="nsew", padx=4, pady=4)
        self.last_snapshot = ""

    def refresh(self) -> None:
        try:
            snapshot = read_snapshot(self.directory)
            status = snapshot["status"]
            assert isinstance(status, dict)
            snapshot_id = status.get("snapshot_epoch_ms", "")
            if snapshot_id != self.last_snapshot:
                self.draw(snapshot)
                self.last_snapshot = str(snapshot_id)
        except Exception as error:
            self.header.configure(text=f"等待遥测文件：{error}", foreground=COLORS["yellow"])
        self.root.after(self.refresh_ms, self.refresh)

    def draw(self, snapshot: dict[str, object]) -> None:
        status = snapshot["status"]
        waveform_rows = snapshot["waveform"]
        constellation_rows = snapshot["constellation"]
        channel_rows = snapshot["channel"]
        sensing_rows = snapshot["sensing_range_doppler"]
        sensing_detections = snapshot["sensing_detections"]
        assert isinstance(status, dict)
        assert isinstance(waveform_rows, list)
        assert isinstance(constellation_rows, list)
        assert isinstance(channel_rows, list)
        assert isinstance(sensing_rows, list)
        assert isinstance(sensing_detections, list)

        age = max(0.0, time.time() - number(status, "snapshot_epoch_ms") / 1000.0)
        rank = int(number(status, "rank", 1.0))
        condition_text = (
            f"条件数 {number(status, 'channel_condition_median'):.2f}/"
            f"{number(status, 'channel_condition_p90'):.2f}"
        )
        self.header.configure(
            text=(
                f"OpenISAC  Rank-{status.get('rank', '?')} / {status.get('modulation', '?')}   "
                f"FFT/CP {status.get('fft_size', '?')}/{status.get('cp_length', '?')}   "
                f"SNR {number(status, 'snr_db'):.1f} dB   "
                f"{condition_text}   "
                f"帧 {int(number(status, 'frame_id')):,}   刷新延迟 {age:.1f} s"
            ),
            foreground=COLORS["text"],
        )

        for layer, plot in enumerate(self.layers):
            selected = [row for row in constellation_rows if int(row["layer"]) == layer]
            if not selected:
                plot.axes("同相 I", "正交 Q")
                plot.create_text(
                    170, 110, text=f"Rank-{rank} 模式无 Layer {layer}",
                    fill=COLORS["muted"])
                continue
            plot.constellation(
                np.asarray([float(row["ideal_i"]) for row in selected]),
                np.asarray([float(row["ideal_q"]) for row in selected]),
                np.asarray([float(row["equalized_i"]) for row in selected]),
                np.asarray([float(row["equalized_q"]) for row in selected]),
            )

        samples = np.asarray([int(row["sample"]) for row in waveform_rows])
        rx_i = np.asarray([float(row["rx0_i"]) for row in waveform_rows])
        rx_q = np.asarray([float(row["rx0_q"]) for row in waveform_rows])
        self.waveform.line_plot(
            [(samples, rx_i, COLORS["cyan"], "Rx0 I"),
             (samples, rx_q, COLORS["purple"], "Rx0 Q")],
            "采样点", "幅度",
        )

        fft = np.asarray([int(row["fft"]) for row in channel_rows])
        order = np.fft.fftshift(np.arange(len(fft)))
        centered = np.arange(-len(fft) // 2, len(fft) // 2)
        channel_series = []
        links = (
            (("h00", COLORS["cyan"]), ("h11", COLORS["yellow"]),
             ("h22", COLORS["green"]), ("h33", COLORS["purple"]))
            if rank == 4 else
            (("h00", COLORS["cyan"]), ("h01", COLORS["yellow"]),
             ("h10", COLORS["green"]), ("h11", COLORS["purple"]))
        )
        self.channel.title = f"{rank}×{rank} 信道估计幅频响应"
        for link, color in links:
            values = np.asarray([
                complex(float(row[f"{link}_i"]), float(row[f"{link}_q"]))
                for row in channel_rows
            ])
            magnitude = 20.0 * np.log10(np.maximum(np.abs(values[order]), 1e-9))
            channel_series.append((centered, magnitude, color, link.upper()))
        self.channel.line_plot(channel_series, "子载波索引", "幅度 dB")

        self.sensing.range_doppler(sensing_rows, sensing_detections)
        range_spacing = number(status, "sensing_range_spacing_m")
        packets_in = int(number(status, "udp_packets_in"))
        packets_out = int(number(status, "udp_packets_out"))
        mimo_metrics = (
            f"条件数 中位/P90     {number(status, 'channel_condition_median'):7.2f} / "
            f"{number(status, 'channel_condition_p90'):7.2f}\n"
            f"病态子载波 >10      {number(status, 'ill_conditioned_subcarrier_percent'):9.2f} %\n"
        )
        if rank == 4:
            mimo_metrics += (
                f"感知功率合并链路    {int(number(status, 'sensing_link_count')):9d}\n"
            )
        metrics_text = (
            "实时测量\n\n"
            f"导频/帧             {status.get('pilot_mode', 'fdm'):>9s} / "
            f"{int(number(status, 'frame_symbols')):d} 符号, "
            f"{number(status, 'frame_period_us'):.0f} us\n"
            f"EVM                 {number(status, 'evm_percent'):9.3f} %\n"
            f"信道估计 NMSE       {number(status, 'channel_nmse_db'):9.3f} dB\n"
            f"空间相关 Tx/Rx      {number(status, 'tx_spatial_correlation'):7.3f} / "
            f"{number(status, 'rx_spatial_correlation'):7.3f}\n"
            f"{mimo_metrics}"
            f"频偏 真值/估计      {number(status, 'cfo_true_hz'):7.2f} / "
            f"{number(status, 'cfo_estimated_hz'):7.2f} Hz\n"
            f"频偏估计误差        {number(status, 'cfo_error_hz'):9.3f} Hz\n"
            f"SFO 真值/残余       {number(status, 'sfo_true_ppm'):7.3f} / "
            f"{number(status, 'sfo_residual_ppm'):7.3f} ppm\n"
            f"定时 真值/估计      {number(status, 'timing_true_samples'):7.0f} / "
            f"{number(status, 'timing_estimated_samples'):7.0f} sample\n"
            f"同步峰值            {number(status, 'timing_metric'):9.4f}\n"
            f"噪声方差            {number(status, 'noise_variance'):9.3e}\n\n"
            f"星座 有效/排除填充  {int(number(status, 'constellation_valid_symbols')):5d} / "
            f"{int(number(status, 'constellation_padding_symbols_excluded')):5d}\n\n"
            "链路统计\n\n"
            f"Socket 接收          {int(number(status, 'ingress_socket_packets')):,}\n"
            f"入口队列丢包        {int(number(status, 'ingress_queue_drops')):,}\n"
            f"队列 当前/峰值      {int(number(status, 'ingress_queue_depth')):,} / "
            f"{int(number(status, 'ingress_queue_high_watermark')):,}\n"
            f"UDP 输入/输出       {packets_in:,} / {packets_out:,}\n"
            f"UDP 丢弃            {int(number(status, 'udp_dropped')):,}\n"
            f"PHY 帧               {int(number(status, 'phy_frames')):,}\n"
            f"FER                  {number(status, 'fer_percent'):9.4f} %\n\n"
            "动态感知\n\n"
            f"相干积累帧数      {int(number(status, 'sensing_coherent_frames')):9d}\n"
            f"距离分辨率          {range_spacing:9.3f} m\n"
            f"速度分辨率          {number(status, 'sensing_velocity_spacing_mps'):9.3f} m/s\n"
            f"最强目标距离        {number(status, 'sensing_peak_range_m'):9.3f} m\n"
            f"最强目标速度        {number(status, 'sensing_peak_velocity_mps'):9.3f} m/s\n"
            f"CFAR 目标数         {int(number(status, 'sensing_detection_count')):9d}"
        )
        self.metrics.configure(text=metrics_text)

    def run(self) -> None:
        self.root.after(150, self.refresh)
        self.root.mainloop()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("telemetry_dir", nargs="?", type=Path,
                        default=Path("measurement/live_phy_monitor"))
    parser.add_argument("--refresh", type=float, default=1.0,
                        help="GUI polling interval in seconds")
    parser.add_argument("--check", action="store_true",
                        help="read one snapshot and exit without opening a window")
    args = parser.parse_args()
    directory = args.telemetry_dir.resolve()
    if args.check:
        snapshot = read_snapshot(directory)
        status = snapshot["status"]
        assert isinstance(status, dict)
        print(
            f"OK: Rank-{status['rank']} {status['modulation']}, "
            f"EVM={number(status, 'evm_percent'):.3f}%, "
            f"CFO estimate={number(status, 'cfo_estimated_hz'):.3f} Hz"
        )
        return
    LiveMonitor(directory, args.refresh).run()


if __name__ == "__main__":
    main()
