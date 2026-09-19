#include "pair_dialog.h"

#include <windowsx.h>

#include <algorithm>
#include <cassert>
#include <memory>
#include <mutex>
#include <vector>

#include "app.h"
#include "ble.h"
#include "models.h"
#include "resource.h"
#include "storage.h"

namespace omron::pair_dialog {

namespace {

constexpr size_t kMaxFound = 16;
constexpr size_t kMaxName = 32;

struct Found {
    uint64_t address;
    std::wstring name;
    int rssi;
};

struct State {
    const std::vector<std::wstring>* taken = nullptr;
    std::optional<Result> result;
    std::mutex mutex;
    std::vector<Found> found;  // shared with the scanner thread; bounded by kMaxFound
    std::unique_ptr<ble::Scanner> scanner;
    std::vector<const models::DeviceLayout*> layouts;
};

State* Self(HWND dlg) {
    return reinterpret_cast<State*>(GetWindowLongPtrW(dlg, DWLP_USER));
}

void OnFound(HWND dlg, State& s, const ble::FoundDevice& dev) {
    assert(dlg != nullptr && dev.address != 0);
    if (!ble::IsOmronName(dev.name)) return;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        auto it = std::find_if(s.found.begin(), s.found.end(), [&](const Found& f) { return f.address == dev.address; });
        if (it == s.found.end()) {
            if (s.found.size() >= kMaxFound) return;
            s.found.push_back({dev.address, dev.name, dev.rssi});
            changed = true;
        } else if (it->rssi != dev.rssi) {
            it->rssi = dev.rssi;
            changed = true;
        }
    }
    if (changed) {
        const BOOL posted = PostMessageW(dlg, app::WM_APP_FOUND, 0, 0);
        assert(posted);
        (void)posted;
    }
}

void RefreshList(HWND dlg, State& s) {
    HWND list = GetDlgItem(dlg, IDC_PAIR_LIST);
    assert(list != nullptr);
    assert(s.found.size() <= kMaxFound);
    const int selected = ListBox_GetCurSel(list);
    ListBox_ResetContent(list);
    std::lock_guard<std::mutex> lock(s.mutex);
    for (const Found& f : s.found) {
        const std::wstring line = f.name + L"    " + storage::FormatAddress(f.address) + L"    " + std::to_wstring(f.rssi) + L" dBm";
        ListBox_AddString(list, line.c_str());
    }
    if (!s.found.empty()) ListBox_SetCurSel(list, selected >= 0 && selected < static_cast<int>(s.found.size()) ? selected : 0);
    SetDlgItemTextW(dlg, IDC_PAIR_STATUS, s.found.empty() ? L"Scanning ... no OMRON monitor heard yet." : L"Select the monitor, then click Pair.");
    EnableWindow(GetDlgItem(dlg, IDOK), !s.found.empty());
}

bool Collect(HWND dlg, State& s) {
    assert(dlg != nullptr && s.taken != nullptr);
    const int sel = ListBox_GetCurSel(GetDlgItem(dlg, IDC_PAIR_LIST));
    const int model = ComboBox_GetCurSel(GetDlgItem(dlg, IDC_PAIR_MODEL));
    wchar_t name[kMaxName + 1] = {};
    GetDlgItemTextW(dlg, IDC_PAIR_NAME, name, kMaxName + 1);
    std::wstring nick(name);
    while (!nick.empty() && nick.back() == L' ') nick.pop_back();
    while (!nick.empty() && nick.front() == L' ') nick.erase(nick.begin());
    if (sel < 0) {
        SetDlgItemTextW(dlg, IDC_PAIR_STATUS, L"Select the monitor from the list first.");
        return false;
    }
    if (model < 0 || model >= static_cast<int>(s.layouts.size())) {
        SetDlgItemTextW(dlg, IDC_PAIR_STATUS, L"Choose the monitor's model.");
        return false;
    }
    if (nick.empty() || nick.find(L',') != std::wstring::npos || nick.find(L'"') != std::wstring::npos) {
        SetDlgItemTextW(dlg, IDC_PAIR_STATUS, L"Enter a nickname (letters and digits, e.g. evolv).");
        return false;
    }
    if (std::find(s.taken->begin(), s.taken->end(), nick) != s.taken->end()) {
        SetDlgItemTextW(dlg, IDC_PAIR_STATUS, L"That nickname is already used - pick another.");
        return false;
    }
    Result r;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (sel >= static_cast<int>(s.found.size())) return false;
        r.address = s.found[static_cast<size_t>(sel)].address;
    }
    r.name = nick;
    r.model = s.layouts[static_cast<size_t>(model)]->model;
    assert(r.address != 0 && !r.model.empty());
    s.result = r;
    return true;
}

INT_PTR CALLBACK Proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INITDIALOG: {
            auto* s = reinterpret_cast<State*>(lp);
            SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(s));
            HWND combo = GetDlgItem(dlg, IDC_PAIR_MODEL);
            assert(combo != nullptr);
            for (const models::DeviceLayout* layout : models::AllLayouts()) {
                s->layouts.push_back(layout);
                ComboBox_AddString(combo, (std::wstring(layout->retail) + L"  -  " + layout->model).c_str());
            }
            assert(s->layouts.size() == models::kLayoutCount);
            ComboBox_SetCurSel(combo, 0);
            Edit_LimitText(GetDlgItem(dlg, IDC_PAIR_NAME), kMaxName);
            s->scanner = std::make_unique<ble::Scanner>([dlg, s](const ble::FoundDevice& dev) { OnFound(dlg, *s, dev); });
            try {
                s->scanner->Start();
            } catch (const winrt::hresult_error& exc) {
                SetDlgItemTextW(dlg, IDC_PAIR_STATUS, (L"Bluetooth is not available: " + std::wstring(exc.message())).c_str());
            }
            return TRUE;
        }
        case app::WM_APP_FOUND:
            RefreshList(dlg, *Self(dlg));
            return TRUE;
        case WM_COMMAND:
            if (LOWORD(wp) == IDOK) {
                if (Collect(dlg, *Self(dlg))) EndDialog(dlg, IDOK);
                return TRUE;
            }
            if (LOWORD(wp) == IDCANCEL) {
                EndDialog(dlg, IDCANCEL);
                return TRUE;
            }
            return FALSE;
        case WM_DESTROY:
            if (State* s = Self(dlg)) s->scanner.reset();
            return TRUE;
    }
    return FALSE;
}

}  // namespace

std::optional<Result> Show(HWND owner, const std::vector<std::wstring>& taken_names) {
    State state;
    state.taken = &taken_names;
    DialogBoxParamW(reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE)), MAKEINTRESOURCEW(IDD_PAIR), owner, Proc, reinterpret_cast<LPARAM>(&state));
    return state.result;
}

}  // namespace omron::pair_dialog
