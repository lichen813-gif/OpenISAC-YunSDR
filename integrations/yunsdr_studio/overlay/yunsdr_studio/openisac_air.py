"""Independent Studio video sender and receiver over synchronized 2-channel IQ."""

from collections import deque
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from queue import Empty, Full, Queue
from secrets import randbits
from struct import Struct
from threading import Event
from time import perf_counter

import numpy as np

from .device import VIDEO_DMA_SAMPLES, video_dma_samples
from .iq import IQ_DTYPE, complex_iq, quantize
from .ofdm import NoFrame
from .openisac import FFT, RATE, SYMBOL, _decode_at, _positions, generate_openisac, layout
from .openisac_live import (HEADER, LIVE_RATES, PROFILE, ROBUST_LIVE_RATES, _encode_frames, _load_av, _receive_packets,
                            _send_packets)
from .openisac_video import DATAGRAM

FRAGMENT = Struct("!4sQIB")
AIR_MAGIC = b"YSR2"
FRAME = PROFILE.required_samples
DMA = VIDEO_DMA_SAMPLES
LEAD_BLOCKS = 48


def _fragment_layout(profile=PROFILE):
    capacity = layout(profile)[0] - FRAGMENT.size
    count = (DATAGRAM+capacity-1)//capacity
    return capacity, count


def video_rates(profile, sample_rate=RATE):
    _, count = _fragment_layout(profile)
    dma = video_dma_samples(sample_rate)
    chunks = (count*profile.required_samples+dma-1)//dma
    max_rate = DATAGRAM*8*sample_rate//(chunks*dma)
    return tuple(rate for rate in (*ROBUST_LIVE_RATES, *LIVE_RATES) if rate <= max_rate)


def encode_datagram(data, stream, seq, profile=PROFILE, *, sample_rate=RATE):
    capacity, count = _fragment_layout(profile)
    if len(data) != DATAGRAM:
        raise ValueError("实时视频数据报长度不匹配")
    frame = profile.required_samples
    dma = video_dma_samples(sample_rate)
    padded = (count*frame+dma-1)//dma*dma
    wave = np.empty((2, count*frame), np.complex64)
    for part in range(count):
        fragment = FRAGMENT.pack(AIR_MAGIC, stream, seq, part)+data[part*capacity:(part+1)*capacity]
        wave[:, part*frame:(part+1)*frame] = generate_openisac(profile, fragment, (seq*count+part) % 65536)[0]
    packed = np.zeros((2, padded, 2), IQ_DTYPE)
    packed[:, :count*frame] = quantize(wave, -12, common_peak=True)
    return packed


def _cp_matches(samples, *, fft=FFT, cp=SYMBOL-FFT, decimation=4, threshold=.20):
    # The OFDM cyclic prefix gives a cheap CFO-insensitive gate for idle
    # DMA blocks. Exact ZC timing and CRC still qualify every decoded frame.
    if samples.shape[1] < fft+cp:
        return np.empty(0, dtype=bool)
    width = cp//decimation
    a = samples[:, :-fft:decimation]
    b = samples[:, fft::decimation]

    def window_sum(values):
        prefix = np.r_[0, np.cumsum(values, dtype=np.complex128 if np.iscomplexobj(values) else np.float64)]
        return prefix[width:]-prefix[:-width]

    cross = window_sum(np.sum(a.conj()*b, axis=0))
    energy = window_sum(np.sum(abs(a)**2, axis=0))*window_sum(np.sum(abs(b)**2, axis=0))
    floor = energy.max(initial=0)*1e-8 + 1e-20
    return abs(cross)**2 > threshold*np.maximum(energy, floor)


