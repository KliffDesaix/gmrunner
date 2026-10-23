#!/bin/sh
# Builds a ready-to-copy SD card layout:
#   tools/make-sdcard.sh "/path/to/game folder" [GameName] [out dir]
# Copy the contents of the out dir (default: dist/sdcard) to the root of the SD card.
set -eu
game="${1:?usage: make-sdcard.sh <game dir> [name] [out dir]}"
name="${2:-$(basename "$game")}"
out="${3:-dist/sdcard}"
root="$(cd "$(dirname "$0")/.." && pwd)"

[ -f "$root/gmrunner.nro" ] || { echo "build gmrunner.nro first: make -f Makefile.switch" >&2; exit 1; }
[ -f "$game/data.win" ] || [ -f "$game/game.unx" ] || { echo "no data.win in $game" >&2; exit 1; }

mkdir -p "$out/switch/gmrunner" "$out/config/nx-hbmenu/fileassoc"
cp "$root/gmrunner.nro" "$out/switch/gmrunner/"
cp "$root/switch/hbmenu-fileassoc.cfg" "$out/config/nx-hbmenu/fileassoc/gmrunner.cfg"
mkdir -p "$out/switch/gmrunner/games/$name"
# Everything except the Windows runner itself.
for f in "$game"/*; do
    case "$f" in
        *.exe|*.dll|*.EXE|*.DLL) ;;
        *) cp -R "$f" "$out/switch/gmrunner/games/$name/" ;;
    esac
done
# A per-game NRO (own title and icon, starts this game directly) for the Homebrew Menu and
# home-menu forwarders. Optional: needs Docker and macOS sips.
if command -v docker >/dev/null && command -v sips >/dev/null && [ -f "$root/gmrunner.elf" ]; then
    sh "$root/tools/make-game-nro.sh" "$game" "$name" --folder "$name" --out "$out" ||
        echo "(per-game NRO not built; see tools/make-game-nro.sh)"
fi
echo "SD card layout ready in $out"
