"""Validated configuration objects for reproducible PHY simulations."""

from __future__ import annotations

import math
from dataclasses import asdict, dataclass
from typing import Any, Mapping


MODULATION_BITS = {
    "qpsk": 2,
    "16qam": 4,
    "64qam": 6,
    "256qam": 8,
}


@dataclass(frozen=True)
class ChannelTap:
    """One deterministic TDL path relative to each MIMO link."""

    delay_samples: int
    gain_db: float
    phase_deg: float = 0.0


DEFAULT_TDL_TAPS = (
    ChannelTap(0, 0.0, 0.0),
    ChannelTap(3, -4.0, 45.0),
    ChannelTap(9, -8.0, -80.0),
)


def parse_tap_string(text: str) -> tuple[ChannelTap, ...]:
    """Parse ``delay:gain_db:phase_deg`` entries separated by comma/semicolon."""

    entries = [item.strip() for item in text.replace(";", ",").split(",") if item.strip()]
    if not entries:
        raise ValueError("at least one TDL tap is required")
    taps = []
    for entry in entries:
        fields = [item.strip() for item in entry.split(":")]
        if len(fields) not in (2, 3):
            raise ValueError(f"invalid TDL tap '{entry}'; use delay:gain_db:phase_deg")
        taps.append(
            ChannelTap(
                delay_samples=int(fields[0]),
                gain_db=float(fields[1]),
                phase_deg=float(fields[2]) if len(fields) == 3 else 0.0,
            )
        )
    return tuple(taps)


