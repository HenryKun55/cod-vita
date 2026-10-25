#!/bin/sh
# Automated visual check of the Vita build in Vita3K: boots straight into a level
# (a test copy of the game directory whose Freedom.ini has DefaultScene=<scene>),
# plays a scripted input (ux0:data/ff/input.txt, the FF_INPUT syntax of
# shim/src/platform_common.c), captures the Vita3K window at every "shot=NAME" of
# the script into $OUT/NAME.png and stops at "quit" (or the timeout).
#
# It runs its own Vita3K instance on its own file system (build-vita3k-qa: the
# firmware copied once from the normal one, the app installed from the build every
# run), so it never touches the normal Vita3K installation and can run while
# someone plays there.
#
# usage: tools/qa_vita3k.sh "<script>" [out dir]
#   e.g. tools/qa_vita3k.sh "3000:mx+2000,4000:my+400,6000:shot=floor,7000:quit"
# env:   BUILD   Vita build directory (default build-vita)
#        SCENE   start scene (default SinglePlayer\C01A\FF-C01A_MAIN.gms)
#        MHZ     emulated CPU clock (default 444; the timing then resembles hardware's)
#        CORES   guest threads running at once (VITA3K_CORES; the Vita has 3)
#        CFG     contents of ux0:data/ff/ff.cfg for the run (default none)
#        INI     "Key value;..." settings for the test copy's Freedom.ini
#        VITA3K  Vita3K binary (default the local source build)
#        TIMEOUT seconds (default 300)
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SCRIPT=${1:?usage: tools/qa_vita3k.sh "<script>" [out dir]}
OUT=${2:-$ROOT/build-vita/qa}
BUILD=${BUILD:-$ROOT/build-vita}
SCENE=${SCENE:-'SinglePlayer\C01A\FF-C01A_MAIN.gms'}
VITA3K=${VITA3K:-$HOME/Documents/workspace/games/Vita3K/build/macos-ninja/bin/Release/Vita3K.app/Contents/MacOS/Vita3K}
TIMEOUT=${TIMEOUT:-300}
MAIN="$HOME/Library/Application Support/Vita3K/Vita3K"
HOME_QA="$ROOT/build-vita3k-qa"
FS="$HOME_QA/fs"
CONF="$HOME_QA/config.yml"
APP="$FS/ux0/app/FFRD00001"
DATA="$FS/ux0/data/ff"
QA="$ROOT/gamedata/qa"
LOG=${TMPDIR:-/tmp}/ff_qa_vita3k.log
MATCH="$CONF"   # in this instance's command line only

[ -f "$BUILD/ff.vpk" ] || { echo "no $BUILD/ff.vpk; build first" >&2; exit 1; }
BUILD=$(cd "$BUILD" && pwd)   # absolute: the install below runs in another directory
OUT=$(mkdir -p "$OUT" && cd "$OUT" && pwd)
[ -x "$VITA3K" ] || { echo "no Vita3K at $VITA3K" >&2; exit 1; }
if pgrep -f "$MATCH" > /dev/null; then
    echo "a QA Vita3K instance is already running" >&2
    exit 1
fi

# The QA file system: the firmware partitions from the normal one, once.
if [ ! -d "$FS/vs0" ]; then
    mkdir -p "$FS"
    for p in os0 vs0 vd0 ur0 sa0 pd0 tm0 ud0 gro0 grw0 imc0 xmc0 uma0 sd0 host0; do
        if [ -d "$MAIN/fs/$p" ]; then cp -R "$MAIN/fs/$p" "$FS/$p"; else mkdir -p "$FS/$p"; fi
    done
    mkdir -p "$FS/ux0/app" "$FS/ux0/data" "$FS/ux0/user"
    [ -d "$MAIN/fs/ux0/user" ] && cp -R "$MAIN/fs/ux0/user/." "$FS/ux0/user/"
fi
sed "s|^pref-path:.*|pref-path: $FS|" "$MAIN/config.yml" > "$CONF"
# VITA3K_PREF_PATH (the local Vita3K build) sets this run's file system whatever the
# default config.yml says, and leaves that file alone (other ports' runners share it).
# Kept as a safety net: the user's copy is put back when this run ends.
MAIN_CONF_SAVE="$HOME_QA/main-config.yml.save"
cp "$MAIN/config.yml" "$MAIN_CONF_SAVE"

