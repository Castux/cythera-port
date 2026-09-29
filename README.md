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

You need your own copy of the game (`Cythera_Installer.sit`). Nothing
copyrighted is part of this repository.

## Building

Requirements: a C11 compiler, `make`, SDL2 development files, Python 3.

```sh
# macOS
brew install sdl2 unar
# Debian/Ubuntu
sudo apt install build-essential libsdl2-dev unar python3-venv

make
```

## Installing the game data

```sh
tools/setup_gamedata.sh /path/to/Cythera_Installer.sit gamedata
```

This unpacks the StuffIt archive, then extracts the Installer VISE package
inside it (using [installer-vise](https://github.com/mrmidi/installer-vise)).
It also downloads the free GeneralUser GS SoundFont, which is used to play the
game's QuickTime music.

## Running

```sh
./build/cythera
```

Useful options (`./build/cythera --help` lists them all):

| Option | Meaning |
|---|---|
| `--data DIR` | game directory (default `gamedata`) |
| `--scale N` | window scale factor (default: as large as fits) |
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

## Registration

Cythera was shareware. Ambrosia's registration application, included with the
game, runs under the port as well:

```sh
./build/cythera --app "Register Cythera"
```

Choose *Enter License Code* and enter your name, number of copies and code
exactly as you received them. Pasting from the host clipboard works (⌘V). The
license is saved in `~/.cythera-port/System Folder/Preferences`, where the
game finds it.

## Developer tools

- `--headless --script FILE` runs without a window and replays scripted input
  (`wait`, `click`, `move`, `key`, `hold`, `type`, `shot file.png`, `bt`,
  `dumpwin`, `trace on|off`, `quit`). See `tests/scripts/`. A first line
  `# app: NAME` makes `tests/run.sh` run another application.
- `--turbo N` runs the emulated clock N× faster (for tests); `--timeout N` sets
  a time limit.
- `--trace-traps` logs every Toolbox call with symbolized callers, and
  `--profile` prints Toolbox and guest-function hot spots.
- `--wav FILE` records the audio output; `--render-pict FILE OUT.png` renders a
  PICT file.
- `tools/ppcdis.py` is a symbolizing PowerPC disassembler for the game binary
  (function names come from the CodeWarrior traceback tables);
  `tools/rsrc.py` and `tools/pef.py` parse resource forks and PEF containers.

## License / credits

The port's own code is released for private use of legitimately owned copies.
Third-party single-header libraries in `third_party/`: stb_truetype and
stb_image_write (public domain), font8x8 (public domain), TinySoundFont (MIT),
minicoro (MIT/Unlicense). Cythera and Delver are trademarks of their respective
owners. The Delver file-format knowledge comes from delvmod
(Bryce Schroeder) and the DelvTechWiki.
