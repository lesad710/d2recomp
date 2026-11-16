// kernel32 / advapi32 / version / winmm / shell32 / comdlg32 / imm32 for the recompiled game.
#define _GNU_SOURCE
#include "native.h"
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dirent.h>
#include <fnmatch.h>
#include <errno.h>
#include <time.h>
#include <strings.h>
#include <ctype.h>
#include <sched.h>

#define K "kernel32.dll"
#define ERR_FILE_NOT_FOUND 2
#define ERR_PATH_NOT_FOUND 3
#define ERR_ACCESS_DENIED 5
#define ERR_INVALID_HANDLE 6
#define ERR_NO_MORE_FILES 18
#define ERR_ALREADY_EXISTS 183
#define ERR_FILE_EXISTS 80
#define INVALID_HANDLE 0xFFFFFFFFu

Cpu* cpu_new(void);
void thread_release(int slot);
int gv_owned(u32 a, u32* base, u32* size);

// ------------------------------------------------------------------ handles
static HObj g_handles[4096];
static pthread_mutex_t g_hmu = PTHREAD_MUTEX_INITIALIZER;
#define HANDLE_BASE 0x100u

u32 handle_new(int type, void* p) {
    pthread_mutex_lock(&g_hmu);
    for (u32 i = 1; i < 4096; ++i)
        if (!g_handles[i].type) {
            g_handles[i].type = type; g_handles[i].p = p;
            pthread_mutex_unlock(&g_hmu);
            return HANDLE_BASE + i * 4;
        }
    pthread_mutex_unlock(&g_hmu);
    LOG("FATAL: out of handles");
    return 0;
}

HObj* handle_get(u32 h) {
    if (h < HANDLE_BASE || (h - HANDLE_BASE) % 4) return NULL;
    u32 i = (h - HANDLE_BASE) / 4;
    if (i >= 4096 || !g_handles[i].type) return NULL;
    return &g_handles[i];
}

void handle_close(u32 h) {
    HObj* o = handle_get(h);
    if (o) { o->type = 0; o->p = NULL; }
}

// ------------------------------------------------------------------ paths
// "C:\D2\..." and relative paths (cwd C:\D2) -> host paths under the game dir; "Save" -> save dir.
static char g_cwd[260] = "C:\\D2\\";

static int ci_component(const char* dir, const char* name, char* out, size_t n) {
    DIR* d = opendir(dir[0] ? dir : ".");
    if (!d) return 0;
    struct dirent* e;
    int ok = 0;
    while ((e = readdir(d)))
        if (!strcasecmp(e->d_name, name)) { snprintf(out, n, "%s", e->d_name); ok = 1; break; }
    closedir(d);
    return ok;
}

const char* host_path(const char* win, char* out, size_t outsz) {
    char full[1024];
    if (!win) { out[0] = 0; return out; }
    if (win[0] && win[1] == ':') snprintf(full, sizeof full, "%s", win);
    else if (win[0] == '\\' || win[0] == '/') snprintf(full, sizeof full, "C:%s", win);
    else snprintf(full, sizeof full, "%s%s", g_cwd, win);
    for (char* p = full; *p; ++p) if (*p == '/') *p = '\\';
    // root mapping
    const char* rest;
    char root[1024];
    if (!strncasecmp(full, "C:\\D2\\save", 10) && (full[10] == '\\' || full[10] == 0)) {
        snprintf(root, sizeof root, "%s/Save", g_savedir);
        mkdir(root, 0755);
        rest = full + 10;
    } else if (!strncasecmp(full, "C:\\D2\\data", 10) && (full[10] == '\\' || full[10] == 0)) {
        // <game>/data is the language pack's: <game>/lang/<D2LANG>/data, none for English (the MPQs')
        const char* lang = getenv("D2LANG");
        if (lang && *lang) snprintf(root, sizeof root, "%s/lang/%s/data", g_gamedir, lang);
        else snprintf(root, sizeof root, "%s/.no-data", g_gamedir);
        rest = full + 10;
    } else if (!strncasecmp(full, "C:\\D2", 5) && (full[5] == '\\' || full[5] == 0)) {
        snprintf(root, sizeof root, "%s", g_gamedir);
        rest = full + 5;
    } else {
        snprintf(root, sizeof root, "%s/drive_c", g_savedir);
        mkdir(root, 0755);
        rest = full + 2;
    }
    // walk components, matching existing entries case-insensitively
    snprintf(out, outsz, "%s", root);
    char comp[256];
    while (*rest) {
        while (*rest == '\\') rest++;
        if (!*rest) break;
        size_t k = 0;
        while (rest[k] && rest[k] != '\\') k++;
        snprintf(comp, sizeof comp, "%.*s", (int)k, rest);
        rest += k;
        if (!strcmp(comp, ".")) continue;
        if (!strcmp(comp, "..")) { char* s = strrchr(out, '/'); if (s && s != out) *s = 0; continue; }
        char real[256];
        size_t l = strlen(out);
        if (ci_component(out, comp, real, sizeof real)) snprintf(out + l, outsz - l, "/%s", real);
        else snprintf(out + l, outsz - l, "/%s", comp);
    }
    return out;
}

// ------------------------------------------------------------------ files
typedef struct { int fd; char path[1024]; } FileObj;

static void nt_CreateFileA(Cpu* c) {
    const char* name = gstr(ARG(0));
    u32 access = ARG(1), disp = ARG(4);
    char hp[1024];
    host_path(name, hp, sizeof hp);
    int fl = 0;
    if ((access & 0xC0000000u) == 0xC0000000u || (access & 0x10000000u)) fl = O_RDWR;
    else if (access & 0x40000000u) fl = O_WRONLY;
    else fl = O_RDONLY;
    struct stat st;
    int exists = stat(hp, &st) == 0;
    switch (disp) {
        case 1: if (exists) { set_last_error(ERR_FILE_EXISTS); RET(INVALID_HANDLE, 7); } fl |= O_CREAT; break;  // CREATE_NEW
        case 2: fl |= O_CREAT | O_TRUNC; break;                                                                 // CREATE_ALWAYS
        case 3: if (!exists) { if (getenv("D2FILES")) LOG("CreateFile(%s): no such file (%s)", name, hp); set_last_error(ERR_FILE_NOT_FOUND); RET(INVALID_HANDLE, 7); } break;  // OPEN_EXISTING
        case 4: fl |= O_CREAT; break;                                                                           // OPEN_ALWAYS
        case 5: if (!exists) { set_last_error(ERR_FILE_NOT_FOUND); RET(INVALID_HANDLE, 7); } fl |= O_TRUNC; break;
    }
    if (exists && S_ISDIR(st.st_mode)) { set_last_error(ERR_ACCESS_DENIED); RET(INVALID_HANDLE, 7); }
    int fd = open(hp, fl, 0644);
    if (fd < 0 && (fl & O_ACCMODE) != O_RDONLY && errno == EACCES) fd = open(hp, (fl & ~O_ACCMODE) | O_RDONLY);
    if (fd < 0) {
        LOG("CreateFile(%s) failed: %s", name, strerror(errno));
        set_last_error(errno == ENOENT ? ERR_FILE_NOT_FOUND : ERR_ACCESS_DENIED);
        RET(INVALID_HANDLE, 7);
    }
    FileObj* f = calloc(1, sizeof *f);
    f->fd = fd;
    snprintf(f->path, sizeof f->path, "%s", hp);
    set_last_error((disp == 2 || disp == 4) && exists ? ERR_ALREADY_EXISTS : 0);
    if (getenv("D2FILES")) LOG("CreateFile(%s) ok", name);
    RET(handle_new(H_FILE, f), 7);
}

static FileObj* file_of(u32 h) {
    HObj* o = handle_get(h);
    return o && o->type == H_FILE ? o->p : NULL;
}

static void nt_ReadFile(Cpu* c) {
    FileObj* f = file_of(ARG(0));
    u32 buf = ARG(1), n = ARG(2), pread_ = ARG(3);
    if (!f) { set_last_error(ERR_INVALID_HANDLE); RET(0, 5); }
    ssize_t got = 0;
    while ((u32)got < n) {
        ssize_t r = read(f->fd, H(buf + got), n - got);
        if (r <= 0) break;
        got += r;
    }
    if (pread_) wr32(pread_, (u32)got);
    RET(1, 5);
}

static void nt_WriteFile(Cpu* c) {
    u32 h = ARG(0), buf = ARG(1), n = ARG(2), pw = ARG(3);
    if (h == 0xFFFFFFF5u || h == 0xFFFFFFF4u) {  // std handles
        fwrite(H(buf), 1, n, stderr);
        if (pw) wr32(pw, n);
        RET(1, 5);
    }
    FileObj* f = file_of(h);
    if (!f) { set_last_error(ERR_INVALID_HANDLE); RET(0, 5); }
    ssize_t r = write(f->fd, H(buf), n);
    if (pw) wr32(pw, r > 0 ? (u32)r : 0);
    RET(r >= 0, 5);
}

static void nt_SetFilePointer(Cpu* c) {
    FileObj* f = file_of(ARG(0));
    s32 lo = (s32)ARG(1);
    u32 phi = ARG(2), method = ARG(3);
    if (!f) { set_last_error(ERR_INVALID_HANDLE); RET(0xFFFFFFFFu, 4); }
    s64 off = phi ? ((s64)(s32)rd32(phi) << 32) | (u32)lo : lo;
    off_t r = lseek(f->fd, off, method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END);
    if (r < 0) { set_last_error(131); RET(0xFFFFFFFFu, 4); }
    if (phi) wr32(phi, (u32)((u64)r >> 32));
    set_last_error(0);
    RET((u32)r, 4);
}

