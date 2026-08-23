"""Small Windows/Linux GUI for manual modulation and constellation display."""

from __future__ import annotations

import os
import threading
from pathlib import Path
from typing import Any

# The bundled Windows Python runtime keeps Tcl/Tk data outside a standard venv.
# The environment installer copies those scripts into .venv/tcl so the GUI can
# locate them without modifying the system Python installation.
_PROJECT_ROOT = Path(__file__).resolve().parents[1]
_LOCAL_TCL = _PROJECT_ROOT / ".venv" / "tcl" / "tcl8.6"
_LOCAL_TK = _PROJECT_ROOT / ".venv" / "tcl" / "tk8.6"
if _LOCAL_TCL.exists():
    os.environ.setdefault("TCL_LIBRARY", str(_LOCAL_TCL))
if _LOCAL_TK.exists():
    os.environ.setdefault("TK_LIBRARY", str(_LOCAL_TK))

from tkinter import BooleanVar, StringVar, Tk, filedialog, messagebox, ttk

from .config import DEFAULT_TDL_TAPS, SimulationConfig, parse_tap_string
from .plotting import draw_channel_response, draw_constellation, draw_time_waveform
from .simulation import SimulationArtifacts, simulate_alamouti_ofdm


MODULATIONS = {
    "QPSK": "qpsk",
    "16-QAM": "16qam",
    "64-QAM": "64qam",
    "256-QAM": "256qam",
}


