"""CSV persistence for readings and a small JSON registry of known monitors."""

from __future__ import annotations

import csv
import json
import logging
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

from omron_bp.models import Reading

log = logging.getLogger("omron_bp")

CSV_COLUMNS = (
    "timestamp",
    "model",
    "device",
    "user",
    "systolic",
    "diastolic",
    "pulse",
    "movement",
    "irregular_heartbeat",
)
TIMESTAMP_FORMAT = "%Y-%m-%d %H:%M:%S"
MAX_CSV_ROWS = 1_000_000

# A reading is identified by which monitor/user it came from, when, and its values.
# The values are part of the key because a monitor with a stopped clock stamps
# every new measurement with the same time.
ReadingKey = tuple[str, int, str, int, int, int]


def _key(device: str, user: int, reading: Reading) -> ReadingKey:
    return device, user, reading.timestamp.strftime(TIMESTAMP_FORMAT), reading.systolic, reading.diastolic, reading.pulse


def existing_keys(path: Path) -> set[ReadingKey]:
    """Keys of every reading already in the CSV (empty set if it does not exist)."""
    if not path.is_file():
        return set()
    keys: set[ReadingKey] = set()
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        for row_number, row in enumerate(reader):
            if row_number >= MAX_CSV_ROWS:
                raise RuntimeError(f"{path} has more than {MAX_CSV_ROWS} rows")
            keys.add((row["device"], int(row["user"]), row["timestamp"], int(row["systolic"]), int(row["diastolic"]), int(row["pulse"])))
    return keys


def append_readings(
    path: Path,
    device: str,
    model: str,
    per_user: list[list[Reading]],
) -> int:
    """Append readings not already present.  Returns the number written."""
    assert device and model
    assert len(per_user) <= 8, "unexpected user count"
    seen = existing_keys(path)
    is_new_file = not path.is_file()
    written = 0
    with path.open("a", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=CSV_COLUMNS)
        if is_new_file:
            writer.writeheader()
        for user_index, readings in enumerate(per_user):
            user = user_index + 1
            for reading in readings:
                key = _key(device, user, reading)
                if key in seen:
                    continue
                seen.add(key)
                writer.writerow(
                    {
                        "timestamp": reading.timestamp.strftime(TIMESTAMP_FORMAT),
                        "model": model,
                        "device": device,
                        "user": user,
                        "systolic": reading.systolic,
                        "diastolic": reading.diastolic,
                        "pulse": reading.pulse,
                        "movement": int(reading.movement),
                        "irregular_heartbeat": int(reading.irregular_heartbeat),
                    }
                )
                written += 1
    assert written >= 0
    return written


def export_readings(source: Path, out: Path, since: datetime | None, device: str | None) -> int:
    """Copy rows taken at or after ``since`` (all if None), optionally for one device, to ``out``."""
    assert source != out, "output must differ from the source"
    written = 0
    with source.open(newline="", encoding="utf-8") as src, out.open("w", newline="", encoding="utf-8") as dst:
        reader = csv.DictReader(src)
        assert reader.fieldnames is not None and tuple(reader.fieldnames) == CSV_COLUMNS, reader.fieldnames
        writer = csv.DictWriter(dst, fieldnames=CSV_COLUMNS)
        writer.writeheader()
        rows = []
        for row_number, row in enumerate(reader):
            if row_number >= MAX_CSV_ROWS:
                raise RuntimeError(f"{source} has more than {MAX_CSV_ROWS} rows")
            if device is not None and row["device"] != device:
                continue
            stamp = datetime.strptime(row["timestamp"], TIMESTAMP_FORMAT)
            if since is not None and stamp < since:
                continue
            rows.append((stamp, row))
        rows.sort(key=lambda item: item[0])
        for _, row in rows:
            writer.writerow(row)
            written += 1
    assert written <= MAX_CSV_ROWS
    return written


# --- device registry ----------------------------------------------------------


@dataclass(frozen=True)
class KnownDevice:
    name: str
    model: str
    address: str


def load_devices(path: Path) -> dict[str, KnownDevice]:
    if not path.is_file():
        return {}
    raw = json.loads(path.read_text(encoding="utf-8"))
    assert isinstance(raw, dict), f"{path} must hold a JSON object"
    devices: dict[str, KnownDevice] = {}
    for name, entry in raw.items():
        assert isinstance(entry, dict) and "model" in entry and "address" in entry, name
        devices[name] = KnownDevice(name=name, model=str(entry["model"]), address=str(entry["address"]))
    return devices


def save_device(path: Path, device: KnownDevice) -> None:
    assert device.name and device.model and device.address
    devices = load_devices(path)
    devices[device.name] = device
    payload = {d.name: {"model": d.model, "address": d.address} for d in devices.values()}
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    log.info("saved '%s' (%s at %s) to %s", device.name, device.model, device.address, path)
