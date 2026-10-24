"""深睡唤醒受控测试：发 W 强制进 L2（6 s 定时唤醒 + 走完整启动），
随后重新打开串口回读固件写在 NVS 里的上一次现场。

用法: python sleeptest.py [端口]
"""
import sys
import time
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM3"


def read_for(ser, secs):
    buf = b""
    t = time.time()
    while time.time() - t < secs:
        buf += ser.read(ser.in_waiting or 1)
    return buf.decode("utf-8", "replace")


def open_port(port, settle=0.4):
    s = serial.Serial(port, 115200, timeout=0.2)
    s.setDTR(True)
    s.setRTS(True)
    time.sleep(settle)
    s.reset_input_buffer()
    return s


print("== 1) 触发强制深睡 ==")
s = open_port(PORT)
time.sleep(2.6)                     # 等启动完成，避免命令被 boot 吃掉
s.write(b"W")
time.sleep(0.8)
print(read_for(s, 1.2))
s.close()

print("== 2) 等待 6 s 定时唤醒 + 完整启动 ==")
time.sleep(11.0)

print("== 3) 重连回读上一次现场 ==")
for i in range(12):
    try:
        s = open_port(PORT, 0.2)
        break
    except Exception as e:
        if i == 11:
            raise
        time.sleep(1.0)
print(read_for(s, 6.0))
s.close()
