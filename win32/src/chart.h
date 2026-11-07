// Direct2D chart control: systolic/diastolic and pulse over time for one monitor, or for all of them as one data set.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "storage.h"

namespace omron::chart {

constexpr wchar_t kClassName[] = L"OmronChart";

struct Point {
    int64_t epoch;  // seconds
    int systolic, diastolic, pulse;
    bool movement, irregular;
    std::wstring monitor;  // nickname of the monitor that took the reading
};

void RegisterClass(HINSTANCE instance);
HWND Create(HWND parent, int id);

// Replace the data (the readings to plot, any order) and the visible range.
// show_monitor names the monitor in the hover box, for a view that mixes monitors.
void SetData(HWND chart, const std::vector<storage::StoredReading>& rows, bool show_monitor);
void SetRangeDays(HWND chart, int days);  // 0 = all

}  // namespace omron::chart
