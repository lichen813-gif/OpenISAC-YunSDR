from pathlib import Path

import yaml

from openisac_phy import SimulationConfig
from openisac_phy.resource_grid import build_resource_allocation


def test_formal_2x2_spatial_profile_frame_structure_is_frozen() -> None:
    config_path = (
        Path(__file__).resolve().parents[1]
        / "configs"
        / "mimo_2x2_spatial_multiplexing_realtime_1024.yaml"
    )
    cfg = SimulationConfig.from_mapping(
        yaml.safe_load(config_path.read_text(encoding="utf-8"))
    )
    allocation = build_resource_allocation(cfg)

    assert (cfg.fft_size, cfg.cp_length) == (1024, 128)
    assert cfg.sample_rate_hz == 15_360_000.0
    assert (cfg.nt, cfg.nr, cfg.layers) == (2, 2, 2)
    assert cfg.modulation == "64qam"
    assert allocation.data_indices.size == 672
    assert allocation.pilot_indices.size == 216
    assert allocation.phase_reference_indices.size == 8
    assert allocation.null_indices.size == 128
    assert cfg.frame_bits == 16_128
    assert cfg.payload_bytes == 2014
    assert cfg.padding_bits == 0
    assert cfg.preamble_enable is True
    assert cfg.sfo_ppm == 20.0
    assert cfg.sfo_tracking_enable is True
    assert cfg.sfo_resampling_enable is False
    assert cfg.sfo_resampling_interpolator == "sinc8"
    assert cfg.doppler_hz == 100.0
    assert cfg.doppler_model == "continuous"
