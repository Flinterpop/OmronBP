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
    bool one_person = false;  // every reading is the same person, so user slots mean nothing to the reader
};

// Mean of n values summing to sum, rounded to the nearest whole number with .5 going up, as the HTML page's
// Math.round does, so the app, the PDF and the page agree on every average.
long RoundedMean(long sum, long n);

pdf::Report Build(const std::vector<storage::StoredReading>& rows, const Options& options);

}  // namespace omron::report
