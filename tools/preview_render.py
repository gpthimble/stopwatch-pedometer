# -*- coding: utf-8 -*-
"""v2.1 界面预览：按固件实际布局与字号渲染 6 个页面"""
import math
from PIL import Image, ImageDraw, ImageFont

W = H = 466
SS = 4          # 圆环超采样倍数

BG     = (0, 0, 0)
TRACK  = (26, 28, 32)
TEXT   = (240, 243, 248)
DIM    = (138, 148, 160)
ACCENT = (0, 226, 168)
WARN   = (255, 96, 76)
GOLD   = (255, 198, 62)
BTN    = (38, 42, 48)

CN  = "C:/Windows/Fonts/msyh.ttc"
CNB = "C:/Windows/Fonts/msyhbd.ttc"
NUM = "C:/Windows/Fonts/arialbd.ttf"

CX = CY = 233


def f(path, size):
    return ImageFont.truetype(path, int(size))


F_H1  = f(CNB, 24)
F_L   = f(CN, 24)
F_NUM = f(NUM, 72)
F_MID = f(NUM, 40)
F_SM  = f(NUM, 18)
F_CN16 = f(CNB, 16)


def canvas():
    img = Image.new("RGB", (W, H), BG)
    return img, ImageDraw.Draw(img)


def ct(d, x, y, txt, font, fill):
    d.text((x, y), txt, font=font, fill=fill, anchor="mm")


def cl(d, x, y, txt, font, fill):
    d.text((x, y), txt, font=font, fill=fill, anchor="lm")


def cr(d, x, y, txt, font, fill):
    d.text((x, y), txt, font=font, fill=fill, anchor="rm")


def ring(img, r_out, r_in, a0, span, col_track, col_fill):
    """抗锯齿圆环：4 倍超采样后缩回"""
    big = W * SS
    def disk(r, col):
        m = Image.new("L", (big, big), 0)
        dd = ImageDraw.Draw(m)
        c = (CX * SS, CY * SS)
        dd.ellipse([c[0] - r * SS, c[1] - r * SS, c[0] + r * SS, c[1] + r * SS], fill=col)
        return m

    annulus = Image.new("L", (big, big), 0)
    annulus.paste(disk(r_out, 255), (0, 0))
    annulus.paste(disk(r_in, 0), (0, 0))
    annulus = annulus.resize((W, H), Image.LANCZOS)

    img.paste(Image.new("RGB", (W, H), col_track), (0, 0), annulus)

    if span > 0.05:
        sec = Image.new("L", (big, big), 0)
        ds = ImageDraw.Draw(sec)
        c = (CX * SS, CY * SS)
        ds.pieslice([c[0] - r_out * SS, c[1] - r_out * SS, c[0] + r_out * SS, c[1] + r_out * SS],
                    a0, a0 + span, fill=255)
        sec.paste(Image.new("L", (big, big), 0), (0, 0), disk(r_in, 255))
        sec = sec.resize((W, H), Image.LANCZOS)
        img.paste(Image.new("RGB", (W, H), col_fill), (0, 0), sec)


def page_main(steps=5462, goal=8000, t="08:42"):
    img, d = canvas()
    p = min(1.0, steps / goal)
    r_out, r_in = CX - 2, CX - 2 - 26

    col = ACCENT
    if p > 0.85:
        k = (p - 0.85) / 0.15
        col = tuple(int(ACCENT[i] + (GOLD[i] - ACCENT[i]) * k) for i in range(3))
    ring(img, r_out, r_in, -90, 360 * p, TRACK, col)
    d = ImageDraw.Draw(img)

    ct(d, CX, CY - 122, t, F_MID, DIM)
    ct(d, CX, CY - 62, "今日步数", F_L, DIM)
    ct(d, CX, CY - 6, f"{steps}", F_NUM, TEXT)
    ct(d, CX, CY + 54, f"{int(p*100)}% · 目标 {goal}", F_H1, ACCENT)
    ct(d, CX, CY + 104, "3.90 公里 · 134 千卡", F_L, DIM)
    return img


def page_stats():
    img, d = canvas()
    ct(d, CX, 78, "今日数据", F_H1, TEXT)
    rows = [("距离", "3.90 公里"), ("消耗", "134 千卡"),
            ("活跃", "1:12:40"), ("步频", "104 步/分"), ("完成", "68%")]
    y = 148
    for i, (k, v) in enumerate(rows):
        if i:
            d.rectangle([70, y - 30, W - 70, y - 29], fill=TRACK)
        cl(d, 78, y, k, F_L, DIM)
        cr(d, W - 78, y, v, F_H1, TEXT)
        y += 62
    ct(d, CX, 430, "电量 76%", F_L, DIM)
    return img


