// ============================================================
//  ped_core.h —— 硬件常量 / 用户设置 / 历史数据 / 存储
//  M5Stack StopWatch (C152)  ESP32-S3R8 + BMI270 + CO5300 AMOLED
// ============================================================
#pragma once

#include <M5Unified.h>
#include <Preferences.h>
#include <math.h>

// ---------------- 引脚（来自 M5GFX / M5Unified 的板卡定义） ----------------
static constexpr int     PIN_I2C_SDA = 47;   // 与 SCL 同一条内部总线
static constexpr int     PIN_I2C_SCL = 48;
static constexpr uint8_t ADDR_IMU    = 0x68; // BMI270
static constexpr uint8_t ADDR_PM1    = 0x6E; // M5PM1 电源管理
static constexpr uint8_t ADDR_RTC    = 0x32; // RX8130
static constexpr uint8_t ADDR_TOUCH  = 0x15; // CST820 触摸
static constexpr uint8_t ADDR_IOE1   = 0x4F; // M5IOE1 IO 扩展
static constexpr int     PIN_BTN_A   = 2;    // 黄键
static constexpr int     PIN_BTN_B   = 1;    // 蓝键
static constexpr int     PIN_TP_INT  = 13;   // 触摸中断（仅用于说明，不作唤醒源）

// ---------------- 深睡 ----------------
static constexpr uint32_t SLEEP_TICK_S = 60;      // 深睡中每 60 s 醒一次读硬件计步
                                                 // 计数由 BMI270 自己累加，醒来晚只影响"最后一次读数"
                                                 // 20s→60s 把唤醒摊销的电流从 ~60µA 降到 ~20µA
static constexpr uint32_t RTC_MAGIC    = 0x50454432u; // 'PED2'

// PMIC 状态灯（M5PM1 只有开/关，无 PWM）：false = 常关，省 1~3 mA
static constexpr bool POWER_LED_ON = false;

#define HIST_DAYS 7

// 今日按 1 分钟一格，共 1440 格，用于主页外环按时间顺序着色。
// 每格 2 bit：0 无步数 / 1 走路 / 2 跑步，全天占 360 B。
#define BKT_N 1440
struct DayBuckets
{
    uint8_t lvl[BKT_N / 4];
};

// 分时格读写。同一格内只保留更高的强度，后写的弱状态不覆盖强状态。
static inline uint8_t bktGet(const uint8_t *arr, int i)
{
    return (uint8_t)((arr[i >> 2] >> ((i & 3) << 1)) & 0x03);
}

static inline void bktPut(uint8_t *arr, int i, uint8_t lv)
{
    const int     sh = (i & 3) << 1;
    const uint8_t c  = (uint8_t)((arr[i >> 2] >> sh) & 0x03);
    if (lv <= c) return;
    arr[i >> 2] = (uint8_t)((arr[i >> 2] & ~(0x03 << sh)) | (uint8_t)(lv << sh));
}

// ------------------------------------------------------------
// 用户设置（可在设备内直接修改并保存）
// ------------------------------------------------------------
struct Settings
{
    uint32_t goal        = 8000;   // 每日目标步数
    uint16_t heightCm    = 172;    // 身高，决定步幅
    uint16_t weightKg    = 65;     // 体重，决定消耗
    uint8_t  bright      = 160;    // 亮度 40..255
    uint8_t  sound       = 2;      // 0 静音 / 1 震动 / 2 震动+提示音
    uint8_t  screenOffS  = 15;     // 无操作息屏秒数
    uint8_t  sleepAfterS = 45;     // 息屏后进入深睡的秒数
    uint16_t themeHue    = 165;    // 主题色色相 0..359（环、数值、选中态）
    uint16_t runHue      = 12;     // 强调色色相 0..359（跑步段）
    uint8_t  themeVal    = 92;     // 主题色亮度 30..100（%）
    uint8_t  runVal      = 100;    // 强调色亮度 30..100（%）
};

