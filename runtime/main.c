// Recompiled Diablo II runtime: guest memory, PE loading of the original images (data sections come
// from the player's own DLLs; the code is the recompiled C), the call dispatcher, threads and startup.
#define _GNU_SOURCE
#include "native.h"
#include "tables.h"
#include <stdarg.h>
#include <sys/mman.h>
#include <pthread.h>
#include <signal.h>
#include <execinfo.h>
#include <time.h>
#include <unistd.h>
#include <strings.h>
#include <ctype.h>
#include <SDL.h>  // on Android this turns main() into SDL_main (SDLActivity calls it)
#ifdef __ANDROID__
#include <android/log.h>
#endif

u8* G;
u32 g_w800 = 800;
char g_gamedir[1024];
char g_savedir[1024];
static FILE* g_log;
static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;

extern void overlay_log(const char* s);
extern volatile u32 g_native_calls;
extern const char* volatile g_last_native;

void logf_(const char* fmt, ...) {
    va_list ap;
    char line[256];
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    overlay_log(line);
#ifdef __ANDROID__
    __android_log_write(ANDROID_LOG_INFO, "d2recomp", line);
#endif
    pthread_mutex_lock(&g_log_mu);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fputc('\n', g_log);
        fflush(g_log);
    }
    pthread_mutex_unlock(&g_log_mu);
}

// ------------------------------------------------------------------ guest addresses
// 0x00010000 PEB, TEBs           0x00400000 Game.exe
// 0x01000000..0x5F000000 heap    0x60000000..0x6F000000 thread stacks
// 0x6F800000.. game DLLs         0x70000000 Storm.dll
// 0xF0000000.. native thunks (no memory behind them)
#define PEB_ADDR 0x00010000u
#define TEB_BASE 0x00020000u
#define TEB_SIZE 0x2000u
#define MAX_THREADS 120
#define STACK_BASE 0x60000000u
#define STACK_SIZE 0x00100000u
#define THUNK_BASE 0xF0000000u

