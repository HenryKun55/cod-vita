/* user32.dll + gdi32.dll: one top-level window driven by the platform layer. */
#include "platform.h"
#include "shim.h"

#include <stdio.h>
#include <stdlib.h>

#include <strings.h>

#define HWND_MAIN 0x00010010u
#define HDC_FAKE 0x00020020u
#define HICON_FAKE 0x00030030u
#define HCURSOR_FAKE 0x00030040u
/* Windows that are not the game's window (system control classes, the dedicated
 * console): handles that exist for the game but get no messages, no platform
 * window and are never shown. */
#define HWND_VIRT_BASE 0x00040000u
#define IS_VIRT(h) ((h) >= HWND_VIRT_BASE && (h) < HWND_VIRT_BASE + 0x10000u)
static uint32_t virt_count;
static void show_main(Ctx *c, int show);

/* Case-insensitive substring test (newlib has no strcasestr). */
static int contains_ci(const char *s, const char *sub)
{
    size_t n = strlen(sub);
    for (; *s; s++)
        if (strncasecmp(s, sub, n) == 0)
            return 1;
    return 0;
}

#define WM_CREATE 0x0001u
#define WM_DESTROY 0x0002u
#define WM_SIZE 0x0005u
#define WM_ACTIVATE 0x0006u
#define WM_SETFOCUS 0x0007u
#define WM_CLOSE 0x0010u
#define WM_QUIT 0x0012u
#define WM_SHOWWINDOW 0x0018u
#define WM_ACTIVATEAPP 0x001Cu
#define WM_NCCREATE 0x0081u

typedef struct WndClass {
    char name[64];
    uint32_t wndproc;
} WndClass;

static WndClass classes[16];

static struct {
    int exists;
    int visible, activated;
    uint32_t wndproc;
    uint32_t style, exstyle;
    int32_t x, y, w, h;
} wnd;

/* Posted message queue. */
typedef struct Msg {
    uint32_t hwnd, message, wparam, lparam, time;
} Msg;

#define MSGQ 512
static Msg msgq[MSGQ];
static unsigned msg_head, msg_tail;
static int quit_posted;
static uint32_t quit_code;

void user32_post_message(uint32_t message, uint32_t wparam, uint32_t lparam)
{
    /* Mouse messages carry the state of every button in wParam (MK_LBUTTON...):
     * the game reads the buttons from there, not from the message kind. */
    if (message >= 0x0200u && message <= 0x020Du) {
        uint32_t mk = (g_mouse.buttons[0] ? 0x01u : 0u) | (g_mouse.buttons[1] ? 0x02u : 0u) |
                      (g_mouse.buttons[2] ? 0x10u : 0u);
        wparam = (wparam & 0xFFFF0000u) | mk;
    }
    unsigned next = (msg_tail + 1u) % MSGQ;
    if (next == msg_head)
        return;
    msgq[msg_tail].hwnd = HWND_MAIN;
    msgq[msg_tail].message = message;
    msgq[msg_tail].wparam = wparam;
    msgq[msg_tail].lparam = lparam;
    msgq[msg_tail].time = win32_tick_count();
    msg_tail = next;
}

static uint32_t send_message(Ctx *c, uint32_t message, uint32_t wparam, uint32_t lparam)
{
    if (!wnd.exists || !wnd.wndproc)
        return 0;
    uint32_t args[4] = { HWND_MAIN, message, wparam, lparam };
    return guest_call(c, wnd.wndproc, 4, args);
}

static int get_message(uint32_t out, uint32_t remove)
{
    if (msg_head == msg_tail) {
        if (!quit_posted)
            return 0;
        WR32(out + 0u, 0);
        WR32(out + 4u, WM_QUIT);
        WR32(out + 8u, quit_code);
        WR32(out + 12u, 0);
        WR32(out + 16u, win32_tick_count());
        WR32(out + 20u, 0);
        WR32(out + 24u, 0);
        if (remove)
            quit_posted = 0;
        return 1;
    }
    Msg *m = &msgq[msg_head];
    WR32(out + 0u, m->hwnd);
    WR32(out + 4u, m->message);
    WR32(out + 8u, m->wparam);
    WR32(out + 12u, m->lparam);
    WR32(out + 16u, m->time);
    WR32(out + 20u, (uint32_t)g_mouse.x);
    WR32(out + 24u, (uint32_t)g_mouse.y);
    if (remove)
        msg_head = (msg_head + 1u) % MSGQ;
    return 1;
}

