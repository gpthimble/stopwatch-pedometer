# StopWatch 计步器 v3.2

固件发布包。功能说明与编译方法见仓库根目录的 [README](../README.md)。

## 文件

| 文件 | 大小 | 说明 |
|---|---|---|
| `StopWatch_Pedometer_v3.2.bin` | 1,437,872 B | 完整镜像（bootloader + 分区表 + app），写入 `0x0` |
| `app_only.bin` | 1,372,336 B | 仅 app，写入 `0x10000`，用于已刷完整镜像的升级 |
| `flash.bat` | — | Windows 烧写脚本 |

```
SHA256  StopWatch_Pedometer_v3.2.bin
08270553C1012E1B51C9CEB4EF9119DFA3582325068205F98A6F2D885ED4B4C3

SHA256  app_only.bin
FAE5F3C19F2D3CCB1680EDBA1B5A94563424F1CCAAACDF39EDB349876BCF8143
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
        0x0 StopWatch_Pedometer_v3.2.bin
```

三个 `keep` 用于保留镜像 header 中的 flash 模式；省略时 header 被改写为 `qio`，ROM 停在 `mode:QIO` 之后反复复位。

网页烧写使用 <https://espressif.github.io/esp-launcher/>，地址 `0x0`、Size `16MB`、Mode `DIO`、Freq `80MHz`。

整片擦除将清除设置与步数记录。仅将 `app_only.bin` 写入 `0x10000` 且不执行擦除时保留。
