/* Benchmark hooks of the renderer and the shims (no benchmark driver yet). */
#ifndef FF_BENCH_H
#define FF_BENCH_H

#include <stdint.h>

static inline int bench_enabled(void) { return 0; }
static inline int bench_shot_due(void) { return -1; }
static inline void bench_frame(uint64_t now, uint64_t frame_us, uint64_t draw_us, uint64_t swap_us, uint32_t draws,
                               uint32_t gpu_draws, uint32_t verts)
{
    (void)now, (void)frame_us, (void)draw_us, (void)swap_us, (void)draws, (void)gpu_draws, (void)verts;
}

/* Per-frame event counts (plain increments, nobody reads them yet). */
typedef struct BenchCounters {
    uint32_t file_opens, file_reads, read_bytes, heap_allocs;
    uint64_t read_us;
} BenchCounters;
extern BenchCounters g_bench_ctr;

#endif
