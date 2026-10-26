// ============================================================
//  ped_ui.h —— 全部界面绘制与触摸手势
//  圆环与圆按钮用逐像素覆盖率混合，边缘抗锯齿
// ============================================================
#pragma once

#include "ped_font.h"

M5Canvas g_canvas(&M5.Display);
M5Canvas g_canvasPrev(&M5.Display);
M5Canvas *ui = &g_canvas;

bool g_canvasReady     = false;   // g_canvas 里有完整的一帧
bool g_canvasPrevReady = false;   // g_canvasPrev 可用（显存申请成功）

// ---------------- 脏区（整宽条带）----------------
// 官方 UserDemo 的做法是「120 行双缓冲 + LV_DISPLAY_RENDER_MODE_PARTIAL」：
// 每次只渲染并传输一条带，而不是整屏 438 KB。这里用同样的思路，
// 但保留整屏画布（省掉双缓冲切换），只把「渲染范围」和「传输范围」收窄到条带。
// 这两个值默认覆盖全屏，此时行为与改造前完全一致。
int g_bandY0 = 0;
int g_bandY1 = 0;                 // 由 uiInit 在知道 g_h 之后设为 g_h

// ---------------- 调色 ----------------
// C_ACCENT 由设置里的主题色派生，C_RUN 由强调色派生（跑步段专用）
static uint16_t C_BG, C_TRACK, C_TEXT, C_DIM, C_ACCENT, C_WARN, C_GOLD, C_BTN, C_RUN;

// 主题色与强调色改变后重算，其余中性色（文字、轨道、警告）不随主题变化
static void applyTheme()
{
    C_ACCENT = themeColor();
    C_RUN    = runColor();
}

// 字模见 ped_font.h（genfont.py 从微软雅黑/雅黑粗体生成）

static int g_w = 466, g_h = 466, g_cx = 233, g_cy = 233;

// 只在「条带」与 [y0,y1) 的交集上清背景。
// 整屏渲染时等价于 fillRect(0,y0,g_w,h)；条带渲染时不会去写带外的像素
// （PSRAM 上的 writePixel 是 read-modify-write，白写就是纯浪费）。
static void bgRect(int y0, int y1)
{
    int a = (y0 > g_bandY0) ? y0 : g_bandY0;
    int b = (y1 < g_bandY1) ? y1 : g_bandY1;
    if (a < b) ui->fillRect(0, a, g_w, b - a, C_BG);
}

// ------------------------------------------------------------
// 触感：短促的「按钮」感
// 底层脉冲在 ped_core.h（vibOn / vibService），这里只管手感分档。
// ------------------------------------------------------------
static uint32_t g_tickMs = 0;
static float    g_slideHint = 0.0f;   // 横向切页进度 0..1（设置页底部画条）

// 提示音：只有「震动+声音」这一档才响
static void uiBeep(int freq, int ms)
{
    if (g_cfg.sound < 2) return;
    if (!M5.Speaker.isEnabled()) return;
    M5.Speaker.tone(freq, ms);
}

static void uiTick()                            // 过一格：轻一点的齿感
{
    const uint32_t now = millis();
    if (now - g_tickMs < 60) return;            // 快拖时合并成「哒、哒」的节律
    g_tickMs = now;
    vibOn(170, 10);
}

static void uiSettle()                          // 吸附落位的一瞬间：短促一下
{
    g_tickMs = millis();
    vibOn(200, 14);
}

static void uiConfirm()                         // 确认：略长一点，仍然是短震
{
    g_tickMs = millis();
    vibOn(200, 26);
}

// 翻页：一记干脆的短震；「震动+声音」档再补一声轻响
static void uiPage()
{
    g_tickMs = millis();
    vibOn(190, 12);
    uiBeep(1320, 22);
}

