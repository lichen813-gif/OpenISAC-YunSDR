"""Command-line runner for the OpenISAC Python PHY model."""

from __future__ import annotations

import argparse
import csv
import json
from dataclasses import replace
from pathlib import Path

import yaml

from .config import SimulationConfig
from .simulation import simulate_mimo_ofdm


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="OpenISAC Python MIMO-OFDM simulator")
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--snr-db", help="optional comma-separated SNR override")
    parser.add_argument("--output", type=Path, default=Path("results/mimo_phy.csv"))
    return parser.parse_args()


def main() -> None:
    args = _parse_args()
    with args.config.open("r", encoding="utf-8") as handle:
        values = yaml.safe_load(handle) or {}
    base = SimulationConfig.from_mapping(values)
    configured_snrs = values.get("simulation", {}).get("snr_db", base.snr_db)
    if args.snr_db:
        snrs = [float(item) for item in args.snr_db.split(",") if item.strip()]
    elif isinstance(configured_snrs, list):
        snrs = [float(item) for item in configured_snrs]
    else:
        snrs = [float(configured_snrs)]
    if not snrs:
        raise ValueError("at least one SNR point is required")

    rows = []
    for index, snr_db in enumerate(snrs):
        cfg = replace(base, snr_db=snr_db, seed=base.seed + index)
        row = simulate_mimo_ofdm(cfg)
        rows.append(row)
        print(
            f"{cfg.modulation:6s} {cfg.channel:8s} {cfg.nt}x{cfg.nr} "
            f"mode={row['mode']} detector={row['detector']} "
            f"SNR={snr_db:5.1f} dB BER={row['ber']:.6g} "
            f"BLER={row['bler']:.6g} CRC={row['crc_failure_rate']:.6g} "
            f"EVM={row['evm_rms']:.6g} "
            f"timing={row['timing_success_rate']:.3f} "
            f"pilot-CPE={row['mean_pilot_phase_coherence']:.3f} "
            f"apply={row['pilot_phase_application_rate']:.1%} "
            f"CSI={row['channel_estimation_mode']} "
            f"NMSE={row['channel_estimation_nmse']:.6g} "
            f"phase={row['estimated_differential_phase_deg_mean']:.2f}/"
            f"{row['true_slot_phase_offset_deg']:.2f}deg "
            f"SFO={row['estimated_sfo_ppm_mean']:.2f}/"
            f"{row['true_sfo_ppm']:.2f}ppm "
            f"residual={row['mean_absolute_residual_sfo_ppm']:.2f}ppm "
            f"Doppler={row['maximum_doppler_hz']:.1f}Hz "
            f"model={row['doppler_model']} "
            f"intra-var={row['intrasymbol_channel_variation_nmse']:.4g} "
            f"pair-var={row['alamouti_pair_channel_variation_nmse']:.4g} "
            f"ref-coh={row['mean_phase_reference_coherence']:.3f} "
            f"rate={row['net_payload_rate_bps'] / 1e6:.3f}Mb/s "
            f"goodput={row['goodput_bps'] / 1e6:.3f}Mb/s"
        )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)
    manifest = args.output.with_suffix(".json")
    manifest.write_text(
        json.dumps({"config": values, "results": rows}, indent=2, ensure_ascii=False),
        encoding="utf-8",
    )
    print(f"Wrote {args.output.resolve()}")


if __name__ == "__main__":
    main()
