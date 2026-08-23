#!/usr/bin/env python3
"""Export deterministic formal-frame vectors for Windows/Linux C++ tests."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from dataclasses import replace
from pathlib import Path

import numpy as np
import yaml

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from openisac_phy import ofdm, qam  # noqa: E402
from openisac_phy.config import SimulationConfig  # noqa: E402
from openisac_phy.crc import append_crc16  # noqa: E402
from openisac_phy.ldpc import (  # noqa: E402
    LDPC_K,
    Ldpc5041008,
    LdpcMiniHeader,
    control_qpsk_labels,
    interleave_blocks,
    modulation_flag,
    payload_blocks_field_for_len,
    scramble_bits,
    transmit_rank_flag,
)
from openisac_phy.ldpc_frame import build_ldpc_mimo_frame_layout  # noqa: E402
from openisac_phy.phase_reference import map_phase_references  # noqa: E402
from openisac_phy.preamble import build_zc_preamble  # noqa: E402
from openisac_phy.resource_grid import (  # noqa: E402
    build_resource_allocation,
    deterministic_spatial_pilots,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--config",
        type=Path,
        default=ROOT
        / "configs"
        / "mimo_2x2_spatial_multiplexing_ldpc_realtime_1024.yaml",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=ROOT.parent / "measurement" / "cpp_frame_vectors",
    )
    parser.add_argument("--sequence", type=lambda value: int(value, 0), default=0x1234)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    values = yaml.safe_load(args.config.read_text(encoding="utf-8")) or {}
    config = replace(SimulationConfig.from_mapping(values), frames=1)
    layout = build_ldpc_mimo_frame_layout(config)
    allocation = build_resource_allocation(config)
    codec = Ldpc5041008(iterations=config.ldpc_iterations)

    user_payload = bytes(
        ((index * 37 + 11) & 0xFF) for index in range(layout.user_payload_bytes)
    )
    information = append_crc16(user_payload)
    information_bits = np.unpackbits(
        np.frombuffer(information, dtype=np.uint8), bitorder="big"
    )
    codewords = codec.encode_bits(information_bits.reshape(-1, LDPC_K)).reshape(-1)
    transmitted_bits = interleave_blocks(scramble_bits(codewords))
    payload_labels = qam.bits_to_labels(
        transmitted_bits, config.bits_per_symbol
    ).reshape(-1)
    full_payload_labels = np.zeros(
        layout.payload_layer_symbol_capacity, dtype=np.uint8
    )
    full_payload_labels[: layout.coded_qam_symbol_count] = payload_labels
    payload_symbols = qam.modulate(
        full_payload_labels, config.bits_per_symbol
    ).reshape(layout.payload_physical_re_count, config.ldpc_transmit_rank)

    header = LdpcMiniHeader(
        version=1,
        flags=(
            modulation_flag(config.bits_per_symbol)
            | transmit_rank_flag(config.ldpc_transmit_rank)
        ),
        payload_len=layout.information_bytes,
        payload_blocks=payload_blocks_field_for_len(layout.information_bytes),
        seq=args.sequence & 0xFFFF,
    )
    control_labels = control_qpsk_labels(header).astype(np.uint8)
    control_symbols = qam.modulate(control_labels, 2)

    tx_grid = np.zeros((1, 2, config.fft_size, 2), dtype=np.complex128)
    for payload_index, (time_index, data_position) in enumerate(
        zip(layout.payload_time_indices, layout.payload_data_positions)
    ):
        fft_index = allocation.data_indices[data_position]
        if config.ldpc_transmit_rank == 2:
            tx_grid[0, time_index, fft_index, :] = (
                payload_symbols[payload_index] / np.sqrt(2.0)
            )
        else:
            tx_grid[0, time_index, fft_index, 0] = payload_symbols[
                payload_index, 0
            ]
    control_fft_indices = allocation.data_indices[layout.control_data_positions]
    tx_grid[0, 0, control_fft_indices, 0] = control_symbols
    pilot_grid, _ = deterministic_spatial_pilots(
        1, allocation.pilot_centered, config.seed, config.nt
    )
    tx_grid[:, :, allocation.pilot_indices, :] = pilot_grid
    tx_grid = map_phase_references(tx_grid, allocation.phase_reference_indices)
    data_time = ofdm.modulate(np.transpose(tx_grid, (0, 1, 3, 2)), config.cp_length)
    _, preamble_time = build_zc_preamble(
        1, config.nt, config.fft_size, config.cp_length, config.zc_root
    )
    tx_time = np.concatenate((preamble_time, data_time), axis=1)[0]

    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    arrays = {
        "data_fft_indices_i16.bin": allocation.data_indices.astype("<i2"),
        "pilot_fft_indices_i16.bin": allocation.pilot_indices.astype("<i2"),
        "control_data_positions_i16.bin": layout.control_data_positions.astype("<i2"),
        "payload_time_indices_u8.bin": layout.payload_time_indices.astype("u1"),
        "payload_data_positions_i16.bin": layout.payload_data_positions.astype("<i2"),
        "information_bytes_u8.bin": np.frombuffer(information, dtype=np.uint8),
        "codewords_packed_msb_u8.bin": np.packbits(codewords, bitorder="big"),
        "transmitted_bits_packed_msb_u8.bin": np.packbits(
            transmitted_bits, bitorder="big"
        ),
        "control_labels_u8.bin": control_labels,
        "payload_labels_u8.bin": full_payload_labels,
        "payload_symbols_cf32.bin": payload_symbols.astype("<c8"),
        "tx_grid_cf32.bin": tx_grid[0].astype("<c8"),
        "tx_time_cf32.bin": tx_time.astype("<c8"),
    }
    files: dict[str, object] = {}
    for filename, array in arrays.items():
        contiguous = np.ascontiguousarray(array)
        path = output_dir / filename
        path.write_bytes(contiguous.tobytes())
        files[filename] = {
            "dtype": contiguous.dtype.str,
            "shape": list(contiguous.shape),
            "bytes": contiguous.nbytes,
            "sha256": hashlib.sha256(contiguous.tobytes()).hexdigest(),
        }

    manifest = {
        "format_version": 2,
        "byte_order": "little-endian; packed bits are MSB-first within each byte",
        "complex_layout": "interleaved float32 real, imag (C++ std::complex<float>)",
        "source_config": str(args.config.resolve()),
        "sequence": args.sequence & 0xFFFF,
        "fft_size": config.fft_size,
        "cp_length": config.cp_length,
        "modulation": config.modulation,
        "transmit_rank": config.ldpc_transmit_rank,
        "ldpc": {"n": 1008, "k": 504, "blocks": layout.payload_blocks},
        "resource_counts": {
            "data_subcarriers": allocation.data_indices.size,
            "control_re": layout.control_data_positions.size,
            "payload_physical_re": layout.payload_physical_re_count,
            "payload_layer_symbols": layout.payload_layer_symbol_capacity,
            "coded_qam_symbols": layout.coded_qam_symbol_count,
            "padding_qam_symbols": layout.padding_qam_symbol_count,
            "information_bytes": layout.information_bytes,
            "user_payload_bytes": layout.user_payload_bytes,
        },
        "first_values": {
            "control_labels": control_labels[:16].tolist(),
            "payload_labels": full_payload_labels[:16].tolist(),
            "control_data_positions": layout.control_data_positions[:16].tolist(),
        },
        "files": files,
    }
    manifest_path = output_dir / "manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False), encoding="utf-8"
    )
    print(f"Exported {len(arrays)} vectors to {output_dir}")
    print(f"Manifest {manifest_path}")


if __name__ == "__main__":
    main()
