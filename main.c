/**
 * @file main.c
 * @brief desk-info — 横屏信息台（直接绘制 /dev/fb0 + 屏幕触控设置 + HTTP）
 *
 * 目标设备：智伴 GS705B（ACTIONS OWL / Linux 3.10 / uClibc）
 * 屏：/dev/fb0 = 480×1600 虚拟（2 帧 480×800），RGB565，物理横屏 800×480。
 * 触控：goodix-ts (/dev/input/event2)，EV_ABS 报坐标 + EV_KEY 报 HOME(102)/BACK(30)。
 *
 * 屏幕设置：时钟界面右上角"设置"按钮 → 进入设置页（亮度加减/24小时制/返回）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <linux/fb.h>
#include <linux/input.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

typedef unsigned short u16;

#define HTTP_PORT      9000
#define CFG_PATH       "/var/upgrade/desk.conf"
#define BL_BRIGHTNESS  "/sys/class/backlight/owl_backlight/brightness"
#define BL_POWER       "/sys/class/backlight/owl_backlight/bl_power"
#define TOUCH_DEV      "/dev/input/event2"
#define TOUCH_LOG      "/tmp/touch.log"

/* ==================== 帧缓冲 ==================== */
static u16 *g_fb = NULL;
static int FB_W, FB_H, FB_VH;
static int SCR_W, SCR_H;

static inline u16 rgb565(int r, int g, int b)
{
    return (u16)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | ((b & 0xf8) >> 3));
}

/* 旋转方向 0-3（可被 /tmp/desk_rotate 运行时覆盖，方便调试切换） */
static int g_rotate = 1;
static long g_draw_off = 0;      /* 绘制目标帧在 fb 中的像素偏移 */
static int g_cur_frame = 1;      /* 当前正在显示的帧(0/1) */

/* 读 fb 的 pan, 同步"当前显示帧" */
static void read_pan(void)
{
    FILE *f = fopen("/sys/class/graphics/fb0/pan", "r");
    if (!f) return;
    int px_ = 0, py = 0;
    if (fscanf(f, "%d,%d", &px_, &py) == 2) {
        g_cur_frame = (py / FB_H) & 1;
        g_draw_off = (long)g_cur_frame * FB_W * FB_H;
    }
    fclose(f);
}

/* 双缓冲: 先画到后台帧, 再翻 pan 原子切换 —— 消除无 vsync 全屏重绘的撕裂闪烁 */
static void begin_frame(void)
{
    g_draw_off = (long)(g_cur_frame ^ 1) * FB_W * FB_H;
}

static void end_frame(void)
{
    int target = g_cur_frame ^ 1;
    char b[32];
    snprintf(b, sizeof(b), "0,%d", target * FB_H);
    int fd = open("/sys/class/graphics/fb0/pan", O_WRONLY);
    int ok = 0;
    if (fd >= 0) { ok = (write(fd, b, strlen(b)) > 0); close(fd); }
    if (ok) {
        g_cur_frame = target;
    } else {
        /* 翻页失败则退化为整帧拷贝(慢但正确) */
        memcpy(g_fb + (long)g_cur_frame * FB_W * FB_H,
               g_fb + (long)target * FB_W * FB_H,
               (size_t)FB_W * FB_H * 2);
    }
}

/* 横屏逻辑坐标 -> 帧缓冲，4 种可用变换 */
static void s2f(int sx, int sy, int *fx, int *fy)
{
    switch (g_rotate & 3) {
    case 0: *fx = sy;            *fy = FB_H - 1 - sx; break;
    case 1: *fx = FB_W - 1 - sy; *fy = sx;            break;
    case 2: *fx = sy;            *fy = sx;            break;
    default:*fx = FB_W - 1 - sy; *fy = FB_H - 1 - sx; break;
    }
}

static inline void px(int sx, int sy, u16 c)
{
    if (sx < 0 || sx >= SCR_W || sy < 0 || sy >= SCR_H) return;
    int fx, fy;
    s2f(sx, sy, &fx, &fy);
    long off = g_draw_off + (long)fy * FB_W + fx;   /* 只写绘制目标帧 */
    g_fb[off] = c;
}

static const unsigned char FONT[39][7] = {
    /* 0-9 */
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},{0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},{0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    /* : - */
    {0x00,0x06,0x06,0x00,0x06,0x06,0x00},{0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
    /* A-Z */
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, /* A */
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, /* B */
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, /* C */
    {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}, /* D */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, /* E */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, /* F */
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, /* G */
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, /* H */
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, /* I */
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, /* J */
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, /* K */
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, /* L */
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, /* M */
    {0x11,0x11,0x19,0x15,0x13,0x11,0x11}, /* N */
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* O */
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, /* P */
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, /* Q */
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, /* R */
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, /* S */
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, /* T */
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, /* U */
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, /* V */
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}, /* W */
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, /* X */
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, /* Y */
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, /* Z */
    {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}, /* + */
};

static int font_idx(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch == ':') return 10;
    if (ch == '-') return 11;
    if (ch >= 'A' && ch <= 'Z') return 12 + (ch - 'A');
    if (ch >= 'a' && ch <= 'z') return 12 + (ch - 'a');
    if (ch == '+') return 38;
    return 11;
}

static void draw_char(char ch, int sx, int sy, int scale, u16 color)
{
    int idx = font_idx(ch);
    for (int r = 0; r < 7; r++)
        for (int c = 0; c < 5; c++) {
            if (!(FONT[idx][r] & (0x10 >> c))) continue;
            for (int dy = 0; dy < scale; dy++)
                for (int dx = 0; dx < scale; dx++)
                    px(sx + c * scale + dx, sy + r * scale + dy, color);
        }
}

/* 只算文字宽度(不画) — 避免"量宽度"时把黑字画在屏上 */
static int text_width(const char *s, int scale)
{
    int w = 0;
    for (; *s; s++) w += (*s == ' ') ? 3 * scale : 6 * scale;
    return w;
}

