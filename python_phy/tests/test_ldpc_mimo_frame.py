from openisac_phy.config import SimulationConfig
from openisac_phy.ldpc_frame import build_ldpc_mimo_frame_layout
from openisac_phy.simulation import simulate_alamouti_ofdm


def _formal_ldpc_config(**overrides: object) -> SimulationConfig:
    values = dict(
        modulation="64qam",
        fft_size=1024,
        cp_length=128,
        guard_left=64,
        guard_right=63,
        dc_null=True,
        pilot_spacing=4,
        frames=1,
        nt=2,
        nr=2,
        mode="spatial_multiplexing",
        layers=2,
        detector="mmse",
        fec_mode="ldpc_1008_504",
        channel="rayleigh",
        snr_db=40.0,
    )
    values.update(overrides)
    return SimulationConfig(**values)


def test_formal_ldpc_mimo_resource_layout_is_exact() -> None:
    config = _formal_ldpc_config()
    layout = build_ldpc_mimo_frame_layout(config)

    assert config.data_subcarrier_count == 672
    assert layout.control_data_positions.size == 128
    assert layout.payload_physical_re_count == 1216
    assert layout.payload_layer_symbol_capacity == 2432
    assert layout.payload_blocks == 14
    assert layout.coded_qam_symbol_count == 2352
    assert layout.padding_qam_symbol_count == 80
    assert layout.information_bytes == 882
    assert layout.user_payload_bytes == 880
    assert config.padding_bits == 480


def test_yaml_style_ldpc_parameters_are_loaded() -> None:
    config = SimulationConfig.from_mapping(
        {
            "phy": {"modulation": "64qam", "fft_size": 256, "cp_length": 32},
            "resource_grid": {"dc_null": True},
            "mimo": {
                "mode": "spatial_multiplexing",
                "nt": 2,
                "nr": 2,
                "layers": 2,
                "detector": "mmse",
            },
            "fec": {
                "enabled": True,
                "scheme": "ldpc_1008_504",
                "decoder_iterations": 5,
                "control_re_count": 128,
                "transmit_rank": 1,
            },
        }
    )

    assert config.fec_mode == "ldpc_1008_504"
    assert config.ldpc_iterations == 5
    assert config.ldpc_control_re_count == 128
    assert config.ldpc_transmit_rank == 1


def test_ldpc_mimo_noiseless_end_to_end_recovers_header_payload_and_crc() -> None:
    result = simulate_alamouti_ofdm(
        _formal_ldpc_config(
            fft_size=256,
            cp_length=32,
            guard_left=0,
            guard_right=0,
            pilot_spacing=0,
            frames=2,
            detector="zf",
            channel="rayleigh",
            snr_db=float("inf"),
            seed=501,
            channel_seed=502,
        )
    )

    assert result["pre_ldpc_ber"] == 0.0
    assert result["post_ldpc_ber"] == 0.0
    assert result["bit_errors"] == 0
    assert result["crc_failures"] == 0
    assert result["control_header_failures"] == 0
    assert result["ldpc_syndrome_failure_rate"] == 0.0
    assert result["mean_control_marker_metric"] == 1.0
    assert result["control_evm_rms"] < 1.0e-11


def test_ldpc_rank1_noiseless_frame_uses_single_stream_and_mrc() -> None:
    result = simulate_alamouti_ofdm(
        _formal_ldpc_config(
            fft_size=256,
            cp_length=32,
            guard_left=0,
            guard_right=0,
            pilot_spacing=0,
            frames=2,
            ldpc_transmit_rank=1,
            channel="rayleigh",
            snr_db=float("inf"),
            seed=801,
            channel_seed=802,
        )
    )

    assert result["transmit_rank"] == 1
    assert result["ldpc_payload_blocks"] == 2
    assert result["ldpc_user_payload_bytes"] == 124
    assert result["bit_errors"] == 0
    assert result["crc_failures"] == 0
    assert result["control_header_failures"] == 0
