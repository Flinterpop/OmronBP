// High-level operations run on a worker thread: pair a monitor, download its readings.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "ble.h"
#include "models.h"
#include "storage.h"

namespace omron::workflow {

using Log = std::function<void(ble::Level, const std::wstring&)>;

struct Paths {
    std::filesystem::path csv;
    std::filesystem::path devices;
};

struct ClockStatus {
    bool known = false;      // the model has a clock layout
    bool readable = false;   // the record decoded to a valid date
    bool verified = false;   // checksum matched, so the layout is trusted
    bool corrected = false;  // PC time was written (applied when the session ends)
    long drift_seconds = 0;  // monitor minus PC
    models::Timestamp monitor_time;
};

struct DownloadResult {
    std::vector<std::vector<models::Reading>> per_user;
    size_t appended = 0;
    ClockStatus clock;
};

constexpr long kClockToleranceSeconds = 30;

// Reads the clock record, reports the drift and, when `sync` is set and the record checksum
// verified the layout, writes `now` into it.  Pure apart from the EEPROM I/O, so it is unit-tested.
ClockStatus CheckClock(ble::EepromIo& io, const models::DeviceLayout& layout, const models::Timestamp& now, bool sync, const Log& log);

// Program the pairing key into a monitor that is in pairing mode, bonding it to
// Windows first (releasing `others`, since Windows holds one OMRON bond at a time).
void PairMonitor(uint64_t address, const models::DeviceLayout& layout, const std::vector<uint64_t>& others, const Log& log);

// Bond if needed (releasing `others`), unlock, read every record, append new ones to the CSV,
// then compare the monitor clock with the PC and (if `sync_clock` and the layout verified) correct it.
DownloadResult Download(const storage::KnownDevice& device, const std::vector<uint64_t>& others, const Paths& paths, bool sync_clock, const Log& log);

}  // namespace omron::workflow
