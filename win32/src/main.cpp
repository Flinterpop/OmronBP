// OMRON blood-pressure reader: main window.
//
// Layout:  [Read all] [Pair...] [Open CSV] [Forget] [Export...]      [30d] [90d] [12m] [All]
//          monitors list | summary line
//                        | chart (Direct2D)
//                        | readings table
//          status line                       [clock sync] [Bluetooth details]
//          activity log
#include <windows.h>

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>
#include <winrt/base.h>

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "app.h"
#include "chart.h"
#include "export_dialog.h"
#include "pdf.h"
#include "report.h"
#include "resource.h"
#include "models.h"
#include "pair_dialog.h"
#include "storage.h"
#include "workflow.h"

namespace omron::app {

HFONT CreateUiFont(HWND hwnd, int point_size, bool bold) {
    const int height = -MulDiv(point_size, static_cast<int>(GetDpiForWindow(hwnd)), 72);
    return CreateFontW(height, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void LogQueue::Push(ble::Level level, const std::wstring& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ == kCapacity) {
        // Drop the oldest debug line rather than grow without bound.
        head_ = (head_ + 1) % kCapacity;
        --count_;
        ++dropped_;
    }
    ring_[(head_ + count_) % kCapacity] = {level, text};
    ++count_;
}

size_t LogQueue::TakeDropped() {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t dropped = dropped_;
    dropped_ = 0;
    return dropped;
}

bool LogQueue::Pop(Entry& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    assert(count_ <= kCapacity && head_ < kCapacity);
    if (count_ == 0) return false;
    out = ring_[head_];
    head_ = (head_ + 1) % kCapacity;
    --count_;
    return true;
}

}  // namespace omron::app

namespace {

using namespace omron;

enum Id : int {
    kIdReadAll = 100, kIdPair, kIdOpenCsv, kIdRemove, kIdExport, kIdDevices, kIdReadings, kIdChart, kIdSummary,
    kIdRange30, kIdRange90, kIdRange365, kIdRangeAll, kIdStatus, kIdLog, kIdVerbose, kIdSyncClock,
};

constexpr wchar_t kClassName[] = L"OmronBPMain";
constexpr size_t kMaxListRows = 5000;
constexpr int kLogLimitChars = 200000;

std::wstring FormatDate(const models::Timestamp& t) {
    wchar_t buf[16];
    swprintf_s(buf, L"%04d-%02d-%02d", t.year, t.month, t.day);
    return buf;
}

std::wstring FormatTime(const models::Timestamp& t) {
    wchar_t buf[8];
    swprintf_s(buf, L"%02d:%02d", t.hour, t.minute);
    return buf;
}

std::wstring ClockNote(const workflow::ClockStatus& c) {
    if (!c.known || !c.readable) return L"";
    const long minutes = c.drift_seconds / 60;
    if (c.corrected) return L", clock was " + std::to_wstring(std::labs(minutes)) + (c.drift_seconds < 0 ? L" min slow, now set" : L" min fast, now set");
    if (std::labs(c.drift_seconds) <= workflow::kClockToleranceSeconds) return L", clock OK";
    if (!c.verified) return L", clock " + std::to_wstring(std::labs(minutes)) + L" min off (layout unverified, not changed)";
    return L", clock " + std::to_wstring(std::labs(minutes)) + L" min off (sync disabled)";
}

class MainWindow {
public:
    static void Register(HINSTANCE instance);
    HWND Create(HINSTANCE instance);

private:
    static LRESULT CALLBACK StaticProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Proc(UINT msg, WPARAM wp, LPARAM lp);

    void OnCreate();
    void CreateColumns();
    void CreateFonts();
    void Layout();
    void OnCommand(int id);
    void OnNotify(const NMHDR* hdr);
    void LoadData();
    void RefreshDeviceList();
    void RefreshSelected();
    void RefreshReadingsTable(const std::vector<storage::StoredReading>& rows);
    void SetRange(int id);
    void SetBusy(bool busy);
    void StartWorker(std::function<bool()> job);
    void ReadMonitors(std::vector<storage::KnownDevice> targets);
    void PairNew();
    void RemoveSelected();
    void ExportReadings();
    void DrainLog();
    void AppendLog(const std::wstring& line);
    void OnDone(bool ok);
    std::vector<uint64_t> OtherAddresses(uint64_t keep) const;