SHIM(user32, RegisterClassA)
{
    uint32_t wc = ARG(0);
    const char *name = gstr(RD32(wc + 36u));
    for (int i = 0; i < 16; i++) {
        if (!classes[i].name[0]) {
            snprintf(classes[i].name, sizeof(classes[i].name), "%s", name);
            classes[i].wndproc = RD32(wc + 4u);
            rt_log("RegisterClassA(%s) wndproc=%08X", name, classes[i].wndproc);
            stdcall_ret(c, 0xC000u + (uint32_t)i, 1);
            return;
        }
    }
    stdcall_ret(c, 0, 1);
}

SHIM(user32, UnregisterClassA) { stdcall_ret(c, 1, 2); }

SHIM(user32, CreateWindowExA)
{
    uint32_t exstyle = ARG(0), cls = ARG(1), title = ARG(2), style = ARG(3);
    int32_t x = (int32_t)ARG(4), y = (int32_t)ARG(5), w = (int32_t)ARG(6), h = (int32_t)ARG(7);
    const char *cname = cls >= 0x10000u ? gstr(cls) : NULL;
    uint32_t proc = 0;
    for (int i = 0; i < 16; i++) {
        if ((cname && strcasecmp(classes[i].name, cname) == 0) || (!cname && cls == 0xC000u + (uint32_t)i))
            proc = classes[i].wndproc;
    }
    /* The dedicated console and the start-up splash are not shown: the game window
     * is the one SDL window. */
    if (!proc || (cname && (contains_ci(cname, "console") || contains_ci(cname, "splash")))) {
        uint32_t hv = HWND_VIRT_BASE + ++virt_count;
        rt_log("CreateWindowExA(%s, \"%s\") -> %08X (not the game window)", cname ? cname : "#atom",
               title ? gstr(title) : "", hv);
        stdcall_ret(c, hv, 12);
        return;
    }
    if (w <= 0 || (uint32_t)w == 0x80000000u)
        w = 640;
    if (h <= 0 || (uint32_t)h == 0x80000000u)
        h = 480;
    rt_log("CreateWindowExA(%s, \"%s\", style %08X, %dx%d)", cname ? cname : "#atom", title ? gstr(title) : "",
           style, w, h);
    wnd.exists = 1;
    wnd.visible = 0;
    wnd.activated = 0;
    wnd.wndproc = proc;
    wnd.style = style;
    wnd.exstyle = exstyle;
    wnd.x = x;
    wnd.y = y;
    wnd.w = w;
    wnd.h = h;
    platform_create_window(w, h, title ? gstr(title) : "Call of Duty");

    /* WM_NCCREATE / WM_CREATE with a CREATESTRUCTA. */
    uint32_t cs = heap_alloc(48);
    WR32(cs + 0u, ARG(11));
    WR32(cs + 4u, ARG(10));
    WR32(cs + 8u, ARG(9));
    WR32(cs + 12u, ARG(8));
    WR32(cs + 16u, (uint32_t)h);
    WR32(cs + 20u, (uint32_t)w);
    WR32(cs + 24u, (uint32_t)y);
    WR32(cs + 28u, (uint32_t)x);
    WR32(cs + 32u, style);
    WR32(cs + 36u, title);
    WR32(cs + 40u, cls);
    WR32(cs + 44u, exstyle);
    send_message(c, WM_NCCREATE, 0, cs);
    uint32_t r = send_message(c, WM_CREATE, 0, cs);
    heap_free(cs);
    if (r == 0xFFFFFFFFu) {
        wnd.exists = 0;
        stdcall_ret(c, 0, 12);
        return;
    }
    if (style & 0x10000000u)   /* WS_VISIBLE */
        show_main(c, 1);
    stdcall_ret(c, HWND_MAIN, 12);
}

void user32_window_size(int *w, int *h)
{
    *w = wnd.w > 0 ? wnd.w : 640;
    *h = wnd.h > 0 ? wnd.h : 480;
}

SHIM(user32, DestroyWindow)
{
    if (IS_VIRT(ARG(0))) {
        stdcall_ret(c, 0, 1);
        return;
    }
    send_message(c, WM_DESTROY, 0, 0);
    wnd.exists = 0;
    stdcall_ret(c, 1, 1);
}

/* Messages go out only when the visibility changes, as in Windows: the game's
 * window procedure calls ShowWindow while handling them. */
