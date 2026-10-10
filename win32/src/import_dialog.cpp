#include "import_dialog.h"

#include <windowsx.h>

#include <cassert>

#include "models.h"
#include "resource.h"

namespace omron::import_dialog {

namespace {

struct State {
    const std::wstring* file_text = nullptr;
    const std::vector<storage::KnownDevice>* devices = nullptr;
    size_t initial = 0;
    const std::function<size_t(const Choice&)>* count_new = nullptr;
    std::optional<Choice> result;
};

State* Self(HWND dlg) {
    return reinterpret_cast<State*>(GetWindowLongPtrW(dlg, DWLP_USER));
}

Choice Current(HWND dlg) {
    assert(dlg != nullptr);
    Choice c;
    const int monitor = ComboBox_GetCurSel(GetDlgItem(dlg, IDC_IMPORT_MONITOR));
    const int user = ComboBox_GetCurSel(GetDlgItem(dlg, IDC_IMPORT_USER));
    c.monitor = monitor > 0 ? static_cast<size_t>(monitor) : 0;
    c.user = user > 0 ? user + 1 : 1;
    return c;
}

// One entry per user slot of the chosen monitor's model, keeping the current pick when it still exists.
void FillUsers(HWND dlg) {
    State* s = Self(dlg);
    assert(s != nullptr && s->devices != nullptr);
    const Choice c = Current(dlg);
    assert(c.monitor < s->devices->size());
    const models::DeviceLayout* layout = models::FindLayout((*s->devices)[c.monitor].model);
    const int users = layout ? static_cast<int>(layout->user_count) : 1;
    HWND combo = GetDlgItem(dlg, IDC_IMPORT_USER);
    const int previous = ComboBox_GetCurSel(combo);
    ComboBox_ResetContent(combo);
    for (int u = 1; u <= users && u <= static_cast<int>(models::kMaxUsers); ++u) ComboBox_AddString(combo, (L"User " + std::to_wstring(u)).c_str());
    ComboBox_SetCurSel(combo, previous >= 0 && previous < users ? previous : 0);
}

void UpdatePreview(HWND dlg) {
    State* s = Self(dlg);
    assert(s != nullptr && s->count_new != nullptr);
    const size_t n = (*s->count_new)(Current(dlg));
    const std::wstring text = n == 0 ? L"All of these readings are already stored for this monitor and user - nothing to import."
                                     : std::to_wstring(n) + (n == 1 ? L" new reading will be added to readings.csv." : L" new readings will be added to readings.csv.");
    SetDlgItemTextW(dlg, IDC_IMPORT_INFO, text.c_str());
    EnableWindow(GetDlgItem(dlg, IDOK), n > 0);
}

void OnInit(HWND dlg, State* s) {
    assert(s != nullptr && !s->devices->empty());
    SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(s));
    SetDlgItemTextW(dlg, IDC_IMPORT_FILE, s->file_text->c_str());
    HWND combo = GetDlgItem(dlg, IDC_IMPORT_MONITOR);
    for (size_t i = 0; i < s->devices->size() && i < storage::kMaxDevices; ++i) {
        const storage::KnownDevice& d = (*s->devices)[i];
        const models::DeviceLayout* layout = models::FindLayout(d.model);
        ComboBox_AddString(combo, (d.name + L"  (" + (layout ? std::wstring(layout->retail) : d.model) + L")").c_str());
    }
    ComboBox_SetCurSel(combo, static_cast<int>(s->initial < s->devices->size() ? s->initial : 0));
    FillUsers(dlg);
    UpdatePreview(dlg);
}

INT_PTR CALLBACK Proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INITDIALOG) {
        OnInit(dlg, reinterpret_cast<State*>(lp));
        return TRUE;
    }
    if (msg != WM_COMMAND) return FALSE;
    const int id = LOWORD(wp);
    if (id == IDOK) {
        Self(dlg)->result = Current(dlg);
        EndDialog(dlg, IDOK);
        return TRUE;
    }
    if (id == IDCANCEL) {
        EndDialog(dlg, IDCANCEL);
        return TRUE;
    }
    if (HIWORD(wp) != CBN_SELCHANGE || (id != IDC_IMPORT_MONITOR && id != IDC_IMPORT_USER)) return FALSE;
    if (id == IDC_IMPORT_MONITOR) FillUsers(dlg);
    UpdatePreview(dlg);
    return TRUE;
}

}  // namespace

std::optional<Choice> Show(HWND owner, const std::wstring& file_text, const std::vector<storage::KnownDevice>& devices, size_t initial,
                           const std::function<size_t(const Choice&)>& count_new) {
    assert(count_new && !devices.empty());
    assert(owner != nullptr);
    State state;
    state.file_text = &file_text;
    state.devices = &devices;
    state.initial = initial;
    state.count_new = &count_new;
    const INT_PTR ended = DialogBoxParamW(reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE)), MAKEINTRESOURCEW(IDD_IMPORT), owner, Proc,
                                          reinterpret_cast<LPARAM>(&state));
    if (ended <= 0) return std::nullopt;  // -1 / 0: the template could not be created
    return state.result;
}

}  // namespace omron::import_dialog
