"""轮询 COM3，出现后立即烧写固件（突破深睡循环窗口）"""
import subprocess, sys, time

PY = r"C:/Users/giant/.workbuddy/binaries/python/envs/default/Scripts/python.exe"
PORT = "COM3"
BIN = "out_dbg/StopWatchPedometer.ino.bin"
FLASH = ["--chip", "esp32s3", "--port", PORT, "--baud", "115200",
         "--before", "default_reset", "--after", "hard_reset", "write_flash",
         "--flash_mode", "keep", "--flash_freq", "keep", "--flash_size", "keep",
         "0x10000", BIN]

def port_ok():
    from serial.tools import list_ports
    return any(p.device == PORT for p in list_ports.comports())

deadline = time.time() + 1800
attempt = 0
while time.time() < deadline:
    attempt += 1
    if not port_ok():
        print(f"[{attempt}] {PORT} 不在（深睡中），1.5s 后重试...", flush=True)
        time.sleep(1.5)
        continue
    print(f"[{attempt}] 抓到 {PORT}！立即烧写...", flush=True)
    r = subprocess.run([PY, "-m", "esptool"] + FLASH, capture_output=True, text=True, timeout=240)
    out = (r.stdout or "") + (r.stderr or "")
    tail = "\n".join(out.strip().splitlines()[-12:])
    print(tail, flush=True)
    if "Hash of data verified." in out:
        print("=== FLASH OK ===", flush=True)
        sys.exit(0)
    if "Could not open" in out or "doesn't exist" in out or "busy" in out:
        print("--- 端口又睡了，继续轮询 ---", flush=True)
        time.sleep(1.5)
        continue
    print("--- 其他错误，继续重试 ---", flush=True)
    time.sleep(2)
print("=== TIMEOUT: 5 分钟内未完成烧写 ===", flush=True)
sys.exit(1)