/* The game window becomes visible (ShowWindow, or created with WS_VISIBLE). */
static void show_main(Ctx *c, int show)
{
    wnd.visible = show;
    platform_show_window(show);
    send_message(c, WM_SHOWWINDOW, (uint32_t)show, 0);
    if (show && !wnd.activated) {
        wnd.activated = 1;
        send_message(c, WM_ACTIVATEAPP, 1, 0);
        send_message(c, WM_ACTIVATE, 1, 0);
        send_message(c, WM_SETFOCUS, 0, 0);
        send_message(c, WM_SIZE, 0, ((uint32_t)wnd.h << 16) | (uint32_t)wnd.w);
    }
}

SHIM(user32, ShowWindow)
{
    if (IS_VIRT(ARG(0))) {
        stdcall_ret(c, 0, 2);
        return;
    }
    uint32_t cmd = ARG(1);
    int show = cmd != 0;
    int was = wnd.visible;
    if (show != was)
        show_main(c, show);
    stdcall_ret(c, (uint32_t)was, 2);
}

SHIM(user32, UpdateWindow) { stdcall_ret(c, 1, 1); }
SHIM(user32, InvalidateRect) { stdcall_ret(c, 1, 3); }
SHIM(user32, SetFocus) { stdcall_ret(c, HWND_MAIN, 1); }
SHIM(user32, SetForegroundWindow) { stdcall_ret(c, 1, 1); }
SHIM(user32, GetForegroundWindow) { stdcall_ret(c, HWND_MAIN, 0); }

SHIM(user32, SetWindowTextA)
{
    if (IS_VIRT(ARG(0))) {
        stdcall_ret(c, 0, 2);
        return;
    }
    platform_set_title(gstr(ARG(1)));
    stdcall_ret(c, 1, 2);
}

SHIM(user32, SetWindowPos)
{
    if (IS_VIRT(ARG(0))) {
        stdcall_ret(c, 0, 7);
        return;
    }
    uint32_t flags = ARG(6);
    if (!(flags & 0x1u)) {    /* !SWP_NOSIZE */
        wnd.w = (int32_t)ARG(4);
        wnd.h = (int32_t)ARG(5);
    }
    stdcall_ret(c, 1, 7);
}

SHIM(user32, GetWindowLongA)
{
    if (IS_VIRT(ARG(0))) {
        stdcall_ret(c, 0, 2);
        return;
    }
    int32_t idx = (int32_t)ARG(1);
    uint32_t r = 0;
    switch (idx) {
    case -4: r = wnd.wndproc; break;
    case -16: r = wnd.style; break;
    case -20: r = wnd.exstyle; break;
    case -6: r = GUEST_IMAGE_BASE; break;
    default: break;
    }
    stdcall_ret(c, r, 2);
}

SHIM(user32, GetClientRect)
{
    uint32_t r = ARG(1);
    WR32(r + 0u, 0);
    WR32(r + 4u, 0);
    WR32(r + 8u, (uint32_t)wnd.w);
    WR32(r + 12u, (uint32_t)wnd.h);
    stdcall_ret(c, 1, 2);
}

SHIM(user32, ClientToScreen) { stdcall_ret(c, 1, 2); }
SHIM(user32, AdjustWindowRect) { stdcall_ret(c, 1, 3); }

SHIM(user32, GetSystemMetrics)
{
    uint32_t i = ARG(0);
    uint32_t r = 0;
    if (i == 0)
        r = 1024;         /* SM_CXSCREEN */
    else if (i == 1)
        r = 768;          /* SM_CYSCREEN */
    stdcall_ret(c, r, 1);
}

SHIM(user32, GetDC) { stdcall_ret(c, HDC_FAKE, 1); }
SHIM(user32, ReleaseDC) { stdcall_ret(c, 1, 2); }
SHIM(user32, LoadIconA) { stdcall_ret(c, HICON_FAKE, 2); }
SHIM(user32, LoadCursorA) { stdcall_ret(c, HCURSOR_FAKE, 2); }

static int cursor_count;

SHIM(user32, ShowCursor)
{
    cursor_count += ARG(0) ? 1 : -1;
    platform_show_cursor(cursor_count >= 0);
    stdcall_ret(c, (uint32_t)cursor_count, 1);
}

SHIM(user32, MessageBoxA)
{
    rt_log("MessageBoxA(\"%s\", \"%s\")", ARG(2) ? gstr(ARG(2)) : "", ARG(1) ? gstr(ARG(1)) : "");
    stdcall_ret(c, 1, 4);   /* IDOK */
}

