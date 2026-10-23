#!/bin/sh
# Builds the PortMaster runner, dist/portmaster/gmrunner.aarch64, in Docker (see Dockerfile).
#   tools/portmaster/build.sh
# On an Apple silicon Mac or an arm64 Linux machine the container runs natively; elsewhere
# Docker emulates arm64 (slower, same result).
set -eu
root="$(cd "$(dirname "$0")/../.." && pwd)"
image=gmrunner-portmaster-build
docker build --platform linux/arm64 -t "$image" "$root/tools/portmaster"
mkdir -p "$root/dist/portmaster"
docker run --rm --platform linux/arm64 -e GMR_PRIVATE -v "$root":/src -w /src "$image" sh -c '
    set -e
    cmake -S . -B build-portmaster -DCMAKE_BUILD_TYPE=Release -DGMR_TARGET=handheld \
          -DCMAKE_DISABLE_FIND_PACKAGE_BZip2=ON >/dev/null  # the built-in bzip2 decoder, no libbz2
    cmake --build build-portmaster -j "$(nproc)" --target gmrunner
    strip -o dist/portmaster/gmrunner.aarch64 build-portmaster/gmrunner
    echo "glibc versions needed: $(objdump -T dist/portmaster/gmrunner.aarch64 | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1)"
    echo "shared libraries: $(objdump -p dist/portmaster/gmrunner.aarch64 | awk "/NEEDED/ {print \$2}" | tr "\n" " ")"
'
echo "built dist/portmaster/gmrunner.aarch64"
