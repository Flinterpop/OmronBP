#include "models.h"

#include <cassert>
#include <cstdio>
#include <cwctype>
#include <stdexcept>

namespace omron::models {

namespace {

constexpr int kDaysInMonth[13] = {0, 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

bool IsLeap(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

// clang-format off
// Settings read at 0x0260 / written at 0x0286; the clock occupies bytes 0x14..0x1e of that record.
constexpr ClockLayout kHem7600TClock{0x0274, 0x029A, 10, 2, /*year*/ 3, /*month*/ 2, /*day*/ 5, /*hour*/ 4, /*minute*/ 7, /*second*/ 6, /*pad*/ 8, /*checksum*/ 9, 8};
// Settings read at 0x0010 / written at 0x0054; the clock occupies bytes 0x2c..0x3c of that record.
constexpr ClockLayout kHem7342TClock{0x003C, 0x0080, 16, 8, /*year*/ 8, /*month*/ 9, /*day*/ 10, /*hour*/ 11, /*minute*/ 12, /*second*/ 13, /*pad*/ 15, /*checksum*/ 14, 14};

constexpr DeviceLayout kHem7600T{
    L"HEM-7600T", L"BP7000 (Evolv)", Endian::Big,
    {0x02AC, 0}, {100, 0}, 1, 0x0E, 0x38,
    {   // dia        sys         pulse     mov       ihb       year      month     day       hour      minute    second
        {0, 7},   {8, 15},    {24, 31}, {32, 32}, {33, 33}, {16, 23}, {34, 37}, {38, 42}, {43, 47}, {52, 57}, {58, 63},
    },
    &kHem7600TClock,
};

constexpr DeviceLayout kHem7342T{
    L"HEM-7342T", L"BP7450 / BP7455CAN (10 Series)", Endian::Little,
    {0x0098, 0x06D8}, {100, 100}, 2, 0x10, 0x10,
    {   // dia          sys         pulse       mov       ihb       year       month     day       hour      minute    second
        {112, 119}, {120, 127}, {104, 111}, {80, 80}, {81, 81}, {98, 103}, {82, 85}, {86, 90}, {91, 95}, {68, 73}, {74, 79},
    },
    &kHem7342TClock,
};
// clang-format on

constexpr std::array<const DeviceLayout*, kLayoutCount> kLayouts{&kHem7600T, &kHem7342T};

struct Alias {
    const wchar_t* name;
    const DeviceLayout* layout;
};

constexpr Alias kAliases[] = {
    {L"HEM-7600T", &kHem7600T}, {L"BP7000", &kHem7600T},    {L"EVOLV", &kHem7600T},
    {L"HEM-7342T", &kHem7342T}, {L"BP7450", &kHem7342T},    {L"BP7455", &kHem7342T},
    {L"BP7455CAN", &kHem7342T}, {L"10SERIES", &kHem7342T},
};

bool SameIgnoringCaseAndPunctuation(std::wstring_view a, std::wstring_view b) {
    // Compare with '-', '_' and spaces removed, case-insensitively; both sides are short.
    assert(a.size() < 64 && b.size() < 64);
    size_t i = 0, j = 0;
    const size_t limit = a.size() + b.size() + 1;
    for (size_t steps = 0; steps < limit; ++steps) {
        while (i < a.size() && (a[i] == L'-' || a[i] == L'_' || a[i] == L' ')) ++i;
        while (j < b.size() && (b[j] == L'-' || b[j] == L'_' || b[j] == L' ')) ++j;
        assert(i <= a.size() && j <= b.size());
        if (i == a.size() || j == b.size()) return i == a.size() && j == b.size();
        if (std::towupper(a[i]) != std::towupper(b[j])) return false;
        ++i;
        ++j;
    }
    return false;
}

}  // namespace

bool Timestamp::IsValid() const {
    if (year < 2000 || year > 2099 || month < 1 || month > 12 || day < 1) return false;
    const int max_day = (month == 2 && !IsLeap(year)) ? 28 : kDaysInMonth[month];
    if (day > max_day) return false;
    return hour >= 0 && hour < 24 && minute >= 0 && minute < 60 && second >= 0 && second < 60;
}

std::string Timestamp::ToString() const {
    assert(IsValid());
    char buf[24];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d", year, month, day, hour, minute, second);
    return buf;
}

std::optional<Timestamp> Timestamp::Parse(std::string_view text) {
    Timestamp t;
    if (text.size() != 19) return std::nullopt;
    if (std::sscanf(std::string(text).c_str(), "%4d-%2d-%2d %2d:%2d:%2d", &t.year, &t.month, &t.day, &t.hour,
                    &t.minute, &t.second) != 6) {
        return std::nullopt;
    }
    if (!t.IsValid()) return std::nullopt;
    return t;
}

int64_t Timestamp::ToEpoch() const {
    assert(IsValid());
    // Days from civil (Howard Hinnant's algorithm), no recursion, no library time zone handling.
    const int y = year - (month <= 2 ? 1 : 0);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = static_cast<int64_t>(era) * 146097 + doe - 719468;
    return days * 86400 + hour * 3600 + minute * 60 + second;
}

uint32_t BitsToInt(const uint8_t* data, size_t size, int first_bit, int last_bit, Endian endian) {
    assert(data != nullptr && size > 0 && size <= kMaxRecordSize);
    assert(0 <= first_bit && first_bit <= last_bit && last_bit < static_cast<int>(size) * 8);
    assert(last_bit - first_bit < 32);
    if (first_bit < 0 || last_bit >= static_cast<int>(size) * 8 || last_bit - first_bit >= 32) {
        throw std::runtime_error("bit range out of record");
    }
    uint32_t value = 0;
    for (int bit = first_bit; bit <= last_bit; ++bit) {
        // Byte index within the big-endian view of the integer; little-endian devices reverse the bytes.
        const size_t be_index = static_cast<size_t>(bit / 8);
        const size_t index = endian == Endian::Big ? be_index : size - 1 - be_index;
        const int shift = 7 - (bit % 8);
        value = (value << 1) | ((data[index] >> shift) & 1u);
    }
    return value;
}

std::optional<Reading> ParseRecord(const DeviceLayout& layout, const uint8_t* record, size_t size) {
    assert(record != nullptr);
    assert(layout.record_size <= kMaxRecordSize);
    if (size != layout.record_size) {
        throw std::runtime_error("record size mismatch");
    }
    bool empty = true;
    for (size_t i = 0; i < size; ++i) {
        if (record[i] != 0xFF) {
            empty = false;
            break;
        }
    }
    if (empty) return std::nullopt;

    const RecordBits& b = layout.bits;
    auto field = [&](BitRange r) { return static_cast<int>(BitsToInt(record, size, r.first, r.last, layout.endian)); };
    Reading reading;
    reading.systolic = field(b.systolic) + kSystolicOffset;
    reading.diastolic = field(b.diastolic);
    reading.pulse = field(b.pulse);
    reading.movement = field(b.movement) != 0;
    reading.irregular_heartbeat = field(b.irregular_heartbeat) != 0;
    reading.when.year = field(b.year) + kYearOffset;
    reading.when.month = field(b.month);
    reading.when.day = field(b.day);
    reading.when.hour = field(b.hour);
    reading.when.minute = field(b.minute);
    // Some firmware stores seconds in a 6-bit field that can exceed 59.
    reading.when.second = field(b.second) > kMaxSecond ? kMaxSecond : field(b.second);
    if (!reading.when.IsValid()) {
        throw std::runtime_error("invalid timestamp in record");
    }
    const bool plausible = reading.systolic >= kMinPressure && reading.systolic <= kMaxPressure &&
                           reading.diastolic >= kMinPressure && reading.diastolic <= kMaxPressure &&
                           reading.pulse >= kMinPulse && reading.pulse <= kMaxPulse;
    if (!plausible) {
        throw std::runtime_error("implausible values in record");
    }
    return reading;
}

bool ClockChecksumOk(const ClockLayout& clock, const uint8_t* record, size_t size) {
    assert(record != nullptr);
    if (size != clock.size) return false;
    unsigned sum = 0;
    for (size_t i = 0; i < clock.checksum_span; ++i) sum += record[i];
    return (sum & 0xFF) == record[clock.checksum_offset];
}

std::optional<Timestamp> ParseClock(const ClockLayout& clock, const uint8_t* record, size_t size) {
    assert(record != nullptr);
    if (size != clock.size) return std::nullopt;
    assert(clock.checksum_span < clock.size);
    Timestamp t;
    t.year = record[clock.year] + kYearOffset;
    t.month = record[clock.month];
    t.day = record[clock.day];
    t.hour = record[clock.hour];
    t.minute = record[clock.minute];
    t.second = record[clock.second] > kMaxSecond ? kMaxSecond : record[clock.second];
    if (!t.IsValid()) return std::nullopt;
    return t;
}

void EncodeClock(const ClockLayout& clock, const uint8_t* current, size_t size, const Timestamp& when, uint8_t* out) {
    assert(current != nullptr && out != nullptr && size == clock.size && when.IsValid());
    for (size_t i = 0; i < size; ++i) out[i] = current[i];
    out[clock.year] = static_cast<uint8_t>(when.year - kYearOffset);
    out[clock.month] = static_cast<uint8_t>(when.month);
    out[clock.day] = static_cast<uint8_t>(when.day);
    out[clock.hour] = static_cast<uint8_t>(when.hour);
    out[clock.minute] = static_cast<uint8_t>(when.minute);
    out[clock.second] = static_cast<uint8_t>(when.second);
    out[clock.pad_offset] = 0;
    unsigned sum = 0;
    for (size_t i = 0; i < clock.checksum_span; ++i) sum += out[i];
    out[clock.checksum_offset] = static_cast<uint8_t>(sum & 0xFF);
    assert(ClockChecksumOk(clock, out, size));
}

const DeviceLayout* FindLayout(std::wstring_view name) {
    assert(!name.empty());
    for (const Alias& alias : kAliases) {
        if (SameIgnoringCaseAndPunctuation(alias.name, name)) return alias.layout;
    }
    return nullptr;
}

const std::array<const DeviceLayout*, kLayoutCount>& AllLayouts() {
    static_assert(kLayouts.size() == kLayoutCount);
    return kLayouts;
}

}  // namespace omron::models