SHIM(user32, DefWindowProcA)
{
    uint32_t msg = ARG(1);
    uint32_t r = 0;
    if (msg == WM_CLOSE) {
        uint32_t args[4] = { HWND_MAIN, WM_DESTROY, 0, 0 };
        if (wnd.wndproc)
            guest_call(c, wnd.wndproc, 4, args);
    } else if (msg == WM_NCCREATE) {
        r = 1;
    }
    stdcall_ret(c, r, 4);
}

SHIM(user32, PostMessageA)
{
    user32_post_message(ARG(1), ARG(2), ARG(3));
    stdcall_ret(c, 1, 4);
}

SHIM(user32, PostQuitMessage)
{
    quit_posted = 1;
    quit_code = ARG(0);
    stdcall_ret(c, 0, 1);
}

SHIM(user32, PeekMessageA)
{
    uint32_t out = ARG(0), remove = ARG(4) & 1u;
    platform_pump();
    stdcall_ret(c, (uint32_t)get_message(out, remove), 5);
}

SHIM(user32, GetMessageA)
{
    uint32_t out = ARG(0);
    while (!get_message(out, 1))
        platform_wait();
    stdcall_ret(c, RD32(out + 4u) != WM_QUIT, 4);
}

SHIM(user32, TranslateMessage) { stdcall_ret(c, 0, 1); }

SHIM(user32, DispatchMessageA)
{
    uint32_t m = ARG(0);
    if (IS_VIRT(RD32(m))) {
        stdcall_ret(c, 0, 1);
        return;
    }
    uint32_t r = send_message(c, RD32(m + 4u), RD32(m + 8u), RD32(m + 12u));
    stdcall_ret(c, r, 1);
}

/* ---- gdi32 -------------------------------------------------------------- */

SHIM(gdi32, GetDeviceCaps)
{
    uint32_t idx = ARG(1);
    uint32_t r = 0;
    switch (idx) {
    case 8: r = 1024; break;      /* HORZRES */
    case 10: r = 768; break;      /* VERTRES */
    case 12: r = 32; break;       /* BITSPIXEL */
    case 14: r = 1; break;        /* PLANES */
    case 116: r = 60; break;      /* VREFRESH */
    default: break;
    }
    stdcall_ret(c, r, 2);
}

SHIM(gdi32, CreateICA) { stdcall_ret(c, HDC_FAKE, 4); }
SHIM(gdi32, DeleteDC) { stdcall_ret(c, 1, 1); }

/* ---- more user32 / gdi32 ------------------------------------------------------ */

SHIM(user32, RegisterClassExA)
{
    uint32_t wc = ARG(0);                /* WNDCLASSEXA: cbSize first, else as WNDCLASSA + 4 */
    const char *name = gstr(RD32(wc + 40u));
    for (int i = 0; i < 16; i++) {
        if (!classes[i].name[0]) {
            snprintf(classes[i].name, sizeof(classes[i].name), "%s", name ? name : "");
            classes[i].wndproc = RD32(wc + 8u);
            rt_log("RegisterClassExA(%s) wndproc=%08X", classes[i].name, classes[i].wndproc);
            stdcall_ret(c, 0xC000u + (uint32_t)i, 1);
            return;
        }
    }
    stdcall_ret(c, 0, 1);
}

SHIM(user32, GetClassInfoExA)
{
    const char *name = gstr(ARG(1));
    for (int i = 0; i < 16; i++)
        if (classes[i].name[0] && name && strcasecmp(classes[i].name, name) == 0) {
            WR32(ARG(2) + 8u, classes[i].wndproc);
            stdcall_ret(c, 0xC000u + (uint32_t)i, 3);
            return;
        }
    stdcall_ret(c, 0, 3);
}

SHIM(user32, FindWindowA) { stdcall_ret(c, 0, 2); }   /* no other instance running */

static uint32_t next_wm_atom = 0xC100u;
SHIM(user32, RegisterWindowMessageA) { stdcall_ret(c, next_wm_atom++, 1); }

SHIM(user32, SetWindowLongA)
{
    if (IS_VIRT(ARG(0))) {
        stdcall_ret(c, 0, 3);
        return;
    }
    int32_t idx = (int32_t)ARG(1);
    uint32_t v = ARG(2), old = 0;
    switch (idx) {
    case -4: old = wnd.wndproc; wnd.wndproc = v; break;
    case -16: old = wnd.style; wnd.style = v; break;
    case -20: old = wnd.exstyle; wnd.exstyle = v; break;
    default: break;
    }
    stdcall_ret(c, old, 3);
}

