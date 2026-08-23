#!/usr/bin/env python3
"""Generate C++-alignment vectors and exercise the Python LDPC reference."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from openisac_phy import qam  # noqa: E402
from openisac_phy.ldpc import (  # noqa: E402
    LDPC_K,
    Ldpc5041008,
    LdpcMiniHeader,
    control_qpsk_labels,
    decode_ldpc_payload_llrs,
    encode_ldpc_packet,
    pack_mini_header,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Validate Python/C++ LDPC alignment")
    parser.add_argument("--blocks", type=int, default=256)
    parser.add_argument("--snr-db", default="-2,-1,0,1,2")
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=ROOT.parent / "measurement" / "ldpc_cpp_alignment",
    )
    return parser.parse_args()


def sha256_packed_bits(bits: np.ndarray) -> str:
    packed = np.packbits(np.asarray(bits, dtype=np.uint8), bitorder="big")
    return hashlib.sha256(packed.tobytes()).hexdigest()


def main() -> None:
    args = parse_args()
    if args.blocks <= 0:
        raise ValueError("blocks must be positive")
    snrs = [float(value) for value in args.snr_db.split(",") if value.strip()]
    if not snrs:
        raise ValueError("at least one SNR point is required")

    codec = Ldpc5041008(iterations=6)
    golden_payload = bytes(range(63))
    golden_codeword = codec.encode_bytes(golden_payload).reshape(-1)
    golden_packet = encode_ldpc_packet(
        golden_payload, 6, seq=0x1234, codec=codec
    )
    golden = {
        "information_hex": golden_payload.hex(),
        "mini_header_word_hex": f"{pack_mini_header(golden_packet.header):016x}",
        "codeword_packed_sha256": sha256_packed_bits(golden_codeword),
        "transmitted_bits_packed_sha256": sha256_packed_bits(
            golden_packet.transmitted_payload_bits
        ),
        "control_labels_u8_sha256": hashlib.sha256(
            golden_packet.control_qpsk_labels.astype(np.uint8).tobytes()
        ).hexdigest(),
        "marker_first_16_labels": golden_packet.control_qpsk_labels[:16].tolist(),
        "systematic_positions_sha256": hashlib.sha256(
            codec.systematic_positions.astype("<i4").tobytes()
        ).hexdigest(),
    }

    rng = np.random.default_rng(0x4F504953)
    information = rng.integers(0, 2, size=(args.blocks, LDPC_K), dtype=np.uint8)
    codewords = codec.encode_bits(information)
    rows: list[dict[str, object]] = []
    for snr_db in snrs:
        snr_linear = 10.0 ** (snr_db / 10.0)
        sigma = np.sqrt(1.0 / (2.0 * snr_linear))
        transmitted = 1.0 - 2.0 * codewords.astype(np.float64)
        received = transmitted + rng.normal(0.0, sigma, size=transmitted.shape)
        llrs = 2.0 * received / (sigma * sigma)
        result = codec.decode(llrs)
        row = {
            "test": "bpsk_awgn",
            "snr_db": snr_db,
            "channel_ber": float(np.mean((llrs < 0.0) != codewords)),
            "post_ldpc_ber": float(np.mean(result.information_bits != information)),
            "post_ldpc_fer": float(
                np.mean(np.any(result.information_bits != information, axis=1))
            ),
            "syndrome_failure_rate": float(np.mean(result.syndrome_weights != 0)),
            "iterations": result.iterations,
        }
        rows.append(row)
        print(
            f"BPSK SNR={snr_db:5.1f} dB channel_BER={row['channel_ber']:.6g} "
            f"post_BER={row['post_ldpc_ber']:.6g} FER={row['post_ldpc_fer']:.4f} "
            f"syndrome_fail={row['syndrome_failure_rate']:.4f}"
        )

    # One formal-frame-sized 64-QAM information region: fourteen complete
    # LDPC blocks, including the transport CRC in the integrated OFDM path.
    qam_payload = bytes((index * 37 + 11) & 0xFF for index in range(14 * 63))
    qam_packet = encode_ldpc_packet(qam_payload, 6, seq=29, codec=codec)
    tx_qam = qam.modulate(qam_packet.payload_qam_labels, 6)
    qam_rows: list[dict[str, object]] = []
    for snr_db in (10.0, 15.0, 20.0, 25.0):
        noise_variance = 10.0 ** (-snr_db / 10.0)
        noise = (
            rng.normal(size=tx_qam.shape) + 1j * rng.normal(size=tx_qam.shape)
        ) * np.sqrt(noise_variance / 2.0)
        rx_qam = tx_qam + noise
        llrs = qam.max_log_llrs(rx_qam, noise_variance, 6).reshape(-1)
        decoded, result = decode_ldpc_payload_llrs(
            llrs, len(qam_payload), codec=codec
        )
        transmitted_bit_ber = float(
            np.mean((llrs < 0.0) != qam_packet.transmitted_payload_bits)
        )
        expected_information = np.unpackbits(
            np.frombuffer(qam_payload, dtype=np.uint8).reshape(14, 63),
            axis=1,
            bitorder="big",
        )
        post_ber = float(np.mean(result.information_bits != expected_information))
        row = {
            "test": "64qam_awgn_packet",
            "snr_db": snr_db,
            "channel_ber": transmitted_bit_ber,
            "post_ldpc_ber": post_ber,
            "post_ldpc_fer": float(decoded != qam_payload),
            "syndrome_failure_rate": float(np.mean(result.syndrome_weights != 0)),
            "iterations": result.iterations,
        }
        qam_rows.append(row)
        rows.append(row)
        print(
            f"64QAM SNR={snr_db:4.0f} dB channel_BER={transmitted_bit_ber:.6g} "
            f"post_BER={post_ber:.6g} packet_ok={decoded == qam_payload}"
        )

    minus_one = min(rows, key=lambda row: abs(float(row["snr_db"]) + 1.0))
    qam_15 = next(row for row in qam_rows if float(row["snr_db"]) == 15.0)
    expected_header = LdpcMiniHeader(1, 0x08, 63, 1, 0x1234)
    checks = {
        "generator_produces_zero_syndrome": not np.any(codec.syndrome(golden_codeword)),
        "encoder_codeword_matches_cpp_digest": golden["codeword_packed_sha256"]
        == "829f8d6150c0bc13a310b74bd18f73a98d09fcea54d673177c72611f7bb4f189",
        "scramble_interleave_matches_cpp_digest": golden[
            "transmitted_bits_packed_sha256"
        ]
        == "a575021d373a226dfefe0ae8b848752e62ab135e4b57b1e9d41aa0dc03d9bfcd",
        "control_region_matches_cpp_digest": golden["control_labels_u8_sha256"]
        == "0b00bf28117d554431934a7cee8046420442da84dfc24c5a4be9a720c112f1d7",
        "mini_header_matches_cpp_word": golden["mini_header_word_hex"]
        == "18003f011234c527"
        and golden_packet.header == expected_header,
        "minus_1_db_decoder_reduces_ber_by_60_percent": float(
            minus_one["post_ldpc_ber"]
        )
        < 0.4 * float(minus_one["channel_ber"]),
        "64qam_15_db_packet_round_trip": float(qam_15["post_ldpc_fer"]) == 0.0,
    }

    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    golden_path = output_dir / "golden_vectors.json"
    golden_path.write_text(json.dumps(golden, indent=2), encoding="utf-8")
    csv_path = output_dir / "results.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)
    report_path = output_dir / "report.json"
    report_path.write_text(
        json.dumps(
            {
                "ldpc": {
                    "N": 1008,
                    "K": 504,
                    "decoder": "horizontal-layered normalized min-sum",
                    "iterations": 6,
                    "llr_convention": "positive means bit 0",
                },
                "results": rows,
                "checks": checks,
                "all_checks_passed": all(checks.values()),
            },
            indent=2,
        ),
        encoding="utf-8",
    )

    os.environ.setdefault("MPLCONFIGDIR", str(ROOT / ".matplotlib"))
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    bpsk_rows = [row for row in rows if row["test"] == "bpsk_awgn"]
    fig, axes = plt.subplots(1, 2, figsize=(10.5, 4.2), constrained_layout=True)
    axes[0].semilogy(
        [float(row["snr_db"]) for row in bpsk_rows],
        [max(float(row["channel_ber"]), 1.0e-7) for row in bpsk_rows],
        marker="o",
        label="Hard channel BER",
    )
    axes[0].semilogy(
        [float(row["snr_db"]) for row in bpsk_rows],
        [max(float(row["post_ldpc_ber"]), 1.0e-7) for row in bpsk_rows],
        marker="s",
        label="Post-LDPC BER",
    )
    axes[1].semilogy(
        [float(row["snr_db"]) for row in qam_rows],
        [max(float(row["channel_ber"]), 1.0e-7) for row in qam_rows],
        marker="o",
        label="64-QAM demapper BER",
    )
    axes[1].semilogy(
        [float(row["snr_db"]) for row in qam_rows],
        [max(float(row["post_ldpc_ber"]), 1.0e-7) for row in qam_rows],
        marker="s",
        label="Post-LDPC BER",
    )
    axes[0].set_title("LDPC(1008,504) BPSK/AWGN")
    axes[1].set_title("C++ packet chain · 64-QAM/AWGN")
    for axis in axes:
        axis.set_xlabel("Es/N0 (dB)")
        axis.set_ylabel("BER")
        axis.grid(True, which="both", linestyle=":", alpha=0.6)
        axis.legend()
    figure_path = output_dir / "ldpc_alignment.png"
    fig.savefig(figure_path, dpi=180)
    plt.close(fig)

    print(f"Checks: {sum(checks.values())}/{len(checks)} passed")
    print(f"Golden {golden_path}")
    print(f"Results {csv_path}")
    print(f"Report {report_path}")
    print(f"Figure {figure_path}")
    if not all(checks.values()):
        raise SystemExit("one or more LDPC alignment checks failed")


if __name__ == "__main__":
    main()
