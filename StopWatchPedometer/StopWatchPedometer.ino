// ============================================================
//  M5Stack StopWatch 计步器 / Pedometer   v2.0
//
//  硬件: ESP32-S3R8 + BMI270 + 466×466 CO5300 AMOLED + RX8130 + M5PM1
//
//  · 计步: BMI270 硬件计步器（feature engine），休眠时继续计数
//  · 休眠: 息屏静止后进入 LP2 深睡，每 20 s 静默醒来读一次计数，约 90 µA
//  · 界面: 贴边环形进度 / 今日数据 / 近 7 天历史 / 设置 / 时间 / 调试
//  · 操作: 黄键切页、蓝键息屏、两键同按 10 s 清零、电源键亮屏、双击关机、
//          左右滑动切页、上下滑动翻行、点按 ± 改设置
// ============================================================

#include <M5Unified.h>
#include "ped_core.h"
#include "ped_imu.h"
#include "ped_sleep.h"
#include "ped_ui.h"

// ------------------------------------------------------------
static uint32_t g_bootMs      = 0;
static bool     g_coldBoot    = true;
static uint32_t g_lastBattMs  = 0;
static int32_t  g_firstRender = -1;      // 首次出帧耗时，写进 NVS 供下次启动回读

// ------------------------------------------------------------
// 屏幕 / 背光
// ------------------------------------------------------------
// 完整最小上电序列：SLPOUT → 150ms → DISPON。
// 背景：M5GFX 初始化完成后，若面板因电池供电唤醒时电源轨未稳而把
// 初始化表整体打丢（面板仍在 sleep），单纯的 0x29 是无效的 —— 必须
// 先补 SLPOUT 并等足 150ms（CO5300 规格），再开显示。亮度由调用方
// 在本函数之后 setBrightness() 补上。
// 对已完成正常初始化的屏：sleep-out 态收 SLPOUT 是无害 no-op，
// 0x29 幂等，重复执行安全。
// 4 字节 QSPI 命令帧：02 00 cmd 00（dlen_16bit=0，单字节命令不会移位）。
static void panelDisplayOn()
{
    M5.Display.startWrite();
    M5.Display.writeCommand(0x02);
    M5.Display.writeCommand(0x00);
    M5.Display.writeCommand(0x11);   // SLPOUT
    M5.Display.writeCommand(0x00);
    M5.Display.endWrite();
    delay(150);                      // SLPOUT 稳定期（规格 ≥120ms）
    M5.Display.startWrite();
    M5.Display.writeCommand(0x02);
    M5.Display.writeCommand(0x00);
    M5.Display.writeCommand(0x29);   // DISPON
    M5.Display.writeCommand(0x00);
    M5.Display.endWrite();
}

static void screenOn()
{
    if (g_bright == 0) {
        // 运行期从不 SLPIN，这里也就不需要 SLPOUT —— 只把亮度拉回来。
        // （息屏窗口只做亮度 0，见 screenOff；深睡路径的面板 SLPIN 由
        //   唤醒时的 M5.begin 硬件复位+全初始化兜底。）
        M5.Display.setBrightness(g_cfg.bright);
        g_bright = g_cfg.bright;
        if (!M5.Touch.isEnabled()) M5.Touch.begin(&M5.Display);   // 恢复触摸
        g_forceRender = true;
    }
}

static void screenOff()
{
    if (g_bright != 0) {
        M5.Display.setBrightness(0);
        g_bright = 0;
        // 故意**不发 SLPIN**：息屏窗口只有几十秒，面板逻辑这点电流可忽略；
        // 而运行期 SLPIN/SLPOUT 循环正是"唤醒后弧光/点不亮"坏态的来源。
        // 整夜省电由深睡路径（enterDeepSleep 里的 Display.sleep）负责。
        M5.Touch.end();          // 熄屏后连触摸都不轮询：省电，也彻底断掉触摸唤醒
    }
}

static void touchActivity(uint32_t now)
{
    g_lastActiveMs = now;
    screenOn();
}

// ------------------------------------------------------------
// 电源 / 电量
// ------------------------------------------------------------
static void refreshPower()
{
    g_battery = (int)M5.Power.getBatteryLevel();
    uint16_t mv = 0;
    if (M5.Power.M5pm1.getBatteryVoltage(&mv) && mv > 2000) g_vbat = mv;
    g_usbPower = (((int)M5.Power.M5pm1.getPowerSource()) & 1) != 0;   // bit0 = 5VIN
}

// ------------------------------------------------------------
// 深睡
// ------------------------------------------------------------
static bool canDeepSleep()
{
    if (g_usbPower) return false;        // 插着 USB 不休眠，方便调试/充电
    if (!pedimu::ready) return false;    // 硬件计步不可用时保持常开
    if (g_rtc.failCnt >= 3) return false; // 静默通道连不上就放弃深睡，避免反复重启
    return true;
}

// 屏幕供电轨（L3B，IOE1 IO8）断电 —— 官方 L2 的"断电睡"。
// 官方 L2 睡前不做 SLPIN，而是 IOE1 factory_reset 释放 L3B_EN，整条屏轨掉电，
// 唤醒 = 面板全新上电；我们此前是"带电睡"（SLPIN 后 L3B 一直供着），
// 夜间漏电 3-6 mA 且唤醒时面板不是冷启动。
// GPIO_OUT(0x05) 位映射：bitN = IO(N+1)。M5GFX 初始化后的状态是 0b10011001
// （IO1/4/5/8 高），把 bit7(IO8) 清 0 → 0x19，其余位原样保持。
// 整字节直写，不依赖读回（M5.In_I2C 的读不可靠、写可用 —— 触摸 0xE5 同路实测）。
// L3B 还带扬声器/振动马达，深睡期间本就不工作，一起断电无害。
// 唤醒路径：板级恢复的触摸复位序列 bitOn 0x05 会重新拉高 IO8（L3B 回来），
// 加上 delay(120) 稳定期，面板冷启动时序裕量充足。
static bool screenRailOff()
{
    const bool ok = M5.In_I2C.writeRegister8(ADDR_IOE1, 0x05, 0x19, 100000);
    Serial.printf("[slp] screenRailOff(L3B)=%d\n", (int)ok);
    return ok;
}

static void enterDeepSleep()
{
    saveToday(true);
    cfgFlush();

    g_rtc.magic     = RTC_MAGIC;
    g_rtc.steps     = g_steps;
    g_rtc.run       = g_runSteps;
    g_rtc.activeSec = g_activeSec;
    g_rtc.dayKey    = g_dayKey;

    M5.Power.setVibration(0);
    M5.Display.setBrightness(0);
    g_bright = 0;
    M5.Display.sleep();            // 面板 SLPIN（若断轨失败，这是兜底低功耗态）
    pedsleep::railsOff();          // 尽力而为：lgfx 直连在完整启动态常失败，无害跳过
    screenRailOff();               // 断 L3B 屏轨 —— 面板整夜零电流，唤醒 = 冷启动（官方同款）
    // 触摸深睡（官方 L2 同款，0xE5=0x03，约省 0.5-1 mA）：睡死态下它不再应答
    // 0x15，唤醒路径由 pm1Recover 的 IOE1 硬复位序列救回（已实测验证）。
    // 必须用 M5.In_I2C 写 —— 完整启动状态下 lgfx 直连总线不可靠。
    M5.In_I2C.writeRegister8(ADDR_TOUCH, 0xE5, 0x03, 400000);

    pedimu::prepareSleep();
    pedsleep::quietPeripherals(true);

    // 等按键松开，避免长按导致醒-睡反复
    for (int i = 0; i < 40 && pedsleep::buttonHeld(); ++i) delay(50);

    pedsleep::enter(false);            // 不返回
}

