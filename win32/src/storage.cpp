#include "storage.h"

#include <windows.h>

#include <algorithm>
#include <cassert>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace omron::storage {

namespace {

constexpr const char* kHeader = "timestamp,model,device,user,systolic,diastolic,pulse,movement,irregular_heartbeat";
constexpr size_t kColumns = 9;

constexpr size_t kMaxLineChars = 4096;

std::vector<std::string> SplitCsvLine(const std::string& line) {
    assert(line.size() <= kMaxLineChars);
    std::vector<std::string> fields;
    std::string current;
    for (char c : line) {
        if (c == ',') {
            fields.push_back(current);
            current.clear();
        } else if (c != '\r') {
            current.push_back(c);
        }
    }
    fields.push_back(current);
    assert(!fields.empty());
    return fields;
}

int ParseInt(const std::string& text, int lo, int hi) {
    if (text.empty() || text.size() > 6) throw std::runtime_error("bad number in CSV: '" + text + "'");
    int value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') throw std::runtime_error("bad number in CSV: '" + text + "'");
        value = value * 10 + (c - '0');
    }
    if (value < lo || value > hi) throw std::runtime_error("number out of range in CSV: " + text);
    return value;
}

// A reading is identified by monitor, user, time and values; the values matter because a
// monitor with a stopped clock stamps every new measurement with the same time.
using Key = std::tuple<std::wstring, int, std::string, int, int, int>;

Key MakeKey(const std::wstring& device, int user, const models::Reading& r) {
    return {device, user, r.when.ToString(), r.systolic, r.diastolic, r.pulse};
}

// --- minimal JSON for {"name": {"model": "...", "address": "..."}, ...} ---------------------------

class JsonReader {
public:
    explicit JsonReader(const std::string& text) : text_(text) {}

    std::vector<KnownDevice> ReadDevices() {
        assert(pos_ == 0);
        std::vector<KnownDevice> devices;
        SkipSpace();
        Expect('{');
        SkipSpace();
        if (Peek() == '}') return devices;
        for (size_t entries = 0; entries <= kMaxDevices; ++entries) {
            KnownDevice device;
            device.name = FromUtf8(ReadString());
            SkipSpace();
            Expect(':');
            ReadEntry(device);
            if (device.model.empty() || device.address.empty()) throw std::runtime_error("devices.json entry incomplete");
            devices.push_back(device);
            assert(devices.size() <= kMaxDevices + 1);
            SkipSpace();
            if (Peek() == ',') {
                ++pos_;
                SkipSpace();
                continue;
            }
            Expect('}');
            return devices;
        }
        throw std::runtime_error("devices.json has too many entries");
    }

private:
    void ReadEntry(KnownDevice& device) {
        assert(!device.name.empty());
        SkipSpace();
        Expect('{');
        for (size_t fields = 0; fields < 8; ++fields) {
            SkipSpace();
            const std::string key = ReadString();
            SkipSpace();
            Expect(':');
            SkipSpace();
            const std::string value = ReadString();
            assert(!key.empty());
            if (key == "model") device.model = FromUtf8(value);
            if (key == "address") device.address = FromUtf8(value);
            SkipSpace();
            if (Peek() == ',') {
                ++pos_;
                continue;
            }
            Expect('}');
            return;
        }
        throw std::runtime_error("devices.json entry has too many fields");
    }

    char Peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }
    void SkipSpace() {
        const size_t limit = text_.size();  // loop bound: at most one step per remaining character
        while (pos_ < limit && (text_[pos_] == ' ' || text_[pos_] == '\n' || text_[pos_] == '\r' || text_[pos_] == '\t')) ++pos_;
        assert(pos_ <= limit);
    }
    void Expect(char c) {
        if (Peek() != c) throw std::runtime_error(std::string("devices.json: expected '") + c + "'");
        ++pos_;
    }
    std::string ReadString() {
        Expect('"');
        std::string out;
        const size_t limit = text_.size();  // loop bound: at most one step per remaining character
        while (pos_ < limit && text_[pos_] != '"') {
            if (text_[pos_] == '\\' && pos_ + 1 < text_.size()) ++pos_;  // keep the escaped char literally
            out.push_back(text_[pos_++]);
        }
        Expect('"');
        assert(out.size() < kMaxLineChars);
        return out;
    }

    const std::string& text_;
    size_t pos_ = 0;
};

