#!/usr/bin/env python3
"""Tiny interactive helper for the MT9V034 ESP32-S3 tester.

Opens the USB Serial/JTAG console, waits for the REPL prompt, then sends
each command and prints everything received.
"""
import argparse
import sys
import time

import serial


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM34")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--cmd", action="append", default=[])
    ap.add_argument("--delay", type=float, default=1.0)
    ap.add_argument("--wait", type=float, default=15.0)
    ap.add_argument("--boot-log", action="store_true")
    args = ap.parse_args()

    s = serial.Serial(args.port, args.baud, timeout=0.1)
    start = time.time()
    buf = b""

    while time.time() - start < args.wait:
        buf += s.read(8192)
        if b"mt9v>" in buf:
            break

    if args.boot_log:
        sys.stdout.buffer.write(buf)
        sys.stdout.buffer.flush()

    for cmd in args.cmd:
        s.write((cmd + "\n").encode())
        s.flush()
        t0 = time.time()
        out = b""
        while time.time() - t0 < args.delay:
            out += s.read(8192)
            time.sleep(0.02)
        sys.stdout.buffer.write(out)
        sys.stdout.buffer.flush()

    s.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
