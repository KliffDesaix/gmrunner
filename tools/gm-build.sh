#!/bin/bash
# Builds a GameMaker project (.yyp) into a folder gmRunner can run, with GameMaker's own compiler
# (Igor, VM output for macOS): game.ios plus the project's included files.
#
#   tools/gm-build.sh path/to/project.yyp out-dir
#
# Needs GameMaker installed and signed in (Igor won't build otherwise); GMTEST_RUNTIME, GMTEST_USER
# and GMTEST_PREFABS point at the runtime, user folder and prefab libraries when they aren't found
# by themselves.
set -eu
[ $# -eq 2 ] || { echo "usage: $0 project.yyp out-dir" >&2; exit 2; }
project="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
out="$(mkdir -p "$2" && cd "$2" && pwd)"

runtime="${GMTEST_RUNTIME:-$(ls -d /Users/Shared/GameMakerStudio2*/Cache/runtimes/runtime-* 2>/dev/null | sort -V | tail -1)}"
user="${GMTEST_USER:-$(ls -d "$HOME"/Library/Application\ Support/GameMakerStudio2*/*_* 2>/dev/null | grep -v unknownUser | head -1)}"
# The IDE's prefab libraries (downloaded when a project that uses them is opened).
prefabs="${GMTEST_PREFABS:-$(ls -d /Users/Shared/GameMakerStudio2*/Prefabs 2>/dev/null | tail -1)}"
[ -n "$runtime" ] || { echo "no GameMaker runtime found (set GMTEST_RUNTIME)" >&2; exit 1; }
[ -n "$user" ] || { echo "GameMaker isn't signed in (no user folder; set GMTEST_USER)" >&2; exit 1; }

work="$out/.igor"
rm -rf "${work:?}"; mkdir -p "$work/temp" "$work/cache" "$work/out"
# Igor's project loader often crashes (an access violation in its parallel loading), far less
# on one core, so it gets one and tries again. Its macOS packaging step fails too, after the game
# data is written, which is all we need.
for try in 1 2 3 4 5 6 7 8; do
    ( cd "$work" && DOTNET_PROCESSOR_COUNT=1 "$runtime/bin/igor/osx/arm64/Igor" --project="$project" \
        --rp="$runtime" --uf="$user" ${prefabs:+--pf="$prefabs"} --temp="$work/temp" --cache="$work/cache" \
        --of="$work/out/game.zip" \
        -r=VM -j=8 -- Mac PackageZip > "$out/igor.log" 2>&1 ) 2>/dev/null || true
    [ -f "$work/out/assets/game.ios" ] && break
done
[ -f "$work/out/assets/game.ios" ] || { echo "Igor didn't build $project; see $out/igor.log" >&2; exit 1; }
cp -R "$work/out/assets/." "$out/"  # game.ios, audio groups, ...
cp "$work/out/"*.yytex "$out/" 2>/dev/null || true  # dynamic texture groups
datafiles="$(dirname "$project")/datafiles"
[ -d "$datafiles" ] && cp -R "$datafiles/." "$out/"
rm -rf "${work:?}"
