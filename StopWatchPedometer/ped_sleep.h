// ============================================================
//  ped_sleep.h —— 深睡（L2）与"静默唤醒"快速通道
//
//  · 息屏且静止后进入 ESP32-S3 deep sleep，BMI270 继续用硬件计步器计数
//  · 每 SLEEP_TICK_S 秒定时醒来：只初始化 I2C、读一次计数、再睡，
//    不初始化屏幕/触摸/PMIC，全程约 10 ms
//  · 任意按键（黄键/蓝键）用 ext1 低电平唤醒，醒来走完整启动流程
//  · USB 供电时不进深睡（避免调试时反复重启）
// ============================================================
#pragma once

#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <esp_system.h>

namespace pedsleep {

static constexpr int     SILENT_PORT = 1;      // 与 M5Unified 使用同一条 I2C
static constexpr uint8_t PM1_PWR_CFG = 0x06;
static constexpr uint8_t PM1_VBAT_L  = 0x22;
static constexpr uint8_t PWR_CFG_DCDC_EN = 1 << 1;
static constexpr uint8_t PWR_CFG_LDO_EN  = 1 << 2;
static constexpr uint8_t PWR_CFG_BOOST_EN= 1 << 3;
static constexpr uint8_t PWR_CFG_LED_EN  = 1 << 4;

// ---------------- 底层 I2C（静默通道） ----------------
inline void i2cOpen()
{
    lgfx::i2c::init(SILENT_PORT, PIN_I2C_SDA, PIN_I2C_SCL);
}

inline bool rawWr(uint8_t addr, uint8_t reg, uint8_t val)
{
    for (int i = 0; i < 3; ++i) {
        auto r = lgfx::i2c::writeRegister8(SILENT_PORT, addr, reg, val, 0, 400000);
        if (r.has_value()) return true;
        delay(2);
    }
    return false;
}

inline bool rawRd(uint8_t addr, uint8_t reg, uint8_t *buf, size_t n)
{
    for (int i = 0; i < 3; ++i) {
        auto r = lgfx::i2c::readRegister(SILENT_PORT, addr, reg, buf, n, 400000);
        if (r.has_value()) return true;
        delay(2);
    }
    return false;
}

// 读电池电压（mV），失败返回 0
inline uint16_t readVbat()
{
    uint8_t b[2] = {0, 0};
    if (!rawRd(ADDR_PM1, PM1_VBAT_L, b, 2)) return 0;
    return (uint16_t)((b[1] << 8) | b[0]);
}

// ------------------------------------------------
// 按键是否仍被按住（避免长按导致反复唤醒）
// ------------------------------------------------
inline bool buttonHeld()
{
    return (digitalRead(PIN_BTN_A) == LOW) || (digitalRead(PIN_BTN_B) == LOW);
}

// 深睡唤醒后引脚仍在 RTC 复用上，digitalRead 不一定可靠，这里直接读 RTC IO
inline bool buttonHeldRtc()
{
    return (rtc_gpio_get_level((gpio_num_t)PIN_BTN_A) == 0) ||
           (rtc_gpio_get_level((gpio_num_t)PIN_BTN_B) == 0);
}

// ------------------------------------------------
// 唤醒后把 CST820 从深睡里叫回来
//
// 必须做：M5GFX 用 I2C 地址表 [0x32, 0x68, 0x15] 探测板卡，
// 有 0x15(CST820) 才认定是 StopWatch。触摸睡着时探测失败，
// 面板配置落空 —— 表现为"电源灯亮、屏幕全黑、触摸无反应"，
// 只有冷启动（触摸芯片被断电复位）才恢复。
// ------------------------------------------------
inline bool touchAlive()
{
    uint8_t v = 0;
    return rawRd(0x15, 0xE5, &v, 1);
}

inline bool reviveTouch()
{
    i2cOpen();
    delay(3);
    for (int i = 0; i < 6; ++i) {
        uint8_t v = 0;
        if (rawRd(0x15, 0xE5, &v, 1)) {
            if (v != 0x03) return true;          // 已经醒着
            rawWr(0x15, 0xE5, 0x00);             // 退出深睡
            delay(12);
        } else {
            delay(25);
        }
    }
    return false;
}

// ------------------------------------------------
// 睡眠前的外设收尾
//
// 只做"无害"的收尾。**不要再自己写 PM1 的 0x06(PWR_CFG)**：
// PM1 是常电域，写坏它会让它进入 I2C idle sleep，而 M5GFX 的板卡探测
// 必须先读 PM1 才能继续、它清 0x09 的代码又在探测成功之后 —— 一旦读不到
// 就形成死锁，表现为唤醒后黑屏无响应，只有整机断电才能恢复。
// 现在改为启动时无条件清 0x09/0x0A（见 setup 里的 pm1Recover），
// 睡前不再动 PM1 的任何寄存器。
// ------------------------------------------------
inline void quietPeripherals(bool viaM5)
{
    (void)viaM5;
}

// ------------------------------------------------
// 睡前关掉 AMOLED 的 BOOST 压轨（bit3），与官方 L2 对齐
// （官方 kL2PowerValue 只保留 DCDC|LDO）。
//
// 背景：面板即使发了 SLPIN，BOOST 转换器仍带电；实测整夜深睡掉 20% 电
// （约 7.5 mA 平均），面板+BOOST 是大头。LED_EN 不用管 —— 它只是使能位，
// 灯亮不亮由 PM1 充电逻辑决定，不插 USB 时本来就不亮（用户实测证实）。
//
// 安全性：只做 read-modify-write 的 bit 清除，**不碰 0x09**（I2C idle
// sleep，之前死锁的根源）；唤醒路径由 pm1Recover 把 BOOST 置回来。
// ------------------------------------------------
inline void railsOff()
{
    // 尽力而为：在完整启动上下文中 M5Unified 接管了 I2C 端口，这里直连会
    // read FAIL（实测）。真正生效要等深睡唤醒后的裸环境 —— 但那时已经睡了。
    // 失败就跳过，无副作用；BOOST 保持常开时夜间耗电由面板 SLPIN 兜底。
    i2cOpen();
    delay(2);
    uint8_t cfg = 0;
    if (rawRd(ADDR_PM1, PM1_PWR_CFG, &cfg, 1) && (cfg & PWR_CFG_BOOST_EN)) {
        rawWr(ADDR_PM1, PM1_PWR_CFG, (uint8_t)(cfg & ~PWR_CFG_BOOST_EN));
    }
}


// ------------------------------------------------
// 进入深睡（不返回）
// ------------------------------------------------
inline void enter(bool onlyTimer = false)
{
    // 先判断按键是否还按着（此时引脚仍是普通 GPIO，读取才准）
    const bool keyWake = !onlyTimer && !buttonHeld();

    // 按键引脚：RTC 上拉，低电平唤醒
    rtc_gpio_init((gpio_num_t)PIN_BTN_A);
    rtc_gpio_set_direction((gpio_num_t)PIN_BTN_A, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en((gpio_num_t)PIN_BTN_A);
    rtc_gpio_pulldown_dis((gpio_num_t)PIN_BTN_A);

    rtc_gpio_init((gpio_num_t)PIN_BTN_B);
    rtc_gpio_set_direction((gpio_num_t)PIN_BTN_B, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en((gpio_num_t)PIN_BTN_B);
    rtc_gpio_pulldown_dis((gpio_num_t)PIN_BTN_B);

    // 触摸中断线高阻，避免持续漏电
    rtc_gpio_isolate((gpio_num_t)PIN_TP_INT);

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_TICK_S * 1000000ULL);

    if (keyWake) {
        const uint64_t mask = (1ULL << PIN_BTN_A) | (1ULL << PIN_BTN_B);
        esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
    }

    // 保住 RTC 上拉
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    // 关键：上面两次调用会重新配置 RTC IO，上拉必须放在它们之后重下一次，
    // 否则按键线可能悬空，ext1 永远等不到低电平 —— 表现就是按了键也叫不醒。
    rtc_gpio_pullup_en((gpio_num_t)PIN_BTN_A);
    rtc_gpio_pulldown_dis((gpio_num_t)PIN_BTN_A);
    rtc_gpio_pullup_en((gpio_num_t)PIN_BTN_B);
    rtc_gpio_pulldown_dis((gpio_num_t)PIN_BTN_B);

    Serial.printf("[slp] keyWake=%d rtcA=%d rtcB=%d\n", (int)keyWake,
                  (int)rtc_gpio_get_level((gpio_num_t)PIN_BTN_A),
                  (int)rtc_gpio_get_level((gpio_num_t)PIN_BTN_B));
    Serial.flush();
    delay(20);                  // 让 RTC IO 配置稳定下来再进睡

    g_rtc.magic = RTC_MAGIC;
    esp_deep_sleep_start();
}

// 深睡里读 RX8130 的时与分（BCD），换算成当天第几分钟；读不到返回 -1
inline int rtcMinuteOfDay()
{
    uint8_t b[3];
    if (!rawRd(ADDR_RTC, 0x10, b, 3)) return -1;   // 0x10 秒 / 0x11 分 / 0x12 时
    const int h = (int)((b[2] >> 4) & 0x03) * 10 + (int)(b[2] & 0x0F);
    const int m = (int)((b[1] >> 4) & 0x07) * 10 + (int)(b[1] & 0x0F);
    if (h > 23 || m > 59) return -1;
    return h * 60 + m;
}

// ------------------------------------------------
// 静默唤醒：读计数 -> 累加 -> 再睡
// 返回 0 表示已睡下不返回；1 表示按键被按住（应走完整启动把屏点亮）；
// 2 表示静默通道失败
// ------------------------------------------------
inline int silentTick()
{
    // 兜底：如果是定时醒来、但按键此刻正被按住，说明按键唤醒没拦住，
    // 直接走完整启动，绝不会出现"按了键但屏幕一直黑"的死状态。
    if (buttonHeldRtc()) return 1;

    i2cOpen();
    delay(2);

    uint8_t id = 0;
    if (!rawRd(ADDR_IMU, 0x00, &id, 1) || id != 0x24) return 2;

    if (!rawWr(ADDR_IMU, 0x2F, 0)) return 2;     // 计数输出在 page 0
    delay(1);

    uint8_t b[8];
    if (!rawRd(ADDR_IMU, 0x30, b, 8)) return 2;
    const uint32_t raw = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                         ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);

    const uint32_t delta = hwAccumulate(raw);
    g_rtc.steps += delta;
    const uint8_t gait = (uint8_t)(b[4] & 0x03);   // 0 静止 / 1 步行 / 2 跑步 / 3 未知
    if (delta) {
        if (gait == 2) g_rtc.run += delta;
        g_rtc.activeSec += SLEEP_TICK_S;
        // 分时明细：这一格记成走路或跑步
        const int mi = rtcMinuteOfDay();
        if (mi >= 0) bktPut(g_rtc.bktLvl, mi, (gait == 2) ? 2 : 1);
    }
    g_rtc.wakes++;

    // 每 15 次唤醒（约 5 分钟）查一次电池
    if ((g_rtc.wakes % 15) == 0) {
        uint16_t mv = readVbat();
        if (mv > 2000) {
            g_rtc.vbat = mv;
            g_rtc.lowBat = (mv < 3450) ? 1 : 0;
            if (mv < 3300) {
                // 低电：状态灯闪两下提醒充电
                uint8_t c = 0;
                if (rawRd(ADDR_PM1, PM1_PWR_CFG, &c, 1)) {
                    for (int i = 0; i < 2; ++i) {
                        rawWr(ADDR_PM1, PM1_PWR_CFG, (uint8_t)(c | PWR_CFG_LED_EN));
                        delay(60);
                        rawWr(ADDR_PM1, PM1_PWR_CFG, (uint8_t)(c & ~PWR_CFG_LED_EN));
                        delay(120);
                    }
                }
            }
        }
    }

    // 落盘：每 15 次唤醒（约 5 分钟）或累计 200 步
    static constexpr uint32_t PERSIST_STEPS = 200;
    bool needSave = ((g_rtc.wakes % 15) == 0) || (delta >= PERSIST_STEPS);

    if (needSave) {
        g_prefs.begin("pedo2", false);
        g_prefs.putUInt("steps", g_rtc.steps);
        g_prefs.putUInt("run",   g_rtc.run);
        g_prefs.putUInt("active", g_rtc.activeSec);
        g_prefs.putBytes("bkt3", g_rtc.bktLvl, sizeof(g_rtc.bktLvl));
        if (g_rtc.dayKey) g_prefs.putInt("day", g_rtc.dayKey);
    }

    quietPeripherals(false);
    enter(false);
    return 0;
}

// ------------------------------------------------
// 调试：只允许按键唤醒（无定时器），用于验证 ext1 是否有效
// ------------------------------------------------
inline void enterKeysOnly()
{
    rtc_gpio_init((gpio_num_t)PIN_BTN_A);
    rtc_gpio_set_direction((gpio_num_t)PIN_BTN_A, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en((gpio_num_t)PIN_BTN_A);
    rtc_gpio_pulldown_dis((gpio_num_t)PIN_BTN_A);
    rtc_gpio_init((gpio_num_t)PIN_BTN_B);
    rtc_gpio_set_direction((gpio_num_t)PIN_BTN_B, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en((gpio_num_t)PIN_BTN_B);
    rtc_gpio_pulldown_dis((gpio_num_t)PIN_BTN_B);

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    const uint64_t mask = (1ULL << PIN_BTN_A) | (1ULL << PIN_BTN_B);
    esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
    // 兜底定时器：万一按键没叫醒也能在 120 s 后自己回来，测试不会把设备睡死
    esp_sleep_enable_timer_wakeup(120ULL * 1000000ULL);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    rtc_gpio_pullup_en((gpio_num_t)PIN_BTN_A);
    rtc_gpio_pulldown_dis((gpio_num_t)PIN_BTN_A);
    rtc_gpio_pullup_en((gpio_num_t)PIN_BTN_B);
    rtc_gpio_pulldown_dis((gpio_num_t)PIN_BTN_B);

    g_rtc.magic   = RTC_MAGIC;
    g_rtc.dbgBoot = 1;          // 醒来强制完整启动，方便观察屏幕
    esp_deep_sleep_start();
}

// ------------------------------------------------
// 调试：把两个按键切到 RTC 域后回读电平，检查内部上拉是否生效
// ------------------------------------------------
inline void dumpKeyLevels()
{
    Serial.printf("[K] digital A=%d B=%d\n",
                  (int)digitalRead(PIN_BTN_A), (int)digitalRead(PIN_BTN_B));

    rtc_gpio_init((gpio_num_t)PIN_BTN_A);
    rtc_gpio_set_direction((gpio_num_t)PIN_BTN_A, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en((gpio_num_t)PIN_BTN_A);
    rtc_gpio_pulldown_dis((gpio_num_t)PIN_BTN_A);
    rtc_gpio_init((gpio_num_t)PIN_BTN_B);
    rtc_gpio_set_direction((gpio_num_t)PIN_BTN_B, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en((gpio_num_t)PIN_BTN_B);
    rtc_gpio_pulldown_dis((gpio_num_t)PIN_BTN_B);
    delay(5);

    Serial.printf("[K] rtc     A=%d B=%d  (不按键应为 1/1)\n",
                  (int)rtc_gpio_get_level((gpio_num_t)PIN_BTN_A),
                  (int)rtc_gpio_get_level((gpio_num_t)PIN_BTN_B));
}

} // namespace pedsleep
