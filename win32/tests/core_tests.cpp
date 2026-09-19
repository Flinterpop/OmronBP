// Hardware-free tests of the protocol framing, record decoding and storage.
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../src/models.h"
#include "../src/pdf.h"
#include "../src/report.h"
#include "../src/protocol.h"
#include "../src/storage.h"
#include "../src/workflow.h"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        ++g_checks;                                                                  \
        if (!(cond)) {                                                               \
            ++g_failures;                                                            \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                            \
    } while (0)

#define CHECK_THROWS(expr)                                   \
    do {                                                     \
        ++g_checks;                                          \
        bool threw = false;                                  \
        try {                                                \
            (void)(expr);                                    \
        } catch (const std::exception&) {                    \
            threw = true;                                    \
        }                                                    \
        if (!threw) {                                        \
            ++g_failures;                                    \
            std::printf("FAIL %s:%d: no throw\n", __FILE__, __LINE__); \
        }                                                    \
    } while (0)

using namespace omron;

protocol::Bytes MakeReply(uint16_t kind, uint16_t address, const uint8_t* data, size_t size) {
    protocol::Bytes b;
    b.data[0] = static_cast<uint8_t>(protocol::kHeaderSize + size + 2);
    b.data[1] = static_cast<uint8_t>(kind >> 8);
    b.data[2] = static_cast<uint8_t>(kind & 0xFF);
    b.data[3] = static_cast<uint8_t>(address >> 8);
    b.data[4] = static_cast<uint8_t>(address & 0xFF);
    b.data[5] = static_cast<uint8_t>(size);
    std::memcpy(b.data.data() + 6, data, size);
    b.data[6 + size] = 0;
    b.data[7 + size] = protocol::XorChecksum(b.data.data(), 7 + size);
    b.size = 8 + size;
    return b;
}

void TestProtocol() {
    CHECK(protocol::XorChecksum(protocol::kStartCommand.data(), 8) == 0);
    CHECK(protocol::XorChecksum(protocol::kEndCommand.data(), 8) == 0);
    // Documented example: 0x26 bytes from 0x0260 -> 08 01 00 02 60 26 00 4d
    const protocol::Bytes cmd = protocol::BuildReadCommand(0x0260, 0x26);
    const uint8_t expected[] = {0x08, 0x01, 0x00, 0x02, 0x60, 0x26, 0x00, 0x4d};
    CHECK(cmd.size == 8 && std::memcmp(cmd.begin(), expected, 8) == 0);

    uint8_t payload[0x38];
    for (size_t i = 0; i < sizeof payload; ++i) payload[i] = static_cast<uint8_t>(i);
    const protocol::Bytes reply = MakeReply(protocol::kRspRead, 0x02AC, payload, sizeof payload);
    CHECK(reply.size == 64);
    protocol::Packet p = protocol::ParsePacket(reply.begin(), reply.size);
    CHECK(p.kind == protocol::kRspRead && p.address == 0x02AC && p.DataSize() == 0x38 && p.Data()[5] == 5);

    protocol::Bytes bad = reply;
    bad.data[63] ^= 1;
    CHECK_THROWS(protocol::ParsePacket(bad.begin(), bad.size));

    protocol::ChannelAssembler asm_;
    CHECK(!asm_.Push(0, reply.begin(), 16).has_value());
    CHECK(!asm_.Push(3, reply.begin() + 48, 16).has_value());
    CHECK(!asm_.Push(1, reply.begin() + 16, 16).has_value());
    auto done = asm_.Push(2, reply.begin() + 32, 16);
    CHECK(done.has_value() && done->address == 0x02AC && done->DataSize() == 0x38);

    // 24-byte reply: channel 0 full, channel 1 carries 8 bytes; out of order.
    uint8_t small[16] = {};
    const protocol::Bytes reply2 = MakeReply(protocol::kRspRead, 0x0010, small, 16);
    CHECK(!asm_.Push(1, reply2.begin() + 16, 8).has_value());
    auto done2 = asm_.Push(0, reply2.begin(), 16);
    CHECK(done2.has_value() && done2->raw.size == 24);

    // End-of-session reply fits one chunk; status byte is data[0].
    uint8_t status = 0;
    const protocol::Bytes end = MakeReply(protocol::kRspEnd, 0, &status, 1);
    auto done3 = asm_.Push(0, end.begin(), end.size);
    CHECK(done3.has_value() && done3->kind == protocol::kRspEnd && done3->Data()[0] == 0);

    uint8_t absurd[16] = {0xFF};
    CHECK_THROWS(asm_.Push(0, absurd, 16));

    // Write command: length, 01c0, address, size, data, 00, crc.
    uint8_t block[16];
    for (size_t i = 0; i < 16; ++i) block[i] = static_cast<uint8_t>(i);
    const protocol::Bytes wcmd = protocol::BuildWriteCommand(0x0080, block, 16);
    CHECK(wcmd.size == 24 && wcmd[0] == 24 && wcmd[1] == 0x01 && wcmd[2] == 0xC0 && wcmd[3] == 0x00 && wcmd[4] == 0x80 && wcmd[5] == 16);
    CHECK(std::memcmp(wcmd.begin() + 6, block, 16) == 0 && wcmd[22] == 0 && protocol::XorChecksum(wcmd.begin(), wcmd.size) == 0);
}

