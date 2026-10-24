"""深睡唤醒回归：发命令 -> 设备进 L2（USB 掉）-> 唤醒后重连抓启动日志。

关键点：打开串口前先把 DTR/RTS 置为 False，否则 open() 会拉 EN 触发芯片复位，
命令会被吃掉、也看不到深睡唤醒的真实启动日志。

用法: waketest.py <cmd> [总时长秒]
"""
import sys
import time
import serial

PORT = "COM3"
cmd = sys.argv[1] if len(sys.argv) > 1 else "N"
total = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0


def try_open(timeout=0.05):
    try:
        s = serial.Serial()
        s.port = PORT
        s.baudrate = 115200
        s.timeout = timeout
        s.dtr = False          # 必须在 open 之前设置
        s.rts = False
        s.open()
        return s
    except Exception:
        return None


# ---- 1) 连上并等启动完成 ----
s = None
for _ in range(40):
    s = try_open()
    if s is not None:
        break
    time.sleep(0.3)
if s is None:
    print("!! 打不开串口")
    sys.exit(1)

boot = b""
t = time.time()
while time.time() - t < 5.0:
    try:
        boot += s.read(s.in_waiting or 1)
    except Exception:
        break
    if b"setup done" in boot:
        break
print(">>> 已就绪 (%d bytes 启动输出)" % len(boot))

# ---- 2) 发命令 ----
s.reset_input_buffer()
s.write(cmd.encode())
s.flush()
print(">>> 已发送 %r" % cmd)

# ---- 3) 跨深睡抓日志 ----
buf = b""
t0 = time.time()
reconnects = 0
while time.time() - t0 < total:
    if s is None:
        s = try_open()
        if s is None:
            time.sleep(0.35)
            continue
        reconnects += 1
    try:
        buf += s.read(s.in_waiting or 1)
    except Exception:
        try:
            s.close()
        except Exception:
            pass
        s = None

if s is not None:
    try:
        s.close()
    except Exception:
        pass

print(">>>> 重连 %d 次, %d bytes" % (reconnects, len(buf)))
print(buf.decode("utf-8", "replace"))
