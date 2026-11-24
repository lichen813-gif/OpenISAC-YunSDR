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

from .device import VIDEO_DMA_SAMPLES
from .iq import IQ_DTYPE, complex_iq, quantize
from .ofdm import NoFrame
from .openisac import FFT, SYMBOL, _decode_at, _positions, generate_openisac, layout
from .openisac_live import (HEADER, LIVE_RATES, PROFILE, _encode_frames, _load_av, _receive_packets,
                            _send_packets)
from .openisac_video import DATAGRAM

FRAGMENT = Struct("!4sQIB")
AIR_MAGIC = b"YSR2"
FRAME = PROFILE.required_samples
DMA = VIDEO_DMA_SAMPLES
LEAD_BLOCKS = 48


def _fragment_layout():
    capacity = layout(PROFILE)[0] - FRAGMENT.size
    count = (DATAGRAM+capacity-1)//capacity
    if count*FRAME > DMA:
        raise RuntimeError("视频分片超过单个 TX DMA 块")
    return capacity, count


def encode_datagram(data, stream, seq):
    capacity, count = _fragment_layout()
    if len(data) != DATAGRAM:
        raise ValueError("实时视频数据报长度不匹配")
    wave = np.zeros((2, DMA), np.complex64)
    for part in range(count):
        fragment = FRAGMENT.pack(AIR_MAGIC, stream, seq, part)+data[part*capacity:(part+1)*capacity]
        wave[:, part*FRAME:(part+1)*FRAME] = generate_openisac(PROFILE, fragment, (seq*count+part) % 65536)[0]
    return quantize(wave, -12, common_peak=True)


def _has_ofdm(samples):
    # The OFDM cyclic prefix gives a cheap CFO-insensitive gate for idle
    # DMA blocks. Exact ZC timing and CRC still qualify every decoded frame.
    decimation = 4
    width = (SYMBOL-FFT)//decimation
    a = samples[:, :-FFT:decimation]
    b = samples[:, FFT::decimation]

    def window_sum(values):
        prefix = np.r_[0, np.cumsum(values, dtype=np.complex128 if np.iscomplexobj(values) else np.float64)]
        return prefix[width:]-prefix[:-width]

    cross = window_sum(np.sum(a.conj()*b, axis=0))
    energy = window_sum(np.sum(abs(a)**2, axis=0))*window_sum(np.sum(abs(b)**2, axis=0))
    floor = energy.max(initial=0)*1e-8 + 1e-20
    return np.any(abs(cross)**2 > .20*np.maximum(energy, floor))


class AirReceiver:
    def __init__(self, stats, decode_workers=None):
        self.stats = stats
        self.decode_workers = decode_workers
        self.capacity, self.parts = _fragment_layout()
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
        self.tail = joined[:, -(FRAME-1):].copy()
        if joined.shape[1] < FRAME or not _has_ofdm(joined):
            return
        try:
            indices, _ = _positions(joined, PROFILE)
        except NoFrame:
            return
        def decode(index):
            try:
                return _decode_at(joined, PROFILE, index, diagnostics=False)
            except ValueError:
                return None

        # Keep reassembly in sample order while the independent PHY frames
        # are decoded in parallel.
        decoded_frames = (map(decode, indices) if self.decode_workers is None else
                          self.decode_workers.map(decode, indices))
        previous = -FRAME
        for index, decoded in zip(indices, decoded_frames):
            if decoded is None or index < previous+FRAME:
                continue
            if decoded.crc_ok and decoded.payload.startswith(AIR_MAGIC):
                previous = index
                if item := self._reassemble(decoded.payload):
                    yield item


def _simulate_datagram(data, seq, stream, stats, noise_power_ratio):
    # A datagram comprises a complete burst. DMA alignment adds only idle
    # samples after it, so the simulator can process independent bursts.
    active = _fragment_layout()[1]*FRAME
    iq = complex_iq(encode_datagram(data, stream, seq)[:, :active])
    sigma = np.sqrt(np.mean(np.abs(iq)**2)*noise_power_ratio/2)
    rng = np.random.default_rng([816, seq])
    noise = rng.standard_normal((*iq.shape, 2), dtype=np.float32)
    iq += sigma*noise.view(np.complex64).reshape(iq.shape)
    receiver = AirReceiver(stats)
    return next(receiver._push_iq(iq, seq*DMA), None)