// Inverse of ParseRecord for tests: pack raw field values into a record.
std::vector<uint8_t> Encode(const models::DeviceLayout& layout, const models::RecordBits& values) {
    const size_t bits = layout.record_size * 8;
    std::vector<uint8_t> be(layout.record_size, 0);  // big-endian view
    auto put = [&](models::BitRange r, int value) {
        for (int bit = r.last, v = value; bit >= r.first; --bit, v >>= 1) {
            if (v & 1) be[static_cast<size_t>(bit / 8)] |= static_cast<uint8_t>(0x80 >> (bit % 8));
        }
    };
    put(layout.bits.diastolic, values.diastolic.first);
    put(layout.bits.systolic, values.systolic.first);
    put(layout.bits.pulse, values.pulse.first);
    put(layout.bits.movement, values.movement.first);
    put(layout.bits.irregular_heartbeat, values.irregular_heartbeat.first);
    put(layout.bits.year, values.year.first);
    put(layout.bits.month, values.month.first);
    put(layout.bits.day, values.day.first);
    put(layout.bits.hour, values.hour.first);
    put(layout.bits.minute, values.minute.first);
    put(layout.bits.second, values.second.first);
    (void)bits;
    if (layout.endian == models::Endian::Little) return std::vector<uint8_t>(be.rbegin(), be.rend());
    return be;
}