static void nt_GetFileSize(Cpu* c) {
    FileObj* f = file_of(ARG(0));
    if (!f) RET(0xFFFFFFFFu, 2);
    struct stat st;
    fstat(f->fd, &st);
    if (ARG(1)) wr32(ARG(1), (u32)((u64)st.st_size >> 32));
    RET((u32)st.st_size, 2);
}

static u64 unix_to_filetime(time_t t) { return ((u64)t + 11644473600ull) * 10000000ull; }

static void nt_GetFileTime(Cpu* c) {
    FileObj* f = file_of(ARG(0));
    if (!f) RET(0, 4);
    struct stat st;
    fstat(f->fd, &st);
    u64 ft = unix_to_filetime(st.st_mtime);
    for (int i = 1; i <= 3; ++i) if (ARG(i)) wr64(ARG(i), ft);
    RET(1, 4);
}

static void nt_SetFileTime(Cpu* c) { RET(1, 4); }
static void nt_SetEndOfFile(Cpu* c) {
    FileObj* f = file_of(ARG(0));
    if (!f) RET(0, 1);
    off_t pos = lseek(f->fd, 0, SEEK_CUR);
    RET(ftruncate(f->fd, pos) == 0, 1);
}
static void nt_FlushFileBuffers(Cpu* c) { RET(1, 1); }
static void nt_GetFileType(Cpu* c) {
    u32 h = ARG(0);
    if (h >= 0xFFFFFFF4u) RET(2, 1);  // FILE_TYPE_CHAR
    RET(file_of(h) ? 1 : 0, 1);
}

static void nt_DeleteFileA(Cpu* c) { char hp[1024]; host_path(gstr(ARG(0)), hp, sizeof hp); RET(unlink(hp) == 0, 1); }
static void nt_CreateDirectoryA(Cpu* c) {
    char hp[1024];
    host_path(gstr(ARG(0)), hp, sizeof hp);
    if (mkdir(hp, 0755) == 0) RET(1, 2);
    set_last_error(errno == EEXIST ? ERR_ALREADY_EXISTS : ERR_PATH_NOT_FOUND);
    RET(0, 2);
}
static void nt_GetFileAttributesA(Cpu* c) {
    char hp[1024];
    struct stat st;
    host_path(gstr(ARG(0)), hp, sizeof hp);
    if (stat(hp, &st)) { if (getenv("D2FILES")) LOG("GetFileAttributes(%s): none", gstr(ARG(0))); set_last_error(ERR_FILE_NOT_FOUND); RET(0xFFFFFFFFu, 1); }
    RET(S_ISDIR(st.st_mode) ? 0x10 : 0x80, 1);
}
static void nt_GetCurrentDirectoryA(Cpu* c) {
    u32 n = ARG(0), buf = ARG(1);
    size_t l = strlen(g_cwd);
    char tmp[260];
    snprintf(tmp, sizeof tmp, "%s", g_cwd);
    if (l > 3 && tmp[l - 1] == '\\') tmp[--l] = 0;
    if (n > l) gput(buf, tmp, (u32)l + 1);
    RET(n > l ? l : l + 1, 2);
}
static void nt_SetCurrentDirectoryA(Cpu* c) {
    const char* p = gstr(ARG(0));
    char full[260];
    if (p[0] && p[1] == ':') snprintf(full, sizeof full, "%s", p);
    else snprintf(full, sizeof full, "%s%s", g_cwd, p);
    size_t l = strlen(full);
    if (l && full[l - 1] != '\\') strcat(full, "\\");
    snprintf(g_cwd, sizeof g_cwd, "%s", full);
    RET(1, 1);
}
static void nt_GetFullPathNameA(Cpu* c) {
    const char* p = gstr(ARG(0));
    u32 n = ARG(1), buf = ARG(2), pfile = ARG(3);
    char full[600];
    if (p[0] && p[1] == ':') snprintf(full, sizeof full, "%s", p);
    else if (p[0] == '\\') snprintf(full, sizeof full, "C:%s", p);
    else snprintf(full, sizeof full, "%s%s", g_cwd, p);
    u32 l = (u32)strlen(full);
    if (n <= l) RET(l + 1, 4);
    gput(buf, full, l + 1);
    if (pfile) { char* s = strrchr(full, '\\'); wr32(pfile, buf + (s ? (u32)(s - full) + 1 : 0)); }
    RET(l, 4);
}
static void nt_GetTempPathA(Cpu* c) {
    const char* t = "C:\\D2\\tmp\\";
    u32 l = (u32)strlen(t);
    char hp[1024];
    host_path("C:\\D2\\tmp", hp, sizeof hp);
    mkdir(hp, 0755);
    if (ARG(0) > l) gput(ARG(1), t, l + 1);
    RET(l, 2);
}
static void nt_GetTempFileNameA(Cpu* c) {
    static int n;
    char name[300];
    snprintf(name, sizeof name, "%s\\%.3s%04X.tmp", gstr(ARG(0)), gstr(ARG(1)), ++n & 0xFFFF);
    gput(ARG(3), name, (u32)strlen(name) + 1);
    RET(n, 4);
}

// FindFirstFile: WIN32_FIND_DATAA (attrs, 3 filetimes, size hi/lo, 2 reserved, name[260], alt[14])
typedef struct { DIR* d; char dir[1024]; char pat[260]; } FindObj;
static int find_next(FindObj* f, u32 out) {
    struct dirent* e;
    while ((e = readdir(f->d))) {
        if (fnmatch(f->pat, e->d_name, FNM_CASEFOLD)) continue;
        char p[1300];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", f->dir, e->d_name);
        if (stat(p, &st)) continue;
        memset(H(out), 0, 320);
        wr32(out, S_ISDIR(st.st_mode) ? 0x10 : 0x80);
        u64 ft = unix_to_filetime(st.st_mtime);
        wr64(out + 4, ft); wr64(out + 12, ft); wr64(out + 20, ft);
        wr32(out + 28, (u32)((u64)st.st_size >> 32));
        wr32(out + 32, (u32)st.st_size);
        snprintf((char*)H(out + 44), 260, "%s", e->d_name);
        return 1;
    }
    return 0;
}
static void nt_FindFirstFileA(Cpu* c) {
    const char* spec = gstr(ARG(0));
    char dirw[600], pat[260];
    const char* s = strrchr(spec, '\\');
    if (!s) s = strrchr(spec, '/');
    if (s) { snprintf(dirw, sizeof dirw, "%.*s", (int)(s - spec), spec); snprintf(pat, sizeof pat, "%s", s + 1); }
    else { snprintf(dirw, sizeof dirw, "."); snprintf(pat, sizeof pat, "%s", spec); }
    if (!strcmp(pat, "*.*")) strcpy(pat, "*");
    FindObj* f = calloc(1, sizeof *f);
    host_path(dirw, f->dir, sizeof f->dir);
    snprintf(f->pat, sizeof f->pat, "%s", pat);
    f->d = opendir(f->dir);
    if (!f->d || !find_next(f, ARG(1))) {
        if (f->d) closedir(f->d);
        free(f);
        set_last_error(ERR_FILE_NOT_FOUND);
        RET(INVALID_HANDLE, 2);
    }
    RET(handle_new(H_FIND, f), 2);
}
static void nt_FindNextFileA(Cpu* c) {
    HObj* o = handle_get(ARG(0));
    if (!o || o->type != H_FIND) RET(0, 2);
    if (find_next(o->p, ARG(1))) RET(1, 2);
    set_last_error(ERR_NO_MORE_FILES);
    RET(0, 2);
}
static void nt_FindClose(Cpu* c) {
    HObj* o = handle_get(ARG(0));
    if (o && o->type == H_FIND) { FindObj* f = o->p; closedir(f->d); free(f); handle_close(ARG(0)); }
    RET(1, 1);
}

// file mappings: the file is read into guest memory (the game maps MPQs read-only)
typedef struct { u32 file; u32 size; } MapObj;
static void nt_CreateFileMappingA(Cpu* c) {
    FileObj* f = file_of(ARG(0));
    if (!f) RET(0, 6);
    MapObj* m = calloc(1, sizeof *m);
    m->file = ARG(0);
    struct stat st;
    fstat(f->fd, &st);
    m->size = ARG(4) ? ARG(4) : (u32)st.st_size;
    RET(handle_new(H_MAPPING, m), 6);
}
typedef struct { u32 addr, size; } View;
static View g_views[256];
static void nt_MapViewOfFile(Cpu* c) {
    HObj* o = handle_get(ARG(0));
    if (!o || o->type != H_MAPPING) RET(0, 5);
    MapObj* m = o->p;
    FileObj* f = file_of(m->file);
    u32 off = ARG(3), n = ARG(4) ? ARG(4) : m->size - off;
    u32 a = gvalloc(n);
    LOG("MapViewOfFile(%s, off %u, %u KB) = %08X", f ? f->path : "?", off, n >> 10, a);
    if (!a) RET(0, 5);
    if (f) pread(f->fd, H(a), n, off);
    for (int i = 0; i < 256; ++i) if (!g_views[i].addr) { g_views[i].addr = a; g_views[i].size = n; break; }
    RET(a, 5);
}
static void nt_UnmapViewOfFile(Cpu* c) {
    u32 a = ARG(0);
    for (int i = 0; i < 256; ++i) if (g_views[i].addr == a) { gvfree(a); g_views[i].addr = 0; }
    RET(1, 1);
}

