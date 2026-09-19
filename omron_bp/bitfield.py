"""Bit-field extraction from fixed-size records.

OMRON packs each stored reading into a 14- or 16-byte record with fields
that are not byte aligned.  Bit positions are counted from the most
significant bit of the record interpreted as one big integer, using the
byte order of the device (bit 0 is the MSB of that integer).  This mirrors
the convention used by the reverse-engineered layouts so they can be
transcribed directly.
"""

from typing import Literal

Endianness = Literal["big", "little"]

MAX_RECORD_BYTES = 64


def bits_to_int(data: bytes, first_bit: int, last_bit: int, endianness: Endianness) -> int:
    """Return the integer held in bits ``first_bit..last_bit`` (inclusive) of ``data``.

    Bit 0 is the most significant bit of ``int.from_bytes(data, endianness)``.
    """
    assert 0 < len(data) <= MAX_RECORD_BYTES, f"record size {len(data)} out of range"
    total_bits = len(data) * 8
    assert 0 <= first_bit <= last_bit < total_bits, f"bit range {first_bit}..{last_bit} outside {total_bits}"
    assert endianness in ("big", "little"), endianness

    value = int.from_bytes(data, endianness)
    width = last_bit - first_bit + 1
    shift = total_bits - (last_bit + 1)
    result = (value >> shift) & ((1 << width) - 1)

    assert 0 <= result < (1 << width)
    return result