void TestModels() {
    const uint8_t two[2] = {0x12, 0x34};
    CHECK(models::BitsToInt(two, 2, 0, 7, models::Endian::Big) == 0x12);
    CHECK(models::BitsToInt(two, 2, 0, 7, models::Endian::Little) == 0x34);
    CHECK(models::BitsToInt(two, 2, 4, 11, models::Endian::Big) == 0x23);

    // Field values: (value, unused) pairs reuse BitRange.first as the value.
    const models::RecordBits sample{{84, 0}, {132 - 25, 0}, {67, 0}, {1, 0}, {0, 0}, {26, 0}, {9, 0}, {18, 0}, {7, 0}, {42, 0}, {15, 0}};
    const auto& layouts = models::AllLayouts();
    const size_t count = layouts.size();
    CHECK(count == 2);
    for (size_t i = 0; i < count; ++i) {
        const std::vector<uint8_t> raw = Encode(*layouts[i], sample);
        auto r = models::ParseRecord(*layouts[i], raw.data(), raw.size());
        CHECK(r.has_value());
        if (!r) continue;
        CHECK(r->systolic == 132 && r->diastolic == 84 && r->pulse == 67 && r->movement && !r->irregular_heartbeat);
        CHECK(r->when.ToString() == "2026-09-18 07:42:15");
    }
    const models::DeviceLayout* evolv = models::FindLayout(L"bp7000");
    const models::DeviceLayout* ten = models::FindLayout(L"BP7455CAN");
    CHECK(evolv && std::wstring(evolv->model) == L"HEM-7600T");
    CHECK(ten && std::wstring(ten->model) == L"HEM-7342T");
    CHECK(models::FindLayout(L"hem_7342t") == ten);
    CHECK(models::FindLayout(L"10 Series") == ten);
    CHECK(models::FindLayout(L"HEM-9999T") == nullptr);

    // Byte positions match the documented layouts.
    const std::vector<uint8_t> e = Encode(*evolv, sample);
    CHECK(e[0] == 84 && e[1] == 132 - 25 && e[2] == 26 && e[3] == 67);
    const std::vector<uint8_t> t = Encode(*ten, sample);
    CHECK(t[0] == 132 - 25 && t[1] == 84 && t[2] == 67 && (t[3] & 0x3F) == 26);

    // Real record captured from the 10 Series: 145/94, 54 bpm.
    const uint8_t real[16] = {0x78, 0x5e, 0x36, 0x1a, 0xc6, 0x07, 0x69, 0x13, 0x00, 0x00, 0x36, 0x01, 0x00, 0x00, 0xa6, 0x00};
    auto rr = models::ParseRecord(*ten, real, 16);
    CHECK(rr && rr->systolic == 145 && rr->diastolic == 94 && rr->pulse == 54 && rr->when.year == 2026);

    std::vector<uint8_t> empty(evolv->record_size, 0xFF);
    CHECK(!models::ParseRecord(*evolv, empty.data(), empty.size()).has_value());
    models::RecordBits bad = sample;
    bad.month.first = 13;
    const std::vector<uint8_t> badraw = Encode(*evolv, bad);
    CHECK_THROWS(models::ParseRecord(*evolv, badraw.data(), badraw.size()));

    // Clock records: round trip, checksum, and the field order of each model.
    const models::Timestamp when{2026, 9, 18, 21, 7, 42};
    for (size_t i = 0; i < count; ++i) {
        const models::ClockLayout* clock = layouts[i]->clock;
        CHECK(clock != nullptr);
        if (!clock) continue;
        uint8_t current[32], out[32];
        for (size_t k = 0; k < clock->size; ++k) current[k] = static_cast<uint8_t>(0x10 + k);
        models::EncodeClock(*clock, current, clock->size, when, out);
        CHECK(models::ClockChecksumOk(*clock, out, clock->size));
        CHECK(std::memcmp(out, current, clock->prefix) == 0 && out[clock->pad_offset] == 0);
        const auto back = models::ParseClock(*clock, out, clock->size);
        CHECK(back && back->ToString() == "2026-09-18 21:07:42");
        out[clock->checksum_offset] ^= 1;
        CHECK(!models::ClockChecksumOk(*clock, out, clock->size));
    }
    {
        uint8_t zero[16] = {}, e10[16], t16[16];
        models::EncodeClock(*evolv->clock, zero, 10, when, e10);
        CHECK(e10[2] == 9 && e10[3] == 26 && e10[4] == 21 && e10[5] == 18 && e10[6] == 42 && e10[7] == 7);  // month, year, hour, day, second, minute
        models::EncodeClock(*ten->clock, zero, 16, when, t16);
        CHECK(t16[8] == 26 && t16[9] == 9 && t16[10] == 18 && t16[11] == 21 && t16[12] == 7 && t16[13] == 42 && t16[15] == 0);
        // Records captured live from both monitors verify.
        const uint8_t live_ten[16] = {0xc8, 0xa8, 0, 0, 0, 0, 0, 0, 0x1a, 0x09, 0x12, 0x14, 0x0f, 0x12, 0xda, 0x00};
        const uint8_t live_evolv[10] = {0xa0, 0xc0, 0x08, 0x1a, 0x11, 0x16, 0x3f, 0x25, 0xf2, 0x0d};
        CHECK(models::ClockChecksumOk(*ten->clock, live_ten, 16) && models::ParseClock(*ten->clock, live_ten, 16)->ToString() == "2026-09-18 20:15:18");
        CHECK(models::ClockChecksumOk(*evolv->clock, live_evolv, 10) && models::ParseClock(*evolv->clock, live_evolv, 10)->ToString() == "2026-08-22 17:37:59");
    }

    auto ts = models::Timestamp::Parse("2026-03-01 00:00:00");
    auto ts2 = models::Timestamp::Parse("2026-02-28 00:00:00");
    CHECK(ts && ts2 && ts->ToEpoch() - ts2->ToEpoch() == 86400);
    CHECK(!models::Timestamp::Parse("2026-02-30 00:00:00").has_value());
}

