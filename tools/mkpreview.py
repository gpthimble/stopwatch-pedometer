import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CELL = 300
CAP  = 32
GAP  = 20
PAD  = 26
BG   = (11, 13, 16)
CAPC = (154, 163, 176)

shots = [
    ("s_p0.png", "主页面"),
    ("s_p1.png", "今日数据"),
    ("s_p2.png", "近 7 天"),
    ("s_p3.png", "设置"),
    ("s_p8.png", "设置 · 清零"),
    ("s_p4.png", "对时 · 日期"),
    ("s_p5.png", "对时 · 时间"),
]

cols, rows = 4, 2
W = PAD * 2 + cols * CELL + (cols - 1) * GAP
H = PAD * 2 + rows * (CAP + CELL) + (rows - 1) * GAP
img = Image.new("RGB", (W, H), BG)
d = ImageDraw.Draw(img)
font = ImageFont.truetype("C:/Windows/Fonts/msyhbd.ttc", 20)

for i, (name, title) in enumerate(shots):
    r, c = divmod(i, cols)
    x = PAD + c * (CELL + GAP)
    y = PAD + r * (CAP + CELL + GAP)
    tw = d.textlength(title, font=font)
    d.text((x + (CELL - tw) / 2, y + 5), title, font=font, fill=CAPC)
    p = os.path.join(ROOT, name)
    im = Image.open(p).convert("RGB").resize((CELL, CELL), Image.LANCZOS)
    img.paste(im, (x, y + CAP))

out = os.path.join(ROOT, "dist", "preview_ui.png")
img.save(out)
print("saved", out, img.size)
