#include "omron_import.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace omron::omron_import {

namespace {

using Row = std::vector<std::string>;

constexpr std::array<std::string_view, 12> kMonths = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
// A flag column counts as set unless it holds one of these (after trimming and lower-casing).
constexpr std::array<std::string_view, 7> kFalseFlags = {"", "0", "no", "false", "-", "--", "none"};
constexpr size_t kMaxColumns = 64;
constexpr int kNoon = 12;

std::string Lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    assert(out.size() == text.size());
    return out;
}

std::string_view Trim(std::string_view text) {
    const size_t first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    const size_t last = text.find_last_not_of(" \t");
    assert(last >= first);
    return text.substr(first, last - first + 1);
}

// Non-negative decimal of 1 to 4 digits; nullopt otherwise.
std::optional<int> Digits(std::string_view text) {
    if (text.empty() || text.size() > 4) return std::nullopt;
    int value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return std::nullopt;
        value = value * 10 + (c - '0');
    }
    assert(value >= 0 && value <= 9999);
    return value;
}

// RFC 4180 style: quoted fields may hold commas, doubled quotes and line breaks.
std::vector<Row> SplitCsv(const std::string& text) {
    assert(text.size() <= kMaxFileBytes);
    std::vector<Row> rows;
    Row row;
    std::string field;
    bool quoted = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted) {
            if (c != '"') field.push_back(c);
            else if (i + 1 < text.size() && text[i + 1] == '"') field.push_back(text[++i]);
            else quoted = false;
        } else if (c == '"') {
            quoted = true;
        } else if (c == ',' || c == '\n') {
            row.push_back(field);
            field.clear();
            if (row.size() > kMaxColumns) throw std::runtime_error("too many columns");
            if (c == '\n') rows.push_back(std::move(row)), row.clear();
            if (rows.size() > kMaxRows) throw std::runtime_error("more than 100,000 rows");
        } else if (c != '\r') {
            field.push_back(c);
        }
    }
    if (!field.empty() || !row.empty()) rows.push_back(row), rows.back().push_back(field);
    return rows;
}

std::optional<size_t> Column(const Row& header, std::string_view prefix, bool anywhere) {
    assert(!prefix.empty() && header.size() <= kMaxColumns);
    for (size_t i = 0; i < header.size(); ++i) {
        const std::string name = Lower(Trim(header[i]));
        if (anywhere ? name.find(prefix) != std::string::npos : name.rfind(prefix, 0) == 0) return i;
    }
    return std::nullopt;
}

bool Flag(const Row& row, std::optional<size_t> col) {
    if (!col || *col >= row.size()) return false;
    const std::string value = Lower(Trim(row[*col]));
    return std::find(kFalseFlags.begin(), kFalseFlags.end(), value) == kFalseFlags.end();
}

int Number(const std::string& text, int lo, int hi, const char* what, size_t line) {
    assert(lo <= hi && what != nullptr);
    const std::optional<int> value = Digits(Trim(text));
    if (!value || *value < lo || *value > hi) {
        throw std::runtime_error("line " + std::to_string(line) + ": " + what + " '" + text + "' is not between " + std::to_string(lo) + " and " + std::to_string(hi));
    }
    return *value;
}

}  // namespace

std::optional<models::Timestamp> ParseDateTime(std::string_view date, std::string_view time) {
    std::string d(date);
    std::replace(d.begin(), d.end(), ',', ' ');
    std::istringstream parts(d);
    std::string month_text, day_text, year_text, extra;
    if (!(parts >> month_text >> day_text >> year_text) || (parts >> extra) || month_text.size() < 3) return std::nullopt;
    const auto month = std::find(kMonths.begin(), kMonths.end(), Lower(month_text.substr(0, 3)));
    std::string clock = Lower(Trim(time));
    const bool pm = clock.ends_with("pm"), am = clock.ends_with("am");
    if (am || pm) clock = std::string(Trim(std::string_view(clock).substr(0, clock.size() - 2)));
    const size_t colon = clock.find(':');
    if (month == kMonths.end() || colon == std::string::npos) return std::nullopt;
    const auto day = Digits(day_text), year = Digits(year_text);
    auto hour = Digits(std::string_view(clock).substr(0, colon));
    const auto minute = Digits(std::string_view(clock).substr(colon + 1));
    if (!day || !year || !hour || !minute) return std::nullopt;
    if (am || pm) {
        if (*hour < 1 || *hour > kNoon) return std::nullopt;
        hour = *hour % kNoon + (pm ? kNoon : 0);
    }
    models::Timestamp t;
    t.year = *year, t.month = static_cast<int>(month - kMonths.begin()) + 1, t.day = *day, t.hour = *hour, t.minute = *minute, t.second = 0;
    if (!t.IsValid()) return std::nullopt;
    return t;
}

std::vector<models::Reading> ParseExport(const std::string& text) {
    const std::vector<Row> rows = SplitCsv(text.starts_with("\xEF\xBB\xBF") ? text.substr(3) : text);
    if (rows.empty()) throw std::runtime_error("the file is empty");
    const Row& header = rows[0];
    const auto date = Column(header, "date", false), time = Column(header, "time", false), sys = Column(header, "systolic", false),
               dia = Column(header, "diastolic", false), pulse = Column(header, "pulse", false);
    if (!date || !time || !sys || !dia || !pulse) throw std::runtime_error("not an OMRON app export: expected Date, Time, Systolic, Diastolic and Pulse columns");
    const auto irregular = Column(header, "irregular", true), movement = Column(header, "movement", true);
    const size_t needed = std::max({*date, *time, *sys, *dia, *pulse}) + 1;
    std::vector<models::Reading> out;
    for (size_t i = 1; i < rows.size(); ++i) {
        const Row& row = rows[i];
        if (std::all_of(row.begin(), row.end(), [](const std::string& f) { return Trim(f).empty(); })) continue;
        if (row.size() < needed) throw std::runtime_error("line " + std::to_string(i + 1) + ": too few columns");
        const std::optional<models::Timestamp> when = ParseDateTime(row[*date], row[*time]);
        if (!when) throw std::runtime_error("line " + std::to_string(i + 1) + ": unrecognised date/time '" + row[*date] + " " + row[*time] + "'");
        models::Reading r;
        r.when = *when;
        r.systolic = Number(row[*sys], models::kMinPressure, models::kMaxPressure, "systolic", i + 1);
        r.diastolic = Number(row[*dia], models::kMinPressure, models::kMaxPressure, "diastolic", i + 1);
        r.pulse = Number(row[*pulse], models::kMinPulse, models::kMaxPulse, "pulse", i + 1);
        r.movement = Flag(row, movement);
        r.irregular_heartbeat = Flag(row, irregular);
        out.push_back(r);
    }
    std::stable_sort(out.begin(), out.end(), [](const models::Reading& a, const models::Reading& b) { return a.when.ToEpoch() < b.when.ToEpoch(); });
    assert(out.size() < rows.size());
    return out;
}

std::vector<models::Reading> ReadExport(const std::filesystem::path& file) {
    assert(!file.empty());
    if (std::filesystem::file_size(file) > kMaxFileBytes) throw std::runtime_error("the file is larger than 16 MB");
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open the file");
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return ParseExport(buffer.str());
}

}  // namespace omron::omron_import
