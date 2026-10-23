# Roadmap and implementation notes

## Status

Complete GameMaker Studio 1.4 games (build 1773, bytecode 16) play from boot to their end
screens, with music and sound, on macOS (OpenGL 2.1), Linux (OpenGL ES 2 / Mesa), Android and
the Switch. GameMaker 2022+ games (bytecode 17) play through their menus, cutscenes and core
gameplay; samples of several hundred rooms load and run with no VM errors.

Rooms are smoke-tested headless (`--smoke`, or `--goto ROOM@STEP` with a fixed `--seed` and a
fresh `--save-dir`) for VM errors on the normal game paths.

## Next steps

- Test on Switch hardware: controller mapping, docked/handheld switching, frame pacing,
  memory use in applet mode.
- Hardware performance pass if needed: a step takes 0.1-0.4 ms on an M4 in most games and
  ~1.2 ms on the busiest title screen tested (1.9 ms before Value's reference counting went inline and
  arithmetic, comparisons and conversions on numbers got in-place fast paths); expect several
  times that on the Switch's Cortex-A57. The interpreter loop (`VM::execute`) and variable
  lookups are what's left in profiles. Start-up: texture pages are bzip2+QOI in GMS2 games
  (PNG in 1.4); the bzip2 decoder decodes most Huffman codes with one table lookup.
- More GMS 1.4 games to widen built-in coverage (most 1.4-era titles are bytecode 15/16).
- More GMS2 games: drawing the built-in layer filters, audio bus effects (reverb, delay,
  filters, ...); sequence text and clip-mask tracks, `sequence_instance_override_object`;
  particle systems in global space (`part_system_global_space`).

## Bytecode notes (GMS 1.4, bytecode 15/16)

- Each instruction is a 32-bit word: opcode in bits 24-31, types in bits 16-23, operand in
  bits 0-15, followed by 0-2 extra words.
- Variable/function references are resolved by walking VARI/FUNC occurrence chains: each
  reference operand holds (in its low 27 bits) the distance to the next occurrence.
- Self and global variables share one id space; local ids are frame slots.
- Array access: the operand's instance field is unused; stack order for `pop.v.T [array]`
  is `value, instance, index` (index on top). `pop.i.v` is the compound-assignment form
  with the value on top.
- `dup.T n` duplicates `(n + 1) * sizeof(T)` **bytes** of stack (int 4, double/int64 8,
  variable 16), which can span several items (`dup.l 0` after two `pushi` duplicates both).
- `pushenv` jumps to its own `popenv` when the `with` target is empty; `popenv` with offset
  `0xF00000` leaves the loop early (used before `exit` inside `with`).
- `break -1` is `chkindex` (array bounds check); 2D arrays are flattened as `i * 32000 + j`.

## Runtime semantics worth knowing

- OPTN flag `0x400000` ("treat uninitialized variables as 0") is honoured; 1.4 games rely
  on it. Flag `0x800000` selects Create event before instance creation code.
- Variables of deactivated instances, and of instances destroyed earlier in the same step,
  stay readable by id and by object (`obj.var`). Hit-stop and game-over code in 1.4 games
  depends on this; `instance_exists` and `with` still skip them.
- Events are dispatched object by object (ascending object index), then by creation order
  within an object, as in GameMaker's runner. Plain creation order breaks games: its
  level controller clears `global.levelcontrol` in Room End, which other persistent
  objects read in their own Room End.
- Alarms only count down when the object (or a parent) has the matching alarm event; the
  value is truncated, counted down while >= 0, and fires on reaching exactly 0 (so an alarm
  set to 0 never fires, and it reads 0 inside its own event).
- Render targets store row 0 at the top so sprites and surfaces share one UV convention;
  only the final present to the real screen flips.
- The game draws into a virtual window (`window_set_size` changes it), which is then
  letterboxed onto the real screen, optionally rotated for vertical play.

### Engine subsystems added for GMS2 games

