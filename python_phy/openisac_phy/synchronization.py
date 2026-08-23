"""ZC/CP-based coarse sample timing and carrier-frequency synchronization."""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np


@dataclass(frozen=True)
class SyncEstimate:
    timing_offsets: np.ndarray
    cfo_normalized: np.ndarray
    peak_metrics: np.ndarray
    metrics: np.ndarray


def estimate_zc_synchronization(
    stream: np.ndarray,
    reference_symbol: np.ndarray,
    *,
    fft_size: int,
    cp_length: int,
    ofdm_symbols: int,
    max_search_samples: int,
    correlation_skip_samples: int = 0,
) -> SyncEstimate:
    """Find a ZC preamble by matched filtering and estimate CFO from all CPs.

    The reference contains one complete CP-OFDM preamble.  Correlation power
    is normalized independently on every receive antenna before diversity
    combining, so unknown channel phase cannot cancel the timing metric.
    """

    stream = np.asarray(stream, dtype=np.complex128)
    reference = np.asarray(reference_symbol, dtype=np.complex128).reshape(-1)
    if stream.ndim != 3:
        raise ValueError("stream must have shape [batch, rx, sample]")
    if fft_size <= 0 or cp_length <= 0 or ofdm_symbols <= 0:
        raise ValueError("FFT, CP and OFDM symbol count must be positive")
    symbol_length = fft_size + cp_length
    if reference.shape != (symbol_length,):
        raise ValueError("reference_symbol must contain one complete CP-OFDM symbol")
    if not 0 <= correlation_skip_samples < cp_length:
        raise ValueError("correlation_skip_samples must be in [0, cp_length)")
    required = max_search_samples + ofdm_symbols * symbol_length
    if stream.shape[-1] < required:
        raise ValueError("stream is too short for the synchronization search window")

    batches, nr, _ = stream.shape
    metrics = np.zeros((batches, max_search_samples + 1), dtype=np.float64)
    reference_energy = float(np.sum(np.abs(reference) ** 2))
    for candidate in range(max_search_samples + 1):
        window = stream[:, :, candidate : candidate + symbol_length]
        correlation = np.sum(window * np.conj(reference)[None, None, :], axis=-1)
        window_energy = np.sum(np.abs(window) ** 2, axis=-1)
        branch_metric = np.abs(correlation) ** 2 / np.maximum(
            reference_energy * window_energy, 1.0e-30
        )
        metrics[:, candidate] = np.mean(branch_metric, axis=1)

    timing_offsets = np.argmax(metrics, axis=1).astype(np.int64)
    correlations = np.zeros(batches, dtype=np.complex128)
    for batch in range(batches):
        start = int(timing_offsets[batch])
        for symbol in range(ofdm_symbols):
            symbol_start = start + symbol * symbol_length
            prefix = stream[
                batch,
                :,
                symbol_start + correlation_skip_samples : symbol_start + cp_length,
            ]
            tail = stream[
                batch,
                :,
                symbol_start
                + fft_size
                + correlation_skip_samples : symbol_start
                + fft_size
                + cp_length,
            ]
            correlations[batch] += np.sum(np.conj(prefix) * tail)
    cfo_normalized = np.angle(correlations) / (2.0 * np.pi)
    peak_metrics = metrics[np.arange(batches), timing_offsets]
    return SyncEstimate(timing_offsets, cfo_normalized, peak_metrics, metrics)


def build_impaired_stream(
    rx_symbols: np.ndarray,
    *,
    fft_size: int,
    timing_offset_samples: int,
    cfo_normalized: float,
    search_padding_samples: int,
    noise_variance: float,
    rng: np.random.Generator,
) -> np.ndarray:
    """Serialize aligned OFDM symbols, add prefix delay and continuous CFO."""

    rx_symbols = np.asarray(rx_symbols, dtype=np.complex128)
    if rx_symbols.ndim != 4:
        raise ValueError("rx_symbols must have shape [batch, symbol, rx, sample]")
    if timing_offset_samples < 0 or search_padding_samples < 0:
        raise ValueError("timing offset and search padding must be non-negative")
    if noise_variance < 0:
        raise ValueError("noise_variance must be non-negative")
    batches, symbols, nr, samples_per_symbol = rx_symbols.shape
    payload = np.transpose(rx_symbols, (0, 2, 1, 3)).reshape(
        batches, nr, symbols * samples_per_symbol
    )
    stream_length = timing_offset_samples + payload.shape[-1] + search_padding_samples
    if noise_variance > 0:
        sigma = np.sqrt(noise_variance / 2.0)
        stream = sigma * (
            rng.standard_normal((batches, nr, stream_length))
            + 1j * rng.standard_normal((batches, nr, stream_length))
        )
    else:
        stream = np.zeros((batches, nr, stream_length), dtype=np.complex128)
    stop = timing_offset_samples + payload.shape[-1]
    stream[:, :, timing_offset_samples:stop] = payload
    sample_index = np.arange(stream_length, dtype=np.float64)
    rotation = np.exp(1j * 2.0 * np.pi * cfo_normalized * sample_index / fft_size)
    return stream * rotation[None, None, :]