// 星期：Sakamoto 算法，不依赖 RTC 的 weekDay 字段
static const char *weekdayName(int y, int m, int d)
{
    static const char *zh[7] = {"周日", "周一", "周二", "周三",
                                "周四", "周五", "周六"};
    static const char *en[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    int w = (y + y / 4 - y / 100 + y / 400 + t[(m - 1) % 12] + d) % 7;
    if (w < 0) w += 7;
    return g_cfg.lang ? en[w] : zh[w];
}

// 页面：主页 / 历史 / 设置入口参与左右翻页。
// 设置列表从入口点按进入，对时与配色再从列表进入；PG_DEBUG 仅串口可达。
enum { PG_MAIN = 0, PG_HIST, PG_DOOR, PG_SET, PG_TIME, PG_COLOR, PG_DEBUG };
#define PG_CYCLE_MAX PG_DOOR

// 主页的子视图与历史的子视图都由上下滑动切换
static bool g_mainDetail = false;   // 主页：今日详情
static int  g_histIdx    = -1;      // 历史：-1 柱状图，0..6 七天

// ------------------------------------------------------------
// 抗锯齿基础：按覆盖率把颜色与黑色背景混合
// ------------------------------------------------------------
static inline uint16_t shade565(uint16_t c, uint32_t a)
{
    const uint32_t r = ((c >> 11) & 0x1F) * a / 255u;
    const uint32_t g = ((c >>  5) & 0x3F) * a / 255u;
    const uint32_t b = ( c        & 0x1F) * a / 255u;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

// ------------------------------------------------------------
// 文本：8bpp 字模，逐像素与黑底混合
// ------------------------------------------------------------
static inline const PedGlyph *pedFind(const PedFont &f, uint32_t cp)
{
    int lo = 0, hi = (int)f.n - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) >> 1;
        if (f.gl[mid].cp == cp) return &f.gl[mid];
        if (f.gl[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
    }
    return nullptr;
}

static inline uint32_t pedNextCp(const char *&s)
{
    const uint8_t c = (uint8_t)*s;
    if (c < 0x80) { ++s; return c; }
    uint32_t cp = 0;
    int n = 0;
    if      ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 3; }
    else { ++s; return 0xFFFD; }
    ++s;
    for (int i = 0; i < n && *s; ++i) {
        cp = (cp << 6) | ((uint8_t)*s & 0x3F);
        ++s;
    }
    return cp;
}

static int pedTextWidth(const char *s, const PedFont &f)
{
    int w = 0;
    while (*s) {
        const char *p = s;
        const PedGlyph *g = pedFind(f, pedNextCp(p));
        w += g ? g->adv : (f.ascent / 2);
        s = p;
    }
    return w;
}

// align: 0 左 / 1 中 / 2 右，y 为垂直中心
static void pedDrawText(int x, int y, const char *s, const PedFont &f,
                        uint16_t col, int align)
{
    s = trText(s);                  // 中文原文 → 当前语言
    const int w = pedTextWidth(s, f);
    int pen = (align == 1) ? (x - w / 2) : (align == 2 ? x - w : x);
    const int base = y + (f.ascent - f.descent) / 2;

    while (*s) {
        const char *p = s;
        const PedGlyph *g = pedFind(f, pedNextCp(p));
        s = p;
        if (!g) { pen += f.ascent / 2; continue; }

        if (g->w && g->h) {
            const uint8_t *bm = f.bmp + g->ofs;
            const int gx = pen + g->xo;
            const int gy = base - f.ascent + g->yo;
            for (int j = 0; j < g->h; ++j) {
                const int py = gy + j;
                // 条带裁剪：字形整行落在带外时直接跳过，连 writePixel 都不用发。
                // writePixel 对 PSRAM 是 read-modify-write，这一跳是主要收益之一。
                if (py >= g_bandY0 && py < g_bandY1) {
                    for (int i = 0; i < g->w; ++i) {
                        const uint8_t a = bm[i];
                        if (!a) continue;
                        const int px = gx + i;
                        if (px < 0 || px >= g_w) continue;
                        ui->writePixel(px, py, (a >= 250) ? col : shade565(col, a));
                    }
                }
                bm += g->w;
            }
        }
        pen += g->adv;
    }
}

static void textC(int x, int y, const char *s, const PedFont &f, uint16_t col)
{ pedDrawText(x, y, s, f, col, 1); }

static void textL(int x, int y, const char *s, const PedFont &f, uint16_t col)
{ pedDrawText(x, y, s, f, col, 0); }

static void textR(int x, int y, const char *s, const PedFont &f, uint16_t col)
{ pedDrawText(x, y, s, f, col, 2); }


// 圆环：rOut/rIn 半径，a0 起始角（度，-90 = 12 点），span 扫过角度
// 扇区部分用 cFill，其余用 cTrack，一次逐像素混合完成
// drawTrack=false 时只叠加扇区，不重画轨道 —— 用于在同一圈上叠第二段颜色
static void drawRingAA(float rOut, float rIn, float a0, float span,
                       uint16_t cTrack, uint16_t cFill, bool drawTrack = true)
{
    if (rIn < 0.0f)  rIn  = 0.0f;
    if (rIn >= rOut) return;

    const bool doFill   = (span > 0.05f);
    const bool fullFill = (span >= 359.5f);
    const bool wide     = (span > 180.0f);

    const float u0x = cosf(a0 * DEG_TO_RAD), u0y = sinf(a0 * DEG_TO_RAD);
    const float a1  = a0 + span;
    const float u1x = cosf(a1 * DEG_TO_RAD), u1y = sinf(a1 * DEG_TO_RAD);

    const uint32_t tr = (cTrack >> 11) & 0x1F, tg = (cTrack >> 5) & 0x3F, tb = cTrack & 0x1F;
    const uint32_t fr = (cFill  >> 11) & 0x1F, fg = (cFill  >> 5) & 0x3F, fb = cFill  & 0x1F;

    const int yBeg = (int)floorf(g_cy - rOut);
    const int yEnd = (int)ceilf (g_cy + rOut);

    for (int y = yBeg; y <= yEnd; ++y) {
        if (y < g_bandY0 || y >= g_bandY1) continue;
        const float dy = (float)y + 0.5f - (float)g_cy;
        const float oo = rOut * rOut - dy * dy;
        if (oo <= 0.0f) continue;

        const float xo = sqrtf(oo);
        const float ii = rIn * rIn - dy * dy;
        const float xi = (ii > 0.0f) ? sqrtf(ii) : 0.0f;

        for (int side = 0; side < 2; ++side) {
            const float lo = (side == 0) ? (g_cx - xo) : (g_cx + xi);
            const float hi = (side == 0) ? (g_cx - xi) : (g_cx + xo);
            int xs = (int)floorf(lo);
            int xe = (int)ceilf (hi);
            if (xs < 0) xs = 0;
            if (xe > g_w - 1) xe = g_w - 1;

            for (int x = xs; x <= xe; ++x) {
                const float dx = (float)x + 0.5f - (float)g_cx;
                const float r  = sqrtf(dx * dx + dy * dy);

                // 径向覆盖率：内外两条边各取半像素过渡
                float cov = 0.5f + (rOut - r);
                if (cov <= 0.0f) continue;
                const float cin = 0.5f + (r - rIn);
                if (cin < cov) cov = cin;
                if (cov <= 0.0f) continue;   // 负覆盖率转 uint32 会溢出成垃圾色
                if (cov > 1.0f) cov = 1.0f;

                // 角度覆盖率
                float ang = 0.0f;
                if (doFill) {
                    if (fullFill) {
                        ang = 1.0f;
                    } else {
                        // 到两条边界射线的有符号距离，单位 = 像素
                        const float s0 = u0x * dy - u0y * dx;
                        const float s1 = u1x * dy - u1y * dx;
                        float e0 = 0.5f + s0;
                        float e1 = 0.5f - s1;
                        if (e0 < 0.0f) e0 = 0.0f; else if (e0 > 1.0f) e0 = 1.0f;
                        if (e1 < 0.0f) e1 = 0.0f; else if (e1 > 1.0f) e1 = 1.0f;
                        ang = wide ? (e0 + e1 - e0 * e1) : (e0 * e1);
                        if (ang < 0.0f) ang = 0.0f;
                    }
                } else {
                    // 无进度时整圈都是轨道色：下面用 aT = cov
                }

                uint32_t aF = (uint32_t)(cov * ang * 255.0f + 0.5f);
                uint32_t aT = (uint32_t)(cov * 255.0f + 0.5f) - aF;
                // 叠加模式：不画轨道，扇区边缘的残余覆盖率并入扇区色
                if (!drawTrack) {
                    if (!aF) continue;
                    aF += aT;
                    aT = 0;
                }
                if (!aT && !aF) continue;

                const uint32_t R = (tr * aT + fr * aF) / 255u;
                const uint32_t G = (tg * aT + fg * aF) / 255u;
                const uint32_t B = (tb * aT + fb * aF) / 255u;
                ui->writePixel(x, y, (uint16_t)((R << 11) | (G << 5) | B));
            }
        }
    }
}

// 快速 atan2，误差约 0.004 rad，用于按角度取色
static inline float cwAtan2(float y, float x)
{
    const float ax = fabsf(x), ay = fabsf(y);
    const float a  = (ax > ay) ? ay / (ax + 1e-9f) : ax / (ay + 1e-9f);
    const float s  = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    if (ay > ax)  r = 1.57079637f - r;
    if (x < 0.0f) r = 3.14159274f - r;
    if (y < 0.0f) r = -r;
    return r;
}

// 屏幕坐标 → 色相：0° 在正上方，顺时针递增
static inline int cwHueOf(int dx, int dy)
{
    int h = (int)lroundf(cwAtan2((float)dx, (float)(-dy)) * 57.29578f);
    if (h < 0)   h += 360;
    if (h >= 360) h -= 360;
    return h;
}

// 时间轴外环：整圈按当天 1440 个 1 分钟时段等分，0 点在正上方。
// 已过的时段按该分钟的强度取色：无步数 = 静止色，走路 = 主题色，跑步 = 强调色。
// 当前分钟只涂到此刻为止，其后与未到的分钟用更暗的未来色。
static void drawRingTimeline(float rOut, float rIn, float span,
                             uint16_t cIdle, uint16_t cFuture,
                             uint16_t cWalk, uint16_t cRun)
{
    if (rIn < 0.0f) rIn = 0.0f;
    if (rIn >= rOut) return;
    if (span >= 360.0f) span = 360.0f;

    const int   cur = g_bktCur;
    const float k   = (float)BKT_N / (span > 0.5f ? span : 0.5f);
    const int yBeg = (int)floorf(g_cy - rOut);
    const int yEnd = (int)ceilf (g_cy + rOut);

    for (int y = yBeg; y <= yEnd; ++y) {
        if (y < g_bandY0 || y >= g_bandY1) continue;
        const float dy = (float)y + 0.5f - (float)g_cy;
        const float oo = rOut * rOut - dy * dy;
        if (oo <= 0.0f) continue;

        const float xo = sqrtf(oo);
        const float ii = rIn * rIn - dy * dy;
        const float xi = (ii > 0.0f) ? sqrtf(ii) : 0.0f;

        for (int side = 0; side < 2; ++side) {
            const float lo = (side == 0) ? (g_cx - xo) : (g_cx + xi);
            const float hi = (side == 0) ? (g_cx - xi) : (g_cx + xo);
            int xs = (int)floorf(lo);
            int xe = (int)ceilf (hi);
            if (xs < 0) xs = 0;
            if (xe > g_w - 1) xe = g_w - 1;

            for (int x = xs; x <= xe; ++x) {
                const float dx = (float)x + 0.5f - (float)g_cx;
                const float r  = sqrtf(dx * dx + dy * dy);

                float cov = 0.5f + (rOut - r);
                if (cov <= 0.0f) continue;
                const float cin = 0.5f + (r - rIn);
                if (cin < cov) cov = cin;
                if (cov <= 0.0f) continue;
                if (cov > 1.0f) cov = 1.0f;

                const float a = (float)cwHueOf((int)dx, (int)dy);
                uint16_t c = cFuture;
                if (a < span) {
                    const float idx = a * k;
                    int b = (int)idx;
                    if (b < 0) b = 0;
                    if (b >= BKT_N) b = BKT_N - 1;
                    // 当前分钟只涂到此刻，其余按该分钟记录的强度取色
                    if (b > cur || (b == cur && (idx - (float)b) > g_bktFrac)) {
                        c = cFuture;
                    } else {
                        const uint8_t lv = bktGet(g_bkt.lvl, b);
                        c = (lv == 2) ? cRun : (lv == 1 ? cWalk : cIdle);
                    }
                }
                ui->writePixel(x, y, shade565(c, (uint32_t)(cov * 255.0f + 0.5f)));
            }
        }
    }
}

// 圆形填充，边缘抗锯齿
static void fillCircleAA(float ccx, float ccy, float r, uint16_t col)
{
    const int yBeg = (int)floorf(ccy - r);
    const int yEnd = (int)ceilf (ccy + r);
    for (int y = yBeg; y <= yEnd; ++y) {
        if (y < g_bandY0 || y >= g_bandY1) continue;
        const float dy = (float)y + 0.5f - ccy;
        const float oo = r * r - dy * dy;
        if (oo <= 0.0f) continue;
        const float xw = sqrtf(oo);
        int xs = (int)floorf(ccx - xw);
        int xe = (int)ceilf (ccx + xw);
        if (xs < 0) xs = 0;
        if (xe > g_w - 1) xe = g_w - 1;
        for (int x = xs; x <= xe; ++x) {
            const float dx = (float)x + 0.5f - ccx;
            const float d  = sqrtf(dx * dx + dy * dy);
            float cov = 0.5f + (r - d);
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            ui->writePixel(x, y, shade565(col, (uint32_t)(cov * 255.0f + 0.5f)));
        }
    }
}

// 圆角矩形，边缘抗锯齿
static void fillRoundRectAA(float x0, float y0, float w, float h, float r, uint16_t col)
{
    const float cx0 = x0 + r, cy0 = y0 + r;
    const float cx1 = x0 + w - r, cy1 = y0 + h - r;
    const int xs0 = (int)floorf(x0) - 1, xs1 = (int)ceilf(x0 + w) + 1;
    const int ys0 = (int)floorf(y0) - 1, ys1 = (int)ceilf(y0 + h) + 1;
    for (int y = ys0; y <= ys1; ++y) {
        if (y < g_bandY0 || y >= g_bandY1) continue;
        const float py = (float)y + 0.5f;
        const float qy = fmaxf(fmaxf(cy0 - py, py - cy1), 0.0f);
        for (int x = xs0; x <= xs1; ++x) {
            if (x < 0 || x >= g_w) continue;
            const float px = (float)x + 0.5f;
            const float qx = fmaxf(fmaxf(cx0 - px, px - cx1), 0.0f);
            const float d  = sqrtf(qx * qx + qy * qy) - r;
            float cov = 0.5f - d;
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            ui->writePixel(x, y, shade565(col, (uint32_t)(cov * 255.0f + 0.5f)));
        }
    }
}

// ------------------------------------------------------------
// 主页面
// ------------------------------------------------------------
// 电量：四段细条，纯粹的扁平刻度，不做电池外形
static void drawBattery(int cx, int cy, int pct, bool usb)
{
    if (pct > 100) pct = 100;

    const int segW = 16, segH = 10, gap = 5;
    const int barW = 4 * segW + 3 * gap;
    const int x0   = cx - barW / 2;

    const uint16_t on  = usb ? C_ACCENT
                             : (pct >= 40 ? C_ACCENT : (pct >= 20 ? C_GOLD : C_WARN));
    const uint16_t off = usb ? shade565(C_ACCENT, 90) : C_TRACK;

    int lit = (pct < 0) ? 0 : (pct + 24) / 25;   // 0..4；读不到电量就全空
    if (pct > 0 && lit < 1) lit = 1;

    for (int i = 0; i < 4; ++i) {
        const int x = x0 + i * (segW + gap);
        if (i < lit) ui->fillRoundRect(x, cy - segH / 2, segW, segH, segH / 2, on);
        else         ui->drawRoundRect(x, cy - segH / 2, segW, segH, segH / 2, off);
    }
}

static void drawMain()
{
    char buf[48];
    const float prog = g_cfg.goal ? (float)g_steps / (float)g_cfg.goal : 0.0f;
    const float p    = prog > 1.0f ? 1.0f : prog;

    // 双环：外圈是 24 小时活动状态，内圈是目标完成度，外沿贴住屏幕边缘
    const float rOut = (float)g_cx - 2.0f;
    const float oIn  = rOut - 13.0f;          // 外圈内沿
    const float iOut = oIn - 5.0f;            // 内圈外沿
    const float iIn  = iOut - 22.0f;          // 内圈内沿

    // 外圈：0 点在正上方，顺时针走完 24 小时，每分钟一格
    // 已过的分钟区分静止 / 走路 / 跑步，未到的分钟用更暗的轨道色
    drawRingTimeline(rOut, oIn, 360.0f, C_BTN, C_TRACK, C_ACCENT, C_RUN);

    // 内圈：当日目标完成度
    uint16_t cFill = C_ACCENT;
    if (p > 0.85f) cFill = lerp565(C_ACCENT, C_GOLD, (p - 0.85f) / 0.15f);
    drawRingAA(iOut, iIn, -90.0f, 360.0f * p, C_TRACK, cFill);

    const bool hasRtc = M5.Rtc.isEnabled() && rtcValid();

    // 时间
    if (hasRtc) {
        auto dt = M5.Rtc.getDateTime();
        snprintf(buf, sizeof(buf), "%02d:%02d", dt.time.hours, dt.time.minutes);
    } else {
        snprintf(buf, sizeof(buf), "--:--");
    }
    textC(g_cx, g_cy - 146, buf, F_MID, C_TEXT);

    // 日期 + 星期
    if (hasRtc) {
        auto dt = M5.Rtc.getDateTime();
        if (g_cfg.lang) snprintf(buf, sizeof(buf), "%d/%d %s",
                                 dt.date.month, dt.date.date,
                                 weekdayName(dt.date.year, dt.date.month, dt.date.date));
        else            snprintf(buf, sizeof(buf), "%d月%d日 %s",
                                 dt.date.month, dt.date.date,
                                 weekdayName(dt.date.year, dt.date.month, dt.date.date));
    } else {
        snprintf(buf, sizeof(buf), T("--月--日", "--/--"));
    }
    textC(g_cx, g_cy - 106, buf, F_L, C_DIM);

    // 步数：整个画面的主角，居中
    snprintf(buf, sizeof(buf), "%u", (unsigned)g_steps);
    textC(g_cx, g_cy - 4, buf, F_HUGE, C_TEXT);

    snprintf(buf, sizeof(buf), T("%d%% · 目标 %u", "%d%% · goal %u"),
             (int)(prog * 100.0f + 0.5f), (unsigned)g_cfg.goal);
    textC(g_cx, g_cy + 94, buf, F_S, C_ACCENT);

    if (g_vbat > 0 && g_vbat < 3450) {
        snprintf(buf, sizeof(buf), T("电量低 %d%% 请充电", "Low battery %d%%, charge now"), g_battery);
        textC(g_cx, g_cy + 136, buf, F_S, C_WARN);
    } else {
        snprintf(buf, sizeof(buf), T("%.2f 公里 · %d 千卡", "%.2f km · %d kcal"),
                 kmOf(g_steps), kcalOf(g_steps));
        textC(g_cx, g_cy + 136, buf, F_S, C_DIM);
    }

    // 底部电量标志（只有图标，居中）
    drawBattery(g_cx, g_cy + 168, g_battery, g_usbPower);
}

// ------------------------------------------------------------
// 今日数据
// ------------------------------------------------------------
static void drawStats()
{
    char v[24];
    textC(g_cx, 78, "今日数据", F_H1, C_TEXT);

    const char *keys[] = {"距离", "消耗", "活跃", "步频", "完成"};
    int y = 148;
    for (int i = 0; i < 5; ++i) {
        if (i) ui->drawFastHLine(70, y - 30, g_w - 140, C_TRACK);
        switch (i) {
            case 0: snprintf(v, sizeof(v), T("%.2f 公里", "%.2f km"), kmOf(g_steps)); break;
            case 1: snprintf(v, sizeof(v), T("%d 千卡", "%d kcal"), kcalOf(g_steps)); break;
            case 2: snprintf(v, sizeof(v), "%u:%02u:%02u",
                             (unsigned)(g_activeSec / 3600),
                             (unsigned)((g_activeSec / 60) % 60),
                             (unsigned)(g_activeSec % 60)); break;
            case 3: snprintf(v, sizeof(v), T("%u 步/分", "%u spm"), (unsigned)g_cadence); break;
            default: snprintf(v, sizeof(v), "%d%%",
                             g_cfg.goal ? (int)(100.0f * g_steps / g_cfg.goal) : 0); break;
        }
        textL(78, y, keys[i], F_L, C_DIM);
        textR(g_w - 78, y, v, F_H1, C_TEXT);
        y += 62;
    }

    if (g_battery >= 0) snprintf(v, sizeof(v), T("电量 %d%%", "Battery %d%%"), g_battery);
    else                snprintf(v, sizeof(v), "USB 供电");
    textC(g_cx, 430, v, F_L, C_DIM);
}

// ------------------------------------------------------------
// 设置入口
// ------------------------------------------------------------
// 齿角圆化：和距离场配合的多项式 smooth-min，k 越大圆角越大
static inline float sminQ(float a, float b, float k)
{
    float h = 0.5f + 0.5f * (b - a) / k;
    if (h < 0.0f) h = 0.0f;
    else if (h > 1.0f) h = 1.0f;
    return b + (a - b) * h - k * h * (1.0f - h);
}

// 齿轮：8 个梯形齿 + 轴孔。齿宽由齿根到齿顶线性收窄，齿根与齿顶用 smooth-min
// 圆化，轮廓就是一枚真齿轮的侧影。边界按符号距离求覆盖率，所以斜边、齿顶弧、
// 轴孔都是抗锯齿的（旧版本按扇形三角形栅格化，窄处会漏像素，就是那些黑点）。
// 齿身只沿光轴做一层渐变，不描边、不打轮廓光 —— 图标越简单越像齿轮。
static void drawGear(int ccx, int ccy)
{
    const float R0   = 74.0f;                  // 齿根圆
    const float R1   = 96.0f;                  // 齿顶圆
    const float WR   = 18.0f;                  // 齿根处的半齿宽
    const float WT   = 14.5f;                  // 齿顶处的半齿宽
    const float RH   = 32.0f;                  // 轴孔
    const float step = 0.7853982f;             // 2π/8
    const float K    = 5.0f;                   // 齿根/齿顶圆角

    const float sat = 0.90f;                   // 与 themeColor() 同一基色
    const float val = g_cfg.themeVal / 100.0f;
    const uint16_t cLit  = hsv565(g_cfg.themeHue, sat * 0.62f, fminf(1.0f, val + 0.06f));
    const uint16_t cDeep = hsv565(g_cfg.themeHue, fminf(1.0f, sat * 1.08f), val * 0.55f);

    const float LX = -0.40f, LY = -0.92f;      // 光从左上方来
    const float invDR = 1.0f / (R1 - R0);
    const float inv2R = 1.0f / (2.0f * R1);
    const int   r1 = (int)R1 + 2;

    for (int y = ccy - r1; y <= ccy + r1; ++y) {
        if (y < g_bandY0 || y >= g_bandY1) continue;
        const float dy = (float)y + 0.5f - (float)ccy;
        for (int x = ccx - r1; x <= ccx + r1; ++x) {
            if (x < 0 || x >= g_w) continue;
            const float dx = (float)x + 0.5f - (float)ccx;
            const float r2 = dx * dx + dy * dy;
            if (r2 > (R1 + 1.5f) * (R1 + 1.5f)) continue;
            const float r = sqrtf(r2);

            float a = fmodf(cwAtan2(dy, dx) + step * 0.5f, step);
            if (a < 0.0f) a += step;
            a = fabsf(a - step * 0.5f);                      // 到最近齿中心的角距
            const float u = r * sinf(a);                     // 到齿中心线的垂距

            // 半齿宽随半径收窄：齿根 WR → 齿顶 WT，齿侧就是斜的
            const float rc = (r < R0) ? R0 : (r > R1 ? R1 : r);
            const float w  = WT + (WR - WT) * (R1 - rc) * invDR;

            float d;
            if (r < R0) {
                // 轮体内：正对齿间时最近的边界是齿根弧，齿下方是齿根圆角
                const float dr = R0 - r, du = w - u;
                d = (u > w) ? dr : sqrtf(dr * dr + du * du);
            } else {
                const float dArc = ((u < w) ? R1 : R0) - r;
                const float dSid = w - u;
                d = (dSid > 0.0f) ? sminQ(dArc, dSid, K)
                                  : -sminQ(-dArc, -dSid, K);
            }
            d = fminf(d, r - RH);                            // 挖掉轴孔

            float cov = 0.5f + d;
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;

            // 渐变：沿光轴线性插值，亮面在左上
            float t = 0.5f - (dx * LX + dy * LY) * inv2R;
            if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
            uint16_t c = lerp565(cLit, cDeep, t);

            // 孔边压一点暗，轴孔才像通的
            float hq = (r - RH) * (1.0f / 6.0f);
            if (hq < 1.0f) {
                if (hq < 0.0f) hq = 0.0f;
                c = shade565(c, (uint32_t)(255.0f - 50.0f * (1.0f - hq)));
            }
            ui->writePixel(x, y, (cov >= 0.996f) ? c : lerp565(C_BG, c, cov));
        }
    }
}

// 设置不与其它页面同级翻页，单独一页作入口，点按后才进入列表
static void drawDoor()
{
    drawGear(g_cx, 196);
    textC(g_cx, 352, "设置", F_H1, C_TEXT);
}

// ------------------------------------------------------------
// 近 7 天
// ------------------------------------------------------------
// 20261024 → 自 2000-01-01 起的日数（用于按日期对齐柱子）
static int32_t keyToDays(int32_t key)
{
    if (key <= 0) return -1;
    const int y = key / 10000, m = (key / 100) % 100, d = key % 100;
    if (y < 2000 || y > 2099 || m < 1 || m > 12 || d < 1 || d > 31) return -1;

    static const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int32_t n = 0;
    for (int yy = 2000; yy < y; ++yy)
        n += ((yy % 4 == 0 && yy % 100 != 0) || yy % 400 == 0) ? 366 : 365;
    for (int mm = 1; mm < m; ++mm) n += mdays[mm - 1];
    if (m > 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) ++n;
    return n + d - 1;
}

static void drawHist()
{
    char v[32];
    histSyncToday();                 // 今天的实时成绩也算一根柱子
    textC(g_cx, 78, "近 7 天", F_H1, C_TEXT);
    textC(g_cx, 112, "上下滑动 · 查看每日", F_S, C_DIM);

    const int x0 = 70, x1 = g_w - 70;
    const int yb = 336, yt = 150;
    const int slot = (x1 - x0) / HIST_DAYS;
    const int bw   = slot - 14;

    // 今天固定在最右一格，向左回推 6 天；没记录的那天留空
    int32_t todayK = todayKey();
    if (todayK == 0) todayK = g_dayKey;
    const int32_t baseDays = keyToDays(todayK);

    DayRec slotRec[HIST_DAYS];
    uint32_t sum = 0, mx = g_cfg.goal;
    int      cnt = 0;

    for (int i = 0; i < HIST_DAYS; ++i) {
        slotRec[i].day = 0; slotRec[i].steps = 0;
        if (baseDays < 0) break;
        const int32_t want = baseDays - (HIST_DAYS - 1 - i);
        for (int j = 0; j < HIST_DAYS; ++j) {
            if (keyToDays(g_hist[j].day) == want) { slotRec[i] = g_hist[j]; break; }
        }
        if (slotRec[i].day) {
            sum += slotRec[i].steps; ++cnt;
            if (slotRec[i].steps > mx) mx = slotRec[i].steps;
        }
    }

    if (baseDays < 0 || sum == 0) {               // 全新设备 / 全为 0：别留一片空白
        textC(g_cx, 218, "还没有记录", F_H1, C_DIM);
        textC(g_cx, 262, "走几步就会出现在这里", F_S, shade565(C_DIM, 150));
        return;
    }

    ui->drawFastHLine(x0, yb, x1 - x0, C_TRACK);

    for (int i = 0; i < HIST_DAYS; ++i) {
        const DayRec r = slotRec[i];
        const int bx = x0 + i * slot + (slot - bw) / 2;

        if (r.day == 0) {
            ui->fillRect(bx, yb - 4, bw, 4, C_TRACK);
            continue;
        }
        int hgt = (int)((float)(yb - yt) * (float)r.steps / (float)mx);
        if (hgt < 4) hgt = 4;
        const bool isToday = (keyToDays(r.day) == baseDays);
        uint16_t c = (r.steps >= g_cfg.goal) ? C_ACCENT : lerp565(C_DIM, C_ACCENT, 0.45f);
        if (isToday && r.steps < g_cfg.goal) c = lerp565(C_ACCENT, C_DIM, 0.25f);
        ui->fillRect(bx, yb - hgt, bw, hgt, c);

        // 柱顶一段按跑步占比着色，与主页色环同一强调色
        if (r.run > 0) {
            int hr = (int)((float)hgt * (float)r.run / (float)r.steps + 0.5f);
            if (hr > hgt) hr = hgt;
            if (hr >= 3) ui->fillRect(bx, yb - hgt, bw, hr, C_RUN);
        }

        snprintf(v, sizeof(v), "%u", (unsigned)r.steps);
        textC(bx + bw / 2, yb - hgt - 12, v, F_TINY, C_DIM);

        snprintf(v, sizeof(v), "%d", (int)(r.day % 100));
        textC(bx + bw / 2, yb + 22, v, F_S, isToday ? C_TEXT : C_DIM);
    }

    snprintf(v, sizeof(v), T("日均 %u 步", "Avg %u steps"), (unsigned)(cnt ? sum / cnt : 0));
    textC(g_cx, 408, v, F_H1, C_ACCENT);
}

static int daysInMonth(int y, int m);

// 日期前后移动若干天（只接受 YYYYMMDD，最多跨几个月）
static int32_t keyShift(int32_t key, int delta)
{
    if (key <= 0 || delta == 0) return key;
    int y = (int)(key / 10000), m = (int)((key / 100) % 100), d = (int)(key % 100);
    const int step = (delta > 0) ? 1 : -1;
    for (int n = 0; n < (delta > 0 ? delta : -delta); ++n) {
        const int dm = daysInMonth(y, m);
        d += step;
        if (d > dm) { d = 1; if (++m > 12) { m = 1; ++y; } }
        if (d < 1)  { if (--m < 1) { m = 12; --y; } d = daysInMonth(y, m); }
    }
    return (int32_t)y * 10000 + m * 100 + d;
}

// 历史子视图：某一天的详情，版式与主页一致
// idx = 0 今天，1 昨天，…… 6 为六天前
static void drawHistDay(int idx)
{
    char b[56];
    histSyncToday();

    int32_t baseK = todayKey();
    if (baseK == 0) baseK = g_dayKey;
    if (idx < 0)    idx = 0;
    if (idx >= HIST_DAYS) idx = HIST_DAYS - 1;
    const int32_t dayK = keyShift(baseK, -idx);
    const int32_t key  = keyToDays(dayK);

    DayRec r = {0, 0, 0};
    for (int j = 0; j < HIST_DAYS; ++j) {
        if (key >= 0 && keyToDays(g_hist[j].day) == key) { r = g_hist[j]; break; }
    }

    const float prog = g_cfg.goal ? (float)r.steps / (float)g_cfg.goal : 0.0f;
    const float p    = prog > 1.0f ? 1.0f : prog;

    // 单环：当天目标完成度，环尾按跑步步数占比叠强调色
    const float rOut = (float)g_cx - 2.0f;
    const float rIn  = rOut - 22.0f;
    drawRingAA(rOut, rIn, -90.0f, 360.0f * p, C_TRACK, C_ACCENT);
    if (r.steps && r.run) {
        float f = (float)r.run / (float)r.steps;
        if (f > 1.0f) f = 1.0f;
        const float span = 360.0f * p * f;
        if (span > 0.8f) {
            drawRingAA(rOut, rIn, -90.0f + 360.0f * p - span, span, C_TRACK, C_RUN, false);
        }
    }

    // 顶部：第几天，日期与星期
    snprintf(b, sizeof(b), "%s · %d/%d", T("今天", "Today"),
             idx + 1, HIST_DAYS);
    textC(g_cx, g_cy - 168, b, F_S, C_DIM);

    const int dm2 = (int)((dayK / 100) % 100), dd2 = (int)(dayK % 100);
    const char *wk2 = weekdayName((int)(dayK / 10000), dm2, dd2);
    if (dayK > 0) {
        if (g_cfg.lang) snprintf(b, sizeof(b), "%d/%d %s", dm2, dd2, wk2);
        else            snprintf(b, sizeof(b), "%d月%d日 %s", dm2, dd2, wk2);
    } else {
        snprintf(b, sizeof(b), T("--月--日", "--/--"));
    }
    textC(g_cx, g_cy - 128, b, F_L, C_TEXT);

    if (!r.day) {
        textC(g_cx, g_cy - 10, "无记录", F_MID, C_DIM);
        textC(g_cx, g_cy + 60, "这一天没有同步过成绩", F_S, shade565(C_DIM, 160));
        textC(g_cx, 414, "上下滑动 · 切换日期", F_S, C_DIM);
        return;
    }

    snprintf(b, sizeof(b), "%u", (unsigned)r.steps);
    textC(g_cx, g_cy - 4, b, F_HUGE, C_TEXT);

    snprintf(b, sizeof(b), T("%d%% · 目标 %u", "%d%% · goal %u"),
             (int)(prog * 100.0f + 0.5f), (unsigned)g_cfg.goal);
    textC(g_cx, g_cy + 94, b, F_S, C_ACCENT);

    snprintf(b, sizeof(b), T("%.2f 公里 · %d 千卡", "%.2f km · %d kcal"),
             kmOf(r.steps), kcalOf(r.steps));
    textC(g_cx, g_cy + 132, b, F_S, C_DIM);

    if (r.run) {
        snprintf(b, sizeof(b), T("跑步 %u 步 · 占 %d%%", "Run %u · %d%%"),
                 (unsigned)r.run, (int)(100.0f * (float)r.run / (float)r.steps + 0.5f));
        textC(g_cx, g_cy + 166, b, F_S, C_RUN);
    } else {
        textC(g_cx, g_cy + 166, "无跑步记录", F_S, shade565(C_DIM, 160));
    }

    textC(g_cx, 414, "上下滑动 · 切换日期", F_S, C_DIM);
}

// ------------------------------------------------------------
// 设置 / 对时
// ------------------------------------------------------------
static int g_tY, g_tMo, g_tD, g_tH, g_tMi;

// 对时页状态
static int   g_timeMode = 0;      // 0 日期 / 1 时间
static int   g_timeTab  = 0;      // 时间模式：0 调时 / 1 调分
static float g_wPx[3]   = {0.0f, 0.0f, 0.0f};   // 日期轮各列像素偏移（正=内容下移=数值变大）
static int   g_wCol     = -1;                   // 正在拖的日期列
static int   g_wStartY  = 0;
static float g_wStartPx = 0.0f;
static bool  g_wMoved   = false;
static float g_listPx   = 0.0f;   // 设置列表滚动位置（像素）

#define SET_ROW_TIME  7           // 「对时」行，点按进入子页
#define SET_ROW_COLOR 8           // 「配色」行，点按进入色轮
#define SET_ROW_LANG  9           // 「语言」行，点按或左右按钮切换

// 入口型行（有右箭头）与数值行（有 − / +）的区分
static bool rowIsLink(int i) { return i == SET_ROW_TIME || i == SET_ROW_COLOR; }

#define LIST_VP0  96              // 设置列表可视区（圆屏，纵向要收紧）
#define LIST_VP1  366             // 96..366 正好容纳 5 行 54 px，第 5 行不被裁切
#define ROW_Y0    123             // 第 1 行中心
#define ROW_H     54
#define ROW_PAGE  5

#define ROW_NAME_X 52             // 名称左端
#define PILL_X0   208             // 胶囊左端
#define PILL_X1   432             // 胶囊右端
#define PILL_H    40
#define GLY_MINUS 236             // 减号中心
#define GLY_PLUS  404             // 加号中心
#define VAL_CX    320             // 数值中心

#define WH_CY     276             // 日期轮中心
#define WH_H      56              // 每个槽位高
#define WH_VP0    152
#define WH_VP1    400

// 表盘：与屏幕同心，数字贴着屏幕边缘
#define DIAL_RING 212             // 外环半径
#define DIAL_NR   172             // 12 个数字所在半径
#define DIAL_DOT  30              // 选中圆点半径
#define DIAL_DEAD 110             // 中心读数区，触摸不当作表盘

static const int WH_X[3] = {104, 233, 362};

static void timeLoad()
{
    if (!M5.Rtc.isEnabled() || !rtcValid()) {
        g_tY = 2026; g_tMo = 1; g_tD = 1; g_tH = 12; g_tMi = 0;
        return;
    }
    auto dt = M5.Rtc.getDateTime();
    g_tY  = dt.date.year;  g_tMo = dt.date.month; g_tD = dt.date.date;
    g_tH  = dt.time.hours; g_tMi = dt.time.minutes;
}

static int rowCount() { return 10; }

// 面板亮度内部是 40..255，界面按百分比显示与调节
static int brightPct()
{
    return (int)(((unsigned)g_cfg.bright * 100u + 127u) / 255u);
}

static const char *rowName(int i)
{
    static const char *setName[] = {"每日目标", "身高", "体重", "亮度",
                                    "提示", "息屏", "深睡", "对时", "配色", "语言"};
    return setName[i % 10];
}

static void rowValue(int i, char *b, size_t n)
{
    switch (i) {
        case 0: snprintf(b, n, T("%u 步", "%u steps"), (unsigned)g_cfg.goal); break;
        case 1: snprintf(b, n, "%u cm", g_cfg.heightCm); break;
        case 2: snprintf(b, n, "%u kg", g_cfg.weightKg); break;
        case 3: snprintf(b, n, "%u%%", (unsigned)brightPct()); break;
        case 4: snprintf(b, n, "%s", g_cfg.sound == 0 ? "静音" : (g_cfg.sound == 1 ? "震动" : "震动+声音")); break;
        case 5: snprintf(b, n, "%u s", g_cfg.screenOffS); break;
        case 6: snprintf(b, n, "%u s", g_cfg.sleepAfterS); break;
        case 9: snprintf(b, n, "%s", g_cfg.lang ? "English" : "简体中文"); break;
        default: snprintf(b, n, "%s", ""); break;
    }
}

static int daysInMonth(int y, int m)
{
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int v = d[(m - 1) % 12];
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) v = 29;
    return v;
}

