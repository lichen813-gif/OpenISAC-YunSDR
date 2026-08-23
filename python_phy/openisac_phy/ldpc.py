"""OpenISAC LDPC(1008,504) and packet-framing golden model.

The bit order, scrambler and 21 x 48 block interleaver match the C++
``LDPCCodec``/``LdpcPacketFraming`` implementation.  The decoder is a clear
NumPy horizontal-layered normalized min-sum reference, not a speed substitute
for AFF3CT.
"""

from __future__ import annotations

from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path

import numpy as np

from .crc import crc16_ccitt


LDPC_K = 504
LDPC_N = 1008
LDPC_M = 504
LDPC_INFO_BYTES = 63
LDPC_INTERLEAVER_ROWS = 21
LDPC_INTERLEAVER_COLS = 48


@dataclass(frozen=True)
class Alist:
    n: int
    m: int
    col_adj: tuple[np.ndarray, ...]
    row_adj: tuple[np.ndarray, ...]


@dataclass(frozen=True)
class LdpcDecodeResult:
    information_bits: np.ndarray
    codeword_bits: np.ndarray
    syndrome_weights: np.ndarray
    iterations: int


@dataclass(frozen=True)
class LdpcMiniHeader:
    version: int = 1
    flags: int = 0
    payload_len: int = 0
    payload_blocks: int = 0
    seq: int = 0


@dataclass(frozen=True)
class EncodedLdpcPacket:
    header: LdpcMiniHeader
    control_qpsk_labels: np.ndarray
    payload_qam_labels: np.ndarray
    transmitted_payload_bits: np.ndarray


def _data_lines(path: Path) -> list[str]:
    lines: list[str] = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].strip()
        if line:
            lines.append(line)
    return lines


def read_alist(path: str | Path) -> Alist:
    """Read the line-oriented alist dialect used by the C++ codec."""

    source = Path(path)
    lines = _data_lines(source)
    if len(lines) < 4:
        raise ValueError(f"invalid alist file: {source}")
    n, m = (int(value) for value in lines[0].split()[:2])
    if n <= 0 or m <= 0:
        raise ValueError("alist dimensions must be positive")

    line_index = 2  # maximum-degree line is present but not otherwise needed

    def read_degrees(count: int) -> list[int]:
        nonlocal line_index
        result: list[int] = []
        while len(result) < count:
            if line_index >= len(lines):
                raise ValueError("truncated alist degree section")
            result.extend(int(value) for value in lines[line_index].split())
            line_index += 1
        if len(result) != count or any(value < 0 for value in result):
            raise ValueError("invalid alist degree section")
        return result

    col_degrees = read_degrees(n)
    row_degrees = read_degrees(m)

    col_adj: list[np.ndarray] = []
    for degree in col_degrees:
        if line_index >= len(lines):
            raise ValueError("truncated alist column adjacency")
        values = [int(value) - 1 for value in lines[line_index].split()[:degree]]
        line_index += 1
        values = [value for value in values if value >= 0]
        if len(values) != degree or any(value >= m for value in values):
            raise ValueError("invalid alist column adjacency")
        col_adj.append(np.asarray(values, dtype=np.int32))

    row_adj: list[np.ndarray] = []
    for degree in row_degrees:
        if line_index >= len(lines):
            raise ValueError("truncated alist row adjacency")
        values = [int(value) - 1 for value in lines[line_index].split()[:degree]]
        line_index += 1
        values = [value for value in values if value >= 0]
        if len(values) != degree or any(value >= n for value in values):
            raise ValueError("invalid alist row adjacency")
        row_adj.append(np.asarray(values, dtype=np.int32))

    return Alist(n=n, m=m, col_adj=tuple(col_adj), row_adj=tuple(row_adj))


def _default_matrix_paths() -> tuple[Path, Path]:
    repository = Path(__file__).resolve().parents[2]
    return repository / "LDPC_504_1008.alist", repository / "LDPC_504_1008G.alist"


