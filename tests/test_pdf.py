from pathlib import Path

from omron_bp.pdf import Column, Report, ReportOptions, build_report, text_width, write_report


def row(day: int, sys_: int = 130, ihb: str = "0") -> dict[str, str]:
    return {
        "timestamp": f"2026-05-{day:02d} 07:00:00", "model": "HEM-7342T", "device": "ten", "user": "2",
        "systolic": str(sys_), "diastolic": "85", "pulse": "55", "movement": "0", "irregular_heartbeat": ihb,
    }


def test_text_width_matches_helvetica_metrics() -> None:
    assert abs(text_width("123", 10) - 16.68) < 0.01
    assert text_width("ABC", 10, bold=True) > text_width("ABC", 10)


def test_report_paginates_and_is_well_formed(tmp_path: Path) -> None:
    rows = [row(1 + i % 28, 120 + i % 40, "1" if i % 30 == 0 else "0") for i in range(120)]
    report = build_report(rows, ReportOptions("ten (OMRON HEM-7342T)", "last 180 days", "2026-09-18"))
    assert len(report.rows) == 120 and len(report.columns) == 6 and len(report.summary) == 3
    assert report.rows[0][:3] == ["2026-05-01", "07:00", "120"] and report.rows[0][5] == "irregular heartbeat"
    out = tmp_path / "r.pdf"
    assert write_report(out, report) == 3
    data = out.read_bytes()
    assert data.startswith(b"%PDF-1.4")
    assert b"/Count 3" in data and b"(Page 3 of 3)" in data
    assert rb"Monitor: ten \(OMRON HEM-7342T\)" in data
    xref = int(data[data.rfind(b"startxref") + 10 :].split()[0])
    assert data[xref : xref + 4] == b"xref"


def test_all_monitors_adds_column_and_empty_report_is_valid(tmp_path: Path) -> None:
    report = build_report([row(3)], ReportOptions("all monitors", "everything", "2026-09-18", show_monitor_column=True))
    assert [c.label for c in report.columns][2] == "Monitor" and report.rows[0][2] == "ten (user 2)"
    empty = Report("t", [], [], [Column("A", 54)], [], "f")
    assert write_report(tmp_path / "e.pdf", empty) == 1
