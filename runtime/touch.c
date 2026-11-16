// Touch controls for phones, in the style of Diablo II: Resurrected's mobile-like pad:
//  - a floating stick on the left: the hero walks where it points (the cursor is held ahead of the hero);
//  - ATK (left skill) and SK (right skill) on the right, aimed at the nearest monster automatically;
//  - potions 1-4 above them, small panel keys in the top right corner;
//  - anything else on the picture is the mouse (tap / drag = left button, long press = right button).
// Everything is drawn over the picture with a small built-in 5x7 font (no SDL_ttf on Android).
// The monster list comes straight from the game's memory (Diablo II 1.13c client structures).
#include "native.h"
#include "win.h"
#include <SDL.h>
#include <GLES3/gl3.h>
#include <math.h>

#define VK_ESC 0x1B
#define VK_TAB 0x09
#define VK_ALT 0x12
#define VK_SHIFT 0x10

enum { SP_NONE, SP_KB, SP_ALT, SP_ATK, SP_SKILL };
typedef struct {
    const char* label;
    int vk;            // key sent while held; 0 = special
    int special;
    int round;
    float x, y, w, h;  // drawable pixels (computed)
    int finger;        // holding finger + 1
} Btn;

static Btn g_btn[] = {
    {"ATK", 0, SP_ATK, 1}, {"SK", 0, SP_SKILL, 1},
    {"1", '1', 0, 1}, {"2", '2', 0, 1}, {"3", '3', 0, 1}, {"4", '4', 0, 1},
    {"I", 'I'}, {"C", 'C'}, {"T", 'T'}, {"S", 'S'}, {"TAB", VK_TAB}, {"W", 'W'},
    {"Q", 'Q'}, {"RUN", 'R'}, {"ALT", 0, SP_ALT}, {"ESC", VK_ESC}, {"KB", 0, SP_KB},
};
#define NBTN (int)(sizeof g_btn / sizeof g_btn[0])
#define CELL_RING NBTN
#define CELL_KNOB (NBTN + 1)
#define NCELL (NBTN + 2)
static int g_on = -1;
static int g_alt_on;
static int g_ww, g_wh;                // drawable size
static float g_px, g_py, g_pw, g_ph;  // the picture in drawable pixels

// ------------------------------------------------------------------ game state (1.13c)
static u32 r32(u32 a) { return a >= 0x10000 ? rd32(a) : 0; }
static u32 r16(u32 a) { return a >= 0x10000 ? rd16(a) : 0; }
static u32 player(void) { return r32(0x6FBCBBFC); }
static int unit_xy(u32 u, float* x, float* y) {
    u32 p = r32(u + 0x2C);
    if (!p) return 0;
    *x = r16(p + 2) + r16(p + 0) / 65536.f;
    *y = r16(p + 6) + r16(p + 4) / 65536.f;
    return 1;
}
// the hero's feet on the screen (game pixels); the view follows the hero
static float hero_sx(void) { return g_view.w / 2.f; }
static float hero_sy(void) { static float y = -1; if (y < 0) { const char* e = getenv("D2HEROY"); y = e ? (float)atof(e) : 283.f; } return y; }
static int g_aim_all = -1;  // tests: NPCs count as targets too, and the target is marked (D2AIMALL)
static int hostile(u32 u) {
    if (r32(u) != 1) return 0;
    u32 mode = r32(u + 0x10);
    if (mode == 0 || mode == 12) return 0;  // dying / dead
    u32 txt = r32(u + 4);
    if (txt == 271 || txt == 338 || txt == 359 || txt == 560 || txt == 561) return 0;  // hirelings
    u32 md = r32(u + 0x14), ms = md ? r32(md) : 0;
    if (!ms) return 0;
    if (g_aim_all < 0) g_aim_all = getenv("D2AIMALL") != NULL;
    if ((r32(ms + 0xC) & 0x300) && !g_aim_all) return 0;  // npc / interact
    return 1;
}
// nearest live monster on the screen: its body in game pixels; returns the unit or 0
static u32 find_target(int* sx, int* sy) {
    u32 pl = player();
    float hx, hy;
    if (!pl || !unit_xy(pl, &hx, &hy)) return 0;
    u32 room = r32(r32(pl + 0x2C) + 0x1C);
    if (!room) return 0;
    u32 near = r32(room), n = r32(room + 0x24);
    if (n > 64) return 0;
    u32 best = 0;
    float bd = 1e9f;
    for (u32 k = 0; k < n; ++k) {
        u32 rm = r32(near + 4 * k);
        int guard = 0;
        for (u32 u = r32(rm + 0x74); u && guard < 512; u = r32(u + 0xE8), ++guard) {
            if (u == pl || !hostile(u)) continue;
            float x, y;
            if (!unit_xy(u, &x, &y)) continue;
            float dx = x - hx, dy = y - hy;
            float px = hero_sx() + (dx - dy) * 16.f, py = hero_sy() + (dx + dy) * 8.f;
            if (px < 8 || px > g_view.w - 8 || py < 40 || py > g_view.h - 60) continue;
            float d = dx * dx + dy * dy;
            if (d < bd) { bd = d; best = u; *sx = (int)px; *sy = (int)py - 22; }
        }
    }
    return best;
}

