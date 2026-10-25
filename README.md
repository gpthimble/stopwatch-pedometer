# StopWatch 计步器

M5Stack StopWatch（C152）的计步固件。步数由 BMI270 片上 feature engine 统计，MCU 不读取加速度数据，深睡期间继续计数。

<img src="docs/screenshots/p0_main.png" width="260">

## 硬件

ESP32-S3R8（16 MB Flash / 8 MB OPI PSRAM）、BMI270、466×466 CO5300 AMOLED、CST820 触摸、RX8130 RTC、M5PM1 电源管理、M5IOE1 IO 扩展、450 mAh 电池。

## 功能

- 主页：环形完成度、时间日期、步数、距离与消耗、四段电量条
- 今日数据：距离、消耗、活跃时长、步频、完成度
- 近 7 天柱状图，今天固定在最右格，未记录的日期留空
- 设置 9 项，列表滚动跟手
- 对时：日期三列滚轮 + 24 小时表盘，写入 RX8130
- 息屏、深睡、震动与提示音

## 页面

主页、今日数据、近 7 天、设置四页左右循环。对时页由设置中的「对时」进入。

| 主页 | 今日数据 | 近 7 天 | 设置 |
|---|---|---|---|
| <img src="docs/screenshots/p0_main.png" width="150"> | <img src="docs/screenshots/p1_today.png" width="150"> | <img src="docs/screenshots/p2_week.png" width="150"> | <img src="docs/screenshots/p3_settings.png" width="150"> |

对时页从设置进入，左滑依次切换日期、时、分，点按确认并写入 RTC：

<img src="docs/screenshots/p4_time.png" width="260">

## 操作

屏幕为圆形，四角位于可视区外。界面不设按钮，操作只用手势。

| 手势 | 作用 |
|---|---|
| 左滑 / 右滑 | 翻页；对时页进下一层 / 退上一层 |
| 点按 | 翻下一页；设置页改值；对时页写入 RTC |
| 上下拖动 | 设置列表滚动、日期轮跟随手滚动，松手吸附 |

| 按键 | 作用 |
|---|---|
| 黄键短按 / 长按 1.2 s | 下一页 / 回主页 |
| 蓝键短按 / 长按 0.9 s | 息屏 / 今日步数清零 |
| 电源键单击 / 双击 | 亮屏 / 关机 |

只有按键能点亮屏幕，触摸不唤醒。

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
| 清零今日 | 连点两次，5 s 内有效 | — | — |

设置存于 NVS 命名空间 `pedo2`，掉电保留。步幅按身高 × 0.415 计算，消耗按 0.53 kcal/(kg·km) 计算。

## 技术栈

| 部分 | 做法 |
|---|---|
| 绘图 | M5GFX（LovyanGFX）驱动 CO5300，QSPI 80 MHz。468×468 RGB565 画布放 PSRAM，双缓冲，整屏推送。文字是预生成点阵字模，由 `tools/genfont.py` 从源码字符串里提取用到的字 |
| 触摸 | CST820，I2C 轮询。手势在固件内判定：按下时先锁定主轴，纵向滚动列表、横向切换页面，滚动带吸附 |
| 计步 | BMI270 片上 feature engine，硬件计数。MCU 每秒读一次寄存器取差值，不跑软件峰值检测 |
| 计时 | RX8130 RTC，I2C，掉电走时。每秒读一次用于显示与跨天判断 |
| 电源 | M5PM1 三路 LDO，M5IOE1 扩展 IO。屏幕和扬声器在独立轨上，可以单独断电；ESP32-S3 用 deep sleep + ext1 按键唤醒 |
| 存储 | NVS 存设置和今日成绩。改值先标脏，静置 700 ms 才落盘，避免连续点按反复擦写 |
| 字体 | 点阵字模编进固件，不依赖文件系统 |

## 休眠与续航

息屏和深睡是两级：无操作 15 s（可调）亮度归零，再等 45 s（可调）进 deep sleep。

进深睡前会断开屏幕所在的 L3B 轨（M5IOE1 的 IO8 拉低），AMOLED 不再有供电。BMI270 留在常电轨 L1 上，继续按 100 Hz 采样并计数。深睡期间每 60 s 定时唤醒一次，只初始化 I2C 读计数器，约 25 ms 后继续睡，不点屏。

| 状态 | 电流 |
|---|---|
| 深睡（屏幕断电，BMI270 计数中） | 约 0.26 mA |
| 亮屏正常操作 | 约 45 mA |
| 屏幕全亮 | 约 154 mA |

450 mAh 电池深睡理论 70 天。计入自放电、转换效率和温度，按 **55–60 天** 估。

深睡电流主要是 BMI270（约 200 µA），其次是 3.3 V 轨常电的 PMIC、触摸芯片和漏电（约 40 µA），定时唤醒摊销约 15 µA。BMI270 降到 25 Hz 并开启 `adv_power_save` 可降至 10 µA 量级，但 25 个步数参数按 100 Hz 整定，降采样后精度下降，因此维持 100 Hz。

插 USB 时不进深睡，方便调试。

## 刷入

完整镜像写 **`0x0`**（bootloader 在 0x0，不是老款 ESP32 的 0x1000）；只升级 app 写 `0x10000`。

