# Native plan: every emulated cost out of the game's code

Freedom.Exe has 20,947 functions. All of them already run as ARM code compiled
from C (static recompilation), but that C still carries the x86 machine:
flags computed for every operation, the x87 stack kept in double precision,
locals and arguments in guest memory, calls through the guest stack, indirect
calls through a lookup table. "Native" here means removing that machine where
it costs time, in two ways:

1. **Generator level (all functions at once).** Changes in `tools/recomp/gen.py`
   and `runtime/include/recomp.h` that make every recompiled function cheaper.
2. **Hand native (the hot functions).** Decompiled and rewritten in C
   (`runtime/src/native_*.c`), checked against the recompiled code.

Rewriting 20,947 functions by hand is neither possible nor useful: almost all
of them run rarely. What matters is the time. The method decides the order:
measure, rewrite what costs, verify, measure again.

## Method (for every function)

1. **Measure** on Vita3K at 333 MHz (`tools/qa_vita3k.sh`, isolated instance),
   in the scenes of the scene set below: profile build (`build-vita-prof`,
   FF_PROFILE), per-frame counters (`ff.cfg stats=1`).
2. **Decompile** from the disassembly; write it in C with the guest's operation
   order (single precision: the game runs the x87 at 24 bits).
   Decompiler: Ghidra 12.1.3 through the ReVa / REA MCP servers (program
   `/Freedom.Exe.0`) for what a function does, callers and structures; the x87
   disassembly for the exact operation order (the decompiler reorders sums).
3. **Verify**:
   - `FF_NATIVE_CHECK=1` (native and recompiled run on the same state, results
     compared; desktop build);
   - for anything that writes vertex buffers, `stats=2` (vertex buffer verifier)
     and the store watch (`-DFF_WATCH=ON` builds);
   - screenshots of the scene set.
4. **Measure again**; commit with the numbers.

## Scene set

The profile of one view is not the game. Each scene gets a script for
`tools/qa_vita3k.sh` (walk timings change as the game gets faster, so the
scripts are anchored on a later key press and checked by screenshot):

| Scene | What it stresses |
|---|---|
| C01A start bedroom, at the window | indoor lighting, static meshes |
| C01A bedroom window looking at the city | far geometry, culling, ~1000 draws |
| C01A corridor with the soldier and the rescued man | characters, skinning, AI |
| C01A street (first combat) | AI, collision, particles, many characters |
| A cutscene | animation, video, 2D |
| Main menu / loading | menu code, file loading |
| A later outdoor level (C02/C03) | large draw distance, squads |

## Wave 1: hand native, hot math (status)

| Function | What | Share before | Status |
|---|---|---|---|
| 0x006A5600 | per-vertex lighting | 10-14% | native; constants in registers |
| 0x00694BF0 (loop) | skinning | 7-10% | native (scalar; NEON is slower on Vita3K) |
| 0x006167C0 | collision octree segment query | 8-25% | native, reentrant |
| 0x005FF460 | segment / box test | 5% | native |
| 0x006915B0, 0x00691410 | vertex colours in/out of the lighting | 4-5% | native |
| 0x00690D20 | unlit vertices into the lit buffer | (writer) | native |
| 0x006959D0 (loop) | particle quads | 3% | native |
| 0x0058DE80 | visibility of objects (view planes, portals) | 4.5% | native; 24k calls checked, identical |
| 0x006071C0, 0x00607120, 0x00606F80, 0x00606ED0, 0x006067D0, 0x006065D0, 0x006069E0 | scene graph transforms up/down the parent chain | ~4% | native; 1.86M calls checked (float cancellation only) |
| 0x005D5D30 | rdtsc timing wrapper (its cost is the clock read) | 1.7% | left alone: timing is not touched |
| 0x005FDEA0 | oriented box overlap (15 separating axes) | 1.7% | native; 420k calls checked, identical |
| 0x00691CF0 ("UpdateStrip") | keyframe vertex tween before the lighting | 1.7% | tween loop native; 16k calls checked: colours exact, floats within a few ulps (double vs float) |
| 0x006924C0, 0x00692860, 0x00692470 | mesh submit around the lighting | ~2.3% | next |
| 0x00407110, 0x00410CC0 | (to identify) | ~2.8% | next |
| 0x00698E80, 0x006164F0, 0x00616B70, 0x00614AC0 | octree / scene graph | ~2.7% | next |
| 0x005F8A90, 0x005F87F0 | vector / matrix helpers | ~1.3% | next |
| 0x00574850, 0x00573F70, 0x00574140, 0x005797C0, 0x005809B0 | (to identify: animation?) | ~2.3% | next |

The list is refreshed from each scene's profile. A function stays native only
if it checks exactly (or within documented rounding) and the scene gets faster.

## Wave 2: generator level (every function)

Each one is a change to all the recompiled code, so each one is measured on the
whole scene set and verified with `tools/recomp/difftest.py`:

- **x87 in single precision** (`FF_X87_FLOAT`, exists, off). The game runs the
  x87 at 24-bit precision after CreateDevice, so `float` registers are closer to
  the real game than `double` there; every float load, store and operation gets
  cheaper on the Cortex-A9. Risk: code running before CreateDevice (start-up,
  the rdtsc calibration) runs at 53 bits. Gate: the calibration and all timing
  values in the log must stay bit-identical (the clock is not to change), and
  difftest's failures must be only the precision ones.
- **Dead flags**: flags computed only where a later instruction reads them
  (already partly done; measure how much flag code remains in the hot code).
- **Locals in registers**: stack slots of a function that never escape (no
  address taken) kept in C locals instead of guest memory.
- **Direct calls for known virtual targets**: indirect calls whose target is
  always the same in a scene go straight to the function.

## Wave 3: the draw path and the render thread

- Game thread: the draw recording (`pk_record_buffers`), state setting.
- Render thread: vitaGL uniforms, pack, near clip, GL state (it becomes the
  limit once the game thread is under it: 24 ms in the bedroom view).

## Rules

- The clock and the timing are not touched.
- Verification is on Vita3K at 333 MHz (isolated QA instance) and the console,
  not the Mac.
- A cache or shortcut is accepted only after a verifier proved it exact on the
  scene set (the lighting cache was dropped: the lights change every call).