// A panel or dialog is up (inventory, character, NPC talk / trade, stash, waypoint, Esc menu...): D2Client's UI
// flags (1.13c 0x6FBAAD80, one dword per panel). Found by diffing memory with the inventory shut and open; the
// entries that are always on in a game (0, 6, 7, 0x15, 0x22, 0x23, 0x26), the automap (0x0A) and Alt (0x0D) don't count.
static int panel_open(void) {
    static const u32 ignore = 1u << 0 | 1u << 6 | 1u << 7 | 1u << 0x0A | 1u << 0x0D | 1u << 0x15;
    for (int i = 1; i < 0x22; ++i) if (!(ignore >> i & 1) && r32(0x6FBAAD80 + 4 * i)) return 1;
    return 0;
}
// A fingertip is wide and D2 only takes the unit right under the cursor: a tap near a town NPC or an item on the
// ground goes onto it. Screen point in game pixels (lifted to the NPC's body); 0 when nothing is near enough.
#define SNAP_R 42.f  // game pixels (~75 phone pixels): about a fingertip
static u32 snap_near(int gx, int gy, float r, int* sx, int* sy) {
    u32 pl = player();
    float hx, hy;
    if (!pl || panel_open() || gy >= g_view.h - 48 || !unit_xy(pl, &hx, &hy)) return 0;
    u32 room = r32(r32(pl + 0x2C) + 0x1C);
    if (!room) return 0;
    u32 near = r32(room), n = r32(room + 0x24);
    if (n > 64) return 0;
    float bd = r * r;
    u32 found = 0;
    for (u32 k = 0; k < n; ++k) {
        u32 rm = r32(near + 4 * k);
        int guard = 0;
        for (u32 u = r32(rm + 0x74); u && guard < 512; u = r32(u + 0xE8), ++guard) {
            u32 type = r32(u), md, ms, p;
            float x, y, lift;
            if (type == 4) {  // an item: mode 3 on the ground, 5 falling; static path: x +0xC, y +0x10
                u32 m = r32(u + 0x10);
                if ((m != 3 && m != 5) || !(p = r32(u + 0x2C))) continue;
                x = (float)r32(p + 0xC); y = (float)r32(p + 0x10); lift = 4;
            } else if (type == 1) {  // a live NPC (MonStats flags 0x300: npc / interact)
                u32 m = r32(u + 0x10);
                if (m == 0 || m == 12 || !(md = r32(u + 0x14)) || !(ms = r32(md)) || !(r32(ms + 0xC) & 0x300)) continue;
                if (!unit_xy(u, &x, &y)) continue;
                lift = 30;
            } else continue;
            float dx = x - hx, dy = y - hy;
            float px = hero_sx() + (dx - dy) * 16.f, py = hero_sy() + (dx + dy) * 8.f - lift;
            if (px < 8 || px > g_view.w - 8 || py < 40 || py > g_view.h - 60) continue;  // off screen / under the HUD
            float d = (px - gx) * (px - gx) + (py - gy) * (py - gy);
            if (d < bd) { bd = d; *sx = (int)px; *sy = (int)py; found = u; }
        }
    }
    return found;
}
static u32 snap_target(int gx, int gy, int* sx, int* sy) { return snap_near(gx, gy, SNAP_R, sx, sy); }

