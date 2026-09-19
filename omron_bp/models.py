"""Per-model EEPROM layouts and record formats.

Each OMRON model stores readings as a ring buffer of fixed-size records per
user.  The field positions inside a record differ between models, so every
model is described as data (a :class:`DeviceLayout`) rather than code.

Retail names map to OMRON's internal HEM- part numbers; e.g. the North
American "BP7000" (Evolv) is a HEM-7600T and the "BP7450" (10 Series) is a
HEM-7342T.  The BP7455CAN is the Canadian variant of the BP7450.
"""

from dataclasses import dataclass
from datetime import datetime

from omron_bp.bitfield import Endianness, bits_to_int

# Value ranges used to reject garbage records (e.g. partially erased slots).
MIN_PRESSURE_MMHG = 20
MAX_PRESSURE_MMHG = 300
MIN_PULSE_BPM = 20
MAX_PULSE_BPM = 250

# Physiologically impossible values are what an erased slot decodes to.
SYSTOLIC_OFFSET = 25
YEAR_OFFSET = 2000
MAX_SECOND = 59


@dataclass(frozen=True)
class Reading:
    """One stored blood-pressure measurement."""

    timestamp: datetime
    systolic: int
    diastolic: int
    pulse: int
    movement: bool
    irregular_heartbeat: bool

    def __post_init__(self) -> None:
        assert MIN_PRESSURE_MMHG <= self.systolic <= MAX_PRESSURE_MMHG, f"systolic {self.systolic}"
        assert MIN_PRESSURE_MMHG <= self.diastolic <= MAX_PRESSURE_MMHG, f"diastolic {self.diastolic}"
        assert MIN_PULSE_BPM <= self.pulse <= MAX_PULSE_BPM, f"pulse {self.pulse}"


BitRange = tuple[int, int]


@dataclass(frozen=True)
class RecordBits:
    """Bit positions (inclusive, MSB-first) of each field inside one record."""

    diastolic: BitRange
    systolic: BitRange
    pulse: BitRange
    movement: BitRange
    irregular_heartbeat: BitRange
    year: BitRange
    month: BitRange
    day: BitRange
    hour: BitRange
    minute: BitRange
    second: BitRange


@dataclass(frozen=True)
class ClockLayout:
    """The settings record that holds the monitor's clock.

    The record is read from ``read_address`` and written back to ``write_address``
    (the monitor exposes separate read and write views of its settings).  Bytes
    ``0..prefix`` are copied back unchanged, the six date/time fields are set at
    their offsets, ``pad_offset`` is zeroed and ``checksum_offset`` receives the
    low byte of the sum of the first ``checksum_span`` bytes.  The same checksum
    is verified on read, which is how the layout is confirmed before anything is
    ever written.
    """

    read_address: int
    write_address: int
    size: int
    prefix: int
    year: int
    month: int
    day: int
    hour: int
    minute: int
    second: int
    pad_offset: int
    checksum_offset: int
    checksum_span: int

    def __post_init__(self) -> None:
        assert 0 < self.size <= 0x38
        offsets = (self.year, self.month, self.day, self.hour, self.minute, self.second, self.pad_offset, self.checksum_offset)
        assert len(set(offsets)) == len(offsets) and all(0 <= o < self.size for o in offsets)
        assert 0 <= self.prefix < self.size and 0 < self.checksum_span < self.size


@dataclass(frozen=True)
class DeviceLayout:
    """Where records live in EEPROM and how to decode them."""

    model: str
    endianness: Endianness
    user_start_addresses: tuple[int, ...]
    records_per_user: tuple[int, ...]
    record_size: int
    read_block_size: int
    bits: RecordBits
    clock: ClockLayout | None = None

    def __post_init__(self) -> None:
        assert len(self.user_start_addresses) == len(self.records_per_user)
        assert 0 < self.record_size <= 32
        assert 0 < self.read_block_size <= 0x38, "device replies carry at most 0x38 data bytes"

    @property
    def user_count(self) -> int:
        return len(self.user_start_addresses)

    def user_region(self, user_index: int) -> tuple[int, int]:
        """Return ``(address, byte_count)`` of the whole record area for one user."""
        assert 0 <= user_index < self.user_count, user_index
        size = self.records_per_user[user_index] * self.record_size
        return self.user_start_addresses[user_index], size


def parse_record(layout: DeviceLayout, record: bytes) -> Reading:
    """Decode one raw record.  Raises ``ValueError`` if the slot is not a valid reading."""
    assert len(record) == layout.record_size, f"expected {layout.record_size} bytes, got {len(record)}"
    bits = layout.bits

    def field(rng: BitRange) -> int:
        return bits_to_int(record, rng[0], rng[1], layout.endianness)

    systolic = field(bits.systolic) + SYSTOLIC_OFFSET
    diastolic = field(bits.diastolic)
    pulse = field(bits.pulse)
    assert systolic >= SYSTOLIC_OFFSET and diastolic >= 0 and pulse >= 0
    # Some firmware stores seconds in a 6-bit field that can exceed 59.
    second = min(field(bits.second), MAX_SECOND)
    try:
        timestamp = datetime(
            field(bits.year) + YEAR_OFFSET,
            field(bits.month),
            field(bits.day),
            field(bits.hour),
            field(bits.minute),
            second,
        )
    except ValueError as exc:
        raise ValueError(f"invalid timestamp in record {record.hex()}") from exc

    in_range = (
        MIN_PRESSURE_MMHG <= systolic <= MAX_PRESSURE_MMHG
        and MIN_PRESSURE_MMHG <= diastolic <= MAX_PRESSURE_MMHG
        and MIN_PULSE_BPM <= pulse <= MAX_PULSE_BPM
    )
    if not in_range:
        raise ValueError(f"implausible values in record {record.hex()}")

    return Reading(
        timestamp=timestamp,
        systolic=systolic,
        diastolic=diastolic,
        pulse=pulse,
        movement=bool(field(bits.movement)),
        irregular_heartbeat=bool(field(bits.irregular_heartbeat)),
    )