// 每日历史
struct DayRec
{
    int32_t  day;    // YYYYMMDD，0 = 空
    uint32_t steps;
    uint32_t run;    // 其中判定为跑步的步数
};

// 深睡期间保存在 RTC 内存中的数据（关机断电后失效）
struct SleepState
{
    uint32_t magic;
    uint32_t steps;
    uint32_t run;        // 当日跑步步数
    uint32_t activeSec;
    int32_t  dayKey;
    uint32_t hwBase;
    uint32_t hwRaw;
    uint32_t wakes;
    uint16_t vbat;
    uint8_t  hwInit;     // 1 = 已建立计数基线
    uint8_t  lowBat;
    uint8_t  failCnt;    // 静默唤醒连续失败次数，达到 3 次则停用深睡
    uint8_t  dbgBoot;    // 调试：强制下次唤醒走完整启动，不跑静默通道
    uint8_t  bktLvl[BKT_N / 4];   // 今日各时段的运动强度（与 g_bkt 同格式）
};
RTC_DATA_ATTR SleepState g_rtc;

// ------------------------------------------------------------
// 全局状态
// ------------------------------------------------------------
Preferences g_prefs;

Settings g_cfg;
DayRec   g_hist[HIST_DAYS];

uint32_t g_steps      = 0;   // 今日步数
uint32_t g_runSteps   = 0;   // 今日其中判定为跑步的步数
DayBuckets g_bkt;            // 今日分时明细（清零由 bucketClear 负责）
uint32_t g_activeSec  = 0;   // 今日活跃秒数
int32_t  g_dayKey     = 0;   // 今日日期键
uint32_t g_savedSteps = 0xFFFFFFFFu;

int      g_page       = 0;
int      g_setScroll  = 0;
uint8_t  g_bright     = 0;
int      g_battery    = -1;   // %
uint16_t g_vbat       = 0;    // mV
bool     g_usbPower   = false;
bool     g_forceRender = false;   // 置位后主循环强制重绘一帧
bool     g_imuOk      = false;

uint32_t g_lastActiveMs = 0;
uint32_t g_lastStepMs   = 0;
uint32_t g_cadence      = 0;

// 步频窗口：每秒格子，5 秒滑窗
static constexpr int CAD_N = 5;
uint32_t g_cadBuf[CAD_N];
int      g_cadIdx = 0;

// 硬件计步（BMI270），本次开机后累计
uint32_t g_hwSteps = 0;
uint8_t g_actCode = 0;   // BMI270 活动状态输出 0-3

// 震动：非阻塞脉冲。脉宽写死在这里，由 vibService() 到点关断 ——
// 交给主循环按帧关断会让实际脉宽随渲染耗时漂移，手感忽重忽轻。
uint8_t  g_vibLvl = 0;      // 当前档位，0 = 停
uint32_t g_vibEnd = 0;      // 该关断的时刻
uint32_t g_nextMilestone = 1000;

static void vibOn(uint8_t lvl, uint16_t ms)
{
    if (g_cfg.sound == 0) return;               // 静音：不震
    M5.Power.setVibration(lvl);
    g_vibLvl = lvl;
    g_vibEnd = millis() + ms;
}

static void vibService()
{
    if (g_vibLvl && (int32_t)(millis() - g_vibEnd) >= 0) {
        M5.Power.setVibration(0);
        g_vibLvl = 0;
    }
}

// ------------------------------------------------------------
// 步频：硬件每 1 s 送来一次增量，5 格滑窗折成步/分
// ------------------------------------------------------------
static void cadencePush(uint32_t steps)
{
    g_cadBuf[g_cadIdx] = steps;
    g_cadIdx = (g_cadIdx + 1) % CAD_N;
    uint32_t sum = 0;
    for (int i = 0; i < CAD_N; ++i) sum += g_cadBuf[i];
    g_cadence = sum ? sum * (60 / CAD_N) : 0;
}

static void cadenceClear()
{
    for (int i = 0; i < CAD_N; ++i) g_cadBuf[i] = 0;
    g_cadIdx   = 0;
    g_cadence  = 0;
}