// ------------------------------------------------------------------ handles: close, sync objects, threads
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int signaled, manual;
} Event;

typedef struct {
    pthread_t th;
    u32 start, param;
    int done, suspended;
    u32 exit_code, tid;
    pthread_mutex_t mu;
    pthread_cond_t cv;
} Thread;

static void nt_CloseHandle(Cpu* c) {
    u32 h = ARG(0);
    HObj* o = handle_get(h);
    if (!o) RET(1, 1);
    if (o->type == H_FILE) { FileObj* f = o->p; close(f->fd); free(f); }
    else if (o->type == H_MAPPING) free(o->p);
    handle_close(h);
    RET(1, 1);
}

static Event* event_new(int manual, int initial) {
    Event* e = calloc(1, sizeof *e);
    pthread_mutex_init(&e->mu, NULL);
    pthread_cond_init(&e->cv, NULL);
    e->manual = manual; e->signaled = initial;
    return e;
}
static void nt_CreateEventA(Cpu* c) { RET(handle_new(H_EVENT, event_new(ARG(1), ARG(2))), 4); }
static void nt_OpenEventA(Cpu* c) { set_last_error(ERR_FILE_NOT_FOUND); RET(0, 3); }
static void nt_SetEvent(Cpu* c) {
    HObj* o = handle_get(ARG(0));
    if (!o || o->type != H_EVENT) RET(0, 1);
    Event* e = o->p;
    pthread_mutex_lock(&e->mu);
    e->signaled = 1;
    pthread_cond_broadcast(&e->cv);
    pthread_mutex_unlock(&e->mu);
    RET(1, 1);
}
static void nt_ResetEvent(Cpu* c) {
    HObj* o = handle_get(ARG(0));
    if (!o || o->type != H_EVENT) RET(0, 1);
    Event* e = o->p;
    pthread_mutex_lock(&e->mu);
    e->signaled = 0;
    pthread_mutex_unlock(&e->mu);
    RET(1, 1);
}

static void deadline(struct timespec* ts, u32 ms) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

// wait on one object: 0 = signaled, 0x102 = timeout
static u32 wait_one(u32 h, u32 ms) {
    HObj* o = handle_get(h);
    if (!o) return 0xFFFFFFFFu;
    struct timespec ts;
    if (ms != 0xFFFFFFFFu) deadline(&ts, ms);
    if (o->type == H_EVENT) {
        Event* e = o->p;
        pthread_mutex_lock(&e->mu);
        while (!e->signaled) {
            if (ms == 0) { pthread_mutex_unlock(&e->mu); return 0x102; }
            int r = ms == 0xFFFFFFFFu ? pthread_cond_wait(&e->cv, &e->mu) : pthread_cond_timedwait(&e->cv, &e->mu, &ts);
            if (r == ETIMEDOUT) { pthread_mutex_unlock(&e->mu); return 0x102; }
        }
        if (!e->manual) e->signaled = 0;
        pthread_mutex_unlock(&e->mu);
        return 0;
    }
    if (o->type == H_THREAD) {
        Thread* t = o->p;
        pthread_mutex_lock(&t->mu);
        while (!t->done) {
            if (ms == 0) { pthread_mutex_unlock(&t->mu); return 0x102; }
            int r = ms == 0xFFFFFFFFu ? pthread_cond_wait(&t->cv, &t->mu) : pthread_cond_timedwait(&t->cv, &t->mu, &ts);
            if (r == ETIMEDOUT) { pthread_mutex_unlock(&t->mu); return 0x102; }
        }
        pthread_mutex_unlock(&t->mu);
        return 0;
    }
    return 0;
}

static void nt_WaitForSingleObject(Cpu* c) { RET(wait_one(ARG(0), ARG(1)), 2); }

static void nt_WaitForMultipleObjects(Cpu* c) {
    u32 n = ARG(0), arr = ARG(1), all = ARG(2), ms = ARG(3);
    u32 waited = 0;
    for (;;) {
        if (all) {
            int ok = 1;
            for (u32 i = 0; i < n; ++i) if (wait_one(rd32(arr + 4 * i), 0) != 0) ok = 0;
            if (ok) RET(0, 4);
        } else {
            for (u32 i = 0; i < n; ++i) if (wait_one(rd32(arr + 4 * i), 0) == 0) RET(i, 4);
        }
        if (ms != 0xFFFFFFFFu && waited >= ms) RET(0x102, 4);
        usleep(1000);
        waited++;
    }
}

static void* thread_main(void* arg) {
    Thread* t = arg;
    pthread_mutex_lock(&t->mu);
    while (t->suspended) pthread_cond_wait(&t->cv, &t->mu);
    pthread_mutex_unlock(&t->mu);
    Cpu* c = cpu_new();
    t->tid = 2000 + c->thread;
    u32 r = call_guest(c, t->start, 1, &t->param);
    pthread_mutex_lock(&t->mu);
    t->exit_code = r;
    t->done = 1;
    pthread_cond_broadcast(&t->cv);
    pthread_mutex_unlock(&t->mu);
    thread_release(c->thread);
    return NULL;
}

static void nt_CreateThread(Cpu* c) {
    Thread* t = calloc(1, sizeof *t);
    t->start = ARG(2); t->param = ARG(3);
    t->suspended = (ARG(4) & 4) != 0;
    pthread_mutex_init(&t->mu, NULL);
    pthread_cond_init(&t->cv, NULL);
    u32 h = handle_new(H_THREAD, t);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64u << 20);
    pthread_create(&t->th, &at, thread_main, t);
    pthread_detach(t->th);
    if (ARG(5)) wr32(ARG(5), 3000 + (h & 0xFFF));
    RET(h, 6);
}
static void nt_ResumeThread(Cpu* c) {
    HObj* o = handle_get(ARG(0));
    if (o && o->type == H_THREAD) {
        Thread* t = o->p;
        pthread_mutex_lock(&t->mu);
        int was = t->suspended;
        t->suspended = 0;
        pthread_cond_broadcast(&t->cv);
        pthread_mutex_unlock(&t->mu);
        RET(was, 1);
    }
    RET(0, 1);
}
static void nt_SuspendThread(Cpu* c) { RET(0, 1); }
static void nt_ExitThread(Cpu* c) {
    // unwinding the host stack of a guest thread: only reached from a thread's own code
    LOG("ExitThread(%u)", ARG(0));
    pthread_exit(NULL);
}
static void nt_GetThreadPriority(Cpu* c) { RET(0, 1); }
static void nt_SetThreadPriority(Cpu* c) { RET(1, 2); }
static void nt_GetThreadContext(Cpu* c) { RET(0, 2); }
static void nt_GetCurrentThread(Cpu* c) { RET(0xFFFFFFFEu, 0); }
static void nt_GetCurrentThreadId(Cpu* c) { RET(rd32(c->fs_base + 0x24), 0); }
static void nt_GetCurrentProcess(Cpu* c) { RET(0xFFFFFFFFu, 0); }
static void nt_GetCurrentProcessId(Cpu* c) { RET(1000, 0); }
static void nt_DuplicateHandle(Cpu* c) {
    u32 src = ARG(1), out = ARG(3);
    if (out) wr32(out, src);
    RET(1, 7);
}
static void nt_ExitProcess(Cpu* c) { LOG("ExitProcess(%u)", ARG(0)); exit((int)ARG(0)); }
static void nt_TerminateProcess(Cpu* c) { LOG("TerminateProcess(%u)", ARG(1)); exit((int)ARG(1)); }
static void nt_GetExitCodeProcess(Cpu* c) { if (ARG(1)) wr32(ARG(1), 259); RET(1, 2); }
static void nt_CreateProcessA(Cpu* c) { LOG("CreateProcessA(%s, %s) refused", gstr(ARG(0)) ? gstr(ARG(0)) : "", gstr(ARG(1)) ? gstr(ARG(1)) : ""); RET(0, 10); }
static void nt_ReadProcessMemory(Cpu* c) {
    memmove(H(ARG(2)), H(ARG(1)), ARG(3));
    if (ARG(4)) wr32(ARG(4), ARG(3));
    RET(1, 5);
}
static void nt_FlushInstructionCache(Cpu* c) { RET(1, 3); }
static void nt_CreateIoCompletionPort(Cpu* c) { RET(0, 4); }
static void nt_GetQueuedCompletionStatus(Cpu* c) { usleep(ARG(4) == 0xFFFFFFFFu ? 100000 : ARG(4) * 1000); RET(0, 5); }