std::string ReadWholeFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

}  // namespace

std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) throw std::runtime_error("text cannot be converted to UTF-8");
    std::string out(static_cast<size_t>(size), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    assert(written == size);
    if (written != size) throw std::runtime_error("UTF-8 conversion failed");
    return out;
}

std::wstring FromUtf8(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) throw std::runtime_error("text is not valid UTF-8");
    std::wstring out(static_cast<size_t>(size), L'\0');
    const int written = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size);
    assert(written == size);
    if (written != size) throw std::runtime_error("UTF-8 conversion failed");
    return out;
}

std::wstring FormatAddress(uint64_t address) {
    wchar_t buf[24];
    swprintf_s(buf, L"%02llX:%02llX:%02llX:%02llX:%02llX:%02llX", (address >> 40) & 0xFF, (address >> 32) & 0xFF,
               (address >> 24) & 0xFF, (address >> 16) & 0xFF, (address >> 8) & 0xFF, address & 0xFF);
    return buf;
}

uint64_t ParseAddress(const std::wstring& text) {
    assert(text.size() <= 17);
    uint64_t value = 0;
    int digits = 0;
    for (wchar_t c : text) {
        if (c == L':' || c == L'-') continue;
        int nibble;
        if (c >= L'0' && c <= L'9') nibble = c - L'0';
        else if (c >= L'a' && c <= L'f') nibble = c - L'a' + 10;
        else if (c >= L'A' && c <= L'F') nibble = c - L'A' + 10;
        else return 0;
        value = (value << 4) | static_cast<uint64_t>(nibble);
        ++digits;
    }
    assert(value <= 0xFFFFFFFFFFFFull);
    return digits == 12 ? value : 0;
}

uint64_t KnownDevice::AddressValue() const {
    return ParseAddress(address);
}

std::vector<StoredReading> LoadReadings(const std::filesystem::path& csv) {
    assert(!csv.empty());
    std::vector<StoredReading> rows;
    if (!std::filesystem::exists(csv)) return rows;
    std::ifstream in(csv);
    if (!in) throw std::runtime_error("cannot open " + csv.string());
    std::string line;
    if (!std::getline(in, line)) return rows;
    if (line.size() >= 3 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line != kHeader) throw std::runtime_error("unexpected CSV header in " + csv.string());
    for (size_t n = 0; std::getline(in, line); ++n) {
        if (n >= kMaxRows) throw std::runtime_error("CSV has more than 1,000,000 rows");
        if (line.empty() || line == "\r") continue;
        const std::vector<std::string> f = SplitCsvLine(line);
        if (f.size() != kColumns) throw std::runtime_error("CSV row " + std::to_string(n + 2) + " has " + std::to_string(f.size()) + " columns");
        StoredReading row;
        const auto when = models::Timestamp::Parse(f[0]);
        if (!when) throw std::runtime_error("CSV row " + std::to_string(n + 2) + ": bad timestamp");
        row.reading.when = *when;
        row.model = FromUtf8(f[1]);
        row.device = FromUtf8(f[2]);
        row.user = ParseInt(f[3], 1, 8);
        row.reading.systolic = ParseInt(f[4], models::kMinPressure, models::kMaxPressure);
        row.reading.diastolic = ParseInt(f[5], models::kMinPressure, models::kMaxPressure);
        row.reading.pulse = ParseInt(f[6], models::kMinPulse, models::kMaxPulse);
        row.reading.movement = ParseInt(f[7], 0, 1) != 0;
        row.reading.irregular_heartbeat = ParseInt(f[8], 0, 1) != 0;
        row.epoch = when->ToEpoch();
        rows.push_back(row);
    }
    assert(rows.size() <= kMaxRows);
    return rows;
}

