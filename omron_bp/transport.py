"""OMRON "BLEsmart" GATT transport.

The monitor exposes one service with four write ("TX") characteristics,
four notify ("RX") characteristics and one unlock characteristic.  A
command is split into 16-byte chunks written to TX channel 0, 1, 2, 3 in
turn; the reply arrives as 16-byte notifications on the matching RX
channels and is reassembled here.

Packet format (both directions)::

    byte 0      total packet length
    bytes 1-2   packet type (0x0100 read, 0x8100 read reply, ...)
    bytes 3-4   EEPROM address, big-endian
    byte 5      data length
    bytes 6..   data
    byte n-2    0x00 padding
    byte n-1    XOR checksum so that all bytes XOR to zero

The unlock characteristic is separate: a 17-byte write whose first byte is
the operation (0x02 enter pairing mode, 0x00 store key, 0x01 unlock) and
whose remaining 16 bytes are the key.
"""

from __future__ import annotations

import asyncio
import logging
from collections.abc import Sequence
from dataclasses import dataclass

from bleak import BleakClient
from bleak.backends.characteristic import BleakGATTCharacteristic

log = logging.getLogger("omron_bp")

SERVICE_UUID = "ecbe3980-c9a2-11e1-b1bd-0002a5d5c51b"
RX_UUIDS: tuple[str, ...] = (
    "49123040-aee8-11e1-a74d-0002a5d5c51b",
    "4d0bf320-aee8-11e1-a0d9-0002a5d5c51b",
    "5128ce60-aee8-11e1-b84b-0002a5d5c51b",
    "560f1420-aee8-11e1-8184-0002a5d5c51b",
)
TX_UUIDS: tuple[str, ...] = (
    "db5b55e0-aee7-11e1-965e-0002a5d5c51b",
    "e0b8a060-aee7-11e1-92f4-0002a5d5c51b",
    "0ae12b00-aee8-11e1-a192-0002a5d5c51b",
    "10e1ba60-aee8-11e1-89e5-0002a5d5c51b",
)
UNLOCK_UUID = "b305b680-aee7-11e1-a730-0002a5d5c51b"

CHANNEL_COUNT = 4
CHANNEL_WIDTH = 16
MAX_PACKET = CHANNEL_COUNT * CHANNEL_WIDTH
HEADER_SIZE = 6
TRAILER_SIZE = 2
MAX_READ_SIZE = MAX_PACKET - HEADER_SIZE - TRAILER_SIZE  # 0x38
KEY_SIZE = 16

# Packet types.
CMD_READ = 0x0100
CMD_WRITE = 0x01C0
RSP_WRITE = 0x81C0
RSP_START = 0x8000
RSP_READ = 0x8100
RSP_END = 0x8F00

# Unlock-characteristic operations and their expected reply prefixes.
UNLOCK_OP_ENTER_PAIRING = 0x02
UNLOCK_OP_STORE_KEY = 0x00
UNLOCK_OP_UNLOCK = 0x01
UNLOCK_RSP_PAIRING_MODE = bytes((0x82, 0x00))
UNLOCK_RSP_KEY_STORED = bytes((0x80, 0x00))
UNLOCK_RSP_UNLOCKED = bytes((0x81, 0x00))

START_COMMAND = bytes.fromhex("0800000000100018")
END_COMMAND = bytes.fromhex("080f000000000007")

REPLY_TIMEOUT_S = 1.0
MAX_RETRIES = 5
PAIRING_ATTEMPTS = 10
PAIRING_RETRY_DELAY_S = 1.0
UNLOCK_TIMEOUT_S = 10.0


class ProtocolError(RuntimeError):
    """The monitor replied with something unexpected."""


def xor_checksum(data: bytes) -> int:
    assert len(data) <= MAX_PACKET
    result = 0
    for byte in data:
        result ^= byte
    return result


