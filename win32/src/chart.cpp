#include "chart.h"

#include <d2d1.h>
#include <dwrite.h>
#include <windowsx.h>
#include <winrt/base.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <string>

namespace omron::chart {

namespace {

constexpr int kGapDays = 30;  // no line across a gap longer than this
constexpr int kSysHigh = 140, kDiaHigh = 90;
constexpr size_t kMaxDotsDrawn = 150;
constexpr float kMarginL = 44, kMarginR = 56, kMarginT = 26, kMarginB = 26;
constexpr float kPanelGap = 10;
constexpr int64_t kDay = 86400;

// Validated light palette (see the Python chart / dataviz notes).
constexpr D2D1_COLOR_F Rgb(uint32_t hex) {
    return D2D1_COLOR_F{((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f, 1.0f};
}
constexpr D2D1_COLOR_F kSurface = Rgb(0xfcfcfb), kInk = Rgb(0x0b0b0b), kInk2 = Rgb(0x52514e), kMuted = Rgb(0x898781);
constexpr D2D1_COLOR_F kGrid = Rgb(0xe1e0d9), kAxis = Rgb(0xc3c2b7);
constexpr D2D1_COLOR_F kSys = Rgb(0x2a78d6), kDia = Rgb(0xeb6834), kPulse = Rgb(0x1baf7a);

struct Civil {
    int year, month, day;
};

Civil CivilFromEpoch(int64_t epoch) {
    // Inverse of Timestamp::ToEpoch (Howard Hinnant's civil_from_days).
    const int64_t z = (epoch >= 0 ? epoch : epoch - 86399) / 86400 + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t y = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    assert(m >= 1 && m <= 12 && d >= 1 && d <= 31);
    assert(y > 1900 && y < 2200);
    return {static_cast<int>(y + (m <= 2 ? 1 : 0)), static_cast<int>(m), static_cast<int>(d)};
}

int64_t EpochFromCivil(int y, int m, int d) {
    y -= m <= 2 ? 1 : 0;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (static_cast<int64_t>(era) * 146097 + doe - 719468) * 86400;
}

const wchar_t* kMonths[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};

std::wstring FormatDateTime(int64_t epoch) {
    const Civil c = CivilFromEpoch(epoch);
    const int64_t secs = ((epoch % 86400) + 86400) % 86400;
    wchar_t buf[48];
    swprintf_s(buf, L"%s %d, %d  %02lld:%02lld", kMonths[c.month - 1], c.day, c.year, secs / 3600, (secs / 60) % 60);
    return buf;
}

float NiceStep(float span, int target) {
    const float raw = span / static_cast<float>(target);
    const float pow10 = std::pow(10.0f, std::floor(std::log10(raw)));
    for (float m : {1.0f, 2.0f, 5.0f, 10.0f}) {
        if (m * pow10 >= raw) return m * pow10;
    }
    return 10.0f * pow10;
}

struct Series {
    const wchar_t* label;
    D2D1_COLOR_F color;
    int Point::*value;
};

struct Panel {
    D2D1_RECT_F plot;  // inner plotting rectangle
    float ymin, ymax;
    int64_t t0, t1;
    float X(int64_t t) const { return plot.left + static_cast<float>(t - t0) / static_cast<float>(std::max<int64_t>(1, t1 - t0)) * (plot.right - plot.left); }
    float Y(int v) const { return plot.top + (ymax - static_cast<float>(v)) / (ymax - ymin) * (plot.bottom - plot.top); }
};

class ChartWindow {
public:
    explicit ChartWindow(HWND hwnd) : hwnd_(hwnd) {}

    void SetData(const std::vector<storage::StoredReading>& rows);
    void SetRangeDays(int days);
    void OnPaint();
    void OnSize();
    void OnMouseMove(int x, int y);
    void OnMouseLeave();
    void DiscardTarget() { target_ = nullptr; }

private:
    bool EnsureTarget();
    void ComputeVisible();
    void DrawEmpty();
    void DrawPanel(const Panel& p, const wchar_t* title, const Series* series, size_t count, bool refs);
    void DrawXAxis(const Panel& p);
    void DrawLegend(const Panel& p, const Series* series, size_t count);
    void DrawHover(const Panel& bp, const Panel& pulse);
    void Text(const std::wstring& text, float x, float y, IDWriteTextFormat* fmt, const D2D1_COLOR_F& color, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING, float width = 200);

    HWND hwnd_;
    winrt::com_ptr<ID2D1Factory> factory_;
    winrt::com_ptr<IDWriteFactory> dwrite_;
    winrt::com_ptr<ID2D1HwndRenderTarget> target_;
    winrt::com_ptr<ID2D1SolidColorBrush> brush_;
    winrt::com_ptr<IDWriteTextFormat> small_, title_, bold_;
    std::vector<Point> all_;
    std::vector<Point> visible_;
    int range_days_ = 0;
    int hover_ = -1;
    bool tracking_ = false;
    Panel bp_{}, pulse_{};
};

void ChartWindow::SetData(const std::vector<storage::StoredReading>& rows) {
    all_.clear();
    all_.reserve(rows.size());
    for (const storage::StoredReading& r : rows) {
        all_.push_back({r.epoch, r.reading.systolic, r.reading.diastolic, r.reading.pulse, r.reading.movement, r.reading.irregular_heartbeat});
    }
    std::sort(all_.begin(), all_.end(), [](const Point& a, const Point& b) { return a.epoch < b.epoch; });
    ComputeVisible();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ChartWindow::SetRangeDays(int days) {
    assert(days >= 0);
    range_days_ = days;
    ComputeVisible();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ChartWindow::ComputeVisible() {
    visible_.clear();
    hover_ = -1;
    if (all_.empty()) return;
    // "Last N days" is relative to the newest reading, so a monitor not read for a while still shows something.
    const int64_t cutoff = range_days_ == 0 ? INT64_MIN : all_.back().epoch - static_cast<int64_t>(range_days_) * kDay;
    for (const Point& p : all_) {
        if (p.epoch >= cutoff) visible_.push_back(p);
    }
}

bool ChartWindow::EnsureTarget() {
    assert(hwnd_ != nullptr);
    if (!factory_) {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.put()))) return false;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite_.put())))) return false;
        auto make = [&](float size, DWRITE_FONT_WEIGHT weight) {
            winrt::com_ptr<IDWriteTextFormat> fmt;
            dwrite_->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", fmt.put());
            if (fmt) fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            return fmt;
        };
        small_ = make(11.0f, DWRITE_FONT_WEIGHT_NORMAL);
        title_ = make(12.0f, DWRITE_FONT_WEIGHT_NORMAL);
        bold_ = make(11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    }
    if (target_) return true;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const UINT dpi = GetDpiForWindow(hwnd_);
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
    props.dpiX = props.dpiY = static_cast<float>(dpi);
    const D2D1_HWND_RENDER_TARGET_PROPERTIES hprops = D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top));
    if (FAILED(factory_->CreateHwndRenderTarget(props, hprops, target_.put()))) return false;
    assert(dpi >= 96);
    target_->CreateSolidColorBrush(kInk, brush_.put());
    return brush_ != nullptr;
}

