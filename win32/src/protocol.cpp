#include "protocol.h"

#include <cassert>
#include <cstring>
#include <string>

namespace omron::protocol {

namespace {

std::string Hex(const uint8_t* data, size_t size) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < size && i < kMaxPacket; ++i) {
        out.push_back(kDigits[data[i] >> 4]);
        out.push_back(kDigits[data[i] & 0x0F]);
    }
    return out;
}

}  // namespace

uint8_t XorChecksum(const uint8_t* data, size_t size) {
    assert(data != nullptr || size == 0);
    assert(size <= kMaxPacket);
    uint8_t result = 0;
    for (size_t i = 0; i < size; ++i) {
        result ^= data[i];
    }
    return result;
}

Bytes BuildReadCommand(uint16_t address, uint8_t size) {
    assert(size > 0 && size <= kMaxReadSize);
    Bytes cmd;
    cmd.data[0] = static_cast<uint8_t>(kHeaderSize + kTrailerSize);
    cmd.data[1] = static_cast<uint8_t>(kCmdRead >> 8);
    cmd.data[2] = static_cast<uint8_t>(kCmdRead & 0xFF);
    cmd.data[3] = static_cast<uint8_t>(address >> 8);
    cmd.data[4] = static_cast<uint8_t>(address & 0xFF);
    cmd.data[5] = size;
    cmd.data[6] = 0x00;
    cmd.data[7] = XorChecksum(cmd.data.data(), 7);
    cmd.size = kHeaderSize + kTrailerSize;
    assert(XorChecksum(cmd.begin(), cmd.size) == 0);
    return cmd;
}

Bytes BuildWriteCommand(uint16_t address, const uint8_t* data, size_t size) {
    assert(data != nullptr && size > 0 && size <= kMaxReadSize);
    Bytes cmd;
    cmd.data[0] = static_cast<uint8_t>(kHeaderSize + kTrailerSize + size);
    cmd.data[1] = static_cast<uint8_t>(kCmdWrite >> 8);
    cmd.data[2] = static_cast<uint8_t>(kCmdWrite & 0xFF);
    cmd.data[3] = static_cast<uint8_t>(address >> 8);
    cmd.data[4] = static_cast<uint8_t>(address & 0xFF);
    cmd.data[5] = static_cast<uint8_t>(size);
    std::memcpy(cmd.data.data() + kHeaderSize, data, size);
    cmd.data[kHeaderSize + size] = 0x00;
    cmd.data[kHeaderSize + size + 1] = XorChecksum(cmd.data.data(), kHeaderSize + size + 1);
    cmd.size = kHeaderSize + kTrailerSize + size;
    assert(XorChecksum(cmd.begin(), cmd.size) == 0);
    return cmd;
}

Packet ParsePacket(const uint8_t* raw, size_t size) {
    assert(raw != nullptr);
    if (size < kHeaderSize + kTrailerSize || size > kMaxPacket) {
        throw ProtocolError("packet size " + std::to_string(size) + " out of range");
    }
    if (raw[0] != size) {
        throw ProtocolError("length byte " + std::to_string(raw[0]) + " != packet length " +
                            std::to_string(size) + ": " + Hex(raw, size));
    }
    if (XorChecksum(raw, size) != 0) {
        throw ProtocolError("checksum failure: " + Hex(raw, size));
    }
    assert(raw[0] == size);
    Packet packet;
    packet.kind = static_cast<uint16_t>((raw[1] << 8) | raw[2]);
    packet.address = static_cast<uint16_t>((raw[3] << 8) | raw[4]);
    std::memcpy(packet.raw.data.data(), raw, size);
    packet.raw.size = size;
    return packet;
}

void ChannelAssembler::Reset() {
    present_.fill(false);
}

std::optional<Packet> ChannelAssembler::Push(size_t channel, const uint8_t* chunk, size_t size) {
    assert(channel < kChannelCount);
    assert(chunk != nullptr);
    if (size == 0 || size > kChannelWidth + 4) {
        throw ProtocolError("bad chunk size " + std::to_string(size));
    }
    std::memcpy(chunks_[channel].data.data(), chunk, size);
    chunks_[channel].size = size;
    present_[channel] = true;

    if (!present_[0]) {
        return std::nullopt;
    }
    const size_t packet_size = chunks_[0][0];
    if (packet_size < kHeaderSize + kTrailerSize || packet_size > kMaxPacket) {
        Reset();
        throw ProtocolError("bad packet length byte " + std::to_string(packet_size));
    }
    const size_t needed = (packet_size + kChannelWidth - 1) / kChannelWidth;
    for (size_t i = 0; i < needed; ++i) {
        if (!present_[i]) {
            return std::nullopt;
        }
    }
    std::array<uint8_t, kMaxPacket + kChannelCount * 4> combined{};
    size_t total = 0;
    for (size_t i = 0; i < needed; ++i) {
        std::memcpy(combined.data() + total, chunks_[i].begin(), chunks_[i].size);
        total += chunks_[i].size;
    }
    Reset();
    if (total < packet_size) {
        throw ProtocolError("short reply: " + std::to_string(total) + " of " + std::to_string(packet_size));
    }
    return ParsePacket(combined.data(), packet_size);
}

}  // namespace omron::protocol
