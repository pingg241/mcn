/* PC 宿主版：用真实 UDP 套接字跑 app/ 全部代码。笔记本代替 STM32 当终端时用它：
 * 开一个窗口显示 480×320 界面（Windows 用 Win32，Linux/WSL 用 X11），空格/回车/鼠标左键当 KEY；
 * 控制台命令可以直接在本程序的命令行窗口里敲（UDP 6000 照样能用）；也可无窗口运行并导出 BMP。
 *
 *   mca_host [--fpga IP] [--local IP] [--seconds N] [--bmp 前缀] [--nv 目录] [--key-at ms[,ms...]] [--no-window]
 *
 * 本机端口 5000/5001/6000 绑定在 --local 地址上，默认 0.0.0.0（所有网卡）；与 tools/mca_tool.py fake-fpga
 * 配合时假 FPGA 用 127.0.0.2，宿主用 127.0.0.1。 */
#ifndef _WIN32
#define _DEFAULT_SOURCE                      /* -std=c11 下 glibc 默认不声明 clock_gettime、select 等 POSIX 接口 */
#endif
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET sock_t;
#define CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
typedef int sock_t;
#define INVALID_SOCKET (-1)
#define CLOSESOCK close
#ifdef MCA_HAVE_X11
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#endif
#endif

#if defined(_WIN32) || defined(MCA_HAVE_X11)
#define HAVE_WINDOW 1
#endif

#include "app.h"
#include "console.h"
#include "platform.h"
#include "settings.h"
#include "ui.h"

#define NV_SLOTS     8
#define NV_SLOT_SIZE (128u * 1024u)

static const uint16_t ports[3] = {5000, 5001, 6000};
static sock_t socks[3];
static uint32_t local_ip;                 /* 0 = 0.0.0.0 */
static uint16_t fb[LCD_W * LCD_H];
static unsigned flushes;
static const char *nv_dir = "host_nv";
static uint32_t key_at[16];
static int n_key_at;
static int win_key, win_dirty;

/* ---------------- 平台接口 ---------------- */
uint32_t plat_now_ms(void)
{
#ifdef _WIN32
    static ULONGLONG t0;
    if (!t0) t0 = GetTickCount64();
    return (uint32_t)(GetTickCount64() - t0);
#else
    static struct timespec t0;
    struct timespec t;
    if (!t0.tv_sec) clock_gettime(CLOCK_MONOTONIC, &t0);
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
#endif
}

int plat_udp_send(uint16_t local_port, uint32_t dst_ip, uint16_t dst_port, const void *data, size_t len)
{
    for (int i = 0; i < 3; i++) {
        if (ports[i] != local_port) continue;
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(dst_port);
        a.sin_addr.s_addr = htonl(dst_ip);
        return sendto(socks[i], (const char *)data, (int)len, 0, (struct sockaddr *)&a, sizeof(a)) == (int)len ? 0 : -1;
    }
    return -1;
}

int plat_link_speed(void) { return 100; }

uint32_t plat_local_ip(void)
{
    if (local_ip) return local_ip;
    /* 绑在 0.0.0.0 上时，显示通往 FPGA 的那块网卡的地址（UDP connect 只查路由，不发包）；网线可能后插，2 s 重查 */
    static uint32_t cached, for_ip, t_last;
    uint32_t now = plat_now_ms();
    if (for_ip != g_set.fpga_ip || now - t_last >= 2000 || !t_last) {
        for_ip = g_set.fpga_ip;
        t_last = now | 1u;
        cached = 0;
        sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s == INVALID_SOCKET) return 0;
        struct sockaddr_in a;
        socklen_t l = sizeof(a);
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(ports[0]);
        a.sin_addr.s_addr = htonl(for_ip);
        if (connect(s, (struct sockaddr *)&a, sizeof(a)) == 0 && getsockname(s, (struct sockaddr *)&a, &l) == 0)
            cached = ntohl(a.sin_addr.s_addr);
        CLOSESOCK(s);
    }
    return cached;
}

