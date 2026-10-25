/* Fixes of game behaviour that only shows on a fast host (entry hooks: PATCHES in
 * tools/recomp/gen.py). Each returns 0 to run the guest code after it. */
#include "runtime.h"

/* A RoQ cinematic catches up to its target frame (cinematic struct +0x20,
 * only ever raised) by reading frames in a loop. The target is recomputed only
 * when the clip's info chunk moves the start time, which happens on a 2003 PC
 * because a millisecond passes between the reset and the first read; here both
 * fall in the same millisecond, so the next clip keeps the previous clip's last
 * frame as its target and plays to its end at once (the Infinity Ward logo and
 * the intro after the Activision one were skipped). The reset clears it. */
int cod_fix_cin_reset(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    (void)c, (void)esp, (void)ecx, (void)eax, (void)ret;
    uint32_t handle = RD32(0x0056EE44u);
    if (handle < 16u)
        WR32(0x0086A5F4u + handle * 0x1D0u, 0);
    return 0;
}
