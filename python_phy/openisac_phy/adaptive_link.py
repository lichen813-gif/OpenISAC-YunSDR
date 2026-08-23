"""Stateful next-frame rank/MCS controller with conservative hysteresis."""

from __future__ import annotations

from dataclasses import dataclass


MODULATION_BITS = {"qpsk": 2, "16qam": 4, "64qam": 6, "256qam": 8}


@dataclass(frozen=True, order=True)
class LinkMode:
    rank: int
    modulation: str

    def __post_init__(self) -> None:
        if self.rank not in {1, 2}:
            raise ValueError("rank must be 1 or 2")
        if self.modulation not in MODULATION_BITS:
            raise ValueError("unsupported modulation")

    @property
    def nominal_bits_per_re(self) -> int:
        return self.rank * MODULATION_BITS[self.modulation]


@dataclass(frozen=True)
class ControllerUpdate:
    previous: LinkMode
    desired: LinkMode
    selected: LinkMode
    reason: str
    pending_upshift_count: int


class AdaptiveLinkController:
    """Apply immediate downshifts and confirmed one-step upshifts."""

    MODES = (
        LinkMode(1, "qpsk"),
        LinkMode(1, "16qam"),
        LinkMode(2, "qpsk"),
        LinkMode(1, "64qam"),
        LinkMode(2, "16qam"),
        LinkMode(1, "256qam"),
        LinkMode(2, "64qam"),
        LinkMode(2, "256qam"),
    )

    def __init__(
        self,
        *,
        initial_mode: LinkMode = LinkMode(1, "qpsk"),
        upshift_confirmation_frames: int = 3,
    ) -> None:
        if initial_mode not in self.MODES:
            raise ValueError("initial mode is not in the supported mode table")
        if upshift_confirmation_frames <= 0:
            raise ValueError("upshift confirmation must be positive")
        self.current = initial_mode
        self.upshift_confirmation_frames = upshift_confirmation_frames
        self._pending_desired: LinkMode | None = None
        self._pending_count = 0

    @classmethod
    def _index(cls, mode: LinkMode) -> int:
        return cls.MODES.index(mode)

    def _reset_pending(self) -> None:
        self._pending_desired = None
        self._pending_count = 0

    @staticmethod
    def _confirmed_step(current: LinkMode, desired: LinkMode) -> LinkMode:
        modulation_order = ("qpsk", "16qam", "64qam", "256qam")
        current_mcs = modulation_order.index(current.modulation)
        desired_mcs = modulation_order.index(desired.modulation)
        if desired_mcs > current_mcs:
            return LinkMode(current.rank, modulation_order[current_mcs + 1])
        if current.rank != desired.rank:
            return desired
        return desired

    def observe(
        self,
        desired: LinkMode,
        *,
        crc_failed: bool = False,
        outage: bool = False,
    ) -> ControllerUpdate:
        """Consume current-frame feedback and select the following frame mode."""

        if desired not in self.MODES:
            raise ValueError("desired mode is not in the supported mode table")
        previous = self.current
        current_index = self._index(previous)
        desired_index = self._index(desired)

        if crc_failed or outage:
            selected_index = max(0, min(current_index - 1, desired_index))
            self.current = self.MODES[selected_index]
            self._reset_pending()
            reason = "crc_fast_downshift" if crc_failed else "outage_fast_downshift"
        elif desired_index < current_index:
            self.current = desired
            self._reset_pending()
            reason = "quality_downshift"
        elif desired_index == current_index:
            self._reset_pending()
            reason = "hold"
        else:
            if self._pending_desired == desired:
                self._pending_count += 1
            else:
                self._pending_desired = desired
                self._pending_count = 1
            if self._pending_count >= self.upshift_confirmation_frames:
                self.current = self._confirmed_step(previous, desired)
                self._reset_pending()
                reason = "confirmed_step_upshift"
            else:
                reason = "upshift_hysteresis"

        return ControllerUpdate(
            previous=previous,
            desired=desired,
            selected=self.current,
            reason=reason,
            pending_upshift_count=self._pending_count,
        )
