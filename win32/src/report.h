// Builds the printable readings report (summary + table) from stored readings.
#pragma once

#include <string>
#include <vector>

#include "pdf.h"
#include "storage.h"

namespace omron::report {

struct Options {
    std::wstring scope;      // e.g. "ten (OMRON HEM-7342T)" or "all monitors"
    std::wstring period;     // e.g. "last 90 days (2026-06-20 to 2026-09-18)"
    std::wstring generated;  // "2026-09-18"
    bool show_monitor_column = false;
};

pdf::Report Build(const std::vector<storage::StoredReading>& rows, const Options& options);

}  // namespace omron::report
