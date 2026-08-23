import matplotlib
import numpy as np

matplotlib.use("Agg")
import matplotlib.pyplot as plt

from openisac_phy.plotting import draw_time_waveform


def test_time_waveform_zero_limit_draws_complete_frame() -> None:
    tx_time = np.ones((1, 3, 2, 80), dtype=np.complex128)
    rx_time = np.ones((1, 3, 2, 80), dtype=np.complex128)
    figure, axes = plt.subplots()
    try:
        draw_time_waveform(
            axes,
            tx_time,
            rx_time,
            signal="Tx0",
            component="real",
            max_samples=0,
            preamble_symbols=1,
        )
        assert len(axes.lines[0].get_xdata()) == 240
        assert "240 samples" in axes.get_title()
    finally:
        plt.close(figure)


def test_time_waveform_positive_limit_crops_display_only() -> None:
    tx_time = np.ones((1, 3, 2, 80), dtype=np.complex128)
    rx_time = np.ones((1, 3, 2, 80), dtype=np.complex128)
    figure, axes = plt.subplots()
    try:
        draw_time_waveform(
            axes,
            tx_time,
            rx_time,
            signal="Tx0",
            component="real",
            max_samples=100,
        )
        assert len(axes.lines[0].get_xdata()) == 100
        assert "100 samples" in axes.get_title()
    finally:
        plt.close(figure)
