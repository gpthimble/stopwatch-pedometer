# StopWatch 计步器 v2.3

M5Stack StopWatch（C152）固件。硬件：ESP32-S3R8 / 16MB Flash / 8MB OPI PSRAM / BMI270 / 466×466 CO5300 AMOLED / RX8130 / M5PM1。

计步全部由 BMI270 的硬件步数引擎完成，MCU 不采样加速度。息屏静止后 ESP32 进 deep sleep，睡眠期间 BMI270 照常计数，系统电流约 **0.26 mA**，450 mAh 电池理论可用约 **70 天**。

界面预览见 `preview_ui.png`。

## 文件

| 文件 | 大小 | 说明 |
|---|---|---|
| `StopWatch_Pedometer_v2.3.bin` | 1,196,368 B | 完整镜像（bootloader + 分区表 + app），烧 **`0x0`** |
| `app_only.bin` | 1,130,832 B | 仅 app，烧 `0x10000`，只用于已刷过完整镜像的升级 |
| `flash.bat` | — | Windows 一键烧录 |
| `preview_ui.png` | — | 7 个页面预览 |

```
SHA256  StopWatch_Pedometer_v2.3.bin
9D1D3AEE5CA180FD757ABFD99997C776E7201EE0FA1005B463BFE421C4D4E215
SHA256  app_only.bin
DD4C758BA6A7375823717B616CB55296256155C75B3788305D9C711B6D107751
```

## 刷写

**地址必须是 `0x0`。** ESP32-S3 的 bootloader 在 `0x0`，`0x1000` 是老款 ESP32 的偏移。烧错的表现是屏黑、灯亮、ROM 反复复位，改地址重刷即可，不会损坏设备。

进下载模式：USB 接电脑，长按电源键约 2 s，绿灯亮后松手。

```powershell
.\flash.bat                # 单串口，自动识别
.\flash.bat COM5           # 多串口时指定
.\flash.bat COM5 keep      # 只把 app_only.bin 写到 0x10000，不清除设置和记录
```

PowerShell 必须加 `.\`，否则会把 `flash.bat` 当命令名去 PATH 里找。脚本会依次寻找 `dist\esptool.exe`、Arduino 核心自带的 `esptool.exe`、`python -m esptool`、`py -3 -m esptool`，找到即用，不需要 `pip install`。

脚本做三件事：整片擦除 → 写入 → 校验。看到 `VERIFY OK` 才算成功，中途出现 `DEVICE LOST` 就是掉线了，重新进下载模式再跑一次。

**整片擦除会清掉设置和步数记录**（合并镜像里 0x9000 的 NVS 区被填充为 `0xFF`）。要保留数据就只烧 `app_only.bin` 到 `0x10000`，不要擦除。

手动命令（三条依次执行）：

```bash
esptool --chip esp32s3 -p COM5 -b 460800 erase_flash
esptool --chip esp32s3 -p COM5 -b 460800 write_flash \
        --flash_mode keep --flash_freq keep --flash_size keep \
        0x0 StopWatch_Pedometer_v2.3.bin
