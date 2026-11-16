// Tables emitted by tools/recomp.py.
#pragma once
#include "cpu.h"
typedef struct { u32 addr; GuestFn fn; } GuestFnEntry;
typedef struct { u32 slot; const char* dll; const char* name; unsigned ordinal; } GuestImport;
typedef struct { const char* file; u32 base, pref, size, entry; } GuestImage;
extern const unsigned guest_fn_count;
extern const GuestFnEntry guest_fns[];
extern const unsigned guest_import_count;
extern const GuestImport guest_imports[];
extern const unsigned guest_image_count;
extern const GuestImage guest_images[];
