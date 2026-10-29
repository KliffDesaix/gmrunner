# Contributing to gmRunner

Thanks for helping. gmRunner runs GameMaker games from their compiled data (`data.win`,
`game.unx`, `game.ios`) with its own runtime, and aims to behave exactly as GameMaker's own
runner does. Bug reports with a clear description, compatibility reports and pull requests are
all welcome.

## Ground rules

These keep the project legal and shareable. Pull requests that break them can't be merged.

- **No game files.** Don't commit or attach a game's data, assets, saves or decompiled code, in
  pull requests, issues or tests, even for free games. Describe what the game does instead; the
  maintainers will reproduce it with their own copy.
- **No GameMaker internals.** Don't decompile, disassemble or copy GameMaker's own runners or
  tools (the Windows/macOS/console runners, YYC, Igor). How GameMaker behaves is learned from
  its documentation, from tests run in GameMaker (see below), and from YoYo Games' open-source
  [HTML5 runner](https://github.com/YoYoGames/GameMaker-HTML5) (Apache 2.0, credited in
  [licenses/](licenses/README.md)), which may be read and followed.
- **No game names in commits or code.** Commit messages, comments and docs describe the
  behaviour ("a menu cursor kept in a 2D array couldn't move"), not which commercial game showed
  it. Naming the game in an issue is fine.
- **Licensing.** gmRunner is GPL-3.0-or-later, and your contribution is accepted under the same
  licence. Code from other projects needs a compatible licence and an entry in
  [licenses/](licenses/README.md).

## Building

- Desktop (macOS, Linux; the runner you'll test with): [README](README.md#building-desktop).
- Nintendo Switch: [docs/SWITCH.md](docs/SWITCH.md) (Docker with `devkitpro/devkita64`).
- PortMaster handhelds: [docs/PORTMASTER.md](docs/PORTMASTER.md) (Docker).
- Android and iOS: [docs/MOBILE.md](docs/MOBILE.md).

## Testing

**Feature tests** (`tests/gmtest`): a GameMaker project whose scripts log what they observe as
`name = value` lines; `tests/gmtest/reference.txt` holds the results of GameMaker's own runner,
and gmRunner has to match them. CI runs them on every push and pull request.

```bash
tools/gmtest.sh --prebuilt    # run the compiled test game kept in tests/gmtest/game
```

To add a test, add lines to a script in `tests/gmtest/src/scripts/` (or a new one, called from
`gmt_run_all`) that log with `gmt_log("area.what", value)`. The results then need GameMaker
itself: with GameMaker installed and signed in, `tools/gmtest.sh` builds the project with Igor
(and updates `tests/gmtest/game`), and after a Run in the IDE (`tests/gmtest/gmtest/gmtest.yyp`)
`tools/gmtest.sh --take-reference` stores GameMaker's results. Without GameMaker, open the pull
request with the new test and gmRunner's values, and say so: a maintainer will take the
reference.

**Games you own**: these can't be shared, so they're tested locally.

```bash
./build/gmrunner "/path/to/game" --check   # what the game needs, before starting it
./build/gmrunner "/path/to/game" --smoke   # visit every room headless and report problems
./build/gmrunner "/path/to/game" --trace   # play it and log runtime problems
```

`--goto ROOM@FRAME`, `--press`, `--input`, `--screenshot` and `--set` (see `--help`) script a run
so a scene can be reached and compared before and after a change; `build/gmdump` decodes a game's
data (`gmdump data.win disasm-all DIR` for its bytecode). Before a pull request that touches the
runtime, run a few games you have and say which ones in the description (by engine version and
genre if you'd rather not name them).

## Code

- C++17, four-space indentation, about 110 columns. Match the code around your change: naming,
  idioms, how much it's commented.
- GML functions live in `src/runtime/lib/*Lib.cpp` (`GMFN` to define, `GMREG` to register).
  Implement what GameMaker does, including its edge cases; when that isn't clear, write a
  feature test and check it against GameMaker.
- Comments say why, or which GameMaker behaviour a piece of code follows, not what the next line
  does.
- A function that can't be supported on a platform returns what GameMaker returns there (often
  0 or `false`), rather than pretending to succeed.

## Pull requests

- One topic per pull request; keep refactoring separate from behaviour changes.
- The first line of each commit message says what changed; the body says why, and what was
  wrong before.
- CI (desktop builds with the feature tests, and the Switch build) must pass.
- Update the docs and `--help` when behaviour or options change.

## Reporting bugs

Use the issue forms. The most useful reports include gmRunner's version, the platform, the output
of `--check`, and a `--trace` log of the problem, with the steps to reach it. Please don't attach
game files.