static void map_guest(void) {
    G = mmap(NULL, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (G == MAP_FAILED) { perror("guest mmap"); exit(1); }
    mprotect(G, 0x10000, PROT_NONE);  // null page: catch guest null dereferences
}

// ------------------------------------------------------------------ natives and thunks
static NativeTable* g_tables;
void native_register(NativeTable* t) { t->next = g_tables; g_tables = t; }

typedef struct { const char* dll; const char* name; unsigned ordinal; GuestFn fn; } Thunk;
static Thunk g_thunks[4096];
static unsigned g_nthunks;
static pthread_mutex_t g_thunk_mu = PTHREAD_MUTEX_INITIALIZER;

static void nt_missing(Cpu* c);

static const NativeExport* native_find(const char* dll, const char* name, unsigned ordinal) {
    for (NativeTable* t = g_tables; t; t = t->next)
        for (unsigned i = 0; i < t->n; ++i) {
            const NativeExport* e = &t->e[i];
            if (strcasecmp(e->dll, dll)) continue;
            if (name ? (e->name && !strcmp(e->name, name)) : (!e->name && e->ordinal == ordinal)) return e;
        }
    return NULL;
}

int native_exists(const char* dll, const char* name, unsigned ordinal) { return native_find(dll, name, ordinal) != NULL; }

u32 native_thunk(const char* dll, const char* name, unsigned ordinal) {
    pthread_mutex_lock(&g_thunk_mu);
    for (unsigned i = 0; i < g_nthunks; ++i) {
        Thunk* t = &g_thunks[i];
        if (!strcasecmp(t->dll, dll) && (name ? (t->name && !strcmp(t->name, name)) : (!t->name && t->ordinal == ordinal))) {
            pthread_mutex_unlock(&g_thunk_mu);
            return THUNK_BASE + i * 16;
        }
    }
    const NativeExport* e = native_find(dll, name, ordinal);
    Thunk* t = &g_thunks[g_nthunks];
    t->dll = dll; t->name = name; t->ordinal = ordinal;
    t->fn = e ? e->fn : nt_missing;
    u32 a = THUNK_BASE + g_nthunks * 16;
    g_nthunks++;
    pthread_mutex_unlock(&g_thunk_mu);
    return a;
}

static Thunk* thunk_at(u32 a) {
    if (a < THUNK_BASE || (a - THUNK_BASE) % 16) return NULL;
    u32 i = (a - THUNK_BASE) / 16;
    return i < g_nthunks ? &g_thunks[i] : NULL;
}

static __thread u32 tl_last_thunk;
int g_trace;
static void nt_missing(Cpu* c) {
    Thunk* t = thunk_at(tl_last_thunk);
    if (t && t->name) LOG("FATAL: unimplemented import %s!%s", t->dll, t->name);
    else if (t) LOG("FATAL: unimplemented import %s!#%u", t->dll, t->ordinal);
    unimpl(c, rd32(c->r[ESP]), "import");
}

GuestFn lookup_fn(u32 a) {
    if (a >= THUNK_BASE) {
        Thunk* t = thunk_at(a);
        if (!t) return NULL;
        tl_last_thunk = a;
        g_native_calls++;
        g_last_native = t->name ? t->name : t->dll;
        if (g_trace) {
            Cpu* c = cpu_current();
            u32 sp = c ? c->r[ESP] : 0;
            LOG("[t%d] %s!%s(%08X, %08X, %08X, %08X) ret %08X", c ? c->thread : -1, t->dll, t->name ? t->name : "#",
                sp ? rd32(sp + 4) : 0, sp ? rd32(sp + 8) : 0, sp ? rd32(sp + 12) : 0, sp ? rd32(sp + 16) : 0, sp ? rd32(sp) : 0);
        }
        return t->fn;
    }
    // direct-mapped cache in front of the binary search (indirect calls are everywhere)
    // one 64-bit word per slot (address << 32 | index + 1), so threads never see a torn entry
    static u64 cache[1 << 16];
    u32 slot = (a * 2654435761u) >> 16;
    u64 e = __atomic_load_n(&cache[slot], __ATOMIC_RELAXED);
    if ((u32)(e >> 32) == a && (u32)e) return guest_fns[(u32)e - 1].fn;
    unsigned lo = 0, hi = guest_fn_count;
    while (lo < hi) {
        unsigned m = (lo + hi) / 2;
        if (guest_fns[m].addr < a) lo = m + 1;
        else hi = m;
    }
    GuestFn f = lo < guest_fn_count && guest_fns[lo].addr == a ? guest_fns[lo].fn : NULL;
    if (f) __atomic_store_n(&cache[slot], (u64)a << 32 | (lo + 1), __ATOMIC_RELAXED);
    return f;
}

static const char* where(u32 a, char* buf, size_t n) {
    for (unsigned i = 0; i < guest_image_count; ++i) {
        const GuestImage* im = &guest_images[i];
        if (a >= im->base && a < im->base + im->size) { snprintf(buf, n, "%s+0x%X", im->file, a - im->base); return buf; }
    }
    Thunk* t = thunk_at(a);
    if (t) { snprintf(buf, n, "%s!%s", t->dll, t->name ? t->name : "#ord"); return buf; }
    snprintf(buf, n, "0x%08X", a);
    return buf;
}

void call_indirect(Cpu* c, u32 target) {
    GuestFn f = lookup_fn(target);
    if (!f) {
        char b[96];
        LOG("FATAL: indirect call to %s (no function), from %08X", where(target, b, sizeof b), rd32(c->r[ESP]));
        unimpl(c, rd32(c->r[ESP]), "indirect call target");
    }
    if (++c->depth > 20000) unimpl(c, target, "guest call depth");
    f(c);
    c->depth--;
}

void jump_indirect(Cpu* c, u32 target) {
    GuestFn f = lookup_fn(target);
    if (!f) {
        char b[96];
        LOG("FATAL: indirect jump to %s (no function)", where(target, b, sizeof b));
        unimpl(c, target, "indirect jump target");
    }
    f(c);
}

#define MAGIC_RET 0xFFFF0000u
u32 call_guest(Cpu* c, u32 fn, int nargs, const u32* args) {
    u32 esp = c->r[ESP];
    for (int i = nargs - 1; i >= 0; --i) push32(c, args[i]);
    push32(c, MAGIC_RET);
    call_indirect(c, fn);
    c->r[ESP] = esp;
    return c->r[EAX];
}

// Guest call chain by heuristic: dwords on the guest stack that point just after a call in a game module.
void guest_stack_scan(Cpu* c, int max) {
    char b[96];
    u32 sp = c->r[ESP];
    int n = 0;
    for (u32 a = sp; a < sp + 0x2000 && n < max; a += 4) {
        u32 v = rd32(a);
        for (unsigned i = 0; i < guest_image_count; ++i) {
            const GuestImage* im = &guest_images[i];
            if (v > im->base + 0x1000 && v < im->base + im->size) {
                u8 op5 = rd8(v - 5), op6 = rd8(v - 6), op2 = rd8(v - 2), op3 = rd8(v - 3);
                if (op5 == 0xE8 || (op6 == 0xFF && (rd8(v - 5) & 0x38) == 0x10) || (op2 == 0xFF && (rd8(v - 1) & 0x38) == 0x10) ||
                    (op3 == 0xFF && (rd8(v - 2) & 0x38) == 0x10)) {
                    LOG("  [esp+%03X] %s", a - sp, where(v, b, sizeof b));
                    n++;
                }
                break;
            }
        }
    }
}

void host_backtrace(void) {
    void* bt[64];
    int n = backtrace(bt, 64);
    char** s = backtrace_symbols(bt, n);
    for (int i = 0; i < n; ++i) LOG("  %s", s[i]);
}

_Noreturn void unimpl(Cpu* c, u32 at, const char* what) {
    char b[96];
    LOG("FATAL: %s at %s  eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X",
        what, where(at, b, sizeof b), c->r[0], c->r[1], c->r[2], c->r[3], c->r[4], c->r[5], c->r[6], c->r[7]);
    host_backtrace();
    _exit(3);
}

_Noreturn void guest_int(Cpu* c, u32 at, int n) {
    char what[32];
    snprintf(what, sizeof what, n == 0 ? "division by zero" : "int %d", n);
    unimpl(c, at, what);
}

void guest_cpuid(Cpu* c) {
    u32 leaf = c->r[EAX];
    if (leaf == 0) {
        c->r[EAX] = 1;
        memcpy(&c->r[EBX], "Genu", 4); memcpy(&c->r[EDX], "ineI", 4); memcpy(&c->r[ECX], "ntel", 4);
    } else {
        // a Pentium Pro without MMX/SSE: the game and its CRT take their plain x86/x87 paths
        c->r[EAX] = 0x00000612; c->r[EBX] = 0; c->r[ECX] = 0;
        c->r[EDX] = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 15);
    }
}

