// user32 / gdi32: the game's window is an SDL window (created by Glide's grSstWinOpen, or on ShowWindow);
// SDL events become Windows messages for the game's window procedure. GDI is inert: the game draws with
// Glide. MessageBox text goes to the log.
#include "native.h"
#include "win.h"
#include <SDL.h>
#include <ctype.h>
#include <strings.h>
#include <pthread.h>

#define U "user32.dll"
#define GD "gdi32.dll"

// ------------------------------------------------------------------ windows
typedef struct { char name[64]; u32 proc, style, extra; } WClass;
static WClass g_classes[32];
static int g_nclasses;

typedef struct {
    int used;
    u32 proc, style, exstyle, parent, userdata;
    int x, y, w, h, visible;
    char title[128], cls[64];
    struct { char name[32]; u32 v; } props[8];
} Wnd;
#define HWND_BASE 0x00010010u
static Wnd g_wnds[16];
u32 g_main_hwnd;
GameView g_view = {640, 480, 1280, 960};

static Wnd* wnd(u32 h) {
    if (h < HWND_BASE || (h - HWND_BASE) % 4) return NULL;
    u32 i = (h - HWND_BASE) / 4;
    return i < 16 && g_wnds[i].used ? &g_wnds[i] : NULL;
}

// ------------------------------------------------------------------ messages
typedef struct { u32 hwnd, msg, wp, lp, time, x, y; } Msg;
static Msg g_q[512];
static int g_qh, g_qt;
static int g_quit;
static u8 g_keys[256];   // VK down state
static int g_mx, g_my;   // cursor in game coordinates
static pthread_mutex_t g_qmu = PTHREAD_MUTEX_INITIALIZER;

void post_msg(u32 hwnd, u32 msg, u32 wp, u32 lp) {
    pthread_mutex_lock(&g_qmu);
    int n = (g_qt + 1) % 512;
    if (n != g_qh) {
        g_q[g_qt] = (Msg){hwnd, msg, wp, lp, SDL_GetTicks(), (u32)g_mx, (u32)g_my};
        g_qt = n;
    }
    pthread_mutex_unlock(&g_qmu);
}

static int sdl_to_vk(SDL_Keycode k) {
    if (k >= 'a' && k <= 'z') return k - 'a' + 'A';
    if (k >= '0' && k <= '9') return k;
    if (k >= SDLK_F1 && k <= SDLK_F12) return 0x70 + (k - SDLK_F1);
    switch (k) {
        case SDLK_RETURN: case SDLK_KP_ENTER: return 0x0D;
        case SDLK_ESCAPE: case SDLK_AC_BACK: return 0x1B;  // the phone's Back is Esc
        case SDLK_BACKSPACE: return 0x08;
        case SDLK_TAB: return 0x09;
        case SDLK_SPACE: return 0x20;
        case SDLK_LSHIFT: case SDLK_RSHIFT: return 0x10;
        case SDLK_LCTRL: case SDLK_RCTRL: return 0x11;
        case SDLK_LALT: case SDLK_RALT: return 0x12;
        case SDLK_PAUSE: return 0x13;
        case SDLK_PAGEUP: return 0x21;
        case SDLK_PAGEDOWN: return 0x22;
        case SDLK_END: return 0x23;
        case SDLK_HOME: return 0x24;
        case SDLK_LEFT: return 0x25;
        case SDLK_UP: return 0x26;
        case SDLK_RIGHT: return 0x27;
        case SDLK_DOWN: return 0x28;
        case SDLK_PRINTSCREEN: return 0x2C;
        case SDLK_INSERT: return 0x2D;
        case SDLK_DELETE: return 0x2E;
        case SDLK_MINUS: return 0xBD;
        case SDLK_EQUALS: return 0xBB;
        case SDLK_LEFTBRACKET: return 0xDB;
        case SDLK_RIGHTBRACKET: return 0xDD;
        case SDLK_SEMICOLON: return 0xBA;
        case SDLK_QUOTE: return 0xDE;
        case SDLK_BACKQUOTE: return 0xC0;
        case SDLK_COMMA: return 0xBC;
        case SDLK_PERIOD: return 0xBE;
        case SDLK_SLASH: return 0xBF;
        case SDLK_BACKSLASH: return 0xDC;
        case SDLK_KP_PLUS: return 0x6B;
        case SDLK_KP_MINUS: return 0x6D;
        case SDLK_KP_MULTIPLY: return 0x6A;
        case SDLK_KP_DIVIDE: return 0x6F;
    }
    if (k >= SDLK_KP_1 && k <= SDLK_KP_9) return 0x61 + (k - SDLK_KP_1);
    if (k == SDLK_KP_0) return 0x60;
    return 0;
}

static u32 mk_buttons(void) {
    u32 b = 0;
    if (g_keys[0x01]) b |= 1;   // MK_LBUTTON
    if (g_keys[0x02]) b |= 2;   // MK_RBUTTON
    if (g_keys[0x10]) b |= 4;   // MK_SHIFT
    if (g_keys[0x11]) b |= 8;   // MK_CONTROL
    if (g_keys[0x04]) b |= 16;  // MK_MBUTTON
    return b;
}

static void to_game(int wx, int wy) {
    // the game picture is letterboxed into the window (see glide.c)
    float sx = (float)(wx - g_view.ox) * g_view.w / (g_view.dw ? g_view.dw : g_view.w);
    float sy = (float)(wy - g_view.oy) * g_view.h / (g_view.dh ? g_view.dh : g_view.h);
    g_mx = (int)sx; g_my = (int)sy;
    if (g_mx < 0) g_mx = 0;
    if (g_my < 0) g_my = 0;
    if (g_mx >= g_view.w) g_mx = g_view.w - 1;
    if (g_my >= g_view.h) g_my = g_view.h - 1;
}