// c: 0 年 / 1 月 / 2 日；j: 相对当前值的槽位偏移
static int wheelValue(int c, int j)
{
    if (c == 0) return g_tY + j;
    if (c == 1) {
        int v = (g_tMo - 1 + j) % 12;
        if (v < 0) v += 12;
        return v + 1;
    }
    const int dm = daysInMonth(g_tY, g_tMo);
    int v = (g_tD - 1 + j) % dm;
    if (v < 0) v += dm;
    return v + 1;
}

// 返回 true 表示值确实变了
static bool wheelStep(int c, int dir)
{
    if (c == 0) {
        int v = g_tY + dir;
        if (v < 2024 || v > 2099) return false;
        g_tY = v;
    } else if (c == 1) {
        g_tMo += dir;
        if (g_tMo < 1)  g_tMo = 12;
        if (g_tMo > 12) g_tMo = 1;
    } else {
        const int dm = daysInMonth(g_tY, g_tMo);
        g_tD += dir;
        if (g_tD < 1)  g_tD = dm;
        if (g_tD > dm) g_tD = 1;
    }
    const int dm = daysInMonth(g_tY, g_tMo);
    if (g_tD > dm) g_tD = dm;
    return true;
}

// 跟手滚动：把超出 ±一个槽位的位移换算成数值变化，剩下的留作像素偏移
// 到头了（比如 2099 年）就衰减成橡皮筋，不再继续滚
static float wheelCarry(int c, float px)
{
    int guard = 0;
    while (px >= (float)WH_H && guard++ < 40) {
        if (!wheelStep(c, +1)) { px = (float)WH_H * 0.34f; break; }
        px -= (float)WH_H;
        uiTick();
    }
    guard = 0;
    while (px <= -(float)WH_H && guard++ < 40) {
        if (!wheelStep(c, -1)) { px = -(float)WH_H * 0.34f; break; }
        px += (float)WH_H;
        uiTick();
    }
    return px;
}

