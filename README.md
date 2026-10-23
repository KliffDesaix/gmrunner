# gmrunner

An open-source runner for GameMaker games compiled to bytecode (`data.win`), aimed at
running them as native Nintendo Switch homebrew, on Android and iOS, and on Linux handhelds
through PortMaster, with a desktop build (macOS/Linux/Windows) for development and testing.

It contains no YoYo Games code: it reads the game's `data.win` and runs its bytecode with
its own VM and runtime. You supply the game files you own.

**Target:** GameMaker Studio 1.4 games (bytecode 15/16) and GameMaker Studio 2 / 2.3+ games
(bytecode 17). Complete GMS 1.4 games play from boot to their end screens, and GameMaker
2022+ games play through their menus, cutscenes and core gameplay.

## Status

| Part | State |
|---|---|
| `data.win` loader + disassembler | GMS 1.4 and GMS2 up to 2024 (PNG, QOI and bzip2+QOI textures, room layers and layer effects, tilesets, audio groups, animation curves, asset tags) |
| Bytecode VM | all GMS 1.4 opcodes, `with`, scripts, arrays (copied on write as each version does, following the copy-on-write option), 2D arrays; GMS 2.3+ functions, methods, structs, constructors and statics, `callv`, global scripts, the 2023 `dup` swap form and variable hashes |
| Runtime | rooms, persistent rooms/instances, object inheritance, all event types, alarms, timelines, paths, views, tiles, backgrounds, activation; GMS2 layers (scripts, shaders), cameras, tile maps, asset layers, sequences, Clean Up events, time sources, async events, particle systems and emitters |
| Collision | bounding boxes (scale/rotation) and precise per-pixel masks |
| Graphics | OpenGL 2.1 / GLES 2, batched sprites/text/primitives, TrueType fonts (`font_add`, and Liberation fonts standing in for the default font: Mono with GameMaker 2's metrics, Sans for 1.4's Arial), surfaces, application surface, blend modes, fog, alpha test, the game's own GLSL ES shaders; vertex buffers, matrices, depth, culling and lighting for 3D; run-time sprites, layer filters (with the filter shaders the game ships) |
| Audio | software mixer: embedded WAV, streamed OGG, gain fades, pitch, positions, looping and loop points; emitters, listener and falloff models, audio group gain, sync groups, buses (effects not processed); keeps time without an audio device |
| Video | `video_*`, picture and sound in sync: FFmpeg on desktop (when installed), Switch and PortMaster, the system's decoders on Android (MediaCodec) and iOS (AVFoundation) |
| Input | keyboard (with `keyboard_set_map`), gamepads, mouse, touch (`device_mouse_*` 0-4); on Switch a stick-driven pointer for mouse games; controller and on-screen buttons mapped per game to keys and gamepad buttons (chosen from the game's code, changed in an in-game menu) |
| Extensions | native (DLL) functions return 0 or ""; FMODGMS (FMOD) is emulated on the runner's audio |
| Files | ini, text and binary files, per-game save directory |
| Built-ins | ~1580 GML functions (incl. aliases and platform stubs) and ~165 built-in variables |
| Switch | `Makefile.switch` builds `gmrunner.nro` (SDL2 + GLES2); same GLES path verified on Mesa, not yet run on hardware. See [docs/SWITCH.md](docs/SWITCH.md) |
| Android, iOS | `android/` (Gradle) and `tools/ios/build.sh` (Xcode): a game list in each platform's own design (Material 3 Expressive, SwiftUI with Liquid Glass), on-screen controls, games copied into the app's folder. See [docs/MOBILE.md](docs/MOBILE.md) |
| Linux handhelds | `tools/portmaster/build.sh` builds an arm64 PortMaster runner (SDL2 + GLES2, fullscreen); `tools/make-portmaster.sh` makes a port per game. See [docs/PORTMASTER.md](docs/PORTMASTER.md) |

## Building (desktop)

Needs CMake and SDL2 (`brew install sdl2 cmake`). FFmpeg (`brew install ffmpeg`) is optional and enables video playback. libbz2 (part of macOS, `libbz2-dev` on Linux) is used when found, as a second decoder for bzip2 texture pages.

```bash
cmake -S . -B build
cmake --build build -j
```

## Running

```bash
./build/gmrunner "/path/to/game folder"
```

Keyboard: arrows, Z/X/C, Enter, Escape as on Windows. Alt+Enter toggles fullscreen.
Saves go to `~/Library/Application Support/gmrunner/<game>/` on macOS.

Useful options:

```
--scale N            initial window scale
--linear             smooth scaling instead of nearest-neighbour
--rotate 90          rotate the picture (vertical/TATE play)
--mute               no audio
--stats              print step time / draw calls every second
```

Testing options (used to develop the runner without playing by hand):

```
--headless           no window or GL: game logic only
--hidden             render in a hidden window (for screenshots)
--fast               don't limit the frame rate
--frames N           quit after N steps
--press 150:Z,700:Z  tap keys at given steps
--input 300:Z:down   hold/release keys at given steps
--goto ROOM@STEP     jump to a room (debugging)
--screenshot 100,200 --sheet out.png   capture steps into one contact sheet
--record-audio out.wav                  mix audio offline to a WAV file
--seed N             deterministic random numbers (repeatable runs)
--save-dir DIR       use a separate save folder (tests start from a fresh save)
--gles               use an OpenGL ES 2 context, the code path the Switch uses
--capture-screen     screenshots show the final letterboxed/rotated screen
--trace              log room changes and their callers
```

Debugging environment variables:

```
GMR_TRACE_ID=<instance id>      log that instance's create/destroy/(de)activation
GMR_TRACE_GLOBAL=<name>         print a global after every room start
GMR_WATCH_GLOBAL=<name>         print a global whenever it changes, and each write with its code
GMR_TRACE_CALL=<fn>[,<fn>]       log every call to those built-ins with arguments, result and calling function
GMR_DUMP_OBJ=<obj>[,<obj>]      print every instance of those objects (all variables) ...
GMR_DUMP_EVERY=<N>              ... every N steps (default 30)
GMR_LIST_AT=<step>              list the live instances at that step
```

Per-game settings can also go in a `gmrunner.ini` next to `data.win` (see docs/SWITCH.md).

## Trying other games

Any game made with GameMaker Studio 1.4 or GameMaker Studio 2 / GameMaker (bytecode 17,
including the 2023 and 2024 releases) that ships a `data.win` is a candidate. Point the
runner at the folder that contains `data.win` (plus the game's other files):

```bash
./build/gmrunner "/path/to/game folder" --check   # compatibility report, doesn't start the game
./build/gmrunner "/path/to/game folder" --smoke   # boot it, visit every room headless, report problems
./build/gmrunner "/path/to/game folder"            # play it
./build/gmrunner "/path/to/game folder" --trace    # play it and log runtime problems
```

`--smoke` starts the game headless (fixed seed, throwaway save folder), runs its start-up,
then enters every room for a few seconds while tapping common keys, and prints the runtime
errors, unimplemented engine functions and warnings it hit, grouped and counted
(`--smoke-boot N` / `--smoke-frames N` change the step counts). `--smoke-shots DIR` also saves each room's
last frame (named after the room showing at the time), 4x4 contact sheets of them and an index,
for looking over a whole game at once.

With `--fast` (and so in `--smoke`), `current_time` and `get_timer` count game time (one frame
per step) instead of wall time, so the same inputs and `--seed` give the same frames every run.

`--check` reports the GameMaker version, whether the game can run at all (YYC builds,
which have no bytecode, can't), engine functions the runner doesn't implement yet, and
missing audio files. While playing, anything the runner can't handle is printed once as a
`[gml]` line in the terminal; that output is what's needed to fix a game.

For the Switch, `tools/make-sdcard.sh "/path/to/game folder" <Name>` (e.g. `MyGame`; the
name is the game's folder on the SD card and its title) adds the game next to any others
already in `dist/sdcard/`; with several games, the runner shows a menu. It also builds
`<Name>.nro`, which starts that game directly with its own title and icon: the NRO to point a
home-menu forwarder at (see [docs/SWITCH.md](docs/SWITCH.md)).

For Linux handhelds, `tools/make-portmaster.sh "/path/to/game folder" "Title"` makes a
PortMaster port in `dist/portmaster/ports/` (see [docs/PORTMASTER.md](docs/PORTMASTER.md)).

## Tools

`gmdump` inspects a `data.win` with the runner's own loader:

```bash
./build/gmdump game/data.win info
./build/gmdump game/data.win verify          # decode + validate every code entry
./build/gmdump game/data.win code <name>     # disassemble one script/event (shared blobs: see entry=)
./build/gmdump game/data.win locals <name>   # the FUNC chunk's local variable list for a code entry
./build/gmdump game/data.win builtins        # engine functions the game calls
./build/gmdump game/data.win sequences       # sequences: settings, track tree and keyframes
./build/gmdump game/data.win textures out/   # export texture pages as PNG
```

## Layout

```
src/data       data.win parser
src/util       bzip2/QOI texture decoding, PNG writer, filesystem helpers
src/vm         values, bytecode decoder, interpreter
src/render     GL loader and batched renderer
src/runtime    rooms/instances/events, graphics, audio, input, collision
src/runtime/lib  GML built-in functions
src/platform/sdl SDL2 host: Main.cpp (frame loop), Host.h (shared pieces), Target*.cpp (desktop,
               Switch, Linux handheld, mobile: finding the game, saves, error screen, controllers),
               TouchPad.cpp (on-screen controls), Buttons.cpp (what buttons press), GameMenu.cpp
               (the in-game menu), Picker.cpp (a drawn game list)
android/       Android app: Compose launcher and in-game menu, SDL activity
ios/           iOS launcher and in-game menu (SwiftUI), Info.plist
switch/launcher  Switch game list (Borealis)
tools          gmdump, Switch and PortMaster packaging
third_party    stb_image, stb_vorbis (public domain)
```

See [docs/ROADMAP.md](docs/ROADMAP.md) for implementation notes.

## License

gmRunner is free software: you can redistribute it and/or modify it under the terms of the GNU
General Public License as published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version. See [LICENSE](LICENSE).

## Third-party software

The builds include open-source libraries (SDL2, stb, and per platform AndroidX/Compose, libnx,
Mesa, FFmpeg, Borealis and others); [licenses/](licenses/README.md) lists which app contains
what, with the license texts each app also shows.

## Your games

gmRunner is for playing games you own on the devices you have: copy in the files of a game you
bought or that its developer gives away. It doesn't unlock, convert or share games, and it can't
be used to make ready-to-play copies of them. Please respect each game's licence; some developers
ask that their games only be played on the platforms they release them for.

## Trademarks

gmRunner is an independent project. It is not affiliated with, endorsed by or supported by
YoYo Games or Opera. GameMaker is a trademark of YoYo Games Ltd. and Nintendo Switch of Nintendo;
they are used here only to say what gmRunner works with. gmRunner contains no games and no part
of GameMaker.
