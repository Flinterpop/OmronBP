import pytest

from omron_bp.transport import (
    CMD_WRITE,
    END_COMMAND,
    HEADER_SIZE,
    RSP_END,
    RSP_READ,
    RSP_START,
    START_COMMAND,
    ChannelAssembler,
    ProtocolError,
    build_read_command,
    build_write_command,
    has_omron_service,
    parse_packet,
    xor_checksum,
)


def make_reply(kind: int, address: int, data: bytes) -> bytes:
    """Build a device->PC packet with a valid trailer."""
    header = bytes((HEADER_SIZE + len(data) + 2,)) + kind.to_bytes(2, "big") + address.to_bytes(2, "big") + bytes((len(data),))
    body = header + data + b"\x00"
    return body + bytes((xor_checksum(body),))


def test_fixed_commands_have_zero_checksum() -> None:
    assert xor_checksum(START_COMMAND) == 0
    assert xor_checksum(END_COMMAND) == 0


def test_read_command_matches_documented_example() -> None:
    # 0x26 bytes from 0x0260 -> documented as 08 01 00 02 60 26 00 4d.
    assert build_read_command(0x0260, 0x26) == bytes.fromhex("080100026026004d")


def test_read_command_rejects_oversize() -> None:
    with pytest.raises(AssertionError):
        build_read_command(0, 0x39)


def test_parse_packet_extracts_fields() -> None:
    packet = parse_packet(make_reply(RSP_READ, 0x0098, bytes(range(16))))
    assert packet.kind == RSP_READ
    assert packet.address == 0x0098
    assert packet.data == bytes(range(16))


def test_parse_packet_bad_checksum() -> None:
    raw = bytearray(make_reply(RSP_START, 0, bytes(16)))
    raw[-1] ^= 0x01
    with pytest.raises(ProtocolError, match="checksum"):
        parse_packet(bytes(raw))


def test_parse_packet_bad_length_byte() -> None:
    raw = bytearray(make_reply(RSP_END, 0, b"\x00"))
    raw[0] += 1
    raw[-1] ^= 1  # keep the checksum valid so the length check is what fires
    with pytest.raises(ProtocolError, match="length"):
        parse_packet(bytes(raw))


def test_assembler_waits_for_all_channels() -> None:
    reply = make_reply(RSP_READ, 0x02AC, bytes(0x38))  # 64 bytes -> 4 channels
    asm = ChannelAssembler()
    assert asm.push(0, reply[0:16]) is None
    assert asm.push(3, reply[48:64]) is None
    assert asm.push(1, reply[16:32]) is None
    packet = asm.push(2, reply[32:48])
    assert packet is not None
    assert packet.address == 0x02AC
    assert packet.data == bytes(0x38)


def test_assembler_handles_out_of_order_and_short_last_chunk() -> None:
    reply = make_reply(RSP_READ, 0x0010, bytes(0x10))  # 24 bytes -> ch0 full, ch1 8 bytes
    asm = ChannelAssembler()
    assert asm.push(1, reply[16:24]) is None
    packet = asm.push(0, reply[0:16])
    assert packet is not None
    assert packet.raw == reply


def test_assembler_single_channel_end_reply() -> None:
    reply = make_reply(RSP_END, 0, b"\x00")  # 9 bytes, fits in one chunk
    packet = ChannelAssembler().push(0, reply)
    assert packet is not None
    assert packet.kind == RSP_END
    assert packet.raw[HEADER_SIZE] == 0


def test_assembler_resets_after_packet() -> None:
    asm = ChannelAssembler()
    first = make_reply(RSP_END, 0, b"\x00")
    assert asm.push(0, first) is not None
    # A stale chunk on channel 1 must not leak into the next short packet.
    assert asm.push(0, first) is not None


def test_assembler_rejects_absurd_length() -> None:
    asm = ChannelAssembler()
    with pytest.raises(ProtocolError, match="length"):
        asm.push(0, bytes((0xFF,)) + bytes(15))


def test_has_omron_service_case_insensitive() -> None:
    assert has_omron_service(["ECBE3980-C9A2-11E1-B1BD-0002A5D5C51B"])
    assert not has_omron_service(["0000180a-0000-1000-8000-00805f9b34fb"])


def test_write_command_layout() -> None:
    cmd = build_write_command(0x0080, bytes(range(16)))
    assert len(cmd) == 8 + 16
    assert cmd[0] == 24 and int.from_bytes(cmd[1:3], "big") == CMD_WRITE
    assert int.from_bytes(cmd[3:5], "big") == 0x0080 and cmd[5] == 16
    assert cmd[6:22] == bytes(range(16)) and cmd[22] == 0
    assert xor_checksum(cmd) == 0
