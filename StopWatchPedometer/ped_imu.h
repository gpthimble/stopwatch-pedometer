// ============================================================
//  ped_imu.h —— BMI270 硬件计步器
//
//  Bosch BMI270 的步数功能属于 feature engine：
//    · 8 KB 微代码由 M5Unified 在 M5.begin() 时上传，之后内部状态为 init_ok
//    · 25 个算法参数在 feature page 3 起，每页 8 个 16 位字，第 25 个在 page 6
//      （数据手册 Table 15 腕式表，出厂即为该值，仍写入以保证确定性）
//    · 使能位：page 6 偏移 0x02 字的 bit12 → 寄存器 0x33 的 bit4
//      （BMI2_STEP_COUNT_FEAT_EN_OFFSET=0x01，MASK=0x10）
//    · 计数输出：feature page 0 偏移 0x00，4 字节 32 位小端 → 寄存器 0x30..0x33
//
//  改动步骤必须按顺序：先关引擎 → 写参数 → 再开引擎（Bosch 应用笔记要求）
//  本文件所有写操作都会回读比对，dbg* 字段存的是芯片里的真实值，不是本地缓冲
// ============================================================
#pragma once

#include "bmi270_legacy_config.h"

namespace pedimu {

// ---------------- 寄存器 ----------------
static constexpr uint8_t R_CHIP_ID   = 0x00;
static constexpr uint8_t R_INT_STAT  = 0x21;   // bit0 = init_ok
static constexpr uint8_t R_ACC_CONF  = 0x40;
static constexpr uint8_t R_ACC_RANGE = 0x41;
static constexpr uint8_t R_IF_CONF   = 0x6B;   // 辅机 I2C，用完要关
static constexpr uint8_t R_PWR_CONF  = 0x7C;
static constexpr uint8_t R_PWR_CTRL  = 0x7D;
static constexpr uint8_t R_FEAT_PAGE = 0x2F;
static constexpr uint8_t R_FEAT_DATA = 0x30;   // 每页 16 字节窗口

static constexpr uint8_t PAGE_OUT    = 0;      // 输出页：步数计数等输出
static constexpr uint8_t PAGE_PARAMS = 3;      // 25 个参数起始页
static constexpr uint8_t PAGE_CFG    = 6;      // 步计数 feature 配置页
// Bosch 字节偏移语义（bmi2_defs.h）：使能字节 = STEP_CNT_4_STRT_ADDR(0x02)
// + STEP_COUNT_FEAT_EN_OFFSET(0x01) = page6 字节 3。
// bit3=en_detector / bit4=en_counter(MASK 0x10) / bit5=en_activity(MASK 0x20)。
// 出厂字节 3 = 0x00（全关），M5Unified 配置文件内嵌值同为 0。
static constexpr uint8_t STEP_EN_BYTE = 3;
static constexpr uint8_t STEP_EN_BIT  = 0x10;

// ---------------- 手机/口袋模式（Bosch "Legacy / Smartphone" 变体）----------------
// 官方 BMI270_SensorAPI README 把配置分为三个变体：
//   base    → "Wrist worn Step counter"   腕戴，按手臂摆动建模（本文件原路径）
//   context → "Step counter"              通用
//   legacy  → "Smartphone Step counter"   手机/口袋，按身体上下起伏建模 ★
// 三者是**不同的 8 KB 微码 + 不同的算法参数**，只能整包替换，不能靠寄存器开关切换。
//
// legacy 的 feature 页布局与 base 不同：
//   步数使能位：page 4 起始 0x00 + EN_OFFSET(0x01) → **page4 字节 1 bit4**
//               （base 是 page6 字节 3）
//   步数输出：  page 0 起始 0x00，4 字节 LE —— 与 base 完全相同，读取代码无需改动
// config 数据：见 bmi270_legacy_config.h（Bosch 编译产物，逐字节不可改）

static constexpr uint8_t R_CMD        = 0x7E;
static constexpr uint8_t R_INIT_CTRL  = 0x59;
static constexpr uint8_t R_INIT_ADDR  = 0x5B;   // 10 位“字地址”：低4位在高字节? 见 uploadConfig
static constexpr uint8_t R_INIT_DATA  = 0x5E;
static constexpr uint8_t R_INT_MAP    = 0x58;
static constexpr uint8_t CMD_SOFT_RST = 0xB6;
static constexpr uint8_t PAGE_STEP_PHONE = 4;
static constexpr uint8_t PHONE_EN_BYTE   = 1;
static constexpr uint32_t PHONE_CFG_SIZE = 8192;

enum : uint8_t { MODE_WRIST = 0, MODE_PHONE = 1 };

uint8_t  mode       = MODE_WRIST;   // 实际生效的模式
uint8_t  dbgModeTry = MODE_WRIST;   // 本次尝试的模式（用于失败时排查）

// ---------------- 诊断 ----------------
uint8_t  dbgChip    = 0;
uint8_t  dbgStat    = 0;      // INTERNAL_STATUS
uint8_t  dbgAccConf = 0;
uint8_t  dbgAccRange = 0;     // 0x41 原始值（出厂态保持，不再强写）
uint8_t  dbgIfConf  = 0;      // 0x6B 出厂值（写 0 前的快照）
uint8_t  dbgPwrCtrl = 0;
uint8_t  dbgPwrConf = 0;
uint8_t  dbgPage6[16] = {0};  // page 6 回读
uint8_t  dbgPage0[16] = {0};  // page 0 回读
uint16_t dbgParam   = 0;      // page 3 第 1 个参数回读
uint8_t  dbgErr     = 0;      // 0 正常 / 1 芯片 ID / 2 读 page6 / 3 关引擎
                              // 4 写参数 / 5 读 page6 / 6 开引擎
uint8_t  dbgVerify  = 0;      // bit0 使能位回读到位 / bit1 参数回读一致
bool     ready      = false;
uint32_t lastCount  = 0;
uint32_t lastAct    = 0;

// ---------------- 底层 I2C ----------------
static inline bool wr8(uint8_t reg, uint8_t v)
{
    return M5.In_I2C.writeRegister8(ADDR_IMU, reg, v, 400000);
}
static inline uint8_t rd8(uint8_t reg)
{
    return M5.In_I2C.readRegister8(ADDR_IMU, reg, 400000);
}
static inline bool rdn(uint8_t reg, uint8_t *buf, size_t n)
{
    return M5.In_I2C.readRegister(ADDR_IMU, reg, buf, n, 400000);
}

// ---------------- feature page 访问 ----------------
// 切页后要给芯片一点时间，否则紧跟的突发读可能仍在旧页
static bool readPage(uint8_t pg, uint8_t buf[16])
{
    if (!wr8(R_FEAT_PAGE, pg)) return false;
    delayMicroseconds(400);
    return rdn(R_FEAT_DATA, buf, 16);
}

// ---------------- 初始化 ----------------
// 在 M5.begin() 之后调用（此时 8 KB 配置已上传）。
//
// ★ 最终结论（2026-10-24 完整排查，含 Bosch AN000/数据手册/API 源码核对）：
// · 使能字 = page 6 word1（字节 2-3），使能字节 = STEP_CNT_4(0x02) +
//   FEAT_EN_OFFSET(0x01) = 字节 3：bit3=en_detector / bit4=en_counter /
//   bit5=en_activity。出厂字节 3 = 0x00（全关）—— M5Unified 配置文件内嵌的
//   使能字同样是 0，计数器必须由主机使能。我们写 bit4（en_counter）是正确的。
// · 死零（走 20 步 chip=0）的根源：出厂 ACC_RANGE=0x02（±8g），而 Table 15
//   腕戴参数组按 ±2g、100Hz 标定，±8g 下信号幅度只有 1/4，落在门槛以下。
// · 灵敏度异常（正常摆臂不计数、大摆臂才触发且双计）的对账结果：唯一能正常
//   计数的配置（22:02 实验 chip=36）与其他版本的差异是 IF_CONF=0x00 —— 辅机
//   I2C 使能时 feature 引擎时序被干扰。本版恢复 IF_CONF=0x00。
// · act 字段：en_activity 从未使能，page0 字节 4-5 不是有效活动状态，仅供参考。
// · 官方渠道（数据手册/AN000/API/M5Unified/官方 demo）均无"口袋参数表"，
//   Table 15 腕戴参数是唯一公开配置；参数保持出厂，不再自写。
// ---------------- 8 KB 微码上传 ----------------
// INIT_ADDR_0 是 10 位“字地址”（1 字 = 2 字节）：低 4 位写第一字节，高 6 位写第二字节。
// 与 M5Unified BMI270_Class::_upload_file 的算式完全等价（word = byte/2）。
// 一次性写 8 KB 会让单次 I2C 传输长达 ~180 ms，这里按 512 B 分块，块间重新定位字地址。
static bool uploadConfig(const uint8_t *cfg, uint32_t total)
{
    for (uint32_t off = 0; off < total; off += 512) {
        uint32_t word = off >> 1;
        uint8_t addr[2] = { (uint8_t)((off >> 1) & 0x0F), (uint8_t)(off >> 5) };
        if (!M5.In_I2C.writeRegister(ADDR_IMU, R_INIT_ADDR, addr, 2, 400000)) {
            Serial.printf("[imu] upload addr fail @%u\n", (unsigned)off);
            return false;
        }
        uint32_t len = (total - off >= 512) ? 512 : (total - off);
        if (!M5.In_I2C.writeRegister(ADDR_IMU, R_INIT_DATA, (uint8_t *)(cfg + off), len, 400000)) {
            Serial.printf("[imu] upload data fail @%u\n", (unsigned)off);
            return false;
        }
        delay(2);
    }
    return true;
}

// ---------------- 手机/口袋模式初始化 ----------------
// 流程严格照 Bosch bmi2_sec_init：软复位 → 关 power save → 传 8 KB → INIT_CTRL=1
// → 等 INTERNAL_STATUS.init_ok → 配 ACC → 使能步数
bool beginPhone()
{
    dbgModeTry = MODE_PHONE;
    dbgErr = 0; dbgVerify = 0;

    dbgChip = rd8(R_CHIP_ID);
    if (dbgChip != 0x24) { dbgErr = 1; ready = false; return false; }

    wr8(R_CMD, CMD_SOFT_RST);
    delay(3);
    for (int i = 0; i < 16 && rd8(R_PWR_CONF) == 0; ++i) delay(1);

    wr8(R_PWR_CONF, 0x00);        // 关 advanced power save，feature 引擎要求
    delay(1);

    if (!uploadConfig(bmi270_legacy_config_file, PHONE_CFG_SIZE)) {
        dbgErr = 7; ready = false; return false;
    }
    wr8(R_INIT_CTRL, 0x01);
    wr8(R_INT_MAP, 0xFF);

    dbgStat = 0;
    for (int i = 0; i < 60; ++i) {          // init 约需 10 ms，给足 120 ms
        dbgStat = rd8(R_INT_STAT);
        if (dbgStat & 0x01) break;
        delay(2);
    }
    if (!(dbgStat & 0x01)) { dbgErr = 8; ready = false; return false; }

    // 传感器配置（与腕戴路径同标定条件）
    wr8(R_PWR_CONF, 0x00);
    wr8(R_ACC_CONF, 0x28);        // 100 Hz NORMAL
    wr8(R_ACC_RANGE, 0x00);       // ±2 g
    dbgIfConf = rd8(R_IF_CONF);
    wr8(R_IF_CONF, 0x00);         // 关辅机 I2C
    wr8(R_PWR_CTRL, 0x04);        // 只开加速度
    delay(50);

    // legacy 页 4 原始值 dump
    uint8_t chk[16];
    if (!readPage(PAGE_STEP_PHONE, chk)) { dbgErr = 2; ready = false; return false; }
    Serial.print("[imu] phone p4 pre");
    for (int i = 0; i < 16; ++i) Serial.printf(" %02X", chk[i]);
    Serial.println();

    chk[PHONE_EN_BYTE] |= STEP_EN_BIT;      // en_counter = bit4
    if (!wr8(R_FEAT_PAGE, PAGE_STEP_PHONE)) { dbgErr = 3; ready = false; return false; }
    delayMicroseconds(400);
    if (!M5.In_I2C.writeRegister(ADDR_IMU, R_FEAT_DATA, chk, 16, 400000)) {
        dbgErr = 4; ready = false; return false;
    }
    delayMicroseconds(400);

    // 回读校验
    if (readPage(PAGE_STEP_PHONE, chk)) {
        memcpy(dbgPage6, chk, 16);          // 复用缓冲：此处装的是 page 4
        dbgVerify |= (chk[PHONE_EN_BYTE] & STEP_EN_BIT) ? 0x01 : 0x00;
    }
    if (readPage(PAGE_OUT, chk)) memcpy(dbgPage0, chk, 16);

    dbgAccConf  = rd8(R_ACC_CONF);
    dbgAccRange = rd8(R_ACC_RANGE);
    dbgPwrCtrl  = rd8(R_PWR_CTRL);
    dbgPwrConf  = rd8(R_PWR_CONF);

    if (!(dbgVerify & 0x01)) { dbgErr = 9; ready = false; return false; }

    mode   = MODE_PHONE;
    ready  = true;
    dbgErr = 0;
    return true;
}

// ---------------- 腕戴模式初始化（base 变体，原路径）----------------
bool beginWrist()
{
    dbgModeTry = MODE_WRIST;
    dbgChip = rd8(R_CHIP_ID);
    if (dbgChip != 0x24) { dbgErr = 1; ready = false; return false; }

    // 只开加速度（M5Unified 走到 PWR_CTRL=0x0E，陀螺全开白耗电）
    wr8(R_PWR_CTRL, 0x04);
    // 步计数参数组（Table 15）的标定条件：±2g + 100Hz NORMAL（Bosch API 同款）
    wr8(R_ACC_CONF,  0x28);    // 100Hz + NORMAL 平均
    wr8(R_ACC_RANGE, 0x00);    // ±2g（出厂 0x02 = ±8g，步数引擎在此量程下死零）
    // 关辅机 I2C：辅机使能时 feature 引擎时序受扰（实测正常摆臂不出步）
    dbgIfConf = rd8(R_IF_CONF);
    wr8(R_IF_CONF, 0x00);
    delay(40);

    // ---- 出厂配置 dump（page 3/4/5/6），写串口留证 ----
    uint8_t chk[16];
    for (uint8_t pg = PAGE_PARAMS; pg <= PAGE_CFG; ++pg) {
        if (readPage(pg, chk)) {
            Serial.printf("[imu] factory p%u", pg);
            for (int i = 0; i < 16; ++i) Serial.printf(" %02X", chk[i]);
            Serial.println();
        }
    }

    // 使能步计数器（仅 en_counter，bit4；不碰 detector/activity）
    if (!readPage(PAGE_CFG, chk)) { dbgErr = 2; ready = false; return false; }
    chk[3] |= STEP_EN_BIT;
    if (!wr8(R_FEAT_PAGE, PAGE_CFG)) { dbgErr = 3; ready = false; return false; }
    delayMicroseconds(400);
    if (!M5.In_I2C.writeRegister(ADDR_IMU, R_FEAT_DATA, chk, 16, 400000)) {
        dbgErr = 4; ready = false; return false;
    }
    delayMicroseconds(400);

    // 全部回读，填诊断字段
    dbgVerify = 0;
    if (readPage(PAGE_CFG, chk)) {
        memcpy(dbgPage6, chk, 16);
        // en_counter = page6 字节 3 bit4，应已置位
        dbgVerify |= (chk[3] & STEP_EN_BIT) ? 0x01 : 0x00;
    }
    if (readPage(PAGE_PARAMS, chk)) {
        dbgParam = (uint16_t)(chk[0] | (chk[1] << 8));
        dbgVerify |= 0x02;             // 参数保持出厂值
    }
    if (readPage(PAGE_OUT, chk)) memcpy(dbgPage0, chk, 16);

    dbgAccConf = rd8(R_ACC_CONF);
    dbgPwrCtrl = rd8(R_PWR_CTRL);
    dbgPwrConf = rd8(R_PWR_CONF);
    dbgStat    = rd8(R_INT_STAT);
    dbgAccRange = rd8(R_ACC_RANGE);

    dbgErr = 0;
    ready  = true;
    return ready;
}

// ---------------- 统一入口 ----------------
// 默认走手机/口袋模式（legacy）；若 8 KB 上传或 init_ok 失败，用 M5Unified 的
// base 配置重新初始化并退回腕戴算法——软复位已擦掉微码，必须重传才能救回来。
bool begin()
{
    if (beginPhone()) return true;

    Serial.printf("[imu] phone init failed err=%u stat=0x%02X -> fallback wrist\n",
                  (unsigned)dbgErr, (unsigned)dbgStat);
    ready = false;
    mode  = MODE_WRIST;
    M5.Imu.begin(&M5.In_I2C);
    delay(20);
    return beginWrist();
}

// ---------------- 读计数 ----------------
// count = page 0 的 32 位步数；act = 同页字节 4-5（en_activity 未使能，值无意义）
bool readSteps(uint32_t *count, uint32_t *act)
{
    uint8_t b[16];
    if (!readPage(PAGE_OUT, b)) return false;

    *count = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
             ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    if (act) *act = (uint32_t)b[4] | ((uint32_t)b[5] << 8);

    memcpy(dbgPage0, b, 16);
    lastCount = *count;
    lastAct   = act ? *act : 0;
    return true;
}

// 进深睡前确认引擎还在跑（只保电源配置，绝不碰 feature page —— 见 begin() 注释）
void prepareSleep()
{
    if (!ready) return;
    wr8(R_PWR_CONF, 0x00);
    wr8(R_PWR_CTRL, 0x04);
}

} // namespace pedimu

// ------------------------------------------------------------
// 计数累加：硬件计数器只增不减，取模差算增量
// ------------------------------------------------------------
static uint32_t hwAccumulate(uint32_t raw)
{
    if (!g_rtc.hwInit) {           // 第一次读：只记基线
        g_rtc.hwInit = 1;
        g_rtc.hwBase = raw;
        g_rtc.hwRaw  = raw;
        return 0;
    }

    const uint32_t base = g_rtc.hwBase;
    uint32_t delta = (raw >= base) ? (raw - base) : 0;   // 芯片复位归零则丢弃
    if (delta > 20000u) delta = 0;

    g_rtc.hwBase = raw;
    g_rtc.hwRaw  = raw;
    return delta;
}
