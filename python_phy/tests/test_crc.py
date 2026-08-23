from openisac_phy.crc import append_crc16, check_crc16, crc16_ccitt


def test_crc16_ccitt_standard_vector() -> None:
    assert crc16_ccitt(b"123456789") == 0x29B1
    assert check_crc16(append_crc16(b"OpenISAC"))


def test_crc_detects_corruption() -> None:
    frame = bytearray(append_crc16(b"OpenISAC"))
    frame[2] ^= 0x01
    assert not check_crc16(bytes(frame))

