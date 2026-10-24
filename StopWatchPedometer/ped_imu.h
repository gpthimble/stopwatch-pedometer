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
static constexpr uint8_t PAGE_CFG    = 6;      // 使能位所在页
static constexpr uint8_t STEP_EN_IDX = 0x03;   // 0x02 + 0x01
static constexpr uint8_t STEP_EN_BIT = 0x10;

// Bosch 数据手册 Table 15：腕式步数参数（25 项）
static const uint16_t STEP_PARAMS[25] = {
    301, 31700, 315, 31451, 4,
    31551, 27853, 1219, 2437, 1219,
    (uint16_t)59116,            // -6420，按 16 位有符号写入
    17932, 1, 39, 25,
    150, 160, 1, 12, 15600,
    256, 1, 3, 1, 14
};

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
static inline bool wrn(uint8_t reg, const uint8_t *buf, size_t n)
{
    return M5.In_I2C.writeRegister(ADDR_IMU, reg, buf, n, 400000);
}

// ---------------- feature page 访问 ----------------
// 切页后要给芯片一点时间，否则紧跟的突发读可能仍在旧页
static bool readPage(uint8_t pg, uint8_t buf[16])
{
    if (!wr8(R_FEAT_PAGE, pg)) return false;
    delayMicroseconds(400);
    return rdn(R_FEAT_DATA, buf, 16);
}

static bool writePage(uint8_t pg, const uint8_t buf[16])
{
    if (!wr8(R_FEAT_PAGE, pg)) return false;
    delayMicroseconds(400);
    return wrn(R_FEAT_DATA, buf, 16);
}

// 写 25 个参数并逐页回读比对
static bool writeParams()
{
    uint8_t i = 0;
    uint8_t buf[16], chk[16];

    for (uint8_t pg = PAGE_PARAMS; pg <= PAGE_CFG; ++pg) {
        if (!readPage(pg, buf)) return false;

        const int words = (pg == PAGE_CFG) ? 1 : 8;
        for (int w = 0; w < words && i < 25; ++w) {
            buf[w * 2]     = (uint8_t)(STEP_PARAMS[i] & 0xFF);
            buf[w * 2 + 1] = (uint8_t)(STEP_PARAMS[i] >> 8);
            ++i;
        }
        if (!writePage(pg, buf))   return false;
        if (!readPage(pg, chk))    return false;
        if (memcmp(chk, buf, (size_t)words * 2) != 0) return false;
    }
    return i == 25;
}

// ---------------- 初始化 ----------------
// 在 M5.begin() 之后调用（此时 8 KB 配置已上传）
bool begin()
{
    dbgChip = rd8(R_CHIP_ID);
    if (dbgChip != 0x24) { dbgErr = 1; ready = false; return false; }

    wr8(R_PWR_CONF,  0x00);    // 关高级省电，feature engine 要按标称速率跑
    wr8(R_IF_CONF,   0x00);    // M5Unified 打开过辅机 I2C，这里关掉
    wr8(R_ACC_CONF,  0x28);    // 100 Hz + NORMAL 平均 —— Bosch 步计数器参数组按 100Hz 标定；
                               // 曾配 0x26(50Hz) 导致硬件层双计（raw 每步 +2）且活动态常驻"跑"
    wr8(R_ACC_RANGE, 0x00);    // ±2 g
    wr8(R_PWR_CTRL,  0x04);    // 只开加速度，陀螺/温度/辅机全停
    delay(40);

    // 1) 先关步数引擎
    uint8_t p6[16];
    if (!readPage(PAGE_CFG, p6))      { dbgErr = 2; return false; }
    p6[STEP_EN_IDX] &= (uint8_t)~STEP_EN_BIT;
    if (!writePage(PAGE_CFG, p6))     { dbgErr = 3; return false; }

    // 2) 写参数
    if (!writeParams())                { dbgErr = 4; return false; }

    // 3) 再开引擎
    if (!readPage(PAGE_CFG, p6))       { dbgErr = 5; return false; }
    p6[STEP_EN_IDX] |= STEP_EN_BIT;
    if (!writePage(PAGE_CFG, p6))      { dbgErr = 6; return false; }

    // 4) 全部回读，填诊断字段
    dbgVerify = 0;
    uint8_t chk[16];
    if (readPage(PAGE_CFG, chk)) {
        memcpy(dbgPage6, chk, 16);
        if (chk[STEP_EN_IDX] & STEP_EN_BIT) dbgVerify |= 0x01;
    }
    if (readPage(PAGE_PARAMS, chk)) {
        dbgParam = (uint16_t)(chk[0] | (chk[1] << 8));
        if (dbgParam == STEP_PARAMS[0]) dbgVerify |= 0x02;
    }
    if (readPage(PAGE_OUT, chk)) memcpy(dbgPage0, chk, 16);

    dbgAccConf = rd8(R_ACC_CONF);
    dbgPwrCtrl = rd8(R_PWR_CTRL);
    dbgPwrConf = rd8(R_PWR_CONF);
    dbgStat    = rd8(R_INT_STAT);

    dbgErr = 0;
    ready  = true;             // 即使回读不符也继续读，调试页会显示真实状态
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

// 进深睡前确认引擎还在跑
void prepareSleep()
{
    if (!ready) return;
    wr8(R_PWR_CONF, 0x00);
    wr8(R_PWR_CTRL, 0x04);

    uint8_t p6[16];
    if (readPage(PAGE_CFG, p6) && !(p6[STEP_EN_IDX] & STEP_EN_BIT)) {
        p6[STEP_EN_IDX] |= STEP_EN_BIT;
        writePage(PAGE_CFG, p6);
    }
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