// ------------------------------------------------------------
// 工具
// ------------------------------------------------------------
static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static uint16_t lerp565(uint16_t c1, uint16_t c2, float t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    int r1 = (c1 >> 11) & 0x1F, g1 = (c1 >> 5) & 0x3F, b1 = c1 & 0x1F;
    int r2 = (c2 >> 11) & 0x1F, g2 = (c2 >> 5) & 0x3F, b2 = c2 & 0x1F;
    return (uint16_t)(((r1 + (int)((r2 - r1) * t)) << 11) |
                      ((g1 + (int)((g2 - g1) * t)) << 5) |
                       (b1 + (int)((b2 - b1) * t)));
}

// HSV → RGB565。配色只调色相，饱和度与亮度固定，保证界面元素始终可读。
static inline uint16_t hsv565(uint16_t h, float s, float v)
{
    while (h >= 360) h -= 360;
    const float c = v * s;
    const float x = c * (1.0f - fabsf(fmodf((float)h / 60.0f, 2.0f) - 1.0f));
    const float m = v - c;
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if      (h <  60) { r = c; g = x; }
    else if (h < 120) { r = x; g = c; }
    else if (h < 180) { g = c; b = x; }
    else if (h < 240) { g = x; b = c; }
    else if (h < 300) { r = x; b = c; }
    else              { r = c; b = x; }
    const uint16_t R = (uint16_t)((r + m) * 255.0f + 0.5f);
    const uint16_t G = (uint16_t)((g + m) * 255.0f + 0.5f);
    const uint16_t B = (uint16_t)((b + m) * 255.0f + 0.5f);
    return (uint16_t)(((R & 0xF8) << 8) | ((G & 0xFC) << 3) | (B >> 3));
}

static inline uint16_t themeColor() { return hsv565(g_cfg.themeHue, 0.90f, g_cfg.themeVal / 100.0f); }
static inline uint16_t runColor()   { return hsv565(g_cfg.runHue,   0.86f, g_cfg.runVal   / 100.0f); }

// 步幅由身高推算：行走步幅 ≈ 身高 × 0.415
static inline float strideMeters()
{
    return g_cfg.heightCm * 0.415f / 100.0f;
}

// 行走消耗：约 0.53 kcal / (kg · km)
static inline int kcalOf(uint32_t steps)
{
    float km = steps * strideMeters() / 1000.0f;
    return (int)(km * g_cfg.weightKg * 0.53f + 0.5f);
}

static inline float kmOf(uint32_t steps)
{
    return steps * strideMeters() / 1000.0f;
}

// ------------------------------------------------------------
// 时间 / 日期
// ------------------------------------------------------------
static bool rtcValid()
{
    auto dt = M5.Rtc.getDateTime();
    return (dt.date.year >= 2024);
}

static int32_t todayKey()
{
    if (!M5.Rtc.isEnabled()) return 0;
    auto dt = M5.Rtc.getDateTime();
    if (dt.date.year < 2024) return 0;
    return (int32_t)dt.date.year * 10000 + dt.date.month * 100 + dt.date.date;
}

static void rtcApply(int y, int mo, int d, int h, int mi)
{
    if (!M5.Rtc.isEnabled()) return;
    m5::rtc_datetime_t dt;
    dt.date.year = (int16_t)y; dt.date.month = (int8_t)mo; dt.date.date = (int8_t)d;
    dt.date.weekDay = 0;
    dt.time.hours = (int8_t)h; dt.time.minutes = (int8_t)mi; dt.time.seconds = 0;
    M5.Rtc.setDateTime(dt);
}

// 年月日是否合法（对时页用）
static bool k_valid(int y, int mo, int d)
{
    if (y < 2024 || y > 2099) return false;
    if (mo < 1 || mo > 12)    return false;
    if (d  < 1 || d  > 31)    return false;
    return true;
}

