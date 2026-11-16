// glide3x on OpenGL ES 3: the game (D2Glide.dll) renders 2D sprites as textured fans in window
// coordinates with 8-bit palettized textures. Textures live in an emulated TMU memory (start address ->
// texture); palettized ones are R8 index textures resolved through a 256x1 palette texture in the shader,
// so palette changes cost one small upload. Draws are batched while the state is unchanged, rendered into
// an FBO at the game's resolution, and grBufferSwap scales it into the window (with the gamma table).
#include "native.h"
#include "win.h"
#include <SDL.h>
#include <GLES3/gl3.h>

#define GL3 "glide3x.dll"

static SDL_Window* g_win;
static SDL_GLContext g_gl;
static GLuint g_fbo, g_fbtex, g_prog, g_blit, g_vbo, g_vao, g_paltex, g_gammatex, g_lfbtex;
static GLint u_scale, u_texmode, u_cc, u_cinv, u_ac, u_ainv, u_constc, u_chroma_on, u_chroma, u_pal;
static GLint ub_tex, ub_gamma, ub_rect;
static int g_open;

int glide_window_ready(void) { return g_open; }


// ------------------------------------------------------------------ state
typedef struct {
    u32 addr, size;
    int w, h, fmt;
    GLuint tex;
} Tex;
#define MAXTEX 16384
static Tex g_tex[MAXTEX];
static int g_ntex;
static int g_cur = -1;          // the texture grTexSource selected
static float g_sw = 256, g_th = 256;  // s/t scale of the current texture
static u32 g_pal[256];

typedef struct {
    int texmode;                // 0 none, 1 palette, 2 rgba
    GLuint tex;
    int cc[4], cinv, ac[4], ainv;
    u32 constc;
    int chroma_on;
    u32 chroma;
    int blend[4];
    int cmask;
} State;
static State g_st, g_drawn;     // current state, state of the queued batch
static int g_colormask = 1;
static int g_cformat;  // grSstWinOpen: 0 ARGB, 1 ABGR, 2 RGBA, 3 BGRA

// a GrColor_t of the window's color format -> 0xAARRGGBB
static u32 to_argb(u32 v) {
    switch (g_cformat) {
        case 1: return (v & 0xFF00FF00u) | ((v >> 16) & 0xFF) | ((v & 0xFF) << 16);  // ABGR
        case 2: return (v >> 8) | (v << 24);                                        // RGBA
        case 3: return __builtin_bswap32(v);                                         // BGRA
    }
    return v;
}

typedef struct { float x, y, u, v; u32 c; float ti; } Vtx;  // ti: the batch's texture slot (see begin_draw)
#define MAXV 65536
static Vtx g_v[MAXV];
static int g_nv;
// One batch samples up to NSLOT textures (units 0..NSLOT-1, palette on unit NSLOT): the automap and the world
// switch textures every sprite, and a flush per switch cost 1300 draw calls a frame with the map open.
#define NSLOT 8
static GLuint g_slots[NSLOT];
static int g_nslots;
static float g_slot;  // the slot of the texture the next vertices sample
static u32 g_draws;  // draw calls (flushes), logged with the frame rate

// vertex layout (byte offsets into the game's vertex, -1 = absent)
static int L_xy = 0, L_q = -1, L_st0 = -1, L_pargb = -1, L_rgb = -1, L_a = -1;

// ------------------------------------------------------------------ GL setup
static const char* VS =
    "#version 300 es\n"
    "layout(location=0) in vec2 pos; layout(location=1) in vec2 uv; layout(location=2) in vec4 col; layout(location=3) in float ti;\n"
    "uniform vec2 scale; out vec2 vuv; out vec4 vcol; flat out int vti;\n"
    "void main(){ vuv = uv; vcol = col; vti = int(ti + 0.5); gl_Position = vec4(pos.x*scale.x-1.0, 1.0-pos.y*scale.y, 0.0, 1.0); }\n";
static const char* FS =
    "#version 300 es\nprecision highp float; precision highp int;\n"
    "in vec2 vuv; in vec4 vcol; flat in int vti; out vec4 o;\n"
    "uniform sampler2D t0, t1, t2, t3, t4, t5, t6, t7; uniform sampler2D pal; uniform int texmode;\n"
    // the batch slot's texture (no mipmaps: an explicit LOD keeps the branches well defined)
    "vec4 samp(vec2 p){ if (vti < 4) { if (vti < 2) return vti == 0 ? textureLod(t0, p, 0.0) : textureLod(t1, p, 0.0);\n"
    "    return vti == 2 ? textureLod(t2, p, 0.0) : textureLod(t3, p, 0.0); }\n"
    "  if (vti < 6) return vti == 4 ? textureLod(t4, p, 0.0) : textureLod(t5, p, 0.0);\n"
    "  return vti == 6 ? textureLod(t6, p, 0.0) : textureLod(t7, p, 0.0); }\n"
    "uniform ivec4 cc; uniform int cinv; uniform ivec4 ac; uniform int ainv;\n"
    "uniform vec4 constc; uniform int chroma_on; uniform vec3 chroma;\n"
    "vec4 texel(){ if (texmode==1){ float i = samp(vuv).r; return textureLod(pal, vec2((i*255.0+0.5)/256.0, 0.5), 0.0); }\n"
    "  if (texmode==2) return samp(vuv); return vec4(1.0); }\n"
    "vec3 factor3(int f, vec3 local, float la, float oa, vec4 t){\n"
    "  if (f==0) return vec3(0.0); if (f==1) return local; if (f==2) return vec3(oa); if (f==3) return vec3(la);\n"
    "  if (f==4) return vec3(t.a); if (f==5) return t.rgb; if (f==8) return vec3(1.0); if (f==9) return 1.0-local;\n"
    "  if (f==10) return vec3(1.0-oa); if (f==11) return vec3(1.0-la); if (f==12) return vec3(1.0-t.a); return vec3(1.0); }\n"
    "float factor1(int f, float local, float oa, vec4 t){\n"
    "  if (f==0) return 0.0; if (f==1||f==3) return local; if (f==2) return oa; if (f==4) return t.a; if (f==8) return 1.0;\n"
    "  if (f==9||f==11) return 1.0-local; if (f==10) return 1.0-oa; if (f==12) return 1.0-t.a; return 1.0; }\n"
    "vec3 comb3(int fn, vec3 f, vec3 local, vec3 other, float la){\n"
    "  if (fn==0) return vec3(0.0); if (fn==1) return local; if (fn==2) return vec3(la); if (fn==3) return f*other;\n"
    "  if (fn==4) return f*other+local; if (fn==5) return f*other+la; if (fn==6) return f*(other-local);\n"
    "  if (fn==7) return f*(other-local)+local; if (fn==8) return f*(other-local)+la; if (fn==9) return -f*local+local;\n"
    "  if (fn==16) return -f*local+la; return other; }\n"
    "float comb1(int fn, float f, float local, float other){\n"
    "  if (fn==0) return 0.0; if (fn==1||fn==2) return local; if (fn==3) return f*other; if (fn==4||fn==5) return f*other+local;\n"
    "  if (fn==6) return f*(other-local); if (fn==7||fn==8) return f*(other-local)+local; if (fn==9||fn==16) return -f*local+local; return other; }\n"
    "void main(){ vec4 t = texel();\n"
    "  if (chroma_on==1 && texmode!=0 && distance(t.rgb, chroma) < 0.002) discard;\n"
    "  vec3 cl = cc.z==0 ? vcol.rgb : constc.rgb;\n"
    "  vec3 co = cc.w==0 ? vcol.rgb : (cc.w==1 ? t.rgb : constc.rgb);\n"
    "  float al = ac.z==0 ? vcol.a : (ac.z==1 ? constc.a : 1.0);\n"
    "  float ao = ac.w==0 ? vcol.a : (ac.w==1 ? t.a : constc.a);\n"
    "  vec3 c = clamp(comb3(cc.x, factor3(cc.y, cl, al, ao, t), cl, co, al), 0.0, 1.0); if (cinv==1) c = 1.0-c;\n"
    "  float a = clamp(comb1(ac.x, factor1(ac.y, al, ao, t), al, ao), 0.0, 1.0); if (ainv==1) a = 1.0-a;\n"
    "  o = vec4(c, a); }\n";