SHIM(user32, SendMessageA)
{
    if (IS_VIRT(ARG(0))) {
        /* The dedicated console's text (EM_REPLACESEL into its edit control) goes to the log. */
        if (ARG(1) == 0x00C2u && ARG(3)) {
            char line[1024];
            snprintf(line, sizeof(line), "%s", gstr(ARG(3)));
            for (char *p = line; *p; p++)
                if (*p == '\r' || *p == '\n')
                    *p = ' ';
            if (line[0])
                rt_log("[con] %s", line);
        }
        stdcall_ret(c, 0, 4);
        return;
    }
    uint32_t r = send_message(c, ARG(1), ARG(2), ARG(3));
    stdcall_ret(c, r, 4);
}

SHIM(user32, WaitMessage)
{
    platform_wait();
    stdcall_ret(c, 1, 0);
}

uint32_t msvcrt_format(uint32_t dst, uint32_t fmt, uint32_t ap);

/* wsprintfA is cdecl (variable arguments). */
SHIM(user32, wsprintfA)
{
    uint32_t n = msvcrt_format(ARG(0), ARG(1), c->r[4] + 12u);
    c->r[0] = n;
    c->r[4] += 4u;
}

SHIM(user32, wvsprintfA) { stdcall_ret(c, msvcrt_format(ARG(0), ARG(1), ARG(2)), 3); }

SHIM(user32, MessageBoxW)
{
    char text[512], cap[128];
    uint32_t t = ARG(1), k = ARG(2);
    size_t i;
    for (i = 0; t && i + 1 < sizeof(text) && RD16(t + 2u * i); i++)
        text[i] = (char)RD16(t + 2u * i);
    text[i] = 0;
    for (i = 0; k && i + 1 < sizeof(cap) && RD16(k + 2u * i); i++)
        cap[i] = (char)RD16(k + 2u * i);
    cap[i] = 0;
    rt_log("MessageBoxW(\"%s\", \"%s\")", cap, text);
    stdcall_ret(c, 1, 4);
}

/* ---- cursor, window geometry ---------------------------------------------------- */

/* The game reads the mouse as Quake 3 does on Windows: GetCursorPos, the offset
 * from the window centre, SetCursorPos back to the centre. The cursor is virtual:
 * where SetCursorPos put it plus the relative motion since. */
static int32_t cursor_x = 320, cursor_y = 240;

SHIM(user32, GetCursorPos)
{
    if ((g_mouse.dx || g_mouse.dy) && getenv("COD_MOUSE_DEBUG"))
        rt_log("GetCursorPos: %d,%d + %d,%d", cursor_x, cursor_y, g_mouse.dx, g_mouse.dy);
    cursor_x += g_mouse.dx;
    cursor_y += g_mouse.dy;
    g_mouse.dx = g_mouse.dy = 0;
    WR32(ARG(0), (uint32_t)cursor_x);
    WR32(ARG(0) + 4u, (uint32_t)cursor_y);
    stdcall_ret(c, 1, 1);
}

SHIM(user32, SetCursorPos)
{
    static int n;
    if (getenv("COD_MOUSE_DEBUG") && n++ < 5)
        rt_log("SetCursorPos(%d, %d)", ARG(0), ARG(1));
    cursor_x = (int32_t)ARG(0);
    cursor_y = (int32_t)ARG(1);
    stdcall_ret(c, 1, 2);
}
SHIM(user32, ScreenToClient) { stdcall_ret(c, 1, 2); }
SHIM(user32, GetCursor) { stdcall_ret(c, HCURSOR_FAKE, 0); }
SHIM(user32, SetCursor) { stdcall_ret(c, HCURSOR_FAKE, 1); }
/* The game captures the mouse while it reads it (in the game and the menus). */
SHIM(user32, SetCapture)
{
    platform_capture_mouse(1);
    stdcall_ret(c, 0, 1);
}

SHIM(user32, ReleaseCapture)
{
    platform_capture_mouse(0);
    stdcall_ret(c, 1, 0);
}
SHIM(user32, ClipCursor) { stdcall_ret(c, 1, 1); }
SHIM(user32, GetDesktopWindow) { stdcall_ret(c, 0x00010001u, 0); }
SHIM(user32, GetParent) { stdcall_ret(c, 0, 1); }
SHIM(user32, AdjustWindowRectEx) { stdcall_ret(c, 1, 4); }