static int draw_text(const char *s, int sx, int sy, int scale, u16 color)
{
    int x = sx;
    for (; *s; s++) {
        if (*s == ' ') { x += 3 * scale; continue; }
        draw_char(*s, x, sy, scale, color);
        x += 6 * scale;
    }
    return x - sx;
}

/* ==================== 中文字体(stb_truetype + gb2312.ttf) ==================== */
static unsigned char *g_font_data = NULL;
static stbtt_fontinfo g_font;
static float g_font_scale = 0;
static int g_font_px = 24;

static int font_load(const char *path, int px)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return -1; }
    g_font_data = (unsigned char *)malloc(sz);
    if (!g_font_data) { fclose(f); return -1; }
    if (fread(g_font_data, 1, sz, f) != (size_t)sz) { fclose(f); free(g_font_data); g_font_data = NULL; return -1; }
    fclose(f);
    if (!stbtt_InitFont(&g_font, g_font_data, stbtt_GetFontOffsetForIndex(g_font_data, 0)))
        return -1;
    g_font_px = px;
    g_font_scale = stbtt_ScaleForPixelHeight(&g_font, (float)px);
    return 0;
}

static unsigned int utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    unsigned int cp;
    if (p[0] < 0x80) { cp = p[0]; *s += 1; }
    else if ((p[0] & 0xE0) == 0xC0) { cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); *s += 2; }
    else if ((p[0] & 0xF0) == 0xE0) { cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); *s += 3; }
    else if ((p[0] & 0xF8) == 0xF0) { cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); *s += 4; }
    else { cp = '?'; *s += 1; }
    return cp;
}

/* 用 TTF 渲染 UTF-8 文本(支持中文), (x,y) 为左上角 */
static void draw_utf8(const char *s, int x, int y, u16 color)
{
    if (!g_font_data || g_font_scale <= 0) return;
    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&g_font, &ascent, &descent, &lineGap);
    int base = y + (int)(ascent * g_font_scale);
    int cx = x;
    while (*s) {
        unsigned int cp = utf8_next(&s);
        if (cp == '\n') { cx = x; base += (int)((ascent - descent + lineGap) * g_font_scale); continue; }
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&g_font, cp, &adv, &lsb);
        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(&g_font, cp, g_font_scale, g_font_scale, &x0, &y0, &x1, &y1);
        int w = x1 - x0, h = y1 - y0;
        if (w > 0 && h > 0) {
            int gw = w, gh = h;
            unsigned char *bmp = stbtt_GetCodepointBitmap(&g_font, 0, g_font_scale, cp, &gw, &gh, 0, 0);
            if (bmp) {
                for (int gy = 0; gy < gh; gy++)
                    for (int gx = 0; gx < gw; gx++)
                        if (bmp[gy * gw + gx] > 110)
                            px(cx + x0 + gx, base + y0 + gy, color);
                stbtt_FreeBitmap(bmp, 0);
            }
        }
        cx += (int)(adv * g_font_scale);
    }
}

/* UTF-8 文本宽度(像素) */
static int utf8_width(const char *s)
{
    if (!g_font_data || g_font_scale <= 0) return 0;
    int w = 0;
    while (*s) {
        unsigned int cp = utf8_next(&s);
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&g_font, cp, &adv, &lsb);
        w += (int)(adv * g_font_scale);
    }
    return w;
}

/* 居中绘制 UTF-8 文本; 超过 maxw 自动折成两行 */
static void draw_utf8_centered(const char *s, int y, u16 color)
{
    if (!g_font_data || g_font_scale <= 0 || !s || !s[0]) return;
    int maxw = SCR_W - 40;
    int w = utf8_width(s);
    if (w <= maxw) {
        int x = (SCR_W - w) / 2;
        if (x < 10) x = 10;
        draw_utf8(s, x, y, color);
        return;
    }
    /* 太长: 按字符数对半折行 */
    int total = 0;
    const char *p = s;
    while (*p) { utf8_next(&p); total++; }
    int half = total / 2;
    p = s;
    for (int i = 0; i < half && *p; i++) utf8_next(&p);
    char l1[300], l2[300];
    int n1 = (int)(p - s);
    if (n1 > (int)sizeof(l1) - 1) n1 = (int)sizeof(l1) - 1;
    memcpy(l1, s, n1); l1[n1] = '\0';
    strncpy(l2, p, sizeof(l2) - 1); l2[sizeof(l2) - 1] = '\0';
    int w1 = utf8_width(l1), w2 = utf8_width(l2);
    int x1 = (SCR_W - w1) / 2; if (x1 < 10) x1 = 10;
    int x2 = (SCR_W - w2) / 2; if (x2 < 10) x2 = 10;
    draw_utf8(l1, x1, y - 20, color);
    draw_utf8(l2, x2, y + 22, color);
}

static void fill_rect(int sx, int sy, int w, int h, u16 c)
{
    for (int y = sy; y < sy + h; y++)
        for (int x = sx; x < sx + w; x++)
            px(x, y, c);
}

/* ==================== 配置 ==================== */
typedef struct {
    int brightness;
    int hour24;
    int rotate;          /* 屏幕旋转 0-3 */
    int sleep_min;       /* 无操作多少分钟后息屏, 0=不息屏 */
    int volume;          /* 系统音量 0-40 (实际由 sair 原生 sound 库执行) */
    int alarm_on;        /* 闹钟开关 */
    int alarm_h, alarm_m;/* 闹钟时间 */
    char city[32];
    char lat[16];
    char lon[16];
    int refresh_min;
} desk_cfg_t;

static desk_cfg_t g_cfg;
static pthread_mutex_t g_cfg_lock = PTHREAD_MUTEX_INITIALIZER;