// Hooks for the phone's touch layer (android port): synthetic input enters here.
void input_mouse(int gx, int gy, int button, int down) {
    extern float g_menu_dx;
    gx -= (int)g_menu_dx;  // centred widescreen menus
    if (gx < 0) gx = 0;
    if (g_menu_dx > 0 && gx > 799) gx = 799;  // the black margins beside a menu
    // the widescreen HUD's ends sit moved in (glide.c hud_shift): clicks on them go back to where the game has them
    if (g_menu_dx == 0 && g_view.w > 800 && gy >= g_view.h - 48) {
        int dx = (g_view.w - 800) / 2;
        if (gx >= dx && gx < dx + 170) gx -= dx;
        else if (gx >= g_view.w - 170 - dx && gx < g_view.w - dx) gx += dx;
    }
    g_mx = gx; g_my = gy;
    u32 lp = (u32)(g_my & 0xFFFF) << 16 | (u32)(g_mx & 0xFFFF);
    if (button == 0) { post_msg(g_main_hwnd, 0x0200, mk_buttons(), lp); return; }
    int vk = button == 1 ? 1 : button == 2 ? 2 : 4;
    g_keys[vk] = (u8)down;
    u32 m = button == 1 ? (down ? 0x0201 : 0x0202) : button == 2 ? (down ? 0x0204 : 0x0205) : (down ? 0x0207 : 0x0208);
    post_msg(g_main_hwnd, m, mk_buttons(), lp);
}
void input_key(int vk, int down) {
    extern int g_log_frames;
    if (vk == 0x1B && !down) g_log_frames = 40;  // the frames after Esc (its menu fades in)
    g_keys[vk & 0xFF] = (u8)down;
    u32 lp = down ? 1 : 0xC0000001u;
    post_msg(g_main_hwnd, vk == 0x12 ? (down ? 0x0104 : 0x0105) : (down ? 0x0100 : 0x0101), (u32)vk, lp);
}
void input_char(u16 ch) { post_msg(g_main_hwnd, 0x0102, ch, 1); }

char g_shot_name[64];
static float g_menu_dx_get(void) { extern float g_menu_dx; return g_menu_dx; }
static void autoplay(void) {
    static const char* p;
    static u32 t0, next = 0xFFFFFFFFu;
    static int init;
    // a click is a hover, then the button down and up a few frames later (the menus need the hover)
    static int pend_x, pend_y, pend_btn, pend_stage;
    static u32 pend_t;
    if (pend_stage && SDL_GetTicks() - pend_t >= 120u * pend_stage) {
        input_mouse(pend_x, pend_y, 0, 0);
        input_mouse(pend_x, pend_y, pend_btn, pend_stage == 1);
        { extern int g_trace; static u32 tr0; if (getenv("D2MSGLOG")) { g_trace = 1; tr0 = SDL_GetTicks(); } (void)tr0; }
        pend_stage = pend_stage == 1 ? 2 : 0;
    }
    { extern int g_trace; static u32 trs; if (g_trace && getenv("D2MSGLOG")) { if (!trs) trs = SDL_GetTicks(); else if (SDL_GetTicks() - trs > 1500) { g_trace = 0; trs = 0; } } }
    if (!init) { init = 1; p = getenv("D2AUTO"); t0 = SDL_GetTicks(); if (p) next = (u32)atoi(p); }
    // scripted runs act as the active app even while the window has no focus
    static int act;
    if (p && !act && SDL_GetTicks() - t0 > 3000) { act = 1; post_msg(g_main_hwnd, 0x001C, 1, 0); }
    while (p && *p && SDL_GetTicks() - t0 >= next) {
        const char* c = strchr(p, ':');
        if (!c) { p = NULL; break; }
        c++;
        if (c[0] == 'f' && c[1] >= '0' && c[1] <= '9' && (c[2] == 'd' || c[2] == 'm' || c[2] == 'u') && c[3] == '=') {
            // a finger (touch controls): f<id><d|m|u>=x,y in thousandths of the window
            SDL_Event ev = {0};
            ev.type = c[2] == 'd' ? SDL_FINGERDOWN : c[2] == 'm' ? SDL_FINGERMOTION : SDL_FINGERUP;
            ev.tfinger.fingerId = c[1] - '0';
            ev.tfinger.x = atoi(c + 4) / 1000.f;
            const char* q = strchr(c + 4, ',');
            ev.tfinger.y = q ? atoi(q + 1) / 1000.f : 0;
            SDL_PushEvent(&ev);
            LOG("auto: finger %.4s %.3f,%.3f", c, ev.tfinger.x, ev.tfinger.y);
        }
        else if (!strncmp(c, "text=", 5)) { for (const char* t = c + 5; *t && *t != ';'; ++t) input_char((u8)*t); }
        else if (!strncmp(c, "log=", 4)) { extern int g_log_frames; g_log_frames = atoi(c + 4); }
        else if (!strncmp(c, "npctap=", 7)) {
            extern int touch_npctap_test(int, int);
            const char* q = strchr(c + 7, ',');
            if (!touch_npctap_test(atoi(c + 7), q ? atoi(q + 1) : 0)) LOG("auto: npctap: nothing near");
        }
        else if (!strncmp(c, "aim", 3)) {
            extern int touch_aim_test(int*, int*);
            int x = 0, y = 0, ok = touch_aim_test(&x, &y);
            LOG("auto: aim %d at %d,%d", ok, x, y);
            if (ok) input_mouse(x + (int)g_menu_dx_get(), y, 0, 0);
        }
        else if (!strncmp(c, "dump", 4)) {
            // guest memory (non-zero 64 KB chunks: u32 address + data) for offline digging
            extern u8* G; extern char g_savedir[];
            char path[1100]; snprintf(path, sizeof path, "%s/mem.dump", g_savedir);
            FILE* f = fopen(path, "wb");
            static const u8 zero[65536];
            for (u64 a = 0x00400000; f && a < 0x80000000ull; a += 65536)
                if (memcmp(G + a, zero, 65536)) { u32 a32 = (u32)a; fwrite(&a32, 4, 1, f); fwrite(G + a, 1, 65536, f); }
            if (f) fclose(f);
            LOG("auto: dumped guest memory");
        }
        else if (!strncmp(c, "shot=", 5)) { int n = 0; while (c[5 + n] && c[5 + n] != ';' && n < 63) { g_shot_name[n] = c[5 + n]; n++; } g_shot_name[n] = 0; }
        else if (!strncmp(c, "key=", 4)) { int vk = atoi(c + 4); input_key(vk, 1); input_key(vk, 0); LOG("auto: key %d", vk); }
        else {
            int x = atoi(c), y = 0, btn = 1;
            const char* q = strchr(c, ',');
            if (q) y = atoi(q + 1);
            if (q && strchr(q + 1, ',') && strchr(q + 1, ',') < strchr(c, ';' ) ) btn = 2;
            extern float g_menu_dx;
            x += (int)g_menu_dx;
            input_mouse(x, y, 0, 0);
            pend_x = x; pend_y = y; pend_btn = btn; pend_t = SDL_GetTicks(); pend_stage = 1;
            LOG("auto: click %d,%d", x, y);
        }
        const char* sc = strchr(c, ';');
        if (!sc) { p = NULL; break; }
        p = sc + 1;
        next = (u32)atoi(p);
    }
}

