"""Dedicated phase-reference mapping and channel-independent CPE tracking."""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np


@dataclass(frozen=True)
class PhaseReferenceEstimate:
    differential_phase_rad: np.ndarray
    phase_slope_rad_per_subcarrier: np.ndarray
    estimated_sfo_ppm: np.ndarray
    coherence: np.ndarray
    applied: np.ndarray


def map_phase_references(
    tx_grid: np.ndarray,
    phase_reference_indices: np.ndarray,
) -> np.ndarray:
    """Map repeated unit pilots on Tx0 in both OFDM slots."""

    tx_grid = np.asarray(tx_grid, dtype=np.complex128)
    indices = np.asarray(phase_reference_indices, dtype=np.int64)
    if tx_grid.ndim != 4 or tx_grid.shape[1] != 2 or tx_grid.shape[3] < 2:
        raise ValueError("tx_grid must have shape [batch, 2, subcarrier, nt>=2]")
    if indices.size == 0:
        raise ValueError("at least one phase-reference subcarrier is required")
    if np.any((indices < 0) | (indices >= tx_grid.shape[2])):
        raise ValueError("phase-reference index is outside the resource grid")
    mapped = tx_grid.copy()
    mapped[:, :, indices, :] = 0.0
    mapped[:, 0, indices, 0] = 1.0
    mapped[:, 1, indices, 0] = 1.0
    return mapped


def correct_differential_phase(
    rx_grid: np.ndarray,
    phase_reference_indices: np.ndarray,
    minimum_coherence: float = 0.0,
    *,
    phase_reference_centered_subcarriers: np.ndarray | None = None,
    slope_tracking: bool = False,
    samples_per_symbol: int | None = None,
) -> tuple[np.ndarray, PhaseReferenceEstimate]:
    """Estimate and correct slot differential phase without channel knowledge.

    With ``slope_tracking``, fit ``phase(k) = intercept + slope*k`` across
    phase references.  The slope captures sampling-frequency offset while the
    intercept captures ordinary common-phase rotation.
    """

    rx_grid = np.asarray(rx_grid, dtype=np.complex128)
    indices = np.asarray(phase_reference_indices, dtype=np.int64)
    if rx_grid.ndim != 4 or rx_grid.shape[1] != 2:
        raise ValueError("rx_grid must have shape [batch, 2, subcarrier, rx]")
    if indices.size == 0:
        raise ValueError("at least one phase-reference subcarrier is required")
    if np.any((indices < 0) | (indices >= rx_grid.shape[2])):
        raise ValueError("phase-reference index is outside the resource grid")
    if not np.isfinite(minimum_coherence) or not 0.0 <= minimum_coherence <= 1.0:
        raise ValueError("minimum_coherence must be in [0, 1]")
    if phase_reference_centered_subcarriers is None:
        centered = np.where(indices < rx_grid.shape[2] // 2, indices, indices - rx_grid.shape[2])
    else:
        centered = np.asarray(
            phase_reference_centered_subcarriers, dtype=np.float64
        )
        if centered.shape != indices.shape:
            raise ValueError("one centered subcarrier is required per phase reference")
    if slope_tracking and indices.size < 2:
        raise ValueError("phase-slope tracking requires at least two references")
    if samples_per_symbol is None:
        samples_per_symbol = rx_grid.shape[2]
    if samples_per_symbol <= 0:
        raise ValueError("samples_per_symbol must be positive")

    slot0 = rx_grid[:, 0, indices, :]
    slot1 = rx_grid[:, 1, indices, :]
    per_reference_correlation = np.sum(np.conj(slot0) * slot1, axis=2)
    power0 = np.sum(np.abs(slot0) ** 2, axis=(1, 2))
    power1 = np.sum(np.abs(slot1) ** 2, axis=(1, 2))
    batches = rx_grid.shape[0]
    phase = np.empty(batches, dtype=np.float64)
    slope = np.zeros(batches, dtype=np.float64)
    if slope_tracking:
        order = np.argsort(centered)
        fit_subcarriers = centered[order]
        design = np.column_stack((np.ones(indices.size), fit_subcarriers))
        for batch in range(batches):
            measured_phase = np.unwrap(
                np.angle(per_reference_correlation[batch, order])
            )
            weights = np.maximum(
                np.abs(per_reference_correlation[batch, order]), 1.0e-15
            )
            weighted_design = design * np.sqrt(weights)[:, None]
            weighted_phase = measured_phase * np.sqrt(weights)
            fitted, *_ = np.linalg.lstsq(
                weighted_design, weighted_phase, rcond=None
            )
            phase[batch], slope[batch] = fitted
    else:
        correlation = np.sum(per_reference_correlation, axis=1)
        phase[:] = np.angle(correlation)

    fitted_reference_phase = phase[:, None] + slope[:, None] * centered[None, :]
    aligned_correlation = np.sum(
        per_reference_correlation * np.exp(-1j * fitted_reference_phase), axis=1
    )
    coherence = np.abs(aligned_correlation) ** 2 / np.maximum(
        power0 * power1, 1.0e-30
    )
    applied = coherence >= minimum_coherence
    applied_phase = np.where(applied, phase, 0.0)
    applied_slope = np.where(applied, slope, 0.0)
    fft_indices = np.arange(rx_grid.shape[2], dtype=np.float64)
    all_centered = np.where(
        fft_indices < rx_grid.shape[2] // 2,
        fft_indices,
        fft_indices - rx_grid.shape[2],
    )
    correction_phase = (
        applied_phase[:, None] + applied_slope[:, None] * all_centered[None, :]
    )
    corrected = rx_grid.copy()
    corrected[:, 1, :, :] *= np.exp(-1j * correction_phase)[:, :, None]
    estimated_sfo_ppm = (
        -slope * rx_grid.shape[2] / (2.0 * np.pi * samples_per_symbol) * 1.0e6
    )
    return corrected, PhaseReferenceEstimate(
        phase,
        slope,
        estimated_sfo_ppm,
        coherence,
        applied,
    )
