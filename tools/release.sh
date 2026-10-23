#!/bin/sh
# Builds a gmRunner release from a clean checkout into dist/release/<version>/:
#
#   gmrunner-<version>-switch.nro         Nintendo Switch (Docker: devkitpro/devkita64)
#   gmrunner-<version>-portmaster.zip     PortMaster: one "gmRunner" port with a game list (Docker)
#   gmrunner-<version>-android.apk        Android (Gradle, JDK 17, Android SDK and NDK)
#   gmrunner-<version>-ios.ipa            iOS, unsigned, for SideStore / AltStore (Xcode)
#   gmrunner-<version>-source.tar.gz      the source of exactly these builds (GPL: it goes with them)
#   ffmpeg-<version>.tar.xz               FFmpeg's source, linked into the PortMaster runner (LGPL)
#   SHA256SUMS
#
#   tools/release.sh                      the version in VERSION
#   tools/release.sh 0.2.0                sets VERSION to 0.2.0 first (commit it before tagging)
#   tools/release.sh --only switch,ios    some of the builds (switch portmaster android ios source)
#   tools/release.sh --with-private       builds the local modules in private/ in too (left out
#                                         otherwise; they aren't in the source archive either)
#
# Nothing is published: dist/release/<version>/ is what gets uploaded, e.g. with
#   gh release create v<version> dist/release/<version>/* --title "gmRunner <version>"
# The Switch NRO links devkitPro's FFmpeg (GPL): its source is devkitPro's switch-ffmpeg package,
# named with its version in the release's NOTICE.txt.
set -eu
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

only="switch portmaster android ios source"
private=0
while [ $# -gt 0 ]; do
    case "$1" in
        --with-private) private=1; shift ;;
        --only) only="$(printf '%s' "${2:?--only needs a list}" | tr ',' ' ')"; shift 2 ;;
        -*) echo "unknown option $1" >&2; exit 2 ;;
        *) printf '%s\n' "$1" > VERSION; shift ;;
    esac
done
version="$(tr -d ' \n' < VERSION)"
want() { case " $only " in *" $1 "*) return 0 ;; *) return 1 ;; esac; }
export GMR_PRIVATE="$private"  # read by CMake, Makefile.switch and the Docker builds

# Release builds come from committed source only, so the source archive matches them.
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    echo "uncommitted changes (VERSION included): commit them first" >&2
    git status --short --untracked-files=no >&2
    exit 1
fi
commit="$(git rev-parse --short HEAD)"
out="$root/dist/release/$version"
rm -rf "${out:?}"
mkdir -p "$out"
echo "gmRunner $version ($commit) -> $out$([ "$private" = 1 ] && echo " (with private modules)")"

docker_up() {
    docker info >/dev/null 2>&1 || { echo "Docker isn't running (needed for the Switch and PortMaster builds)" >&2; exit 1; }
}

if want switch; then
    docker_up
    echo "== Switch"
    docker run --rm -e GMR_PRIVATE -v "$root":/src -w /src devkitpro/devkita64 \
        sh -c "tools/switch/build-launcher.sh >/dev/null && make -f Makefile.switch clean >/dev/null && make -f Makefile.switch -j4 >/dev/null"
    cp gmrunner.nro "$out/gmrunner-$version-switch.nro"
    ffmpeg_switch="$(docker run --rm devkitpro/devkita64 sh -c 'dkp-pacman -Q switch-ffmpeg 2>/dev/null || pacman -Q switch-ffmpeg 2>/dev/null' || true)"
fi

if want portmaster; then
    docker_up
    echo "== PortMaster"
    tools/portmaster/build.sh >/dev/null
    stage="$(mktemp -d)"
    tools/make-portmaster.sh --list "$stage/ports" >/dev/null
    cp docs/PORTMASTER.md "$stage/ports/gmrunner/README.md"
    (cd "$stage/ports" && zip -qry "$out/gmrunner-$version-portmaster.zip" .)
    rm -rf "${stage:?}"
    # FFmpeg's source, the release the PortMaster build image builds (checked against its hash)
    ffv="$(sed -n 's/^ARG FFMPEG_VERSION=//p' tools/portmaster/Dockerfile)"
    ffsum="$(sed -n 's/^ARG FFMPEG_SHA256=//p' tools/portmaster/Dockerfile)"
    curl -fsSL -o "$out/ffmpeg-$ffv.tar.xz" "https://ffmpeg.org/releases/ffmpeg-$ffv.tar.xz"
    echo "$ffsum  $out/ffmpeg-$ffv.tar.xz" | shasum -a 256 -c - >/dev/null
fi

if want android; then
    echo "== Android"
    keyprops="${GMR_KEYSTORE_PROPERTIES:-$HOME/.gmrunner/keystore.properties}"
    [ -f "$keyprops" ] || { echo "no release key: run tools/android/make-release-key.sh first" >&2; exit 1; }
    [ -n "${JAVA_HOME:-}" ] || [ ! -d "$HOME/Library/Android/jdk17/Contents/Home" ] || export JAVA_HOME="$HOME/Library/Android/jdk17/Contents/Home"
    (cd android && ./gradlew -q assembleRelease)
    cp android/app/build/outputs/apk/release/app-release.apk "$out/gmrunner-$version-android.apk"
fi

if want ios; then
    echo "== iOS"
    tools/ios/build.sh >/dev/null
    cp dist/ios/gmRunner.ipa "$out/gmrunner-$version-ios.ipa"
fi

if want source; then
    echo "== source"
    tarball="$out/gmrunner-$version-source.tar"
    git archive --format=tar --prefix="gmrunner-$version/" -o "$tarball" HEAD
    # builds with the private modules are GPL'd as a whole: their source goes with them
    if [ "$private" = 1 ] && [ -d private ]; then
        tar -rf "$tarball" -s ",^private,gmrunner-$version/private," private
    fi
    gzip -9 "$tarball"
fi

cat > "$out/NOTICE.txt" <<EOF
gmRunner $version (commit $commit)

gmRunner is free software under the GNU General Public License, version 3 or later, with NO
WARRANTY. Its source for these builds is gmrunner-$version-source.tar.gz; the licenses of the
third-party software in each build are in licenses/ inside it (and shown by each app).

Third-party source:
- FFmpeg (PortMaster runner, LGPL 2.1 or later): $(ls "$out" | grep '^ffmpeg-' || echo "see tools/portmaster/Dockerfile")
- FFmpeg (Switch, GPL 2.0 or later): devkitPro's switch-ffmpeg package${ffmpeg_switch:+ ($ffmpeg_switch)},
  source at https://github.com/devkitPro/pacman-packages (switch/ffmpeg)
- SDL2, libnx, Mesa, libdrm, dav1d, bzip2, zlib, Borealis and the rest: see licenses/README.md
  in the source archive for each one's license and home page

gmRunner contains no games and no part of GameMaker. GameMaker is a trademark of YoYo Games
Ltd.; gmRunner is not affiliated with or endorsed by YoYo Games or Opera.
EOF

(cd "$out" && shasum -a 256 $(ls | grep -v SHA256SUMS) > SHA256SUMS)
echo "done:"
ls -la "$out"
