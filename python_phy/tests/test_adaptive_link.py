from openisac_phy.adaptive_link import AdaptiveLinkController, LinkMode


def test_controller_requires_three_confirmations_and_steps_up_once() -> None:
    controller = AdaptiveLinkController(upshift_confirmation_frames=3)
    desired = LinkMode(2, "64qam")

    first = controller.observe(desired)
    second = controller.observe(desired)
    third = controller.observe(desired)

    assert first.selected == LinkMode(1, "qpsk")
    assert second.selected == LinkMode(1, "qpsk")
    assert third.selected == LinkMode(1, "16qam")
    assert third.reason == "confirmed_step_upshift"


def test_controller_quality_downshift_is_immediate() -> None:
    controller = AdaptiveLinkController(initial_mode=LinkMode(2, "64qam"))

    update = controller.observe(LinkMode(1, "64qam"))

    assert update.selected == LinkMode(1, "64qam")
    assert update.reason == "quality_downshift"


def test_controller_crc_failure_forces_at_least_one_step_down() -> None:
    controller = AdaptiveLinkController(initial_mode=LinkMode(2, "256qam"))

    update = controller.observe(LinkMode(2, "256qam"), crc_failed=True)

    assert update.selected == LinkMode(2, "64qam")
    assert update.reason == "crc_fast_downshift"
