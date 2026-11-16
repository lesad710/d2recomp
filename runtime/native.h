// Native (host) implementations of the Windows API the game imports.
//
// A native function is `void nt_Name(Cpu* c)`. On entry the guest stack holds the return address at
// [esp] and the arguments above it (stdcall/cdecl push right to left). RET pops the return address
// and, for stdcall, the arguments.
#pragma once
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#define ARG(i) rd32(c->r[ESP] + 4 + 4 * (i))
#define ARGF(i) (*(gaf32*)H(c->r[ESP] + 4 + 4 * (i)))
#define RET(v, n) do { c->r[EAX] = (u32)(v); c->r[ESP] += 4 + 4 * (n); return; } while (0)
#define RETC(v) do { c->r[EAX] = (u32)(v); c->r[ESP] += 4; return; } while (0)  // cdecl

static inline const char* gstr(u32 a) { return a ? (const char*)H(a) : NULL; }
static inline u16* gwstr(u32 a) { return a ? (u16*)H(a) : NULL; }
static inline void gput(u32 dst, const void* src, u32 n) { if (dst) memcpy(H(dst), src, n); }

typedef struct {
    const char* dll;
    const char* name;  // NULL: by ordinal
    unsigned ordinal;
    GuestFn fn;
} NativeExport;

// Each native source registers its table with NATIVE_TABLE(name, ...).
typedef struct NativeTable { const NativeExport* e; unsigned n; struct NativeTable* next; } NativeTable;
void native_register(NativeTable* t);
#define NATIVE_TABLE(tag, ...) \
    static const NativeExport tag##_exports[] = { __VA_ARGS__ }; \
    static NativeTable tag##_table = { tag##_exports, sizeof(tag##_exports) / sizeof(tag##_exports[0]), 0 }; \
    __attribute__((constructor)) static void tag##_reg(void) { native_register(&tag##_table); }

// ---- runtime services (main.c)
void logf_(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
#define LOG(...) logf_(__VA_ARGS__)
u32 native_thunk(const char* dll, const char* name, unsigned ordinal);  // guest address of a native
u32 call_guest(Cpu* c, u32 fn, int nargs, const u32* args);           // stdcall/cdecl agnostic (esp restored)
Cpu* cpu_current(void);
u32 module_handle(const char* name);       // base of a loaded game module (loads it), 0 if unknown
u32 module_load(const char* name);         // LoadLibrary
u32 module_export(u32 base, const char* name, unsigned ordinal);
const char* module_name_of(u32 base);
void set_last_error(u32 e);
u32 get_last_error(void);

// guest memory allocation (heap.c)
u32 galloc(u32 size);
u32 gcalloc(u32 size);
void gfree(u32 a);
u32 grealloc(u32 a, u32 size);
u32 gsize(u32 a);
u32 gvalloc(u32 size);  // page-aligned, zeroed (VirtualAlloc)
void gvfree(u32 a);
u32 gstrdup(const char* s);

// handles (k32.c)
enum HType { H_NONE, H_FILE, H_EVENT, H_THREAD, H_FIND, H_MAPPING, H_HEAP, H_MUTEX, H_SEM, H_PROCESS, H_REGKEY, H_SOCKET };
typedef struct HObj { int type; void* p; } HObj;
u32 handle_new(int type, void* p);
HObj* handle_get(u32 h);
void handle_close(u32 h);

// paths: Windows path -> host path (case-insensitive lookup); game dir is "C:\D2"
const char* host_path(const char* win, char* out, size_t outsz);
extern char g_gamedir[1024];
extern char g_savedir[1024];