static void pump_sdl(void) {
    extern int glide_window_ready(void);
    if (!glide_window_ready()) return;
    // the game polls PeekMessage in a tight loop; SDL's event pump (with D-Bus behind it) is expensive
    static u32 last;
    u32 now = SDL_GetTicks();
    if (now - last < 4) return;
    last = now;
    autoplay();
    SDL_Event e;
    extern int touch_event(const SDL_Event*);
    extern void touch_tick(void);
    extern int touch_enabled(void);
    int touch = touch_enabled();
    if (touch) touch_tick();
    while (SDL_PollEvent(&e)) {
        if (touch && touch_event(&e)) continue;
        switch (e.type) {
            case SDL_QUIT: post_msg(g_main_hwnd, 0x0010, 0, 0); break;  // WM_CLOSE
            case SDL_MOUSEMOTION:
                to_game(e.motion.x, e.motion.y);
                input_mouse(g_mx, g_my, 0, 0);
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP: {

                to_game(e.button.x, e.button.y);
                int b = e.button.button == SDL_BUTTON_LEFT ? 1 : e.button.button == SDL_BUTTON_RIGHT ? 2 : 3;
                input_mouse(g_mx, g_my, b, e.type == SDL_MOUSEBUTTONDOWN);
                break;
            }
            case SDL_MOUSEWHEEL:
                post_msg(g_main_hwnd, 0x020A, (u32)((e.wheel.y > 0 ? 120 : -120) << 16) | mk_buttons(), (u32)(g_my << 16) | (u32)g_mx);
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP: {
                if (e.key.keysym.sym == SDLK_F11) { if (e.type == SDL_KEYDOWN) { extern int g_overlay_on; g_overlay_on ^= 1; } break; }
                int vk = sdl_to_vk(e.key.keysym.sym);
                if (!vk) break;
                input_key(vk, e.type == SDL_KEYDOWN);
                // Enter / Escape / Backspace / Tab also arrive as characters (chat, names)
                if (e.type == SDL_KEYDOWN && (vk == 0x0D || vk == 0x1B || vk == 0x08 || vk == 0x09)) input_char((u16)vk);
                break;
            }
            case SDL_TEXTINPUT: {
                // UTF-8 -> code points; the game wants characters of its code page (CP1251) in WM_CHAR
                const u8* s = (const u8*)e.text.text;
                while (*s) {
                    u32 cp;
                    if (*s < 0x80) cp = *s++;
                    else if ((*s & 0xE0) == 0xC0) { cp = (u32)(*s & 0x1F) << 6 | (s[1] & 0x3F); s += 2; }
                    else if ((*s & 0xF0) == 0xE0) { cp = (u32)(*s & 0x0F) << 12 | (u32)(s[1] & 0x3F) << 6 | (s[2] & 0x3F); s += 3; }
                    else { s += 4; continue; }
                    if (cp >= 0x410 && cp <= 0x44F) cp = 0xC0 + (cp - 0x410);
                    else if (cp == 0x401) cp = 0xA8;
                    else if (cp == 0x451) cp = 0xB8;
                    if (cp < 256) input_char((u16)cp);
                }
                break;
            }
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) post_msg(g_main_hwnd, 0x001C, 1, 0);
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST && !getenv("D2AUTO")) post_msg(g_main_hwnd, 0x001C, 0, 0);
                if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) { extern void glide_resized(void); glide_resized(); }
                break;
        }
    }
}

static int take_msg(Msg* m, u32 hwnd, u32 lo, u32 hi, int remove) {
    pthread_mutex_lock(&g_qmu);
    for (int i = g_qh; i != g_qt; i = (i + 1) % 512) {
        Msg* q = &g_q[i];
        if (hwnd && q->hwnd != hwnd) continue;
        if ((lo || hi) && (q->msg < lo || q->msg > hi)) continue;
        *m = *q;
        if (remove && (q->msg == 0x201 || q->msg == 0x202 || q->msg == 0x1C) && getenv("D2MSGLOG")) LOG("msg %X lp %X", q->msg, q->lp);
        if (remove) {
            // remove i, shifting the rest
            for (int j = i; j != g_qt; j = (j + 1) % 512) {
                int k = (j + 1) % 512;
                if (k == g_qt) break;
                g_q[j] = g_q[k];
            }
            g_qt = (g_qt + 511) % 512;
        }
        pthread_mutex_unlock(&g_qmu);
        return 1;
    }
    pthread_mutex_unlock(&g_qmu);
    return 0;
}

static void put_msg(u32 p, Msg* m) {
    wr32(p, m->hwnd); wr32(p + 4, m->msg); wr32(p + 8, m->wp); wr32(p + 12, m->lp);
    wr32(p + 16, m->time); wr32(p + 20, m->x); wr32(p + 24, m->y);
}

static void nt_PeekMessageA(Cpu* c) {
    pump_sdl();
    Msg m;
    if (take_msg(&m, 0, ARG(2), ARG(3), ARG(4) & 1)) { put_msg(ARG(0), &m); RET(1, 5); }
    if (g_quit) { m = (Msg){0, 0x0012, 0, 0, 0, 0, 0}; put_msg(ARG(0), &m); if (ARG(4) & 1) g_quit = 0; RET(1, 5); }
    RET(0, 5);
}
static void nt_GetMessageA(Cpu* c) {
    Msg m;
    for (;;) {
        pump_sdl();
        if (take_msg(&m, 0, ARG(2), ARG(3), 1)) break;
        if (g_quit) { m = (Msg){0, 0x0012, 0, 0, 0, 0, 0}; put_msg(ARG(0), &m); RET(0, 4); }
        SDL_Delay(5);
    }
    put_msg(ARG(0), &m);
    RET(m.msg != 0x0012, 4);
}
static void nt_TranslateMessage(Cpu* c) { RET(0, 1); }  // characters come from SDL_TEXTINPUT

