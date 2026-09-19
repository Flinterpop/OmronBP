from datetime import datetime

import pytest

from omron_bp.models import (
    HEM_7342T,
    HEM_7600T,
    SYSTOLIC_OFFSET,
    YEAR_OFFSET,
    DeviceLayout,
    RecordBits,
    clock_checksum_ok,
    encode_clock,
    is_empty_slot,
    parse_clock,
    parse_record,
    resolve_layout,
)


def encode_record(layout: DeviceLayout, fields: dict[str, int]) -> bytes:
    """Inverse of parse_record: pack raw field values into a record."""
    total_bits = layout.record_size * 8
    value = 0
    for name, (first, last) in vars(layout.bits).items():
        width = last - first + 1
        raw = fields[name]
        assert 0 <= raw < (1 << width), name
        value |= raw << (total_bits - (last + 1))
    return value.to_bytes(layout.record_size, layout.endianness)


SAMPLE = {
    "systolic": 132 - SYSTOLIC_OFFSET,
    "diastolic": 84,
    "pulse": 67,
    "movement": 1,
    "irregular_heartbeat": 0,
    "year": 2026 - YEAR_OFFSET,
    "month": 9,
    "day": 18,
    "hour": 7,
    "minute": 42,
    "second": 15,
}


@pytest.mark.parametrize("layout", [HEM_7600T, HEM_7342T], ids=lambda lay: lay.model)
def test_round_trip(layout: DeviceLayout) -> None:
    reading = parse_record(layout, encode_record(layout, SAMPLE))
    assert reading.systolic == 132
    assert reading.diastolic == 84
    assert reading.pulse == 67
    assert reading.movement is True
    assert reading.irregular_heartbeat is False
    assert reading.timestamp == datetime(2026, 9, 18, 7, 42, 15)


def test_hem_7600t_byte_positions() -> None:
    # Big-endian: the first four bytes are dia, sys-25, year-2000, pulse.
    raw = encode_record(HEM_7600T, SAMPLE)
    assert raw[0] == 84
    assert raw[1] == 132 - SYSTOLIC_OFFSET
    assert raw[2] == 26
    assert raw[3] == 67


def test_hem_7342t_byte_positions() -> None:
    # Little-endian: the first three bytes are sys-25, dia, pulse.
    raw = encode_record(HEM_7342T, SAMPLE)
    assert raw[0] == 132 - SYSTOLIC_OFFSET
    assert raw[1] == 84
    assert raw[2] == 67
    assert raw[3] & 0x3F == 26


def test_seconds_clamped_to_59() -> None:
    raw = encode_record(HEM_7600T, {**SAMPLE, "second": 63})
    assert parse_record(HEM_7600T, raw).timestamp.second == 59


def test_invalid_date_rejected() -> None:
    raw = encode_record(HEM_7600T, {**SAMPLE, "month": 13})
    with pytest.raises(ValueError, match="timestamp"):
        parse_record(HEM_7600T, raw)


def test_implausible_pressure_rejected() -> None:
    raw = encode_record(HEM_7342T, {**SAMPLE, "diastolic": 0})
    with pytest.raises(ValueError, match="implausible"):
        parse_record(HEM_7342T, raw)


def test_empty_slot() -> None:
    assert is_empty_slot(HEM_7600T, b"\xff" * HEM_7600T.record_size)
    assert not is_empty_slot(HEM_7600T, encode_record(HEM_7600T, SAMPLE))


def test_user_regions() -> None:
    assert HEM_7600T.user_region(0) == (0x02AC, 100 * 0x0E)
    assert HEM_7342T.user_region(1) == (0x06D8, 100 * 0x10)


@pytest.mark.parametrize(
    ("name", "model"),
    [
        ("BP7000", "HEM-7600T"),
        ("bp7000", "HEM-7600T"),
        ("Evolv", "HEM-7600T"),
        ("BP7455CAN", "HEM-7342T"),
        ("bp7450", "HEM-7342T"),
        ("hem-7342t", "HEM-7342T"),
        ("HEM_7600T", "HEM-7600T"),
        ("10 series", "HEM-7342T"),
    ],
)
def test_resolve_layout(name: str, model: str) -> None:
    assert resolve_layout(name).model == model


def test_resolve_unknown() -> None:
    with pytest.raises(KeyError, match="unknown model"):
        resolve_layout("HEM-9999T")


def test_record_bits_cover_all_fields() -> None:
    expected = {"diastolic", "systolic", "pulse", "movement", "irregular_heartbeat", "year", "month", "day", "hour", "minute", "second"}
    assert set(RecordBits.__dataclass_fields__) == expected


def test_clock_layouts_round_trip() -> None:
    for layout in (HEM_7600T, HEM_7342T):
        clock = layout.clock
        assert clock is not None
        current = bytes(range(0x10, 0x10 + clock.size))  # arbitrary existing record
        when = datetime(2026, 9, 18, 21, 7, 42)
        record = encode_clock(clock, current, when)
        assert len(record) == clock.size
        assert record[: clock.prefix] == current[: clock.prefix]
        assert clock_checksum_ok(clock, record)
        assert parse_clock(clock, record) == when
        assert record[clock.pad_offset] == 0
        broken = bytearray(record)
        broken[clock.checksum_offset] ^= 0x01
        assert not clock_checksum_ok(clock, bytes(broken))


def test_clock_field_positions_match_reference() -> None:
    when = datetime(2026, 9, 18, 21, 7, 42)
    evolv = HEM_7600T.clock
    ten = HEM_7342T.clock
    assert evolv is not None and ten is not None
    e = encode_clock(evolv, bytes(10), when)
    assert list(e[2:8]) == [9, 26, 21, 18, 42, 7]  # month, year, hour, day, second, minute
    assert e[9] == sum(e[:8]) & 0xFF
    t = encode_clock(ten, bytes(16), when)
    assert list(t[8:14]) == [26, 9, 18, 21, 7, 42]  # year, month, day, hour, minute, second
    assert t[14] == sum(t[:14]) & 0xFF and t[15] == 0


def test_invalid_clock_record_rejected() -> None:
    clock = HEM_7600T.clock
    assert clock is not None
    bad = bytearray(10)
    bad[clock.month] = 13
    with pytest.raises(ValueError, match="clock"):
        parse_clock(clock, bytes(bad))
