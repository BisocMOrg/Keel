#!/usr/bin/env python3
"""Strict, offline Intel HEX validation for STM32C011F4 main flash only."""
from pathlib import Path
import re
import sys

FLASH_BASE = 0x08000000
FLASH_END = 0x08004000


def inspect_hex(text):
    base, eof, entry = 0, False, None
    image = {}
    for number, line in enumerate(text.splitlines(), 1):
        record = line.strip()
        if not record:
            continue
        if eof:
            raise ValueError(f"line {number}: record after EOF")
        if not re.fullmatch(r":[0-9a-fA-F]+", record) or len(record[1:]) % 2:
            raise ValueError(f"line {number}: invalid Intel HEX characters or length")
        data = bytes.fromhex(record[1:])
        if len(data) < 5 or len(data) != data[0] + 5:
            raise ValueError(f"line {number}: record length does not match byte count")
        if sum(data) & 255:
            raise ValueError(f"line {number}: checksum error")
        count, address, kind = data[0], int.from_bytes(data[1:3], "big"), data[3]
        payload = data[4:-1]
        if kind == 0:
            if count == 0 or address + count > 0x10000:
                raise ValueError(f"line {number}: empty or wrapping data record")
            absolute = base + address
            if absolute < FLASH_BASE or absolute + count > FLASH_END:
                raise ValueError(f"line {number}: data outside 16 KB main flash")
            for offset, value in enumerate(payload):
                if absolute + offset in image:
                    raise ValueError(f"line {number}: overlapping data records")
                image[absolute + offset] = value
        elif kind == 1:
            if count != 0 or address != 0:
                raise ValueError(f"line {number}: malformed EOF")
            eof = True
        elif kind in (2, 4):
            if count != 2 or address != 0:
                raise ValueError(f"line {number}: malformed extended address")
            base = int.from_bytes(payload, "big") << (4 if kind == 2 else 16)
        elif kind == 5:
            if count != 4 or address != 0 or entry is not None:
                raise ValueError(f"line {number}: malformed or duplicate entry point")
            entry = int.from_bytes(payload, "big")
            if not FLASH_BASE <= entry < FLASH_END or not entry & 1:
                raise ValueError(f"line {number}: entry point is not Thumb main flash")
        else:
            raise ValueError(f"line {number}: unsupported record type {kind:#x}")
    if not eof:
        raise ValueError("missing EOF record")
    if not image:
        raise ValueError("no data records")
    if any(address not in image for address in range(FLASH_BASE, FLASH_BASE + 8)):
        raise ValueError("missing initial stack/reset vectors at main-flash base")
    return len(image), min(image), max(image) + 1


def main():
    if len(sys.argv) != 2:
        raise ValueError("usage: check_hex.py FIRMWARE.hex")
    count, low, high = inspect_hex(Path(sys.argv[1]).read_text(encoding="ascii"))
    print(f"image: {count} bytes, 0x{low:08x}..0x{high - 1:08x} end=0x{high:08x}")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, UnicodeError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