// Critical sections with Windows' semantics: an owner thread and a recursion count, and Leave does not
// check the caller (D2CMP enters on the game thread and its loader thread leaves). Keyed by the guest
// CRITICAL_SECTION's address; one host mutex + condvar guards them all.
typedef struct CS { u32 addr; int owner; u32 count; struct CS* next; } CS;
static CS* g_cs[1024];
static pthread_mutex_t g_csmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cscv = PTHREAD_COND_INITIALIZER;
static CS* cs_find(u32 a) {  // with g_csmu held
    u32 b = (a >> 3) & 1023;
    for (CS* s = g_cs[b]; s; s = s->next)
        if (s->addr == a) return s;
    CS* s = calloc(1, sizeof *s);
    s->addr = a;
    s->next = g_cs[b];
    g_cs[b] = s;
    return s;
}
static void cs_reset(u32 a) {
    pthread_mutex_lock(&g_csmu);
    CS* s = cs_find(a);
    s->owner = 0; s->count = 0;
    pthread_cond_broadcast(&g_cscv);
    pthread_mutex_unlock(&g_csmu);
}
// Windows' layout: {DebugInfo, LockCount=-1, RecursionCount, OwningThread, LockSemaphore, SpinCount}; Fog
// validates it, including DebugInfo->CriticalSection == cs and a non-empty ProcessLocksList.
static void nt_InitializeCriticalSection(Cpu* c) {
    u32 cs = ARG(0), dbg = gcalloc(32);
    memset(H(cs), 0, 24);
    wr32(cs, dbg);
    wr32(cs + 4, 0xFFFFFFFFu);
    wr32(dbg + 4, cs);
    wr32(dbg + 8, dbg + 8);
    wr32(dbg + 12, dbg + 8);
    cs_reset(cs);
    RET(0, 1);
}
static void nt_DeleteCriticalSection(Cpu* c) { cs_reset(ARG(0)); RET(0, 1); }
static void nt_EnterCriticalSection(Cpu* c) {
    u32 a = ARG(0);
    int me = c->thread + 1;
    pthread_mutex_lock(&g_csmu);
    CS* s = cs_find(a);
    while (s->owner && s->owner != me) pthread_cond_wait(&g_cscv, &g_csmu);
    s->owner = me;
    s->count++;
    wr32(a + 8, s->count);                       // RecursionCount
    wr32(a + 12, rd32(c->fs_base + 0x24));       // OwningThread
    pthread_mutex_unlock(&g_csmu);
    RET(0, 1);
}
static void nt_LeaveCriticalSection(Cpu* c) {
    u32 a = ARG(0);
    pthread_mutex_lock(&g_csmu);
    CS* s = cs_find(a);
    if (s->count && --s->count == 0) {
        s->owner = 0;
        wr32(a + 12, 0);
        pthread_cond_broadcast(&g_cscv);
    }
    wr32(a + 8, s->count);
    pthread_mutex_unlock(&g_csmu);
    RET(0, 1);
}
static void nt_InterlockedIncrement(Cpu* c) { RET(__atomic_add_fetch((u32*)H(ARG(0)), 1, __ATOMIC_SEQ_CST), 1); }
static void nt_InterlockedDecrement(Cpu* c) { RET(__atomic_sub_fetch((u32*)H(ARG(0)), 1, __ATOMIC_SEQ_CST), 1); }
static void nt_InterlockedExchange(Cpu* c) { RET(__atomic_exchange_n((u32*)H(ARG(0)), ARG(1), __ATOMIC_SEQ_CST), 2); }

// TLS (slots in the TEB at 0xE10, Windows' layout)
static u32 g_tls_used[64];
static void nt_TlsAlloc(Cpu* c) {
    for (u32 i = 0; i < 64; ++i)
        if (!__atomic_exchange_n(&g_tls_used[i], 1, __ATOMIC_SEQ_CST)) RET(i, 0);
    RET(0xFFFFFFFFu, 0);
}
static void nt_TlsFree(Cpu* c) { if (ARG(0) < 64) g_tls_used[ARG(0)] = 0; RET(1, 1); }
static void nt_TlsGetValue(Cpu* c) { set_last_error(0); RET(ARG(0) < 64 ? rd32(c->fs_base + 0xE10 + 4 * ARG(0)) : 0, 1); }
static void nt_TlsSetValue(Cpu* c) { if (ARG(0) < 64) wr32(c->fs_base + 0xE10 + 4 * ARG(0), ARG(1)); RET(ARG(0) < 64, 2); }

static void nt_Sleep(Cpu* c) {
    u32 ms = ARG(0);
    if (ms) usleep(ms * 1000);
    else sched_yield();
    RET(0, 1);
}

// ------------------------------------------------------------------ memory
#define PROCESS_HEAP 0x00F00000u
static void nt_GetProcessHeap(Cpu* c) { RET(PROCESS_HEAP, 0); }
static void nt_HeapCreate(Cpu* c) { static u32 n; RET(PROCESS_HEAP + 0x1000 * ++n, 3); }
static void nt_HeapDestroy(Cpu* c) { RET(1, 1); }
static void nt_HeapAlloc(Cpu* c) {
    u32 a = (ARG(1) & 8) ? gcalloc(ARG(2)) : galloc(ARG(2));
    RET(a, 3);
}
static void nt_HeapFree(Cpu* c) { gfree(ARG(2)); RET(1, 3); }
static void nt_HeapReAlloc(Cpu* c) {
    u32 flags = ARG(1), p = ARG(2), n = ARG(3);
    u32 old = gsize(p);
    if (flags & 0x10) {  // HEAP_REALLOC_IN_PLACE_ONLY
        u32 cap = old == 0xFFFFFFFFu ? 0 : rd32(p - 16 + 12) - 16;
        if (n <= cap) { wr32(p - 16, n); RET(p, 4); }
        RET(0, 4);
    }
    u32 a = grealloc(p, n);
    if (a && (flags & 8) && old != 0xFFFFFFFFu && n > old) memset(H(a + old), 0, n - old);
    RET(a, 4);
}
static void nt_HeapSize(Cpu* c) { RET(gsize(ARG(2)), 3); }
static void nt_GlobalAlloc(Cpu* c) { RET((ARG(0) & 0x40) ? gcalloc(ARG(1)) : galloc(ARG(1)), 2); }
static void nt_GlobalFree(Cpu* c) { gfree(ARG(0)); RET(0, 1); }
static void nt_GlobalLock(Cpu* c) { RET(ARG(0), 1); }
static void nt_GlobalUnlock(Cpu* c) { RET(1, 1); }
static void nt_GlobalMemoryStatus(Cpu* c) {
    u32 p = ARG(0);
    wr32(p, 32); wr32(p + 4, 30);
    wr32(p + 8, 512u << 20); wr32(p + 12, 384u << 20);
    wr32(p + 16, 1024u << 20); wr32(p + 20, 900u << 20);
    wr32(p + 24, 2047u << 20); wr32(p + 28, 1536u << 20);
    RET(0, 1);
}
static void nt_VirtualAlloc(Cpu* c) {
    u32 addr = ARG(0), size = ARG(1);
    u32 b, s;
    if (addr && gv_owned(addr, &b, &s)) RET(addr, 4);  // commit inside a reservation
    u32 a = gvalloc(size);
    if (size >= (4u << 20) || !a) LOG("VirtualAlloc(%u KB, type %X) = %08X from %08X", size >> 10, ARG(2), a, rd32(c->r[ESP]));
    if (size >= (8u << 20)) { extern void guest_stack_scan(Cpu*, int); guest_stack_scan(c, 30); }
    RET(a, 4);
}
static void nt_VirtualFree(Cpu* c) {
    if (ARG(2) & 0x8000) gvfree(ARG(0));  // MEM_RELEASE
    RET(1, 3);
}
static void nt_VirtualQuery(Cpu* c) {
    u32 a = ARG(0), mbi = ARG(1);
    u32 b = a & ~0xFFFu, s = 0x1000;
    gv_owned(a, &b, &s);
    memset(H(mbi), 0, 28);
    wr32(mbi, a & ~0xFFFu); wr32(mbi + 4, b); wr32(mbi + 8, 0x40);
    wr32(mbi + 12, b + s - (a & ~0xFFFu)); wr32(mbi + 16, 0x1000); wr32(mbi + 20, 0x40); wr32(mbi + 24, 0x20000);
    RET(28, 3);
}
static void nt_VirtualProtect(Cpu* c) { if (ARG(3)) wr32(ARG(3), 0x40); RET(1, 4); }
static void nt_VirtualLock(Cpu* c) { RET(1, 2); }
static void nt_VirtualUnlock(Cpu* c) { RET(1, 2); }
static void nt_IsBadReadPtr(Cpu* c) { RET(ARG(0) < 0x10000, 2); }
static void nt_IsBadWritePtr(Cpu* c) { RET(ARG(0) < 0x10000, 2); }
static void nt_IsBadCodePtr(Cpu* c) { RET(ARG(0) < 0x10000, 1); }

// ------------------------------------------------------------------ modules and resources
// native "modules": handles that GetProcAddress resolves among the natives
static const char* const g_native_dlls[] = {"kernel32.dll", "user32.dll", "gdi32.dll", "advapi32.dll", "winmm.dll",
    "wsock32.dll", "ws2_32.dll", "dsound.dll", "ddraw.dll", "version.dll", "shell32.dll", "comdlg32.dll", "imm32.dll",
    "glide3x.dll", "binkw32.dll", "smackw32.dll", "ijl11.dll", "ntdll.dll", "ole32.dll"};
