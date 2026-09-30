# Cythera for modern systems

This is a port of **Cythera** (Ambrosia Software, 1999), the classic Mac RPG built
on Glenn Andreas's Delver engine, to current operating systems (macOS, Linux,
Windows) using C and SDL2.

The approach: the original PowerPC game code runs **unmodified** on a built-in
PowerPC interpreter. The classic Mac OS Toolbox it calls (QuickDraw, Window, Menu,
Dialog, Control, List and TextEdit Managers, Sound Manager, QuickTime Music,
File/Resource/Memory Managers, threads, and more) is reimplemented natively. The
game data and scripts are used as they are. No Apple ROM or System software is
needed. Details: [docs/ANALYSIS.md](docs/ANALYSIS.md) and [docs/PLAN.md](docs/PLAN.md).

The original game installer is included in [`orig/`](orig/): `Cythera.bin`
(MacBinary) and `Cythera_Installer.sit` (StuffIt), which contain the same
installer.

### About the original game

Cythera is © 1999 Ambrosia Software and its authors. This repository
includes the original game files, on these grounds: Ambrosia Software no
longer exists, the hardware and operating system the game required are long
gone, and the game can no longer be bought, nor its authors supported
financially.

Additionally, the former president of Ambrosia SW released himself an app that 
generates valid keys for all Ambrosia products, including Cythera, which we take
as a clear sign of legal permission.
See: [Decoder Ring](https://macintoshgarden.org/games/decoder-ring).

No harm to anyone's intellectual property is intended. This is a fan and
hobby project, with no commercial purpose, made to keep a beloved game
playable.

## Building

Requirements: a C11 compiler, `make`, SDL2 development files, Python 3.

```sh
# macOS
brew install sdl2 unar
# Debian/Ubuntu
sudo apt install build-essential libsdl2-dev unar python3-venv

make
```

Optional but recommended: FreeType, for crisp (hinted) text. It is used when
`pkg-config freetype2` finds it (`brew install freetype`, `apt install
libfreetype-dev`, `pacman -S mingw-w64-x86_64-freetype`), or build a minimal
static copy from the [source tarball](https://download.savannah.gnu.org/releases/freetype/)
with `tools/build_freetype.sh freetype-2.x.tar.xz`. Without it, text is rasterised
unhinted and looks rough at small sizes.

Windows: build in an [MSYS2](https://www.msys2.org/) MinGW64 shell
(`pacman -S make mingw-w64-x86_64-gcc mingw-w64-x86_64-SDL2 python`), then `make`.
With a standalone MinGW-w64 instead (native `mingw32-make`, run from Git Bash),
put its `bin` first in `PATH` and pass the SDL flags as Windows paths, e.g.
`mingw32-make CC=gcc PYTHON=python SDL_CFLAGS=-IC:/mingw64/include/SDL2
SDL_LIBS="-LC:/mingw64/lib -lmingw32 -lSDL2"`.
The data setup script needs a Unix shell with `unar` (WSL works).

## Installing the game data

```sh
tools/setup_gamedata.sh                     # orig/Cythera.bin -> gamedata/
tools/setup_gamedata.sh orig/Cythera_Installer.sit gamedata
```

This unpacks the installer from the MacBinary file (no extra tools needed) or
from the StuffIt archive (needs `unar`), then extracts the Installer VISE
package (using [installer-vise](https://github.com/mrmidi/installer-vise)).
Set `PYTHON` if Python 3 isn't `python3` (on Windows, `PYTHON=python`).
It also downloads the free GeneralUser GS SoundFont, which is used to play the
game's QuickTime music.

## Prebuilt packages

The GitHub Actions workflow ([.github/workflows/build.yml](.github/workflows/build.yml))
builds ready-to-play packages, game included, on every push (as artifacts of the
workflow run); pushing a `v*` tag publishes them as a release.

- **Windows** (`.zip`): extract anywhere and run `Cythera.exe`. It isn't signed, so
  SmartScreen may warn: *More info* › *Run anyway*.
- **macOS** (`.dmg`, Apple Silicon and Intel, macOS 11 or later): drag Cythera to
  Applications. It isn't notarized, so the first time macOS refuses to open it;
  then go to System Settings › Privacy & Security and click *Open Anyway* (or run
  `xattr -dr com.apple.quarantine /Applications/Cythera.app`). With an Apple
  Developer ID, the workflow signs and notarizes it instead (see the secrets
  listed at the top of the workflow).
- **Linux** (`.tar.gz`): needs SDL2 (`libsdl2-2.0-0`); run `./cythera`.

In a package the game folder sits next to the program and is never written to;
preferences and saved games go to `~/.cythera-port/` as usual. To build the same
way locally on Windows, add `GUI=1 LDFLAGS=-static` and static SDL libraries.

## Running

```sh
./build/cythera
```

Useful options (`./build/cythera --help` lists them all):

| Option | Meaning |
|---|---|
| `--data DIR` | game directory (default `gamedata`) |
| `--scale N` | window scale factor (default: as large as fits) |
| `--fullscreen` | start fullscreen |
| `--screen WxH` | emulated screen size (default 640x480, as the game was designed for) |
| `--soundfont FILE` | General MIDI SoundFont for the music |
| `--app NAME` | run another application from the game folder |
| `-v` / `-q` | more / less logging |

Preferences and saved games are kept in `~/.cythera-port/`
(`System Folder/Preferences`, `Saved Games`).

### Controls

The same as the original. The mouse does most things, and arrow keys or the
keypad move. The ⌘ key is the host's Command key (Ctrl on Linux/Windows);
Option is Alt.

- The **menu bar is hidden**, as in the original. Click the top edge of the
  screen, where the menu bar would be, to open the File menu (save, load,
  preferences, quit).
- **Hold the mouse button** on an object or character to get the contextual menu.
- **Drag** items onto your portrait in the party roster (bottom right) to pick them up.
- **Alt+Enter** (or **Ctrl+⌘+F** on macOS) toggles fullscreen. The window can be
  resized freely; the picture is scaled by whole multiples to keep pixels sharp.

## Registration

Cythera was shareware, and registration codes can no longer be bought, so by
default the port plays the registered game: it stands in for the game's
licence check (nothing is written to disk). The title screen shows
"Registered To: Cythera Port"; choose the name with `--registered "Your Name"`.

To run the original code entirely unaltered, build with `make LICENSE_BYPASS=0`
(after `make clean`, or in another `BUILD=` directory). The game is then
unregistered unless you enter a code with Ambrosia's registration
application, included with the game, which runs under the port as well:

```sh
./build/cythera --app "Register Cythera"
```

Choose *Enter License Code* and enter your name, number of copies and code
exactly as you received them. Pasting from the host clipboard works (⌘V). The
license is saved in `~/.cythera-port/System Folder/Preferences`, where the
game finds it.

## Developer tools

- `make test` runs the PowerPC interpreter's unit tests; `tests/run.sh [SCRIPT...]`
  replays the scripted scenarios (hermetic: it never touches `~/.cythera-port`).
- `--headless --script FILE` runs without a window and replays scripted input
  (`wait`, `click`, `move`, `key`, `hold`, `type`, `shot file.png`, `bt`,
  `dumpwin`, `trace on|off`, `expect TEXT`, `quit`). See `tests/scripts/`. A first line
  `# app: NAME` makes `tests/run.sh` run another application.
- `--turbo N` runs the emulated clock N× faster (for tests); `--timeout N` sets
  a time limit.
- `--trace-traps` logs every Toolbox call with symbolized callers, and
  `--profile` prints Toolbox and guest-function hot spots.
- `CYTHERA_WATCH=off,...` logs the registers each time execution reaches the given
  code offsets (hex, as printed by `tools/ppcdis.py`): a cheap breakpoint.
- `--wav FILE` records the audio output; `--render-pict FILE OUT.png` renders a
  PICT file.
- `tools/ppcdis.py` is a symbolizing PowerPC disassembler for the game binary
  (function names come from the CodeWarrior traceback tables);
  `tools/rsrc.py` and `tools/pef.py` parse resource forks and PEF containers.
- `tools/delv_props.py` lists the game's maps and the objects on each (positions,
  names, containers), using [delvmod](https://github.com/BryceSchroeder/delvmod)
  cloned into `work/delvmod`. Handy for writing scripted play-throughs.

## License

The port's own code is released as public domain. No copyright claims are 
made on it or the tools used to create this port.

The original game, included, is © 1999 Ambrosia Software and originaly 
released as shareware with a 30 days trial period.

Third-party single-header libraries in `third_party/`: stb_truetype and
stb_image_write (public domain), font8x8 (public domain), TinySoundFont (MIT),
minicoro (MIT/Unlicense). The Delver file-format knowledge comes from delvmod
(Bryce Schroeder) and the DelvTechWiki.

## Cythera Credits

```
•Glenn Andreas
Delver Engine
Cythera Scenario
Cythera Artwork

•Andrew Welch
SoundTool
RegistrationTool
MonitorTool
Cythera Sounds

•Marcus Conge
Cythera Artwork

•Randy Pringle
Cythera Music

•Alpha Testers
Troy Baumgarten
Nathan Fleming
Dave (Mercutio) Fried
Jesse "Hybrid" Liesch
Etienne Pelaprat
Collin "Mr. Splat" Petty
Trevor Powell
Jake Wallace
Dan "Well Jigger My Biscuits" Wood

•Additional Alpha Testers
Chris "TheDew" DeWan
Ryan "Zarathustra" Fritsch
Brian P. McCarty
Jon M-L Reisenweaver
Ked "Amacus" Shayer
Jason "Alpha Corpse Abuser" Whong
Ted Woodward

•Beta Testers
Carlos Andrade
Mary Cook
Ryan Fritsch
Marc Khadpe
William MacKay
Brian P. McCarty
Will Oram
Austin Parker
Alex Piltch
R. Dwight Porcher
Jon Reisenweaver
Dan Schimpf
David Simon
Ben Spees
Stephen Smithwick
Eric B. Venet
Greg Weston
Ted Woodward
Trevor Zylstra

•Logistics & Support
David Dunham
Jason Whong

•Special Thanks
Sam Wang - Argos Font
The Source of Chaos
Caribou Coffee (Blue Crayon)

•Quotes
"I want to catch rats!"
What do you expect from such a "fun guy"?
El Nino did it...
WWBBD?

•In Memorial
Gene Leonard (1951-1997)
```