SHIM(user32, GetWindowRect)
{
    uint32_t r = ARG(1);
    WR32(r + 0u, (uint32_t)wnd.x);
    WR32(r + 4u, (uint32_t)wnd.y);
    WR32(r + 8u, (uint32_t)(wnd.x + wnd.w));
    WR32(r + 12u, (uint32_t)(wnd.y + wnd.h));
    stdcall_ret(c, 1, 2);
}

SHIM(user32, MoveWindow)
{
    if (IS_VIRT(ARG(0))) {
        stdcall_ret(c, 0, 6);
        return;
    }
    wnd.x = (int32_t)ARG(1);
    wnd.y = (int32_t)ARG(2);
    wnd.w = (int32_t)ARG(3);
    wnd.h = (int32_t)ARG(4);
    stdcall_ret(c, 1, 6);
}

/* EnumDisplaySettingsA(device, mode index, DEVMODEA *): a few common modes. */
SHIM(user32, EnumDisplaySettingsA)
{
    static const uint16_t modes[][2] = { { 640, 480 }, { 800, 600 }, { 960, 544 }, { 1024, 768 }, { 1280, 720 } };
    uint32_t i = ARG(1), dm = ARG(2);
    if (i == 0xFFFFFFFFu || i == 0xFFFFFFFEu)
        i = 3;                                  /* ENUM_CURRENT_SETTINGS: 1024x768 */
    if (i >= sizeof(modes) / sizeof(modes[0])) {
        stdcall_ret(c, 0, 3);
        return;
    }
    WR32(dm + 104u, 32);                        /* dmBitsPerPel */
    WR32(dm + 108u, modes[i][0]);               /* dmPelsWidth */
    WR32(dm + 112u, modes[i][1]);               /* dmPelsHeight */
    WR32(dm + 120u, 60);                        /* dmDisplayFrequency */
    WR32(dm + 40u, 0x001C0000u | 0x00400000u);  /* dmFields: BITSPERPEL|PELSWIDTH|PELSHEIGHT|DISPLAYFREQUENCY */
    stdcall_ret(c, 1, 3);
}

/* ---- keyboard ------------------------------------------------------------------ */

/* Scan code (DirectInput DIK code) of each virtual key, US layout. */
static const uint8_t vk_scan[256] = {
    [0x08] = 0x0E, [0x09] = 0x0F, [0x0D] = 0x1C, [0x10] = 0x2A, [0x11] = 0x1D, [0x12] = 0x38,
    [0x13] = 0xC5, [0x14] = 0x3A, [0x1B] = 0x01, [0x20] = 0x39, [0x21] = 0xC9, [0x22] = 0xD1,
    [0x23] = 0xCF, [0x24] = 0xC7, [0x25] = 0xCB, [0x26] = 0xC8, [0x27] = 0xCD, [0x28] = 0xD0,
    [0x2D] = 0xD2, [0x2E] = 0xD3,
    ['0'] = 0x0B, ['1'] = 0x02, ['2'] = 0x03, ['3'] = 0x04, ['4'] = 0x05, ['5'] = 0x06, ['6'] = 0x07,
    ['7'] = 0x08, ['8'] = 0x09, ['9'] = 0x0A,
    ['A'] = 0x1E, ['B'] = 0x30, ['C'] = 0x2E, ['D'] = 0x20, ['E'] = 0x12, ['F'] = 0x21, ['G'] = 0x22,
    ['H'] = 0x23, ['I'] = 0x17, ['J'] = 0x24, ['K'] = 0x25, ['L'] = 0x26, ['M'] = 0x32, ['N'] = 0x31,
    ['O'] = 0x18, ['P'] = 0x19, ['Q'] = 0x10, ['R'] = 0x13, ['S'] = 0x1F, ['T'] = 0x14, ['U'] = 0x16,
    ['V'] = 0x2F, ['W'] = 0x11, ['X'] = 0x2D, ['Y'] = 0x15, ['Z'] = 0x2C,
    [0x60] = 0x52, [0x61] = 0x4F, [0x62] = 0x50, [0x63] = 0x51, [0x64] = 0x4B, [0x65] = 0x4C,
    [0x66] = 0x4D, [0x67] = 0x47, [0x68] = 0x48, [0x69] = 0x49, [0x6A] = 0x37, [0x6B] = 0x4E,
    [0x6D] = 0x4A, [0x6E] = 0x53, [0x6F] = 0xB5,
    [0x70] = 0x3B, [0x71] = 0x3C, [0x72] = 0x3D, [0x73] = 0x3E, [0x74] = 0x3F, [0x75] = 0x40,
    [0x76] = 0x41, [0x77] = 0x42, [0x78] = 0x43, [0x79] = 0x44, [0x7A] = 0x57, [0x7B] = 0x58,
    [0x90] = 0x45, [0x91] = 0x46, [0xA0] = 0x2A, [0xA1] = 0x36, [0xA2] = 0x1D, [0xA3] = 0x9D,
    [0xA4] = 0x38, [0xA5] = 0xB8,
    [0xBA] = 0x27, [0xBB] = 0x0D, [0xBC] = 0x33, [0xBD] = 0x0C, [0xBE] = 0x34, [0xBF] = 0x35,
    [0xC0] = 0x29, [0xDB] = 0x1A, [0xDC] = 0x2B, [0xDD] = 0x1B, [0xDE] = 0x28,
};