- Time sources (`time_source_*`, `call_later`): a tree under `time_source_global`, `_game` and a
  hidden parent for `call_later`, ticked after timelines; every source is ticked before any
  callback runs (YoYo's HandleTimeSources).
- Particles follow YoYo's yyParticle.js: per-type ranges, the deterministic wiggle (`ran`), colour
  modes (one/two/three, RGB, HSV, mix), separate x/y sizes, sub-images, emitters with region
  shapes and linear / Gaussian / inverse-Gaussian distributions, delays and intervals. Since
  2023.2 particles belong to their emitter; before that a system has one list. The built-in
  shape images are generated procedurally.
- Particle system assets (PSYS, with emitters in PSEM, 2023.2+) become systems as YoYo's
  MakeInstance does: the particle types of all emitters are made at start (so the game's own
  `part_type_create` ids come after them), and a system copies its asset's emitters last first.
  Room editor systems sit on asset layers (destroyed with the room); sequence particle tracks
  make one per key, step it themselves and draw it in the track's space.
- 3D: vertex formats and buffers (`vertex_*`, raw submits with GameMaker's attribute names),
  world/view/projection matrices in GameMaker's layout (the 2D camera is YoYo's Build2DView:
  look-at from z -16000 plus an orthographic projection), camera matrices, matrix stack, depth
  test/write/func and culling with lazily created depth buffers, the 8 directional/point lights of
  the built-in shader, and extra sampler stages.
- Layers: begin/end scripts and layer shaders run around each layer in every draw pass, and
  instances on invisible layers get no draw events.
- The GUI layer has its own size (`display_set_gui_size`, `display_set_gui_maximize`), mapped
  onto the window; `device_mouse_x/y_to_gui` use it.
- Run-time sprites (`sprite_add`, `sprite_create_from_surface`, ...) get their own texture pages;
  surfaces can be read back (`surface_getpixel`, `buffer_get_surface`).
- Audio: emitters and the listener with OpenAL-style falloff models (none, inverse, linear,
  exponent, their clamped and scaled forms) and left/right balance from the listener's right
  vector (look-at x up), so GameMaker's default orientation mirrors left and right as it does
  there; audio group gain, loop points, sync groups started sample-aligned, `audio_play_sound_ext`.
  Buses carry a gain; their effects are recorded but not processed.
- Async events (HTTP 62, dialogs 63, save/load 72) are queued with their `async_load` map and
  dispatched after Begin Step. Async buffer saves/loads complete at once; HTTP requests fail as
  if offline (no network access); dialogs return their default.
- Animation curves (ACRV) are GML structs evaluated like YoYo's runner (linear, centripetal
  Catmull-Rom or bezier samples); asset tags come from TAGS ((type << 24) | index keys).
- Layer effects: the `fx_*` / `layer_*_fx` API keeps filter parameters (room layers' effects are
  loaded from the data). Filters are drawn with the shaders the game ships for them
  (`_filter_<name>_shader`, `src/runtime/Filters.cpp`); the 2023+ scripted effects
  (`_effect_<name>_script`) aren't run yet (no test game that can run uses one).
- Cameras follow YoYo's camera manager: ids are never reused, the default camera comes first,
  every room creates one camera per view (destroyed when a non-persistent room ends; a
  persistent room keeps its own), `camera_create` cameras live until `camera_destroy`, and only
  cameras of visible views follow their target. The active camera is set while a view draws
  (the default camera when views are off) and is -1 outside the room's draw.
- Without an audio device (`--mute`, headless, smoke tests) each step mixes one frame of audio
  and drops it, so track positions and `audio_is_playing` advance; music-synced cutscenes
  (an intro cutscene seen in testing) otherwise wait forever.
- Video (`video_*`): one video at a time decoded by FFmpeg (MediaCodec on Android, AVFoundation on
  iOS) into a surface (video_format_rgba);
  its audio track is decoded up front and played as a sound whose position is the video clock.
  `video_start` / `video_end` arrive as Async Social events.