esptool --chip esp32s3 -p COM5 -b 460800 verify_flash 0x0 StopWatch_Pedometer_v2.3.bin
```

`keep` 是必须的。镜像 header 里已经是 `DIO / 80MHz / 16MB`，用 `--flash_mode qio` 强改 header 会让 ROM 在 `mode:QIO, clock div:1` 之后停住，表现为黑屏加 `ets_loader.c 78` 复位循环。

网页烧录 <https://espressif.github.io/esp-launcher/>：Flash Address `0x0`、File 选完整镜像、Flash Size `16MB`、Mode `DIO`、Freq `80MHz`。

刷完短按一次电源键重启。

镜像不碰 ROM 和 eFuse，长按电源键进下载模式的设定不变。万一失效，用排针 9 脚（G0）短接 11 脚（GND）再上电，可强制进入。

## 操作

屏幕是圆的，四角在可视区之外，因此**页面上不放按钮**，全部靠手势。

| 手势 | 功能 |
|---|---|
| 左滑 | 下一层：普通页翻下一页；对时页 日期 → 调时 → 调分 |
| 右滑 | 上一层：普通页翻上一页；对时页退回上一步，日期页右滑回设置 |
| 点按 | 普通页翻下一页；设置页点行内 `−` `＋` 改值、点「对时」进入对时页；对时页写入 RTC |
| 上下拖动 | 设置页滚动列表、对时页日期轮跟手滚动，松手吸附到整格 |

| 按键 | 功能 |
|---|---|
| 黄键短按 | 下一页：主页 → 今日数据 → 近 7 天 → 设置 |
| 黄键长按 1.2 s | 回主页 |
| 蓝键短按 | 息屏 |
| 蓝键长按 0.9 s | 今日步数清零（等同设置页「清零今日」） |
| 电源键单击 | 亮屏 |
| 电源键双击 | 关机（先存盘） |

**只有按键能点亮屏幕**，触摸不唤醒 —— 熄屏后连触摸控制器都不再轮询，既不误触也不白耗电。
亮屏后的第一下触摸若手指恰好在屏上会被丢弃，不触发原动作。

## 页面

1. **主页** — 贴边环形进度、时间、日期+星期、超大号步数、完成百分比 · 目标、距离 · 消耗，底部四段电量条（≥75% / 50% / 25% 分段点亮，低电转金色/红色，插 USB 时整体变绿）。
2. **今日数据** — 距离、消耗、活跃时长、步频、完成度、电量。
3. **近 7 天** — 柱状图。**今天固定在最右一格**，向左回推 6 天，没记录的那天留空，今天的日期用亮色加粗；含今天的实时步数（不必等到跨天才出现）。底部显示这 7 天的日均。
4. **设置** — 9 项，5 行一屏，像素级跟手滚动，松手吸附到整行。
   按下后先判定主轴并锁死：纵向只滚列表，横向只切页（底部画进度条，滑够 40 px 切页），两者不会互相干扰。
   `−`/`+` 的热区比视觉按钮宽一倍（整行右段各 66 px），抬起时判定，容差 24 px，手抖不会丢点击。
5. **对时** — 日期页三列上下滚轮（年 / 月 / 日，跟手、吸附）；时间页 24 小时制表盘，数字贴屏幕边缘，中心是当前读数。
6. **硬件计步** — 当前计数值、`0x1E`/`0x30` 两个寄存器原始值、引擎使能位、参数回读、唤醒次数、电池电压。

对时页的层级是「日期 → 调时 → 调分」：左滑进下一层，右滑退回上一层，点按确认并把秒归零。

表盘的 24 小时由拖动角度累积决定，不需要选上午/下午：顺时针转过 12 点自动进入下午，边缘刻度随之从 12–23 显示；逆时针转回则回到上午的 0–11。

## 设置项

| 项 | 范围 | 步进 | 默认 |
|---|---|---|---|
| 每日目标 | 1000–40000 步 | 500 | 8000 |
| 身高 | 120–220 cm | 1 | 172 |
| 体重 | 30–180 kg | 1 | 65 |
| 亮度 | 40–255 | 15 | 160 |
| 提示 | 静音 / 震动 / 震动+声音 | — | 震动+声音 |
| 息屏 | 5–120 s | 5 | 15 |
| 深睡 | 15–240 s | 15 | 45 |
| 对时 | 进入对时页 | — | — |
| 清零今日 | 连按两次，间隔 5 s 内有效 | — | — |

保存在 NVS 命名空间 `pedo2`，断电不丢。步幅按身高 × 0.415 推算，消耗按 0.53 kcal/(kg·km)。

每次按 `−`/`+` 只标记「待落盘」，静置 700 ms 后才真正写 NVS —— 连续点按不会每次都擦写一页，但也不会因为没写而丢设置。离开设置页、进入对时页、关机、进深睡前都会立刻强制写一次。

今日成绩每 30 s 落盘一次（有变化才写），关机、跨天、清零前各强制写一次；同一份成绩同时记在 NVS 和 RTC 内存里，深睡唤醒时取两者较大值，避免睡眠期间丢步。

## 触感

震动是「挡位感」而不是持续嗡鸣：过一格 170 档 10 ms，吸附落位 200 档 14 ms，翻页 190 档 12 ms，确认 200 档 26 ms。

提示音档位跟 `提示` 设置联动：静音不震不响；震动只震；震动+声音在翻页、达标提示时补一声短音（翻页 1320 Hz / 22 ms），刚切到「震动+声音」时会先试听一声。

脉冲用非阻塞定时关断（`vibOn` / `vibService`），每渲染一帧、每次采样都检查一次。**不要在这里 `delay()`**：一帧要 15–25 ms，阻塞会把触摸采样拖慢，表现为拖动不跟手、丢格。

## 计步

BMI270 的步数功能属于 feature engine，M5Unified 在 `M5.begin()` 时已上传 8 KB 微代码，固件再做两件事：

1. **写算法参数** — 25 个 16 位参数位于 feature page 3 起（每页 8 个，最后一个在 page 6 的 0x30），取值来自 Bosch 数据手册 Table 15 的腕式参数表。出厂这些参数是全 0，**不写就不会计数**。
2. **打开引擎** — page 6 偏移 `0x02` 的字里 bit12（寄存器 `0x33` bit4）置 1。

读取：每秒读一次 feature page 0 的 `0x30..0x33`（32 位计数），同时读 `0x1E` 的 16 位镜像。开机时自动判定哪个寄存器在递增并用它，差值累加为今日步数。

配套设置：加速度 50 Hz、±2 g、只开加速度通道（陀螺/温度/辅机全关），`PWR_CONF` 关掉高级省电以保证引擎按标称速率运行。

固件不采样加速度，也不运行软件峰值检测。

## 休眠

- 无操作 15 s → 亮度 0。AMOLED 黑屏即像素熄灭，没有中间暗档。
- 息屏后再等 45 s（`息屏 + 深睡`）→ deep sleep。
- 睡眠期间 BMI270 继续计数。每 60 s 定时唤醒一次，只初始化 I2C 读计数，约 25 ms 后继续睡，不点屏。
  周期从 20 s 放宽到 60 s 是有意的：计数由 BMI270 自己累加，醒来晚只影响「最后一次读数」，代价是最多丢 60 s 的步数，换来唤醒摊销电流从约 60 µA 降到约 15 µA。
- 黄键/蓝键按下（ext1 低电平）唤醒，走完整启动，约 0.7 s 后接受操作。
- 插 USB 时不休眠，方便调试。
- 静默通道连续失败 3 次 → 停用深睡，保持常开。

### 深睡电流构成（估算）

| 来源 | 电流 | 说明 |
|---|---|---|
| BMI270（加速度 50 Hz，normal 模式） | **200 µA** | 数据手册 A-only Normal Mode 典型值，硬件计步的必要开销 |
| ESP32-S3 deep sleep + RTC 内存 | 10 µA | RTC timer 保持 |
| 60 s 定时唤醒摊销 | 15 µA | 每次约 25 ms @ 35 mA |
| M5PM1 + LDO + 触摸芯片深睡 + 漏电 | 40 µA | 3.3 V 轨保持供电（BMI270 / RTC 要用） |
| **合计** | **约 265 µA** | |

450 mAh ÷ 0.265 mA ≈ 1700 h ≈ **70 天**（理论）。计入电池自放电、DCDC 效率、温度，实际按 **55–60 天** 估。

BMI270 是绝对大头。它在 low-power 模式（ODR 25 Hz + `PWR_CONF.adv_power_save=1`）只要 10 µA，能把这个数字推到 150 天以上，
但 Bosch 那 25 个腕式参数是按 50 Hz 整定的，降采样后计步精度会变，因此这里选择保精度。要试的话改 `ped_imu.h` 的 `R_ACC_CONF` 与 `R_PWR_CONF` 两行即可。

工作时（亮屏、正常操作、屏幕全亮）参考 M5Stack 官方 PowerDemo：正常操作 45 mA，屏幕全亮 154 mA。

电源灯默认关闭（`ped_core.h` 的 `POWER_LED_ON`），M5PM1 只有开关没有 PWM，省 1–3 mA。

低电提醒：

- 主页电量条低于 20% 转红。
- 深睡中每 15 次唤醒（约 15 分钟）读一次电池，低于 3300 mV 时状态灯闪两下。

## 编译

需要 arduino-cli + esp32 core 3.3.7 + M5Unified 0.2.27。

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32s3box:PartitionScheme=app3M_fat9M_16MB,DebugLevel=none" \
  --libraries tools/libs --output-dir out_v2 StopWatchPedometer

esptool --chip esp32s3 merge-bin -o dist/StopWatch_Pedometer_v2.3.bin \
  0x0 out_v2/StopWatchPedometer.ino.bootloader.bin \
  0x8000 out_v2/StopWatchPedometer.ino.partitions.bin \
  0xe000 <core>/tools/partitions/boot_app0.bin \
  0x10000 out_v2/StopWatchPedometer.ino.bin \
  --flash-mode keep --flash-freq keep --flash-size keep
```