def page_hist():
    img, d = canvas()
    ct(d, CX, 78, "近 7 天", F_H1, TEXT)
    data = [6120, 8340, 5410, 9880, 7020, 4310, 5462]
    x0, x1, yb, yt = 70, W - 70, 336, 150
    slot = (x1 - x0) // 7
    bw = slot - 14
    mx = max(max(data), 8000)
    d.rectangle([x0, yb, x1, yb + 1], fill=TRACK)
    days = ["17", "18", "19", "20", "21", "22", "23"]
    for i, v in enumerate(data):
        bx = x0 + i * slot + (slot - bw) // 2
        hgt = max(4, int((yb - yt) * v / mx))
        col = ACCENT if v >= 8000 else (100, 150, 138)
        d.rectangle([bx, yb - hgt, bx + bw, yb], fill=col)
        ct(d, bx + bw // 2, yb - hgt - 16, str(v), F_CN16, DIM)
        ct(d, bx + bw // 2, yb + 22, days[i], F_CN16, DIM)
    ct(d, CX, 408, "日均 6649 步", F_H1, ACCENT)
    return img


def row(d, y, name, val, name_font=F_L, val_font=F_H1):
    cl(d, 40, y, name, name_font, TEXT)
    cr(d, 300, y, val, val_font, TEXT)
    d.ellipse([348 - 23, y - 23, 348 + 23, y + 23], fill=BTN)
    d.rectangle([337, y - 2, 359, y + 3], fill=TEXT)
    d.ellipse([414 - 23, y - 23, 414 + 23, y + 23], fill=BTN)
    d.rectangle([403, y - 2, 425, y + 3], fill=TEXT)
    d.rectangle([411, y - 12, 416, y + 10], fill=TEXT)


def page_set():
    img, d = canvas()
    ct(d, CX, 66, "设置", F_H1, TEXT)
    cr(d, W - 40, 66, "1/3", F_SM, DIM)
    rows = [("每日目标", "8000 步"), ("身高", "172 cm"),
            ("体重", "65 kg"), ("亮度", "160"), ("提示", "震动+声音")]
    for k, (name, val) in enumerate(rows):
        y = 120 + k * 62
        if k:
            d.rectangle([40, y - 31, W - 40, y - 30], fill=TRACK)
        row(d, y, name, val)
    ct(d, CX, 428, "左右滑动切页 · 上下滑动翻行", F_CN16, DIM)
    return img


def page_time():
    img, d = canvas()
    ct(d, CX, 62, "对时", F_H1, TEXT)
    rows = [("年", "2026"), ("月", "10"), ("日", "23"), ("时", "21"), ("分", "26")]
    for k, (name, val) in enumerate(rows):
        y = 116 + k * 54
        if k:
            d.rectangle([40, y - 27, W - 40, y - 26], fill=TRACK)
        row(d, y, name, val)
    d.rounded_rectangle([CX - 96, 392, CX + 96, 444], radius=26, fill=ACCENT)
    ct(d, CX, 418, "确定", F_H1, BG)
    ct(d, CX, 456, "写入 2026-10-23 21:26:00", F_CN16, DIM)
    return img


def page_debug():
    img, d = canvas()
    ct(d, CX, 66, "硬件计步", F_H1, TEXT)
    ct(d, CX, 140, "12746", F_NUM, TEXT)
    ct(d, CX, 200, "0x1E 12746   0x30 12746", F_SM, DIM)
    ct(d, CX, 238, "源 0x30 · 本机硬件 12746 步", F_CN16, TEXT)
    ct(d, CX, 282, "IMU 24  ST 01  EN 10", F_SM, DIM)
    ct(d, CX, 308, "参数 012D / 期望 012D", F_CN16, DIM)
    ct(d, CX, 348, "唤醒 96 次", F_CN16, DIM)
    ct(d, CX, 378, "电池 3812 mV", F_CN16, DIM)
    ct(d, CX, 428, "计数来自 BMI270 硬件引擎", F_CN16, DIM)
    return img


pages = [page_main(), page_stats(), page_hist(), page_set(), page_time(), page_debug()]
titles = ["主页面", "今日数据", "近 7 天", "设置", "对时", "硬件计步"]

GAP = 26
cols, rows_n = 3, 2
CW = cols * W + (cols + 1) * GAP
CH = rows_n * H + (rows_n + 1) * GAP + 34

out = Image.new("RGB", (CW, CH), (24, 26, 30))
d = ImageDraw.Draw(out)

for i, p in enumerate(pages):
    c, r = i % cols, i // cols
    x = GAP + c * (W + GAP)
    y = GAP + 34 + r * (H + GAP)
    out.paste(p, (x, y))
    d.text((x + W // 2, y - 16), titles[i], font=f(CNB, 18),
           fill=(200, 205, 212), anchor="mm")

out.save("preview_ui.png")
print("saved preview_ui.png", out.size)