static u32 call_wndproc(u32 proc, u32 h, u32 msg, u32 wp, u32 lp) {
    if (!proc) return 0;
    u32 a[4] = {h, msg, wp, lp};
    return call_guest(cpu_current(), proc, 4, a);
}
static u32 def_proc(u32 h, u32 msg, u32 wp, u32 lp) {
    switch (msg) {
        case 0x0010: { extern void exit(int); LOG("WM_CLOSE: quitting"); exit(0); }  // WM_CLOSE (DefWindowProc destroys)
        case 0x0081: return 1;  // WM_NCCREATE
        case 0x0020: return 1;  // WM_SETCURSOR
        case 0x0014: return 1;  // WM_ERASEBKGND
    }
    (void)h; (void)wp; (void)lp;
    return 0;
}
static void nt_DispatchMessageA(Cpu* c) {
    u32 p = ARG(0);
    Wnd* w = wnd(rd32(p));
    u32 r = w ? call_wndproc(w->proc, rd32(p), rd32(p + 4), rd32(p + 8), rd32(p + 12)) : 0;
    RET(r, 1);
}
static void nt_SendMessageA(Cpu* c) {
    Wnd* w = wnd(ARG(0));
    RET(w ? call_wndproc(w->proc, ARG(0), ARG(1), ARG(2), ARG(3)) : 0, 4);
}
static void nt_PostMessageA(Cpu* c) { post_msg(ARG(0), ARG(1), ARG(2), ARG(3)); RET(1, 4); }
static void nt_PostQuitMessage(Cpu* c) { g_quit = 1; RET(0, 1); }
static void nt_DefWindowProcA(Cpu* c) { RET(def_proc(ARG(0), ARG(1), ARG(2), ARG(3)), 4); }
static void nt_CallWindowProcA(Cpu* c) {
    u32 proc = ARG(0);
    RET(call_wndproc(proc, ARG(1), ARG(2), ARG(3), ARG(4)), 5);
}
static void nt_DefDlgProcA(Cpu* c) { RET(0, 4); }

static void nt_RegisterClassA(Cpu* c) {
    u32 wc = ARG(0);
    if (g_nclasses >= 32) RET(0, 1);
    WClass* k = &g_classes[g_nclasses++];
    k->style = rd32(wc); k->proc = rd32(wc + 4); k->extra = rd32(wc + 12);
    u32 name = rd32(wc + 36);
    if (name >= 0x10000) snprintf(k->name, sizeof k->name, "%s", gstr(name));
    else snprintf(k->name, sizeof k->name, "#%u", name);
    LOG("RegisterClass(%s) proc %08X", k->name, k->proc);
    RET(0xC000 + g_nclasses, 1);
}
static void nt_UnregisterClassA(Cpu* c) { RET(1, 2); }
static void nt_GetClassInfoA(Cpu* c) { RET(0, 3); }
static void nt_GetClassLongA(Cpu* c) { RET(0, 2); }
static void nt_SetClassLongA(Cpu* c) { RET(0, 3); }
static void nt_GetClassNameA(Cpu* c) {
    Wnd* w = wnd(ARG(0));
    const char* n = w ? w->cls : "";
    u32 l = (u32)strlen(n);
    if (ARG(2) && l >= ARG(2)) l = ARG(2) - 1;
    gput(ARG(1), n, l); wr8(ARG(1) + l, 0);
    RET(l, 3);
}