#define NATIVE_MOD_BASE 0x7E000000u
static int native_dll_index(const char* name) {
    const char* b = strrchr(name, '\\');
    b = b ? b + 1 : name;
    for (unsigned i = 0; i < sizeof g_native_dlls / sizeof *g_native_dlls; ++i) {
        size_t n = strlen(g_native_dlls[i]);
        if (!strcasecmp(b, g_native_dlls[i]) || (!strncasecmp(b, g_native_dlls[i], n - 4) && !b[n - 4])) return i;
    }
    return -1;
}
static void nt_GetModuleHandleA(Cpu* c) {
    const char* n = gstr(ARG(0));
    if (!n) RET(0x00400000u, 1);
    u32 h = module_handle(n);
    if (!h) { int i = native_dll_index(n); if (i >= 0) h = NATIVE_MOD_BASE + i * 0x10000; }
    if (!h) set_last_error(126);
    RET(h, 1);
}
extern u32 module_load_data(const char* name);
static void nt_LoadLibraryA(Cpu* c) {
    const char* n = gstr(ARG(0));
    u32 h = module_load(n);
    if (!h && native_dll_index(n) < 0) h = module_load_data(n);
    if (!h) { int i = native_dll_index(n); if (i >= 0) h = NATIVE_MOD_BASE + i * 0x10000; }
    LOG("LoadLibrary(%s) = %08X", n, h);
    if (!h) set_last_error(126);
    RET(h, 1);
}
static void nt_LoadLibraryExA(Cpu* c) {
    const char* n = gstr(ARG(0));
    u32 h = module_load(n);
    if (!h && native_dll_index(n) < 0) h = module_load_data(n);
    if (!h) { int i = native_dll_index(n); if (i >= 0) h = NATIVE_MOD_BASE + i * 0x10000; }
    LOG("LoadLibraryEx(%s) = %08X", n, h);
    RET(h, 3);
}
static void nt_FreeLibrary(Cpu* c) { RET(1, 1); }
static void nt_GetProcAddress(Cpu* c) {
    u32 h = ARG(0), p = ARG(1);
    const char* name = p >= 0x10000 ? gstr(p) : NULL;
    unsigned ord = p < 0x10000 ? p : 0;
    u32 a = 0;
    if (h >= NATIVE_MOD_BASE && h < NATIVE_MOD_BASE + 0x200000) {
        const char* dll = g_native_dlls[(h - NATIVE_MOD_BASE) >> 16];
        extern int native_exists(const char* dll, const char* name, unsigned ordinal);
        if (native_exists(dll, name, ord)) a = native_thunk(dll, name, ord);
        if (!a && getenv("D2TRACE")) LOG("GetProcAddress(%s, %s) -> not provided", dll, name ? name : "#");
    } else a = module_export(h, name, ord);
    if (!a) set_last_error(127);
    RET(a, 2);
}
static void nt_GetModuleFileNameA(Cpu* c) {
    u32 h = ARG(0), buf = ARG(1), n = ARG(2);
    const char* f = module_name_of(h ? h : 0x00400000u);
    char p[300];
    snprintf(p, sizeof p, "C:\\D2\\%s", f ? f : "Game.exe");
    u32 l = (u32)strlen(p);
    if (l >= n) l = n - 1;
    memcpy(H(buf), p, l);
    wr8(buf + l, 0);
    RET(l, 3);
}

// resources: walk the image's .rsrc tree (type -> name -> language)
static u32 rsrc_find_entry(u32 base, u32 dir, u32 id, int first) {
    u32 nnamed = rd16(dir + 12), nid = rd16(dir + 14);
    u32 e = dir + 16;
    for (u32 i = 0; i < nnamed + nid; ++i, e += 8) {
        u32 nm = rd32(e), off = rd32(e + 4);
        int match = first;
        if (!first) {
            if (id < 0x10000) match = !(nm & 0x80000000u) && nm == id;
            else if (nm & 0x80000000u) {
                // compare UTF-16 name with the ASCII string at id
                u32 s = base + (nm & 0x7FFFFFFFu);
                u32 len = rd16(s);
                const char* want = gstr(id);
                const char* w = want[0] == '#' ? NULL : want;
                if (w && strlen(w) == len) {
                    match = 1;
                    for (u32 k = 0; k < len; ++k)
                        if (toupper(rd16(s + 2 + 2 * k)) != toupper((unsigned char)w[k])) { match = 0; break; }
                }
            }
        }
        if (match) return off;
    }
    return 0xFFFFFFFFu;
}
static void nt_FindResourceA(Cpu* c) {
    u32 h = ARG(0) ? ARG(0) : 0x00400000u, name = ARG(1), type = ARG(2);
    if (name >= 0x10000 && gstr(name)[0] == '#') name = (u32)atoi(gstr(name) + 1);
    if (type >= 0x10000 && gstr(type)[0] == '#') type = (u32)atoi(gstr(type) + 1);
    u32 pe = h + rd32(h + 0x3C);
    u32 rva = rd32(pe + 24 + 96 + 2 * 8);
    if (!rva) RET(0, 3);
    u32 root = h + rva;
    u32 o = rsrc_find_entry(root, root, type, 0);
    if (o == 0xFFFFFFFFu) RET(0, 3);
    u32 d2 = root + (o & 0x7FFFFFFFu);
    o = rsrc_find_entry(root, d2, name, 0);
    if (o == 0xFFFFFFFFu) RET(0, 3);
    u32 d3 = root + (o & 0x7FFFFFFFu);
    o = rsrc_find_entry(root, d3, 0, 1);
    if (o == 0xFFFFFFFFu) RET(0, 3);
    RET(root + o, 3);  // the data entry: {rva, size, codepage, reserved}
}
static void nt_LoadResource(Cpu* c) { u32 h = ARG(0) ? ARG(0) : 0x00400000u; RET(ARG(1) ? h + rd32(ARG(1)) : 0, 2); }
static void nt_LockResource(Cpu* c) { RET(ARG(0), 1); }
static void nt_SizeofResource(Cpu* c) { RET(ARG(1) ? rd32(ARG(1) + 4) : 0, 2); }
static void nt_FreeResource(Cpu* c) { RET(0, 1); }

// ------------------------------------------------------------------ process environment
static u32 g_cmdline_g, g_env_g, g_envw_g;
void k32_init(const char* cmdline) {
    g_cmdline_g = gstrdup(cmdline);
    g_env_g = gcalloc(16);
    g_envw_g = gcalloc(16);
    wr32(0x00010000u + 0x18, PROCESS_HEAP);  // PEB.ProcessHeap
}
static void nt_GetCommandLineA(Cpu* c) { RET(g_cmdline_g, 0); }
static void nt_GetEnvironmentStrings(Cpu* c) { RET(g_env_g, 0); }
static void nt_GetEnvironmentStringsW(Cpu* c) { RET(g_envw_g, 0); }
static void nt_FreeEnvironmentStringsA(Cpu* c) { RET(1, 1); }
static void nt_FreeEnvironmentStringsW(Cpu* c) { RET(1, 1); }
static void nt_GetEnvironmentVariableA(Cpu* c) { set_last_error(203); RET(0, 3); }
static void nt_GetStartupInfoA(Cpu* c) { memset(H(ARG(0)), 0, 68); wr32(ARG(0), 68); RET(0, 1); }
static void nt_GetStdHandle(Cpu* c) { RET(ARG(0), 1); }
static void nt_SetStdHandle(Cpu* c) { RET(1, 2); }
static void nt_SetHandleCount(Cpu* c) { RET(ARG(0), 1); }
static void nt_GetLastError(Cpu* c) { RET(rd32(c->fs_base + 0x34), 0); }
static void nt_SetLastError(Cpu* c) { wr32(c->fs_base + 0x34, ARG(0)); RET(0, 1); }
static void nt_GetVersion(Cpu* c) { RET(0x0A280105u, 0); }  // Windows XP SP2
static void nt_GetVersionExA(Cpu* c) {
    u32 p = ARG(0);
    u32 sz = rd32(p);
    memset(H(p + 4), 0, sz > 4 ? sz - 4 : 0);
    wr32(p + 4, 5); wr32(p + 8, 1); wr32(p + 12, 2600); wr32(p + 16, 2);
    strcpy((char*)H(p + 20), "Service Pack 2");
    RET(1, 1);
}
static void nt_GetSystemInfo(Cpu* c) {
    u32 p = ARG(0);
    memset(H(p), 0, 36);
    wr32(p + 4, 4096); wr32(p + 8, 0x10000); wr32(p + 12, 0x7FFEFFFF);
    wr32(p + 16, 1); wr32(p + 20, 1); wr32(p + 24, 586); wr32(p + 28, 0x10000);
    wr16(p + 32, 6); wr16(p + 34, 0x0602);
    RET(0, 1);
}
static void nt_GetComputerNameA(Cpu* c) { gput(ARG(0), "D2PHONE", 8); if (ARG(1)) wr32(ARG(1), 7); RET(1, 2); }
static void nt_GetWindowsDirectoryA(Cpu* c) { gput(ARG(0), "C:\\WINDOWS", 11); RET(10, 2); }
static void nt_GetDiskFreeSpaceA(Cpu* c) {
    if (ARG(1)) wr32(ARG(1), 8);
    if (ARG(2)) wr32(ARG(2), 512);
    if (ARG(3)) wr32(ARG(3), 0x40000);  // 1 GB free of 2 GB: the game multiplies these in 32 bits
    if (ARG(4)) wr32(ARG(4), 0x80000);
    RET(1, 5);
}
static void nt_GetDriveTypeA(Cpu* c) { RET(3, 1); }
static void nt_GetLogicalDriveStringsA(Cpu* c) { if (ARG(0) >= 5) gput(ARG(1), "C:\\\0", 5); RET(4, 2); }
static void nt_GetVolumeInformationA(Cpu* c) {
    if (ARG(1) && ARG(2)) gput(ARG(1), "D2", 3);
    if (ARG(3)) wr32(ARG(3), 0x1D2B00B5u);
    if (ARG(4)) wr32(ARG(4), 255);
    if (ARG(5)) wr32(ARG(5), 3);
    if (ARG(6) && ARG(7)) gput(ARG(6), "NTFS", 5);
    RET(1, 8);
}
static void nt_OutputDebugStringA(Cpu* c) { LOG("debug: %s", gstr(ARG(0))); RET(0, 1); }
static void nt_FormatMessageA(Cpu* c) { RET(0, 7); }
static void nt_RaiseException(Cpu* c) { LOG("RaiseException(%08X) ignored", ARG(0)); RET(0, 4); }
static void nt_RtlUnwind(Cpu* c) {
    // pop the SEH chain to the target frame (the handlers' __except bodies run in their own code)
    u32 target = ARG(0), rv = ARG(3);
    if (target) wr32(c->fs_base, target);
    RET(rv, 4);
}
static void nt_SetUnhandledExceptionFilter(Cpu* c) { RET(0, 1); }
static void nt_UnhandledExceptionFilter(Cpu* c) { RET(1, 1); }
static void nt_MulDiv(Cpu* c) {
    s32 a = (s32)ARG(0), b = (s32)ARG(1), d = (s32)ARG(2);
    if (!d) RET(0xFFFFFFFFu, 3);
    s64 p = (s64)a * b;
    RET((s32)((p + (p >= 0 ? d / 2 : -d / 2)) / d), 3);
}

