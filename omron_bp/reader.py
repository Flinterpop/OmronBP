"""High-level operations: discover, pair with, and download from a monitor."""

from __future__ import annotations

import asyncio
import logging
from dataclasses import dataclass
from datetime import datetime

from bleak import BleakClient, BleakScanner

from omron_bp import bond
from omron_bp.models import ClockLayout, DeviceLayout, Reading, clock_checksum_ok, encode_clock, is_empty_slot, parse_clock, parse_record
from omron_bp.transport import KEY_SIZE, SERVICE_UUID, OmronTransport, has_omron_service

log = logging.getLogger("omron_bp")

# OMRON monitors advertise as "BLEsmart_<16 hex digits>" (case varies).
ADVERTISED_NAME_PREFIX = "blesmart_"

# The same default key as the omblepy / UBPM projects, so a monitor paired
# here also works with those tools (and vice-versa).
DEFAULT_KEY = bytes.fromhex("deadbeaf12341234deadbeaf12341234")

CONNECT_TIMEOUT_S = 30.0
CLOCK_TOLERANCE_S = 30  # correct the monitor's clock only beyond this drift
SERVICE_POLL_ATTEMPTS = 20
SERVICE_POLL_INTERVAL_S = 0.25


@dataclass(frozen=True)
class FoundDevice:
    address: str
    name: str
    rssi: int
    is_omron: bool


async def scan(timeout_s: float, include_all: bool = False) -> list[FoundDevice]:
    """Return advertising devices, strongest signal first."""
    assert 0 < timeout_s <= 120, timeout_s
    discovered = await BleakScanner.discover(timeout=timeout_s, return_adv=True)
    found: list[FoundDevice] = []
    for device, adv in discovered.values():
        name = device.name or adv.local_name or ""
        is_omron = name.lower().startswith(ADVERTISED_NAME_PREFIX) or has_omron_service(adv.service_uuids)
        if is_omron or include_all:
            found.append(FoundDevice(address=device.address, name=name, rssi=adv.rssi, is_omron=is_omron))
    found.sort(key=lambda d: d.rssi, reverse=True)
    assert all(d.is_omron for d in found) or include_all
    return found


async def _wait_for_service(client: BleakClient) -> None:
    """The WinRT backend can report services a moment after connect()."""
    for _ in range(SERVICE_POLL_ATTEMPTS):
        if any(s.uuid.lower() == SERVICE_UUID for s in client.services):
            return
        await asyncio.sleep(SERVICE_POLL_INTERVAL_S)
    raise RuntimeError("connected, but the OMRON service is missing - is this really an OMRON monitor?")


async def pair(address: str, key: bytes = DEFAULT_KEY, others: list[str] | None = None) -> None:
    """Program ``key`` into the monitor.  It must be in pairing mode.

    The monitor answers the key-programming command with error 0x0F until
    the link is bonded, and Windows holds only one OMRON bond at a time
    (see :mod:`omron_bp.bond`), so bonds with ``others`` are released first.
    """
    assert address, "address required"
    assert len(key) == KEY_SIZE
    await bond.release_others(others or [], address)
    log.info("connecting to %s for pairing", address)
    async with BleakClient(address, timeout=CONNECT_TIMEOUT_S) as client:
        await _wait_for_service(client)
        await bond.ensure_bonded(client)
        transport = OmronTransport(client)
        await transport.pair(key)
        # A freshly paired monitor expects one session before it will sleep cleanly.
        await transport.open()
        try:
            await transport.start_session()
            await transport.end_session()
        finally:
            await transport.close()
    log.info("paired %s", address)


def split_records(layout: DeviceLayout, user_index: int, region: bytes) -> list[Reading]:
    """Decode every occupied slot of one user's record area."""
    assert len(region) % layout.record_size == 0, len(region)
    slot_count = len(region) // layout.record_size
    assert slot_count == layout.records_per_user[user_index]
    readings: list[Reading] = []
    for slot in range(slot_count):
        raw = region[slot * layout.record_size : (slot + 1) * layout.record_size]
        if is_empty_slot(layout, raw):
            continue
        try:
            readings.append(parse_record(layout, raw))
        except ValueError as exc:
            log.warning("user %d slot %d skipped: %s", user_index + 1, slot, exc)
    readings.sort(key=lambda r: r.timestamp)
    return readings