static void cfg_defaults(void)
{
    g_cfg.brightness = 200; g_cfg.hour24 = 1; g_cfg.rotate = 1; g_cfg.sleep_min = 0;
    g_cfg.volume = 20; g_cfg.alarm_on = 0; g_cfg.alarm_h = 7; g_cfg.alarm_m = 0;
    snprintf(g_cfg.city, sizeof(g_cfg.city), "深圳");
    snprintf(g_cfg.lat, sizeof(g_cfg.lat), "22.5431");
    snprintf(g_cfg.lon, sizeof(g_cfg.lon), "114.0579");
    g_cfg.refresh_min = 30;
}

static void sysfs_write_int(const char *path, int val)
{
    char b[32]; snprintf(b, sizeof(b), "%d", val);
    int fd = open(path, O_WRONLY);
    if (fd < 0) return;
    write(fd, b, strlen(b)); close(fd);
}

static void cfg_load(void)
{
    cfg_defaults();
    FILE *f = fopen(CFG_PATH, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char *p = strchr(line, '\n'); if (p) *p = '\0';
        p = strchr(line, '\r'); if (p) *p = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "brightness"))  g_cfg.brightness = atoi(v);
        else if (!strcmp(k, "hour24")) g_cfg.hour24 = atoi(v);
        else if (!strcmp(k, "city"))   snprintf(g_cfg.city, sizeof(g_cfg.city), "%s", v);
        else if (!strcmp(k, "lat"))    snprintf(g_cfg.lat, sizeof(g_cfg.lat), "%s", v);
        else if (!strcmp(k, "lon"))    snprintf(g_cfg.lon, sizeof(g_cfg.lon), "%s", v);
        else if (!strcmp(k, "refresh"))g_cfg.refresh_min = atoi(v);
        else if (!strcmp(k, "rotate")) g_cfg.rotate = atoi(v) & 3;
        else if (!strcmp(k, "sleep"))  g_cfg.sleep_min = atoi(v);
        else if (!strcmp(k, "volume")) g_cfg.volume = atoi(v);
        else if (!strcmp(k, "alarm_on")) g_cfg.alarm_on = atoi(v);
        else if (!strcmp(k, "alarm_h")) g_cfg.alarm_h = atoi(v);
        else if (!strcmp(k, "alarm_m")) g_cfg.alarm_m = atoi(v);
    }
    fclose(f);
    if (g_cfg.brightness < 10)  g_cfg.brightness = 10;
    if (g_cfg.brightness > 900) g_cfg.brightness = 900;
    if (g_cfg.refresh_min < 5)  g_cfg.refresh_min = 5;
    if (g_cfg.refresh_min > 1440) g_cfg.refresh_min = 1440;
    if (!g_cfg.city[0]) snprintf(g_cfg.city, sizeof(g_cfg.city), "深圳");
}

static void cfg_save(void)
{
    FILE *f = fopen(CFG_PATH, "w");
    if (!f) return;
    fprintf(f, "brightness=%d\n", g_cfg.brightness);
    fprintf(f, "hour24=%d\n", g_cfg.hour24);
    fprintf(f, "city=%s\n", g_cfg.city);
    fprintf(f, "lat=%s\n", g_cfg.lat);
    fprintf(f, "lon=%s\n", g_cfg.lon);
    fprintf(f, "refresh=%d\n", g_cfg.refresh_min);
    fprintf(f, "rotate=%d\n", g_cfg.rotate);
    fprintf(f, "sleep=%d\n", g_cfg.sleep_min);
    fprintf(f, "volume=%d\n", g_cfg.volume);
    fprintf(f, "alarm_on=%d\n", g_cfg.alarm_on);
    fprintf(f, "alarm_h=%d\n", g_cfg.alarm_h);
    fprintf(f, "alarm_m=%d\n", g_cfg.alarm_m);
    fclose(f);
}

/* ==================== 天气(明文 HTTP + Open-Meteo) ==================== */
static char g_weather[80] = "";
static volatile int g_weather_ok = 0;

/* 极简 HTTP GET: body 取出到 out(响应头丢弃) */
static int http_get(const char *host, const char *path, char *out, int out_sz)
{
    struct hostent *he = gethostbyname(host);
    if (!he) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(80);
    memcpy(&a.sin_addr, he->h_addr, he->h_length);
    struct timeval tv; tv.tv_sec = 10; tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
    char req[512];
    int rl = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: desk-info\r\nConnection: close\r\n\r\n",
        path, host);
    if (send(fd, req, rl, 0) < 0) { close(fd); return -1; }
    int total = 0, n;
    while (total < out_sz - 1 && (n = recv(fd, out + total, out_sz - 1 - total, 0)) > 0)
        total += n;
    close(fd);
    out[total] = '\0';
    char *body = strstr(out, "\r\n\r\n");
    if (!body) return -1;
    body += 4;
    int bl = (int)strlen(body);
    memmove(out, body, bl + 1);
    return 0;
}

static const char *wmo_desc(int c)
{
    if (c == 0) return "晴";
    if (c <= 2) return "多云";
    if (c == 3) return "阴";
    if (c == 45 || c == 48) return "雾";
    if (c >= 51 && c <= 57) return "毛毛雨";
    if (c >= 61 && c <= 67) return "雨";
    if (c >= 71 && c <= 77) return "雪";
    if (c >= 80 && c <= 82) return "阵雨";
    if (c >= 85 && c <= 86) return "阵雪";
    if (c >= 95) return "雷雨";
    return "未知";
}

/* 温度四舍五入取整(不引入浮点, 软浮点调设备硬浮点 libm 会出错) */
static int parse_temp(const char *p)
{
    int v = atoi(p);
    const char *dot = strchr(p, '.');
    if (dot && dot[1] >= '5') { if (v < 0) v--; else v++; }
    return v;
}

