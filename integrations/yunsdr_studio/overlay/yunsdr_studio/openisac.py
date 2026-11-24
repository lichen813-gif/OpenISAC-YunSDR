"""OpenISAC formal frame and coherent sensing adapter for the Studio workbench."""

import ctypes as C
from dataclasses import dataclass, replace
from functools import lru_cache
import os
from pathlib import Path

import numpy as np
from scipy.fft import fft, ifft, next_fast_len
from scipy.signal import find_peaks

from .ofdm import NoFrame

RATE = 15_360_000
FFT = 1024
SYMBOL = FFT + 128
ORDERS = (4, 16, 64, 256)
DEFAULT_CARRIERS = 816
# The independent PHY reference uses the original carrier layout.
LEGACY_CARRIERS = 896
TOPOLOGIES = (("2×2 · 2 层空间复用", 2), ("4×4 · 4 层空间复用", 4),
              ("8×8 · 8 层空间复用", 8), ("2×2 · Alamouti STBC", "stbc"),
              ("1×1 · SISO", 1))
RUNTIME = Path(__file__).resolve().parents[1] / "output" / "openisac_runtime"
DATA_SAMPLES = 2 * SYMBOL
_float_ptr = C.POINTER(C.c_float)
_u32_ptr = C.POINTER(C.c_uint32)


def csi_frequency_response(csi, sample_rate):
    """Return the occupied baseband bins and RX-by-TX channel magnitude in dB."""
    half = DEFAULT_CARRIERS//2
    bins = np.arange(-half, half+1)
    magnitude = 20*np.log10(np.maximum(np.abs(csi[..., bins]), 1e-8))
    magnitude[..., half] = np.nan  # DC is unused.
    return bins*sample_rate/(FFT*1e6), magnitude


@dataclass(frozen=True)
class OpenIsacConfig:
    ports: int = 2
    stbc: bool = False
    order: int = 64
    dmrs: bool = False
    carriers: int = DEFAULT_CARRIERS

    def __post_init__(self):
        if (self.ports not in (1, 2, 4, 8) or (self.stbc and self.ports != 2)
                or self.order not in ORDERS or self.carriers not in (DEFAULT_CARRIERS, LEGACY_CARRIERS)):
            raise ValueError("无效的 OpenISAC 模式")

    @classmethod
    def from_topology(cls, topology, order, dmrs):
        if topology == "stbc":
            return cls(2, True, order, dmrs)
        return cls(topology, False, order, dmrs)

    @property
    def edge(self):
        return self.carriers//2

    def nominal_bandwidth(self, rate):
        # Include half a bin beyond each of the two outer carrier centres.
        return (self.carriers+1)*rate/FFT

    @property
    def layers(self):
        return 1 if self.stbc else self.ports

    @property
    def bits(self):
        return self.order.bit_length() - 1

    @property
    def required_samples(self):
        return (5 if self.dmrs else 3) * SYMBOL

    @property
    def mode(self):
        scheme = "Alamouti STBC" if self.stbc else ("SISO" if self.ports == 1 else f"{self.layers} 层空间复用")
        return f"OpenISAC · {self.ports}×{self.ports} {scheme} · {'DM-RS' if self.dmrs else 'FDM'} · {self.carriers} 载波"

    def validate_capture(self, channels, rate, samples=None):
        if self.carriers != DEFAULT_CARRIERS:
            raise ValueError("工作台 OpenISAC 正式帧使用 816 载波")
        if not 0 < rate <= 0xffffffff:
            raise ValueError("OpenISAC 需要有效的 IQ 采样率")
        if len(channels) != self.ports:
            raise ValueError(f"OpenISAC 需要 {self.ports} 路同步 IQ")
        if samples is not None and samples < self.required_samples:
            raise ValueError(f"OpenISAC 每路需要至少 {self.required_samples:,} 点")


@dataclass(frozen=True)
class OpenIsacResult:
    sync_index: int
    cfo_hz: float
    sfo_ppm: float
    sequence: int
    bits: int
    payload: bytes
    crc_ok: bool
    evm: tuple[float, ...]
    bit_errors: tuple[int, ...]
    compared_bits: tuple[int, ...]
    constellation: np.ndarray
    csi: np.ndarray | None
    sample_rate: int = RATE
    coded_symbols: int = 0

    @property
    def payload_constellations(self):
        return payload_points(self.constellation, self.coded_symbols)