// ------------------------------------------------------------
// RTC 内存与 NVS 合并
// ------------------------------------------------------------
static void mergeRtcState()
{
    if (g_rtc.magic != RTC_MAGIC) {
        g_rtc.magic     = RTC_MAGIC;
        g_rtc.steps     = g_steps;
        g_rtc.run       = g_runSteps;
        g_rtc.activeSec = g_activeSec;
        g_rtc.dayKey    = g_dayKey;
        g_rtc.hwInit    = 0;
        g_rtc.hwBase    = 0;
        g_rtc.hwRaw     = 0;
        g_rtc.wakes     = 0;
        g_rtc.vbat      = 0;
        g_rtc.lowBat    = 0;
        g_rtc.failCnt   = 0;
        return;
    }

    int32_t today = todayKey();
    if (today != 0 && g_rtc.dayKey != 0 && today > g_rtc.dayKey) {
        // 睡眠期间跨天：昨天的成绩入库，今日从 0 开始
        Serial.printf("[day] rollover(sleep) %ld -> %ld steps=%u\n",
                      (long)g_rtc.dayKey, (long)today, (unsigned)g_rtc.steps);
        histArchive(g_rtc.dayKey, g_rtc.steps, g_rtc.run);
        histSave();
        g_rtc.steps     = 0;
        g_rtc.run       = 0;
        g_rtc.activeSec = 0;
        g_steps     = 0;
        g_runSteps  = 0;
        g_activeSec = 0;
        bucketClear();
        memcpy(g_rtc.bktLvl, g_bkt.lvl, sizeof(g_bkt.lvl));
        g_nextMilestone = 1000;
    } else {
        if (g_rtc.steps     > g_steps)     g_steps     = g_rtc.steps;
        if (g_rtc.run       > g_runSteps)  g_runSteps  = g_rtc.run;
        if (g_rtc.activeSec > g_activeSec) g_activeSec = g_rtc.activeSec;
        // 睡眠期间的分时明细只存在 RTC 里，逐分钟取较高强度合并
        for (int i = 0; i < BKT_N; ++i) {
            const uint8_t a = bktGet(g_rtc.bktLvl, i);
            const uint8_t b = bktGet(g_bkt.lvl,    i);
            if (a > b) bktPut(g_bkt.lvl, i, a);
        }
    }
    if (g_rtc.vbat) g_vbat = g_rtc.vbat;

    if (today) {
        g_dayKey     = today;
        g_rtc.dayKey = today;
    }
}

// ------------------------------------------------------------
// 串口调试接口（USB CDC，任意波特率）
//   c        打印计数 / 活动状态 / 本次硬件步数
//   S        把当前帧原样 dump（先一行 "IMG w h n"，随后 n 字节 RGB565）
//   n / p    下一页 / 上一页
//   0..5     直接跳到某页
// ------------------------------------------------------------
static void dbgPrint()
{
    uint32_t c = 0, act = 0;
    if (pedimu::readSteps(&c, &act)) {
        Serial.printf("[cnt] chip=%u act=%u dbgAct=%u steps=%u hw=%u\n",
                      (unsigned)c, (unsigned)act, g_actCode,
                      (unsigned)g_steps, (unsigned)g_hwSteps);
    } else {
        Serial.println("[cnt] read fail");
    }
}

static void dbgFrame()
{
    uiRender(false);                    // 画进 canvas，不推屏
    const uint32_t n = (uint32_t)g_w * (uint32_t)g_h * 2u;
    Serial.printf("IMG %d %d %u\n", g_w, g_h, (unsigned)n);
    Serial.flush();

    const uint8_t *p = (const uint8_t *)g_canvas.getBuffer();
    uint32_t left = n;
    while (left) {
        const uint32_t chunk = (left > 4096u) ? 4096u : left;
        Serial.write(p, chunk);
        Serial.flush();
        p    += chunk;
        left -= chunk;
    }
}

// 串口对时：'T' + YYYYMMDDHHMM + '#'，写完立刻回读校验
static char g_setBuf[16];
static int  g_setLen = -1;

static int setField(int off, int n)
{
    char t[8];
    for (int i = 0; i < n; ++i) t[i] = g_setBuf[off + i];
    t[n] = 0;
    return atoi(t);
}