class ConstellationApp:
    def __init__(self, root: Tk) -> None:
        self.root = root
        self.root.title("OpenISAC MIMO/STBC PHY Viewer")
        self.root.minsize(1000, 860)
        self.latest: tuple[dict[str, Any], SimulationArtifacts, SimulationConfig] | None = None

        self.modulation = StringVar(value="64-QAM")
        self.nt = StringVar(value="2")
        self.nr = StringVar(value="2")
        self.mode = StringVar(value="stbc")
        self.detector = StringVar(value="mmse")
        self.pairing = StringVar(value="time")
        self.channel = StringVar(value="rayleigh")
        self.snr_db = StringVar(value="25")
        self.frames = StringVar(value="300")
        self.fft_size = StringVar(value="1024")
        self.cp_length = StringVar(value="128")
        self.subcarrier_spacing_khz = StringVar(value="15")
        self.guard_left = StringVar(value="0")
        self.guard_right = StringVar(value="0")
        self.dc_null = BooleanVar(value=True)
        self.pilot_spacing = StringVar(value="8")
        self.pilot_offset = StringVar(value="0")
        self.channel_estimation = StringVar(value="perfect")
        self.channel_estimation_taps = StringVar(value="10")
        self.preamble_enable = BooleanVar(value=True)
        self.zc_root = StringVar(value="29")
        self.synchronization_enable = BooleanVar(value=True)
        self.pilot_phase_tracking_enable = BooleanVar(value=True)
        self.pilot_phase_min_coherence = StringVar(value="0.9")
        self.phase_reference_tracking_enable = BooleanVar(value=False)
        self.phase_reference_count = StringVar(value="2")
        self.sfo_tracking_enable = BooleanVar(value=False)
        self.sfo_resampling_enable = BooleanVar(value=False)
        self.sfo_resampling_interpolator = StringVar(value="sinc8")
        self.timing_offset_samples = StringVar(value="5")
        self.timing_search_samples = StringVar(value="16")
        self.cfo_hz = StringVar(value="500")
        self.sfo_ppm = StringVar(value="0")
        self.slot_phase_offset_deg = StringVar(value="0")
        self.waveform_channel = StringVar(value="Tx0")
        self.waveform_component = StringVar(value="i/q")
        self.waveform_max_samples = StringVar(value="0")
        self.channel_link = StringVar(value="Tx0→Rx0")
        self.doppler_hz = StringVar(value="0")
        self.doppler_model = StringVar(value="symbol")
        self.tx_correlation = StringVar(value="0")
        self.rx_correlation = StringVar(value="0")
        self.spatial_rank = StringVar(value="0")
        self.taps = StringVar(value="0:0:0, 3:-4:45, 9:-8:-80")
        self.status = StringVar(value="Set parameters and click Run simulation")

        controls = ttk.Frame(root, padding=10)
        controls.pack(side="top", fill="x")
        self._combo(controls, "Modulation", self.modulation, tuple(MODULATIONS), 0)
        nr_combo = self._combo(controls, "Rx antennas", self.nr, ("1", "2", "4", "8"), 1)
        nr_combo.bind("<<ComboboxSelected>>", self._update_waveform_choices)
        self._combo(
            controls,
            "Channel",
            self.channel,
            ("awgn", "static", "rayleigh", "tdl"),
            2,
        )
        self._entry(controls, "Es/N0 (dB)", self.snr_db, 3)
        self._entry(controls, "Frames", self.frames, 4)

        self.run_button = ttk.Button(controls, text="Run simulation", command=self.run_simulation)
        self.run_button.grid(row=0, column=5, padx=8, pady=(18, 0), sticky="ew")
        self.save_button = ttk.Button(
            controls, text="Save PNG", command=self.save_png, state="disabled"
        )
        self.save_button.grid(row=0, column=6, padx=4, pady=(18, 0), sticky="ew")
        for column in range(7):
            controls.columnconfigure(column, weight=1)

        mimo_controls = ttk.Frame(root, padding=(10, 0, 10, 5))
        mimo_controls.pack(side="top", fill="x")
        mode_combo = self._combo(
            mimo_controls,
            "MIMO mode",
            self.mode,
            ("stbc", "sfbc", "spatial_multiplexing"),
            0,
        )
        mode_combo.bind("<<ComboboxSelected>>", self._mode_changed)
        self._combo(
            mimo_controls,
            "SM detector",
            self.detector,
            ("zf", "mmse"),
            1,
        )
        nt_combo = self._combo(
            mimo_controls,
            "Tx antennas/layers",
            self.nt,
            ("2", "4", "8"),
            2,
        )
        nt_combo.bind("<<ComboboxSelected>>", self._update_waveform_choices)
        ttk.Label(
            mimo_controls,
            text="Spatial multiplexing: N layers, estimated/perfect CSI, normalized total power",
        ).grid(row=0, column=3, padx=8, pady=(18, 0), sticky="w")
        for column in range(4):
            mimo_controls.columnconfigure(column, weight=1)

        ofdm_controls = ttk.Frame(root, padding=(10, 0, 10, 5))
        ofdm_controls.pack(side="top", fill="x")
        self._entry(ofdm_controls, "FFT", self.fft_size, 0)
        self._entry(ofdm_controls, "CP samples", self.cp_length, 1)
        self._entry(ofdm_controls, "Spacing (kHz)", self.subcarrier_spacing_khz, 2)
        self._entry(ofdm_controls, "Guard left", self.guard_left, 3)
        self._entry(ofdm_controls, "Guard right", self.guard_right, 4)
        self._entry(ofdm_controls, "Pilot spacing", self.pilot_spacing, 5)
        self._entry(ofdm_controls, "Pilot offset", self.pilot_offset, 6)
        ttk.Checkbutton(ofdm_controls, text="Null DC", variable=self.dc_null).grid(
            row=0, column=7, padx=8, pady=(18, 0), sticky="w"
        )
        pairing_combo = self._combo(
            ofdm_controls,
            "Alamouti pairing",
            self.pairing,
            ("time", "frequency"),
            8,
        )
        pairing_combo.bind("<<ComboboxSelected>>", self._pairing_changed)
        for column in range(9):
            ofdm_controls.columnconfigure(column, weight=1)

        preamble_controls = ttk.Frame(root, padding=(10, 0, 10, 5))
        preamble_controls.pack(side="top", fill="x")
        ttk.Checkbutton(
            preamble_controls,
            text="Enable ZC frame preamble",
            variable=self.preamble_enable,
        ).grid(row=0, column=0, padx=8, pady=(18, 0), sticky="w")
        self._entry(preamble_controls, "ZC root", self.zc_root, 1)
        ttk.Label(
            preamble_controls,
            text="OpenISAC-compatible Tx0 ZC matched-filter frame detection",
        ).grid(row=0, column=2, padx=8, pady=(18, 0), sticky="w")
        for column in range(3):
            preamble_controls.columnconfigure(column, weight=1)

        sync_controls = ttk.Frame(root, padding=(10, 0, 10, 5))
        sync_controls.pack(side="top", fill="x")
        ttk.Checkbutton(
            sync_controls,
            text="Enable timing/CFO synchronization",
            variable=self.synchronization_enable,
        ).grid(row=0, column=0, padx=8, pady=(18, 0), sticky="w")
        self._entry(sync_controls, "Timing offset (samples)", self.timing_offset_samples, 1)
        self._entry(sync_controls, "Timing search (samples)", self.timing_search_samples, 2)
        self._entry(sync_controls, "CFO (Hz)", self.cfo_hz, 3)
        ttk.Label(
            sync_controls,
            text="CP: coarse timing/CFO",
        ).grid(row=0, column=4, padx=8, pady=(18, 0), sticky="w")
        ttk.Checkbutton(
            sync_controls,
            text="Pilot CPE tracking",
            variable=self.pilot_phase_tracking_enable,
        ).grid(row=0, column=5, padx=8, pady=(18, 0), sticky="w")
        self._entry(sync_controls, "CPE min coherence", self.pilot_phase_min_coherence, 6)
        estimator_combo = self._combo(
            sync_controls,
            "Channel estimate",
            self.channel_estimation,
            ("perfect", "ls_linear", "ls_dft", "lmmse"),
            7,
        )
        estimator_combo.bind("<<ComboboxSelected>>", self._channel_estimator_changed)
        self._entry(sync_controls, "CSI taps", self.channel_estimation_taps, 8)
        for column in range(9):
            sync_controls.columnconfigure(column, weight=1)

        phase_controls = ttk.Frame(root, padding=(10, 0, 10, 5))
        phase_controls.pack(side="top", fill="x")
        ttk.Checkbutton(
            phase_controls,
            text="Independent phase references",
            variable=self.phase_reference_tracking_enable,
            command=self._phase_reference_changed,
        ).grid(row=0, column=0, padx=8, pady=(18, 0), sticky="w")
        self._entry(phase_controls, "Reference tones", self.phase_reference_count, 1)
        self._entry(phase_controls, "Slot phase jump (deg)", self.slot_phase_offset_deg, 2)
        ttk.Checkbutton(
            phase_controls,
            text="SFO phase-slope tracking",
            variable=self.sfo_tracking_enable,
            command=self._sfo_tracking_changed,
        ).grid(row=0, column=3, padx=8, pady=(18, 0), sticky="w")
        self._entry(phase_controls, "SFO (ppm)", self.sfo_ppm, 4)
        ttk.Checkbutton(
            phase_controls,
            text="Closed-loop SFO resampling",
            variable=self.sfo_resampling_enable,
            command=self._sfo_resampling_changed,
        ).grid(row=0, column=5, padx=8, pady=(18, 0), sticky="w")
        self._combo(
            phase_controls,
            "SFO interpolator",
            self.sfo_resampling_interpolator,
            ("sinc8", "cubic", "sinc24"),
            6,
        )
        for column in range(7):
            phase_controls.columnconfigure(column, weight=1)

        waveform_controls = ttk.Frame(root, padding=(10, 0, 10, 5))
        waveform_controls.pack(side="top", fill="x")
        ttk.Label(waveform_controls, text="Time waveform channel").pack(side="left", padx=(4, 3))
        self.waveform_combo = ttk.Combobox(
            waveform_controls,
            textvariable=self.waveform_channel,
            values=("Tx0", "Tx1", "Rx0", "Rx1"),
            state="readonly",
            width=8,
        )
        self.waveform_combo.pack(side="left", padx=(0, 12))
        self.waveform_combo.bind("<<ComboboxSelected>>", self._redraw_waveform)
        ttk.Label(waveform_controls, text="Component").pack(side="left", padx=(4, 3))
        component_combo = ttk.Combobox(
            waveform_controls,
            textvariable=self.waveform_component,
            values=("i/q", "real", "imag", "magnitude"),
            state="readonly",
            width=10,
        )
        component_combo.pack(side="left")
        component_combo.bind("<<ComboboxSelected>>", self._redraw_waveform)
        ttk.Label(waveform_controls, text="Display samples (0=all)").pack(
            side="left", padx=(16, 3)
        )
        waveform_samples_entry = ttk.Entry(
            waveform_controls, textvariable=self.waveform_max_samples, width=8
        )
        waveform_samples_entry.pack(side="left")
        waveform_samples_entry.bind("<Return>", self._redraw_waveform)
        waveform_samples_entry.bind("<FocusOut>", self._redraw_waveform)
        ttk.Label(waveform_controls, text="Channel link").pack(side="left", padx=(16, 3))
        self.link_combo = ttk.Combobox(
            waveform_controls,
            textvariable=self.channel_link,
            values=("Tx0→Rx0", "Tx1→Rx0", "Tx0→Rx1", "Tx1→Rx1"),
            state="readonly",
            width=10,
        )
        self.link_combo.pack(side="left")
        self.link_combo.bind("<<ComboboxSelected>>", self._redraw_channel_response)

        tap_controls = ttk.Frame(root, padding=(10, 0, 10, 5))
        tap_controls.pack(side="top", fill="x")
        ttk.Label(tap_controls, text="TDL taps (delay:gain dB:phase deg)").pack(
            side="left", padx=(4, 5)
        )
        ttk.Entry(tap_controls, textvariable=self.taps).pack(side="left", fill="x", expand=True)
        ttk.Label(tap_controls, text="Maximum Doppler (Hz)").pack(
            side="left", padx=(16, 5)
        )
        ttk.Entry(tap_controls, textvariable=self.doppler_hz, width=10).pack(
            side="left"
        )
        ttk.Label(tap_controls, text="Doppler model").pack(
            side="left", padx=(10, 4)
        )
        ttk.Combobox(
            tap_controls,
            textvariable=self.doppler_model,
            values=("symbol", "continuous"),
            state="readonly",
            width=10,
        ).pack(side="left")
        ttk.Label(tap_controls, text="Tx/Rx correlation").pack(
            side="left", padx=(16, 5)
        )
        ttk.Entry(tap_controls, textvariable=self.tx_correlation, width=6).pack(
            side="left"
        )
        ttk.Entry(tap_controls, textvariable=self.rx_correlation, width=6).pack(
            side="left", padx=(3, 0)
        )
        ttk.Label(tap_controls, text="Spatial rank (0=auto)").pack(
            side="left", padx=(16, 5)
        )
        ttk.Entry(tap_controls, textvariable=self.spatial_rank, width=6).pack(
            side="left"
        )

        ttk.Label(root, textvariable=self.status, padding=(12, 2)).pack(side="bottom", fill="x")

        config_dir = Path(__file__).resolve().parents[1] / ".matplotlib"
        config_dir.mkdir(parents=True, exist_ok=True)
        os.environ.setdefault("MPLCONFIGDIR", str(config_dir))
        from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg, NavigationToolbar2Tk
        from matplotlib.figure import Figure

        self.figure = Figure(figsize=(8.2, 9.0), dpi=100, constrained_layout=True)
        grid_spec = self.figure.add_gridspec(3, 1, height_ratios=(1.55, 0.8, 0.7))
        self.constellation_axes = self.figure.add_subplot(grid_spec[0])
        self.waveform_axes = self.figure.add_subplot(grid_spec[1])
        self.channel_axes = self.figure.add_subplot(grid_spec[2])
        self.constellation_axes.set_title("Constellation will appear here")
        self.constellation_axes.set_xlabel("In-phase (normalized)")
        self.constellation_axes.set_ylabel("Quadrature (normalized)")
        self.constellation_axes.grid(True, linestyle=":", alpha=0.5)
        self.waveform_axes.set_title("Time-domain waveform will appear here")
        self.waveform_axes.set_xlabel("Time sample index")
        self.waveform_axes.set_ylabel("Normalized amplitude")
        self.waveform_axes.grid(True, linestyle=":", alpha=0.5)
        self.channel_axes.set_title("Channel frequency response will appear here")
        self.channel_axes.set_xlabel("Subcarrier index (FFT-shifted)")
        self.channel_axes.set_ylabel("Magnitude (dB)")
        self.channel_axes.grid(True, linestyle=":", alpha=0.5)
        self.canvas = FigureCanvasTkAgg(self.figure, master=root)
        self.canvas.get_tk_widget().pack(side="top", fill="both", expand=True)
        toolbar = NavigationToolbar2Tk(self.canvas, root, pack_toolbar=False)
        toolbar.update()
        toolbar.pack(side="bottom", fill="x")

    @staticmethod
    def _combo(
        parent: ttk.Frame,
        label: str,
        variable: StringVar,
        values: tuple[str, ...],
        column: int,
    ) -> ttk.Combobox:
        ttk.Label(parent, text=label).grid(row=0, column=column, padx=4, sticky="n")
        combo = ttk.Combobox(
            parent, textvariable=variable, values=values, state="readonly", width=11
        )
        combo.grid(row=0, column=column, padx=4, pady=(18, 0), sticky="ew")
        return combo

    @staticmethod
    def _entry(parent: ttk.Frame, label: str, variable: StringVar, column: int) -> None:
        ttk.Label(parent, text=label).grid(row=0, column=column, padx=4, sticky="n")
        ttk.Entry(parent, textvariable=variable, width=10).grid(
            row=0, column=column, padx=4, pady=(18, 0), sticky="ew"
        )

    def _config(self) -> SimulationConfig:
        waveform_max_samples = int(self.waveform_max_samples.get())
        if waveform_max_samples < 0:
            raise ValueError(
                "Display samples must be 0 (all) or a positive integer"
            )
        cfg = SimulationConfig(
            modulation=MODULATIONS[self.modulation.get()],
            fft_size=int(self.fft_size.get()),
            cp_length=int(self.cp_length.get()),
            subcarrier_spacing_hz=float(self.subcarrier_spacing_khz.get()) * 1000.0,
            guard_left=int(self.guard_left.get()),
            guard_right=int(self.guard_right.get()),
            dc_null=bool(self.dc_null.get()),
            pilot_spacing=int(self.pilot_spacing.get()),
            pilot_offset=int(self.pilot_offset.get()),
            preamble_enable=bool(self.preamble_enable.get()),
            zc_root=int(self.zc_root.get()),
            synchronization_enable=bool(self.synchronization_enable.get()),
            pilot_phase_tracking_enable=bool(self.pilot_phase_tracking_enable.get()),
            pilot_phase_min_coherence=float(self.pilot_phase_min_coherence.get()),
            phase_reference_tracking_enable=bool(
                self.phase_reference_tracking_enable.get()
            ),
            phase_reference_count=int(self.phase_reference_count.get()),
            sfo_tracking_enable=bool(self.sfo_tracking_enable.get()),
            sfo_resampling_enable=bool(self.sfo_resampling_enable.get()),
            sfo_resampling_interpolator=self.sfo_resampling_interpolator.get(),
            timing_offset_samples=int(self.timing_offset_samples.get()),
            timing_search_samples=int(self.timing_search_samples.get()),
            cfo_hz=float(self.cfo_hz.get()),
            sfo_ppm=float(self.sfo_ppm.get()),
            slot_phase_offset_deg=float(self.slot_phase_offset_deg.get()),
            channel_estimation=self.channel_estimation.get(),
            channel_estimation_taps=int(self.channel_estimation_taps.get()),
            frames=int(self.frames.get()),
            nt=int(self.nt.get()),
            nr=int(self.nr.get()),
            mode=self.mode.get(),
            layers=int(self.nt.get()) if self.mode.get() == "spatial_multiplexing" else 1,
            detector=self.detector.get(),
            pairing=self.pairing.get(),
            channel=self.channel.get(),
            doppler_hz=float(self.doppler_hz.get()),
            doppler_model=self.doppler_model.get(),
            tx_correlation=float(self.tx_correlation.get()),
            rx_correlation=float(self.rx_correlation.get()),
            spatial_rank=int(self.spatial_rank.get()),
            channel_taps=(
                parse_tap_string(self.taps.get())
                if self.channel.get() == "tdl"
                else DEFAULT_TDL_TAPS
            ),
            snr_db=float(self.snr_db.get()),
            seed=23063,
        )
        cfg.validate()
        return cfg

    def _update_waveform_choices(self, _event: object | None = None) -> None:
        if self.mode.get() == "spatial_multiplexing" and int(self.nr.get()) < int(self.nt.get()):
            self.nr.set(self.nt.get())
        choices = [f"Tx{index}" for index in range(int(self.nt.get()))] + [
            f"Rx{index}" for index in range(int(self.nr.get()))
        ]
        links = [
            f"Tx{tx}→Rx{rx}"
            for rx in range(int(self.nr.get()))
            for tx in range(int(self.nt.get()))
        ]
        self.waveform_combo.configure(values=choices)
        self.link_combo.configure(values=links)
        if self.waveform_channel.get() not in choices:
            self.waveform_channel.set("Rx0")
        if self.channel_link.get() not in links:
            self.channel_link.set("Tx0→Rx0")
        if (
            self.latest is not None
            and self.latest[2].nr == int(self.nr.get())
            and self.latest[2].nt == int(self.nt.get())
        ):
            self._redraw_waveform()
            self._redraw_channel_response()
        elif self.latest is not None:
            self.status.set("Rx antenna setting changed; click Run simulation to update plots")

    def _redraw_waveform(self, _event: object | None = None) -> None:
        if self.latest is None:
            return
        _result, artifacts, _cfg = self.latest
        try:
            max_samples = int(self.waveform_max_samples.get())
            if max_samples < 0:
                raise ValueError
        except ValueError:
            self.status.set("Display samples must be 0 (all) or a positive integer")
            return
        draw_time_waveform(
            self.waveform_axes,
            artifacts.tx_time,
            artifacts.rx_time,
            signal=self.waveform_channel.get(),
            component=self.waveform_component.get(),
            max_samples=max_samples,
            preamble_symbols=1 if _cfg.preamble_enable else 0,
        )
        self.canvas.draw_idle()

    def _channel_estimator_changed(self, _event: object | None = None) -> None:
        if self.channel_estimation.get() != "perfect":
            self.pilot_phase_tracking_enable.set(False)
            self.status.set(
                "LS selected: perfect-CSI pilot CPE tracking disabled; click Run simulation"
            )

    def _pairing_changed(self, _event: object | None = None) -> None:
        if self.pairing.get() == "frequency":
            self.mode.set("sfbc")
            if int(self.pilot_spacing.get()) <= 0:
                self.pilot_spacing.set("4")
            if self.channel_estimation.get() == "perfect":
                self.channel_estimation.set("lmmse")
            self.pilot_phase_tracking_enable.set(False)
            self.phase_reference_tracking_enable.set(False)
            self.sfo_tracking_enable.set(False)
            self.sfo_resampling_enable.set(False)
            self.status.set(
                "SFBC selected: per-symbol FDM pilots + estimated CSI"
            )
        else:
            if self.mode.get() == "sfbc":
                self.mode.set("stbc")
            self.status.set("Time-paired Alamouti STBC selected")

    def _mode_changed(self, _event: object | None = None) -> None:
        if self.mode.get() == "stbc":
            self.nt.set("2")
            self.pairing.set("time")
            self.status.set("Time-paired Alamouti STBC selected")
        elif self.mode.get() == "sfbc":
            self.nt.set("2")
            self.pairing.set("frequency")
            self._pairing_changed()
        else:
            self.pairing.set("time")
            if int(self.nr.get()) < int(self.nt.get()):
                self.nr.set(self.nt.get())
                self._update_waveform_choices()
            self.pilot_spacing.set("4")
            self.channel_estimation.set("lmmse")
            self.pilot_phase_tracking_enable.set(False)
            self.phase_reference_tracking_enable.set(False)
            self.sfo_tracking_enable.set(False)
            self.sfo_resampling_enable.set(False)
            self.status.set(
                "2-layer spatial multiplexing: FDM pilots + LMMSE CSI selected"
            )

    def _phase_reference_changed(self) -> None:
        if self.phase_reference_tracking_enable.get():
            self.pilot_phase_tracking_enable.set(False)
            self.status.set(
                "Independent phase references enabled; perfect-CSI CPE tracking disabled"
            )
        else:
            self.sfo_tracking_enable.set(False)
            self.sfo_resampling_enable.set(False)

    def _sfo_tracking_changed(self) -> None:
        if self.sfo_tracking_enable.get():
            self.phase_reference_tracking_enable.set(True)
            self.pilot_phase_tracking_enable.set(False)
            if int(self.phase_reference_count.get()) < 4:
                self.phase_reference_count.set("8")
            self.status.set(
                "SFO slope tracking enabled; 8 distributed references recommended"
            )
        else:
            self.sfo_resampling_enable.set(False)

    def _sfo_resampling_changed(self) -> None:
        if self.sfo_resampling_enable.get():
            self.sfo_tracking_enable.set(True)
            self.phase_reference_tracking_enable.set(True)
            self.preamble_enable.set(True)
            self.synchronization_enable.set(True)
            if int(self.phase_reference_count.get()) < 4:
                self.phase_reference_count.set("8")
            self.status.set("Two-pass estimated-SFO resampling enabled")

    def _redraw_channel_response(self, _event: object | None = None) -> None:
        if self.latest is None:
            return
        _result, artifacts, cfg = self.latest
        pilot_indices = artifacts.pilot_indices
        if (
            (cfg.mode == "spatial_multiplexing" or cfg.pairing == "frequency")
            and artifacts.pilot_tx_assignments.size
        ):
            tx = int(self.channel_link.get()[2])
            pilot_indices = pilot_indices[artifacts.pilot_tx_assignments == tx]
        draw_channel_response(
            self.channel_axes,
            artifacts.receiver_channel,
            link=self.channel_link.get(),
            pilot_indices=pilot_indices,
            phase_reference_indices=artifacts.phase_reference_indices,
            reference_channel_grid=(
                artifacts.channel if cfg.channel_estimation != "perfect" else None
            ),
            channel_estimation_label=cfg.channel_estimation,
        )
        self.canvas.draw_idle()

    def run_simulation(self) -> None:
        try:
            cfg = self._config()
        except (KeyError, ValueError) as error:
            messagebox.showerror("Invalid parameter", str(error))
            return
        self.run_button.configure(state="disabled")
        self.save_button.configure(state="disabled")
        self.status.set("Simulation running...")
        threading.Thread(target=self._worker, args=(cfg,), daemon=True).start()

    def _worker(self, cfg: SimulationConfig) -> None:
        try:
            result, artifacts = simulate_alamouti_ofdm(cfg, return_artifacts=True)
        except Exception as error:  # GUI boundary: surface numerical/config errors.
            self.root.after(0, self._simulation_failed, error)
            return
        self.root.after(0, self._simulation_complete, result, artifacts, cfg)

    def _simulation_failed(self, error: Exception) -> None:
        self.run_button.configure(state="normal")
        self.status.set("Simulation failed")
        messagebox.showerror("Simulation failed", str(error))

    def _simulation_complete(
        self,
        result: dict[str, Any],
        artifacts: SimulationArtifacts,
        cfg: SimulationConfig,
    ) -> None:
        modulation_label = next(
            label for label, value in MODULATIONS.items() if value == cfg.modulation
        )
        coding_name = (
            f"{cfg.layers}-layer SM/{cfg.detector.upper()}"
            if cfg.mode == "spatial_multiplexing"
            else "STBC"
            if cfg.pairing == "time"
            else "SFBC"
        )
        title = (
            f"{cfg.nt}x{cfg.nr} {coding_name} · {modulation_label} · "
            f"{cfg.channel} · N={cfg.fft_size}, CP={cfg.cp_length} · "
            f"Es/N0={cfg.snr_db:g} dB"
        )
        draw_constellation(
            self.constellation_axes,
            artifacts.equalized_symbols,
            cfg.bits_per_symbol,
            title=title,
            seed=cfg.seed,
        )
        draw_time_waveform(
            self.waveform_axes,
            artifacts.tx_time,
            artifacts.rx_time,
            signal=self.waveform_channel.get(),
            component=self.waveform_component.get(),
            max_samples=int(self.waveform_max_samples.get()),
            preamble_symbols=result["preamble_symbols"],
        )
        draw_channel_response(
            self.channel_axes,
            artifacts.receiver_channel,
            link=self.channel_link.get(),
            pilot_indices=(
                artifacts.pilot_indices[
                    artifacts.pilot_tx_assignments
                    == int(self.channel_link.get()[2])
                ]
                if (cfg.mode == "spatial_multiplexing" or cfg.pairing == "frequency")
                and artifacts.pilot_tx_assignments.size
                else artifacts.pilot_indices
            ),
            phase_reference_indices=artifacts.phase_reference_indices,
            reference_channel_grid=(
                artifacts.channel if cfg.channel_estimation != "perfect" else None
            ),
            channel_estimation_label=cfg.channel_estimation,
        )
        self.canvas.draw_idle()
        self.latest = (result, artifacts, cfg)
        self.status.set(
            f"BER={result['ber']:.6g}   BLER={result['bler']:.6g}   "
            f"CRC={result['crc_failure_rate']:.6g}   EVM={result['evm_rms']:.6g}   "
            f"pilots={result['pilot_subcarriers']}   "
            f"preamble={result['preamble_symbols']} ZC(root={cfg.zc_root})   "
            f"timing={result['estimated_timing_offset_mean']:.2f}/"
            f"{result['true_timing_offset_samples']} samples   "
            f"CFO={result['estimated_cfo_hz_mean']:.1f}/"
            f"{result['true_cfo_hz']:.1f} Hz   "
            f"SFO={result['estimated_sfo_ppm_mean']:.2f}/"
            f"{result['true_sfo_ppm']:.2f} ppm   "
            f"residual={result['mean_absolute_residual_sfo_ppm']:.2f} ppm   "
            f"pilot-CPE={result['mean_pilot_phase_coherence']:.3f}   "
            f"apply={result['pilot_phase_application_rate']:.1%}   "
            f"phase={result['estimated_differential_phase_deg_mean']:.1f}/"
            f"{result['true_slot_phase_offset_deg']:.1f} deg   "
            f"ref-coh={result['mean_phase_reference_coherence']:.3f}   "
            f"Doppler={result['maximum_doppler_hz']:.1f} Hz   "
            f"model={result['doppler_model']}   "
            f"intra-var={result['intrasymbol_channel_variation_nmse']:.3g}   "
            f"pair-var={result['alamouti_pair_channel_variation_nmse']:.3g}   "
            f"rate={result['net_payload_rate_bps'] / 1e6:.2f} Mb/s   "
            f"goodput={result['goodput_bps'] / 1e6:.2f} Mb/s   "
            f"NMSE={result['channel_estimation_nmse']:.3g}   "
            f"Fs={cfg.sample_rate_hz / 1e6:.3f} MHz   CSI={cfg.channel_estimation}"
        )
        self.run_button.configure(state="normal")
        self.save_button.configure(state="normal")

    def save_png(self) -> None:
        if self.latest is None:
            return
        _result, _artifacts, cfg = self.latest
        mode_name = (
            f"sm_{cfg.detector}"
            if cfg.mode == "spatial_multiplexing"
            else "stbc"
            if cfg.pairing == "time"
            else "sfbc"
        )
        initial = (
            f"{mode_name}_2x{cfg.nr}_"
            f"{cfg.modulation}_{cfg.channel}_{cfg.snr_db:g}db.png"
        )
        path = filedialog.asksaveasfilename(
            title="Save PHY plots",
            defaultextension=".png",
            initialfile=initial,
            filetypes=(("PNG image", "*.png"),),
        )
        if path:
            self.figure.savefig(path, dpi=180)
            self.status.set(f"Saved: {path}")


def main() -> None:
    root = Tk()
    ConstellationApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
