#!/bin/sh
# Installs build-vita/ff.vpk into Vita3K (macOS) and links the game data:
# ux0:app/FFRD00001 and ux0:data/ff/pc -> gamedata/pc. Run from the repo root.
set -e
FS="${VITA3K_FS:-$HOME/Library/Application Support/Vita3K/Vita3K/fs}/ux0"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
rm -rf "$FS/app/FFRD00001"
mkdir -p "$FS/app/FFRD00001" "$FS/data/ff"
(cd "$FS/app/FFRD00001" && unzip -q -o "$ROOT/build-vita/ff.vpk")
ln -sfn "$ROOT/gamedata/pc" "$FS/data/ff/pc"
echo "installed: $FS/app/FFRD00001 (log: $FS/data/ff/ff.log)"