static void dbgPoll()
{
    while (Serial.available()) {
        const int ch = Serial.read();

        // 对时数据收集中：这一段的数字先交给缓冲区，别被单键命令吃掉
        if (g_setLen >= 0) {
            if (ch >= '0' && ch <= '9' && g_setLen < 12) { g_setBuf[g_setLen++] = (char)ch; continue; }
            if (ch != '#') continue;
            if (g_setLen == 12) {
                const int yy = setField(0, 4), mo = setField(4, 2), dd = setField(6, 2);
                const int hh = setField(8, 2), mi = setField(10, 2);
                if (k_valid(yy, mo, dd)) {
                    rtcApply(yy, mo, dd, hh, mi);
                    auto dt = M5.Rtc.getDateTime();
                    Serial.printf("RTC %04d-%02d-%02d %02d:%02d:%02d\n",
                                  dt.date.year, dt.date.month, dt.date.date,
                                  dt.time.hours, dt.time.minutes, dt.time.seconds);
                    g_forceRender = true;
                }
            }
            g_setLen = -1;
            continue;
        }

        if (ch == 'c') {
            dbgPrint();
        } else if (ch == 'S') {
            dbgFrame();
        } else if (ch == 'n') {
            pageNext(); g_forceRender = true;
        } else if (ch == 'p') {
            pagePrev(); g_forceRender = true;
        } else if (ch == 't') {
            g_timeMode ^= 1; g_forceRender = true;      // 对时页：日期/时间
        } else if (ch == 'u') {
            g_timeTab ^= 1; g_forceRender = true;       // 对时页：调时/调分
        } else if (ch >= '0' && ch <= '5') {
            if (ch - '0' == PG_TIME) enterTimePage();
            else { g_page = ch - '0'; g_setScroll = 0; g_listPx = 0.0f; }
            g_forceRender = true;
        } else if (ch == 'T') {
            g_setLen = 0;
        } else if (ch == 'I') {
            // 诊断：BMI270 真实配置 + 计数现场。排查"每步双计"时用。
            uint32_t raw = 0, act = 0;
            pedimu::readSteps(&raw, &act);
            Serial.printf("[imu] err=%u verify=%02X accconf=%02X accrange=%02X pwrctrl=%02X pwrconf=%02X stat=%02X param=%04X\n",
                          (unsigned)pedimu::dbgErr, (unsigned)pedimu::dbgVerify,
                          (unsigned)pedimu::dbgAccConf, (unsigned)pedimu::dbgAccRange,
                          (unsigned)pedimu::dbgPwrCtrl,
                          (unsigned)pedimu::dbgPwrConf, (unsigned)pedimu::dbgStat,
                          (unsigned)pedimu::dbgParam);
            Serial.printf("[imu] raw=%lu hwBase=%lu hwInit=%u g_rtc.steps=%lu g_steps=%lu act=%lu\n",
                          (unsigned long)raw, (unsigned long)g_rtc.hwBase,
                          (unsigned)g_rtc.hwInit, (unsigned long)g_rtc.steps,
                          (unsigned long)g_steps, (unsigned long)act);
        } else if (ch == 'x') {
            g_steps = 1234;                 // 调试：写一个固定步数并落盘
            saveToday(true);
            Serial.printf("[x] steps=1234 saved, nvs=%u\n",
                          (unsigned)g_prefs.getUInt("steps", 0));
        } else if (ch == 'y') {
            Serial.printf("[y] nvs steps=%u day=%d today=%d cold=%d\n",
                          (unsigned)g_prefs.getUInt("steps", 0),
                          (int)g_prefs.getInt("day", 0),
                          (int)todayKey(), (int)g_coldBoot);
        } else if (ch == 'z') {
            g_steps = 0; g_activeSec = 0;   // 调试：清零并落盘
            saveToday(true);
            Serial.println("[z] cleared");
        } else if (ch == 'H') {
            // 调试：写 6 天历史（今天沿用实时成绩），用于核对柱状图与逐日页
            static const uint32_t sSt[HIST_DAYS] = {5200, 8600, 3100, 12400, 7400, 9800, 0};
            static const uint32_t sRn[HIST_DAYS] = {0, 2100, 900, 5200, 1500, 3600, 0};
            int32_t base = todayKey();
            if (base <= 0) base = g_dayKey;
            memset(g_hist, 0, sizeof(g_hist));
            for (int i = 0; i < HIST_DAYS && base > 0; ++i) {
                const int32_t d = keyShift(base, -(HIST_DAYS - 1 - i));
                if (sSt[i]) histPush(d, sSt[i], sRn[i]);
            }
            histSave();
            Serial.printf("[H] history seeded base=%ld\n", (long)base);
            g_forceRender = true;
        } else if (ch == 'A') {
            // 调试：按当前时刻回推一段活动轨迹，用于核对时间轴外环
            bucketClear();
            const int c = bucketNow();
            struct { int a, b; uint8_t lv; } sg[] = {
                {-180, -150, 1}, {-150, -120, 2}, {-120, -95, 1},
                {-80,  -62,  1}, {-62,  -40,  2}, {-40,  -12, 1},
            };
            uint32_t walk = 0, run = 0;
            for (auto &s : sg) {
                const int a = c + s.a, b2 = c + s.b;
                for (int m = (a < 0 ? 0 : a); m < b2 && m < BKT_N; ++m)
                    bktPut(g_bkt.lvl, m, s.lv);
                if (s.lv == 1) walk += (uint32_t)(b2 - a) * 60;
                else           run  += (uint32_t)(b2 - a) * 60;
            }
            g_steps     = walk + run;
            g_runSteps  = run;
            g_activeSec = (walk + run) / 100;
            saveToday(true);
            Serial.printf("[A] timeline seeded cur=%d steps=%u run=%u\n",
                          c, (unsigned)g_steps, (unsigned)g_runSteps);
            g_forceRender = true;
        } else if (ch == 'k') {
            Serial.printf("[k] mem goal=%u h=%u w=%u br=%u snd=%u off=%u slp=%u dirty=%d\n",
                          (unsigned)g_cfg.goal, (unsigned)g_cfg.heightCm,
                          (unsigned)g_cfg.weightKg, (unsigned)g_cfg.bright,
                          (unsigned)g_cfg.sound, (unsigned)g_cfg.screenOffS,
                          (unsigned)g_cfg.sleepAfterS, (int)g_cfgDirty);
            Serial.printf("[k] nvs goal=%u h=%u w=%u br=%u snd=%u off=%u slp=%u\n",
                          (unsigned)g_prefs.getUInt  ("goal",   0),
                          (unsigned)g_prefs.getUShort("height", 0),
                          (unsigned)g_prefs.getUShort("weight", 0),
                          (unsigned)g_prefs.getUChar ("bright", 0),
                          (unsigned)g_prefs.getUChar ("sound",  0),
                          (unsigned)g_prefs.getUChar ("offs",   0),
                          (unsigned)g_prefs.getUChar ("slps",   0));
        } else if (ch == 'g') {
            rowAdjust(3, +1);               // 调试：模拟点一次「亮度 +」
            g_forceRender = true;
            Serial.printf("[g] bright=%u dirty=%d\n",
                          (unsigned)g_cfg.bright, (int)g_cfgDirty);
        } else if (ch == 'G') {
            rowAdjust(3, -1);               // 调试：模拟点一次「亮度 −」
            g_forceRender = true;
            Serial.printf("[G] bright=%u dirty=%d\n",
                          (unsigned)g_cfg.bright, (int)g_cfgDirty);
        } else if (ch == 'w') {
            Serial.printf("[w] usb=%d imu=%d fail=%d magic=%d tick=%us\n",
                          (int)g_usbPower, (int)pedimu::ready, (int)g_rtc.failCnt,
                          (int)(g_rtc.magic == RTC_MAGIC), (unsigned)SLEEP_TICK_S);
            Serial.printf("[w] wakes=%u steps=%u vbat=%umV pct=%d low=%d dirty=%d\n",
                          (unsigned)g_rtc.wakes, (unsigned)g_steps,
                          (unsigned)g_vbat, (int)g_battery,
                          (int)g_rtc.lowBat, (int)g_cfgDirty);
        } else if (ch == 'B') {
            clearToday();                   // 调试：等价于设置页「清零今日」
            Serial.println("[B] cleared today");
        } else if (ch == 'E') {
            g_page   = PG_SET;              // 调试：设置列表滚到底
            g_listPx = (float)((rowCount() - ROW_PAGE) * ROW_H);
            g_forceRender = true;
        } else if (ch == 'R') {
            Preferences bp;
            bp.begin("boot", true);
            Serial.printf("[R] prev cause=%d pm1=%02X render=%d bright=%d panel=%dx%d steps=%u\n",
                          bp.getInt("cause", -1), (unsigned)bp.getUChar("pm1", 0),
                          bp.getInt("render", -1), bp.getInt("bright", -1),
                          bp.getInt("pw", -1), bp.getInt("ph", -1),
                          (unsigned)bp.getUInt("steps", 0));
            bp.end();
        } else if (ch == 'K') {
            pedsleep::dumpKeyLevels();
        } else if (ch == 'L') {
            // 调试：只允许按键唤醒，验证 ext1 是否真的生效
            Serial.println("[L] keys-only sleep, press YELLOW or BLUE to wake");
            Serial.flush();
            M5.Display.setBrightness(0);
            g_rtc.steps     = g_steps;
            g_rtc.run       = g_runSteps;
            g_rtc.activeSec = g_activeSec;
            g_rtc.dayKey    = g_dayKey;
            pedsleep::enterKeysOnly();
        } else if (ch == 'N') {
            // 调试：完整模拟真实深睡（含外设收尾），8 秒定时唤醒后走完整启动
            Serial.println("[N] real L2 in 8s, then full boot");
            Serial.flush();
            g_rtc.steps     = g_steps;
            g_rtc.run       = g_runSteps;
            g_rtc.activeSec = g_activeSec;
            g_rtc.dayKey    = g_dayKey;
            g_rtc.dbgBoot   = 1;
            M5.Power.setVibration(0);
            M5.Display.setBrightness(0);
            g_bright = 0;
            pedimu::prepareSleep();
            pedsleep::quietPeripherals(true);
            esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
            esp_sleep_enable_timer_wakeup(8ULL * 1000000ULL);
            esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
            esp_deep_sleep_start();
        } else if (ch == 'W') {
            // 调试：强制进 L2，6 秒后定时唤醒并要求走完整启动（跳过静默通道）
            // 0x06 必须走外部 I2C 直读：M5.In_I2C 的端口映射在这里读不到真实值
            const uint8_t pc = lgfx::i2c::readRegister8(1, ADDR_PM1, 0x06, 400000).value_or(0);
            Serial.printf("[W] force L2 6s  pm1cfg=%02X src=%d icause=%d\n",
                          (unsigned)pc, (int)M5.Power.M5pm1.getPowerSource(),
                          (int)esp_sleep_get_wakeup_cause());
            Serial.flush();
            g_rtc.magic   = RTC_MAGIC;
            g_rtc.steps   = g_steps;
            g_rtc.run     = g_runSteps;
            g_rtc.activeSec = g_activeSec;
            g_rtc.dayKey  = g_dayKey;
            g_rtc.dbgBoot = 1;
            g_rtc.wakes   = 0;
            M5.Display.setBrightness(0);
            M5.Display.sleep();        // 与 enterDeepSleep 相同的睡前序列：面板 SLPIN
            pedsleep::railsOff();      // 尽力而为，失败无害
            screenRailOff();           // 断 L3B 屏轨：唤醒 = 面板冷启动（官方 L2 同款）
            M5.In_I2C.writeRegister8(ADDR_TOUCH, 0xE5, 0x03, 400000);   // 触摸深睡
            esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
            esp_sleep_enable_timer_wakeup(6ULL * 1000000ULL);
            esp_deep_sleep_start();
        } else if (ch == 'P') {
            // 调试：量化「画布放 PSRAM」与「画布放内部 RAM」的真实差距。
            // 两种写入方式分别测：大块顺序写（带宽受限）与零散小矩形（cache 受限）。
            // 后者模拟字模与抗锯齿描边的 read-modify-write，才是真正的痛点。
            constexpr int TW = 468, TH = 200;      // 468×200×2 = 187 KB
            M5Canvas tmp(&M5.Display);
            tmp.setPsram(false);                   // 关键：强制内部 RAM
            tmp.setColorDepth(16);
            if (!tmp.createSprite(TW, TH)) {
                Serial.printf("[P] 内部 RAM 187KB 申请失败, heap=%u\n",
                              (unsigned)ESP.getFreeHeap());
                return;
            }
            Serial.printf("[P] 内部RAM画布 ok, heap 用完剩 %u\n", (unsigned)ESP.getFreeHeap());

            struct R { uint32_t big, small; };

            auto bench = [&](M5Canvas &cv, R *out) {
                // 预热点：让 cache 进入稳定状态，避免测到冷启动抖动
                cv.fillRect(0, 0, TW, TH, C_TEXT);
                uint32_t t = micros();
                cv.fillRect(0, 0, TW, TH, C_BG);
                out->big = micros() - t;

                t = micros();
                for (int i = 0; i < 900; ++i) {
                    const int x = (i * 37) % (TW - 26);
                    const int y = (i * 53) % (TH - 26);
                    cv.fillRect(x, y, 26, 26, C_TEXT);
                }
                out->small = micros() - t;
            };

            R ps{}, ir{};
            bench(g_canvas, &ps);
            bench(tmp, &ir);
            tmp.deleteSprite();

            const float pb = (float)ps.big / 1000.0f, psml = (float)ps.small / 1000.0f;
            const float ib = (float)ir.big / 1000.0f, isml = (float)ir.small / 1000.0f;
            Serial.printf("[P] psram  big=%6.2fms small=%6.2fms\n", pb, psml);
            Serial.printf("[P] intram big=%6.2fms small=%6.2fms\n", ib, isml);
            Serial.printf("[P] 提速    big=%4.1fx small=%4.1fx\n", pb / ib, psml / isml);
            Serial.printf("[P] 整屏428KB: 内部RAM放不下(heap=%u)\n",
                          (unsigned)ESP.getFreeHeap());
        } else if (ch == 'V') {
            // 调试：把一帧的耗时拆成「清屏 / 绘制 / 推送」三段。
            // 判断"把像素缓冲搬到内部 RAM"值不值，就看「绘制」这一段有多大。
            auto drawPage = [](void) {
                switch (g_page) {
                    case PG_MAIN:  if (g_mainDetail) drawStats(); else drawMain(); break;
                    case PG_HIST:  if (g_histIdx < 0) drawHist(); else drawHistDay(g_histIdx); break;
                    case PG_SET:   drawSet();   break;
                    case PG_TIME:  drawTime();  break;
                    default:       drawDebugHw(); break;
                }
            };

            ui->fillScreen(C_BG);                    // 预热
            uint32_t t = micros();
            ui->fillScreen(C_BG);
            const uint32_t tFill = micros() - t;

            t = micros();
            drawPage();
            const uint32_t tDraw = micros() - t;

            t = micros();
            g_canvas.pushSprite(0, 0);
            const uint32_t tPush = micros() - t;

            const float total = (tFill + tDraw + tPush) / 1000.0f;
            Serial.printf("[V] page=%d  清屏 %.2f  绘制 %.2f  推送 %.2f ms  (共 %.2f)\n",
                          g_page, tFill / 1000.0f, tDraw / 1000.0f, tPush / 1000.0f, total);
            Serial.printf("[V] 绘制占比 %.0f%%  —— 这一段才是搬到内部RAM能省下的\n",
                          100.0f * (float)tDraw / (float)(tFill + tDraw + tPush));
        } else if (ch == 'F') {
            // 调试：直接量各条刷新路径的「单帧耗时」，换算理论帧率。
            // 不能靠"清屏+绘制+推送相加"推算 —— 那样会漏掉 DMA 启动、
            // 窗口设置、裁剪计算等固定开销，而它们在短帧里占比很大。
            constexpr int NP = 20;   // 纯推屏次数
            constexpr int NS = 10;   // 滑动帧次数
            constexpr int NF = 6;    // 完整帧次数
            const float AREA = (float)g_w * (float)g_h * 2.0f;

            // 1) 纯推屏：像素已在 PSRAM 里，只走 QSPI 发出去。这是推送的硬地板。
            g_canvas.pushSprite(0, 0);
            g_canvas.pushSprite(0, 0);
            uint32_t t = micros();
            for (int i = 0; i < NP; ++i) g_canvas.pushSprite(0, 0);
            const float tPush = (float)(micros() - t) / 1000.0f / (float)NP;

            // 2) 滑动单帧：uiSlide 的一次迭代 —— 两张画布各推一条裁剪带。
            //    两条带的宽度互补，总面积≈整屏，但多了一次窗口设置的开销。
            g_canvasPrev.pushSprite(-g_w / 2, 0);
            g_canvas.pushSprite(g_w / 2, 0);
            t = micros();
            for (int i = 0; i < NS; ++i) {
                g_canvasPrev.pushSprite(-g_w / 2, 0);
                g_canvas.pushSprite(g_w / 2, 0);
            }
            const float tSlide = (float)(micros() - t) / 1000.0f / (float)NS;

            // 3) 完整一帧：清屏 + 绘制 + 推屏，这是 uiRender(true) 的真实成本。
            const int pg0 = g_page;
            float tFull = 0.0f;
            for (int i = 0; i < NF; ++i) {
                g_page = pg0;
                t = micros();
                uiRender(true);
                tFull += (float)(micros() - t) / 1000.0f;
            }
            tFull /= (float)NF;

            // 4) 一整个翻页过渡（7 帧）
            const float tSlideAll = tSlide * 7.0f;

            Serial.printf("[F] 推屏  %.2f ms -> %5.1f fps  (%.2f MB/s)\n",
                          tPush, 1000.0f / tPush, AREA / tPush / 1000.0f);
            Serial.printf("[F] 滑动帧 %.2f ms -> %5.1f fps  (7帧=%.0f ms)\n",
                          tSlide, 1000.0f / tSlide, tSlideAll);
            Serial.printf("[F] 完整帧 %.2f ms -> %5.1f fps  (page=%d)\n",
                          tFull, 1000.0f / tFull, pg0);
            Serial.printf("[F] 硬件地板: QSPI 4线@80MHz=40MB/s, 438KB=%5.2f ms -> %5.1f fps\n",
                          AREA / 40000.0f, 40000.0f / AREA);
            g_page = pg0;
            g_canvasReady = true;
        } else if (ch == 'b') {
            // 调试：同页对比「整屏渲染+整屏推送」与「条带渲染+条带推送」。
            // 先用串口切到设置页（'3'）或对时页（'4'）再发 b，那里才有条带收益。
            // 注意不能用大写 B —— 那是清零步数。
            const int save = g_page;
            constexpr int NP = 6;
            float tFull = 0.0f, tBand = 0.0f;
            for (int i = 0; i < NP; ++i) {
                const uint32_t t = micros();
                uiRender(true);
                tFull += (float)(micros() - t) / 1000.0f;
            }
            for (int i = 0; i < NP; ++i) {
                const uint32_t t = micros();
                uiRenderBand(LIST_VP0, LIST_VP1);
                tBand += (float)(micros() - t) / 1000.0f;
            }
            tFull /= (float)NP;
            tBand /= (float)NP;
            Serial.printf("[B] page=%d  整屏 %.2f ms (%.1f fps)  条带94-372 %.2f ms (%.1f fps)  提速 %.2fx\n",
                          save, tFull, 1000.0f / tFull, tBand, 1000.0f / tBand, tFull / tBand);
            g_page = save;
            g_canvasReady = true;
        } else if (ch == 'Z') {
            // 调试：真的去申请「整屏内部 RAM 画布」，16bit 与 8bit 各试一次。
            // 顺便量出 8bit 方案的 清屏/绘制/推送 耗时，判断值不值。
            auto drawPage = [](void) {
                switch (g_page) {
                    case PG_MAIN:  if (g_mainDetail) drawStats(); else drawMain(); break;
                    case PG_HIST:  if (g_histIdx < 0) drawHist(); else drawHistDay(g_histIdx); break;
                    case PG_SET:   drawSet();   break;
                    case PG_TIME:  drawTime();  break;
                    default:       drawDebugHw(); break;
                }
            };

            Serial.printf("[Z] 需要 %u B, 可用堆 %u B\n",
                          (unsigned)(g_w * g_h * 2), (unsigned)ESP.getFreeHeap());

            // --- 16bit ---
            {
                M5Canvas c(&M5.Display);
                c.setPsram(false);
                c.setColorDepth(16);
                const bool ok = c.createSprite(g_w, g_h);
                Serial.printf("[Z] 16bit 内部RAM: %s\n", ok ? "成功" : "失败(放不下)");
                if (ok) {
                    const uint32_t t = micros();
                    c.fillRect(0, 0, g_w, g_h, C_BG);
                    Serial.printf("[Z]   清屏 %.2f ms\n", (micros() - t) / 1000.0f);
                    c.deleteSprite();
                }
            }

            // --- 8bit ---
            {
                M5Canvas c(&M5.Display);
                c.setPsram(false);
                c.setColorDepth(8);
                if (!c.createSprite(g_w, g_h)) {
                    Serial.println("[Z] 8bit 内部RAM: 失败");
                    return;
                }
                Serial.printf("[Z] 8bit 内部RAM: 成功, 剩 %u B\n", (unsigned)ESP.getFreeHeap());

                M5Canvas *save = ui;
                ui = &c;

                uint32_t t = micros();
                c.fillScreen(C_BG);
                const uint32_t tFill = micros() - t;

                t = micros();
                drawPage();
                const uint32_t tDraw = micros() - t;

                t = micros();
                c.pushSprite(0, 0);          // 8bit -> 16bit 面板, 含调色板转换
                const uint32_t tPush = micros() - t;

                ui = save;
                Serial.printf("[Z] 8bit 清屏 %.2f  绘制 %.2f  推送 %.2f ms (共 %.2f)\n",
                              tFill / 1000.0f, tDraw / 1000.0f, tPush / 1000.0f,
                              (tFill + tDraw + tPush) / 1000.0f);
                c.deleteSprite();
                g_forceRender = true;        // 恢复 16bit 正常显示
            }
        }
    }
}

