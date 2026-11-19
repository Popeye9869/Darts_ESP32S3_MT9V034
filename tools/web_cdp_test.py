#!/usr/bin/env python3
"""Drive headless Edge over CDP to verify the ESP32 web UI.

Launch Edge first (the script expects an existing debug port):
  msedge --headless=new --remote-debugging-port=9222 --user-data-dir=%TEMP%\\edge-cdp about:blank
Then:
  python tools/web_cdp_test.py --url http://192.168.31.85/
"""
import argparse
import asyncio
import base64
import json
import time
import urllib.request

import websockets


async def main_async(args):
    targets = json.load(urllib.request.urlopen("http://127.0.0.1:9222/json"))
    page = next(t for t in targets if t["type"] == "page")
    async with websockets.connect(page["webSocketDebuggerUrl"], max_size=8 * 1024 * 1024) as ws:
        ident = 0

        async def send(method, params=None):
            nonlocal ident
            ident += 1
            await ws.send(json.dumps({"id": ident, "method": method, "params": params or {}}))
            while True:
                msg = json.loads(await ws.recv())
                if msg.get("id") == ident:
                    return msg

        await send("Page.enable")
        await send("Runtime.enable")
        await send("Emulation.setDeviceMetricsOverride",
                   {"width": args.width, "height": args.height,
                    "deviceScaleFactor": 1, "mobile": False})
        await send("Page.navigate", {"url": args.url})
        await asyncio.sleep(2.0)
        if args.preset:
            expr = ("[...document.querySelectorAll('#presets button')]"
                    f".find(b => b.textContent === '{args.preset}')?.click()")
            await send("Runtime.evaluate", {"expression": expr})
        await asyncio.sleep(args.wait)
        for expr in args.eval:
            res = await send("Runtime.evaluate", {"expression": expr, "returnByValue": True})
            print("eval", expr, "->", res.get("result", {}).get("result", {}).get("value"))
            await asyncio.sleep(args.eval_wait)

        expr = r"""JSON.stringify({
          ready: (typeof ws !== 'undefined' && ws) ? ws.readyState : -1,
          mode: document.getElementById('s-mode').textContent,
          presets: document.querySelectorAll('#presets button').length,
          seq: document.getElementById('s-seq').textContent,
          canvas: view.width + 'x' + view.height,
          conn: document.getElementById('conn-dot').className,
          status: lastStatus,
          log: document.getElementById('log').textContent.slice(-300)
        })"""
        res = await send("Runtime.evaluate", {"expression": expr, "returnByValue": True})
        value = res.get("result", {}).get("result", {}).get("value")
        print(value)
        if args.screenshot:
            shot = await send("Page.captureScreenshot", {"format": "png"})
            data = shot.get("result", {}).get("data")
            if data:
                with open(args.screenshot, "wb") as f:
                    f.write(base64.b64decode(data))
                print("screenshot", args.screenshot)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://192.168.31.85/")
    ap.add_argument("--wait", type=float, default=6.0)
    ap.add_argument("--width", type=int, default=1440)
    ap.add_argument("--height", type=int, default=900)
    ap.add_argument("--preset", default="")
    ap.add_argument("--eval", action="append", default=[])
    ap.add_argument("--eval-wait", type=float, default=1.5)
    ap.add_argument("--screenshot", default="docs/images/web_ui.png")
    args = ap.parse_args()
    asyncio.run(main_async(args))


if __name__ == "__main__":
    main()
