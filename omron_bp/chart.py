"""Render ``readings.csv`` as a self-contained HTML chart.

The page template (``chart.html``) carries all of its own CSS and JavaScript;
the readings are embedded as JSON, so the file opens offline and nothing is
sent anywhere.
"""

from __future__ import annotations

import csv
import json
import logging
from datetime import datetime
from pathlib import Path

from omron_bp.storage import CSV_COLUMNS, MAX_CSV_ROWS, TIMESTAMP_FORMAT

log = logging.getLogger("omron_bp")

TEMPLATE = Path(__file__).with_name("chart.html")
DATA_MARKER = "/*__DATA__*/{}"
MAX_GROUPS = 32


def _load_rows(csv_path: Path) -> list[dict[str, str]]:
    assert csv_path.is_file(), f"{csv_path} does not exist"
    with csv_path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        assert reader.fieldnames is not None and tuple(reader.fieldnames) == CSV_COLUMNS, reader.fieldnames
        rows: list[dict[str, str]] = []
        for row in reader:
            if len(rows) >= MAX_CSV_ROWS:
                raise RuntimeError(f"{csv_path} has more than {MAX_CSV_ROWS} rows")
            rows.append(row)
    return rows


def _group(rows: list[dict[str, str]]) -> list[dict[str, object]]:
    """One group per (device, user), readings sorted by time, as compact JSON-ready dicts."""
    groups: dict[tuple[str, int], dict[str, object]] = {}
    for row in rows:
        key = (row["device"], int(row["user"]))
        group = groups.setdefault(key, {"device": row["device"], "model": row["model"], "user": key[1], "readings": []})
        stamp = datetime.strptime(row["timestamp"], TIMESTAMP_FORMAT)
        readings = group["readings"]
        assert isinstance(readings, list)
        readings.append(
            {
                "t": int(stamp.timestamp() * 1000),
                "sys": int(row["systolic"]),
                "dia": int(row["diastolic"]),
                "pulse": int(row["pulse"]),
                "mov": int(row["movement"]),
                "ihb": int(row["irregular_heartbeat"]),
            }
        )
    assert len(groups) <= MAX_GROUPS, "unexpected number of device/user slots"
    for group in groups.values():
        readings = group["readings"]
        assert isinstance(readings, list)
        readings.sort(key=lambda r: int(r["t"]))
    return [groups[k] for k in sorted(groups)]


def render(csv_path: Path, out_path: Path) -> int:
    """Write the chart page.  Returns the number of readings plotted."""
    rows = _load_rows(csv_path)
    if not rows:
        raise RuntimeError(f"{csv_path} has no readings yet - run 'read' first")
    payload = {
        "generated": datetime.now().strftime("%Y-%m-%d %H:%M"),
        "source": csv_path.name,
        "groups": _group(rows),
    }
    assert TEMPLATE.is_file(), TEMPLATE
    template = TEMPLATE.read_text(encoding="utf-8")
    assert template.count(DATA_MARKER) == 1, "template data marker missing"
    page = template.replace(DATA_MARKER, json.dumps(payload, separators=(",", ":")))
    out_path.write_text(page, encoding="utf-8")
    log.info("wrote %s (%d readings)", out_path, len(rows))
    return len(rows)
