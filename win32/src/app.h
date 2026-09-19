// Shared UI plumbing: window messages, the bounded worker->UI log queue, DPI helpers.
#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <string>

#include "ble.h"

namespace omron::app {

constexpr UINT WM_APP_LOG = WM_APP + 1;    // worker -> UI: entries waiting in the log queue
constexpr UINT WM_APP_DONE = WM_APP + 2;   // worker finished; wParam 1 = success, 0 = failure
constexpr UINT WM_APP_FOUND = WM_APP + 3;  // scanner heard a monitor; pair dialog refreshes its list

constexpr wchar_t kAppTitle[] = L"OMRON Blood Pressure";

// Fixed-capacity ring of log lines written by the worker thread and drained by the UI thread.
class LogQueue {
public:
    struct Entry {
        ble::Level level = ble::Level::Info;
        std::wstring text;
    };

    void Push(ble::Level level, const std::wstring& text);
    bool Pop(Entry& out);

private:
    static constexpr size_t kCapacity = 256;
    std::mutex mutex_;
    std::array<Entry, kCapacity> ring_{};
    size_t head_ = 0;
    size_t count_ = 0;
    size_t dropped_ = 0;
};

inline int Scale(HWND hwnd, int dip) {
    const UINT dpi = GetDpiForWindow(hwnd);
    return MulDiv(dip, static_cast<int>(dpi), 96);
}

HFONT CreateUiFont(HWND hwnd, int point_size, bool bold = false);

}  // namespace omron::app