// ------------------------------------------------------------------ time
static u64 mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void nt_GetTickCount(Cpu* c) { RET((u32)mono_ms(), 0); }
static void nt_timeGetTime(Cpu* c) { RET((u32)mono_ms(), 0); }
static void nt_QueryPerformanceFrequency(Cpu* c) { wr64(ARG(0), 1000000); RET(1, 1); }
static void nt_QueryPerformanceCounter(Cpu* c) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    wr64(ARG(0), (u64)ts.tv_sec * 1000000 + ts.tv_nsec / 1000);
    RET(1, 1);
}
static void put_systemtime(u32 p, struct tm* t, int ms) {
    wr16(p, t->tm_year + 1900); wr16(p + 2, t->tm_mon + 1); wr16(p + 4, t->tm_wday); wr16(p + 6, t->tm_mday);
    wr16(p + 8, t->tm_hour); wr16(p + 10, t->tm_min); wr16(p + 12, t->tm_sec); wr16(p + 14, ms);
}
static void nt_GetSystemTime(Cpu* c) {
    struct timeval tv; gettimeofday(&tv, NULL);
    struct tm t; gmtime_r(&tv.tv_sec, &t);
    put_systemtime(ARG(0), &t, tv.tv_usec / 1000);
    RET(0, 1);
}
static void nt_GetLocalTime(Cpu* c) {
    struct timeval tv; gettimeofday(&tv, NULL);
    struct tm t; localtime_r(&tv.tv_sec, &t);
    put_systemtime(ARG(0), &t, tv.tv_usec / 1000);
    RET(0, 1);
}
static void nt_GetSystemTimeAsFileTime(Cpu* c) {
    struct timeval tv; gettimeofday(&tv, NULL);
    wr64(ARG(0), unix_to_filetime(tv.tv_sec) + tv.tv_usec * 10ull);
    RET(0, 1);
}
static void nt_FileTimeToLocalFileTime(Cpu* c) {
    time_t now = time(NULL);
    struct tm lt; localtime_r(&now, &lt);
    wr64(ARG(1), rd64(ARG(0)) + (s64)lt.tm_gmtoff * 10000000ll);
    RET(1, 2);
}
static void nt_FileTimeToSystemTime(Cpu* c) {
    u64 ft = rd64(ARG(0));
    time_t t = (time_t)(ft / 10000000ull - 11644473600ull);
    struct tm tm; gmtime_r(&t, &tm);
    put_systemtime(ARG(1), &tm, (int)(ft / 10000 % 1000));
    RET(1, 2);
}
static void nt_SystemTimeToFileTime(Cpu* c) {
    u32 p = ARG(0);
    struct tm tm = {0};
    tm.tm_year = rd16(p) - 1900; tm.tm_mon = rd16(p + 2) - 1; tm.tm_mday = rd16(p + 6);
    tm.tm_hour = rd16(p + 8); tm.tm_min = rd16(p + 10); tm.tm_sec = rd16(p + 12);
    wr64(ARG(1), unix_to_filetime(timegm(&tm)) + rd16(p + 14) * 10000ull);
    RET(1, 2);
}
static void nt_CompareFileTime(Cpu* c) {
    u64 a = rd64(ARG(0)), b = rd64(ARG(1));
    RET(a < b ? 0xFFFFFFFFu : a > b, 2);
}
static void nt_GetTimeZoneInformation(Cpu* c) { memset(H(ARG(0)), 0, 172); RET(0, 1); }
static void nt_GetDateFormatA(Cpu* c) {
    u32 st = ARG(2), out = ARG(4), n = ARG(5);
    char b[32];
    if (st) snprintf(b, sizeof b, "%02u.%02u.%04u", rd16(st + 6), rd16(st + 2), rd16(st));
    else { time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); strftime(b, sizeof b, "%d.%m.%Y", &tm); }
    u32 l = (u32)strlen(b) + 1;
    if (n == 0) RET(l, 6);
    if (n < l) RET(0, 6);
    gput(out, b, l);
    RET(l, 6);
}

// ------------------------------------------------------------------ code pages (ACP = 1251)
static const u16 cp1251_hi[128] = {
    0x0402,0x0403,0x201A,0x0453,0x201E,0x2026,0x2020,0x2021,0x20AC,0x2030,0x0409,0x2039,0x040A,0x040C,0x040B,0x040F,
    0x0452,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x0098,0x2122,0x0459,0x203A,0x045A,0x045C,0x045B,0x045F,
    0x00A0,0x040E,0x045E,0x0408,0x00A4,0x0490,0x00A6,0x00A7,0x0401,0x00A9,0x0404,0x00AB,0x00AC,0x00AD,0x00AE,0x0407,
    0x00B0,0x00B1,0x0406,0x0456,0x0491,0x00B5,0x00B6,0x00B7,0x0451,0x2116,0x0454,0x00BB,0x0458,0x0405,0x0455,0x0457,
};
static u16 cp_to_w(u8 b) {
    if (b < 0x80) return b;
    if (b < 0xC0) return cp1251_hi[b - 0x80];
    return 0x0410 + (b - 0xC0);
}
static u8 w_to_cp(u16 w) {
    if (w < 0x80) return (u8)w;
    if (w >= 0x0410 && w <= 0x044F) return (u8)(0xC0 + (w - 0x0410));
    for (int i = 0; i < 64; ++i) if (cp1251_hi[i] == w) return (u8)(0x80 + i);
    return '?';
}
static void nt_GetACP(Cpu* c) { RET(1251, 0); }
static void nt_GetOEMCP(Cpu* c) { RET(866, 0); }
static void nt_IsValidCodePage(Cpu* c) { RET(1, 1); }
static void nt_GetCPInfo(Cpu* c) {
    u32 p = ARG(1);
    memset(H(p), 0, 20);
    wr32(p, 1); wr8(p + 4, '?');
    RET(1, 2);
}
static void nt_MultiByteToWideChar(Cpu* c) {
    u32 src = ARG(2), out = ARG(4), outn = ARG(5);
    s32 n = (s32)ARG(3);
    if (n < 0) n = (s32)strlen(gstr(src)) + 1;
    if (outn == 0) RET(n, 6);
    s32 k = n < (s32)outn ? n : (s32)outn;
    for (s32 i = 0; i < k; ++i) wr16(out + 2 * i, cp_to_w(rd8(src + i)));
    RET(k, 6);
}
static void nt_WideCharToMultiByte(Cpu* c) {
    u32 src = ARG(2), out = ARG(4), outn = ARG(5);
    s32 n = (s32)ARG(3);
    if (n < 0) { n = 0; while (rd16(src + 2 * n)) n++; n++; }
    if (outn == 0) RET(n, 8);
    s32 k = n < (s32)outn ? n : (s32)outn;
    for (s32 i = 0; i < k; ++i) wr8(out + i, w_to_cp(rd16(src + 2 * i)));
    if (ARG(7)) wr32(ARG(7), 0);
    RET(k, 8);
}
static void nt_GetStringTypeA(Cpu* c) {
    u32 src = ARG(2), out = ARG(4);
    s32 n = (s32)ARG(3);
    if (n < 0) n = (s32)strlen(gstr(src)) + 1;
    for (s32 i = 0; i < n; ++i) {
        u8 ch = rd8(src + i);
        u16 t = 0;
        u16 w = cp_to_w(ch);
        if (isupper(ch) || (w >= 0x410 && w <= 0x42F)) t |= 1 | 0x100;
        if (islower(ch) || (w >= 0x430 && w <= 0x44F)) t |= 2 | 0x100;
        if (isdigit(ch)) t |= 4;
        if (isspace(ch)) t |= 8;
        if (ispunct(ch)) t |= 0x10;
        if (iscntrl(ch)) t |= 0x20;
        if (ch == ' ' || ch == '\t') t |= 0x40;
        if (isxdigit(ch)) t |= 0x80;
        if (isalpha(ch)) t |= 0x100;
        wr16(out + 2 * i, t);
    }
    RET(1, 5);
}
static void nt_GetStringTypeW(Cpu* c) {
    u32 src = ARG(1), out = ARG(3);
    s32 n = (s32)ARG(2);
    if (n < 0) { n = 0; while (rd16(src + 2 * n)) n++; n++; }
    for (s32 i = 0; i < n; ++i) {
        u16 w = rd16(src + 2 * i), t = 0;
        if (w < 128) {
            u8 ch = (u8)w;
            if (isupper(ch)) t |= 1 | 0x100;
            if (islower(ch)) t |= 2 | 0x100;
            if (isdigit(ch)) t |= 4;
            if (isspace(ch)) t |= 8;
            if (ispunct(ch)) t |= 0x10;
            if (iscntrl(ch)) t |= 0x20;
            if (ch == ' ' || ch == '\t') t |= 0x40;
            if (isxdigit(ch)) t |= 0x80;
        } else if (w >= 0x410 && w <= 0x42F) t = 0x101;
        else if (w >= 0x430 && w <= 0x44F) t = 0x102;
        wr16(out + 2 * i, t);
    }
    RET(1, 4);
}
static u16 wupper(u16 w) { if (w < 128) return (u16)toupper(w); if (w >= 0x430 && w <= 0x44F) return w - 0x20; if (w >= 0x450 && w <= 0x45F) return w - 0x50; return w; }
static u16 wlower(u16 w) { if (w < 128) return (u16)tolower(w); if (w >= 0x410 && w <= 0x42F) return w + 0x20; if (w >= 0x400 && w <= 0x40F) return w + 0x50; return w; }
static void nt_LCMapStringA(Cpu* c) {
    u32 flags = ARG(1), src = ARG(2), out = ARG(4), outn = ARG(5);
    s32 n = (s32)ARG(3);
    if (n < 0) n = (s32)strlen(gstr(src)) + 1;
    if (outn == 0) RET(n, 6);
    for (s32 i = 0; i < n && i < (s32)outn; ++i) {
        u16 w = cp_to_w(rd8(src + i));
        if (flags & 0x200) w = wupper(w);
        if (flags & 0x100) w = wlower(w);
        wr8(out + i, w_to_cp(w));
    }
    RET(n, 6);
}
static void nt_LCMapStringW(Cpu* c) {
    u32 flags = ARG(1), src = ARG(2), out = ARG(4), outn = ARG(5);
    s32 n = (s32)ARG(3);
    if (n < 0) { n = 0; while (rd16(src + 2 * n)) n++; n++; }
    if (outn == 0) RET(n, 6);
    for (s32 i = 0; i < n && i < (s32)outn; ++i) {
        u16 w = rd16(src + 2 * i);
        if (flags & 0x200) w = wupper(w);
        if (flags & 0x100) w = wlower(w);
        wr16(out + 2 * i, w);
    }
    RET(n, 6);
}
static void locale_str(Cpu* c, int wide) {
    u32 type = ARG(1) & 0xFFFF, out = ARG(2), n = ARG(3);
    const char* v = "";
    char b[16];
    switch (type) {
        case 0x1: v = "0419"; break;          // LOCALE_ILANGUAGE
        case 0x3: v = "RUS"; break;           // LOCALE_SABBREVLANGNAME
        case 0x7: v = "Russia"; break;
        case 0xB: v = "866"; break;           // LOCALE_IDEFAULTCODEPAGE
        case 0x1004: v = "1251"; break;       // LOCALE_IDEFAULTANSICODEPAGE
        case 0x5A: v = "ru"; break;
        case 0x14: v = "\xd1\xf3\xe1"; break;
        case 0xE: v = ","; break;             // decimal
        case 0xF: v = " "; break;             // thousand
        default: snprintf(b, sizeof b, "%u", 0); v = b;
    }
    u32 l = (u32)strlen(v) + 1;
    if (n == 0) RET(l, 4);
    if (n < l) RET(0, 4);
    for (u32 i = 0; i < l; ++i) { if (wide) wr16(out + 2 * i, (u8)v[i]); else wr8(out + i, v[i]); }
    RET(l, 4);
}
static void nt_GetLocaleInfoA(Cpu* c) { locale_str(c, 0); }
static void nt_GetLocaleInfoW(Cpu* c) { locale_str(c, 1); }
static void nt_GetSystemDefaultLCID(Cpu* c) { RET(0x419, 0); }
static void nt_GetUserDefaultLCID(Cpu* c) { RET(0x419, 0); }
static void nt_GetSystemDefaultLangID(Cpu* c) { RET(0x419, 0); }
static void nt_GetUserDefaultLangID(Cpu* c) { RET(0x419, 0); }
static void nt_IsValidLocale(Cpu* c) { RET(1, 2); }
static void nt_EnumSystemLocalesA(Cpu* c) { RET(1, 2); }

