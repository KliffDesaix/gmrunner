# Running GameMaker games on Linux handhelds (PortMaster)

gmrunner builds as a PortMaster port for ARM Linux handhelds: Anbernic, Powkiddy, Retroid,
Miyoo Flip and similar devices running ArkOS, ROCKNIX, muOS, Knulli, AmberELEC or another
firmware with [PortMaster](https://portmaster.games) installed. Each game becomes its own entry
in the Ports menu.

It is the same runner as on desktop and Switch, built fullscreen with OpenGL ES 2 and
controllers through SDL (PortMaster supplies each device's controller mapping).

## 1. Build the runner

The build runs in Docker on Debian 10 for arm64. Its old glibc (2.28) makes one binary run on
every current firmware, and the C++ runtime is linked in, so the device only has to provide SDL2,
which every firmware with PortMaster has.

```bash
tools/portmaster/build.sh
```

This produces `dist/portmaster/gmrunner.aarch64`. On an Apple silicon Mac or an arm64 Linux
machine the container runs natively; on x86 Docker emulates arm64, which is slower. The first
build also compiles a small FFmpeg into the image (the decoders games' videos need, LGPL), so
`video_open` works; the runner links it statically.

Without Docker, any arm64 Linux with g++, CMake and `libsdl2-dev` works:

```bash
cmake -S . -B build-portmaster -DCMAKE_BUILD_TYPE=Release -DGMR_TARGET=handheld -DCMAKE_DISABLE_FIND_PACKAGE_BZip2=ON
cmake --build build-portmaster --target gmrunner
```

The binary is 64-bit ARM only. Firmwares with a 32-bit userland (some older stock systems) are
not supported.

## 2. Make a port for a game

Use games you own (see [Your games](../README.md#your-games)). `tools/make-portmaster.sh --list`
makes one "gmRunner" port with a game list instead: copy games into its `gmrunner/games/` folder.

```bash
tools/make-portmaster.sh "/path/to/game folder" "Title"
```

The game folder is the Windows build's folder, the one with `data.win`; the `.exe` and `.dll`
files are left out. This writes `dist/portmaster/ports/`:

```
Title.sh                       the entry in the Ports menu
gmrunner/gmrunner.aarch64      the runner, shared by every game
gmrunner/games/Title/          the game's files
gmrunner/saves/Title/          the game's saves (created on first run)
gmrunner/log.txt               the last run's log
```

Run it once per game. All games share `gmrunner/`, so copying the `ports/` folder again only adds
the new ones.

## 3. Copy it to the device

Copy the contents of `dist/portmaster/ports/` into the device's ports folder: the one that
already holds the other ports' `.sh` files, e.g. `/roms/ports/` (ArkOS, AmberELEC),
`/roms/ports/` or `/storage/roms/ports/` (ROCKNIX), `/mnt/mmc/ROMS/Ports/` or the SD 2 equivalent
(muOS), `/roms/ports/` on the second card (Knulli). Then refresh the game list (on some firmwares
restart EmulationStation) and start the game from Ports.

## Controls

| Handheld                       | GameMaker                  | Keys (usual)    |
|--------------------------------|----------------------------|-----------------|
| A / B / X / Y (by position)    | gp_face1 / 2 / 3 / 4       | Z / X / - / C   |
| D-pad, left stick              | gp_pad*, gp_axisl*         | arrow keys      |
| L1 / R1 / L2 / R2              | gp_shoulderl / r / lb / rb |                 |
| Start                          | gp_start                   | Enter           |
| Select                         | gp_select                  | Escape          |
| Start and Select together      | the in-game menu (resume, controls, exit) | |

Face buttons follow GameMaker's positions: `gp_face1` is the bottom button, whatever it is
labelled. The keys each button presses are chosen per game and can be changed in the in-game
menu (see [SWITCH.md](SWITCH.md#button-mapping)). PortMaster's own quit hotkey also works. Games that read the mouse get the stick
pointer as on the Switch (right stick, R2 = left click, L2 = right click); on devices without a
right stick set `mouse_stick = left` in the game's `gmrunner.ini`.

## Per-game settings

`gmrunner/games/<Title>/gmrunner.ini` takes the same keys as on the Switch (see
[SWITCH.md](SWITCH.md#per-game-settings)): `rotate` for vertical games on a landscape screen,
`filter`, `integer_scale`, `button_layout`, `map_<button>`, `mouse_stick`, `trace`, ...

## Performance

These devices are much slower than a PC or a Switch. The runner interprets the game's bytecode
like GameMaker's own VM runner, so small 2D games run well on an H700 (RG35XX family) or RK3566
(RG353, RGB30) device. Heavy games, or games that rely on many surfaces and shaders, may not
reach full speed; RK3326 devices (RG351, R36S) are the slowest. Memory is usually 1 GB: texture
pages are loaded when a game first draws from them, and on devices with 2 GB or less the ones
unused longest are dropped again past a quarter of the memory (`GMR_TEXTURE_BUDGET_MB` in
`gmRunner.sh` sets another limit). Very large games may still not fit.

## Not supported yet

- 32-bit (armhf) firmwares
- Everything listed under "Not supported yet" in [SWITCH.md](SWITCH.md#not-supported-yet)
