// OMRON "BLEsmart" packet framing.  Mirrors omron_bp/transport.py.
//
// Packet layout (both directions):
//   byte 0      total packet length
//   bytes 1-2   packet type (0x0100 read, 0x8100 read reply, ...)
//   bytes 3-4   EEPROM address, big-endian
//   byte 5      data length
//   bytes 6..   data
//   byte n-2    0x00 padding
//   byte n-1    XOR checksum so that all bytes XOR to zero
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>

namespace omron::protocol {

constexpr size_t kChannelCount = 4;
constexpr size_t kChannelWidth = 16;
constexpr size_t kMaxPacket = kChannelCount * kChannelWidth;  // 64
constexpr size_t kHeaderSize = 6;
constexpr size_t kTrailerSize = 2;
constexpr size_t kMaxReadSize = kMaxPacket - kHeaderSize - kTrailerSize;  // 0x38
constexpr size_t kKeySize = 16;

constexpr uint16_t kCmdRead = 0x0100;
constexpr uint16_t kCmdWrite = 0x01C0;
constexpr uint16_t kRspWrite = 0x81C0;
constexpr uint16_t kRspStart = 0x8000;
constexpr uint16_t kRspRead = 0x8100;
constexpr uint16_t kRspEnd = 0x8F00;

// Unlock-characteristic operations and the reply prefixes that mean success.
constexpr uint8_t kUnlockOpEnterPairing = 0x02;
constexpr uint8_t kUnlockOpStoreKey = 0x00;
constexpr uint8_t kUnlockOpUnlock = 0x01;
constexpr uint8_t kUnlockRspPairingMode = 0x82;
constexpr uint8_t kUnlockRspKeyStored = 0x80;
constexpr uint8_t kUnlockRspUnlocked = 0x81;

constexpr std::array<uint8_t, 8> kStartCommand{0x08, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x18};
constexpr std::array<uint8_t, 8> kEndCommand{0x08, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07};

// The same key as omblepy / UBPM / the Python tool, so every tool can talk to a monitor paired by any other.
constexpr std::array<uint8_t, kKeySize> kDefaultKey{0xDE, 0xAD, 0xBE, 0xAF, 0x12, 0x34, 0x12, 0x34,
                                                    0xDE, 0xAD, 0xBE, 0xAF, 0x12, 0x34, 0x12, 0x34};

class ProtocolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A byte string of bounded size (no heap).
struct Bytes {
    std::array<uint8_t, kMaxPacket> data{};
    size_t size = 0;

    const uint8_t* begin() const { return data.data(); }
    const uint8_t* end() const { return data.data() + size; }
    uint8_t operator[](size_t i) const { return data[i]; }
};

uint8_t XorChecksum(const uint8_t* data, size_t size);

// Command to read `size` bytes of EEPROM starting at `address`.
Bytes BuildReadCommand(uint16_t address, uint8_t size);

// Command to write `size` bytes (at most kMaxReadSize) to EEPROM at `address`.
Bytes BuildWriteCommand(uint16_t address, const uint8_t* data, size_t size);

// A reassembled, checksum-verified reply.
struct Packet {
    uint16_t kind = 0;
    uint16_t address = 0;
    Bytes raw;

    size_t DataSize() const { return raw.size - kHeaderSize - kTrailerSize; }
    const uint8_t* Data() const { return raw.begin() + kHeaderSize; }
};

Packet ParsePacket(const uint8_t* raw, size_t size);

// Reassembles one packet from per-channel notification chunks.  Pure logic, unit-tested.
class ChannelAssembler {
public:
    void Reset();
    // Store a chunk; returns the packet once every needed channel has arrived.
    std::optional<Packet> Push(size_t channel, const uint8_t* chunk, size_t size);

private:
    std::array<Bytes, kChannelCount> chunks_{};
    std::array<bool, kChannelCount> present_{};
};

}  // namespace omron::protocol
