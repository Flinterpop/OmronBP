#include "export_dialog.h"

#include <windowsx.h>

#include <cassert>

#include "resource.h"

namespace omron::export_dialog {

namespace {

constexpr Period kPeriods[] = {
    {L"Last 30 days", 30}, {L"Last 60 days", 60}, {L"Last 90 days", 90}, {L"Last 180 days", 180}, {L"Last 12 months", 365}, {L"Everything", 0},
};
constexpr int kDefaultPeriod = 2;  // 90 days

struct State {
    const std::wstring* selected_name = nullptr;
    const std::function<size_t(const Choice&)>* count = nullptr;
    std::optional<Choice> result;
};

State* Self(HWND dlg) {
    return reinterpret_cast<State*>(GetWindowLongPtrW(dlg, DWLP_USER));
}

Choice Current(HWND dlg) {
    Choice c;
    c.all_monitors = IsDlgButtonChecked(dlg, IDC_EXPORT_ALL) == BST_CHECKED;
    const int sel = ComboBox_GetCurSel(GetDlgItem(dlg, IDC_EXPORT_PERIOD));
    c.days = (sel >= 0 && sel < static_cast<int>(std::size(kPeriods))) ? kPeriods[sel].days : 0;
    c.pdf = IsDlgButtonChecked(dlg, IDC_EXPORT_PDF) == BST_CHECKED;
    return c;
}

void UpdatePreview(HWND dlg) {
    State* s = Self(dlg);
    const size_t n = (*s->count)(Current(dlg));
    const std::wstring text = n == 0 ? L"No readings match - nothing to export." : std::to_wstring(n) + (n == 1 ? L" reading will be exported." : L" readings will be exported.");
    SetDlgItemTextW(dlg, IDC_EXPORT_INFO, text.c_str());
    EnableWindow(GetDlgItem(dlg, IDOK), n > 0);
}

INT_PTR CALLBACK Proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INITDIALOG: {
            auto* s = reinterpret_cast<State*>(lp);
            SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(s));
            HWND combo = GetDlgItem(dlg, IDC_EXPORT_PERIOD);
            for (const Period& p : kPeriods) ComboBox_AddString(combo, p.label);
            ComboBox_SetCurSel(combo, kDefaultPeriod);
            const bool have_selected = !s->selected_name->empty();
            SetDlgItemTextW(dlg, IDC_EXPORT_SELECTED, have_selected ? (L"Selected monitor (" + *s->selected_name + L")").c_str() : L"Selected monitor");
            EnableWindow(GetDlgItem(dlg, IDC_EXPORT_SELECTED), have_selected);
            CheckRadioButton(dlg, IDC_EXPORT_SELECTED, IDC_EXPORT_ALL, have_selected ? IDC_EXPORT_SELECTED : IDC_EXPORT_ALL);
            CheckDlgButton(dlg, IDC_EXPORT_PDF, BST_CHECKED);
            UpdatePreview(dlg);
            return TRUE;
        }
        case WM_COMMAND: {
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
            if (id == IDC_EXPORT_SELECTED || id == IDC_EXPORT_ALL || (id == IDC_EXPORT_PERIOD && HIWORD(wp) == CBN_SELCHANGE)) {
                UpdatePreview(dlg);
                return TRUE;
            }
            return FALSE;
        }
    }
    return FALSE;
}

}  // namespace

const Period* Periods(size_t* count) {
    assert(count != nullptr);
    *count = std::size(kPeriods);
    return kPeriods;
}

std::optional<Choice> Show(HWND owner, const std::wstring& selected_name, const std::function<size_t(const Choice&)>& count) {
    assert(count);
    State state;
    state.selected_name = &selected_name;
    state.count = &count;
    DialogBoxParamW(reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE)), MAKEINTRESOURCEW(IDD_EXPORT), owner, Proc, reinterpret_cast<LPARAM>(&state));
    return state.result;
}

}  // namespace omron::export_dialog
