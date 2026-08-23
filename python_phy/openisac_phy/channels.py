"""Channel models for the initial MIMO/STBC milestone."""

from __future__ import annotations

import numpy as np

from .config import ChannelTap


def exponential_correlation_matrix(size: int, coefficient: float) -> np.ndarray:
    """Return the real Hermitian exponential antenna-correlation matrix."""

    if size <= 0:
        raise ValueError("correlation-matrix size must be positive")
    if not np.isfinite(coefficient) or not 0.0 <= coefficient <= 1.0:
        raise ValueError("antenna correlation must be finite and in [0, 1]")
    indices = np.arange(size, dtype=np.int64)
    return coefficient ** np.abs(indices[:, None] - indices[None, :])


def _hermitian_square_root(matrix: np.ndarray) -> np.ndarray:
    eigenvalues, eigenvectors = np.linalg.eigh(matrix)
    return (eigenvectors * np.sqrt(np.maximum(eigenvalues, 0.0))[None, :]) @ np.conj(
        eigenvectors.T
    )


def apply_spatial_correlation_and_rank(
    channel: np.ndarray,
    tx_correlation: float = 0.0,
    rx_correlation: float = 0.0,
    spatial_rank: int = 0,
) -> np.ndarray:
    """Apply Kronecker antenna correlation and optional exact-rank truncation.

    ``channel`` has shape ``[batch,nr,nt]``. ``spatial_rank=0`` preserves the
    natural rank; a positive value keeps the strongest singular modes while
    preserving each realization's Frobenius energy.
    """

    values = np.asarray(channel, dtype=np.complex128)
    if values.ndim != 3:
        raise ValueError("channel must have shape [batch,nr,nt]")
    batches, nr, nt = values.shape
    if batches <= 0 or nr <= 0 or nt <= 0:
        raise ValueError("channel dimensions must be positive")
    if not 0 <= spatial_rank <= min(nr, nt):
        raise ValueError("spatial_rank must be zero or no greater than min(nr,nt)")

    rx_root = _hermitian_square_root(
        exponential_correlation_matrix(nr, rx_correlation)
    )
    tx_root = _hermitian_square_root(
        exponential_correlation_matrix(nt, tx_correlation)
    )
    correlated = np.einsum(
        "ri,bij,jt->brt", rx_root, values, tx_root, optimize=True
    )
    if spatial_rank == 0 or spatial_rank == min(nr, nt):
        return correlated

    original_energy = np.sum(np.abs(correlated) ** 2, axis=(1, 2), keepdims=True)
    u, singular_values, vh = np.linalg.svd(correlated, full_matrices=False)
    truncated = (
        u[:, :, :spatial_rank]
        * singular_values[:, None, :spatial_rank]
    ) @ vh[:, :spatial_rank, :]
    truncated_energy = np.sum(np.abs(truncated) ** 2, axis=(1, 2), keepdims=True)
    return truncated * np.sqrt(
        original_energy / np.maximum(truncated_energy, 1.0e-30)
    )


def flat_fading(
    batches: int,
    nr: int,
    nt: int,
    profile: str,
    rng: np.random.Generator,
    tx_correlation: float = 0.0,
    rx_correlation: float = 0.0,
    spatial_rank: int = 0,
) -> np.ndarray:
    """Create block-flat channels with shape ``[batch, nr, nt]``."""

    if batches <= 0 or nr <= 0 or nt <= 0:
        raise ValueError("batches, nr and nt must be positive")
    if profile == "awgn":
        return np.ones((batches, nr, nt), dtype=np.complex128)
    if profile == "static":
        rx_phase = np.arange(nr, dtype=np.float64)[:, None] * 0.37
        tx_phase = np.arange(nt, dtype=np.float64)[None, :] * -0.61
        base = np.exp(1j * (rx_phase + tx_phase))
        return np.broadcast_to(base, (batches, nr, nt)).copy()
    if profile == "rayleigh":
        independent = (
            rng.standard_normal((batches, nr, nt))
            + 1j * rng.standard_normal((batches, nr, nt))
        ) / np.sqrt(2.0)
        return apply_spatial_correlation_and_rank(
            independent,
            tx_correlation=tx_correlation,
            rx_correlation=rx_correlation,
            spatial_rank=spatial_rank,
        )
    raise ValueError(f"unsupported flat-fading profile: {profile}")


