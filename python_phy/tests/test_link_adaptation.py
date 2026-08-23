import numpy as np

from openisac_phy.link_adaptation import recommend_2x2_rank_mcs


def test_rank_mcs_recommends_rank2_for_well_conditioned_high_sinr() -> None:
    channel = np.broadcast_to(np.eye(2), (3, 1, 8, 2, 2)).copy()
    mse = np.full((3, 1, 8, 2), 0.009)

    decision = recommend_2x2_rank_mcs(
        channel, mse, 0.01, "mmse", "64qam"
    )

    np.testing.assert_array_equal(decision.recommended_rank, 2)
    np.testing.assert_array_equal(decision.recommended_modulation, "64qam")
    assert np.all(decision.configured_mcs_supported)
    assert not np.any(decision.outage)


def test_rank_mcs_falls_back_to_rank1_for_nearly_singular_channel() -> None:
    channel = np.zeros((2, 1, 8, 2, 2), dtype=np.complex128)
    channel[..., 0, 0] = 1.0
    channel[..., 1, 0] = 0.5
    channel[..., 0, 1] = 1.0
    channel[..., 1, 1] = 0.5
    mse = np.full((2, 1, 8, 2), 0.5)

    decision = recommend_2x2_rank_mcs(
        channel, mse, 0.01, "mmse", "64qam"
    )

    np.testing.assert_array_equal(decision.recommended_rank, 1)
    np.testing.assert_allclose(decision.minimum_eigenvalue_ratio, 0.0)
    assert not np.any(decision.configured_mcs_supported)


def test_rank_mcs_uses_closed_form_per_frame_metrics() -> None:
    channel = np.zeros((1, 2, 4, 2, 2), dtype=np.complex128)
    channel[..., 0, 0] = 2.0
    channel[..., 1, 1] = 1.0
    mse = np.full((1, 2, 4, 2), 0.1)

    decision = recommend_2x2_rank_mcs(
        channel, mse, 0.1, "zf", "16qam"
    )

    np.testing.assert_allclose(decision.minimum_eigenvalue_ratio, 0.25)
    np.testing.assert_allclose(decision.rank2_bottleneck_sinr_db, 10.0)
    assert decision.recommended_rank.tolist() == [2]
