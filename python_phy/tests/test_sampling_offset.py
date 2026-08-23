import numpy as np

from openisac_phy.sampling_offset import (
    apply_sampling_frequency_offset,
    inverse_sampling_offset_ppm,
    phase_slope_to_sfo_ppm,
)


def test_sampling_frequency_offset_resamples_complex_tone() -> None:
    samples = np.arange(4096, dtype=np.float64)
    normalized_frequency = 0.05
    tone = np.exp(1j * 2.0 * np.pi * normalized_frequency * samples)
    shifted = apply_sampling_frequency_offset(tone[None, None, :], 100.0)[0, 0]
    central_phase = np.unwrap(np.angle(shifted[32:-32]))
    measured_frequency = np.mean(np.diff(central_phase)) / (2.0 * np.pi)
    expected_frequency = normalized_frequency / (1.0 + 100.0e-6)
    assert abs(measured_frequency - expected_frequency) < 2.0e-7


def test_four_point_cubic_resampler_tracks_complex_tone_with_fixed_support() -> None:
    samples = np.arange(4096, dtype=np.float64)
    normalized_frequency = 0.15
    tone = np.exp(1j * 2.0 * np.pi * normalized_frequency * samples)
    shifted = apply_sampling_frequency_offset(
        tone[None, None, :], 500.0, method="cubic"
    )[0, 0]
    central = shifted[32:-32]
    measured_frequency = np.mean(np.diff(np.unwrap(np.angle(central)))) / (
        2.0 * np.pi
    )
    expected_frequency = normalized_frequency / (1.0 + 500.0e-6)

    assert abs(measured_frequency - expected_frequency) < 2.0e-7
    assert abs(np.mean(np.abs(central)) - 1.0) < 0.012


def test_eight_tap_sinc_resampler_preserves_high_order_qam_bandwidth() -> None:
    samples = np.arange(4096, dtype=np.float64)
    normalized_frequency = 0.35
    tone = np.exp(1j * 2.0 * np.pi * normalized_frequency * samples)
    shifted = apply_sampling_frequency_offset(
        tone[None, None, :], 500.0, method="sinc8"
    )[0, 0, 32:-32]

    assert abs(np.mean(np.abs(shifted)) - 1.0) < 0.03


def test_phase_slope_conversion_uses_receiver_clock_convention() -> None:
    expected_ppm = 80.0
    slope = -2.0 * np.pi * expected_ppm * 1.0e-6 * 80 / 64
    measured = phase_slope_to_sfo_ppm(slope, 64, 80)
    np.testing.assert_allclose(measured, expected_ppm, atol=1.0e-12)


def test_inverse_sampling_offset_uses_exact_reciprocal_ratio_per_batch() -> None:
    offsets = np.array([50.0, -80.0, 500.0])
    inverse = inverse_sampling_offset_ppm(offsets)
    np.testing.assert_allclose(
        (1.0 + offsets * 1.0e-6) * (1.0 + inverse * 1.0e-6),
        1.0,
        atol=1.0e-15,
    )
