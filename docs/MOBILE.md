# Running GameMaker games on Android and iOS

gmrunner runs on phones and tablets as an app with a game list of the platform's own kind
(Material 3 Expressive on Android, SwiftUI with Liquid Glass on iOS). You copy the games you
own into the app's folder, as with emulators; the runner itself is the same as on desktop and
Switch, fullscreen with OpenGL ES.

Both builds use SDL2 built from its source release. Download it once and unpack it into
`third_party/SDL2` (for example from https://github.com/libsdl-org/SDL/releases, SDL2 2.32.x).

## The game list

- Tap a game to play it. The **grid / list** button in the top bar switches the view; the grid
  shows each game's icon, a standard controller icon until you set one.
- **Long press** a game to rename it, give it an icon from your photos, or choose how its
  in-game menu opens (Android: a sheet; iOS: the context menu). These are kept in the game's
  own `gmrunner.ini` (`title =`, `icon =`, `menu_shortcut =`), so the Switch list shows the
  title and icon too when you copy the folder there.
- Saves go in `Saves/<game>` next to the games.

## Playing

- **On-screen controls** (d-pad, A/B/X/Y in GameMaker's positions with A at the bottom, L/R,
  Select/Start) appear unless a controller is connected, with only the buttons the game uses.
  Each presses the game's gamepad button and the keys chosen for that game (see
  [Button mapping](SWITCH.md#button-mapping)). Touches that miss them are the game's mouse,
  so mouse and touch games work as well; games that only read the mouse get no buttons. They
  keep clear of the notch or camera cutout and the home indicator. `touch_controls = off` (or
  `on`, to keep them with a controller connected) in the game's `gmrunner.ini` overrides this.
- **The in-game menu** pauses the game: Resume, Controls (change what each button presses),
  On-screen controls, Menu shortcut and Exit game. It is the platform's own sheet (SwiftUI on
  iOS, Material 3 on Android), and saves to the game's `gmrunner.ini`. It opens with the
  floating button at the top of the screen, or as chosen under Menu shortcut (`menu_shortcut =`):
  a three-finger tap (`fingers`), Android's back button (`back`), or only Start + Select
  (`none`). Start and Select held together, on a controller or on the screen, always open it.
- The screen turns to suit the game (portrait for vertical games).
- Bluetooth and USB controllers work as on desktop.
- Android's back button is Escape (usually the game's pause menu), unless it opens the menu.
- Videos (`video_open`) play through the system's own decoders, MediaCodec on Android and
  AVFoundation on iOS (H.264 and the other formats the device supports), with their sound.

## Android

Build the APK (Android SDK with platform 36, NDK r26 and CMake 3.22, and JDK 17; Android Studio
installs them, or the command-line tools):

```bash
cd android
./gradlew assembleRelease
```

The APK is `android/app/build/outputs/apk/release/app-release.apk` (signed with the debug key,
for sideloading). Install it with `adb install` or by opening it on the phone.

Games go in a **gmrunner** folder on the phone's internal storage
(`/storage/emulated/0/gmrunner/<game>/data.win`, copied over USB or with a file manager). The
app asks for "All files access" on first start to read it; without that permission it uses its
own folder instead (`Android/data/org.gmrunner.app/files/`, which the list shows when it is
empty). The runner's log is `gmrunner.log` in the same folder.

Each game runs in a process of its own (`:game`); leaving it returns to the list.

## iOS

Build with Xcode (26 or later for Liquid Glass; earlier versions build too, with materials
instead):

```bash
tools/ios/build.sh             # dist/ios/gmRunner.ipa, unsigned
tools/ios/build.sh simulator   # dist/ios/simulator/gmRunner.app for the iOS Simulator
```

Install the `.ipa` with SideStore or AltStore, which sign it with your Apple ID. To build and run
from Xcode instead, open `build-ios/gmrunner.xcodeproj` (created by the script) and set your team
under Signing.

Games go in the app's folder in the **Files** app: On My iPhone (or iPad) > **gmRunner** >
`<game>/data.win`. The empty list has a "Show in Files" button that opens it. Copy the game
folders there from the Files app, AirDrop, or Finder (the iPhone's Files tab). The runner's log
is `gmrunner.log` in the same folder.

When a game ends (Exit game in the menu, or the game calling `game_end`), the list comes back.

The iOS Simulator connects a virtual controller of its own, so the on-screen controls stay
hidden there unless the game's `gmrunner.ini` has `touch_controls = on`. Games can also be put
straight into the Simulator's copy of the folder:
`xcrun simctl get_app_container booted org.gmrunner.app data` prints the app's folder, and the
games go in its `Documents`.

## Not supported yet

- Everything listed under "Not supported yet" in [SWITCH.md](SWITCH.md#not-supported-yet)