uint16_t *plat_framebuffer(void) { return fb; }
void plat_lcd_flush(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; flushes++; win_dirty = 1; }
int plat_lcd_busy(void) { return 0; }

/* 窗口里按住的键，加上 --key-at 指定的时刻各按一次（按住 100 ms） */
uint32_t plat_keys(void)
{
    uint32_t now = plat_now_ms();
    for (int i = 0; i < n_key_at; i++)
        if (now >= key_at[i] && now < key_at[i] + 100) return 1;
    return win_key ? 1u : 0u;
}
void plat_led(int on) { (void)on; }

static void nv_path(char *out, size_t n, unsigned slot) { snprintf(out, n, "%s/nv_slot%u.bin", nv_dir, slot); }

unsigned plat_nv_slots(void) { return NV_SLOTS; }
size_t plat_nv_slot_size(void) { return NV_SLOT_SIZE; }

int plat_nv_erase(unsigned slot)
{
    char path[512];
    static uint8_t ff[NV_SLOT_SIZE];
    if (slot >= NV_SLOTS) return -1;
    memset(ff, 0xFF, sizeof(ff));
    nv_path(path, sizeof(path), slot);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(ff, 1, sizeof(ff), f);
    fclose(f);
    return w == sizeof(ff) ? 0 : -1;
}

int plat_nv_write(unsigned slot, size_t off, const void *data, size_t len)
{
    char path[512];
    if (slot >= NV_SLOTS || off % 32 || off + len > NV_SLOT_SIZE) return -1;
    nv_path(path, sizeof(path), slot);
    FILE *f = fopen(path, "r+b");
    if (!f) return -1;
    fseek(f, (long)off, SEEK_SET);
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    return w == len ? 0 : -1;
}

int plat_nv_read(unsigned slot, size_t off, void *data, size_t len)
{
    char path[512];
    if (slot >= NV_SLOTS || off + len > NV_SLOT_SIZE) return -1;
    nv_path(path, sizeof(path), slot);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, (long)off, SEEK_SET);
    size_t r = fread(data, 1, len, f);
    fclose(f);
    return r == len ? 0 : -1;
}

void plat_log(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("[%7.3f] %s", plat_now_ms() / 1000.0, buf);
    fflush(stdout);
    console_log(buf);
}

/* ---------------- BMP ---------------- */
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }

static int write_bmp(const char *path)
{
    const uint32_t row = LCD_W * 3, size = 54 + row * LCD_H;
    uint8_t h[54] = {'B', 'M'};
    put32(h + 2, size);
    put32(h + 10, 54);
    put32(h + 14, 40);
    put32(h + 18, LCD_W);
    put32(h + 22, LCD_H);
    put16(h + 26, 1);
    put16(h + 28, 24);
    put32(h + 34, row * LCD_H);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(h, 1, sizeof(h), f);
    static uint8_t line[LCD_W * 3];
    for (int y = LCD_H - 1; y >= 0; y--) {
        for (int x = 0; x < LCD_W; x++) {
            uint16_t c = fb[y * LCD_W + x];
            uint8_t r = (uint8_t)((c >> 11) << 3), g = (uint8_t)(((c >> 5) & 63) << 2), b = (uint8_t)((c & 31) << 3);
            line[x * 3] = (uint8_t)(b | b >> 5);
            line[x * 3 + 1] = (uint8_t)(g | g >> 6);
            line[x * 3 + 2] = (uint8_t)(r | r >> 5);
        }
        fwrite(line, 1, sizeof(line), f);
    }
    fclose(f);
    return 0;
}

/* ---------------- 窗口（Windows：Win32） ---------------- */
#if defined(_WIN32)
static HWND win;
static int win_closed;