@lru_cache(maxsize=4)
def _load_matrices(h_path: str, g_path: str) -> tuple[np.ndarray, np.ndarray, tuple[np.ndarray, ...]]:
    h_alist = read_alist(h_path)
    g_alist = read_alist(g_path)

    if (h_alist.n, h_alist.m) != (LDPC_N, LDPC_M):
        raise ValueError("OpenISAC H matrix must be 504 x 1008")
    h = np.zeros((LDPC_M, LDPC_N), dtype=np.uint8)
    for row, columns in enumerate(h_alist.row_adj):
        h[row, columns] = 1

    g = np.zeros((LDPC_K, LDPC_N), dtype=np.uint8)
    if (g_alist.n, g_alist.m) == (LDPC_N, LDPC_K):
        for row, columns in enumerate(g_alist.row_adj):
            g[row, columns] = 1
    elif (g_alist.n, g_alist.m) == (LDPC_K, LDPC_N):
        for codeword_bit, information_columns in enumerate(g_alist.row_adj):
            g[information_columns, codeword_bit] = 1
    else:
        raise ValueError("OpenISAC G matrix dimensions are incompatible")

    if np.any((g.astype(np.uint16) @ h.T.astype(np.uint16)) & 1):
        raise ValueError("OpenISAC G and H matrices are inconsistent")
    return h, g, h_alist.row_adj


class Ldpc5041008:
    """C++-compatible encoder and reference layered min-sum decoder."""

    def __init__(
        self,
        h_path: str | Path | None = None,
        g_path: str | Path | None = None,
        *,
        iterations: int = 6,
        normalization: float = 1.0,
    ) -> None:
        default_h, default_g = _default_matrix_paths()
        self.h_path = Path(h_path) if h_path is not None else default_h
        self.g_path = Path(g_path) if g_path is not None else default_g
        if iterations <= 0:
            raise ValueError("iterations must be positive")
        if not 0.0 < normalization <= 1.0:
            raise ValueError("normalization must be in (0, 1]")
        self.iterations = int(iterations)
        self.normalization = float(normalization)
        self.h, self.g, self.check_rows = _load_matrices(
            str(self.h_path.resolve()), str(self.g_path.resolve())
        )
        identity = np.eye(LDPC_K, dtype=np.uint8)
        candidates = np.flatnonzero(np.sum(self.g, axis=0) == 1)
        systematic: list[int] = []
        for information_bit in range(LDPC_K):
            matching = candidates[
                np.all(self.g[:, candidates] == identity[:, information_bit, None], axis=0)
            ]
            if matching.size != 1:
                raise ValueError("generator matrix is not uniquely systematic")
            systematic.append(int(matching[0]))
        self.systematic_positions = np.asarray(systematic, dtype=np.int32)

    @staticmethod
    def _bit_batches(bits: np.ndarray, width: int) -> tuple[np.ndarray, bool]:
        values = np.asarray(bits, dtype=np.uint8)
        was_one_dimensional = values.ndim == 1
        if was_one_dimensional:
            values = values[None, :]
        if values.ndim != 2 or values.shape[1] != width or np.any(values > 1):
            raise ValueError(f"expected binary array with final size {width}")
        return values, was_one_dimensional

    def encode_bits(self, information_bits: np.ndarray) -> np.ndarray:
        values, squeeze = self._bit_batches(information_bits, LDPC_K)
        codewords = (values.astype(np.uint16) @ self.g.astype(np.uint16)) & 1
        result = codewords.astype(np.uint8)
        return result[0] if squeeze else result

    def encode_bytes(self, information_bytes: bytes | np.ndarray) -> np.ndarray:
        if isinstance(information_bytes, bytes):
            values = np.frombuffer(information_bytes, dtype=np.uint8)
        else:
            values = np.asarray(information_bytes, dtype=np.uint8)
        if values.ndim == 1:
            if values.size % LDPC_INFO_BYTES:
                raise ValueError("byte count must be a multiple of 63")
            values = values.reshape(-1, LDPC_INFO_BYTES)
        if values.ndim != 2 or values.shape[1] != LDPC_INFO_BYTES:
            raise ValueError("expected one or more 63-byte LDPC information blocks")
        bits = np.unpackbits(values, axis=1, bitorder="big")
        return self.encode_bits(bits)

    def syndrome(self, codeword_bits: np.ndarray) -> np.ndarray:
        values, squeeze = self._bit_batches(codeword_bits, LDPC_N)
        syndrome = (values.astype(np.uint16) @ self.h.T.astype(np.uint16)) & 1
        result = syndrome.astype(np.uint8)
        return result[0] if squeeze else result

    def decode(self, llrs: np.ndarray) -> LdpcDecodeResult:
        channel = np.asarray(llrs, dtype=np.float64)
        squeeze = channel.ndim == 1
        if squeeze:
            channel = channel[None, :]
        if channel.ndim != 2 or channel.shape[1] != LDPC_N:
            raise ValueError("expected LLR array with final size 1008")
        if not np.all(np.isfinite(channel)):
            raise ValueError("LLRs must be finite")

        batch = channel.shape[0]
        beliefs = channel.copy()
        max_degree = max(len(row) for row in self.check_rows)
        messages = np.zeros((batch, LDPC_M, max_degree), dtype=np.float64)
        hard = beliefs < 0.0
        used_iterations = 0

        for iteration in range(1, self.iterations + 1):
            for check, variables in enumerate(self.check_rows):
                degree = len(variables)
                old = messages[:, check, :degree]
                extrinsic = beliefs[:, variables] - old
                magnitudes = np.abs(extrinsic)
                minimum_index = np.argmin(magnitudes, axis=1)
                minimum = magnitudes[np.arange(batch), minimum_index]
                if degree > 1:
                    second = np.partition(magnitudes, 1, axis=1)[:, 1]
                else:
                    second = minimum
                signs = np.where(extrinsic < 0.0, -1.0, 1.0)
                sign_product = np.prod(signs, axis=1)
                outgoing_magnitude = np.broadcast_to(minimum[:, None], extrinsic.shape).copy()
                outgoing_magnitude[np.arange(batch), minimum_index] = second
                updated = (
                    self.normalization
                    * sign_product[:, None]
                    * signs
                    * outgoing_magnitude
                )
                beliefs[:, variables] = extrinsic + updated
                messages[:, check, :degree] = updated

            hard = beliefs < 0.0
            used_iterations = iteration
            if np.all(np.count_nonzero(self.syndrome(hard.astype(np.uint8)), axis=1) == 0):
                break

        codeword_bits = hard.astype(np.uint8)
        information_bits = codeword_bits[:, self.systematic_positions]
        weights = np.count_nonzero(self.syndrome(codeword_bits), axis=1).astype(np.int32)
        if squeeze:
            information_bits = information_bits[0]
            codeword_bits = codeword_bits[0]
        return LdpcDecodeResult(
            information_bits=information_bits,
            codeword_bits=codeword_bits,
            syndrome_weights=weights,
            iterations=used_iterations,
        )


