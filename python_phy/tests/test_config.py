from openisac_phy.config import SimulationConfig


def test_yaml_style_snr_list_uses_first_point_for_base_config() -> None:
    cfg = SimulationConfig.from_mapping(
        {
            "phy": {"modulation": "16qam", "fft_size": 64, "cp_length": 16},
            "mimo": {
                "mode": "stbc",
                "scheme": "alamouti",
                "nt": 2,
                "nr": 2,
                "layers": 1,
                "stbc": {"pairing": "time"},
            },
            "channel": {"profile": "rayleigh", "doppler_hz": 300},
            "simulation": {"frames": 10, "snr_db": [0, 5, 10], "seed": 3},
        }
    )
    assert cfg.snr_db == 0.0
    assert cfg.nr == 2
    assert cfg.doppler_hz == 300.0


def test_yaml_style_continuous_doppler_model_is_loaded() -> None:
    cfg = SimulationConfig.from_mapping(
        {"channel": {"profile": "rayleigh", "doppler_hz": 500, "doppler_model": "continuous"}}
    )
    assert cfg.doppler_model == "continuous"


def test_invalid_doppler_model_is_rejected() -> None:
    cfg = SimulationConfig(doppler_model="sampled_once")
    try:
        cfg.validate()
    except ValueError as error:
        assert "symbol or continuous" in str(error)
        return
    raise AssertionError("invalid Doppler model was accepted")


def test_yaml_style_spatial_multiplexing_parameters_are_loaded() -> None:
    cfg = SimulationConfig.from_mapping(
        {
            "resource_grid": {"pilots": {"enabled": False}},
            "receiver": {"channel_estimation": "perfect"},
            "mimo": {
                "mode": "spatial_multiplexing",
                "nt": 2,
                "nr": 2,
                "layers": 2,
                "detector": "zf",
            },
            "simulation": {"channel_seed": 101, "noise_seed": 102},
        }
    )
    assert cfg.mode == "spatial_multiplexing"
    assert cfg.layers == 2
    assert cfg.detector == "zf"
    assert cfg.channel_seed == 101
    assert cfg.noise_seed == 102


def test_yaml_style_sfbc_accepts_fdm_pilots_and_estimated_csi() -> None:
    cfg = SimulationConfig.from_mapping(
        {
            "phy": {"fft_size": 128, "cp_length": 16},
            "resource_grid": {
                "dc_null": True,
                "pilots": {"enabled": True, "spacing": 4},
            },
            "receiver": {
                "channel_estimation": "ls_dft",
                "channel_estimation_taps": 10,
            },
            "mimo": {
                "mode": "sfbc",
                "nt": 2,
                "nr": 2,
                "layers": 1,
                "stbc": {"pairing": "frequency"},
            },
        }
    )

    assert cfg.mode == "sfbc"
    assert cfg.pairing == "frequency"
    assert cfg.pilot_spacing == 4
    assert cfg.channel_estimation == "ls_dft"


def test_yaml_style_correlated_rank_limited_rayleigh_is_loaded() -> None:
    cfg = SimulationConfig.from_mapping(
        {
            "mimo": {
                "mode": "spatial_multiplexing",
                "nt": 4,
                "nr": 4,
                "layers": 4,
            },
            "channel": {
                "profile": "rayleigh",
                "tx_correlation": 0.8,
                "rx_correlation": 0.6,
                "spatial_rank": 2,
            },
        }
    )

    assert cfg.tx_correlation == 0.8
    assert cfg.rx_correlation == 0.6
    assert cfg.spatial_rank == 2


def test_yaml_style_synchronization_parameters_are_loaded() -> None:
    cfg = SimulationConfig.from_mapping(
            {
                "phy": {"fft_size": 64, "cp_length": 16, "subcarrier_spacing_hz": 15000},
                "resource_grid": {"pilots": {"enabled": True, "spacing": 8}},
                "synchronization": {
                "enabled": True,
                "preamble_enabled": True,
                "zc_root": 29,
                "pilot_phase_tracking": True,
                "pilot_phase_min_coherence": 0.9,
                "timing_offset_samples": 5,
                "timing_search_samples": 16,
                "cfo_hz": 500,
            },
        }
    )
    assert cfg.synchronization_enable is True
    assert cfg.preamble_enable is True
    assert cfg.zc_root == 29
    assert cfg.pilot_phase_tracking_enable is True
    assert cfg.pilot_phase_min_coherence == 0.9
    assert cfg.timing_offset_samples == 5
    assert cfg.timing_search_samples == 16
    assert cfg.cfo_hz == 500.0


def test_default_openisac_fft_and_cp_parameters() -> None:
    cfg = SimulationConfig()
    assert cfg.fft_size == 1024
    assert cfg.cp_length == 128
    assert cfg.zc_root == 29


def test_pilot_phase_tracking_requires_pilots() -> None:
    cfg = SimulationConfig(pilot_phase_tracking_enable=True, pilot_spacing=0)
    try:
        cfg.validate()
    except ValueError as error:
        assert "requires enabled comb pilots" in str(error)
        return
    raise AssertionError("pilot phase tracking without pilots was accepted")