/* 帧缓冲本身就是 RGB565，用 BI_BITFIELDS 直接贴；窗口拉伸时保持 3:2，四周补黑边 */
static void win_paint(HDC dc)
{
    RECT rc;
    GetClientRect(win, &rc);
    int cw = rc.right, ch = rc.bottom;
    int w = cw, h = cw * LCD_H / LCD_W;
    if (h > ch) { h = ch; w = ch * LCD_W / LCD_H; }
    int x0 = (cw - w) / 2, y0 = (ch - h) / 2;
    struct {
        BITMAPINFOHEADER h;
        DWORD mask[3];
    } bi;
    memset(&bi, 0, sizeof(bi));
    bi.h.biSize = sizeof(bi.h);
    bi.h.biWidth = LCD_W;
    bi.h.biHeight = -LCD_H;                  /* 负数：自上而下 */
    bi.h.biPlanes = 1;
    bi.h.biBitCount = 16;
    bi.h.biCompression = BI_BITFIELDS;
    bi.mask[0] = 0xF800;
    bi.mask[1] = 0x07E0;
    bi.mask[2] = 0x001F;
    /* 整数倍放大用最近邻，字更清楚；非整数倍用 HALFTONE 平滑 */
    if (w % LCD_W) {
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, NULL);
    } else {
        SetStretchBltMode(dc, COLORONCOLOR);
    }
    StretchDIBits(dc, x0, y0, w, h, 0, 0, LCD_W, LCD_H, fb, (BITMAPINFO *)&bi, DIB_RGB_COLORS, SRCCOPY);
    HBRUSH k = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RECT r[4] = {{0, 0, cw, y0}, {0, y0 + h, cw, ch}, {0, y0, x0, y0 + h}, {x0 + w, y0, cw, y0 + h}};
    for (int i = 0; i < 4; i++)
        if (r[i].right > r[i].left && r[i].bottom > r[i].top) FillRect(dc, &r[i], k);
}

static LRESULT CALLBACK win_proc(HWND hw, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_KEYDOWN:
        if (wp == VK_SPACE || wp == VK_RETURN) win_key = 1;
        return 0;
    case WM_KEYUP:
        if (wp == VK_SPACE || wp == VK_RETURN) win_key = 0;
        return 0;
    case WM_LBUTTONDOWN:
        SetCapture(hw);
        win_key = 1;
        return 0;
    case WM_LBUTTONUP:
        ReleaseCapture();
        win_key = 0;
        return 0;
    case WM_KILLFOCUS:
        win_key = 0;                         /* 按住时切走窗口，别卡成长按 */
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hw, &ps);
        win_paint(dc);
        EndPaint(hw, &ps);
        return 0;
    }
    case WM_DESTROY:
        win_closed = 1;
        return 0;
    }
    return DefWindowProcW(hw, m, wp, lp);
}

static int win_open(void)
{
    SetProcessDPIAware();                    /* 高分屏上按物理像素放大，不让系统再糊一遍 */
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = win_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.lpszClassName = L"mca_host";
    if (!RegisterClassW(&wc)) return -1;
    /* 取放得下工作区（去掉任务栏）的最大整数倍，居中摆放 */
    const DWORD style = WS_OVERLAPPEDWINDOW;
    RECT wa, r;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    for (int s = 4; s >= 1; s--) {
        r = (RECT){0, 0, LCD_W * s, LCD_H * s};
        AdjustWindowRect(&r, style, FALSE);
        if (s == 1 || (r.right - r.left <= wa.right - wa.left && r.bottom - r.top <= wa.bottom - wa.top)) break;
    }
    int ww = r.right - r.left, wh = r.bottom - r.top;
    int x = wa.left + (wa.right - wa.left - ww) / 2, y = wa.top + (wa.bottom - wa.top - wh) / 2;
    win = CreateWindowW(L"mca_host", L"MCA 终端 — 空格 / 回车 / 鼠标左键 = KEY：短按切页，长按 1 s 清谱（波形页换信号源）",
                        style, x > wa.left ? x : wa.left, y > wa.top ? y : wa.top, ww, wh, NULL, NULL, wc.hInstance, NULL);
    if (!win) return -1;
    ShowWindow(win, SW_SHOWNORMAL);
    return 0;
}

/* 主循环每圈调一次：处理窗口消息，界面有刷新就重画；窗口关了返回 1 */
static int win_service(void)
{
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (win_dirty) {
        win_dirty = 0;
        InvalidateRect(win, NULL, FALSE);
    }
    return win_closed;
}

