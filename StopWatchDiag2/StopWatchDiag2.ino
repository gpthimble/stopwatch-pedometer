// StopWatch 启动诊断 v2
// 目的: 在 M5.begin() 之前就输出信息, 定位"黑屏 + 反复复位"卡在哪一步。
// 输出全程走 USB CDC (115200)。烧录地址 0x0。
#include <M5Unified.h>
#include <Wire.h>

static constexpr int PIN_SDA = 47;   // StopWatch I2C: SDA=47 SCL=48
static constexpr int PIN_SCL = 48;

static void banner(const char *s)
{
    Serial.printf("\n===== %s =====\n", s);
    Serial.flush();
}

void setup()
{
    Serial.begin(115200);

    // USB 复位后主机需要重新枚举, 先持续心跳 5 秒, 保证一定被看到
    uint32_t t0 = millis();
    while (millis() - t0 < 5000) {
        Serial.printf("[HB] alive %lu ms  (若你看到这行, app 已正常运行)\n", (unsigned long)millis());
        delay(250);
    }

    banner("STAGE 0: 环境信息");
    Serial.printf("reset_reason : %d  (1=POWERON 3=SW 4=OW 5=DEEPSLEEP 6=SDIO 7=TG0WDT 8=TASK_WDT 9=WDT 12=EXT 13=BRWONOUT)\n",
                  (int)esp_reset_reason());
    Serial.printf("chip         : %s rev %d\n", ESP.getChipModel(), (int)ESP.getChipRevision());
    Serial.printf("cpu freq     : %u MHz\n", (unsigned)getCpuFrequencyMhz());
    Serial.printf("flash size   : %u bytes\n", (unsigned)ESP.getFlashChipSize());
    Serial.printf("psram size   : %u bytes  << 0 表示 PSRAM 未初始化\n", (unsigned)ESP.getPsramSize());
    Serial.printf("free heap    : %u bytes\n", (unsigned)ESP.getFreeHeap());
    Serial.flush();

    banner("STAGE 1: I2C 总线扫描 (SDA=47 SCL=48)");
    Wire.begin(PIN_SDA, PIN_SCL, 400000);
    delay(50);
    int found = 0;
    for (uint8_t a = 1; a < 127; ++a) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            const char *n = "";
            switch (a) {
                case 0x6E: n = "  <- M5PM1 电源管理 (关键)"; break;
                case 0x4F: n = "  <- M5IOE1 IO 扩展 (关键)"; break;
                case 0x32: n = "  <- RX8130 RTC"; break;
                case 0x68: n = "  <- BMI270 IMU"; break;
                case 0x15: n = "  <- CST820 触摸"; break;
                default: break;
            }
            Serial.printf("  ACK 0x%02X%s\n", a, n);
            ++found;
        }
    }
    Serial.printf("  --> 共 %d 个器件应答\n", found);
    if (found == 0) {
        Serial.println("  !! I2C 上没有任何器件: 3.3V 供电可能未建立 (PMIC 处于关机态)");
    }
    Serial.flush();

    banner("STAGE 2: 调用 M5.begin()  (此前信息均已输出完毕)");
    auto cfg = M5.config();
    cfg.clear_display = false;          // 先不清屏, 便于观察
    M5.begin(cfg);

    banner("STAGE 3: M5.begin() 已返回");
    Serial.printf("board id     : %d  (30 = M5Stack-StopWatch)\n", (int)M5.getBoard());
    Serial.printf("display      : %d x %d\n", M5.Display.width(), M5.Display.height());
    Serial.printf("imu enabled  : %d\n", M5.Imu.isEnabled() ? 1 : 0);
    Serial.printf("battery      : %d %%\n", M5.Power.getBatteryLevel());
    Serial.flush();

    M5.Display.setBrightness(180);
    M5.Display.fillScreen(TFT_GREEN);
    M5.Display.setTextColor(TFT_BLACK, TFT_GREEN);
    M5.Display.setTextSize(2);
    M5.Display.setTextDatum(middle_center);
    if (M5.getBoard() == 30) {
        M5.Display.drawString("DIAG OK", M5.Display.width() / 2, M5.Display.height() / 2 - 20);
    } else {
        M5.Display.drawString("BOARD UNKNOWN", M5.Display.width() / 2, M5.Display.height() / 2 - 20);
    }
    char b2[64];
    snprintf(b2, sizeof(b2), "board=%d  %dx%d  psram=%uK",
             (int)M5.getBoard(), M5.Display.width(), M5.Display.height(),
             (unsigned)(ESP.getPsramSize() / 1024));
    M5.Display.setTextSize(1);
    M5.Display.drawString(b2, M5.Display.width() / 2, M5.Display.height() / 2 + 30);
    Serial.println("STAGE 3 done, 屏幕应显示绿底字样");
    Serial.flush();
}

void loop()
{
    static uint32_t last = 0;
    static int i = 0;
    if (millis() - last > 3000) {
        last = millis();
        const uint16_t c[4] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE};
        M5.Display.fillScreen(c[i & 3]);
        Serial.printf("[LOOP] color %d\n", i & 3);
        ++i;
    }
    delay(10);
}