// Orders for the hero straight into the game's client, the way its own click handler gives them (no cursor):
// D2Client 0x6FACA010 (eax = y, ecx = x, stack: packet type, unit) paths the unit to a map point and sends
// the move to the server through the action function 0x6FADA7C0. Types: 1 walk, 3 run to a location.
// Called from the message pump (the game's main thread, between frames); the guest registers are kept.
static u32 guest_call_regs(u32 fn, u32 eax, u32 ecx, u32 ebx, u32 edi, u32 a0, u32 a1) {
    Cpu* c = cpu_current();
    if (!c) return 0;
    Cpu save = *c;
    c->r[EAX] = eax; c->r[ECX] = ecx; c->r[EBX] = ebx; c->r[EDI] = edi;
    u32 args[2] = {a0, a1};
    u32 r = call_guest(c, fn, 2, args);
    *c = save;
    if (getenv("D2WALKLOG")) LOG("guest %08X(eax %d, ecx %d, ebx %d, edi %d, %u, %08X) = %u", fn, (int)eax, (int)ecx, (int)ebx, (int)edi, a0, a1, r);
    return r;
}
static int walk_to(float wx, float wy, int run) {  // 0: no path there (a wall)
    u32 pl = player();
    if (!pl || wx < 1 || wy < 1) return 0;
    return guest_call_regs(0x6FACA010, (u32)(int)wy, (u32)(int)wx, 0, 0, run ? 3 : 1, pl) != 0;  // eax = y, ecx = x
}
// The action function itself (eax = the hero, stack: packet type, 0): 6 / 0x0D = the left / right skill on a
// unit (ebx = its type, edi = its id; the hero walks into range first), 5 / 0x0C = on a map point (ebx, edi = x, y).
static void act(u32 type, u32 ebx, u32 edi) {
    u32 pl = player();
    if (pl) guest_call_regs(0x6FADA7C0, pl, 0, ebx, edi, type, 0);
}
// the hero is busy with an attack / a spell (player modes: 7-16 attacks, casts, kicks; 18 sequence)
static int hero_busy(void) {
    u32 m = r32(player() + 0x10);
    return (m >= 7 && m <= 16) || m == 18;
}

// ------------------------------------------------------------------ 5x7 font: only the characters the labels use
static const struct { char c; u8 rows[7]; } FONT[] = {
    {'A', {14, 17, 17, 31, 17, 17, 17}}, {'B', {30, 17, 17, 30, 17, 17, 30}}, {'C', {14, 17, 16, 16, 16, 17, 14}},
    {'E', {31, 16, 16, 30, 16, 16, 31}}, {'I', {14, 4, 4, 4, 4, 4, 14}}, {'K', {17, 18, 20, 24, 20, 18, 17}},
    {'L', {16, 16, 16, 16, 16, 16, 31}}, {'M', {17, 27, 21, 21, 17, 17, 17}}, {'N', {17, 25, 21, 19, 17, 17, 17}},
    {'Q', {14, 17, 17, 17, 21, 18, 13}}, {'R', {30, 17, 17, 30, 20, 18, 17}}, {'S', {15, 16, 16, 14, 1, 1, 30}},
    {'T', {31, 4, 4, 4, 4, 4, 4}}, {'U', {17, 17, 17, 17, 17, 17, 14}}, {'W', {17, 17, 17, 21, 21, 21, 10}},
    {'1', {4, 12, 4, 4, 4, 4, 14}}, {'2', {14, 17, 1, 2, 4, 8, 31}}, {'3', {30, 1, 1, 14, 1, 1, 30}},
    {'4', {2, 6, 10, 18, 31, 2, 2}},
};
static const u8* glyph(char c) {
    for (unsigned i = 0; i < sizeof FONT / sizeof *FONT; ++i) if (FONT[i].c == c) return FONT[i].rows;
    return NULL;
}

#define CELL 128
static GLuint g_tex, g_prog, g_vao, g_vbo;
static GLint u_rect, u_uv, u_tint;

