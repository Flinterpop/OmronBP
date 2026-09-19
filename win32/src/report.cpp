#include "report.h"

#include <algorithm>
#include <cassert>

namespace omron::report {

namespace {

constexpr int kSysHigh = 140, kDiaHigh = 90;

std::wstring Date(const models::Timestamp& t) {
    wchar_t buf[16];
    swprintf_s(buf, L"%04d-%02d-%02d", t.year, t.month, t.day);
    return buf;
}

std::wstring Time(const models::Timestamp& t) {
    wchar_t buf[8];
    swprintf_s(buf, L"%02d:%02d", t.hour, t.minute);
    return buf;
}

std::wstring Summary(const std::vector<storage::StoredReading>& rows) {
    assert(!rows.empty());
    long sys = 0, dia = 0, pulse = 0;
    int sys_lo = 999, sys_hi = 0, dia_lo = 999, dia_hi = 0, p_lo = 999, p_hi = 0;
    for (const storage::StoredReading& r : rows) {
        const models::Reading& m = r.reading;
        sys += m.systolic, dia += m.diastolic, pulse += m.pulse;
        sys_lo = std::min(sys_lo, m.systolic), sys_hi = std::max(sys_hi, m.systolic);
        dia_lo = std::min(dia_lo, m.diastolic), dia_hi = std::max(dia_hi, m.diastolic);
        p_lo = std::min(p_lo, m.pulse), p_hi = std::max(p_hi, m.pulse);
    }
    const long n = static_cast<long>(rows.size());
    assert(sys_lo <= sys_hi && dia_lo <= dia_hi && p_lo <= p_hi);
    std::wstring text = L"Average " + std::to_wstring(sys / n) + L"/" + std::to_wstring(dia / n) + L" mmHg   (systolic " + std::to_wstring(sys_lo) + L"-" +
                        std::to_wstring(sys_hi) + L", diastolic " + std::to_wstring(dia_lo) + L"-" + std::to_wstring(dia_hi) + L")   pulse " +
                        std::to_wstring(pulse / n) + L" bpm (" + std::to_wstring(p_lo) + L"-" + std::to_wstring(p_hi) + L")";
    return text;
}

std::wstring Flags(const std::vector<storage::StoredReading>& rows) {
    assert(!rows.empty() && rows.size() <= pdf::kMaxRows);
    int high = 0, irregular = 0, movement = 0;
    for (const storage::StoredReading& r : rows) {
        if (r.reading.systolic >= kSysHigh || r.reading.diastolic >= kDiaHigh) ++high;
        if (r.reading.irregular_heartbeat) ++irregular;
        if (r.reading.movement) ++movement;
    }
    assert(high <= static_cast<int>(rows.size()));
    std::wstring text = std::to_wstring(high) + L" of " + std::to_wstring(rows.size()) + L" readings at or above 140/90";
    if (irregular) text += L"   -   " + std::to_wstring(irregular) + L" flagged irregular heartbeat";
    if (movement) text += L"   -   " + std::to_wstring(movement) + L" flagged movement";
    return text;
}

}  // namespace

pdf::Report Build(const std::vector<storage::StoredReading>& rows, const Options& options) {
    assert(!options.scope.empty() && !options.period.empty() && !options.generated.empty());
    assert(rows.size() <= pdf::kMaxRows);
    pdf::Report r;
    r.title = L"Blood pressure readings";
    r.subtitle = {L"Monitor: " + options.scope, L"Period: " + options.period + L"      Generated: " + options.generated + L"      Times are as set on the monitor."};
    if (!rows.empty()) {
        r.summary = {std::to_wstring(rows.size()) + (rows.size() == 1 ? L" reading, " : L" readings, ") + Date(rows.front().reading.when) + L" to " + Date(rows.back().reading.when),
                     Summary(rows), Flags(rows)};
    }
    // Column x positions in points; numbers are right-aligned at their x.
    if (options.show_monitor_column) {
        r.columns = {{L"Date", 54, false}, {L"Time", 124, false}, {L"Monitor", 168, false}, {L"Systolic", 300, true}, {L"Diastolic", 360, true}, {L"Pulse", 412, true}, {L"Notes", 432, false}};
    } else {
        r.columns = {{L"Date", 54, false}, {L"Time", 130, false}, {L"Systolic", 250, true}, {L"Diastolic", 320, true}, {L"Pulse", 380, true}, {L"Notes", 404, false}};
    }
    r.rows.reserve(rows.size());
    for (const storage::StoredReading& s : rows) {
        std::wstring notes;
        if (s.reading.irregular_heartbeat) notes = L"irregular heartbeat";
        if (s.reading.movement) notes += notes.empty() ? L"movement" : L", movement";
        std::vector<std::wstring> cells{Date(s.reading.when), Time(s.reading.when)};
        if (options.show_monitor_column) cells.push_back(s.device + (s.user > 1 ? L" (user " + std::to_wstring(s.user) + L")" : L""));
        cells.push_back(std::to_wstring(s.reading.systolic));
        cells.push_back(std::to_wstring(s.reading.diastolic));
        cells.push_back(std::to_wstring(s.reading.pulse));
        cells.push_back(notes);
        r.rows.push_back(std::move(cells));
    }
    r.footer = L"Exported from OMRON monitor memory by OmronBP. Reference: 140/90 mmHg.";
    assert(r.rows.size() == rows.size());
    return r;
}

}  // namespace omron::report