// ------------------------------------------------------------------ ini files (D2 has none by default)
static void nt_GetPrivateProfileStringA(Cpu* c) {
    u32 def = ARG(2), out = ARG(3), n = ARG(4);
    const char* d = def ? gstr(def) : "";
    u32 l = (u32)strlen(d);
    if (l >= n) l = n ? n - 1 : 0;
    if (n) { memcpy(H(out), d, l); wr8(out + l, 0); }
    RET(l, 6);
}
static void nt_GetPrivateProfileIntA(Cpu* c) { RET(ARG(2), 4); }
static void nt_GetProfileStringA(Cpu* c) {
    u32 def = ARG(2), out = ARG(3), n = ARG(4);
    const char* d = def ? gstr(def) : "";
    u32 l = (u32)strlen(d);
    if (l >= n) l = n ? n - 1 : 0;
    if (n) { memcpy(H(out), d, l); wr8(out + l, 0); }
    RET(l, 5);
}

// ------------------------------------------------------------------ registry (in memory, D2's keys)
typedef struct RVal { char key[200], name[80]; u32 type, size; u8 data[4096]; struct RVal* next; } RVal;
static RVal* g_reg;
typedef struct { char path[200]; } RKey;
static void reg_set(const char* key, const char* name, u32 type, const void* data, u32 size) {
    for (RVal* v = g_reg; v; v = v->next)
        if (!strcasecmp(v->key, key) && !strcasecmp(v->name, name)) {
            v->type = type; v->size = size > 4096 ? 4096 : size; memcpy(v->data, data, v->size);
            return;
        }
    RVal* v = calloc(1, sizeof *v);
    snprintf(v->key, sizeof v->key, "%s", key);
    snprintf(v->name, sizeof v->name, "%s", name);
    v->type = type; v->size = size > 4096 ? 4096 : size;
    memcpy(v->data, data, v->size);
    v->next = g_reg;
    g_reg = v;
}
static void reg_defaults(void) {
    static int done;
    if (done) return;
    done = 1;
    const char* k = "software\\blizzard entertainment\\diablo ii";  // matched as a path suffix
    reg_set(k, "InstallPath", 1, "C:\\D2\\", 7);
    reg_set(k, "Save Path", 1, "C:\\D2\\Save\\", 12);
    reg_set(k, "Program", 1, "C:\\D2\\Game.exe", 16);
    u32 one = 1, zero = 0;
    reg_set(k, "UseCD", 4, &zero, 4);
    reg_set(k, "SkipToBNet", 4, &zero, 4);
    reg_set(k, "Gamma", 4, &(u32){155}, 4);
    reg_set(k, "Light Quality", 4, &one, 4);
    reg_set(k, "Resolution", 4, &one, 4);  // 800x600 (the widescreen mode replaces it)
    (void)one;
}
static u32 reg_open(u32 root, const char* sub) {
    reg_defaults();
    RKey* k = calloc(1, sizeof *k);
    const char* base = "";
    HObj* o = handle_get(root);
    if (o && o->type == H_REGKEY) base = ((RKey*)o->p)->path;
    snprintf(k->path, sizeof k->path, "%s%s%s", base, base[0] && sub && sub[0] ? "\\" : "", sub ? sub : "");
    for (char* p = k->path; *p; ++p) *p = (char)tolower((unsigned char)*p);
    return handle_new(H_REGKEY, k);
}
static void nt_RegOpenKeyExA(Cpu* c) { wr32(ARG(4), reg_open(ARG(0), gstr(ARG(1)))); RET(0, 5); }
static void nt_RegOpenKeyA(Cpu* c) { wr32(ARG(2), reg_open(ARG(0), gstr(ARG(1)))); RET(0, 3); }
static void nt_RegCreateKeyExA(Cpu* c) { wr32(ARG(7), reg_open(ARG(0), gstr(ARG(1)))); if (ARG(8)) wr32(ARG(8), 2); RET(0, 9); }
static void nt_RegCreateKeyA(Cpu* c) { wr32(ARG(2), reg_open(ARG(0), gstr(ARG(1)))); RET(0, 3); }
static void nt_RegCloseKey(Cpu* c) { HObj* o = handle_get(ARG(0)); if (o && o->type == H_REGKEY) { free(o->p); handle_close(ARG(0)); } RET(0, 1); }
static void nt_RegFlushKey(Cpu* c) { RET(0, 1); }
static void nt_RegDeleteKeyA(Cpu* c) { RET(0, 2); }
static void nt_RegDeleteValueA(Cpu* c) { RET(0, 2); }
static void nt_RegEnumValueA(Cpu* c) { RET(259, 8); }
// Values are keyed by the key's path below its root (HKCU and HKLM share one store).
static const char* reg_path(u32 h) {
    HObj* o = handle_get(h);
    return o && o->type == H_REGKEY ? ((RKey*)o->p)->path : NULL;
}
static void nt_RegQueryValueExA(Cpu* c) {
    const char* path = reg_path(ARG(0));
    const char* name = ARG(1) ? gstr(ARG(1)) : "";
    u32 ptype = ARG(3), data = ARG(4), psize = ARG(5);
    if (!path) RET(6, 6);
    for (RVal* v = g_reg; v; v = v->next) {
        size_t kl = strlen(v->key), pl = strlen(path);
        int match = pl >= kl && !strcasecmp(path + pl - kl, v->key);
        if (!match || strcasecmp(v->name, name)) continue;
        if (ptype) wr32(ptype, v->type);
        if (psize) {
            u32 cap = rd32(psize);
            wr32(psize, v->size);
            if (data && cap < v->size) RET(234, 6);
        }
        if (data) gput(data, v->data, v->size);
        RET(0, 6);
    }
    RET(2, 6);
}
static void nt_RegSetValueExA(Cpu* c) {
    const char* path = reg_path(ARG(0));
    if (!path) RET(6, 6);
    reg_set(path, ARG(1) ? gstr(ARG(1)) : "", ARG(3), H(ARG(4)), ARG(5));
    RET(0, 6);
}
static void nt_GetUserNameA(Cpu* c) { gput(ARG(0), "player", 7); if (ARG(1)) wr32(ARG(1), 7); RET(1, 2); }
static void nt_FreeSid(Cpu* c) { RET(0, 1); }
static void nt_OpenSCManagerA(Cpu* c) { RET(0, 3); }
static void nt_OpenServiceA(Cpu* c) { RET(0, 3); }
static void nt_CloseServiceHandle(Cpu* c) { RET(1, 1); }
static void nt_RegisterServiceCtrlHandlerA(Cpu* c) { RET(0, 2); }
static void nt_SetServiceStatus(Cpu* c) { RET(0, 2); }
static void nt_StartServiceCtrlDispatcherA(Cpu* c) { RET(0, 1); }