/* ---------------- 窗口（Linux / WSLg：X11） ---------------- */
#elif defined(MCA_HAVE_X11)
static Display *dpy;
static Window xw;
static Atom wm_delete;
static XImage *img;
static int *xmap;                            /* 窗口列 → 帧缓冲列 */
static int win_closed;
static uint32_t t_paint;

/* RGB565 → 窗口大小的 32 位图再 XPutImage。能放大整数倍就取整数倍（字清楚），保持 3:2，四周黑边 */
static void win_paint(void)
{
    XWindowAttributes wa;
    XGetWindowAttributes(dpy, xw, &wa);
    int cw = wa.width, ch = wa.height;
    if (cw <= 0 || ch <= 0) return;
    if (!img || img->width != cw || img->height != ch) {
        if (img) XDestroyImage(img);         /* 连同 data 一起 free */
        img = NULL;
        free(xmap);
        xmap = malloc((size_t)cw * sizeof(int));
        char *px = malloc((size_t)cw * (size_t)ch * 4u);
        if (!xmap || !px) { free(px); return; }
        img = XCreateImage(dpy, wa.visual, (unsigned)wa.depth, ZPixmap, 0, px, (unsigned)cw, (unsigned)ch, 32, 0);
        if (!img) { free(px); return; }
    }
    double s = (double)cw / LCD_W < (double)ch / LCD_H ? (double)cw / LCD_W : (double)ch / LCD_H;
    if (s >= 1) s = (int)s;
    int w = (int)(LCD_W * s), h = (int)(LCD_H * s);
    int x0 = (cw - w) / 2, y0 = (ch - h) / 2;
    for (int x = 0; x < w; x++) xmap[x] = x * LCD_W / w;
    /* 常见的 24/32 位 TrueColor（0x00RRGGBB）直接写；别的格式走 XPutPixel */
    int fast = img->bits_per_pixel == 32 && img->red_mask == 0xFF0000 && img->green_mask == 0xFF00 &&
               img->blue_mask == 0xFF && img->byte_order == LSBFirst;
    memset(img->data, 0, (size_t)img->bytes_per_line * (size_t)ch);
    for (int y = 0; y < h; y++) {
        const uint16_t *src = fb + (size_t)(y * LCD_H / h) * LCD_W;
        uint32_t *dst = (uint32_t *)(img->data + (size_t)(y0 + y) * (size_t)img->bytes_per_line) + x0;
        for (int x = 0; x < w; x++) {
            uint16_t c = src[xmap[x]];
            uint32_t r = (c >> 11) << 3, g = ((c >> 5) & 63) << 2, b = (c & 31) << 3;
            uint32_t v = (r | r >> 5) << 16 | (g | g >> 6) << 8 | (b | b >> 5);
            if (fast) dst[x] = v;
            else XPutPixel(img, x0 + x, y0 + y, v);
        }
    }
    XPutImage(dpy, xw, DefaultGC(dpy, DefaultScreen(dpy)), img, 0, 0, 0, 0, (unsigned)cw, (unsigned)ch);
    XFlush(dpy);
}

