#!/usr/bin/env python3
"""Visible, deterministic acceptance tests for the first OpenISAC STBC model."""

from __future__ import annotations

import argparse
import csv
import json
import math
import sys
from dataclasses import replace
from pathlib import Path
from typing import Any

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from openisac_phy import SimulationConfig, simulate_alamouti_ofdm  # noqa: E402
from openisac_phy.crc import append_crc16, check_crc16, crc16_ccitt  # noqa: E402
from openisac_phy.ofdm import demodulate as ofdm_demodulate  # noqa: E402
from openisac_phy.ofdm import modulate as ofdm_modulate  # noqa: E402
from openisac_phy.mimo import (  # noqa: E402
    detect_spatial_multiplexing,
    spatial_multiplexing_encode_grid,
)
from openisac_phy.preamble import build_zc_preamble, generate_zc_frequency  # noqa: E402
from openisac_phy.qam import hard_demodulate, labels_to_bits, max_log_llrs, modulate  # noqa: E402
from openisac_phy.resource_grid import deterministic_spatial_pilots  # noqa: E402
from openisac_phy.sfbc import (  # noqa: E402
    alamouti_sfbc_combine_grid,
    alamouti_sfbc_encode_grid,
)
from openisac_phy.stbc import TX_SCALE, alamouti_combine_grid, alamouti_encode_grid  # noqa: E402
from openisac_phy.synchronization import (  # noqa: E402
    build_impaired_stream,
    estimate_cp_synchronization,
    estimate_zc_synchronization,
    extract_synchronized_symbols,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Explicit OpenISAC Python PHY acceptance tests")
    parser.add_argument("--frames", type=int, default=1000, help="frames per noisy SNR point")
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=ROOT.parent / "measurement" / "python_phy_explicit_validation",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if args.frames <= 0:
        raise ValueError("frames must be positive")
    checks: list[dict[str, Any]] = []
    curves: list[dict[str, Any]] = []

    def check(category: str, name: str, passed: bool, measured: Any, expected: str) -> None:
        row = {
            "category": category,
            "name": name,
            "passed": bool(passed),
            "measured": measured,
            "expected": expected,
        }
        checks.append(row)
        status = "PASS" if passed else "FAIL"
        print(f"{status:4s} [{category:12s}] {name}: measured={measured}; expected={expected}")

    # 1. CRC is checked against the standard published check value.
    crc = crc16_ccitt(b"123456789")
    check("CRC", "CRC-16/CCITT-FALSE standard vector", crc == 0x29B1, f"0x{crc:04X}", "0x29B1")
    frame = append_crc16(b"OpenISAC")
    check("CRC", "appended frame passes", check_crc16(frame), check_crc16(frame), "True")

    # 2. Every point of every supported constellation is checked explicitly.
    for bits_per_symbol, name in ((2, "QPSK"), (4, "16-QAM"), (6, "64-QAM"), (8, "256-QAM")):
        labels = np.arange(1 << bits_per_symbol, dtype=np.int64)
        symbols = modulate(labels, bits_per_symbol)
        decided = hard_demodulate(symbols, bits_per_symbol)
        llrs = max_log_llrs(symbols, 0.1, bits_per_symbol)
        bits = labels_to_bits(labels, bits_per_symbol)
        check("QAM", f"{name} all labels hard round-trip", np.array_equal(decided, labels), int(np.count_nonzero(decided != labels)), "0 label errors")
        check("QAM", f"{name} average constellation power", np.isclose(np.mean(np.abs(symbols) ** 2), 1.0), f"{np.mean(np.abs(symbols) ** 2):.15g}", "1.0")
        llr_sign_errors = int(np.count_nonzero((llrs < 0.0) != bits.astype(bool)))
        check("QAM", f"{name} LLR sign", llr_sign_errors == 0, llr_sign_errors, "0 sign errors")

    # 3. Show the exact Alamouti code matrix for known complex inputs.
    known_symbols = np.asarray([[[1.0 + 2.0j, 3.0 - 1.0j]]])
    mapped = alamouti_encode_grid(known_symbols)[0, :, 0, :]
    expected_mapping = TX_SCALE * np.asarray(
        [[1.0 + 2.0j, 3.0 - 1.0j], [-3.0 - 1.0j, 1.0 - 2.0j]]
    )
    mapping_error = float(np.max(np.abs(mapped - expected_mapping)))
    check("STBC mapping", "known Alamouti matrix", mapping_error < 1.0e-14, f"max error {mapping_error:.3e}", "< 1e-14")
    print("     mapped matrix =")
    print(np.array2string(mapped, precision=6))

    # 4. Use a known non-trivial channel and prove exact recovery without noise.
    channel = np.asarray([[[[0.8 + 0.1j, -0.3 + 0.7j]]]])
    rx_grid = np.einsum("btkx,bkrx->btkr", alamouti_encode_grid(known_symbols), channel)
    recovered, _ = alamouti_combine_grid(rx_grid, channel, 0.0)
    recovery_error = float(np.max(np.abs(recovered - known_symbols)))
    check("STBC combine", "known 2x1 channel/no noise", recovery_error < 1.0e-14, f"max error {recovery_error:.3e}", "< 1e-14")
    print(f"     transmitted = {known_symbols.reshape(-1)}")
    print(f"     recovered   = {recovered.reshape(-1)}")

    # 4b. Repeat the visible algebra check for adjacent-frequency Alamouti.
    sfbc_symbols = np.asarray(
        [[[1.0 + 2.0j, 3.0 - 1.0j], [-2.0 + 0.5j, 0.25 + 1.5j]]]
    )
    sfbc_indices = np.asarray([2, 3])
    sfbc_grid = alamouti_sfbc_encode_grid(sfbc_symbols, sfbc_indices, 8)
    expected_sfbc = TX_SCALE * np.asarray(
        [[1.0 + 2.0j, -2.0 + 0.5j], [2.0 + 0.5j, 1.0 - 2.0j]]
    )
    sfbc_mapping_error = float(np.max(np.abs(sfbc_grid[0, 0, 2:4] - expected_sfbc)))
    check(
        "SFBC mapping",
        "known adjacent-frequency Alamouti matrix",
        sfbc_mapping_error < 1.0e-14,
        f"max error {sfbc_mapping_error:.3e}",
        "< 1e-14",
    )
    sfbc_channel = np.zeros((1, 2, 8, 1, 2), dtype=np.complex128)
    sfbc_channel[:, :, 2:4, 0, :] = np.asarray([0.8 + 0.1j, -0.3 + 0.7j])
    sfbc_rx = np.einsum("btkx,btkrx->btkr", sfbc_grid, sfbc_channel)
    sfbc_recovered, _ = alamouti_sfbc_combine_grid(
        sfbc_rx, sfbc_channel, sfbc_indices, 0.0
    )
    sfbc_recovery_error = float(np.max(np.abs(sfbc_recovered - sfbc_symbols)))
    check(
        "SFBC combine",
        "known 2x1 flat adjacent-pair channel/no noise",
        sfbc_recovery_error < 1.0e-14,
        f"max error {sfbc_recovery_error:.3e}",
        "< 1e-14",
    )

    # 5. Verify the complete unitary CP-OFDM numerical round trip.
    rng = np.random.default_rng(101)
    grid = rng.standard_normal((2, 2, 2, 64)) + 1j * rng.standard_normal((2, 2, 2, 64))
    recovered_grid = ofdm_demodulate(ofdm_modulate(grid, 16), 64, 16)
    ofdm_error = float(np.max(np.abs(recovered_grid - grid)))
    check("OFDM", "IFFT/CP/remove-CP/FFT", ofdm_error < 1.0e-12, f"max error {ofdm_error:.3e}", "< 1e-12")

    # 6. Insert a known sample delay and CFO, then visibly verify CP recovery.
    sync_stream = build_impaired_stream(
        ofdm_modulate(grid, 16),
        fft_size=64,
        timing_offset_samples=7,
        cfo_normalized=0.12,
        search_padding_samples=16,
        noise_variance=0.0,
        rng=rng,
    )
    sync_estimate = estimate_cp_synchronization(
        sync_stream,
        fft_size=64,
        cp_length=16,
        ofdm_symbols=2,
        max_search_samples=16,
    )
    synchronized = extract_synchronized_symbols(
        sync_stream,
        timing_offsets=sync_estimate.timing_offsets,
        cfo_normalized=sync_estimate.cfo_normalized,
        fft_size=64,
        cp_length=16,
        ofdm_symbols=2,
    )
    timing_errors = int(np.count_nonzero(sync_estimate.timing_offsets != 7))
    cfo_error = float(np.max(np.abs(sync_estimate.cfo_normalized - 0.12)))
    sync_sample_error = float(np.max(np.abs(synchronized - ofdm_modulate(grid, 16))))
    check("synchronization", "CP timing offset=7", timing_errors == 0, timing_errors, "0 frame errors")
    check("synchronization", "CP CFO normalized=0.12", cfo_error < 1.0e-12, f"max error {cfo_error:.3e}", "< 1e-12")
    check("synchronization", "CFO-corrected samples", sync_sample_error < 1.0e-12, f"max error {sync_sample_error:.3e}", "< 1e-12")

    # 6b. Verify the OpenISAC ZC definition and matched-filter frame timing.
    defaults = SimulationConfig()
    check(
        "ZC preamble",
        "OpenISAC default FFT/CP/root",
        (defaults.fft_size, defaults.cp_length, defaults.zc_root) == (1024, 128, 29),
        f"N={defaults.fft_size}, CP={defaults.cp_length}, root={defaults.zc_root}",
        "N=1024, CP=128, root=29",
    )
    zc = generate_zc_frequency(64, 29)
    zc_index = np.arange(64)
    zc_expected = np.exp(-1j * np.pi * 29 * zc_index**2 / 64)
    zc_error = float(np.max(np.abs(zc - zc_expected)))
    check(
        "ZC preamble",
        "C++-compatible frequency sequence",
        zc_error < 1.0e-12,
        f"max error {zc_error:.3e}",
        "< 1e-12",
    )
    _, zc_preamble = build_zc_preamble(2, 1, 64, 16, 29)
    zc_frame = np.concatenate((zc_preamble, ofdm_modulate(grid[:, :, :1, :], 16)), axis=1)
    zc_stream = build_impaired_stream(
        zc_frame,
        fft_size=64,
        timing_offset_samples=7,
        cfo_normalized=0.12,
        search_padding_samples=16,
        noise_variance=0.0,
        rng=rng,
    )
    zc_estimate = estimate_zc_synchronization(
        zc_stream,
        zc_preamble[0, 0, 0],
        fft_size=64,
        cp_length=16,
        ofdm_symbols=3,
        max_search_samples=16,
    )
    zc_timing_errors = int(np.count_nonzero(zc_estimate.timing_offsets != 7))
    zc_cfo_error = float(np.max(np.abs(zc_estimate.cfo_normalized - 0.12)))
    check(
        "ZC preamble",
        "matched-filter timing offset=7",
        zc_timing_errors == 0,
        zc_timing_errors,
        "0 frame errors",
    )
    check(
        "ZC preamble",
        "CP CFO normalized=0.12",
        zc_cfo_error < 1.0e-12,
        f"max error {zc_cfo_error:.3e}",
        "< 1e-12",
    )

    # 7. End-to-end noiseless cases include frame CRC and high-order QAM.
    for nr, modulation in ((1, "qpsk"), (1, "16qam"), (2, "64qam"), (2, "256qam")):
        result = simulate_alamouti_ofdm(
            SimulationConfig(
                modulation=modulation,
                fft_size=64,
                cp_length=16,
                frames=32,
                nr=nr,
                channel="rayleigh",
                snr_db=float("inf"),
                seed=200 + nr,
            )
        )
        passed = result["bit_errors"] == result["block_errors"] == result["crc_failures"] == 0
        measured = f"bit={result['bit_errors']}, block={result['block_errors']}, crc={result['crc_failures']}"
        check("end-to-end", f"2x{nr} {modulation} Rayleigh/no noise", passed, measured, "all zero")

    tdl_result = simulate_alamouti_ofdm(
        SimulationConfig(
            modulation="256qam",
            fft_size=64,
            cp_length=16,
            frames=32,
            nr=2,
            channel="tdl",
            dc_null=True,
            pilot_spacing=8,
            synchronization_enable=True,
            pilot_phase_tracking_enable=True,
            timing_offset_samples=5,
            timing_search_samples=16,
            cfo_hz=500.0,
            snr_db=float("inf"),
            seed=211,
        )
    )
    tdl_passed = (
        tdl_result["bit_errors"]
        == tdl_result["block_errors"]
        == tdl_result["crc_failures"]
        == 0
    )
    check(
        "end-to-end",
        "2x2 256qam TDL/no noise",
        tdl_passed,
        f"bit={tdl_result['bit_errors']}, block={tdl_result['block_errors']}, crc={tdl_result['crc_failures']}",
        "all zero",
    )
    check(
        "resource grid",
        "64-point DC-null comb pilots",
        tdl_result["pilot_subcarriers"] == 7 and tdl_result["data_subcarriers"] == 56,
        f"pilots={tdl_result['pilot_subcarriers']}, data={tdl_result['data_subcarriers']}",
        "pilots=7, data=56",
    )
    check(
        "resource grid",
        "noiseless pilot EVM",
        tdl_result["pilot_evm_rms"] < 1.0e-12,
        f"{tdl_result['pilot_evm_rms']:.3e}",
        "< 1e-12",
    )
    check(
        "synchronization",
        "TDL end-to-end timing recovery",
        tdl_result["timing_success_rate"] == 1.0,
        f"{tdl_result['timing_success_rate']:.6g}",
        "1.0",
    )
    check(
        "synchronization",
        "TDL end-to-end CFO recovery",
        tdl_result["mean_absolute_cfo_error_hz"] < 1.0e-8,
        f"{tdl_result['mean_absolute_cfo_error_hz']:.3e} Hz",
        "< 1e-8 Hz",
    )
    check(
        "pilot tracking",
        "noiseless pilot phase coherence",
        tdl_result["mean_pilot_phase_coherence"] > 1.0 - 1.0e-12,
        f"{tdl_result['mean_pilot_phase_coherence']:.15g}",
        "> 1 - 1e-12",
    )

    tracking_base = SimulationConfig(
        modulation="256qam",
        fft_size=64,
        cp_length=16,
        dc_null=True,
        pilot_spacing=8,
        frames=500,
        nr=2,
        channel="tdl",
        synchronization_enable=True,
        pilot_phase_min_coherence=0.9,
        timing_offset_samples=0,
        timing_search_samples=0,
        cfo_hz=3000.0,
        snr_db=25.0,
        seed=23063,
    )
    without_tracking = simulate_alamouti_ofdm(
        replace(tracking_base, pilot_phase_tracking_enable=False)
    )
    with_tracking = simulate_alamouti_ofdm(
        replace(tracking_base, pilot_phase_tracking_enable=True)
    )
    check(
        "pilot tracking",
        "256qam TDL pilot tracking lowers BER",
        with_tracking["ber"] < without_tracking["ber"],
        f"off={without_tracking['ber']:.8g}, on={with_tracking['ber']:.8g}",
        "on < off",
    )
    check(
        "pilot tracking",
        "256qam TDL pilot tracking lowers EVM",
        with_tracking["evm_rms"] < without_tracking["evm_rms"],
        f"off={without_tracking['evm_rms']:.8g}, on={with_tracking['evm_rms']:.8g}",
        "on < off",
    )

    ls_static = simulate_alamouti_ofdm(
        SimulationConfig(
            modulation="256qam",
            fft_size=64,
            cp_length=16,
            dc_null=True,
            pilot_spacing=8,
            frames=32,
            nr=2,
            channel="static",
            snr_db=float("inf"),
            channel_estimation="ls_linear",
            seed=250,
        )
    )
    check(
        "LS channel",
        "flat channel/no noise end-to-end",
        ls_static["bit_errors"] == ls_static["crc_failures"] == 0,
        f"bit={ls_static['bit_errors']}, crc={ls_static['crc_failures']}",
        "all zero",
    )
    check(
        "LS channel",
        "flat channel LS NMSE",
        ls_static["channel_estimation_nmse"] < 1.0e-28,
        f"{ls_static['channel_estimation_nmse']:.3e}",
        "< 1e-28",
    )

    ls_tdl_base = SimulationConfig(
        modulation="qpsk",
        fft_size=64,
        cp_length=16,
        dc_null=True,
        frames=32,
        nr=2,
        channel="tdl",
        snr_db=float("inf"),
        channel_estimation="ls_linear",
        seed=251,
    )
    ls_tdl_dense = simulate_alamouti_ofdm(replace(ls_tdl_base, pilot_spacing=2))
    ls_tdl_sparse = simulate_alamouti_ofdm(replace(ls_tdl_base, pilot_spacing=8))
    check(
        "LS channel",
        "TDL channel exact at pilot tones",
        ls_tdl_dense["pilot_channel_nmse"] < 1.0e-28,
        f"{ls_tdl_dense['pilot_channel_nmse']:.3e}",
        "< 1e-28",
    )
    check(
        "LS channel",
        "dense pilots lower linear interpolation NMSE",
        ls_tdl_dense["channel_estimation_nmse"]
        < ls_tdl_sparse["channel_estimation_nmse"],
        f"spacing2={ls_tdl_dense['channel_estimation_nmse']:.6g}, "
        f"spacing8={ls_tdl_sparse['channel_estimation_nmse']:.6g}",
        "spacing2 < spacing8",
    )

    advanced_base = SimulationConfig(
        modulation="64qam",
        fft_size=64,
        cp_length=16,
        dc_null=True,
        pilot_spacing=4,
        frames=500,
        nr=2,
        channel="tdl",
        snr_db=20.0,
        channel_estimation_taps=10,
        seed=277,
    )
    linear_noisy = simulate_alamouti_ofdm(
        replace(advanced_base, channel_estimation="ls_linear")
    )
    dft_noisy = simulate_alamouti_ofdm(
        replace(advanced_base, channel_estimation="ls_dft")
    )
    lmmse_noisy = simulate_alamouti_ofdm(
        replace(advanced_base, channel_estimation="lmmse")
    )
    dft_noiseless = simulate_alamouti_ofdm(
        replace(advanced_base, channel_estimation="ls_dft", snr_db=float("inf"))
    )
    check(
        "DFT-LS",
        "finite-delay noiseless reconstruction",
        dft_noiseless["bit_errors"] == 0
        and dft_noiseless["channel_estimation_nmse"] < 1.0e-28,
        f"bit={dft_noiseless['bit_errors']}, "
        f"NMSE={dft_noiseless['channel_estimation_nmse']:.3e}",
        "bit=0, NMSE < 1e-28",
    )
    check(
        "DFT-LS",
        "noisy TDL NMSE below linear interpolation",
        dft_noisy["channel_estimation_nmse"]
        < linear_noisy["channel_estimation_nmse"],
        f"linear={linear_noisy['channel_estimation_nmse']:.6g}, "
        f"DFT={dft_noisy['channel_estimation_nmse']:.6g}",
        "DFT < linear",
    )
    check(
        "LMMSE",
        "noisy TDL BER below linear interpolation",
        lmmse_noisy["ber"] < linear_noisy["ber"],
        f"linear={linear_noisy['ber']:.8g}, LMMSE={lmmse_noisy['ber']:.8g}",
        "LMMSE < linear",
    )
    check(
        "LMMSE",
        "regularized NMSE below unregularized DFT-LS",
        lmmse_noisy["channel_estimation_nmse"]
        < dft_noisy["channel_estimation_nmse"],
        f"DFT={dft_noisy['channel_estimation_nmse']:.6g}, "
        f"LMMSE={lmmse_noisy['channel_estimation_nmse']:.6g}",
        "LMMSE < DFT",
    )

    phase_base = SimulationConfig(
        modulation="64qam",
        fft_size=64,
        cp_length=16,
        dc_null=True,
        pilot_spacing=2,
        frames=64,
        nr=2,
        channel="tdl",
        snr_db=float("inf"),
        channel_estimation="ls_dft",
        channel_estimation_taps=10,
        slot_phase_offset_deg=30.0,
        seed=311,
    )
    phase_off = simulate_alamouti_ofdm(
        replace(phase_base, phase_reference_tracking_enable=False)
    )
    phase_on = simulate_alamouti_ofdm(
        replace(
            phase_base,
            phase_reference_tracking_enable=True,
            phase_reference_count=2,
        )
    )
    check(
        "phase reference",
        "channel-independent 30-degree estimate",
        abs(phase_on["estimated_differential_phase_deg_mean"] - 30.0) < 1.0e-10,
        f"{phase_on['estimated_differential_phase_deg_mean']:.12g} deg",
        "30 deg",
    )
    check(
        "phase reference",
        "noiseless phase-jump recovery",
        phase_off["bit_errors"] > 0 and phase_on["bit_errors"] == 0,
        f"off={phase_off['bit_errors']}, on={phase_on['bit_errors']}",
        "off > 0, on = 0",
    )

    noisy_phase_base = replace(
        phase_base,
        pilot_spacing=4,
        frames=300,
        snr_db=20.0,
        channel_estimation="lmmse",
        pilot_phase_min_coherence=0.9,
        seed=313,
    )
    noisy_phase_off = simulate_alamouti_ofdm(
        replace(noisy_phase_base, phase_reference_tracking_enable=False)
    )
    noisy_phase_on = simulate_alamouti_ofdm(
        replace(
            noisy_phase_base,
            phase_reference_tracking_enable=True,
            phase_reference_count=2,
        )
    )
    check(
        "phase reference",
        "20-dB LMMSE BER improvement",
        noisy_phase_on["ber"] < noisy_phase_off["ber"],
        f"off={noisy_phase_off['ber']:.8g}, on={noisy_phase_on['ber']:.8g}",
        "on < off",
    )
    check(
        "phase reference",
        "20-dB reference coherence",
        noisy_phase_on["mean_phase_reference_coherence"] > 0.98,
        f"{noisy_phase_on['mean_phase_reference_coherence']:.6g}",
        "> 0.98",
    )

    # 7e. Validate time-domain SFO resampling and wideband phase-slope tracking.
    sfo_base = SimulationConfig(
        modulation="64qam",
        fft_size=1024,
        cp_length=128,
        guard_left=64,
        guard_right=63,
        dc_null=True,
        pilot_spacing=8,
        preamble_enable=True,
        synchronization_enable=True,
        timing_offset_samples=20,
        timing_search_samples=64,
        cfo_hz=1200.0,
        phase_reference_tracking_enable=True,
        phase_reference_count=8,
        pilot_phase_min_coherence=0.9,
        sfo_ppm=50.0,
        frames=100,
        nr=2,
        channel="tdl",
        snr_db=30.0,
        channel_estimation="lmmse",
        channel_estimation_taps=10,
        seed=23065,
    )
    sfo_off = simulate_alamouti_ofdm(
        replace(sfo_base, sfo_tracking_enable=False)
    )
    sfo_on = simulate_alamouti_ofdm(
        replace(sfo_base, sfo_tracking_enable=True)
    )
    check(
        "SFO tracking",
        "1024-point 50-ppm estimate",
        abs(sfo_on["estimated_sfo_ppm_mean"] - 50.0) < 5.0,
        f"{sfo_on['estimated_sfo_ppm_mean']:.6g} ppm",
        "50 +/- 5 ppm",
    )
    check(
        "SFO tracking",
        "phase-slope correction lowers BER",
        sfo_on["ber"] < sfo_off["ber"],
        f"off={sfo_off['ber']:.8g}, on={sfo_on['ber']:.8g}",
        "on < off",
    )
    check(
        "SFO tracking",
        "phase-slope correction lowers EVM",
        sfo_on["evm_rms"] < sfo_off["evm_rms"],
        f"off={sfo_off['evm_rms']:.8g}, on={sfo_on['evm_rms']:.8g}",
        "on < off",
    )
    check(
        "SFO tracking",
        "wideband phase-reference coherence",
        sfo_on["mean_phase_reference_coherence"] > 0.99,
        f"{sfo_on['mean_phase_reference_coherence']:.6g}",
        "> 0.99",
    )

    # 7f. Demonstrate loss of Alamouti orthogonality under link-dependent Doppler.
    doppler_base = SimulationConfig(
        modulation="64qam",
        fft_size=64,
        cp_length=16,
        dc_null=True,
        pilot_spacing=2,
        frames=100,
        nr=2,
        channel="tdl",
        snr_db=float("inf"),
        channel_estimation="ls_dft",
        channel_estimation_taps=10,
        seed=501,
    )
    doppler_static = simulate_alamouti_ofdm(
        replace(doppler_base, doppler_hz=0.0)
    )
    doppler_varying = simulate_alamouti_ofdm(
        replace(doppler_base, doppler_hz=500.0)
    )
    check(
        "Doppler",
        "static channel has zero slot variation",
        doppler_static["stbc_channel_variation_nmse"] == 0.0,
        f"{doppler_static['stbc_channel_variation_nmse']:.6g}",
        "0",
    )
    check(
        "Doppler",
        "500-Hz paths create Alamouti mismatch",
        doppler_varying["stbc_channel_variation_nmse"] > 0.03,
        f"{doppler_varying['stbc_channel_variation_nmse']:.6g}",
        "> 0.03",
    )
    check(
        "Doppler",
        "block variation creates noiseless errors",
        doppler_static["bit_errors"] == 0 and doppler_varying["bit_errors"] > 0,
        f"static={doppler_static['bit_errors']}, varying={doppler_varying['bit_errors']}",
        "static=0, varying>0",
    )
    continuous_doppler_base = SimulationConfig(
        modulation="64qam",
        fft_size=64,
        cp_length=16,
        dc_null=True,
        frames=100,
        nt=2,
        nr=2,
        mode="spatial_multiplexing",
        layers=2,
        detector="mmse",
        channel="rayleigh",
        channel_estimation="perfect",
        snr_db=float("inf"),
        doppler_hz=3000.0,
        seed=8801,
    )
    block_doppler = simulate_alamouti_ofdm(continuous_doppler_base)
    continuous_doppler = simulate_alamouti_ofdm(
        replace(continuous_doppler_base, doppler_model="continuous")
    )
    check(
        "continuous Doppler",
        "symbol-block perfect-CSI model hides intrasymbol ICI",
        block_doppler["bit_errors"] == 0
        and block_doppler["intrasymbol_channel_variation_nmse"] == 0.0,
        (
            f"bits={block_doppler['bit_errors']}, "
            f"intra={block_doppler['intrasymbol_channel_variation_nmse']:.6g}"
        ),
        "bits=0 and intra=0",
    )
    check(
        "continuous Doppler",
        "sample-continuous model reports strong intrasymbol variation",
        continuous_doppler["intrasymbol_channel_variation_nmse"] > 0.5,
        f"{continuous_doppler['intrasymbol_channel_variation_nmse']:.6g}",
        "> 0.5",
    )
    check(
        "continuous Doppler",
        "3000-Hz paths rotate over 60 degrees inside FFT window",
        continuous_doppler["maximum_intrasymbol_phase_rotation_deg"] > 60.0,
        f"{continuous_doppler['maximum_intrasymbol_phase_rotation_deg']:.3f} deg",
        "> 60 deg",
    )
    check(
        "continuous Doppler",
        "intrasymbol ICI creates visible noiseless BER",
        continuous_doppler["ber"] > 0.1,
        f"{continuous_doppler['ber']:.8g}",
        "> 0.1",
    )
    check(
        "continuous Doppler",
        "intrasymbol ICI creates visible noiseless EVM",
        continuous_doppler["evm_rms"] > 0.2,
        f"{continuous_doppler['evm_rms']:.8g}",
        "> 0.2",
    )
    sfbc_flat = simulate_alamouti_ofdm(
        SimulationConfig(
            modulation="64qam",
            fft_size=64,
            cp_length=16,
            dc_null=True,
            pairing="frequency",
            pilot_spacing=0,
            frames=100,
            nr=2,
            channel="static",
            snr_db=float("inf"),
            channel_estimation="perfect",
            seed=501,
        )
    )
    check(
        "SFBC end-to-end",
        "noiseless flat channel CRC",
        sfbc_flat["bit_errors"] == 0 and sfbc_flat["crc_failures"] == 0,
        f"bits={sfbc_flat['bit_errors']}, CRC={sfbc_flat['crc_failures']}",
        "bits=0, CRC=0",
    )
    flat_doppler_base = SimulationConfig(
        modulation="64qam",
        fft_size=64,
        cp_length=16,
        dc_null=True,
        pilot_spacing=0,
        frames=100,
        nr=2,
        channel="static",
        snr_db=float("inf"),
        channel_estimation="perfect",
        seed=501,
    )
    stbc_varying_flat = simulate_alamouti_ofdm(
        replace(flat_doppler_base, pairing="time", doppler_hz=500.0)
    )
    sfbc_varying = simulate_alamouti_ofdm(
        replace(flat_doppler_base, pairing="frequency", doppler_hz=500.0)
    )
    check(
        "STBC/SFBC",
        "SFBC is more robust to 500-Hz symbol-rate Doppler",
        sfbc_varying["ber"] < stbc_varying_flat["ber"],
        f"STBC={stbc_varying_flat['ber']:.8g}, SFBC={sfbc_varying['ber']:.8g}",
        "SFBC < STBC",
    )
    sfbc_csi_noiseless = simulate_alamouti_ofdm(
        SimulationConfig(
            modulation="qpsk",
            fft_size=128,
            cp_length=16,
            dc_null=True,
            pilot_spacing=4,
            pairing="frequency",
            channel_estimation="ls_dft",
            channel_estimation_taps=10,
            frames=6,
            nr=2,
            channel="tdl",
            snr_db=float("inf"),
            seed=2201,
        )
    )
    check(
        "SFBC CSI",
        "FDM-pilot DFT-LS TDL/no-noise recovery",
        sfbc_csi_noiseless["bit_errors"] == 0
        and sfbc_csi_noiseless["channel_estimation_nmse"] < 1.0e-25,
        (
            f"bits={sfbc_csi_noiseless['bit_errors']}, "
            f"NMSE={sfbc_csi_noiseless['channel_estimation_nmse']:.3e}"
        ),
        "bits=0, NMSE < 1e-25",
    )
    sfbc_csi_base = SimulationConfig(
        modulation="64qam",
        fft_size=256,
        cp_length=32,
        guard_left=16,
        guard_right=15,
        dc_null=True,
        pilot_spacing=4,
        pairing="frequency",
        channel_estimation="ls_dft",
        channel_estimation_taps=10,
        frames=100,
        nr=2,
        channel="tdl",
        doppler_hz=500.0,
        snr_db=30.0,
        seed=2401,
        noise_seed=2402,
    )
    sfbc_linear = simulate_alamouti_ofdm(
        replace(sfbc_csi_base, channel_estimation="ls_linear")
    )
    sfbc_lmmse = simulate_alamouti_ofdm(
        replace(sfbc_csi_base, channel_estimation="lmmse")
    )
    check(
        "SFBC CSI",
        "LMMSE lowers TDL channel NMSE versus linear interpolation",
        sfbc_lmmse["channel_estimation_nmse"]
        < sfbc_linear["channel_estimation_nmse"],
        (
            f"linear={sfbc_linear['channel_estimation_nmse']:.6g}, "
            f"LMMSE={sfbc_lmmse['channel_estimation_nmse']:.6g}"
        ),
        "LMMSE < linear",
    )
    check(
        "SFBC CSI",
        "LMMSE lowers 64-QAM BER versus linear interpolation",
        sfbc_lmmse["ber"] < sfbc_linear["ber"],
        f"linear={sfbc_linear['ber']:.8g}, LMMSE={sfbc_lmmse['ber']:.8g}",
        "LMMSE < linear",
    )

    # 7g. Two-layer spatial multiplexing with perfect-CSI ZF/MMSE detection.
    sm_symbols = np.asarray(
        [[[[1.0 + 0.5j, -0.5 + 1.0j], [0.25 - 0.75j, 1.5 + 0.25j]]]]
    )
    sm_symbols = np.repeat(sm_symbols, 2, axis=1)
    sm_channel = np.broadcast_to(np.eye(2), (1, 2, 2, 2, 2)).copy()
    sm_grid = spatial_multiplexing_encode_grid(sm_symbols, np.asarray([2, 3]), 8)
    sm_received = np.einsum(
        "btkx,btkrx->btkr", sm_grid[:, :, 2:4, :], sm_channel
    )
    for detector_name in ("zf", "mmse"):
        sm_recovered, _ = detect_spatial_multiplexing(
            sm_received, sm_channel, 0.0, detector_name
        )
        sm_error = float(np.max(np.abs(sm_recovered - sm_symbols)))
        check(
            "spatial MIMO",
            f"identity-channel {detector_name.upper()} exact recovery",
            sm_error < 1.0e-14,
            f"max error {sm_error:.3e}",
            "< 1e-14",
        )
    sm_base = SimulationConfig(
        modulation="64qam",
        fft_size=64,
        cp_length=16,
        dc_null=True,
        frames=150,
        nr=2,
        mode="spatial_multiplexing",
        layers=2,
        channel="rayleigh",
        snr_db=15.0,
        seed=77,
        channel_seed=701,
        noise_seed=702,
    )
    sm_noiseless = simulate_alamouti_ofdm(
        replace(sm_base, detector="zf", snr_db=float("inf"))
    )
    check(
        "spatial MIMO",
        "2x2 64-QAM Rayleigh/no-noise CRC",
        sm_noiseless["bit_errors"] == 0 and sm_noiseless["crc_failures"] == 0,
        f"bits={sm_noiseless['bit_errors']}, CRC={sm_noiseless['crc_failures']}",
        "bits=0, CRC=0",
    )
    sm_zf = simulate_alamouti_ofdm(replace(sm_base, detector="zf"))
    sm_mmse = simulate_alamouti_ofdm(replace(sm_base, detector="mmse"))
    check(
        "spatial MIMO",
        "MMSE lowers EVM versus ZF",
        sm_mmse["evm_rms"] < sm_zf["evm_rms"],
        f"ZF={sm_zf['evm_rms']:.6g}, MMSE={sm_mmse['evm_rms']:.6g}",
        "MMSE < ZF",
    )
    stbc_rate = simulate_alamouti_ofdm(
        replace(
            sm_base,
            mode="stbc",
            layers=1,
            pairing="time",
            detector="mmse",
        )
    )
    check(
        "spatial MIMO",
        "two layers double gross PHY rate",
        sm_mmse["gross_phy_rate_bps"] == 2.0 * stbc_rate["gross_phy_rate_bps"],
        f"STBC={stbc_rate['gross_phy_rate_bps']:.3f}, SM={sm_mmse['gross_phy_rate_bps']:.3f}",
        "SM = 2 x STBC",
    )
    visible_pilots, visible_assignments = deterministic_spatial_pilots(
        4, np.arange(-16, 17, 4), 901
    )
    pilot_orthogonal = bool(
        np.all(np.count_nonzero(visible_pilots, axis=-1) == 1)
        and np.allclose(np.sum(np.abs(visible_pilots) ** 2, axis=-1), 1.0)
        and np.array_equal(
            visible_assignments, np.arange(visible_assignments.size) % 2
        )
    )
    check(
        "spatial CSI",
        "FDM pilots alternate Tx and preserve unit total power",
        pilot_orthogonal,
        f"assignments={visible_assignments.tolist()}",
        "alternating Tx0/Tx1, power=1",
    )
    spatial_csi_base = SimulationConfig(
        modulation="64qam",
        fft_size=64,
        cp_length=16,
        dc_null=True,
        pilot_spacing=2,
        frames=200,
        nr=2,
        mode="spatial_multiplexing",
        layers=2,
        detector="mmse",
        channel="tdl",
        channel_estimation="ls_dft",
        channel_estimation_taps=10,
        snr_db=10.0,
        seed=811,
        noise_seed=812,
    )
    spatial_csi_noiseless = simulate_alamouti_ofdm(
        replace(spatial_csi_base, snr_db=float("inf"))
    )
    check(
        "spatial CSI",
        "DFT-LS TDL/no-noise end-to-end",
        spatial_csi_noiseless["bit_errors"] == 0
        and spatial_csi_noiseless["channel_estimation_nmse"] < 1.0e-25,
        f"bits={spatial_csi_noiseless['bit_errors']}, NMSE={spatial_csi_noiseless['channel_estimation_nmse']:.3e}",
        "bits=0, NMSE < 1e-25",
    )
    spatial_dft = simulate_alamouti_ofdm(spatial_csi_base)
    spatial_lmmse = simulate_alamouti_ofdm(
        replace(spatial_csi_base, channel_estimation="lmmse")
    )
    check(
        "spatial CSI",
        "LMMSE lowers noisy spatial-channel NMSE",
        spatial_lmmse["channel_estimation_nmse"]
        < spatial_dft["channel_estimation_nmse"],
        f"DFT={spatial_dft['channel_estimation_nmse']:.6g}, LMMSE={spatial_lmmse['channel_estimation_nmse']:.6g}",
        "LMMSE < DFT-LS",
    )
    for antennas in (4, 8):
        scalable = simulate_alamouti_ofdm(
            SimulationConfig(
                modulation="qpsk",
                fft_size=32,
                cp_length=8,
                dc_null=True,
                frames=4,
                nt=antennas,
                nr=antennas,
                mode="spatial_multiplexing",
                layers=antennas,
                detector="zf",
                channel="rayleigh",
                snr_db=float("inf"),
                seed=1100 + antennas,
                channel_seed=1200 + antennas,
            )
        )
        check(
            "scalable MIMO",
            f"{antennas}x{antennas} full-layer Rayleigh/no-noise",
            scalable["bit_errors"] == 0
            and scalable["crc_failures"] == 0
            and scalable["mean_channel_rank"] == antennas,
            f"bits={scalable['bit_errors']}, CRC={scalable['crc_failures']}, rank={scalable['mean_channel_rank']:.1f}",
            f"bits=0, CRC=0, rank={antennas}",
        )

    spatial_sync_base = SimulationConfig(
        modulation="64qam",
        fft_size=64,
        cp_length=16,
        dc_null=True,
        pilot_spacing=2,
        preamble_enable=True,
        synchronization_enable=True,
        timing_search_samples=16,
        channel_estimation="ls_linear",
        frames=20,
        nt=2,
        nr=2,
        mode="spatial_multiplexing",
        layers=2,
        detector="mmse",
        channel="rayleigh",
        snr_db=40.0,
        seed=1301,
        channel_seed=1302,
        noise_seed=1303,
    )
    spatial_sync_baseline = simulate_alamouti_ofdm(spatial_sync_base)
    spatial_synchronized = simulate_alamouti_ofdm(
        replace(spatial_sync_base, timing_offset_samples=7, cfo_hz=1200.0)
    )
    spatial_unsynchronized = simulate_alamouti_ofdm(
        replace(
            spatial_sync_base,
            synchronization_enable=False,
            timing_offset_samples=7,
            cfo_hz=1200.0,
        )
    )
    check(
        "spatial sync",
        "ZC finds 7-sample timing offset",
        spatial_synchronized["timing_success_rate"] == 1.0,
        spatial_synchronized["timing_success_rate"],
        "1.0",
    )
    check(
        "spatial sync",
        "CP phase estimates 1200-Hz CFO",
        spatial_synchronized["mean_absolute_cfo_error_hz"] < 25.0,
        f"MAE={spatial_synchronized['mean_absolute_cfo_error_hz']:.3f} Hz",
        "MAE < 25 Hz",
    )
    check(
        "spatial sync",
        "synchronization restores spatial-MIMO goodput",
        spatial_synchronized["ber"] < spatial_unsynchronized["ber"]
        and spatial_synchronized["goodput_bps"]
        >= 0.9 * spatial_sync_baseline["goodput_bps"],
        (
            f"BER {spatial_unsynchronized['ber']:.4g}->{spatial_synchronized['ber']:.4g}, "
            f"goodput={spatial_synchronized['goodput_bps']/1e6:.3f} Mb/s"
        ),
        "BER reduced and goodput >= 90% baseline",
    )
    spatial_sfo_base = SimulationConfig(
        modulation="64qam",
        fft_size=256,
        cp_length=32,
        guard_left=16,
        guard_right=15,
        dc_null=True,
        pilot_spacing=4,
        preamble_enable=True,
        synchronization_enable=True,
        phase_reference_tracking_enable=True,
        phase_reference_count=16,
        pilot_phase_min_coherence=0.8,
        sfo_tracking_enable=True,
        sfo_resampling_interpolator="sinc24",
        sfo_ppm=500.0,
        channel_estimation="ls_linear",
        frames=40,
        nt=2,
        nr=2,
        mode="spatial_multiplexing",
        layers=2,
        detector="mmse",
        channel="rayleigh",
        snr_db=40.0,
        seed=23063,
    )
    spatial_sfo_phase_only = simulate_alamouti_ofdm(spatial_sfo_base)
    spatial_sfo_closed_loop = simulate_alamouti_ofdm(
        replace(spatial_sfo_base, sfo_resampling_enable=True)
    )
    check(
        "spatial SFO",
        "phase references estimate 500-ppm SFO",
        spatial_sfo_closed_loop["mean_absolute_sfo_error_ppm"] < 50.0,
        (
            f"estimate={spatial_sfo_closed_loop['estimated_sfo_ppm_mean']:.3f} ppm, "
            f"MAE={spatial_sfo_closed_loop['mean_absolute_sfo_error_ppm']:.3f} ppm"
        ),
        "MAE < 50 ppm",
    )
    check(
        "spatial SFO",
        "coherence gate applies closed-loop resampling to every frame",
        spatial_sfo_closed_loop["sfo_resampling_application_rate"] == 1.0,
        f"rate={spatial_sfo_closed_loop['sfo_resampling_application_rate']:.1%}",
        "100%",
    )
    check(
        "spatial SFO",
        "estimated-SFO resampling lowers BER and EVM",
        spatial_sfo_closed_loop["ber"] < 0.6 * spatial_sfo_phase_only["ber"]
        and spatial_sfo_closed_loop["evm_rms"]
        < 0.85 * spatial_sfo_phase_only["evm_rms"],
        (
            f"BER {spatial_sfo_phase_only['ber']:.6g}->"
            f"{spatial_sfo_closed_loop['ber']:.6g}, EVM "
            f"{spatial_sfo_phase_only['evm_rms']:.6g}->"
            f"{spatial_sfo_closed_loop['evm_rms']:.6g}"
        ),
        "BER < 60% and EVM < 85% of phase-only result",
    )
    check(
        "spatial SFO",
        "second-pass residual is bounded and CRC improves",
        spatial_sfo_closed_loop["mean_absolute_residual_sfo_ppm"] < 100.0
        and spatial_sfo_closed_loop["crc_failure_rate"]
        < spatial_sfo_phase_only["crc_failure_rate"],
        (
            f"residual={spatial_sfo_closed_loop['mean_absolute_residual_sfo_ppm']:.3f} ppm, "
            f"CRC {spatial_sfo_phase_only['crc_failure_rate']:.1%}->"
            f"{spatial_sfo_closed_loop['crc_failure_rate']:.1%}"
        ),
        "residual < 100 ppm and CRC failure rate reduced",
    )
    realtime_sfo_base = replace(
        spatial_sfo_base,
        fft_size=1024,
        cp_length=128,
        guard_left=64,
        guard_right=63,
        phase_reference_count=32,
        sfo_ppm=50.0,
        frames=100,
        channel_seed=33063,
        noise_seed=43063,
    )
    realtime_sfo = simulate_alamouti_ofdm(
        replace(
            realtime_sfo_base,
            sfo_resampling_enable=True,
            sfo_resampling_interpolator="sinc8",
        )
    )
    reference_sfo = simulate_alamouti_ofdm(
        replace(
            realtime_sfo_base,
            sfo_resampling_enable=True,
            sfo_resampling_interpolator="sinc24",
        )
    )
    check(
        "real-time SFO",
        "eight-tap receiver cuts interpolation work by two thirds",
        realtime_sfo["sfo_interpolator_taps"] == 8
        and realtime_sfo["resampler_tap_mac_per_frame"]
        == reference_sfo["resampler_tap_mac_per_frame"] / 3.0,
        (
            f"taps={realtime_sfo['sfo_interpolator_taps']}, MAC/frame "
            f"{reference_sfo['resampler_tap_mac_per_frame']:.0f}->"
            f"{realtime_sfo['resampler_tap_mac_per_frame']:.0f}"
        ),
        "8 taps and one-third reference MAC/frame",
    )
    check(
        "real-time SFO",
        "eight-tap BER stays within 10 percent of reference",
        realtime_sfo["ber"] <= 1.10 * reference_sfo["ber"],
        f"sinc8={realtime_sfo['ber']:.8g}, sinc24={reference_sfo['ber']:.8g}",
        "sinc8 <= 1.10 x sinc24",
    )
    check(
        "real-time SFO",
        "eight-tap EVM stays within 3 percent of reference",
        realtime_sfo["evm_rms"] <= 1.03 * reference_sfo["evm_rms"],
        (
            f"sinc8={realtime_sfo['evm_rms']:.8g}, "
            f"sinc24={reference_sfo['evm_rms']:.8g}"
        ),
        "sinc8 <= 1.03 x sinc24",
    )
    check(
        "real-time SFO",
        "eight-tap profile preserves goodput and bounded residual",
        realtime_sfo["goodput_bps"] >= 0.95 * reference_sfo["goodput_bps"]
        and realtime_sfo["mean_absolute_residual_sfo_ppm"] < 10.0,
        (
            f"goodput={realtime_sfo['goodput_bps']/1e6:.3f}/"
            f"{reference_sfo['goodput_bps']/1e6:.3f} Mb/s, residual="
            f"{realtime_sfo['mean_absolute_residual_sfo_ppm']:.3f} ppm"
        ),
        ">=95% reference goodput and residual <10 ppm",
    )
    correlated_base = SimulationConfig(
        modulation="64qam",
        fft_size=64,
        cp_length=16,
        dc_null=True,
        frames=100,
        nt=2,
        nr=2,
        mode="spatial_multiplexing",
        layers=2,
        channel="rayleigh",
        snr_db=25.0,
        seed=3301,
        channel_seed=3302,
        noise_seed=3303,
    )
    independent_mmse = simulate_alamouti_ofdm(
        replace(correlated_base, detector="mmse")
    )
    correlated_mmse = simulate_alamouti_ofdm(
        replace(
            correlated_base,
            detector="mmse",
            tx_correlation=0.95,
            rx_correlation=0.95,
        )
    )
    correlated_zf = simulate_alamouti_ofdm(
        replace(
            correlated_base,
            detector="zf",
            tx_correlation=0.95,
            rx_correlation=0.95,
        )
    )
    check(
        "correlated MIMO",
        "rho=0.95 worsens mean condition number",
        correlated_mmse["mean_channel_condition_number"]
        > 5.0 * independent_mmse["mean_channel_condition_number"],
        (
            f"independent={independent_mmse['mean_channel_condition_number']:.3g}, "
            f"correlated={correlated_mmse['mean_channel_condition_number']:.3g}"
        ),
        "correlated > 5 x independent",
    )
    check(
        "correlated MIMO",
        "MMSE lowers EVM versus ZF at rho=0.95",
        correlated_mmse["evm_rms"] < correlated_zf["evm_rms"],
        f"ZF={correlated_zf['evm_rms']:.6g}, MMSE={correlated_mmse['evm_rms']:.6g}",
        "MMSE < ZF",
    )
    rank_one = simulate_alamouti_ofdm(
        replace(
            correlated_base,
            detector="mmse",
            spatial_rank=1,
            snr_db=40.0,
        )
    )
    check(
        "rank-deficient",
        "rank-one channel cannot carry two independent layers",
        rank_one["mean_channel_rank"] == 1.0
        and rank_one["rank_deficient_rate"] == 1.0
        and rank_one["goodput_bps"] == 0.0,
        (
            f"rank={rank_one['mean_channel_rank']:.1f}, "
            f"deficient={rank_one['rank_deficient_rate']:.1%}, "
            f"BER={rank_one['ber']:.6g}, goodput={rank_one['goodput_bps']:.1f}"
        ),
        "rank=1, deficient=100%, goodput=0",
    )

    # 8. Visible Monte-Carlo curves. The same random seed is used at each SNR
    # so differences are dominated by noise scaling rather than sample changes.
    scenarios = (
        ("rayleigh", 1, "qpsk", (0.0, 10.0, 20.0)),
        ("rayleigh", 1, "16qam", (0.0, 15.0, 30.0)),
        ("rayleigh", 2, "64qam", (5.0, 20.0, 35.0)),
        ("rayleigh", 2, "256qam", (10.0, 25.0, 40.0)),
        ("tdl", 2, "64qam", (15.0, 25.0, 35.0)),
    )
    for scenario_index, (channel_name, nr, modulation, snrs) in enumerate(scenarios):
        scenario_rows = []
        for snr_db in snrs:
            result = simulate_alamouti_ofdm(
                SimulationConfig(
                    modulation=modulation,
                    fft_size=64,
                    cp_length=16,
                    frames=args.frames,
                    nr=nr,
                    channel=channel_name,
                    dc_null=(channel_name == "tdl"),
                    pilot_spacing=8 if channel_name == "tdl" else 0,
                    snr_db=snr_db,
                    seed=1000 + scenario_index,
                )
            )
            row = {
                "nr": nr,
                "nt": 2,
                "modulation": modulation,
                "channel": channel_name,
                "snr_db": snr_db,
                "frames": args.frames,
                "ber": result["ber"],
                "bler": result["bler"],
                "crc_failure_rate": result["crc_failure_rate"],
                "evm_rms": result["evm_rms"],
            }
            curves.append(row)
            scenario_rows.append(row)
            print(
                f"DATA [BER curve   ] 2x{nr} {modulation:6s} {channel_name:8s} "
                f"SNR={snr_db:5.1f} dB "
                f"BER={row['ber']:.8g} BLER={row['bler']:.6g} "
                f"CRC={row['crc_failure_rate']:.6g} EVM={row['evm_rms']:.6g}"
            )
        bers = [float(row["ber"]) for row in scenario_rows]
        monotonic = all(right <= left for left, right in zip(bers, bers[1:]))
        check(
            "BER curve",
            f"2x{nr} {modulation} {channel_name} BER non-increasing",
            monotonic,
            bers,
            "monotonic with SNR",
        )

    args.output_dir.mkdir(parents=True, exist_ok=True)
    checks_path = args.output_dir / "checks.csv"
    curves_path = args.output_dir / "curves.csv"
    report_path = args.output_dir / "report.json"
    with checks_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(checks[0].keys()))
        writer.writeheader()
        writer.writerows(checks)
    with curves_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(curves[0].keys()))
        writer.writeheader()
        writer.writerows(curves)
    report_path.write_text(
        json.dumps({"checks": checks, "curves": curves}, indent=2, ensure_ascii=False),
        encoding="utf-8",
    )

    failures = [row for row in checks if not row["passed"]]
    print(f"\nSummary: {len(checks) - len(failures)}/{len(checks)} checks passed")
    print(f"Checks: {checks_path.resolve()}")
    print(f"Curves: {curves_path.resolve()}")
    print(f"Report: {report_path.resolve()}")
    if failures:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