@dataclass(frozen=True)
class SimulationConfig:
    """Configuration for the first 2-Tx Alamouti CP-OFDM model.

    ``snr_db`` is Es/N0 for an information QAM symbol. Noise variance means
    E[|n|^2] for one complex receive sample. Total transmit power is kept at
    one by the Alamouti encoder's 1/sqrt(2) antenna normalization.
    """

    modulation: str = "qpsk"
    fft_size: int = 1024
    cp_length: int = 128
    subcarrier_spacing_hz: float = 15000.0
    guard_left: int = 0
    guard_right: int = 0
    dc_null: bool = False
    pilot_spacing: int = 0
    pilot_offset: int = 0
    preamble_enable: bool = False
    zc_root: int = 29
    synchronization_enable: bool = False
    pilot_phase_tracking_enable: bool = False
    pilot_phase_min_coherence: float = 0.0
    phase_reference_tracking_enable: bool = False
    phase_reference_count: int = 2
    sfo_tracking_enable: bool = False
    sfo_resampling_enable: bool = False
    sfo_resampling_interpolator: str = "sinc8"
    timing_offset_samples: int = 0
    timing_search_samples: int = 32
    cfo_hz: float = 0.0
    sfo_ppm: float = 0.0
    slot_phase_offset_deg: float = 0.0
    channel_estimation: str = "perfect"
    channel_estimation_taps: int = 10
    frames: int = 1000
    nt: int = 2
    nr: int = 1
    mode: str = "stbc"
    layers: int = 1
    detector: str = "mmse"
    fec_mode: str = "none"
    ldpc_iterations: int = 6
    ldpc_control_re_count: int = 128
    ldpc_transmit_rank: int = 2
    pairing: str = "time"
    channel: str = "rayleigh"
    doppler_hz: float = 0.0
    doppler_model: str = "symbol"
    tx_correlation: float = 0.0
    rx_correlation: float = 0.0
    spatial_rank: int = 0
    channel_taps: tuple[ChannelTap, ...] = DEFAULT_TDL_TAPS
    snr_db: float = 10.0
    seed: int = 0x5A17
    channel_seed: int | None = None
    noise_seed: int | None = None

    @property
    def bits_per_symbol(self) -> int:
        return MODULATION_BITS[self.modulation]

    @property
    def frame_bits(self) -> int:
        symbols_per_tone = (
            2 * self.layers if self.mode == "spatial_multiplexing" else 2
        )
        return symbols_per_tone * self.data_subcarrier_count * self.bits_per_symbol

    @property
    def ldpc_payload_layer_symbols(self) -> int:
        if self.fec_mode != "ldpc_1008_504":
            return 0
        physical_data_res = 2 * self.data_subcarrier_count
        return (
            physical_data_res - self.ldpc_control_re_count
        ) * self.ldpc_transmit_rank

    @property
    def ldpc_payload_blocks(self) -> int:
        if self.fec_mode != "ldpc_1008_504":
            return 0
        symbols_per_block = 1008 // self.bits_per_symbol
        return self.ldpc_payload_layer_symbols // symbols_per_block

    @property
    def active_centered_subcarriers(self) -> tuple[int, ...]:
        centered = tuple(range(-self.fft_size // 2, self.fft_size // 2))
        stop = self.fft_size - self.guard_right if self.guard_right else self.fft_size
        active = centered[self.guard_left : stop]
        return tuple(k for k in active if not (self.dc_null and k == 0))

    @property
    def pilot_centered_subcarriers(self) -> tuple[int, ...]:
        if self.pilot_spacing <= 0:
            return ()
        return tuple(
            k
            for k in self.active_centered_subcarriers
            if (k - self.pilot_offset) % self.pilot_spacing == 0
        )

    @property
    def data_centered_subcarriers(self) -> tuple[int, ...]:
        pilots = set(self.pilot_centered_subcarriers)
        candidates = tuple(k for k in self.active_centered_subcarriers if k not in pilots)
        if self.pairing == "time":
            return candidates

        # SFBC maps each Alamouti word to two adjacent frequencies.  Tones at
        # a guard/DC/pilot boundary that do not have an adjacent partner are
        # deliberately left null instead of pairing across a frequency gap.
        paired: list[int] = []
        index = 0
        while index + 1 < len(candidates):
            if candidates[index + 1] == candidates[index] + 1:
                paired.extend((candidates[index], candidates[index + 1]))
                index += 2
            else:
                index += 1
        return tuple(paired)

    @property
    def phase_reference_centered_subcarriers(self) -> tuple[int, ...]:
        if not self.phase_reference_tracking_enable:
            return ()
        pilots = self.pilot_centered_subcarriers
        if self.phase_reference_count == 1:
            return (pilots[len(pilots) // 2],)
        positions = tuple(
            round(index * (len(pilots) - 1) / (self.phase_reference_count - 1))
            for index in range(self.phase_reference_count)
        )
        return tuple(pilots[position] for position in positions)

    @property
    def channel_pilot_centered_subcarriers(self) -> tuple[int, ...]:
        references = set(self.phase_reference_centered_subcarriers)
        return tuple(k for k in self.pilot_centered_subcarriers if k not in references)

    @property
    def data_subcarrier_count(self) -> int:
        return len(self.data_centered_subcarriers)

    @property
    def sample_rate_hz(self) -> float:
        return self.fft_size * self.subcarrier_spacing_hz

    @property
    def useful_symbol_duration_s(self) -> float:
        return 1.0 / self.subcarrier_spacing_hz

    @property
    def cp_duration_s(self) -> float:
        return self.cp_length / self.sample_rate_hz

    @property
    def payload_bytes(self) -> int:
        if self.fec_mode == "ldpc_1008_504":
            return self.ldpc_payload_blocks * 63 - 2
        return self.frame_bits // 8 - 2

    @property
    def padding_bits(self) -> int:
        if self.fec_mode == "ldpc_1008_504":
            return (
                self.ldpc_payload_layer_symbols * self.bits_per_symbol
                - self.ldpc_payload_blocks * 1008
            )
        return self.frame_bits - (self.payload_bytes + 2) * 8

    def validate(self) -> None:
        if self.modulation not in MODULATION_BITS:
            raise ValueError(f"unsupported modulation: {self.modulation}")
        if self.fft_size <= 0 or self.fft_size % 2 != 0:
            raise ValueError("fft_size must be a positive even integer")
        if not 0 <= self.cp_length <= self.fft_size:
            raise ValueError("cp_length must be in [0, fft_size]")
        if not math.isfinite(self.subcarrier_spacing_hz) or self.subcarrier_spacing_hz <= 0:
            raise ValueError("subcarrier_spacing_hz must be positive and finite")
        if self.guard_left < 0 or self.guard_right < 0:
            raise ValueError("guard subcarriers must be non-negative")
        if self.guard_left + self.guard_right >= self.fft_size:
            raise ValueError("guard subcarriers leave no active spectrum")
        if self.pilot_spacing < 0:
            raise ValueError("pilot_spacing must be zero or positive")
        if not self.active_centered_subcarriers:
            raise ValueError("resource grid has no active subcarriers")
        if self.pilot_spacing > 0 and not self.pilot_centered_subcarriers:
            raise ValueError("pilot configuration places no pilot subcarriers")
        if not self.data_centered_subcarriers:
            raise ValueError("resource grid has no data subcarriers")
        if self.timing_offset_samples < 0:
            raise ValueError("timing_offset_samples must be non-negative")
        if self.timing_search_samples < self.timing_offset_samples:
            raise ValueError("timing_search_samples must cover timing_offset_samples")
        normalized_zc_root = self.zc_root % self.fft_size
        if normalized_zc_root == 0 or math.gcd(normalized_zc_root, self.fft_size) != 1:
            raise ValueError("zc_root must be non-zero and coprime to fft_size")
        if self.synchronization_enable and self.cp_length <= 0:
            raise ValueError("CP synchronization requires cp_length > 0")
        if self.pilot_phase_tracking_enable and self.pilot_spacing <= 0:
            raise ValueError("pilot phase tracking requires enabled comb pilots")
        if not math.isfinite(self.pilot_phase_min_coherence) or not (
            0.0 <= self.pilot_phase_min_coherence <= 1.0
        ):
            raise ValueError("pilot_phase_min_coherence must be in [0, 1]")
        if self.phase_reference_count <= 0:
            raise ValueError("phase_reference_count must be positive")
        if self.phase_reference_tracking_enable:
            if self.pilot_spacing <= 0:
                raise ValueError("phase-reference tracking requires enabled comb pilots")
            if self.phase_reference_count >= len(self.pilot_centered_subcarriers):
                raise ValueError("phase references must leave at least one channel pilot")
            if self.pilot_phase_tracking_enable:
                raise ValueError("select either perfect-CSI CPE tracking or phase references")
        if self.sfo_tracking_enable:
            if not self.phase_reference_tracking_enable:
                raise ValueError("SFO tracking requires enabled phase references")
            if self.phase_reference_count < 2:
                raise ValueError("SFO tracking requires at least two phase references")
        if self.sfo_resampling_enable:
            if not self.sfo_tracking_enable:
                raise ValueError("SFO resampling requires enabled SFO tracking")
            if not self.synchronization_enable or not self.preamble_enable:
                raise ValueError("SFO resampling requires ZC preamble synchronization")
        if self.sfo_resampling_interpolator not in {"cubic", "sinc8", "sinc24"}:
            raise ValueError(
                "SFO resampling interpolator must be cubic, sinc8 or sinc24"
            )
        if not math.isfinite(self.cfo_hz):
            raise ValueError("cfo_hz must be finite")
        if abs(self.cfo_hz) >= 0.5 * self.subcarrier_spacing_hz:
            raise ValueError("CP-based CFO estimate requires |cfo_hz| < subcarrier_spacing_hz/2")
        if not math.isfinite(self.sfo_ppm) or abs(self.sfo_ppm) >= 10000.0:
            raise ValueError("sfo_ppm must be finite with magnitude below 10000")
        if not math.isfinite(self.slot_phase_offset_deg):
            raise ValueError("slot_phase_offset_deg must be finite")
        if self.channel_estimation not in {"perfect", "ls_linear", "ls_dft", "lmmse"}:
            raise ValueError(f"unsupported channel estimation mode: {self.channel_estimation}")
        if self.channel_estimation != "perfect" and self.pilot_spacing <= 0:
            raise ValueError("pilot-aided channel estimation requires enabled comb pilots")
        if not 1 <= self.channel_estimation_taps <= self.fft_size:
            raise ValueError("channel_estimation_taps must be in [1, fft_size]")
        if (
            self.channel_estimation == "ls_dft"
            and (
                len(self.channel_pilot_centered_subcarriers) // self.nt
                if self.mode == "spatial_multiplexing" or self.pairing == "frequency"
                else len(self.channel_pilot_centered_subcarriers)
            )
            < self.channel_estimation_taps
        ):
            raise ValueError(
                "DFT-LS requires at least channel_estimation_taps pilot tones per Tx"
            )
        if self.channel_estimation != "perfect" and self.pilot_phase_tracking_enable:
            raise ValueError(
                "pilot phase tracking currently requires perfect channel estimation"
            )
        if self.frames <= 0:
            raise ValueError("frames must be positive")
        if self.nr <= 0:
            raise ValueError("nr must be positive")
        if self.mode not in {"stbc", "sfbc", "spatial_multiplexing"}:
            raise ValueError("mode must be stbc, sfbc or spatial_multiplexing")
        if self.detector not in {"zf", "mmse"}:
            raise ValueError("detector must be zf or mmse")
        if self.fec_mode not in {"none", "ldpc_1008_504"}:
            raise ValueError("fec_mode must be none or ldpc_1008_504")
        if self.ldpc_iterations <= 0:
            raise ValueError("ldpc_iterations must be positive")
        if self.ldpc_control_re_count <= 0:
            raise ValueError("ldpc_control_re_count must be positive")
        if self.mode == "spatial_multiplexing":
            if self.nt not in {2, 4, 8} or self.layers != self.nt or self.nr < self.layers:
                raise ValueError(
                    "spatial multiplexing requires nt=layers in {2,4,8} and nr>=layers"
                )
            if self.pilot_phase_tracking_enable:
                raise ValueError(
                    "spatial multiplexing does not support perfect-CSI pilot CPE tracking"
                )
        elif self.layers != 1:
            raise ValueError("Alamouti STBC/SFBC requires layers=1")
        elif self.nt != 2:
            raise ValueError("Alamouti STBC/SFBC requires nt=2")
        if self.fec_mode == "ldpc_1008_504":
            if self.mode != "spatial_multiplexing" or self.nt != 2 or self.layers != 2:
                raise ValueError("LDPC mixed-control framing currently requires 2x2 spatial multiplexing")
            if self.ldpc_control_re_count != 128:
                raise ValueError("C++ LDPC control region requires exactly 128 physical RE")
            if self.ldpc_control_re_count > self.data_subcarrier_count:
                raise ValueError("LDPC control region must fit in the first data OFDM symbol")
            if self.ldpc_transmit_rank not in {1, 2}:
                raise ValueError("LDPC transmit rank must be 1 or 2")
            if self.ldpc_transmit_rank > self.layers:
                raise ValueError("LDPC transmit rank cannot exceed configured layers")
            if 1008 % self.bits_per_symbol:
                raise ValueError("LDPC codeword must be divisible by modulation order")
            if self.ldpc_payload_blocks <= 0:
                raise ValueError("resource grid cannot hold one LDPC payload block")
        if self.pairing not in {"time", "frequency"}:
            raise ValueError("pairing must be 'time' (STBC) or 'frequency' (SFBC)")
        if self.pairing == "frequency":
            if (
                self.pilot_phase_tracking_enable
                or self.phase_reference_tracking_enable
                or self.sfo_tracking_enable
            ):
                raise ValueError("SFBC does not yet support pilot phase/SFO tracking")
            data = self.data_centered_subcarriers
            if len(data) % 2 or any(data[i + 1] != data[i] + 1 for i in range(0, len(data), 2)):
                raise ValueError("SFBC data tones must form adjacent frequency pairs")
        if self.channel not in {"awgn", "static", "rayleigh", "tdl"}:
            raise ValueError(f"unsupported channel: {self.channel}")
        if not math.isfinite(self.doppler_hz) or self.doppler_hz < 0.0:
            raise ValueError("doppler_hz must be finite and non-negative")
        if self.doppler_model not in {"symbol", "continuous"}:
            raise ValueError("doppler_model must be symbol or continuous")
        if not math.isfinite(self.tx_correlation) or not 0.0 <= self.tx_correlation <= 1.0:
            raise ValueError("tx_correlation must be finite and in [0, 1]")
        if not math.isfinite(self.rx_correlation) or not 0.0 <= self.rx_correlation <= 1.0:
            raise ValueError("rx_correlation must be finite and in [0, 1]")
        if not 0 <= self.spatial_rank <= min(self.nr, self.nt):
            raise ValueError("spatial_rank must be zero or no greater than min(nr,nt)")
        if (
            self.channel != "rayleigh"
            and (
                self.tx_correlation != 0.0
                or self.rx_correlation != 0.0
                or self.spatial_rank != 0
            )
        ):
            raise ValueError(
                "antenna correlation and spatial_rank currently require rayleigh channel"
            )
        if self.channel == "tdl":
            if not self.channel_taps:
                raise ValueError("TDL channel requires at least one tap")
            delays = [tap.delay_samples for tap in self.channel_taps]
            if any(delay < 0 for delay in delays):
                raise ValueError("TDL tap delays must be non-negative")
            if len(set(delays)) != len(delays):
                raise ValueError("TDL tap delays must be unique")
            if max(delays) > self.cp_length:
                raise ValueError(
                    f"TDL maximum delay {max(delays)} exceeds CP length {self.cp_length}"
                )
            if self.synchronization_enable and max(delays) >= self.cp_length:
                raise ValueError(
                    "CP synchronization requires TDL maximum delay below CP length"
                )
            for tap in self.channel_taps:
                if not math.isfinite(tap.gain_db) or not math.isfinite(tap.phase_deg):
                    raise ValueError("TDL gain and phase must be finite")
        if self.payload_bytes <= 0:
            raise ValueError("OFDM frame must hold a payload and CRC-16")

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)

    @classmethod
    def from_mapping(cls, values: Mapping[str, Any]) -> "SimulationConfig":
        phy = values.get("phy", values)
        mimo = values.get("mimo", {})
        channel = values.get("channel", {})
        simulation = values.get("simulation", {})
        resource_grid = values.get("resource_grid", {})
        pilots = resource_grid.get("pilots", {})
        synchronization = values.get("synchronization", {})
        receiver = values.get("receiver", {})
        fec = values.get("fec", {})

        mode = str(mimo.get("mode", "stbc")).lower()
        scheme = str(mimo.get("scheme", mimo.get("stbc", {}).get("scheme", "alamouti"))).lower()
        nt = int(mimo.get("nt", 2))
        layers = int(mimo.get("layers", 1))
        pairing = str(mimo.get("stbc", {}).get("pairing", "time")).lower()
        detector = str(mimo.get("detector", receiver.get("detector", "mmse"))).lower()
        if mode == "spatial_multiplexing":
            if nt not in {2, 4, 8} or layers != nt or detector not in {"zf", "mmse"}:
                raise ValueError(
                    "spatial multiplexing requires nt=layers in {2,4,8} and detector=zf/mmse"
                )
        elif mode in {"stbc", "sfbc"}:
            if nt != 2 or scheme != "alamouti" or layers != 1:
                raise ValueError("STBC/SFBC requires nt=2, scheme=alamouti and layers=1")
            if mode == "sfbc":
                pairing = "frequency"
            if pairing not in {"time", "frequency"}:
                raise ValueError("mimo.stbc.pairing must be time or frequency")
        else:
            raise ValueError("mimo.mode must be stbc, sfbc or spatial_multiplexing")

        snr_value = simulation.get("snr_db", 10.0)
        if isinstance(snr_value, (list, tuple)):
            if not snr_value:
                raise ValueError("simulation.snr_db list must not be empty")
            snr_value = snr_value[0]

        tap_values = channel.get("taps")
        if tap_values is None:
            channel_taps = DEFAULT_TDL_TAPS
        else:
            channel_taps = tuple(
                ChannelTap(
                    delay_samples=int(tap["delay_samples"]),
                    gain_db=float(tap["gain_db"]),
                    phase_deg=float(tap.get("phase_deg", 0.0)),
                )
                for tap in tap_values
            )

        cfg = cls(
            modulation=str(phy.get("modulation", "qpsk")).lower(),
            fft_size=int(phy.get("fft_size", 1024)),
            cp_length=int(phy.get("cp_length", 128)),
            subcarrier_spacing_hz=float(phy.get("subcarrier_spacing_hz", 15000.0)),
            guard_left=int(resource_grid.get("guard_left", 0)),
            guard_right=int(resource_grid.get("guard_right", 0)),
            dc_null=bool(resource_grid.get("dc_null", False)),
            pilot_spacing=(
                int(pilots.get("spacing", 8)) if bool(pilots.get("enabled", False)) else 0
            ),
            pilot_offset=int(pilots.get("offset", 0)),
            preamble_enable=bool(synchronization.get("preamble_enabled", False)),
            zc_root=int(synchronization.get("zc_root", 29)),
            synchronization_enable=bool(synchronization.get("enabled", False)),
            pilot_phase_tracking_enable=bool(
                synchronization.get("pilot_phase_tracking", False)
            ),
            pilot_phase_min_coherence=float(
                synchronization.get("pilot_phase_min_coherence", 0.0)
            ),
            phase_reference_tracking_enable=bool(
                synchronization.get("phase_reference_tracking", False)
            ),
            phase_reference_count=int(
                synchronization.get("phase_reference_count", 2)
            ),
            sfo_tracking_enable=bool(
                synchronization.get("sfo_tracking", False)
            ),
            sfo_resampling_enable=bool(
                synchronization.get("sfo_resampling", False)
            ),
            sfo_resampling_interpolator=str(
                synchronization.get("sfo_resampling_interpolator", "sinc8")
            ).lower(),
            timing_offset_samples=int(synchronization.get("timing_offset_samples", 0)),
            timing_search_samples=int(synchronization.get("timing_search_samples", 32)),
            cfo_hz=float(synchronization.get("cfo_hz", 0.0)),
            sfo_ppm=float(synchronization.get("sfo_ppm", 0.0)),
            slot_phase_offset_deg=float(
                synchronization.get("slot_phase_offset_deg", 0.0)
            ),
            channel_estimation=str(
                receiver.get("channel_estimation", "perfect")
            ).lower(),
            channel_estimation_taps=int(
                receiver.get(
                    "channel_estimation_taps",
                    receiver.get("channel_length_samples", 10),
                )
            ),
            frames=int(simulation.get("frames", 1000)),
            nt=nt,
            nr=int(mimo.get("nr", 1)),
            mode=mode,
            layers=layers,
            detector=detector,
            fec_mode=(
                str(fec.get("scheme", "ldpc_1008_504")).lower()
                if bool(fec.get("enabled", False))
                else "none"
            ),
            ldpc_iterations=int(fec.get("decoder_iterations", 6)),
            ldpc_control_re_count=int(fec.get("control_re_count", 128)),
            ldpc_transmit_rank=int(fec.get("transmit_rank", 2)),
            pairing=pairing,
            channel=str(channel.get("profile", "rayleigh")).lower(),
            doppler_hz=float(channel.get("doppler_hz", 0.0)),
            doppler_model=str(channel.get("doppler_model", "symbol")).lower(),
            tx_correlation=float(channel.get("tx_correlation", 0.0)),
            rx_correlation=float(channel.get("rx_correlation", 0.0)),
            spatial_rank=int(channel.get("spatial_rank", 0)),
            channel_taps=channel_taps,
            snr_db=float(snr_value),
            seed=int(simulation.get("seed", 0x5A17)),
            channel_seed=(
                int(simulation["channel_seed"])
                if simulation.get("channel_seed") is not None
                else None
            ),
            noise_seed=(
                int(simulation["noise_seed"])
                if simulation.get("noise_seed") is not None
                else None
            ),
        )
        cfg.validate()
        return cfg