static int win_open(void)
{
    static const char title[] = "MCA 终端 — 空格 / 回车 / 鼠标左键 = KEY：短按切页，长按 1 s 清谱（波形页换信号源）";
    dpy = XOpenDisplay(NULL);
    if (!dpy) return -1;
    int scr = DefaultScreen(dpy);
    /* 取放得下的最大整数倍。桌面给了 _NET_WORKAREA（GNOME、KDE 等）就按工作区，只留标题栏；
     * WSLg 没有，按整屏扣掉实测的 WSLg 边框（宽 76、高 97）和 Windows 任务栏 */
    int sw = DisplayWidth(dpy, scr), sh = DisplayHeight(dpy, scr), mw = 100, mh = 170, s = 4;
    Atom wa_atom = XInternAtom(dpy, "_NET_WORKAREA", True), type;
    int fmt;
    unsigned long n, after;
    unsigned char *prop = NULL;
    if (wa_atom != None &&
        XGetWindowProperty(dpy, RootWindow(dpy, scr), wa_atom, 0, 4, False, AnyPropertyType, &type, &fmt, &n, &after,
                           &prop) == Success && prop && fmt == 32 && n == 4) {
        sw = (int)((long *)prop)[2];
        sh = (int)((long *)prop)[3];
        mw = 20;
        mh = 50;
    }
    if (prop) XFree(prop);
    while (s > 1 && (LCD_W * s + mw > sw || LCD_H * s + mh > sh)) s--;
    xw = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, 0, (unsigned)(LCD_W * s), (unsigned)(LCD_H * s), 0,
                             BlackPixel(dpy, scr), BlackPixel(dpy, scr));
    XStoreName(dpy, xw, "MCA terminal");
    XChangeProperty(dpy, xw, XInternAtom(dpy, "_NET_WM_NAME", False), XInternAtom(dpy, "UTF8_STRING", False), 8,
                    PropModeReplace, (const unsigned char *)title, (int)strlen(title));
    XClassHint cls = {(char *)"mca_host", (char *)"mca_host"};
    XSetClassHint(dpy, xw, &cls);
    wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, xw, &wm_delete, 1);
    XSelectInput(dpy, xw, ExposureMask | StructureNotifyMask | KeyPressMask | KeyReleaseMask | ButtonPressMask |
                              ButtonReleaseMask | FocusChangeMask);
    Bool ok;
    XkbSetDetectableAutoRepeat(dpy, True, &ok); /* 按住不放时，自动重复别插进假的"松开"，否则长按会变成连按 */
    XMapWindow(dpy, xw);
    XFlush(dpy);
    return 0;
}

static int win_service(void)
{
    while (XPending(dpy)) {
        XEvent e;
        XNextEvent(dpy, &e);
        switch (e.type) {
        case Expose:
        case ConfigureNotify:
            win_dirty = 1;
            break;
        case KeyPress:
        case KeyRelease: {
            KeySym k = XLookupKeysym(&e.xkey, 0);
            if (k == XK_space || k == XK_Return || k == XK_KP_Enter) win_key = e.type == KeyPress;
            break;
        }
        case ButtonPress:
        case ButtonRelease:
            if (e.xbutton.button == Button1) win_key = e.type == ButtonPress;
            break;
        case FocusOut:
            win_key = 0;
            break;
        case ClientMessage:
            if ((Atom)e.xclient.data.l[0] == wm_delete) win_closed = 1;
            break;
        }
    }
    /* 界面刷新很密时限到约 30 帧/s，整窗 XPutImage 不便宜 */
    uint32_t now = plat_now_ms();
    if (win_dirty && now - t_paint >= 33) {
        win_dirty = 0;
        t_paint = now;
        win_paint();
    }
    return win_closed;
}
#endif

/* ---------------- 命令行窗口里直接敲控制台命令 ---------------- */
static int want_quit;

static void con_out(const char *s, size_t n)
{
    fwrite(s, 1, n, stdout);
    fflush(stdout);
}

/* 一行输入：quit / exit 退出，其余交给控制台 */
static void stdin_line(char *line)
{
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    size_t n = strcspn(p, "\r\n");
    p[n] = 0;
    if (!strcmp(p, "quit") || !strcmp(p, "exit")) want_quit = 1;
    else if (*p) console_line(p, con_out);
}

#ifdef _WIN32
/* select 管不了控制台句柄，开个线程读，一次交一行给主循环 */
static char in_line[256];
static volatile LONG in_ready;

static DWORD WINAPI stdin_thread(LPVOID arg)
{
    (void)arg;
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    int console = GetConsoleMode(h, &mode);
    char buf[sizeof(in_line)];
    for (;;) {
        if (console) {                       /* ReadConsoleW + 转 UTF-8，存谱名写中文也不乱 */
            wchar_t w[200];
            DWORD n = 0;
            if (!ReadConsoleW(h, w, 199, &n, NULL) || n == 0) return 0;
            int k = WideCharToMultiByte(CP_UTF8, 0, w, (int)n, buf, (int)sizeof(buf) - 1, NULL, NULL);
            buf[k > 0 ? k : 0] = 0;
        } else if (!fgets(buf, sizeof(buf), stdin)) {
            return 0;
        }
        while (in_ready) Sleep(1);           /* 等主循环取走上一行 */
        memcpy(in_line, buf, sizeof(buf));
        InterlockedExchange(&in_ready, 1);
    }
}

