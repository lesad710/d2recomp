// Guest CPU model for the statically recompiled Diablo II code.
//
// Every recompiled x86 function is `void f_XXXXXXXX(Cpu* c)`. Registers, flags and the x87 stack live
// in Cpu; guest memory is one 4 GB reservation at G (guest address a -> G + a), so x86 pointers stay
// 32-bit and data structures keep their layout. The guest stack is in guest memory: call pushes the
// x86 return address (code reading it, e.g. Fog's error reports, sees the real value) and ret pops it.
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

enum { EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI };

typedef struct Cpu {
    u32 r[8];
    // flags, one byte each (only computed where a later instruction reads them)
    u8 cf, zf, sf, of, pf, df, af, _pad;
    // x87: st(i) = st[(top + i) & 7]
    double st[8];
    u32 top;
    u16 fpu_cw, fpu_sw;  // control word (rounding for fist*), status word (C0..C3 from compares)
    u8 fpu_tag[8];
    // MMX / SSE (mm aliased separately: the game never mixes MMX and x87 state)
    u64 mm[8];
    union { u64 q[2]; u32 d[4]; u16 w[8]; u8 b[16]; double f64[2]; float f32[4]; } xmm[8];
    u32 mxcsr;
    u32 fs_base;  // guest address of this thread's TEB
    int thread;   // host thread index
    u32 depth;    // host call depth (overflow guard)
} Cpu;

extern u8* G;  // guest memory base
extern u32 g_w800;  // the game's "800 x 600" width (widescreen: 1068)

typedef u8 __attribute__((may_alias)) ga8;
typedef u16 __attribute__((may_alias, aligned(1))) ga16;
typedef u32 __attribute__((may_alias, aligned(1))) ga32;
typedef u64 __attribute__((may_alias, aligned(1))) ga64;
typedef float __attribute__((may_alias, aligned(1))) gaf32;
typedef double __attribute__((may_alias, aligned(1))) gaf64;

#define H(a) (G + (u32)(a))
static inline u8 rd8(u32 a) { return *(ga8*)H(a); }
static inline u16 rd16(u32 a) { return *(ga16*)H(a); }
static inline u32 rd32(u32 a) { return *(ga32*)H(a); }
static inline u64 rd64(u32 a) { return *(ga64*)H(a); }
static inline float rdf32(u32 a) { return *(gaf32*)H(a); }
static inline double rdf64(u32 a) { return *(gaf64*)H(a); }
static inline void wr8(u32 a, u8 v) { *(ga8*)H(a) = v; }
static inline void wr16(u32 a, u16 v) { *(ga16*)H(a) = v; }
static inline void wr32(u32 a, u32 v) { *(ga32*)H(a) = v; }
static inline void wr64(u32 a, u64 v) { *(ga64*)H(a) = v; }
static inline void wrf32(u32 a, float v) { *(gaf32*)H(a) = v; }
static inline void wrf64(u32 a, double v) { *(gaf64*)H(a) = v; }

// 80-bit extended reals (fld/fstp tbyte): converted through double.
double rdf80(u32 a);
void wrf80(u32 a, double v);

static inline void push32(Cpu* c, u32 v) { c->r[ESP] -= 4; wr32(c->r[ESP], v); }
static inline u32 pop32(Cpu* c) { u32 v = rd32(c->r[ESP]); c->r[ESP] += 4; return v; }

static inline u8 parity8(u32 v) { return !__builtin_parity(v & 0xFF); }

// ---- flags (full) -------------------------------------------------------------------------
#define SZP(c, r, bits) do { u32 _r = (u32)(r) & ((bits) == 32 ? 0xFFFFFFFFu : ((1u << (bits)) - 1)); \
    (c)->zf = _r == 0; (c)->sf = (_r >> ((bits) - 1)) & 1; (c)->pf = parity8(_r); } while (0)
#define FLAGS_ADD(c, a, b, r, bits) do { u32 _m = (bits) == 32 ? 0xFFFFFFFFu : ((1u << (bits)) - 1); \
    u32 _a = (u32)(a) & _m, _b = (u32)(b) & _m, _r = (u32)(r) & _m; \
    (c)->cf = _r < _a; (c)->of = ((~(_a ^ _b) & (_a ^ _r)) >> ((bits) - 1)) & 1; \
    (c)->af = ((_a ^ _b ^ _r) >> 4) & 1; SZP(c, _r, bits); } while (0)
#define FLAGS_ADC(c, a, b, r, cin, bits) do { u32 _m = (bits) == 32 ? 0xFFFFFFFFu : ((1u << (bits)) - 1); \
    u32 _a = (u32)(a) & _m, _b = (u32)(b) & _m, _r = (u32)(r) & _m; \
    (c)->cf = (cin) ? _r <= _a : _r < _a; (c)->of = ((~(_a ^ _b) & (_a ^ _r)) >> ((bits) - 1)) & 1; \
    (c)->af = ((_a ^ _b ^ _r) >> 4) & 1; SZP(c, _r, bits); } while (0)
