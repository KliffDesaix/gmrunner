#!/bin/sh
# Builds the iOS app with Xcode (docs/MOBILE.md):
#   tools/ios/build.sh            dist/ios/gmRunner.ipa, unsigned, for SideStore / AltStore
#                                 (they sign it with your Apple ID when installing)
#   tools/ios/build.sh simulator  dist/ios/simulator/gmRunner.app, for the iOS Simulator
# Needs Xcode (26 or later for Liquid Glass) and the SDL2 source release in third_party/SDL2.
set -eu
root="$(cd "$(dirname "$0")/../.." && pwd)"
kind="${1:-device}"
if [ "$kind" = simulator ]; then sdk=iphonesimulator; build="$root/build-ios-sim"; else sdk=iphoneos; build="$root/build-ios"; fi
cmake -S "$root" -B "$build" -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
      -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_SYSROOT=$sdk >/dev/null
xcodebuild -project "$build/gmrunner.xcodeproj" -target gmrunner -configuration Release -sdk $sdk \
           CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY="" -quiet
app="$(find "$build" -name gmRunner.app -type d -path "*Release-$sdk*" | head -1)"
[ -n "$app" ] || { echo "build produced no gmRunner.app" >&2; exit 1; }
if [ "$kind" = simulator ]; then
    rm -rf "$root/dist/ios/simulator" && mkdir -p "$root/dist/ios/simulator"
    cp -R "$app" "$root/dist/ios/simulator/"
    echo "built dist/ios/simulator/gmRunner.app"
else
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/Payload" "$root/dist/ios"
    cp -R "$app" "$tmp/Payload/"
    (cd "$tmp" && rm -f "$root/dist/ios/gmRunner.ipa" && zip -qry "$root/dist/ios/gmRunner.ipa" Payload)
    rm -rf "${tmp:?}"
    echo "built dist/ios/gmRunner.ipa (unsigned: install it with SideStore or AltStore)"
fi
