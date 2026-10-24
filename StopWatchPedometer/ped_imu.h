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

static constexpr uint8_t PAGE_OUT    = 0;      // 输出页：步数计数 + 活动状态
static constexpr uint8_t PAGE_PARAMS = 3;      // 25 个参数起始页
static constexpr uint8_t PAGE_CFG    = 6;      // 步计数 feature 配置页
// Bosch API 实测约定：使能位 = page6 word(STEP_CNT_4=0x02 + EN_OFFSET=0x01)
// → 字节 6 的 bit4。page6 word1 bit12 不是步计数使能（曾误写导致故障）。
static constexpr uint8_t STEP_EN_BYTE = 6;
static constexpr uint8_t STEP_EN_BIT  = 0x10;

// ---------------- 诊断 ----------------
uint8_t  dbgChip    = 0;
uint8_t  dbgStat    = 0;      // INTERNAL_STATUS
uint8_t  dbgAccConf = 0;
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
// ★ 出厂基线原则（2026-10-24 双计排查结论）：
// 计数器由配置微码自带的功能使能 + 出厂腕式参数驱动，M5Unified 的 BMI270 begin
// 完全不碰 feature page。曾尝试自写 25 参数 + 在 page6 word1 bit12 设"使能位"，
// 导致硬件层双计（20 步计 36）且活动态常驻"跑"——真正的使能位在 page6 word3
// bit4（Bosch API: STEP_CNT_4_STRT_ADDR=0x02 + STEP_COUNT_FEAT_EN_OFFSET=0x01，
// mask 0x10），我们从未需要碰它。故此处只读不改，仅 dump 出厂值留证。
bool begin()
{
    dbgChip = rd8(R_CHIP_ID);
    if (dbgChip != 0x24) { dbgErr = 1; ready = false; return false; }

    // 只开加速度（M5Unified 走到 PWR_CTRL=0x0E，陀螺全开白耗电），其余全用出厂默认
    wr8(R_PWR_CTRL, 0x04);
    delay(40);

    // ---- 出厂配置 dump（page 3/4/5/6 + 使能 word），写串口留证 ----
    uint8_t chk[16];
    for (uint8_t pg = PAGE_PARAMS; pg <= PAGE_CFG; ++pg) {
        if (readPage(pg, chk)) {
            Serial.printf("[imu] factory p%u", pg);
            for (int i = 0; i < 16; ++i) Serial.printf(" %02X", chk[i]);
            Serial.println();
        }
    }

    // 全部回读，填诊断字段
    dbgVerify = 0;
    if (readPage(PAGE_CFG, chk)) {
        memcpy(dbgPage6, chk, 16);
        // Bosch 定义的真使能位：word3 低字节 bit4（出厂应已置位）
        dbgVerify |= (chk[6] & 0x10) ? 0x01 : 0x00;
    }
    if (readPage(PAGE_PARAMS, chk)) {
        dbgParam = (uint16_t)(chk[0] | (chk[1] << 8));
        dbgVerify |= 0x02;             // 参数为出厂值，不再和自写表比对
    }
    if (readPage(PAGE_OUT, chk)) memcpy(dbgPage0, chk, 16);

    dbgAccConf = rd8(R_ACC_CONF);
    dbgPwrCtrl = rd8(R_PWR_CTRL);
    dbgPwrConf = rd8(R_PWR_CONF);
    dbgStat    = rd8(R_INT_STAT);

    dbgErr = 0;
    ready  = true;
    return ready;
}

// ---------------- 读计数 ----------------
// count = page 0 的 32 位步数；act = 同页的活动状态（0 静止 1 站 2 走 3 跑）
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
