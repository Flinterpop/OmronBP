// Per-model EEPROM layouts and record decoding.  Mirrors omron_bp/models.py.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace omron::models {

constexpr int kMinPressure = 20;
constexpr int kMaxPressure = 300;
constexpr int kMinPulse = 20;
constexpr int kMaxPulse = 250;
constexpr int kSystolicOffset = 25;
constexpr int kYearOffset = 2000;
constexpr int kMaxSecond = 59;
constexpr size_t kMaxRecordSize = 32;
constexpr size_t kMaxUsers = 2;

enum class Endian { Big, Little };

struct Timestamp {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    bool IsValid() const;
    std::string ToString() const;  // "YYYY-MM-DD HH:MM:SS"
    static std::optional<Timestamp> Parse(std::string_view text);
    int64_t ToEpoch() const;  // seconds, treating the fields as UTC (ordering only)
};

struct Reading {
    Timestamp when;
    int systolic = 0;
    int diastolic = 0;
    int pulse = 0;
    bool movement = false;
    bool irregular_heartbeat = false;
};

struct BitRange {
    int first;
    int last;
};

struct RecordBits {
    BitRange diastolic, systolic, pulse, movement, irregular_heartbeat, year, month, day, hour, minute, second;
};

// The settings record holding the monitor clock: read from `read_address`, written back
// to `write_address`.  Bytes 0..prefix are copied back unchanged, the six fields are set at
// their offsets, `pad_offset` is zeroed and `checksum_offset` receives the low byte of the
// sum of the first `checksum_span` bytes.  The same checksum is verified on read, which is
// how the layout is confirmed before anything is written.
struct ClockLayout {
    uint16_t read_address;
    uint16_t write_address;
    size_t size;
    size_t prefix;
    size_t year, month, day, hour, minute, second;
    size_t pad_offset;
    size_t checksum_offset;
    size_t checksum_span;
};

struct DeviceLayout {
    const wchar_t* model;  // internal OMRON model, e.g. L"HEM-7600T"
    const wchar_t* retail;  // what the box says, for the UI
    Endian endian;
    std::array<uint16_t, kMaxUsers> user_start;
    std::array<int, kMaxUsers> records_per_user;
    size_t user_count;
    size_t record_size;
    size_t read_block_size;
    RecordBits bits;
    const ClockLayout* clock;  // nullptr when unknown for the model
};

// Bit 0 is the MSB of the record interpreted as one big integer in the device's byte order.
uint32_t BitsToInt(const uint8_t* data, size_t size, int first_bit, int last_bit, Endian endian);

// Decodes one record; returns nullopt for an empty (all 0xFF) slot; throws std::runtime_error for garbage.
std::optional<Reading> ParseRecord(const DeviceLayout& layout, const uint8_t* record, size_t size);

// True if the record carries the expected checksum, i.e. the clock layout matches this monitor.
bool ClockChecksumOk(const ClockLayout& clock, const uint8_t* record, size_t size);

// Decodes the monitor clock; returns nullopt for an invalid date.
std::optional<Timestamp> ParseClock(const ClockLayout& clock, const uint8_t* record, size_t size);

// Builds the record to write so the clock reads `when`; `out` receives clock.size bytes.
void EncodeClock(const ClockLayout& clock, const uint8_t* current, size_t size, const Timestamp& when, uint8_t* out);

// Known layouts and the names (model or retail alias) that select them; case-insensitive.
const DeviceLayout* FindLayout(std::wstring_view name);
constexpr size_t kLayoutCount = 2;
const std::array<const DeviceLayout*, kLayoutCount>& AllLayouts();

}  // namespace omron::models