void TestStorage() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / L"omron_bp_tests";
    std::filesystem::create_directories(dir);
    const std::filesystem::path csv = dir / L"r.csv";
    std::filesystem::remove(csv);

    models::Reading a;
    a.when = *models::Timestamp::Parse("2026-09-01 08:00:00");
    a.systolic = 120, a.diastolic = 80, a.pulse = 60;
    models::Reading b = a;
    b.when.day = 2;
    b.irregular_heartbeat = true;
    CHECK(storage::AppendReadings(csv, L"evolv", L"HEM-7600T", {{a, b}}) == 2);
    CHECK(storage::AppendReadings(csv, L"evolv", L"HEM-7600T", {{a, b}}) == 0);
    CHECK(storage::AppendReadings(csv, L"ten", L"HEM-7342T", {{}, {a}}) == 1);
    // Same timestamp, different values (stopped monitor clock) is a distinct reading.
    models::Reading c = a;
    c.systolic = 158;
    CHECK(storage::AppendReadings(csv, L"ten", L"HEM-7342T", {{}, {a, c}}) == 1);
    const auto rows = storage::LoadReadings(csv);
    CHECK(rows.size() == 4);
    CHECK(rows[1].reading.irregular_heartbeat && rows[1].device == L"evolv" && rows[1].user == 1);
    CHECK(rows[2].device == L"ten" && rows[2].user == 2 && rows[2].epoch == a.when.ToEpoch());

    const std::filesystem::path json = dir / L"devices.json";
    std::filesystem::remove(json);
    CHECK(storage::LoadDevices(json).empty());
    storage::SaveDevices(json, {{L"evolv", L"HEM-7600T", L"EC:21:E5:5E:F8:D6"}, {L"ten", L"HEM-7342T", L"00:5F:BF:06:F7:CF"}});
    const auto devices = storage::LoadDevices(json);
    CHECK(devices.size() == 2 && devices[0].name == L"evolv" && devices[1].address == L"00:5F:BF:06:F7:CF");
    CHECK(devices[0].AddressValue() == 0xEC21E55EF8D6ull);
    CHECK(storage::FormatAddress(0xEC21E55EF8D6ull) == L"EC:21:E5:5E:F8:D6");
    CHECK(storage::ParseAddress(L"zz") == 0);

    // The Python tool's own output must load unchanged.
    const std::filesystem::path py = dir / L"py.json";
    {
        FILE* f = nullptr;
        _wfopen_s(&f, py.c_str(), L"wb");
        CHECK(f != nullptr);
        if (f) {
            std::fputs("{\n  \"ten\": {\n    \"model\": \"HEM-7342T\",\n    \"address\": \"00:5F:BF:06:F7:CF\"\n  }\n}\n", f);
            std::fclose(f);
        }
    }
    const auto pydev = storage::LoadDevices(py);
    CHECK(pydev.size() == 1 && pydev[0].model == L"HEM-7342T");
    // Export: time bound and per-device filter, written in the same format.
    const auto all = storage::LoadReadings(csv);
    const auto since = storage::FilterReadings(all, a.when.ToEpoch() + 1, L"");
    CHECK(since.size() == 1 && since[0].reading.when.day == 2);
    const auto only_ten = storage::FilterReadings(all, 0, L"ten");
    CHECK(only_ten.size() == 2 && only_ten[0].device == L"ten" && only_ten[1].reading.systolic == 158);
    const std::filesystem::path exported = dir / L"export.csv";
    CHECK(storage::WriteReadings(exported, only_ten) == 2);
    const auto reread = storage::LoadReadings(exported);
    CHECK(reread.size() == 2 && reread[0].user == 2 && reread[0].reading.systolic == 120);
    CHECK(storage::LocalNow().IsValid());
    std::filesystem::remove_all(dir);
}

