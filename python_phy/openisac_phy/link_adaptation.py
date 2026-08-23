"""Low-complexity 2x2 rank and MCS recommendation for C++ implementation."""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np


MCS_REQUIRED_SINR_DB = {
    "qpsk": 4.0,
    "16qam": 10.0,
    "64qam": 18.0,
    "256qam": 26.0,
}


@dataclass(frozen=True)
class LinkAdaptationDecision:
    recommended_rank: np.ndarray
    recommended_modulation: np.ndarray
    outage: np.ndarray
    rank2_bottleneck_sinr_db: np.ndarray
    rank1_sinr_db: np.ndarray
    minimum_eigenvalue_ratio: np.ndarray
    configured_mcs_supported: np.ndarray


def predict_2x2_detector_mse(
    channel: np.ndarray, noise_variance: float, detector: str
) -> np.ndarray:
    """Predict two-layer ZF/MMSE MSE without detecting payload symbols."""

    h = np.asarray(channel, dtype=np.complex128)
    if h.ndim != 5 or h.shape[-2:] != (2, 2):
        raise ValueError("channel must have shape [frame,time,subcarrier,2,2]")
    if detector not in {"zf", "mmse"}:
        raise ValueError("detector must be zf or mmse")
    if noise_variance < 0.0 or not np.isfinite(noise_variance):
        raise ValueError("noise_variance must be finite and non-negative")
    effective = h / np.sqrt(2.0)
    hermitian = np.swapaxes(np.conj(effective), -1, -2)
    gram = hermitian @ effective
    if detector == "zf":
        weights = np.linalg.pinv(effective)
    else:
        identity = np.eye(2, dtype=np.complex128)
        weights = np.linalg.solve(gram + noise_variance * identity, hermitian)
    transfer_error = weights @ effective - np.eye(2, dtype=np.complex128)
    covariance = transfer_error @ np.swapaxes(
        np.conj(transfer_error), -1, -2
    ) + noise_variance * (weights @ np.swapaxes(np.conj(weights), -1, -2))
    return np.maximum(
        np.real(np.diagonal(covariance, axis1=-2, axis2=-1)), 0.0
    )


def _mcs_from_sinr(sinr_db: np.ndarray, margin_db: float) -> np.ndarray:
    usable = np.asarray(sinr_db, dtype=np.float64) - margin_db
    result = np.full(usable.shape, "qpsk", dtype="<U7")
    for name in ("16qam", "64qam", "256qam"):
        result[usable >= MCS_REQUIRED_SINR_DB[name]] = name
    return result


def recommend_2x2_rank_mcs(
    channel: np.ndarray,
    equivalent_mse: np.ndarray,
    noise_variance: float,
    detector: str,
    configured_modulation: str,
    *,
    rank2_minimum_eigenvalue_ratio: float = 0.05,
    rank2_minimum_sinr_db: float = 4.0,
    implementation_margin_db: float = 2.0,
) -> LinkAdaptationDecision:
    """Recommend STBC/rank-1 fallback or rank-2 plus one square-QAM MCS.

    The decision uses one averaged 2x2 Gram matrix and two averaged detector
    MSE values per frame.  Its hot-path implementation therefore needs no
    SVD, iterative search, or matrix decomposition.
    """

    h = np.asarray(channel, dtype=np.complex128)
    mse = np.asarray(equivalent_mse, dtype=np.float64)
    if h.ndim != 5 or h.shape[-2:] != (2, 2):
        raise ValueError("channel must have shape [frame,time,subcarrier,2,2]")
    if mse.shape != h.shape[:-2] + (2,):
        raise ValueError("equivalent_mse must match channel frame/time/subcarrier/layer")
    if detector not in {"zf", "mmse"}:
        raise ValueError("detector must be zf or mmse")
    if configured_modulation not in MCS_REQUIRED_SINR_DB:
        raise ValueError("unsupported configured modulation")
    if noise_variance < 0.0 or not np.isfinite(noise_variance):
        raise ValueError("noise_variance must be finite and non-negative")

    gram = np.swapaxes(np.conj(h), -1, -2) @ h
    mean_gram = np.mean(gram, axis=(1, 2))
    a = np.real(mean_gram[:, 0, 0])
    d = np.real(mean_gram[:, 1, 1])
    b = mean_gram[:, 0, 1]
    trace = np.maximum(a + d, 0.0)
    root = np.sqrt(np.maximum((a - d) ** 2 + 4.0 * np.abs(b) ** 2, 0.0))
    maximum_eigenvalue = np.maximum(0.5 * (trace + root), 1.0e-15)
    minimum_eigenvalue = np.maximum(0.5 * (trace - root), 0.0)
    eigenvalue_ratio = minimum_eigenvalue / maximum_eigenvalue

    mean_mse = np.maximum(np.mean(mse, axis=(1, 2)), 1.0e-15)
    if detector == "mmse":
        layer_sinr = np.maximum(1.0 / mean_mse - 1.0, 1.0e-15)
    else:
        layer_sinr = 1.0 / mean_mse
    rank2_sinr_db = 10.0 * np.log10(np.min(layer_sinr, axis=-1))

    if noise_variance == 0.0:
        rank1_sinr_db = np.full(trace.shape, 150.0)
    else:
        # Alamouti/STBC transmits with total power one over both antennas.
        rank1_sinr_db = 10.0 * np.log10(
            np.maximum(trace / (2.0 * noise_variance), 1.0e-15)
        )

    rank2_allowed = (
        (eigenvalue_ratio >= rank2_minimum_eigenvalue_ratio)
        & (rank2_sinr_db >= rank2_minimum_sinr_db + implementation_margin_db)
    )
    recommended_rank = np.where(rank2_allowed, 2, 1).astype(np.int8)
    selected_sinr_db = np.where(rank2_allowed, rank2_sinr_db, rank1_sinr_db)
    recommended_modulation = _mcs_from_sinr(
        selected_sinr_db, implementation_margin_db
    )
    outage = (
        selected_sinr_db - implementation_margin_db
        < MCS_REQUIRED_SINR_DB["qpsk"]
    )
    configured_mcs_supported = rank2_allowed & (
        rank2_sinr_db - implementation_margin_db
        >= MCS_REQUIRED_SINR_DB[configured_modulation]
    )
    return LinkAdaptationDecision(
        recommended_rank=recommended_rank,
        recommended_modulation=recommended_modulation,
        outage=outage,
        rank2_bottleneck_sinr_db=rank2_sinr_db,
        rank1_sinr_db=rank1_sinr_db,
        minimum_eigenvalue_ratio=eigenvalue_ratio,
        configured_mcs_supported=configured_mcs_supported,
    )