def run_live_video(*, source, camera, output, stats, cancel, wire_rate=LIVE_RATES[0], snr=45, loop=False):
    if wire_rate not in LIVE_RATES or not np.isfinite(snr):
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
            incoming.put_nowait(phy.submit(_simulate_datagram, data, seq, stream, stats, noise_power_ratio))
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


def _tx_radio(device, av, source, camera, loop, rate, stats, cancel, *, rx_block=None, stream=None):
    raw_clock = np.empty((2, DMA, 2), IQ_DTYPE)
    for _ in range(9):
        if cancel.is_set():
            return
        device.video_read(raw_clock)
    encoder_queue, air_queue = Queue(maxsize=4), Queue(maxsize=8)
    if stream is None:
        stream = randbits(64)

    def enqueue(data, seq):
        try:
            air_queue.put_nowait((data, seq))
        except Full:
            with stats.lock:
                stats.air_dropped += 1

    with ThreadPoolExecutor(2, thread_name_prefix="studio-radio-tx") as workers:
        encoder = workers.submit(_encode_frames, av, source, camera, loop, rate, encoder_queue, cancel, stats)
        producer = workers.submit(_send_packets, enqueue, encoder_queue, encoder, lambda: None, cancel, stats, rate)
        stats.started = perf_counter()
        last_end = 0
        try:
            while True:
                timestamp = device.video_read(raw_clock)
                if rx_block is not None:
                    rx_block(raw_clock, timestamp)
                ticks = timestamp+DMA
                if last_end > ticks+(LEAD_BLOCKS+4)*DMA:
                    raise RuntimeError("TX 时间戳停滞或倒退，无法继续调度视频帧")
                try:
                    data, seq = air_queue.get_nowait()
                except Empty:
                    if producer.done():
                        producer.result()
                        if stats.finished is None:
                            stats.finished = perf_counter()
                        if ticks >= last_end+(2*DMA if rx_block is not None else 0):
                            break
                    continue
                wave = encode_datagram(data, stream, seq)
                timestamp = max(ticks+LEAD_BLOCKS*DMA, last_end)
                device.video_write(wave, timestamp)
                last_end = timestamp+DMA
                with stats.lock:
                    stats.air_packets += 1
        except BaseException:
            cancel.set()
            raise
        finally:
            stats.finished = stats.finished or perf_counter()


def _receive_radio(device, av, output, stats, cancel, *, transmit=None, stream=None):
    queue = Queue(maxsize=32)
    done = Event()

    def put(samples, timestamp):
        try:
            queue.put_nowait((samples.copy(), timestamp))
        except Full:
            with stats.lock:
                stats.iq_dropped += 1

    def read_iq():
        try:
            if transmit is not None:
                transmit(put)
                return
            samples = np.empty((2, DMA, 2), IQ_DTYPE)
            while not cancel.is_set():
                put(samples, device.video_read(samples))
        finally:
            done.set()

    def receive(assembler):
        while not cancel.is_set():
            try:
                samples, timestamp = queue.get(timeout=.04)
            except Empty:
                if done.is_set():
                    return
                continue
            for item in assembler.push(samples, timestamp):
                if stream is None:
                    yield item
                elif item[1] == stream:
                    # Same host clock and this session's actual RF samples.
                    yield item[0], None

    with ThreadPoolExecutor(4, thread_name_prefix="studio-radio-rx") as workers:
        assembler = AirReceiver(stats, workers)
        reader = workers.submit(read_iq)
        stats.started = perf_counter()
        try:
            _receive_packets(av, receive(assembler), output, stats)
            if done.is_set():
                reader.result()
        finally:
            cancel.set()
            stats.finished = perf_counter()


def run_air_video(device, *, role, source, camera, output, stats, cancel, wire_rate, loop):
    if role in ("tx", "rf") and ((source is None) == (camera is None) or wire_rate not in LIVE_RATES):
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
    with device.video_stream():
        if role == "tx":
            _tx_radio(device, av, source, camera, loop, wire_rate, stats, cancel)
        elif role == "rx":
            _receive_radio(device, av, output, stats, cancel)
        else:
            stream = randbits(64)

            def transmit(put):
                _tx_radio(device, av, source, camera, loop, wire_rate, stats, cancel, rx_block=put, stream=stream)

            _receive_radio(device, av, output, stats, cancel, transmit=transmit, stream=stream)