def build_read_command(address: int, size: int) -> bytes:
    """Command to read ``size`` bytes of EEPROM starting at ``address``."""
    assert 0 <= address <= 0xFFFF, address
    assert 0 < size <= MAX_READ_SIZE, size
    body = (
        bytes((HEADER_SIZE + TRAILER_SIZE,))
        + CMD_READ.to_bytes(2, "big")
        + address.to_bytes(2, "big")
        + bytes((size, 0x00))
    )
    packet = body + bytes((xor_checksum(body),))
    assert len(packet) == HEADER_SIZE + TRAILER_SIZE and xor_checksum(packet) == 0
    return packet


def build_write_command(address: int, data: bytes) -> bytes:
    """Command to write ``data`` to EEPROM at ``address`` (one block)."""
    assert 0 <= address <= 0xFFFF, address
    assert 0 < len(data) <= MAX_READ_SIZE, len(data)
    body = (
        bytes((HEADER_SIZE + TRAILER_SIZE + len(data),))
        + CMD_WRITE.to_bytes(2, "big")
        + address.to_bytes(2, "big")
        + bytes((len(data),))
        + data
        + bytes((0x00,))
    )
    packet = body + bytes((xor_checksum(body),))
    assert xor_checksum(packet) == 0
    return packet


@dataclass(frozen=True)
class Packet:
    """A reassembled, checksum-verified reply."""

    kind: int
    address: int
    raw: bytes

    @property
    def data(self) -> bytes:
        """Data bytes between the header and the trailer."""
        return self.raw[HEADER_SIZE : len(self.raw) - TRAILER_SIZE]


def parse_packet(raw: bytes) -> Packet:
    assert HEADER_SIZE + TRAILER_SIZE <= len(raw) <= MAX_PACKET, len(raw)
    if raw[0] != len(raw):
        raise ProtocolError(f"length byte {raw[0]} != packet length {len(raw)}: {raw.hex()}")
    if xor_checksum(raw) != 0:
        raise ProtocolError(f"checksum failure: {raw.hex()}")
    return Packet(
        kind=int.from_bytes(raw[1:3], "big"),
        address=int.from_bytes(raw[3:5], "big"),
        raw=raw,
    )


class ChannelAssembler:
    """Reassemble one packet from per-channel notification chunks.

    Pure logic (no Bluetooth) so it can be unit tested.
    """

    def __init__(self) -> None:
        self._chunks: list[bytes | None] = [None] * CHANNEL_COUNT

    def reset(self) -> None:
        self._chunks = [None] * CHANNEL_COUNT

    def push(self, channel: int, chunk: bytes) -> Packet | None:
        """Store ``chunk``; return the packet once every needed channel has arrived."""
        assert 0 <= channel < CHANNEL_COUNT, channel
        assert 0 < len(chunk) <= CHANNEL_WIDTH + 4, len(chunk)
        self._chunks[channel] = bytes(chunk)

        first = self._chunks[0]
        if first is None:
            return None
        packet_size = first[0]
        if packet_size < HEADER_SIZE + TRAILER_SIZE or packet_size > MAX_PACKET:
            self.reset()
            raise ProtocolError(f"bad packet length byte {packet_size}")
        needed = (packet_size + CHANNEL_WIDTH - 1) // CHANNEL_WIDTH
        parts = self._chunks[:needed]
        if any(part is None for part in parts):
            return None

        combined = b"".join(part for part in parts if part is not None)[:packet_size]
        self.reset()
        return parse_packet(combined)


