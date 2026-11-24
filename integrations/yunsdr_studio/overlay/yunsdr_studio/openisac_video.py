"""Byte-checked MPEG-TS loopback through native OpenISAC PHY frames."""

from dataclasses import dataclass
from itertools import islice
import os
from pathlib import Path
import shutil
import subprocess
from tempfile import TemporaryDirectory
from threading import Event
from time import monotonic

import numpy as np

from .iq import complex_iq, quantize
from .openisac import RATE, SensingResult, generate_openisac, layout, receive_burst, sense_decoded_frames

TS_PACKET = 188
DATAGRAM = 7 * TS_PACKET


@dataclass(frozen=True)
class VideoResult:
    path: Path
    datagrams: int
    bytes: int
    seconds: float
    sensing: SensingResult | None = None

    @property
    def mbps(self):
        return self.bytes*8/self.seconds/1e6


def _cancelled(cancel):
    if cancel.is_set():
        raise RuntimeError("视频回环已取消")


def _ffmpeg():
    path = shutil.which("ffmpeg")
    if path:
        return path
    try:
        from imageio_ffmpeg import get_ffmpeg_exe
    except ImportError as exc:
        raise RuntimeError("MP4 转 MPEG-TS 需要 FFmpeg。可配置 PATH 或安装 imageio-ffmpeg") from exc
    return get_ffmpeg_exe()


def _process(command, **kwargs):
    return subprocess.Popen(command, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0), **kwargs)


def _ts_file(source, folder, cancel):
    if source.suffix.lower() in (".ts", ".mts"):
        return source
    output = folder / "input.ts"
    command = [_ffmpeg(), "-v", "error", "-nostdin", "-i", str(source), "-map", "0:v:0",
               "-map", "0:a:0?", "-c", "copy", "-f", "mpegts", str(output)]
    with _process(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE) as process:
        try:
            while True:
                _cancelled(cancel)
                try:
                    _, errors = process.communicate(timeout=.2)
                except subprocess.TimeoutExpired:
                    continue
                if process.returncode:
                    raise ValueError(f"无法将视频封装为 MPEG-TS · {errors.decode(errors='replace').strip()}")
                break
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()
    return output


def _validate_ts(packet):
    if len(packet) % TS_PACKET or any(packet[i] != 0x47 for i in range(0, len(packet), TS_PACKET)):
        raise ValueError("MPEG-TS 不是 188 字节对齐的有效传输流")


def _burst_channel(wave, rng, snr, period, coherent, first_sample):
    ports = wave.shape[0]
    indices = np.arange(1, ports+1, dtype=np.float32)
    direct = np.eye(ports, dtype=np.complex64) + np.complex64(.12j)
    echo = np.exp(.31j*np.outer(indices, indices))/np.sqrt(np.float32(ports))
    delay = 9
    received = np.pad(direct@wave, ((0, 0), (237, 100)))
    time = (first_sample+np.arange(wave.shape[1]))/RATE
    span = slice(237+delay, 237+delay+wave.shape[1])
    doppler = 2*RATE/(coherent*period)
    received[:, span] += .09*(echo@wave)*np.exp(2j*np.pi*doppler*time)
    adc_time = (first_sample+np.arange(received.shape[1])-237)/RATE
    received *= np.exp(2j*np.pi*310*adc_time)
    noise = np.sqrt(np.mean(abs(received)**2)*10**(-snr/10)/2)
    received += noise*(rng.standard_normal(received.shape, dtype=np.float32)
                       + 1j*rng.standard_normal(received.shape, dtype=np.float32))
    return complex_iq(quantize(received, -12, common_peak=True))


def _transfer_burst(inp, out, config, snr, cancel, progress, total_bytes, coherent, rf_hz):
    start = monotonic()
    rng = np.random.default_rng(9704)
    capacity = min(DATAGRAM, layout(config)[0])
    read_size = max(TS_PACKET, capacity//TS_PACKET*TS_PACKET)

    def fragments():
        for block in iter(lambda: inp.read(read_size), b""):
            _validate_ts(block)
            for start in range(0, len(block), capacity):
                yield block[start:start+capacity]

    packet_iter = fragments()
    frames_per_burst = max(32, coherent)
    packets = sent = 0
    sensing = None
    while batch := list(islice(packet_iter, frames_per_burst)):
        _cancelled(cancel)
        waveforms = []
        for i, packet in enumerate(batch):
            _cancelled(cancel)
            waveforms.append(generate_openisac(config, packet, (packets+i) % 65536)[0])
        samples = _burst_channel(np.concatenate(waveforms, axis=1), rng, snr, config.required_samples,
                                 coherent or frames_per_burst, packets*config.required_samples)
        collect = coherent > 0 and sensing is None and len(batch) >= coherent
        decoded = []
        for i, frame in enumerate(receive_burst(samples, config, csi=collect)):
            _cancelled(cancel)
            if (i >= len(batch) or frame.sequence != (packets+i) % 65536
                    or not frame.crc_ok or frame.payload != batch[i]):
                raise RuntimeError(f"第 {packets+i+1:,} 个视频 PHY 帧序号或载荷校验失败")
            out.write(frame.payload)
            sent += len(frame.payload)
            decoded.append(frame)
        if len(decoded) != len(batch):
            raise RuntimeError(f"第 {packets+len(decoded)+1:,} 个视频 PHY 帧同步失败")
        if collect:
            sensing = sense_decoded_frames(decoded[:coherent], config, rf_hz, True)
        packets += len(decoded)
        if progress:
            progress(sent, total_bytes)
    return packets, sent, max(monotonic()-start, 1e-6), sensing


def video_loopback(source, destination, config, *, snr=45, coherent=0, rf_hz=5.8e9, cancel=None, progress=None):
    source, destination = Path(source).resolve(), Path(destination).resolve()
    if source == destination or not source.is_file():
        raise ValueError("请选择有效的源视频和不同的接收文件")
    if coherent not in (0, 16, 32, 64, 128):
        raise ValueError("视频感知需要 16/32/64/128 帧")
    if not np.isfinite(snr) or (coherent and (not np.isfinite(rf_hz) or rf_hz <= 0)):
        raise ValueError("视频回环需要有效的 SNR 和感知中心频率")
    config.validate_capture(range(config.ports), RATE)
    cancel = cancel if cancel is not None else Event()
    with TemporaryDirectory(prefix="openisac-video-", dir=destination.parent) as temp:
        folder = Path(temp)
        ts = _ts_file(source, folder, cancel)
        total = ts.stat().st_size
        if not total:
            raise ValueError("MPEG-TS 数据为空")
        recovered = folder / "received.ts"
        with ts.open("rb") as inp, recovered.open("wb") as out:
            packets, sent, seconds, result = _transfer_burst(inp, out, config, snr, cancel, progress,
                                                             total, coherent, rf_hz)
        _cancelled(cancel)
        recovered.replace(destination)
    return VideoResult(destination, packets, sent, seconds, result)