# The app from this build, and the test copy of the game directory.
rm -rf "$APP"
mkdir -p "$APP" "$DATA"
(cd "$APP" && unzip -q -o "$BUILD/ff.vpk") || exit 1
rm -rf "$QA"
mkdir -p "$QA"
for f in "$ROOT"/gamedata/pc/*; do
    n=$(basename "$f")
    [ "$n" = "Freedom.ini" ] || ln -s "../pc/$n" "$QA/$n"
done
# (backslashes doubled: sed would read \C, \F... as escapes)
SCENE_SED=$(printf '%s' "$SCENE" | sed 's/\\/\\\\/g')
sed "s|^DefaultScene=.*|DefaultScene=$SCENE_SED|" "$ROOT/gamedata/pc/Freedom.ini" > "$QA/Freedom.ini"
# INI="Key value;Key value": settings of the test copy's Freedom.ini (e.g. "DrawDistance 0.6").
if [ -n "${INI:-}" ]; then
    printf '%s' "$INI" | tr ';' '\n' | while read -r k v; do
        [ -n "$k" ] && sed -i '' "s|^$k .*|$k $v|" "$QA/Freedom.ini"
    done
fi
ln -sfn "$QA" "$DATA/pc"
printf '%s' "$SCRIPT" > "$DATA/input.txt"
rm -f "$DATA/shot_now.txt" "$DATA/ff.log" "$DATA/profile.txt" "$DATA/ff.cfg"
if [ -n "${CFG:-}" ]; then printf '%s\n' "$CFG" > "$DATA/ff.cfg"; fi
mkdir -p "$OUT"
touch "$OUT"

stop() {
    pkill -9 -f "$MATCH" 2>/dev/null
    sleep 1
    cp "$MAIN_CONF_SAVE" "$MAIN/config.yml"
    rm -f "$DATA/input.txt" "$DATA/shot_now.txt"
}
trap 'stop; exit 1' INT TERM

WINID_BIN=${TMPDIR:-/tmp}/ff_window_id
[ "$WINID_BIN" -nt "$ROOT/tools/window_id.swift" ] || swiftc -O "$ROOT/tools/window_id.swift" -o "$WINID_BIN" || exit 1

(VITA3K_PREF_PATH="$FS/" VITA3K_CPU_MHZ=${MHZ:-444} VITA3K_CORES=${CORES:-} "$VITA3K" -c "$CONF" -w -r FFRD00001 > "$LOG" 2>&1 &)
started=0
while [ $started -lt 30 ] && ! pgrep -f "$MATCH" > /dev/null; do
    sleep 1
    started=$((started + 1))
done
shots=0
waited=0
while [ $waited -lt $((TIMEOUT * 2)) ] && pgrep -f "$MATCH" > /dev/null; do
    if [ -s "$DATA/shot_now.txt" ]; then
        name=$(cat "$DATA/shot_now.txt")
        rm -f "$DATA/shot_now.txt"
        pid=$(pgrep -f "$MATCH" | head -n 1)
        win=$("$WINID_BIN" "$pid" "(FFRD00001)" | head -n 1)
        if [ -n "$win" ] && screencapture -x -o -l "$win" "$OUT/$name.png"; then
            shots=$((shots + 1))
            echo "shot $name"
        else
            echo "shot $name: no Vita3K window" >&2
        fi
    fi
    # The game exits at "quit" but Vita3K stays open: its log says so.
    if grep -aq "\[input\] quit" "$DATA/ff.log" 2>/dev/null; then
        break
    fi
    sleep 0.5
    waited=$((waited + 1))
done
stop
if [ -s "$DATA/profile.txt" ]; then   # (removed before the run: this run's)
    cp "$DATA/profile.txt" "$OUT/profile.txt"
    echo "profile: $OUT/profile.txt"
fi
cp "$DATA/ff.log" "$OUT/ff.log" 2>/dev/null
echo "$shots screenshot(s) in $OUT; log: $OUT/ff.log"