void guest_rdtsc(Cpu* c) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    u64 t = (u64)ts.tv_sec * 1000000000ull + ts.tv_nsec;
    c->r[EAX] = (u32)t; c->r[EDX] = (u32)(t >> 32);
}

double rdf80(u32 a) {
    u64 m = rd64(a);
    u16 se = rd16(a + 8);
    int e = se & 0x7FFF;
    double s = (se & 0x8000) ? -1.0 : 1.0;
    if (e == 0 && m == 0) return s * 0.0;
    if (e == 0x7FFF) return (m << 1) ? NAN : s * INFINITY;
    return s * ldexp((double)m, e - 16383 - 63);
}

void wrf80(u32 a, double v) {
    u16 se = signbit(v) ? 0x8000 : 0;
    v = fabs(v);
    u64 m = 0;
    if (isnan(v)) { se |= 0x7FFF; m = 0xC000000000000000ull; }
    else if (isinf(v)) { se |= 0x7FFF; m = 0x8000000000000000ull; }
    else if (v != 0) {
        int e;
        double f = frexp(v, &e);  // v = f * 2^e, f in [0.5,1)
        m = (u64)ldexp(f, 64);
        se |= (u16)(e - 1 + 16383);
    }
    wr64(a, m);
    wr16(a + 8, se);
}

// ------------------------------------------------------------------ threads and TEBs
static pthread_key_t g_cpu_key;
static int g_nthreads_used[MAX_THREADS];
static pthread_mutex_t g_thr_mu = PTHREAD_MUTEX_INITIALIZER;

