// Guest memory allocator: HeapAlloc/GlobalAlloc/malloc-style blocks and VirtualAlloc regions, all
// inside the 32-bit guest address space.
//   0x01000000..0x20000000  heap: small blocks by size class (16..2048), larger ones first-fit
//   0x20000000..0x5F000000  VirtualAlloc: page-granular, exact-size reuse
#include "native.h"
#include <pthread.h>

#define HEAP_LO 0x01000000u
#define HEAP_HI 0x20000000u
#define VA_LO 0x20000000u
#define VA_HI 0x5F000000u
#define HDR 16u               // [size][class][magic][pad] before every block
#define MAGIC 0xD2A110C5u
#define NCLASS 128            // 16-byte classes up to 2048
#define SMALL_MAX (NCLASS * 16)

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static u32 heap_top = HEAP_LO;
static u32 free_small[NCLASS + 1];  // singly linked through the first word of the payload
typedef struct Big { u32 a, size; struct Big* next; } Big;  // free large blocks, address ordered
static Big* free_big;

static u32 bump(u32 n) {
    n = (n + 4095) & ~4095u;
    if (heap_top + n > HEAP_HI) { LOG("FATAL: guest heap exhausted"); return 0; }
    u32 a = heap_top;
    heap_top += n;
    return a;
}

static void big_free(u32 a, u32 total);
static u32 big_alloc(u32 total) {
    Big **pp = &free_big, *b;
    for (; (b = *pp); pp = &b->next) {
        if (b->size < total) continue;
        u32 a = b->a;
        if (b->size - total >= 64) { b->a += total; b->size -= total; }
        else { total = b->size; *pp = b->next; free(b); }
        wr32(a, total);
        return a;
    }
    u32 a = bump(total);
    if (!a) return 0;
    u32 got = (total + 4095) & ~4095u;
    if (got - total >= 64) big_free(a + total, got - total);  // the page tail stays free
    else total = got;
    wr32(a, total);
    return a;
}

static void big_free(u32 a, u32 total) {
    Big **q = &free_big, *prev = NULL;
    while (*q && (*q)->a < a) { prev = *q; q = &(*q)->next; }
    Big* nb = malloc(sizeof(Big));
    nb->a = a; nb->size = total; nb->next = *q; *q = nb;
    if (nb->next && nb->a + nb->size == nb->next->a) {  // merge with the following
        Big* n = nb->next; nb->size += n->size; nb->next = n->next; free(n);
    }
    if (prev && prev->a + prev->size == nb->a) {  // merge with the preceding
        prev->size += nb->size; prev->next = nb->next; free(nb);
    }
}

u32 galloc(u32 size) {
    if (size == 0) size = 1;
    pthread_mutex_lock(&mu);
    u32 a, cls = 0, total;
    if (size <= SMALL_MAX) {
        cls = (size + 15) / 16;
        total = cls * 16 + HDR;
        a = free_small[cls];
        if (a) free_small[cls] = rd32(a + HDR);
        else {
            // carve a run of blocks of this class from a 64 KB slab
            u32 n = 65536 / total; if (n < 1) n = 1;
            u32 slab = big_alloc(n * total);
            if (!slab) { pthread_mutex_unlock(&mu); return 0; }
            for (u32 i = 1; i < n; ++i) {
                u32 b = slab + i * total;
                wr32(b + HDR, free_small[cls]);
                free_small[cls] = b;
            }
            a = slab;
        }
    } else {
        total = ((size + 15) & ~15u) + HDR;
        a = big_alloc(total);
        if (!a) { pthread_mutex_unlock(&mu); return 0; }
        total = rd32(a);
    }
    wr32(a, size);
    wr32(a + 4, cls);
    wr32(a + 8, MAGIC);
    wr32(a + 12, total);
    pthread_mutex_unlock(&mu);
    return a + HDR;
}

u32 gcalloc(u32 size) {
    u32 a = galloc(size);
    if (a) memset(H(a), 0, size);
    return a;
}

void gfree(u32 p) {
    if (!p) return;
    u32 a = p - HDR;
    if (rd32(a + 8) != MAGIC) { LOG("warning: free of a non-heap pointer %08X", p); return; }
    pthread_mutex_lock(&mu);
    u32 cls = rd32(a + 4), total = rd32(a + 12);
    wr32(a + 8, 0);
    if (cls) {
        wr32(a + HDR, free_small[cls]);
        free_small[cls] = a;
    } else big_free(a, total);
    pthread_mutex_unlock(&mu);
}

u32 gsize(u32 p) {
    if (!p || rd32(p - HDR + 8) != MAGIC) return 0xFFFFFFFFu;
    return rd32(p - HDR);
}

u32 grealloc(u32 p, u32 size) {
    if (!p) return galloc(size);
    u32 old = gsize(p);
    if (old == 0xFFFFFFFFu) return 0;
    u32 cap = rd32(p - HDR + 12) - HDR;
    if (size <= cap && size > cap / 2) { wr32(p - HDR, size); return p; }
    u32 n = galloc(size);
    if (!n) return 0;
    memcpy(H(n), H(p), old < size ? old : size);
    gfree(p);
    return n;
}

u32 gstrdup(const char* s) {
    u32 n = (u32)strlen(s) + 1;
    u32 a = galloc(n);
    memcpy(H(a), s, n);
    return a;
}

// ---- VirtualAlloc regions
typedef struct VR { u32 a, size; int used; struct VR* next; } VR;
static VR* vrs;
static u32 va_top = VA_LO;

u32 gvalloc(u32 size) {
    size = (size + 0xFFFF) & ~0xFFFFu;  // Windows allocation granularity
    pthread_mutex_lock(&mu);
    for (VR* r = vrs; r; r = r->next)
        if (!r->used && r->size >= size) {
            if (r->size > size) {  // split
                VR* n = malloc(sizeof(VR));
                n->a = r->a + size; n->size = r->size - size; n->used = 0; n->next = r->next;
                r->next = n; r->size = size;
            }
            r->used = 1;
            memset(H(r->a), 0, size);
            pthread_mutex_unlock(&mu);
            return r->a;
        }
    if (va_top + size > VA_HI) { pthread_mutex_unlock(&mu); LOG("FATAL: VirtualAlloc space exhausted"); return 0; }
    VR* r = malloc(sizeof(VR));
    r->a = va_top; r->size = size; r->used = 1; r->next = vrs; vrs = r;
    va_top += size;
    pthread_mutex_unlock(&mu);
    return r->a;
}

void gvfree(u32 a) {
    pthread_mutex_lock(&mu);
    for (VR* r = vrs; r; r = r->next)
        if (r->a == a) { r->used = 0; break; }
    pthread_mutex_unlock(&mu);
}

int gv_owned(u32 a, u32* base, u32* size) {
    int ok = 0;
    pthread_mutex_lock(&mu);
    for (VR* r = vrs; r; r = r->next)
        if (r->used && a >= r->a && a < r->a + r->size) { *base = r->a; *size = r->size; ok = 1; break; }
    pthread_mutex_unlock(&mu);
    return ok;
}

u32 heap_used_kb(void) { return (heap_top - HEAP_LO) >> 10; }
u32 va_used_kb(void) {
    u32 kb = 0;
    pthread_mutex_lock(&mu);
    for (VR* r = vrs; r; r = r->next) if (r->used) kb += r->size >> 10;
    pthread_mutex_unlock(&mu);
    return kb;
}