- Sequences (SEQN, `src/runtime/lib/SequenceLib.cpp`) follow yySequence.js / yyRoom.js. The
  asset is a GML struct tree (`tracks[]`, `keyframes[]`, `channels[]`) and playback compiles it
  again each step once the game has been handed it, so edits such as
  `seq.tracks[i].keyframes[0].channels[0].spriteIndex = ...` show. Track lists are reversed on
  load (the file lists the top track first; GameMaker keeps and draws them bottom first).
  Parameter tracks (position, rotation, scale, origin, colour multiply/add, image index/speed,
  gain/pitch) become per-track transforms nested into the parent's (children add the parent's
  origin back unless the track's trait says not to); graphic tracks draw through that affine
  transform, sequence tracks nest other sequences at a rescaled head, instance tracks create
  their instances on the element's layer on first play, steer them every step and draw them in
  track order (`drawn_by_sequence`), audio tracks play sounds (without emitters). A sequence is
  updated right before the Step event and only drawn once it has been updated. Broadcast
  messages arrive as Other > Broadcast Message with `event_data`; moments call their function.
- Extensions: native (DLL) functions listed in EXTN return 0 or "" as if the library were
  missing, except FMODGMS, which is emulated on the runner's audio; GML extension files are
  ordinary scripts.
- A multi-chapter game's launcher passes `launcher` to each chapter; started without it, a chapter
  looks for its music in its own folder instead of the shared `mus/`.

### Checked against YoYo's HTML5 runner

YoYo's GameMaker-HTML5 runtime (Apache 2.0, github.com/YoYoGames/GameMaker-HTML5) is used as
the reference for engine behaviour; gmrunner reimplements it, it doesn't include that code.
Where the HTML5 port differs from the native runners the games were built with, native
behaviour wins (event dispatch order by object, `other` in a created instance's Create).

- Step: once a room change, restart or game end is requested, the rest of the step is
  skipped, drawing included. GMS2 remembers previous positions and advances `image_index`
  before Begin Step, then wraps it (one cycle) and runs Animation End; 1.4 animates after
  End Step. Instances created during a step skip that step's alarms, timelines, collisions
  and outside/boundary checks (GMS2).
- Motion, per instance: friction (towards 0, never crossing it), gravity, path following,
  then `x += hspeed`. sin/cos are cut to 6 decimals, degrees use 0.0174532925, and speeds,
  components and directions within 0.0001 of a whole number snap to it.
- Paths: anchored at `path_xstart/ystart` (the instance, or the path's first point when
  absolute), rotated around the first point; following one sets `direction` from the
  movement and `speed` to 0; Path End only fires while `path_speed` isn't 0.
- Collisions: object pairs are registered once (the reverse pair or a parent pair already
  covering it is skipped). Each colliding pair is handled once per step: both go back to
  their previous position if either is solid, then both get their event, matched up the
  other object's parent chain first.
- Keyboard: held, pressed, released passes, each key by key, then vk_anykey / vk_nokey.
- Outside Room fires on leaving; Intersect Boundary, Outside View n and Intersect View n
  Boundary fire every step the condition holds.
- Timelines fire moments in [old, new) going forward and (new, old] going backward.
- `instance_create_depth/layer`: Pre-Create, then the optional struct's variables, then Create.
- Layer names are matched case-insensitively.
- `variable_instance_get/set/exists` (and `variable_struct_*` / `struct_*_from_hash` on an
  instance) reach the per-instance built-ins (`x`, `image_alpha`, `alarm`, ...) by name; games'
  tweens set `image_alpha` this way.
- GMS2 keeps the window and application surface at the size the game started with; each frame
  the bounding box of the visible view ports is stretched over the application surface
  (`g_DisplayScaleX/Y`), so ported 640x320 rooms still fill 640x480.
  `surface_resize(application_surface, ...)` sets the application size (a game rendering at 192x192
  and is scaled up). GMS 1.4 sizes both to every room.
- Alpha: `draw_set_alpha` clamps to 0..1. The HTML5 port lets a negative sprite alpha wrap to
  nearly opaque, but the native runner draws it invisibly (a name-entry screen tested relies on it).
- Dates are UTC-based day counts; their fields are read and built in local time or UTC as
  `date_set_timezone` says.
- Paths: mirror, flip, rotate and rescale act about the centre of the points' bounding box.
- ds_grid region queries swap and clamp their corners into the grid; disks cover the cells
  whose centres are within the radius; strings count towards min/max but not sums.
- GMS2 tiles (in room asset layers, from `layer_tile_create`) take a sprite's first frame, not a
  background; rooms converted from 1.4 keep most of their art in such tiles.
- Nine-slice sprites (2.3.2+): `draw_sprite*`, instances and layer sprites are drawn at
  sprite size times scale with the borders kept at their size and the edges and centre
  stretched, repeated, mirrored, drawn once (blank repeat) or hidden; the origin scales with the
  drawn size, and `draw_sprite_stretched` uses the top-left corner. A tiled floor tested
  is a 20x20 tile repeated this way.
- The application surface is made by the first draw, and surface 0 never exists, so a game's
  "no surface yet" 0 fails `surface_exists` (window silhouettes drawn this way would otherwise
  resize the application surface). `surface_resize(application_surface, ...)` before the first
  draw (when it still reads -1) sets the application size.
- An event an object defines overrides its parent's even when it has no code: an empty child
  Create stops the parent's from running (Kultisti's camera objects rely on it), and an empty
  Draw turns off the default drawing.
