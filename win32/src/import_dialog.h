// Modal "Import from the OMRON app" dialog: which monitor and user an export belongs to.
#pragma once

#include <windows.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "storage.h"

namespace omron::import_dialog {

struct Choice {
    size_t monitor = 0;  // index into the devices passed to Show
    int user = 1;
};

// `file_text` describes the file (name, count, date span); `count_new` previews how many readings
// a choice would add.  `initial` is the monitor selected on opening.  Returns nullopt if cancelled.
std::optional<Choice> Show(HWND owner, const std::wstring& file_text, const std::vector<storage::KnownDevice>& devices, size_t initial,
                           const std::function<size_t(const Choice&)>& count_new);

}  // namespace omron::import_dialog
