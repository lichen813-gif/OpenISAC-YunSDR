import numpy as np

from openisac_phy import ofdm
from openisac_phy.synchronization import (
    estimate_cfo_at_known_timing,
    build_impaired_stream,
    estimate_cp_synchronization,
    extract_synchronized_symbols,
)


def test_known_timing_cp_cfo_estimator_avoids_second_timing_search() -> None:
    fft_size = 64
    cp_length = 16
    symbols = 3
    timing = 7
    normalized_cfo = 0.12
    rng = np.random.default_rng(2701)
    useful = rng.standard_normal((symbols, fft_size)) + 1j * rng.standard_normal(
        (symbols, fft_size)
    )
    framed = np.concatenate((useful[:, -cp_length:], useful), axis=1).reshape(-1)
    stream = np.zeros((1, 1, timing + framed.size + 8), dtype=np.complex128)
    stream[0, 0, timing : timing + framed.size] = framed
    sample = np.arange(stream.shape[-1], dtype=np.float64)
    stream *= np.exp(1j * 2.0 * np.pi * normalized_cfo * sample / fft_size)[
        None, None, :
    ]

    estimate = estimate_cfo_at_known_timing(
        stream,
        np.array([timing]),
        fft_size=fft_size,
        cp_length=cp_length,
        ofdm_symbols=symbols,
    )
    np.testing.assert_allclose(estimate, normalized_cfo, atol=1.0e-12)


def _ofdm_symbols(
    rng: np.random.Generator, batches: int, symbols: int, nr: int, fft_size: int, cp: int
) -> np.ndarray:
    grid = (
        rng.standard_normal((batches, symbols, nr, fft_size))
        + 1j * rng.standard_normal((batches, symbols, nr, fft_size))
    ) / np.sqrt(2.0)
    return ofdm.modulate(grid, cp)


def test_cp_sync_recovers_noiseless_timing_and_cfo() -> None:
    rng = np.random.default_rng(101)
    fft_size = 64
    cp = 16
    original = _ofdm_symbols(rng, batches=4, symbols=2, nr=2, fft_size=fft_size, cp=cp)
    stream = build_impaired_stream(
        original,
        fft_size=fft_size,
        timing_offset_samples=7,
        cfo_normalized=0.12,
        search_padding_samples=16,
        noise_variance=0.0,
        rng=rng,
    )
    estimate = estimate_cp_synchronization(
        stream,
        fft_size=fft_size,
        cp_length=cp,
        ofdm_symbols=2,
        max_search_samples=16,
    )
    recovered = extract_synchronized_symbols(
        stream,
        timing_offsets=estimate.timing_offsets,
        cfo_normalized=estimate.cfo_normalized,
        fft_size=fft_size,
        cp_length=cp,
        ofdm_symbols=2,
    )

    np.testing.assert_array_equal(estimate.timing_offsets, np.full(4, 7))
    np.testing.assert_allclose(estimate.cfo_normalized, 0.12, atol=1.0e-12)
    np.testing.assert_allclose(recovered, original, atol=1.0e-12)


def test_cp_sync_is_accurate_with_awgn_and_receive_diversity() -> None:
    rng = np.random.default_rng(103)
    fft_size = 64
    cp = 16
    original = _ofdm_symbols(rng, batches=100, symbols=2, nr=2, fft_size=fft_size, cp=cp)
    stream = build_impaired_stream(
        original,
        fft_size=fft_size,
        timing_offset_samples=5,
        cfo_normalized=-0.08,
        search_padding_samples=16,
        noise_variance=1.0e-3,
        rng=rng,
    )
    estimate = estimate_cp_synchronization(
        stream,
        fft_size=fft_size,
        cp_length=cp,
        ofdm_symbols=2,
        max_search_samples=16,
    )

    timing_success = np.mean(estimate.timing_offsets == 5)
    mean_cfo_error = np.mean(np.abs(estimate.cfo_normalized + 0.08))
    assert timing_success >= 0.98
    assert mean_cfo_error < 0.005
