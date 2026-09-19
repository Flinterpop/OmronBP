// Modal "Pair a monitor" dialog: shows monitors as they advertise, collects model and nickname.
#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace omron::pair_dialog {

struct Result {
    uint64_t address = 0;
    std::wstring name;   // nickname
    std::wstring model;  // internal model, e.g. HEM-7600T
};

// Returns nullopt if cancelled.  `taken_names` keeps nicknames unique.
std::optional<Result> Show(HWND owner, const std::vector<std::wstring>& taken_names);

}  // namespace omron::pair_dialog
