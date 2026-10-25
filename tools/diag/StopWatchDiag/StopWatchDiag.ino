// StopWatch 串口自检固件 —— 只做硬件确认, 不含计步逻辑
// 烧录地址 0x0 ; 波特率 115200 ; 屏幕会循环显示纯色
#include <M5Unified.h>

static const char *boardName(int b)
{
    switch (b) {
        case 0:  return "unknown  << 板卡未识别, 显示参数可能不对";
        case 30: return "M5Stack-StopWatch  << 板卡识别成功";
        case 10: return "M5Stack-CoreS3";
        case 26: return "M5StickS3";
        case 22: return "M5Tab5";
        default: return "other (非 StopWatch)";
    }
}

static void drawText(const char *l1, const char *l2, uint16_t fg)
{
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(fg, TFT_BLACK);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextSize(2);
    M5.Display.drawString(l1, M5.Display.width() / 2, M5.Display.height() / 2 - 20);
    M5.Display.setTextSize(1);
    M5.Display.drawString(l2, M5.Display.width() / 2, M5.Display.height() / 2 + 30);
}

void setup()
{
    auto cfg = M5.config();
    M5.begin(cfg);

    Serial.begin(115200);
    delay(300);

    int w = M5.Display.width();
    int h = M5.Display.height();

    Serial.println();
    Serial.println("===== StopWatch DIAG =====");
    Serial.printf("board      = %d  %s\n", (int)M5.getBoard(), boardName((int)M5.getBoard()));
    Serial.printf("display    = %d x %d\n", w, h);
    Serial.printf("psram size = %u bytes\n", (unsigned)ESP.getPsramSize());
    Serial.printf("flash size = %u bytes\n", (unsigned)ESP.getFlashChipSize());
    Serial.printf("cpu freq   = %u MHz\n", (unsigned)getCpuFrequencyMhz());
    Serial.printf("imu ready  = %d\n", M5.Imu.isEnabled() ? 1 : 0);
    Serial.printf("battery    = %d %%\n", M5.Power.getBatteryLevel());
    Serial.println("==========================");

    M5.Display.setBrightness(180);
    char buf[64];
    snprintf(buf, sizeof(buf), "%dx%d  board=%d  imu=%d", w, h, (int)M5.getBoard(), M5.Imu.isEnabled() ? 1 : 0);
    drawText("DIAG OK", buf, TFT_GREEN);
    delay(1500);
}

void loop()
{
    // 纯色轮播, 用来确认面板像素全亮 / 无偏色 / 无重影
    const uint16_t colors[4] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE};
    const char    *names[4]  = {"RED", "GREEN", "BLUE", "WHITE"};
    static int i = 0;

    M5.Display.fillScreen(colors[i]);
    Serial.printf("frame %s\n", names[i]);
    i = (i + 1) & 3;
    delay(1200);
}