Cpu* cpu_current(void) { return pthread_getspecific(g_cpu_key); }

static int thread_slot(void) {
    pthread_mutex_lock(&g_thr_mu);
    for (int i = 0; i < MAX_THREADS; ++i)
        if (!g_nthreads_used[i]) { g_nthreads_used[i] = 1; pthread_mutex_unlock(&g_thr_mu); return i; }
    pthread_mutex_unlock(&g_thr_mu);
    LOG("FATAL: out of guest threads");
    _exit(3);
}

void thread_release(int slot) {
    pthread_mutex_lock(&g_thr_mu);
    g_nthreads_used[slot] = 0;
    pthread_mutex_unlock(&g_thr_mu);
}

void set_last_error(u32 e) { Cpu* c = cpu_current(); if (c) wr32(c->fs_base + 0x34, e); }
u32 get_last_error(void) { Cpu* c = cpu_current(); return c ? rd32(c->fs_base + 0x34) : 0; }

// The crash handler runs on its own stack, so a host stack overflow is reported too.
static void alt_stack(void) {
    stack_t ss;
    ss.ss_sp = malloc(1 << 16);
    ss.ss_size = 1 << 16;
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);
}

// A fresh guest CPU for this host thread: TEB, stack, FPU defaults.
Cpu* cpu_new(void) {
    alt_stack();
    Cpu* c = calloc(1, sizeof(Cpu));
    int slot = thread_slot();
    c->thread = slot;
    c->fs_base = TEB_BASE + slot * TEB_SIZE;
    memset(H(c->fs_base), 0, TEB_SIZE);
    u32 stack_top = STACK_BASE + (slot + 1) * STACK_SIZE - 16;
    wr32(c->fs_base + 0x00, 0xFFFFFFFFu);  // SEH chain end
    wr32(c->fs_base + 0x04, stack_top);
    wr32(c->fs_base + 0x08, STACK_BASE + slot * STACK_SIZE);
    wr32(c->fs_base + 0x18, c->fs_base);
    wr32(c->fs_base + 0x20, 1000);                    // process id
    wr32(c->fs_base + 0x24, 2000 + slot);             // thread id
    wr32(c->fs_base + 0x2C, c->fs_base + 0x1000);     // TLS array (static TLS)
    wr32(c->fs_base + 0x30, PEB_ADDR);
    c->r[ESP] = stack_top;
    c->fpu_cw = 0x27F;  // Windows default: 53-bit precision, round to nearest
    c->mxcsr = 0x1F80;
    pthread_setspecific(g_cpu_key, c);
    return c;
}

// ------------------------------------------------------------------ modules
typedef struct { const GuestImage* im; int state; } Module;  // state 0 not loaded, 1 loading, 2 ready
static Module g_mods[64];
static pthread_mutex_t g_mod_mu = PTHREAD_MUTEX_INITIALIZER;

static int mod_index(const char* name) {
    const char* b = strrchr(name, '\\');
    b = b ? b + 1 : name;
    const char* s = strrchr(b, '/');
    b = s ? s + 1 : b;
    for (unsigned i = 0; i < guest_image_count; ++i) {
        const char* f = guest_images[i].file;
        size_t n = strlen(f);
        if (!strcasecmp(f, b)) return i;
        // "D2Glide" without extension
        if (!strncasecmp(f, b, n - 4) && b[n - 4] == 0) return i;
    }
    return -1;
}

typedef struct { char name[64]; u32 base; } DataMod;
static DataMod g_datamods[16];
static int g_ndatamods;
static DataMod* datamod_of(u32 base);
const char* module_name_of(u32 base) {
    for (unsigned i = 0; i < guest_image_count; ++i)
        if (guest_images[i].base == base) return guest_images[i].file;
    DataMod* m = datamod_of(base);
    return m ? m->name : NULL;
}

static int read_file_host(const char* path, u8** out, size_t* n) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    *out = malloc(*n);
    size_t r = fread(*out, 1, *n, f);
    fclose(f);
    return r == *n;
}

