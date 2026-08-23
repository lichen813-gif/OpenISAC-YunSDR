import hashlib

import numpy as np
import pytest

from openisac_phy import qam
from openisac_phy.ldpc import (
    LDPC_K,
    LDPC_N,
    Ldpc5041008,
    LdpcMiniHeader,
    bch_decode_mini_header,
    bch_encode_mini_header,
    control_qpsk_labels,
    decode_control_llrs,
    decode_ldpc_payload_llrs,
    deinterleave_blocks,
    encode_ldpc_packet,
    interleave_blocks,
    marker_qpsk_labels,
    pack_mini_header,
    scramble_bits,
    soft_descramble,
    transmit_rank_flag,
    transmit_rank_from_flags,
    unpack_mini_header,
)


@pytest.fixture(scope="module")
def codec() -> Ldpc5041008:
    return Ldpc5041008()


def test_openisac_ldpc_matrices_and_systematic_mapping(codec: Ldpc5041008) -> None:
    assert codec.h.shape == (504, 1008)
    assert codec.g.shape == (504, 1008)
    assert np.all(np.sum(codec.h, axis=0) == 3)
    assert np.all(np.sum(codec.h, axis=1) == 6)
    assert np.array_equal(codec.g[:, codec.systematic_positions], np.eye(504, dtype=np.uint8))


def test_cpp_encoder_golden_vector_and_zero_syndrome(codec: Ldpc5041008) -> None:
    information_bytes = bytes(range(63))
    information_bits = np.unpackbits(
        np.frombuffer(information_bytes, dtype=np.uint8), bitorder="big"
    )
    codeword = codec.encode_bytes(information_bytes).reshape(-1)
    digest = hashlib.sha256(np.packbits(codeword, bitorder="big").tobytes()).hexdigest()

    assert digest == "829f8d6150c0bc13a310b74bd18f73a98d09fcea54d673177c72611f7bb4f189"
    assert np.count_nonzero(codec.syndrome(codeword)) == 0
    assert np.array_equal(codeword[codec.systematic_positions], information_bits)


def test_scrambler_soft_descrambler_and_block_interleaver_round_trip() -> None:
    bits = np.arange(2 * LDPC_N, dtype=np.uint8) & 1
    scrambled = scramble_bits(bits)
    interleaved = interleave_blocks(scrambled)
    deinterleaved = deinterleave_blocks(interleaved)
    llrs = np.where(deinterleaved, -7.0, 7.0)
    recovered = soft_descramble(llrs) < 0.0

    assert np.array_equal(deinterleaved, scrambled)
    assert np.array_equal(recovered.astype(np.uint8), bits)


def test_cpp_mini_header_and_control_region_golden_vector() -> None:
    header = LdpcMiniHeader(
        version=1, flags=0x08, payload_len=63, payload_blocks=1, seq=0x1234
    )
    word = pack_mini_header(header)
    labels = control_qpsk_labels(header)
    digest = hashlib.sha256(labels.astype(np.uint8).tobytes()).hexdigest()

    assert word == 0x18003F011234C527
    assert unpack_mini_header(word) == header
    assert marker_qpsk_labels()[:16].tolist() == [
        1, 3, 0, 0, 3, 2, 0, 0, 0, 2, 1, 1, 1, 1, 2, 3
    ]
    assert digest == "0b00bf28117d554431934a7cee8046420442da84dfc24c5a4be9a720c112f1d7"


def test_integrated_header_rank_flag_round_trip() -> None:
    assert transmit_rank_flag(1) == 0
    assert transmit_rank_flag(2) == 1
    assert transmit_rank_from_flags(0x08 | transmit_rank_flag(1)) == 1
    assert transmit_rank_from_flags(0x08 | transmit_rank_flag(2)) == 2


def test_bch_header_corrects_ten_errors_and_control_llrs_decode() -> None:
    header = LdpcMiniHeader(
        version=1, flags=0x0C, payload_len=126, payload_blocks=2, seq=7
    )
    codeword = bch_encode_mini_header(pack_mini_header(header))
    codeword[[0, 7, 13, 24, 38, 49, 61, 82, 101, 126]] ^= 1
    assert unpack_mini_header(bch_decode_mini_header(codeword)) == header

    control_bits = qam.labels_to_bits(control_qpsk_labels(header), 2).reshape(-1)
    decoded, metric = decode_control_llrs(np.where(control_bits, -9.0, 9.0))
    assert decoded == header
    assert metric == 1.0


def test_noiseless_ldpc_packet_round_trip_matches_cpp_pipeline(codec: Ldpc5041008) -> None:
    payload = bytes((index * 37 + 11) & 0xFF for index in range(70))
    packet = encode_ldpc_packet(payload, 6, seq=29, codec=codec)
    llrs = np.where(packet.transmitted_payload_bits, -20.0, 20.0)
    decoded, result = decode_ldpc_payload_llrs(llrs, len(payload), codec=codec)

    assert decoded == payload
    assert packet.control_qpsk_labels.size == 128
    assert packet.payload_qam_labels.size == 2 * LDPC_N // 6
    assert np.all(result.syndrome_weights == 0)


def test_layered_min_sum_reduces_awgn_bit_errors(codec: Ldpc5041008) -> None:
    rng = np.random.default_rng(123)
    information = rng.integers(0, 2, size=(64, LDPC_K), dtype=np.uint8)
    codewords = codec.encode_bits(information)
    snr_linear = 10.0 ** (-1.0 / 10.0)
    sigma = np.sqrt(1.0 / (2.0 * snr_linear))
    received = 1.0 - 2.0 * codewords.astype(np.float64)
    received += rng.normal(0.0, sigma, size=received.shape)
    llrs = 2.0 * received / (sigma * sigma)
    result = codec.decode(llrs)

    channel_ber = np.mean((llrs < 0.0) != codewords)
    decoded_ber = np.mean(result.information_bits != information)
    assert channel_ber > 0.09
    assert decoded_ber < 0.04
    assert decoded_ber < 0.4 * channel_ber