def scrambler_sequence(length: int, init: int = 0x5A) -> np.ndarray:
    if length < 0:
        raise ValueError("length must be non-negative")
    lfsr = init & 0xFF
    sequence = np.empty(length, dtype=np.uint8)
    for index in range(length):
        bit = ((lfsr >> 7) ^ (lfsr >> 3) ^ (lfsr >> 2) ^ (lfsr >> 1)) & 1
        sequence[index] = bit
        lfsr = ((lfsr << 1) | bit) & 0xFF
    return sequence


def scramble_bits(bits: np.ndarray, init: int = 0x5A) -> np.ndarray:
    values = np.asarray(bits, dtype=np.uint8)
    if np.any(values > 1):
        raise ValueError("bits must contain only 0 and 1")
    return values ^ scrambler_sequence(values.shape[-1], init)


def soft_descramble(llrs: np.ndarray, init: int = 0x5A) -> np.ndarray:
    values = np.asarray(llrs, dtype=np.float64)
    signs = 1.0 - 2.0 * scrambler_sequence(values.shape[-1], init)
    return values * signs


def _block_permutation(block_size: int = LDPC_N, rows: int = LDPC_INTERLEAVER_ROWS) -> tuple[np.ndarray, np.ndarray]:
    if block_size <= 0 or rows <= 0 or block_size % rows:
        raise ValueError("block size must be divisible by rows")
    columns = block_size // rows
    interleave = np.empty(block_size, dtype=np.int32)
    deinterleave = np.empty(block_size, dtype=np.int32)
    for row in range(rows):
        for column in range(columns):
            source = row * columns + column
            destination = column * rows + row
            interleave[destination] = source
            deinterleave[source] = destination
    return interleave, deinterleave


def _apply_block_map(values: np.ndarray, mapping: np.ndarray) -> np.ndarray:
    source = np.asarray(values)
    block_size = mapping.size
    if source.shape[-1] % block_size:
        raise ValueError("final dimension must be a multiple of the LDPC block size")
    shaped = source.reshape(*source.shape[:-1], -1, block_size)
    return shaped[..., mapping].reshape(source.shape).copy()


def interleave_blocks(values: np.ndarray) -> np.ndarray:
    return _apply_block_map(values, _block_permutation()[0])