static void map_image(const GuestImage* im) {
    char path[2048], hp[2048];
    snprintf(path, sizeof path, "C:\\D2\\%s", im->file);
    host_path(path, hp, sizeof hp);
    u8* d; size_t n;
    if (!read_file_host(hp, &d, &n)) { LOG("FATAL: cannot read %s", hp); _exit(2); }
    u32 pe = *(u32*)(d + 0x3C);
    u16 nsec = *(u16*)(d + pe + 6);
    u16 optsz = *(u16*)(d + pe + 20);
    u32 hdrsz = *(u32*)(d + pe + 24 + 60);
    memcpy(H(im->base), d, hdrsz < n ? hdrsz : n);
    u8* sec = d + pe + 24 + optsz;
    for (int i = 0; i < nsec; ++i, sec += 40) {
        u32 va = *(u32*)(sec + 12), rawsz = *(u32*)(sec + 16), rawp = *(u32*)(sec + 20);
        if (rawsz && rawp < n) memcpy(H(im->base + va), d + rawp, rawsz <= n - rawp ? rawsz : n - rawp);
    }
    // rebase (same delta the recompiler applied)
    u32 delta = im->base - im->pref;
    if (delta) {
        u32 rva = *(u32*)(d + pe + 24 + 96 + 5 * 8), size = *(u32*)(d + pe + 24 + 96 + 5 * 8 + 4);
        u32 p = im->base + rva, end = p + size;
        while (p < end) {
            u32 page = rd32(p), bsz = rd32(p + 4);
            if (!bsz) break;
            for (u32 o = 8; o < bsz; o += 2) {
                u16 e = rd16(p + o);
                if ((e >> 12) == 3) { u32 at = im->base + page + (e & 0xFFF); wr32(at, rd32(at) + delta); }
            }
            p += bsz;
        }
        // the optional header's ImageBase as Windows leaves it
        wr32(im->base + pe + 24 + 28, im->base);
    }
    free(d);
}

u32 module_export(u32 base, const char* name, unsigned ordinal) {
    u32 pe = base + rd32(base + 0x3C);
    u32 ed = rd32(pe + 24 + 96), edsz = rd32(pe + 24 + 96 + 4);
    if (!ed) return 0;
    u32 e = base + ed;
    u32 obase = rd32(e + 16), nfun = rd32(e + 20), nnames = rd32(e + 24);
    u32 funs = base + rd32(e + 28), names = base + rd32(e + 32), ords = base + rd32(e + 36);
    u32 idx = 0xFFFFFFFF;
    if (name) {
        for (u32 i = 0; i < nnames; ++i)
            if (!strcmp((const char*)H(base + rd32(names + 4 * i)), name)) { idx = rd16(ords + 2 * i); break; }
    } else if (ordinal >= obase && ordinal - obase < nfun) idx = ordinal - obase;
    if (idx == 0xFFFFFFFF || idx >= nfun) return 0;
    u32 rva = rd32(funs + 4 * idx);
    if (!rva) return 0;
    (void)edsz;
    return base + rva;
}

static void resolve_imports(const GuestImage* im) {
    for (unsigned i = 0; i < guest_import_count; ++i) {
        const GuestImport* gi = &guest_imports[i];
        if (gi->slot < im->base || gi->slot >= im->base + im->size) continue;
        int m = mod_index(gi->dll);
        u32 a;
        if (m >= 0) {
            a = module_export(guest_images[m].base, gi->name, gi->ordinal);
            if (!a) { LOG("FATAL: %s imports missing %s!%s#%u", im->file, gi->dll, gi->name ? gi->name : "", gi->ordinal); _exit(2); }
        } else {
            a = native_thunk(gi->dll, gi->name, gi->ordinal);
        }
        wr32(gi->slot, a);
    }
}

static int g_static_load;  // loading Game.exe's static imports: DllMain's lpReserved is non-NULL then

