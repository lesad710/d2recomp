// Debug overlay drawn over the game picture (F11 toggles): the last log lines, frame rate, guest memory
// use and the Windows API traffic. Text is rendered with SDL_ttf into a texture a few times a second.
#include "native.h"
#include <SDL.h>
#ifndef D2_NO_TTF
#include <SDL_ttf.h>
#endif
#include <GLES3/gl3.h>
#include <pthread.h>
#include <stdarg.h>

#define NLINES 14
static char g_lines[NLINES][160];
static int g_head;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
volatile u32 g_native_calls;
const char* volatile g_last_native = "";
int g_overlay_on = 1;

void overlay_log(const char* s) {
    pthread_mutex_lock(&g_mu);
    snprintf(g_lines[g_head], sizeof g_lines[0], "%s", s);
    g_head = (g_head + 1) % NLINES;
    pthread_mutex_unlock(&g_mu);
}

extern u32 heap_used_kb(void);
extern u32 va_used_kb(void);

#ifdef D2_NO_TTF
void overlay_draw(int ww, int wh) { (void)ww; (void)wh; }
#else
static TTF_Font* g_font;
static GLuint g_tex, g_prog, g_vbo, g_vao;
static int g_tw, g_th;
static u32 g_last_render, g_frames, g_fps_frames, g_fps_t0;
static float g_fps;

static const char* VS = "#version 300 es\nlayout(location=0) in vec2 p; uniform vec4 r; out vec2 uv;\n"
    "void main(){ uv = vec2(p.x, p.y); gl_Position = vec4(r.x + p.x*r.z, r.y - p.y*r.w, 0.0, 1.0); }\n";
static const char* FS = "#version 300 es\nprecision mediump float; in vec2 uv; out vec4 o; uniform sampler2D t;\n"
    "void main(){ o = texture(t, uv); }\n";

static GLuint sh(GLenum k, const char* s) { GLuint x = glCreateShader(k); glShaderSource(x, 1, &s, NULL); glCompileShader(x); return x; }

static void init(void) {
    if (TTF_Init() < 0) { g_overlay_on = 0; return; }
    const char* paths[] = {"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "/system/fonts/DroidSansMono.ttf",
                           "/system/fonts/RobotoMono-Regular.ttf", "/system/fonts/Roboto-Regular.ttf"};
    for (unsigned i = 0; i < sizeof paths / sizeof *paths && !g_font; ++i) g_font = TTF_OpenFont(paths[i], 16);
    if (!g_font) { LOG("overlay: no font"); g_overlay_on = 0; return; }
    g_prog = glCreateProgram();
    glAttachShader(g_prog, sh(GL_VERTEX_SHADER, VS));
    glAttachShader(g_prog, sh(GL_FRAGMENT_SHADER, FS));
    glLinkProgram(g_prog);
    glGenTextures(1, &g_tex);
    glGenVertexArrays(1, &g_vao);
    glBindVertexArray(g_vao);
    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    static const float q[] = {0, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 1};
    glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, 0);
}

static void render_text(void) {
    char lines[NLINES + 3][200];
    int n = 0;
    snprintf(lines[n++], 200, "d2recomp  %.1f fps  frame %u  heap %u MB  virtual %u MB", g_fps, g_frames,
             heap_used_kb() >> 10, va_used_kb() >> 10);
    snprintf(lines[n++], 200, "win32 calls %u  last: %s", g_native_calls, g_last_native);
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < NLINES; ++i) {
        const char* s = g_lines[(g_head + i) % NLINES];
        if (s[0]) snprintf(lines[n++], 200, "%s", s);
    }
    pthread_mutex_unlock(&g_mu);
    int lh = TTF_FontLineSkip(g_font), w = 0;
    for (int i = 0; i < n; ++i) { int tw = 0; TTF_SizeUTF8(g_font, lines[i], &tw, NULL); if (tw > w) w = tw; }
    w += 12;
    if (w > 1600) w = 1600;
    int h = n * lh + 8;
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ABGR8888);
    SDL_FillRect(s, NULL, SDL_MapRGBA(s->format, 0, 0, 0, 170));
    for (int i = 0; i < n; ++i) {
        SDL_Color col = i < 2 ? (SDL_Color){255, 220, 90, 255} : (SDL_Color){200, 255, 200, 255};
        SDL_Surface* t = TTF_RenderUTF8_Blended(g_font, lines[i], col);
        if (!t) continue;
        SDL_Rect r = {6, 4 + i * lh, t->w, t->h};
        SDL_SetSurfaceBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_BlitSurface(t, NULL, s, &r);
        SDL_FreeSurface(t);
    }
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, s->pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    g_tw = w; g_th = h;
    SDL_FreeSurface(s);
}

// Called by the renderer after the game picture is drawn into the default framebuffer.
void overlay_draw(int ww, int wh) {
    g_frames++;
    u32 now = SDL_GetTicks();
    if (!g_fps_t0) g_fps_t0 = now;
    g_fps_frames++;
    if (now - g_fps_t0 >= 1000) { g_fps = g_fps_frames * 1000.f / (now - g_fps_t0); g_fps_frames = 0; g_fps_t0 = now; }
    if (!g_overlay_on) return;
    if (!g_font) { init(); if (!g_font) return; }
    if (now - g_last_render > 250 || !g_tw) { render_text(); g_last_render = now; }
    glUseProgram(g_prog);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glUniform1i(glGetUniformLocation(g_prog, "t"), 0);
    // top-left, at 1:1 pixels
    glUniform4f(glGetUniformLocation(g_prog, "r"), -1.f, 1.f, 2.f * g_tw / ww, 2.f * g_th / wh);
    glBindVertexArray(g_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisable(GL_BLEND);
}
#endif
