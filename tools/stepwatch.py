"""抗断线的串口日志守望：COM3 消失就轮询等待，出现即重连，全部带时间戳写入 stepmon.log。

用法: python tools/stepwatch.py [分钟数，默认 15]
"""
import sys
import time

import serial

PORT = "COM3"
BAUD = 115200
MINUTES = float(sys.argv[1]) if len(sys.argv) > 1 else 15

deadline = time.time() + MINUTES * 60
log = open("out_dbg/stepmon.log", "ab")
print(f"[watch] up to {MINUTES:.0f} min, log=out_dbg/stepmon.log", flush=True)

ser = None
while time.time() < deadline:
    try:
        if ser is None or not ser.is_open:
            ser = serial.Serial(PORT, BAUD, timeout=2)
            ts = time.strftime("%H:%M:%S")
            entry = f"\n=== {ts} PORT OPEN ===\n"
            log.write(entry.encode()); log.flush()
            print(entry, end="", flush=True)
        line = ser.readline()
        if line:
            ts = time.strftime("%H:%M:%S")
            log.write(f"[{ts}] ".encode() + line)
            log.flush()
            s = line.decode(errors="replace").strip()
            if s and any(k in s for k in ("stp", "cnt", "reset", "boot", "ped", "slp", "imu")):
                print(f"[{ts}] {s}", flush=True)
    except Exception:
        try:
            if ser is not None:
                ser.close()
        except Exception:
            pass
        ser = None
        time.sleep(1)

if ser is not None:
    ser.close()
log.close()
print("[watch] done", flush=True)