static void module_init(int m) {
    const GuestImage* im = &guest_images[m];
    Module* md = &g_mods[m];
    if (md->state) return;
    md->state = 1;
    md->im = im;
    map_image(im);
    // dependencies first (statically imported game modules)
    for (unsigned i = 0; i < guest_import_count; ++i) {
        const GuestImport* gi = &guest_imports[i];
        if (gi->slot < im->base || gi->slot >= im->base + im->size) continue;
        int d = mod_index(gi->dll);
        if (d >= 0 && !g_mods[d].state) module_init(d);
    }
    resolve_imports(im);
    if (strcasecmp(im->file, "Game.exe") && im->entry) {
        LOG("init %s", im->file);
        // DLL_PROCESS_ATTACH; Windows passes a non-NULL lpReserved for modules loaded with the process
        // (Storm builds its MPQ crypt table from a different seed when it sees NULL, i.e. a LoadLibrary)
        u32 args[3] = {im->base, 1, g_static_load ? 0x0012FFB0u : 0};
        u32 r = call_guest(cpu_current(), im->entry, 3, args);
        if (!r) LOG("warning: DllMain of %s returned FALSE", im->file);
    }
    md->state = 2;
}

u32 module_load(const char* name) {
    int m = mod_index(name);
    if (m < 0) return 0;
    pthread_mutex_lock(&g_mod_mu);
    module_init(m);
    pthread_mutex_unlock(&g_mod_mu);
    return guest_images[m].base;
}

// DLLs that are not game code (D2.LNG: resources only): mapped as data, never executed.

static DataMod* datamod_of(u32 base) {
    for (int i = 0; i < g_ndatamods; ++i) if (g_datamods[i].base == base) return &g_datamods[i];
    return NULL;
}

u32 module_load_data(const char* name) {
    const char* b = strrchr(name, '\\');
    b = b ? b + 1 : name;
    for (int i = 0; i < g_ndatamods; ++i) if (!strcasecmp(g_datamods[i].name, b)) return g_datamods[i].base;
    char path[1100], hp[1100];
    snprintf(path, sizeof path, "C:\\D2\\%s", b);
    host_path(path, hp, sizeof hp);
    u8* d; size_t n;
    if (!read_file_host(hp, &d, &n)) return 0;
    if (n < 0x40 || d[0] != 'M' || d[1] != 'Z') { free(d); return 0; }
    u32 pe = *(u32*)(d + 0x3C);
    u32 size = *(u32*)(d + pe + 24 + 56), hdrsz = *(u32*)(d + pe + 24 + 60);
    u16 nsec = *(u16*)(d + pe + 6), optsz = *(u16*)(d + pe + 20);
    u32 base = gvalloc(size);
    memcpy(H(base), d, hdrsz < n ? hdrsz : n);
    u8* sec = d + pe + 24 + optsz;
    for (int i = 0; i < nsec; ++i, sec += 40) {
        u32 va = *(u32*)(sec + 12), rawsz = *(u32*)(sec + 16), rawp = *(u32*)(sec + 20);
        if (rawsz && rawp < n && va < size) memcpy(H(base + va), d + rawp, rawsz <= n - rawp ? rawsz : n - rawp);
    }
    free(d);
    if (g_ndatamods < 16) { snprintf(g_datamods[g_ndatamods].name, 64, "%s", b); g_datamods[g_ndatamods++].base = base; }
    LOG("mapped data module %s at %08X", b, base);
    return base;
}

u32 module_handle(const char* name) {
    int m = mod_index(name);
    if (m < 0 || !g_mods[m].state) return 0;
    return guest_images[m].base;
}

// ------------------------------------------------------------------ crashes
static void on_fault(int sig, siginfo_t* si, void* uc) {
    (void)uc;
    u8* a = si->si_addr;
    Cpu* cc = cpu_current();
    if (cc) LOG("thread %d: guest esp=%08X depth %u", cc->thread, cc->r[ESP], cc->depth);
    if (a >= G && a < G + (1ull << 32)) LOG("FATAL: signal %d, guest address 0x%08X", sig, (u32)(a - G));
    else LOG("FATAL: signal %d at host %p", sig, a);
    host_backtrace();
    _exit(4);
}

// ------------------------------------------------------------------ startup
extern void k32_init(const char* cmdline);
static const char* g_cmdline;

