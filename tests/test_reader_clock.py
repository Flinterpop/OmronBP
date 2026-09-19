"""Clock check/sync decision logic over an in-memory EEPROM (no Bluetooth)."""

import asyncio
from dataclasses import replace
from datetime import datetime, timedelta

import pytest

from omron_bp import reader
from omron_bp.cli import _print_clock
from omron_bp.models import HEM_7342T, ClockLayout, encode_clock, parse_clock

LIVE_TEN = bytes.fromhex("c8a80000000000001a0912140f12da00")  # captured: 2026-09-18 20:15:18


class FakeTransport:
    """Stands in for OmronTransport: a byte array with the two EEPROM primitives."""

    def __init__(self) -> None:
        self.memory = bytearray(b"\xff" * 0x1000)
        self.writes: list[tuple[int, bytes]] = []

    async def read_eeprom(self, address: int, size: int, block_size: int) -> bytes:
        assert block_size > 0
        return bytes(self.memory[address : address + size])

    async def write_eeprom(self, address: int, data: bytes) -> None:
        self.writes.append((address, data))
        self.memory[address : address + len(data)] = data


def clock() -> ClockLayout:
    assert HEM_7342T.clock is not None
    return HEM_7342T.clock


def transport_with(record: bytes) -> FakeTransport:
    t = FakeTransport()
    t.memory[clock().read_address : clock().read_address + len(record)] = record
    return t


def check(t: FakeTransport, sync: bool) -> reader.ClockStatus:
    status = asyncio.run(reader._check_clock(t, HEM_7342T, sync))  # type: ignore[arg-type]
    assert status is not None
    return status


def test_drifted_clock_is_corrected_and_prefix_preserved() -> None:
    t = transport_with(LIVE_TEN)
    status = check(t, sync=True)
    assert status.verified and status.corrected and status.monitor_time == datetime(2026, 9, 18, 20, 15, 18)
    assert len(t.writes) == 1 and t.writes[0][0] == clock().write_address
    written = t.writes[0][1]
    assert written[:2] == LIVE_TEN[:2]
    assert abs(parse_clock(clock(), written) - datetime.now()) < timedelta(seconds=5)


def test_sync_disabled_only_reports() -> None:
    t = transport_with(LIVE_TEN)
    status = check(t, sync=False)
    assert status.verified and not status.corrected and t.writes == []


def test_bad_checksum_never_writes() -> None:
    broken = bytearray(LIVE_TEN)
    broken[clock().checksum_offset] ^= 1
    t = transport_with(bytes(broken))
    status = check(t, sync=True)
    assert not status.verified and not status.corrected and t.writes == []


def test_within_tolerance_is_left_alone() -> None:
    now = datetime.now().replace(microsecond=0) - timedelta(seconds=10)
    t = transport_with(encode_clock(clock(), LIVE_TEN, now))
    status = check(t, sync=True)
    assert status.verified and not status.corrected and abs(status.drift_seconds) <= reader.CLOCK_TOLERANCE_S
    assert t.writes == []


def test_unreadable_record_is_reported() -> None:
    bad = bytearray(LIVE_TEN)
    bad[clock().month] = 13
    t = transport_with(bytes(bad))
    status = check(t, sync=True)
    assert status.monitor_time is None and not status.corrected and t.writes == []


def test_model_without_clock_layout() -> None:
    layout = replace(HEM_7342T, clock=None)
    assert asyncio.run(reader._check_clock(FakeTransport(), layout, True)) is None  # type: ignore[arg-type]


@pytest.mark.parametrize(
    ("status", "expected"),
    [
        (None, "no known clock layout"),
        (reader.ClockStatus(None, 0, False, False), "could not be read"),
        (reader.ClockStatus(datetime(2026, 9, 18), -3502, True, True), "was -3502 s off, set to PC time"),
        (reader.ClockStatus(datetime(2026, 9, 18), 4, True, False), "OK (+4 s)"),
        (reader.ClockStatus(datetime(2026, 9, 18), 400, False, False), "NOT corrected (record layout unverified"),
        (reader.ClockStatus(datetime(2026, 9, 18), 400, True, False), "not corrected (--no-clock-sync)"),
    ],
)
def test_clock_summary_lines(status: reader.ClockStatus | None, expected: str, capsys: pytest.CaptureFixture[str]) -> None:
    _print_clock(status)
    assert expected in capsys.readouterr().out