// ------------------------------------------------------------
// 计步同步
// ------------------------------------------------------------
static void syncHardwareSteps()
{
    if (!pedimu::ready) { g_imuOk = false; return; }

    uint32_t c = 0, act = 0;
    if (!pedimu::readSteps(&c, &act)) { g_imuOk = false; return; }
    g_imuOk = true;
    g_actCode = (uint8_t)act;

    const uint32_t d = hwAccumulate(c);
    if (d) {
        // 本秒的步态由硬件给出，按状态把这段增量归入跑步或步行。
        // 状态取自读取瞬间，与实际步态最多差一个采样周期。
        const bool isRun = (g_actCode == pedimu::ACT_RUN);
        if (isRun) g_runSteps += d;

        // [stp] 诊断：raw=硬件计数器现值 d=本次增量。正常走路 raw 每次 +1；
        // 若 raw 每次 +2 → BMI270 在硬件层双计（参数/配置问题）；
        // 若 raw +1 但 g 连跳 +2 → 软件重复累加（不可能，单路径）。
        Serial.printf("[stp] raw=%lu d=%lu g_steps=%lu act=%lu run=%lu\n",
                      (unsigned long)c, (unsigned long)d,
                      (unsigned long)(g_steps + d), (unsigned long)act,
                      (unsigned long)g_runSteps);
        stepsAdd(d, isRun ? 2 : 1);
        g_hwSteps += d;
    }
}