static uint32_t scan_to_vk(uint32_t sc)
{
    for (uint32_t vk = 1; vk < 256u; vk++)
        if (vk_scan[vk] == sc && !(vk >= 0xA0u && vk <= 0xA5u))
            return vk;
    return 0;
}

/* Virtual key of a DirectInput key code (a scan code, 0x80 = extended). */
uint32_t user32_dik_to_vk(uint8_t dik)
{
    switch (dik) {
    case 0xC8: return 0x26u;   /* up */
    case 0xD0: return 0x28u;   /* down */
    case 0xCB: return 0x25u;   /* left */
    case 0xCD: return 0x27u;   /* right */
    case 0x9D: return 0x11u;   /* right control */
    case 0xB8: return 0x12u;   /* right alt */
    case 0x9C: return 0x0Du;   /* keypad enter */
    case 0x1D: return 0x11u;   /* left control */
    case 0x2A: case 0x36: return 0x10u;   /* shifts */
    default: return dik & 0x80 ? 0u : scan_to_vk(dik);
    }
}

static uint32_t map_vk(uint32_t code, uint32_t type)
{
    switch (type) {
    case 0:                                     /* MAPVK_VK_TO_VSC */
        return code < 256u ? vk_scan[code] & 0x7Fu : 0;
    case 1:                                     /* MAPVK_VSC_TO_VK */
    case 3:                                     /* MAPVK_VSC_TO_VK_EX */
        return scan_to_vk(code & 0xFFu);
    case 2:                                     /* MAPVK_VK_TO_CHAR */
        if ((code >= '0' && code <= '9') || (code >= 'A' && code <= 'Z') || code == ' ')
            return code;
        return 0;
    default:
        return 0;
    }
}

SHIM(user32, MapVirtualKeyA) { stdcall_ret(c, map_vk(ARG(0), ARG(1)), 2); }
SHIM(user32, MapVirtualKeyExA) { stdcall_ret(c, map_vk(ARG(0), ARG(1)), 3); }
SHIM(user32, GetKeyboardLayout) { stdcall_ret(c, 0x04090409u, 1); }

static int vk_down(uint32_t vk)
{
    if (vk == 0x01u) return g_mouse.buttons[0] != 0;
    if (vk == 0x02u) return g_mouse.buttons[1] != 0;
    if (vk == 0x04u) return g_mouse.buttons[2] != 0;
    if (vk == 0x10u) return g_key_state[0x2A] || g_key_state[0x36];
    if (vk == 0x11u) return g_key_state[0x1D] || g_key_state[0x9D];
    if (vk == 0x12u) return g_key_state[0x38] || g_key_state[0xB8];
    uint8_t sc = vk < 256u ? vk_scan[vk] : 0;
    return sc && g_key_state[sc];
}

SHIM(user32, GetAsyncKeyState) { stdcall_ret(c, vk_down(ARG(0)) ? 0x8000u : 0u, 1); }

SHIM(user32, GetKeyboardState)
{
    uint32_t out = ARG(0);
    for (uint32_t vk = 0; vk < 256u; vk++)
        WR8(out + vk, vk_down(vk) ? 0x80u : 0u);
    stdcall_ret(c, 1, 1);
}

