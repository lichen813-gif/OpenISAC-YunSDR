"""Frame CRC helpers matching OpenISAC's CRC-16/CCITT-FALSE implementation."""

from __future__ import annotations


def crc16_ccitt(data: bytes) -> int:
    """CRC-16/CCITT-FALSE: poly=0x1021, init=0xffff, xorout=0."""

    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def append_crc16(data: bytes) -> bytes:
    return data + crc16_ccitt(data).to_bytes(2, byteorder="big")


def check_crc16(frame: bytes) -> bool:
    if len(frame) < 2:
        return False
    expected = int.from_bytes(frame[-2:], byteorder="big")
    return crc16_ccitt(frame[:-2]) == expected
