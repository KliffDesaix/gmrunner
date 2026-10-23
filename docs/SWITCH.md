# Running GameMaker games on Nintendo Switch

gmrunner builds as a normal homebrew app (`gmrunner.nro`) for a Switch running custom
firmware (Atmosphère). It does not need Linux/Switchroot and does not touch any official
game's files.

## 1. Build the .nro

Install devkitPro (https://devkitpro.org/wiki/Getting_Started), then the Switch packages:

```bash
sudo dkp-pacman -S switch-dev switch-sdl2 switch-mesa switch-libdrm_nouveau
```

For games that play videos (`video_open`) also install FFmpeg;
the build picks it up automatically (the Docker image below already includes it):

```bash
sudo dkp-pacman -S switch-ffmpeg
```

`switch-bzip2` (also in the Docker image) is used when present as a second decoder for bzip2
texture pages; the built-in one handles all of them, randomised blocks included.

Build from the repository root:

```bash
make -f Makefile.switch
```

This produces `gmrunner.nro`.

Without a local devkitPro install you can use the official Docker image:

```bash
docker run --rm -v "$PWD":/src -w /src devkitpro/devkita64 make -f Makefile.switch
```

### The game list (Borealis)

With [Borealis](https://github.com/xfangfang/borealis) (the homebrew UI library in the style of
the Switch's own menus, Apache-2.0) in `third_party/borealis`, the NRO gets a game list in the
console's look. Build it before `make`; the Makefile links it in when it is there:

```bash
git clone --depth 1 https://github.com/xfangfang/borealis third_party/borealis
docker run --rm -v "$PWD":/src -w /src devkitpro/devkita64 \
    sh -c "tools/switch/build-launcher.sh && make -f Makefile.switch"
```

Without it, a plain text list is used instead. The same list can be previewed on a desktop:
`cmake -S switch/launcher -B build-launcher -DPLATFORM_DESKTOP=ON -DUSE_SDL2=ON` (Borealis's
`lib/extern/SDL` must then hold SDL2's source).

## 2. Put it on the SD card

Copy in games you own (see [Your games](../README.md#your-games)).

```
sdmc:/switch/gmrunner/gmrunner.nro
sdmc:/switch/gmrunner/games/MyGame/data.win
sdmc:/switch/gmrunner/games/MyGame/*.ogg          (all of the game's files)
```

Copy the whole game folder (data.win plus every .ogg and other file next to it). The
Windows `.exe` and `.dll` files are not needed.

The runner looks for a game in this order:

1. a path passed on the command line (hbmenu file association, see below),
2. `romfs:/game.txt` inside the .nro, naming a folder in `sdmc:/switch/gmrunner/games/`
   (this is what the per-game NROs below use),
3. the folders in `sdmc:/switch/gmrunner/games/` that contain a `data.win`, in the game list:
   **A** starts a game, **Y** switches between grid and list, **X** (or holding a finger on a
   game) renames it or picks its icon from the pictures in its folder (copy a `.png` or `.jpg`
   there), **+** returns to the Homebrew Menu. When the game ends, the list comes back. The
   title and icon are kept in the game's `gmrunner.ini` (`title =`, `icon =`), which the Android
   and iOS launchers use too. Without the Borealis list (see above): one game starts straight
   away; with several, a text menu lets you pick one (D-pad + A).

Optional: copy `switch/hbmenu-fileassoc.cfg` to `sdmc:/config/nx-hbmenu/fileassoc/gmrunner.cfg`
so hbmenu can open `data.win` files directly with gmrunner.

`tools/make-sdcard.sh` assembles all of this for you:

```bash
tools/make-sdcard.sh "/path/to/My Game" MyGame
```

It writes `dist/sdcard/` (the .nro, the game without its .exe/.dll, and the hbmenu file
association). Copy the contents of `dist/sdcard/` to the root of the SD card.

When Docker and macOS `sips` are available it also builds a per-game NRO,
`sdmc:/switch/gmrunner/<Name>.nro`, with the game's title and the icon from its `.exe`. It
shows up as its own entry in the Homebrew Menu and starts that game directly (no picker).
`tools/make-game-nro.sh` builds one on its own and takes `--icon`, `--author` and `--bg`. The
NRO holds no game files: the game stays a folder on the SD card, copied from your own copy.

Saves go to `sdmc:/switch/gmrunner/saves/<game>/`.

## 3. Launch with full memory

Homebrew started from the album applet only gets around 400 MB, which is not enough for
larger games (some keep hundreds of MB of audio). Use one of:

- **A home-menu forwarder (recommended).** A forwarder is a small installable title that
  launches an NRO from the SD card, as a full application with all of its memory, from its
  own icon on the HOME menu. Build the per-game NRO (above), then create the forwarder with
  any NSP forwarder generator, pointing it at `sdmc:/switch/gmrunner/<Name>.nro` and using
  `dist/forwarders/<Name>.jpg` as its icon. Generators need your console's own keys
  (`prod.keys`, dumped from your Switch with Lockpick_RCM); gmrunner does not create or ship
  NSPs. Install the forwarder with your usual title installer. Quitting the game (or a game
  calling `game_end`) returns to the HOME menu.
- **Title takeover:** hold **R** while launching any installed game, then pick the game in
  the Homebrew Menu.

## Controls

| Switch            | GameMaker                  | Keys (usual defaults) |
|-------------------|----------------------------|-----------------------|
| A / B / X / Y     | gp_face1 / 2 / 3 / 4       | Z / X / - / C         |
| D-pad, left stick | gp_pad*, gp_axisl*         | arrow keys            |
| L / R / ZL / ZR   | gp_shoulderl / r / lb / rb |                       |
| +                 | gp_start                   | Enter                 |
| -                 | gp_select                  | Escape                |
| + and - together  | the in-game menu           |                       |

Up to four controllers are players 1-4 (`gamepad_*` slots 0-3; handheld mode is player 1).
A single Joy-Con counts as a sideways controller: its buttons are mapped by their sideways
position (right = A, bottom = B, top = X, left = Y), SL/SR are L/R, its `+` or `-` is Start
and its stick is the left stick. So two separate Joy-Cons make two players with nothing to
rebind (local 2-player games, for example).

Each button also presses keys, so games that only read the keyboard work with a controller.
Which keys depends on the game: see [Button mapping](#button-mapping).

### The in-game menu

Holding **+ and -** together (Start and Select on other controllers) pauses the game and opens
the menu: Resume, Controls (what each button presses), and Exit game, which returns to the game
list (or to the Homebrew Menu). On phones the menu also opens from a button on the screen; see
[MOBILE.md](MOBILE.md).

### Button mapping

Every button presses the game's own gamepad button plus some keys. The keys are chosen per
game from what its code checks: a game that reads Space and not Z gets Space on A, one that
reads WASD gets WASD on the d-pad (with the arrows when it reads both), keys the usual ones
miss go on X, R and L, and the on-screen controls on phones only show the buttons that do
something. Change any of it in the menu's Controls page, or in the game's `gmrunner.ini`, one
line per button:

```ini
map_a = gp_face1 vk_space   ; the A button presses gamepad A and Space
map_x = Q                   ; X presses only Q
map_l = none                ; L does nothing
```

Buttons: `a b x y l r l2 r2 select start l3 r3 up down left right`, named by position as on an
Xbox controller and the on-screen pad (`a` is gp_face1: A on a Switch, or with
`button_layout = xbox` the bottom button). Targets: keys by their GameMaker names (`Z`, `1`,
`vk_space`, `vk_enter`, `vk_shift`, `vk_up`, `vk_f1` ...) and gamepad buttons (`gp_face1`,
`gp_shoulderl`, `gp_padu` ...), as many as wanted. `gmrunner --check` prints the map a game
gets, and the log has it as an `[input] buttons:` line.

### Mouse games

Games that read the mouse get two ways to point:

- **Touchscreen** (handheld): the first finger is the mouse (position and left button); further
  fingers are `device_mouse_*` devices 1-4 for multi-touch games.
- **Stick pointer** (any mode): the right stick moves a pointer, ZR is the left button and ZL the
  right one. The runner draws the pointer itself unless the game hides the cursor
  (`window_set_cursor(cr_none)`), as games that draw their own cursor do. It only turns on for
  games that use the mouse at all, so the right stick stays free otherwise.

## Per-game settings

Put a `gmrunner.ini` next to the game's `data.win`:

```ini
rotate = 0              ; 90 or 270 for vertical (TATE) play with the console on its side
filter = nearest        ; or linear
integer_scale = 0       ; 1 = only whole-number scaling (sharper, more border)
gamepad_to_keyboard = 1 ; 0 = buttons press no keys unless map_<button> says so
map_a = gp_face1 Z      ; what a button presses (see Button mapping); left out = chosen per game
button_layout = nintendo ; or xbox: map by position (B = gp_face1) instead of by label
show_fps = 0
mouse_stick = right     ; stick that moves the mouse pointer: right, left or off
mouse_speed = 1.0       ; pointer speed, in screen heights per second at full tilt
touch = 1               ; touchscreen drives the mouse
; diagnostics, written to sdmc:/switch/gmrunner/gmrunner.log:
trace = 0               ; 1 = log room changes and the sounds played
trace_calls =           ; engine functions to log with arguments, e.g. audio_play_sound,audio_stop_sound
```

Some vertical games have their own TATE option in their settings menu too.

## Not supported yet

- YYC-compiled games (no bytecode in data.win); the runner says so when one is opened
- Games that depend on Windows DLL extensions