// ------------------------------------------------------------
// 存储
// ------------------------------------------------------------
static void cfgLoad()
{
    g_prefs.begin("pedo2", false);
    g_cfg.goal        = g_prefs.getUInt ("goal",    g_cfg.goal);
    g_cfg.heightCm    = g_prefs.getUShort("height", g_cfg.heightCm);
    g_cfg.weightKg    = g_prefs.getUShort("weight", g_cfg.weightKg);
    g_cfg.bright      = g_prefs.getUChar ("bright",  g_cfg.bright);
    g_cfg.sound       = g_prefs.getUChar ("sound",   g_cfg.sound);
    g_cfg.screenOffS  = g_prefs.getUChar ("offs",    g_cfg.screenOffS);
    g_cfg.sleepAfterS = g_prefs.getUChar ("slps",    g_cfg.sleepAfterS);
    g_cfg.themeHue    = g_prefs.getUShort("hue1",    g_cfg.themeHue);
    g_cfg.runHue      = g_prefs.getUShort("hue2",    g_cfg.runHue);
    g_cfg.themeVal    = g_prefs.getUChar ("val1",    g_cfg.themeVal);
    g_cfg.runVal      = g_prefs.getUChar ("val2",    g_cfg.runVal);

    // 容错
    if (g_cfg.goal < 500 || g_cfg.goal > 60000) g_cfg.goal = 8000;
    if (g_cfg.heightCm < 100 || g_cfg.heightCm > 230) g_cfg.heightCm = 172;
    if (g_cfg.weightKg < 30 || g_cfg.weightKg > 200) g_cfg.weightKg = 65;
    if (g_cfg.bright < 20) g_cfg.bright = 160;
    if (g_cfg.sound > 2) g_cfg.sound = 2;
    if (g_cfg.screenOffS < 3) g_cfg.screenOffS = 15;
    if (g_cfg.sleepAfterS < 10) g_cfg.sleepAfterS = 45;
    if (g_cfg.themeHue >= 360) g_cfg.themeHue = 165;
    if (g_cfg.runHue >= 360) g_cfg.runHue = 12;
    if (g_cfg.themeVal < 30 || g_cfg.themeVal > 100) g_cfg.themeVal = 92;
    if (g_cfg.runVal   < 30 || g_cfg.runVal   > 100) g_cfg.runVal   = 100;

    // 历史记录的字段在 v2.5 增加了跑步步数，旧键的长度对不上，读新键
    if (g_prefs.getBytesLength("hist2") == sizeof(g_hist)) {
        g_prefs.getBytes("hist2", g_hist, sizeof(g_hist));
    }
}

static void cfgSave()
{
    g_prefs.putUInt  ("goal",    g_cfg.goal);
    g_prefs.putUShort("height",  g_cfg.heightCm);
    g_prefs.putUShort("weight",  g_cfg.weightKg);
    g_prefs.putUChar ("bright",  g_cfg.bright);
    g_prefs.putUChar ("sound",   g_cfg.sound);
    g_prefs.putUChar ("offs",    g_cfg.screenOffS);
    g_prefs.putUChar ("slps",    g_cfg.sleepAfterS);
    g_prefs.putUShort("hue1",    g_cfg.themeHue);
    g_prefs.putUShort("hue2",    g_cfg.runHue);
    g_prefs.putUChar ("val1",    g_cfg.themeVal);
    g_prefs.putUChar ("val2",    g_cfg.runVal);
}

// ------------------------------------------------------------
// 设置项的延迟落盘
// 每按一下 +/− 就写一次 NVS 会卡顿（一次 putXxx 要擦写页），
// 但完全不写＝重启后全丢。折中：改完 700 ms 内没有新改动才落盘，
// 关机 / 深睡 / 离开设置页时立刻强制写。
// ------------------------------------------------------------
static bool     g_cfgDirty   = false;
static uint32_t g_cfgDirtyMs = 0;

static void cfgTouch()                       // 标记「设置变了」
{
    g_cfgDirty   = true;
    g_cfgDirtyMs = millis();
}

static void cfgFlush()                       // 立即写盘
{
    if (!g_cfgDirty) return;
    g_cfgDirty = false;
    cfgSave();
}