```bash
esptool --chip esp32s3 -p COM5 -b 460800 write_flash \
  --flash_mode keep --flash_freq keep --flash_size keep \
  0x0 dist/StopWatch_Pedometer_v2.4.bin
```

三个 `keep` 必须保留：镜像 header 已为 DIO / 80 MHz / 16 MB，改成 `qio` 会使 ROM 停在 `mode:QIO` 之后反复复位。

进下载模式：USB 接电脑，长按电源键约 2 s。网页烧录用 <https://espressif.github.io/esp-launcher/>，地址 `0x0`、Size `16MB`、Mode `DIO`、Freq `80MHz`。烧完短按电源键重启。

整片擦除会清掉设置和步数记录。只升级 app 不擦除，则保留。

## 编译

需要 arduino-cli 和 esp32 core 3.3.7。依赖库有脚本拉取：

```bash
python tools/bootstrap.py --build
```

脚本会 clone M5Unified 0.2.27 和 M5GFX 0.2.34 到 `tools/libs/`，给 M5GFX 打上 `docs/patches/0001-co5300-wakeup-timing.patch`，然后编译。

补丁修的是 CO5300 唤醒时序：复位释放后等 120 ms（原为 2 ms），并把 SLPOUT 提到初始化表最前。不打补丁，冷启动会间歇性黑屏或花屏。

手动编译：

```bash
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3box:PartitionScheme=app3M_fat9M_16MB,DebugLevel=none" \
  --libraries tools/libs --output-dir out_dbg StopWatchPedometer
```

板卡为 `esp32s3box`。通用 `esp32s3` 的 `build.memory_type` 是 `qio_qspi`，会链接 QSPI 版 IDF 库，OPI PSRAM 无法启动，表现为黑屏与 `ets_loader.c 78` 复位循环。

## 计步实现

BMI270 的步数功能跑在片上 feature engine 里。固件在 `M5.begin()` 之后做四件事：

1. 软复位，擦掉 M5Unified 上传的默认配置
2. 上传 8192 字节的 legacy 配置包，等 `INTERNAL_STATUS.init_ok`
3. 配加速度计：100 Hz、±2 g、只开加速度通道、关辅机 I2C
4. 置 page 4 字节 1 的 bit4（`en_counter`），启动计数器

legacy 是 Bosch 针对手机场景的配置包，按口袋中的躯干起伏整定。M5Unified 默认上传 base 配置，按腕戴整定，摆臂幅度大时会多计。上传或 `init_ok` 失败时自动重传 base 配置，退回腕戴模式。

计数器读数在 feature page 0 的 `0x30..0x33`（32 位小端），每秒读一次，取差值累加。

## 串口命令

115200 打开串口，单字符命令：

| 命令 | 作用 |
|---|---|
| `0`–`5` | 跳到指定页，`5` 是芯片诊断页（正常操作不可达） |
| `n` / `p` | 下一页 / 上一页 |
| `c` | 打印计数与 IMU 状态 |
| `S` | dump 一帧 RGB565 |
| `I` | dump BMI270 寄存器与 feature 页 |
| `w` | 深睡诊断 |
| `k` | 对照内存设置与 NVS 实际值 |
| `B` | 清零今日 |

`tools/devtool.py` 是抓帧工具：

```bash
python tools/devtool.py seq COM3 "3" page3.png
```

## 已知限制

- 手持晃动、骑车会被计入。
- 无 Wi-Fi / BLE，数据只在本机。
- 跨天清零依赖 RTC，RTC 未校准则不自动清零。
- 深睡唤醒要重建 QSPI 面板，亮屏到可操作约 0.7 s。
- 计步每秒轮询一次，步频快时屏幕上会一次跳两格，累计数值不受影响。
- 目标默认 8000 步，与界面截图中已改为 10000 的设置值不同。

## 参考项目

| 项目 | 用途 |
|---|---|
| [boschsensortec/BMI270_SensorAPI](https://github.com/boschsensortec/BMI270_SensorAPI) | BMI270 配置包、feature 页地址、初始化流程 |
| [m5stack/M5Unified](https://github.com/m5stack/M5Unified) | 板级初始化、I2C、电源按键 |
| [m5stack/M5GFX](https://github.com/m5stack/M5GFX)（LovyanGFX） | 显示驱动与绘图，打了唤醒时序补丁 |
| [m5stack/M5StopWatch-UserDemo](https://github.com/m5stack/M5StopWatch-UserDemo) | M5PM1 / M5IOE1 / CO5300 的寄存器用法参考 |
| [m5stack/M5StopWatch-PowerDemo](https://github.com/m5stack/M5StopWatch-PowerDemo) | 电源分轨与深睡实测数据 |
| [espressif/arduino-esp32](https://github.com/espressif/arduino-esp32) | 3.3.7 核心 |
| [espressif/esptool](https://github.com/espressif/esptool) | 烧写与镜像合并 |

## 关于 AI 参与

本项目的固件代码、调试脚本和文档由 AI（WorkBuddy）编写，硬件实测、现象反馈和验收由项目作者完成。寄存器取值、配置包来源与页地址均对照上游源码和 Bosch 文档核对，关键结论在代码注释中标注出处。
