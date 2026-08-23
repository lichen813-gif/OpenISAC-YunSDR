#!/usr/bin/env python3
"""Generate and optionally display an OpenISAC STBC constellation plot."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from openisac_phy import SimulationConfig, simulate_alamouti_ofdm  # noqa: E402
from openisac_phy.config import parse_tap_string  # noqa: E402
from openisac_phy.plotting import plot_phy_overview  # noqa: E402


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Display an equalized Alamouti constellation")
    parser.add_argument(
        "--modulation",
        choices=("qpsk", "16qam", "64qam", "256qam"),
        default="64qam",
    )
    parser.add_argument("--nr", type=int, choices=(1, 2, 4, 8), default=2)
    parser.add_argument("--nt", type=int, choices=(2, 4, 8), default=2)
    parser.add_argument(
        "--mode",
        choices=("stbc", "sfbc", "spatial_multiplexing"),
        default="stbc",
    )
    parser.add_argument("--detector", choices=("zf", "mmse"), default="mmse")
    parser.add_argument(
        "--pairing",
        choices=("time", "frequency"),
        default="time",
        help="time=STBC, frequency=adjacent-subcarrier SFBC",
    )
    parser.add_argument(
        "--channel", choices=("awgn", "static", "rayleigh", "tdl"), default="rayleigh"
    )
    parser.add_argument("--doppler-hz", type=float, default=0.0)
    parser.add_argument(
        "--doppler-model", choices=("symbol", "continuous"), default="symbol"
    )
    parser.add_argument("--tx-correlation", type=float, default=0.0)
    parser.add_argument("--rx-correlation", type=float, default=0.0)
    parser.add_argument("--spatial-rank", type=int, default=0, help="0 keeps natural rank")
    parser.add_argument(
        "--taps",
        default="0:0:0,3:-4:45,9:-8:-80",
        help="TDL delay:gain_db:phase_deg entries",
    )
    parser.add_argument("--snr-db", type=float, default=25.0)
    parser.add_argument("--frames", type=int, default=300)
    parser.add_argument("--fft-size", type=int, default=1024)
    parser.add_argument("--cp-length", type=int, default=128)
    parser.add_argument("--subcarrier-spacing-hz", type=float, default=15000.0)
    parser.add_argument("--guard-left", type=int, default=0)
    parser.add_argument("--guard-right", type=int, default=0)
    parser.add_argument(
        "--dc-null", action=argparse.BooleanOptionalAction, default=True
    )
    parser.add_argument("--pilot-spacing", type=int, default=8)
    parser.add_argument("--pilot-offset", type=int, default=0)
    parser.add_argument(
        "--channel-estimation",
        choices=("perfect", "ls_linear", "ls_dft", "lmmse"),
        default="perfect",
    )
    parser.add_argument("--channel-estimation-taps", type=int, default=10)
    parser.add_argument(
        "--synchronization", action=argparse.BooleanOptionalAction, default=True
    )
    parser.add_argument(
        "--preamble", action=argparse.BooleanOptionalAction, default=True
    )
    parser.add_argument("--zc-root", type=int, default=29)
    parser.add_argument(
        "--pilot-phase-tracking", action=argparse.BooleanOptionalAction, default=True
    )
    parser.add_argument("--pilot-phase-min-coherence", type=float, default=0.9)
    parser.add_argument(
        "--phase-reference-tracking",
        action=argparse.BooleanOptionalAction,
        default=False,
    )
    parser.add_argument("--phase-reference-count", type=int, default=2)
    parser.add_argument(
        "--sfo-tracking", action=argparse.BooleanOptionalAction, default=False
    )
    parser.add_argument(
        "--sfo-resampling", action=argparse.BooleanOptionalAction, default=False
    )
    parser.add_argument(
        "--sfo-interpolator",
        choices=("sinc8", "cubic", "sinc24"),
        default="sinc8",
    )
    parser.add_argument("--timing-offset-samples", type=int, default=5)
    parser.add_argument("--timing-search-samples", type=int, default=16)
    parser.add_argument("--cfo-hz", type=float, default=500.0)
    parser.add_argument("--sfo-ppm", type=float, default=0.0)
    parser.add_argument("--slot-phase-offset-deg", type=float, default=0.0)
    parser.add_argument("--seed", type=int, default=23063)
    parser.add_argument("--max-points", type=int, default=20000)
    parser.add_argument(
        "--max-samples",
        type=int,
        default=0,
        help="time samples to draw; 0 draws the complete frame",
    )
    parser.add_argument("--waveform-channel", default="Tx0", help="Tx0, Tx1, Rx0, ...")
    parser.add_argument(
        "--waveform-component",
        choices=("i/q", "real", "imag", "magnitude"),
        default="i/q",
    )
    parser.add_argument("--channel-link", default="Tx0-Rx0", help="for example Tx1-Rx0")
    parser.add_argument("--show", action="store_true", help="open an interactive plot window")
    parser.add_argument("--output", type=Path, help="PNG output path")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    pairing = "frequency" if args.mode == "sfbc" else args.pairing
    sfbc_mode = pairing == "frequency"
    pilot_tracking_restricted = args.mode == "spatial_multiplexing" or sfbc_mode
    reference_tracking_restricted = sfbc_mode
    cfg = SimulationConfig(
        modulation=args.modulation,
        fft_size=args.fft_size,
        cp_length=args.cp_length,
        subcarrier_spacing_hz=args.subcarrier_spacing_hz,
        guard_left=args.guard_left,
        guard_right=args.guard_right,
        dc_null=args.dc_null,
        pilot_spacing=args.pilot_spacing,
        pilot_offset=args.pilot_offset,
        preamble_enable=args.preamble,
        zc_root=args.zc_root,
        synchronization_enable=args.synchronization,
        pilot_phase_tracking_enable=(
            args.pilot_phase_tracking if not pilot_tracking_restricted else False
        ),
        pilot_phase_min_coherence=args.pilot_phase_min_coherence,
        phase_reference_tracking_enable=(
            args.phase_reference_tracking if not reference_tracking_restricted else False
        ),
        phase_reference_count=args.phase_reference_count,
        sfo_tracking_enable=args.sfo_tracking if not reference_tracking_restricted else False,
        sfo_resampling_enable=(
            args.sfo_resampling if not reference_tracking_restricted else False
        ),
        sfo_resampling_interpolator=args.sfo_interpolator,
        timing_offset_samples=args.timing_offset_samples,
        timing_search_samples=args.timing_search_samples,
        cfo_hz=args.cfo_hz,
        sfo_ppm=args.sfo_ppm,
        slot_phase_offset_deg=args.slot_phase_offset_deg,
        channel_estimation=args.channel_estimation,
        channel_estimation_taps=args.channel_estimation_taps,
        frames=args.frames,
        nt=args.nt,
        nr=args.nr,
        mode=args.mode,
        layers=args.nt if args.mode == "spatial_multiplexing" else 1,
        detector=args.detector,
        pairing=pairing,
        channel=args.channel,
        doppler_hz=args.doppler_hz,
        doppler_model=args.doppler_model,
        tx_correlation=args.tx_correlation,
        rx_correlation=args.rx_correlation,
        spatial_rank=args.spatial_rank,
        channel_taps=parse_tap_string(args.taps),
        snr_db=args.snr_db,
        seed=args.seed,
    )
    result, artifacts = simulate_alamouti_ofdm(cfg, return_artifacts=True)
    modulation_name = args.modulation.upper().replace("QAM", "-QAM")
    if args.modulation == "qpsk":
        modulation_name = "QPSK"
    output = args.output or (
        ROOT.parent
        / "measurement"
        / "constellation"
        / f"{'sm_' + args.detector if args.mode == 'spatial_multiplexing' else 'stbc' if pairing == 'time' else 'sfbc'}_{args.nt}x{args.nr}_"
        f"{args.modulation}_{args.channel}_{args.snr_db:g}db.png"
    )
    coding_name = (
        f"{args.nt}-layer SM/{args.detector.upper()}"
        if args.mode == "spatial_multiplexing"
        else "Alamouti STBC"
        if pairing == "time"
        else "Alamouti SFBC"
    )
    title = (
        f"OpenISAC {args.nt}x{args.nr} {coding_name} · {modulation_name} · "
        f"{args.channel} · N={args.fft_size}, CP={args.cp_length} · "
        f"Es/N0={args.snr_db:g} dB"
    )
    saved = plot_phy_overview(
        artifacts.equalized_symbols,
        cfg.bits_per_symbol,
        artifacts.tx_time,
        artifacts.rx_time,
        artifacts.receiver_channel,
        title=title,
        signal=args.waveform_channel,
        component=args.waveform_component,
        channel_link=args.channel_link,
        pilot_indices=(
            artifacts.pilot_indices[
                artifacts.pilot_tx_assignments == int(args.channel_link[2])
            ]
            if (cfg.mode == "spatial_multiplexing" or cfg.pairing == "frequency")
            and artifacts.pilot_tx_assignments.size
            else artifacts.pilot_indices
        ),
        phase_reference_indices=artifacts.phase_reference_indices,
        reference_channel_grid=(
            artifacts.channel if cfg.channel_estimation != "perfect" else None
        ),
        channel_estimation_label=cfg.channel_estimation,
        output=output,
        show=args.show,
        max_points=args.max_points,
        max_samples=args.max_samples,
        preamble_symbols=result["preamble_symbols"],
        seed=args.seed,
    )
    print(
        f"BER={result['ber']:.8g} BLER={result['bler']:.6g} "
        f"CRC={result['crc_failure_rate']:.6g} EVM={result['evm_rms']:.6g} "
        f"timing={result['estimated_timing_offset_mean']:.3f}/"
        f"{result['true_timing_offset_samples']} samples "
        f"CFO={result['estimated_cfo_hz_mean']:.3f}/{result['true_cfo_hz']:.3f} Hz"
        f" SFO={result['estimated_sfo_ppm_mean']:.3f}/{result['true_sfo_ppm']:.3f} ppm"
        f" residual={result['mean_absolute_residual_sfo_ppm']:.3f} ppm"
        f" Doppler={result['maximum_doppler_hz']:.3f} Hz"
        f" model={result['doppler_model']}"
        f" intra-var={result['intrasymbol_channel_variation_nmse']:.6g}"
        f" pair-var={result['alamouti_pair_channel_variation_nmse']:.6g}"
        f" rate={result['net_payload_rate_bps'] / 1e6:.3f} Mb/s"
        f" goodput={result['goodput_bps'] / 1e6:.3f} Mb/s"
        f" preamble={result['preamble_symbols']} ZC(root={cfg.zc_root})"
        f" pilot-CPE={result['mean_pilot_phase_coherence']:.4f}"
        f" apply={result['pilot_phase_application_rate']:.1%}"
        f" NMSE={result['channel_estimation_nmse']:.6g}"
        f" phase={result['estimated_differential_phase_deg_mean']:.3f}/"
        f"{result['true_slot_phase_offset_deg']:.3f} deg"
        f" ref-coh={result['mean_phase_reference_coherence']:.4f}"
    )
    print(f"Constellation: {saved}")


if __name__ == "__main__":
    main()