def test_ls_channel_estimation_requires_pilots() -> None:
    cfg = SimulationConfig(channel_estimation="ls_linear", pilot_spacing=0)
    try:
        cfg.validate()
    except ValueError as error:
        assert "requires enabled comb pilots" in str(error)
        return
    raise AssertionError("LS channel estimation without pilots was accepted")


def test_ls_mode_rejects_perfect_csi_pilot_phase_tracker() -> None:
    cfg = SimulationConfig(
        channel_estimation="ls_linear",
        pilot_spacing=8,
        pilot_phase_tracking_enable=True,
    )
    try:
        cfg.validate()
    except ValueError as error:
        assert "requires perfect channel estimation" in str(error)
        return
    raise AssertionError("LS mode accepted the perfect-CSI pilot phase tracker")


def test_dft_ls_requires_enough_pilots_for_assumed_channel_length() -> None:
    cfg = SimulationConfig(
        fft_size=64,
        cp_length=16,
        channel_estimation="ls_dft",
        channel_estimation_taps=10,
        pilot_spacing=8,
    )
    try:
        cfg.validate()
    except ValueError as error:
        assert "at least channel_estimation_taps pilot tones" in str(error)
        return
    raise AssertionError("underdetermined DFT-LS configuration was accepted")


def test_yaml_style_lmmse_receiver_parameters_are_loaded() -> None:
    cfg = SimulationConfig.from_mapping(
        {
            "resource_grid": {"pilots": {"enabled": True, "spacing": 4}},
            "receiver": {
                "channel_estimation": "lmmse",
                "channel_estimation_taps": 10,
            },
        }
    )
    assert cfg.channel_estimation == "lmmse"
    assert cfg.channel_estimation_taps == 10


def test_phase_references_must_leave_enough_dft_channel_pilots() -> None:
    cfg = SimulationConfig(
        fft_size=64,
        cp_length=16,
        dc_null=True,
        pilot_spacing=4,
        phase_reference_tracking_enable=True,
        phase_reference_count=2,
        channel_estimation="ls_dft",
        channel_estimation_taps=14,
    )
    try:
        cfg.validate()
    except ValueError as error:
        assert "at least channel_estimation_taps pilot tones" in str(error)
        return
    raise AssertionError("DFT-LS accepted too few channel pilots after phase reservation")


def test_yaml_style_phase_reference_parameters_are_loaded() -> None:
    cfg = SimulationConfig.from_mapping(
        {
            "resource_grid": {"pilots": {"enabled": True, "spacing": 4}},
            "synchronization": {
                "phase_reference_tracking": True,
                "phase_reference_count": 2,
                "sfo_tracking": True,
                "sfo_ppm": 80,
                "slot_phase_offset_deg": 30,
            },
            "receiver": {
                "channel_estimation": "lmmse",
                "channel_estimation_taps": 10,
            },
        }
    )
    assert cfg.phase_reference_tracking_enable is True
    assert cfg.phase_reference_count == 2
    assert cfg.sfo_tracking_enable is True
    assert cfg.sfo_ppm == 80.0
    assert cfg.slot_phase_offset_deg == 30.0


def test_sfo_tracking_requires_multiple_phase_references() -> None:
    cfg = SimulationConfig(
        pilot_spacing=8,
        phase_reference_tracking_enable=True,
        phase_reference_count=1,
        sfo_tracking_enable=True,
    )
    try:
        cfg.validate()
    except ValueError as error:
        assert "at least two phase references" in str(error)
        return
    raise AssertionError("SFO tracking accepted only one phase reference")


def test_yaml_style_spatial_sfo_resampling_parameters_are_loaded() -> None:
    cfg = SimulationConfig.from_mapping(
        {
            "resource_grid": {"pilots": {"enabled": True, "spacing": 4}},
            "synchronization": {
                "enabled": True,
                "preamble_enabled": True,
                "phase_reference_tracking": True,
                "phase_reference_count": 8,
                "sfo_tracking": True,
                "sfo_resampling": True,
            },
            "receiver": {"channel_estimation": "ls_linear"},
            "mimo": {
                "mode": "spatial_multiplexing",
                "nt": 2,
                "nr": 2,
                "layers": 2,
            },
        }
    )

    assert cfg.mode == "spatial_multiplexing"
    assert cfg.sfo_resampling_enable is True


def test_sfo_resampling_requires_zc_synchronization() -> None:
    cfg = SimulationConfig(
        pilot_spacing=4,
        phase_reference_tracking_enable=True,
        phase_reference_count=8,
        sfo_tracking_enable=True,
        sfo_resampling_enable=True,
    )
    try:
        cfg.validate()
    except ValueError as error:
        assert "ZC preamble synchronization" in str(error)
        return
    raise AssertionError("SFO resampling accepted a receiver without ZC synchronization")


def test_invalid_sfo_resampling_interpolator_is_rejected() -> None:
    cfg = SimulationConfig(sfo_resampling_interpolator="fft_resampler")
    try:
        cfg.validate()
    except ValueError as error:
        assert "cubic, sinc8 or sinc24" in str(error)
        return
    raise AssertionError("unsupported SFO interpolator was accepted")