static void stdin_start(void) { CloseHandle(CreateThread(NULL, 0, stdin_thread, NULL, 0, NULL)); }

static void stdin_service(void)
{
    if (!in_ready) return;
    stdin_line(in_line);
    InterlockedExchange(&in_ready, 0);
}
#else
/* 标准输入直接进 select。后台运行（mca_host &）时读终端会被 SIGTTIN 挂起，忽略它，读出错就不再读 */
static int stdin_on = 1;
static char in_buf[256];
static size_t in_len;

static void stdin_start(void) { signal(SIGTTIN, SIG_IGN); }

static void stdin_read(void)
{
    ssize_t n = read(STDIN_FILENO, in_buf + in_len, sizeof(in_buf) - 1 - in_len);
    if (n <= 0) { stdin_on = 0; return; }
    in_len += (size_t)n;
    in_buf[in_len] = 0;
    char *nl;
    while ((nl = strchr(in_buf, '\n')) != NULL) {
        *nl = 0;
        stdin_line(in_buf);
        in_len -= (size_t)(nl + 1 - in_buf);
        memmove(in_buf, nl + 1, in_len + 1);
    }
    if (in_len == sizeof(in_buf) - 1) in_len = 0; /* 一行太长，丢掉 */
}
#endif

/* ---------------- 主循环 ---------------- */
static uint32_t parse_ip(const char *s)
{
    unsigned a, b, c, d;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        fprintf(stderr, "bad ip %s\n", s);
        exit(2);
    }
    return (a << 24) | (b << 16) | (c << 8) | d;
}

