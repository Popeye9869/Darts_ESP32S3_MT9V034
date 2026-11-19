#!/usr/bin/env python3
"""Generate the wiring and frame-timing diagrams used by the test plan.

Requires Pillow. Chinese text uses Microsoft YaHei on Windows.
"""
from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
IMG_DIR = ROOT / "images"
IMG_DIR.mkdir(parents=True, exist_ok=True)

FONT_CN = "C:/Windows/Fonts/msyh.ttc"
FONT_CN_BOLD = "C:/Windows/Fonts/msyhbd.ttc"
FONT_MONO = "C:/Windows/Fonts/consola.ttf"

INK = "#0F172A"
MUTED = "#475569"
LINE = "#CBD5E1"
CARD = "#FFFFFF"
BG = "#F7F9FC"
PANEL = "#F8FAFC"

RED = "#DC2626"
BLACK = "#111827"
BLUE = "#2563EB"
ORANGE = "#EA580C"
GREEN = "#16A34A"
PURPLE = "#7C3AED"
GRAY = "#94A3B8"


def font(path: str, size: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(path, size)


def text(draw: ImageDraw.ImageDraw, xy, s, f, fill=INK, anchor="la"):
    draw.text(xy, s, font=f, fill=fill, anchor=anchor)


def rounded(draw, box, radius=16, fill=CARD, outline=LINE, width=2):
    draw.rounded_rectangle(box, radius=radius, fill=fill, outline=outline, width=width)


def dashed(draw, xy, fill, width=3, dash=12, gap=9):
    x1, y1, x2, y2 = xy
    if y1 == y2:
        x = x1
        while x < x2:
            draw.line((x, y1, min(x + dash, x2), y2), fill=fill, width=width)
            x += dash + gap
    else:
        y = y1
        while y < y2:
            draw.line((x1, y, x2, min(y + dash, y2)), fill=fill, width=width)
            y += dash + gap


def draw_wiring():
    W, H = 2200, 1400
    im = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(im)

    f_title = font(FONT_CN_BOLD, 48)
    f_sub = font(FONT_CN, 24)
    f_card = font(FONT_CN_BOLD, 30)
    f_card_sub = font(FONT_CN, 20)
    f_pin = font(FONT_MONO, 21)
    f_name = font(FONT_CN, 21)
    f_label = font(FONT_CN, 20)
    f_note = font(FONT_CN, 18)
    f_small = font(FONT_CN, 17)

    text(d, (W // 2, 52), "ESP32-S3 与 MT9V034 接线图（8-bit 并行 DVP）", f_title, INK, "mm")
    text(
        d,
        (W // 2, 108),
        "规格内设计：752×480 全局快门，26.667 MHz PCLK；40 MHz 超频见文档",
        f_sub,
        MUTED,
        "mm",
    )

    # Cards
    rounded(d, (50, 160, 670, 1180), 24, CARD, LINE, 3)
    rounded(d, (1530, 160, 2150, 1180), 24, CARD, LINE, 3)
    text(d, (360, 205), "MT9V034 模块 P1（22-pin）", f_card, INK, "mm")
    text(d, (360, 250), "3.3 V · 10-bit 并行 · 全局快门", f_card_sub, MUTED, "mm")
    text(d, (1840, 205), "ESP32-S3 DevKitC-1（N16R8）", f_card, INK, "mm")
    text(d, (1840, 250), "LCD_CAM 8-bit DVP + GDMA", f_card_sub, MUTED, "mm")

    # Connector panels
    rounded(d, (90, 290, 630, 1110), 14, PANEL, "#94A3B8", 2)
    rounded(d, (1570, 290, 2110, 1110), 14, PANEL, "#94A3B8", 2)

    rows = {
        "VCC": 340,
        "GND": 374,
        "SCL": 408,
        "SDA": 442,
        "VS": 476,
        "HS": 510,
        "PCLK": 544,
        "XCLK": 578,
        "D9": 612,
        "D8": 646,
        "D7": 680,
        "D6": 714,
        "D5": 748,
        "D4": 782,
        "D3": 816,
        "D2": 850,
        "D1": 884,
        "D0": 918,
        "RST": 952,
        "PDN": 986,
        "LED": 1020,
        "TRIG": 1054,
    }
    pin_no = {name: i + 1 for i, name in enumerate(rows)}
    pin_color = {
        "VCC": RED,
        "GND": BLACK,
        "SCL": BLUE,
        "SDA": BLUE,
        "VS": ORANGE,
        "HS": ORANGE,
        "PCLK": GREEN,
        "XCLK": GREEN,
        "D9": PURPLE,
        "D8": PURPLE,
        "D7": PURPLE,
        "D6": PURPLE,
        "D5": PURPLE,
        "D4": PURPLE,
        "D3": PURPLE,
        "D2": PURPLE,
        "D1": GRAY,
        "D0": GRAY,
        "RST": RED,
        "PDN": RED,
        "LED": GRAY,
        "TRIG": GRAY,
    }
    pin_label = {
        "VCC": "VCC  (3.3 V)",
        "GND": "GND",
        "SCL": "SCL  (SCLK)",
        "SDA": "SDA  (SDATA)",
        "VS": "VS  (FRAME_VALID)",
        "HS": "HS  (LINE_VALID)",
        "PCLK": "PCLK",
        "XCLK": "XCLK  (SYSCLK)",
        "D9": "D9  (DOUT9, MSB)",
        "D8": "D8  (DOUT8)",
        "D7": "D7  (DOUT7)",
        "D6": "D6  (DOUT6)",
        "D5": "D5  (DOUT5)",
        "D4": "D4  (DOUT4)",
        "D3": "D3  (DOUT3)",
        "D2": "D2  (DOUT2, 8-bit LSB)",
        "D1": "D1  (DOUT1, 10-bit only)",
        "D0": "D0  (DOUT0, 10-bit only)",
        "RST": "RST  (RESET_BAR)",
        "PDN": "PDN  (STANDBY)",
        "LED": "LED  (LED_OUT)",
        "TRIG": "TRIG  (EXPOSURE)",
    }
    for name, y in rows.items():
        color = pin_color[name]
        d.ellipse((112, y - 13, 138, y + 13), fill="#E2E8F0", outline=LINE, width=1)
        text(d, (125, y), str(pin_no[name]), font(FONT_MONO, 17), INK, "mm")
        text(d, (150, y), pin_label[name], f_name, color, "lm")
        # pin stub to connector edge
        d.line((600, y, 630, y), fill=color, width=3)

    # Right-side labels
    right_rows = {
        "3V3": 340,
        "GND": 374,
        "GPIO5": 408,
        "GPIO4": 442,
        "GPIO6": 476,
        "GPIO7": 510,
        "GPIO13": 544,
        "GPIO15": 578,
        "GPIO21": 952,
        "GPIO14": 986,
        "GPIO2": 1020,
        "GPIO1": 1054,
    }
    right_text = {
        "3V3": "3V3",
        "GND": "GND",
        "GPIO5": "GPIO5   (I2C SCL)",
        "GPIO4": "GPIO4   (I2C SDA)",
        "GPIO6": "GPIO6   (VSYNC)",
        "GPIO7": "GPIO7   (HREF / DE)",
        "GPIO13": "GPIO13  (PCLK)",
        "GPIO15": "GPIO15  (XCLK out)",
        "GPIO21": "GPIO21  (RESET)",
        "GPIO14": "GPIO14  (PWDN)",
        "GPIO2": "GPIO2   (LED, opt.)",
        "GPIO1": "GPIO1   (TRIG, opt.)",
    }
    for name, y in right_rows.items():
        color = {
            "3V3": RED,
            "GND": BLACK,
            "GPIO5": BLUE,
            "GPIO4": BLUE,
            "GPIO6": ORANGE,
            "GPIO7": ORANGE,
            "GPIO13": GREEN,
            "GPIO15": GREEN,
            "GPIO21": RED,
            "GPIO14": RED,
            "GPIO2": GRAY,
            "GPIO1": GRAY,
        }[name]
        d.ellipse((1560, y - 9, 1578, y + 9), fill=color, outline=color)
        text(d, (1595, y), right_text[name], f_name, color, "lm")

    # Data block on the right
    rounded(d, (1585, 598, 2095, 866), 10, "#F5F3FF", "#C4B5FD", 2)
    text(d, (1600, 610), "8-bit 数据总线", f_small, PURPLE, "la")
    data_map = [
        ("GPIO16", "DOUT9 / D7"),
        ("GPIO17", "DOUT8 / D6"),
        ("GPIO18", "DOUT7 / D5"),
        ("GPIO12", "DOUT6 / D4"),
        ("GPIO10", "DOUT5 / D3"),
        ("GPIO8", "DOUT4 / D2"),
        ("GPIO9", "DOUT3 / D1"),
        ("GPIO11", "DOUT2 / D0 (8-bit LSB)"),
    ]
    for i, (gpio, sig) in enumerate(data_map):
        y = 646 + i * 28
        d.ellipse((1560, y - 7, 1576, y + 7), fill=PURPLE, outline=PURPLE)
        text(d, (1598, y), f"{gpio}  ←  {sig}", font(FONT_MONO, 18), INK, "lm")

    # Horizontal connections
    def hline(y, color, width=3, dashed_line=False):
        if dashed_line:
            dashed(d, (630, y, 1570, y), color, width, 14, 10)
        else:
            d.line((630, y, 1570, y), fill=color, width=width)

    hline(rows["VCC"], RED, 6)
    hline(rows["GND"], BLACK, 6)
    hline(rows["SCL"], BLUE, 3)
    hline(rows["SDA"], BLUE, 3)
    hline(rows["VS"], ORANGE, 3)
    hline(rows["HS"], ORANGE, 3)
    hline(rows["PCLK"], GREEN, 4)
    hline(rows["XCLK"], GREEN, 4)
    hline(rows["RST"], RED, 3)
    hline(rows["PDN"], RED, 3)
    hline(rows["LED"], GRAY, 2, True)
    hline(rows["TRIG"], GRAY, 2, True)

    # Data bus: left/right vertical bus bars + one thick trunk + label box
    d.line((630, rows["D9"], 630, rows["D2"]), fill=PURPLE, width=5)
    d.line((1570, rows["D9"], 1570, rows["D2"]), fill=PURPLE, width=5)
    for name in ("D9", "D8", "D7", "D6", "D5", "D4", "D3", "D2"):
        d.line((1570, rows[name], 1585, rows[name]), fill=PURPLE, width=3)
    d.line((630, 731, 1570, 731), fill=PURPLE, width=18)
    label_box = (770, 706, 1430, 756)
    rounded(d, label_box, 12, CARD, PURPLE, 2)
    text(
        d,
        (1100, 731),
        "D[9:2] → GPIO16,17,18,12,10,8,9,11",
        font(FONT_MONO, 19),
        PURPLE,
        "mm",
    )

    # NC for DOUT1/DOUT0
    for y in (rows["D1"], rows["D0"]):
        dashed(d, (630, y, 780, y), GRAY, 2, 10, 8)
        d.line((780, y - 12, 804, y + 12), fill=GRAY, width=3)
        d.line((780, y + 12, 804, y - 12), fill=GRAY, width=3)
    text(d, (820, 901), "NC（8-bit 模式不使用）", f_label, GRAY, "lm")

    # Inline labels
    inline = [
        (rows["VCC"] - 17, "3.3 V", RED),
        (rows["GND"] - 17, "GND", BLACK),
        (rows["SCL"] - 17, "I2C SCL · 400 kHz", BLUE),
        (rows["SDA"] - 17, "I2C SDA", BLUE),
        (rows["VS"] - 17, "FRAME_VALID", ORANGE),
        (rows["HS"] - 17, "LINE_VALID", ORANGE),
        (rows["PCLK"] - 17, "PCLK 26.667 MHz", GREEN),
        (rows["XCLK"] - 17, "XCLK 26.667 MHz（S3 CAM_CLK，div=6）", GREEN),
        (rows["RST"] - 17, "RESET_BAR · 10 kΩ 下拉", RED),
        (rows["PDN"] - 17, "STANDBY · 10 kΩ 下拉", RED),
        (rows["LED"] - 17, "LED_OUT（可选）", GRAY),
        (rows["TRIG"] - 17, "TRIG / EXPOSURE（可选）", GRAY),
    ]
    for y, s, color in inline:
        # clear a small strip so text is readable over the line
        d.rectangle((905, y - 14, 1310, y + 14), fill=BG)
        text(d, (1100, y), s, f_label, color, "mm")

    # Notes box
    rounded(d, (690, 1110, 1510, 1210), 14, "#EFF6FF", "#93C5FD", 2)
    text(d, (710, 1126), "I2C：1.5 kΩ 上拉到 3.3 V；7-bit 地址 0x48（S_CTRL_ADR0/1 = 0）。", f_note, INK)
    text(d, (710, 1156), "上电：3.3 V → XCLK → 释放 RESET（≥750 ns）→ 10 个 XCLK → I2C。", f_note, INK)
    text(d, (710, 1186), "RESET_BAR 10 kΩ 下拉；STANDBY 10 kΩ 下拉；OE 10 kΩ 上拉。", f_note, INK)

    # Footer notes
    text(
        d,
        (W // 2, 1255),
        "8-bit 模式只接 DOUT9..DOUT2；DOUT1/DOUT0 悬空。传感器仍输出 10-bit，S3 取高 8 位（损失 2 LSB）。",
        f_small,
        MUTED,
        "ma",
    )
    text(
        d,
        (W // 2, 1288),
        "27–40 MHz 下不建议杜邦线：走线 <5 cm、数据等长 ±5 mm、源端 33 Ω 串阻、完整地平面。",
        f_small,
        MUTED,
        "ma",
    )
    text(
        d,
        (W // 2, 1321),
        "模块若为 2.8 V I/O 或自带稳压，先核对原理图；所有 I/O 不得超过 3.3 V。",
        f_small,
        MUTED,
        "ma",
    )

    im.save(IMG_DIR / "esp32s3_mt9v034_wiring.png", optimize=True)
    print(IMG_DIR / "esp32s3_mt9v034_wiring.png")


def draw_timing():
    W, H = 2000, 1100
    im = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(im)
    f_title = font(FONT_CN_BOLD, 42)
    f_sub = font(FONT_CN, 22)
    f_label = font(FONT_CN_BOLD, 24)
    f_note = font(FONT_CN, 19)
    f_mono = font(FONT_MONO, 19)
    f_small = font(FONT_CN, 17)

    text(d, (W // 2, 45), "MT9V034 帧时序与帧率计算", f_title, INK, "mm")
    text(d, (W // 2, 95), "非按比例；一个 PCLK 输出一个 10-bit 像素，LV 高期间数据有效", f_sub, MUTED, "mm")

    x0, x1, x2 = 220, 1120, 1260
    y_fv = 235

    # FV: active high, then vertical blanking low
    text(d, (80, y_fv), "FV / VSYNC", f_label, ORANGE, "lm")
    d.line((x0, y_fv + 38, x0, y_fv - 38), fill=ORANGE, width=4)
    d.line((x0, y_fv - 38, x1, y_fv - 38), fill=ORANGE, width=4)
    d.line((x1, y_fv - 38, x1, y_fv + 38), fill=ORANGE, width=4)
    d.line((x1, y_fv + 38, x2, y_fv + 38), fill=ORANGE, width=4)
    d.line((x2, y_fv + 38, x2, y_fv - 38), fill=ORANGE, width=4)

    # active frame bracket
    d.line((x0, y_fv - 72, x1, y_fv - 72), fill=ORANGE, width=2)
    d.line((x0, y_fv - 82, x0, y_fv - 62), fill=ORANGE, width=2)
    d.line((x1, y_fv - 82, x1, y_fv - 62), fill=ORANGE, width=2)
    text(d, ((x0 + x1) // 2, y_fv - 88), "有效帧 = N_rows × T_row", f_note, ORANGE, "mb")
    # vblank bracket
    d.line((x1, y_fv - 72, x2, y_fv - 72), fill=ORANGE, width=2)
    d.line((x2, y_fv - 82, x2, y_fv - 62), fill=ORANGE, width=2)
    text(d, ((x1 + x2) // 2, y_fv - 88), "VBlank = R0x06 × T_row", f_note, ORANGE, "mb")

    # LV: two pulses, ellipsis, last pulse
    y_lv = 420
    text(d, (80, y_lv), "LV / HREF", f_label, BLUE, "lm")
    hi, lo = 112, 36
    # first high + low
    d.line((x0, y_lv + 34, x0, y_lv - 34), fill=BLUE, width=4)
    d.line((x0, y_lv - 34, x0 + hi, y_lv - 34), fill=BLUE, width=4)
    d.line((x0 + hi, y_lv - 34, x0 + hi, y_lv + 34), fill=BLUE, width=4)
    d.line((x0 + hi, y_lv + 34, x0 + hi + lo, y_lv + 34), fill=BLUE, width=4)
    # second high
    d.line((x0 + hi + lo, y_lv + 34, x0 + hi + lo, y_lv - 34), fill=BLUE, width=4)
    d.line((x0 + hi + lo, y_lv - 34, x0 + 2 * hi + lo, y_lv - 34), fill=BLUE, width=4)
    d.line((x0 + 2 * hi + lo, y_lv - 34, x0 + 2 * hi + lo, y_lv + 34), fill=BLUE, width=4)
    d.line((x0 + 2 * hi + lo, y_lv + 34, x0 + 2 * hi + 2 * lo, y_lv + 34), fill=BLUE, width=4)
    # ellipsis
    text(d, (x0 + 2 * hi + 2 * lo + 25, y_lv), ". . .", font(FONT_MONO, 34), BLUE, "lm")

    # last pulse near the end of active frame
    lx = 930
    d.line((lx, y_lv + 34, lx, y_lv - 34), fill=BLUE, width=4)
    d.line((lx, y_lv - 34, lx + hi, y_lv - 34), fill=BLUE, width=4)
    d.line((lx + hi, y_lv - 34, lx + hi, y_lv + 34), fill=BLUE, width=4)
    d.line((lx + hi, y_lv + 34, lx + hi + lo, y_lv + 34), fill=BLUE, width=4)
    d.line((lx + hi + lo, y_lv + 34, lx + hi + lo, y_lv - 34), fill=BLUE, width=4)
    d.line((lx + hi + lo, y_lv - 34, x1, y_lv - 34), fill=BLUE, width=4)

    # row bracket
    d.line((x0, y_lv + 70, x0 + hi + lo, y_lv + 70), fill=BLUE, width=2)
    d.line((x0, y_lv + 60, x0, y_lv + 80), fill=BLUE, width=2)
    d.line((x0 + hi + lo, y_lv + 60, x0 + hi + lo, y_lv + 80), fill=BLUE, width=2)
    text(d, (x0 + (hi + lo) // 2, y_lv + 84), "T_row = (R0x04 + R0x05) / f_PCLK", f_note, BLUE, "ma")
    text(d, (x0 + hi // 2, y_lv - 48), "752 PCLK", f_mono, BLUE, "ma")
    # Put the short-blanking label on a second row with a leader so the two labels
    # do not read as one string.
    text(d, (x0 + hi + lo // 2 + 6, y_lv - 92), "61 PCLK", f_mono, BLUE, "ma")
    d.line((x0 + hi + lo // 2, y_lv - 76, x0 + hi + lo // 2, y_lv - 22), fill=BLUE, width=1)

    # PCLK
    y_p = 610
    text(d, (80, y_p), "PCLK", f_label, GREEN, "lm")
    px, period, amp = x0, 16, 30
    d.line((x0, y_p + amp, x0, y_p - amp), fill=GREEN, width=3)
    for i in range(0, 40):
        xx = px + i * period
        if i % 2 == 0:
            d.line((xx, y_p - amp, xx + period, y_p - amp), fill=GREEN, width=3)
        else:
            d.line((xx, y_p + amp, xx + period, y_p + amp), fill=GREEN, width=3)
        d.line((xx, y_p - amp, xx, y_p + amp), fill=GREEN, width=3)
    text(d, (x0, y_p - 58), "连续输出，LV 高时每个上升沿采样", f_note, MUTED, "la")
    text(d, (x0 + 40 * period + 20, y_p), "26.667 MHz（37.5 ns）", f_note, GREEN, "lm")

    # DOUT
    y_d = 770
    text(d, (80, y_d), "DOUT[9:0]", f_label, PURPLE, "lm")
    for start, width in [(x0, 2 * hi + lo), (lx, hi)]:
        d.rectangle((start, y_d - 32, start + width, y_d + 32), fill="#EDE9FE", outline=PURPLE, width=2)
        text(d, (start + width // 2, y_d), "D9..D0", font(FONT_MONO, 19), PURPLE, "mm")
    text(d, (x0 + 2 * hi + 2 * lo + 25, y_d), ". . .", font(FONT_MONO, 34), PURPLE, "lm")
    text(d, (x0, y_d + 52), "每个 PCLK 一个 10-bit 像素；S3 只接 D9..D2，得到 8-bit 灰度。", f_note, MUTED, "la")

    # Equations box
    rounded(d, (1320, 150, 1950, 430), 16, "#FFFFFF", LINE, 2)
    text(d, (1345, 170), "计算式（默认/最小消隐均适用）", font(FONT_CN_BOLD, 22), INK, "la")
    eqs = [
        "T_row   = (R0x04 + R0x05) / f_PCLK",
        "T_frame = ((N_rows + R0x06) × T_row)",
        "          + 4 / f_PCLK",
        "fps     = 1 / T_frame",
        "N_rows  = R0x03 / row_bin  (1/2/4)",
        "f_PCLK  = f_XCLK = 26.667 MHz",
    ]
    for i, s in enumerate(eqs):
        text(d, (1345, 208 + i * 30), s, f_mono, INK, "la")

    # Examples box
    rounded(d, (1320, 460, 1950, 860), 16, "#FFFFFF", LINE, 2)
    text(d, (1345, 480), "示例（26.667 MHz，最小消隐）", font(FONT_CN_BOLD, 22), INK, "la")
    vals = [
        "752×480，R0x05=61，R0x06=4   → 67.8 fps",
        "752×240（row bin 2）         → 134.4 fps",
        "752×120（row bin 4）         → 264.5 fps",
        "752×60（窗口）               → 512.5 fps",
        "默认 752×480，R0x05=94，R0x06=45 → 60.0 fps",
    ]
    for i, s in enumerate(vals):
        text(d, (1345, 525 + i * 42), s, f_note, INK, "la")
    text(d, (1345, 738), "40 MHz 超频（非规格）：752×480 V=45 → 93.7 fps；详见 8.4", f_small, RED, "la")
    text(d, (1345, 790), "行消隐 61、场消隐 4 为数据手册最小值；", f_small, MUTED, "la")
    text(d, (1345, 815), "实际以寄存器读回和示波器测量为准。", f_small, MUTED, "la")

    text(
        d,
        (W // 2, 1015),
        "以上为传感器读出上限的理论值；系统实际上限还受 S3 DMA、PSRAM 带宽、缓存和 CPU 负载限制，必须分别测量 FV / DMA / 应用帧率。",
        f_note,
        MUTED,
        "ma",
    )

    im.save(IMG_DIR / "mt9v034_frame_timing.png", optimize=True)
    print(IMG_DIR / "mt9v034_frame_timing.png")


if __name__ == "__main__":
    draw_wiring()
    draw_timing()
