import csv
from datetime import datetime
from pathlib import Path

from omron_bp.models import Reading
from omron_bp.storage import CSV_COLUMNS, KnownDevice, append_readings, existing_keys, export_readings, load_devices, save_device


def reading(day: int, sys_: int = 120) -> Reading:
    return Reading(datetime(2026, 9, day, 8, 0, 0), sys_, 80, 60, False, False)


def test_append_creates_file_with_header(tmp_path: Path) -> None:
    path = tmp_path / "r.csv"
    assert append_readings(path, "evolv", "HEM-7600T", [[reading(1), reading(2)]]) == 2
    with path.open(newline="") as handle:
        rows = list(csv.DictReader(handle))
    assert [tuple(rows[0].keys())] == [CSV_COLUMNS]
    assert [r["timestamp"] for r in rows] == ["2026-09-01 08:00:00", "2026-09-02 08:00:00"]
    assert rows[0]["user"] == "1" and rows[0]["systolic"] == "120"


def test_append_skips_duplicates(tmp_path: Path) -> None:
    path = tmp_path / "r.csv"
    append_readings(path, "evolv", "HEM-7600T", [[reading(1)]])
    assert append_readings(path, "evolv", "HEM-7600T", [[reading(1), reading(2)]]) == 1
    assert len(existing_keys(path)) == 2


def test_same_time_on_other_device_or_user_is_kept(tmp_path: Path) -> None:
    path = tmp_path / "r.csv"
    append_readings(path, "evolv", "HEM-7600T", [[reading(1)]])
    assert append_readings(path, "ten", "HEM-7342T", [[reading(1)], [reading(1)]]) == 2


def test_registry_round_trip(tmp_path: Path) -> None:
    path = tmp_path / "devices.json"
    assert load_devices(path) == {}
    save_device(path, KnownDevice("evolv", "HEM-7600T", "AA:BB:CC:DD:EE:01"))
    save_device(path, KnownDevice("ten", "HEM-7342T", "AA:BB:CC:DD:EE:02"))
    save_device(path, KnownDevice("evolv", "HEM-7600T", "AA:BB:CC:DD:EE:03"))  # overwrite
    devices = load_devices(path)
    assert set(devices) == {"evolv", "ten"}
    assert devices["evolv"].address == "AA:BB:CC:DD:EE:03"


def test_export_time_bound_and_device(tmp_path: Path) -> None:
    dt = datetime
    path = tmp_path / "r.csv"
    append_readings(path, "evolv", "HEM-7600T", [[reading(1), reading(20)]])
    append_readings(path, "ten", "HEM-7342T", [[], [reading(25)]])
    out = tmp_path / "out.csv"
    assert export_readings(path, out, since=dt(2026, 9, 10), device=None) == 2
    rows = list(csv.DictReader(out.open(newline="")))
    assert [r["device"] for r in rows] == ["evolv", "ten"]
    assert export_readings(path, out, since=None, device="ten") == 1
    assert export_readings(path, out, since=dt(2030, 1, 1), device=None) == 0
    assert out.read_text().splitlines() == [",".join(CSV_COLUMNS)]


def test_same_timestamp_different_values_is_kept(tmp_path: Path) -> None:
    # A monitor with a stopped clock stamps every new reading identically.
    path = tmp_path / "r.csv"
    assert append_readings(path, "evolv", "HEM-7600T", [[reading(1, 148)]]) == 1
    assert append_readings(path, "evolv", "HEM-7600T", [[reading(1, 148), reading(1, 158)]]) == 1
    assert len(existing_keys(path)) == 2
