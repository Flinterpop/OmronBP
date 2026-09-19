"""Windows bond management.

OMRON monitors all distribute the same Identity Resolving Key when they
bond, and Windows refuses to bond a second device whose IRK matches one it
already holds (System log, BTHUSB event 35).  Because every readout needs
an encrypted (bonded) link, only one OMRON monitor can be bonded at a time,
so the reader releases the others before bonding the one it is about to
read.  Monitors accept a fresh bond in normal mode, so this costs nothing
but a second or two per switch.
"""

from __future__ import annotations

import asyncio
import logging
import sys
from typing import TYPE_CHECKING

from bleak import BleakClient
from bleak.exc import BleakError

if TYPE_CHECKING:
    from winrt.windows.devices.enumeration import DeviceInformation

log = logging.getLogger("omron_bp")

BOND_SETTLE_ATTEMPTS = 3
BOND_SETTLE_INTERVAL_S = 1.0
BOND_RETRY_ATTEMPTS = 6
BOND_RETRY_INTERVAL_S = 1.5
MAX_RELEASE = 16


def _address_int(address: str) -> int:
    digits = address.replace(":", "").replace("-", "")
    assert len(digits) == 12, address
    return int(digits, 16)


async def _device_information(client: BleakClient) -> DeviceInformation:
    from winrt.windows.devices.enumeration import DeviceInformation  # noqa: PLC0415

    requester = client._backend._requester  # type: ignore[attr-defined]  # noqa: SLF001
    assert requester is not None, "client is not connected"
    return await DeviceInformation.create_from_id_async(requester.device_information.id)


async def is_bonded(client: BleakClient) -> bool:
    if sys.platform != "win32":
        return True
    info = await _device_information(client)
    return bool(info.pairing.is_paired)


async def ensure_bonded(client: BleakClient) -> None:
    """Bond the connected monitor if Windows does not already hold its keys.

    The monitor may start a bond itself right after connecting; asking at
    the same moment fails with OPERATION_ALREADY_IN_PROGRESS, so give that a
    moment to settle and retry.
    """
    if sys.platform != "win32":
        return
    for _ in range(BOND_SETTLE_ATTEMPTS):
        if await is_bonded(client):
            return
        await asyncio.sleep(BOND_SETTLE_INTERVAL_S)
    last_error: BleakError | None = None
    for attempt in range(1, BOND_RETRY_ATTEMPTS + 1):
        try:
            await client.pair()
            log.info("bonded with %s", client.address)
            return
        except BleakError as exc:
            last_error = exc
            if "IN_PROGRESS" not in str(exc):
                raise
            log.debug("bond attempt %d: %s", attempt, exc)
            await asyncio.sleep(BOND_RETRY_INTERVAL_S)
    assert last_error is not None
    raise last_error


async def release(address: str) -> bool:
    """Forget the Windows bond for ``address``.  Returns True if one was removed."""
    if sys.platform != "win32":
        return False
    from winrt.windows.devices.bluetooth import BluetoothLEDevice  # noqa: PLC0415
    from winrt.windows.devices.enumeration import DeviceInformation  # noqa: PLC0415

    device = await BluetoothLEDevice.from_bluetooth_address_async(_address_int(address))
    if device is None:
        return False
    info = await DeviceInformation.create_from_id_async(device.device_information.id)
    if not info.pairing.is_paired:
        return False
    result = await info.pairing.unpair_async()
    log.info("released bond with %s (status %s)", address, result.status)
    return True


async def release_others(addresses: list[str], keep: str) -> None:
    """Drop bonds for every listed monitor except ``keep``."""
    assert len(addresses) <= MAX_RELEASE
    for address in addresses:
        if address.upper() != keep.upper():
            await release(address)
