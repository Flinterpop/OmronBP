"""Command-line interface: ``scan``, ``pair`` and ``read``."""

from __future__ import annotations

import argparse
import asyncio
import csv
import logging
import sys
import time
import webbrowser
from collections.abc import Sequence
from datetime import datetime, timedelta
from pathlib import Path

from bleak.exc import BleakDeviceNotFoundError, BleakError

from omron_bp import chart, pdf, reader
from omron_bp.models import ALIASES, LAYOUTS, Reading, resolve_layout
from omron_bp.storage import KnownDevice, append_readings, export_readings, load_devices, save_device
from omron_bp.transport import KEY_SIZE, ProtocolError

log = logging.getLogger("omron_bp")

DEFAULT_SCAN_S = 8.0
MAX_SCAN_S = 60.0
DEFAULT_CSV = Path("readings.csv")
DEFAULT_REGISTRY = Path("devices.json")
DEFAULT_CHART = Path("readings.html")
MAX_DEVICES_PER_RUN = 16
CONNECT_ATTEMPTS = 3
CONNECT_RETRY_DELAY_S = 2.0


def _parse_key(text: str | None) -> bytes:
    if text is None:
        return reader.DEFAULT_KEY
    try:
        key = bytes.fromhex(text)
    except ValueError as exc:
        raise SystemExit(f"--key must be {KEY_SIZE * 2} hex digits") from exc
    if len(key) != KEY_SIZE:
        raise SystemExit(f"--key must be {KEY_SIZE * 2} hex digits, got {len(text)}")
    return key


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="omron_bp", description="Download readings from OMRON Bluetooth blood-pressure monitors.")
    parser.add_argument("--debug", action="store_true", help="log every Bluetooth packet")
    parser.add_argument("--devices", type=Path, default=DEFAULT_REGISTRY, help=f"device registry file (default {DEFAULT_REGISTRY})")
    parser.add_argument("--key", help=f"pairing key as {KEY_SIZE * 2} hex digits (default: shared with omblepy/UBPM)")
    sub = parser.add_subparsers(dest="command", required=True)

    p_scan = sub.add_parser("scan", help="list advertising monitors (press the monitor's Bluetooth button first)")
    p_scan.add_argument("--timeout", type=float, default=DEFAULT_SCAN_S, help="seconds to listen")
    p_scan.add_argument("--all", action="store_true", help="show every BLE device, not just OMRON")

    p_pair = sub.add_parser("pair", help="pair a monitor (hold its Bluetooth button until the P blinks)")
    p_pair.add_argument("name", help="nickname to save it under, e.g. evolv")
    p_pair.add_argument("--model", required=True, help="model, e.g. BP7000, BP7455CAN, HEM-7600T")
    p_pair.add_argument("--address", help="Bluetooth address; omit to auto-detect the only monitor in range")
    p_pair.add_argument("--timeout", type=float, default=DEFAULT_SCAN_S, help="seconds to scan when auto-detecting")

    p_read = sub.add_parser("read", help="download readings and append them to the CSV")
    p_read.add_argument("names", nargs="*", help="registered device names (default: all registered)")
    p_read.add_argument("--model", help="read an unregistered monitor of this model ...")
    p_read.add_argument("--address", help="... at this Bluetooth address")
    p_read.add_argument("--csv", type=Path, default=DEFAULT_CSV, help=f"output file (default {DEFAULT_CSV})")
    p_read.add_argument("--no-clock-sync", action="store_true", help="report the monitor clock drift but never correct it")

    p_models = sub.add_parser("models", help="list supported models")
    assert p_models is not None

    p_chart = sub.add_parser("chart", help="render the CSV as an interactive HTML chart")
    p_chart.add_argument("--csv", type=Path, default=DEFAULT_CSV, help=f"input file (default {DEFAULT_CSV})")
    p_chart.add_argument("--out", type=Path, default=DEFAULT_CHART, help=f"output page (default {DEFAULT_CHART})")
    p_chart.add_argument("--open", action="store_true", help="open the page in the default browser afterwards")

    p_export = sub.add_parser("export", help="write a time-bounded copy of the readings to a new CSV")
    p_export.add_argument("--days", type=int, default=0, help="only readings from the last N days (default: everything)")
    p_export.add_argument("--device", help="only this registered nickname (default: all monitors)")
    p_export.add_argument("--csv", type=Path, default=DEFAULT_CSV, help=f"input file (default {DEFAULT_CSV})")
    p_export.add_argument("--out", type=Path, help="output file (default: readings_<device>_<period>_<date>.csv)")
    p_export.add_argument("--pdf", action="store_true", help="also write a PDF report (summary and table) next to the CSV")
    return parser