static void rowAdjust(int i, int dir)
{
    switch (i) {
        case 0: {
            int32_t g = (int32_t)g_cfg.goal + dir * 500;
            if (g < 1000)  g = 1000;
            if (g > 40000) g = 40000;
            g_cfg.goal = (uint32_t)g;
        } break;
        case 1: {
            int v = (int)g_cfg.heightCm + dir;
            if (v < 120) v = 120;
            if (v > 220) v = 220;
            g_cfg.heightCm = (uint16_t)v;
        } break;
        case 2: {
            int v = (int)g_cfg.weightKg + dir;
            if (v < 30) v = 30;
            if (v > 180) v = 180;
            g_cfg.weightKg = (uint16_t)v;
        } break;
        case 3: {
            int pct = brightPct() + dir * 5;       // 界面按 5% 一档
            if (pct < 16)  pct = 16;
            if (pct > 100) pct = 100;
            const int v = (pct * 255 + 50) / 100;
            g_cfg.bright = (uint8_t)v;
            M5.Display.setBrightness(g_cfg.bright);
            g_bright = g_cfg.bright;
        } break;
        case 4: {
            int v = (int)g_cfg.sound + dir;
            if (v < 0) v = 2;
            if (v > 2) v = 0;
            g_cfg.sound = (uint8_t)v;
            if (g_cfg.sound >= 2 && !M5.Speaker.isEnabled()) M5.Speaker.begin();
            uiBeep(1046, 40);       // 切到「震动+声音」时马上能听到，便于确认喇叭可用
        } break;
        case 5: {
            int v = (int)g_cfg.screenOffS + dir * 5;
            if (v < 5) v = 5;
            if (v > 120) v = 120;
            g_cfg.screenOffS = (uint8_t)v;
        } break;
        case 6: {
            int v = (int)g_cfg.sleepAfterS + dir * 15;
            if (v < 15) v = 15;
            if (v > 240) v = 240;
            g_cfg.sleepAfterS = (uint8_t)v;
        } break;
        case 9:
            g_cfg.lang = g_cfg.lang ? 0 : 1;   // 两种语言直接切换
            g_forceRender = true;              // 整屏文案都变了
            break;
        default: break;
    }
    cfgTouch();             // 改完标记待落盘（静置 700 ms 后写 NVS）
}

// 右/左箭头
static void chevronAt(int x, int y, int dir, uint16_t col)
{
    for (int t = -1; t <= 1; ++t) {
        ui->drawLine(x - 6 * dir + t, y - 11, x + 5 * dir + t, y, col);
        ui->drawLine(x + 5 * dir + t, y, x - 6 * dir + t, y + 11, col);
    }
}

// 有宽度的线段，边缘抗锯齿
static void thickLine(int x0, int y0, int x1, int y1, float w, uint16_t col)
{
    const float fx = (float)(x1 - x0), fy = (float)(y1 - y0);
    const float len2 = fx * fx + fy * fy;
    if (len2 < 0.01f) return;

    const float hw = w * 0.5f;
    const int pad = (int)w + 2;
    const int xs = (x0 < x1 ? x0 : x1) - pad;
    const int xe = (x0 > x1 ? x0 : x1) + pad;
    const int ys = (y0 < y1 ? y0 : y1) - pad;
    const int ye = (y0 > y1 ? y0 : y1) + pad;

    for (int y = ys; y <= ye; ++y) {
        if (y < g_bandY0 || y >= g_bandY1) continue;
        for (int x = xs; x <= xe; ++x) {
            if (x < 0 || x >= g_w) continue;
            const float px = (float)x + 0.5f - (float)x0;
            const float py = (float)y + 0.5f - (float)y0;
            float t = (px * fx + py * fy) / len2;
            if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
            const float dx = px - t * fx, dy = py - t * fy;
            const float d  = sqrtf(dx * dx + dy * dy);
            float cov = 0.5f + hw - d;
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            ui->writePixel(x, y, shade565(col, (uint32_t)(cov * 255.0f + 0.5f)));
        }
    }
}