// M5.begin 之前抢读到的 BMI270 硬件计数器（用于挽救重启窗口内丢失的步数）
static uint32_t g_preBootRaw = 0;

// ------------------------------------------------------------
void setup()
{
    g_bootMs = millis();

    Serial.begin(115200);
    Serial.printf("\n[ped] reset=%d heap=%u psram=%u\n",
                  (int)esp_reset_reason(), (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getPsramSize());

    // 回读上一次运行留下的现场：唤醒后是否成功出帧、亮度、PM1 配置
    {
        Preferences bp;
        bp.begin("boot", true);
        Serial.printf("[prev] cause=%d pm1=%02X render=%d bright=%d steps=%u\n",
                      bp.getInt("cause", -1), (unsigned)bp.getUChar("pm1", 0),
                      bp.getInt("render", -1), bp.getInt("bright", -1),
                      (unsigned)bp.getUInt("steps", 0));
        bp.end();
    }

    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

    if (cause != ESP_SLEEP_WAKEUP_UNDEFINED) {
        // 深睡唤醒：先试"静默通道"（只读计数、不点屏），成功则直接再睡
        g_coldBoot = false;
        if (cause == ESP_SLEEP_WAKEUP_TIMER && g_rtc.magic == RTC_MAGIC && !g_rtc.dbgBoot) {
            const int r = pedsleep::silentTick();   // 正常情况下不返回
            if (r == 2) g_rtc.failCnt++;            // 静默通道真的连不上
            else        g_rtc.failCnt = 0;          // 按键被按住：正常走完整启动
        } else if (g_rtc.magic == RTC_MAGIC) {
            g_rtc.failCnt = 0;      // 按键唤醒能走完全流程，说明硬件正常
        }
        g_rtc.dbgBoot = 0;

        // 唤醒启动：先把 CST820 唤醒，保证 M5.begin() 里的板卡探测能认出 StopWatch
        const bool tok = pedsleep::reviveTouch();
        Serial.printf("[boot] reviveTouch=%d\n", (int)tok);
    }

    // ---- 完整启动 ----
    // 板级恢复，必须在 M5.begin() 之前。
    //
    // M5GFX 的板卡探测顺序是：先在总线上找 0x32/0x68/0x15（有 CST820 才算 StopWatch），
    // 探测成功之后才 _check_m5pm1/_check_m5ioe1，然后才通过 M5IOE1 的 IO4 给触摸做硬件复位。
    // 而 CST820 一旦被写进深睡（0xE5=0x03）就**不再应答 0x15**，于是探测失败 → board 保持 0
    // → 永远不会执行到那次硬件复位 → 死锁。PM1 进入 I2C idle sleep 时同理。
    // 两个器件都在常电域、深睡不断电，所以坏状态能跨复位存活，只有拔电池才恢复 ——
    // 这正是"黑屏、无响应、只能双击电源键关机再开机"的成因。
    //
    // 对策：开机先自己把三者救活，再交给 M5GFX。
    {
        constexpr int BUS = 1;
        lgfx::i2c::init(BUS, PIN_I2C_SDA, PIN_I2C_SCL);
        delay(3);

        // 1) PM1：0x09 是 I2C idle sleep 开关，0x0A 是看门狗，都清 0。
        //    **不要**整字节覆写 0x06(PWR_CFG)：那是 CHG/DCDC/LDO/BOOST 使能位；
        //    但 BOOST(bit3)/DCDC(bit1)/LDO(bit2) 必须 bitOn 回来 —— 睡前
        //    railsOff() 关过 BOOST，而 M5GFX 初始化面板时压轨必须已在供电。
        const bool w09 = lgfx::i2c::writeRegister8(BUS, ADDR_PM1, 0x09, 0x00, 0, 100000).has_value();
        const bool w0a = lgfx::i2c::writeRegister8(BUS, ADDR_PM1, 0x0A, 0x00, 0, 100000).has_value();
        constexpr uint8_t RAILS = 0x02 | 0x04 | 0x08;   // DCDC|LDO|BOOST
        uint8_t c06 = 0;
        const bool r06 = lgfx::i2c::readRegister(BUS, ADDR_PM1, 0x06, &c06, 1, 100000).has_value();
        const bool w06 = r06 && ((c06 & RAILS) == RAILS ||
                                 lgfx::i2c::writeRegister8(BUS, ADDR_PM1, 0x06,
                                                           (uint8_t)(c06 | RAILS), 0, 100000).has_value());
        // 2) IOE1：0x23 同理，是它的 I2C sleep 开关。
        const bool w23 = lgfx::i2c::writeRegister8(BUS, ADDR_IOE1, 0x23, 0x00, 0, 100000).has_value();

        // 3) CST820：先看应答，睡死了就照抄 M5GFX 的官方复位序列用电平复位硬拉回来
        //    （软件命令 0xE5 在深睡态已经写不进去，必须走 IOE1 的 IO4 = TP RST）。
        //    序列与 M5GFX.cpp StopWatch 初始化逐行一致：pin1/3/4/5/8 一起配推挽输出、
        //    pin1/4/5/8 一起拉高（pin5 顺带复位 OLED，此时面板未初始化，无害）、
        //    pin4/5 拉低 8 ms 再释放。之前只动 pin4 的简化版实测救不回来。
        uint8_t tp = 0xFF;
        bool tpAck = lgfx::i2c::readRegister(BUS, ADDR_TOUCH, 0xE5, &tp, 1, 400000).has_value();
        if (!tpAck || tp == 0x03) {
            lgfx::i2c::writeRegister8(BUS, ADDR_IOE1, 0x23, 0x00, 0, 100000);      // 清 IOE1 I2C sleep
            lgfx::i2c::bitOff(BUS, ADDR_IOE1, 0x13, 0b10011101, 100000);           // 推挽
            lgfx::i2c::bitOn (BUS, ADDR_IOE1, 0x03, 0b10011101, 100000);           // 输出
            lgfx::i2c::bitOn (BUS, ADDR_IOE1, 0x05, 0b10011001, 100000);           // HIGH 1,4,5,8
            delay(10);
            lgfx::i2c::bitOff(BUS, ADDR_IOE1, 0x05, 0b00011000, 100000);           // LOW 4,5：复位 TP+OLED
            delay(8);
            lgfx::i2c::bitOn (BUS, ADDR_IOE1, 0x05, 0b00011000, 100000);           // 释放
            delay(60);                                                             // 等 CST820 固件加载完
            tpAck = lgfx::i2c::readRegister(BUS, ADDR_TOUCH, 0xE5, &tp, 1, 400000).has_value();
        }
        if (tpAck && tp == 0x03) {
            lgfx::i2c::writeRegister8(BUS, ADDR_TOUCH, 0xE5, 0x00, 0, 400000);
            delay(12);
            tpAck = lgfx::i2c::readRegister(BUS, ADDR_TOUCH, 0xE5, &tp, 1, 400000).has_value();
        }

        // 4) 打印探测名单四个地址的应答（bit0..3 = 0x32/0x68/0x15/0x50），
        //    出问题时这一行就能区分是"触摸没醒"还是"PM1 没醒"。
        uint32_t mask = 0;
        const uint8_t probe[] = {0x32, 0x68, 0x15, 0x50};
        for (int i = 0; i < 4; ++i) {
            if (lgfx::i2c::beginTransaction(BUS, probe[i], 100000).has_value()) mask |= (1u << i);
            lgfx::i2c::endTransaction(BUS);
        }
        Serial.printf("[st] bus mask=%02X (32/68/15/50) pm1=%d%d%d cfg=%02X->%02X ioe1=%d tp=%02X ack=%d\n",
                      (unsigned)mask, (int)w09, (int)w0a, (int)w06,
                      (unsigned)c06, (unsigned)(c06 | RAILS), (int)w23,
                      (unsigned)tp, (int)tpAck);

        // 5) 电源轨稳定期。官方 HAL 的顺序是 i2c → pmic → ioe → delay(50) → display_init：
        //    屏幕供电走 L3B 轨（IOE1 IO8），电池唤醒时从 bitOn 到轨压建立需要时间，
        //    面板初始化若撞上轨压爬升期，初始化命令会整体打丢（弧光/黑屏根因）。
        delay(120);
    }

    Serial.println("[st] cfgLoad");
    cfgLoad();
    Serial.println("[st] m5cfg");
    // loadToday() 要等 M5.begin() 之后：RTC 没起来时 todayKey() 返回 0，
    // 跨天判断和 g_dayKey 都会失效。

    // ★ 抢读 BMI270 硬件计数器：M5.begin 会软复位 BMI270（计数器清零），
    //   而 RTC 基线 hwBase 只记录到最后一次静默存盘（深睡下最长 60s 前）。
    //   期间走过的步数必须在此读走，否则启动后与基线做差时被吞掉
    //   （实测：电池下走 20 步、60s 内插回 USB → 步数纹丝不动）。
    g_preBootRaw = 0;
    {
        pedsleep::i2cOpen();
        delay(2);
        uint8_t pg = 0, b[4] = { 0, 0, 0, 0 };
        if (pedsleep::rawWr(ADDR_IMU, 0x2F, pg) &&
            pedsleep::rawRd(ADDR_IMU, 0x30, b, 4)) {
            g_preBootRaw = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
        }
        Serial.printf("[ped] preBootRaw=%lu\n", (unsigned long)g_preBootRaw);
        Serial.flush();
    }

    auto cfg = M5.config();
    cfg.internal_imu    = true;
    cfg.internal_rtc    = true;
    cfg.internal_spk    = (g_cfg.sound >= 2);   // 只有需要提示音时才初始化编解码器
    cfg.internal_mic    = false;
    cfg.output_power    = true;
    cfg.serial_baudrate = 115200;
    Serial.println("[st] M5.begin ...");
    Serial.flush();
    M5.begin(cfg);
    Serial.println("[st] M5.begin done");
    Serial.printf("[ped] M5.begin panel=%dx%d dma=%u\n",
                  (int)M5.Display.width(), (int)M5.Display.height(),
                  (unsigned)ESP.getFreeHeap());
    Serial.printf("[boot] cause=%d cold=%d fail=%u pm1=%02X src=%d\n",
                  (int)cause, (int)g_coldBoot, (unsigned)g_rtc.failCnt,
                  (unsigned)M5.In_I2C.readRegister8(ADDR_PM1, 0x06, 400000),
                  (int)M5.Power.M5pm1.getPowerSource());

    C_BG     = rgb(0, 0, 0);
    C_TRACK  = rgb(26, 28, 32);
    C_TEXT   = rgb(240, 243, 248);
    C_DIM    = rgb(138, 148, 160);
    C_WARN   = rgb(255, 96, 76);
    C_GOLD   = rgb(255, 198, 62);
    C_BTN    = rgb(38, 42, 48);
    applyTheme();          // C_ACCENT / C_RUN 由设置里的色相派生

    if (!POWER_LED_ON) {
        M5.Power.M5pm1.setLedEnLevel(false);
    }

    g_w  = M5.Display.width();
    g_h  = M5.Display.height();
    if (g_w <= 0 || g_h <= 0) { g_w = g_h = 466; }
    g_cx = g_w / 2;
    g_cy = g_h / 2;

    // 脏区默认覆盖全屏：此时绘制裁剪判断与改造前等价（只多一次比较）
    g_bandY0 = 0;
    g_bandY1 = g_h;

    g_canvas.setPsram(true);
    g_canvas.setColorDepth(16);
    bool cvOk = g_canvas.createSprite(g_w, g_h);

    g_canvasPrev.setPsram(true);
    g_canvasPrev.setColorDepth(16);
    g_canvasPrevReady = g_canvasPrev.createSprite(g_w, g_h);

    Serial.printf("[ped] canvas %dx%d ok=%d prev=%d psramfree=%u\n",
                  g_w, g_h, (int)cvOk, (int)g_canvasPrevReady,
                  (unsigned)ESP.getFreePsram());

    g_bright = g_cfg.bright;
    M5.Display.setBrightness(g_cfg.bright);

    // 面板初始化已稳定，补一发 DISPON（0x29），兜住初始化表里
    // "SLPOUT 后 0ms 就发 0x29" 的时序竞争。见 panelDisplayOn() 注释。
    panelDisplayOn();
    M5.Display.setBrightness(g_cfg.bright);

    // 注意：这里**不要**再做 sleep()/wakeup() 的"保险舞步"。
    // M5.begin 内部已经对 OLED 做了硬件复位（IOE1 pin5 拉低 8ms）并跑完整
    // 初始化命令表（结尾 0x29 DISPON），面板状态是干净的。实测在初始化后
    // 立刻 SLPIN 会与面板内部的电源/伽马稳定过程竞争，偶发把面板打进坏态：
    // 微弱局部辉光、软件 SLPIN/SLPOUT 都救不回来，只有下次硬件复位（=重启
    // 或深睡唤醒）才能清除 —— 对应"唤醒后屏幕顶端一点圆弧光、息屏无效、
    // 按键有声音震动但屏不亮、过一阵自己恢复"的现象。

    // RTC 已就绪，这时恢复今日成绩并做跨天判断才准确
    loadToday();
    bucketTick();
    Serial.printf("[ped] loadToday steps=%u day=%d today=%d\n",
                  (unsigned)g_steps, (int)g_dayKey, (int)todayKey());

    // BMI270：写步数算法参数 + 打开硬件步数引擎
    g_imuOk = pedimu::begin();
    Serial.printf("[imu] chip=%02X stat=%02X acc=%02X ctrl=%02X pconf=%02X verify=%d err=%d\n",
                  pedimu::dbgChip, pedimu::dbgStat, pedimu::dbgAccConf,
                  pedimu::dbgPwrCtrl, pedimu::dbgPwrConf,
                  pedimu::dbgVerify, pedimu::dbgErr);
    Serial.printf("[imu] mode=%s err=%d\n",
                  pedimu::mode == pedimu::MODE_PHONE ? "PHONE" : "WRIST",
                  (int)pedimu::dbgErr);
    Serial.printf("[imu] pcfg ");
    for (int i = 0; i < 16; ++i) Serial.printf("%02X ", pedimu::dbgPage6[i]);
    Serial.printf("\n[imu] page0=%02X %02X %02X %02X\n",
                  pedimu::dbgPage0[0], pedimu::dbgPage0[1],
                  pedimu::dbgPage0[2], pedimu::dbgPage0[3]);

    refreshPower();
    mergeRtcState();

    // 合并 preBoot 抢读：与 RTC 基线 hwBase 做差 = 最后一次静默存盘后走过的步数。
    // 若 M5.begin 没有复位计数器（计数器仍 ≈ preBootRaw），差值≈0，天然防双计。
    if (pedimu::ready && g_preBootRaw) {
        const uint32_t dPre = hwAccumulate(g_preBootRaw);
        if (dPre) {
            Serial.printf("[ped] preBoot merge +%lu\n", (unsigned long)dPre);
            stepsAdd(dPre);
            g_hwSteps += dPre;
        }
    }

    // 冷启动后 BMI270 的计数器从 0 开始，重建基线；
    // 深睡唤醒时计数器没复位，保留原基线继续累加
    if (pedimu::ready && (g_coldBoot || !g_rtc.hwInit)) {
        syncHardwareSteps();       // 建立基线，本次返回 0
    }

    g_lastActiveMs = millis();
    g_lastStepMs   = millis();

    // 冷启动才显示开机画面（按键唤醒直接进主界面）
    if (g_coldBoot) {
        ui->fillScreen(C_BG);
        textC(g_cx, g_cy - 70, "计步器", F_H1, C_TEXT);
        textC(g_cx, g_cy - 10, "M5Stack StopWatch", F_SM, C_DIM);
        if (!g_imuOk) textC(g_cx, g_cy + 44, "IMU 初始化失败", F_L, C_WARN);
        ui->pushSprite(0, 0);
        M5.Power.setVibration(160);
        delay(60);
        M5.Power.setVibration(0);
    }

    g_lastBattMs = millis();
    g_forceRender = true;
    Serial.printf("[ped] setup done %u ms\n", (unsigned)millis());
}

// ------------------------------------------------------------
void loop()
{
    const uint32_t now = millis();
    const bool interactable = (now - g_bootMs) > 650;   // 开机/唤醒后屏蔽误触

    M5.update();
    dbgPoll();
    cfgService();          // 设置项静置 700 ms 后落盘

    // 插着 USB 时每 3 s 打一行计数，方便观察是否在走
    static uint32_t lastDbgMs = 0;
    if (g_usbPower && now - lastDbgMs > 3000) {
        lastDbgMs = now;
        dbgPrint();
    }

    // 启动 2.5 s 后把本次现场写进 NVS，供下次启动回读（唤醒诊断用）
    static bool bootMarked = false;
    if (!bootMarked && now - g_bootMs > 2500) {
        bootMarked = true;
        Preferences bp;
        bp.begin("boot", false);
        bp.putInt  ("cause",  (int)esp_sleep_get_wakeup_cause());
        bp.putUChar("pm1",    M5.In_I2C.readRegister8(ADDR_PM1, 0x06, 400000));
        bp.putInt  ("render", (int)g_firstRender);
        bp.putInt  ("bright", (int)g_bright);
        bp.putUInt ("steps",  g_steps);
        bp.putInt  ("pw",     g_w);
        bp.putInt  ("ph",     g_h);
        bp.end();
    }

    bool redraw = false;

    // 脏区只属于「本帧触摸」：上一帧没消费掉的标记一律作废，
    // 避免残留到步数变化等全局触发帧，把本该全屏的重绘错窄成条带。
    uiClearBand();

    // ---------- 触摸 ----------
    // 熄屏后一律不响应触摸：不亮屏、不下发手势 —— 只有按键能唤醒。
    // 这样做既防口袋/包里的误触，也省掉熄屏期间每帧的触摸读取。
    static bool swallowing = false;
    if (g_bright > 0) {
        auto td = M5.Touch.getDetail(0);
        if (interactable) {
            if (td.isPressed()) {
                touchActivity(now);         // 手指在屏上就不该自动息屏
                if (!swallowing) {
                    if (uiTouch(now)) redraw = true;
                }
            } else if (swallowing) {
                swallowing = false;     // 丢弃唤醒那一下的抬手
            } else {
                if (uiTouch(now)) redraw = true;
            }
        }
    } else {
        swallowing = true;
        uiTouchAbort();                 // 熄屏瞬间把未完成的手势清干净
    }

    // ---------- 按键 ----------
    if (interactable) {
        static bool aLong = false, bLong = false, wakeConsumed = false;

        const bool anyDown = M5.BtnA.isPressed() || M5.BtnB.isPressed();
        if (g_bright == 0 && anyDown) {
            touchActivity(now);        // 黑屏时任意按键只负责亮屏，不执行原动作
            wakeConsumed = true;
            redraw = true;
        }

        // 黄键长按 1.2 s：回到主页
        if (M5.BtnA.pressedFor(1200) && !aLong) {
            aLong = true;
            if (g_page != PG_MAIN || g_mainDetail || g_histIdx >= 0) {
                g_page = PG_MAIN; g_setScroll = 0;
                g_mainDetail = false; g_histIdx = -1;
                redraw = true;
            }
            touchActivity(now);
        }
        if (M5.BtnA.wasReleased()) {
            if (!aLong && !wakeConsumed && g_bright > 0) {   // 短按：下一页
                pageNext();
                redraw = true;
            }
            aLong = false;
        }

        // 蓝键短按息屏；长按不执行动作，清零改由两键同按触发
        if (M5.BtnB.pressedFor(900) && !bLong) {
            bLong = true;
            touchActivity(now);
        }
        if (M5.BtnB.wasReleased()) {
            if (!bLong && !wakeConsumed) {                   // 短按：立即息屏
                screenOff();
                g_lastActiveMs = now - (uint32_t)g_cfg.screenOffS * 1000u;
                redraw = true;
            }
            bLong = false;
        }

        // 黄键与蓝键同时按住 10 s：清零当日成绩。
        // 单键长按曾用于清零，误触概率高，改为组合键 + 10 s。
        static uint32_t bothMs   = 0;
        static bool     bothDone = false;
        if (M5.BtnA.isPressed() && M5.BtnB.isPressed()) {
            if (bothMs == 0) bothMs = now;
            if (!bothDone && now - bothMs >= 10000) {
                bothDone = true;
                clearToday();            // 连同硬件计数基准一起归零，否则会立刻加回旧步数
                cadenceClear();
                feedback(1);
                redraw = true;
            }
        } else {
            bothMs   = 0;
            bothDone = false;
        }

        if (!anyDown) wakeConsumed = false;

        // ---------- 电源键：单击亮屏 / 双击关机 ----------
        // M5PM1 的 PEK 中断未使能，getKeyState() 读 IRQ_STATUS3 的锁存位，
        // 恒为 0，本段不产生动作。双击关机与长按进入下载模式由 M5PM1 自身响应。
        static uint32_t pekLast = 0, pekPoll = 0;
        static bool     pekArmed = false;
        if (now - pekPoll >= 40) {
            pekPoll = now;
            if (M5.Power.getKeyState()) {
                touchActivity(now);
                if (pekArmed && (now - pekLast >= 120) && (now - pekLast <= 800)) {
                    pekArmed = false;
                    saveToday(true);
                    cfgFlush();
                    M5.Power.setVibration(200);
                    delay(120);
                    M5.Power.setVibration(0);
                    M5.Display.setBrightness(0);
                    delay(150);
                    M5.Power.powerOff();
                    delay(300);
                    M5.Display.setBrightness(g_cfg.bright);
                } else {
                    pekArmed = true;
                    pekLast  = now;
                }
            }
        }
        if (pekArmed && (now - pekLast > 800)) pekArmed = false;
    }

    // ---------- 计步：每秒读一次 BMI270 硬件计数器 ----------
    static uint32_t lastPoll = 0;
    if (now - lastPoll >= 1000) {
        lastPoll = now;
        bucketTick();                       // 先刷新时段，本秒的步数才记对格
        const uint32_t before = g_steps;
        syncHardwareSteps();
        cadencePush(g_steps - before);
    }

    // 活跃时长
    static uint32_t activeTick = 0;
    if (now - activeTick >= 1000) {
        activeTick = now;
        if (now - g_lastStepMs < 5000) g_activeSec++;
    }

    // 静止一段时间后步频归零
    if (now - g_lastStepMs > 5000 && g_cadence != 0) cadenceClear();

    // 震动关断（脉冲到期就停，避免马达长鸣）
    vibService();

    // ---------- 电量 / 跨天 ----------
    if (now - g_lastBattMs > 30000) {
        g_lastBattMs = now;
        refreshPower();
        rolloverIfNeeded();
        saveToday();
    }

    // ---------- 自动息屏 → 深睡 ----------
    const uint32_t idle = now - g_lastActiveMs;
    if (g_bright > 0 && idle > (uint32_t)g_cfg.screenOffS * 1000u) {
        screenOff();
    }
    if (g_bright == 0 &&
        idle > ((uint32_t)g_cfg.screenOffS + g_cfg.sleepAfterS) * 1000u &&
        canDeepSleep()) {
        enterDeepSleep();   // 不返回
    }

    // ---------- 渲染 ----------
    static int32_t  lastSteps = -1;
    static int      lastPage  = -1;
    static int      lastMin   = -1;
    static uint32_t lastCad   = 0xFFFFFFFFu;
    static uint32_t lastFrame = 0;

    int minute = -1;
    if (M5.Rtc.isEnabled()) minute = M5.Rtc.getDateTime().time.minutes;

    bool need = redraw || g_forceRender ||
                g_steps != (uint32_t)lastSteps || g_page != lastPage ||
                minute != lastMin || g_cadence != lastCad;

    if (g_page == PG_DEBUG && now - lastFrame > 150) need = true;

    if (need && g_bright > 0) {
        const int prevPageIdx = lastPage;
        lastSteps = (int32_t)g_steps;
        lastPage  = g_page;
        lastMin   = minute;
        lastCad   = g_cadence;
        lastFrame = now;
        g_forceRender = false;

        const bool slide = (prevPageIdx >= 0 && prevPageIdx != g_page &&
                            g_canvasPrevReady && g_canvasReady);
        int dir = 1;
        if (prevPageIdx == PG_CYCLE_MAX && g_page == PG_MAIN)      dir =  1;
        else if (prevPageIdx == PG_MAIN && g_page == PG_CYCLE_MAX) dir = -1;
        else                                                       dir = (g_page > prevPageIdx) ? 1 : -1;

        const uint32_t t0 = millis();
        if (slide) {
            uiSnapshot();          // 旧页搬进 prev，再画新页
            uiRender(false);
            uiSlide(dir);
        } else if (g_dirtyY0 < g_dirtyY1) {
            // 触摸/滚轮明确标了脏区：只重绘并推送那一条。
            // 官方 UserDemo 的 LV_DISPLAY_RENDER_MODE_PARTIAL 就是这个思路。
            uiRenderBand(g_dirtyY0, g_dirtyY1);
            uiClearBand();
        } else {
            uiRender(true);
        }
        g_canvasReady = true;
        static bool tShown = false;
        if (!tShown) {
            tShown = true;
            g_firstRender = (int32_t)(millis() - t0);
            Serial.printf("[ped] first render %u ms (page %d)\n",
                          (unsigned)g_firstRender, g_page);
        }
    } else if (g_bright == 0) {
        lastSteps = (int32_t)g_steps;
        lastPage  = g_page;
        lastMin   = minute;
        lastCad   = g_cadence;
        g_forceRender = true;      // 下次亮屏必定重绘
    }

    delay(2);
}
