/**
 * @file main.c
 * @brief desk-info — 横屏信息台（独立项目，直接绘制 /dev/fb0 + HTTP 设置页）
 *
 * 目标设备：智伴 GS705B（ACTIONS OWL / Linux 3.10 / uClibc）
 * 屏：/dev/fb0 = 480×1600 虚拟（2 帧 480×800），RGB565，物理横屏 800×480。
 *
 * 功能：
 *  - 直接画帧缓冲的大时钟 + 日期
 *  - HTTP 设置页（端口 9000）：亮度/24小时制/天气城市经纬度/刷新周期
 *  - 设置存 /var/upgrade/desk.conf（key=value）
 *
 * 编译：静态链接（避免设备 hard-float uClibc ABI 不匹配）
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
#include <linux/fb.h>

typedef unsigned short u16;

/* ==================== 常量 ==================== */
#define HTTP_PORT      9000
#define CFG_PATH       "/var/upgrade/desk.conf"
#define BL_BRIGHTNESS  "/sys/class/backlight/owl_backlight/brightness"

/* ==================== 帧缓冲 ==================== */
static u16 *g_fb = NULL;
static int FB_W, FB_H, FB_VH;
static int SCR_W, SCR_H;

static inline u16 rgb565(int r, int g, int b)
{
    return (u16)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | ((b & 0xf8) >> 3));
}

static inline void px(int sx, int sy, u16 c)
{
    if (sx < 0 || sx >= SCR_W || sy < 0 || sy >= SCR_H) return;
    int fx = sy;
    int fy = FB_H - 1 - sx;          /* 旋转方向 A；反了改 fy = sx */
    long off = (long)fy * FB_W + fx;
    g_fb[off] = c;
    g_fb[off + (long)FB_W * FB_H] = c; /* 双缓冲两帧都写 */
}

static const unsigned char FONT[12][7] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, /* 0 */
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, /* 1 */
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, /* 2 */
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, /* 3 */
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, /* 4 */
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, /* 5 */
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, /* 6 */
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, /* 7 */
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, /* 8 */
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, /* 9 */
    {0x00,0x06,0x06,0x00,0x06,0x06,0x00}, /* : */
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, /* - */
};

static void draw_char(char ch, int sx, int sy, int scale, u16 color)
{
    int idx;
    if (ch >= '0' && ch <= '9') idx = ch - '0';
    else if (ch == ':')         idx = 10;
    else                        idx = 11;
    for (int r = 0; r < 7; r++)
        for (int c = 0; c < 5; c++) {
            if (!(FONT[idx][r] & (0x10 >> c))) continue;
            for (int dy = 0; dy < scale; dy++)
                for (int dx = 0; dx < scale; dx++)
                    px(sx + c * scale + dx, sy + r * scale + dy, color);
        }
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

/* ==================== 配置 ==================== */
typedef struct {
    int brightness;      /* 背光亮度 10-900 */
    int hour24;          /* 1=24小时制 0=12小时制 */
    char city[32];
    char lat[16];
    char lon[16];
    int refresh_min;     /* 天气刷新周期(分钟) */
} desk_cfg_t;

static desk_cfg_t g_cfg;
static pthread_mutex_t g_cfg_lock = PTHREAD_MUTEX_INITIALIZER;

static void cfg_defaults(void)
{
    g_cfg.brightness = 200;
    g_cfg.hour24 = 1;
    snprintf(g_cfg.city, sizeof(g_cfg.city), "深圳");
    snprintf(g_cfg.lat, sizeof(g_cfg.lat), "22.5431");
    snprintf(g_cfg.lon, sizeof(g_cfg.lon), "114.0579");
    g_cfg.refresh_min = 30;
}

static int sysfs_read_int(const char *path, int def)
{
    char b[32];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return def;
    int n = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (n <= 0) return def;
    b[n] = '\0';
    return atoi(b);
}

static void sysfs_write_int(const char *path, int val)
{
    char b[32];
    snprintf(b, sizeof(b), "%d", val);
    int fd = open(path, O_WRONLY);
    if (fd < 0) return;
    write(fd, b, strlen(b));
    close(fd);
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
    fclose(f);
}

/* ==================== HTTP 服务 ==================== */

static void url_decode(const char *src, char *dst, int dst_size)
{
    int o = 0;
    for (int i = 0; src[i] && o < dst_size - 1; i++) {
        if (src[i] == '%' && src[i+1] && src[i+2]) {
            char h[3] = {src[i+1], src[i+2], 0};
            dst[o++] = (char)strtol(h, NULL, 16);
            i += 2;
        } else if (src[i] == '+') {
            dst[o++] = ' ';
        } else {
            dst[o++] = src[i];
        }
    }
    dst[o] = '\0';
}

static int form_get(const char *body, const char *key, char *out, int out_size)
{
    out[0] = '\0';
    int klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            int i = 0;
            while (*p && *p != '&' && i < out_size - 1) out[i++] = *p++;
            out[i] = '\0';
            return 0;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return -1;
}

static void http_send(int c, const char *status, const char *ctype, const char *body, int len)
{
    char hdr[512];
    int hl = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %s\r\nContent-Type: %s; charset=utf-8\r\nContent-Length: %d\r\n"
        "Connection: close\r\n\r\n",
        status, ctype, len);
    send(c, hdr, hl, 0);
    send(c, body, len, 0);
}

