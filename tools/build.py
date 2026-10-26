#!/usr/bin/env python3
"""一键构建：获取依赖、应用补丁、编译固件。

用法:
    python tools/build.py
    build.bat                    # Windows 双击或命令行

输出目录 out/，产物为 StopWatchPedometer.ino.bin。
"""
import glob
import hashlib
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIBS = os.path.join(ROOT, "tools", "libs")
OUT = os.path.join(ROOT, "out")

DEPS = [
    ("M5Unified", "https://github.com/m5stack/M5Unified.git", "0.2.27", None),
    ("M5GFX", "https://github.com/m5stack/M5GFX.git", "0.2.34",
     os.path.join("docs", "patches", "0001-co5300-wakeup-timing.patch")),
]

FQBN = "esp32:esp32:esp32s3box:PartitionScheme=app3M_fat9M_16MB,DebugLevel=none"
CORE = "esp32:esp32 (3.3.7)"


def run(args, cwd=None):
    print("+", " ".join(args), flush=True)
    return subprocess.run(args, cwd=cwd).returncode


def probe(args, cwd=None):
    """静默执行，仅返回退出码。"""
    return subprocess.run(args, cwd=cwd,
                          stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode


def fail(msg):
    print("[fail] " + msg, file=sys.stderr)
    sys.exit(1)


def need(tool):
    if not shutil.which(tool):
        fail(f"未找到 {tool}")


def find_arduino_cli():
    for name in ("arduino-cli.exe", "arduino-cli"):
        p = os.path.join(ROOT, "tools", name)
        if os.path.isfile(p):
            return p
    found = shutil.which("arduino-cli")
    if found:
        return found
    fail("未找到 arduino-cli。可放入 tools/ 或加入 PATH：\n"
         "        https://github.com/arduino/arduino-cli/releases")


def check_core(cli):
    r = subprocess.run([cli, "core", "list"], capture_output=True, text=True)
    if CORE not in r.stdout:
        print(f"[warn] 未检测到 esp32 core 3.3.7，当前已安装：")
        for line in r.stdout.splitlines()[1:]:
            if line.strip():
                print("        " + line.strip())


def fetch(name, url, ref, patch):
    dst = os.path.join(LIBS, name)
    if os.path.isdir(dst):
        print(f"[skip] {name} {ref} 已存在")
    else:
        os.makedirs(LIBS, exist_ok=True)
        if run(["git", "-c", "advice.detachedHead=false",
                "clone", "--depth", "1", "--branch", ref, url, dst]) != 0:
            fail(f"{name} 获取失败，检查网络或手动放入 tools/libs/{name}")
        print(f"[ok] {name} {ref}")

    if not patch:
        return
    apply_patch(dst, name, os.path.join(ROOT, patch))


def apply_patch(dst, name, pf):
    marker = os.path.join(dst, ".patch_applied")
    if os.path.exists(marker):
        print(f"[skip] {name} 补丁已应用")
        return
    if not os.path.isfile(pf):
        fail(f"缺少补丁文件 {pf}")

    p = os.path.abspath(pf)
    if probe(["git", "apply", "--ignore-whitespace", "--check", p], cwd=dst) == 0:
        if run(["git", "apply", "--ignore-whitespace", p], cwd=dst) != 0:
            fail(f"{name} 补丁应用失败")
        print(f"[ok] {name} 补丁已应用")
    elif probe(["git", "apply", "-R", "--ignore-whitespace", "--check", p], cwd=dst) == 0:
        print(f"[ok] {name} 补丁已存在")
    else:
        fail(f"{name} 补丁与该版本不匹配，删除 tools/libs/{name} 后重试")

    open(marker, "w", encoding="utf-8").write("ok\n")


def check_font():
    """界面里的每个汉字都必须在 ped_font.h 里，否则屏上只留一个空位。

    ped_font.h 由 tools/genfont.py 从源码的字符串字面量里抓字生成；
    改了文案却没重新生成字模，缺的字会静默渲染成空白（曾经把
    「简体中文」显示成「 体 」）。
    """
    src_dir = os.path.join(ROOT, "StopWatchPedometer")
    text = ""
    for p in glob.glob(os.path.join(src_dir, "*.h")) + glob.glob(os.path.join(src_dir, "*.ino")):
        if p.endswith("ped_font.h"):
            continue
        with open(p, encoding="utf-8") as f:
            text += f.read()

    used = set()
    for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', text):
        for ch in m.group(1):
            o = ord(ch)
            if o >= 0x20 and (0x4E00 <= o <= 0x9FFF or o > 0x7F):
                used.add(ch)

    fp = os.path.join(src_dir, "ped_font.h")
    if not os.path.isfile(fp):
        fail("缺少 ped_font.h，先运行 python tools/genfont.py")
    font = open(fp, encoding="utf-8").read()
    seg = font.split("static const PedGlyph kGlH1[]")[1].split("};")[0]
    have = set(int(x, 16) for x in re.findall(r"\{0x([0-9A-Fa-f]{4,6}),", seg))

    missing = sorted(ch for ch in used if ord(ch) not in have)
    if missing:
        print(f"[font] 字库缺 {len(missing)} 个字：{''.join(missing)}")
        fail("请先运行 python tools/genfont.py 重新生成字模：\n"
             "        python tools/genfont.py")
    print(f"[ok] 字库覆盖 {len(used)} 个字")


def main():
    print("== 环境检查 ==")
    need("git")
    cli = find_arduino_cli()
    check_core(cli)

    print("\n== 字库检查 ==")
    check_font()

    print("\n== 获取依赖 ==")
    for name, url, ref, patch in DEPS:
        fetch(name, url, ref, patch)

    print("\n== 编译 ==")
    rc = run([cli, "compile", "--fqbn", FQBN,
              "--libraries", LIBS, "--output-dir", OUT,
              os.path.join(ROOT, "StopWatchPedometer")])
    if rc != 0:
        fail("编译失败")

    bin_path = os.path.join(OUT, "StopWatchPedometer.ino.bin")
    if not os.path.isfile(bin_path):
        fail("未生成 " + os.path.relpath(bin_path, ROOT))

    blob = open(bin_path, "rb").read()
    print("\n[ok] 编译完成")
    print(f"     产物 {os.path.relpath(bin_path, ROOT)}")
    print(f"     大小 {len(blob):,} B")
    print(f"     SHA256 {hashlib.sha256(blob).hexdigest().upper()}")
    print(f"\n烧写：esptool --chip esp32s3 -p <PORT> -b 921600 write_flash "
          f"0x10000 {os.path.relpath(bin_path, ROOT)}")


if __name__ == "__main__":
    main()