def deinterleave_blocks(values: np.ndarray) -> np.ndarray:
    return _apply_block_map(values, _block_permutation()[1])


def payload_blocks_for_len(payload_len: int) -> int:
    if not 0 <= payload_len <= 0xFFFF:
        raise ValueError("payload length must fit uint16")
    return (payload_len + LDPC_INFO_BYTES - 1) // LDPC_INFO_BYTES


def payload_blocks_field_for_len(payload_len: int) -> int:
    return min(payload_blocks_for_len(payload_len), 0xFF)


def _payload_blocks_field_matches(payload_len: int, field: int) -> bool:
    blocks = payload_blocks_for_len(payload_len)
    return field == (0xFF if blocks >= 0xFF else blocks)


def modulation_flag(bits_per_symbol: int) -> int:
    return {2: 0x00, 4: 0x04, 6: 0x08, 8: 0x0C}[bits_per_symbol]


def transmit_rank_flag(transmit_rank: int) -> int:
    """Encode integrated OFDM payload rank in the mini-header low flag bit."""

    if transmit_rank not in {1, 2}:
        raise ValueError("transmit rank must be 1 or 2")
    return 0x01 if transmit_rank == 2 else 0x00


def transmit_rank_from_flags(flags: int) -> int:
    return 2 if flags & 0x01 else 1


def pack_mini_header(header: LdpcMiniHeader) -> int:
    if header.version != 1 or header.flags & ~0x0F:
        raise ValueError("unsupported mini-header version or flags")
    if not _payload_blocks_field_matches(header.payload_len, header.payload_blocks):
        raise ValueError("payload block count does not match length")
    if not 0 <= header.seq <= 0xFFFF:
        raise ValueError("sequence must fit uint16")
    prefix = (
        ((header.version & 0x0F) << 44)
        | ((header.flags & 0x0F) << 40)
        | (header.payload_len << 24)
        | (header.payload_blocks << 16)
        | header.seq
    )
    prefix_bytes = prefix.to_bytes(6, byteorder="big")
    return (prefix << 16) | crc16_ccitt(prefix_bytes)


def unpack_mini_header(word: int) -> LdpcMiniHeader:
    if not 0 <= word < (1 << 64):
        raise ValueError("mini-header word must be uint64")
    prefix = word >> 16
    if crc16_ccitt(prefix.to_bytes(6, byteorder="big")) != (word & 0xFFFF):
        raise ValueError("mini-header CRC mismatch")
    header = LdpcMiniHeader(
        version=(prefix >> 44) & 0x0F,
        flags=(prefix >> 40) & 0x0F,
        payload_len=(prefix >> 24) & 0xFFFF,
        payload_blocks=(prefix >> 16) & 0xFF,
        seq=prefix & 0xFFFF,
    )
    if header.version != 1 or header.flags & ~0x0F:
        raise ValueError("unsupported mini-header version or flags")
    if not _payload_blocks_field_matches(header.payload_len, header.payload_blocks):
        raise ValueError("mini-header block count mismatch")
    return header


@lru_cache(maxsize=1)
def _bch_tables() -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    exp = np.zeros(127, dtype=np.uint8)
    log = np.full(128, -1, dtype=np.int16)
    value = 1
    for index in range(127):
        exp[index] = value
        log[value] = index
        shifted = value << 1
        if shifted & 0x80:
            shifted ^= 0x83
        value = shifted & 0x7F
    if value != 1:
        raise RuntimeError("BCH primitive polynomial order check failed")

    def multiply(a: int, b: int) -> int:
        if a == 0 or b == 0:
            return 0
        return int(exp[(int(log[a]) + int(log[b])) % 127])

    seen = np.zeros(127, dtype=np.uint8)
    roots: list[int] = []
    for root in range(1, 21):
        exponent = root % 127
        for _ in range(7):
            if not seen[exponent]:
                seen[exponent] = 1
                roots.append(exponent)
            exponent = (exponent * 2) % 127
    polynomial = [1]
    for exponent in sorted(roots):
        root = int(exp[exponent])
        next_polynomial = [0] * (len(polynomial) + 1)
        for index, coefficient in enumerate(polynomial):
            next_polynomial[index] ^= multiply(coefficient, root)
            next_polynomial[index + 1] ^= coefficient
        polynomial = next_polynomial
    generator = np.asarray(polynomial, dtype=np.uint8)
    if generator.size != 64 or np.any(generator > 1) or generator[-1] != 1:
        raise RuntimeError("invalid BCH(127,64) generator")
    return exp, log, generator


