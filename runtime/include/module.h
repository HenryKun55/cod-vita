/* The guest modules: CoDSP.exe and the game DLLs it loads with LoadLibrary.
 *
 * The recompiler generated each DLL's code for a fixed base inside guest memory
 * (g_guest_modules, gen.py write_modules), so loading one maps its file there,
 * applies its relocations for that base, points its IAT at the host imports and
 * runs DllMain. */
#ifndef COD_MODULE_H
#define COD_MODULE_H

#include <stdint.h>

typedef struct GuestExport {
    const char *name;
    uint32_t va;
} GuestExport;

typedef struct GuestModule {
    const char *name;          /* file name, lower case */
    uint32_t base;             /* where the code was generated for */
    uint32_t preferred_base;   /* the file's own image base */
    uint32_t size;             /* SizeOfImage */
    uint32_t entry;            /* entry point (DllMain for a DLL), at `base` */
    const GuestExport *exports;
} GuestModule;

extern const GuestModule g_guest_modules[];
extern const uint32_t g_guest_module_count;

struct Ctx;

/* Index of the module whose file name (any directory, any case) is `path`, or -1. */
int module_find(const char *path);
/* Index of the module loaded at `base`, or -1. */
int module_by_base(uint32_t base);
/* Maps module i (once; later calls count references) and returns its base. A DLL's
 * DllMain runs with DLL_PROCESS_ATTACH on first load; 0 when it refuses. */
uint32_t module_load(struct Ctx *c, int i);
/* Drops a reference; the last one runs DllMain(DLL_PROCESS_DETACH) and the next
 * load maps the file again (fresh data, as Windows does). */
int module_free(struct Ctx *c, uint32_t base);
/* Address of a named export of the module at base, 0 if none. */
uint32_t module_export(uint32_t base, const char *name);
/* The exe's entry point after mapping it (module 0). */
uint32_t module_load_exe(void);

#endif
