import pytest

from omron_bp.bitfield import bits_to_int


def test_big_endian_msb_first() -> None:
    data = bytes([0b1010_0000, 0x00])
    assert bits_to_int(data, 0, 0, "big") == 1
    assert bits_to_int(data, 1, 1, "big") == 0
    assert bits_to_int(data, 0, 3, "big") == 0b1010


def test_big_endian_spans_bytes() -> None:
    data = bytes([0x0F, 0xF0])
    assert bits_to_int(data, 4, 11, "big") == 0xFF


def test_little_endian_reverses_bytes() -> None:
    data = bytes([0x12, 0x34])
    # As little-endian int this is 0x3412, so bits 0-7 are 0x34.
    assert bits_to_int(data, 0, 7, "little") == 0x34
    assert bits_to_int(data, 8, 15, "little") == 0x12


def test_rejects_bad_range() -> None:
    with pytest.raises(AssertionError):
        bits_to_int(b"\x00", 0, 8, "big")
    with pytest.raises(AssertionError):
        bits_to_int(b"\x00", 3, 2, "big")