# --- commands -------------------------------------------------------------------


def cmd_models() -> int:
    for model in sorted(LAYOUTS):
        aliases = sorted(alias for alias, target in ALIASES.items() if target == model)
        print(f"{model:<12} {', '.join(aliases)}")
    return 0


def cmd_chart(args: argparse.Namespace) -> int:
    count = chart.render(args.csv, args.out)
    print(f"{count} readings charted in {args.out}")
    if args.open:
        webbrowser.open(args.out.resolve().as_uri())
    return 0


def cmd_export(args: argparse.Namespace) -> int:
    if args.days < 0 or args.days > 36500:
        raise SystemExit("--days must be between 0 and 36500")
    since = datetime.now() - timedelta(days=args.days) if args.days else None
    out = args.out or Path(
        f"readings_{args.device or 'all'}_{'last' + str(args.days) + 'days' if args.days else 'everything'}_{datetime.now():%Y-%m-%d}.csv"
    )
    count = export_readings(args.csv, out, since=since, device=args.device)
    print(f"{count} reading{'s' if count != 1 else ''} exported to {out}")
    if args.pdf:
        options = pdf.ReportOptions(
            scope=args.device or "all monitors",
            period=f"last {args.days} days" if args.days else "all stored readings",
            generated=f"{datetime.now():%Y-%m-%d}",
            show_monitor_column=args.device is None,
        )
        with out.open(newline="", encoding="utf-8") as handle:
            rows = list(csv.DictReader(handle))
        pdf_path = out.with_suffix(".pdf")
        pages = pdf.write_report(pdf_path, pdf.build_report(rows, options))
        print(f"{pages}-page PDF report written to {pdf_path}")
    return 0 if count else 1


def cmd_scan(timeout_s: float, include_all: bool) -> int:
    if not 0 < timeout_s <= MAX_SCAN_S:
        raise SystemExit(f"--timeout must be between 0 and {MAX_SCAN_S}")
    print(f"Scanning for {timeout_s:.0f}s ... (press the Bluetooth button on the monitor)")
    found = asyncio.run(reader.scan(timeout_s, include_all))
    if not found:
        print("No OMRON monitors seen. Try again with the monitor's Bluetooth symbol showing.")
        return 1
    print(f"{'ADDRESS':<20} {'RSSI':>5}  NAME")
    for dev in found:
        tag = "" if dev.is_omron else "  (not OMRON)"
        print(f"{dev.address:<20} {dev.rssi:>5}  {dev.name}{tag}")
    return 0


def _auto_detect_address(timeout_s: float) -> str:
    print(f"Scanning for {timeout_s:.0f}s to find the monitor ...")
    found = asyncio.run(reader.scan(timeout_s))
    if len(found) != 1:
        seen = ", ".join(f"{d.name} ({d.address})" for d in found) or "none"
        raise SystemExit(f"expected exactly one OMRON monitor in range, saw: {seen}. Use --address.")
    return found[0].address


def cmd_pair(args: argparse.Namespace, key: bytes) -> int:
    layout = resolve_layout(args.model)
    address = args.address or _auto_detect_address(args.timeout)
    print(f"Pairing '{args.name}' ({layout.model}) at {address}.")
    others = _other_addresses(load_devices(args.devices), address)
    try:
        asyncio.run(reader.pair(address, key, others))
    except BleakDeviceNotFoundError as exc:
        raise SystemExit(f"{exc} Is the monitor still in pairing mode? Re-enter it and run again.") from exc
    save_device(args.devices, KnownDevice(name=args.name, model=layout.model, address=address))
    print(f"Paired. Next: python -m omron_bp read {args.name}")
    return 0


