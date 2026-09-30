# Developing

How to build, test and debug the port. What the original program and its data
look like is in [ANALYSIS.md](ANALYSIS.md); the milestones and history are in
[PLAN.md](PLAN.md).

## Building

Requirements: a C11 compiler, `make`, SDL2 development files and Python 3. The
other libraries are vendored in `third_party/` (FreeType, TinySoundFont,
minicoro, stb_image_write, font8x8).

```sh
# macOS
brew install sdl2
# Debian/Ubuntu
sudo apt install build-essential libsdl2-dev python3-venv

make                # build/cythera
make test           # the PowerPC interpreter's unit tests
```

**Windows**, with [MSYS2](https://www.msys2.org/) (UCRT64 shell, as in CI):
`pacman -S make mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-SDL2 mingw-w64-ucrt-x86_64-python`,
then `make PYTHON=python`. With a standalone MinGW-w64 instead (native
`mingw32-make`, run from Git Bash): put its `bin` first in `PATH` (Git's own
`/mingw64/bin` has DLLs that make `cc1.exe` fail silently), and pass the SDL
flags as Windows paths, since `sdl2-config`'s MSYS paths don't reach a native
make:

```sh
mingw32-make CC=gcc PYTHON=python SDL_CFLAGS=-IC:/mingw64/include/SDL2 \
  SDL_LIBS="-LC:/mingw64/lib -lmingw32 -lSDL2"
```

Build options (the Makefile doesn't track flags: after changing one, `make
clean` or use another `BUILD=` directory):

| Option | Effect |
|---|---|
| `BUILD=dir` | object and binary directory (default `build`) |
| `LICENSE_BYPASS=0` | run the original shareware registration flow unaltered (see README) |
| `GUI=1` | Windows: no console window (packages) |
| `LDFLAGS=-static`, static `SDL_LIBS` | a self-contained binary (the Windows package) |

## Game data

`tools/setup_gamedata.sh [INSTALLER] [OUTDIR]` extracts the game from the
original installer in `orig/` into `gamedata/`: the MacBinary `Cythera.bin`
(the default, unpacked in Python) or the StuffIt `Cythera_Installer.sit` (needs
`unar`). Both contain the same Installer VISE application, which
[installer-vise](https://github.com/mrmidi/installer-vise) (pinned, installed
in a temporary venv) unpacks. It also downloads the GeneralUser GS SoundFont
for the music. Set `PYTHON` if Python 3 isn't `python3`.

Without `--data`, the program uses `./gamedata`, or else the `gamedata` folder
next to the executable (`Contents/Resources` in the macOS app). In the second
case the folder is treated as read-only: the game never writes into it, so a
signed app bundle stays intact.

## Source layout

```
src/
  main.c             command line, loading the PEF, starting the PPC code
  cpu/ppc.[ch]       PowerPC interpreter (user-mode UISA + FPU)
  loader/pef.c       PEF loader: sections, pidata, relocations, import binding
  os/                the Mac OS Toolbox, one file per manager:
    trap.c             import -> native handler dispatch, guest callbacks (UPPs)
    memory.c           Memory Manager (zones, pointers, handles)
    resources.c files.c   Resource Manager, File Manager over a host directory
    qd_*.c             QuickDraw: ports, GWorlds, regions, CopyBits, shapes,
                       text (NFNT, TrueType via FreeType), PICT, cursors
    windows.c events.c menus.c dialogs.c controls.c lists.c textedit.c
    sound.c music.c    Sound Manager, QuickTime Music (tunes, note allocator)
    threads.c          Thread Manager (coroutines)
    license.c          the shareware registration bypass
    misc.c init.c ...  Gestalt, Time/Process Managers, Apple Events, start-up
  host/sdl.c         SDL video, audio, input, clipboard; scripted input
  plat.h             the few POSIX calls, for Windows
tools/               Python tools (below) and gen_traps.py (the trap table)
tests/               CPU unit tests, scripted scenarios, run.sh
```

Each imported call has a `TRAP(Name)` handler; `tools/gen_traps.py` builds the
table from those. [TOOLBOX.md](TOOLBOX.md) lists every import and how complete
it is.

## Tests

- `make test`: about 37,800 checks of the PowerPC interpreter against reference
  models.
- `tests/run.sh [SCRIPT...]` replays the scripted scenarios in `tests/scripts/`
  (all by default) and reports crashes, unimplemented calls, stalls and failed
  `expect`s. Runs are hermetic: each script gets a fresh temporary System
  Folder, copied from a fixture that `tests/fixture.txt` makes (a new game
  saved as "Hero"), never `~/.cythera-port`. `CYTHERA=path/to/binary` picks
  the program, `CYTHERA_ARGS` adds options. Screenshots and logs go to
  `work/shots/`.
- CI runs both on every push (Linux), see [Releases](#ci-and-releases).

Scripts (`--script FILE`) are one command per line: `wait N` (frames),
`click X Y`, `dclick`, `mousedown`/`mouseup`, `move`, `hold`, `key NAME`,
`type TEXT`, `shot FILE.png`, `expect TEXT` (fails unless that text was drawn
recently), `peek ADDR [N]` (hex dump of guest memory), `bt`, `dumpwin`,
`trace on|off`, `quit`. A first line `# app: NAME` makes `run.sh` start another
application. `peek 228578 4` gives the hero's position: map number, then x and
y as 12-bit fields (the character table is at 0x228558, 32 bytes each).

`--deterministic` runs headless on a virtual clock driven by the executed
instructions (the date starts at 2000-01-01), so a script replays identically.
Scripts that depend on wandering characters or combat depend on the exact
build, and on the fonts, since text layout decides line breaks and
conversation pages.

## Debugging

| Option | Use |
|---|---|
| `--headless` | no window |
| `--trace-traps`, `--trace-only A,B` | log Toolbox calls with symbolized callers and the emulated time |
| `--profile` | Toolbox and guest-function hot spots at exit |
| `--trap-stats FILE` | per-call counts at exit (`CYTHERA_ARGS="--trap-stats F" tests/run.sh` over all scenarios) |
| `--strict` | abort on an unimplemented call |
| `--turbo N`, `--timeout N` | faster emulated clock, time limit |
| `--wav FILE` | record the audio output |
| `--render-pict FILE OUT.png` | render a PICT file |
| `--shot-at-trap NAME[:N] OUT.png` | screenshot at the Nth call of a trap |
| `-v` / `-q` | more / less logging |

`CYTHERA_WATCH=off,...` logs the registers whenever execution reaches the given
code offsets (hex, as `tools/ppcdis.py` prints them): a cheap breakpoint.
`CYTHERA_FONT_DIR` is searched first for substitute system fonts (`Geneva.ttf`,
`Chicago.ttf`, ...). `SDL_RENDER_VSYNC=1` turns vsync back on.

## Analysis tools

- `tools/ppcdis.py gamedata/Cythera funcs|dis NAME|xref NAME|all`: a symbolizing
  PowerPC disassembler for the game binary (names from CodeWarrior's traceback
  tables; `xref` lists callers of a function or Toolbox call).
- `tools/pef.py`, `tools/rsrc.py`: PEF containers and resource forks.
- `tools/delv_archive.py info|list|dump ID|export DIR [SEL]`: the Delver
  archive (scenario or saved game): lists and decrypts resources, exports
  images and maps as PNG, sounds as WAV, props, strings and scripts as text.
- `tools/delv_props.py maps|props MAP`: maps and the objects on each, using
  [delvmod](https://github.com/BryceSchroeder/delvmod) cloned into
  `work/delvmod`. Handy for planning scripted routes.

## CI and releases

[`.github/workflows/build.yml`](../.github/workflows/build.yml) extracts the game
once, then builds and packages it on each platform: a Linux tarball (Ubuntu
22.04, which also runs all the tests), a static Windows zip (MSYS2 UCRT64,
`GUI=1`) and a universal macOS app in a disk image (static SDL2, per-arch
builds joined with `lipo`). Pushing a `v*` tag publishes a GitHub release with
the three packages. Failed jobs repeat their build and test errors as
annotations, which are readable without log access.

The macOS app is ad-hoc signed. With these repository secrets it is signed
with a Developer ID and notarized instead: `MACOS_CERT_P12` (base64 of the
.p12), `MACOS_CERT_PASSWORD`, `MACOS_SIGN_IDENTITY`, and `APPLE_ID`,
`APPLE_TEAM_ID`, `APPLE_APP_PASSWORD` for notarytool. Public repositories also
get artifact attestations: `gh attestation verify FILE -R Castux/cythera-port`.

## Portability rules

- Guest memory is big-endian and only accessed through the `rd*`/`wr*`
  helpers, so the host's endianness doesn't matter.
- Buffers for `realpath` must be `PATH_MAX` bytes: glibc's `_FORTIFY_SOURCE`
  (on by default in Ubuntu's gcc) aborts on anything smaller, whatever the
  path.
- `rename()` doesn't replace an existing file on Windows: remove it first.
- The SDL audio thread reads guest memory (sound buffers, tunes): code there
  must never call `fatal()`, and must stop the sound on bad data instead.