static void nt_CreateWindowExA(Cpu* c) {
    u32 cls = ARG(1), title = ARG(2);
    char cname[64];
    if (cls >= 0x10000) snprintf(cname, sizeof cname, "%s", gstr(cls));
    else snprintf(cname, sizeof cname, "#%u", cls);
    WClass* k = NULL;
    for (int i = 0; i < g_nclasses; ++i) if (!strcasecmp(g_classes[i].name, cname)) k = &g_classes[i];
    int i;
    for (i = 0; i < 16 && g_wnds[i].used; ++i) {}
    if (i == 16) RET(0, 12);
    Wnd* w = &g_wnds[i];
    memset(w, 0, sizeof *w);
    w->used = 1;
    w->proc = k ? k->proc : 0;
    w->exstyle = ARG(0); w->style = ARG(3);
    w->x = (int)ARG(4); w->y = (int)ARG(5); w->w = (int)ARG(6); w->h = (int)ARG(7);
    w->parent = ARG(8);
    snprintf(w->cls, sizeof w->cls, "%s", cname);
    if (title) snprintf(w->title, sizeof w->title, "%s", gstr(title));
    u32 h = HWND_BASE + 4 * i;
    if (!g_main_hwnd && !w->parent) g_main_hwnd = h;
    LOG("CreateWindowEx(%s, \"%s\", %dx%d) = %08X proc %08X", cname, w->title, w->w, w->h, h, w->proc);
    // WM_NCCREATE / WM_CREATE with a CREATESTRUCT
    u32 cs = gcalloc(48);
    wr32(cs, ARG(11)); wr32(cs + 4, ARG(10)); wr32(cs + 8, ARG(9)); wr32(cs + 12, ARG(8));
    wr32(cs + 16, ARG(7)); wr32(cs + 20, ARG(6)); wr32(cs + 24, ARG(5)); wr32(cs + 28, ARG(4));
    wr32(cs + 32, ARG(3)); wr32(cs + 36, title); wr32(cs + 40, cls); wr32(cs + 44, ARG(0));
    call_wndproc(w->proc, h, 0x0081, 0, cs);
    u32 r = call_wndproc(w->proc, h, 0x0001, 0, cs);
    gfree(cs);
    if (r == 0xFFFFFFFFu) { w->used = 0; RET(0, 12); }
    RET(h, 12);
}
static void nt_DestroyWindow(Cpu* c) {
    Wnd* w = wnd(ARG(0));
    if (w) { call_wndproc(w->proc, ARG(0), 0x0002, 0, 0); w->used = 0; }
    RET(1, 1);
}
static void nt_ShowWindow(Cpu* c) {
    Wnd* w = wnd(ARG(0));
    int was = w ? w->visible : 0;
    if (w) w->visible = ARG(1) != 0;
    RET(was, 2);
}
static void nt_UpdateWindow(Cpu* c) { RET(1, 1); }
static void nt_SetWindowPos(Cpu* c) {
    Wnd* w = wnd(ARG(0));
    u32 fl = ARG(6);
    if (w && !(fl & 2)) { w->x = (int)ARG(2); w->y = (int)ARG(3); }
    if (w && !(fl & 1)) { w->w = (int)ARG(4); w->h = (int)ARG(5); }
    RET(1, 7);
}
static void put_rect(u32 p, int l, int t, int r, int b) { wr32(p, (u32)l); wr32(p + 4, (u32)t); wr32(p + 8, (u32)r); wr32(p + 12, (u32)b); }
static void nt_GetClientRect(Cpu* c) { put_rect(ARG(1), 0, 0, g_view.w, g_view.h); RET(1, 2); }
static void nt_GetWindowRect(Cpu* c) { put_rect(ARG(1), 0, 0, g_view.w, g_view.h); RET(1, 2); }
static void nt_AdjustWindowRectEx(Cpu* c) { RET(1, 4); }
static void nt_ClientToScreen(Cpu* c) { RET(1, 2); }
static void nt_ScreenToClient(Cpu* c) { RET(1, 2); }
static void nt_GetCursorPos(Cpu* c) { wr32(ARG(0), (u32)g_mx); wr32(ARG(0) + 4, (u32)g_my); RET(1, 1); }
static void nt_SetCursorPos(Cpu* c) { g_mx = (int)ARG(0); g_my = (int)ARG(1); RET(1, 2); }
static void nt_GetAsyncKeyState(Cpu* c) { RET(g_keys[ARG(0) & 0xFF] ? 0x8000 : 0, 1); }
static void nt_GetKeyState(Cpu* c) { RET(g_keys[ARG(0) & 0xFF] ? 0xFF80 : 0, 1); }
static void nt_GetKeyboardLayout(Cpu* c) { RET(0x04190419u, 1); }
static void nt_IsIconic(Cpu* c) { RET(0, 1); }
static void nt_IsWindow(Cpu* c) { RET(wnd(ARG(0)) != NULL, 1); }
static void nt_IsWindowVisible(Cpu* c) { Wnd* w = wnd(ARG(0)); RET(w && w->visible, 1); }
static void nt_IsWindowEnabled(Cpu* c) { RET(1, 1); }
static void nt_EnableWindow(Cpu* c) { RET(0, 2); }
static void nt_GetForegroundWindow(Cpu* c) { RET(g_main_hwnd, 0); }
static void nt_SetForegroundWindow(Cpu* c) { RET(1, 1); }
static void nt_GetActiveWindow(Cpu* c) { RET(g_main_hwnd, 0); }
static void nt_SetActiveWindow(Cpu* c) { RET(g_main_hwnd, 1); }
static void nt_GetFocus(Cpu* c) { RET(g_main_hwnd, 0); }
static void nt_SetFocus(Cpu* c) { RET(g_main_hwnd, 1); }
static void nt_GetDesktopWindow(Cpu* c) { RET(0x00010000u, 0); }
static void nt_GetParent(Cpu* c) { Wnd* w = wnd(ARG(0)); RET(w ? w->parent : 0, 1); }
static void nt_GetWindow(Cpu* c) { RET(0, 2); }
static void nt_GetTopWindow(Cpu* c) { RET(0, 1); }
static void nt_FindWindowA(Cpu* c) { RET(0, 2); }      // no other running copy
static void nt_FindWindowExA(Cpu* c) { RET(0, 4); }
static void nt_GetWindowLongA(Cpu* c) {
    Wnd* w = wnd(ARG(0));
    s32 i = (s32)ARG(1);
    if (!w) RET(0, 2);
    switch (i) {
        case -4: RET(w->proc, 2);
        case -16: RET(w->style, 2);
        case -20: RET(w->exstyle, 2);
        case -21: RET(w->userdata, 2);
        case -6: RET(0x00400000u, 2);
    }
    RET(0, 2);
}
static void nt_SetWindowLongA(Cpu* c) {
    Wnd* w = wnd(ARG(0));
    s32 i = (s32)ARG(1);
    u32 v = ARG(2), old = 0;
    if (!w) RET(0, 3);
    switch (i) {
        case -4: old = w->proc; w->proc = v; break;
        case -16: old = w->style; w->style = v; break;
        case -20: old = w->exstyle; w->exstyle = v; break;
        case -21: old = w->userdata; w->userdata = v; break;
    }
    RET(old, 3);
}
static void nt_GetPropA(Cpu* c) {
    Wnd* w = wnd(ARG(0));
    if (w) for (int i = 0; i < 8; ++i) if (w->props[i].name[0] && !strcasecmp(w->props[i].name, gstr(ARG(1)))) RET(w->props[i].v, 2);
    RET(0, 2);
}
static void nt_SetPropA(Cpu* c) {
    Wnd* w = wnd(ARG(0));
    if (!w) RET(0, 3);
    for (int i = 0; i < 8; ++i)
        if (!w->props[i].name[0] || !strcasecmp(w->props[i].name, gstr(ARG(1)))) {
            snprintf(w->props[i].name, 32, "%s", gstr(ARG(1)));
            w->props[i].v = ARG(2);
            RET(1, 3);
        }
    RET(0, 3);
}
static void nt_RemovePropA(Cpu* c) {
    Wnd* w = wnd(ARG(0));
    if (w) for (int i = 0; i < 8; ++i) if (w->props[i].name[0] && !strcasecmp(w->props[i].name, gstr(ARG(1)))) { w->props[i].name[0] = 0; RET(w->props[i].v, 2); }
    RET(0, 2);
}
static void nt_GetWindowTextA(Cpu* c) {
    Wnd* w = wnd(ARG(0));
    const char* t = w ? w->title : "";
    u32 l = (u32)strlen(t);
    if (ARG(2) && l >= ARG(2)) l = ARG(2) - 1;
    gput(ARG(1), t, l); wr8(ARG(1) + l, 0);
    RET(l, 3);
}
static void nt_GetWindowTextLengthA(Cpu* c) { Wnd* w = wnd(ARG(0)); RET(w ? strlen(w->title) : 0, 1); }
static void nt_GetWindowThreadProcessId(Cpu* c) { if (ARG(1)) wr32(ARG(1), 1000); RET(2000, 2); }
static void nt_GetWindowPlacement(Cpu* c) {
    u32 p = ARG(1);
    memset(H(p), 0, 44); wr32(p, 44); wr32(p + 8, 1);
    put_rect(p + 28, 0, 0, g_view.w, g_view.h);
    RET(1, 2);
}
static void nt_InvalidateRect(Cpu* c) { RET(1, 3); }
static void nt_BeginPaint(Cpu* c) { memset(H(ARG(1)), 0, 64); wr32(ARG(1), 0x00020001u); put_rect(ARG(1) + 8, 0, 0, g_view.w, g_view.h); RET(0x00020001u, 2); }
static void nt_EndPaint(Cpu* c) { RET(1, 2); }
static void nt_GetDC(Cpu* c) { RET(0x00020001u, 1); }
static void nt_ReleaseDC(Cpu* c) { RET(1, 2); }
static void nt_GetUpdateRgn(Cpu* c) { RET(1, 3); }  // NULLREGION
static void nt_GetSystemMetrics(Cpu* c) {
    switch (ARG(0)) {
        case 0: RET(1280, 1);   // SM_CXSCREEN
        case 1: RET(960, 1);    // SM_CYSCREEN
        case 4: RET(20, 1);     // SM_CYCAPTION
        case 5: case 6: RET(1, 1);
        case 7: case 8: RET(3, 1);
        case 11: case 12: RET(32, 1);  // cursor size
        case 15: RET(20, 1);
        case 32: case 33: RET(4, 1);
        case 43: RET(3, 1);     // SM_CMOUSEBUTTONS
        case 19: RET(1, 1);     // SM_MOUSEPRESENT
        case 78: RET(1280, 1);
        case 79: RET(960, 1);
    }
    RET(0, 1);
}
static void nt_SystemParametersInfoA(Cpu* c) {
    if (ARG(0) == 0x30 && ARG(2)) put_rect(ARG(2), 0, 0, 1280, 960);  // SPI_GETWORKAREA
    RET(1, 4);
}
static void nt_ChangeDisplaySettingsA(Cpu* c) { RET(0, 2); }
static void nt_MonitorFromWindow(Cpu* c) { RET(0x00030001u, 2); }
static void nt_GetMonitorInfoA(Cpu* c) {
    u32 p = ARG(1);
    put_rect(p + 4, 0, 0, 1280, 960); put_rect(p + 20, 0, 0, 1280, 960); wr32(p + 36, 1);
    RET(1, 2);
}
static int g_cursor_count;
static void nt_LoadCursorA(Cpu* c) { RET(0x00040001u, 2); }
static void nt_LoadIconA(Cpu* c) { RET(0x00040002u, 2); }
static void nt_LoadImageA(Cpu* c) { RET(0x00040003u, 6); }
static void nt_CreateCursor(Cpu* c) { RET(0x00040004u, 7); }
static void nt_DestroyCursor(Cpu* c) { RET(1, 1); }
static void nt_SetCursor(Cpu* c) { RET(0x00040001u, 1); }
static void nt_ShowCursor(Cpu* c) { g_cursor_count += ARG(0) ? 1 : -1; RET((u32)g_cursor_count, 1); }
static void nt_LoadStringA(Cpu* c) { if (ARG(3)) wr8(ARG(2), 0); RET(0, 4); }
static void nt_LoadAcceleratorsA(Cpu* c) { RET(0, 2); }
static void nt_TranslateAcceleratorA(Cpu* c) { RET(0, 3); }
static void nt_IsDialogMessageA(Cpu* c) { RET(0, 2); }
static void nt_DialogBoxParamA(Cpu* c) { LOG("DialogBoxParam(%u) refused", ARG(1)); RET(0xFFFFFFFFu, 5); }
static void nt_EndDialog(Cpu* c) { RET(1, 2); }
static void nt_GetDlgItem(Cpu* c) { RET(0, 2); }
static void nt_GetDlgCtrlID(Cpu* c) { RET(0, 1); }
static void nt_GetDlgItemTextA(Cpu* c) { if (ARG(3)) wr8(ARG(2), 0); RET(0, 4); }
static void nt_SetDlgItemTextA(Cpu* c) { RET(1, 3); }
static void nt_SendDlgItemMessageA(Cpu* c) { RET(0, 5); }
static void nt_RegisterWindowMessageA(Cpu* c) { static u32 n = 0xC100; RET(n++, 1); }
static void nt_OpenClipboard(Cpu* c) { RET(0, 1); }
static void nt_CloseClipboard(Cpu* c) { RET(1, 0); }
static void nt_EmptyClipboard(Cpu* c) { RET(0, 0); }
static void nt_GetClipboardData(Cpu* c) { RET(0, 1); }
static void nt_SetClipboardData(Cpu* c) { RET(0, 2); }
static void nt_IsClipboardFormatAvailable(Cpu* c) { RET(0, 1); }
static void nt_CharLowerBuffA(Cpu* c) {
    u32 p = ARG(0), n = ARG(1);
    for (u32 i = 0; i < n; ++i) {
        u8 ch = rd8(p + i);
        if (ch >= 'A' && ch <= 'Z') ch += 32;
        else if (ch >= 0xC0 && ch <= 0xDF) ch += 32;
        wr8(p + i, ch);
    }
    RET(n, 2);
}
static void nt_CharNextA(Cpu* c) { u32 p = ARG(0); RET(rd8(p) ? p + 1 : p, 1); }
static void nt_CopyRect(Cpu* c) { memmove(H(ARG(0)), H(ARG(1)), 16); RET(1, 2); }
static void nt_SetRect(Cpu* c) { put_rect(ARG(0), (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4)); RET(1, 5); }
static void nt_IntersectRect(Cpu* c) {
    u32 d = ARG(0), a = ARG(1), b = ARG(2);
    s32 l = (s32)rd32(a) > (s32)rd32(b) ? (s32)rd32(a) : (s32)rd32(b);
    s32 t = (s32)rd32(a + 4) > (s32)rd32(b + 4) ? (s32)rd32(a + 4) : (s32)rd32(b + 4);
    s32 r = (s32)rd32(a + 8) < (s32)rd32(b + 8) ? (s32)rd32(a + 8) : (s32)rd32(b + 8);
    s32 bo = (s32)rd32(a + 12) < (s32)rd32(b + 12) ? (s32)rd32(a + 12) : (s32)rd32(b + 12);
    if (l >= r || t >= bo) { put_rect(d, 0, 0, 0, 0); RET(0, 3); }
    put_rect(d, l, t, r, bo);
    RET(1, 3);
}
static void nt_PtInRect(Cpu* c) {
    u32 r = ARG(0);
    s32 x = (s32)ARG(1), y = (s32)ARG(2);
    RET(x >= (s32)rd32(r) && x < (s32)rd32(r + 8) && y >= (s32)rd32(r + 4) && y < (s32)rd32(r + 12), 3);
}
static void nt_DrawEdge(Cpu* c) { RET(1, 4); }
static void nt_DrawFocusRect(Cpu* c) { RET(1, 2); }
static void nt_DrawTextA(Cpu* c) { RET(16, 5); }
static void nt_MessageBoxA(Cpu* c) {
    LOG("MessageBox [%s]: %s", ARG(2) ? gstr(ARG(2)) : "", ARG(1) ? gstr(ARG(1)) : "");
    RET(1, 4);
}

