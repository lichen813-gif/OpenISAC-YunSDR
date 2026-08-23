#!/usr/bin/env python3
"""Run full ChannelSimulator -> BS -> UE BER/BLER validation for SISO QAM."""

from __future__ import annotations

import argparse
import csv
import os
import signal
import subprocess
import time
from pathlib import Path

import yaml

from sensing_runtime_protocol import build_control_command, make_control_dealer, make_tcp_endpoint


def load_yaml(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as handle:
        return yaml.safe_load(handle) or {}


def save_yaml(path: Path, cfg: dict) -> None:
    with path.open("w", encoding="utf-8") as handle:
        yaml.safe_dump(cfg, handle, sort_keys=False)


def send_command(port: int, command: bytes, value: int) -> None:
    sock = make_control_dealer(make_tcp_endpoint("127.0.0.1", port))
    try:
        sock.send(build_control_command(command, value))
    finally:
        sock.close(linger=500)


def read_rows(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        return []
    with path.open("r", newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def wait_epoch(path: Path, epoch_id: int, timeout_s: float) -> dict[str, str]:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        for row in read_rows(path):
            if int(row.get("epoch_id", "-1")) == epoch_id:
                return row
        time.sleep(0.2)
    raise TimeoutError(f"timed out waiting for epoch {epoch_id} in {path}")


def stop_process(process: subprocess.Popen | None) -> None:
    if process is None or process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGINT)
        process.wait(timeout=3.0)
    except (ProcessLookupError, subprocess.TimeoutExpired):
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Full-pipeline SISO QAM ChannelSimulator sweep")
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--bs-config", type=Path, default=Path("config/BS_Sim.yaml"))
    parser.add_argument("--ue-config", type=Path, default=Path("config/UE_Sim.yaml"))
    parser.add_argument("--modulation", choices=("qpsk", "16qam", "64qam", "256qam"), default="16qam")
    parser.add_argument("--channel", choices=("awgn", "multipath"), default="multipath")
    parser.add_argument("--snr-db", default="8,12,16,20,24,28")
    parser.add_argument("--payload-bytes", type=int, default=1024)
    parser.add_argument("--packets-per-point", type=int, default=256)
    parser.add_argument("--startup-wait", type=float, default=5.0)
    parser.add_argument("--settle", type=float, default=1.0)
    parser.add_argument("--drain", type=float, default=2.0)
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--output-dir", type=Path, default=Path("measurement/siso_qam_channel_sim"))
    return parser.parse_args()


def configure_measurement(cfg: dict, run_id: str, output_dir: Path, args: argparse.Namespace) -> None:
    measurement = cfg.setdefault("measurement", {})
    measurement.update(
        measurement_enable=True,
        measurement_mode="internal_prbs",
        measurement_run_id=run_id,
        measurement_output_dir=str(output_dir),
        measurement_payload_bytes=args.payload_bytes,
        measurement_prbs_seed=0x5A,
        measurement_packets_per_point=args.packets_per_point,
        measurement_max_packets_per_frame=1,
    )


def main() -> None:
    args = parse_args()
    snrs = [float(item) for item in args.snr_db.split(",") if item.strip()]
    if not snrs:
        raise ValueError("at least one SNR point is required")
    run_id = f"{args.modulation}_{args.channel}_{time.strftime('%Y%m%d_%H%M%S')}"
    run_dir = (args.output_dir / run_id).resolve()
    run_dir.mkdir(parents=True, exist_ok=True)

    bs_cfg = load_yaml(args.bs_config)
    ue_cfg = load_yaml(args.ue_config)
    bs_cfg.setdefault("downlink", {})["modulation"] = args.modulation
    ue_cfg.setdefault("downlink", {})["modulation"] = args.modulation
    simulation = bs_cfg.setdefault("simulation", {})
    simulation.update(
        enable_comm_rx=True,
        enable_sensing_rx=False,
        snr_control_enable=True,
        target_snr_db=snrs[0],
        targets=[],
        bistatic_targets=[],
    )
    simulation["comm_multipath_taps"] = (
        [{"delay_samples": 0, "gain_db": 0.0, "phase_deg": 0.0}]
        if args.channel == "awgn"
        else [
            {"delay_samples": 0, "gain_db": 0.0, "phase_deg": 0.0},
            {"delay_samples": 3, "gain_db": -4.0, "phase_deg": 45.0},
            {"delay_samples": 9, "gain_db": -8.0, "phase_deg": -80.0},
        ]
    )
    ue_cfg.setdefault("sensing", {})["bi_enabled"] = False
    configure_measurement(bs_cfg, run_id, run_dir, args)
    configure_measurement(ue_cfg, run_id, run_dir, args)
    save_yaml(run_dir / "BS.yaml", bs_cfg)
    save_yaml(run_dir / "UE.yaml", ue_cfg)

    simulator_port = int(simulation.get("control_port", 10002))
    bs_port = int(bs_cfg.get("network_output", {}).get("control_port", 9999))
    ue_port = int(ue_cfg.get("network_output", {}).get("control_port", 10001))
    bs_summary = run_dir / "bs_measurement_summary.csv"
    ue_summary = run_dir / "ue_measurement_summary.csv"
    processes: list[tuple[str, subprocess.Popen, object]] = []
    try:
        for name, command, config_name in (
            ("ChannelSimulator", args.build_dir / "ChannelSimulator", "BS.yaml"),
            ("UE", args.build_dir / "UE", None),
            ("BS", args.build_dir / "BS", None),
        ):
            log_handle = (run_dir / f"{name}.log").open("wb")
            argv = [str(command.resolve())]
            if config_name is not None:
                argv.append(config_name)
            process = subprocess.Popen(
                argv,
                cwd=run_dir,
                stdout=log_handle,
                stderr=subprocess.STDOUT,
                preexec_fn=os.setsid,
            )
            processes.append((name, process, log_handle))
            time.sleep(2.0 if name == "ChannelSimulator" else 1.0)
        time.sleep(args.startup_wait)

        for epoch_id, snr_db in enumerate(snrs, start=1):
            send_command(simulator_port, b"SNR ", int(round(snr_db * 100.0)))
            time.sleep(args.settle)
            send_command(ue_port, b"MRST", epoch_id)
            send_command(bs_port, b"MRST", epoch_id)
            wait_epoch(bs_summary, epoch_id, args.timeout)
            time.sleep(args.drain)
        send_command(ue_port, b"MRST", len(snrs) + 1)
        wait_epoch(ue_summary, len(snrs), args.timeout)
    finally:
        for _name, process, _handle in reversed(processes):
            stop_process(process)
        for _name, _process, handle in processes:
            handle.close()

    ue_rows = {int(row["epoch_id"]): row for row in read_rows(ue_summary)}
    output_rows: list[dict[str, object]] = []
    for epoch_id, snr_db in enumerate(snrs, start=1):
        row = ue_rows.get(epoch_id, {})
        output_rows.append(
            {
                "modulation": args.modulation,
                "channel": args.channel,
                "target_snr_db": snr_db,
                "estimated_snr_db": row.get("estimated_snr_db_mean", "nan"),
                "packets_expected": row.get("expected_packets", "0"),
                "packets_received": row.get("successful_packets", "0"),
                "ber_decoded": row.get("ber_decoded", "nan"),
                "bler": row.get("bler", "nan"),
                "evm_rms": row.get("evm_rms_mean", "nan"),
            }
        )
    result_path = run_dir / "sweep_summary.csv"
    with result_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(output_rows[0].keys()))
        writer.writeheader()
        writer.writerows(output_rows)
    print(f"Wrote {result_path}")


if __name__ == "__main__":
    main()
