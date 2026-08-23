import numpy as np

from openisac_phy.config import SimulationConfig
from openisac_phy.resource_grid import build_resource_allocation
from openisac_phy.sfbc import alamouti_sfbc_combine_grid, alamouti_sfbc_encode_grid
from openisac_phy.simulation import simulate_alamouti_ofdm
from openisac_phy.stbc import TX_SCALE


def test_sfbc_known_mapping() -> None:
    symbols = np.array([[[1 + 2j, 3 + 4j], [5 + 6j, 7 + 8j]]])
    grid = alamouti_sfbc_encode_grid(symbols, np.array([2, 3]), 8)

    expected_time0 = np.array([[1 + 2j, 5 + 6j], [-5 + 6j, 1 - 2j]])
    expected_time1 = np.array([[3 + 4j, 7 + 8j], [-7 + 8j, 3 - 4j]])
    np.testing.assert_allclose(grid[0, 0, 2:4], TX_SCALE * expected_time0)
    np.testing.assert_allclose(grid[0, 1, 2:4], TX_SCALE * expected_time1)


def test_sfbc_flat_channel_round_trip() -> None:
    rng = np.random.default_rng(11)
    symbols = rng.standard_normal((3, 4, 2)) + 1j * rng.standard_normal((3, 4, 2))
    indices = np.array([2, 3, 6, 7])
    tx_grid = alamouti_sfbc_encode_grid(symbols, indices, 8)
    channel = rng.standard_normal((3, 2, 8, 2, 2)) + 1j * rng.standard_normal(
        (3, 2, 8, 2, 2)
    )
    channel[:, :, 3] = channel[:, :, 2]
    channel[:, :, 7] = channel[:, :, 6]
    rx_grid = np.einsum("btkx,btkxr->btkr", tx_grid, np.swapaxes(channel, -1, -2))

    recovered, variance = alamouti_sfbc_combine_grid(rx_grid, channel, indices, 0.0)

    np.testing.assert_allclose(recovered, symbols, atol=1.0e-12)
    np.testing.assert_allclose(variance, 0.0)


def test_sfbc_resource_allocation_uses_adjacent_even_pairs() -> None:
    config = SimulationConfig(
        fft_size=64,
        cp_length=8,
        guard_left=3,
        guard_right=4,
        dc_null=True,
        pairing="frequency",
    )
    allocation = build_resource_allocation(config)

    assert allocation.data_indices.size % 2 == 0
    centered = allocation.data_centered
    assert np.all(centered[1::2] == centered[0::2] + 1)


def test_sfbc_end_to_end_noiseless_crc_and_bits() -> None:
    result = simulate_alamouti_ofdm(
        SimulationConfig(
            modulation="qpsk",
            fft_size=64,
            cp_length=16,
            dc_null=True,
            pairing="frequency",
            frames=6,
            nr=2,
            channel="static",
            snr_db=float("inf"),
            seed=2026,
        )
    )

    assert result["mode"] == "sfbc"
    assert result["pairing"] == "frequency"
    assert result["bit_errors"] == 0
    assert result["block_errors"] == 0
    assert result["crc_failures"] == 0
    assert result["evm_rms"] < 1.0e-12


def test_sfbc_fdm_pilot_ls_recovers_flat_channel_per_symbol() -> None:
    result = simulate_alamouti_ofdm(
        SimulationConfig(
            modulation="64qam",
            fft_size=64,
            cp_length=16,
            dc_null=True,
            pilot_spacing=4,
            pairing="frequency",
            channel_estimation="ls_linear",
            frames=8,
            nr=2,
            channel="rayleigh",
            doppler_hz=500.0,
            snr_db=float("inf"),
            seed=2101,
            channel_seed=2102,
        )
    )

    assert result["bit_errors"] == 0
    assert result["crc_failures"] == 0
    assert result["channel_estimation_nmse"] < 1.0e-25
    assert result["pilot_channel_nmse"] < 1.0e-25


def test_sfbc_fdm_pilot_dft_ls_recovers_noiseless_tdl() -> None:
    result = simulate_alamouti_ofdm(
        SimulationConfig(
            modulation="qpsk",
            fft_size=128,
            cp_length=16,
            dc_null=True,
            pilot_spacing=4,
            pairing="frequency",
            channel_estimation="ls_dft",
            channel_estimation_taps=10,
            frames=6,
            nr=2,
            channel="tdl",
            snr_db=float("inf"),
            seed=2201,
        )
    )

    assert result["bit_errors"] == 0
    assert result["crc_failures"] == 0
    assert result["channel_estimation_nmse"] < 1.0e-25
    assert result["alamouti_pair_channel_variation_nmse"] > 0.0