// ---- wsprintf: Windows' printf subset over guest arguments
static u32 wsfmt(u32 out, const char* f, u32 ap) {
    char buf[1100];
    size_t o = 0;
    while (*f && o < 1024) {
        if (*f != '%') { buf[o++] = *f++; continue; }
        f++;
        if (*f == '%') { buf[o++] = '%'; f++; continue; }
        char spec[32] = "%";
        size_t sl = 1;
        while (*f && strchr("-+ #0", *f) && sl < 20) spec[sl++] = *f++;
        while (isdigit((unsigned char)*f) && sl < 24) spec[sl++] = *f++;
        if (*f == '.') { spec[sl++] = *f++; while (isdigit((unsigned char)*f) && sl < 28) spec[sl++] = *f++; }
        int wide = 0;
        if (*f == 'l' || *f == 'h') { if (*f == 'l' && (f[1] == 's' || f[1] == 'c')) wide = 1; f++; }
        char conv = *f ? *f++ : 0;
        char tmp[600];
        switch (conv) {
            case 'd': case 'i': spec[sl++] = 'd'; spec[sl] = 0; snprintf(tmp, sizeof tmp, spec, (s32)rd32(ap)); ap += 4; break;
            case 'u': spec[sl++] = 'u'; spec[sl] = 0; snprintf(tmp, sizeof tmp, spec, rd32(ap)); ap += 4; break;
            case 'x': case 'X': spec[sl++] = conv; spec[sl] = 0; snprintf(tmp, sizeof tmp, spec, rd32(ap)); ap += 4; break;
            case 'c': case 'C': tmp[0] = (char)rd32(ap); tmp[1] = 0; ap += 4; break;
            case 's': case 'S': {
                u32 p = rd32(ap); ap += 4;
                char sv[512] = "(null)";
                if (p) {
                    if (wide || conv == 'S') { int i = 0; for (; i < 511 && rd16(p + 2 * i); ++i) sv[i] = (char)rd16(p + 2 * i); sv[i] = 0; }
                    else snprintf(sv, sizeof sv, "%s", gstr(p));
                }
                spec[sl++] = 's'; spec[sl] = 0;
                snprintf(tmp, sizeof tmp, spec, sv);
                break;
            }
            default: tmp[0] = 0;
        }
        size_t tl = strlen(tmp);
        if (o + tl > 1024) tl = 1024 - o;
        memcpy(buf + o, tmp, tl);
        o += tl;
    }
    buf[o] = 0;
    gput(out, buf, (u32)o + 1);
    return (u32)o;
}
static void nt_wsprintfA(Cpu* c) { RETC(wsfmt(ARG(0), gstr(ARG(1)), c->r[ESP] + 12)); }
static void nt_wvsprintfA(Cpu* c) { RET(wsfmt(ARG(0), gstr(ARG(1)), ARG(2)), 3); }

