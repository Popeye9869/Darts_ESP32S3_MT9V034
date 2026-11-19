#!/usr/bin/env python3
"""Run the MT9V034 frame-rate matrix over the USB serial console.

The firmware's `mode` command reprograms the sensor and `bench` measures
VSY / DMA / application frame counts.  This script drives the whole matrix
in one serial session (opening the port resets the board) and writes CSV.
"""
import argparse
import csv
import re
import sys
import time

import serial

MODES = [
    "full", "bin2", "bin4", "752x60", "376x240", "188x120",
    "80x60", "80x30", "80x15", "80x8", "80x4", "80x2", "80x1",
]


def read_until(ser: serial.Serial, needle: bytes, timeout: float) -> bytes:
    buf = bytearray()
    deadline = time.time() + timeout
    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            buf.extend(chunk)
            if needle in buf:
                break
        else:
            time.sleep(0.01)
    return bytes(buf)


def parse_result(text: str):
    m = re.search(r"^RESULT .*$", text, re.MULTILINE)
    if not m:
        return None
    out = {}
    for token in m.group(0).split()[1:]:
        if "=" in token:
            k, v = token.split("=", 1)
            out[k] = v
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM34")
    ap.add_argument("--seconds", type=int, default=3)
    ap.add_argument("--out", default="docs/frame_rate_results.csv")
    ap.add_argument("--modes", nargs="*", default=MODES)
    args = ap.parse_args()

    ser = serial.Serial(args.port, 115200, timeout=0.05)
    ser.write(b"\n")
    ser.flush()
    boot = read_until(ser, b"mt9v> ", 15)
    if b"mt9v> " not in boot:
        print(f"no REPL prompt: {boot[-300:]!r}", file=sys.stderr)
        return 1

    rows = []
    for mode in args.modes:
        ser.write(f"mode {mode}\n".encode())
        ser.flush()
        out = read_until(ser, b"mt9v> ", 5)
        if b"MODE apply failed" in out:
            print(f"[{mode}] apply failed: {out[-200:]!r}", file=sys.stderr)
            continue
        ser.write(b"capstart\n")
        ser.flush()
        read_until(ser, b"mt9v> ", 5)
        ser.write(f"bench {args.seconds}\n".encode())
        ser.flush()
        out = read_until(ser, b"mt9v> ", args.seconds + 10)
        ser.write(b"capstop\n")
        ser.flush()
        read_until(ser, b"mt9v> ", 5)

        row = parse_result(out.decode("utf-8", "replace"))
        if not row:
            print(f"[{mode}] no RESULT: {out[-300:]!r}", file=sys.stderr)
            continue
        row["mode"] = mode
        rows.append(row)
        print(f"{mode:>10}  w={row.get('w')} h={row.get('h')}  "
              f"vsy={row.get('vsy_ring')} fps  dma={row.get('dma_fps')}  "
              f"app={row.get('app_fps')}  drops={row.get('drops')}")

    ser.close()

    if rows:
        keys = ["mode", "w", "h", "hblank", "vblank", "row_bin", "exp",
                "vsy_fps", "vsy_ring", "dma_fps", "app_fps", "calc_fps",
                "vsy_edges", "dma_frames", "app_frames", "drops", "ms"]
        with open(args.out, "w", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(f, fieldnames=keys, extrasaction="ignore")
            w.writeheader()
            w.writerows(rows)
        print(f"wrote {args.out} ({len(rows)} rows)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
