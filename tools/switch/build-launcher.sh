#!/bin/sh
# Builds the Switch game list (switch/launcher, Borealis) into build-switch-launcher/:
# libgmrlauncher.a and libborealis.a, which Makefile.switch then links into gmrunner.nro, and
# romfs/ with the resources they load at run time (packed into the NRO). Run it inside the
# devkitPro image, before make:
#   docker run --rm -v "$PWD":/src -w /src devkitpro/devkita64 \
#       sh -c "tools/switch/build-launcher.sh && make -f Makefile.switch"
# Needs Borealis in third_party/borealis (git clone https://github.com/xfangfang/borealis).
set -eu
root="$(cd "$(dirname "$0")/../.." && pwd)"
out="$root/build-switch-launcher"
[ -f "$root/third_party/borealis/library/CMakeLists.txt" ] || { echo "Borealis is missing: third_party/borealis" >&2; exit 1; }
cmake -S "$root/switch/launcher" -B "$out" -DPLATFORM_SWITCH=ON -DUSE_SDL2=ON -DSDL2=ON -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$out" -j "$(nproc)" --target gmrlauncher
# Run-time resources (romfs:/): Borealis's strings, status icons and Material icon font (the
# Switch's own font comes from the system), and the launcher's standard game icon.
b="$root/third_party/borealis/resources"
rm -rf "$out/romfs" && mkdir -p "$out/romfs/img" "$out/romfs/material"
cp -R "$b/i18n" "$out/romfs/"
cp -R "$b/img/sys" "$out/romfs/img/"
cp "$b/material/MaterialIcons-Regular.ttf" "$b/material/LICENSE.txt" "$out/romfs/material/"
cp -R "$root/switch/launcher/resources/." "$out/romfs/"
find "$out/romfs/i18n" -name 'demo.json' -delete
# The licenses of the third-party software in the NRO (the game list's Licenses screen).
mkdir -p "$out/romfs/licenses"
for f in GPL-3.0 SDL2 gamemaker-html5 stb liberation-fonts libnx mesa libdrm ffmpeg dav1d bzip2 zlib borealis nanovg yoga fmt tinyxml2 tweeny \
         libretro-common switch-libpulsar glad material-icons-font GPL-2.0 Apache-2.0; do
    cp "$root/licenses/$f.txt" "$out/romfs/licenses/"
done
echo "built $out (libgmrlauncher.a, borealis, romfs)"
