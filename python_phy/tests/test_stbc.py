import numpy as np

from openisac_phy.stbc import alamouti_combine_grid, alamouti_encode_grid


def test_alamouti_perfect_csi_noiseless_round_trip() -> None:
    rng = np.random.default_rng(11)
    batches, subcarriers, nr = 5, 32, 3
    symbols = rng.standard_normal((batches, subcarriers, 2)) + 1j * rng.standard_normal(
        (batches, subcarriers, 2)
    )
    tx_grid = alamouti_encode_grid(symbols)
    channel = (
        rng.standard_normal((batches, subcarriers, nr, 2))
        + 1j * rng.standard_normal((batches, subcarriers, nr, 2))
    ) / np.sqrt(2.0)
    rx_grid = np.einsum("btkx,bkrx->btkr", tx_grid, channel, optimize=True)
    recovered, equivalent_variance = alamouti_combine_grid(rx_grid, channel, 0.0)
    assert np.allclose(recovered, symbols, rtol=1.0e-12, atol=1.0e-12)
    assert np.count_nonzero(equivalent_variance) == 0


def test_alamouti_total_transmit_power_is_normalized() -> None:
    rng = np.random.default_rng(12)
    symbols = (
        rng.standard_normal((10000, 1, 2)) + 1j * rng.standard_normal((10000, 1, 2))
    ) / np.sqrt(2.0)
    grid = alamouti_encode_grid(symbols)
    total_power = np.mean(np.sum(np.abs(grid) ** 2, axis=-1))
    assert np.isclose(total_power, 1.0, rtol=0.03)