static const char* BVS =
    "#version 300 es\nlayout(location=0) in vec2 pos; uniform vec4 rect; out vec2 vuv;\n"
    "void main(){ vuv = pos; vec2 p = rect.xy + pos*rect.zw; gl_Position = vec4(p*2.0-1.0, 0.0, 1.0); }\n";
static const char* BFS =
    "#version 300 es\nprecision mediump float; in vec2 vuv; out vec4 o; uniform sampler2D tex; uniform sampler2D gamma;\n"
    "void main(){ vec3 c = texture(tex, vuv).rgb;\n"
    "  o = vec4(texture(gamma, vec2(c.r*255.0/256.0+0.5/256.0, 0.5)).r, texture(gamma, vec2(c.g*255.0/256.0+0.5/256.0, 0.5)).g,\n"
    "           texture(gamma, vec2(c.b*255.0/256.0+0.5/256.0, 0.5)).b, 1.0); }\n";

static GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; glGetShaderInfoLog(s, sizeof log, NULL, log); LOG("shader: %s", log); }
    return s;
}
static GLuint program(const char* vs, const char* fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[2048]; glGetProgramInfoLog(p, sizeof log, NULL, log); LOG("link: %s", log); }
    return p;
}

void glide_resized(void) {
    int ww, wh;
    SDL_GL_GetDrawableSize(g_win, &ww, &wh);
    // keep the game's aspect, centred
    float s = (float)ww / g_view.w < (float)wh / g_view.h ? (float)ww / g_view.w : (float)wh / g_view.h;
    g_view.dw = (int)(g_view.w * s); g_view.dh = (int)(g_view.h * s);
    g_view.ox = (ww - g_view.dw) / 2; g_view.oy = (wh - g_view.dh) / 2;
    // mouse events arrive in window points: on HiDPI the drawable is larger
    int pw, ph;
    SDL_GetWindowSize(g_win, &pw, &ph);
    if (pw && pw != ww) { float k = (float)pw / ww; g_view.dw = (int)(g_view.dw * k); g_view.dh = (int)(g_view.dh * k); g_view.ox = (int)(g_view.ox * k); g_view.oy = (int)(g_view.oy * k); }
}

