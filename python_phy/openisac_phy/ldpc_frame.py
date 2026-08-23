"""Resource layout helpers for the 2x2 mixed-control LDPC frame."""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from .config import SimulationConfig


@dataclass(frozen=True)
class LdpcMimoFrameLayout:
    control_data_positions: np.ndarray
    payload_time_indices: np.ndarray
    payload_data_positions: np.ndarray
    payload_physical_re_count: int
    payload_layer_symbol_capacity: int
    payload_blocks: int
    coded_qam_symbol_count: int
    padding_qam_symbol_count: int
    information_bytes: int
    user_payload_bytes: int


def build_ldpc_mimo_frame_layout(config: SimulationConfig) -> LdpcMimoFrameLayout:
    config.validate()
    if config.fec_mode != "ldpc_1008_504":
        raise ValueError("LDPC frame layout requires fec_mode=ldpc_1008_504")

    data_count = config.data_subcarrier_count
    control_count = config.ldpc_control_re_count
    control_positions = np.rint(
        np.linspace(0, data_count - 1, control_count)
    ).astype(np.int64)
    if np.unique(control_positions).size != control_count:
        raise ValueError("control RE selection produced duplicate positions")

    control_set = set(control_positions.tolist())
    payload_time: list[int] = []
    payload_data: list[int] = []
    for time_index in range(2):
        for data_position in range(data_count):
            if time_index == 0 and data_position in control_set:
                continue
            payload_time.append(time_index)
            payload_data.append(data_position)

    payload_physical = len(payload_time)
    layer_capacity = payload_physical * config.ldpc_transmit_rank
    symbols_per_block = 1008 // config.bits_per_symbol
    payload_blocks = layer_capacity // symbols_per_block
    coded_symbols = payload_blocks * symbols_per_block
    information_bytes = payload_blocks * 63
    return LdpcMimoFrameLayout(
        control_data_positions=control_positions,
        payload_time_indices=np.asarray(payload_time, dtype=np.int64),
        payload_data_positions=np.asarray(payload_data, dtype=np.int64),
        payload_physical_re_count=payload_physical,
        payload_layer_symbol_capacity=layer_capacity,
        payload_blocks=payload_blocks,
        coded_qam_symbol_count=coded_symbols,
        padding_qam_symbol_count=layer_capacity - coded_symbols,
        information_bytes=information_bytes,
        user_payload_bytes=information_bytes - 2,
    )
