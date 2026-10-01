# Cythera for modern systems

This is a port of **Cythera** (Ambrosia Software, 1999), the classic Mac RPG built
on Glenn Andreas's Delver engine, to current operating systems (macOS, Linux,
Windows) using C and SDL2.

The original PowerPC game code runs **unmodified** on a built-in PowerPC
interpreter, and the classic Mac OS Toolbox it calls (QuickDraw, the Window,
Menu, Dialog, Control, List and TextEdit Managers, the Sound Manager, QuickTime
Music, files, resources, memory, threads...) is reimplemented natively. The
game's data and scripts are used as they are. No Apple ROM or System software
is needed. The original game installer is included in [`orig/`](orig/).

Documentation for developers: [docs/DEVELOPING.md](docs/DEVELOPING.md) (building,
testing, tools), [docs/ANALYSIS.md](docs/ANALYSIS.md) (how the original works),
[docs/TOOLBOX.md](docs/TOOLBOX.md) (the reimplemented calls) and
[docs/PLAN.md](docs/PLAN.md) (goals, status, history).

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

## Download and play

Ready-to-play packages, game included, are on the
[releases page](https://github.com/Castux/cythera-port/releases):

- **Windows** (`.zip`): extract anywhere and run `Cythera.exe`. It isn't signed, so
  SmartScreen may warn: *More info* › *Run anyway*.
- **macOS** (`.dmg`, Apple Silicon and Intel, macOS 11 or later): drag Cythera to
  Applications. It isn't notarized, so the first time macOS refuses to open it:
  go to System Settings › Privacy & Security and click *Open Anyway* (or run
  `xattr -dr com.apple.quarantine /Applications/Cythera.app`).
- **Linux** (`.tar.gz`): needs SDL2 (`libsdl2-2.0-0`); run `./cythera`.

Preferences and saved games are kept in `~/.cythera-port/` (on Windows,
`%APPDATA%\.cythera-port\`): `System Folder/Preferences` and `Saved Games`.

Every 10 minutes of play, the game in progress is also backed up, as with
File › Backup, into "*NAME* autosave 1", 2 and 3 in turn (the oldest is
replaced, never the game you have open). Your own saves are left alone: if
the game ever crashes, open the latest autosave from File › Open.

To build it yourself instead: install SDL2 and Python 3, run `make` and
`tools/setup_gamedata.sh`, then `./build/cythera`. Details, including Windows,
are in [docs/DEVELOPING.md](docs/DEVELOPING.md).

### Options

`cythera --help` lists them all. The useful ones:

| Option | Meaning |
|---|---|
| `--scale N` | window scale factor (default: as large as fits) |
| `--fullscreen` | start fullscreen |
| `--screen WxH` | a fixed emulated screen size (default 640x480, as the game was designed for) |
| `--registered NAME` | the name the game is registered to (see below) |
| `--data DIR` | game directory (default: `gamedata`, here or next to the program) |
| `--soundfont FILE` | General MIDI SoundFont for the music |
| `--autosave MIN` | back up the game in progress every MIN minutes of play (default 10; 0: off) |
| `--app NAME` | run another application from the game folder |

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
- **Ctrl+Alt+Enter** (**⌘+Option+Enter** on macOS) switches the display mode:
  - *Classic* (the default): the game's 640×480 screen, enlarged to fit the window.
  - *Large 2×* and *Large 1×*: the game gets a bigger screen, the size of the
    window (or the whole display, in fullscreen) at 2 or 1 screen pixels per game
    pixel, and follows the window as it's resized. Drag the map window's
    bottom-right corner to see more of the world.

  The game re-arranges its windows when the size changes, as it did on a Mac when
  the resolution changed. The display mode, window size and fullscreen are
  remembered in `~/.cythera-port/port.cfg`.

### Registration

Cythera was shareware, and registration codes can no longer be bought, so the
port plays the registered game: it stands in for the game's licence check
(nothing is written to disk). The title screen shows "Registered To: Cythera
Port"; choose the name with `--registered "Your Name"`.

To run the original code entirely unaltered, build with `make LICENSE_BYPASS=0`.
The game is then unregistered unless you enter a code in Ambrosia's
registration application, which runs under the port as well
(`cythera --app "Register Cythera"`, *Enter License Code*; pasting with ⌘V
works). The licence is saved in the port's `System Folder/Preferences`.

## License

The port's own code is released as public domain. No copyright claims are 
made on it or the tools used to create this port.

The original game, included, is © 1999 Ambrosia Software and originaly 
released as shareware with a 30 days trial period.

Third-party code in `third_party/`: FreeType (a subset, FreeType License),
TinySoundFont (MIT), minicoro (MIT/Unlicense), stb_image_write and font8x8
(public domain). Built-in fonts: ChicagoFLF (Robin Casady, public domain) and
DejaVu Sans (Bitstream Vera license: `third_party/fonts/LICENSE.DejaVu`,
`Font license.txt` in the packages). The Delver file-format knowledge comes from delvmod
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