// 行：左侧名称，右侧一枚圆角胶囊，胶囊内是「− 数值 +」
// 胶囊宽度固定，减号与加号贴两端，数值居中；数值过长时自动降一级字模
static void drawPill(int y, uint16_t col)
{
    fillRoundRectAA((float)PILL_X0, (float)(y - PILL_H / 2),
                    (float)(PILL_X1 - PILL_X0), (float)PILL_H,
                    (float)(PILL_H / 2), col);
}

static void glyphMinus(int x, int y, uint16_t col)
{
    ui->fillRect(x - 10, y - 2, 21, 5, col);
}

static void glyphPlus(int x, int y, uint16_t col)
{
    ui->fillRect(x - 10, y - 2, 21, 5, col);
    ui->fillRect(x - 2, y - 10, 5, 21, col);
}

static void drawPillValue(int x, int y, const char *val, uint16_t col)
{
    const int room = GLY_PLUS - GLY_MINUS - 34;
    const PedFont *f = &F_H1;
    if (pedTextWidth(val, *f) > room) f = &F_L;
    if (pedTextWidth(val, *f) > room) f = &F_S;
    textC(x, y, val, *f, col);
}

static void drawRowAt(int y, const char *name, const char *val)
{
    textL(ROW_NAME_X, y, name, F_L, C_TEXT);
    drawPill(y, C_BTN);
    glyphMinus(GLY_MINUS, y, C_DIM);
    drawPillValue(VAL_CX, y, val, C_TEXT);
    glyphPlus(GLY_PLUS, y, C_TEXT);
}

static void drawRow(int i, int y)
{
    char v[24];
    rowValue(i, v, sizeof(v));
    drawRowAt(y, rowName(i), v);
}

// 「对时」行：胶囊内显示当前 RTC 时间，右端是进入箭头
static void drawRowLink(int y)
{
    char v[24];
    if (M5.Rtc.isEnabled() && rtcValid()) {
        auto dt = M5.Rtc.getDateTime();
        snprintf(v, sizeof(v), "%02d:%02d", dt.time.hours, dt.time.minutes);
    } else {
        snprintf(v, sizeof(v), "--:--");
    }
    textL(ROW_NAME_X, y, rowName(SET_ROW_TIME), F_L, C_TEXT);
    drawPill(y, C_BTN);
    drawPillValue(VAL_CX, y, v, C_ACCENT);
    chevronAt(GLY_PLUS, y, 1, C_DIM);
}

// 「配色」行：胶囊内是主题色与强调色两个色点，右端是进入箭头
static void drawRowColor(int y)
{
    textL(ROW_NAME_X, y, rowName(SET_ROW_COLOR), F_L, C_TEXT);
    drawPill(y, C_BTN);
    fillCircleAA(288.0f, (float)y, 13.0f, themeColor());
    fillCircleAA(332.0f, (float)y, 13.0f, runColor());
    chevronAt(GLY_PLUS, y, 1, C_DIM);
}

// 「语言」行：只有当前语言名，胶囊内点按即切换
static void drawRowLang(int y)
{
    textL(ROW_NAME_X, y, rowName(SET_ROW_LANG), F_L, C_TEXT);
    drawPill(y, C_BTN);
    drawPillValue(VAL_CX, y, g_cfg.lang ? "English" : "简体中文", C_ACCENT);
}

static void clearToday()
{
    g_steps     = 0;
    g_runSteps  = 0;
    g_activeSec = 0;
    bucketClear();
    g_hwSteps   = 0;
    g_nextMilestone = 1000;
    g_rtc.steps     = 0;
    g_rtc.run       = 0;
    g_rtc.activeSec = 0;
    g_rtc.hwInit    = 0;        // 下次读计数时重建基线，否则会立刻把旧步数加回来
    g_rtc.hwBase    = 0;
    g_rtc.hwRaw     = 0;
    saveToday(true);
    histSyncToday();
    histSave();
    g_forceRender = true;
}

static void drawSet()
{
    char v[24];

    // 列表按像素滚动，先画整片再把超出可视区的部分抹掉（等效裁剪）
    const int i0 = (int)floorf(g_listPx / (float)ROW_H) - 1;
    for (int i = i0; i <= i0 + ROW_PAGE + 2; ++i) {
        if (i < 0 || i >= rowCount()) continue;
        const int y = ROW_Y0 + (int)lroundf((float)(i * ROW_H) - g_listPx);
        if (i == SET_ROW_TIME)       drawRowLink(y);
        else if (i == SET_ROW_COLOR) drawRowColor(y);
        else if (i == SET_ROW_LANG)  drawRowLang(y);
        else                         drawRow(i, y);
    }
    // 视口外的部分只清与条带的交集：条带渲染时带外清了也不会被推出去，纯浪费
    bgRect(0, LIST_VP0);
    bgRect(LIST_VP1, g_h);

    textC(g_cx, 66, "设置", F_H1, C_TEXT);

    const int pages = rowCount() - ROW_PAGE + 1;
    int cur = (int)lroundf(g_listPx / (float)ROW_H) + 1;
    if (cur < 1) cur = 1;
    if (cur > pages) cur = pages;
    snprintf(v, sizeof(v), "%d/%d", cur, pages);
    textR(384, 66, v, F_SM, C_DIM);

    textC(g_cx, 404, "上下拖动 · 左右切页", F_S, C_DIM);

    // 横向滑动的切页进度：从中心向两侧张开，滑够 40 px 就切页
    if (g_slideHint > 0.02f) {
        const int w = (int)(g_slideHint * 70.0f);
        if (w >= 2) ui->fillRoundRect(g_cx - w, 384, w * 2, 5, 2, C_ACCENT);
    }
}

// ------------------------------------------------------------
// 对时
// ------------------------------------------------------------
// 三个上下翻页的日期轮
static void drawDateWheel()
{
    char v[8];

    // 选中行的底衬
    fillRoundRectAA(18, WH_CY - 27, g_w - 36, 54, 27, rgb(17, 19, 23));

    for (int c = 0; c < 3; ++c) {
        const float off = g_wPx[c];                 // 跟手：直接用当前像素偏移
        for (int j = -3; j <= 3; ++j) {
            const int val = wheelValue(c, j);
            if (c == 0 && (val < 2024 || val > 2099)) continue;

            const float dist = fabsf((float)j + off / (float)WH_H);
            if (dist > 1.86f) continue;

            const int y = WH_CY + (int)lroundf((float)j * WH_H + off);

            uint16_t col;
            if (dist < 0.35f) {
                col = C_ACCENT;
            } else if (dist < 0.90f) {
                col = lerp565(C_ACCENT, C_DIM, (dist - 0.35f) / 0.55f);
            } else {
                float a = 210.0f - 120.0f * (dist - 0.90f);
                if (a < 55.0f) a = 55.0f;
                col = shade565(C_DIM, (uint32_t)a);
            }

            snprintf(v, sizeof(v), "%d", val);
            textC(WH_X[c], y, v, F_MID, col);
        }
    }
}

// 表盘：铺满屏幕，数字贴边，读数在正中
// 24 小时制：边缘刻度随上午/下午自动切换成 0-11 或 12-23
static void drawDial()
{
    char v[16];
    const bool hourMode = (g_timeTab == 0);
    const float R = (float)DIAL_NR;

    // 外环
    drawRingAA((float)DIAL_RING, (float)DIAL_RING - 2.0f, -90.0f, 360.0f,
               rgb(44, 48, 55), rgb(44, 48, 55));

    const int   base   = (g_tH >= 12) ? 12 : 0;      // 上午 0-11 / 下午 12-23
    const int   slot   = hourMode ? (g_tH - base) : 0;
    const float selDeg = hourMode ? (float)(slot * 30) : (float)(g_tMi * 6);
    const float selRad = selDeg * DEG_TO_RAD;
    const int   sx = g_cx + (int)lroundf(sinf(selRad) * R);
    const int   sy = g_cy - (int)lroundf(cosf(selRad) * R);

    // 12 个刻度
    for (int i = 0; i < 12; ++i) {
        const float a  = (float)i * 30.0f * DEG_TO_RAD;
        const int   px = g_cx + (int)lroundf(sinf(a) * R);
        const int   py = g_cy - (int)lroundf(cosf(a) * R);

        if (hourMode) {
            if (i == slot) continue;               // 选中值稍后画在圆点里
            snprintf(v, sizeof(v), "%d", base + i);
        } else {
            float da = fabsf((float)i * 30.0f - selDeg);
            if (da > 180.0f) da = 360.0f - da;
            if (da < 12.0f) continue;              // 被圆点盖住的不画
            snprintf(v, sizeof(v), "%02d", i * 5);
        }
        textC(px, py, v, F_MID, C_DIM);
    }

    // 选中圆点（里面就是 24 小时制的当前值）
    fillCircleAA((float)sx, (float)sy, (float)DIAL_DOT + 3.0f, C_BG);   // 吃掉底下的刻度
    fillCircleAA((float)sx, (float)sy, (float)DIAL_DOT, C_ACCENT);
    {
        char w[4];
        snprintf(w, sizeof(w), "%d", hourMode ? g_tH : g_tMi);
        textC(sx, sy, w, F_MID, C_BG);
    }

    // 中心：单位 + 大号读数 + 另一半
    textC(g_cx, g_cy - 74, hourMode ? "时" : "分", F_S, C_DIM);

    if (hourMode) snprintf(v, sizeof(v), "%02d", g_tH);
    else          snprintf(v, sizeof(v), "%02d", g_tMi);
    textC(g_cx, g_cy - 2, v, F_BIG, C_TEXT);

    if (hourMode) snprintf(v, sizeof(v), T("分 %02d", "Min %02d"), g_tMi);
    else          snprintf(v, sizeof(v), T("时 %02d", "Hour %02d"), g_tH);
    textC(g_cx, g_cy + 76, v, F_H1, C_DIM);
    textC(g_cx, g_cy + 110, "点按确认", F_S, shade565(C_DIM, 120));
}

static void drawTime()
{
    if (g_timeMode == 0) {
        drawDateWheel();
        // 滚轮溢出部分裁掉，再画周边（同上：只清条带与该区域的交集）
        bgRect(0, WH_VP0);
        bgRect(WH_VP1, g_h);

        static const char *u[3] = {"年", "月", "日"};
        for (int i = 0; i < 3; ++i) textC(WH_X[i], 140, u[i], F_S, C_DIM);

        textC(g_cx, 54, "对时", F_H1, C_TEXT);
        textC(g_cx, 92, "点按确认", F_S, shade565(C_DIM, 150));
        textC(g_cx, 406, "左滑下一步 · 右滑返回", F_S, C_DIM);
    } else {
        drawDial();
    }

    // 圆屏：四角不在可视区，全部操作走手势
}

// ------------------------------------------------------------
// 配色（色轮）
// ------------------------------------------------------------
#define CW_R    160             // 色环外径
#define CW_W    24              // 环宽
#define CW_RIN  (CW_R - CW_W)

static int   g_colTarget = 0;      // 0 主题色 / 1 强调色
static float g_colHue    = 0.0f;   // 当前色相，保留连续值以便跟手
static float g_colPrev   = 0.0f;   // 上一帧角度，用于求增量
static int   g_colVal    = 92;     // 当前亮度 30..100（%）
static bool  g_valDrag   = false;  // 中心圆拖动中
static int   g_valY0     = 0;      // 拖动起点
static int   g_valV0     = 92;     // 拖动起始亮度

static float colHueWrapped()
{
    float h = fmodf(g_colHue, 360.0f);
    if (h < 0.0f) h += 360.0f;
    return h;
}

