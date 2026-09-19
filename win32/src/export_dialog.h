// Modal "Export readings" dialog: which monitors and how far back.
#pragma once

#include <windows.h>

#include <functional>
#include <optional>
#include <string>

namespace omron::export_dialog {

struct Choice {
    bool all_monitors = false;
    int days = 0;  // 0 = everything
    bool pdf = true;  // also write a PDF report next to the CSV
};

// `count` previews how many readings a choice covers.  Returns nullopt if cancelled.
std::optional<Choice> Show(HWND owner, const std::wstring& selected_name, const std::function<size_t(const Choice&)>& count);

// Period presets shown in the dialog, in order.
struct Period {
    const wchar_t* label;
    int days;
};
const Period* Periods(size_t* count);

}  // namespace omron::export_dialog