def bch_encode_mini_header(word: int) -> np.ndarray:
    _, _, generator = _bch_tables()
    message = np.asarray([(word >> (63 - index)) & 1 for index in range(64)], dtype=np.uint8)
    work = np.zeros(127, dtype=np.uint8)
    work[63:] = message
    for position in range(126, 62, -1):
        if work[position]:
            shift = position - 63
            work[shift : shift + 64] ^= generator
    return np.concatenate((work[:63], message))


def _gf_multiply(a: int, b: int) -> int:
    if a == 0 or b == 0:
        return 0
    exp, log, _ = _bch_tables()
    return int(exp[(int(log[a]) + int(log[b])) % 127])


def _gf_divide(a: int, b: int) -> int:
    if a == 0:
        return 0
    if b == 0:
        raise ZeroDivisionError("BCH GF divide by zero")
    exp, log, _ = _bch_tables()
    return int(exp[(int(log[a]) - int(log[b])) % 127])


def _bch_syndromes(code_bits: np.ndarray) -> np.ndarray:
    exp, _, _ = _bch_tables()
    bits = np.asarray(code_bits, dtype=np.uint8)
    if bits.shape != (127,) or np.any(bits > 1):
        raise ValueError("BCH codeword must contain 127 bits")
    syndromes = np.zeros(20, dtype=np.uint8)
    active_positions = np.flatnonzero(bits)
    for syndrome in range(1, 21):
        accumulator = 0
        for position in active_positions:
            accumulator ^= int(exp[(syndrome * int(position)) % 127])
        syndromes[syndrome - 1] = accumulator
    return syndromes


def bch_decode_mini_header(code_bits: np.ndarray) -> int:
    """Correct up to ten hard BCH errors and return the packed 64-bit word."""

    bits = np.asarray(code_bits, dtype=np.uint8).copy()
    syndromes = _bch_syndromes(bits)
    if np.any(syndromes):
        locator = np.zeros(21, dtype=np.uint8)
        previous = np.zeros(21, dtype=np.uint8)
        locator[0] = 1
        previous[0] = 1
        degree = 0
        shift = 1
        scale = 1

        for index in range(20):
            discrepancy = int(syndromes[index])
            for coefficient in range(1, degree + 1):
                if locator[coefficient] and syndromes[index - coefficient]:
                    discrepancy ^= _gf_multiply(
                        int(locator[coefficient]),
                        int(syndromes[index - coefficient]),
                    )
            if discrepancy == 0:
                shift += 1
                continue

            saved = locator.copy()
            factor = _gf_divide(discrepancy, scale)
            for coefficient in range(0, locator.size - shift):
                if previous[coefficient]:
                    locator[coefficient + shift] ^= _gf_multiply(
                        factor, int(previous[coefficient])
                    )
            if 2 * degree <= index:
                degree = index + 1 - degree
                previous = saved
                scale = discrepancy
                shift = 1
            else:
                shift += 1

        if degree > 10:
            raise ValueError("BCH error count exceeds correction capability")
        exp, _, _ = _bch_tables()
        error_positions: list[int] = []
        for position in range(127):
            x = int(exp[(127 - (position % 127)) % 127])
            value = int(locator[0])
            x_power = 1
            for coefficient in range(1, degree + 1):
                x_power = _gf_multiply(x_power, x)
                if locator[coefficient]:
                    value ^= _gf_multiply(int(locator[coefficient]), x_power)
            if value == 0:
                error_positions.append(position)
        if len(error_positions) != degree:
            raise ValueError("BCH error locator has inconsistent degree")
        bits[error_positions] ^= 1
        if np.any(_bch_syndromes(bits)):
            raise ValueError("BCH correction left a non-zero syndrome")

    word = 0
    for bit in bits[63:]:
        word = (word << 1) | int(bit)
    return word


def marker_qpsk_labels() -> np.ndarray:
    labels = np.empty(64, dtype=np.int64)
    for index in range(64):
        value = (0x4F504953 ^ ((index * 0x9E3779B9) & 0xFFFFFFFF)) & 0xFFFFFFFF
        value ^= value >> 16
        value = (value * 0x7FEB352D) & 0xFFFFFFFF
        value ^= value >> 15
        value = (value * 0x846CA68B) & 0xFFFFFFFF
        value ^= value >> 16
        labels[index] = (value >> 5) & 0x03
    return labels


