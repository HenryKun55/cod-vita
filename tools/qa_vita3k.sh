#!/bin/sh
# Runs the Vita build in Vita3K with a scripted input (ux0:data/cod1/input.txt, the
# COD_INPUT syntax of shim/src/platform_common.c), captures the Vita3K window at
# every "shot=NAME" into $OUT/NAME.png and stops at "quit" (or the timeout).
#
# It runs its own Vita3K instance on its own file system (build-vita3k-qa: the
# firmware copied once from the normal one, the app installed from the build every
# run), with ux0:data/cod1/pc linked to gamedata/pc.
#
# usage: tools/qa_vita3k.sh "<script>" [out dir]
#   e.g. tools/qa_vita3k.sh "20000:shot=menu,21000:quit"
# env:   BUILD   Vita build directory (default build-vita)
#        MHZ     emulated CPU clock (default 333, the console's)
#        ARGS    the game's command line (ux0:data/cod1/args.txt), e.g. "+map training"
#        ENV     "NAME=value;..." environment of the run (ux0:data/cod1/env.txt)
#        TEXCACHE=0  Vita3K's texture cache off
#        MEMMAP  Vita3K's memory-mapping setting for the run
#        VITA3K  Vita3K binary (default the local source build)
#        TIMEOUT seconds (default 300)
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SCRIPT=${1:?usage: tools/qa_vita3k.sh "<script>" [out dir]}
OUT=${2:-$ROOT/build-vita/qa}
BUILD=${BUILD:-$ROOT/build-vita}
VITA3K=${VITA3K:-$HOME/Documents/workspace/games/Vita3K/build/macos-ninja/bin/Release/Vita3K.app/Contents/MacOS/Vita3K}
TIMEOUT=${TIMEOUT:-300}
TITLE=CODS00001
MAIN="$HOME/Library/Application Support/Vita3K/Vita3K"
HOME_QA="$ROOT/build-vita3k-qa"
FS="$HOME_QA/fs"
CONF="$HOME_QA/config.yml"
APP="$FS/ux0/app/$TITLE"
DATA="$FS/ux0/data/cod1"
LOG=${TMPDIR:-/tmp}/cod1_qa_vita3k.log
MATCH="$CONF"

[ -f "$BUILD/cod1.vpk" ] || { echo "no $BUILD/cod1.vpk; build first" >&2; exit 1; }
BUILD=$(cd "$BUILD" && pwd)
OUT=$(mkdir -p "$OUT" && cd "$OUT" && pwd)
[ -x "$VITA3K" ] || { echo "no Vita3K at $VITA3K" >&2; exit 1; }
if pgrep -f "$MATCH" > /dev/null; then
    echo "a QA Vita3K instance is already running" >&2
    exit 1
fi

if [ ! -d "$FS/vs0" ]; then
    mkdir -p "$FS"
    for p in os0 vs0 vd0 ur0 sa0 pd0 tm0 ud0 gro0 grw0 imc0 xmc0 uma0 sd0 host0; do
        if [ -d "$MAIN/fs/$p" ]; then cp -R "$MAIN/fs/$p" "$FS/$p"; else mkdir -p "$FS/$p"; fi
    done
    mkdir -p "$FS/ux0/app" "$FS/ux0/data" "$FS/ux0/user"
    [ -d "$MAIN/fs/ux0/user" ] && cp -R "$MAIN/fs/ux0/user/." "$FS/ux0/user/"
fi
sed "s|^pref-path:.*|pref-path: $FS|" "$MAIN/config.yml" > "$CONF"
# TEXCACHE=0: Vita3K's texture cache off (it write-protects texture memory).
if [ "${TEXCACHE:-1}" = 0 ]; then sed -i '' "s|^texture-cache:.*|texture-cache: false|" "$CONF"; fi
# MEMMAP=double-buffer|page-table|native-buffer|disabled: Vita3K's GPU memory mapping.
if [ -n "${MEMMAP:-}" ]; then sed -i '' "s|^memory-mapping:.*|memory-mapping: $MEMMAP|" "$CONF"; fi
MAIN_CONF_SAVE="$HOME_QA/main-config.yml.save"
cp "$MAIN/config.yml" "$MAIN_CONF_SAVE"

rm -rf "$APP"
mkdir -p "$APP" "$DATA"
(cd "$APP" && unzip -q -o "$BUILD/cod1.vpk") || exit 1
ln -sfn "$ROOT/gamedata/pc" "$DATA/pc"
printf '%s' "$SCRIPT" > "$DATA/input.txt"
rm -f "$DATA/shot_now.txt" "$DATA/cod1.log" "$DATA/args.txt" "$DATA/profile.txt"
if [ -n "${ARGS:-}" ]; then printf '%s\n' "$ARGS" > "$DATA/args.txt"; fi
rm -f "$DATA/env.txt"
if [ -n "${ENV:-}" ]; then printf '%s\n' "$ENV" | tr ';' '\n' > "$DATA/env.txt"; fi

stop() {
    pkill -9 -f "$MATCH" 2>/dev/null
    sleep 1
    cp "$MAIN_CONF_SAVE" "$MAIN/config.yml"
    rm -f "$DATA/input.txt" "$DATA/shot_now.txt"
}
trap 'stop; exit 1' INT TERM

WINID_BIN=${TMPDIR:-/tmp}/cod1_window_id
[ "$WINID_BIN" -nt "$ROOT/tools/window_id.swift" ] || swiftc -O "$ROOT/tools/window_id.swift" -o "$WINID_BIN" || exit 1

# The log is capped: a guest thread stuck on a bad read logs the fault forever
# (22 GB in one run).
(VITA3K_PREF_PATH="$FS/" VITA3K_CPU_MHZ=${MHZ:-333} "$VITA3K" -c "$CONF" -w -r $TITLE 2>&1 | head -c 200000000 > "$LOG" &)
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
        win=$("$WINID_BIN" "$pid" "($TITLE)" | head -n 1)
        if [ -n "$win" ] && screencapture -x -o -l "$win" "$OUT/$name.png"; then
            shots=$((shots + 1))
            echo "shot $name"
        else
            echo "shot $name: no Vita3K window" >&2
        fi
    fi
    if grep -aq "\[input\] quit" "$DATA/cod1.log" 2>/dev/null; then
        break
    fi
    sleep 0.5
    waited=$((waited + 1))
done
stop
cp "$DATA/cod1.log" "$OUT/cod1.log" 2>/dev/null
if [ -s "$DATA/profile.txt" ]; then cp "$DATA/profile.txt" "$OUT/profile.txt"; echo "profile: $OUT/profile.txt"; fi
echo "$shots screenshot(s) in $OUT; log: $OUT/cod1.log"
