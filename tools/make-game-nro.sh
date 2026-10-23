#!/bin/sh
# Builds a per-game NRO: the runner with the game's own title and icon that starts that game
# directly (no picker). Point a home-menu forwarder at it, or launch it from the Homebrew Menu.
#
#   tools/make-game-nro.sh "/path/to/game folder" ["Title"] [options]
#     --icon FILE     icon image (png/jpg/ico/bmp); default: the icon inside the game's .exe
#     --author TEXT   publisher shown on the Switch (default: gmRunner)
#     --bg RRGGBB     background behind transparent icon pixels (default: ffffff)
#     --folder NAME   game folder name under sdmc:/switch/gmrunner/games/ (default: the
#                     folder's own name)
#     --out DIR       SD card layout to write into (default: dist/sdcard)
#
# The NRO holds no game files, only the name of the game's folder: the game itself stays a folder
# on the SD card that the player copies from their own copy.
# Writes <out>/switch/gmrunner/<Name>.nro and <out's parent>/forwarders/<Name>.jpg (the icon, for the
# forwarder tool). The game itself goes in <out>/switch/gmrunner/games/<folder name>/
# (tools/make-sdcard.sh copies it there). Needs Docker (devkitpro/devkita64) and macOS `sips`.
set -eu
game="${1:?usage: make-game-nro.sh <game dir> [title] [--icon f] [--author a] [--bg rrggbb] [--folder name] [--out dir]}"
shift
folder="$(basename "$game")"
title="$folder"
if [ $# -gt 0 ] && [ "${1#--}" = "$1" ]; then title="$1"; shift; fi
icon=""; author="gmRunner"; bg="ffffff"; out="dist/sdcard"
while [ $# -gt 0 ]; do
    case "$1" in
        --icon) icon="$2"; shift 2 ;;
        --author) author="$2"; shift 2 ;;
        --bg) bg="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        --folder) folder="$2"; shift 2 ;;
        *) echo "unknown option $1" >&2; exit 1 ;;
    esac
done
root="$(cd "$(dirname "$0")/.." && pwd)"
[ -f "$root/gmrunner.elf" ] || { echo "build the Switch runner first: make -f Makefile.switch" >&2; exit 1; }
command -v sips >/dev/null || { echo "needs macOS sips for the icon conversion" >&2; exit 1; }
name="$(printf '%s' "$title" | tr -cd 'A-Za-z0-9')"
[ -n "$name" ] || name="Game"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# Icon: given file, else the game's .exe icon, else the runner's own.
if [ -z "$icon" ]; then
    for exe in "$game"/*.exe; do
        [ -f "$exe" ] && python3 "$root/tools/exe-icon.py" "$exe" "$tmp/game.ico" 2>/dev/null && icon="$tmp/game.ico" && break
    done
fi
[ -n "$icon" ] || icon="$root/switch/icon.png"
sips -s format bmp "$icon" --out "$tmp/src.bmp" >/dev/null
# Fit into 256x256: whole-number nearest-neighbour scaling keeps pixel art sharp.
python3 - "$tmp/src.bmp" "$tmp/icon.bmp" "$bg" <<'EOF'
import struct, sys
src, dst, bg = sys.argv[1], sys.argv[2], sys.argv[3]
d = open(src, "rb").read()
off = struct.unpack_from("<I", d, 10)[0]
w, h = struct.unpack_from("<ii", d, 18)
bpp = struct.unpack_from("<H", d, 28)[0]
top_down = h < 0
h = abs(h)
stride = (w * bpp // 8 + 3) & ~3
bgc = tuple(int(bg[i:i + 2], 16) for i in (0, 2, 4))
def pixel(x, y):
    row = y if top_down else h - 1 - y
    o = off + row * stride + x * (bpp // 8)
    b, g, r = d[o], d[o + 1], d[o + 2]
    a = d[o + 3] if bpp == 32 else 255
    return tuple((c * a + k * (255 - a)) // 255 for c, k in zip((r, g, b), bgc))
S = 256
scale = max(1, S // max(w, h)) if max(w, h) <= S else S / max(w, h)
sw, sh = int(w * scale), int(h * scale)
ox, oy = (S - sw) // 2, (S - sh) // 2
out = bytearray()
for y in range(S - 1, -1, -1):  # BMP rows bottom-up
    for x in range(S):
        sx, sy = x - ox, y - oy
        if 0 <= sx < sw and 0 <= sy < sh:
            r, g, b = pixel(min(w - 1, int(sx / scale)), min(h - 1, int(sy / scale)))
        else:
            r, g, b = bgc
        out += bytes((b, g, r))
hdr = struct.pack("<2sIHHI", b"BM", 54 + len(out), 0, 0, 54)
info = struct.pack("<IiiHHIIiiII", 40, S, S, 1, 24, 0, len(out), 2835, 2835, 0, 0)
open(dst, "wb").write(hdr + info + out)
EOF
sips -s format jpeg -s formatOptions 90 "$tmp/icon.bmp" --out "$tmp/icon.jpg" >/dev/null

mkdir -p "$tmp/romfs"
# The game list's resources and the third-party licenses, as in gmrunner.nro (when built).
[ -d "$root/build-switch-launcher/romfs" ] && cp -R "$root/build-switch-launcher/romfs/." "$tmp/romfs/"
printf '%s\n' "$folder" > "$tmp/romfs/game.txt"
cp "$root/gmrunner.elf" "$tmp/"
docker run --rm --platform linux/amd64 -v "$tmp":/w -w /w devkitpro/devkita64 sh -c \
    "\$DEVKITPRO/tools/bin/nacptool --create \"$title\" \"$author\" \"1.0.0\" game.nacp >/dev/null &&
     \$DEVKITPRO/tools/bin/elf2nro gmrunner.elf game.nro --icon=icon.jpg --nacp=game.nacp --romfsdir=romfs >/dev/null"

fwd="$(dirname "$out")/forwarders"  # next to the SD card layout, not on the card
mkdir -p "$out/switch/gmrunner" "$fwd"
cp "$tmp/game.nro" "$out/switch/gmrunner/$name.nro"
cp "$tmp/icon.jpg" "$fwd/$name.jpg"
echo "built $out/switch/gmrunner/$name.nro  (\"$title\", game folder: games/$folder)"
echo "forwarder: point it at sdmc:/switch/gmrunner/$name.nro, icon $fwd/$name.jpg"