- `audio_exists` is true for a voice that is still playing, not only for sound assets: games
  stop the previous track or resume a song only when it says so.
- GMS2's default camera is the first camera, made before any game code, so it is camera 0
  (games size surfaces with `camera_get_view_width(0)`).
- `is_real` is true for any plain number, int32 included (variables written with `pop.v.i`),
  but not for bools or int64s; door code checking an entrance number relies on it.

## GameMaker Studio 2 support

Tested on GameMaker 2022.8+ games and loaded/smoke-tested on games from GameMaker Studio
2.0-2.1 (bytecode 16) through 2.2, 2.3, 2023.2+ and 2024 (`--smoke`).

Loader:
- GMS2 GEN8 (game speed), 2022.9 TXTR entries, PNG / QOI / bzip2+QOI textures
  (`src/util/Bzip2.cpp`, `src/util/Image.cpp`), tilesets, fonts with kerning (glyph list
  located by shape), 2022.5 object "managed" flag, room layers (instances, backgrounds,
  tiles, assets) and 48-byte room instances, constructor scripts, VARI padding, function
  chains that point at the operand word, audio groups from `audiogroupN.dat`.
- Asset lists can have empty (0) slots in GameMaker 2024 builds that strip unused assets; the
  slot is kept as an empty asset so later indices stay right. The 2024 compilers also call
  `@@SetStatic@@` at the start of constructors and `@@string@@` for `string()` with arguments.
- Room instances are 48 bytes from GMS 2.2.2 (image speed and index), 40 before. The size is
  read from the spacing of a room's entries; a room with a single instance has none, so it
  follows the game's other rooms (guessing wrong reads a garbage pre-create code, which ran a
  random script in a character-select room).
- SEQN: version 1, then per sequence its settings, broadcast-message keyframes, tracks (model
  name, name, built-in parameter, traits, creation flag, tags, owned resources, subtracks,
  keyframes whose data depends on the model; real/colour keys may embed an animation curve),
  function ids and moment keyframes. Asset layers list room sequences after the sprites (2.3+).
- Sprite headers: `-1, version, type, playback speed, speed type` for every GMS2 version,
  then a sequence offset (version 2+) and a nine-slice offset (version 3+).
- Room layer effect fields (2022.1+) are detected from the data (an instance layer without
  them starts with a count and known instance ids), not from the FEDS/FEAT chunks.
- GameMaker 2024 rooms have one more list after the tiles list, moving the layer list from
  +88 to +92 (detected: older rooms hold the physics "world" flag, 0 or 1, there).
- Data file lookup: `data.win`, Linux `assets/game.unx`, macOS
  `Contents/Resources/game.ios`; the folder holding it is the game folder.