def control_qpsk_labels(header: LdpcMiniHeader) -> np.ndarray:
    code_bits = bch_encode_mini_header(pack_mini_header(header))
    padded = np.pad(code_bits, (0, 1))
    header_labels = (padded[0::2].astype(np.int64) << 1) | padded[1::2]
    return np.concatenate((marker_qpsk_labels(), header_labels))


def marker_metric_from_llrs(llrs: np.ndarray) -> float:
    values = np.asarray(llrs, dtype=np.float64).reshape(-1)
    if values.size != 128:
        raise ValueError("marker requires 128 QPSK bit LLRs")
    expected = np.empty(128, dtype=np.uint8)
    labels = marker_qpsk_labels()
    expected[0::2] = (labels >> 1) & 1
    expected[1::2] = labels & 1
    correlation = np.sum(np.where(expected, -values, values))
    energy = np.sum(np.abs(values))
    return 0.0 if energy <= 1.0e-9 else float(correlation / energy)


def decode_control_llrs(control_llrs: np.ndarray) -> tuple[LdpcMiniHeader, float]:
    values = np.asarray(control_llrs, dtype=np.float64).reshape(-1)
    if values.size != 256:
        raise ValueError("control region requires 256 QPSK bit LLRs")
    metric = marker_metric_from_llrs(values[:128])
    if metric < 0.50:
        raise ValueError("LDPC packet marker was not detected")
    hard_header = (values[128 : 128 + 127] < 0.0).astype(np.uint8)
    return unpack_mini_header(bch_decode_mini_header(hard_header)), metric


def encode_ldpc_packet(
    payload: bytes,
    bits_per_symbol: int,
    *,
    seq: int = 0,
    codec: Ldpc5041008 | None = None,
) -> EncodedLdpcPacket:
    """Build C++-compatible control labels and scrambled/interleaved payload labels."""

    if bits_per_symbol not in {2, 4, 6, 8} or LDPC_N % bits_per_symbol:
        raise ValueError("payload modulation must be QPSK/16/64/256-QAM")
    blocks = payload_blocks_for_len(len(payload))
    header = LdpcMiniHeader(
        version=1,
        flags=modulation_flag(bits_per_symbol),
        payload_len=len(payload),
        payload_blocks=payload_blocks_field_for_len(len(payload)),
        seq=seq,
    )
    if blocks == 0:
        transmitted = np.empty(0, dtype=np.uint8)
        labels = np.empty(0, dtype=np.int64)
    else:
        padded = payload + bytes(blocks * LDPC_INFO_BYTES - len(payload))
        active_codec = codec if codec is not None else Ldpc5041008()
        encoded = active_codec.encode_bytes(padded).reshape(-1)
        transmitted = interleave_blocks(scramble_bits(encoded))
        grouped = transmitted.reshape(-1, bits_per_symbol)
        weights = 1 << np.arange(bits_per_symbol - 1, -1, -1, dtype=np.int64)
        labels = np.sum(grouped * weights, axis=1, dtype=np.int64)
    return EncodedLdpcPacket(
        header=header,
        control_qpsk_labels=control_qpsk_labels(header),
        payload_qam_labels=labels,
        transmitted_payload_bits=transmitted,
    )


def decode_ldpc_payload_llrs(
    payload_llrs: np.ndarray,
    payload_len: int,
    *,
    codec: Ldpc5041008 | None = None,
) -> tuple[bytes, LdpcDecodeResult]:
    blocks = payload_blocks_for_len(payload_len)
    values = np.asarray(payload_llrs, dtype=np.float64).reshape(-1)
    if values.size != blocks * LDPC_N:
        raise ValueError("payload LLR count does not match mini-header")
    if blocks == 0:
        empty = np.empty((0, LDPC_K), dtype=np.uint8)
        result = LdpcDecodeResult(empty, np.empty((0, LDPC_N), dtype=np.uint8), np.empty(0, dtype=np.int32), 0)
        return b"", result
    deinterleaved = deinterleave_blocks(values)
    descrambled = soft_descramble(deinterleaved).reshape(blocks, LDPC_N)
    active_codec = codec if codec is not None else Ldpc5041008()
    result = active_codec.decode(descrambled)
    information = np.asarray(result.information_bits, dtype=np.uint8).reshape(blocks, LDPC_K)
    decoded = np.packbits(information, axis=1, bitorder="big").tobytes()
    return decoded[:payload_len], result
