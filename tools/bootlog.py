"""用 esptool 做一次硬复位（USB-JTAG 的标准复位时序），紧接着抓启动日志。"""
import subprocess
import sys
import time
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM3"
T = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0
ESPTOOL = r"C:\Users\giant\AppData\Local\Arduino15\packages\esp32\tools\esptool_py\4.5.1\esptool.exe"

subprocess.run([ESPTOOL, "--chip", "esp32s3", "--port", PORT, "--baud", "115200",
                "--before", "default_reset", "--after", "hard_reset", "flash_id"],
               capture_output=True)

s = serial.Serial()
s.port = PORT
s.baudrate = 115200
s.timeout = 0.05
s.dtr = False
s.rts = False
s.open()

buf = b""
t = time.time()
while time.time() - t < T:
    try:
        buf += s.read(s.in_waiting or 1)
    except Exception as e:
        print("ERR", e)
        break
s.close()
print(buf.decode("utf-8", "replace"))
