// Minimal PDF writer for a paginated text table (Letter, Helvetica, no dependencies).
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace omron::pdf {

constexpr size_t kMaxColumns = 12;
constexpr size_t kMaxRows = 100'000;

struct Column {
    std::wstring label;
    float x;           // left edge in points (or right edge when right_aligned)
    bool right_aligned;
};

struct Report {
    std::wstring title;                  // bold, large
    std::vector<std::wstring> subtitle;  // lines under the title
    std::vector<std::wstring> summary;   // lines in the summary box
    std::vector<Column> columns;
    std::vector<std::vector<std::wstring>> rows;  // each row has columns.size() cells
    std::wstring footer;                 // left of the page number
};

// Writes the report; returns the number of pages.  Throws std::runtime_error on I/O failure.
size_t WriteReport(const std::filesystem::path& path, const Report& report);

// Width of `text` in points at `size` for Helvetica (regular); used for right alignment and fitting.
float TextWidth(const std::wstring& text, float size, bool bold);

}  // namespace omron::pdf