class AirReceiver:
    def __init__(self, stats, profile=PROFILE, sample_rate=RATE):
        self.stats = stats
        self.profile = profile
        self.sample_rate = sample_rate
        self.frame = profile.required_samples
        self.capacity, self.parts = _fragment_layout(profile)
        self.tail = np.empty((2, 0), np.complex64)
        self.end = None
        self.stream = self.seq = None
        self.fragments = []
        self.retired = deque(maxlen=8)

    def _reassemble(self, payload):
        if len(payload) <= FRAGMENT.size:
            return
        magic, stream, seq, index = FRAGMENT.unpack_from(payload)
        content = payload[FRAGMENT.size:]
        if (magic != AIR_MAGIC or index >= self.parts or stream in self.retired or
                len(content) != min(self.capacity, DATAGRAM-index*self.capacity)):
            return
        if stream != self.stream:
            # Join an already-running transmitter at its next full datagram.
            if index:
                return
            if self.stream is not None:
                self.retired.append(self.stream)
            self.stream, self.seq = stream, None
        if seq != self.seq:
            self.seq = seq
            self.fragments.clear()
        if index != len(self.fragments):
            return
        self.fragments.append(content)
        if len(self.fragments) == self.parts:
            data = b"".join(self.fragments)
            if HEADER.unpack_from(data)[1] == seq:
                return data, stream

    def push(self, samples, timestamp):
        yield from self._push_iq(complex_iq(samples), timestamp)

    def _push_iq(self, samples, timestamp):
        if self.end is not None and self.end != timestamp:
            self.tail = self.tail[:, :0]
            self.fragments.clear()
            with self.stats.lock:
                self.stats.iq_gaps += 1
        self.end = timestamp+samples.shape[1]
        joined = np.concatenate((self.tail, samples), axis=1)
        self.tail = joined[:, -(self.frame-1):].copy()
        yield from self.accept(self.decode_block(joined))

    def decode_block(self, joined, *, packed=False):
        if self.sample_rate == RATE:
            return self._decode_window(complex_iq(joined) if packed else joined)
        stride = 16
        sparse = joined[:, ::stride]
        if packed:
            sparse = complex_iq(sparse)
        matches = _cp_matches(sparse, fft=FFT//stride, cp=(SYMBOL-FFT)//stride, decimation=1, threshold=.56)
        if not matches.any():
            return ()
        payloads = []
        # Keep FFT search windows bounded as DMA blocks grow with the clock.
        # The overlap includes each frame completed across a window boundary.
        for offset in range(0, joined.shape[1], DMA):
            start = max(0, offset-self.frame+1)
            stop = min(joined.shape[1], offset+DMA)
            first = (start+stride-1)//stride
            end = (stop+stride-1)//stride - SYMBOL//stride + 1
            if end > first and matches[first:end].any():
                candidate = joined[:, start:stop]
                payloads.extend(self._decode_window(complex_iq(candidate) if packed else candidate))
        return payloads

    def _decode_window(self, joined):
        if joined.shape[1] < self.frame or not _cp_matches(joined).any():
            return ()
        try:
            indices, _ = _positions(joined, self.profile)
        except NoFrame:
            return ()
        payloads = []
        rejected = 0
        previous = -self.frame
        for index in indices:
            if index < previous+self.frame:
                continue
            try:
                decoded = _decode_at(joined, self.profile, index, sample_rate=self.sample_rate, diagnostics=False)
            except ValueError:
                rejected += 1
                continue
            if decoded.crc_ok and decoded.payload.startswith(AIR_MAGIC):
                previous = index
                payloads.append(decoded.payload)
                if self.stats.claim_phy_sample():
                    try:
                        sample = _decode_at(joined, self.profile, index, sample_rate=self.sample_rate, csi=True)
                    except ValueError:
                        continue
                    self.stats.publish_phy_sample(sample)
            else:
                rejected += 1
        with self.stats.lock:
            self.stats.phy_candidates += len(payloads)+rejected
            self.stats.phy_rejected += rejected
        return payloads

    def accept(self, payloads, *, gap=False):
        if gap:
            self.fragments.clear()
        for payload in payloads:
            if item := self._reassemble(payload):
                yield item


def _simulate_datagram(data, seq, stream, stats, noise_power_ratio, profile, sample_rate):
    # A datagram comprises a complete burst. DMA alignment adds only idle
    # samples after it, so the simulator can process independent bursts.
    active = _fragment_layout(profile)[1]*profile.required_samples
    iq = complex_iq(encode_datagram(data, stream, seq, profile, sample_rate=sample_rate)[:, :active])
    sigma = np.sqrt(np.mean(np.abs(iq)**2)*noise_power_ratio/2)
    rng = np.random.default_rng([816, seq])
    noise = rng.standard_normal((*iq.shape, 2), dtype=np.float32)
    iq += sigma*noise.view(np.complex64).reshape(iq.shape)
    receiver = AirReceiver(stats, profile, sample_rate)
    return next(receiver.accept(receiver.decode_block(iq)), None)


def run_live_video(*, source, camera, output, stats, cancel, wire_rate=LIVE_RATES[0], snr=45, loop=False,
                   profile=PROFILE, sample_rate=RATE):
    if wire_rate not in video_rates(profile, sample_rate) or not np.isfinite(snr):
        raise ValueError("请选择有效的实时视频码率和 SNR")
    if (source is None) == (camera is None):
        raise ValueError("请选择摄像头或本地视频")
    if source is not None and not Path(source).is_file():
        raise ValueError("找不到本地视频文件")
    av = _load_av()
    encoded = Queue(maxsize=4)
    incoming = Queue(maxsize=64)
    finished = Event()
    stream = randbits(64)
    noise_power_ratio = 10**(-snr/10)

    def enqueue(data, seq):
        if incoming.full():
            with stats.lock:
                stats.air_dropped += 1
        else:
            incoming.put_nowait(phy.submit(_simulate_datagram, data, seq, stream, stats, noise_power_ratio, profile,
                                           sample_rate))
            with stats.lock:
                stats.phy_queue = incoming.qsize()
                stats.phy_queue_peak = max(stats.phy_queue_peak, stats.phy_queue)

    def datagrams():
        while not finished.is_set() or not incoming.empty():
            try:
                pending = incoming.get(timeout=.02)
            except Empty:
                continue
            with stats.lock:
                stats.phy_queue = incoming.qsize()
            result = pending.result()
            if result is not None:
                yield result[0], None

    # Decode bursts concurrently and release the results in transmission order.
    with ThreadPoolExecutor(4, thread_name_prefix="openisac-sim-phy") as phy, \
            ThreadPoolExecutor(2, thread_name_prefix="openisac-live") as workers:
        receiver = workers.submit(_receive_packets, av, datagrams(), output, stats)
        encoder = workers.submit(_encode_frames, av, source, camera, loop, wire_rate, encoded, cancel, stats)
        stats.started = perf_counter()

        def check():
            if receiver.done():
                receiver.result()

        try:
            _send_packets(enqueue, encoded, encoder, check, cancel, stats, wire_rate)
            stats.finished = perf_counter()
        finally:
            cancel.set()
            finished.set()
        receiver.result()
        encoder.result()


def _tx_radio(device, av, source, camera, loop, rate, stats, cancel, profile, sample_rate,
              *, rx_block=None, stream=None):
    dma = video_dma_samples(sample_rate)
    raw_clock = np.empty((2, dma, 2), IQ_DTYPE)
    for _ in range(9):
        if cancel.is_set():
            return
        device.video_read(raw_clock)
    encoder_queue, air_queue = Queue(maxsize=4), Queue(maxsize=8)
    if stream is None:
        stream = randbits(64)

    def enqueue(data, seq):
        # Waveform generation runs on the producer worker, outside the
        # timing-sensitive DMA read/write loop.
        if air_queue.full():
            with stats.lock:
                stats.air_dropped += 1
            return
        wave = encode_datagram(data, stream, seq, profile, sample_rate=sample_rate)
        air_queue.put_nowait(wave)

    with ThreadPoolExecutor(2, thread_name_prefix="studio-radio-tx") as workers:
        encoder = workers.submit(_encode_frames, av, source, camera, loop, rate, encoder_queue, cancel, stats)
        producer = workers.submit(_send_packets, enqueue, encoder_queue, encoder, lambda: None, cancel, stats, rate)
        stats.started = perf_counter()
        last_end = 0
        wave, offset = None, 0
        try:
            while True:
                timestamp = device.video_read(raw_clock)
                if rx_block is not None:
                    rx_block(raw_clock, timestamp)
                ticks = timestamp+dma
                if last_end > ticks+(LEAD_BLOCKS+4)*dma:
                    raise RuntimeError("TX 时间戳停滞或倒退，无法继续调度视频帧")
                if wave is None:
                    try:
                        wave = air_queue.get_nowait()
                    except Empty:
                        if producer.done():
                            producer.result()
                            if stats.finished is None:
                                stats.finished = perf_counter()
                            if ticks >= last_end+(2*dma if rx_block is not None else 0):
                                break
                        continue
                    offset = 0
                timestamp = last_end if offset else max(ticks+LEAD_BLOCKS*dma, last_end)
                if timestamp <= ticks:
                    with stats.lock:
                        stats.air_dropped += 1
                    wave = None
                    continue
                device.video_write(wave[:, offset:offset+dma], timestamp)
                last_end = timestamp+dma
                offset += dma
                if offset == wave.shape[1]:
                    wave = None
                    with stats.lock:
                        stats.air_packets += 1
        except BaseException:
            cancel.set()
            raise
        finally:
            stats.finished = stats.finished or perf_counter()


def _receive_radio(device, av, output, stats, cancel, profile, sample_rate, *, transmit=None, stream=None):
    queue = Queue(maxsize=96)
    done = Event()
    assembler = AirReceiver(stats, profile, sample_rate)
    tail = None
    end = None
    dropped = False

    def put(samples, timestamp):
        nonlocal end, tail, dropped
        gap = end is not None and end != timestamp
        end = timestamp+samples.shape[1]
        if gap:
            with stats.lock:
                stats.iq_gaps += 1
        if queue.full():
            tail, dropped = None, True
            with stats.lock:
                stats.iq_dropped += 1
            return
        # Retain raw IQ overlap for a ZC frame crossing a DMA boundary. Keep
        # sync/LDPC work out of this thread and process blocks in parallel.
        joined = np.concatenate((tail, samples), axis=1) if tail is not None and not gap else samples.copy()
        tail = samples[:, -(profile.required_samples-1):].copy()
        queue.put_nowait((gap or dropped, workers.submit(assembler.decode_block, joined, packed=True)))
        dropped = False

    def read_iq():
        try:
            if transmit is not None:
                transmit(put)
                return
            samples = np.empty((2, video_dma_samples(sample_rate), 2), IQ_DTYPE)
            while not cancel.is_set():
                put(samples, device.video_read(samples))
        finally:
            done.set()

    def receive():
        while not done.is_set() or not queue.empty():
            try:
                gap, pending = queue.get(timeout=.04)
            except Empty:
                continue
            for item in assembler.accept(pending.result(), gap=gap):
                if stream is None:
                    yield item
                elif item[1] == stream:
                    # Same host clock and this session's actual RF samples.
                    yield item[0], None

    with ThreadPoolExecutor(4, thread_name_prefix="studio-radio-phy") as workers, \
            ThreadPoolExecutor(1, thread_name_prefix="studio-radio-io") as io:
        reader = io.submit(read_iq)
        stats.started = perf_counter()
        try:
            _receive_packets(av, receive(), output, stats)
            reader.result()
        finally:
            cancel.set()
            stats.finished = perf_counter()


def run_air_video(device, *, role, source, camera, output, stats, cancel, wire_rate, loop, profile=PROFILE,
                  sample_rate=RATE):
    if role in ("tx", "rf") and ((source is None) == (camera is None) or
                                 wire_rate not in video_rates(profile, sample_rate)):
        raise ValueError("设备发射需要有效的视频源和链路码率")
    if role not in ("tx", "rx", "rf"):
        raise ValueError("请选择设备发送、设备接收或本机射频回环")
    if role in ("tx", "rf") and source is not None and not source.is_file():
        raise ValueError("找不到本地视频文件")
    if role == "rf":
        settings = device.read_settings(0)
        if settings.tx_lo != settings.rx_lo:
            raise ValueError("本机射频回环要求 CH 0 / CH 1 的 TX LO 与 RX LO 一致")
    av = _load_av()
    if cancel.is_set():
        return
    with device.video_stream(sample_rate):
        if role == "tx":
            _tx_radio(device, av, source, camera, loop, wire_rate, stats, cancel, profile, sample_rate)
        elif role == "rx":
            _receive_radio(device, av, output, stats, cancel, profile, sample_rate)
        else:
            stream = randbits(64)

            def transmit(put):
                _tx_radio(device, av, source, camera, loop, wire_rate, stats, cancel, profile, sample_rate, rx_block=put,
                          stream=stream)

            _receive_radio(device, av, output, stats, cancel, profile, sample_rate, transmit=transmit, stream=stream)