static void colorApply()
{
    const uint16_t h = (uint16_t)lroundf(colHueWrapped()) % 360;
    if (g_colTarget == 0) { g_cfg.themeHue = h; g_cfg.themeVal = (uint8_t)g_colVal; }
    else                  { g_cfg.runHue   = h; g_cfg.runVal   = (uint8_t)g_colVal; }
    applyTheme();
    cfgTouch();
}

// 一行内的一段环带：逐像素取色，同色相邻像素合并成一条横线
static void cwRow(int y, int xa, int xb, int dy, const uint16_t *lut)
{
    if (xa > xb) return;
    int      runX = xa;
    uint16_t runC = lut[cwHueOf(xa - g_cx, dy)];
    for (int x = xa + 1; x <= xb; ++x) {
        const uint16_t c = lut[cwHueOf(x - g_cx, dy)];
        if (c == runC) continue;
        ui->writeFastHLine(runX, y, x - runX, runC);
        runX = x;
        runC = c;
    }
    ui->writeFastHLine(runX, y, xb + 1 - runX, runC);
}

// 细弧：在环带内按角度取像素，用于亮度指示（a1 = 360 时画满圈）
static void ringArcFill(int r, int w, int a0, int a1, uint16_t col)
{
    if (a1 <= a0) return;
    const int ro  = r + (w >> 1), ri = r - (w >> 1);
    const int ro2 = ro * ro,      ri2 = ri * ri;
    ui->startWrite();
    for (int dy = -ro; dy <= ro; ++dy) {
        const int y = g_cy + dy;
        if (y < 0 || y >= g_h) continue;
        const int  xo   = (int)sqrtf((float)(ro2 - dy * dy));
        const bool hole = (dy > -ri && dy < ri);
        const int  xi   = hole ? (int)sqrtf((float)(ri2 - dy * dy)) : 0;
        const int  segs = hole ? 2 : 1;
        for (int s = 0; s < segs; ++s) {
            const int xa = (s == 0) ? g_cx - xo : g_cx + xi;
            const int xb = (s == 0 && hole) ? g_cx - xi : g_cx + xo;
            for (int x = xa; x <= xb; ++x) {
                const int h = cwHueOf(x - g_cx, dy);
                if (h >= a0 && h < a1) ui->writePixel(x, y, col);
            }
        }
    }
    ui->endWrite();
}

static void drawColorWheel()
{
    char v[16];

    // 色相表：360 档，整环只查表
    static uint16_t lut[360];
    static bool     lutReady = false;
    if (!lutReady) {
        for (int i = 0; i < 360; ++i) lut[i] = hsv565((uint16_t)i, 1.00f, 1.00f);
        lutReady = true;
    }

    // 色相环：按行取环带区间，左右两段逐像素写入。
    // 不用扇形三角形拼贴，避免细窄三角形栅格化时漏像素。
    const int ro2 = CW_R * CW_R, ri2 = CW_RIN * CW_RIN;
    ui->startWrite();
    for (int dy = -CW_R; dy <= CW_R; ++dy) {
        const int y = g_cy + dy;
        if (y < 0 || y >= g_h) continue;
        const int xo = (int)sqrtf((float)(ro2 - dy * dy));
        if (dy > -CW_RIN && dy < CW_RIN) {
            const int xi = (int)sqrtf((float)(ri2 - dy * dy));
            cwRow(y, g_cx - xo, g_cx - xi, dy, lut);
            cwRow(y, g_cx + xi, g_cx + xo, dy, lut);
        } else {
            cwRow(y, g_cx - xo, g_cx + xo, dy, lut);
        }
    }
    ui->endWrite();

    // 指示点
    const float ro  = (float)CW_R, ri = (float)CW_RIN;
    const float hw  = colHueWrapped();
    const float ah  = hw * DEG_TO_RAD;
    const float rm  = (ro + ri) * 0.5f;
    const int   hx  = g_cx + (int)lroundf(sinf(ah) * rm);
    const int   hy  = g_cy - (int)lroundf(cosf(ah) * rm);
    const float vv  = (float)g_colVal / 100.0f;
    fillCircleAA((float)hx, (float)hy, 15.0f, C_BG);
    fillCircleAA((float)hx, (float)hy, 13.0f, C_TEXT);
    fillCircleAA((float)hx, (float)hy, 8.0f, hsv565((uint16_t)lroundf(hw), 1.00f, vv));

    // 中心预览：颜色随亮度变化，圆内显示亮度
    const uint16_t csel = (g_colTarget == 0) ? themeColor() : runColor();
    fillCircleAA((float)g_cx, (float)g_cy, 52.0f, csel);

    // 亮度弧：满圈对应 30..100
    ringArcFill(64, 5, 0, 360, C_BTN);
    ringArcFill(64, 5, 0, (g_colVal - 30) * 360 / 70, C_TEXT);

    snprintf(v, sizeof(v), "%d%%", g_colVal);
    textC(g_cx, g_cy, v, F_S, (g_colVal >= 62) ? C_BG : C_TEXT);

    textC(g_cx, g_cy - 84, (g_colTarget == 0) ? "主题色" : "强调色", F_S, C_DIM);
    snprintf(v, sizeof(v), T("色相 %d° · 亮度 %d%%", "Hue %d° · Bright %d%%"),
             (int)lroundf(hw), g_colVal);
    textC(g_cx, g_cy + 84, v, F_S, C_DIM);

    textC(g_cx, 414, "环上选色相 · 圆内调亮度", F_S, C_DIM);
}

// ------------------------------------------------------------
// 调试
// ------------------------------------------------------------
static void drawDebugHw()
{
    char v[64];
    textC(g_cx, 56, "硬件计步", F_H1, C_TEXT);

    snprintf(v, sizeof(v), "%u", (unsigned)pedimu::lastCount);
    textC(g_cx, 124, v, F_NUM, C_TEXT);

    // 步态取自 page0 字节 4（0 静止 / 1 步行 / 2 跑步 / 3 未知）
    static const char *actZh[] = {"静止", "步行", "跑步", "未知"};
    static const char *actEn[] = {"still", "walk", "run", "unknown"};
    const uint8_t ac = g_actCode & 3;
    snprintf(v, sizeof(v), T("步态 %s · 本次硬件 %u 步", "Gait %s · hw %u steps"),
             g_cfg.lang ? actEn[ac] : actZh[ac], (unsigned)g_hwSteps);
    textC(g_cx, 186, v, F_S, C_DIM);

    // 使能字节随配置包变：legacy(手机) 在 page4 字节 1，base(腕戴) 在 page6 字节 3
    const bool phone = (pedimu::mode == pedimu::MODE_PHONE);
    const uint8_t enByte = phone ? pedimu::PHONE_EN_BYTE : pedimu::STEP_EN_BYTE;

    snprintf(v, sizeof(v), "IMU %02X  INIT %02X  EN %02X  V%X",
             pedimu::dbgChip, pedimu::dbgStat, pedimu::dbgPage6[enByte], pedimu::dbgVerify);
    textC(g_cx, 224, v, F_SM,
          (pedimu::dbgVerify & 1) ? C_DIM : C_WARN);

    if (phone) {
        snprintf(v, sizeof(v), T("配置 LEGACY（手机）", "Config LEGACY (phone)"));
        textC(g_cx, 254, v, F_SM, C_DIM);
    } else {
        snprintf(v, sizeof(v), T("参数 %04X", "Param %04X"), pedimu::dbgParam);
        textC(g_cx, 254, v, F_SM,
              (pedimu::dbgVerify & 2) ? C_DIM : C_WARN);
    }

    snprintf(v, sizeof(v), "%s %02X %02X %02X %02X %02X %02X %02X %02X",
             phone ? "p4" : "p6",
             pedimu::dbgPage6[0], pedimu::dbgPage6[1], pedimu::dbgPage6[2],
             pedimu::dbgPage6[3], pedimu::dbgPage6[4], pedimu::dbgPage6[5],
             pedimu::dbgPage6[6], pedimu::dbgPage6[7]);
    textC(g_cx, 288, v, F_SM, C_DIM);

    snprintf(v, sizeof(v), "p0 %02X %02X %02X %02X %02X %02X %02X %02X",
             pedimu::dbgPage0[0], pedimu::dbgPage0[1], pedimu::dbgPage0[2],
             pedimu::dbgPage0[3], pedimu::dbgPage0[4], pedimu::dbgPage0[5],
             pedimu::dbgPage0[6], pedimu::dbgPage0[7]);
    textC(g_cx, 318, v, F_SM, C_DIM);

    snprintf(v, sizeof(v), "ACC %02X  CTRL %02X  PCONF %02X",
             pedimu::dbgAccConf, pedimu::dbgPwrCtrl, pedimu::dbgPwrConf);
    textC(g_cx, 352, v, F_SM, C_DIM);

    snprintf(v, sizeof(v), T("唤醒 %u · 电池 %u mV", "Wakes %u · %u mV"),
                 (unsigned)g_rtc.wakes, (unsigned)g_vbat);
    textC(g_cx, 384, v, F_S, C_DIM);

    textC(g_cx, 428, "数值全部来自芯片回读", F_S, C_DIM);
}

// ------------------------------------------------------------
// 把上一帧搬到 prev 画布（翻页前调用）
static void uiSnapshot()
{
    if (!g_canvasReady || !g_canvasPrevReady) return;
    memcpy(g_canvasPrev.getBuffer(), g_canvas.getBuffer(),
           (size_t)g_w * (size_t)g_h * 2);
}

// 翻页过渡：旧页滑出、新页滑入，dir = +1 向右翻、-1 向左翻
static void uiSlide(int dir)
{
    if (!g_canvasPrevReady) return;
    static const int N = 7;
    for (int i = 1; i <= N; ++i) {
        const int d = g_w * i / N;
        if (dir > 0) {
            g_canvasPrev.pushSprite(-d, 0);
            g_canvas.pushSprite(g_w - d, 0);
        } else {
            g_canvasPrev.pushSprite(d, 0);
            g_canvas.pushSprite(d - g_w, 0);
        }
        vibService();
    }
}

static void drawPage()
{
    switch (g_page) {
        case PG_MAIN:  if (g_mainDetail) drawStats(); else drawMain(); break;
        case PG_HIST:  if (g_histIdx < 0) drawHist(); else drawHistDay(g_histIdx); break;
        case PG_DOOR:  drawDoor();  break;
        case PG_SET:   drawSet();   break;
        case PG_TIME:  drawTime();  break;
        case PG_COLOR: drawColorWheel(); break;
        default:       drawDebugHw(); break;
    }
}

static void uiRender(bool push = true)
{
    ui->fillScreen(C_BG);
    drawPage();
    if (push) ui->pushSprite(0, 0);
    vibService();                      // 画完一帧顺手把到点的震动关掉
}

// 把画布的一条「整宽条带」推到屏幕。
// pushSprite 没有源矩形重载，推不了一部分；但整宽条带在画布缓冲里是连续
// 内存（pitch == g_w），所以可以直接当紧凑位图交给 writePixels。
static void uiPushBand(int y0, int h)
{
    if (h <= 0) return;
    const uint16_t *src = (const uint16_t *)g_canvas.getBuffer()
                        + (size_t)y0 * (size_t)g_w;
    M5.Display.startWrite();
    M5.Display.setAddrWindow(0, y0, g_w, h);
    M5.Display.writePixels(src, (uint32_t)g_w * (uint32_t)h);
    M5.Display.endWrite();
}

// 只重绘并只推送 [y0, y1) 这一条 —— 对应官方 UserDemo 的
// LV_DISPLAY_RENDER_MODE_PARTIAL + 120 行缓冲。
// 带外的图元在 drawRingAA / fillCircleAA / fillRoundRectAA / pedDrawText
// 的循环里被 g_bandY0/g_bandY1 直接跳过，不进 writePixel（PSRAM 上是 RMW）。
static void uiRenderBand(int y0, int y1)
{
    if (y0 < 0) y0 = 0;
    if (y1 > g_h) y1 = g_h;
    const int h = y1 - y0;
    if (h <= 0) return;
    if (h >= g_h) { uiRender(true); return; }

    const int saveY0 = g_bandY0, saveY1 = g_bandY1;
    g_bandY0 = y0;
    g_bandY1 = y1;

    ui->fillRect(0, y0, g_w, h, C_BG);   // 只清这一条，不用 fillScreen
    drawPage();
    uiPushBand(y0, h);

    g_bandY0 = saveY0;
    g_bandY1 = saveY1;
    vibService();
}

