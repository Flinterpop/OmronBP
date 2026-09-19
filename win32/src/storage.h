// readings.csv and devices.json, in exactly the format the Python tool uses.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "models.h"

namespace omron::storage {

constexpr size_t kMaxRows = 1'000'000;
constexpr size_t kMaxDevices = 16;

struct StoredReading {
    models::Reading reading;
    std::wstring device;  // nickname
    std::wstring model;
    int user = 1;
    int64_t epoch = 0;  // derived from reading.when, for ordering and the chart
};

struct KnownDevice {
    std::wstring name;
    std::wstring model;  // internal model, e.g. HEM-7342T
    std::wstring address;  // "AA:BB:CC:DD:EE:FF"
    uint64_t AddressValue() const;
};

// All rows of the CSV (empty if the file does not exist).  Throws std::runtime_error on a malformed file.
std::vector<StoredReading> LoadReadings(const std::filesystem::path& csv);

// Appends readings not already present (same device, user and timestamp).  Returns the number written.
size_t AppendReadings(const std::filesystem::path& csv, const std::wstring& device, const std::wstring& model,
                      const std::vector<std::vector<models::Reading>>& per_user);

// Readings taken on or after `since_epoch` (0 = all), optionally for one nickname only.  Oldest first.
std::vector<StoredReading> FilterReadings(const std::vector<StoredReading>& rows, int64_t since_epoch, const std::wstring& device);

// Writes rows to a new CSV in the same format as readings.csv (overwrites).  Returns rows written.
size_t WriteReadings(const std::filesystem::path& csv, const std::vector<StoredReading>& rows);

// Local calendar date/time now, as a Timestamp (for "last N days" bounds).
models::Timestamp LocalNow();

std::vector<KnownDevice> LoadDevices(const std::filesystem::path& json);
void SaveDevices(const std::filesystem::path& json, const std::vector<KnownDevice>& devices);

std::wstring FormatAddress(uint64_t address);  // "AA:BB:CC:DD:EE:FF"
uint64_t ParseAddress(const std::wstring& text);  // 0 if malformed

std::string ToUtf8(const std::wstring& text);
std::wstring FromUtf8(const std::string& text);

}  // namespace omron::storage
