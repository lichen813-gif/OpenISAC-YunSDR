import numpy as np

from openisac_phy.qam import hard_demodulate, labels_to_bits, max_log_llrs, modulate


def test_all_supported_constellations_round_trip() -> None:
    for bits_per_symbol in (2, 4, 6, 8):
        labels = np.arange(1 << bits_per_symbol, dtype=np.int64)
        symbols = modulate(labels, bits_per_symbol)
        assert np.array_equal(hard_demodulate(symbols, bits_per_symbol), labels)
        assert np.isclose(np.mean(np.abs(symbols) ** 2), 1.0)


def test_llr_sign_matches_msb_first_labels() -> None:
    for bits_per_symbol in (2, 4, 6, 8):
        labels = np.arange(1 << bits_per_symbol, dtype=np.int64)
        symbols = modulate(labels, bits_per_symbol)
        llrs = max_log_llrs(symbols, 0.1, bits_per_symbol)
        expected_bits = labels_to_bits(labels, bits_per_symbol)
        assert np.array_equal(llrs < 0.0, expected_bits.astype(bool))
