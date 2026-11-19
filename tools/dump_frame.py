#!/usr/bin/env python3
"""Capture one raw 8-bit frame from the MT9V034 tester over USB serial.

Usage:
  python tools/dump_frame.py --port COM34 --out docs/images/frame.png

The firmware prints a text header ``DUMP_BEGIN <w> <h> <bytes>`` followed by
exactly ``bytes`` raw pixels, then ``DUMP_END``.
"""
import argparse
import re
import sys
import time

import serial
from PIL import Image, ImageOps


def read_until_header(ser: serial.Serial, timeout: float) -> bytes:
    buf = bytearray()
    deadline = time.time() + timeout
    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            buf.extend(chunk)
            m = re.search(rb"DUMP_BEGIN (\d+) (\d+) (\d+)\r?\n", buf)
            if m:
                return bytes(buf[m.end():]), tuple(int(x) for x in m.groups())
        else:
            time.sleep(0.01)
    raise TimeoutError(f"no DUMP_BEGIN header, got {bytes(buf[:200])!r}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM34")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--out", default="docs/images/frame.png")
    ap.add_argument("--raw", default=None)
    ap.add_argument("--timeout", type=float, default=10.0)
    ap.add_argument("--pre", action="append", default=[],
                    help="command to run after the prompt before dumping")
    args = ap.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=0.05)
    # Ask the REPL for a fresh prompt (the old one may have been lost when
    # the previous serial session closed).
    ser.write(b"\n")
    ser.flush()
    # Wait for the REPL prompt.
    deadline = time.time() + 10
    boot = bytearray()
    while time.time() < deadline:
        boot.extend(ser.read(4096))
        if b"mt9v> " in boot:
            break

    for cmd in args.pre:
        ser.write((cmd + "\n").encode())
        ser.flush()
        time.sleep(1.0)
        ser.read(8192)

    ser.write(b"dump\n")
    ser.flush()

    tail, (w, h, nbytes) = read_until_header(ser, args.timeout)
    data = bytearray(tail)
    while len(data) < nbytes:
        chunk = ser.read(nbytes - len(data))
        if chunk:
            data.extend(chunk)
        else:
            time.sleep(0.005)
    data = bytes(data[:nbytes])

    # Drain the trailer.
    trailer = bytearray()
    deadline = time.time() + 2
    while time.time() < deadline and b"DUMP_END" not in trailer:
        trailer.extend(ser.read(4096))
    ser.close()

    usable = data[: w * h]
    path = args.out
    if args.raw:
        with open(args.raw, "wb") as f:
            f.write(usable)
    img = Image.frombytes("L", (w, h), usable)
    ImageOps.autocontrast(img).save(path)
    vals = list(usable)
    print(f"saved {path}: {w}x{h}, bytes={nbytes}, min={min(vals)}, max={max(vals)}, "
          f"mean={sum(vals)/len(vals):.1f}")
    print(f"trailer={bytes(trailer[-32:])!r}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