size_t AppendReadings(const std::filesystem::path& csv, const std::wstring& device, const std::wstring& model,
                      const std::vector<std::vector<models::Reading>>& per_user) {
    assert(!device.empty() && !model.empty());
    assert(per_user.size() <= models::kMaxUsers);
    std::set<Key> seen;
    for (const StoredReading& row : LoadReadings(csv)) {
        seen.insert(MakeKey(row.device, row.user, row.reading));
    }
    const bool is_new = !std::filesystem::exists(csv);
    std::ofstream out(csv, std::ios::app);
    if (!out) throw std::runtime_error("cannot write " + csv.string());
    if (is_new) out << kHeader << "\n";
    const std::string model8 = ToUtf8(model), device8 = ToUtf8(device);
    size_t written = 0;
    for (size_t u = 0; u < per_user.size(); ++u) {
        for (const models::Reading& r : per_user[u]) {
            const int user = static_cast<int>(u) + 1;
            if (!seen.insert(MakeKey(device, user, r)).second) continue;
            out << r.when.ToString() << ',' << model8 << ',' << device8 << ',' << user << ',' << r.systolic << ','
                << r.diastolic << ',' << r.pulse << ',' << (r.movement ? 1 : 0) << ',' << (r.irregular_heartbeat ? 1 : 0)
                << "\n";
            ++written;
        }
    }
    return written;
}

std::vector<StoredReading> FilterReadings(const std::vector<StoredReading>& rows, int64_t since_epoch, const std::wstring& device) {
    std::vector<StoredReading> out;
    for (const StoredReading& r : rows) {
        if (r.epoch < since_epoch) continue;
        if (!device.empty() && r.device != device) continue;
        out.push_back(r);
    }
    std::sort(out.begin(), out.end(), [](const StoredReading& a, const StoredReading& b) { return a.epoch < b.epoch; });
    return out;
}

size_t WriteReadings(const std::filesystem::path& csv, const std::vector<StoredReading>& rows) {
    assert(rows.size() <= kMaxRows);
    std::ofstream out(csv, std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write " + csv.string());
    out << kHeader << '\n';
    for (const StoredReading& r : rows) {
        out << r.reading.when.ToString() << ',' << ToUtf8(r.model) << ',' << ToUtf8(r.device) << ',' << r.user << ',' << r.reading.systolic << ','
            << r.reading.diastolic << ',' << r.reading.pulse << ',' << (r.reading.movement ? 1 : 0) << ',' << (r.reading.irregular_heartbeat ? 1 : 0) << '\n';
    }
    return rows.size();
}

models::Timestamp LocalNow() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    models::Timestamp t;
    t.year = st.wYear, t.month = st.wMonth, t.day = st.wDay, t.hour = st.wHour, t.minute = st.wMinute, t.second = st.wSecond;
    assert(t.IsValid());
    return t;
}

std::vector<KnownDevice> LoadDevices(const std::filesystem::path& json) {
    assert(!json.empty());
    if (!std::filesystem::exists(json)) return {};
    const std::string text = ReadWholeFile(json);
    JsonReader reader(text);
    std::vector<KnownDevice> devices = reader.ReadDevices();
    assert(devices.size() <= kMaxDevices);
    return devices;
}

void SaveDevices(const std::filesystem::path& json, const std::vector<KnownDevice>& devices) {
    assert(devices.size() <= kMaxDevices);
    assert(!json.empty());
    std::ofstream out(json, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write " + json.string());
    out << "{\n";
    for (size_t i = 0; i < devices.size(); ++i) {
        out << "  \"" << ToUtf8(devices[i].name) << "\": {\n"
            << "    \"model\": \"" << ToUtf8(devices[i].model) << "\",\n"
            << "    \"address\": \"" << ToUtf8(devices[i].address) << "\"\n"
            << "  }" << (i + 1 < devices.size() ? "," : "") << "\n";
    }
    out << "}\n";
}

}  // namespace omron::storage