static void cfgService()                     // 每帧调用：静置够了就落盘
{
    if (g_cfgDirty && (millis() - g_cfgDirtyMs) >= 700) cfgFlush();
}

static void histSave()
{
    g_prefs.putBytes("hist2", g_hist, sizeof(g_hist));
}

// 把某天成绩压入历史（同一天则覆盖）
static void histPush(int32_t day, uint32_t steps, uint32_t run)
{
    if (day <= 0) return;
    for (int i = 0; i < HIST_DAYS; ++i) {
        if (g_hist[i].day == day) { g_hist[i].steps = steps; g_hist[i].run = run; return; }
    }
    // 找一个空位，否则挤掉最旧的
    int slot = -1;
    for (int i = 0; i < HIST_DAYS; ++i) if (g_hist[i].day == 0) { slot = i; break; }
    if (slot < 0) {
        int32_t oldest = g_hist[0].day; slot = 0;
        for (int i = 1; i < HIST_DAYS; ++i) if (g_hist[i].day < oldest) { oldest = g_hist[i].day; slot = i; }
    }
    g_hist[slot].day   = day;
    g_hist[slot].steps = steps;
    g_hist[slot].run   = run;
}

// 按日期升序取第 k 条（k=0 最早）
static DayRec histAt(int k)
{
    DayRec tmp[HIST_DAYS];
    int n = 0;
    for (int i = 0; i < HIST_DAYS; ++i) if (g_hist[i].day > 0) tmp[n++] = g_hist[i];
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            if (tmp[j].day < tmp[i].day) { DayRec t = tmp[i]; tmp[i] = tmp[j]; tmp[j] = t; }
    DayRec empty = {0, 0, 0};
    return (k < n) ? tmp[k] : empty;
}

// 归档：只在不小于已有值时更新。
// NVS 与 RTC 内存两套状态可能先后不一，直接覆盖会把真实记录抹成 0。
static void histArchive(int32_t day, uint32_t steps, uint32_t run)
{
    if (day <= 0) return;
    for (int i = 0; i < HIST_DAYS; ++i) {
        if (g_hist[i].day == day) {
            if (steps > g_hist[i].steps) {
                g_hist[i].steps = steps;
                g_hist[i].run   = run;
            }
            return;
        }
    }
    histPush(day, steps, run);
}

// 把「今天」的实时成绩同步进历史数组（只改内存，跨天时才 histSave）
static void histSyncToday()
{
    if (g_dayKey <= 0) return;
    for (int i = 0; i < HIST_DAYS; ++i) {
        if (g_hist[i].day == g_dayKey) {
            g_hist[i].steps = g_steps;
            g_hist[i].run   = g_runSteps;
            return;
        }
    }
    histPush(g_dayKey, g_steps, g_runSteps);
}

static uint32_t histAvg()
{
    uint32_t sum = 0; int n = 0;
    for (int i = 0; i < HIST_DAYS; ++i) if (g_hist[i].day > 0) { sum += g_hist[i].steps; ++n; }
    return n ? sum / n : 0;
}

// ------------------------------------------------------------
// 分时明细：每 1 分钟一格。当前格与格内进度由主循环每秒刷新，
// 累加步数时直接用缓存值，避免每步都去读一次 RTC。
// frac 返回该格已经走过的比例，用于外环只涂到此刻。
// ------------------------------------------------------------
static int bucketNow(float *frac = nullptr)
{
    if (frac) *frac = 1.0f;
    if (!M5.Rtc.isEnabled()) return 0;
    auto dt = M5.Rtc.getDateTime();
    if (dt.date.year < 2024) return 0;
    int b = (int)dt.time.hours * 60 + (int)dt.time.minutes;
    if (b < 0 || b >= BKT_N) b = 0;
    if (frac) *frac = (float)dt.time.seconds / 60.0f;
    return b;
}

int   g_bktCur  = 0;      // 当前格序号
float g_bktFrac = 1.0f;   // 当前格已走过的比例 0..1

