import numpy as np

from openisac_phy import SimulationConfig, simulate_alamouti_ofdm
from openisac_phy.config import ChannelTap
from openisac_phy.resource_grid import build_resource_allocation, deterministic_pilot_pairs


def test_comb_pilot_and_dc_allocation() -> None:
    cfg = SimulationConfig(
        fft_size=64,
        cp_length=16,
        dc_null=True,
        pilot_spacing=8,
        pilot_offset=0,
        frames=1,
    )
    allocation = build_resource_allocation(cfg)
    assert allocation.pilot_centered.tolist() == [-32, -24, -16, -8, 8, 16, 24]
    assert allocation.data_indices.size == 56
    assert allocation.pilot_indices.size == 7
    assert allocation.null_indices.size == 1
    assert 0 in allocation.null_indices


def test_deterministic_pilots_are_unit_bpsk() -> None:
    centered = np.asarray([-24, -16, -8, 8, 16, 24])
    first = deterministic_pilot_pairs(4, centered, seed=99)
    second = deterministic_pilot_pairs(4, centered, seed=99)
    assert np.array_equal(first, second)
    assert set(np.unique(first).tolist()) <= {-1.0 + 0.0j, 1.0 + 0.0j}


def test_pilot_guard_padding_and_tdl_noiseless_round_trip() -> None:
    cfg = SimulationConfig(
        modulation="qpsk",
        fft_size=64,
        cp_length=16,
        subcarrier_spacing_hz=30000.0,
        guard_left=4,
        guard_right=4,
        dc_null=True,
        pilot_spacing=8,
        pilot_offset=0,
        frames=10,
        nr=2,
        channel="tdl",
        channel_taps=(
            ChannelTap(0, 0.0, 0.0),
            ChannelTap(3, -4.0, 45.0),
            ChannelTap(9, -8.0, -80.0),
        ),
        snr_db=float("inf"),
        seed=41,
    )
    result, artifacts = simulate_alamouti_ofdm(cfg, return_artifacts=True)
    assert result["bit_errors"] == 0
    assert result["crc_failures"] == 0
    assert result["active_subcarriers"] == 55
    assert result["pilot_subcarriers"] == 6
    assert result["data_subcarriers"] == 49
    assert result["padding_bits"] == 4
    assert artifacts.pilot_symbols.shape == (10, 6, 2)
    assert artifacts.equalized_pilots.shape == (10, 6, 2)
    assert artifacts.pilot_indices.shape == (6,)
    assert result["pilot_evm_rms"] < 1.0e-12


def test_phase_references_are_reserved_from_channel_pilots() -> None:
    cfg = SimulationConfig(
        fft_size=64,
        cp_length=16,
        dc_null=True,
        pilot_spacing=4,
        phase_reference_tracking_enable=True,
        phase_reference_count=2,
        frames=1,
    )
    allocation = build_resource_allocation(cfg)

    assert allocation.phase_reference_centered.tolist() == [-32, 28]
    assert allocation.phase_reference_indices.size == 2
    assert allocation.pilot_indices.size == 13
    assert allocation.data_indices.size == 48
    assert set(allocation.phase_reference_indices).isdisjoint(allocation.pilot_indices)
