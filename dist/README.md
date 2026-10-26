# StopWatch 计步器 v2.5

固件发布包。功能说明与编译方法见仓库根目录的 [README](../README.md)。

## 文件

| 文件 | 大小 | 说明 |
|---|---|---|
| `StopWatch_Pedometer_v2.5.bin` | 1,470,112 B | 完整镜像（bootloader + 分区表 + app），写入 `0x0` |
| `app_only.bin` | 1,404,576 B | 仅 app，写入 `0x10000`，用于已刷完整镜像的升级 |
| `flash.bat` | — | Windows 烧写脚本 |

```
SHA256  StopWatch_Pedometer_v2.5.bin
B6CA51B8FB2963F948405C3CA39449AC1B7642E2E80C88D2A598D6F7EF1C5A14

SHA256  app_only.bin
EF7B7AD61966E95B6F7A2F792FB0DF76B0BF3258F7E68EB4A4434C083FA37F1A
```

## 刷入

完整镜像的写入地址为 `0x0`。ESP32-S3 的 bootloader 位于 `0x0`，`0x1000` 为老款 ESP32 的偏移。

进入下载模式：连接 USB，长按电源键约 2 s。烧写完成后双击电源键关机，再单击开机。

```powershell
.\flash.bat                # 单串口，自动识别
.\flash.bat COM5           # 多串口时指定
.\flash.bat COM5 keep      # 将 app_only.bin 写入 0x10000，保留设置与记录
```

在 PowerShell 中执行当前目录的脚本须加 `.\` 前缀，否则将按命令名在 PATH 中查找。

手动命令：

```bash
esptool --chip esp32s3 -p COM5 -b 460800 erase_flash
esptool --chip esp32s3 -p COM5 -b 460800 write_flash \
        --flash_mode keep --flash_freq keep --flash_size keep \
        0x0 StopWatch_Pedometer_v2.5.bin
```

三个 `keep` 用于保留镜像 header 中的 flash 模式；省略时 header 被改写为 `qio`，ROM 停在 `mode:QIO` 之后反复复位。

网页烧写使用 <https://espressif.github.io/esp-launcher/>，地址 `0x0`、Size `16MB`、Mode `DIO`、Freq `80MHz`。

整片擦除将清除设置与步数记录。仅将 `app_only.bin` 写入 `0x10000` 且不执行擦除时保留。