static void make_fbo(void) {
    if (g_fbo) { glDeleteFramebuffers(1, &g_fbo); glDeleteTextures(1, &g_fbtex); }
    glGenTextures(1, &g_fbtex);
    glBindTexture(GL_TEXTURE_2D, g_fbtex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_view.w, g_view.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &g_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_fbtex, 0);
    glViewport(0, 0, g_view.w, g_view.h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

static void set_gamma_table(const u8* r, const u8* g, const u8* b) {
    u8 t[256 * 4];
    for (int i = 0; i < 256; ++i) { t[4 * i] = r[i]; t[4 * i + 1] = g[i]; t[4 * i + 2] = b[i]; t[4 * i + 3] = 255; }
    glBindTexture(GL_TEXTURE_2D, g_gammatex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, t);
}

static void gl_init(void) {
    SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) LOG("SDL_Init: %s", SDL_GetError());
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
#ifdef __ANDROID__
    g_win = SDL_CreateWindow("Diablo II", 0, 0, 0, 0, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_ALLOW_HIGHDPI);
#else
    g_win = SDL_CreateWindow("Diablo II (recompiled)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, g_view.w * 2, g_view.h * 2,
                             SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
#endif
    if (!g_win) LOG("SDL_CreateWindow: %s", SDL_GetError());
    g_gl = SDL_GL_CreateContext(g_win);
    if (!g_gl) LOG("SDL_GL_CreateContext: %s", SDL_GetError());
    SDL_GL_SetSwapInterval(1);
    LOG("GL: %s / %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));
#ifdef __ANDROID__
    SDL_StopTextInput();  // SDL starts with text input on; the KB button opens the keyboard
#endif
    g_prog = program(VS, FS);
    g_blit = program(BVS, BFS);
#define U(n) u_##n = glGetUniformLocation(g_prog, #n)
    U(scale); U(texmode); U(cc); U(cinv); U(ac); U(ainv); U(constc); U(chroma_on); U(chroma); U(pal);
    glUseProgram(g_prog);
    for (int i = 0; i < NSLOT; ++i) { char n[4] = {'t', (char)('0' + i), 0}; glUniform1i(glGetUniformLocation(g_prog, n), i); }
    glUniform1i(u_pal, NSLOT);
    ub_tex = glGetUniformLocation(g_blit, "tex");
    ub_gamma = glGetUniformLocation(g_blit, "gamma");
    ub_rect = glGetUniformLocation(g_blit, "rect");
    glGenVertexArrays(1, &g_vao);
    glBindVertexArray(g_vao);
    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof g_v, NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)20);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)8);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vtx), (void*)16);
    glGenTextures(1, &g_paltex);
    glBindTexture(GL_TEXTURE_2D, g_paltex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenTextures(1, &g_gammatex);
    glBindTexture(GL_TEXTURE_2D, g_gammatex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    u8 id[256];
    for (int i = 0; i < 256; ++i) id[i] = (u8)i;
    set_gamma_table(id, id, id);
    glGenTextures(1, &g_lfbtex);
    make_fbo();
    glide_resized();
    g_open = 1;
}

// ------------------------------------------------------------------ batching
static void apply_blend(const int* b) {
    static const GLenum src[] = {GL_ZERO, GL_SRC_ALPHA, GL_DST_COLOR, GL_DST_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
                                 GL_ONE_MINUS_DST_COLOR, GL_ONE_MINUS_DST_ALPHA};
    static const GLenum dst[] = {GL_ZERO, GL_SRC_ALPHA, GL_SRC_COLOR, GL_DST_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
                                 GL_ONE_MINUS_SRC_COLOR, GL_ONE_MINUS_DST_ALPHA};
    if (b[0] == 4 && b[1] == 0) { glDisable(GL_BLEND); return; }
    glEnable(GL_BLEND);
    GLenum s = b[0] < 8 ? src[b[0]] : GL_SRC_ALPHA_SATURATE, d = b[1] < 8 ? dst[b[1]] : GL_ZERO;
    GLenum sa = b[2] < 8 ? src[b[2]] : GL_ONE, da = b[3] < 8 ? dst[b[3]] : GL_ZERO;
    glBlendFuncSeparate(s, d, sa, da);
}

static void flush(void) {
    if (!g_nv || !g_open) { g_nv = 0; g_nslots = 0; return; }
    State* s = &g_drawn;
    static int statelog = -1;
    if (statelog < 0) statelog = getenv("D2STATELOG") != NULL;
    extern int g_log_frames;
    if (statelog || g_log_frames) {
        static int n;
        if (g_log_frames || n++ < 400) LOG("draw %d verts tex %u mode %d cc %d %d %d %d inv %d ac %d %d %d %d inv %d const %08X chroma %d %08X blend %d %d %d %d",
            g_nv, s->tex, s->texmode, s->cc[0], s->cc[1], s->cc[2], s->cc[3], s->cinv, s->ac[0], s->ac[1], s->ac[2], s->ac[3], s->ainv,
            s->constc, s->chroma_on, s->chroma, s->blend[0], s->blend[1], s->blend[2], s->blend[3]);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glViewport(0, 0, g_view.w, g_view.h);
    glUseProgram(g_prog);
    glUniform2f(u_scale, 2.0f / g_view.w, 2.0f / g_view.h);
    glUniform1i(u_texmode, s->texmode);
    glUniform4i(u_cc, s->cc[0], s->cc[1], s->cc[2], s->cc[3]);
    glUniform1i(u_cinv, s->cinv);
    glUniform4i(u_ac, s->ac[0], s->ac[1], s->ac[2], s->ac[3]);
    glUniform1i(u_ainv, s->ainv);
    u32 k = s->constc;
    glUniform4f(u_constc, ((k >> 16) & 255) / 255.f, ((k >> 8) & 255) / 255.f, (k & 255) / 255.f, (k >> 24) / 255.f);
    glUniform1i(u_chroma_on, s->chroma_on);
    glUniform3f(u_chroma, ((s->chroma >> 16) & 255) / 255.f, ((s->chroma >> 8) & 255) / 255.f, (s->chroma & 255) / 255.f);
    glActiveTexture(GL_TEXTURE0 + NSLOT);
    glBindTexture(GL_TEXTURE_2D, g_paltex);
    for (int i = 0; i < g_nslots; ++i) { glActiveTexture(GL_TEXTURE0 + i); glBindTexture(GL_TEXTURE_2D, g_slots[i]); }
    glActiveTexture(GL_TEXTURE0);
    apply_blend(s->blend);
    glColorMask(s->cmask, s->cmask, s->cmask, GL_FALSE);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    // orphan the buffer: the driver hands out fresh storage instead of waiting for the GPU (Mali)
    glBufferData(GL_ARRAY_BUFFER, g_nv * sizeof(Vtx), g_v, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, g_nv);
    g_nv = 0;
    g_nslots = 0;
    g_draws++;
}

static int batch_uses(GLuint tex) {
    for (int i = 0; i < g_nslots; ++i) if (g_slots[i] == tex) return 1;
    return 0;
}
// The queued batch goes on while only the texture changes (a free slot, or one the batch already has).
static void begin_draw(int nverts) {
    g_st.cmask = g_colormask;
    State a = g_st, b = g_drawn;
    a.tex = b.tex = 0;
    if (g_nv && (memcmp(&a, &b, sizeof a) || g_nv + nverts > MAXV)) flush();
    int slot = -1;
    if (g_st.texmode && g_st.tex) {
        for (int i = 0; i < g_nslots; ++i) if (g_slots[i] == g_st.tex) slot = i;
        if (slot < 0) {
            if (g_nslots == NSLOT) flush();
            slot = g_nslots;
            g_slots[g_nslots++] = g_st.tex;
        }
    }
    g_slot = slot < 0 ? 0.f : (float)slot;
    g_drawn = g_st;
}

// Widescreen menus: the front end draws in 800 pixels; when a frame stays inside them, the next ones are
// shifted to the centre (input is shifted back in user.c through g_menu_dx).
float g_menu_dx;
static float g_frame_maxx;
static float g_prim_dx;  // this primitive's widescreen HUD shift (see hud_shift)

// Widescreen HUD: the control panel's left and right ends (globes, skill buttons, run / stamina) are drawn at
// the screen's edges; they are moved in to sit beside the centred middle part, as in 800x600. The draws come
// from D2Client's control panel function 0x6FAD9250 (only the screen's outer 170 pixels move: the side panels'
// borders it also draws stay). Input follows in user.c.
int g_hud_draw;  // > 0 while D2Client's control panel function runs (wrapped by tools/recomp.py WRAP)
static int in_hud_draw(void) { return g_hud_draw > 0; }
float hud_shift_for(float x) {  // x in game pixels -> the shift a HUD element there gets
    float dx = (g_view.w - 800) / 2.f;
    if (x < 170) return dx;                 // the left globe and the left skill button
    if (x >= g_view.w - 170) return -dx;    // the right ones
    return 0;
}
static void hud_shift(u32 first_vtx) {
    g_prim_dx = 0;
    if (g_view.w <= 800 || g_menu_dx != 0 || rd32(0x6FBCBBFC) == 0) return;
    if (!in_hud_draw()) return;
    g_prim_dx = hud_shift_for(rdf32(first_vtx + L_xy));
}

static Vtx read_vtx(u32 p) {
    Vtx v;
    v.x = rdf32(p + L_xy);
    if (v.x > g_frame_maxx) g_frame_maxx = v.x;
    v.x += g_menu_dx + g_prim_dx;
    v.y = rdf32(p + L_xy + 4);
    v.ti = g_slot;
    if (L_st0 >= 0) { v.u = rdf32(p + L_st0) / g_sw; v.v = rdf32(p + L_st0 + 4) / g_th; }
    else v.u = v.v = 0;
    if (L_pargb >= 0) {
        u32 c = rd32(p + L_pargb);  // packed ARGB whatever the window color format (Glide 3)
        v.c = (c & 0xFF00FF00u) | ((c >> 16) & 0xFF) | ((c & 0xFF) << 16);  // -> RGBA bytes (ABGR word)
    } else if (L_rgb >= 0) {
        float r = rdf32(p + L_rgb), g = rdf32(p + L_rgb + 4), b = rdf32(p + L_rgb + 8);
        float a = L_a >= 0 ? rdf32(p + L_a) : 255.f;
        v.c = (u32)(r < 0 ? 0 : r > 255 ? 255 : r) | (u32)(g < 0 ? 0 : g > 255 ? 255 : g) << 8 |
              (u32)(b < 0 ? 0 : b > 255 ? 255 : b) << 16 | (u32)(a < 0 ? 0 : a > 255 ? 255 : a) << 24;
    } else v.c = 0xFFFFFFFFu;
    return v;
}

static void emit_prim(int mode, int n, u32 (*vp)(int, void*), void* ctx) {
    if (n < 3 && mode >= 3) return;
    hud_shift(vp(0, ctx));
    if (mode == 0 || mode == 1 || mode == 2) {  // points / lines as thin quads
        int step = mode == 2 ? 2 : 1;
        int segs = mode == 0 ? n : mode == 1 ? n - 1 : n / 2;
        begin_draw(segs * 6);
        for (int i = 0; i < segs; ++i) {
            Vtx a = read_vtx(vp(mode == 2 ? 2 * i : i, ctx)), b = mode == 0 ? a : read_vtx(vp(mode == 2 ? 2 * i + 1 : i + 1, ctx));
            float dx = b.x - a.x, dy = b.y - a.y;
            float len = sqrtf(dx * dx + dy * dy);
            float nx = len > 0 ? -dy / len * 0.5f : 0.5f, ny = len > 0 ? dx / len * 0.5f : 0.5f;
            if (mode == 0) { b.x += 1; b.y += 1; nx = 0.5f; ny = -0.5f; }
            Vtx q[4] = {a, a, b, b};
            q[0].x += nx; q[0].y += ny; q[1].x -= nx; q[1].y -= ny;
            q[2].x -= nx; q[2].y -= ny; q[3].x += nx; q[3].y += ny;
            g_v[g_nv++] = q[0]; g_v[g_nv++] = q[1]; g_v[g_nv++] = q[2];
            g_v[g_nv++] = q[0]; g_v[g_nv++] = q[2]; g_v[g_nv++] = q[3];
        }
        (void)step;
        return;
    }
    int tris = n - 2;
    if (mode == 6) tris = n / 3;
    begin_draw(tris * 3);
    if (mode == 6) {
        for (int i = 0; i + 2 < n; i += 3) {
            g_v[g_nv++] = read_vtx(vp(i, ctx)); g_v[g_nv++] = read_vtx(vp(i + 1, ctx)); g_v[g_nv++] = read_vtx(vp(i + 2, ctx));
        }
    } else if (mode == 4) {  // strip
        for (int i = 0; i < tris; ++i) {
            int a = i, b = i + 1, cc = i + 2;
            if (i & 1) { int t = a; a = b; b = t; }
            g_v[g_nv++] = read_vtx(vp(a, ctx)); g_v[g_nv++] = read_vtx(vp(b, ctx)); g_v[g_nv++] = read_vtx(vp(cc, ctx));
        }
    } else {  // fan / polygon
        Vtx v0 = read_vtx(vp(0, ctx));
        Vtx prev = read_vtx(vp(1, ctx));
        for (int i = 2; i < n; ++i) {
            Vtx cur = read_vtx(vp(i, ctx));
            g_v[g_nv++] = v0; g_v[g_nv++] = prev; g_v[g_nv++] = cur;
            prev = cur;
        }
    }
}

static u32 vp_ptrs(int i, void* ctx) { return rd32(*(u32*)ctx + 4 * i); }
typedef struct { u32 base, stride; } Contig;
static u32 vp_contig(int i, void* ctx) { Contig* k = ctx; return k->base + k->stride * i; }

// ------------------------------------------------------------------ textures
static void tex_dims(u32 info, int* w, int* h, int* large) {
    s32 lg = (s32)rd32(info + 4), aspect = (s32)rd32(info + 8);
    *large = 1 << lg;
    if (aspect >= 0) { *w = *large; *h = *large >> aspect; }
    else { *h = *large; *w = *large >> -aspect; }
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
}

static int tex_bpp(int fmt) { return fmt >= 0x8 && fmt != 0x12 ? 2 : fmt == 0x12 ? 4 : 1; }

static int tex_find(u32 addr) {
    for (int i = 0; i < g_ntex; ++i) if (g_tex[i].addr == addr) return i;
    return -1;
}

static void nt__grTexDownloadMipMap_16(Cpu* c) {
    u32 addr = ARG(1), info = ARG(3);
    int w, h, large;
    tex_dims(info, &w, &h, &large);
    int fmt = (int)rd32(info + 12);
    u32 data = rd32(info + 16);
    int bpp = tex_bpp(fmt);
    u32 size = (u32)(w * h * bpp);
    // texture memory reuse: drop textures this download overlaps
    for (int i = 0; i < g_ntex; ++i) {
        Tex* t = &g_tex[i];
        if (t->addr < addr + size && addr < t->addr + t->size && t->addr != addr) {
            // the queued batch may still sample it: draw that first (else sprites come out invisible, or with
            // whatever texture GL hands the recycled name next - flickering monsters, missing items on the ground)
            if (g_nv && batch_uses(t->tex)) flush();
            if (g_st.tex == t->tex) g_st.tex = 0;
            glDeleteTextures(1, &t->tex);
            g_tex[i] = g_tex[--g_ntex];
            if (g_cur == g_ntex) g_cur = i;
            else if (g_cur == i) g_cur = -1;
            --i;
        }
    }
    int k = tex_find(addr);
    if (k < 0) {
        if (g_ntex >= MAXTEX) { LOG("glide: texture table full"); RET(0, 4); }
        k = g_ntex++;
        g_tex[k].addr = addr;
        glGenTextures(1, &g_tex[k].tex);
    }
    Tex* t = &g_tex[k];
    // a texture the queued batch samples is about to change
    if (g_nv && batch_uses(t->tex)) flush();
    t->w = w; t->h = h; t->fmt = fmt; t->size = size;
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (fmt == 0x5 || fmt == 0x3 || fmt == 0x2) {  // P_8 / INTENSITY_8 / ALPHA_8: one byte
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, H(data));
    } else {
        // 16/32-bit formats -> RGBA8
        u8* rgba = malloc((size_t)w * h * 4);
        for (int i = 0; i < w * h; ++i) {
            u32 r = 0, g = 0, b = 0, a = 255;
            if (bpp == 2) {
                u16 p = rd16(data + 2 * i);
                switch (fmt) {
                    case 0xa: r = (p >> 11) << 3; g = ((p >> 5) & 63) << 2; b = (p & 31) << 3; break;              // 565
                    case 0xb: a = p & 0x8000 ? 255 : 0; r = ((p >> 10) & 31) << 3; g = ((p >> 5) & 31) << 3; b = (p & 31) << 3; break;
                    case 0xc: a = (p >> 12) * 17; r = ((p >> 8) & 15) * 17; g = ((p >> 4) & 15) * 17; b = (p & 15) * 17; break;
                    case 0xe: { u32 q = g_pal[p & 255]; a = p >> 8; r = (q >> 16) & 255; g = (q >> 8) & 255; b = q & 255; break; }  // AP_88
                    default: r = g = b = p >> 8; break;
                }
            } else {
                u32 p = rd32(data + 4 * i);
                a = p >> 24; r = (p >> 16) & 255; g = (p >> 8) & 255; b = p & 255;
            }
            rgba[4 * i] = (u8)r; rgba[4 * i + 1] = (u8)g; rgba[4 * i + 2] = (u8)b; rgba[4 * i + 3] = (u8)a;
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        free(rgba);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    RET(0, 4);
}

static void nt__grTexSource_16(Cpu* c) {
    u32 addr = ARG(1), info = ARG(3);
    int k = tex_find(addr);
    g_cur = k;
    int w, h, large;
    tex_dims(info, &w, &h, &large);
    // Glide s/t: 0..256 across the larger side
    g_sw = 256.0f * w / large;
    g_th = 256.0f * h / large;
    if (k >= 0) {
        g_st.tex = g_tex[k].tex;
        int f = g_tex[k].fmt;
        g_st.texmode = (f == 0x5) ? 1 : (f == 0x3 || f == 0x2) ? 2 : 2;
    } else { g_st.tex = 0; g_st.texmode = 0; }
    RET(0, 4);
}

static void nt__grTexDownloadTable_8(Cpu* c) {
    u32 type = ARG(0), data = ARG(1);
    if (type == 0x2 || type == 0x3) {  // GR_TEXTABLE_PALETTE (and the 6666 extension)
        u8 t[1024];
        for (int i = 0; i < 256; ++i) {
            u32 p = rd32(data + 4 * i);
            g_pal[i] = p;
            t[4 * i] = (u8)(p >> 16); t[4 * i + 1] = (u8)(p >> 8); t[4 * i + 2] = (u8)p; t[4 * i + 3] = 255;
        }
        flush();
        glBindTexture(GL_TEXTURE_2D, g_paltex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, t);
    }
    RET(0, 2);
}

static void nt__grTexMinAddress_4(Cpu* c) { RET(0, 1); }
static void nt__grTexMaxAddress_4(Cpu* c) { RET(32u << 20, 1); }
static void nt__grTexFilterMode_12(Cpu* c) { RET(0, 3); }
static void nt__grTexCombine_28(Cpu* c) { RET(0, 7); }

// ------------------------------------------------------------------ state setters
static void nt__grColorCombine_20(Cpu* c) {
    g_st.cc[0] = (int)ARG(0); g_st.cc[1] = (int)ARG(1); g_st.cc[2] = (int)ARG(2); g_st.cc[3] = (int)ARG(3); g_st.cinv = ARG(4) != 0;
    RET(0, 5);
}
static void nt__grAlphaCombine_20(Cpu* c) {
    g_st.ac[0] = (int)ARG(0); g_st.ac[1] = (int)ARG(1); g_st.ac[2] = (int)ARG(2); g_st.ac[3] = (int)ARG(3); g_st.ainv = ARG(4) != 0;
    RET(0, 5);
}
static void nt__grAlphaBlendFunction_16(Cpu* c) {
    g_st.blend[0] = (int)ARG(0); g_st.blend[1] = (int)ARG(1); g_st.blend[2] = (int)ARG(2); g_st.blend[3] = (int)ARG(3);
    RET(0, 4);
}
static void nt__grConstantColorValue_4(Cpu* c) { g_st.constc = to_argb(ARG(0)); RET(0, 1); }
static void nt__grChromakeyMode_4(Cpu* c) { g_st.chroma_on = ARG(0) != 0; RET(0, 1); }
static void nt__grChromakeyValue_4(Cpu* c) { g_st.chroma = to_argb(ARG(0)); RET(0, 1); }
static void nt__grColorMask_8(Cpu* c) { g_colormask = ARG(0) != 0; RET(0, 2); }
static void nt__grDepthMask_4(Cpu* c) { RET(0, 1); }
static void nt__grDitherMode_4(Cpu* c) { RET(0, 1); }
static void nt__grCoordinateSpace_4(Cpu* c) { RET(0, 1); }
static void nt__grVertexLayout_12(Cpu* c) {
    u32 p = ARG(0);
    int off = ARG(2) ? (int)ARG(1) : -1;

    switch (p) {
        case 0x01: L_xy = off < 0 ? 0 : off; break;
        case 0x04: L_q = off; break;
        case 0x10: L_a = off; break;
        case 0x20: L_rgb = off; break;
        case 0x30: L_pargb = off; break;
        case 0x40: L_st0 = off; break;
    }
    RET(0, 3);
}

// ------------------------------------------------------------------ drawing
static u32 vp_ptrs(int i, void* ctx);
static u32 vp_contig(int i, void* ctx);
static int g_drawlog;
static void dump_fan(const char* what, int mode, int n, u32 (*vp)(int, void*), void* ctx) {
    static int on = -1;
    if (on < 0) on = getenv("D2GLIDELOG") != NULL;
    if (!on || g_drawlog++ >= 40) return;
    char b[400]; int o = 0;
    o += snprintf(b + o, sizeof b - o, "%s mode %d n %d tex %d (%dx%d sw %.0f th %.0f):", what, mode, n, g_cur,
                  g_cur >= 0 ? g_tex[g_cur].w : 0, g_cur >= 0 ? g_tex[g_cur].h : 0, g_sw, g_th);
    for (int i = 0; i < n && i < 4; ++i) {
        u32 p = vp(i, ctx);
        o += snprintf(b + o, sizeof b - o, " (%.0f,%.0f st %.1f,%.1f)", rdf32(p + L_xy), rdf32(p + L_xy + 4),
                      L_st0 >= 0 ? rdf32(p + L_st0) : 0, L_st0 >= 0 ? rdf32(p + L_st0 + 4) : 0);
    }
    LOG("%s", b);
}
static void nt__grDrawVertexArray_12(Cpu* c) {
    u32 ptrs = ARG(2);
    dump_fan("Array", (int)ARG(0), (int)ARG(1), vp_ptrs, &ptrs);
    emit_prim((int)ARG(0), (int)ARG(1), vp_ptrs, &ptrs);
    RET(0, 3);
}
static void nt__grDrawVertexArrayContiguous_16(Cpu* c) {
    Contig k = {ARG(2), ARG(3)};
    dump_fan("Contig", (int)ARG(0), (int)ARG(1), vp_contig, &k);
    if (g_drawlog > 0 && g_drawlog < 4) {
        char b[600]; int o = snprintf(b, sizeof b, "raw stride %u args %08X %08X %08X %08X:", ARG(3), ARG(0), ARG(1), ARG(2), ARG(3));
        for (int i = 0; i < 28; ++i) o += snprintf(b + o, sizeof b - o, " %08X", rd32(ARG(2) + 4 * i));
        LOG("%s", b);
    }
    emit_prim((int)ARG(0), (int)ARG(1), vp_contig, &k);
    RET(0, 4);
}
static u32 vp_two(int i, void* ctx) { return ((u32*)ctx)[i]; }
static void nt__grDrawLine_8(Cpu* c) {
    u32 v[2] = {ARG(0), ARG(1)};
    int save = g_st.texmode;
    emit_prim(2, 2, vp_two, v);
    g_st.texmode = save;
    RET(0, 2);
}
static void nt__grDrawPoint_4(Cpu* c) {
    u32 v[1] = {ARG(0)};
    emit_prim(0, 1, vp_two, v);
    RET(0, 1);
}

static void nt__grBufferClear_12(Cpu* c) {
    flush();
    if (!g_open) RET(0, 3);
    u32 k = to_argb(ARG(0));
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glColorMask(1, 1, 1, 1);
    glClearColor(((k >> 16) & 255) / 255.f, ((k >> 8) & 255) / 255.f, (k & 255) / 255.f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    RET(0, 3);
}

static void present(void) {
    flush();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    int ww, wh;
    SDL_GL_GetDrawableSize(g_win, &ww, &wh);
    glViewport(0, 0, ww, wh);
    glDisable(GL_BLEND);
    glColorMask(1, 1, 1, 1);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(g_blit);
    glUniform1i(ub_tex, 0);
    glUniform1i(ub_gamma, 1);
    // the FBO's row 0 is the game's top row: flip while blitting
    float x = (float)g_view.ox / ww, w = (float)g_view.dw / ww;
    int dw_px, dh_px, ox_px, oy_px;
    {
        float s = (float)ww / g_view.w < (float)wh / g_view.h ? (float)ww / g_view.w : (float)wh / g_view.h;
        dw_px = (int)(g_view.w * s); dh_px = (int)(g_view.h * s); ox_px = (ww - dw_px) / 2; oy_px = (wh - dh_px) / 2;
    }
    x = (float)ox_px / ww; w = (float)dw_px / ww;
    float y = (float)oy_px / wh, h = (float)dh_px / wh;
    glUniform4f(ub_rect, x, y, w, h);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g_gammatex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_fbtex);
    static const float quad[] = {0, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 1};
    static Vtx tmp[6];
    for (int i = 0; i < 6; ++i) { tmp[i].x = quad[2 * i]; tmp[i].y = quad[2 * i + 1]; }
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof tmp, tmp, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    extern void overlay_draw(int ww, int wh);
    overlay_draw(ww, wh);
    extern void touch_draw(int ww, int wh);
    touch_draw(ww, wh);
    // a snapshot of the window every 60 frames (save dir/screen.ppm) for remote debugging
    static unsigned shots;
    static int shot_on = -1;
    if (shot_on < 0) shot_on = getenv("D2SHOT") != NULL;
    extern char g_shot_name[64];
    int named = g_shot_name[0] != 0;
    if (named || (shot_on && ++shots % 60 == 0)) {
        u8* px = malloc((size_t)ww * wh * 4);
        glReadPixels(0, 0, ww, wh, GL_RGBA, GL_UNSIGNED_BYTE, px);
        char path[1100];
        snprintf(path, sizeof path, "%s/%s.ppm", g_savedir, named ? g_shot_name : "screen");
        g_shot_name[0] = 0;
        FILE* f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", ww, wh);
            for (int y = wh - 1; y >= 0; --y)
                for (int x = 0; x < ww; ++x) fwrite(px + ((size_t)y * ww + x) * 4, 1, 3, f);
            fclose(f);
        }
        free(px);
    }
    SDL_GL_SwapWindow(g_win);
}

int g_log_frames;  // > 0: log every draw's state for that many frames (set by Esc, to debug menus)
// Widescreen: inventory.txt's 800x600 rows (16-31) hold absolute x; the panels are centred, so move them too.
// D2Common keeps the table pointer at 0x6FDEFA5C (sgptDataTables->pInventoryTxt), count at +4... (1.13c)
static void wide_inventory_fix(void) {
    if (g_w800 <= 800) return;
    static u32 done;
    u32 tab = rd32(0x6FDEFA5C);
    if (!tab || tab == done) return;
    done = tab;
    int dx = (int)(g_w800 - 800) / 2;
    for (int r = 16; r < 32; ++r) {
        u32 a = tab + 240u * r;
        int L = (int)rd32(a);
        if (L < 0) {  // a row of body locations only
        } else {
            wr32(a, (u32)(L + dx)); wr32(a + 4, rd32(a + 4) + dx);
            if ((int)rd32(a + 20) >= 0) { wr32(a + 20, rd32(a + 20) + dx); wr32(a + 24, rd32(a + 24) + dx); }
        }
        for (int j = 0; j < 10; ++j) {
            u32 b = a + 40 + 20u * j;
            if ((int)rd32(b) >= 0) { wr32(b, rd32(b) + dx); wr32(b + 4, rd32(b + 4) + dx); }
        }
    }
    LOG("widescreen: inventory layout moved by %d (table %08X)", dx, tab);
}
// Belts.txt the same way: 14 rows of 0x108 bytes (+4 box count, +8 16 boxes {left, right, top, bottom}),
// rows 7-13 for 800x600 with absolute x; the belt sits in the centred control panel (pointer 0x6FDF13DC).
static void wide_belt_fix(void) {
    if (g_w800 <= 800) return;
    static u32 done;
    u32 tab = rd32(0x6FDF13DC);
    if (!tab || tab == done) return;
    done = tab;
    int dx = (int)(g_w800 - 800) / 2;
    for (int r = 7; r < 14; ++r)
        for (int b = 0; b < 16; ++b) {
            u32 a = tab + 0x108u * r + 8 + 16u * b;
            int l = (int)rd32(a);
            if (l > 0 && l < 800) { wr32(a, (u32)(l + dx)); wr32(a + 4, rd32(a + 4) + dx); }
        }
    LOG("widescreen: belt boxes moved by %d (table %08X)", dx, tab);
}

static void nt__grBufferSwap_4(Cpu* c) {
    wide_inventory_fix();
    wide_belt_fix();
    if (g_open) present();
    // menus (no hero: D2Client's player pointer is 0 or the DLL is not loaded yet) are 800 wide: centre them.
    // (Deciding by what was drawn failed: the cursor in the black margin pushed the menu to the left.)
    g_menu_dx = (g_view.w > 800 && rd32(0x6FBCBBFC) == 0) ? (g_view.w - 800) / 2.f : 0.f;
    g_frame_maxx = 0;
    if (g_log_frames) { g_log_frames--; LOG("---- frame end"); }
    static u32 t0, frames, draws0;
    u32 now = SDL_GetTicks();
    frames++;
    if (!t0) t0 = now;
    if (now - t0 >= 5000) {
        LOG("fps %.1f, %u draw calls/frame", frames * 1000.0 / (now - t0), (g_draws - draws0) / (frames ? frames : 1));
        t0 = now; frames = 0; draws0 = g_draws;
    }
    RET(0, 1);
}

// ------------------------------------------------------------------ setup / queries
static void nt__grGlideInit_0(Cpu* c) { RET(0, 0); }
static void nt__grGlideShutdown_0(Cpu* c) { RET(0, 0); }
static void nt__grSstSelect_4(Cpu* c) { RET(0, 1); }
static void nt__grFinish_0(Cpu* c) { flush(); RET(0, 0); }
static void nt__grSstWinOpen_28(Cpu* c) {
    u32 res = ARG(1);
    int w = 640, h = 480;
    switch (res) {
        case 0x7: w = 640; h = 480; break;
        case 0x8: w = (int)g_w800; h = 600; break;
        case 0xc: w = 1024; h = 768; break;
        case 0xd: w = 1280; h = 1024; break;
    }
    LOG("grSstWinOpen(res %X -> %dx%d, color format %u, origin %u)", res, w, h, ARG(3), ARG(4));
    g_view.w = w; g_view.h = h;
    g_cformat = (int)ARG(3);
    if (!g_open) gl_init();
    else { make_fbo(); glide_resized(); }
    g_st.blend[0] = 4; g_st.blend[1] = 0; g_st.blend[2] = 4; g_st.blend[3] = 0;
    g_st.cc[0] = 3; g_st.cc[1] = 8; g_st.cc[2] = 0; g_st.cc[3] = 1;  // texture * 1
    g_st.ac[0] = 3; g_st.ac[1] = 8; g_st.ac[2] = 0; g_st.ac[3] = 1;
    RET(1, 7);
}
static void nt__grSstWinClose_4(Cpu* c) { RET(1, 1); }

static void nt__grGet_12(Cpu* c) {
    u32 pname = ARG(0), len = ARG(1), out = ARG(2);
    u32 v[4] = {0};
    int n = 1;
    switch (pname) {
        case 0x01: v[0] = 16; break;                        // GR_BITS_DEPTH
        case 0x02: v[0] = 8; v[1] = 8; v[2] = 8; v[3] = 8; n = 4; break;  // GR_BITS_RGBA
        case 0x03: v[0] = 0; break;                         // FIFO fullness
        case 0x04: v[0] = 64; break;                        // fog table
        case 0x05: v[0] = 256; break;                       // gamma table entries
        case 0x06: v[0] = 1024; break;                      // state size
        case 0x07: v[0] = 256; break;                       // vertex layout size
        case 0x08: v[0] = 0; break;                         // busy
        case 0x09: v[0] = 1; break;                         // LFB pixel pipe
        case 0x0a: v[0] = 256; break;                       // max texture size
        case 0x0b: v[0] = 3; break;                         // max aspect (8:1)
        case 0x0c: v[0] = 16u << 20; break;                 // FB memory
        case 0x0d: v[0] = 32u << 20; break;                 // TMU memory
        case 0x0e: v[0] = 0; break;
        case 0x0f: v[0] = 1; break;                         // boards
        case 0x10: v[0] = 0; break;
        case 0x11: v[0] = 1; break;
        case 0x12: v[0] = 2; break;
        case 0x13: v[0] = 1; break;                         // TMUs
        case 0x14: v[0] = 0; break;
        case 0x15: case 0x16: v[0] = 1; break;
        case 0x24: v[0] = 16; break;                        // texture align
        case 0x26: v[0] = 0; v[1] = 0; v[2] = g_view.w; v[3] = g_view.h; n = 4; break;
        case 0x2a: v[0] = 8; break;                         // gamma bits
        default: v[0] = 0; break;
    }
    u32 bytes = (u32)n * 4;
    if (len < bytes) bytes = len;
    gput(out, v, bytes);
    RET(bytes, 3);
}

static void nt__grLoadGammaTable_16(Cpu* c) {
    u32 n = ARG(0);
    u8 r[256], g[256], b[256];
    for (int i = 0; i < 256; ++i) {
        u32 k = (u32)i * n / 256;
        r[i] = (u8)rd32(ARG(1) + 4 * k); g[i] = (u8)rd32(ARG(2) + 4 * k); b[i] = (u8)rd32(ARG(3) + 4 * k);
    }
    if (g_open) set_gamma_table(r, g, b);
    RET(0, 4);
}
static void nt__guGammaCorrectionRGB_12(Cpu* c) {
    float gr = ARGF(0), gg = ARGF(1), gb = ARGF(2);
    u8 r[256], g[256], b[256];
    for (int i = 0; i < 256; ++i) {
        r[i] = (u8)(powf(i / 255.f, 1.0f / (gr > 0 ? gr : 1)) * 255.f + 0.5f);
        g[i] = (u8)(powf(i / 255.f, 1.0f / (gg > 0 ? gg : 1)) * 255.f + 0.5f);
        b[i] = (u8)(powf(i / 255.f, 1.0f / (gb > 0 ? gb : 1)) * 255.f + 0.5f);
    }
    if (g_open) set_gamma_table(r, g, b);
    RET(0, 3);
}

// LFB: a guest buffer; writes are drawn over the frame on unlock
static u32 g_lfb, g_lfb_mode, g_lfb_write;
static void nt__grLfbLock_24(Cpu* c) {
    u32 type = ARG(0), wmode = ARG(2), info = ARG(5);
    flush();
    u32 bpp = (wmode == 4 || wmode == 5 || wmode == 0xFF) ? 4 : 2;
    if (wmode == 0xFF) wmode = 5;
    if (!g_lfb) g_lfb = gvalloc(1280 * 1024 * 4);
    g_lfb_mode = wmode;
    g_lfb_write = type & 1;
    if (!g_lfb_write && g_open) {  // read: the current frame
        glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
        u8* px = malloc((size_t)g_view.w * g_view.h * 4);
        glReadPixels(0, 0, g_view.w, g_view.h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        for (int y = 0; y < g_view.h; ++y)
            for (int x = 0; x < g_view.w; ++x) {
                u8* p = px + ((size_t)y * g_view.w + x) * 4;
                u32 at = g_lfb + (u32)(y * g_view.w + x) * bpp;
                if (bpp == 2) wr16(at, (u16)((p[0] >> 3) << 11 | (p[1] >> 2) << 5 | p[2] >> 3));
                else wr32(at, 0xFF000000u | (u32)p[0] << 16 | (u32)p[1] << 8 | p[2]);
            }
        free(px);
    } else memset(H(g_lfb), 0, (size_t)g_view.w * g_view.h * bpp);
    wr32(info + 4, g_lfb);
    wr32(info + 8, (u32)g_view.w * bpp);
    wr32(info + 12, wmode);
    wr32(info + 16, 0);
    RET(1, 6);
}
static void nt__grLfbUnlock_8(Cpu* c) {
    if (g_lfb_write && g_open) {
        u32 bpp = (g_lfb_mode == 4 || g_lfb_mode == 5) ? 4 : 2;
        u8* rgba = malloc((size_t)g_view.w * g_view.h * 4);
        for (int i = 0; i < g_view.w * g_view.h; ++i) {
            u8 r, g, b, a = 255;
            if (bpp == 2) { u16 p = rd16(g_lfb + 2 * i); r = (u8)((p >> 11) << 3); g = (u8)(((p >> 5) & 63) << 2); b = (u8)((p & 31) << 3); if (!p) a = 0; }
            else { u32 p = rd32(g_lfb + 4 * i); r = (u8)(p >> 16); g = (u8)(p >> 8); b = (u8)p; if (!(p & 0xFFFFFF)) a = 0; }
            rgba[4 * i] = r; rgba[4 * i + 1] = g; rgba[4 * i + 2] = b; rgba[4 * i + 3] = a;
        }
        glBindTexture(GL_TEXTURE_2D, g_lfbtex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_view.w, g_view.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        free(rgba);
        // a full-screen textured quad over the frame (black is transparent)
        State save = g_st;
        g_st.texmode = 2; g_st.tex = g_lfbtex;
        g_st.cc[0] = 1; g_st.cc[1] = 0; g_st.cc[2] = 0; g_st.cc[3] = 1;
        g_st.cc[0] = 3; g_st.cc[1] = 8; g_st.cc[2] = 0; g_st.cc[3] = 1;
        g_st.ac[0] = 3; g_st.ac[1] = 8; g_st.ac[2] = 0; g_st.ac[3] = 1;
        g_st.blend[0] = 1; g_st.blend[1] = 5; g_st.blend[2] = 4; g_st.blend[3] = 0;
        g_st.chroma_on = 0;
        begin_draw(6);
        Vtx q[4] = {{0, 0, 0, 0, 0xFFFFFFFFu, g_slot}, {(float)g_view.w, 0, 1, 0, 0xFFFFFFFFu, g_slot},
                    {(float)g_view.w, (float)g_view.h, 1, 1, 0xFFFFFFFFu, g_slot}, {0, (float)g_view.h, 0, 1, 0xFFFFFFFFu, g_slot}};
        g_v[g_nv++] = q[0]; g_v[g_nv++] = q[1]; g_v[g_nv++] = q[2];
        g_v[g_nv++] = q[0]; g_v[g_nv++] = q[2]; g_v[g_nv++] = q[3];
        flush();
        g_st = save;
    }
    RET(1, 2);
}

#define E(n, fn) {GL3, n, 0, fn}
NATIVE_TABLE(glide,
    E("_grAlphaBlendFunction@16", nt__grAlphaBlendFunction_16), E("_grAlphaCombine@20", nt__grAlphaCombine_20),
    E("_grBufferClear@12", nt__grBufferClear_12), E("_grBufferSwap@4", nt__grBufferSwap_4),
    E("_grChromakeyMode@4", nt__grChromakeyMode_4), E("_grChromakeyValue@4", nt__grChromakeyValue_4),
    E("_grColorCombine@20", nt__grColorCombine_20), E("_grColorMask@8", nt__grColorMask_8),
    E("_grConstantColorValue@4", nt__grConstantColorValue_4), E("_grCoordinateSpace@4", nt__grCoordinateSpace_4),
    E("_grDepthMask@4", nt__grDepthMask_4), E("_grDitherMode@4", nt__grDitherMode_4), E("_grDrawLine@8", nt__grDrawLine_8),
    E("_grDrawPoint@4", nt__grDrawPoint_4), E("_grDrawVertexArray@12", nt__grDrawVertexArray_12),
    E("_grDrawVertexArrayContiguous@16", nt__grDrawVertexArrayContiguous_16), E("_grFinish@0", nt__grFinish_0),
    E("_grGet@12", nt__grGet_12), E("_grGlideInit@0", nt__grGlideInit_0), E("_grGlideShutdown@0", nt__grGlideShutdown_0),
    E("_grLfbLock@24", nt__grLfbLock_24), E("_grLfbUnlock@8", nt__grLfbUnlock_8),
    E("_grLoadGammaTable@16", nt__grLoadGammaTable_16), E("_grSstSelect@4", nt__grSstSelect_4),
    E("_grSstWinClose@4", nt__grSstWinClose_4), E("_grSstWinOpen@28", nt__grSstWinOpen_28),
    E("_grTexCombine@28", nt__grTexCombine_28), E("_grTexDownloadMipMap@16", nt__grTexDownloadMipMap_16),
    E("_grTexDownloadTable@8", nt__grTexDownloadTable_8), E("_grTexFilterMode@12", nt__grTexFilterMode_12),
    E("_grTexMaxAddress@4", nt__grTexMaxAddress_4), E("_grTexMinAddress@4", nt__grTexMinAddress_4),
    E("_grTexSource@16", nt__grTexSource_16), E("_grVertexLayout@12", nt__grVertexLayout_12),
    E("_guGammaCorrectionRGB@12", nt__guGammaCorrectionRGB_12),
)
