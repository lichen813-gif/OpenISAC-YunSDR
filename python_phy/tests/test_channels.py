import numpy as np

from openisac_phy.channels import (
    apply_continuous_doppler_mimo,
    apply_spatial_correlation_and_rank,
    evolve_impulse_response_with_doppler,
    flat_fading,
    tdl_impulse_response,
)
from openisac_phy.config import ChannelTap, parse_tap_string


def test_parse_manual_tdl_taps() -> None:
    taps = parse_tap_string("0:0:0, 3:-4:45; 9:-8:-80")
    assert taps == (
        ChannelTap(0, 0.0, 0.0),
        ChannelTap(3, -4.0, 45.0),
        ChannelTap(9, -8.0, -80.0),
    )


def test_tdl_links_have_unit_total_power() -> None:
    taps = (ChannelTap(0, 0.0), ChannelTap(2, -6.0, 30.0))
    response = tdl_impulse_response(3, 2, 2, taps)
    power = np.sum(np.abs(response) ** 2, axis=-1)
    assert response.shape == (3, 2, 2, 3)
    assert np.allclose(power, 1.0)


def test_symbol_rate_doppler_preserves_path_magnitude_and_rotates_phase() -> None:
    response = tdl_impulse_response(
        2, 2, 2, (ChannelTap(0, 0.0), ChannelTap(3, -6.0, 20.0))
    )
    evolved, doppler = evolve_impulse_response_with_doppler(
        response,
        time_slots=3,
        samples_per_symbol=80,
        sample_rate_hz=960000.0,
        maximum_doppler_hz=500.0,
    )

    assert evolved.shape == (2, 3, 2, 2, 4)
    assert doppler.shape == (2, 2, 4)
    np.testing.assert_allclose(np.abs(evolved[:, 0]), np.abs(evolved[:, 1]))
    active = np.abs(response[0]) > 0.0
    measured_rotation = evolved[0, 1][active] / evolved[0, 0][active]
    expected_rotation = np.exp(1j * 2.0 * np.pi * doppler[active] * 80 / 960000.0)
    np.testing.assert_allclose(measured_rotation, expected_rotation, atol=1.0e-12)


def test_continuous_doppler_rotates_every_sample_across_symbol_boundary() -> None:
    tx = np.ones((1, 2, 1, 8), dtype=np.complex128)
    response = np.ones((1, 1, 1, 1), dtype=np.complex128)
    doppler = np.array([[[100.0]]])
    received = apply_continuous_doppler_mimo(
        tx,
        response,
        doppler,
        sample_rate_hz=1000.0,
        noise_variance=0.0,
        rng=np.random.default_rng(9),
    )

    expected = np.exp(1j * 2.0 * np.pi * 0.1 * np.arange(16)).reshape(2, 8)
    np.testing.assert_allclose(received[0, :, 0], expected, atol=1.0e-12)
    assert not np.allclose(received[0, 0, 0, 0], received[0, 0, 0, -1])


def test_kronecker_rayleigh_matches_configured_tx_rx_correlation() -> None:
    channel = flat_fading(
        50000,
        2,
        2,
        "rayleigh",
        np.random.default_rng(3101),
        tx_correlation=0.8,
        rx_correlation=0.6,
    )
    tx_measured = np.mean(channel[:, 0, 0] * np.conj(channel[:, 0, 1]))
    rx_measured = np.mean(channel[:, 0, 0] * np.conj(channel[:, 1, 0]))
    power = np.mean(np.abs(channel[:, 0, 0]) ** 2)

    assert abs(tx_measured / power - 0.8) < 0.02
    assert abs(rx_measured / power - 0.6) < 0.02


def test_exact_rank_projection_preserves_frobenius_energy() -> None:
    rng = np.random.default_rng(3201)
    channel = rng.standard_normal((10, 4, 4)) + 1j * rng.standard_normal((10, 4, 4))
    projected = apply_spatial_correlation_and_rank(channel, spatial_rank=2)

    assert np.all(np.linalg.matrix_rank(projected, tol=1.0e-10) == 2)
    np.testing.assert_allclose(
        np.sum(np.abs(projected) ** 2, axis=(1, 2)),
        np.sum(np.abs(channel) ** 2, axis=(1, 2)),
        rtol=1.0e-12,
    )
