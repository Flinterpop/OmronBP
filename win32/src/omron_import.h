// Reading the CSV that the OMRON connect phone app exports.  Mirrors omron_bp/omron_import.py.
//
//   Date,Time,Systolic (mmHg),Diastolic (mmHg),Pulse (bpm),Notes
//   "Dec 25, 2025",6:10 AM,150,88,48,
//
// Columns are found by header name.  The export has no monitor, user or seconds: readings are
// stamped at second 0 and the caller says which monitor and user they belong to.
#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "models.h"

namespace omron::omron_import {

constexpr size_t kMaxRows = 100'000;
constexpr size_t kMaxFileBytes = 16 * 1024 * 1024;

// "Dec 25, 2025" and "6:10 AM" (or "18:10"); nullopt if either is not recognised.
std::optional<models::Timestamp> ParseDateTime(std::string_view date, std::string_view time);

// All readings in the export text, oldest first.  Throws std::runtime_error naming the bad line.
std::vector<models::Reading> ParseExport(const std::string& text);

// Reads and parses the file (UTF-8, with or without a BOM).
std::vector<models::Reading> ReadExport(const std::filesystem::path& file);

}  // namespace omron::omron_import
