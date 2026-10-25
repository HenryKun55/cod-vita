# ff-recomp

A static recompilation of **Freedom Fighters** (IO Interactive, 2003) from its
x86 Windows executable to portable C, aimed at the **PlayStation Vita** and
running natively on **macOS** while it is being brought up.

No game data is included. You need your own copy of the game: the build
recompiles *your* `Freedom.Exe`, and the game reads its data from your
installation.

## Status

- macOS: the game boots, plays the in-engine intro cutscene and reaches the
  title screen.
- Not yet: render to texture, the AVI movies (DirectShow), the Vita build.

## How it works

1. `tools/recomp` disassembles `Freedom.Exe` (Capstone), finds its functions and
   translates every one of them to C. The generated code runs against a flat
   32-bit guest memory image, with an exact x87 model where the game needs it.
   MSVC C++ exceptions are supported (`runtime/src/eh.c`).
2. `runtime/` loads the executable image and runs the generated code.
3. `shim/` reimplements the parts of Windows the game uses: kernel32 (with real
   threads), user32, msvcrt, Direct3D 8 (on OpenGL, vitaGL on the Vita),
   DirectSound, DirectInput 8 (keyboard and mouse on the desktop, controller on
   the Vita) and files.

The recompiler, runtime and renderer come from the author's Project IGI port.

## Building

Requirements: CMake 3.20+, Ninja, Python 3 with `capstone`, and your game
installation, whose directory (the one with `Freedom.Exe`) is expected at
`gamedata/pc` (or pass `-DFF_EXE=/path/to/Freedom.Exe`).

### macOS

```sh
brew install cmake ninja sdl2
pip3 install capstone
cmake -S . -B build -G Ninja
cmake --build build
cd gamedata/pc && ../../build/ff .
```

The window can be closed at any time. Click in it to capture the mouse
(unlimited turning); Cmd releases it. `Freedom.ini` sets the resolution
(`Resolution 960x544` is the Vita's).

Diagnostics: `FF_LOG_CALLS=<seconds>` logs every host call for 100 ms from that
time; `FF_SHOT=N` saves every Nth frame as a BMP; `FF_TRACE_EVERY=N` logs every
draw of every Nth frame.

### PlayStation Vita

Needs [VitaSDK](https://vitasdk.org):

```sh
tools/build_vitagl_vita.sh
cmake -S . -B build-vita -G Ninja -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
cmake --build build-vita
```

Install `build-vita/ff.vpk` and copy the game directory to `ux0:data/ff/pc`.
Optional settings go in `ux0:data/ff/ff.cfg` (one `key=value` per line).

## Controls (Vita)

| Control | Action |
| --- | --- |
| Left stick | move |
| Right stick | aim / look |
| R / L | fire / precision aim |
| Cross | jump (Enter in the menus) |
| Circle / Square / Triangle | crouch / use / close combat |
| D-pad left, up, right | squad: form up, attack, defend |
| D-pad down | inventory wheel |
| Rear touch upper / lower half | previous / next weapon |
| Select / Start | map / menu |

In the menus: D-pad or left stick moves through the options, Cross selects,
Circle (or Start) goes back; the right stick and R still move and click the cursor.
