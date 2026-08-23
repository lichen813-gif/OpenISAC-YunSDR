import numpy as np

from openisac_phy import ofdm
from openisac_phy.preamble import (
    build_zc_preamble,
    generate_zc_frequency,
    generate_zc_ofdm_symbol,
)
from openisac_phy.synchronization import (
    build_impaired_stream,
    estimate_zc_synchronization,
    extract_synchronized_symbols,
)


def test_zc_frequency_matches_openisac_definition() -> None:
    fft_size = 64
    root = 29
    sequence = generate_zc_frequency(fft_size, root)
    index = np.arange(fft_size)
    expected = np.exp(-1j * np.pi * root * index**2 / fft_size)

    np.testing.assert_allclose(sequence, expected, atol=1.0e-12)
    np.testing.assert_allclose(np.abs(sequence), 1.0, atol=1.0e-12)


def test_zc_ofdm_symbol_has_cyclic_prefix_and_tx0_only_mapping() -> None:
    frequency, time = generate_zc_ofdm_symbol(64, 16, 29)
    mapped_frequency, preamble = build_zc_preamble(3, 2, 64, 16, 29)

    np.testing.assert_allclose(mapped_frequency, frequency)
    np.testing.assert_allclose(time[:16], time[-16:])
    np.testing.assert_allclose(
        preamble[:, 0, 0, :], np.broadcast_to(time, (3, time.size))
    )
    np.testing.assert_array_equal(preamble[:, 0, 1, :], 0.0)
    assert preamble.shape == (3, 1, 2, 80)


def test_zc_sync_recovers_noiseless_timing_and_cfo() -> None:
    rng = np.random.default_rng(401)
    _, preamble = build_zc_preamble(4, 1, 64, 16, 29)
    payload_grid = (
        rng.standard_normal((4, 2, 1, 64))
        + 1j * rng.standard_normal((4, 2, 1, 64))
    ) / np.sqrt(2.0)
    payload = ofdm.modulate(payload_grid, 16)
    frame = np.concatenate((preamble, payload), axis=1)
    stream = build_impaired_stream(
        frame,
        fft_size=64,
        timing_offset_samples=7,
        cfo_normalized=0.12,
        search_padding_samples=16,
        noise_variance=0.0,
        rng=rng,
    )
    estimate = estimate_zc_synchronization(
        stream,
        preamble[0, 0, 0],
        fft_size=64,
        cp_length=16,
        ofdm_symbols=3,
        max_search_samples=16,
    )
    recovered = extract_synchronized_symbols(
        stream,
        timing_offsets=estimate.timing_offsets,
        cfo_normalized=estimate.cfo_normalized,
        fft_size=64,
        cp_length=16,
        ofdm_symbols=3,
    )

    np.testing.assert_array_equal(estimate.timing_offsets, np.full(4, 7))
    np.testing.assert_allclose(estimate.cfo_normalized, 0.12, atol=1.0e-12)
    np.testing.assert_allclose(recovered, frame, atol=1.0e-12)