static void config_to_json(char *buf, int size)
{
    snprintf(buf, size,
        "{\"brightness\":%d,\"hour24\":%d,\"city\":\"%s\",\"lat\":\"%s\","
        "\"lon\":\"%s\",\"refresh_min\":%d}",
        g_cfg.brightness, g_cfg.hour24, g_cfg.city, g_cfg.lat, g_cfg.lon,
        g_cfg.refresh_min);
}

static const char HTML_PAGE[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>desk-info 设置</title>"
"<style>body{font-family:sans-serif;background:#141824;color:#e8ecf8;margin:0;"
"padding:20px;max-width:520px;margin:auto}h1{font-size:20px}"
".row{margin:14px 0}label{display:block;font-size:14px;color:#9aa3c0;margin-bottom:6px}"
"input[type=range]{width:100%}input[type=text],input[type=number]{width:100%;"
"box-sizing:border-box;padding:8px;background:#1a1f33;border:1px solid #2b3352;"
"color:#e8ecf8;border-radius:6px}"
"button{background:#3b4a7a;color:#fff;border:none;padding:10px 20px;border-radius:6px;"
"font-size:15px;cursor:pointer}.val{color:#7fe0a0;font-weight:bold}"
".tip{color:#6b7694;font-size:12px;margin-top:16px}</style></head><body>"
"<h1>desk-info 设置</h1>"
"<div class='row'><label>亮度 <span class='val' id='bv'>--</span></label>"
"<input type='range' id='br' min='10' max='900' oninput='setBr(this.value)'></div>"
"<div class='row'><label><input type='checkbox' id='h24'> 24小时制</label></div>"
"<div class='row'><label>城市</label><input type='text' id='city'></div>"
"<div class='row'><label>纬度</label><input type='text' id='lat'></div>"
"<div class='row'><label>经度</label><input type='text' id='lon'></div>"
"<div class='row'><label>天气刷新(分钟)</label><input type='number' id='rf' min='5' max='1440'></div>"
"<div class='row'><button onclick='save()'>保存</button></div>"
"<div class='tip'>原厂设置(apconfig)已提取：亮度/屏保/闹钟/定时开关机/语言/音量等，"
"后续逐步接入本页。</div>"
"<script>"
"function load(){fetch('/config').then(r=>r.json()).then(c=>{"
"br.value=c.brightness;bv.textContent=c.brightness;h24.checked=!!c.hour24;"
"city.value=c.city;lat.value=c.lat;lon.value=c.lon;rf.value=c.refresh_min;})}"
"function setBr(v){bv.textContent=v;post('brightness='+v)}"
"function post(d){fetch('/config',{method:'POST',headers:{'Content-Type':"
"'application/x-www-form-urlencoded'},body:d})}"
"function save(){post('brightness='+br.value+'&hour24='+(h24.checked?1:0)+"
"'&city='+encodeURIComponent(city.value)+'&lat='+encodeURIComponent(lat.value)+"
"'&lon='+encodeURIComponent(lon.value)+'&refresh_min='+rf.value);"
"alert('已保存')}"
"load();</script></body></html>";

static void http_handle(int c)
{
    char req[4096];
    int n = recv(c, req, sizeof(req) - 1, 0);
    if (n <= 0) return;
    req[n] = '\0';

    char method[8] = "", path[256] = "";
    sscanf(req, "%7s %255s", method, path);

    if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
        http_send(c, "200 OK", "text/html", HTML_PAGE, (int)strlen(HTML_PAGE));
    }
    else if (strcmp(method, "GET") == 0 && strcmp(path, "/config") == 0) {
        char j[512];
        pthread_mutex_lock(&g_cfg_lock);
        config_to_json(j, sizeof(j));
        pthread_mutex_unlock(&g_cfg_lock);
        http_send(c, "200 OK", "application/json", j, (int)strlen(j));
    }
    else if (strcmp(method, "POST") == 0 && strcmp(path, "/config") == 0) {
        char *body = strstr(req, "\r\n\r\n");
        if (!body) body = strstr(req, "\n\n");
        if (body) body += (body[2] == '\n') ? 2 : 4;
        if (body) {
            char tmp[64];
            pthread_mutex_lock(&g_cfg_lock);
            if (form_get(body, "brightness", tmp, sizeof(tmp)) == 0)
                g_cfg.brightness = atoi(tmp);
            if (form_get(body, "hour24", tmp, sizeof(tmp)) == 0)
                g_cfg.hour24 = atoi(tmp);
            if (form_get(body, "city", tmp, sizeof(tmp)) == 0) {
                char dec[64]; url_decode(tmp, dec, sizeof(dec));
                snprintf(g_cfg.city, sizeof(g_cfg.city), "%s", dec);
            }
            if (form_get(body, "lat", tmp, sizeof(tmp)) == 0)
                snprintf(g_cfg.lat, sizeof(g_cfg.lat), "%s", tmp);
            if (form_get(body, "lon", tmp, sizeof(tmp)) == 0)
                snprintf(g_cfg.lon, sizeof(g_cfg.lon), "%s", tmp);
            if (form_get(body, "refresh_min", tmp, sizeof(tmp)) == 0) {
                g_cfg.refresh_min = atoi(tmp);
                if (g_cfg.refresh_min < 5) g_cfg.refresh_min = 5;
                if (g_cfg.refresh_min > 1440) g_cfg.refresh_min = 1440;
            }
            if (g_cfg.brightness < 10)  g_cfg.brightness = 10;
            if (g_cfg.brightness > 900) g_cfg.brightness = 900;
            cfg_save();
            pthread_mutex_unlock(&g_cfg_lock);
            sysfs_write_int(BL_BRIGHTNESS, g_cfg.brightness);
        }
        http_send(c, "200 OK", "application/json", "{\"ok\":true}", 11);
    }
    else {
        http_send(c, "404 Not Found", "text/plain", "not found", 9);
    }
}

