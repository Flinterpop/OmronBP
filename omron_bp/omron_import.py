"""Reading the CSV that the OMRON connect phone app exports (Report (<dates>).csv).

The export has one row per measurement::

    Date,Time,Systolic (mmHg),Diastolic (mmHg),Pulse (bpm),Notes
    "Dec 25, 2025",6:10 AM,150,88,48,

It carries no monitor, user or seconds, so the importer is told which registered monitor and
user slot the readings belong to, and stamps them at second 0.  Columns are found by header
name, so extra or reordered columns (newer app versions add irregular-heartbeat and
body-movement columns) are tolerated.
"""

from __future__ import annotations

import csv
import io
from datetime import datetime
from pathlib import Path

from omron_bp.models import MAX_PRESSURE_MMHG, MAX_PULSE_BPM, MIN_PRESSURE_MMHG, MIN_PULSE_BPM, Reading

MAX_IMPORT_ROWS = 100_000
MAX_FILE_BYTES = 16 * 1024 * 1024
MONTHS = ("jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec")
NOON = 12
# A flag column counts as set unless it is empty or one of these.
FALSE_FLAGS = frozenset({"", "0", "no", "false", "-", "--", "none"})


class OmronExportError(ValueError):
    """The file is not an OMRON app export, or a row in it is malformed."""


def _column(header: list[str], prefix: str) -> int | None:
    """Index of the first column whose name starts with ``prefix`` (case-insensitive)."""
    assert prefix and len(header) < 64
    for index, name in enumerate(header):
        if name.strip().lower().startswith(prefix):
            return index
    return None


def _flag_column(header: list[str], word: str) -> int | None:
    assert word and len(header) < 64
    for index, name in enumerate(header):
        if word in name.lower():
            return index
    return None


def parse_date_time(date_text: str, time_text: str) -> datetime:
    """``"Dec 25, 2025"`` and ``"6:10 AM"`` (or ``"18:10"``) to a datetime at second 0."""
    parts = date_text.replace(",", " ").split()
    if len(parts) != 3 or parts[0][:3].lower() not in MONTHS or not parts[1].isdigit() or not parts[2].isdigit():
        raise OmronExportError(f"unrecognised date {date_text!r}")
    month = MONTHS.index(parts[0][:3].lower()) + 1
    clock = time_text.strip().upper()
    suffix = clock[-2:] if clock.endswith(("AM", "PM")) else ""
    hour_text, _, minute_text = clock.removesuffix(suffix).strip().partition(":")
    if not hour_text.isdigit() or not minute_text.isdigit():
        raise OmronExportError(f"unrecognised time {time_text!r}")
    hour = int(hour_text)
    if suffix:
        if not 1 <= hour <= NOON:
            raise OmronExportError(f"unrecognised time {time_text!r}")
        hour = hour % NOON + (NOON if suffix == "PM" else 0)
    try:
        when = datetime(int(parts[2]), month, int(parts[1]), hour, int(minute_text), 0)
    except ValueError as exc:
        raise OmronExportError(f"invalid date/time {date_text!r} {time_text!r}") from exc
    assert when.second == 0
    return when


def _flag(row: list[str], col: int | None) -> bool:
    return col is not None and col < len(row) and row[col].strip().lower() not in FALSE_FLAGS


def _number(text: str, low: int, high: int, what: str, line: int) -> int:
    value = text.strip()
    if not value.isdigit() or not low <= int(value) <= high:
        raise OmronExportError(f"line {line}: {what} {text!r} is not between {low} and {high}")
    return int(value)


def parse_export(text: str) -> list[Reading]:
    """All readings in an OMRON app export, oldest first.  Raises OmronExportError on a bad file."""
    rows = list(csv.reader(io.StringIO(text.removeprefix("﻿"))))
    if not rows:
        raise OmronExportError("the file is empty")
    if len(rows) > MAX_IMPORT_ROWS:
        raise OmronExportError(f"more than {MAX_IMPORT_ROWS} rows")
    header = rows[0]
    cols = [_column(header, name) for name in ("date", "time", "systolic", "diastolic", "pulse")]
    if any(c is None for c in cols):
        raise OmronExportError("not an OMRON app export: expected Date, Time, Systolic, Diastolic and Pulse columns")
    date_col, time_col, sys_col, dia_col, pulse_col = (c for c in cols if c is not None)
    irregular_col, movement_col = _flag_column(header, "irregular"), _flag_column(header, "movement")
    readings: list[Reading] = []
    for line, row in enumerate(rows[1:], start=2):
        if not any(field.strip() for field in row):
            continue
        if len(row) <= max(date_col, time_col, sys_col, dia_col, pulse_col):
            raise OmronExportError(f"line {line}: too few columns")

        readings.append(
            Reading(
                timestamp=parse_date_time(row[date_col], row[time_col]),
                systolic=_number(row[sys_col], MIN_PRESSURE_MMHG, MAX_PRESSURE_MMHG, "systolic", line),
                diastolic=_number(row[dia_col], MIN_PRESSURE_MMHG, MAX_PRESSURE_MMHG, "diastolic", line),
                pulse=_number(row[pulse_col], MIN_PULSE_BPM, MAX_PULSE_BPM, "pulse", line),
                movement=_flag(row, movement_col),
                irregular_heartbeat=_flag(row, irregular_col),
            )
        )
    readings.sort(key=lambda r: r.timestamp)
    assert len(readings) < len(rows)
    return readings


def read_export(path: Path) -> list[Reading]:
    """Parse the export at ``path`` (UTF-8, with or without a BOM)."""
    assert path.name, "path required"
    if path.stat().st_size > MAX_FILE_BYTES:
        raise OmronExportError(f"{path} is larger than {MAX_FILE_BYTES // (1024 * 1024)} MB")
    return parse_export(path.read_text(encoding="utf-8-sig"))