static uint32_t ascii_of(uint32_t vk, int shift)
{
    uint32_t ch = 0;
    if (vk >= 'A' && vk <= 'Z')
        ch = shift ? vk : vk + 32u;
    else if (vk >= '0' && vk <= '9')
        ch = shift ? (uint32_t)")!@#$%^&*("[vk - '0'] : vk;
    else if (vk == ' ' || vk == 0x0Du || vk == 0x08u || vk == 0x1Bu || vk == 0x09u)
        ch = vk;
    else if (vk >= 0x60u && vk <= 0x69u)
        ch = '0' + vk - 0x60u;
    else {
        static const struct { uint8_t vk; char lo, hi; } punct[] = {
            { 0xBA, ';', ':' }, { 0xBB, '=', '+' }, { 0xBC, ',', '<' }, { 0xBD, '-', '_' }, { 0xBE, '.', '>' },
            { 0xBF, '/', '?' }, { 0xC0, '`', '~' }, { 0xDB, '[', '{' }, { 0xDC, '\\', '|' }, { 0xDD, ']', '}' },
            { 0xDE, '\'', '"' },
        };
        for (size_t i = 0; i < sizeof(punct) / sizeof(punct[0]); i++)
            if (punct[i].vk == vk)
                ch = (uint8_t)(shift ? punct[i].hi : punct[i].lo);
    }
    return ch;
}

static uint32_t to_ascii(uint32_t vk, uint32_t ks, uint32_t out)
{
    uint32_t ch = ascii_of(vk, (RD8(ks + 0x10u) & 0x80u) != 0);
    if (!ch)
        return 0;
    WR16(out, ch);
    return 1;
}

SHIM(user32, ToAscii) { stdcall_ret(c, to_ascii(ARG(0), ARG(2), ARG(3)), 5); }
SHIM(user32, ToAsciiEx) { stdcall_ret(c, to_ascii(ARG(0), ARG(2), ARG(3)), 6); }

/* GetKeyNameTextA(lParam, buf, size): the key's name from its scan code. */
SHIM(user32, GetKeyNameTextA)
{
    uint32_t sc = (ARG(0) >> 16) & 0xFFu;
    if (ARG(0) & 0x01000000u)
        sc |= 0x80u;
    static const struct { uint8_t sc; const char *name; } names[] = {
        { 0x01, "Esc" }, { 0x0E, "Backspace" }, { 0x0F, "Tab" }, { 0x1C, "Enter" }, { 0x1D, "Ctrl" },
        { 0x2A, "Shift" }, { 0x36, "Right Shift" }, { 0x38, "Alt" }, { 0x39, "Space" }, { 0x3A, "Caps Lock" },
        { 0x9D, "Right Ctrl" }, { 0xB8, "Right Alt" }, { 0xC7, "Home" }, { 0xC8, "Up" }, { 0xC9, "Page Up" },
        { 0xCB, "Left" }, { 0xCD, "Right" }, { 0xCF, "End" }, { 0xD0, "Down" }, { 0xD1, "Page Down" },
        { 0xD2, "Insert" }, { 0xD3, "Delete" },
    };
    char buf[32] = "";
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (names[i].sc == sc)
            snprintf(buf, sizeof(buf), "%s", names[i].name);
    if (!buf[0]) {
        uint32_t vk = scan_to_vk(sc);
        if (vk >= 0x70u && vk <= 0x7Bu)
            snprintf(buf, sizeof(buf), "F%u", vk - 0x6Fu);
        else if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z'))
            snprintf(buf, sizeof(buf), "%c", (char)vk);
        else if (vk >= 0x60u && vk <= 0x69u)
            snprintf(buf, sizeof(buf), "Num %u", vk - 0x60u);
        else if (vk && ascii_of(vk, 0))
            snprintf(buf, sizeof(buf), "%c", (char)ascii_of(vk, 0));
    }
    uint32_t size = ARG(2);
    uint32_t n = 0;
    if (size) {
        n = (uint32_t)strlen(buf);
        if (n >= size)
            n = size - 1u;
        memcpy(gbuf(ARG(1)), buf, n);
        WR8(ARG(1) + n, 0);
    }
    stdcall_ret(c, n, 3);
}

/* ---- gdi32 additions ----------------------------------------------------------- */

SHIM(gdi32, GetStockObject) { stdcall_ret(c, 0x00050000u + ARG(0), 1); }
SHIM(gdi32, GetPixelFormat) { stdcall_ret(c, 1, 1); }
