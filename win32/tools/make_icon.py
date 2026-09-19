"""Pack the 32-bit BMP layers drawn by draw_icon.ps1 into a multi-size Windows .ico.

Usage: python make_icon.py <layer dir> <out.ico>

Small sizes are classic DIB entries: BITMAPINFOHEADER (height doubled), bottom-up
32-bit BGRA pixels, then a 1-bit AND mask derived from the alpha channel.  The
128 and 256 px layers are stored as PNG, which Windows Vista and later accept.
No third-party libraries.
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

SIZES = (16, 24, 32, 48, 64, 128, 256)
PNG_FROM = 128  # sizes from here up are stored PNG-compressed
MAX_LAYERS = 16


def read_bmp32(path: Path) -> tuple[int, int, bytes]:
    """Return (width, height, bottom-up BGRA rows) from a 32-bpp BMP written by GDI+."""
    data = path.read_bytes()
    assert data[:2] == b"BM", path
    offset = struct.unpack_from("<I", data, 10)[0]
    header_size, width, height, planes, bpp = struct.unpack_from("<IiiHH", data, 14)
    assert header_size >= 40 and planes == 1 and bpp == 32, (path, header_size, bpp)
    assert width == abs(height) and width in SIZES, (path, width, height)
    rows = abs(height)
    stride = width * 4
    pixels = data[offset : offset + stride * rows]
    assert len(pixels) == stride * rows, path
    if height < 0:  # top-down; flip to bottom-up
        pixels = b"".join(pixels[i * stride : (i + 1) * stride] for i in reversed(range(rows)))
    return width, rows, pixels


def dib_entry(width: int, height: int, pixels: bytes) -> bytes:
    stride = width * 4
    mask_stride = ((width + 31) // 32) * 4
    mask = bytearray()
    for row in range(height):  # bottom-up, like the pixels
        line = bytearray(mask_stride)
        for x in range(width):
            alpha = pixels[row * stride + x * 4 + 3]
            if alpha < 128:
                line[x // 8] |= 0x80 >> (x % 8)
        mask += line
    header = struct.pack("<IiiHHIIiiII", 40, width, height * 2, 1, 32, 0, stride * height + len(mask), 0, 0, 0, 0)
    return header + pixels + bytes(mask)


def main(layer_dir: Path, out: Path) -> None:
    entries: list[tuple[int, bytes]] = []
    for size in SIZES:
        if size >= PNG_FROM:
            # Vista and later accept a PNG-compressed entry; it is a quarter of the DIB size.
            png = (layer_dir / f"icon_{size}.png").read_bytes()
            assert png[:8] == bytes.fromhex("89504e470d0a1a0a"), "not a PNG"
            entries.append((size, png))
            continue
        width, height, pixels = read_bmp32(layer_dir / f"icon_{size}.bmp")
        entries.append((width, dib_entry(width, height, pixels)))
    assert 0 < len(entries) <= MAX_LAYERS
    header = struct.pack("<HHH", 0, 1, len(entries))
    directory = bytearray()
    body = bytearray()
    offset = 6 + 16 * len(entries)
    for width, blob in entries:
        dim = 0 if width >= 256 else width
        directory += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(blob), offset + len(body))
        body += blob
    out.write_bytes(header + bytes(directory) + bytes(body))
    print(f"wrote {out} ({len(entries)} sizes, {out.stat().st_size} bytes)")


if __name__ == "__main__":
    assert len(sys.argv) == 3, "usage: make_icon.py <layer dir> <out.ico>"
    main(Path(sys.argv[1]), Path(sys.argv[2]))
