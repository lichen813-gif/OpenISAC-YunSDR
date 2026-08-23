"""OFDM data, pilot, guard-band and DC resource allocation."""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from .config import SimulationConfig


@dataclass(frozen=True)
class ResourceAllocation:
    data_indices: np.ndarray
    pilot_indices: np.ndarray
    phase_reference_indices: np.ndarray
    null_indices: np.ndarray
    data_centered: np.ndarray
    pilot_centered: np.ndarray
    phase_reference_centered: np.ndarray


def build_resource_allocation(config: SimulationConfig) -> ResourceAllocation:
    """Return native FFT indices while preserving centered-frequency order."""

    config.validate()
    data_centered = np.asarray(config.data_centered_subcarriers, dtype=np.int64)
    pilot_centered = np.asarray(
        config.channel_pilot_centered_subcarriers, dtype=np.int64
    )
    phase_reference_centered = np.asarray(
        config.phase_reference_centered_subcarriers, dtype=np.int64
    )
    data_indices = np.mod(data_centered, config.fft_size)
    pilot_indices = np.mod(pilot_centered, config.fft_size)
    phase_reference_indices = np.mod(phase_reference_centered, config.fft_size)
    used = (
        set(data_indices.tolist())
        | set(pilot_indices.tolist())
        | set(phase_reference_indices.tolist())
    )
    null_indices = np.asarray(
        [index for index in range(config.fft_size) if index not in used], dtype=np.int64
    )
    return ResourceAllocation(
        data_indices=data_indices,
        pilot_indices=pilot_indices,
        phase_reference_indices=phase_reference_indices,
        null_indices=null_indices,
        data_centered=data_centered,
        pilot_centered=pilot_centered,
        phase_reference_centered=phase_reference_centered,
    )


def deterministic_pilot_pairs(
    frames: int,
    pilot_centered: np.ndarray,
    seed: int,
) -> np.ndarray:
    """Generate reproducible unit-power BPSK pairs for Alamouti pilot REs."""

    pilot_centered = np.asarray(pilot_centered, dtype=np.int64)
    if frames <= 0:
        raise ValueError("frames must be positive")
    if pilot_centered.size == 0:
        return np.empty((frames, 0, 2), dtype=np.complex128)
    frame_index = np.arange(frames, dtype=np.uint64)[:, None, None]
    pilot_index = pilot_centered.astype(np.int64)[None, :, None].astype(np.uint64)
    slot_index = np.arange(2, dtype=np.uint64)[None, None, :]
    mixed = (
        np.uint64(seed)
        ^ (frame_index * np.uint64(0x9E3779B1))
        ^ (pilot_index * np.uint64(0x85EBCA77))
        ^ (slot_index * np.uint64(0xC2B2AE3D))
    )
    # Fold high-order index bits before selecting a sign. Centered OFDM pilot
    # indices are commonly even, so using only the raw least-significant bit
    # would accidentally produce one constant sign across all pilot tones.
    mixed ^= mixed >> np.uint64(16)
    mixed *= np.uint64(0x7FEB352D)
    mixed ^= mixed >> np.uint64(15)
    signs = np.where((mixed & np.uint64(1)) == 0, 1.0, -1.0)
    return signs.astype(np.complex128)


def deterministic_spatial_pilots(
    frames: int,
    pilot_centered: np.ndarray,
    seed: int,
    nt: int = 2,
) -> tuple[np.ndarray, np.ndarray]:
    """Build frequency-orthogonal BPSK pilots for SFBC or spatial streams.

    Returns a grid with shape ``[frame, time, pilot, tx]`` and one Tx antenna
    assignment per pilot. Adjacent comb-pilot positions rotate across Tx ports.
    """

    pilot_centered = np.asarray(pilot_centered, dtype=np.int64)
    signs = deterministic_pilot_pairs(frames, pilot_centered, seed)
    if nt <= 0:
        raise ValueError("nt must be positive")
    assignments = np.arange(pilot_centered.size, dtype=np.int64) % nt
    grid = np.zeros((frames, 2, pilot_centered.size, nt), dtype=np.complex128)
    for pilot, tx in enumerate(assignments):
        grid[:, :, pilot, tx] = signs[:, pilot, :]
    return grid, assignments