// ------------------------------------------------------------------ version, shell32, comdlg32, imm32
static void nt_GetFileVersionInfoSizeA(Cpu* c) { if (ARG(1)) wr32(ARG(1), 0); RET(0, 2); }
static void nt_GetFileVersionInfoA(Cpu* c) { RET(0, 4); }
static void nt_VerQueryValueA(Cpu* c) { RET(0, 4); }
static void nt_SHAppBarMessage(Cpu* c) { RET(0, 2); }
static void nt_GetSaveFileNameA(Cpu* c) { RET(0, 1); }
static void nt_imm0_1(Cpu* c) { RET(0, 1); }
static void nt_imm0_2(Cpu* c) { RET(0, 2); }
static void nt_imm0_3(Cpu* c) { RET(0, 3); }
static void nt_imm0_4(Cpu* c) { RET(0, 4); }

#define E(dll, n) {dll, #n, 0, nt_##n}
NATIVE_TABLE(k32,
    E(K, CloseHandle), E(K, CompareFileTime), E(K, CreateDirectoryA), E(K, CreateEventA), E(K, CreateFileA),
    E(K, CreateFileMappingA), E(K, CreateIoCompletionPort), E(K, CreateProcessA), E(K, CreateThread),
    E(K, DeleteCriticalSection), E(K, DeleteFileA), E(K, DuplicateHandle), E(K, EnterCriticalSection),
    E(K, EnumSystemLocalesA), E(K, ExitProcess), E(K, ExitThread), E(K, FileTimeToLocalFileTime),
    E(K, FileTimeToSystemTime), E(K, FindClose), E(K, FindFirstFileA), E(K, FindNextFileA), E(K, FindResourceA),
    E(K, FlushFileBuffers), E(K, FlushInstructionCache), E(K, FormatMessageA), E(K, FreeEnvironmentStringsA),
    E(K, FreeEnvironmentStringsW), E(K, FreeLibrary), E(K, FreeResource), E(K, GetACP), E(K, GetCPInfo),
    E(K, GetCommandLineA), E(K, GetComputerNameA), E(K, GetCurrentDirectoryA), E(K, GetCurrentProcess),
    E(K, GetCurrentProcessId), E(K, GetCurrentThread), E(K, GetCurrentThreadId), E(K, GetDateFormatA),
    E(K, GetDiskFreeSpaceA), E(K, GetDriveTypeA), E(K, GetEnvironmentStrings), E(K, GetEnvironmentStringsW),
    E(K, GetEnvironmentVariableA), E(K, GetExitCodeProcess), E(K, GetFileAttributesA), E(K, GetFileSize),
    E(K, GetFileTime), E(K, GetFileType), E(K, GetFullPathNameA), E(K, GetLastError), E(K, GetLocalTime),
    E(K, GetLocaleInfoA), E(K, GetLocaleInfoW), E(K, GetLogicalDriveStringsA), E(K, GetModuleFileNameA),
    E(K, GetModuleHandleA), E(K, GetOEMCP), E(K, GetPrivateProfileIntA), E(K, GetPrivateProfileStringA),
    E(K, GetProcAddress), E(K, GetProcessHeap), E(K, GetProfileStringA), E(K, GetQueuedCompletionStatus),
    E(K, GetStartupInfoA), E(K, GetStdHandle), E(K, GetStringTypeA), E(K, GetStringTypeW), E(K, GetSystemDefaultLCID),
    E(K, GetSystemDefaultLangID), E(K, GetSystemInfo), E(K, GetSystemTime), E(K, GetSystemTimeAsFileTime),
    E(K, GetTempFileNameA), E(K, GetTempPathA), E(K, GetThreadContext), E(K, GetThreadPriority), E(K, GetTickCount),
    E(K, GetTimeZoneInformation), E(K, GetUserDefaultLCID), E(K, GetUserDefaultLangID), E(K, GetVersion),
    E(K, GetVersionExA), E(K, GetVolumeInformationA), E(K, GetWindowsDirectoryA), E(K, GlobalAlloc), E(K, GlobalFree),
    E(K, GlobalLock), E(K, GlobalMemoryStatus), E(K, GlobalUnlock), E(K, HeapAlloc), E(K, HeapCreate),
    E(K, HeapDestroy), E(K, HeapFree), E(K, HeapReAlloc), E(K, HeapSize), E(K, InitializeCriticalSection),
    E(K, InterlockedDecrement), E(K, InterlockedExchange), E(K, InterlockedIncrement), E(K, IsBadCodePtr),
    E(K, IsBadReadPtr), E(K, IsBadWritePtr), E(K, IsValidCodePage), E(K, IsValidLocale), E(K, LCMapStringA),
    E(K, LCMapStringW), E(K, LeaveCriticalSection), E(K, LoadLibraryA), E(K, LoadLibraryExA), E(K, LoadResource),
    E(K, LockResource), E(K, MapViewOfFile), E(K, MulDiv), E(K, MultiByteToWideChar), E(K, OpenEventA),
    E(K, OutputDebugStringA), E(K, QueryPerformanceCounter), E(K, QueryPerformanceFrequency), E(K, RaiseException),
    E(K, ReadFile), E(K, ReadProcessMemory), E(K, ResetEvent), E(K, ResumeThread), E(K, RtlUnwind),
    E(K, SetCurrentDirectoryA), E(K, SetEndOfFile), E(K, SetEvent), E(K, SetFilePointer), E(K, SetFileTime),
    E(K, SetHandleCount), E(K, SetLastError), E(K, SetStdHandle), E(K, SetThreadPriority),
    E(K, SetUnhandledExceptionFilter), E(K, SizeofResource), E(K, Sleep), E(K, SuspendThread),
    E(K, SystemTimeToFileTime), E(K, TerminateProcess), E(K, TlsAlloc), E(K, TlsFree), E(K, TlsGetValue),
    E(K, TlsSetValue), E(K, UnhandledExceptionFilter), E(K, UnmapViewOfFile), E(K, VirtualAlloc), E(K, VirtualFree),
    E(K, VirtualLock), E(K, VirtualProtect), E(K, VirtualQuery), E(K, VirtualUnlock), E(K, WaitForMultipleObjects),
    E(K, WaitForSingleObject), E(K, WideCharToMultiByte), E(K, WriteFile),
    E("advapi32.dll", CloseServiceHandle), E("advapi32.dll", FreeSid), E("advapi32.dll", GetUserNameA),
    E("advapi32.dll", OpenSCManagerA), E("advapi32.dll", OpenServiceA), E("advapi32.dll", RegCloseKey),
    E("advapi32.dll", RegCreateKeyA), E("advapi32.dll", RegCreateKeyExA), E("advapi32.dll", RegDeleteKeyA),
    E("advapi32.dll", RegDeleteValueA), E("advapi32.dll", RegEnumValueA), E("advapi32.dll", RegFlushKey),
    E("advapi32.dll", RegOpenKeyA), E("advapi32.dll", RegOpenKeyExA), E("advapi32.dll", RegQueryValueExA),
    E("advapi32.dll", RegSetValueExA), E("advapi32.dll", RegisterServiceCtrlHandlerA),
    E("advapi32.dll", SetServiceStatus), E("advapi32.dll", StartServiceCtrlDispatcherA),
    E("version.dll", GetFileVersionInfoA), E("version.dll", GetFileVersionInfoSizeA), E("version.dll", VerQueryValueA),
    E("winmm.dll", timeGetTime), E("shell32.dll", SHAppBarMessage), E("comdlg32.dll", GetSaveFileNameA),
    {"imm32.dll", "ImmGetCandidateListA", 0, nt_imm0_4}, {"imm32.dll", "ImmGetCandidateListCountA", 0, nt_imm0_2},
    {"imm32.dll", "ImmGetCompositionStringA", 0, nt_imm0_4}, {"imm32.dll", "ImmGetContext", 0, nt_imm0_1},
    {"imm32.dll", "ImmGetConversionStatus", 0, nt_imm0_3}, {"imm32.dll", "ImmGetOpenStatus", 0, nt_imm0_1},
    {"imm32.dll", "ImmIsIME", 0, nt_imm0_1}, {"imm32.dll", "ImmReleaseContext", 0, nt_imm0_2},
    {"imm32.dll", "ImmSetConversionStatus", 0, nt_imm0_3}, {"imm32.dll", "ImmSetOpenStatus", 0, nt_imm0_2},
    {"imm32.dll", "ImmSimulateHotKey", 0, nt_imm0_2},
)
