"""Paced H.264 video over the Studio OpenISAC PHY."""

from collections import deque
from contextlib import closing
from fractions import Fraction
from queue import Empty, Full
from struct import Struct
from threading import Lock
from time import perf_counter, perf_counter_ns, sleep

import numpy as np

from .openisac import OpenIsacConfig
from .openisac_video import DATAGRAM

WIDTH, HEIGHT, FPS = 640, 360, 25
VIDEO_CLOCK = Fraction(1, FPS)
LIVE_RATES = (1_000_000, 1_500_000, 2_000_000)
PROFILE = OpenIsacConfig(2, True, 64)
HEADER = Struct("!4sIIHHHBQ")
PAYLOAD = DATAGRAM - HEADER.size
MAGIC = b"YVL1"
MAX_FRAGMENTS = 96


class LatestFrame:
    """One-frame mailbox between capture, decoder and the GUI."""

    def __init__(self):
        self._lock = Lock()
        self._frame = None
        self.dropped = 0

    def put(self, frame, timestamp):
        with self._lock:
            self.dropped += self._frame is not None
            self._frame = (frame, timestamp)

    def take(self):
        with self._lock:
            frame, self._frame = self._frame, None
            return frame


def _percentiles(samples):
    if not samples:
        return {}
    median, p95, p99 = np.percentile(samples, [50, 95, 99])
    return dict(p50_ms=float(median), p95_ms=float(p95), p99_ms=float(p99), max_ms=max(samples))


class LiveStats:
    def __init__(self, role="loop"):
        self.role = role
        self.lock = Lock()
        self.started = None
        self.finished = None
        self.expected = {}
        self.sent = self.received = self.invalid = self.recovered_bytes = 0
        self.in_flight_peak = 0
        self.encode_drop = self.decoded = self.displayed = self.video_packets = self.keepalives = 0
        self.encode_frames = self.incomplete_frames = self.decode_errors = 0
        self.source_queue_peak = self.source_queue = 0
        self.first_display_ms = None
        self.packet_ms = deque(maxlen=250_000)
        self.display_ms = deque(maxlen=30_000)
        self.phy_queue = self.phy_queue_peak = 0
        self.air_packets = self.air_dropped = self.iq_gaps = self.iq_dropped = 0
        self.remote_lost = 0
        self.remote_stream = None
        self.remote_sequence = None

    def send(self, seq, payload_size):
        now = perf_counter_ns()
        with self.lock:
            self.sent += 1
            self.video_packets += payload_size > 0
            self.keepalives += payload_size == 0
            if self.role not in ("loop", "rf"):
                return
            self.expected[seq] = now
            while self.expected and now-next(iter(self.expected.values())) > 2_000_000_000:
                self.expected.pop(next(iter(self.expected)))
            self.in_flight_peak = max(self.in_flight_peak, len(self.expected))

    def accept(self, seq, size):
        now = perf_counter_ns()
        with self.lock:
            stamp = self.expected.pop(seq, None)
            if stamp is None:
                self.invalid += 1
                return False
            self.received += 1
            self.recovered_bytes += size
            self.packet_ms.append((now-stamp)/1e6)
            return True

    def accept_remote(self, stream, seq, size):
        with self.lock:
            if stream != self.remote_stream:
                self.remote_stream, self.remote_sequence = stream, None
            if self.remote_sequence is not None:
                if seq <= self.remote_sequence:
                    return False
                self.remote_lost += seq-self.remote_sequence-1
            self.remote_sequence = seq
            self.received += 1
            self.recovered_bytes += size
            return True

    def show(self, stamp):
        now = perf_counter_ns()
        with self.lock:
            self.displayed += 1
            if stamp is not None:
                self.display_ms.append((now-stamp)/1e6)
            if self.first_display_ms is None and self.started is not None:
                self.first_display_ms = (perf_counter()-self.started)*1000

    def snapshot(self, *, detail=True):
        with self.lock:
            duration = ((self.finished or perf_counter())-self.started) if self.started else 0
            result = dict(seconds=round(duration, 3), role=self.role, sent=self.sent, received=self.received,
                          lost=self.remote_lost if self.role == "rx" else
                          self.sent-self.received if self.role in ("loop", "rf") else None,
                          invalid=self.invalid, recovered_bytes=self.recovered_bytes, air_packets=self.air_packets,
                          air_dropped=self.air_dropped, iq_gaps=self.iq_gaps, iq_dropped=self.iq_dropped,
                          in_flight=len(self.expected), in_flight_peak=self.in_flight_peak,
                          offered_mbps=self.sent*DATAGRAM*8/max(duration, 1e-6)/1e6,
                          video_packets=self.video_packets, keepalives=self.keepalives,
                          source_queue=self.source_queue, source_queue_peak=self.source_queue_peak,
                          encode_drop=self.encode_drop, encode_frames=self.encode_frames, decoded=self.decoded,
                          displayed=self.displayed, incomplete_frames=self.incomplete_frames,
                          decode_errors=self.decode_errors, first_display_ms=self.first_display_ms,
                          phy_queue=self.phy_queue, phy_queue_peak=self.phy_queue_peak)
            if detail:
                packets, frames = tuple(self.packet_ms), tuple(self.display_ms)
            else:
                result["last_display_ms"] = self.display_ms[-1] if self.display_ms else None
                return result
        result["packet_latency"] = _percentiles(packets)
        result["display_latency"] = _percentiles(frames)
        return result