static void usage(void)
{
    fprintf(stderr, "usage: mca_host [--fpga IP] [--local IP] [--seconds N] [--bmp PREFIX] [--nv DIR] "
                    "[--key-at MS[,MS...]] [--no-window]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *fpga = 0, *bmp = 0;
    double seconds = 0;
    int want_window = 1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--no-window")) { want_window = 0; continue; }
        if (i + 1 >= argc) usage();
        if (!strcmp(a, "--fpga")) fpga = argv[++i];
        else if (!strcmp(a, "--local")) local_ip = parse_ip(argv[++i]);
        else if (!strcmp(a, "--seconds")) seconds = atof(argv[++i]);
        else if (!strcmp(a, "--bmp")) bmp = argv[++i];
        else if (!strcmp(a, "--nv")) nv_dir = argv[++i];
        else if (!strcmp(a, "--key-at")) {
            for (char *p = argv[++i]; *p && n_key_at < 16;) {
                key_at[n_key_at++] = (uint32_t)strtoul(p, &p, 10);
                if (*p == ',') p++;
            }
        } else usage();
    }
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    CreateDirectoryA(nv_dir, NULL);
#else
    mkdir(nv_dir, 0755);
#endif
    for (int i = 0; i < 3; i++) {
        socks[i] = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(ports[i]);
        a.sin_addr.s_addr = htonl(local_ip);
        int rcvbuf = 4 << 20;
        setsockopt(socks[i], SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof(rcvbuf));
#ifdef SO_RCVBUFFORCE
        setsockopt(socks[i], SOL_SOCKET, SO_RCVBUFFORCE, &rcvbuf, sizeof(rcvbuf)); /* root 可越过 rmem_max，失败无妨 */
#endif
        if (socks[i] == INVALID_SOCKET || bind(socks[i], (struct sockaddr *)&a, sizeof(a)) != 0) {
            fprintf(stderr, "bind %u.%u.%u.%u:%u failed（端口被占用？是否已经开着一个 mca_host 或 mca_tool）\n",
                    (unsigned)(local_ip >> 24), (unsigned)(local_ip >> 16) & 255, (unsigned)(local_ip >> 8) & 255,
                    (unsigned)local_ip & 255, ports[i]);
            return 1;
        }
    }
#ifndef _WIN32
    /* Linux 的接收缓冲受 net.core.rmem_max 限制（常见只有 208 KB），高计数率时主循环稍一卡就丢包 */
    int got = 0;
    socklen_t gl = sizeof(got);
    if (getsockopt(socks[1], SOL_SOCKET, SO_RCVBUF, &got, &gl) == 0 && got < (1 << 20))
        fprintf(stderr, "UDP 5001 接收缓冲只有 %d KB，高计数率可能丢包；可执行 sudo sysctl -w net.core.rmem_max=4194304\n",
                got / 1024);
#endif

    app_init();
    if (fpga) {
        g_set.fpga_ip = parse_ip(fpga);
        app_setting_changed("fpga_ip");
    }
    uint32_t me = plat_local_ip();
    g_set.mcu_ip = me;
    plat_log("host: 本机 %u.%u.%u.%u（UDP 5000/5001/6000），FPGA %u.%u.%u.%u\n", (unsigned)(me >> 24),
             (unsigned)(me >> 16) & 255, (unsigned)(me >> 8) & 255, (unsigned)me & 255, (unsigned)(g_set.fpga_ip >> 24),
             (unsigned)(g_set.fpga_ip >> 16) & 255, (unsigned)(g_set.fpga_ip >> 8) & 255, (unsigned)g_set.fpga_ip & 255);
#ifdef HAVE_WINDOW
    if (want_window && win_open()) {
        fprintf(stderr, "窗口创建失败（Linux 下检查 DISPLAY），改为无窗口运行\n");
        want_window = 0;
    }
#else
    if (want_window) fprintf(stderr, "编译时没找到 libX11，无窗口运行（装 libx11-dev 后重新 xmake f）\n");
    want_window = 0;
#endif
    stdin_start();
    printf("在这里直接输入命令、回车执行（help 列出全部命令，quit 退出），例如：set test_src 0、status、peaks\n");
    fflush(stdout);

    static uint8_t buf[2048];
    while (!want_quit) {
        fd_set rd;
        FD_ZERO(&rd);
        sock_t maxfd = 0;
        for (int i = 0; i < 3; i++) {
            FD_SET(socks[i], &rd);
            if (socks[i] > maxfd) maxfd = socks[i];
        }
#ifndef _WIN32
        if (stdin_on) FD_SET(STDIN_FILENO, &rd);
#endif
        struct timeval tv = {0, 1000};
        if (select((int)maxfd + 1, &rd, 0, 0, &tv) > 0) {
            for (int i = 0; i < 3; i++) {
                if (!FD_ISSET(socks[i], &rd)) continue;
                struct sockaddr_in from;
                socklen_t fl = sizeof(from);
                int n = recvfrom(socks[i], (char *)buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
                if (n > 0) app_udp_rx(ports[i], ntohl(from.sin_addr.s_addr), ntohs(from.sin_port), buf, (size_t)n);
            }
#ifndef _WIN32
            if (stdin_on && FD_ISSET(STDIN_FILENO, &rd)) stdin_read();
#endif
        }
#ifdef _WIN32
        stdin_service();
#endif
        app_poll();
#ifdef HAVE_WINDOW
        if (want_window && win_service()) break;
#endif
        if (seconds > 0 && plat_now_ms() >= seconds * 1000) break;
    }

    if (bmp) {
        static const char *const names[UI_NSCREENS] = {"spectrum", "wave", "stats"};
        char path[512];
        for (int s = 0; s < UI_NSCREENS; s++) {
            ui_set_screen(s);
            ui_draw();
            snprintf(path, sizeof(path), "%s_%s.bmp", bmp, names[s]);
            if (write_bmp(path) == 0) printf("写出 %s\n", path);
        }
    }
    printf("帧刷新 %u 次\n", flushes);
    for (int i = 0; i < 3; i++) CLOSESOCK(socks[i]);
    return 0;
}