def clock_checksum_ok(clock: ClockLayout, record: bytes) -> bool:
    """True if the record carries the expected checksum, i.e. the layout matches this monitor."""
    assert len(record) == clock.size
    return (sum(record[: clock.checksum_span]) & 0xFF) == record[clock.checksum_offset]


def parse_clock(clock: ClockLayout, record: bytes) -> datetime:
    """Decode the monitor's clock.  Raises ``ValueError`` for an invalid date."""
    assert len(record) == clock.size
    assert clock.checksum_span < clock.size
    try:
        return datetime(
            record[clock.year] + YEAR_OFFSET,
            record[clock.month],
            record[clock.day],
            record[clock.hour],
            record[clock.minute],
            min(record[clock.second], MAX_SECOND),
        )
    except ValueError as exc:
        raise ValueError(f"invalid clock record {record.hex()}") from exc


def encode_clock(clock: ClockLayout, current: bytes, when: datetime) -> bytes:
    """Build the record to write so the monitor's clock reads ``when``."""
    assert len(current) == clock.size
    assert YEAR_OFFSET <= when.year < YEAR_OFFSET + 100, when
    out = bytearray(current)
    out[clock.year] = when.year - YEAR_OFFSET
    out[clock.month] = when.month
    out[clock.day] = when.day
    out[clock.hour] = when.hour
    out[clock.minute] = when.minute
    out[clock.second] = when.second
    out[clock.pad_offset] = 0
    out[clock.checksum_offset] = sum(out[: clock.checksum_span]) & 0xFF
    assert out[: clock.prefix] == current[: clock.prefix]
    assert clock_checksum_ok(clock, bytes(out))
    return bytes(out)


def is_empty_slot(layout: DeviceLayout, record: bytes) -> bool:
    """An unused ring-buffer slot reads back as all 0xFF."""
    assert len(record) == layout.record_size
    return record == b"\xff" * layout.record_size


# --- Model definitions -------------------------------------------------------

HEM_7600T = DeviceLayout(
    model="HEM-7600T",
    endianness="big",
    user_start_addresses=(0x02AC,),
    records_per_user=(100,),
    record_size=0x0E,
    read_block_size=0x38,
    bits=RecordBits(
        diastolic=(0, 7),
        systolic=(8, 15),
        year=(16, 23),
        pulse=(24, 31),
        movement=(32, 32),
        irregular_heartbeat=(33, 33),
        month=(34, 37),
        day=(38, 42),
        hour=(43, 47),
        minute=(52, 57),
        second=(58, 63),
    ),
    # Settings read at 0x0260 / written at 0x0286; clock is bytes 0x14..0x1e of that record.
    clock=ClockLayout(
        read_address=0x0274, write_address=0x029A, size=10, prefix=2,
        month=2, year=3, hour=4, day=5, second=6, minute=7, pad_offset=8, checksum_offset=9, checksum_span=8,
    ),
)

HEM_7342T = DeviceLayout(
    model="HEM-7342T",
    endianness="little",
    user_start_addresses=(0x0098, 0x06D8),
    records_per_user=(100, 100),
    record_size=0x10,
    read_block_size=0x10,
    bits=RecordBits(
        minute=(68, 73),
        second=(74, 79),
        movement=(80, 80),
        irregular_heartbeat=(81, 81),
        month=(82, 85),
        day=(86, 90),
        hour=(91, 95),
        year=(98, 103),
        pulse=(104, 111),
        diastolic=(112, 119),
        systolic=(120, 127),
    ),
    # Settings read at 0x0010 / written at 0x0054; clock is bytes 0x2c..0x3c of that record.
    clock=ClockLayout(
        read_address=0x003C, write_address=0x0080, size=16, prefix=8,
        year=8, month=9, day=10, hour=11, minute=12, second=13, checksum_offset=14, pad_offset=15, checksum_span=14,
    ),
)

LAYOUTS: dict[str, DeviceLayout] = {
    HEM_7600T.model: HEM_7600T,
    HEM_7342T.model: HEM_7342T,
}

# Retail / marketing names -> internal model.
ALIASES: dict[str, str] = {
    "BP7000": "HEM-7600T",
    "EVOLV": "HEM-7600T",
    "BP7450": "HEM-7342T",
    "BP7455": "HEM-7342T",
    "BP7455CAN": "HEM-7342T",
    "10SERIES": "HEM-7342T",
}


def resolve_layout(name: str) -> DeviceLayout:
    """Look up a layout by internal model or retail alias (case-insensitive)."""
    assert name, "model name must not be empty"
    key = name.strip().upper().replace("_", "-")
    canonical = ALIASES.get(key.replace("-", "").replace(" ", ""), key)
    if canonical not in LAYOUTS:
        known = ", ".join(sorted(LAYOUTS) + sorted(ALIASES))
        raise KeyError(f"unknown model '{name}'; known: {known}")
    return LAYOUTS[canonical]
