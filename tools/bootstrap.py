#!/usr/bin/env python3
"""拉取构建依赖并可选择立即编译。

用法:
    python tools/bootstrap.py          只准备依赖
    python tools/bootstrap.py --build  准备依赖后编译

依赖放到 tools/libs/，版本固定，M5GFX 需要应用 docs/patches 里的补丁。
已存在的目录不会被覆盖。
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIBS = os.path.join(ROOT, "tools", "libs")

DEPS = [
    ("M5Unified", "https://github.com/m5stack/M5Unified.git", "0.2.27", None),
    ("M5GFX", "https://github.com/m5stack/M5GFX.git", "0.2.34",
     os.path.join("docs", "patches", "0001-co5300-wakeup-timing.patch")),
]

FQBN = "esp32:esp32:esp32s3box:PartitionScheme=app3M_fat9M_16MB,DebugLevel=none"


def run(args, cwd=None):
    print("+", " ".join(args), flush=True)
    return subprocess.run(args, cwd=cwd, check=False).returncode


def fetch(name, url, ref, patch):
    dst = os.path.join(LIBS, name)
    if os.path.isdir(dst):
        print(f"[skip] {name} 已存在")
    else:
        os.makedirs(LIBS, exist_ok=True)
        if run(["git", "clone", "--depth", "1", "--branch", ref, url, dst]) != 0:
            sys.exit(f"[fail] 拉取 {name} 失败")

    if not patch:
        return

    marker = os.path.join(dst, ".patch_applied")
    if os.path.exists(marker):
        print(f"[skip] {name} 补丁已应用")
        return

    pf = os.path.join(ROOT, patch)
    if not os.path.exists(pf):
        sys.exit(f"[fail] 缺少补丁文件 {patch}")

    run(["git", "apply", "--ignore-whitespace", os.path.abspath(pf)], cwd=dst)
    with open(marker, "w", encoding="utf-8") as f:
        f.write("ok\n")
    print(f"[ok] {name} 已应用补丁")


def find_arduino_cli():
    local = os.path.join(ROOT, "tools", "arduino-cli.exe")
    if os.path.exists(local):
        return local
    local = os.path.join(ROOT, "tools", "arduino-cli")
    if os.path.exists(local):
        return local
    found = shutil.which("arduino-cli")
    if found:
        return found
    sys.exit("[fail] 找不到 arduino-cli，请放到 tools/ 或加进 PATH "
             "(https://github.com/arduino/arduino-cli/releases)")


def main():
    for name, url, ref, patch in DEPS:
        fetch(name, url, ref, patch)

    print("\n依赖就绪：tools/libs/{M5Unified 0.2.27, M5GFX 0.2.34 + patch}")

    if "--build" not in sys.argv:
        print("编译：python tools/bootstrap.py --build")
        return

    cli = find_arduino_cli()
    out = os.path.join(ROOT, "out_dbg")
    rc = run([cli, "compile", "--fqbn", FQBN, "--libraries", os.path.join(ROOT, "tools", "libs"),
              "--output-dir", out, os.path.join(ROOT, "StopWatchPedometer")])
    if rc != 0:
        sys.exit("[fail] 编译失败")
    print(f"[ok] 产物在 {out}")
    print("烧写：esptool --chip esp32s3 -p <PORT> write_flash 0x10000 "
          "out_dbg/StopWatchPedometer.ino.bin")


if __name__ == "__main__":
    main()