def estimate_cp_synchronization(
    stream: np.ndarray,
    *,
    fft_size: int,
    cp_length: int,
    ofdm_symbols: int,
    max_search_samples: int,
    correlation_skip_samples: int = 0,
) -> SyncEstimate:
    """Estimate frame start and CFO by coherently combining CP correlations.

    ``correlation_skip_samples`` excludes the CP samples affected by the
    channel convolution transient. It should be an upper bound on channel
    delay spread and must leave at least one correlated CP sample.
    """

    stream = np.asarray(stream, dtype=np.complex128)
    if stream.ndim != 3:
        raise ValueError("stream must have shape [batch, rx, sample]")
    if fft_size <= 0 or cp_length <= 0 or ofdm_symbols <= 0:
        raise ValueError("FFT, CP and OFDM symbol count must be positive")
    if not 0 <= correlation_skip_samples < cp_length:
        raise ValueError("correlation_skip_samples must be in [0, cp_length)")
    symbol_length = fft_size + cp_length
    required = max_search_samples + ofdm_symbols * symbol_length
    if stream.shape[-1] < required:
        raise ValueError("stream is too short for the synchronization search window")

    batches = stream.shape[0]
    correlations = np.zeros((batches, max_search_samples + 1), dtype=np.complex128)
    metrics = np.zeros((batches, max_search_samples + 1), dtype=np.float64)
    for candidate in range(max_search_samples + 1):
        correlation = np.zeros(batches, dtype=np.complex128)
        prefix_power = np.zeros(batches, dtype=np.float64)
        tail_power = np.zeros(batches, dtype=np.float64)
        for symbol in range(ofdm_symbols):
            start = candidate + symbol * symbol_length
            prefix = stream[
                :, :, start + correlation_skip_samples : start + cp_length
            ]
            tail = stream[
                :,
                :,
                start + fft_size + correlation_skip_samples : start + fft_size + cp_length,
            ]
            correlation += np.sum(np.conj(prefix) * tail, axis=(1, 2))
            prefix_power += np.sum(np.abs(prefix) ** 2, axis=(1, 2))
            tail_power += np.sum(np.abs(tail) ** 2, axis=(1, 2))
        correlations[:, candidate] = correlation
        metrics[:, candidate] = np.abs(correlation) ** 2 / np.maximum(
            prefix_power * tail_power, 1.0e-30
        )

    timing_offsets = np.argmax(metrics, axis=1).astype(np.int64)
    selected = correlations[np.arange(batches), timing_offsets]
    cfo_normalized = np.angle(selected) / (2.0 * np.pi)
    peak_metrics = metrics[np.arange(batches), timing_offsets]
    return SyncEstimate(timing_offsets, cfo_normalized, peak_metrics, metrics)


def estimate_cfo_at_known_timing(
    stream: np.ndarray,
    timing_offsets: np.ndarray,
    *,
    fft_size: int,
    cp_length: int,
    ofdm_symbols: int,
    correlation_skip_samples: int = 0,
) -> np.ndarray:
    """Estimate CP-based CFO when frame timing is already known.

    This is the bounded-cost second-pass operation used after SFO resampling.
    It avoids repeating the full ZC timing search and maps directly to one
    streaming CP correlation accumulator in C++.
    """

    stream = np.asarray(stream, dtype=np.complex128)
    timing = np.asarray(timing_offsets, dtype=np.int64)
    if stream.ndim != 3:
        raise ValueError("stream must have shape [batch, rx, sample]")
    if timing.shape != (stream.shape[0],):
        raise ValueError("one timing offset is required per batch")
    if fft_size <= 0 or cp_length <= 0 or ofdm_symbols <= 0:
        raise ValueError("FFT, CP and OFDM symbol count must be positive")
    if not 0 <= correlation_skip_samples < cp_length:
        raise ValueError("correlation_skip_samples must be in [0, cp_length)")
    symbol_length = fft_size + cp_length
    correlations = np.zeros(stream.shape[0], dtype=np.complex128)
    for batch, frame_start in enumerate(timing):
        if frame_start < 0:
            raise ValueError("timing offsets must be non-negative")
        frame_stop = int(frame_start) + ofdm_symbols * symbol_length
        if frame_stop > stream.shape[-1]:
            raise ValueError("known-timing frame exceeds stream length")
        for symbol in range(ofdm_symbols):
            start = int(frame_start) + symbol * symbol_length
            prefix = stream[
                batch,
                :,
                start + correlation_skip_samples : start + cp_length,
            ]
            tail = stream[
                batch,
                :,
                start
                + fft_size
                + correlation_skip_samples : start
                + fft_size
                + cp_length,
            ]
            correlations[batch] += np.sum(np.conj(prefix) * tail)
    return np.angle(correlations) / (2.0 * np.pi)


def extract_synchronized_symbols(
    stream: np.ndarray,
    *,
    timing_offsets: np.ndarray,
    cfo_normalized: np.ndarray,
    fft_size: int,
    cp_length: int,
    ofdm_symbols: int,
) -> np.ndarray:
    """Correct CFO, slice the stream and return ``[batch,symbol,rx,sample]``."""

    stream = np.asarray(stream, dtype=np.complex128)
    timing_offsets = np.asarray(timing_offsets, dtype=np.int64)
    cfo_normalized = np.asarray(cfo_normalized, dtype=np.float64)
    if timing_offsets.shape != (stream.shape[0],) or cfo_normalized.shape != (stream.shape[0],):
        raise ValueError("one timing/CFO estimate is required per batch")
    symbol_length = fft_size + cp_length
    output = np.empty(
        (stream.shape[0], ofdm_symbols, stream.shape[1], symbol_length),
        dtype=np.complex128,
    )
    for batch in range(stream.shape[0]):
        start = int(timing_offsets[batch])
        stop = start + ofdm_symbols * symbol_length
        if stop > stream.shape[-1]:
            raise ValueError("synchronized frame exceeds stream length")
        sample_index = np.arange(start, stop, dtype=np.float64)
        correction = np.exp(
            -1j * 2.0 * np.pi * cfo_normalized[batch] * sample_index / fft_size
        )
        corrected = stream[batch, :, start:stop] * correction[None, :]
        output[batch] = corrected.reshape(
            stream.shape[1], ofdm_symbols, symbol_length
        ).transpose(1, 0, 2)
    return output
