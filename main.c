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
#include <linux/fb.h>
#include <linux/input.h>

typedef unsigned short u16;

#define HTTP_PORT      9000
#define CFG_PATH       "/var/upgrade/desk.conf"
#define BL_BRIGHTNESS  "/sys/class/backlight/owl_backlight/brightness"
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
static long g_pan_off = 0;   /* 可见帧在 fb 中的像素偏移(按 pan) */

/* 读 fb 的 pan, 决定往哪一帧写(本设备虚拟 480x1600 = 2 帧 480x800) */
static void read_pan(void)
{
    FILE *f = fopen("/sys/class/graphics/fb0/pan", "r");
    if (!f) return;
    int px_ = 0, py = 0;
    if (fscanf(f, "%d,%d", &px_, &py) == 2)
        g_pan_off = (long)py * FB_W;
    fclose(f);
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
    long off = g_pan_off + (long)fy * FB_W + fx;   /* 只写当前可见帧 */
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
    char city[32];
    char lat[16];
    char lon[16];
    int refresh_min;
} desk_cfg_t;

static desk_cfg_t g_cfg;
static pthread_mutex_t g_cfg_lock = PTHREAD_MUTEX_INITIALIZER;

static void cfg_defaults(void)
{
    g_cfg.brightness = 200; g_cfg.hour24 = 1; g_cfg.rotate = 1;
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
    fclose(f);
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

/* ==================== 设置界面 ==================== */
static int g_settings = 0;

typedef struct { int x, y, w, h; } btn_t;

/* 设置页按钮布局（横屏 800x480） */
#define BTN_BRIGHT_MIN  (btn_t){50, 60, 140, 100}
#define BTN_BRIGHT_PLUS (btn_t){700, 60, 140, 100}
#define BTN_HOUR24      (btn_t){50, 220, 700, 80}
#define BTN_BACK        (btn_t){50, 360, 700, 80}

static int hit(btn_t b, int sx, int sy)
{
    return sx >= b.x && sx < b.x + b.w && sy >= b.y && sy < b.y + b.h;
}

static void draw_settings(void)
{
    for (int sy = 0; sy < SCR_H; sy++)
        for (int sx = 0; sx < SCR_W; sx++)
            px(sx, sy, rgb565(18, 20, 30));

    /* 亮度行 */
    fill_rect(BTN_BRIGHT_MIN.x, BTN_BRIGHT_MIN.y, BTN_BRIGHT_MIN.w, BTN_BRIGHT_MIN.h, rgb565(60,60,90));
    draw_text("-", BTN_BRIGHT_MIN.x + 55, BTN_BRIGHT_MIN.y + 35, 8, rgb565(255,255,255));
    char b[16]; snprintf(b, sizeof(b), "%d", g_cfg.brightness);
    int w = text_width(b, 6);
    draw_text(b, (SCR_W - w)/2, 60 + 35, 6, rgb565(255,255,255));
    fill_rect(BTN_BRIGHT_PLUS.x, BTN_BRIGHT_PLUS.y, BTN_BRIGHT_PLUS.w, BTN_BRIGHT_PLUS.h, rgb565(60,60,90));
    draw_text("+", BTN_BRIGHT_PLUS.x + 55, BTN_BRIGHT_PLUS.y + 35, 8, rgb565(255,255,255));

    /* 24小时制 */
    fill_rect(BTN_HOUR24.x, BTN_HOUR24.y, BTN_HOUR24.w, BTN_HOUR24.h, rgb565(50,50,70));
    draw_text("24H", BTN_HOUR24.x + 30, BTN_HOUR24.y + 20, 6, rgb565(255,255,255));
    const char *st = g_cfg.hour24 ? "ON" : "OFF";
    draw_text(st, BTN_HOUR24.x + BTN_HOUR24.w - 200, BTN_HOUR24.y + 20, 6,
              g_cfg.hour24 ? rgb565(127,224,160) : rgb565(200,120,120));

    /* 返回 */
    fill_rect(BTN_BACK.x, BTN_BACK.y, BTN_BACK.w, BTN_BACK.h, rgb565(70,50,50));
    draw_text("BACK", BTN_BACK.x + 30, BTN_BACK.y + 20, 6, rgb565(255,255,255));
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
                    FB_W, FB_H, FB_VH, SCR_W, SCR_H, vi.bits_per_pixel, g_pan_off, g_rotate);
            fclose(lf);
        }
    }
    sysfs_write_int(BL_BRIGHTNESS, g_cfg.brightness);
    httpd_start();
    touch_start();

    char last_clock[16] = "";
    int last_settings = -1, last_brightness = -1, last_hour24 = -1;
    int tick = 0;
    for (;;) {
        /* 旋转运行时覆盖(调试用): /tmp/desk_rotate 里写 0-3 即时切换, 免重编译 */
        if (++tick >= 30) {
            tick = 0;
            read_pan();
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
            if (g_settings) {
                if (hit(BTN_BRIGHT_MIN, sx, sy)) { g_cfg.brightness -= 20; if(g_cfg.brightness<10)g_cfg.brightness=10; sysfs_write_int(BL_BRIGHTNESS,g_cfg.brightness); cfg_save(); }
                else if (hit(BTN_BRIGHT_PLUS, sx, sy)) { g_cfg.brightness += 20; if(g_cfg.brightness>900)g_cfg.brightness=900; sysfs_write_int(BL_BRIGHTNESS,g_cfg.brightness); cfg_save(); }
                else if (hit(BTN_HOUR24, sx, sy)) { g_cfg.hour24 = !g_cfg.hour24; cfg_save(); }
                else if (hit(BTN_BACK, sx, sy)) g_settings = 0;
            } else {
                /* 时钟界面：右上角设置按钮 */
                if (sx > SCR_W - 200 && sy < 100) g_settings = 1;
            }
        }

        /* 按需重绘(内容变化才画, 避免全屏高频无 vsync 撕裂闪烁) */
        if (g_settings) {
            if (g_settings != last_settings || g_cfg.brightness != last_brightness ||
                g_cfg.hour24 != last_hour24) {
                draw_settings();
                last_settings = g_settings; last_brightness = g_cfg.brightness; last_hour24 = g_cfg.hour24;
            }
        } else {
            time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
            int hour = tm.tm_hour;
            if (!g_cfg.hour24) { hour %= 12; if (hour == 0) hour = 12; }
            char clock[16], date[40];
            snprintf(clock, sizeof(clock), "%02d:%02d", hour, tm.tm_min);
            snprintf(date, sizeof(date), "%d-%02d-%02d", tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday);
            if (strcmp(clock, last_clock) != 0 || g_settings != last_settings) {
                /* 背景渐变 */
                for (int sy = 0; sy < SCR_H; sy++) {
                    u16 c = rgb565(14, 18 + sy/14, 34 + sy/7);
                    for (int sx = 0; sx < SCR_W; sx++) px(sx, sy, c);
                }
                /* 大时钟(居中) */
                int scale = 24;
                int cw = text_width(clock, scale);
                int cy = (SCR_H - 7*scale)/2 - 20;
                draw_text(clock, (SCR_W - cw)/2, cy, scale, rgb565(255,255,255));
                /* 日期 */
                int dw = text_width(date, 6);
                draw_text(date, (SCR_W - dw)/2, cy + 7*scale + 18, 6, rgb565(180,200,255));
                /* 右上角设置按钮 */
                fill_rect(SCR_W - 180, 12, 160, 70, rgb565(255,140,0));
                draw_text("SET", SCR_W - 150, 32, 5, rgb565(0,0,0));
                strcpy(last_clock, clock);
                last_settings = g_settings;
            }
        }
        usleep(30000);
    }
    return 0;
}