// 触摸/滚轮只改动局部时，标出「本次需要重绘的垂直范围」（并集语义）。
// 没标记 = 0，调用方按整屏走；这是保守路线 —— 拿不准的一律全屏。
int  g_dirtyY0 = 0, g_dirtyY1 = 0;
static void uiMarkBand(int y0, int y1)
{
    if (y0 >= y1) return;
    if (g_dirtyY0 == g_dirtyY1) { g_dirtyY0 = y0; g_dirtyY1 = y1; return; }
    if (y0 < g_dirtyY0) g_dirtyY0 = y0;
    if (y1 > g_dirtyY1) g_dirtyY1 = y1;
}
static void uiClearBand() { g_dirtyY0 = g_dirtyY1 = 0; }

// 日期轮松手：不足半格弹回，超过半格补一格，然后缓出归零
static void wheelSnap(int c)
{
    float px = g_wPx[c];

    if (px >= (float)WH_H * 0.5f) {
        if (wheelStep(c, +1)) px -= (float)WH_H; else px = (float)WH_H * 0.5f;
    } else if (px <= -(float)WH_H * 0.5f) {
        if (wheelStep(c, -1)) px += (float)WH_H; else px = -(float)WH_H * 0.5f;
    }
    g_wPx[c] = px;

    if (fabsf(px) < 0.5f) { g_wPx[c] = 0.0f; uiSettle(); return; }

    const float start = px;
    static const float k[4] = {0.42f, 0.14f, 0.04f, 0.0f};
    for (int i = 0; i < 4; ++i) {
        g_wPx[c] = start * k[i];
        uiRenderBand(WH_VP0, WH_VP1);   // 日期轮补间只动轮子区域
    }
    g_wPx[c] = 0.0f;
    uiSettle();                     // 落位的一瞬间给一下
}

// 设置列表：松手后吸附到整行
static void listSnap()
{
    const int maxTop = rowCount() - ROW_PAGE;
    float target = roundf(g_listPx / (float)ROW_H) * (float)ROW_H;
    if (target < 0.0f) target = 0.0f;
    if (target > (float)(maxTop * ROW_H)) target = (float)(maxTop * ROW_H);

    const float start = g_listPx;
    g_setScroll = (int)lroundf(target / (float)ROW_H);

    // 已经对齐就别再补 4 帧动画 —— 那 4 帧会把点按拖出 60~100 ms 的延迟
    if (fabsf(target - start) < 0.5f) { g_listPx = target; return; }

    static const float k[4] = {0.55f, 0.84f, 0.96f, 1.0f};
    for (int i = 0; i < 4; ++i) {
        g_listPx = start + (target - start) * k[i];
        uiRenderBand(LIST_VP0, LIST_VP1);   // 列表吸附只动可视区
    }
    g_listPx = target;
    uiSettle();
    // 分页指示「N/M」画在 y=66，在列表视口外 —— 条带补间带不到它。
    // 吸附后行号必然变化，单独补一条窄带刷新。
    uiRenderBand(44, 88);
}

// ------------------------------------------------------------
// 触摸手势
// ------------------------------------------------------------
static bool     g_touchActive = false;
static bool     g_dialActive  = false;
static bool     g_listDrag    = false;
static int      g_listStartY  = 0;
static float    g_listStartPx = 0.0f;
static int      g_pressRow    = -1;      // 按下瞬间锁定的设置行（避免吸附后错位）
static int      g_pressBtn    = 0;       // 按下的按钮：-1 减 / +1 加 / 0 不是按钮
static uint32_t g_btnNextMs   = 0;       // 按住连发的下次触发时刻
static int      g_listStartX  = 0;
static int      g_dirLock     = 0;       // 0=未定 1=横向切页 2=纵向滚动
static bool     g_colorDrag   = false;   // 色轮拖动中
static int      g_tsX = 0, g_tsY = 0;
static uint32_t g_tsMs = 0;

static void pageNext()
{
    if (g_page == PG_SET) cfgFlush();                     // 离开设置页即落盘
    uiPage();
    // 对时 / 配色退回设置列表，设置列表退回入口页
    if (g_page == PG_TIME || g_page == PG_COLOR) {
        g_page = PG_SET;
    } else if (g_page == PG_SET) {
        g_page = PG_DOOR;
    } else {
        g_page = (g_page + 1 > PG_CYCLE_MAX) ? PG_MAIN : g_page + 1;
    }
    g_setScroll = 0; g_listPx = 0.0f;
    g_mainDetail = false; g_histIdx = -1;
}

static void pagePrev()
{
    if (g_page == PG_SET) cfgFlush();
    uiPage();
    if (g_page == PG_TIME || g_page == PG_COLOR) {
        g_page = PG_SET;
    } else if (g_page == PG_SET) {
        g_page = PG_DOOR;
    } else {
        g_page = (g_page == PG_MAIN) ? PG_CYCLE_MAX : g_page - 1;
    }
    g_setScroll = 0; g_listPx = 0.0f;
    g_mainDetail = false; g_histIdx = -1;
}

// 历史上下翻页：柱状图 → 今天 → 昨天 → …… → 六天前 → 柱状图
static void histStep(int dir)
{
    int n = g_histIdx + dir;
    if (n < -1)              n = HIST_DAYS - 1;
    if (n >= HIST_DAYS)      n = -1;
    g_histIdx = n;
    uiTick();
}

static void enterTimePage()
{
    cfgFlush();
    timeLoad();
    g_timeMode = 0;
    g_timeTab  = 0;
    g_wPx[0] = g_wPx[1] = g_wPx[2] = 0.0f;
    g_wCol     = -1;
    g_wMoved   = false;
    g_page     = PG_TIME;
}

static void enterColorPage()
{
    cfgFlush();
    g_colTarget = 0;
    g_colHue    = (float)g_cfg.themeHue;
    g_colVal    = (int)g_cfg.themeVal;
    g_page      = PG_COLOR;
}

// 左滑：主题色 → 强调色 → 返回设置
static void colorNext()
{
    if (g_colTarget == 0) {
        g_colTarget = 1;
        g_colHue    = (float)g_cfg.runHue;
        g_colVal    = (int)g_cfg.runVal;
        uiTick();
        return;
    }
    cfgFlush();
    g_page = PG_SET;
}

// 右滑：强调色 → 主题色 → 返回设置
static void colorPrev()
{
    if (g_colTarget == 1) {
        g_colTarget = 0;
        g_colHue    = (float)g_cfg.themeHue;
        g_colVal    = (int)g_cfg.themeVal;
        uiTick();
        return;
    }
    cfgFlush();
    g_page = PG_SET;
}

// 表盘拖动：把手指角度累积成一个连续量，转满一圈就自动跨到上午 / 下午
static float g_dialAcc  = 0.0f;     // 连续角度（度）
static float g_dialPrev = 0.0f;     // 上一帧角度，用于求增量

static float dialAngle(int x, int y)
{
    const float dx = (float)x - (float)g_cx;
    const float dy = (float)y - (float)g_cy;
    float a = atan2f(dx, -dy) / DEG_TO_RAD;    // 0 = 12 点，顺时针为正
    if (a < 0.0f) a += 360.0f;
    return a;
}

static float dialPeriod() { return (g_timeTab == 0) ? 720.0f : 360.0f; }

static void dialNorm()
{
    const float p = dialPeriod();
    while (g_dialAcc < 0.0f)   g_dialAcc += p;
    while (g_dialAcc >= p)     g_dialAcc -= p;
}

static bool dialApply()
{
    bool changed = false;
    if (g_timeTab == 0) {
        int h = (int)lroundf(g_dialAcc / 30.0f) % 24;
        if (h < 0) h += 24;
        if (h != g_tH) { g_tH = h; changed = true; }
    } else {
        int m = (int)lroundf(g_dialAcc / 6.0f) % 60;
        if (m < 0) m += 60;
        if (m != g_tMi) { g_tMi = m; changed = true; }
    }
    if (changed) uiTick();
    return changed;
}

static bool dialPress(int x, int y)
{
    const float a = dialAngle(x, y);
    if (g_timeTab == 0) {
        const int base = (g_tH >= 12) ? 12 : 0;
        g_dialAcc = (float)((base + (int)lroundf(a / 30.0f) % 12) * 30);
    } else {
        g_dialAcc = (float)((int)lroundf(a / 6.0f) * 6);
    }
    dialNorm();
    g_dialPrev = a;
    return dialApply();
}

static bool dialMove(int x, int y)
{
    const float a = dialAngle(x, y);
    float d = a - g_dialPrev;
    if (d > 180.0f)  d -= 360.0f;
    if (d < -180.0f) d += 360.0f;
    g_dialPrev = a;
    g_dialAcc += d;
    dialNorm();
    return dialApply();
}

// 对时页层级：设置 → 日期 → 调时 → 调分
// 左滑进下一层，右滑退回上一层
static void timeBack()          // 右滑：返回上一层
{
    if (g_timeMode == 1) {
        if (g_timeTab == 1) { g_timeTab = 0; uiTick(); return; }
        g_timeMode = 0;
        uiTick();
        return;
    }
    g_page = PG_SET;
}

static void timeNext()          // 左滑：进入下一层
{
    if (g_timeMode == 0)      { g_timeMode = 1; g_timeTab = 0; uiTick(); }
    else if (g_timeTab == 0)  { g_timeTab = 1; uiTick(); }
}

static void timeConfirm()       // 点按：写入 RTC 并返回
{
    if (k_valid(g_tY, g_tMo, g_tD)) {
        rtcApply(g_tY, g_tMo, g_tD, g_tH, g_tMi);   // 秒归零
        uiConfirm();
    }
    g_page = PG_SET;
}

// 返回 true 表示需要重绘
static bool uiTap(int x, int y)
{
    // ---------------- 对时页 ----------------
    // 圆屏四角不可见，不放按钮：左滑上一层、右滑下一层、点按确认
    if (g_page == PG_TIME) {
        if (g_timeMode == 1) {
            const float dx = (float)x - (float)g_cx;
            const float dy = (float)y - (float)g_cy;
            // 表盘环上按下时已经选过值了，不再当确认
            if (dx * dx + dy * dy >= (float)(DIAL_DEAD * DIAL_DEAD)) return false;
        }
        timeConfirm();
        return true;
    }

    // ---------------- 配色页 ----------------
    // 中心区点按确认；环上按下已经选过色，不当确认
    if (g_page == PG_COLOR) {
        const float dx = (float)x - (float)g_cx;
        const float dy = (float)y - (float)g_cy;
        if (dx * dx + dy * dy >= (float)(CW_RIN * CW_RIN)) return false;
        cfgFlush();
        g_page = PG_SET;
        uiConfirm();
        return true;
    }

    // ---------------- 设置入口 ----------------
    if (g_page == PG_DOOR) {
        cfgFlush();
        g_listPx = 0.0f;
        g_page   = PG_SET;
        uiConfirm();
        return true;
    }

    // ---------------- 主页的今日详情 ----------------
    // 上下滑动进入，点按退回主页
    if (g_page == PG_MAIN && g_mainDetail) {
        g_mainDetail = false;
        uiSettle();
        return true;
    }

    // ---------------- 历史的某一天 ----------------
    if (g_page == PG_HIST && g_histIdx >= 0) {
        g_histIdx = -1;
        uiSettle();
        return true;
    }

    if (g_page != PG_SET) { pageNext(); return true; }

    // ---------------- 设置列表 ----------------
    {
        int i = g_pressRow;                       // 按下时锁定的行，命中最准
        if (i < 0) {
            const float rel = (float)(y - ROW_Y0) + g_listPx;
            i = (int)floorf(rel / (float)ROW_H + 0.5f);
            const float cy = (float)ROW_Y0 + (float)(i * ROW_H) - g_listPx;
            if (fabsf((float)y - cy) > (float)ROW_H * 0.5f) return false;
        }
        if (i < 0 || i >= rowCount()) return false;

        if (i == SET_ROW_TIME) { enterTimePage(); return true; }
        if (i == SET_ROW_COLOR) { enterColorPage(); return true; }
        // 胶囊内任意位置都算按钮：左半减、右半加，按钮的分界与画面一致
        if (x >= PILL_X0 && x <= PILL_X1) {
            rowAdjust(i, (x < VAL_CX) ? -1 : +1);
            uiTick();
        }
        return true;
    }
}