static float sat(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static void put(u8* px, int W, int cx, int x, int y, u8 r, u8 g, u8 b, u8 a) {
    u8* p = px + ((size_t)y * W + cx + x) * 4;
    p[0] = r; p[1] = g; p[2] = b; p[3] = a;
}
static void build_atlas(void) {
    int W = CELL * NCELL;
    u8* px = calloc((size_t)W * CELL, 4);
    float c0 = (CELL - 1) / 2.f;
    for (int b = 0; b < NCELL; ++b) {
        int cx = b * CELL;
        int round = b >= NBTN || g_btn[b].round;
        for (int y = 0; y < CELL; ++y)
            for (int x = 0; x < CELL; ++x) {
                float d = sqrtf((x - c0) * (x - c0) + (y - c0) * (y - c0));
                if (b == CELL_RING) {  // the stick's base: a dark glassy disc, a fine gold rim, a faint inner ring
                    float t = d / c0, px1 = 1.f / c0;  // one pixel in t
                    float disc = sat((0.9f - t) / px1);
                    float rim = sat(1.f - (fabsf(t - 0.93f) - 0.02f) / px1);
                    float inner = sat(1.f - (fabsf(t - 0.5f) - 0.005f) / px1) * 0.35f;
                    float a_disc = disc * (0.22f + 0.2f * t);
                    float a = rim > a_disc ? rim * 0.85f : a_disc;
                    if (inner > a) a = inner;
                    if (a <= 0) continue;
                    float k = rim > 0.3f || inner > a_disc ? 1.f : 0.f;
                    put(px, W, cx, x, y, (u8)(12 + k * 200), (u8)(10 + k * 165), (u8)(8 + k * 100), (u8)(a * 255));
                    continue;
                }
                if (b == CELL_KNOB) {  // the knob: a lit gold dome
                    float t = d / c0, px1 = 1.f / c0;
                    float a = sat((1.f - t) / px1);
                    if (a <= 0) continue;
                    float hx = (x - c0) / c0 + 0.35f, hy = (y - c0) / c0 + 0.35f;  // light from the top left
                    float l = 1.f - sat(sqrtf(hx * hx + hy * hy) / 1.3f);
                    float rim = sat(1.f - (fabsf(t - 0.94f) - 0.03f) / px1);
                    float r = 70 + 150 * l, g = 54 + 125 * l, bl = 30 + 80 * l;
                    r += (235 - r) * rim; g += (205 - g) * rim; bl += (140 - bl) * rim;
                    put(px, W, cx, x, y, (u8)r, (u8)g, (u8)bl, (u8)(a * 235));
                    continue;
                }
                int edge;
                if (round) {
                    if (d >= c0) continue;
                    edge = d >= c0 - 4;
                } else {
                    int ex = x < 8 ? 8 - x : x > CELL - 9 ? x - (CELL - 9) : 0;
                    int ey = y < 8 ? 8 - y : y > CELL - 9 ? y - (CELL - 9) : 0;
                    float cd = sqrtf((float)(ex * ex + ey * ey));
                    if (ex && ey && cd > 8) continue;
                    edge = x < 4 || y < 4 || x >= CELL - 4 || y >= CELL - 4 || (ex && ey && cd > 4);
                }
                if (edge) put(px, W, cx, x, y, 160, 128, 72, 230);
                else put(px, W, cx, x, y, 36, 30, 24, 160);
            }
        if (b >= NBTN) continue;
        const char* s = g_btn[b].label;
        int n = (int)strlen(s), scale = n <= 1 ? 12 : n == 2 ? 9 : 7;
        int tw = n * 6 * scale - scale, x0 = (CELL - tw) / 2, y0 = (CELL - 7 * scale) / 2;
        for (int i = 0; i < n; ++i) {
            const u8* g = glyph(s[i]);
            if (!g) continue;
            for (int r = 0; r < 7; ++r)
                for (int col = 0; col < 5; ++col)
                    if (g[r] & (16 >> col))
                        for (int dy = 0; dy < scale; ++dy)
                            for (int dx = 0; dx < scale; ++dx) {
                                int x = x0 + (i * 6 + col) * scale + dx, y = y0 + r * scale + dy;
                                if (x < 0 || x >= CELL || y < 0 || y >= CELL) continue;
                                put(px, W, cx, x, y, 235, 205, 130, 255);
                            }
        }
    }
    glGenTextures(1, &g_tex);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, CELL, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(px);
}

static const char* VS = "#version 300 es\nlayout(location=0) in vec2 p; uniform vec4 r; uniform vec4 uv; out vec2 t;\n"
    "void main(){ t = uv.xy + p*uv.zw; gl_Position = vec4(r.x + p.x*r.z, r.y - p.y*r.w, 0.0, 1.0); }\n";
static const char* FS = "#version 300 es\nprecision mediump float; in vec2 t; out vec4 o; uniform sampler2D s; uniform vec4 tint;\n"
    "void main(){ o = texture(s, t) * tint; }\n";

static GLuint sh(GLenum k, const char* src) { GLuint x = glCreateShader(k); glShaderSource(x, 1, &src, NULL); glCompileShader(x); return x; }

static void gl_setup(void) {
    g_prog = glCreateProgram();
    glAttachShader(g_prog, sh(GL_VERTEX_SHADER, VS));
    glAttachShader(g_prog, sh(GL_FRAGMENT_SHADER, FS));
    glLinkProgram(g_prog);
    u_rect = glGetUniformLocation(g_prog, "r");
    u_uv = glGetUniformLocation(g_prog, "uv");
    u_tint = glGetUniformLocation(g_prog, "tint");
    glGenVertexArrays(1, &g_vao);
    glBindVertexArray(g_vao);
    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    static const float q[] = {0, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 1};
    glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, 0);
    build_atlas();
}

