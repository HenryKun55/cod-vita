/* Guest module loader: maps CoDSP.exe and the game DLLs into guest memory (see module.h). */
#include "module.h"

#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

uint32_t shim_import_data(const char *dll, const char *name);

static int refs[16];

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

int module_find(const char *path)
{
    const char *name = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':')
            name = p + 1;
    for (uint32_t i = 0; i < g_guest_module_count; i++)
        if (strcasecmp(name, g_guest_modules[i].name) == 0)
            return (int)i;
    return -1;
}

int module_by_base(uint32_t base)
{
    for (uint32_t i = 0; i < g_guest_module_count; i++)
        if (g_guest_modules[i].base == base)
            return (int)i;
    return -1;
}

uint32_t module_export(uint32_t base, const char *name)
{
    int i = module_by_base(base);
    if (i < 0)
        return 0;
    for (const GuestExport *e = g_guest_modules[i].exports; e->name; e++)
        if (strcmp(e->name, name) == 0)
            return e->va;
    return 0;
}

/* Copy the file's headers and sections to the module's base, zero its bss, apply the
 * base relocations for the move from the preferred base and wire its imports. */
static void map_module(const GuestModule *m)
{
    char path[1200];
    snprintf(path, sizeof(path), "%s/%s", g_game_dir, m->name);
    size_t size;
    uint8_t *d = read_file(path, &size);
    if (!d)
        rt_fatal(NULL, "cannot read %s", path);
    uint32_t pe = rd32(d + 0x3C);
    if (pe + 0x100u > size || memcmp(d + pe, "PE\0\0", 4) != 0)
        rt_fatal(NULL, "%s is not a PE file", path);
    uint32_t coff = pe + 4u;
    uint16_t nsec = rd16(d + coff + 2u);
    uint16_t opt_size = rd16(d + coff + 16u);
    uint32_t opt = coff + 20u;
    uint32_t pref = rd32(d + opt + 28u);
    uint32_t image_size = rd32(d + opt + 56u);
    uint32_t hdr_size = rd32(d + opt + 60u);
    if (pref != m->preferred_base || image_size != m->size)
        rt_fatal(NULL, "%s is not the file the port was built from (base %08X size %08X)", path, pref, image_size);
    uint32_t base = m->base;
    memset(g_mem + base, 0, image_size);
    memcpy(g_mem + base, d, hdr_size);
    uint32_t sh = opt + opt_size;
    for (uint16_t i = 0; i < nsec; i++, sh += 40u) {
        uint32_t vsize = rd32(d + sh + 8u);
        uint32_t va = rd32(d + sh + 12u);
        uint32_t raw_size = rd32(d + sh + 16u);
        uint32_t raw_off = rd32(d + sh + 20u);
        uint32_t n = raw_size < vsize || !vsize ? raw_size : vsize;
        memcpy(g_mem + base + va, d + raw_off, n);
    }
    free(d);

    /* Relocations, read from the mapped copy (the .reloc section is in the image). */
    uint32_t delta = base - pref;
    uint32_t rel_rva = RD32(base + opt + 96u + 5u * 8u), rel_size = RD32(base + opt + 96u + 5u * 8u + 4u);
    if (delta && !rel_rva)
        rt_fatal(NULL, "%s has no relocations", m->name);
    if (delta) {
        uint32_t p = base + rel_rva, end = p + rel_size;
        while (p + 8u <= end) {
            uint32_t page = RD32(p), block = RD32(p + 4u);
            if (block < 8u)
                break;
            for (uint32_t k = 0; k < (block - 8u) / 2u; k++) {
                uint32_t e = RD16(p + 8u + 2u * k);
                if ((e >> 12) == 3u) {
                    uint32_t a = base + page + (e & 0xFFFu);
                    WR32(a, RD32(a) + delta);
                } else if ((e >> 12) != 0u) {
                    rt_fatal(NULL, "%s: relocation type %u", m->name, e >> 12);
                }
            }
            p += block;
        }
    }

    /* Point every IAT slot of this module at the host implementation's thunk, or at
     * the variable for the imports that are data. */
    for (uint32_t i = 0; i < g_guest_import_count; i++) {
        const GuestImport *imp = &g_guest_imports[i];
        if (imp->slot < base || imp->slot >= base + image_size)
            continue;
        uint32_t data = shim_import_data(imp->dll, imp->name);
        WR32(imp->slot, data ? data : thunk_register(imp->fn, imp->name));
    }
    rt_log("mapped %s at %08X (built for %08X, file base %08X)", m->name, base, m->base, pref);
}

uint32_t module_load_exe(void)
{
    map_module(&g_guest_modules[0]);
    refs[0] = 1;
    return g_guest_modules[0].entry;
}

uint32_t module_load(Ctx *c, int i)
{
    const GuestModule *m = &g_guest_modules[i];
    if (refs[i]++ > 0)
        return m->base;
    map_module(m);
    if (i > 0) {
        uint32_t args[3] = { m->base, 1u /* DLL_PROCESS_ATTACH */, 0u };
        if (!guest_call(c, m->entry, 3, args)) {
            rt_log("%s: DllMain refused to load", m->name);
            refs[i] = 0;
            return 0;
        }
    }
    return m->base;
}

int module_free(Ctx *c, uint32_t base)
{
    int i = module_by_base(base);
    if (i <= 0 || refs[i] <= 0)
        return 0;
    if (--refs[i] == 0) {
        uint32_t args[3] = { base, 0u /* DLL_PROCESS_DETACH */, 0u };
        guest_call(c, g_guest_modules[i].entry, 3, args);
        rt_log("unloaded %s", g_guest_modules[i].name);
    }
    return 1;
}
