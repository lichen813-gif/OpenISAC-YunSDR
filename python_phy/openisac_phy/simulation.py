"""End-to-end CRC/QAM/Alamouti STBC/SFBC/OFDM simulation pipeline."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, overload

import numpy as np

from . import ofdm, qam
from .channels import (
    apply_continuous_doppler_mimo,
    apply_flat_mimo,
    apply_tdl_mimo,
    deterministic_doppler_frequencies,
    evolve_impulse_response_with_doppler,
    flat_fading,
    sample_impulse_response_with_doppler,
    tdl_impulse_response,
)
from .channel_estimation import (
    estimate_spatial_channel_from_fdm_pilots,
    interpolate_channel_dft_ls,
    interpolate_channel_linear,
    interpolate_channel_lmmse,
    ls_alamouti_channel_at_pilots,
)
from .config import SimulationConfig
from .crc import append_crc16, check_crc16
from .ldpc import (
    LDPC_K,
    LDPC_N,
    Ldpc5041008,
    LdpcMiniHeader,
    control_qpsk_labels,
    decode_control_llrs,
    deinterleave_blocks,
    interleave_blocks,
    marker_metric_from_llrs,
    modulation_flag,
    payload_blocks_field_for_len,
    scramble_bits,
    soft_descramble,
    transmit_rank_flag,
)
from .ldpc_frame import build_ldpc_mimo_frame_layout
from .link_adaptation import predict_2x2_detector_mse, recommend_2x2_rank_mcs
from .resource_grid import (
    build_resource_allocation,
    deterministic_pilot_pairs,
    deterministic_spatial_pilots,
)
from .pilot_tracking import correct_common_phase, phase_difference_to_cfo_hz
from .phase_reference import correct_differential_phase, map_phase_references
from .preamble import build_zc_preamble
from .sampling_offset import (
    apply_sampling_frequency_offset,
    inverse_sampling_offset_ppm,
)
from .sfbc import alamouti_sfbc_combine_grid, alamouti_sfbc_encode_grid
from .mimo import detect_spatial_multiplexing, spatial_multiplexing_encode_grid
from .stbc import alamouti_combine_grid, alamouti_encode_grid
from .synchronization import (
    build_impaired_stream,
    estimate_cp_synchronization,
    estimate_cfo_at_known_timing,
    estimate_zc_synchronization,
    extract_synchronized_symbols,
)


@dataclass(frozen=True)
class SimulationArtifacts:
    """Selected intermediate arrays for plots and Python/C++ golden vectors."""

    tx_symbols: np.ndarray
    equalized_symbols: np.ndarray
    pilot_symbols: np.ndarray
    equalized_pilots: np.ndarray
    data_indices: np.ndarray
    pilot_indices: np.ndarray
    pilot_tx_assignments: np.ndarray
    phase_reference_indices: np.ndarray
    equivalent_noise_variance: np.ndarray
    channel: np.ndarray
    channel_by_symbol: np.ndarray
    doppler_frequencies_hz: np.ndarray
    channel_impulse_response: np.ndarray
    preamble_frequency: np.ndarray
    tx_time: np.ndarray
    rx_time: np.ndarray
    rx_stream: np.ndarray
    synchronization_metric: np.ndarray
    estimated_timing_offsets: np.ndarray
    estimated_cfo_hz: np.ndarray
    pilot_common_phase_rad: np.ndarray
    pilot_phase_coherence: np.ndarray
    pilot_phase_applied: np.ndarray
    estimated_residual_cfo_hz: np.ndarray
    receiver_channel: np.ndarray
    pilot_channel_estimates: np.ndarray
    phase_reference_differential_phase_rad: np.ndarray
    phase_reference_slope_rad_per_subcarrier: np.ndarray
    estimated_sfo_ppm: np.ndarray
    phase_reference_coherence: np.ndarray
    phase_reference_applied: np.ndarray


def _build_frames(
    rng: np.random.Generator,
    frames: int,
    payload_bytes: int,
    frame_bits: int,
) -> tuple[np.ndarray, list[bytes]]:
    payloads = rng.integers(0, 256, size=(frames, payload_bytes), dtype=np.uint8)
    encoded = [append_crc16(row.tobytes()) for row in payloads]
    frame_bytes = np.frombuffer(b"".join(encoded), dtype=np.uint8).reshape(frames, payload_bytes + 2)
    bits = np.unpackbits(frame_bytes, axis=1, bitorder="big")
    if bits.shape[1] > frame_bits:
        raise ValueError("payload and CRC exceed available data resource elements")
    if bits.shape[1] < frame_bits:
        bits = np.pad(bits, ((0, 0), (0, frame_bits - bits.shape[1])))
    return bits, encoded


@overload
def simulate_alamouti_ofdm(
    config: SimulationConfig, *, return_artifacts: bool = False
) -> dict[str, Any]: ...


@overload
def simulate_alamouti_ofdm(
    config: SimulationConfig, *, return_artifacts: bool
) -> tuple[dict[str, Any], SimulationArtifacts]: ...


def simulate_alamouti_ofdm(
    config: SimulationConfig, *, return_artifacts: bool = False
) -> dict[str, Any] | tuple[dict[str, Any], SimulationArtifacts]:
    """Run a deterministic 2-Tx Alamouti CP-OFDM Monte-Carlo point."""

    config.validate()
    rng = np.random.default_rng(config.seed)
    channel_rng = (
        np.random.default_rng(config.channel_seed)
        if config.channel_seed is not None
        else rng
    )
    noise_rng = (
        np.random.default_rng(config.noise_seed)
        if config.noise_seed is not None
        else rng
    )
    allocation = build_resource_allocation(config)
    ldpc_enabled = config.fec_mode == "ldpc_1008_504"
    ldpc_layout = None
    ldpc_codec = None
    ldpc_tx_payload_bits = np.empty((config.frames, 0), dtype=np.uint8)
    ldpc_control_labels = np.empty((config.frames, 0), dtype=np.int64)
    ldpc_control_symbols = np.empty((config.frames, 0), dtype=np.complex128)
    fdm_pilot_mode = (
        config.mode == "spatial_multiplexing" or config.pairing == "frequency"
    )
    if ldpc_enabled:
        ldpc_layout = build_ldpc_mimo_frame_layout(config)
        ldpc_codec = Ldpc5041008(iterations=config.ldpc_iterations)
        payloads = rng.integers(
            0, 256, size=(config.frames, ldpc_layout.user_payload_bytes), dtype=np.uint8
        )
        information_frames = np.stack(
            [
                np.frombuffer(append_crc16(row.tobytes()), dtype=np.uint8)
                for row in payloads
            ]
        )
        tx_bits = np.unpackbits(information_frames, axis=1, bitorder="big")
        information_blocks = tx_bits.reshape(-1, LDPC_K)
        encoded_blocks = ldpc_codec.encode_bits(information_blocks)
        encoded_frames = encoded_blocks.reshape(config.frames, -1)
        ldpc_tx_payload_bits = interleave_blocks(scramble_bits(encoded_frames))
        payload_labels = qam.bits_to_labels(
            ldpc_tx_payload_bits, config.bits_per_symbol
        )
        payload_symbols = qam.modulate(payload_labels, config.bits_per_symbol)
        payload_layer_symbols = qam.modulate(
            np.zeros(
                (config.frames, ldpc_layout.payload_layer_symbol_capacity),
                dtype=np.int64,
            ),
            config.bits_per_symbol,
        )
        payload_layer_symbols[:, : ldpc_layout.coded_qam_symbol_count] = payload_symbols
        payload_layer_symbols = payload_layer_symbols.reshape(
            config.frames,
            ldpc_layout.payload_physical_re_count,
            config.ldpc_transmit_rank,
        )
        headers = [
            LdpcMiniHeader(
                version=1,
                flags=(
                    modulation_flag(config.bits_per_symbol)
                    | transmit_rank_flag(config.ldpc_transmit_rank)
                ),
                payload_len=ldpc_layout.information_bytes,
                payload_blocks=payload_blocks_field_for_len(
                    ldpc_layout.information_bytes
                ),
                seq=frame & 0xFFFF,
            )
            for frame in range(config.frames)
        ]
        ldpc_control_labels = np.stack(
            [control_qpsk_labels(header) for header in headers]
        )
        ldpc_control_symbols = qam.modulate(ldpc_control_labels, 2)
    else:
        tx_bits, _ = _build_frames(
            rng, config.frames, config.payload_bytes, config.frame_bits
        )
        labels = qam.bits_to_labels(tx_bits, config.bits_per_symbol)
        modulated_symbols = qam.modulate(labels, config.bits_per_symbol)
    if config.mode == "spatial_multiplexing":
        pilot_symbol_pairs = np.empty((config.frames, 0, 2), dtype=np.complex128)
        spatial_pilot_grid, spatial_pilot_assignments = deterministic_spatial_pilots(
            config.frames, allocation.pilot_centered, config.seed, config.nt
        )
        if ldpc_enabled:
            assert ldpc_layout is not None
            transmitted_symbols = payload_layer_symbols[:, None, :, :]
            tx_grid = np.zeros(
                (config.frames, 2, config.fft_size, config.nt),
                dtype=np.complex128,
            )
            for payload_index, (time_index, data_position) in enumerate(
                zip(
                    ldpc_layout.payload_time_indices,
                    ldpc_layout.payload_data_positions,
                )
            ):
                fft_index = allocation.data_indices[data_position]
                if config.ldpc_transmit_rank == 2:
                    tx_grid[:, time_index, fft_index, :] = (
                        payload_layer_symbols[:, payload_index, :] / np.sqrt(2.0)
                    )
                else:
                    tx_grid[:, time_index, fft_index, 0] = payload_layer_symbols[
                        :, payload_index, 0
                    ]
            control_fft_indices = allocation.data_indices[
                ldpc_layout.control_data_positions
            ]
            tx_grid[:, 0, control_fft_indices, 0] = ldpc_control_symbols
        else:
            transmitted_symbols = modulated_symbols.reshape(
                config.frames, 2, allocation.data_indices.size, config.layers
            )
            tx_grid = spatial_multiplexing_encode_grid(
                transmitted_symbols, allocation.data_indices, config.fft_size
            )
        tx_grid[:, :, allocation.pilot_indices, :] = spatial_pilot_grid
    elif config.pairing == "frequency":
        pilot_symbol_pairs = np.empty((config.frames, 0, 2), dtype=np.complex128)
        spatial_pilot_grid, spatial_pilot_assignments = deterministic_spatial_pilots(
            config.frames, allocation.pilot_centered, config.seed, config.nt
        )
        data_symbol_pairs = modulated_symbols.reshape(
            config.frames, allocation.data_indices.size, 2
        )
        transmitted_symbols = data_symbol_pairs
    else:
        pilot_symbol_pairs = deterministic_pilot_pairs(
            config.frames, allocation.pilot_centered, config.seed
        )
        spatial_pilot_grid = np.empty((config.frames, 2, 0, config.nt), dtype=np.complex128)
        spatial_pilot_assignments = np.empty(0, dtype=np.int64)
        data_symbol_pairs = modulated_symbols.reshape(
            config.frames, allocation.data_indices.size, 2
        )
        transmitted_symbols = data_symbol_pairs
    if config.mode != "spatial_multiplexing" and config.pairing == "time":
        symbol_pairs = np.zeros(
            (config.frames, config.fft_size, 2), dtype=np.complex128
        )
        symbol_pairs[:, allocation.data_indices, :] = data_symbol_pairs
        symbol_pairs[:, allocation.pilot_indices, :] = pilot_symbol_pairs
        tx_grid = alamouti_encode_grid(symbol_pairs)
    elif config.mode != "spatial_multiplexing":
        tx_grid = alamouti_sfbc_encode_grid(
            data_symbol_pairs, allocation.data_indices, config.fft_size
        )
        tx_grid[:, :, allocation.pilot_indices, :] = spatial_pilot_grid
    if config.phase_reference_tracking_enable:
        tx_grid = map_phase_references(tx_grid, allocation.phase_reference_indices)
    data_tx_time = ofdm.modulate(
        np.transpose(tx_grid, (0, 1, 3, 2)), config.cp_length
    )
    if config.preamble_enable:
        preamble_frequency, preamble_tx_time = build_zc_preamble(
            config.frames,
            config.nt,
            config.fft_size,
            config.cp_length,
            config.zc_root,
        )
        tx_time = np.concatenate((preamble_tx_time, data_tx_time), axis=1)
        preamble_symbol_count = 1
    else:
        preamble_frequency = np.empty(0, dtype=np.complex128)
        tx_time = data_tx_time
        preamble_symbol_count = 0
    noise_variance = 0.0 if np.isposinf(config.snr_db) else 10.0 ** (-config.snr_db / 10.0)
    if config.channel == "tdl":
        channel_impulse_response = tdl_impulse_response(
            config.frames, config.nr, config.nt, config.channel_taps
        )
    else:
        channel = flat_fading(
            config.frames,
            config.nr,
            config.nt,
            config.channel,
            channel_rng,
            tx_correlation=config.tx_correlation,
            rx_correlation=config.rx_correlation,
            spatial_rank=config.spatial_rank,
        )
        channel_impulse_response = channel[..., None]
    intrasymbol_channel_variation_nmse = 0.0
    maximum_intrasymbol_phase_rotation_deg = 0.0
    if config.doppler_hz > 0.0:
        if config.doppler_model == "continuous":
            doppler_frequencies_hz = deterministic_doppler_frequencies(
                channel_impulse_response, config.doppler_hz
            )
            rx_time = apply_continuous_doppler_mimo(
                tx_time,
                channel_impulse_response,
                doppler_frequencies_hz,
                config.sample_rate_hz,
                noise_variance,
                noise_rng,
            )
            samples_per_symbol = config.fft_size + config.cp_length
            symbol_starts = (
                np.arange(tx_time.shape[1], dtype=np.float64)
                * samples_per_symbol
            )
            reference_samples = (
                symbol_starts + config.cp_length + (config.fft_size - 1) / 2.0
            )
            time_varying_impulse_response = sample_impulse_response_with_doppler(
                channel_impulse_response,
                doppler_frequencies_hz,
                reference_samples,
                config.sample_rate_hz,
            )
            useful_start_response = sample_impulse_response_with_doppler(
                channel_impulse_response,
                doppler_frequencies_hz,
                symbol_starts + config.cp_length,
                config.sample_rate_hz,
            )
            useful_end_response = sample_impulse_response_with_doppler(
                channel_impulse_response,
                doppler_frequencies_hz,
                symbol_starts + config.cp_length + config.fft_size - 1,
                config.sample_rate_hz,
            )
            intrasymbol_channel_variation_nmse = float(
                np.mean(
                    np.abs(useful_end_response - useful_start_response) ** 2
                )
                / np.maximum(
                    np.mean(np.abs(useful_start_response) ** 2), 1.0e-30
                )
            )
            maximum_intrasymbol_phase_rotation_deg = float(
                360.0
                * np.max(np.abs(doppler_frequencies_hz))
                * (config.fft_size - 1)
                / config.sample_rate_hz
            )
        else:
            time_varying_impulse_response, doppler_frequencies_hz = (
                evolve_impulse_response_with_doppler(
                    channel_impulse_response,
                    tx_time.shape[1],
                    config.fft_size + config.cp_length,
                    config.sample_rate_hz,
                    config.doppler_hz,
                )
            )
            rx_time = apply_tdl_mimo(
                tx_time, time_varying_impulse_response, noise_variance, noise_rng
            )
        channel_grid_by_symbol = np.transpose(
            np.fft.fft(
                time_varying_impulse_response, n=config.fft_size, axis=-1
            ),
            (0, 1, 4, 2, 3),
        )
    else:
        doppler_frequencies_hz = np.zeros(
            channel_impulse_response.shape[1:], dtype=np.float64
        )
        if config.channel == "tdl":
            rx_time = apply_tdl_mimo(
                tx_time, channel_impulse_response, noise_variance, noise_rng
            )
            channel_grid = np.transpose(
                np.fft.fft(
                    channel_impulse_response, n=config.fft_size, axis=-1
                ),
                (0, 3, 1, 2),
            )
        else:
            rx_time = apply_flat_mimo(
                tx_time, channel, noise_variance, noise_rng
            )
            channel_grid = np.broadcast_to(
                channel[:, None, :, :],
                (config.frames, config.fft_size, config.nr, config.nt),
            )
        channel_grid_by_symbol = np.broadcast_to(
            channel_grid[:, None, :, :, :],
            (config.frames, tx_time.shape[1], config.fft_size, config.nr, config.nt),
        )
    data_channel_by_slot = channel_grid_by_symbol[
        :, preamble_symbol_count:, :, :, :
    ]
    channel_grid = data_channel_by_slot[:, 0]
    if config.slot_phase_offset_deg != 0.0:
        rx_time = rx_time.copy()
        second_data_symbol = preamble_symbol_count + 1
        rx_time[:, second_data_symbol, :, :] *= np.exp(
            1j * np.deg2rad(config.slot_phase_offset_deg)
        )
    true_cfo_normalized = config.cfo_hz / config.subcarrier_spacing_hz
    synchronization_active = (
        config.synchronization_enable
        or config.timing_offset_samples > 0
        or config.cfo_hz != 0.0
        or config.sfo_ppm != 0.0
    )
    if synchronization_active:
        rx_stream = build_impaired_stream(
            rx_time,
            fft_size=config.fft_size,
            timing_offset_samples=config.timing_offset_samples,
            cfo_normalized=true_cfo_normalized,
            search_padding_samples=config.timing_search_samples,
            noise_variance=noise_variance,
            rng=noise_rng,
        )
        rx_stream = apply_sampling_frequency_offset(rx_stream, config.sfo_ppm)
        if config.synchronization_enable:
            if config.preamble_enable:
                sync_estimate = estimate_zc_synchronization(
                    rx_stream,
                    preamble_tx_time[0, 0, 0],
                    fft_size=config.fft_size,
                    cp_length=config.cp_length,
                    ofdm_symbols=rx_time.shape[1],
                    max_search_samples=config.timing_search_samples,
                    correlation_skip_samples=channel_impulse_response.shape[-1] - 1,
                )
            else:
                sync_estimate = estimate_cp_synchronization(
                    rx_stream,
                    fft_size=config.fft_size,
                    cp_length=config.cp_length,
                    ofdm_symbols=rx_time.shape[1],
                    max_search_samples=config.timing_search_samples,
                    correlation_skip_samples=channel_impulse_response.shape[-1] - 1,
                )
            estimated_timing_offsets = sync_estimate.timing_offsets
            estimated_cfo_normalized = sync_estimate.cfo_normalized
            synchronization_metric = sync_estimate.metrics
            synchronization_peak_metric = sync_estimate.peak_metrics
        else:
            estimated_timing_offsets = np.zeros(config.frames, dtype=np.int64)
            estimated_cfo_normalized = np.zeros(config.frames, dtype=np.float64)
            synchronization_metric = np.zeros(
                (config.frames, config.timing_search_samples + 1), dtype=np.float64
            )
            synchronization_peak_metric = np.zeros(config.frames, dtype=np.float64)
        rx_time = extract_synchronized_symbols(
            rx_stream,
            timing_offsets=estimated_timing_offsets,
            cfo_normalized=estimated_cfo_normalized,
            fft_size=config.fft_size,
            cp_length=config.cp_length,
            ofdm_symbols=rx_time.shape[1],
        )
    else:
        rx_stream = np.transpose(rx_time, (0, 2, 1, 3)).reshape(
            config.frames, config.nr, -1
        )
        estimated_timing_offsets = np.zeros(config.frames, dtype=np.int64)
        estimated_cfo_normalized = np.zeros(config.frames, dtype=np.float64)
        synchronization_metric = np.zeros((config.frames, 1), dtype=np.float64)
        synchronization_peak_metric = np.zeros(config.frames, dtype=np.float64)

    estimated_cfo_hz = estimated_cfo_normalized * config.subcarrier_spacing_hz
    data_rx_time = rx_time[:, preamble_symbol_count:, :, :]
    rx_grid = ofdm.demodulate(data_rx_time, config.fft_size, config.cp_length)
    rx_grid = np.transpose(rx_grid, (0, 1, 3, 2))
    channel_variation = data_channel_by_slot[:, 1] - data_channel_by_slot[:, 0]
    stbc_channel_variation_nmse = float(
        np.mean(np.abs(channel_variation) ** 2)
        / np.maximum(np.mean(np.abs(data_channel_by_slot[:, 0]) ** 2), 1.0e-30)
    )
    interslot_channel_correlation = np.sum(
        np.conj(data_channel_by_slot[:, 0]) * data_channel_by_slot[:, 1],
        axis=(1, 2, 3),
    )
    interslot_channel_phase_deg = np.rad2deg(
        np.angle(interslot_channel_correlation)
    )
    if config.pairing == "frequency":
        pair_first = allocation.data_indices[0::2]
        pair_second = allocation.data_indices[1::2]
        frequency_variation = (
            data_channel_by_slot[:, :, pair_second, :, :]
            - data_channel_by_slot[:, :, pair_first, :, :]
        )
        frequency_reference = data_channel_by_slot[:, :, pair_first, :, :]
        alamouti_pair_channel_variation_nmse = float(
            np.mean(np.abs(frequency_variation) ** 2)
            / np.maximum(np.mean(np.abs(frequency_reference) ** 2), 1.0e-30)
        )
    else:
        alamouti_pair_channel_variation_nmse = stbc_channel_variation_nmse
    if config.phase_reference_tracking_enable:
        rx_grid, phase_reference_estimate = correct_differential_phase(
            rx_grid,
            allocation.phase_reference_indices,
            config.pilot_phase_min_coherence,
            phase_reference_centered_subcarriers=np.asarray(
                config.phase_reference_centered_subcarriers, dtype=np.float64
            ),
            slope_tracking=config.sfo_tracking_enable,
            samples_per_symbol=config.fft_size + config.cp_length,
        )
        phase_reference_differential_phase_rad = (
            phase_reference_estimate.differential_phase_rad
        )
        phase_reference_coherence = phase_reference_estimate.coherence
        phase_reference_applied = phase_reference_estimate.applied
        phase_reference_slope_rad_per_subcarrier = (
            phase_reference_estimate.phase_slope_rad_per_subcarrier
        )
        estimated_sfo_ppm = phase_reference_estimate.estimated_sfo_ppm
    else:
        phase_reference_differential_phase_rad = np.zeros(
            config.frames, dtype=np.float64
        )
        phase_reference_coherence = np.zeros(config.frames, dtype=np.float64)
        phase_reference_applied = np.zeros(config.frames, dtype=bool)
        phase_reference_slope_rad_per_subcarrier = np.zeros(
            config.frames, dtype=np.float64
        )
        estimated_sfo_ppm = np.zeros(config.frames, dtype=np.float64)
    residual_sfo_ppm = np.zeros(config.frames, dtype=np.float64)
    sfo_resampling_applied = np.zeros(config.frames, dtype=bool)
    if config.sfo_resampling_enable:
        sfo_resampling_applied = phase_reference_applied.copy()
        resampling_estimate_ppm = np.where(
            sfo_resampling_applied,
            estimated_sfo_ppm,
            0.0,
        )
        rx_stream = apply_sampling_frequency_offset(
            rx_stream,
            inverse_sampling_offset_ppm(resampling_estimate_ppm),
            method=config.sfo_resampling_interpolator,
        )
        estimated_cfo_normalized = estimate_cfo_at_known_timing(
            rx_stream,
            estimated_timing_offsets,
            fft_size=config.fft_size,
            cp_length=config.cp_length,
            ofdm_symbols=rx_time.shape[1],
            correlation_skip_samples=channel_impulse_response.shape[-1] - 1,
        )
        estimated_cfo_hz = (
            estimated_cfo_normalized * config.subcarrier_spacing_hz
        )
        rx_time = extract_synchronized_symbols(
            rx_stream,
            timing_offsets=estimated_timing_offsets,
            cfo_normalized=estimated_cfo_normalized,
            fft_size=config.fft_size,
            cp_length=config.cp_length,
            ofdm_symbols=rx_time.shape[1],
        )
        data_rx_time = rx_time[:, preamble_symbol_count:, :, :]
        rx_grid = ofdm.demodulate(
            data_rx_time, config.fft_size, config.cp_length
        )
        rx_grid = np.transpose(rx_grid, (0, 1, 3, 2))
        rx_grid, residual_phase_reference_estimate = correct_differential_phase(
            rx_grid,
            allocation.phase_reference_indices,
            config.pilot_phase_min_coherence,
            phase_reference_centered_subcarriers=np.asarray(
                config.phase_reference_centered_subcarriers, dtype=np.float64
            ),
            slope_tracking=True,
            samples_per_symbol=config.fft_size + config.cp_length,
        )
        residual_sfo_ppm = residual_phase_reference_estimate.estimated_sfo_ppm
    if config.pilot_phase_tracking_enable:
        rx_grid, pilot_phase_estimate = correct_common_phase(
            rx_grid,
            tx_grid,
            channel_grid,
            allocation.pilot_indices,
            config.pilot_phase_min_coherence,
        )
        pilot_common_phase_rad = pilot_phase_estimate.phase_rad
        pilot_phase_coherence = pilot_phase_estimate.coherence
        pilot_phase_applied = pilot_phase_estimate.applied
        estimated_residual_cfo_hz = phase_difference_to_cfo_hz(
            pilot_common_phase_rad,
            sample_rate_hz=config.sample_rate_hz,
            samples_per_symbol=config.fft_size + config.cp_length,
        )
    else:
        pilot_common_phase_rad = np.zeros((config.frames, 2), dtype=np.float64)
        pilot_phase_coherence = np.zeros((config.frames, 2), dtype=np.float64)
        pilot_phase_applied = np.zeros((config.frames, 2), dtype=bool)
        estimated_residual_cfo_hz = np.zeros(config.frames, dtype=np.float64)
    if fdm_pilot_mode and config.channel_estimation != "perfect":
        (
            receiver_channel_by_symbol,
            pilot_channel_estimates,
            spatial_pilot_assignments,
        ) = estimate_spatial_channel_from_fdm_pilots(
            rx_grid,
            spatial_pilot_grid,
            allocation.pilot_indices,
            allocation.pilot_centered,
            config.fft_size,
            config.channel_estimation,
            config.channel_estimation_taps,
            noise_variance,
        )
        receiver_channel_grid = receiver_channel_by_symbol[:, 0]
    elif not fdm_pilot_mode and config.channel_estimation != "perfect":
        pilot_channel_estimates = ls_alamouti_channel_at_pilots(
            rx_grid, pilot_symbol_pairs, allocation.pilot_indices
        )
        if config.channel_estimation == "ls_linear":
            receiver_channel_grid = interpolate_channel_linear(
                pilot_channel_estimates,
                allocation.pilot_centered,
                config.fft_size,
            )
        elif config.channel_estimation == "ls_dft":
            receiver_channel_grid = interpolate_channel_dft_ls(
                pilot_channel_estimates,
                allocation.pilot_centered,
                config.fft_size,
                config.channel_estimation_taps,
            )
        else:
            receiver_channel_grid = interpolate_channel_lmmse(
                pilot_channel_estimates,
                allocation.pilot_centered,
                config.fft_size,
                config.channel_estimation_taps,
                noise_variance,
            )
        receiver_channel_by_symbol = np.broadcast_to(
            receiver_channel_grid[:, None], data_channel_by_slot.shape
        )
    elif fdm_pilot_mode:
        receiver_channel_by_symbol = data_channel_by_slot.copy()
        receiver_channel_grid = receiver_channel_by_symbol[:, 0]
        pilot_channel_estimates = np.empty(
            (config.frames, 2, allocation.pilot_indices.size, config.nr),
            dtype=np.complex128,
        )
        for pilot, tx in enumerate(spatial_pilot_assignments):
            pilot_channel_estimates[:, :, pilot, :] = data_channel_by_slot[
                :, :, allocation.pilot_indices[pilot], :, tx
            ]
    else:
        receiver_channel_grid = channel_grid.copy()
        pilot_channel_estimates = channel_grid[:, allocation.pilot_indices, :, :].copy()
        receiver_channel_by_symbol = np.broadcast_to(
            receiver_channel_grid[:, None], data_channel_by_slot.shape
        )

    active_indices = np.concatenate(
        (
            allocation.data_indices,
            allocation.pilot_indices,
            allocation.phase_reference_indices,
        )
    )
    if fdm_pilot_mode:
        channel_error = (
            receiver_channel_by_symbol[:, :, active_indices, :, :]
            - data_channel_by_slot[:, :, active_indices, :, :]
        )
        channel_reference = data_channel_by_slot[:, :, active_indices, :, :]
    else:
        channel_error = receiver_channel_grid[:, active_indices, :, :] - channel_grid[
            :, active_indices, :, :
        ]
        channel_reference = channel_grid[:, active_indices, :, :]
    channel_estimation_nmse = float(
        np.mean(np.abs(channel_error) ** 2)
        / np.maximum(
            np.mean(np.abs(channel_reference) ** 2), 1.0e-30
        )
    )
    if fdm_pilot_mode:
        true_pilot_channel = np.empty_like(pilot_channel_estimates)
        for pilot, tx in enumerate(spatial_pilot_assignments):
            true_pilot_channel[:, :, pilot, :] = data_channel_by_slot[
                :, :, allocation.pilot_indices[pilot], :, tx
            ]
    else:
        true_pilot_channel = channel_grid[:, allocation.pilot_indices, :, :]
    pilot_channel_error = pilot_channel_estimates - true_pilot_channel
    pilot_channel_nmse = (
        float(
            np.mean(np.abs(pilot_channel_error) ** 2)
            / np.maximum(
                np.mean(
                    np.abs(true_pilot_channel) ** 2
                ),
                1.0e-30,
            )
        )
        if allocation.pilot_indices.size
        else 0.0
    )
    control_header_failures = 0
    control_marker_metrics = np.empty(0, dtype=np.float64)
    control_evm_rms = 0.0
    pre_ldpc_ber = 0.0
    ldpc_syndrome_failure_rate = 0.0
    ldpc_decoder_iterations_used = 0
    gross_air_bits = config.frame_bits
    if ldpc_enabled:
        assert ldpc_layout is not None and ldpc_codec is not None
        control_fft_indices = allocation.data_indices[
            ldpc_layout.control_data_positions
        ]
        control_received = rx_grid[:, 0][:, control_fft_indices, :]
        control_channel = receiver_channel_by_symbol[:, 0][
            :, control_fft_indices, :, :
        ][..., 0]
        control_gain = np.maximum(
            np.sum(np.abs(control_channel) ** 2, axis=-1), 1.0e-15
        )
        equalized_control = np.sum(
            np.conj(control_channel) * control_received, axis=-1
        ) / control_gain
        control_variance = noise_variance / control_gain
        control_llrs = qam.max_log_llrs(
            equalized_control, control_variance, 2
        ).reshape(config.frames, -1)
        control_marker_metrics = np.asarray(
            [marker_metric_from_llrs(values[:128]) for values in control_llrs],
            dtype=np.float64,
        )
        header_valid = np.ones(config.frames, dtype=bool)
        expected_flag = (
            modulation_flag(config.bits_per_symbol)
            | transmit_rank_flag(config.ldpc_transmit_rank)
        )
        for frame, values in enumerate(control_llrs):
            try:
                header, _ = decode_control_llrs(values)
                if (
                    header.version != 1
                    or header.flags != expected_flag
                    or header.payload_len != ldpc_layout.information_bytes
                    or header.payload_blocks != ldpc_layout.payload_blocks
                    or header.seq != (frame & 0xFFFF)
                ):
                    header_valid[frame] = False
            except ValueError:
                header_valid[frame] = False
        control_header_failures = int(np.count_nonzero(~header_valid))
        control_evm_rms = float(
            np.sqrt(
                np.mean(np.abs(equalized_control - ldpc_control_symbols) ** 2)
                / np.mean(np.abs(ldpc_control_symbols) ** 2)
            )
        )

        payload_received = np.empty(
            (
                config.frames,
                1,
                ldpc_layout.payload_physical_re_count,
                config.nr,
            ),
            dtype=np.complex128,
        )
        payload_channel = np.empty(
            (
                config.frames,
                1,
                ldpc_layout.payload_physical_re_count,
                config.nr,
                config.layers,
            ),
            dtype=np.complex128,
        )
        for time_index in range(2):
            selected = ldpc_layout.payload_time_indices == time_index
            fft_indices = allocation.data_indices[
                ldpc_layout.payload_data_positions[selected]
            ]
            payload_received[:, 0, selected, :] = rx_grid[:, time_index][
                :, fft_indices, :
            ]
            payload_channel[:, 0, selected, :, :] = receiver_channel_by_symbol[
                :, time_index
            ][:, fft_indices, :, :]
        if config.ldpc_transmit_rank == 2:
            equalized, equivalent_variance = detect_spatial_multiplexing(
                payload_received,
                payload_channel,
                noise_variance,
                config.detector,
            )
        else:
            rank1_channel = payload_channel[..., 0]
            rank1_gain = np.maximum(
                np.sum(np.abs(rank1_channel) ** 2, axis=-1), 1.0e-15
            )
            equalized = (
                np.sum(np.conj(rank1_channel) * payload_received, axis=-1)
                / rank1_gain
            )[..., None]
            equivalent_variance = (noise_variance / rank1_gain)[..., None]
        equalized_pilots = np.empty(
            (config.frames, 0, config.layers), dtype=np.complex128
        )
    elif config.mode == "spatial_multiplexing":
        equalized, equivalent_variance = detect_spatial_multiplexing(
            rx_grid[:, :, allocation.data_indices, :],
            receiver_channel_by_symbol[:, :, allocation.data_indices, :, :],
            noise_variance,
            config.detector,
        )
        equalized_pilots = np.empty((config.frames, 0, config.layers), dtype=np.complex128)
    elif config.pairing == "time":
        equalized_grid, equivalent_variance_grid = alamouti_combine_grid(
            rx_grid, receiver_channel_grid, noise_variance
        )
        equalized = equalized_grid[:, allocation.data_indices, :]
        equivalent_variance = equivalent_variance_grid[:, allocation.data_indices]
        equalized_pilots = equalized_grid[:, allocation.pilot_indices, :]
    else:
        equalized, equivalent_variance = alamouti_sfbc_combine_grid(
            rx_grid,
            receiver_channel_by_symbol,
            allocation.data_indices,
            noise_variance,
        )
        equalized_pilots = np.empty((config.frames, 0, 2), dtype=np.complex128)

    decoded_labels = qam.hard_demodulate(equalized, config.bits_per_symbol).reshape(
        config.frames, -1
    )
    if ldpc_enabled:
        assert ldpc_layout is not None and ldpc_codec is not None
        coded_count = ldpc_layout.coded_qam_symbol_count
        coded_equalized = equalized.reshape(config.frames, -1)[:, :coded_count]
        coded_variance = equivalent_variance.reshape(config.frames, -1)[
            :, :coded_count
        ]
        payload_llrs = qam.max_log_llrs(
            coded_equalized, coded_variance, config.bits_per_symbol
        ).reshape(config.frames, -1)
        hard_payload_bits = (payload_llrs < 0.0).astype(np.uint8)
        pre_errors = hard_payload_bits != ldpc_tx_payload_bits
        pre_ldpc_ber = float(np.mean(pre_errors))
        decoder_llrs = soft_descramble(deinterleave_blocks(payload_llrs))
        decode_result = ldpc_codec.decode(decoder_llrs.reshape(-1, LDPC_N))
        rx_bits = decode_result.information_bits.reshape(config.frames, -1)
        errors = tx_bits != rx_bits
        bit_errors = int(np.count_nonzero(errors))
        block_errors = int(np.count_nonzero(np.any(errors, axis=1)))
        syndrome_failures = decode_result.syndrome_weights.reshape(
            config.frames, ldpc_layout.payload_blocks
        ) > 0
        ldpc_syndrome_failure_rate = float(np.mean(syndrome_failures))
        ldpc_decoder_iterations_used = int(decode_result.iterations)
        rx_bytes = np.packbits(rx_bits, axis=1, bitorder="big")
        crc_failed = np.asarray(
            [not check_crc16(row.tobytes()) for row in rx_bytes], dtype=bool
        ) | ~header_valid
        crc_failures = int(np.count_nonzero(crc_failed))
        gross_air_bits = (
            config.ldpc_control_re_count * 2
            + ldpc_layout.payload_layer_symbol_capacity * config.bits_per_symbol
        )
    else:
        rx_bits = qam.labels_to_bits(decoded_labels, config.bits_per_symbol).reshape(
            config.frames, -1
        )
        errors = tx_bits != rx_bits
        bit_errors = int(np.count_nonzero(errors))
        block_errors = int(np.count_nonzero(np.any(errors, axis=1)))
        crc_bits = (config.payload_bytes + 2) * 8
        rx_bytes = np.packbits(rx_bits[:, :crc_bits], axis=1, bitorder="big")
        crc_failures = sum(not check_crc16(row.tobytes()) for row in rx_bytes)
    evm_rms = float(
        np.sqrt(
            np.mean(np.abs(equalized - transmitted_symbols) ** 2)
            / np.mean(np.abs(transmitted_symbols) ** 2)
        )
    )
    pilot_evm_rms = (
        float(
            np.sqrt(
                np.mean(np.abs(equalized_pilots - pilot_symbol_pairs) ** 2)
                / np.mean(np.abs(pilot_symbol_pairs) ** 2)
            )
        )
        if pilot_symbol_pairs.size
        else 0.0
    )

    total_bits = int(tx_bits.size)
    if ldpc_enabled:
        full_tx_labels = qam.hard_demodulate(
            transmitted_symbols, config.bits_per_symbol
        ).reshape(config.frames, -1)
        full_tx_bits = qam.labels_to_bits(
            full_tx_labels, config.bits_per_symbol
        ).reshape(config.frames, -1)
        full_rx_bits = qam.labels_to_bits(
            decoded_labels, config.bits_per_symbol
        ).reshape(config.frames, -1)
        structured_errors = (full_tx_bits != full_rx_bits).reshape(
            config.frames,
            1,
            ldpc_layout.payload_physical_re_count,
            config.ldpc_transmit_rank,
            config.bits_per_symbol,
        )
        layer_bit_errors = np.count_nonzero(structured_errors, axis=(0, 1, 2, 4))
        bits_per_layer = (
            structured_errors.shape[0]
            * structured_errors.shape[1]
            * structured_errors.shape[2]
            * structured_errors.shape[4]
        )
        layer_ber = (layer_bit_errors / bits_per_layer).tolist()
        layer_evm_rms = np.sqrt(
            np.mean(np.abs(equalized - transmitted_symbols) ** 2, axis=(0, 1, 2))
            / np.mean(np.abs(transmitted_symbols) ** 2, axis=(0, 1, 2))
        ).tolist()
    elif config.mode == "spatial_multiplexing":
        structured_errors = errors.reshape(
            config.frames,
            2,
            allocation.data_indices.size,
            config.layers,
            config.bits_per_symbol,
        )
        layer_bit_errors = np.count_nonzero(structured_errors, axis=(0, 1, 2, 4))
        bits_per_layer = structured_errors.shape[0] * structured_errors.shape[1] * structured_errors.shape[2] * structured_errors.shape[4]
        layer_ber = (layer_bit_errors / bits_per_layer).tolist()
        layer_evm_rms = np.sqrt(
            np.mean(np.abs(equalized - transmitted_symbols) ** 2, axis=(0, 1, 2))
            / np.mean(np.abs(transmitted_symbols) ** 2, axis=(0, 1, 2))
        ).tolist()
    else:
        layer_ber = [bit_errors / total_bits]
        layer_evm_rms = [evm_rms]
    recommended_rank2_rate = 0.0
    recommended_rank1_rate = 0.0
    link_adaptation_available = False
    link_adaptation_outage_rate = 0.0
    configured_mcs_support_rate = 0.0
    mean_rank2_bottleneck_sinr_db = 0.0
    mean_rank1_sinr_db = 0.0
    mean_minimum_eigenvalue_ratio = 0.0
    recommended_mcs_counts = {
        "qpsk": 0,
        "16qam": 0,
        "64qam": 0,
        "256qam": 0,
    }
    if (
        config.mode == "spatial_multiplexing"
        and config.layers == 2
        and config.nr == 2
    ):
        link_adaptation_available = True
        adaptation_channel = (
            payload_channel
            if ldpc_enabled
            else receiver_channel_by_symbol[
                :, :, allocation.data_indices, :, :
            ]
        )
        adaptation_mse = (
            equivalent_variance
            if not ldpc_enabled or config.ldpc_transmit_rank == 2
            else predict_2x2_detector_mse(
                adaptation_channel, noise_variance, config.detector
            )
        )
        adaptation = recommend_2x2_rank_mcs(
            adaptation_channel,
            adaptation_mse,
            noise_variance,
            config.detector,
            config.modulation,
        )
        recommended_rank2_rate = float(
            np.mean(adaptation.recommended_rank == 2)
        )
        recommended_rank1_rate = 1.0 - recommended_rank2_rate
        link_adaptation_outage_rate = float(np.mean(adaptation.outage))
        configured_mcs_support_rate = float(
            np.mean(adaptation.configured_mcs_supported)
        )
        mean_rank2_bottleneck_sinr_db = float(
            np.mean(adaptation.rank2_bottleneck_sinr_db)
        )
        mean_rank1_sinr_db = float(np.mean(adaptation.rank1_sinr_db))
        mean_minimum_eigenvalue_ratio = float(
            np.mean(adaptation.minimum_eigenvalue_ratio)
        )
        recommended_mcs_counts = {
            modulation: int(
                np.count_nonzero(adaptation.recommended_modulation == modulation)
            )
            for modulation in recommended_mcs_counts
        }
    data_channels = data_channel_by_slot[:, :, allocation.data_indices, :, :]
    singular_values = np.linalg.svd(data_channels, compute_uv=False)
    channel_rank = np.sum(
        singular_values > singular_values[..., :1] * 1.0e-10, axis=-1
    )
    channel_condition = singular_values[..., 0] / np.maximum(
        singular_values[..., -1], 1.0e-15
    )
    frame_duration_s = (
        tx_time.shape[1]
        * (config.fft_size + config.cp_length)
        / config.sample_rate_hz
    )
    sfo_interpolator_taps = {
        "cubic": 4,
        "sinc8": 8,
        "sinc24": 24,
    }[config.sfo_resampling_interpolator]
    resampler_tap_mac_per_frame = (
        float(
            rx_stream.shape[-1]
            * config.nr
            * sfo_interpolator_taps
            * np.mean(sfo_resampling_applied)
        )
        if config.sfo_resampling_enable
        else 0.0
    )
    result = {
        **config.to_dict(),
        "mode": (
            "spatial_multiplexing"
            if config.mode == "spatial_multiplexing"
            else "stbc"
            if config.pairing == "time"
            else "sfbc"
        ),
        "scheme": "linear_mimo" if config.mode == "spatial_multiplexing" else "alamouti",
        "nt": config.nt,
        "layers": config.layers,
        "transmit_rank": (
            config.ldpc_transmit_rank if ldpc_enabled else config.layers
        ),
        "detector": config.detector if config.mode == "spatial_multiplexing" else "alamouti",
        "pairing": config.pairing,
        "snr_definition": "total radiated power per active RE / N0; total Tx power normalized to 1",
        "noise_variance": noise_variance,
        "channel_path_count": len(config.channel_taps) if config.channel == "tdl" else 1,
        "channel_impulse_length": int(channel_impulse_response.shape[-1]),
        "channel_max_delay_samples": int(channel_impulse_response.shape[-1] - 1),
        "maximum_doppler_hz": config.doppler_hz,
        "intrasymbol_channel_variation_nmse": intrasymbol_channel_variation_nmse,
        "maximum_intrasymbol_phase_rotation_deg": (
            maximum_intrasymbol_phase_rotation_deg
        ),
        "rms_active_path_doppler_hz": float(
            np.sqrt(
                np.mean(
                    doppler_frequencies_hz[np.abs(doppler_frequencies_hz) > 0.0]
                    ** 2
                )
            )
            if np.any(np.abs(doppler_frequencies_hz) > 0.0)
            else 0.0
        ),
        "stbc_channel_variation_nmse": stbc_channel_variation_nmse,
        "alamouti_pair_channel_variation_nmse": alamouti_pair_channel_variation_nmse,
        "mean_interslot_channel_phase_deg": float(
            np.rad2deg(
                np.angle(
                    np.mean(np.exp(1j * np.deg2rad(interslot_channel_phase_deg)))
                )
            )
        ),
        "preamble_enabled": config.preamble_enable,
        "preamble_symbols": preamble_symbol_count,
        "zc_root": config.zc_root,
        "frame_ofdm_symbols": int(tx_time.shape[1]),
        "samples_per_ofdm_symbol": config.fft_size + config.cp_length,
        "samples_per_frame": int(tx_time.shape[1] * tx_time.shape[-1]),
        "active_subcarriers": len(config.active_centered_subcarriers),
        "data_subcarriers": int(allocation.data_indices.size),
        "pilot_subcarriers": int(allocation.pilot_indices.size),
        "phase_reference_subcarriers": int(
            allocation.phase_reference_indices.size
        ),
        "null_subcarriers": int(allocation.null_indices.size),
        "padding_bits": config.padding_bits,
        "fec_mode": config.fec_mode,
        "ldpc_payload_blocks": (
            ldpc_layout.payload_blocks if ldpc_layout is not None else 0
        ),
        "ldpc_information_bytes": (
            ldpc_layout.information_bytes if ldpc_layout is not None else 0
        ),
        "ldpc_user_payload_bytes": (
            ldpc_layout.user_payload_bytes if ldpc_layout is not None else 0
        ),
        "ldpc_control_re_count": (
            config.ldpc_control_re_count if ldpc_enabled else 0
        ),
        "ldpc_coded_bits": int(ldpc_tx_payload_bits.shape[-1]),
        "ldpc_padding_qam_symbols": (
            ldpc_layout.padding_qam_symbol_count if ldpc_layout is not None else 0
        ),
        "control_header_failures": control_header_failures,
        "control_header_failure_rate": control_header_failures / config.frames,
        "mean_control_marker_metric": (
            float(np.mean(control_marker_metrics))
            if control_marker_metrics.size
            else 0.0
        ),
        "control_evm_rms": control_evm_rms,
        "pre_ldpc_ber": pre_ldpc_ber,
        "post_ldpc_ber": bit_errors / total_bits,
        "ldpc_syndrome_failure_rate": ldpc_syndrome_failure_rate,
        "ldpc_decoder_iterations_used": ldpc_decoder_iterations_used,
        "total_bits": total_bits,
        "bit_errors": bit_errors,
        "ber": bit_errors / total_bits,
        "block_errors": block_errors,
        "bler": block_errors / config.frames,
        "crc_failures": crc_failures,
        "crc_failure_rate": crc_failures / config.frames,
        "evm_rms": evm_rms,
        "layer_ber": layer_ber,
        "layer_evm_rms": layer_evm_rms,
        "mean_channel_rank": float(np.mean(channel_rank)),
        "rank_deficient_rate": float(np.mean(channel_rank < config.layers)),
        "mean_channel_condition_number": float(np.mean(channel_condition)),
        "mean_channel_singular_values": np.mean(
            singular_values, axis=tuple(range(singular_values.ndim - 1))
        ).tolist(),
        "mean_minimum_channel_singular_value": float(
            np.mean(singular_values[..., -1])
        ),
        "link_adaptation_available": link_adaptation_available,
        "recommended_rank2_rate": recommended_rank2_rate,
        "recommended_rank1_rate": recommended_rank1_rate,
        "link_adaptation_outage_rate": link_adaptation_outage_rate,
        "configured_mcs_support_rate": configured_mcs_support_rate,
        "mean_rank2_bottleneck_sinr_db": mean_rank2_bottleneck_sinr_db,
        "mean_rank1_sinr_db": mean_rank1_sinr_db,
        "mean_minimum_eigenvalue_ratio": mean_minimum_eigenvalue_ratio,
        "recommended_mcs_counts": recommended_mcs_counts,
        "frame_duration_s": frame_duration_s,
        "realtime_frame_rate_hz": 1.0 / frame_duration_s,
        "sfo_interpolator_taps": sfo_interpolator_taps,
        "resampler_tap_mac_per_frame": resampler_tap_mac_per_frame,
        "resampler_tap_mac_per_second": (
            resampler_tap_mac_per_frame / frame_duration_s
        ),
        "gross_phy_rate_bps": gross_air_bits / frame_duration_s,
        "net_payload_rate_bps": config.payload_bytes * 8 / frame_duration_s,
        "goodput_bps": (
            config.payload_bytes
            * 8
            / frame_duration_s
            * (1.0 - crc_failures / config.frames)
        ),
        "gross_spectral_efficiency_bps_hz": (
            gross_air_bits
            / frame_duration_s
            / (len(config.active_centered_subcarriers) * config.subcarrier_spacing_hz)
        ),
        "pilot_evm_rms": pilot_evm_rms,
        "mean_equivalent_noise_variance": float(np.mean(equivalent_variance)),
        "synchronization_enabled": config.synchronization_enable,
        "synchronization_method": (
            "zc_matched_filter_cp_cfo"
            if config.synchronization_enable and config.preamble_enable
            else "cp"
            if config.synchronization_enable
            else "disabled"
        ),
        "true_timing_offset_samples": config.timing_offset_samples,
        "estimated_timing_offset_mean": float(np.mean(estimated_timing_offsets)),
        "timing_success_rate": float(
            np.mean(estimated_timing_offsets == config.timing_offset_samples)
        ),
        "true_cfo_hz": config.cfo_hz,
        "estimated_cfo_hz_mean": float(np.mean(estimated_cfo_hz)),
        "mean_absolute_cfo_error_hz": float(
            np.mean(np.abs(estimated_cfo_hz - config.cfo_hz))
        ),
        "sfo_tracking_enabled": config.sfo_tracking_enable,
        "sfo_resampling_enabled": config.sfo_resampling_enable,
        "sfo_resampling_application_rate": float(
            np.mean(sfo_resampling_applied)
        ),
        "true_sfo_ppm": config.sfo_ppm,
        "estimated_sfo_ppm_mean": float(np.mean(estimated_sfo_ppm)),
        "mean_absolute_sfo_error_ppm": float(
            np.mean(np.abs(estimated_sfo_ppm - config.sfo_ppm))
        ),
        "estimated_residual_sfo_ppm_mean": float(np.mean(residual_sfo_ppm)),
        "mean_absolute_residual_sfo_ppm": float(
            np.mean(np.abs(residual_sfo_ppm))
        ),
        "mean_phase_slope_rad_per_subcarrier": float(
            np.mean(phase_reference_slope_rad_per_subcarrier)
        ),
        "mean_synchronization_peak_metric": float(
            np.mean(synchronization_peak_metric)
        ),
        "pilot_phase_tracking_enabled": config.pilot_phase_tracking_enable,
        "mean_pilot_phase_coherence": float(np.mean(pilot_phase_coherence)),
        "pilot_phase_application_rate": float(np.mean(pilot_phase_applied)),
        "mean_pilot_common_phase_deg_slot0": float(
            np.rad2deg(np.angle(np.mean(np.exp(1j * pilot_common_phase_rad[:, 0]))))
        ),
        "mean_pilot_common_phase_deg_slot1": float(
            np.rad2deg(np.angle(np.mean(np.exp(1j * pilot_common_phase_rad[:, 1]))))
        ),
        "estimated_residual_cfo_hz_mean": float(
            np.mean(estimated_residual_cfo_hz)
        ),
        "mean_absolute_estimated_residual_cfo_hz": float(
            np.mean(np.abs(estimated_residual_cfo_hz))
        ),
        "phase_reference_tracking_enabled": config.phase_reference_tracking_enable,
        "true_slot_phase_offset_deg": config.slot_phase_offset_deg,
        "estimated_differential_phase_deg_mean": float(
            np.rad2deg(
                np.angle(
                    np.mean(
                        np.exp(1j * phase_reference_differential_phase_rad)
                    )
                )
            )
        ),
        "mean_phase_reference_coherence": float(
            np.mean(phase_reference_coherence)
        ),
        "phase_reference_application_rate": float(
            np.mean(phase_reference_applied)
        ),
        "channel_estimation_mode": config.channel_estimation,
        "channel_estimation_taps": config.channel_estimation_taps,
        "channel_estimation_nmse": channel_estimation_nmse,
        "pilot_channel_nmse": pilot_channel_nmse,
    }
    if not return_artifacts:
        return result
    artifacts = SimulationArtifacts(
        tx_symbols=transmitted_symbols,
        equalized_symbols=equalized,
        pilot_symbols=(
            spatial_pilot_grid
            if fdm_pilot_mode
            else pilot_symbol_pairs
        ),
        equalized_pilots=equalized_pilots,
        data_indices=allocation.data_indices,
        pilot_indices=allocation.pilot_indices,
        pilot_tx_assignments=spatial_pilot_assignments,
        phase_reference_indices=allocation.phase_reference_indices,
        equivalent_noise_variance=equivalent_variance,
        channel=channel_grid,
        channel_by_symbol=data_channel_by_slot,
        doppler_frequencies_hz=doppler_frequencies_hz,
        channel_impulse_response=channel_impulse_response,
        preamble_frequency=preamble_frequency,
        tx_time=tx_time,
        rx_time=rx_time,
        rx_stream=rx_stream,
        synchronization_metric=synchronization_metric,
        estimated_timing_offsets=estimated_timing_offsets,
        estimated_cfo_hz=estimated_cfo_hz,
        pilot_common_phase_rad=pilot_common_phase_rad,
        pilot_phase_coherence=pilot_phase_coherence,
        pilot_phase_applied=pilot_phase_applied,
        estimated_residual_cfo_hz=estimated_residual_cfo_hz,
        receiver_channel=receiver_channel_grid,
        pilot_channel_estimates=pilot_channel_estimates,
        phase_reference_differential_phase_rad=(
            phase_reference_differential_phase_rad
        ),
        phase_reference_slope_rad_per_subcarrier=(
            phase_reference_slope_rad_per_subcarrier
        ),
        estimated_sfo_ppm=estimated_sfo_ppm,
        phase_reference_coherence=phase_reference_coherence,
        phase_reference_applied=phase_reference_applied,
    )
    return result, artifacts


# New generic name; keep simulate_alamouti_ofdm for backward compatibility
# with the existing STBC/SFBC scripts and golden-vector tests.
simulate_mimo_ofdm = simulate_alamouti_ofdm