static void bucketTick()
{
    g_bktCur = bucketNow(&g_bktFrac);
}

static void bucketClear()
{
    memset(&g_bkt, 0, sizeof(g_bkt));
}

// 记下当前这一分钟的状态：0 无步数 / 1 走路 / 2 跑步
static void bktMark(uint8_t lv)
{
    if (!lv) return;
    bktPut(g_bkt.lvl, g_bktCur, lv);
}

// ------------------------------------------------------------
// 今日数据
// ------------------------------------------------------------
static void loadToday()
{
    int32_t savedDay = (int32_t)g_prefs.getInt("day", 0);
    g_steps     = (uint32_t)g_prefs.getUInt("steps", 0);
    g_runSteps  = (uint32_t)g_prefs.getUInt("run", 0);
    g_activeSec = g_prefs.getUInt("active", 0);

    int32_t today = todayKey();
    bool    rolled = false;
    if (today != 0) {
        if (savedDay != 0 && savedDay != today) {
            histArchive(savedDay, g_steps, g_runSteps);
            histSave();
            g_steps = 0;
            g_runSteps = 0;
            g_activeSec = 0;
            g_nextMilestone = 1000;
            rolled = true;
        }
        g_dayKey = today;
        g_prefs.putInt("day", today);
    } else {
        g_dayKey = savedDay;
    }

    // 跨天时桶已经被清空，不能再把昨天的那份读回来
    if (!rolled && g_prefs.getBytesLength("bkt3") == sizeof(g_bkt)) {
        g_prefs.getBytes("bkt3", &g_bkt, sizeof(g_bkt));
    }
    else if (rolled) {
        bucketClear();
    }
    g_savedSteps = g_steps;
}

static void saveToday(bool force = false)
{
    if (!force && g_steps == g_savedSteps) return;
    g_prefs.putUInt("steps", g_steps);
    g_prefs.putUInt("run",   g_runSteps);
    g_prefs.putUInt("active", g_activeSec);
    g_prefs.putBytes("bkt3", &g_bkt, sizeof(g_bkt));
    if (g_dayKey) g_prefs.putInt("day", g_dayKey);
    // RTC 内存同步：深睡醒来合并时靠它取较大值，
    // 两处长期不一致会让归档拿到旧值（0），把历史柱状图抹平
    g_rtc.steps     = g_steps;
    g_rtc.run       = g_runSteps;
    g_rtc.activeSec = g_activeSec;
    memcpy(g_rtc.bktLvl, g_bkt.lvl, sizeof(g_bkt.lvl));
    if (g_dayKey) g_rtc.dayKey = g_dayKey;
    g_savedSteps = g_steps;
}

// 跨天：把昨天成绩入库并清零
static void rolloverIfNeeded()
{
    int32_t k = todayKey();
    if (k == 0) return;
    if (g_dayKey != 0 && k != g_dayKey) {
        histArchive(g_dayKey, g_steps, g_runSteps);
        histSave();
        g_steps = 0;
        g_runSteps = 0;
        g_activeSec = 0;
        bucketClear();
        cadenceClear();
        g_nextMilestone = 1000;
        g_dayKey = k;
        g_prefs.putInt("day", k);
        saveToday(true);
    } else if (g_dayKey == 0) {
        g_dayKey = k;
    }
}

// 计步反馈（震动 / 提示音）
static void feedback(uint8_t level)
{
    vibOn(level ? 200 : 120, level ? 90 : 45);
    if (g_cfg.sound >= 2 && M5.Speaker.isEnabled()) {
        M5.Speaker.tone(level ? 988 : 1319, level ? 90 : 45);
    }
}

static void stepsAdd(uint32_t n, uint8_t lv = 1)
{
    if (n == 0) return;
    g_steps += n;
    bktMark(lv);
    g_lastStepMs = millis();
    if (g_steps >= g_nextMilestone) {
        g_nextMilestone = ((g_steps / 1000) + 1) * 1000;
        feedback(0);
    }
}