def apply_flat_mimo(
    tx_samples: np.ndarray,
    channel: np.ndarray,
    noise_variance: float,
    rng: np.random.Generator,
) -> np.ndarray:
    """Apply a block-constant flat MIMO channel.

    ``tx_samples`` is ``[batch, time, tx, sample]`` and the returned samples
    are ``[batch, time, rx, sample]``. The channel is constant across the two
    Alamouti time slots, which is required by the orthogonal combiner.
    """

    tx_samples = np.asarray(tx_samples, dtype=np.complex128)
    channel = np.asarray(channel, dtype=np.complex128)
    if tx_samples.ndim != 4:
        raise ValueError("tx_samples must have shape [batch, time, tx, sample]")
    expected = (tx_samples.shape[0], channel.shape[1], tx_samples.shape[2])
    if channel.shape != expected:
        raise ValueError(f"channel must have shape {expected}, got {channel.shape}")
    if noise_variance < 0:
        raise ValueError("noise_variance must be non-negative")

    clean = np.einsum("brx,btxn->btrn", channel, tx_samples, optimize=True)
    if noise_variance == 0:
        return clean
    sigma = np.sqrt(noise_variance / 2.0)
    noise = sigma * (
        rng.standard_normal(clean.shape) + 1j * rng.standard_normal(clean.shape)
    )
    return clean + noise


def tdl_impulse_response(
    batches: int,
    nr: int,
    nt: int,
    taps: tuple[ChannelTap, ...],
) -> np.ndarray:
    """Build deterministic frequency-selective MIMO taps ``[batch,nr,nt,delay]``.

    The configured gain and phase define each path. Small deterministic spatial
    phase offsets keep different Tx/Rx links distinct while retaining exactly
    reproducible results. Each link is normalized to unit total tap power.
    """

    if batches <= 0 or nr <= 0 or nt <= 0 or not taps:
        raise ValueError("invalid TDL dimensions or empty tap list")
    length = max(tap.delay_samples for tap in taps) + 1
    response = np.zeros((batches, nr, nt, length), dtype=np.complex128)
    for tap_index, tap in enumerate(taps):
        amplitude = 10.0 ** (tap.gain_db / 20.0)
        for rx in range(nr):
            for tx in range(nt):
                spatial_phase = (rx * 0.37 - tx * 0.61 + rx * tx * 0.19) * (tap_index + 1)
                phase = np.deg2rad(tap.phase_deg) + spatial_phase
                response[:, rx, tx, tap.delay_samples] = amplitude * np.exp(1j * phase)
    power = np.sum(np.abs(response) ** 2, axis=-1, keepdims=True)
    response /= np.sqrt(np.maximum(power, 1.0e-15))
    return response


def apply_tdl_mimo(
    tx_samples: np.ndarray,
    impulse_response: np.ndarray,
    noise_variance: float,
    rng: np.random.Generator,
) -> np.ndarray:
    """Apply independent linear convolution to each OFDM symbol and MIMO link."""

    tx_samples = np.asarray(tx_samples, dtype=np.complex128)
    impulse_response = np.asarray(impulse_response, dtype=np.complex128)
    if tx_samples.ndim != 4:
        raise ValueError("tx_samples must have shape [batch, time, tx, sample]")
    if impulse_response.ndim == 4:
        expected = (tx_samples.shape[0], impulse_response.shape[1], tx_samples.shape[2])
        if impulse_response.shape[:3] != expected:
            raise ValueError(
                f"impulse_response must start with shape {expected}, got {impulse_response.shape}"
            )
    elif impulse_response.ndim == 5:
        expected = (
            tx_samples.shape[0],
            tx_samples.shape[1],
            impulse_response.shape[2],
            tx_samples.shape[2],
        )
        if impulse_response.shape[:4] != expected:
            raise ValueError(
                "time-varying impulse_response must have shape "
                f"{expected} + [delay], got {impulse_response.shape}"
            )
    else:
        raise ValueError("impulse_response must have 4 or 5 dimensions")
    if noise_variance < 0:
        raise ValueError("noise_variance must be non-negative")

    batches, time_slots, nt, sample_count = tx_samples.shape
    nr = impulse_response.shape[-3]
    clean = np.zeros((batches, time_slots, nr, sample_count), dtype=np.complex128)
    for batch in range(batches):
        for time_slot in range(time_slots):
            for rx in range(nr):
                for tx in range(nt):
                    link_response = (
                        impulse_response[batch, time_slot, rx, tx]
                        if impulse_response.ndim == 5
                        else impulse_response[batch, rx, tx]
                    )
                    clean[batch, time_slot, rx] += np.convolve(
                        tx_samples[batch, time_slot, tx],
                        link_response,
                        mode="full",
                    )[:sample_count]
    if noise_variance == 0:
        return clean
    sigma = np.sqrt(noise_variance / 2.0)
    noise = sigma * (
        rng.standard_normal(clean.shape) + 1j * rng.standard_normal(clean.shape)
    )
    return clean + noise