    HWND hwnd_ = nullptr;
    HWND read_all_ = nullptr, pair_ = nullptr, open_csv_ = nullptr, remove_ = nullptr, export_ = nullptr;
    HWND devices_ = nullptr, readings_ = nullptr, chart_ = nullptr, summary_ = nullptr;
    HWND range_[4] = {};
    HWND status_ = nullptr, log_ = nullptr, verbose_ = nullptr, sync_clock_ = nullptr;
    HFONT font_ = nullptr, bold_ = nullptr;

    workflow::Paths paths_;
    std::vector<storage::KnownDevice> devices_list_;
    std::vector<storage::StoredReading> rows_;
    int selected_ = -1;
    int range_id_ = kIdRangeAll;

    app::LogQueue queue_;
    std::thread worker_;
    bool busy_ = false;
};

void MainWindow::Register(HINSTANCE instance) {
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = StaticProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kClassName;
    wc.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    assert(wc.hIcon != nullptr && wc.hIconSm != nullptr);
    const ATOM atom = RegisterClassExW(&wc);
    assert(atom != 0);
    (void)atom;
}

HWND MainWindow::Create(HINSTANCE instance) {
    const std::wstring title = std::wstring(app::kAppTitle) + L"  v" + storage::FromUtf8(APP_VERSION_STRING);
    hwnd_ = CreateWindowExW(0, kClassName, title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 10, 10, nullptr, nullptr, instance, this);
    if (hwnd_) SetWindowPos(hwnd_, nullptr, 0, 0, app::Scale(hwnd_, 1180), app::Scale(hwnd_, 780), SWP_NOMOVE | SWP_NOZORDER);
    return hwnd_;
}

LRESULT CALLBACK MainWindow::StaticProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->Proc(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT MainWindow::Proc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: OnCreate(); return 0;
        case WM_SIZE: Layout(); return 0;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize = {app::Scale(hwnd_, 1000), app::Scale(hwnd_, 560)};
            return 0;
        }
        case WM_DPICHANGED: {
            const RECT* rc = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd_, nullptr, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
            CreateFonts();
            Layout();
            return 0;
        }
        case WM_COMMAND: OnCommand(LOWORD(wp)); return 0;
        case WM_NOTIFY: OnNotify(reinterpret_cast<const NMHDR*>(lp)); return 0;
        case WM_CTLCOLORSTATIC:
            SetBkColor(reinterpret_cast<HDC>(wp), GetSysColor(COLOR_WINDOW));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        case app::WM_APP_LOG: DrainLog(); return 0;
        case app::WM_APP_DONE: OnDone(wp != 0); return 0;
        case WM_CLOSE:
            if (busy_) {
                MessageBoxW(hwnd_, L"A monitor is being read - please wait for it to finish.", app::kAppTitle, MB_ICONINFORMATION);
                return 0;
            }
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            if (worker_.joinable()) worker_.join();
            if (font_) DeleteObject(font_);
            if (bold_) DeleteObject(bold_);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void MainWindow::CreateFonts() {
    if (font_) DeleteObject(font_);
    if (bold_) DeleteObject(bold_);
    font_ = app::CreateUiFont(hwnd_, 10);
    bold_ = app::CreateUiFont(hwnd_, 10, true);
    for (HWND h : {read_all_, pair_, open_csv_, remove_, export_, devices_, readings_, summary_, status_, log_, verbose_, sync_clock_, range_[0], range_[1], range_[2], range_[3]}) {
        if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }
    if (read_all_) SendMessageW(read_all_, WM_SETFONT, reinterpret_cast<WPARAM>(bold_), TRUE);
    if (summary_) SendMessageW(summary_, WM_SETFONT, reinterpret_cast<WPARAM>(bold_), TRUE);
}

void MainWindow::OnCreate() {
    HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd_, GWLP_HINSTANCE));
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
        HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
        if (h == nullptr) throw std::runtime_error("control creation failed");
        return h;
    };
    read_all_ = make(L"BUTTON", L"Read all monitors", WS_TABSTOP | BS_PUSHBUTTON, kIdReadAll);
    pair_ = make(L"BUTTON", L"Pair new monitor...", WS_TABSTOP | BS_PUSHBUTTON, kIdPair);
    open_csv_ = make(L"BUTTON", L"Open readings.csv", WS_TABSTOP | BS_PUSHBUTTON, kIdOpenCsv);
    remove_ = make(L"BUTTON", L"Forget monitor", WS_TABSTOP | BS_PUSHBUTTON, kIdRemove);
    export_ = make(L"BUTTON", L"Export...", WS_TABSTOP | BS_PUSHBUTTON, kIdExport);
    devices_ = make(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER, kIdDevices, WS_EX_CLIENTEDGE);
    readings_ = make(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_NOSORTHEADER, kIdReadings, WS_EX_CLIENTEDGE);
    summary_ = make(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, kIdSummary);
    const wchar_t* labels[4] = {L"30 days", L"90 days", L"12 months", L"All"};
    for (int i = 0; i < 4; ++i) {
        range_[i] = make(L"BUTTON", labels[i], WS_TABSTOP | BS_AUTORADIOBUTTON | BS_PUSHLIKE | (i == 0 ? WS_GROUP : 0), kIdRange30 + i);
    }
    Button_SetCheck(range_[3], BST_CHECKED);
    chart_ = chart::Create(hwnd_, kIdChart);
    status_ = make(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, kIdStatus);
    log_ = make(L"EDIT", L"", WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, kIdLog, WS_EX_CLIENTEDGE);
    verbose_ = make(L"BUTTON", L"Show Bluetooth details", WS_TABSTOP | BS_AUTOCHECKBOX, kIdVerbose);
    sync_clock_ = make(L"BUTTON", L"Keep monitor clocks on PC time", WS_TABSTOP | BS_AUTOCHECKBOX, kIdSyncClock);
    Button_SetCheck(sync_clock_, BST_CHECKED);
    ListView_SetExtendedListViewStyle(devices_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    ListView_SetExtendedListViewStyle(readings_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    CreateColumns();

    wchar_t exe[MAX_PATH];
    const DWORD exe_len = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (exe_len == 0 || exe_len >= MAX_PATH) throw std::runtime_error("cannot locate the executable");
    const std::filesystem::path dir = std::filesystem::path(exe).parent_path();
    assert(!dir.empty());
    paths_.csv = dir / L"readings.csv";
    paths_.devices = dir / L"devices.json";

    CreateFonts();
    LoadData();
    SetWindowTextW(status_, devices_list_.empty() ? L"Welcome. Click 'Pair new monitor...' to add your first OMRON monitor."
                                                  : L"Press the Bluetooth button on a monitor so its symbol shows, then click 'Read all monitors'.");
}

void MainWindow::CreateColumns() {
    assert(devices_ != nullptr && readings_ != nullptr);
    auto column = [&](HWND list, int index, const wchar_t* title, int width, int fmt = LVCFMT_LEFT) {
        LVCOLUMNW col{LVCF_TEXT | LVCF_WIDTH | LVCF_FMT};
        col.pszText = const_cast<wchar_t*>(title);
        col.cx = app::Scale(hwnd_, width);
        col.fmt = fmt;
        const int inserted = ListView_InsertColumn(list, index, &col);
        assert(inserted == index);
        (void)inserted;
    };
    column(devices_, 0, L"Monitor", 84);
    column(devices_, 1, L"Model", 84);
    column(devices_, 2, L"Readings", 66, LVCFMT_RIGHT);
    column(devices_, 3, L"Latest", 140);
    column(readings_, 0, L"Date", 84);
    column(readings_, 1, L"Time", 50);
    column(readings_, 2, L"User", 42, LVCFMT_RIGHT);
    column(readings_, 3, L"Systolic", 60, LVCFMT_RIGHT);
    column(readings_, 4, L"Diastolic", 62, LVCFMT_RIGHT);
    column(readings_, 5, L"Pulse", 48, LVCFMT_RIGHT);
    column(readings_, 6, L"Notes", 170);
    assert(Header_GetItemCount(ListView_GetHeader(devices_)) == 4 && Header_GetItemCount(ListView_GetHeader(readings_)) == 7);
}

void MainWindow::Layout() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    auto s = [&](int dip) { return app::Scale(hwnd_, dip); };
    const int pad = s(10), gap = s(6), bar_h = s(30), status_h = s(20), log_h = s(88), left_w = s(390), range_h = s(22);
    int y = pad;
    int x = pad;
    assert(rc.right > 0 && rc.bottom > 0);
    HDWP dwp = BeginDeferWindowPos(16);
    assert(dwp != nullptr);
    auto place = [&](HWND h, int px, int py, int w, int hgt) {
        assert(h != nullptr && w >= 0 && hgt >= 0);
        if (dwp != nullptr) dwp = DeferWindowPos(dwp, h, nullptr, px, py, w, hgt, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
    };
    place(read_all_, x, y, s(140), bar_h);
    x += s(140) + gap;
    place(pair_, x, y, s(140), bar_h);
    x += s(140) + gap;
    place(open_csv_, x, y, s(130), bar_h);
    x += s(130) + gap;
    place(remove_, x, y, s(110), bar_h);
    x += s(110) + gap;
    place(export_, x, y, s(90), bar_h);
    for (int i = 0; i < 4; ++i) place(range_[i], rc.right - pad - (4 - i) * s(72) + gap / 2, y + s(3), s(72) - gap / 2, bar_h - s(6));
    y += bar_h + pad;

    const int bottom = rc.bottom - pad - log_h - gap - status_h - gap;
    place(devices_, pad, y, left_w, bottom - y);

    const int rx = pad + left_w + pad, rw = rc.right - pad - rx;
    place(summary_, rx, y, rw, range_h);
    const int ry = y + range_h + gap;
    const int chart_h = (bottom - ry) * 58 / 100;
    place(chart_, rx, ry, rw, chart_h);
    place(readings_, rx, ry + chart_h + gap, rw, bottom - (ry + chart_h + gap));

    place(status_, pad, bottom + gap, rc.right - 2 * pad - s(400), status_h);
    place(sync_clock_, rc.right - pad - s(390), bottom + gap, s(210), status_h);
    place(verbose_, rc.right - pad - s(170), bottom + gap, s(170), status_h);
    place(log_, pad, bottom + gap + status_h + gap, rc.right - 2 * pad, log_h);
    if (dwp != nullptr) EndDeferWindowPos(dwp);
    ListView_SetColumnWidth(devices_, 3, LVSCW_AUTOSIZE_USEHEADER);
    ListView_SetColumnWidth(readings_, 6, LVSCW_AUTOSIZE_USEHEADER);
    // Children that shrank leave stale pixels behind; repaint everything once per layout.
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

std::vector<uint64_t> MainWindow::OtherAddresses(uint64_t keep) const {
    std::vector<uint64_t> others;
    for (const storage::KnownDevice& d : devices_list_) {
        if (d.AddressValue() != keep) others.push_back(d.AddressValue());
    }
    return others;
}

void MainWindow::LoadData() {
    try {
        devices_list_ = storage::LoadDevices(paths_.devices);
        rows_ = storage::LoadReadings(paths_.csv);
    } catch (const std::exception& exc) {
        MessageBoxW(hwnd_, storage::FromUtf8(exc.what()).c_str(), app::kAppTitle, MB_ICONERROR);
    }
    std::sort(rows_.begin(), rows_.end(), [](const storage::StoredReading& a, const storage::StoredReading& b) { return a.epoch < b.epoch; });
    RefreshDeviceList();
}

void MainWindow::RefreshDeviceList() {
    assert(devices_list_.size() <= storage::kMaxDevices);
    const int previous = selected_;
    ListView_DeleteAllItems(devices_);
    for (size_t i = 0; i < devices_list_.size(); ++i) {
        const storage::KnownDevice& d = devices_list_[i];
        size_t count = 0;
        const storage::StoredReading* latest = nullptr;
        for (const storage::StoredReading& r : rows_) {
            if (r.device == d.name) {
                ++count;
                latest = &r;  // rows_ is sorted by time
            }
        }
        LVITEMW item{LVIF_TEXT};
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<wchar_t*>(d.name.c_str());
        ListView_InsertItem(devices_, &item);
        const models::DeviceLayout* layout = models::FindLayout(d.model);
        ListView_SetItemText(devices_, item.iItem, 1, const_cast<wchar_t*>(layout ? layout->model : d.model.c_str()));
        ListView_SetItemText(devices_, item.iItem, 2, const_cast<wchar_t*>(std::to_wstring(count).c_str()));
        std::wstring latest_text = latest ? FormatDate(latest->reading.when) + L"  " + std::to_wstring(latest->reading.systolic) + L"/" + std::to_wstring(latest->reading.diastolic)
                                          : L"never read";
        ListView_SetItemText(devices_, item.iItem, 3, const_cast<wchar_t*>(latest_text.c_str()));
    }
    selected_ = devices_list_.empty() ? -1 : std::clamp(previous, 0, static_cast<int>(devices_list_.size()) - 1);
    assert(selected_ < static_cast<int>(devices_list_.size()));
    if (selected_ >= 0) ListView_SetItemState(devices_, selected_, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    RefreshSelected();
}

void MainWindow::RefreshSelected() {
    assert(selected_ >= -1 && selected_ < static_cast<int>(devices_list_.size()));
    std::vector<storage::StoredReading> rows;
    std::wstring summary;
    if (selected_ >= 0 && selected_ < static_cast<int>(devices_list_.size())) {
        const storage::KnownDevice& d = devices_list_[static_cast<size_t>(selected_)];
        for (const storage::StoredReading& r : rows_) {
            if (r.device == d.name) rows.push_back(r);
        }
        if (rows.empty()) {
            summary = d.name + L": no readings yet";
        } else {
            long sys = 0, dia = 0;
            int high = 0;
            for (const storage::StoredReading& r : rows) {
                sys += r.reading.systolic, dia += r.reading.diastolic;
                if (r.reading.systolic >= 140 || r.reading.diastolic >= 90) ++high;
            }
            const storage::StoredReading& last = rows.back();
            summary = d.name + L":  " + std::to_wstring(rows.size()) + L" readings   latest " + std::to_wstring(last.reading.systolic) + L"/" +
                      std::to_wstring(last.reading.diastolic) + L" mmHg, " + std::to_wstring(last.reading.pulse) + L" bpm on " + FormatDate(last.reading.when) +
                      L"   average " + std::to_wstring(sys / static_cast<long>(rows.size())) + L"/" + std::to_wstring(dia / static_cast<long>(rows.size())) +
                      L"   " + std::to_wstring(high) + L" at or above 140/90";
        }
    } else {
        summary = devices_list_.empty() ? L"No monitors paired yet." : L"Select a monitor.";
    }
    assert(!summary.empty());
    SetWindowTextW(summary_, summary.c_str());
    EnableWindow(remove_, selected_ >= 0 && !busy_);
    chart::SetData(chart_, rows);
    RefreshReadingsTable(rows);
}

void MainWindow::RefreshReadingsTable(const std::vector<storage::StoredReading>& rows) {
    assert(readings_ != nullptr && rows.size() <= storage::kMaxRows);
    SendMessageW(readings_, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(readings_);
    const size_t shown = std::min(rows.size(), kMaxListRows);
    for (size_t i = 0; i < shown; ++i) {
        const storage::StoredReading& r = rows[rows.size() - 1 - i];  // newest first
        LVITEMW item{LVIF_TEXT};
        item.iItem = static_cast<int>(i);
        const std::wstring date = FormatDate(r.reading.when), time = FormatTime(r.reading.when);
        item.pszText = const_cast<wchar_t*>(date.c_str());
        ListView_InsertItem(readings_, &item);
        auto set = [&](int col, const std::wstring& text) { ListView_SetItemText(readings_, item.iItem, col, const_cast<wchar_t*>(text.c_str())); };
        set(1, time);
        set(2, std::to_wstring(r.user));
        set(3, std::to_wstring(r.reading.systolic));
        set(4, std::to_wstring(r.reading.diastolic));
        set(5, std::to_wstring(r.reading.pulse));
        std::wstring notes;
        if (r.reading.irregular_heartbeat) notes = L"irregular heartbeat";
        if (r.reading.movement) notes += notes.empty() ? L"movement" : L", movement";
        set(6, notes);
    }
    SendMessageW(readings_, WM_SETREDRAW, TRUE, 0);
}

void MainWindow::SetRange(int id) {
    range_id_ = id;
    const int days = id == kIdRange30 ? 30 : id == kIdRange90 ? 90 : id == kIdRange365 ? 365 : 0;
    chart::SetRangeDays(chart_, days);
}

void MainWindow::SetBusy(bool busy) {
    busy_ = busy;
    for (HWND h : {read_all_, pair_, remove_, export_}) EnableWindow(h, !busy);
    EnableWindow(remove_, !busy && selected_ >= 0);
}

void MainWindow::AppendLog(const std::wstring& line) {
    if (GetWindowTextLengthW(log_) > kLogLimitChars) SetWindowTextW(log_, L"");
    const int end = GetWindowTextLengthW(log_);
    Edit_SetSel(log_, end, end);
    Edit_ReplaceSel(log_, (line + L"\r\n").c_str());
}

void MainWindow::DrainLog() {
    assert(log_ != nullptr && status_ != nullptr);
    app::LogQueue::Entry entry;
    const bool verbose = Button_GetCheck(verbose_) == BST_CHECKED;
    const size_t dropped = queue_.TakeDropped();
    if (dropped > 0) AppendLog(L"    (" + std::to_wstring(dropped) + L" detail lines dropped)");
    for (int i = 0; i < 512 && queue_.Pop(entry); ++i) {
        assert(!entry.text.empty() || entry.level == ble::Level::Debug);
        if (entry.level == ble::Level::Info) {
            SetWindowTextW(status_, entry.text.c_str());
            AppendLog(entry.text);
        } else if (verbose) {
            AppendLog(L"    " + entry.text);
        }
    }
}

void MainWindow::StartWorker(std::function<bool()> job) {
    assert(!busy_);
    assert(job);
    if (worker_.joinable()) worker_.join();
    SetBusy(true);
    SetWindowTextW(log_, L"");
    HWND hwnd = hwnd_;
    worker_ = std::thread([hwnd, job = std::move(job)] {
        ble::InitWorkerThread();
        bool ok = false;
        try {
            ok = job();
        } catch (...) {
            ok = false;
        }
        PostMessageW(hwnd, app::WM_APP_DONE, ok ? 1 : 0, 0);
    });
}

void MainWindow::ReadMonitors(std::vector<storage::KnownDevice> targets) {
    assert(!targets.empty());
    const workflow::Paths paths = paths_;
    const bool sync_clock = Button_GetCheck(sync_clock_) == BST_CHECKED;
    const std::vector<storage::KnownDevice> all = devices_list_;
    assert(targets.size() <= all.size());
    auto log = [this](ble::Level level, const std::wstring& text) {
        queue_.Push(level, text);
        PostMessageW(hwnd_, app::WM_APP_LOG, 0, 0);
    };
    StartWorker([targets, all, paths, sync_clock, log] {
        std::wstring summary;
        bool any = false;
        for (const storage::KnownDevice& device : targets) {
            std::vector<uint64_t> others;
            for (const storage::KnownDevice& d : all) {
                if (d.AddressValue() != device.AddressValue()) others.push_back(d.AddressValue());
            }
            try {
                const workflow::DownloadResult result = workflow::Download(device, others, paths, sync_clock, log);
                size_t total = 0;
                for (const auto& user : result.per_user) total += user.size();
                summary += device.name + L": " + std::to_wstring(result.appended) + L" new of " + std::to_wstring(total) + L" stored" + ClockNote(result.clock) + L".  ";
                any = true;
            } catch (const ble::NotFound&) {
                summary += device.name + L": not heard - press its Bluetooth button and try again.  ";
                log(ble::Level::Info, device.name + L" is not advertising");
            } catch (const std::exception& exc) {
                summary += device.name + L": failed (" + storage::FromUtf8(exc.what()) + L").  ";
                log(ble::Level::Info, device.name + L": " + storage::FromUtf8(exc.what()));
            } catch (const winrt::hresult_error& exc) {
                summary += device.name + L": Bluetooth error (" + std::wstring(exc.message()) + L").  ";
            }
        }
        log(ble::Level::Info, summary);
        return any;
    });
}

void MainWindow::PairNew() {
    std::vector<std::wstring> taken;
    for (const storage::KnownDevice& d : devices_list_) taken.push_back(d.name);
    const std::optional<pair_dialog::Result> choice = pair_dialog::Show(hwnd_, taken);
    if (!choice) return;
    const models::DeviceLayout* layout = models::FindLayout(choice->model);
    assert(layout != nullptr);
    if (layout == nullptr) return;
    // Re-pairing a monitor that is already registered just updates its entry.
    std::vector<storage::KnownDevice> updated = devices_list_;
    updated.erase(std::remove_if(updated.begin(), updated.end(), [&](const storage::KnownDevice& d) { return d.AddressValue() == choice->address; }), updated.end());
    if (updated.size() >= storage::kMaxDevices) {
        MessageBoxW(hwnd_, L"Too many monitors registered.", app::kAppTitle, MB_ICONWARNING);
        return;
    }
    updated.push_back({choice->name, layout->model, storage::FormatAddress(choice->address)});
    const std::vector<uint64_t> others = OtherAddresses(choice->address);
    assert(others.size() < storage::kMaxDevices);
    const workflow::Paths paths = paths_;
    const pair_dialog::Result result = *choice;
    auto log = [this](ble::Level level, const std::wstring& text) {
        queue_.Push(level, text);
        PostMessageW(hwnd_, app::WM_APP_LOG, 0, 0);
    };
    StartWorker([result, layout, others, updated, paths, log] {
        try {
            workflow::PairMonitor(result.address, *layout, others, log);
            storage::SaveDevices(paths.devices, updated);
            log(ble::Level::Info, L"Paired '" + result.name + L"'. Press its Bluetooth button (symbol only) and click 'Read all monitors'.");
            return true;
        } catch (const std::exception& exc) {
            log(ble::Level::Info, L"Pairing failed: " + storage::FromUtf8(exc.what()));
        } catch (const winrt::hresult_error& exc) {
            log(ble::Level::Info, L"Pairing failed: " + std::wstring(exc.message()));
        }
        return false;
    });
}

void MainWindow::RemoveSelected() {
    assert(!busy_);
    if (selected_ < 0 || selected_ >= static_cast<int>(devices_list_.size())) return;
    const storage::KnownDevice device = devices_list_[static_cast<size_t>(selected_)];
    const std::wstring text = L"Forget '" + device.name + L"'?\n\nIts readings stay in readings.csv; only the pairing entry is removed. To read it again you will need to pair it again.";
    if (MessageBoxW(hwnd_, text.c_str(), app::kAppTitle, MB_ICONQUESTION | MB_OKCANCEL) != IDOK) return;
    std::vector<storage::KnownDevice> updated = devices_list_;
    updated.erase(updated.begin() + selected_);
    assert(updated.size() + 1 == devices_list_.size());
    try {
        storage::SaveDevices(paths_.devices, updated);
    } catch (const std::exception& exc) {
        MessageBoxW(hwnd_, storage::FromUtf8(exc.what()).c_str(), app::kAppTitle, MB_ICONERROR);
        return;
    }
    LoadData();
}

void MainWindow::ExportReadings() {
    if (rows_.empty()) {
        MessageBoxW(hwnd_, L"No readings have been downloaded yet.", app::kAppTitle, MB_ICONINFORMATION);
        return;
    }
    const std::wstring selected = selected_ >= 0 && selected_ < static_cast<int>(devices_list_.size()) ? devices_list_[static_cast<size_t>(selected_)].name : L"";
    const models::Timestamp now = storage::LocalNow();
    auto filtered = [&](const export_dialog::Choice& c) {
        const int64_t since = c.days == 0 ? 0 : now.ToEpoch() - static_cast<int64_t>(c.days) * 86400;
        return storage::FilterReadings(rows_, since, c.all_monitors ? L"" : selected);
    };
    const std::optional<export_dialog::Choice> choice = export_dialog::Show(hwnd_, selected, [&](const export_dialog::Choice& c) { return filtered(c).size(); });
    if (!choice) return;
    const std::vector<storage::StoredReading> rows = filtered(*choice);
    assert(!rows.empty());
    assert(choice->days >= 0);

    wchar_t date[16];
    swprintf_s(date, L"%04d-%02d-%02d", now.year, now.month, now.day);
    std::wstring name = L"readings_" + (choice->all_monitors ? std::wstring(L"all") : selected) +
                        (choice->days ? L"_last" + std::to_wstring(choice->days) + L"days" : L"_everything") + L"_" + date + L".csv";
    wchar_t file[MAX_PATH] = {};
    wcscpy_s(file, name.c_str());
    const std::wstring dir = paths_.csv.parent_path().wstring();
    OPENFILENAMEW ofn{sizeof ofn};
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"CSV (comma separated)\0*.csv\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = dir.c_str();
    ofn.lpstrDefExt = L"csv";
    ofn.lpstrTitle = L"Export readings";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return;
    try {
        const size_t written = storage::WriteReadings(file, rows);
        std::wstring message = L"Exported " + std::to_wstring(written) + (written == 1 ? L" reading to " : L" readings to ") + file;
        if (choice->pdf) {
            report::Options options;
            const storage::KnownDevice* dev = selected_ >= 0 ? &devices_list_[static_cast<size_t>(selected_)] : nullptr;
            options.scope = choice->all_monitors ? L"all monitors" : selected + (dev ? L" (OMRON " + dev->model + L")" : L"");
            options.period = choice->days ? L"last " + std::to_wstring(choice->days) + L" days" : L"all stored readings";
            options.generated = date;
            options.show_monitor_column = choice->all_monitors;
            const std::filesystem::path pdf_path = std::filesystem::path(file).replace_extension(L".pdf");
            const size_t pages = pdf::WriteReport(pdf_path, report::Build(rows, options));
            message += L"  and a " + std::to_wstring(pages) + (pages == 1 ? L"-page PDF " : L"-page PDF ") + pdf_path.filename().wstring();
        }
        SetWindowTextW(status_, message.c_str());
        AppendLog(message);
    } catch (const std::exception& exc) {
        MessageBoxW(hwnd_, storage::FromUtf8(exc.what()).c_str(), app::kAppTitle, MB_ICONERROR);
    }
}

void MainWindow::OnCommand(int id) {
    assert(id >= kIdReadAll && id <= kIdSyncClock);
    assert(hwnd_ != nullptr);
    switch (id) {
        case kIdReadAll:
            if (devices_list_.empty()) {
                MessageBoxW(hwnd_, L"No monitors are paired yet. Click 'Pair new monitor...' first.", app::kAppTitle, MB_ICONINFORMATION);
                return;
            }
            ReadMonitors(devices_list_);
            return;
        case kIdPair: PairNew(); return;
        case kIdRemove: RemoveSelected(); return;
        case kIdExport: ExportReadings(); return;
        case kIdOpenCsv:
            if (!std::filesystem::exists(paths_.csv)) {
                MessageBoxW(hwnd_, L"No readings have been downloaded yet.", app::kAppTitle, MB_ICONINFORMATION);
                return;
            }
            ShellExecuteW(hwnd_, L"open", paths_.csv.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return;
        case kIdRange30: case kIdRange90: case kIdRange365: case kIdRangeAll: SetRange(id); return;
        case kIdVerbose: return;
    }
}

void MainWindow::OnNotify(const NMHDR* hdr) {
    if (hdr->idFrom == kIdDevices && hdr->code == LVN_ITEMCHANGED) {
        const auto* change = reinterpret_cast<const NMLISTVIEW*>(hdr);
        if ((change->uNewState & LVIS_SELECTED) && !(change->uOldState & LVIS_SELECTED)) {
            selected_ = change->iItem;
            RefreshSelected();
        }
    }
}

void MainWindow::OnDone(bool ok) {
    DrainLog();
    if (worker_.joinable()) worker_.join();
    SetBusy(false);
    LoadData();
    if (!ok) MessageBeep(MB_ICONWARNING);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    assert(instance != nullptr);
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    MainWindow::Register(instance);
    chart::RegisterClass(instance);
    MainWindow window;
    HWND hwnd = nullptr;
    try {
        hwnd = window.Create(instance);
    } catch (const std::exception& exc) {
        MessageBoxW(nullptr, storage::FromUtf8(exc.what()).c_str(), app::kAppTitle, MB_ICONERROR);
        return 1;
    }
    if (!hwnd) return 1;
    assert(IsWindow(hwnd));
    ShowWindow(hwnd, show);
    MSG msg;
    // The message pump runs until WM_QUIT; GetMessage returns -1 on error, which also ends it.
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return static_cast<int>(msg.wParam);
}