// ------------------------------------------------------------------ layout (all relative to the height)
static void place(Btn* b, float cx, float cy, float d) { b->x = cx - d / 2; b->y = cy - d / 2; b->w = b->h = d; }
static int g_lw, g_lh;  // the game size the layout was made for (it changes: 640x480 splash, then the game's)
static void layout(int ww, int wh) {
    g_ww = ww; g_wh = wh; g_lw = g_view.w; g_lh = g_view.h;
    float s = (float)ww / g_view.w < (float)wh / g_view.h ? (float)ww / g_view.w : (float)wh / g_view.h;
    g_pw = g_view.w * s; g_ph = g_view.h * s;
    g_px = (ww - g_pw) / 2; g_py = (wh - g_ph) / 2;
    float H = (float)wh, R = (float)ww;
    place(&g_btn[0], R - 0.17f * H, 0.64f * H, 0.25f * H);   // ATK
    place(&g_btn[1], R - 0.42f * H, 0.74f * H, 0.17f * H);   // SK
    for (int i = 0; i < 4; ++i)                               // potions
        place(&g_btn[2 + i], R - 0.44f * H + i * 0.115f * H, 0.40f * H, 0.10f * H);
    float sz = 0.088f * H, gap = 0.01f * H;
    for (int i = 6; i < NBTN; ++i) {                          // panel keys: two rows in the top right corner
        int k = i - 6, row = k / 6, col = k % 6;
        g_btn[i].x = R - 6 * (sz + gap) + col * (sz + gap);
        g_btn[i].y = gap + row * (sz + gap);
        g_btn[i].w = g_btn[i].h = sz;
    }
}

// ------------------------------------------------------------------ the stick and the mouse
static SDL_FingerID g_stick_f = -1;
static int g_stick_new;                // the stick was just touched: order at once
static float g_sx0, g_sy0, g_sx, g_sy;  // stick centre / finger (drawable pixels)
static float g_dirx = 1, g_diry = 0;   // last walking direction (unit vector)
static int g_lb, g_rb, g_shift;        // buttons this layer holds down
static int g_mx, g_my;
static u32 g_target;                   // unit ATK/SK is held on
static int g_atk, g_skill;             // ATK / SK held
static int g_sk_pick;                  // this SK press went to an item / NPC instead of the skill

static void mouse_to(int x, int y) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= g_view.w) x = g_view.w - 1;
    if (y >= g_view.h) y = g_view.h - 1;
    g_mx = x; g_my = y;
    input_mouse(x, y, 0, 0);
}
static void button(int which, int down) {  // 1 left, 2 right
    int* st = which == 1 ? &g_lb : &g_rb;
    if (*st == down) return;
    *st = down;
    input_mouse(g_mx, g_my, which, down);
}
static void shift(int down) { if (g_shift != down) { g_shift = down; input_key(VK_SHIFT, down); } }

static float stick_r(void) { return g_wh * 0.15f; }
// below 0.78: the skill list pops up there
static int stick_zone(float x, float y) { (void)x; (void)y; return 0; }  // the stick is gone (user: tap to walk)

// in the menus (no hero yet) the pad is hidden and every touch is the mouse
static int in_game(void) { return player() != 0; }
static int menu_btn(const Btn* b) { return b->special == SP_KB || b->vk == VK_ESC; }  // usable in the menus too
static int btn_live(const Btn* b) { return in_game() || menu_btn(b); }