def _other_addresses(registry: dict[str, KnownDevice], address: str) -> list[str]:
    """Addresses of every registered monitor except ``address`` (bonds to release)."""
    return [d.address for d in registry.values() if d.address.upper() != address.upper()]


def _targets(args: argparse.Namespace) -> list[KnownDevice]:
    if args.address or args.model:
        if not (args.address and args.model):
            raise SystemExit("--model and --address must be given together")
        return [KnownDevice(name=args.address, model=resolve_layout(args.model).model, address=args.address)]
    registry = load_devices(args.devices)
    if args.names:
        missing = [n for n in args.names if n not in registry]
        if missing:
            raise SystemExit(f"not in {args.devices}: {', '.join(missing)}")
        return [registry[n] for n in args.names]
    if not registry:
        raise SystemExit(f"no devices in {args.devices}; run 'pair' first or give --model/--address")
    return list(registry.values())


def _print_summary(device: KnownDevice, per_user: list[list[Reading]]) -> None:
    for user_index, readings in enumerate(per_user):
        print(f"  user {user_index + 1}: {len(readings)} readings")
        if readings:
            last = readings[-1]
            print(f"    latest {last.timestamp:%Y-%m-%d %H:%M}  {last.systolic}/{last.diastolic} mmHg, {last.pulse} bpm")


def _print_clock(clock: reader.ClockStatus | None) -> None:
    if clock is None:
        print("  clock: no known clock layout for this model")
    elif clock.monitor_time is None:
        print("  clock: could not be read")
    elif clock.corrected:
        print(f"  clock: was {clock.drift_seconds:+d} s off, set to PC time (confirmed on the next read)")
    elif abs(clock.drift_seconds) <= reader.CLOCK_TOLERANCE_S:
        print(f"  clock: OK ({clock.drift_seconds:+d} s)")
    elif not clock.verified:
        print(f"  clock: {clock.drift_seconds:+d} s off, NOT corrected (record layout unverified for this monitor)")
    else:
        print(f"  clock: {clock.drift_seconds:+d} s off, not corrected (--no-clock-sync)")


def _download_with_retry(device: KnownDevice, key: bytes, others: list[str], sync_clock: bool) -> reader.DownloadResult | None:
    """Monitors sometimes drop a fresh connection; retry a few times before giving up."""
    layout = resolve_layout(device.model)
    for attempt in range(1, CONNECT_ATTEMPTS + 1):
        try:
            return asyncio.run(reader.download(device.address, layout, key, others, sync_clock))
        except (BleakError, ProtocolError, OSError, TimeoutError, RuntimeError) as exc:
            print(f"  attempt {attempt}/{CONNECT_ATTEMPTS} failed: {exc}")
            if attempt < CONNECT_ATTEMPTS:
                time.sleep(CONNECT_RETRY_DELAY_S)
    return None


def cmd_read(args: argparse.Namespace, key: bytes) -> int:
    targets = _targets(args)
    assert 0 < len(targets) <= MAX_DEVICES_PER_RUN
    registry = load_devices(args.devices)
    failures = 0
    for device in targets:
        layout = resolve_layout(device.model)
        print(f"{device.name} ({layout.model} at {device.address}): connecting ...")
        result = _download_with_retry(device, key, _other_addresses(registry, device.address), not args.no_clock_sync)
        if result is None:
            failures += 1
            print("  FAILED - is the Bluetooth symbol showing on the monitor?")
            continue
        _print_summary(device, result.per_user)
        _print_clock(result.clock)
        added = append_readings(args.csv, device.name, layout.model, result.per_user)
        print(f"  {added} new readings appended to {args.csv}")
    return 1 if failures else 0


def main(argv: Sequence[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    logging.basicConfig(
        level=logging.DEBUG if args.debug else logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
        stream=sys.stderr,
    )
    if not args.debug:
        logging.getLogger("bleak").setLevel(logging.WARNING)
    key = _parse_key(args.key)

    if args.command == "models":
        return cmd_models()
    if args.command == "chart":
        return cmd_chart(args)
    if args.command == "export":
        return cmd_export(args)
    if args.command == "scan":
        return cmd_scan(args.timeout, args.all)
    if args.command == "pair":
        return cmd_pair(args, key)
    assert args.command == "read", args.command
    return cmd_read(args, key)