def deterministic_doppler_frequencies(
    impulse_response: np.ndarray,
    maximum_doppler_hz: float,
) -> np.ndarray:
    """Assign one reproducible Doppler frequency to each active MIMO path."""

    impulse_response = np.asarray(impulse_response, dtype=np.complex128)
    if impulse_response.ndim != 4:
        raise ValueError("impulse_response must have shape [batch, rx, tx, delay]")
    if not np.isfinite(maximum_doppler_hz) or maximum_doppler_hz < 0:
        raise ValueError("maximum_doppler_hz must be finite and non-negative")

    nr, nt, delay_count = impulse_response.shape[1:]
    rx_index = np.arange(nr, dtype=np.float64)[:, None, None]
    tx_index = np.arange(nt, dtype=np.float64)[None, :, None]
    delay_index = np.arange(delay_count, dtype=np.float64)[None, None, :]
    spatial_argument = 0.73 * (rx_index + 1.0) - 0.91 * (
        tx_index + 1.0
    ) + 1.17 * (delay_index + 1.0)
    doppler_hz = maximum_doppler_hz * np.sin(spatial_argument)
    active = np.any(np.abs(impulse_response) > 0.0, axis=0)
    return np.where(active, doppler_hz, 0.0)


def sample_impulse_response_with_doppler(
    impulse_response: np.ndarray,
    doppler_frequencies_hz: np.ndarray,
    sample_times: np.ndarray,
    sample_rate_hz: float,
) -> np.ndarray:
    """Sample continuously rotating MIMO taps at absolute sample positions."""

    impulse_response = np.asarray(impulse_response, dtype=np.complex128)
    doppler = np.asarray(doppler_frequencies_hz, dtype=np.float64)
    times = np.asarray(sample_times, dtype=np.float64)
    if impulse_response.ndim != 4:
        raise ValueError("impulse_response must have shape [batch, rx, tx, delay]")
    if doppler.shape != impulse_response.shape[1:]:
        raise ValueError("doppler frequencies must match [rx, tx, delay]")
    if times.ndim != 1 or np.any(~np.isfinite(times)):
        raise ValueError("sample_times must be a finite one-dimensional array")
    if not np.isfinite(sample_rate_hz) or sample_rate_hz <= 0.0:
        raise ValueError("sample_rate_hz must be positive and finite")
    rotation = np.exp(
        1j
        * 2.0
        * np.pi
        * times[:, None, None, None]
        / sample_rate_hz
        * doppler[None, :, :, :]
    )
    return impulse_response[:, None, :, :, :] * rotation[None, :, :, :, :]