void TestPdf() {
    // Helvetica digit width is 556/1000 em.
    CHECK(std::fabs(pdf::TextWidth(L"123", 10, false) - 16.68f) < 0.01f);
    CHECK(pdf::TextWidth(L"ABC", 10, true) > pdf::TextWidth(L"ABC", 10, false));

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / L"omron_bp_tests";
    std::filesystem::create_directories(dir);
    std::vector<storage::StoredReading> rows;
    for (int i = 0; i < 120; ++i) {
        storage::StoredReading r;
        r.reading.when = *models::Timestamp::Parse("2026-05-01 07:00:00");
        r.reading.when.day = 1 + i % 28;
        r.reading.when.month = 5 + i / 28;
        r.reading.systolic = 120 + i % 40, r.reading.diastolic = 75 + i % 20, r.reading.pulse = 50 + i % 15;
        r.reading.irregular_heartbeat = i % 30 == 0;
        r.device = L"ten", r.model = L"HEM-7342T", r.user = 2, r.epoch = r.reading.when.ToEpoch();
        rows.push_back(r);
    }
    report::Options opt{L"ten (OMRON HEM-7342T)", L"last 180 days", L"2026-09-18", false};
    const pdf::Report rep = report::Build(rows, opt);
    CHECK(rep.rows.size() == 120 && rep.columns.size() == 6 && rep.summary.size() == 3);
    CHECK(rep.rows[0][0] == L"2026-05-01" && rep.rows[0][2] == L"120" && rep.rows[0][5] == L"irregular heartbeat");
    const std::filesystem::path out = dir / L"report.pdf";
    const size_t pages = pdf::WriteReport(out, rep);
    CHECK(pages == 3);  // ~38 rows on page 1, ~46 on the others

    // Structural check: header, trailer, and the xref offset points at "xref".
    std::string text;
    {
        std::ifstream in(out, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    CHECK(text.compare(0, 8, "%PDF-1.4") == 0);
    CHECK(text.find("/Count 3") != std::string::npos);
    CHECK(text.find("(Page 1 of 3)") != std::string::npos && text.find("(Page 3 of 3)") != std::string::npos);
    const size_t sx = text.rfind("startxref\n");
    CHECK(sx != std::string::npos);
    if (sx != std::string::npos) {
        const size_t offset = static_cast<size_t>(std::atoll(text.c_str() + sx + 10));
        CHECK(text.compare(offset, 4, "xref") == 0);
    }
    // Parentheses inside PDF string literals must be escaped with a backslash.
    CHECK(text.find("Monitor: ten \\(OMRON HEM-7342T\\)") != std::string::npos);

    // Empty report still produces one valid page.
    const pdf::Report empty = report::Build({}, opt);
    CHECK(pdf::WriteReport(dir / L"empty.pdf", empty) == 1);
    std::filesystem::remove_all(dir);
}

// In-memory EEPROM standing in for a monitor: the clock logic reads and writes through it.
class FakeEeprom : public ble::EepromIo {
public:
    std::vector<uint8_t> memory = std::vector<uint8_t>(0x1000, 0xFF);
    int writes = 0;
    uint16_t last_write_address = 0;

    std::vector<uint8_t> ReadEeprom(uint16_t address, size_t size, size_t) override {
        return std::vector<uint8_t>(memory.begin() + address, memory.begin() + address + static_cast<long>(size));
    }
    void WriteEeprom(uint16_t address, const uint8_t* data, size_t size) override {
        ++writes;
        last_write_address = address;
        std::copy(data, data + size, memory.begin() + address);
    }
};

void TestClockWorkflow() {
    const models::DeviceLayout* ten = models::FindLayout(L"HEM-7342T");
    CHECK(ten && ten->clock);
    if (!ten || !ten->clock) return;
    const models::ClockLayout& clock = *ten->clock;
    const models::Timestamp now{2026, 9, 18, 21, 34, 26};
    auto quiet = [](ble::Level, const std::wstring&) {};

    // Record captured live: 20:15:18, valid checksum -> 79 min slow -> corrected when sync is on.
    const uint8_t live[16] = {0xc8, 0xa8, 0, 0, 0, 0, 0, 0, 0x1a, 0x09, 0x12, 0x14, 0x0f, 0x12, 0xda, 0x00};
    FakeEeprom io;
    std::copy(live, live + 16, io.memory.begin() + clock.read_address);
    workflow::ClockStatus st = workflow::CheckClock(io, *ten, now, true, quiet);
    CHECK(st.known && st.readable && st.verified && st.corrected);
    CHECK(st.drift_seconds == -(79 * 60 + 8));
    CHECK(io.writes == 1 && io.last_write_address == clock.write_address);
    const auto written = models::ParseClock(clock, io.memory.data() + clock.write_address, clock.size);
    CHECK(written && written->ToString() == "2026-09-18 21:34:26");
    CHECK(io.memory[clock.write_address] == 0xc8 && io.memory[clock.write_address + 1] == 0xa8);  // prefix preserved

    // Sync disabled: reported, not written.
    FakeEeprom io2;
    std::copy(live, live + 16, io2.memory.begin() + clock.read_address);
    st = workflow::CheckClock(io2, *ten, now, false, quiet);
    CHECK(st.verified && !st.corrected && io2.writes == 0);

    // Bad checksum: never written even with sync on.
    FakeEeprom io3;
    std::copy(live, live + 16, io3.memory.begin() + clock.read_address);
    io3.memory[clock.read_address + clock.checksum_offset] ^= 0x01;
    st = workflow::CheckClock(io3, *ten, now, true, quiet);
    CHECK(st.readable && !st.verified && !st.corrected && io3.writes == 0);

    // Within tolerance: nothing written.
    FakeEeprom io4;
    uint8_t close[16];
    models::EncodeClock(clock, live, 16, models::Timestamp{2026, 9, 18, 21, 34, 10}, close);
    std::copy(close, close + 16, io4.memory.begin() + clock.read_address);
    st = workflow::CheckClock(io4, *ten, now, true, quiet);
    CHECK(st.verified && !st.corrected && st.drift_seconds == -16 && io4.writes == 0);

    // Unreadable date: reported as such, no write.
    FakeEeprom io5;
    std::copy(live, live + 16, io5.memory.begin() + clock.read_address);
    io5.memory[clock.read_address + clock.month] = 13;
    st = workflow::CheckClock(io5, *ten, now, true, quiet);
    CHECK(st.known && !st.readable && !st.corrected && io5.writes == 0);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestProtocol();
    std::puts("protocol ok");
    TestModels();
    std::puts("models ok");
    TestStorage();
    std::puts("storage ok");
    TestPdf();
    std::puts("pdf ok");
    TestClockWorkflow();
    std::puts("clock workflow ok");
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
