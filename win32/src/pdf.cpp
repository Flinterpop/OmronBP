#include "pdf.h"

#include <windows.h>

#include <cassert>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace omron::pdf {

namespace {

constexpr float kPageW = 612, kPageH = 792;  // US Letter
constexpr float kMargin = 54;
constexpr float kTitleSize = 16, kBodySize = 10, kTableSize = 9.5f, kSmallSize = 8;
constexpr float kRowH = 14;
constexpr float kLineH = 13;

// Helvetica / Helvetica-Bold advance widths for WinAnsi 32..126, per 1000 em (Adobe AFM).
constexpr int kWidths[95] = {278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278, 556, 556, 556, 556, 556, 556, 556, 556,
                             556, 556, 278, 278, 584, 584, 584, 556, 1015, 667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667, 556, 833, 722, 778,
                             667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 278, 278, 278, 469, 556, 333, 556, 556, 500, 556, 556, 278, 556,
                             556, 222, 222, 500, 222, 833, 556, 556, 556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584};
constexpr int kBoldWidths[95] = {278, 333, 474, 556, 556, 889, 722, 238, 333, 333, 389, 584, 278, 333, 278, 278, 556, 556, 556, 556, 556, 556, 556, 556,
                                 556, 556, 333, 333, 584, 584, 584, 611, 975, 722, 722, 722, 722, 667, 611, 778, 722, 278, 556, 722, 611, 833, 722, 778,
                                 667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 333, 278, 333, 584, 556, 333, 556, 611, 556, 611, 556, 333, 611,
                                 611, 278, 278, 556, 278, 889, 611, 611, 611, 611, 389, 556, 333, 611, 556, 778, 556, 556, 500, 389, 280, 389, 584};

std::string ToWinAnsi(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(1252, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, "?", nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(1252, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, "?", nullptr);
    return out;
}

// PDF string literal: escape the delimiters.
std::string Literal(const std::wstring& text) {
    std::string out = "(";
    for (unsigned char c : ToWinAnsi(text)) {
        if (c == '(' || c == ')' || c == '\\') out.push_back('\\');
        if (c < 32) c = '?';
        out.push_back(static_cast<char>(c));
    }
    out.push_back(')');
    return out;
}

std::string Num(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", v);
    return buf;
}

class Content {
public:
    void Text(const std::wstring& text, float x, float y, float size, bool bold, float gray = 0.0f) {
        stream_ += Num(gray) + " g BT /" + (bold ? "F2" : "F1") + " " + Num(size) + " Tf " + Num(x) + " " + Num(y) + " Td " + Literal(text) + " Tj ET\n";
    }
    void TextRight(const std::wstring& text, float right, float y, float size, bool bold, float gray = 0.0f) {
        Text(text, right - TextWidth(text, size, bold), y, size, bold, gray);
    }
    void Line(float x1, float y1, float x2, float y2, float width, float gray) {
        stream_ += Num(gray) + " G " + Num(width) + " w " + Num(x1) + " " + Num(y1) + " m " + Num(x2) + " " + Num(y2) + " l S\n";
    }
    void Box(float x, float y, float w, float h, float fill_gray) {
        stream_ += Num(fill_gray) + " g " + Num(x) + " " + Num(y) + " " + Num(w) + " " + Num(h) + " re f\n";
    }
    const std::string& Stream() const { return stream_; }

private:
    std::string stream_;
};

class Writer {
public:
    explicit Writer(const std::filesystem::path& path) : out_(path, std::ios::binary | std::ios::trunc) {
        if (!out_) throw std::runtime_error("cannot write " + path.string());
        Emit("%PDF-1.4\n%\xE2\xE3\xCF\xD3\n");
    }

    // Objects 1-4 are reserved: catalog, pages, regular font, bold font.
    size_t AddPage(const Content& content) {
        const size_t stream_obj = NextObject();
        Emit("<< /Length " + std::to_string(content.Stream().size()) + " >>\nstream\n" + content.Stream() + "endstream\n");
        EndObject();
        const size_t page_obj = NextObject();
        Emit("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + Num(kPageW) + " " + Num(kPageH) +
             "] /Resources << /Font << /F1 3 0 R /F2 4 0 R >> >> /Contents " + std::to_string(stream_obj) + " 0 R >>\n");
        EndObject();
        pages_.push_back(page_obj);
        return pages_.size();
    }

    void Finish() {
        // Fixed objects are written last; their numbers were reserved up front.
        offsets_[1] = Tell();
        Emit("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
        offsets_[2] = Tell();
        std::string kids;
        for (size_t p : pages_) kids += std::to_string(p) + " 0 R ";
        Emit("2 0 obj\n<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pages_.size()) + " >>\nendobj\n");
        offsets_[3] = Tell();
        Emit("3 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>\nendobj\n");
        offsets_[4] = Tell();
        Emit("4 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>\nendobj\n");

        const size_t xref = Tell();
        Emit("xref\n0 " + std::to_string(offsets_.size()) + "\n0000000000 65535 f \n");
        for (size_t i = 1; i < offsets_.size(); ++i) {
            char line[24];
            std::snprintf(line, sizeof line, "%010zu 00000 n \n", offsets_[i]);
            Emit(line);
        }
        Emit("trailer\n<< /Size " + std::to_string(offsets_.size()) + " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n");
        out_.flush();
        if (!out_) throw std::runtime_error("PDF write failed");
    }

private:
    size_t NextObject() {
        const size_t number = offsets_.size();
        offsets_.push_back(Tell());
        Emit(std::to_string(number) + " 0 obj\n");
        return number;
    }
    void EndObject() { Emit("endobj\n"); }
    void Emit(const std::string& text) {
        out_.write(text.data(), static_cast<std::streamsize>(text.size()));
        written_ += text.size();
    }
    size_t Tell() const { return written_; }

    std::ofstream out_;
    size_t written_ = 0;
    std::vector<size_t> offsets_{0, 0, 0, 0, 0};  // index = object number; 0 unused
    std::vector<size_t> pages_;
};

// Draws title/subtitle/summary; returns the y where the table may start.
float DrawHeader(Content& c, const Report& r) {
    float y = kPageH - kMargin;
    c.Text(r.title, kMargin, y - kTitleSize, kTitleSize, true);
    y -= kTitleSize + 10;
    for (const std::wstring& line : r.subtitle) {
        c.Text(line, kMargin, y - kBodySize, kBodySize, false, 0.3f);
        y -= kLineH;
    }
    if (!r.summary.empty()) {
        y -= 6;
        const float box_h = static_cast<float>(r.summary.size()) * kLineH + 12;
        c.Box(kMargin, y - box_h, kPageW - 2 * kMargin, box_h, 0.95f);
        float ty = y - 8;
        for (const std::wstring& line : r.summary) {
            c.Text(line, kMargin + 8, ty - kBodySize, kBodySize, false);
            ty -= kLineH;
        }
        y -= box_h + 10;
    }
    return y;
}

float DrawTableHeader(Content& c, const Report& r, float y) {
    for (const Column& col : r.columns) {
        if (col.right_aligned) c.TextRight(col.label, col.x, y - kTableSize, kTableSize, true);
        else c.Text(col.label, col.x, y - kTableSize, kTableSize, true);
    }
    c.Line(kMargin, y - kRowH + 1, kPageW - kMargin, y - kRowH + 1, 0.75f, 0.6f);
    return y - kRowH - 2;
}

void DrawFooter(Content& c, const Report& r, size_t page, size_t pages) {
    c.Line(kMargin, kMargin - 8, kPageW - kMargin, kMargin - 8, 0.5f, 0.75f);
    c.Text(r.footer, kMargin, kMargin - 20, kSmallSize, false, 0.45f);
    c.TextRight(L"Page " + std::to_wstring(page) + L" of " + std::to_wstring(pages), kPageW - kMargin, kMargin - 20, kSmallSize, false, 0.45f);
}

size_t RowsThatFit(float y) {
    const float available = y - kMargin - 4;
    return available <= 0 ? 0 : static_cast<size_t>(available / kRowH);
}

}  // namespace

float TextWidth(const std::wstring& text, float size, bool bold) {
    assert(size > 0);
    const int* widths = bold ? kBoldWidths : kWidths;
    long total = 0;
    for (unsigned char ch : ToWinAnsi(text)) {
        total += (ch >= 32 && ch <= 126) ? widths[ch - 32] : 556;
    }
    return static_cast<float>(total) * size / 1000.0f;
}

size_t WriteReport(const std::filesystem::path& path, const Report& r) {
    assert(!r.columns.empty() && r.columns.size() <= kMaxColumns);
    assert(r.rows.size() <= kMaxRows);
    for (const auto& row : r.rows) {
        if (row.size() != r.columns.size()) throw std::runtime_error("report row has the wrong number of cells");
    }

    // Pass 1: count pages so each footer can say "of N".
    size_t pages = 1;
    {
        Content probe;
        float y = DrawTableHeader(probe, r, DrawHeader(probe, r));
        size_t remaining = r.rows.size();
        size_t fit = RowsThatFit(y);
        for (size_t guard = 0; remaining > fit && guard < kMaxRows; ++guard) {
            remaining -= fit;
            ++pages;
            fit = RowsThatFit(DrawTableHeader(probe, r, kPageH - kMargin));
            assert(fit > 0);
        }
    }

    Writer writer(path);
    size_t index = 0;
    for (size_t page = 1; page <= pages; ++page) {
        Content c;
        float y = page == 1 ? DrawHeader(c, r) : kPageH - kMargin;
        y = DrawTableHeader(c, r, y);
        const size_t fit = RowsThatFit(y);
        for (size_t n = 0; n < fit && index < r.rows.size(); ++n, ++index) {
            if (n % 2 == 1) c.Box(kMargin, y - kRowH + 3, kPageW - 2 * kMargin, kRowH, 0.965f);
            for (size_t col = 0; col < r.columns.size(); ++col) {
                const Column& column = r.columns[col];
                if (column.right_aligned) c.TextRight(r.rows[index][col], column.x, y - kTableSize, kTableSize, false);
                else c.Text(r.rows[index][col], column.x, y - kTableSize, kTableSize, false);
            }
            y -= kRowH;
        }
        if (r.rows.empty()) c.Text(L"No readings in this period.", kMargin, y - kTableSize, kTableSize, false, 0.4f);
        DrawFooter(c, r, page, pages);
        writer.AddPage(c);
    }
    writer.Finish();
    assert(index == r.rows.size());
    return pages;
}

}  // namespace omron::pdf