// 每次 loop 调用
static bool uiTouch(uint32_t now)
{
    vibService();
    auto t = M5.Touch.getDetail(0);
    bool pressed = t.isPressed();

    if (pressed && !g_touchActive) {
        g_touchActive = true;
        g_tsX = t.x; g_tsY = t.y; g_tsMs = now;
        g_pressRow  = -1;
        g_pressBtn  = 0;
        g_dirLock   = 0;
        g_slideHint = 0.0f;

        // 设置列表：按下先记下命中的行和按钮，抬手时轻点才生效
        if (g_page == PG_SET && t.y > LIST_VP0 && t.y < LIST_VP1) {
            const float rel = (float)(t.y - ROW_Y0) + g_listPx;
            const int   i   = (int)floorf(rel / (float)ROW_H + 0.5f);
            if (i >= 0 && i < rowCount()) {
                const float cy = (float)ROW_Y0 + (float)(i * ROW_H) - g_listPx;
                if (fabsf((float)t.y - cy) <= (float)ROW_H * 0.5f) {
                    g_pressRow = i;
                    // 胶囊左右各半：左半减、右半加，与画面上的 − / + 位置一致
                    if (!rowIsLink(i) && t.x >= PILL_X0 && t.x <= PILL_X1) {
                        g_pressBtn  = (t.x < VAL_CX) ? -1 : 1;
                        g_btnNextMs = now + 500;      // 按住 500 ms 后开始连发
                    }
                }
            }

            g_listDrag    = true;
            g_listStartY  = t.y;
            g_listStartX  = t.x;
            g_listStartPx = g_listPx;
        }

        // 日期轮：按下先认列，竖向动起来才真正跟手（免得吃掉左右滑动）
        if (g_page == PG_TIME && g_timeMode == 0) {
            for (int c = 0; c < 3; ++c) {
                const int d = t.x - WH_X[c];
                if (d < -64 || d > 64) continue;
                g_wCol     = c;
                g_wStartY  = t.y;
                g_wStartPx = g_wPx[c];
                g_wMoved   = false;
                break;
            }
        }

        // 时钟表盘：按下即选值，之后跟随拖动（中心读数留给普通点按）
        if (g_page == PG_TIME && g_timeMode == 1) {
            const float dx = (float)t.x - (float)g_cx;
            const float dy = (float)t.y - (float)g_cy;
            const float d2 = dx * dx + dy * dy;
            const float r  = (float)DIAL_RING + 24.0f;
            if (d2 >= (float)(DIAL_DEAD * DIAL_DEAD) && d2 <= r * r) {
                g_dialActive = true;
                return dialPress(t.x, t.y);
            }
        }

        // 色轮：环上按下即选色相；中心圆内上下拖动调亮度
        if (g_page == PG_COLOR) {
            const float dx = (float)t.x - (float)g_cx;
            const float dy = (float)t.y - (float)g_cy;
            const float d2 = dx * dx + dy * dy;
            if (d2 <= 76.0f * 76.0f) {
                g_valDrag = true;
                g_valY0   = t.y;
                g_valV0   = g_colVal;
                return true;
            }
            const float r0 = (float)(CW_RIN - 8);
            const float r1 = (float)(CW_R + 24);
            if (d2 >= r0 * r0 && d2 <= r1 * r1) {
                g_colorDrag = true;
                g_colHue    = dialAngle(t.x, t.y);
                g_colPrev   = g_colHue;
                colorApply();
                return true;
            }
        }
        return false;
    }

    if (pressed) {
        // 按住 − / + 不放：500 ms 后开始连发，每 140 ms 走一档
        if (g_pressBtn != 0 && g_pressRow >= 0 && !rowIsLink(g_pressRow) &&
            g_pressRow != SET_ROW_LANG) {
            const int adx = (t.x > g_listStartX) ? t.x - g_listStartX : g_listStartX - t.x;
            const int ady = (t.y > g_listStartY) ? t.y - g_listStartY : g_listStartY - t.y;
            if (adx < 10 && ady < 10) {
                if ((int32_t)(now - g_btnNextMs) >= 0) {
                    rowAdjust(g_pressRow, g_pressBtn);
                    uiTick();
                    g_btnNextMs = now + 140;
                    return true;
                }
                return false;
            }
        }
        if (g_wCol >= 0) {
            const int dy = g_wStartY - t.y;          // 手指往上 → 数值变大
            if (!g_wMoved) {
                if (dy > 3 || dy < -3) g_wMoved = true;
                else return false;
            }
            float px = wheelCarry(g_wCol, g_wStartPx + (float)dy);
            if (px >  (float)WH_H) px =  (float)WH_H;
            if (px < -(float)WH_H) px = -(float)WH_H;
            // 已经进位的部分要从基准里扣掉，否则手指不动时每帧都会再进一格
            g_wStartPx = px - (float)dy;
            if (fabsf(px - g_wPx[g_wCol]) < 0.5f) return false;
            g_wPx[g_wCol] = px;
            return true;
        }
        if (g_listDrag) {
            // 方向锁：起手的十来像素先判定主轴，锁死后本次手势只做一件事
            // —— 左右就是切页，上下就是滚动，不会边滚边翻
            const int ddx = t.x - g_listStartX;
            const int ddy = t.y - g_listStartY;
            const int adx2 = ddx < 0 ? -ddx : ddx;
            const int ady2 = ddy < 0 ? -ddy : ddy;

            if (g_dirLock == 0) {
                if (adx2 < 10 && ady2 < 10) return false;      // 死区：手抖不算
                g_dirLock = (adx2 > ady2) ? 1 : 2;
                if (g_dirLock == 1) uiTick();                  // 锁到横向：给一下，切页模式
            }
            if (g_dirLock == 1) {                              // 横向：列表保持不动，只画进度
                float h = (float)adx2 / 40.0f;
                if (h > 1.0f) h = 1.0f;
                if (h == g_slideHint) return false;
                g_slideHint = h;
                return true;
            }

            const int   maxTop = rowCount() - ROW_PAGE;
            const float hi     = (float)(maxTop * ROW_H);
            float px = g_listStartPx + (float)(g_listStartY - t.y);
            if (px < 0.0f) px = 0.0f;
            if (px > hi)   px = hi;
            if (fabsf(px - g_listPx) < 0.5f) return false;
            g_listPx = px;
            uiMarkBand(LIST_VP0, LIST_VP1);   // 拖动只动列表可视区
            return true;
        }
        if (g_dialActive) return dialMove(t.x, t.y);

        if (g_valDrag) {
            // 横向为主的位移留给左右切色，不改亮度
            const int adx = t.x > g_tsX ? t.x - g_tsX : g_tsX - t.x;
            const int ady = t.y > g_tsY ? t.y - g_tsY : g_tsY - t.y;
            if (adx > ady + 10) return false;
            int v = g_valV0 + (g_valY0 - t.y) / 2;      // 向上变亮，2 px 一档
            if (v < 30)  v = 30;
            if (v > 100) v = 100;
            if (v == g_colVal) return false;
            g_colVal = v;
            colorApply();
            return true;
        }

        if (g_colorDrag) {
            const float a = dialAngle(t.x, t.y);
            float d = a - g_colPrev;
            if (d > 180.0f)  d -= 360.0f;
            if (d < -180.0f) d += 360.0f;
            g_colPrev = a;
            g_colHue += d;
            colorApply();
            return true;
        }
        return false;
    }

    if (g_touchActive) {
        g_touchActive = false;

        const int dx = t.x - g_tsX;
        const int dy = t.y - g_tsY;
        const uint32_t ms = now - g_tsMs;
        const int adx = dx < 0 ? -dx : dx;
        const int ady = dy < 0 ? -dy : dy;
        const bool hswipe = (adx >= 46 && adx > ady + 12);
        const bool vswipe = (ady >= 40 && ady > adx + 12);

        // 上下滑动：主页进出今日详情，历史翻阅每一天
        if (vswipe) {
            if (g_page == PG_MAIN) {
                const bool detail = (dy < 0);        // 上滑进入详情，下滑退回主页
                if (detail != g_mainDetail) {
                    g_mainDetail = detail;
                    uiSettle();
                }
                return true;
            }
            if (g_page == PG_HIST) {
                histStep(dy < 0 ? +1 : -1);
                return true;
            }
        }

        // 日期轮松手
        if (g_wCol >= 0) {
            const int c = g_wCol;
            g_wCol = -1;
            const bool moved = g_wMoved;
            g_wMoved = false;
            if (hswipe) {                        // 横向：切层级，轮子直接归零
                g_wPx[c] = 0.0f;
                if (dx < 0) timeNext(); else timeBack();
                return true;
            }
            if (moved) { wheelSnap(c); return false; }
            if (adx < 26 && ady < 26 && ms < 1200) return uiTap(t.x, t.y);
            return false;
        }

        // 设置列表松手：轻点 → 按钮/行；拖动 → 只滚列表或只切页
        if (g_listDrag) {
            g_listDrag = false;
            const int lock = g_dirLock;
            g_dirLock = 0;

            // 轻点：只看最终位移，中途锁定过方向也不影响 —— 手抖不该丢掉一次点击
            if (adx < 24 && ady < 24) {
                Serial.printf("[tap] %d,%d row=%d btn=%d d=%d,%d\n",
                              t.x, t.y, g_pressRow, g_pressBtn, dx, dy);
                g_slideHint = 0.0f;
                listSnap();
                if (g_pressBtn != 0 && g_pressRow >= 0 && !rowIsLink(g_pressRow)) {
                    rowAdjust(g_pressRow, g_pressBtn);
                    uiTick();
                    return true;
                }
                return uiTap(t.x, t.y);                  // 对时入口等
            }

            g_slideHint = 0.0f;
            if (lock == 1 && adx >= 40) {                // 横向：切页
                if (dx < 0) pageNext(); else pagePrev();
                return true;
            }
            listSnap();                                  // 纵向：只滚列表
            return false;
        }

        if (g_valDrag) {
            g_valDrag = false;
            if (hswipe) {                                // 横向：切换主题色 / 强调色
                if (dx < 0) colorNext(); else colorPrev();
                return true;
            }
            if (adx < 22 && ady < 22 && ms < 1200) return uiTap(t.x, t.y);  // 中心轻点＝确认
            uiSettle();
            return true;
        }

        if (g_colorDrag) {
            g_colorDrag = false;
            if (hswipe) {                                // 横向：切换主题色 / 强调色
                if (dx < 0) colorNext(); else colorPrev();
                return true;
            }
            if (adx < 26 && ady < 26 && ms < 1200) return uiTap(t.x, t.y);
            uiSettle();
            return true;
        }

        if (g_dialActive) { g_dialActive = false; uiSettle(); return true; }

        if (hswipe) {                                // 横向滑动：左滑下一层，右滑返回
            if (g_page == PG_TIME) {
                if (dx < 0) timeNext(); else timeBack();
            } else if (g_page == PG_COLOR) {
                if (dx < 0) colorNext(); else colorPrev();
            } else if (dx < 0) {
                pageNext();
            } else {
                pagePrev();
            }
            return true;
        }
        if (adx < 26 && ady < 26 && ms < 1200) {     // 点按
            return uiTap(t.x, t.y);
        }
    }
    return false;
}

// 熄屏 / 唤醒时把未完成的手势状态清干净，避免下次亮屏"接着上次的拖动"
static void uiTouchAbort()
{
    g_touchActive = false;
    g_listDrag    = false;
    g_dialActive  = false;
    g_colorDrag   = false;
    g_valDrag     = false;
    g_pressRow    = -1;
    g_pressBtn    = 0;
    g_dirLock     = 0;
    g_slideHint   = 0.0f;
    g_wCol        = -1;
    g_wMoved      = false;
    g_wPx[0] = g_wPx[1] = g_wPx[2] = 0.0f;
}
