# StopWatch 计步器 v2.4

固件发布包。功能说明、编译方法见仓库根目录的 [README](../README.md)。

## 文件

| 文件 | 大小 | 说明 |
|---|---|---|
| `StopWatch_Pedometer_v2.4.bin` | 1,216,512 B | 完整镜像（bootloader + 分区表 + app），烧 **`0x0`** |
| `app_only.bin` | 1,150,912 B | 仅 app，烧 `0x10000`，用于已刷过完整镜像的升级 |
| `flash.bat` | — | Windows 一键烧录 |

```
SHA256  StopWatch_Pedometer_v2.4.bin
1D6D96926D63E3F10F8166F581043BC34487AFDAFFCDA5C9EEA1F1342AD80EDD

SHA256  app_only.bin
294D300D63F96D8914B29633EF2D190F1EA85C73D1E37B296A2432B6D3C1C138
```

## 刷入

完整镜像的烧写地址是 `0x0`；ESP32-S3 的 bootloader 位于 `0x0`，`0x1000` 是老款 ESP32 的偏移。

进下载模式：USB 接电脑，长按电源键约 2 s。烧完短按电源键重启。

```powershell
.\flash.bat                # 单串口，自动识别
.\flash.bat COM5           # 多串口时指定
.\flash.bat COM5 keep      # 只把 app_only.bin 写到 0x10000，不清除设置和记录
```

PowerShell 要加 `.\`，否则会把 `flash.bat` 当命令名去 PATH 里找。

手动命令：

```bash
esptool --chip esp32s3 -p COM5 -b 460800 erase_flash
esptool --chip esp32s3 -p COM5 -b 460800 write_flash \
        --flash_mode keep --flash_freq keep --flash_size keep \
        0x0 StopWatch_Pedometer_v2.4.bin
```

三个 `keep` 用于保留镜像 header 的 flash 模式；缺省时会被改成 `qio`，ROM 停在 `mode:QIO` 之后反复复位。

网页烧录 <https://espressif.github.io/esp-launcher/>：地址 `0x0`、Size `16MB`、Mode `DIO`、Freq `80MHz`。

整片擦除会清掉设置和步数记录。要保留就只烧 `app_only.bin` 到 `0x10000`，不要擦除。