`merge-bin` 也要带三个 `keep`，否则 header 会被改回 `qio`。

**板卡必须用 `esp32s3box`，不能用通用 `esp32s3`。** 后者 `build.boot=qio`，`build.memory_type` 因此是 `qio_qspi`，链接到 QSPI 版 IDF 库，`psramInit()` 驱动 OPI 芯片必然失败；Arduino IDE 的 `PSRAM=OPI` 选项只写 `build.psram_type`，`platform.txt` 并不引用它。症状是 ROM 打印 `ets_loader.c 78` 后进入 `TG0WDT_SYS_RST` 循环、黑屏无日志。

字模由 `tools/genfont.py` 从系统雅黑/雅黑粗体生成，字符集只抓源码字符串字面量里用到的字。**改了界面文案就要重跑一次**，否则新字会画不出来。

## 串口调试

115200 打开串口（USB-Serial/JTAG），单字符命令：

| 命令 | 作用 |
|---|---|
| `0`–`5` | 跳到指定页；`4` 会重建对时页并重新读 RTC |
| `n` / `p` | 下一页 / 上一页 |
| `c` | 打印计数、IMU 状态 |
| `S` | dump 一帧（`IMG w h n` + 大端 RGB565） |
| `t` | 对时页切换 日期 / 时间 |
| `u` | 时间页切换 调时 / 调分 |
| `T<YYYYMMDDHHMM>#` | 串口对时，写完立刻回读 |
| `k` | 打印内存中的设置 + NVS 里存的值（对照即知有没有落盘） |
| `g` / `G` | 模拟点一次「亮度 +」/「亮度 −」 |
| `w` | 深睡诊断：USB、IMU、失败计数、唤醒周期、累计唤醒次数、电池毫伏 |
| `B` | 等价于「清零今日」 |
| `E` | 设置列表滚到底（看最后一页） |
| `x` / `z` | 写 1234 步 / 清零，都会落盘 |