@dataclass(frozen=True)
class SensingResult:
    power: np.ndarray
    ranges: np.ndarray
    doppler: np.ndarray
    velocity: np.ndarray
    # Distance, Doppler, radial velocity and power above the CFAR threshold.
    detections: tuple[tuple[float, float, float, float], ...]


class _Metrics(C.Structure):
    _fields_ = [(name, C.c_uint32) for name in ("sequence", "crc", "payload_bytes", "symbols", "bits", "coded_symbols")]
    _fields_ += [("cfo_hz", C.c_float), ("sfo_ppm", C.c_float), ("evm", C.c_float*8),
                 ("bit_errors", C.c_uint32*8), ("compared_bits", C.c_uint32*8)]


class _Detection(C.Structure):
    _fields_ = [(field, C.c_float) for field in ("range", "doppler", "velocity", "excess")]


def _pointer(data):
    return data.ctypes.data_as(_float_ptr)


def runtime_path():
    return Path(os.environ.get("YUNSDR_OPENISAC_RUNTIME", RUNTIME)).resolve()


class _Native:
    def __init__(self, directory):
        name = "studio_openisac.dll" if os.name == "nt" else "libstudio_openisac.so"
        try:
            lib = C.CDLL(str(directory / name))
        except OSError as exc:
            raise RuntimeError(f"无法载入 OpenISAC 运行库。请构建 openisac_native 并安装到 {directory}") from exc
        u, p, s, z = C.c_uint, C.c_void_p, C.c_char_p, C.c_size_t
        self._bind(lib, "abi", u)
        if self.abi() != 7:
            raise RuntimeError("OpenISAC 运行库 ABI 与 Studio 不匹配")
        self._bind(lib, "open", p, s, s)
        self._bind(lib, "close", None, p)
        self._bind(lib, "profile", C.c_int, u, u, u, u, _u32_ptr, _u32_ptr, s)
        self._bind(lib, "preamble", None, p, u, _float_ptr)
        self._bind(lib, "encode", C.c_int, p, u, u, u, u, u, u, s, z, _float_ptr, _float_ptr, s)
        self._bind(lib, "decode", C.c_int, p, u, u, u, u, _float_ptr, s, _float_ptr, _float_ptr, C.POINTER(_Metrics), s)
        self._bind(lib, "sense", C.c_int, u, u, C.c_float, C.c_float, u, u, _float_ptr, _float_ptr,
                   C.POINTER(_Detection), _u32_ptr, _float_ptr, s)
        error = C.create_string_buffer(512)
        self.context = self.open(os.fsencode(directory), error)
        if not self.context:
            raise RuntimeError(error.value.decode(errors="replace"))
        self.zc = {}
        for carriers in (DEFAULT_CARRIERS, LEGACY_CARRIERS):
            self.zc[carriers//2] = np.empty(SYMBOL, np.complex64)
            self.preamble(self.context, carriers//2, _pointer(self.zc[carriers//2]))

    def _bind(self, library, name, result, *args):
        function = getattr(library, "studio_isac_" + name)
        function.restype, function.argtypes = result, args
        setattr(self, name, function)

    def __del__(self):
        if getattr(self, "context", None):
            self.close(self.context)

    @staticmethod
    def call(function, *args):
        error = C.create_string_buffer(512)
        if not function(*args, error):
            raise ValueError(error.value.decode(errors="replace"))


@lru_cache(maxsize=1)
def _native(directory):
    return _Native(directory)


def backend():
    return _native(runtime_path())


@lru_cache(maxsize=32)
def layout(config):
    native = backend()
    capacity, symbols = C.c_uint32(), C.c_uint32()
    native.call(native.profile, config.ports, config.stbc, config.bits, config.edge,
                C.byref(capacity), C.byref(symbols))
    return capacity.value, symbols.value


def generate_openisac(config, payload, sequence=0):
    capacity, symbols = layout(config)
    if not 0 < len(payload) <= capacity or not 0 <= sequence <= 65535:
        raise ValueError(f"OpenISAC 每帧可容纳 1–{capacity:,} 字节，帧序号范围 0–65535")
    native = backend()
    wave = np.empty((config.ports, config.required_samples), np.complex64)
    constellation = np.empty((symbols//config.layers, config.layers), np.complex64)
    native.call(native.encode, native.context, config.ports, config.stbc, config.bits, config.dmrs, config.edge, sequence,
                bytes(payload), len(payload), _pointer(wave), _pointer(constellation))
    return wave, constellation


def payload_symbol_count(payload_bytes, bits):
    blocks = (payload_bytes+2+62)//63
    return blocks*1008//bits


def payload_points(constellation, coded_symbols):
    layers = constellation.shape[1]
    packed = constellation.reshape(-1)[:coded_symbols]
    return tuple(packed[layer::layers] for layer in range(layers))


@lru_cache(maxsize=8)
def _preamble_spectrum(length, directory, edge):
    return fft(_native(directory).zc[edge], n=length).conj()


def _positions(samples, config):
    """Return whole-frame ZC candidates scored jointly over simultaneous RX ports."""
    if samples.ndim != 2 or samples.shape[0] != config.ports or not np.iscomplexobj(samples):
        raise ValueError(f"OpenISAC 输入需要 {config.ports} 路复数 IQ")
    native = backend()
    count = samples.shape[1] - config.required_samples + 1
    if count <= 0:
        raise NoFrame(f"OpenISAC 需要每路至少 {config.required_samples:,} 点")
    data = samples[:, :count+SYMBOL-1]
    length = next_fast_len(data.shape[1])
    preamble = native.zc[config.edge]
    template = (_preamble_spectrum(length, runtime_path(), config.edge) if length <= 262_144
                else fft(preamble, n=length).conj())
    # Only valid offsets are read, so no template window wraps at the FFT boundary.
    correlation = ifft(fft(data, n=length)*template, overwrite_x=True)[:, :count]
    numerator = np.sum(abs(correlation)**2, axis=0, dtype=np.float64)
    power = np.r_[0, np.cumsum(np.sum(abs(data)**2, axis=0), dtype=np.float64)]
    denominator = power[SYMBOL:]-power[:-SYMBOL]
    # A floor relative to measured peak power avoids false detections in zero guards.
    norm = np.vdot(preamble, preamble).real
    score = numerator / (np.maximum(denominator, max(denominator.max()*1e-7, 1e-18))*norm)
    peaks, _ = find_peaks(np.r_[0, score, 0], height=.18, distance=SYMBOL//2)
    if not peaks.size:
        raise NoFrame("未检测到完整的 OpenISAC ZC 前导")
    return peaks-1, score[peaks-1]


def _decode_at(samples, config, start, *, csi=False, sample_rate=RATE, diagnostics=True):
    native = backend()
    capacity, symbols = layout(replace(config, order=256))
    payload = C.create_string_buffer(capacity)
    info = _Metrics()
    raw = np.ascontiguousarray(samples[:, start:start+config.required_samples], dtype=np.complex64)
    constellation = np.empty(symbols if diagnostics else 0, np.complex64)
    response = np.empty((config.ports, config.ports, FFT), np.complex64) if csi else None
    native.call(native.decode, native.context, config.ports, config.stbc, config.dmrs, config.edge, _pointer(raw), payload,
                _pointer(constellation) if diagnostics else None, _pointer(response) if csi else None, C.byref(info))
    n = config.layers
    return OpenIsacResult(int(start), info.cfo_hz*sample_rate/RATE, info.sfo_ppm, info.sequence, info.bits,
                          payload.raw[:info.payload_bytes], bool(info.crc), tuple(info.evm[:n]),
                          tuple(info.bit_errors[:n]), tuple(info.compared_bits[:n]),
                          constellation[:info.symbols].reshape(-1, n), response, sample_rate, info.coded_symbols)


def receive_openisac(samples, config, *, csi=False, sample_rate=RATE):
    candidates, scores = _positions(samples, config)
    first_failure = None
    bad_crc = None
    # Search in capture order among the strongest candidates, including complete repetitions.
    for index in candidates[scores >= 0.75*scores.max()][:16]:
        try:
            decoded = _decode_at(samples, config, int(index), csi=csi, sample_rate=sample_rate)
        except ValueError as exc:
            if first_failure is None:
                first_failure = exc
        else:
            if decoded.crc_ok:
                return decoded
            if bad_crc is None:
                bad_crc = decoded
    if bad_crc is not None:
        return bad_crc
    raise NoFrame(f"OpenISAC 帧头或译码失败 · {first_failure}")


def receive_burst(samples, config, *, csi=False):
    """Decode complete frames on the sample-time lattice of a finite burst."""
    candidates, scores = _positions(samples, config)
    period = config.required_samples
    anchor = candidates[np.argmax(scores)]
    for index in candidates[(candidates-anchor) % period == 0]:
        yield _decode_at(samples, config, int(index), csi=csi)


def sense_decoded_frames(results, config, rf_hz, reject_static):
    sample_rate = results[0].sample_rate
    period = config.required_samples/sample_rate
    batch = np.stack([item.csi for item in results])
    # Remove common oscillator phase without removing relative path motion.
    cfo = np.median([item.cfo_hz for item in results])
    times = (np.array([item.sync_index for item in results])-results[0].sync_index)/sample_rate
    batch *= np.exp(-2j*np.pi*cfo*times)[:, None, None, None]
    return _sense_cube(batch, period, rf_hz, reject_static, sample_rate)


def sense_openisac(samples, config, rf_hz, coherent=64, *, reject_static=False, sample_rate=RATE):
    if coherent not in (16, 32, 64, 128) or not np.isfinite(rf_hz) or rf_hz <= 0:
        raise ValueError("感知需要 16/32/64/128 帧和有效射频中心频率")
    config.validate_capture(range(config.ports), sample_rate)
    candidates, _ = _positions(samples, config)
    lookup = set(candidates)
    period = config.required_samples
    for first in candidates:
        if first-period in lookup:
            continue
        sequence = range(int(first), int(first)+coherent*period, period)
        if not all(index in lookup for index in sequence):
            continue
        try:
            results = [_decode_at(samples, config, index, csi=True, sample_rate=sample_rate) for index in sequence]
        except ValueError:
            continue
        if all(right.sequence == (left.sequence+1) % 65536 for left, right in zip(results, results[1:])):
            return sense_decoded_frames(results, config, rf_hz, reject_static)
    raise NoFrame(f"需要 {coherent} 个时间连续且序号连续的 OpenISAC 帧")


def sensing_demo(config, rf_hz, coherent=64, *, reject_static=True, sample_rate=RATE):
    """Synthesize contiguous communication frames with a known moving echo."""
    if coherent not in (16, 32, 64, 128):
        raise ValueError("相干处理需要 16/32/64/128 帧")
    rng = np.random.default_rng(901)
    packet_size = min(96, layout(config)[0])
    wave = np.concatenate([generate_openisac(config, rng.bytes(packet_size), i)[0] for i in range(coherent)], axis=1)
    ports = config.ports
    direct = np.eye(ports) + 0.12j*np.ones((ports, ports))
    echo = np.exp(.31j*np.outer(np.arange(1, ports+1), np.arange(1, ports+1))) / np.sqrt(ports)
    received = direct @ wave
    delay = 9
    doppler_hz = 2*sample_rate/(coherent*config.required_samples)
    time = np.arange(wave.shape[1]-delay)/sample_rate
    received[:, delay:] += 0.24*(echo @ wave[:, :-delay])*np.exp(2j*np.pi*doppler_hz*time)
    noise = np.sqrt(np.mean(abs(received)**2)*1e-4/2)
    received += noise*(rng.normal(size=received.shape) + 1j*rng.normal(size=received.shape))
    return sense_openisac(received.astype(np.complex64), config, rf_hz, coherent,
                         reject_static=reject_static, sample_rate=sample_rate)


def _sense_cube(batch, period, rf_hz, reject_static, sample_rate):
    native = backend()
    count, ports, _, nfft = batch.shape
    power = np.empty((count, nfft//2), np.float32)
    hits, detections, bins = C.c_uint32(), (_Detection*32)(), (C.c_float*3)()
    native.call(native.sense, ports, count, period, rf_hz, reject_static, sample_rate, _pointer(batch),
                _pointer(power), detections, C.byref(hits), bins)
    shifted = np.arange(count) - count//2
    entries = tuple((m.range, m.doppler, m.velocity, m.excess) for m in detections[:hits.value])
    rel_db = 10*np.log10(np.maximum(power/max(float(power.max()), 1e-20), 1e-8))
    return SensingResult(rel_db, np.arange(nfft//2)*bins[0], shifted*bins[1], shifted*bins[2], entries)