static void *weather_thread(void *arg)
{
    (void)arg;
    char *buf = (char *)malloc(8192);
    if (!buf) return NULL;
    for (;;) {
        int got = 0;
        char path[256];
        snprintf(path, sizeof(path),
            "/v1/forecast?latitude=%s&longitude=%s&current=temperature_2m,weather_code",
            g_cfg.lat, g_cfg.lon);
        int rc = http_get("api.open-meteo.com", path, buf, 8192);
        if (rc == 0) {
            /* 必须从 "current":{ 里找: 前面的 current_units 里也有同名键, 值是 "°C" 字符串 */
            char *cur = strstr(buf, "\"current\":{");
            if (!cur) cur = buf;
            char *t = strstr(cur, "\"temperature_2m\":");
            char *c = strstr(cur, "\"weather_code\":");
            if (t && c) {
                int temp = parse_temp(t + 17);
                int code = atoi(c + 15);
                snprintf(g_weather, sizeof(g_weather), "%s %s %dC",
                         g_cfg.city, wmo_desc(code), temp);
                g_weather_ok = 1;
                got = 1;
            }
        }
        {
            FILE *lf = fopen("/tmp/desk_weather.log", "w");
            if (lf) {
                fprintf(lf, "rc=%d ok=%d weather=%s\n", rc, got, g_weather);
                if (rc == 0) fprintf(lf, "body: %.500s\n", buf);
                fclose(lf);
            }
        }
        if (got) {
            int mins = g_cfg.refresh_min > 0 ? g_cfg.refresh_min : 30;
            sleep(mins * 60);
        } else {
            sleep(20);   /* 拉取失败 20 秒后重试, 避免一直空着 */
        }
    }
    return NULL;
}

/* ==================== 电量(sysfs power_supply) ==================== */
static int g_batt = -1;      /* 0-100, -1=未知 */
static int g_charging = 0;

static void battery_poll(void)
{
    int fd = open("/sys/class/power_supply/battery/capacity", O_RDONLY);
    if (fd >= 0) {
        char b[16];
        int n = read(fd, b, sizeof(b) - 1);
        close(fd);
        if (n > 0) { b[n] = '\0'; g_batt = atoi(b); }
    }
    fd = open("/sys/class/power_supply/battery/status", O_RDONLY);
    if (fd >= 0) {
        char b[32];
        int n = read(fd, b, sizeof(b) - 1);
        close(fd);
        if (n > 0) {
            b[n] = '\0';
            g_charging = (strncmp(b, "Charging", 8) == 0 || strncmp(b, "Full", 4) == 0);
        }
    }
}

/* ==================== 助手字幕(zhiban 的 sair 写文件过来) ==================== */
#define SUBTITLE_PATH "/tmp/sair_subtitle.txt"
static char g_subtitle[300] = "";

static void subtitle_poll(void)
{
    int fd = open(SUBTITLE_PATH, O_RDONLY);
    if (fd < 0) {                       /* 文件不存在 = 无字幕(助手没在说话) */
        g_subtitle[0] = '\0';
        return;
    }
    char b[sizeof(g_subtitle)];
    int n = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (n < 0) n = 0;
    b[n] = '\0';
    while (n > 0 && (b[n - 1] == '\n' || b[n - 1] == '\r')) b[--n] = '\0';
    if (strcmp(b, g_subtitle) != 0) {
        strncpy(g_subtitle, b, sizeof(g_subtitle) - 1);
        g_subtitle[sizeof(g_subtitle) - 1] = '\0';
    }
}

/* ==================== 触控 ==================== */
static volatile int g_touch_x, g_touch_y;   /* 当前触摸坐标(横屏 800x480) */
static volatile int g_key_event;            /* 最近一次按键 code(102/30/242/243) */
static volatile int g_tap, g_tap_x, g_tap_y;/* 一次点击待处理 */

/* 设备实际事件固定 16 字节(32位内核时间戳)。不用 sizeof(struct input_event):
 * 工具链头文件可能按 64 位时间戳定义成 24 字节, 导致 read 长度不匹配读不到事件。 */
struct raw_ev { unsigned int ts, tus; unsigned short type, code; int value; };

static void *touch_thread(void *arg)
{
    (void)arg;
    int fd = open(TOUCH_DEV, O_RDONLY);
    if (fd < 0) return NULL;
    FILE *log = fopen(TOUCH_LOG, "w");
    struct raw_ev ev;
    int pending = 0;
    for (;;) {
        int n = read(fd, &ev, sizeof(ev));
        if (n != (int)sizeof(ev)) continue;
        if (ev.type == EV_ABS) {
            if (ev.code == ABS_MT_POSITION_X) g_touch_x = ev.value;
            else if (ev.code == ABS_MT_POSITION_Y) g_touch_y = ev.value;
            else if (ev.code == ABS_MT_TRACKING_ID) {
                if (ev.value >= 0) pending = 1;   /* 手指按下, 帧末坐标才齐 */
            }
            if (log) { fprintf(log, "ABS c=%d v=%d\n", ev.code, ev.value); fflush(log); }
        } else if (ev.type == EV_KEY) {
            if (ev.value == 1) g_key_event = ev.code;
            if (log) { fprintf(log, "KEY c=%d v=%d\n", ev.code, ev.value); fflush(log); }
        } else if (ev.type == EV_SYN) {
            if (pending) {
                pending = 0;
                g_tap_x = g_touch_x; g_tap_y = g_touch_y; g_tap = 1;
                if (log) { fprintf(log, "TAP x=%d y=%d\n", g_tap_x, g_tap_y); fflush(log); }
            }
        }
    }
    return NULL;
}

static void touch_start(void)
{
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 64 * 1024);
    pthread_create(&t, &a, touch_thread, NULL);
    pthread_attr_destroy(&a);
}

/* 原始触摸坐标 -> 横屏坐标（与 px 旋转同款，待校准） */
static void raw_to_screen(int tx, int ty, int *sx, int *sy)
{
    /* 触摸设备直接报横屏 800x480 坐标, 与显示同一空间, 无需旋转 */
    *sx = tx;
    *sy = ty;
}