#define FLAGS_SUB(c, a, b, r, bits) do { u32 _m = (bits) == 32 ? 0xFFFFFFFFu : ((1u << (bits)) - 1); \
    u32 _a = (u32)(a) & _m, _b = (u32)(b) & _m, _r = (u32)(r) & _m; \
    (c)->cf = _a < _b; (c)->of = (((_a ^ _b) & (_a ^ _r)) >> ((bits) - 1)) & 1; \
    (c)->af = ((_a ^ _b ^ _r) >> 4) & 1; SZP(c, _r, bits); } while (0)
#define FLAGS_SBB(c, a, b, r, cin, bits) do { u32 _m = (bits) == 32 ? 0xFFFFFFFFu : ((1u << (bits)) - 1); \
    u32 _a = (u32)(a) & _m, _b = (u32)(b) & _m, _r = (u32)(r) & _m; \
    (c)->cf = (cin) ? _a <= _b : _a < _b; (c)->of = (((_a ^ _b) & (_a ^ _r)) >> ((bits) - 1)) & 1; \
    (c)->af = ((_a ^ _b ^ _r) >> 4) & 1; SZP(c, _r, bits); } while (0)
#define FLAGS_LOGIC(c, r, bits) do { (c)->cf = 0; (c)->of = 0; (c)->af = 0; SZP(c, r, bits); } while (0)

// condition codes
#define CC_O(c) ((c)->of)
#define CC_NO(c) (!(c)->of)
#define CC_B(c) ((c)->cf)
#define CC_AE(c) (!(c)->cf)
#define CC_E(c) ((c)->zf)
#define CC_NE(c) (!(c)->zf)
#define CC_BE(c) ((c)->cf | (c)->zf)
#define CC_A(c) (!((c)->cf | (c)->zf))
#define CC_S(c) ((c)->sf)
#define CC_NS(c) (!(c)->sf)
#define CC_P(c) ((c)->pf)
#define CC_NP(c) (!(c)->pf)
#define CC_L(c) ((c)->sf != (c)->of)
#define CC_GE(c) ((c)->sf == (c)->of)
#define CC_LE(c) ((c)->zf | ((c)->sf != (c)->of))
#define CC_G(c) (!(c)->zf && (c)->sf == (c)->of)

// ---- x87 ----------------------------------------------------------------------------------
#define ST(c, i) ((c)->st[((c)->top + (i)) & 7])
static inline void fpush(Cpu* c, double v) { c->top = (c->top - 1) & 7; c->st[c->top] = v; c->fpu_tag[c->top] = 1; }
static inline double fpop(Cpu* c) { double v = c->st[c->top]; c->fpu_tag[c->top] = 0; c->top = (c->top + 1) & 7; return v; }
// compare st0 with v: C3 (ZF) C2 (PF) C0 (CF)
static inline void fcom_set(Cpu* c, double a, double b) {
    u16 sw = c->fpu_sw & ~0x4500;
    if (a != a || b != b) sw |= 0x4500;
    else if (a < b) sw |= 0x0100;
    else if (a == b) sw |= 0x4000;
    c->fpu_sw = sw;
}
static inline void fcomi_set(Cpu* c, double a, double b) {
    c->of = c->sf = c->af = 0;
    if (a != a || b != b) { c->zf = c->pf = c->cf = 1; }
    else { c->zf = a == b; c->pf = 0; c->cf = a < b; }
}
static inline u16 fstsw(Cpu* c) { return (u16)((c->fpu_sw & ~0x3800) | ((c->top & 7) << 11)); }
// float -> integer by the control word's rounding mode
static inline s64 fround(Cpu* c, double v) {
    switch ((c->fpu_cw >> 10) & 3) {
        case 0: return (s64)nearbyint(v);
        case 1: return (s64)floor(v);
        case 2: return (s64)ceil(v);
        default: return (s64)v;
    }
}

// ---- dispatch -----------------------------------------------------------------------------
typedef void (*GuestFn)(Cpu*);
GuestFn lookup_fn(u32 guest);                // NULL if no recompiled function / native thunk starts there
void call_indirect(Cpu* c, u32 target);      // target's return address is already pushed
void jump_indirect(Cpu* c, u32 target);      // tail jump (no push)
_Noreturn void unimpl(Cpu* c, u32 at, const char* what);  // aborts with a report
_Noreturn void guest_int(Cpu* c, u32 at, int n);  // int n / int3 / #DE: fatal report
void guest_cpuid(Cpu* c);
void guest_rdtsc(Cpu* c);

#ifdef __cplusplus
}
#endif