`tools/devtool.py` 是本机的抓帧工具：`shot` / `mon` / `cmd` / `seq`。`seq` 在同一个串口连接里依次发命令再抓帧，是最常用的：

```bash
python tools/devtool.py seq COM3 "4;t" s_dial.png
```

## 已知限制

- 参数表是腕式场景的参数；放在口袋里计数会偏慢，这是引擎的固有取舍。
- 手持晃动、骑车会被计入。
- 无 Wi-Fi / BLE，数据只在本地。
- 跨天清零依赖 RTC，RTC 未校准则不自动清零。手动清零：长按蓝键 0.9 s，或设置页最后一项「清零今日」连点两次。
- 深睡唤醒要重建 QSPI 面板，亮屏到可操作约 0.7 s。

## 启动日志

```
[ped] reset=... heap=... psram=8388608
[ped] M5.begin panel=466x466 dma=...
[ped] canvas 466x466 ok=1 bufsize=434312
[ped] imu ready=1 err=0 param=012D expect=012D
[ped] loadToday steps=... day=... today=...
[ped] setup done ... ms
[ped] first render ... ms (page 0)
```

`psram=0` 或 `canvas ok=0` 表示 PSRAM 没起来，屏会黑；`imu ready=0` 时 `err` 指出卡在哪个寄存器。

## 故障排查

| 现象 | 原因 |
|---|---|
| 屏黑、灯亮、ROM 日志 `invalid header: 0xffffffff` | 烧录地址填了 `0x1000`，改 `0x0` |
| `ets_loader.c 78` 后 `TG0WDT_SYS_RST` 循环，日志里 `mode:QIO` | header 被 `--flash_mode qio` 强改过。用 `keep` 重刷 |
| 同上，但 `verify_flash` 报不一致或出现 `DEVICE LOST` | 镜像只写进去一部分。重新进下载模式，先 `erase_flash` 再刷 |
| 只烧 `app_only.bin` 后黑屏 | `0x8000` 处没有分区表，先刷一次完整镜像 |
| 走路不计步 | 看「硬件计步」页：`EN` 不是 `10` 说明引擎没打开；`参数` 不等于 `期望` 说明参数没写进去 |
| 界面出现空白方块 | 改了文案没重跑 `tools/genfont.py` |
| 拖动不跟手、丢格 | 事件回调里出现了阻塞（`delay`、长 I2C）。震动必须走 `vibOn`/`vibService` |
| `flash.bat` 输出一堆 `'l'`、`'ptool'` 碎片 | 文件行尾变成了 LF。cmd 只认 CRLF |
| `flash.bat` 提示 `not found` 但文件就在当前目录 | PowerShell 不执行当前目录的程序，加 `.\` |

恢复官方固件用 M5Burner 重刷 StopWatch 官方包。
