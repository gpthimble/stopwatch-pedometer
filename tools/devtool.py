"""串口调试工具：抓帧、看计数、翻页。

用法:
  python devtool.py shot  [端口] [页号]   抓一帧存成 shot_pN.png
  python devtool.py mon   [端口] [秒]     监视计数输出
  python devtool.py cmd   [端口] <命令>   发命令并打印回显
"""
import sys, time, os
import serial
from PIL import Image

PORT = "COM3"


def open_port(port):
    s = serial.Serial(port, 115200, timeout=0.2)
    s.setDTR(True)
    s.setRTS(True)
    time.sleep(0.3)
    s.reset_input_buffer()
    return s


def drain(ser, seconds):
    buf = b""
    t = time.time()
    while time.time() - t < seconds:
        buf += ser.read(ser.in_waiting or 1)
    return buf


def shot(ser, page, out):
    if page is not None:
        ser.write(str(page).encode())
        time.sleep(0.6)
    ser.reset_input_buffer()
    ser.write(b"S")

    buf = b""
    deadline = time.time() + 20
    hdr = None
    while time.time() < deadline:
        buf += ser.read(ser.in_waiting or 1)
        i = buf.find(b"IMG ")
        if i >= 0:
            j = buf.find(b"\n", i)
            if j >= 0:
                parts = buf[i:j].split()
                hdr = (int(parts[1]), int(parts[2]), int(parts[3]))
                body = buf[j + 1:]
                break

    if hdr is None:
        print("no IMG header")
        return False

    w, h, n = hdr
    while len(body) < n:
        body += ser.read(min(65536, n - len(body)))
        if len(body) < n and not ser.in_waiting:
            time.sleep(0.01)

    data = body[:n]
    px = bytearray(w * h * 3)
    for k in range(w * h):
        v = (data[2 * k] << 8) | data[2 * k + 1]
        px[3 * k] = ((v >> 11) & 0x1F) * 255 // 31
        px[3 * k + 1] = ((v >> 5) & 0x3F) * 255 // 63
        px[3 * k + 2] = (v & 0x1F) * 255 // 31

    Image.frombytes("RGB", (w, h), bytes(px)).save(out)
    print("saved", out, w, h, len(data))
    return True


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "shot"
    port = sys.argv[2] if len(sys.argv) > 2 else PORT
    ser = open_port(port)

    if mode == "shot":
        page = int(sys.argv[3]) if len(sys.argv) > 3 else None
        for p in ([page] if page is not None else [0]):
            shot(ser, p, "shot_p%d.png" % p)
    elif mode == "mon":
        secs = float(sys.argv[3]) if len(sys.argv) > 3 else 8
        print(drain(ser, secs).decode("utf-8", "replace"))
    elif mode == "cmd":
        time.sleep(2.5)                     # 打开串口会触发复位，先等启动完成
        ser.reset_input_buffer()
        ser.write(sys.argv[3].encode())
        time.sleep(0.6)
        print(drain(ser, 2.0).decode("utf-8", "replace"))
    elif mode == "seq":
        # python devtool.py seq COM3 "4;t" out.png   依次发命令后抓帧
        out = sys.argv[4] if len(sys.argv) > 4 else "shot_seq.png"
        time.sleep(2.5)                     # 等设备启动完成
        for c in sys.argv[3].split(";"):
            if not c:
                continue
            ser.write(c.encode())
            time.sleep(1.2)
        ser.reset_input_buffer()
        shot(ser, None, out)

    ser.close()


if __name__ == "__main__":
    main()
