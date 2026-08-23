"""Linear perfect-CSI detectors for two-layer spatial multiplexing."""

from __future__ import annotations

import numpy as np

def spatial_multiplexing_encode_grid(
    layer_symbols: np.ndarray,
    data_indices: np.ndarray,
    fft_size: int,
) -> np.ndarray:
    """Map two layers to two antennas with fixed total-power normalization.

    ``layer_symbols`` has shape ``[batch, time, data_subcarrier, 2]`` and the
    result has shape ``[batch, time, fft_size, 2]``.
    """

    symbols = np.asarray(layer_symbols, dtype=np.complex128)
    indices = np.asarray(data_indices, dtype=np.int64)
    if symbols.ndim != 4 or symbols.shape[1] != 2 or symbols.shape[-1] < 2:
        raise ValueError(
            "layer_symbols must have shape [batch, 2, data_subcarrier, layers>=2]"
        )
    if indices.ndim != 1 or symbols.shape[2] != indices.size:
        raise ValueError("data_indices must match the data-subcarrier dimension")
    if fft_size <= 0 or np.any(indices < 0) or np.any(indices >= fft_size):
        raise ValueError("data_indices must be valid native FFT indices")
    nt = symbols.shape[-1]
    grid = np.zeros((symbols.shape[0], 2, fft_size, nt), dtype=np.complex128)
    grid[:, :, indices, :] = symbols / np.sqrt(nt)
    return grid


def detect_spatial_multiplexing(
    received: np.ndarray,
    channel: np.ndarray,
    noise_variance: float,
    detector: str,
) -> tuple[np.ndarray, np.ndarray]:
    """Detect two streams using ZF or linear MMSE.

    Args:
        received: ``[batch, time, data_subcarrier, nr]``.
        channel: ``[batch, time, data_subcarrier, nr, 2]``.
        noise_variance: complex receive-sample noise variance.
        detector: ``"zf"`` or ``"mmse"``.

    Returns:
        Detected symbols and predicted per-layer MSE, both shaped
        ``[batch, time, data_subcarrier, 2]``.
    """

    y = np.asarray(received, dtype=np.complex128)
    h = np.asarray(channel, dtype=np.complex128)
    if y.ndim != 4:
        raise ValueError("received must have shape [batch, time, subcarrier, nr]")
    layers = h.shape[-1] if h.ndim == 5 else 0
    if layers < 2 or h.shape != (*y.shape, layers):
        raise ValueError("channel must have shape [batch,time,subcarrier,nr,layers>=2]")
    if y.shape[-1] < layers:
        raise ValueError("detection requires nr>=layers")
    if detector not in {"zf", "mmse"}:
        raise ValueError("detector must be zf or mmse")
    if not np.isfinite(noise_variance) or noise_variance < 0.0:
        raise ValueError("noise_variance must be finite and non-negative")

    effective_channel = h / np.sqrt(layers)
    hermitian = np.swapaxes(np.conj(effective_channel), -1, -2)
    gram = hermitian @ effective_channel
    if detector == "zf":
        weights = np.linalg.pinv(effective_channel)
    else:
        identity = np.eye(layers, dtype=np.complex128)
        regularized = gram + noise_variance * identity
        weights = np.linalg.solve(regularized, hermitian)

    detected = np.squeeze(weights @ y[..., None], axis=-1)
    transfer_error = weights @ effective_channel - np.eye(layers, dtype=np.complex128)
    error_covariance = transfer_error @ np.swapaxes(
        np.conj(transfer_error), -1, -2
    ) + noise_variance * (weights @ np.swapaxes(np.conj(weights), -1, -2))
    predicted_mse = np.maximum(
        np.real(np.diagonal(error_covariance, axis1=-2, axis2=-1)), 0.0
    )
    return detected, predicted_mse