/* ==================== HTTP（保留，远程备用） ==================== */
static const char HTML_PAGE[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'><title>desk-info</title>"
"<style>body{font-family:sans-serif;background:#141824;color:#e8ecf8;padding:20px}"
"input{width:100%;padding:8px;background:#1a1f33;border:1px solid #2b3352;color:#e8ecf8}"
"button{background:#3b4a7a;color:#fff;border:none;padding:10px 20px;border-radius:6px}"
"</style></head><body><h1>desk-info 设置</h1>"
"<p>亮度 <span id='bv'>--</span><input type='range' id='br' min='10' max='900' oninput='setBr(this.value)'></p>"
"<p><label><input type='checkbox' id='h24'> 24小时制</label></p>"
"<p>城市 <input id='city'></p><p>纬度 <input id='lat'></p><p>经度 <input id='lon'></p>"
"<p>刷新(分) <input id='rf' type='number'></p><button onclick='save()'>保存</button>"
"<script>function load(){fetch('/config').then(r=>r.json()).then(c=>{br.value=c.brightness;"
"bv.textContent=c.brightness;h24.checked=!!c.hour24;city.value=c.city;lat.value=c.lat;"
"lon.value=c.lon;rf.value=c.refresh_min})}function post(d){fetch('/config',{method:'POST',"
"headers:{'Content-Type':'application/x-www-form-urlencoded'},body:d})}"
"function setBr(v){bv.textContent=v;post('brightness='+v)}"
"function save(){post('brightness='+br.value+'&hour24='+(h24.checked?1:0)+'&city='+"
"encodeURIComponent(city.value)+'&lat='+encodeURIComponent(lat.value)+'&lon='+"
"encodeURIComponent(lon.value)+'&refresh_min='+rf.value);alert('已保存')}load();</script>"
"</body></html>";

static void url_decode(const char *src, char *dst, int ds)
{
    int o = 0;
    for (int i = 0; src[i] && o < ds - 1; i++) {
        if (src[i] == '%' && src[i+1] && src[i+2]) { char h[3]={src[i+1],src[i+2],0}; dst[o++]=(char)strtol(h,NULL,16); i+=2; }
        else if (src[i] == '+') dst[o++] = ' ';
        else dst[o++] = src[i];
    }
    dst[o] = '\0';
}

static int form_get(const char *body, const char *key, char *out, int os)
{
    out[0] = '\0';
    int kl = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
            p += kl + 1; int i = 0;
            while (*p && *p != '&' && i < os - 1) out[i++] = *p++;
            out[i] = '\0'; return 0;
        }
        p = strchr(p, '&'); if (p) p++;
    }
    return -1;
}

static void http_send(int c, const char *s, const char *ct, const char *b, int len)
{
    char h[512];
    int hl = snprintf(h, sizeof(h), "HTTP/1.1 %s\r\nContent-Type: %s; charset=utf-8\r\n"
        "Content-Length: %d\r\nConnection: close\r\n\r\n", s, ct, len);
    send(c, h, hl, 0); send(c, b, len, 0);
}

static void config_to_json(char *buf, int size)
{
    snprintf(buf, size, "{\"brightness\":%d,\"hour24\":%d,\"city\":\"%s\",\"lat\":\"%s\","
        "\"lon\":\"%s\",\"refresh_min\":%d}", g_cfg.brightness, g_cfg.hour24,
        g_cfg.city, g_cfg.lat, g_cfg.lon, g_cfg.refresh_min);
}

static void http_handle(int c)
{
    char req[4096]; int n = recv(c, req, sizeof(req) - 1, 0);
    if (n <= 0) return; req[n] = '\0';
    char m[8]="", p[256]=""; sscanf(req, "%7s %255s", m, p);
    if (!strcmp(m,"GET") && !strcmp(p,"/")) http_send(c,"200 OK","text/html",HTML_PAGE,(int)strlen(HTML_PAGE));
    else if (!strcmp(m,"GET") && !strcmp(p,"/config")) {
        char j[512]; pthread_mutex_lock(&g_cfg_lock); config_to_json(j,sizeof(j)); pthread_mutex_unlock(&g_cfg_lock);
        http_send(c,"200 OK","application/json",j,(int)strlen(j));
    } else if (!strcmp(m,"POST") && !strcmp(p,"/config")) {
        char *b = strstr(req,"\r\n\r\n"); if (!b) b = strstr(req,"\n\n");
        if (b) b += (b[2]=='\n')?2:4;
        if (b) { char t[64]; pthread_mutex_lock(&g_cfg_lock);
            if (form_get(b,"brightness",t,sizeof(t))==0) g_cfg.brightness=atoi(t);
            if (form_get(b,"hour24",t,sizeof(t))==0) g_cfg.hour24=atoi(t);
            if (form_get(b,"city",t,sizeof(t))==0){char d[64];url_decode(t,d,sizeof(d));snprintf(g_cfg.city,sizeof(g_cfg.city),"%s",d);}
            if (form_get(b,"lat",t,sizeof(t))==0) snprintf(g_cfg.lat,sizeof(g_cfg.lat),"%s",t);
            if (form_get(b,"lon",t,sizeof(t))==0) snprintf(g_cfg.lon,sizeof(g_cfg.lon),"%s",t);
            if (form_get(b,"refresh_min",t,sizeof(t))==0){g_cfg.refresh_min=atoi(t);if(g_cfg.refresh_min<5)g_cfg.refresh_min=5;if(g_cfg.refresh_min>1440)g_cfg.refresh_min=1440;}
            if (g_cfg.brightness<10)g_cfg.brightness=10; if(g_cfg.brightness>900)g_cfg.brightness=900;
            cfg_save(); pthread_mutex_unlock(&g_cfg_lock);
            sysfs_write_int(BL_BRIGHTNESS, g_cfg.brightness);
        }
        http_send(c,"200 OK","application/json","{\"ok\":true}",11);
    } else http_send(c,"404 Not Found","text/plain","nf",2);
}

