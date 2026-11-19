#!/usr/bin/env python3
"""Plot the MT9V034 frame-rate matrix from docs/frame_rate_results.csv."""
import csv
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parents[1]
CSV = ROOT / "docs" / "frame_rate_results.csv"
OUT = ROOT / "docs" / "images" / "frame_rate_results.png"


def main() -> int:
    rows = []
    with CSV.open(encoding="utf-8") as f:
        for row in csv.DictReader(f):
            rows.append(row)

    heights = [int(r["h"]) for r in rows]
    fps = [float(r["vsy_ring"]) for r in rows]

    fig, ax = plt.subplots(figsize=(9, 5.5), dpi=160)
    ax.loglog(heights, fps, "o-", color="#1565c0", linewidth=1.8, markersize=6)

    key = {"full", "bin2", "bin4", "752x60", "80x60", "80x30", "80x1"}
    offsets = {
        "80x60": (8, 8),
        "752x60": (8, -14),
        "full": (-10, 12),
    }
    for r in rows:
        if r["mode"] not in key:
            continue
        h = int(r["h"])
        y = float(r["vsy_ring"])
        label = f'{r["mode"]}  {y:.0f} fps'
        dx, dy = offsets.get(r["mode"], (7, 6))
        ha = "right" if r["mode"] == "full" else "left"
        ax.annotate(label, (h, y), textcoords="offset points", xytext=(dx, dy),
                    fontsize=8, color="#263238", ha=ha)

    ax.set_xlabel("Output frame height (rows, log scale)")
    ax.set_ylabel("Measured frame rate (fps, log scale)")
    ax.set_title("MT9V034 + ESP32-S3 N16R8: frame rate vs. output height\n"
                 "HBlank = max(61, 690 - width), VBlank = 4, AEC/AGC off")
    ax.grid(True, which="both", alpha=0.3)
    ax.set_xlim(0.7, 1500)
    ax.set_ylim(50, 12000)
    ax.axhline(611.4, color="#e53935", linestyle=":", linewidth=0.8, alpha=0.35)
    ax.axhline(68.6, color="#e53935", linestyle=":", linewidth=0.8, alpha=0.35)

    fig.tight_layout()
    OUT.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(OUT)
    print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
