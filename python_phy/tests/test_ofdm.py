import numpy as np

from openisac_phy.ofdm import demodulate, modulate


def test_unitary_cp_ofdm_round_trip() -> None:
    rng = np.random.default_rng(7)
    grid = rng.standard_normal((3, 2, 4, 64)) + 1j * rng.standard_normal((3, 2, 4, 64))
    samples = modulate(grid, cp_length=16)
    recovered = demodulate(samples, fft_size=64, cp_length=16)
    assert np.allclose(recovered, grid, rtol=1.0e-12, atol=1.0e-12)