@dataclass(frozen=True)
class ClockStatus:
    """What the monitor's clock said, and what was done about it."""

    monitor_time: datetime | None  # None if the record could not be decoded
    drift_seconds: int  # monitor minus PC; 0 when unknown
    verified: bool  # the record's checksum matched, so the layout is trusted
    corrected: bool  # a new time was written


@dataclass(frozen=True)
class DownloadResult:
    per_user: list[list[Reading]]
    clock: ClockStatus | None  # None when the model has no known clock layout


async def _check_clock(transport: OmronTransport, layout: DeviceLayout, sync: bool) -> ClockStatus | None:
    """Compare the monitor's clock with the PC's and, if allowed and safe, correct it.

    Writing is only attempted when the record read back carries a valid checksum,
    which proves the layout matches this monitor.
    """
    clock = layout.clock
    if clock is None:
        return None
    record = await transport.read_eeprom(clock.read_address, clock.size, clock.size)
    verified = clock_checksum_ok(clock, record)
    log.debug("clock record %s (checksum %s)", record.hex(), "ok" if verified else "MISMATCH")
    try:
        monitor_time = parse_clock(clock, record)
    except ValueError as exc:
        log.warning("monitor clock unreadable (%s)", exc)
        return ClockStatus(None, 0, verified, False)
    now = datetime.now().replace(microsecond=0)
    drift = int((monitor_time - now).total_seconds())
    log.info("monitor clock %s, PC %s (%+d s)", monitor_time.strftime("%Y-%m-%d %H:%M:%S"), now.strftime("%H:%M:%S"), drift)
    if abs(drift) <= CLOCK_TOLERANCE_S:
        return ClockStatus(monitor_time, drift, verified, False)
    if not verified:
        log.warning("clock record checksum does not match the known layout - not correcting the clock")
        return ClockStatus(monitor_time, drift, False, False)
    if not sync:
        log.info("clock sync disabled; leaving the monitor %d s off", drift)
        return ClockStatus(monitor_time, drift, True, False)
    corrected = await _correct_clock(transport, clock, record)
    return ClockStatus(monitor_time, drift, True, corrected)


async def _correct_clock(transport: OmronTransport, clock: ClockLayout, record: bytes) -> bool:
    """Write PC time into the (verified) clock record.  Returns True once the monitor acknowledges it.

    The monitor applies settings when the session ends, so the record cannot be
    read back within the same session; the drift reported by the next read is
    the confirmation.
    """
    target = datetime.now().replace(microsecond=0)
    await transport.write_eeprom(clock.write_address, encode_clock(clock, record, target))
    log.info("monitor clock set to %s (applied when the session ends)", target.strftime("%Y-%m-%d %H:%M:%S"))
    return True


async def download(
    address: str,
    layout: DeviceLayout,
    key: bytes = DEFAULT_KEY,
    others: list[str] | None = None,
    sync_clock: bool = True,
) -> DownloadResult:
    """Read every stored reading (one list per user slot) and check the monitor's clock.

    ``others`` are the addresses of the user's other OMRON monitors; their
    Windows bonds are released so this one can be bonded (see :mod:`omron_bp.bond`).
    With ``sync_clock`` the clock is corrected when it is more than 30 s off.
    """
    assert address, "address required"
    assert len(key) == KEY_SIZE
    await bond.release_others(others or [], address)
    log.info("connecting to %s (%s)", address, layout.model)
    async with BleakClient(address, timeout=CONNECT_TIMEOUT_S) as client:
        await _wait_for_service(client)
        await bond.ensure_bonded(client)
        transport = OmronTransport(client)
        await transport.open()
        try:
            await transport.unlock(key)
            await transport.start_session()
            try:
                per_user = await _read_all_users(transport, layout)
                clock = await _check_clock(transport, layout, sync_clock)
            finally:
                # Always close the session, otherwise the monitor displays "Err".
                await transport.end_session()
        finally:
            await transport.close()
    assert len(per_user) == layout.user_count
    return DownloadResult(per_user, clock)


async def _read_all_users(transport: OmronTransport, layout: DeviceLayout) -> list[list[Reading]]:
    per_user: list[list[Reading]] = []
    for user_index in range(layout.user_count):
        start, size = layout.user_region(user_index)
        log.info("reading user %d (%d bytes)", user_index + 1, size)
        region = await transport.read_eeprom(start, size, layout.read_block_size)
        per_user.append(split_records(layout, user_index, region))
    return per_user