void ChartWindow::Text(const std::wstring& text, float x, float y, IDWriteTextFormat* fmt, const D2D1_COLOR_F& color, DWRITE_TEXT_ALIGNMENT align, float width) {
    fmt->SetTextAlignment(align);
    brush_->SetColor(color);
    const float left = align == DWRITE_TEXT_ALIGNMENT_TRAILING ? x - width : align == DWRITE_TEXT_ALIGNMENT_CENTER ? x - width / 2 : x;
    target_->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), fmt, D2D1::RectF(left, y - 8, left + width, y + 8), brush_.get());
}

void ChartWindow::DrawEmpty() {
    const D2D1_SIZE_F size = target_->GetSize();
    Text(L"No readings yet - press a monitor's Bluetooth button and click Read all monitors.", size.width / 2, size.height / 2, title_.get(), kMuted, DWRITE_TEXT_ALIGNMENT_CENTER, size.width);
}

void ChartWindow::DrawLegend(const Panel& p, const Series* series, size_t count) {
    float x = p.plot.right - 10;
    const float y = p.plot.top - 12;
    for (size_t i = count; i-- > 0;) {
        x -= 62;
        brush_->SetColor(series[i].color);
        target_->DrawLine(D2D1::Point2F(x, y), D2D1::Point2F(x + 14, y), brush_.get(), 3.0f);
        Text(series[i].label, x + 18, y, small_.get(), kInk2, DWRITE_TEXT_ALIGNMENT_LEADING, 60);
    }
    Text(L"lines at 140 / 90", x - 100, y, small_.get(), kMuted, DWRITE_TEXT_ALIGNMENT_LEADING, 96);
}