def apply_continuous_doppler_mimo(
    tx_samples: np.ndarray,
    impulse_response: np.ndarray,
    doppler_frequencies_hz: np.ndarray,
    sample_rate_hz: float,
    noise_variance: float,
    rng: np.random.Generator,
) -> np.ndarray:
    """Apply sample-continuous path rotation and linear MIMO convolution.

    Unlike the symbol-rate model, every active path rotates at every output
    sample. The absolute phase is continuous across OFDM-symbol boundaries,
    so variation inside the FFT window produces genuine inter-carrier
    interference (ICI).
    """

    tx_samples = np.asarray(tx_samples, dtype=np.complex128)
    impulse_response = np.asarray(impulse_response, dtype=np.complex128)
    doppler = np.asarray(doppler_frequencies_hz, dtype=np.float64)
    if tx_samples.ndim != 4:
        raise ValueError("tx_samples must have shape [batch, time, tx, sample]")
    if impulse_response.ndim != 4:
        raise ValueError("impulse_response must have shape [batch, rx, tx, delay]")
    expected = (tx_samples.shape[0], impulse_response.shape[1], tx_samples.shape[2])
    if impulse_response.shape[:3] != expected:
        raise ValueError(
            f"impulse_response must start with shape {expected}, got {impulse_response.shape}"
        )
    if doppler.shape != impulse_response.shape[1:]:
        raise ValueError("doppler frequencies must match [rx, tx, delay]")
    if not np.isfinite(sample_rate_hz) or sample_rate_hz <= 0.0:
        raise ValueError("sample_rate_hz must be positive and finite")
    if noise_variance < 0:
        raise ValueError("noise_variance must be non-negative")

    batches, time_slots, nt, sample_count = tx_samples.shape
    nr = impulse_response.shape[1]
    delay_count = impulse_response.shape[-1]
    clean = np.zeros((batches, time_slots, nr, sample_count), dtype=np.complex128)
    local_samples = np.arange(sample_count, dtype=np.float64)
    for time_slot in range(time_slots):
        absolute_samples = time_slot * sample_count + local_samples
        rotations = np.exp(
            1j
            * 2.0
            * np.pi
            * doppler[..., None]
            * absolute_samples[None, None, None, :]
            / sample_rate_hz
        )
        for batch in range(batches):
            for rx in range(nr):
                for tx in range(nt):
                    for delay in range(delay_count):
                        coefficient = impulse_response[batch, rx, tx, delay]
                        if coefficient == 0.0 or delay >= sample_count:
                            continue
                        clean[batch, time_slot, rx, delay:] += (
                            coefficient
                            * rotations[rx, tx, delay, delay:]
                            * tx_samples[batch, time_slot, tx, : sample_count - delay]
                        )
    if noise_variance == 0:
        return clean
    sigma = np.sqrt(noise_variance / 2.0)
    noise = sigma * (
        rng.standard_normal(clean.shape) + 1j * rng.standard_normal(clean.shape)
    )
    return clean + noise


def evolve_impulse_response_with_doppler(
    impulse_response: np.ndarray,
    time_slots: int,
    samples_per_symbol: int,
    sample_rate_hz: float,
    maximum_doppler_hz: float,
) -> tuple[np.ndarray, np.ndarray]:
    """Create symbol-rate time-varying MIMO taps and their Doppler rates.

    Each active ``Rx×Tx×tap`` path receives a deterministic Doppler in
    ``[-maximum_doppler_hz, +maximum_doppler_hz]``.  This isolates Alamouti
    block mismatch while retaining fully reproducible Monte-Carlo vectors.
    """

    impulse_response = np.asarray(impulse_response, dtype=np.complex128)
    if impulse_response.ndim != 4:
        raise ValueError("impulse_response must have shape [batch, rx, tx, delay]")
    if time_slots <= 0 or samples_per_symbol <= 0 or sample_rate_hz <= 0:
        raise ValueError("time, symbol length and sample rate must be positive")
    if not np.isfinite(maximum_doppler_hz) or maximum_doppler_hz < 0:
        raise ValueError("maximum_doppler_hz must be finite and non-negative")

    doppler_hz = deterministic_doppler_frequencies(
        impulse_response, maximum_doppler_hz
    )

    symbol_times = (
        np.arange(time_slots, dtype=np.float64) * samples_per_symbol / sample_rate_hz
    )
    evolved = sample_impulse_response_with_doppler(
        impulse_response,
        doppler_hz,
        symbol_times * sample_rate_hz,
        sample_rate_hz,
    )
    return evolved, doppler_hz