static void *httpd_thread(void *arg)
{
    (void)arg;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) return NULL;
    int one = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = htons(HTTP_PORT);
    if (bind(srv, (struct sockaddr*)&a, sizeof(a)) < 0) { close(srv); return NULL; }
    listen(srv, 4);
    for (;;) { int c = accept(srv, NULL, NULL); if (c < 0) { usleep(50000); continue; } http_handle(c); close(c); }
    return NULL;
}

static void httpd_start(void)
{
    pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64*1024);
    pthread_create(&t, &a, httpd_thread, NULL); pthread_attr_destroy(&a);
}

/* ==================== 设置界面（2 页） ==================== */
static int g_settings = 0;
static int g_settings_page = 0;    /* 0=主页 1=更多 */
static time_t g_last_active = 0;   /* 最近一次触摸时刻(息屏计时) */
static int g_screen_off = 0;

#define SROW_N  6
#define SROW_H  74
static const int SROW_Y[SROW_N] = { 10, 84, 158, 232, 306, 380 };

static int hit_row(int sy)
{
    for (int i = 0; i < SROW_N; i++)
        if (sy >= SROW_Y[i] && sy < SROW_Y[i] + SROW_H - 6) return i;
    return -1;
}

/* 给 sair 下发命令(文件式 IPC: /tmp/sair_cmd.json) */
static void sair_cmd(const char *json)
{
    int fd = open("/tmp/sair_cmd.json", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    write(fd, json, strlen(json));
    close(fd);
}

static void cycle_sleep(void)
{
    static const int v[] = {0, 1, 5, 15, 30, 60, 120};
    int i; for (i = 0; i < 7; i++) if (v[i] == g_cfg.sleep_min) break;
    g_cfg.sleep_min = v[(i + 1) % 7];
}

static void cycle_refresh(void)
{
    static const int v[] = {5, 10, 15, 30, 60, 120};
    int i; for (i = 0; i < 6; i++) if (v[i] == g_cfg.refresh_min) break;
    g_cfg.refresh_min = v[(i + 1) % 6];
}

/* 一行: 标签 + 右侧值(整行可点) */
static void srow_label(int i, const char *label, const char *val, u16 valcolor, u16 bg)
{
    int y = SROW_Y[i];
    fill_rect(20, y, SCR_W - 40, SROW_H - 10, bg);
    draw_text(label, 40, y + 20, 6, rgb565(255, 255, 255));
    if (val[0]) {
        int w = text_width(val, 6);
        draw_text(val, SCR_W - 60 - w, y + 20, 6, valcolor);
    }
}

/* 一行: 标签 + [-] 数值 [+] */
static void srow_pm(int i, const char *label, const char *val, u16 bg)
{
    int y = SROW_Y[i];
    fill_rect(20, y, SCR_W - 40, SROW_H - 10, bg);
    draw_text(label, 40, y + 20, 6, rgb565(205, 215, 245));
    fill_rect(232, y + 6, 96, SROW_H - 22, rgb565(72, 72, 112));
    draw_text("-", 268, y + 20, 7, rgb565(255, 255, 255));
    fill_rect(SCR_W - 148, y + 6, 96, SROW_H - 22, rgb565(72, 72, 112));
    draw_text("+", SCR_W - 112, y + 20, 7, rgb565(255, 255, 255));
    int w = text_width(val, 7);
    draw_text(val, (328 + SCR_W - 148 - w) / 2, y + 14, 7, rgb565(255, 255, 255));
}

static void draw_settings(void)
{
    char buf[32];
    begin_frame();
    for (int sy = 0; sy < SCR_H; sy++)
        for (int sx = 0; sx < SCR_W; sx++)
            px(sx, sy, rgb565(16, 18, 28));

    if (g_settings_page == 0) {
        snprintf(buf, sizeof(buf), "%d", g_cfg.brightness);
        srow_pm(0, "BRIGHT", buf, rgb565(45, 48, 68));
        snprintf(buf, sizeof(buf), "%d", g_cfg.volume);
        srow_pm(1, "VOLUME", buf, rgb565(45, 48, 68));
        srow_label(2, "24H", g_cfg.hour24 ? "ON" : "OFF",
                   g_cfg.hour24 ? rgb565(127, 224, 160) : rgb565(210, 130, 130), rgb565(45, 48, 68));
        if (g_cfg.sleep_min == 0) snprintf(buf, sizeof(buf), "OFF");
        else snprintf(buf, sizeof(buf), "%dM", g_cfg.sleep_min);
        srow_label(3, "SLEEP", buf, rgb565(225, 220, 130), rgb565(45, 48, 68));
        snprintf(buf, sizeof(buf), "%d", g_cfg.rotate);
        srow_label(4, "ROTATE", buf, rgb565(225, 220, 130), rgb565(45, 48, 68));
        srow_label(5, "MORE", ">", rgb565(160, 200, 255), rgb565(40, 54, 84));
    } else {
        snprintf(buf, sizeof(buf), "%dM", g_cfg.refresh_min);
        srow_label(0, "REFRESH", buf, rgb565(225, 220, 130), rgb565(45, 48, 68));
        srow_label(1, "ALARM", g_cfg.alarm_on ? "ON" : "OFF",
                   g_cfg.alarm_on ? rgb565(127, 224, 160) : rgb565(210, 130, 130), rgb565(45, 48, 68));
        snprintf(buf, sizeof(buf), "%02d", g_cfg.alarm_h);
        srow_pm(2, "ALARM-H", buf, rgb565(45, 48, 68));
        snprintf(buf, sizeof(buf), "%02d", g_cfg.alarm_m);
        srow_pm(3, "ALARM-M", buf, rgb565(45, 48, 68));
        srow_label(4, "POWER", "OFF", rgb565(255, 175, 175), rgb565(78, 48, 48));
        srow_label(5, "BACK", "", rgb565(255, 255, 255), rgb565(78, 48, 48));
    }
    end_frame();
}

/* ==================== 主程序 ==================== */
int main(void)
{
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) { perror("open /dev/fb0"); return 1; }
    struct fb_var_screeninfo vi;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vi) != 0) { perror("ioctl"); return 1; }
    FB_W = vi.xres; FB_H = vi.yres; FB_VH = vi.yres_virtual;
    SCR_W = FB_H; SCR_H = FB_W;
    long fbsize = (long)FB_W * FB_VH * (vi.bits_per_pixel / 8);
    g_fb = mmap(NULL, fbsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (g_fb == MAP_FAILED) { perror("mmap"); return 1; }

    cfg_load();
    g_rotate = g_cfg.rotate & 3;
    read_pan();
    {
        FILE *lf = fopen("/tmp/desk_fb.log", "w");
        if (lf) {
            fprintf(lf, "FB_W=%d FB_H=%d FB_VH=%d SCR_W=%d SCR_H=%d bpp=%d pan_off=%ld rot=%d\n",
                    FB_W, FB_H, FB_VH, SCR_W, SCR_H, vi.bits_per_pixel, g_draw_off, g_rotate);
            fclose(lf);
        }
    }
    sysfs_write_int(BL_BRIGHTNESS, g_cfg.brightness);
    httpd_start();
    touch_start();
    if (font_load("/usr/local/resource/gb2312.ttf", 26) != 0)
        fprintf(stderr, "[desk] 字体加载失败, 中文将不显示\n");
    battery_poll();
    unlink(SUBTITLE_PATH);   /* 清掉上次残留的字幕 */
    {
        pthread_t wt;
        if (pthread_create(&wt, NULL, weather_thread, NULL) == 0)
            pthread_detach(wt);
    }

    char last_clock[16] = "", last_weather[80] = "", last_sub[300] = "";
    int last_batt = -2, last_chg = -1;
    int last_settings = -1, last_brightness = -1, last_hour24 = -1, last_rotate = -1, last_sleep = -1;
    int last_page = -1, last_volume = -1, last_alarm = -1, last_ah = -1, last_am = -1;
    int alarm_fired = 0;
    int tick = 0;
    for (;;) {
        /* 旋转运行时覆盖(调试用): /tmp/desk_rotate 里写 0-3 即时切换, 免重编译 */
        if (++tick >= 30) {
            tick = 0;
            read_pan();
            subtitle_poll();
            battery_poll();
            /* 闹钟: 到点唤醒助手出声 */
            {
                time_t now = time(NULL);
                struct tm tm; localtime_r(&now, &tm);
                if (g_cfg.alarm_on && tm.tm_hour == g_cfg.alarm_h && tm.tm_min == g_cfg.alarm_m) {
                    if (!alarm_fired) { sair_cmd("{\"cmd\":\"wakeup\"}"); alarm_fired = 1; }
                } else {
                    alarm_fired = 0;
                }
            }
            /* 息屏: 无操作超过 sleep_min 分钟则关背光 */
            if (g_cfg.sleep_min > 0 && !g_screen_off && g_last_active &&
                time(NULL) - g_last_active >= g_cfg.sleep_min * 60) {
                sysfs_write_int(BL_POWER, 1);
                g_screen_off = 1;
            }
            int rfd = open("/tmp/desk_rotate", O_RDONLY);
            if (rfd >= 0) {
                char rb[8]; int rn = read(rfd, rb, sizeof(rb) - 1); close(rfd);
                if (rn > 0) {
                    rb[rn] = '\0'; int rv = atoi(rb);
                    if (rv >= 0 && rv <= 3 && rv != g_rotate) {
                        g_rotate = rv; last_clock[0] = '\0'; last_settings = -1;
                    }
                }
            }
        }
        /* 处理按键 */
        int key = g_key_event;
        if (key) {
            if (key == 102) g_settings = !g_settings;      /* HOME 切设置 */
            else if (key == 30) g_settings = 0;            /* BACK 退出 */
            else if (key == 242 && g_settings) { g_cfg.brightness += 20; if (g_cfg.brightness>900) g_cfg.brightness=900; sysfs_write_int(BL_BRIGHTNESS,g_cfg.brightness); cfg_save(); }
            else if (key == 243 && g_settings) { g_cfg.brightness -= 20; if (g_cfg.brightness<10) g_cfg.brightness=10; sysfs_write_int(BL_BRIGHTNESS,g_cfg.brightness); cfg_save(); }
            g_key_event = 0;
        }
        /* 处理触摸点击 */
        if (g_tap) {
            g_tap = 0;
            int sx, sy; raw_to_screen(g_tap_x, g_tap_y, &sx, &sy);
            g_last_active = time(NULL);
            if (g_screen_off) {                 /* 息屏时首次触摸: 只亮屏 */
                sysfs_write_int(BL_POWER, 0);
                sysfs_write_int(BL_BRIGHTNESS, g_cfg.brightness);
                g_screen_off = 0;
            } else if (g_settings) {
                int row = hit_row(sy);
                int leftb = sx < 330;              /* [-] 区域 */
                int rightb = sx > SCR_W - 200;     /* [+] 区域 */
                if (g_settings_page == 0) {
                    if (row == 0) {                /* 亮度 */
                        if (leftb) g_cfg.brightness -= 30;
                        else if (rightb) g_cfg.brightness += 30;
                        if (g_cfg.brightness < 10) g_cfg.brightness = 10;
                        if (g_cfg.brightness > 900) g_cfg.brightness = 900;
                        sysfs_write_int(BL_BRIGHTNESS, g_cfg.brightness);
                        cfg_save();
                    }
                    else if (row == 1) {           /* 音量(经 sair 原生 sound 库) */
                        if (leftb) g_cfg.volume -= 2;
                        else if (rightb) g_cfg.volume += 2;
                        if (g_cfg.volume < 0) g_cfg.volume = 0;
                        if (g_cfg.volume > 40) g_cfg.volume = 40;
                        char c[64];
                        snprintf(c, sizeof(c), "{\"cmd\":\"set_config\",\"volume\":%d}", g_cfg.volume);
                        sair_cmd(c);
                        cfg_save();
                    }
                    else if (row == 2) { g_cfg.hour24 = !g_cfg.hour24; cfg_save(); }
                    else if (row == 3) { cycle_sleep(); cfg_save(); }
                    else if (row == 4) { g_cfg.rotate = (g_cfg.rotate + 1) & 3; g_rotate = g_cfg.rotate; cfg_save(); }
                    else if (row == 5) { g_settings_page = 1; }
                } else {
                    if (row == 0) { cycle_refresh(); cfg_save(); }
                    else if (row == 1) { g_cfg.alarm_on = !g_cfg.alarm_on; cfg_save(); }
                    else if (row == 2) {
                        if (leftb) g_cfg.alarm_h = (g_cfg.alarm_h + 23) % 24;
                        else if (rightb) g_cfg.alarm_h = (g_cfg.alarm_h + 1) % 24;
                        cfg_save();
                    }
                    else if (row == 3) {
                        if (leftb) g_cfg.alarm_m = (g_cfg.alarm_m + 55) % 60;
                        else if (rightb) g_cfg.alarm_m = (g_cfg.alarm_m + 5) % 60;
                        cfg_save();
                    }
                    else if (row == 4) { sair_cmd("{\"cmd\":\"poweroff\"}"); }
                    else if (row == 5) { g_settings = 0; g_settings_page = 0; }
                }
            } else {
                /* 时钟界面：右上角设置按钮 */
                if (sx > SCR_W - 200 && sy < 100) g_settings = 1;
            }
        }

        /* 按需重绘(内容变化才画, 避免全屏高频无 vsync 撕裂闪烁) */
        if (g_settings) {
            if (g_settings != last_settings || g_settings_page != last_page ||
                g_cfg.brightness != last_brightness || g_cfg.hour24 != last_hour24 ||
                g_cfg.rotate != last_rotate || g_cfg.sleep_min != last_sleep ||
                g_cfg.volume != last_volume || g_cfg.alarm_on != last_alarm ||
                g_cfg.alarm_h != last_ah || g_cfg.alarm_m != last_am) {
                draw_settings();                last_settings = g_settings; last_brightness = g_cfg.brightness;
                last_hour24 = g_cfg.hour24; last_rotate = g_cfg.rotate;
                last_sleep = g_cfg.sleep_min; last_page = g_settings_page;
                last_volume = g_cfg.volume; last_alarm = g_cfg.alarm_on;
                last_ah = g_cfg.alarm_h; last_am = g_cfg.alarm_m;
            }
        } else {
            time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
            int hour = tm.tm_hour;
            if (!g_cfg.hour24) { hour %= 12; if (hour == 0) hour = 12; }
            char clock[16], date[40];
            snprintf(clock, sizeof(clock), "%02d:%02d", hour, tm.tm_min);
            snprintf(date, sizeof(date), "%d-%02d-%02d", tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday);
            if (strcmp(clock, last_clock) != 0 || g_settings != last_settings ||
                strcmp(g_weather, last_weather) != 0 || strcmp(g_subtitle, last_sub) != 0 ||
                g_batt != last_batt || g_charging != last_chg) {
                /* 背景渐变 */
                begin_frame();
                for (int sy = 0; sy < SCR_H; sy++) {
                    u16 c = rgb565(14, 18 + sy/14, 34 + sy/7);
                    for (int sx = 0; sx < SCR_W; sx++) px(sx, sy, c);
                }
                /* 底部字幕(助手 TTS 文本): 居中, 不加背景块/分割线 */
                if (g_subtitle[0])
                    draw_utf8_centered(g_subtitle, 400, rgb565(214, 232, 255));
                /* 大时钟(固定位置, 不因子幕出现而上移) */
                int scale = 22;
                int cw = text_width(clock, scale);
                int cy = 88;
                draw_text(clock, (SCR_W - cw)/2, cy, scale, rgb565(255, 255, 255));
                /* 日期 */
                int dw = text_width(date, 6);
                draw_text(date, (SCR_W - dw)/2, cy + 7*scale + 14, 6, rgb565(180, 200, 255));
                /* 天气(左上角, 中文) */
                if (g_weather_ok)
                    draw_utf8(g_weather, 24, 14, rgb565(150, 210, 255));
                /* 右上角设置按钮 */
                fill_rect(SCR_W - 170, 12, 150, 62, rgb565(255, 140, 0));
                draw_text("SET", SCR_W - 140, 26, 5, rgb565(0, 0, 0));
                /* 电量(设置按钮左侧) */
                if (g_batt >= 0) {
                    char bb[24];
                    snprintf(bb, sizeof(bb), "%d%%", g_batt);
                    int bw = text_width(bb, 5);
                    draw_text(bb, SCR_W - 190 - bw, 26, 5,
                              g_batt <= 15 ? rgb565(255, 120, 120) : rgb565(190, 240, 190));
                    if (g_charging)
                        draw_text("CHG", SCR_W - 190 - bw - 100, 26, 4, rgb565(255, 220, 120));
                }
                strcpy(last_clock, clock);
                strcpy(last_weather, g_weather);
                strcpy(last_sub, g_subtitle);
                last_settings = g_settings;
                last_batt = g_batt; last_chg = g_charging;
                end_frame();
            }
        }
        usleep(30000);
    }
    return 0;
}