void ChartWindow::DrawXAxis(const Panel& p) {
    assert(p.t1 > p.t0 && p.plot.right > p.plot.left);
    brush_->SetColor(kAxis);
    target_->DrawLine(D2D1::Point2F(p.plot.left, p.plot.bottom), D2D1::Point2F(p.plot.right, p.plot.bottom), brush_.get(), 1.0f);
    const int64_t days = (p.t1 - p.t0) / kDay;
    assert(days >= 0);
    const Civil start = CivilFromEpoch(p.t0);
    int drawn = 0;
    if (days > 800) {
        for (int y = start.year; y <= start.year + 100 && drawn < 40; ++y) {
            const int64_t t = EpochFromCivil(y, 1, 1);
            if (t > p.t1) break;
            if (t >= p.t0) { Text(std::to_wstring(y), p.X(t), p.plot.bottom + 12, small_.get(), kMuted, DWRITE_TEXT_ALIGNMENT_CENTER, 60); ++drawn; }
        }
    } else if (days > 120) {
        const int step = days > 400 ? 3 : 1;
        for (int i = 0; i < 80 && drawn < 40; i += step) {
            const int m0 = start.month - 1 + i;
            const int y = start.year + m0 / 12, m = m0 % 12 + 1;
            const int64_t t = EpochFromCivil(y, m, 1);
            if (t > p.t1) break;
            if (t >= p.t0) {
                std::wstring label = kMonths[m - 1];
                if (m == 1 || drawn == 0) label += L" " + std::to_wstring(y % 100);
                Text(label, p.X(t), p.plot.bottom + 12, small_.get(), kMuted, DWRITE_TEXT_ALIGNMENT_CENTER, 60);
                ++drawn;
            }
        }
    } else {
        const int step = days > 40 ? 7 : days > 12 ? 2 : 1;
        const int64_t first = EpochFromCivil(start.year, start.month, start.day);
        for (int i = 0; i < 120 && drawn < 40; i += step) {
            const int64_t t = first + static_cast<int64_t>(i) * kDay;
            if (t > p.t1) break;
            if (t >= p.t0) {
                const Civil c = CivilFromEpoch(t);
                Text(std::wstring(kMonths[c.month - 1]) + L" " + std::to_wstring(c.day), p.X(t), p.plot.bottom + 12, small_.get(), kMuted, DWRITE_TEXT_ALIGNMENT_CENTER, 60);
                ++drawn;
            }
        }
    }
}

void ChartWindow::DrawPanel(const Panel& p, const wchar_t* title, const Series* series, size_t count, bool refs) {
    assert(series != nullptr && count > 0 && count <= 2 && p.ymax > p.ymin);
    assert(!visible_.empty());
    Text(title, p.plot.left, p.plot.top - 12, title_.get(), kInk2);
    const int target = std::clamp(static_cast<int>((p.plot.bottom - p.plot.top) / 36.0f), 2, 6);
    const float step = NiceStep(p.ymax - p.ymin, target);
    for (float v = std::ceil(p.ymin / step) * step; v <= p.ymax + 0.01f; v += step) {
        const float y = p.Y(static_cast<int>(v));
        brush_->SetColor(kGrid);
        target_->DrawLine(D2D1::Point2F(p.plot.left, y), D2D1::Point2F(p.plot.right, y), brush_.get(), 1.0f);
        Text(std::to_wstring(static_cast<int>(v)), p.plot.left - 6, y, small_.get(), kMuted, DWRITE_TEXT_ALIGNMENT_TRAILING, 40);
    }
    if (refs) {
        for (int v : {kSysHigh, kDiaHigh}) {
            if (v < p.ymin || v > p.ymax) continue;
            brush_->SetColor(kAxis);
            target_->DrawLine(D2D1::Point2F(p.plot.left, p.Y(v)), D2D1::Point2F(p.plot.right, p.Y(v)), brush_.get(), 1.0f);
            Text(std::to_wstring(v), p.plot.right + 34, p.Y(v), small_.get(), kMuted);
        }
    }
    DrawXAxis(p);
    for (size_t s = 0; s < count; ++s) {
        brush_->SetColor(series[s].color);
        for (size_t i = 1; i < visible_.size(); ++i) {
            if (visible_[i].epoch - visible_[i - 1].epoch > kGapDays * kDay) continue;
            target_->DrawLine(D2D1::Point2F(p.X(visible_[i - 1].epoch), p.Y(visible_[i - 1].*series[s].value)),
                              D2D1::Point2F(p.X(visible_[i].epoch), p.Y(visible_[i].*series[s].value)), brush_.get(), 2.0f);
        }
        if (visible_.size() <= kMaxDotsDrawn) {
            for (const Point& pt : visible_) {
                const D2D1_ELLIPSE dot = D2D1::Ellipse(D2D1::Point2F(p.X(pt.epoch), p.Y(pt.*series[s].value)), 3.5f, 3.5f);
                brush_->SetColor(kSurface);
                target_->FillEllipse(D2D1::Ellipse(dot.point, 5.0f, 5.0f), brush_.get());
                brush_->SetColor(series[s].color);
                target_->FillEllipse(dot, brush_.get());
            }
        }
        const Point& last = visible_.back();
        Text(std::to_wstring(last.*series[s].value), p.X(last.epoch) + 8, p.Y(last.*series[s].value), bold_.get(), kInk);
    }
}

