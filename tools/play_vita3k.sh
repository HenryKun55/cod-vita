#!/bin/sh
# Plays the Vita build in Vita3K with the emulated CPU at the console's clock:
# installs build-vita/ff.vpk (tools/install_vita3k.sh) and starts it.
#
# usage: tools/play_vita3k.sh [MHz]       (default 444; 333 is the console's default clock)
# env:   BUILD    Vita build directory (default build-vita; build-vita-prof writes
#                 ux0:data/ff/profile.txt)
#        VITA3K   Vita3K binary (default the local source build)
#        NOINSTALL=1  start what is installed
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
MHZ=${1:-444}
BUILD=${BUILD:-$ROOT/build-vita}
VITA3K=${VITA3K:-$HOME/Documents/workspace/games/Vita3K/build/macos-ninja/bin/Release/Vita3K.app/Contents/MacOS/Vita3K}
[ -x "$VITA3K" ] || { echo "no Vita3K at $VITA3K" >&2; exit 1; }
if [ "${NOINSTALL:-0}" != 1 ]; then
    [ -f "$BUILD/ff.vpk" ] || { echo "no $BUILD/ff.vpk; build first" >&2; exit 1; }
    FS="$HOME/Library/Application Support/Vita3K/Vita3K/fs/ux0"
    # tools/install_vita3k.sh installs build-vita; another BUILD gets its eboot on top.
    "$ROOT/tools/install_vita3k.sh" > /dev/null
    if [ "$BUILD" != "$ROOT/build-vita" ]; then
        unzip -p "$BUILD/ff.vpk" eboot.bin > "$FS/app/FFRD00001/eboot.bin"
    fi
    echo "installed $BUILD/ff.vpk"
fi
echo "Vita3K, CPU at $MHZ MHz (log: ux0:data/ff/ff.log)"
VITA3K_CPU_MHZ=$MHZ exec "$VITA3K" -r FFRD00001