Run order: GMS2 runs Pre-Create (object variable definitions), the instance's pre-create
code, Create, then creation code, as YoYo's runner does; the OPTN "create before code" flag
only applies to GMS 1.4 games.

Bytecode 17 / GMS 2.3 semantics:
- Global scripts (`GLOB`) run at start with `self` = global; functions live inside them and
  are referenced with `push.i` + a FUNC chain, becoming method values.
- `callv` calls a function value: stack is args (last first), then `self`, then the function.
- `break -5` = setowner (pops the owner), `break -10` = chknullish, `break -1` = chkindex.
- Multi-dimensional arrays: `push.v [multipush]` / `[multipushpop]` (access kinds 0x10 /
  0x90) pop target and first index and push the inner array; `break -2` pushaf and
  `break -3` popaf read/write its element, `break -4` pushac goes one level deeper.
- Statics: `break -6` isstaticok / `break -7` setstatic per function; target -16 is the
  function's static scope, and structs made by a constructor fall back to it.
- `break.i -11` pushref (2023.8+) carries an extra word: an asset reference with the type in
  the top byte (0 object, 1 sprite, 2 sound, 3 room, 5 script, 6 font, 8 shader, 10 curve),
  or a function through the FUNC chain.
- `push.v stacktop.x` / `pop.v.v stacktop.x` (operand -9) take the target from the stack.
- Instance target -15 is the argument scope; -6 on user variables means self, then global.
  Built-ins are flagged by var id -6 or instance type -6 (`argument0`-`argument3`).
- `pushi.e -9; pushenv` is `with (<value below on the stack>)`; about 40% of one large 2022 game's
  `with` blocks use this form.
- Local VARI entries can be marked -1, so local slots are assigned per code entry by access
  scope, with a per-frame overflow map.
- Scripts may assign arguments that weren't passed (`if (argument0 == undefined) argument0 = 0`);
  those writes are kept per call.
- A numeric string compares equal to the number (`"2" == 2`, used by `switch (string(c))`),
  and numeric strings passed to built-ins that expect numbers are converted. Both apply only
  to GMS2 games; 1.4 keeps its stricter behaviour.
- `pushi.e -9` before a `[stacktop]` push/pop also means "target below" (`speakers[i].voice`), and
  so does a -9 target on `[array]` pushes and pops (`global.arr[i] = v` via `@@Global@@`).
- `argumentN[i]` reads and writes an element of the array passed as argument N (not argument N).
- Newer compilers (2023+) emit a swap form of `dup`: when the second operand byte is set,
  `dup.T a b` moves the top `a * sizeof(T)` bytes below the `((b & 0x7F) >> 3) * sizeof(T)` bytes
  beneath them. `dup.v k 136` brings a method's target above its k arguments
  (`new X(0).f(i)`, `other.damage(1, self)`); `dup.i 4 200` keeps the old value under the target
  in `a[n++] = v`.
- Variable hashes (2023+, `struct_get_from_hash`): the compiler emits `push.i` operands that sit on
  the variable's VARI reference chain, patched to the variable's id at load like function
  references. gmrunner compiles them to its own variable id, which `variable_get_hash` returns.
- Structs have no built-in variables: `s.x` on a struct is an ordinary member, not `x`.
- Names the compiler didn't know are compiled as variable reads (`builtin.time_source_create`,
  `self.time_source_global`); the runner resolves an unset variable with such a name to the
  built-in function (as a function value) or the run-time constant.
- `with (struct)` keeps the struct alive for the whole block; it can be a temporary returned by
  a call.
- Texture handles (`sprite_get_texture`, `surface_get_texture`, `font_get_texture`) are `ptr`
  values; Scribble skips glyphs whose texture `is_numeric`. `font_get_info` glyph positions are on
  the whole texture page.
- When a Create event requests a room change during room start, the room's remaining
  non-persistent instances are created but skip Create and creation code (as in YoYo's
  HTML5 runner, `StartRoom`). A title room tested depends on this after "End it all".
- `#` is not a newline in GMS2 text; Clean Up runs on destroy and room end; sprite playback
  speed can be frames-per-second or per-game-frame.
