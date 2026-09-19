import csv
from datetime import datetime
from pathlib import Path

import pytest

from omron_bp.chart import _group, render
from omron_bp.models import Reading
from omron_bp.storage import CSV_COLUMNS, append_readings


def reading(day: int, hour: int = 8) -> Reading:
    return Reading(datetime(2026, 9, day, hour, 0, 0), 120 + day, 80, 60, False, day % 2 == 0)


def test_group_sorts_by_time_within_device_and_user(tmp_path: Path) -> None:
    path = tmp_path / "r.csv"
    append_readings(path, "evolv", "HEM-7600T", [[reading(3), reading(1)]])
    append_readings(path, "ten", "HEM-7342T", [[], [reading(2)]])
    with path.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    groups = _group(rows)
    assert [(g["device"], g["user"]) for g in groups] == [("evolv", 1), ("ten", 2)]
    evolv = groups[0]["readings"]
    assert isinstance(evolv, list) and [r["sys"] for r in evolv] == [121, 123] and evolv[0]["ihb"] == 0


def test_render_embeds_data_and_refuses_empty(tmp_path: Path) -> None:
    path = tmp_path / "r.csv"
    append_readings(path, "evolv", "HEM-7600T", [[reading(1)]])
    out = tmp_path / "chart.html"
    assert render(path, out) == 1
    html = out.read_text(encoding="utf-8")
    assert "/*__DATA__*/" not in html and '"device":"evolv"' in html and "<title>" in html
    empty = tmp_path / "empty.csv"
    empty.write_text(",".join(CSV_COLUMNS) + "\n", encoding="utf-8")
    with pytest.raises(RuntimeError, match="no readings"):
        render(empty, out)