// ------------------------------------------------------------------ gdi32 (inert)
static void nt_g0_1(Cpu* c) { RET(0, 1); }
static void nt_g1_1(Cpu* c) { RET(1, 1); }
static void nt_g1_2(Cpu* c) { RET(1, 2); }
static void nt_gh_1(Cpu* c) { RET(0x00050001u, 1); }
static void nt_gh_2(Cpu* c) { RET(0x00050001u, 2); }
static void nt_gh_3(Cpu* c) { RET(0x00050001u, 3); }
static void nt_gh_4(Cpu* c) { RET(0x00050001u, 4); }
static void nt_gh_5(Cpu* c) { RET(0x00050001u, 5); }
static void nt_gh_6(Cpu* c) { RET(0x00050001u, 6); }
static void nt_gh_14(Cpu* c) { RET(0x00050002u, 14); }
static void nt_GetDeviceCaps(Cpu* c) {
    switch (ARG(1)) {
        case 8: RET(g_view.w, 2);    // HORZRES
        case 10: RET(g_view.h, 2);   // VERTRES
        case 12: RET(32, 2);         // BITSPIXEL
        case 14: RET(1, 2);          // PLANES
        case 24: RET(256, 2);        // NUMCOLORS
        case 38: RET(0x7E99, 2);     // RASTERCAPS
        case 88: case 90: RET(96, 2);// LOGPIXELS
        case 104: RET(256, 2);       // SIZEPALETTE
        case 116: RET(60, 2);        // VREFRESH
    }
    RET(0, 2);
}
static void nt_GetTextMetricsA(Cpu* c) {
    u32 p = ARG(1);
    memset(H(p), 0, 56);
    wr32(p, 16); wr32(p + 4, 13); wr32(p + 8, 3); wr32(p + 20, 8); wr32(p + 24, 16); wr32(p + 28, 400);
    RET(1, 2);
}
static void nt_GetTextExtentPoint32A(Cpu* c) { wr32(ARG(3), ARG(2) * 8); wr32(ARG(3) + 4, 16); RET(1, 4); }
static void nt_GetCharWidthA(Cpu* c) { for (u32 i = ARG(1); i <= ARG(2); ++i) wr32(ARG(3) + 4 * (i - ARG(1)), 8); RET(1, 4); }
static void nt_GetCharABCWidthsA(Cpu* c) { for (u32 i = ARG(1); i <= ARG(2); ++i) { u32 p = ARG(3) + 12 * (i - ARG(1)); wr32(p, 0); wr32(p + 4, 8); wr32(p + 8, 0); } RET(1, 4); }
static void nt_GetSystemPaletteEntries(Cpu* c) { memset(H(ARG(3)), 0, ARG(2) * 4); RET(ARG(2), 4); }
static void nt_GetPixel(Cpu* c) { RET(0, 3); }
static void nt_GetDIBits(Cpu* c) { RET(0, 7); }
static void nt_GetRegionData(Cpu* c) { RET(0, 3); }
static void nt_ExtTextOutA(Cpu* c) { RET(1, 8); }
static void nt_GdiFlush(Cpu* c) { RET(1, 0); }