void ChartWindow::DrawHover(const Panel& bp, const Panel& pulse) {
    assert(bp.t0 == pulse.t0 && bp.t1 == pulse.t1);
    if (hover_ < 0 || hover_ >= static_cast<int>(visible_.size())) return;
    const Point& pt = visible_[static_cast<size_t>(hover_)];
    const float x = bp.X(pt.epoch);
    assert(x >= bp.plot.left - 1 && x <= bp.plot.right + 1);
    brush_->SetColor(kInk2);
    target_->DrawLine(D2D1::Point2F(x, bp.plot.top), D2D1::Point2F(x, pulse.plot.bottom), brush_.get(), 1.0f);
    const D2D1_COLOR_F colors[3] = {kSys, kDia, kPulse};
    const D2D1_POINT_2F centers[3] = {D2D1::Point2F(x, bp.Y(pt.systolic)), D2D1::Point2F(x, bp.Y(pt.diastolic)), D2D1::Point2F(x, pulse.Y(pt.pulse))};
    for (int i = 0; i < 3; ++i) {
        brush_->SetColor(kSurface);
        target_->FillEllipse(D2D1::Ellipse(centers[i], 7.0f, 7.0f), brush_.get());
        brush_->SetColor(colors[i]);
        target_->FillEllipse(D2D1::Ellipse(centers[i], 5.5f, 5.5f), brush_.get());
    }
    std::wstring lines[3] = {FormatDateTime(pt.epoch), L"Systolic " + std::to_wstring(pt.systolic) + L"   Diastolic " + std::to_wstring(pt.diastolic),
                             L"Pulse " + std::to_wstring(pt.pulse) + (pt.irregular ? L"   irregular heartbeat" : L"") + (pt.movement ? L"   movement" : L"")};
    const float w = 190, h = 54;
    const float left = std::min(x + 14, bp.plot.right - w), top = bp.plot.top + 4;
    brush_->SetColor(kSurface);
    target_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left, top, left + w, top + h), 4, 4), brush_.get());
    brush_->SetColor(kAxis);
    target_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left, top, left + w, top + h), 4, 4), brush_.get(), 1.0f);
    Text(lines[0], left + 8, top + 11, bold_.get(), kInk, DWRITE_TEXT_ALIGNMENT_LEADING, w);
    Text(lines[1], left + 8, top + 27, small_.get(), kInk, DWRITE_TEXT_ALIGNMENT_LEADING, w);
    Text(lines[2], left + 8, top + 43, small_.get(), kInk, DWRITE_TEXT_ALIGNMENT_LEADING, w);
}