def _load_av():
    try:
        import av
    except ImportError as exc:
        raise RuntimeError("实时视频需要 PyAV，请安装 requirements.txt 中的依赖") from exc
    return av


def _fit_size(width, height):
    scale = min(WIDTH/width, HEIGHT/height)
    return max(2, int(width*scale)//2*2), max(2, int(height*scale)//2*2)


def _file_frames(av, path, loop, cancel):
    while not cancel.is_set():
        with av.open(str(path), mode="r") as source:
            if not source.streams.video:
                raise ValueError("所选文件没有视频轨")
            stream = source.streams.video[0]
            stream.codec_context.thread_count = 2
            origin, last_slot = None, -1
            start = perf_counter()
            fps = float(stream.average_rate or FPS)
            for index, frame in enumerate(source.decode(stream)):
                if cancel.is_set():
                    return
                pts = frame.time if frame.time is not None else index/fps
                if origin is None:
                    origin = pts
                elapsed = max(0.0, pts-origin)
                slot = int(elapsed*FPS+.5)
                if slot <= last_slot:
                    continue
                last_slot = slot
                while not cancel.is_set() and (remaining := start+elapsed-perf_counter()) > 0:
                    sleep(min(.02, remaining))
                if cancel.is_set():
                    return
                yield frame, perf_counter_ns()
            if origin is None:
                raise ValueError("所选视频没有可解码画面")
            if not loop or cancel.wait(1/min(FPS, fps)):
                return


def _camera_frames(av, mailbox, cancel):
    while not cancel.is_set():
        if sample := mailbox.take():
            rgb, stamp = sample
            yield av.VideoFrame.from_ndarray(rgb, format="rgb24"), stamp
        cancel.wait(1/FPS)


def _encoder(av, width, height, wire_rate):
    enc = av.CodecContext.create("libx264", "w")
    enc.width, enc.height, enc.pix_fmt = width, height, "yuv420p"
    enc.time_base, enc.framerate = VIDEO_CLOCK, Fraction(FPS)
    kbps = int((wire_rate*.82-FPS*DATAGRAM*4)/1000)
    enc.bit_rate = kbps*1000
    enc.thread_count = 2
    enc.options = {"preset": "ultrafast", "tune": "zerolatency",
                   "x264-params": (f"annexb=1:repeat-headers=1:scenecut=0:keyint={FPS}:min-keyint={FPS}:"
                                   f"vbv-maxrate={kbps}:vbv-bufsize={max(60, kbps//5)}")}
    enc.open()
    return enc


def _encode_frames(av, source, camera, loop, wire_rate, queue, cancel, stats):
    frames = (_camera_frames(av, camera, cancel) if source is None else _file_frames(av, source, loop, cancel))
    enc, force_key = None, False
    with closing(frames):
        for number, (frame, stamp) in enumerate(frames, 1):
            if enc is None:
                enc = _encoder(av, *_fit_size(frame.width, frame.height), wire_rate)
            frame = frame.reformat(width=enc.width, height=enc.height, format=enc.pix_fmt)
            frame.pts, frame.time_base = number, VIDEO_CLOCK
            if force_key:
                frame.pict_type = av.video.frame.PictureType.I
            packets = enc.encode(frame)
            if len(packets) != 1:
                raise RuntimeError("H.264 低延迟编码器未逐帧输出")
            packet = packets[0]
            data = bytes(packet)
            full = len(data) > MAX_FRAGMENTS*PAYLOAD
            if not full:
                try:
                    queue.put_nowait((number, stamp, packet.is_keyframe, data))
                except Full:
                    full = True
            force_key = full
            with stats.lock:
                stats.encode_frames += 1
                stats.encode_drop += full
                stats.source_queue = queue.qsize()
                stats.source_queue_peak = max(stats.source_queue_peak, stats.source_queue)


def _send_packets(emit, queue, encoder, check, cancel, stats, wire_rate):
    period = DATAGRAM*8/wire_rate
    deadline = perf_counter()
    seq = 0
    current = None
    while True:
        check()
        if current is None:
            try:
                number, stamp, key, data = queue.get_nowait()
            except Empty:
                if encoder.done():
                    encoder.result()
                    return
                if cancel.is_set():
                    sleep(.01)
                    continue
            else:
                parts = (len(data)+PAYLOAD-1)//PAYLOAD
                index = 0
                current = data
                with stats.lock:
                    stats.source_queue = queue.qsize()
        remaining = deadline-perf_counter()
        if remaining > 0:
            sleep(remaining)
        deadline = max(deadline+period, perf_counter()-period)
        if current is None:
            datagram = HEADER.pack(MAGIC, seq, 0, 0, 0, 0, 0, 0)
            size = 0
        else:
            chunk = current[index*PAYLOAD:(index+1)*PAYLOAD]
            size = len(chunk)
            datagram = HEADER.pack(MAGIC, seq, number, index, parts, size, key, stamp)+chunk
            index += 1
            if index >= parts:
                current = None
        stats.send(seq, size)
        emit(datagram.ljust(DATAGRAM, b"\0"), seq)
        seq += 1


def _receive_packets(av, packets, output, stats):
    decoder = None
    active_stream = None
    assembling = complete = 0
    scratch = bytearray()
    next_part = 0
    have_key = False
    for data, stream in packets:
        if len(data) == DATAGRAM:
            magic, seq, number, index, count, size, key, timestamp = HEADER.unpack_from(data)
        else:
            magic = None
        if (magic != MAGIC or size > PAYLOAD or (number and not (0 <= index < count <= MAX_FRAGMENTS and size))
                or (not number and (index or count or size or key or timestamp)) or key not in (0, 1)):
            with stats.lock:
                stats.invalid += 1
            continue
        accepted = stats.accept(seq, size) if stream is None else stats.accept_remote(stream, seq, size)
        if not accepted:
            continue
        if decoder is None or active_stream != stream:
            active_stream = stream
            decoder = av.CodecContext.create("h264", "r")
            decoder.thread_count = 1
            assembling = complete = next_part = 0
            scratch.clear()
            have_key = False
        if not number:
            continue
        if number != assembling:
            if scratch:
                with stats.lock:
                    stats.incomplete_frames += 1
            scratch.clear()
            assembling, next_part = number, 0
        if index != next_part:
            if next_part >= 0:
                with stats.lock:
                    stats.incomplete_frames += 1
            scratch.clear()
            next_part = -1
            continue
        scratch.extend(data[HEADER.size:HEADER.size+size])
        next_part += 1
        if next_part != count:
            continue
        if number != complete+1 and not key:
            have_key = False
        complete = number
        if key:
            have_key = True
            decoder.flush_buffers()
        if have_key:
            try:
                frames = decoder.decode(av.Packet(bytes(scratch)))
            except av.error.InvalidDataError:
                have_key = False
                with stats.lock:
                    stats.decode_errors += 1
            else:
                for frame in frames:
                    # PyAV may expose row padding as a non-contiguous view.
                    rgb = np.ascontiguousarray(frame.to_ndarray(format="rgb24"))
                    output.put(rgb, timestamp if stream is None else None)
                    with stats.lock:
                        stats.decoded += 1
        scratch.clear()
