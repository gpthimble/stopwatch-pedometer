"""从系统字体生成 8bpp 抗锯齿字模 -> StopWatchPedometer/ped_font.h

字符集自动从源码里抓（所有非 ASCII 字符 + 可打印 ASCII），
只保留界面真正用到的字，避免把整个 CJK 字库塞进 flash。
"""
import os
import re
import glob

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIR = os.path.join(ROOT, "StopWatchPedometer")
OUT = os.path.join(SRC_DIR, "ped_font.h")

YAHEI = "C:/Windows/Fonts/msyh.ttc"
YAHEI_B = "C:/Windows/Fonts/msyhbd.ttc"
ARIAL_B = "C:/Windows/Fonts/arialbd.ttf"

# name, ttf, size, index, charset
SPECS = [
    ("H1",  YAHEI_B, 27, 0, "cjk"),
    ("L",   YAHEI,   27, 0, "cjk"),
    ("S",   YAHEI_B, 20, 0, "cjk"),
    ("SM",  YAHEI,   21, 0, "cjk"),
    ("NUM", ARIAL_B, 78, 0, "num"),
    ("BIG", ARIAL_B, 100, 0, "num"),
    ("HUGE", ARIAL_B, 120, 0, "num"),
    ("MID", ARIAL_B, 44, 0, "num"),
]

NUM_CHARS = "0123456789:.%-+/ "


def collect_chars():
    text = ""
    for p in glob.glob(os.path.join(SRC_DIR, "*.h")) + glob.glob(os.path.join(SRC_DIR, "*.ino")):
        with open(p, encoding="utf-8") as f:
            text += f.read()
    # 只取字符串字面量里的字符：注释里的汉字不占字库
    chars = set()
    for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', text):
        for ch in m.group(1):
            o = ord(ch)
            if o < 0x20:
                continue
            if 0x4E00 <= o <= 0x9FFF or o > 0x7F:
                chars.add(ch)
    chars |= set(chr(c) for c in range(0x20, 0x7F))
    return chars


def build(name, path, size, index, mode, charset):
    font = ImageFont.truetype(path, size, index=index)
    ascent, descent = font.getmetrics()
    chars = sorted(charset) if mode == "cjk" else sorted(set(NUM_CHARS))

    glyphs = []
    blob = bytearray()

    pad = 4
    for ch in chars:
        adv = int(round(font.getlength(ch)))
        if adv <= 0:
            adv = max(1, size // 3)
        h_box = ascent + descent
        img = Image.new("L", (max(adv, 1) + pad * 2, h_box + pad * 2), 0)
        d = ImageDraw.Draw(img)
        d.text((pad, pad + ascent), ch, font=font, fill=255, anchor="ls")
        bbox = img.getbbox()
        if bbox is None:
            glyphs.append((ord(ch), adv, 0, 0, 0, 0, len(blob)))
            continue
        crop = img.crop(bbox)
        w, h = crop.size
        xo = bbox[0] - pad
        yo = bbox[1] - pad
        glyphs.append((ord(ch), adv, w, h, xo, yo, len(blob)))
        blob += crop.tobytes()

    return glyphs, blob, ascent, descent


def emit():
    charset = collect_chars()
    print("charset:", len(charset))
    parts = []
    parts.append('// 自动生成，勿手改。见 tools/genfont.py\n#pragma once\n')
    parts.append(
        "struct PedGlyph { uint32_t cp; uint16_t adv; uint8_t w, h; int8_t xo, yo; uint32_t ofs; };\n"
        "struct PedFont { const PedGlyph *gl; uint16_t n; const uint8_t *bmp;"
        " int8_t ascent, descent; };\n"
    )

    decls = []
    for name, path, size, index, mode in SPECS:
        glyphs, blob, ascent, descent = build(name, path, size, index, mode, charset)
        g = "kGl%s" % name
        b = "kBm%s" % name
        parts.append("static const PedGlyph %s[] = {" % g)
        line = []
        for cp, adv, w, h, xo, yo, ofs in glyphs:
            line.append("{0x%04X,%d,%d,%d,%d,%d,%u}" % (cp, adv, w, h, xo, yo, ofs))
            if len(line) == 6:
                parts.append("    " + ",".join(line) + ",")
                line = []
        if line:
            parts.append("    " + ",".join(line) + ",")
        parts.append("};")
        parts.append("static const uint8_t %s[%d] = {" % (b, max(len(blob), 1)))
        line = []
        for i, v in enumerate(blob):
            line.append(str(v))
            if len(line) == 32:
                parts.append("    " + ",".join(line) + ",")
                line = []
        if line:
            parts.append("    " + ",".join(line) + ",")
        parts.append("};")
        parts.append(
            "static const PedFont F_%s = { %s, %d, %s, %d, %d };"
            % (name, g, len(glyphs), b, ascent, descent)
        )
        decls.append(name)
        print("%-4s glyphs=%d bytes=%d ascent=%d descent=%d" % (name, len(glyphs), len(blob), ascent, descent))

    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(parts))
    print("wrote", OUT, os.path.getsize(OUT) // 1024, "KB")


if __name__ == "__main__":
    emit()