class OmronTransport:
    """Command/response layer over an open :class:`BleakClient`."""

    def __init__(self, client: BleakClient) -> None:
        self._client = client
        self._assembler = ChannelAssembler()
        self._channel_by_uuid = {uuid.lower(): idx for idx, uuid in enumerate(RX_UUIDS)}
        self._reply: Packet | None = None
        self._reply_ready = asyncio.Event()
        self._unlock_reply: bytes | None = None
        self._unlock_ready = asyncio.Event()
        self._listening = False

    # --- notification plumbing ------------------------------------------

    def _on_rx(self, char: BleakGATTCharacteristic, data: bytearray) -> None:
        channel = self._channel_by_uuid.get(char.uuid.lower())
        assert channel is not None, char.uuid
        log.debug("rx ch%d < %s", channel, bytes(data).hex())
        try:
            packet = self._assembler.push(channel, bytes(data))
        except ProtocolError as exc:
            log.warning("discarding corrupt reply: %s", exc)
            return
        if packet is not None:
            self._reply = packet
            self._reply_ready.set()

    def _on_unlock(self, _char: BleakGATTCharacteristic, data: bytearray) -> None:
        log.debug("rx unlock < %s", bytes(data).hex())
        self._unlock_reply = bytes(data)
        self._unlock_ready.set()

    async def open(self) -> None:
        if self._listening:
            return
        for uuid in RX_UUIDS:
            await self._client.start_notify(uuid, self._on_rx)
        self._listening = True

    async def close(self) -> None:
        if not self._listening:
            return
        self._listening = False
        for uuid in RX_UUIDS:
            await self._client.stop_notify(uuid)

    async def _write_chunks(self, command: bytes) -> None:
        assert 0 < len(command) <= MAX_PACKET
        chunk_count = (len(command) + CHANNEL_WIDTH - 1) // CHANNEL_WIDTH
        for idx in range(chunk_count):
            chunk = command[idx * CHANNEL_WIDTH : (idx + 1) * CHANNEL_WIDTH]
            log.debug("tx ch%d > %s", idx, chunk.hex())
            await self._client.write_gatt_char(TX_UUIDS[idx], chunk)

    def _arm_reply(self) -> None:
        self._assembler.reset()
        self._reply = None
        self._reply_ready.clear()

    def _take_reply(self) -> Packet:
        reply = self._reply
        assert reply is not None, "reply event set without a packet"
        return reply

    async def send(self, command: bytes) -> Packet:
        """Write ``command`` and wait for the reassembled reply, retrying on timeout."""
        assert self._listening, "call open() first"
        for attempt in range(1, MAX_RETRIES + 1):
            self._arm_reply()
            await self._write_chunks(command)
            try:
                await asyncio.wait_for(self._reply_ready.wait(), REPLY_TIMEOUT_S)
            except TimeoutError:
                log.warning("no reply to %s (attempt %d/%d)", command.hex(), attempt, MAX_RETRIES)
                continue
            return self._take_reply()
        raise ProtocolError(f"no reply after {MAX_RETRIES} attempts: {command.hex()}")

    # --- session ----------------------------------------------------------

    async def start_session(self) -> Packet:
        reply = await self.send(START_COMMAND)
        if reply.kind != RSP_START:
            raise ProtocolError(f"unexpected reply to start: {reply.raw.hex()}")
        return reply

    async def end_session(self) -> None:
        reply = await self.send(END_COMMAND)
        if reply.kind != RSP_END:
            raise ProtocolError(f"unexpected reply to end: {reply.raw.hex()}")
        status = reply.raw[HEADER_SIZE]
        if status != 0:
            raise ProtocolError(f"device reported error status {status} at end of session")

    async def read_block(self, address: int, size: int) -> bytes:
        reply = await self.send(build_read_command(address, size))
        if reply.kind != RSP_READ:
            raise ProtocolError(f"unexpected reply to read: {reply.raw.hex()}")
        if reply.address != address:
            raise ProtocolError(f"reply address {reply.address:#06x} != requested {address:#06x}")
        data = reply.data
        if len(data) == 0 and reply.raw[5] == size:
            # Header-only reply: the monitor sends no payload for an erased
            # region.  Erased EEPROM reads as 0xFF, which decodes as empty slots.
            log.debug("erased region at %#06x, substituting 0xff", address)
            return b"\xff" * size
        if len(data) != size:
            raise ProtocolError(f"reply carried {len(data)} bytes, expected {size}: {reply.raw.hex()}")
        return data

    async def write_eeprom(self, address: int, data: bytes) -> None:
        """Write one block (at most 0x38 bytes) and check the monitor's acknowledgement."""
        assert 0 < len(data) <= MAX_READ_SIZE
        reply = await self.send(build_write_command(address, data))
        if reply.kind != RSP_WRITE:
            raise ProtocolError(f"unexpected reply to write: {reply.raw.hex()}")
        if reply.address != address:
            raise ProtocolError(f"write acknowledged for {reply.address:#06x}, expected {address:#06x}")

    async def read_eeprom(self, address: int, size: int, block_size: int) -> bytes:
        """Read an arbitrary-length region in ``block_size`` pieces."""
        assert 0 < block_size <= MAX_READ_SIZE, block_size
        assert 0 < size <= 0x10000, size
        out = bytearray()
        max_blocks = (size + block_size - 1) // block_size
        for _ in range(max_blocks):
            remaining = size - len(out)
            if remaining <= 0:
                break
            chunk = min(remaining, block_size)
            log.debug("read %#06x size %#x", address + len(out), chunk)
            out += await self.read_block(address + len(out), chunk)
        assert len(out) == size
        return bytes(out)

    # --- unlock / pairing ---------------------------------------------------

    def _arm_unlock_reply(self) -> None:
        self._unlock_reply = None
        self._unlock_ready.clear()

    def _take_unlock_reply(self) -> bytes:
        reply = self._unlock_reply
        assert reply is not None, "unlock event set without data"
        return reply

    async def _unlock_exchange(self, op: int, key: bytes) -> bytes:
        assert len(key) == KEY_SIZE
        self._arm_unlock_reply()
        await self._client.write_gatt_char(UNLOCK_UUID, bytes((op,)) + key, response=True)
        await asyncio.wait_for(self._unlock_ready.wait(), UNLOCK_TIMEOUT_S)
        return self._take_unlock_reply()

    async def unlock(self, key: bytes) -> None:
        """Present the stored pairing key.  Required before any session."""
        assert len(key) == KEY_SIZE, "key must be 16 bytes"
        await self._client.start_notify(UNLOCK_UUID, self._on_unlock)
        try:
            reply = await self._unlock_exchange(UNLOCK_OP_UNLOCK, key)
        finally:
            await self._client.stop_notify(UNLOCK_UUID)
        if reply[:2] != UNLOCK_RSP_UNLOCKED:
            raise ProtocolError("key rejected - run 'pair' with the monitor in pairing mode (blinking P)")

    async def pair(self, key: bytes) -> None:
        """Store a new pairing key.  The monitor must be showing the blinking ``P``."""
        assert len(key) == KEY_SIZE, "key must be 16 bytes"
        # Subscribing to an RX channel prompts the monitor to request BLE
        # security, which is what starts the OS-level bonding.
        await self._client.start_notify(RX_UUIDS[0], self._on_rx)
        await self._client.start_notify(UNLOCK_UUID, self._on_unlock)
        try:
            entered = False
            for attempt in range(1, PAIRING_ATTEMPTS + 1):
                reply = await self._unlock_exchange(UNLOCK_OP_ENTER_PAIRING, bytes(KEY_SIZE))
                if reply[:2] == UNLOCK_RSP_PAIRING_MODE:
                    entered = True
                    break
                log.debug("pairing-mode attempt %d/%d: %s", attempt, PAIRING_ATTEMPTS, reply.hex())
                await asyncio.sleep(PAIRING_RETRY_DELAY_S)
            if not entered:
                raise ProtocolError("could not enter key-programming mode - is the monitor showing a blinking P?")
            reply = await self._unlock_exchange(UNLOCK_OP_STORE_KEY, key)
            if reply[:2] != UNLOCK_RSP_KEY_STORED:
                raise ProtocolError(f"monitor refused new key: {reply.hex()}")
        finally:
            await self._client.stop_notify(UNLOCK_UUID)
            await self._client.stop_notify(RX_UUIDS[0])


def has_omron_service(uuids: Sequence[str]) -> bool:
    return SERVICE_UUID in (u.lower() for u in uuids)