#define E(dll, n) {dll, #n, 0, nt_##n}
#define S(dll, n, fn) {dll, #n, 0, fn}
NATIVE_TABLE(user,
    E(U, AdjustWindowRectEx), E(U, BeginPaint), E(U, CallWindowProcA), E(U, ChangeDisplaySettingsA), E(U, CharLowerBuffA),
    E(U, CharNextA), E(U, ClientToScreen), E(U, CloseClipboard), E(U, CopyRect), E(U, CreateCursor), E(U, CreateWindowExA),
    E(U, DefDlgProcA), E(U, DefWindowProcA), E(U, DestroyCursor), E(U, DestroyWindow), E(U, DialogBoxParamA),
    E(U, DispatchMessageA), E(U, DrawEdge), E(U, DrawFocusRect), E(U, DrawTextA), E(U, EmptyClipboard), E(U, EnableWindow),
    E(U, EndDialog), E(U, EndPaint), E(U, FindWindowA), E(U, FindWindowExA), E(U, GetActiveWindow), E(U, GetAsyncKeyState),
    E(U, GetClassInfoA), E(U, GetClassLongA), E(U, GetClassNameA), E(U, GetClientRect), E(U, GetClipboardData),
    E(U, GetCursorPos), E(U, GetDC), E(U, GetDesktopWindow), E(U, GetDlgCtrlID), E(U, GetDlgItem), E(U, GetDlgItemTextA),
    E(U, GetFocus), E(U, GetForegroundWindow), E(U, GetKeyState), E(U, GetKeyboardLayout), E(U, GetMessageA),
    E(U, GetMonitorInfoA), E(U, GetParent), E(U, GetPropA), E(U, GetSystemMetrics), E(U, GetTopWindow), E(U, GetUpdateRgn),
    E(U, GetWindow), E(U, GetWindowLongA), E(U, GetWindowPlacement), E(U, GetWindowRect), E(U, GetWindowTextA),
    E(U, GetWindowTextLengthA), E(U, GetWindowThreadProcessId), E(U, IntersectRect), E(U, InvalidateRect),
    E(U, IsClipboardFormatAvailable), E(U, IsDialogMessageA), E(U, IsIconic), E(U, IsWindow), E(U, IsWindowEnabled),
    E(U, IsWindowVisible), E(U, LoadAcceleratorsA), E(U, LoadCursorA), E(U, LoadIconA), E(U, LoadImageA), E(U, LoadStringA),
    E(U, MessageBoxA), E(U, MonitorFromWindow), E(U, OpenClipboard), E(U, PeekMessageA), E(U, PostMessageA),
    E(U, PostQuitMessage), E(U, PtInRect), E(U, RegisterClassA), E(U, RegisterWindowMessageA), E(U, ReleaseDC),
    E(U, RemovePropA), E(U, ScreenToClient), E(U, SendDlgItemMessageA), E(U, SendMessageA), E(U, SetActiveWindow),
    E(U, SetClassLongA), E(U, SetClipboardData), E(U, SetCursor), E(U, SetCursorPos), E(U, SetDlgItemTextA), E(U, SetFocus),
    E(U, SetForegroundWindow), E(U, SetPropA), E(U, SetRect), E(U, SetWindowLongA), E(U, SetWindowPos), E(U, ShowCursor),
    E(U, ShowWindow), E(U, SystemParametersInfoA), E(U, TranslateAcceleratorA), E(U, TranslateMessage),
    E(U, UnregisterClassA), E(U, UpdateWindow), E(U, wsprintfA), E(U, wvsprintfA),
    S(GD, CombineRgn, nt_gh_4), S(GD, CreateBitmap, nt_gh_5), S(GD, CreateCompatibleDC, nt_gh_1), S(GD, CreateDCA, nt_gh_4),
    S(GD, CreateDIBitmap, nt_gh_6), S(GD, CreateFontA, nt_gh_14), S(GD, CreatePalette, nt_gh_1), S(GD, CreateRectRgn, nt_gh_4),
    S(GD, DeleteDC, nt_g1_1), S(GD, DeleteObject, nt_g1_1), E(GD, ExtTextOutA), E(GD, GdiFlush), S(GD, GdiSetBatchLimit, nt_g1_1),
    E(GD, GetCharABCWidthsA), E(GD, GetCharWidthA), S(GD, GetCurrentObject, nt_gh_2), E(GD, GetDIBits), E(GD, GetDeviceCaps),
    E(GD, GetPixel), E(GD, GetRegionData), S(GD, GetStockObject, nt_gh_1), E(GD, GetSystemPaletteEntries),
    E(GD, GetTextExtentPoint32A), E(GD, GetTextMetricsA), S(GD, RealizePalette, nt_g0_1), S(GD, RectInRegion, nt_g1_2),
    S(GD, Rectangle, nt_gh_5), S(GD, SelectObject, nt_gh_2), S(GD, SelectPalette, nt_gh_3), S(GD, SetBkColor, nt_g1_2),
    S(GD, SetBkMode, nt_g1_2), S(GD, SetPaletteEntries, nt_gh_4), S(GD, SetTextAlign, nt_g1_2), S(GD, SetTextColor, nt_g1_2),
)
