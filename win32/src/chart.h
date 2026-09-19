// Direct2D chart control: systolic/diastolic and pulse over time for one monitor.
#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

#include "storage.h"

namespace omron::chart {

constexpr wchar_t kClassName[] = L"OmronChart";

struct Point {
    int64_t epoch;  // seconds
    int systolic, diastolic, pulse;
    bool movement, irregular;
};

void RegisterClass(HINSTANCE instance);
HWND Create(HWND parent, int id);

// Replace the data (all readings for the selected monitor, any order) and the visible range.
void SetData(HWND chart, const std::vector<storage::StoredReading>& rows);
void SetRangeDays(HWND chart, int days);  // 0 = all

}  // namespace omron::chart