void touch_draw(int ww, int wh) {
    if (g_on <= 0) return;
    if (ww != g_ww || wh != g_wh || g_view.w != g_lw || g_view.h != g_lh) layout(ww, wh);
    int menus = !in_game();
    if (g_aim_all < 0) g_aim_all = getenv("D2AIMALL") != NULL;
    if (!g_prog) gl_setup();
    if (ww != g_ww || wh != g_wh || g_view.w != g_lw || g_view.h != g_lh) layout(ww, wh);
    glUseProgram(g_prog);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glUniform1i(glGetUniformLocation(g_prog, "s"), 0);
    glBindVertexArray(g_vao);
    for (int i = 0; i < NBTN; ++i) {
        Btn* b = &g_btn[i];
        if (menus && !menu_btn(b)) continue;
        int lit = b->finger || (b->special == SP_ALT && g_alt_on) || (b->special == SP_KB && SDL_IsTextInputActive());
        float a = i < 6 ? 0.75f : 0.6f;
        glUniform4f(u_tint, lit ? 1.4f : 1.f, lit ? 1.25f : 1.f, lit ? 0.9f : 1.f, lit ? 1.f : a);
        glUniform4f(u_rect, -1.f + 2.f * b->x / ww, 1.f - 2.f * b->y / wh, 2.f * b->w / ww, 2.f * b->h / wh);
        glUniform4f(u_uv, (float)i / NCELL, 0.f, 1.f / NCELL, 1.f);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    if (menus) { glDisable(GL_BLEND); return; }
    if (g_aim_all > 0) {  // tests: mark the auto-aim target and the hero
        int tx, ty;
        float pts[2][2] = {{hero_sx(), hero_sy()}, {-1, -1}};
        if (find_target(&tx, &ty)) { pts[1][0] = (float)tx; pts[1][1] = (float)ty; }
        for (int i = 0; i < 2; ++i) {
            if (pts[i][0] < 0) continue;
            float mx = g_px + pts[i][0] / g_view.w * g_pw, my = g_py + pts[i][1] / g_view.h * g_ph, m = wh * 0.012f;
            glUniform4f(u_tint, i ? 1.f : 0.3f, i ? 0.2f : 1.f, 0.2f, 1.f);
            glUniform4f(u_rect, -1.f + 2.f * (mx - m) / ww, 1.f - 2.f * (my - m) / wh, 4.f * m / ww, 4.f * m / wh);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }
    }
    glDisable(GL_BLEND);
}

// ------------------------------------------------------------------ input
static SDL_FingerID g_game_finger = -1;
static u32 g_down_ms;
static float g_down_x, g_down_y;
static int g_right;  // the game finger is acting as the right button
static int g_moved;  // it moved far: dragging, not a long press
// A tap snapped onto an NPC / item: the cursor sits there first and the button goes down a little later, once the
// game has drawn a frame with the unit under the cursor (its hover is what the click acts on).
#define SNAP_HOVER_MS 100u  // ms; a few frames at 25-60 fps
static int g_snap;         // 1 waiting for the hover, 2 button down
static int g_snap_up;      // the finger has lifted
static int g_snx, g_sny;
static u32 g_snap_ms;

static void to_game(float x, float y, int* gx, int* gy) {
    *gx = (int)((x - g_px) / g_pw * g_view.w);
    *gy = (int)((y - g_py) / g_ph * g_view.h);
    if (*gx < 0) *gx = 0;
    if (*gy < 0) *gy = 0;
    if (*gx >= g_view.w) *gx = g_view.w - 1;
    if (*gy >= g_view.h) *gy = g_view.h - 1;
}

static void release_all(void) {
    button(1, 0); button(2, 0); shift(0);
    g_target = 0;
}
static void drop_game_finger(void) {
    if (g_snap) { if (g_snap == 2) input_mouse(g_snx, g_sny, 1, 0); g_snap = 0; g_game_finger = -1; return; }
    if (g_game_finger == -1) return;
    input_mouse(g_mx, g_my, g_right ? 2 : 1, 0);
    g_game_finger = -1;
}

static void press(Btn* b, int down) {
    if (b->special == SP_SKILL && down && in_game()) {  // SK: the nearest item / NPC on the screen first
        int sx, sy;
        u32 u = snap_near((int)hero_sx(), (int)hero_sy(), 2000.f, &sx, &sy);
        if (u) {
            if (getenv("D2SNAPLOG")) LOG("touch: SK on unit type %u id %u", r32(u), r32(u + 0xC));
            drop_game_finger();
            release_all();
            act(6, r32(u), r32(u + 0xC));  // the left-click action on that unit: walk there, then talk / pick up
            g_sk_pick = 1;  // its release must not stop a skill it never started
            return;
        }
    }
    if (b->special == SP_SKILL && !down && g_sk_pick) { g_sk_pick = 0; return; }
    if (b->special == SP_ATK || b->special == SP_SKILL) {
        if (b->special == SP_ATK) g_atk = down; else g_skill = down;
        if (down) drop_game_finger();
        release_all();  // the next tick aims afresh
        return;
    }
    if (b->vk) { input_key(b->vk, down); return; }
    if (!down) return;
    if (b->special == SP_KB) { if (SDL_IsTextInputActive()) SDL_StopTextInput(); else SDL_StartTextInput(); }
    if (b->special == SP_ALT) { g_alt_on ^= 1; input_key(VK_ALT, g_alt_on); }
}

// Returns 1 when the event was a touch (handled here).
int touch_event(const SDL_Event* e) {
    if (e->type != SDL_FINGERDOWN && e->type != SDL_FINGERUP && e->type != SDL_FINGERMOTION) return 0;
    if (g_on <= 0 || !g_ww) return 1;
    float x = e->tfinger.x * g_ww, y = e->tfinger.y * g_wh;
    SDL_FingerID f = e->tfinger.fingerId;
    if (e->type == SDL_FINGERDOWN) {
        for (int i = 0; i < NBTN; ++i) {
            Btn* b = &g_btn[i];
            if (!btn_live(b)) continue;
            float pad = b->round ? b->w * 0.1f : 0;
            if (x >= b->x - pad && x < b->x + b->w + pad && y >= b->y - pad && y < b->y + b->h + pad) {
                b->finger = (int)f + 1;
                press(b, 1);
                return 1;
            }
        }
        if (g_stick_f == -1 && in_game() && stick_zone(x, y)) {
            g_stick_f = f;
            g_stick_new = 1;
            g_sx0 = g_sx = x; g_sy0 = g_sy = y;
            drop_game_finger();
            return 1;
        }
        if (g_game_finger != -1 || g_stick_f != -1 || g_atk || g_skill) return 1;  // one thing drives the mouse
        g_game_finger = f;
        g_down_ms = SDL_GetTicks();
        g_down_x = x; g_down_y = y;
        g_moved = 0;
        int gx, gy;
        to_game(x, y, &gx, &gy);
        g_right = 0;
        if (snap_target(gx, gy, &g_snx, &g_sny)) {
            if (getenv("D2SNAPLOG")) LOG("touch: tap %d,%d snapped to %d,%d", gx, gy, g_snx, g_sny);
            mouse_to(g_snx, g_sny);
            g_snap = 1; g_snap_up = 0; g_snap_ms = g_down_ms;
            return 1;
        }
        mouse_to(gx, gy);
        input_mouse(gx, gy, 1, 1);
        return 1;
    }
    if (e->type == SDL_FINGERUP) {
        for (int i = 0; i < NBTN; ++i)
            if (g_btn[i].finger == (int)f + 1) { g_btn[i].finger = 0; press(&g_btn[i], 0); return 1; }
        if (f == g_stick_f) {
            g_stick_f = -1;
            { u32 pl = player(); float hx, hy; if (pl && unit_xy(pl, &hx, &hy)) walk_to(hx, hy, 0); }  // stop here
            if (!g_atk && !g_skill) release_all();
            return 1;
        }
        if (f == g_game_finger && g_snap) { g_snap_up = 1; g_game_finger = -1; return 1; }  // touch_tick releases
        if (f == g_game_finger) {
            int gx, gy;
            to_game(x, y, &gx, &gy);
            mouse_to(gx, gy);
            input_mouse(gx, gy, g_right ? 2 : 1, 0);
            g_game_finger = -1;
        }
        return 1;
    }
    // motion
    if (f == g_stick_f) {
        float dx = x - g_sx0, dy = y - g_sy0, d = sqrtf(dx * dx + dy * dy), r = stick_r();
        if (d > r) { dx *= r / d; dy *= r / d; }
        g_sx = g_sx0 + dx; g_sy = g_sy0 + dy;
        return 1;
    }
    if (f == g_game_finger && g_snap) return 1;  // the cursor stays on the NPC / item
    if (f == g_game_finger) {
        float dx = x - g_down_x, dy = y - g_down_y;
        if (dx * dx + dy * dy > (g_wh * 0.03f) * (g_wh * 0.03f)) g_moved = 1;
        int gx, gy;
        to_game(x, y, &gx, &gy);
        mouse_to(gx, gy);
    }
    return 1;
}

// The phone keyboard follows the game's text fields: D2Win's focused control (0x6F9014AC, 1.13c) being an
// edit box (type 1: the character name, game names, passwords) opens it, losing it closes it again.
static void auto_keyboard(void) {
    static int opened;
    u32 ctl = r32(0x6F9014AC);
    int edit = ctl && r32(ctl) == 1;
    if (edit && !opened && !SDL_IsTextInputActive()) { SDL_StartTextInput(); opened = 1; }
    else if (!edit && opened) { if (SDL_IsTextInputActive()) SDL_StopTextInput(); opened = 0; }
}

int touch_enabled(void) {
    if (g_on < 0) {
#ifdef __ANDROID__
        g_on = 1;
#else
        g_on = getenv("D2TOUCH") != NULL;
#endif
    }
    return g_on;
}

// Every few ms from the message pump: the stick walks, ATK / SK follow their target, long presses.
void touch_tick(void) {
    if (!touch_enabled()) return;
    auto_keyboard();
    if (!g_ww) return;
    if (g_snap) {  // a snapped tap: hover, button down, and up once the finger has lifted
        u32 now = SDL_GetTicks();
        if (g_snap == 1 && now - g_snap_ms >= SNAP_HOVER_MS) { input_mouse(g_snx, g_sny, 1, 1); g_snap = 2; g_snap_ms = now; }
        else if (g_snap == 2 && g_snap_up && now - g_snap_ms >= SNAP_HOVER_MS) { input_mouse(g_snx, g_sny, 1, 0); g_snap = 0; }
    }
    if (!in_game() && (g_stick_f != -1 || g_atk || g_skill)) {  // the game ended under a held pad
        g_stick_f = -1; g_atk = g_skill = 0;
        for (int i = 0; i < NBTN; ++i) g_btn[i].finger = 0;
        release_all();
    }
    if (g_atk || g_skill) {
        // ATK / SK: the skill straight on the nearest monster, again whenever the hero is free (no cursor)
        static u32 last;
        u32 now = SDL_GetTicks();
        if (hero_busy() || now - last < 120) return;
        last = now;
        int sx, sy;
        u32 t = find_target(&sx, &sy);
        if (t) {
            g_target = t;
            act(g_atk ? 6 : 0x0D, r32(t), r32(t + 0xC));
        } else {
            // nothing near: swing / cast where the hero faces
            u32 pl = player();
            float hx, hy;
            if (!pl || !unit_xy(pl, &hx, &hy)) return;
            float sxp = g_dirx * 32.f, syp = g_diry * 16.f;
            float mx = (sxp / 16.f + syp / 8.f) / 2.f, my = (syp / 8.f - sxp / 16.f) / 2.f;
            act(g_atk ? 5 : 0x0C, (u32)(int)(hx + mx * 1.5f), (u32)(int)(hy + my * 1.5f));
        }
        return;
    }
    if (g_stick_f != -1) {
        // one order towards a point well ahead; a new one only when the stick turns or the hero gets close
        // (re-ordering all the time restarts the walk and makes the hero stutter)
        static float last_dx, last_dy, tx, ty;
        static int last_run;
        float dx = g_sx - g_sx0, dy = g_sy - g_sy0, d = sqrtf(dx * dx + dy * dy), r = stick_r();
        if (d < r * 0.2f) return;
        float ux = dx / d, uy = dy / d;
        int run = d > r * 0.6f;
        u32 pl = player();
        float hx, hy;
        if (!pl || !unit_xy(pl, &hx, &hy)) return;
        float left = sqrtf((tx - hx) * (tx - hx) + (ty - hy) * (ty - hy));
        if (getenv("D2WALKLOG")) LOG("stick: hero %.1f,%.1f target %.1f,%.1f left %.1f dot %.2f run %d/%d new %d", hx, hy, tx, ty, left, ux * last_dx + uy * last_dy, run, last_run, g_stick_new);
        if (g_stick_new == 0 && ux * last_dx + uy * last_dy > 0.94f && run == last_run && left > 3.f) return;
        g_stick_new = 0;
        last_dx = ux; last_dy = uy; last_run = run;
        g_dirx = ux; g_diry = uy;
        // the screen direction on the map (screen = ((x-y)*16, (x+y)*8))
        float sxp = ux * 16.f, syp = uy * 8.f * 1.4f;
        float mx = (sxp / 16.f + syp / 8.f) / 2.f, my = (syp / 8.f - sxp / 16.f) / 2.f;
        float ml = sqrtf(mx * mx + my * my);
        if (ml < 1e-4f) return;
        for (float reach = 9.f; reach >= 2.f; reach -= 2.f) {  // shorter when the far point is in a wall
            tx = hx + mx / ml * reach; ty = hy + my / ml * reach;
            if (walk_to(tx, ty, run)) return;
        }
        return;
    }
    // Long press (still finger, 450 ms): the left button becomes the right one (cast the right skill there).
    if (g_game_finger == -1 || g_right || g_moved || g_snap) return;
    if (SDL_GetTicks() - g_down_ms < 450) return;
    int gx, gy;
    to_game(g_down_x, g_down_y, &gx, &gy);
    input_mouse(gx, gy, 1, 0);
    input_mouse(gx, gy, 2, 1);
    g_right = 1;
}

// tests: where the aim would go now (autoplay "aim")
int touch_aim_test(int* sx, int* sy) { return find_target(sx, sy) != 0; }
// tests (autoplay "npctap=dx,dy"): a finger tap (dx, dy) game pixels off the NPC / item nearest the hero
int touch_npctap_test(int dx, int dy) {
    int sx, sy;
    if (!g_ww || !snap_near((int)hero_sx(), (int)hero_sy(), 2000.f, &sx, &sy)) return 0;
    SDL_Event ev = {0};
    ev.tfinger.fingerId = 7;
    ev.tfinger.x = (g_px + (sx + dx) * g_pw / g_view.w) / g_ww;
    ev.tfinger.y = (g_py + (sy + dy) * g_ph / g_view.h) / g_wh;
    ev.type = SDL_FINGERDOWN; SDL_PushEvent(&ev);
    ev.type = SDL_FINGERUP; SDL_PushEvent(&ev);
    LOG("auto: npctap target %d,%d tap %d,%d", sx, sy, sx + dx, sy + dy);
    return 1;
}