void ChartWindow::OnPaint() {
    PAINTSTRUCT ps;
    const HDC dc = BeginPaint(hwnd_, &ps);
    assert(dc != nullptr);
    (void)dc;
    if (EnsureTarget()) {
        target_->BeginDraw();
        target_->Clear(kSurface);
        if (visible_.empty()) {
            DrawEmpty();
        } else {
            const D2D1_SIZE_F size = target_->GetSize();
            int lo = 999, hi = 0, plo = 999, phi = 0;
            for (const Point& p : visible_) {
                lo = std::min(lo, p.diastolic), hi = std::max(hi, p.systolic);
                plo = std::min(plo, p.pulse), phi = std::max(phi, p.pulse);
            }
            const int64_t span = visible_.back().epoch - visible_.front().epoch;
            const int64_t pad = std::max<int64_t>(kDay, span / 40);
            const float bp_h = (size.height - kPanelGap) * 0.6f;
            bp_ = {D2D1::RectF(kMarginL, kMarginT, size.width - kMarginR, bp_h - kMarginB), static_cast<float>((lo - 10) / 10 * 10),
                   static_cast<float>((hi + 19) / 10 * 10), visible_.front().epoch - pad, visible_.back().epoch + pad};
            pulse_ = {D2D1::RectF(kMarginL, bp_h + kPanelGap + kMarginT, size.width - kMarginR, size.height - kMarginB),
                      static_cast<float>((plo - 5) / 10 * 10), static_cast<float>(std::max((phi + 14) / 10 * 10, (plo - 5) / 10 * 10 + 20)), bp_.t0, bp_.t1};
            const Series bp_series[2] = {{L"Systolic", kSys, &Point::systolic}, {L"Diastolic", kDia, &Point::diastolic}};
            const Series pulse_series[1] = {{L"Pulse", kPulse, &Point::pulse}};
            DrawPanel(bp_, L"Blood pressure (mmHg)", bp_series, 2, true);
            DrawLegend(bp_, bp_series, 2);
            DrawPanel(pulse_, L"Pulse (bpm)", pulse_series, 1, false);
            DrawHover(bp_, pulse_);
        }
        if (target_->EndDraw() == D2DERR_RECREATE_TARGET) target_ = nullptr;
    }
    const BOOL ended = EndPaint(hwnd_, &ps);
    assert(ended);
    (void)ended;
}

void ChartWindow::OnSize() {
    if (!target_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    target_->Resize(D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top));
    // Shrinking exposes nothing, so Windows would not repaint on its own and the old, larger drawing would remain.
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ChartWindow::OnMouseMove(int x, int y) {
    if (!tracking_) {
        TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, hwnd_, 0};
        TrackMouseEvent(&tme);
        tracking_ = true;
    }
    (void)y;
    if (visible_.empty() || !target_) return;
    const float dpi = static_cast<float>(GetDpiForWindow(hwnd_));
    assert(dpi > 0 && visible_.size() <= all_.size());
    const float px = static_cast<float>(x) * 96.0f / dpi;
    int best = -1;
    float best_d = 20.0f;
    for (size_t i = 0; i < visible_.size(); ++i) {
        const float d = std::fabs(bp_.X(visible_[i].epoch) - px);
        if (d < best_d) best_d = d, best = static_cast<int>(i);
    }
    assert(best == -1 || best < static_cast<int>(visible_.size()));
    if (best != hover_) {
        hover_ = best;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void ChartWindow::OnMouseLeave() {
    tracking_ = false;
    if (hover_ != -1) {
        hover_ = -1;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

ChartWindow* Self(HWND hwnd) {
    return reinterpret_cast<ChartWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

LRESULT CALLBACK ChartProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCCREATE:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new ChartWindow(hwnd)));
            return TRUE;
        case WM_NCDESTROY:
            delete Self(hwnd);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        case WM_PAINT: Self(hwnd)->OnPaint(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_SIZE: Self(hwnd)->OnSize(); return 0;
        case WM_DPICHANGED_AFTERPARENT: Self(hwnd)->DiscardTarget(); InvalidateRect(hwnd, nullptr, FALSE); return 0;
        case WM_MOUSEMOVE: Self(hwnd)->OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
        case WM_MOUSELEAVE: Self(hwnd)->OnMouseLeave(); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

void RegisterClass(HINSTANCE instance) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = ChartProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassW(&wc);
}

HWND Create(HWND parent, int id) {
    return CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 10, 10, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE)), nullptr);
}

void SetData(HWND chart, const std::vector<storage::StoredReading>& rows) {
    assert(chart != nullptr);
    Self(chart)->SetData(rows);
}

void SetRangeDays(HWND chart, int days) {
    assert(chart != nullptr);
    Self(chart)->SetRangeDays(days);
}

}  // namespace omron::chart