static void *httpd_thread(void *arg)
{
    (void)arg;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) return NULL;
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(HTTP_PORT);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "bind %d failed: %s\n", HTTP_PORT, strerror(errno));
        close(srv);
        return NULL;
    }
    listen(srv, 4);
    fprintf(stderr, "httpd listening on :%d\n", HTTP_PORT);
    for (;;) {
        int c = accept(srv, NULL, NULL);
        if (c < 0) { usleep(50000); continue; }
        http_handle(c);
        close(c);
    }
    return NULL;
}

static void httpd_start(void)
{
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 64 * 1024);
    if (pthread_create(&t, &a, httpd_thread, NULL) != 0)
        fprintf(stderr, "httpd thread create failed\n");
    pthread_attr_destroy(&a);
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
    sysfs_write_int(BL_BRIGHTNESS, g_cfg.brightness); /* 开机应用亮度 */
    httpd_start();

    for (;;) {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);

        int hour = tm.tm_hour;
        if (!g_cfg.hour24) {
            hour = hour % 12;
            if (hour == 0) hour = 12;
        }
        char clock[16], date[40];
        snprintf(clock, sizeof(clock), "%02d:%02d", hour, tm.tm_min);
        snprintf(date, sizeof(date), "%d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

        for (int sy = 0; sy < SCR_H; sy++) {
            u16 c = rgb565(14, 18 + sy / 14, 34 + sy / 7);
            for (int sx = 0; sx < SCR_W; sx++) px(sx, sy, c);
        }
        for (int sx = 0; sx < SCR_W; sx++) px(sx, 0, rgb565(255, 0, 0));

        int scale = 22;
        int cw = draw_text(clock, 0, 0, scale, rgb565(0,0,0));
        int sx0 = (SCR_W - cw) / 2;
        int sy0 = (SCR_H - 7 * scale) / 2 - 20;
        draw_text(clock, sx0, sy0, scale, rgb565(255, 255, 255));

        int dscale = 5;
        int dw = draw_text(date, 0, 0, dscale, rgb565(0,0,0));
        draw_text(date, (SCR_W - dw) / 2, sy0 + 7 * scale + 20, dscale, rgb565(200, 210, 255));

        sleep(1);
    }
    return 0;
}
