"""Comb-pilot common-phase and residual-CFO tracking."""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np


@dataclass(frozen=True)
class PilotPhaseEstimate:
    """Per-frame, per-OFDM-symbol pilot phase diagnostics."""

    phase_rad: np.ndarray
    coherence: np.ndarray
    predicted_pilot_power: np.ndarray
    applied: np.ndarray


def correct_common_phase(
    rx_grid: np.ndarray,
    tx_grid: np.ndarray,
    channel: np.ndarray,
    pilot_indices: np.ndarray,
    minimum_coherence: float = 0.0,
) -> tuple[np.ndarray, PilotPhaseEstimate]:
    """Estimate and remove one common phase per OFDM symbol.

    This milestone uses the existing perfect CSI to predict received pilot
    values. The next channel-estimation stage will replace that dependency
    with orthogonal Tx pilots and estimated CSI.
    """

    rx_grid = np.asarray(rx_grid, dtype=np.complex128)
    tx_grid = np.asarray(tx_grid, dtype=np.complex128)
    channel = np.asarray(channel, dtype=np.complex128)
    pilot_indices = np.asarray(pilot_indices, dtype=np.int64)
    if rx_grid.ndim != 4:
        raise ValueError("rx_grid must have shape [batch, time, subcarrier, rx]")
    if tx_grid.ndim != 4:
        raise ValueError("tx_grid must have shape [batch, time, subcarrier, tx]")
    if rx_grid.shape[:3] != tx_grid.shape[:3]:
        raise ValueError("rx_grid and tx_grid batch/time/subcarrier dimensions must match")
    expected_channel = (
        rx_grid.shape[0],
        rx_grid.shape[2],
        rx_grid.shape[3],
        tx_grid.shape[3],
    )
    if channel.shape != expected_channel:
        raise ValueError(f"channel must have shape {expected_channel}, got {channel.shape}")
    if pilot_indices.size == 0:
        raise ValueError("pilot phase tracking requires at least one pilot subcarrier")
    if not np.isfinite(minimum_coherence) or not 0.0 <= minimum_coherence <= 1.0:
        raise ValueError("minimum_coherence must be in [0, 1]")
    if np.any((pilot_indices < 0) | (pilot_indices >= rx_grid.shape[2])):
        raise ValueError("pilot index is outside the resource grid")

    predicted = np.einsum("btkx,bkrx->btkr", tx_grid, channel, optimize=True)
    predicted_pilots = predicted[:, :, pilot_indices, :]
    received_pilots = rx_grid[:, :, pilot_indices, :]
    correlation = np.sum(
        np.conj(predicted_pilots) * received_pilots, axis=(2, 3)
    )
    predicted_power = np.sum(np.abs(predicted_pilots) ** 2, axis=(2, 3))
    received_power = np.sum(np.abs(received_pilots) ** 2, axis=(2, 3))
    if np.any(predicted_power <= 1.0e-15):
        raise ValueError("pilot pattern has zero predicted receive energy")

    phase = np.angle(correlation)
    coherence = np.abs(correlation) ** 2 / np.maximum(
        predicted_power * received_power, 1.0e-30
    )
    applied = coherence >= minimum_coherence
    applied_phase = np.where(applied, phase, 0.0)
    corrected = rx_grid * np.exp(-1j * applied_phase)[:, :, None, None]
    return corrected, PilotPhaseEstimate(phase, coherence, predicted_power, applied)


def phase_difference_to_cfo_hz(
    phase_rad: np.ndarray,
    *,
    sample_rate_hz: float,
    samples_per_symbol: int,
) -> np.ndarray:
    """Convert wrapped phase change between two OFDM symbols to residual CFO."""

    phase_rad = np.asarray(phase_rad, dtype=np.float64)
    if phase_rad.ndim != 2 or phase_rad.shape[1] != 2:
        raise ValueError("phase_rad must have shape [batch, 2]")
    if sample_rate_hz <= 0 or samples_per_symbol <= 0:
        raise ValueError("sample rate and symbol length must be positive")
    delta = np.angle(np.exp(1j * (phase_rad[:, 1] - phase_rad[:, 0])))
    symbol_period_s = samples_per_symbol / sample_rate_hz
    return delta / (2.0 * np.pi * symbol_period_s)
