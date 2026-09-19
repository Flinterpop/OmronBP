"""Minimal PDF report writer: summary plus a paginated table (Letter, Helvetica, no dependencies).

Mirrors ``win32/src/pdf.cpp`` and ``report.cpp`` so both tools produce the same document.
"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

from omron_bp.storage import CSV_COLUMNS, TIMESTAMP_FORMAT

PAGE_W, PAGE_H = 612.0, 792.0
MARGIN = 54.0
TITLE_SIZE, BODY_SIZE, TABLE_SIZE, SMALL_SIZE = 16.0, 10.0, 9.5, 8.0
ROW_H, LINE_H = 14.0, 13.0
SYS_HIGH, DIA_HIGH = 140, 90
MAX_ROWS = 100_000
MAX_PAGES = 10_000

# Helvetica / Helvetica-Bold advance widths for WinAnsi 32..126, per 1000 em.
_WIDTHS = [278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278, 556, 556, 556, 556, 556, 556, 556, 556,
           556, 556, 278, 278, 584, 584, 584, 556, 1015, 667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667, 556, 833, 722, 778,
           667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 278, 278, 278, 469, 556, 333, 556, 556, 500, 556, 556, 278, 556,
           556, 222, 222, 500, 222, 833, 556, 556, 556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584]
_BOLD_WIDTHS = [278, 333, 474, 556, 556, 889, 722, 238, 333, 333, 389, 584, 278, 333, 278, 278, 556, 556, 556, 556, 556, 556, 556, 556,
                556, 556, 333, 333, 584, 584, 584, 611, 975, 722, 722, 722, 722, 667, 611, 778, 722, 278, 556, 722, 611, 833, 722, 778,
                667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 333, 278, 333, 584, 556, 333, 556, 611, 556, 611, 556, 333, 611,
                611, 278, 278, 556, 278, 889, 611, 611, 611, 611, 389, 556, 333, 611, 556, 778, 556, 556, 500, 389, 280, 389, 584]


@dataclass(frozen=True)
class Column:
    label: str
    x: float
    right_aligned: bool = False


@dataclass
class Report:
    title: str
    subtitle: list[str]
    summary: list[str]
    columns: list[Column]
    rows: list[list[str]]
    footer: str = ""


def text_width(text: str, size: float, bold: bool = False) -> float:
    assert size > 0
    widths = _BOLD_WIDTHS if bold else _WIDTHS
    encoded = text.encode("cp1252", errors="replace")
    return sum(widths[c - 32] if 32 <= c <= 126 else 556 for c in encoded) * size / 1000.0


def _literal(text: str) -> str:
    out = []
    for c in text.encode("cp1252", errors="replace"):
        if c in (0x28, 0x29, 0x5C):  # ( ) backslash
            out.append("\\")
        out.append(chr(c) if c >= 32 else "?")
    return "(" + "".join(out) + ")"


class _Content:
    def __init__(self) -> None:
        self.ops: list[str] = []

    def text(self, text: str, x: float, y: float, size: float, *, bold: bool = False, gray: float = 0.0) -> None:
        self.ops.append(f"{gray:.2f} g BT /{'F2' if bold else 'F1'} {size:.2f} Tf {x:.2f} {y:.2f} Td {_literal(text)} Tj ET")

    def text_right(self, text: str, right: float, y: float, size: float, *, bold: bool = False, gray: float = 0.0) -> None:
        self.text(text, right - text_width(text, size, bold), y, size, bold=bold, gray=gray)

    def line(self, x1: float, y1: float, x2: float, y2: float, *, width: float, gray: float) -> None:
        self.ops.append(f"{gray:.2f} G {width:.2f} w {x1:.2f} {y1:.2f} m {x2:.2f} {y2:.2f} l S")

    def box(self, x: float, y: float, w: float, h: float, gray: float) -> None:
        self.ops.append(f"{gray:.2f} g {x:.2f} {y:.2f} {w:.2f} {h:.2f} re f")

    def stream(self) -> bytes:
        return ("\n".join(self.ops) + "\n").encode("cp1252", errors="replace")


def _draw_header(c: _Content, r: Report) -> float:
    assert r.title and len(r.summary) <= 8
    y = PAGE_H - MARGIN
    c.text(r.title, MARGIN, y - TITLE_SIZE, TITLE_SIZE, bold=True)
    y -= TITLE_SIZE + 10
    for line in r.subtitle:
        c.text(line, MARGIN, y - BODY_SIZE, BODY_SIZE, gray=0.3)
        y -= LINE_H
    if r.summary:
        y -= 6
        box_h = len(r.summary) * LINE_H + 12
        c.box(MARGIN, y - box_h, PAGE_W - 2 * MARGIN, box_h, 0.95)
        ty = y - 8
        for line in r.summary:
            c.text(line, MARGIN + 8, ty - BODY_SIZE, BODY_SIZE)
            ty -= LINE_H
        y -= box_h + 10
    assert y > MARGIN
    return y


def _draw_table_header(c: _Content, r: Report, y: float) -> float:
    for col in r.columns:
        if col.right_aligned:
            c.text_right(col.label, col.x, y - TABLE_SIZE, TABLE_SIZE, bold=True)
        else:
            c.text(col.label, col.x, y - TABLE_SIZE, TABLE_SIZE, bold=True)
    c.line(MARGIN, y - ROW_H + 1, PAGE_W - MARGIN, y - ROW_H + 1, width=0.75, gray=0.6)
    return y - ROW_H - 2


def _draw_footer(c: _Content, r: Report, page: int, pages: int) -> None:
    c.line(MARGIN, MARGIN - 8, PAGE_W - MARGIN, MARGIN - 8, width=0.5, gray=0.75)
    c.text(r.footer, MARGIN, MARGIN - 20, SMALL_SIZE, gray=0.45)
    c.text_right(f"Page {page} of {pages}", PAGE_W - MARGIN, MARGIN - 20, SMALL_SIZE, gray=0.45)


def _rows_that_fit(y: float) -> int:
    available = y - MARGIN - 4
    return 0 if available <= 0 else int(available // ROW_H)


def _page_count(r: Report) -> int:
    assert len(r.rows) <= MAX_ROWS
    probe = _Content()
    fit = _rows_that_fit(_draw_table_header(probe, r, _draw_header(probe, r)))
    remaining, pages = len(r.rows), 1
    for _ in range(MAX_PAGES):
        if remaining <= fit:
            break
        remaining -= fit
        pages += 1
        fit = _rows_that_fit(_draw_table_header(probe, r, PAGE_H - MARGIN))
        assert fit > 0
    return pages


def write_report(path: Path, r: Report) -> int:
    """Write the report; returns the page count."""
    assert 0 < len(r.columns) <= 12
    assert len(r.rows) <= MAX_ROWS
    assert all(len(row) == len(r.columns) for row in r.rows)
    pages = _page_count(r)
    objects: list[bytes] = []  # object 5 onwards; 1-4 are fixed
    page_ids: list[int] = []
    index = 0
    for page in range(1, pages + 1):
        c = _Content()
        y = _draw_header(c, r) if page == 1 else PAGE_H - MARGIN
        y = _draw_table_header(c, r, y)
        fit = _rows_that_fit(y)
        for n in range(fit):
            if index >= len(r.rows):
                break
            if n % 2 == 1:
                c.box(MARGIN, y - ROW_H + 3, PAGE_W - 2 * MARGIN, ROW_H, 0.965)
            for col, cell in zip(r.columns, r.rows[index], strict=True):
                if col.right_aligned:
                    c.text_right(cell, col.x, y - TABLE_SIZE, TABLE_SIZE)
                else:
                    c.text(cell, col.x, y - TABLE_SIZE, TABLE_SIZE)
            y -= ROW_H
            index += 1
        if not r.rows:
            c.text("No readings in this period.", MARGIN, y - TABLE_SIZE, TABLE_SIZE, gray=0.4)
        _draw_footer(c, r, page, pages)
        stream = c.stream()
        objects.append(b"<< /Length " + str(len(stream)).encode() + b" >>\nstream\n" + stream + b"endstream\n")
        stream_id = 4 + len(objects)
        objects.append(
            f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 {PAGE_W:.2f} {PAGE_H:.2f}] "
            f"/Resources << /Font << /F1 3 0 R /F2 4 0 R >> >> /Contents {stream_id} 0 R >>\n".encode()
        )
        page_ids.append(4 + len(objects))
    assert index == len(r.rows)

    kids = " ".join(f"{p} 0 R" for p in page_ids)
    fixed = [
        b"<< /Type /Catalog /Pages 2 0 R >>\n",
        f"<< /Type /Pages /Kids [{kids}] /Count {pages} >>\n".encode(),
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>\n",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>\n",
    ]
    out = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offsets = [0]
    for number, body in enumerate(fixed + objects, start=1):
        offsets.append(len(out))
        out += f"{number} 0 obj\n".encode() + body + b"endobj\n"
    xref = len(out)
    out += f"xref\n0 {len(offsets)}\n0000000000 65535 f \n".encode()
    for offset in offsets[1:]:
        out += f"{offset:010d} 00000 n \n".encode()
    out += f"trailer\n<< /Size {len(offsets)} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n".encode()
    path.write_bytes(bytes(out))
    return pages


# --- readings report ------------------------------------------------------------


@dataclass(frozen=True)
class ReportOptions:
    scope: str
    period: str
    generated: str
    show_monitor_column: bool = False


def _summary(rows: list[dict[str, str]]) -> list[str]:
    assert 0 < len(rows) <= MAX_ROWS
    sys_ = [int(r["systolic"]) for r in rows]
    dia = [int(r["diastolic"]) for r in rows]
    pulse = [int(r["pulse"]) for r in rows]
    high = sum(1 for s, d in zip(sys_, dia, strict=True) if s >= SYS_HIGH or d >= DIA_HIGH)
    irregular = sum(int(r["irregular_heartbeat"]) for r in rows)
    movement = sum(int(r["movement"]) for r in rows)
    n = len(rows)
    assert high <= n
    flags = f"{high} of {n} readings at or above 140/90"
    if irregular:
        flags += f"   -   {irregular} flagged irregular heartbeat"
    if movement:
        flags += f"   -   {movement} flagged movement"
    return [
        f"{n} reading{'s' if n != 1 else ''}, {rows[0]['timestamp'][:10]} to {rows[-1]['timestamp'][:10]}",
        f"Average {sum(sys_) // n}/{sum(dia) // n} mmHg   (systolic {min(sys_)}-{max(sys_)}, diastolic {min(dia)}-{max(dia)})   "
        f"pulse {sum(pulse) // n} bpm ({min(pulse)}-{max(pulse)})",
        flags,
    ]


def build_report(rows: list[dict[str, str]], options: ReportOptions) -> Report:
    """``rows`` are CSV dict rows (CSV_COLUMNS) sorted oldest first."""
    assert all(tuple(r.keys()) == CSV_COLUMNS for r in rows)
    if options.show_monitor_column:
        columns = [Column("Date", 54), Column("Time", 124), Column("Monitor", 168), Column("Systolic", 300, True),
                   Column("Diastolic", 360, True), Column("Pulse", 412, True), Column("Notes", 432)]
    else:
        columns = [Column("Date", 54), Column("Time", 130), Column("Systolic", 250, True), Column("Diastolic", 320, True),
                   Column("Pulse", 380, True), Column("Notes", 404)]
    table: list[list[str]] = []
    for r in rows:
        when = datetime.strptime(r["timestamp"], TIMESTAMP_FORMAT)
        notes = ", ".join(n for n, flag in (("irregular heartbeat", r["irregular_heartbeat"]), ("movement", r["movement"])) if flag == "1")
        cells = [when.strftime("%Y-%m-%d"), when.strftime("%H:%M")]
        if options.show_monitor_column:
            cells.append(r["device"] + (f" (user {r['user']})" if r["user"] != "1" else ""))
        cells += [r["systolic"], r["diastolic"], r["pulse"], notes]
        table.append(cells)
    assert len(table) == len(rows)
    return Report(
        title="Blood pressure readings",
        subtitle=[
            f"Monitor: {options.scope}",
            f"Period: {options.period}      Generated: {options.generated}      Times are as set on the monitor.",
        ],
        summary=_summary(rows) if rows else [],
        columns=columns,
        rows=table,
        footer="Exported from OMRON monitor memory by OmronBP. Reference: 140/90 mmHg.",
    )


__all__ = ["Column", "Report", "ReportOptions", "build_report", "text_width", "write_report"]
