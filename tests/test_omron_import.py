import csv
from datetime import datetime
from pathlib import Path

import pytest

from omron_bp.models import Reading
from omron_bp.omron_import import OmronExportError, parse_date_time, parse_export, read_export
from omron_bp.storage import append_readings, import_readings

# The shape of a real OMRON connect export (values altered).
EXPORT = (
    "Date,Time,Systolic (mmHg),Diastolic (mmHg),Pulse (bpm),Notes\r\n"
    '"Dec 25, 2025",6:10 AM,150,88,48,\r\n'
    '"Dec 19, 2025",10:29 PM,152,89,56,after a walk\r\n'
    '"Sep 13, 2025",12:05 AM,149,86,58,"note, with comma"\r\n'
    "\r\n"
)


def test_parse_date_time_12_and_24_hour() -> None:
    assert parse_date_time("Dec 25, 2025", "6:10 AM") == datetime(2025, 12, 25, 6, 10)
    assert parse_date_time("Dec 19, 2025", "10:29 PM") == datetime(2025, 12, 19, 22, 29)
    assert parse_date_time("Sep 13, 2025", "12:05 AM") == datetime(2025, 9, 13, 0, 5)
    assert parse_date_time("Sep 13, 2025", "12:05 PM") == datetime(2025, 9, 13, 12, 5)
    assert parse_date_time("September 3, 2025", "18:05") == datetime(2025, 9, 3, 18, 5)


@pytest.mark.parametrize(
    ("date", "time"),
    [("Foo 1, 2025", "6:10 AM"), ("Feb 30, 2025", "6:10 AM"), ("Dec 1, 2025", "13:10 PM"), ("Dec 1, 2025", "6 AM")],
)
def test_parse_date_time_rejects_garbage(date: str, time: str) -> None:
    with pytest.raises(OmronExportError):
        parse_date_time(date, time)


def test_parse_export_oldest_first_with_quoted_notes() -> None:
    readings = parse_export("﻿" + EXPORT)
    assert [r.timestamp for r in readings] == [datetime(2025, 9, 13, 0, 5), datetime(2025, 12, 19, 22, 29), datetime(2025, 12, 25, 6, 10)]
    assert (readings[2].systolic, readings[2].diastolic, readings[2].pulse) == (150, 88, 48)
    assert not any(r.movement or r.irregular_heartbeat for r in readings)


def test_parse_export_flag_columns_by_name() -> None:
    text = (
        "Time,Date,Systolic,Diastolic,Pulse,Irregular heartbeat,Body Movement\n"
        '6:10 AM,"Dec 25, 2025",150,88,48,Yes,\n'
        '7:10 AM,"Dec 25, 2025",140,80,50,No,1\n'
    )
    first, second = parse_export(text)
    assert first.irregular_heartbeat and not first.movement
    assert second.movement and not second.irregular_heartbeat


@pytest.mark.parametrize(
    "text",
    ["", "timestamp,model,device\n", "Date,Time,Systolic (mmHg),Diastolic (mmHg),Pulse (bpm)\n\"Dec 25, 2025\",6:10 AM,150,88\n",
     "Date,Time,Systolic (mmHg),Diastolic (mmHg),Pulse (bpm)\n\"Dec 25, 2025\",6:10 AM,150,88,--\n"],
)
def test_parse_export_rejects_bad_files(text: str) -> None:
    with pytest.raises(OmronExportError):
        parse_export(text)


def test_import_is_idempotent_and_skips_bluetooth_duplicates(tmp_path: Path) -> None:
    export = tmp_path / "Report.csv"
    export.write_text(EXPORT, encoding="utf-8")
    out = tmp_path / "readings.csv"
    # The same measurement already downloaded over Bluetooth, with its seconds.
    append_readings(out, "ten", "HEM-7342T", [[], [Reading(datetime(2025, 12, 25, 6, 10, 41), 150, 88, 48, False, False)]])
    readings = read_export(export)
    assert import_readings(out, "ten", "HEM-7342T", 2, readings) == 2
    assert import_readings(out, "ten", "HEM-7342T", 2, readings) == 0
    assert import_readings(out, "ten", "HEM-7342T", 1, readings) == 3
    with out.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    assert len(rows) == 6
    assert rows[1]["timestamp"] == "2025-09-13 00:05:00" and rows[1]["user"] == "2" and rows[1]["model"] == "HEM-7342T"