static void* game_main(void* arg) {
    (void)arg;
    Cpu* c = cpu_new();
    int exe = mod_index("Game.exe");
    pthread_mutex_lock(&g_mod_mu);
    g_static_load = 1;
    module_init(exe);
    g_static_load = 0;
    pthread_mutex_unlock(&g_mod_mu);
    if (getenv("D2HASHTEST")) {
        // Storm's MPQ name hash (eax = name, stack: kind) against tools/mpqcheck.py
        const char* names[] = {"DATA\\GLOBAL\\UI\\CURSOR\\Pentspin.dc6", "data\\local\\use", "(hash table)"};
        for (int i = 0; i < 3; ++i) {
            u32 s = gstrdup(names[i]);
            for (u32 kind = 0; kind < 4; ++kind) {
                u32 esp = c->r[ESP];
                c->r[EAX] = s;
                push32(c, kind);
                push32(c, 0xFFFF0000u);
                call_indirect(c, 0x70025ED0u);
                LOG("hash(%s, %u) = %08X", names[i], kind, c->r[EAX]);
                c->r[ESP] = esp;
            }
        }
        LOG("crypt[0..3] at %08X: %08X %08X %08X %08X", rd32(0x70053120u), rd32(rd32(0x70053120u)), rd32(rd32(0x70053120u) + 4), rd32(rd32(0x70053120u) + 8), rd32(rd32(0x70053120u) + 12));
    }
    LOG("start Game.exe at %08X", guest_images[exe].entry);
    call_guest(c, guest_images[exe].entry, 0, NULL);
    LOG("Game.exe returned %u", c->r[EAX]);
    exit(0);
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <game dir> [save dir] [-- game arguments]\n", argv[0]); return 1; }
    snprintf(g_gamedir, sizeof g_gamedir, "%s", argv[1]);
    snprintf(g_savedir, sizeof g_savedir, "%s", argc > 2 && strcmp(argv[2], "--") && strcmp(argv[2], "--lang") ? argv[2] : argv[1]);
    static char cmd[1024] = "\"C:\\D2\\Game.exe\"";
    for (int i = 2; i < argc; ++i)
        if (!strcmp(argv[i], "--"))
            for (int j = i + 1; j < argc; ++j) { strcat(cmd, " "); strcat(cmd, argv[j]); }
    g_cmdline = cmd;
    // "--lang <code>" before "--": the language pack <game>/lang/<code>/data (read with -direct)
    for (int i = 2; i + 1 < argc && strcmp(argv[i], "--"); ++i)
        if (!strcmp(argv[i], "--lang")) setenv("D2LANG", argv[i + 1], 1);
    if (getenv("D2LANG") && !strstr(cmd, "-direct")) strcat(cmd, " -direct");
    char lp[1100];
    snprintf(lp, sizeof lp, "%s/d2recomp.log", g_savedir);
    g_log = fopen(lp, "w");
    struct sigaction sa = {0};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    g_trace = getenv("D2TRACE") != NULL;
    // "--wide" (or D2WIDE=<width>): the 800x600 mode becomes 16:9
    for (int i = 2; i < argc && strcmp(argv[i], "--"); ++i) if (!strcmp(argv[i], "--wide")) g_w800 = 1068;
        else if (!strncmp(argv[i], "--wide=", 7)) { int w = atoi(argv[i] + 7) & ~1; g_w800 = w < 800 ? 800 : w > 1400 ? 1400 : (u32)w; }
    if (getenv("D2WIDE")) g_w800 = (u32)atoi(getenv("D2WIDE"));
    setenv("SDL_NO_SIGNAL_HANDLERS", "1", 1);  // SIGTERM/SIGINT end the process (SDL would turn them into a quit event)
    map_guest();
    pthread_key_create(&g_cpu_key, NULL);
    // PEB: BeingDebugged = 0, ProcessHeap filled by k32_init
    memset(H(PEB_ADDR), 0, 0x1000);
    k32_init(g_cmdline);
    LOG("d2recomp: %u functions, %u imports, game dir %s, command line %s", guest_fn_count, guest_import_count, g_gamedir, g_cmdline);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 512u << 20);
    pthread_t t;
    pthread_create(&t, &at, game_main, NULL);  // the game thread also owns the window and GL (as on Android)
    pthread_join(t, NULL);
    return 0;
}
