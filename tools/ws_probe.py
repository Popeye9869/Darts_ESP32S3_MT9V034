#!/usr/bin/env python3
"""Measure the ESP32 MT9V034 WebSocket stream and save one frame.

Example:
  python tools/ws_probe.py --host 192.168.31.85 --seconds 5 --div 1 --save frame.png
"""
import argparse
import asyncio
import json
import struct
import time
from pathlib import Path

import websockets
from PIL import Image, ImageOps


async def run(args):
    uri = f"ws://{args.host}/ws"
    frames = 0
    last = None
    last_status = {}
    t0 = None
    first_t = None

    async with websockets.connect(uri, max_size=2 * 1024 * 1024) as ws:
        for cmd in args.pre:
            await ws.send(cmd)
        await ws.send("hello")
        if args.mode:
            await ws.send(f"mode {args.mode}")
        await ws.send("stream on")
        await ws.send(f"stream div {args.div}")
        await ws.send(f"stream pack {args.pack}")
        await ws.send(f"stream fps {args.fps}")

        deadline = time.time() + args.seconds
        while time.time() < deadline:
            try:
                msg = await asyncio.wait_for(ws.recv(), timeout=1.0)
            except asyncio.TimeoutError:
                continue
            if isinstance(msg, str):
                try:
                    obj = json.loads(msg)
                except json.JSONDecodeError:
                    continue
                if obj.get("type") == "status":
                    last_status = obj
                    print(f"status mode={obj['mode']} {obj['w']}x{obj['h']} "
                          f"cap={obj['vsy']:.1f} dma={obj['dma']:.1f} app={obj['app']:.1f} "
                          f"drops={obj['drops']} stream={obj['sfps']:.1f} "
                          f"sdrop={obj['sdropped']} rssi={obj['rssi']}")
            else:
                if len(msg) < 16:
                    continue
                magic, ver, flags, w, h, seq, ts = struct.unpack_from("<2sBBHHII", msg, 0)
                if magic != b"MT":
                    continue
                packed = (flags & 1) != 0
                need = (w * h + 1) // 2 if packed else w * h
                if len(msg) < 16 + need:
                    continue
                if packed:
                    raw = msg[16:16 + need]
                    pixels = bytearray(w * h)
                    for i in range(w * h):
                        pixels[i] = ((raw[i >> 1] >> (0 if (i & 1) else 4)) & 0xF) * 17
                    data = bytes(pixels)
                else:
                    data = msg[16:16 + need]
                if first_t is None:
                    first_t = time.time()
                frames += 1
                last = (w, h, seq, data)
                if args.verbose and frames % 30 == 0:
                    print(f"frame {frames} {w}x{h} seq={seq} bytes={len(msg)}")

        wall = time.time() - (first_t or time.time())
        fps = frames / wall if wall > 0 else 0
        if last:
            w, h, seq, data = last
            img = Image.frombytes("L", (w, h), data)
            out = Path(args.save)
            out.parent.mkdir(parents=True, exist_ok=True)
            ImageOps.autocontrast(img).save(out)
            print(f"last frame {w}x{h} seq={seq}, saved {out}")
        print(f"RESULT frames={frames} wall={wall:.2f}s ws_fps={fps:.1f} "
              f"capture_fps={last_status.get('vsy')} stream_fps={last_status.get('sfps')} "
              f"capture_drops={last_status.get('drops')}")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.31.85")
    ap.add_argument("--seconds", type=float, default=5.0)
    ap.add_argument("--div", type=int, default=1)
    ap.add_argument("--pack", type=int, default=4, choices=[4, 8])
    ap.add_argument("--fps", type=int, default=0)
    ap.add_argument("--mode", default="80x60")
    ap.add_argument("--save", default="docs/images/web_frame.png")
    ap.add_argument("--pre", action="append", default=[])
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    return asyncio.run(run(args))


if __name__ == "__main__":
    raise SystemExit(main())
