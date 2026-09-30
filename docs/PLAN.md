# Porting Plan

## Goal

Make Cythera (Ambrosia Software, 1999) playable again on modern systems, with a
1:1 feature match with the original, from nothing but the compiled binary and
its data (no source, no specification) plus earlier reverse-engineering work
on the data formats ([delvmod](https://github.com/BryceSchroeder/delvmod)):

- reverse engineer the game's data and code;
- keep as much of it intact as possible and replace only the system calls,
  rather than recoding everything;
- use a portable language and library (C and SDL2); an offline data
  conversion step is allowed if needed.

Strategy ([ANALYSIS.md §4](ANALYSIS.md#4-porting-strategy)): run the original
PowerPC engine code on a PPC interpreter and replace the Mac OS Toolbox with a
native C/SDL2 implementation (high-level emulation). Game scripts and data stay
untouched.

Milestones are ordered so that each ends with something runnable and testable.
Status markers: `[ ]` todo, `[~]` in progress, `[x]` done.

## M0 — Analysis & tooling
- [x] Extract the installer (`orig/`: MacBinary or StuffIt) → Installer VISE → game files
- [x] Resource fork parser (`tools/rsrc.py`), PEF parser (`tools/pef.py`)
- [x] Symbolizing PPC disassembler with traceback names (`tools/ppcdis.py`)
- [x] Technical analysis, strategy decision
- [x] `tools/setup_gamedata.sh`: reproducible extraction into `gamedata/`
- [x] Archive dumper for `Cythera Data` (decrypt, list, export) for debugging (`tools/delv_archive.py`)

## M1 — Runtime skeleton
- [x] Build system (Makefile + pkg-config SDL2), `src/` layout
- [x] Guest memory (flat BE buffer, low-mem area, allocator for OS structures)
- [x] PEF loader: sections, pidata unpack, relocations, import binding to trap TVectors
- [x] PPC interpreter: integer, branch, CR, load/store (incl. lmw/stmw/string), FPU
- [x] Trap dispatch + `guest_call` for callbacks; unimplemented-trap logging with symbolized backtraces
- [x] CPU unit tests (`make test`)
- **Exit:** `main` runs until the first unimplemented Toolbox call, with a clean trace.

## M2 — Core OS services (get through startup)
- [x] Memory Manager: pointers, handles (master pointers, lock/purge/resource flags, SetHandleSize), zones, stats
- [x] Resource Manager: open app/scenario/prefs forks, resource chain, Get/Get1/GetNamed/Count/Ind, Release/Detach, Add/Write/Changed/Update
- [x] File Manager: FSSpec, open DF/RF, read/write/seek/EOF, create/delete/exchange, PBGetCatInfo, volumes, FindFolder, aliases (minimal)
- [x] Gestalt, Process Manager, Time Manager, TickCount, Random (exact QD algorithm), misc stubs
- [x] Thread Manager (coroutines via minicoro; host threads were far too slow)
- **Exit:** the app reaches event-loop setup (menus/windows creation).

## M3 — Graphics & windows (first pixels)
- [x] SDL host: 8-bit screen, palette, scaling, cursor, event pump
- [x] QuickDraw core: GrafPort/CGrafPort/GDevice/PixMap structs, ports, colours, pen, clip/vis
- [x] Regions (real Mac region format + full algebra), rects, points
- [x] CopyBits/CopyMask/CopyDeepMask (modes, masks, scaling, colour mapping), GWorlds
- [x] Shapes: rect/roundrect/oval/line/poly/region paint/frame/fill/erase/invert, patterns, ppat
- [x] Text: NFNT/FOND bitmap fonts, TrueType (stb_truetype), styles, widths, font substitution
- [x] PICT v1/v2 decoding (DrawPicture), cicn/crsr/icons
- [x] Window Manager (window list, custom WDEF via PPC callbacks, a standard WDEF, update/activate regions)
- [x] Menu Manager (menu bar, pulldowns, MenuSelect/MenuKey, popups)
- [x] Event Manager (queue, WaitNextEvent, keys/KeyMap, mouse, autoKey, update/activate generation)
- **Exit:** splash screen / title screen visible in a headless PNG screenshot.

## M4 — Dialogs & controls (menus, new game)
- [x] Control Manager (standard + app CDEFs), Dialog Manager (DLOG/DITL/ALRT, ModalDialog, filters, TE items)
- [x] List Manager (ListRec layout, app LDEF via refCon proc)
- [x] TextEdit (TERec, styled text, TETextBox, keyboard editing)
- [x] StandardFile get/put dialogs (Navigation Services absent)
- **Exit:** create a new character and enter the game world.

## M5 — Gameplay
- [~] Main map view, roster, text log, character/inventory windows work
- [x] Keyboard/mouse movement, take/use/talk, conversations, journal, to-do (movement, contextual menu, conversations verified)
- [x] Travel between maps, combat and death, sleeping, shops (buying, haggling), potions (verified by scripts)
- [ ] Magic spells: need training at the Magisterium in Pnyx (not reached by a script yet)
- [x] Save / load games (verified); [x] preferences dialog
- [x] Timing correctness (ticks, animation, heartbeat)
- **Exit:** play through the opening of the game (Odemia) with save/load. (Odemia's gate is
  reached; the town stays closed until Ariadne is rescued from the bandits at the Abandoned Farmhouse.)

## M6 — Audio
- [x] Sound Manager (snd resources, SndDoCommand/Immediate, SndPlay, double buffer, callbacks)
- [x] QuickTime Music: tune header/sequence parsing, note allocator (NAPlayNote), GM synth
- [x] Ambient/spot sounds, volume prefs
- **Exit:** music and sound effects play correctly.

## M7 — Completeness & polish
- [x] Sweep every imported call for correctness; replace all stubs used at runtime
  (inventory and findings: [TOOLBOX.md](TOOLBOX.md))
- [x] Slideshows, cutscenes, end-game, credits (intro slideshow, death cutscene and game over,
  scrolling credits in About; the winning ending uses the same calls, see the 2026-09-30 log)
- [x] Window scaling, fullscreen, HiDPI, configurable screen size
- [x] Performance: not needed (the switch interpreter runs ~80 M guest instructions/s; play
  needs ~13-20 M/s, see the 2026-09-30 log)
- [x] Portability: Linux, Windows (MinGW-w64/MSYS2) and macOS (universal) builds and packages
  in GitHub Actions ([build.yml](../.github/workflows/build.yml)), which also runs the CPU and
  scripted tests on Linux; no host-endianness assumptions (guest memory is accessed through
  rd/wr helpers). Rules learnt on the way: [DEVELOPING.md](DEVELOPING.md#portability-rules).
- [x] Documentation: README (players), DEVELOPING, ANALYSIS, TOOLBOX, this plan
- **Exit:** full-feature playable port.

## M8 — Save editor (tests and testers start at any stage)
- [x] Phase 0: the saved-game format ([ANALYSIS.md §3.1](ANALYSIS.md#31-saved-games))
- [x] Phase 1: `tools/delv_save.py`: show, byte-identical round trip, safe edits (position
  within and across zones, stats, HP/MP, skills, gold and items, clock), each loaded in the
  game (`tests/scripts/saveedit*.txt`); story flags listed read-only
- [ ] Phase 2: story flag edits (what each QF/QV means; quest to-dos, journal)
- [ ] Phase 3: stage saves (after the test, Ariadne rescued, Magisterium...) for the scripts
- **Exit:** a gameplay script can start from a saved stage instead of replaying the opening.

## Working notes
- Commit and push after each significant chunk.
- Headless screenshot tests are the main autonomous verification tool.

## Progress log
- 2026-09-30 (save editor): saved games decoded ([ANALYSIS.md §3.1](ANALYSIS.md#31-saved-games)):
  the 0400 stream (`Char`: karma, difficulty, the 32 story values QV and 256 story flags QF, clock
  and day, play time; active monsters, spell effects, windows), the character table F009 (every
  byte the VM's Character fields reach), the zone props with the character slots (F306), the
  to-do list, the heap. No checksum. Characters' belongings live in the current zone's prop
  list, so moving to another zone replays `GoToLocation`'s bookkeeping (carry, renumber, cue
  characters): checked against a real zone change in play (identical but for what the scripts
  did) and by loading moved saves (Omen's Test -> LandKing Hall, -> the World at night,
  Catamarca -> World, back into a zone stored in the save). `tools/delv_save.py` shows and edits;
  `run.sh` applies a script's `# save-edit:` line to the fixture first. Story flags have no names
  in the data; scripts using each are listed (QF 0 = first audience with Alaric). Also:
  `delv_archive.py` labels saves' pages (04 is the game state there) and no longer fails on
  consoles without Mac Roman glyphs.
- 2026-09-30 (release, CI, crash): GitHub Actions build, test and package the game for Linux
  (tarball), Windows (static zip, no console) and macOS (universal app in a dmg, ad-hoc signed;
  Developer ID signing and notarization when secrets are set); v* tags publish a release
  (v0.1.1). Packaged builds find gamedata next to the program and never write into it. A
  windowed-only startup crash (1 in 6 to 5 in 8 starts) is fixed: TuneGetStatus reported
  tune = 0 with a stale tunePtr after a stop, GMSTune::Pause saves (tunePtr - tune) / 4 as its
  resume point, so two pauses in a row (focus changes) made Resume queue a wild address and
  the audio thread read past guest memory. The audio thread now never calls fatal().
  Performance, measured: ~80 M guest instructions/s (deterministic runs); real-time play
  averages ~13 M/s including loading (1.1 G in 87 s), so no instruction cache or JIT is needed.
- 2026-09-30 (gameplay): new scripts play through what was left unverified: `travel` (Omen's Test ->
  LandKing Hall -> World -> Odemia's gate), `sleep` (the bed in the hero's quarters; "It is already
  morning" afterwards), `potion` (Far Sight potion: the overhead map), `combat` (the bandits at the
  Abandoned Farmhouse kill the hero: death cutscene, then GameOver), `shop` (40 oboloi from the
  quarters, walk to Catamarca, buy bread from Parium with a round of haggling). Bugs found and fixed:
  - (also found here, fixed by the Toolbox sweep: StdCLib's PL string functions, which left the
    player's name empty everywhere, e.g. "A ruffian missed .", and the CDEF 63 pop-up.)
  - SeedFill overflowed its stack buffer (heap corruption, a crash in LandKing Hall's pool room).
  - CopyBits clipped to the current port even when drawing elsewhere: cutscene work areas below
    row 480 of their 544x544 GWorld were lost (a white box under the death cutscene picture).
  - TrackControl jumped to address -1 for controls whose action is -1 (autoTrack: the CDEF
    tracks itself), a crash on the shop dialog's quantity arrows.
  Monsters act as turns pass, so the combat script steps back and forth until the hero dies; a
  key (not a click) ends the death slideshow, because GameOver ends as soon as the button is down.
  Monster eggs (props with flags 0x42 holding a monster) hatch with probability d2 % when the
  hero comes near (TActiveMonster::HatchEgg; d1 bits restrict to day or night): the Omen's Test
  maze eggs have d2 = 0, hence no monsters there.
  Not reached: spells (Mana/Casting need training at the Magisterium in Pnyx; scrolls are learned
  with a grimoire only after that), selling (no merchant who buys is reachable before Odemia opens),
  the winning ending. The endings are VM script: Alaric's Talk (0x1802) after he is cured runs
  SpecialView, BeginSlideshow, Slideshow(512, 0x0243[i]), EndSlideshow and GameOver("You have saved
  the land..."), the same calls as the death sequence (0x1801 OnDeath, texts 0x0241), which the
  combat script exercises; the bad ending (0x180d, Pelagon) is the same again. Reaching them
  needs the whole plot, and faking the state would not test anything the death path doesn't.
- 2026-09-30 (archive dumper): `tools/delv_archive.py` lists, decrypts and exports the
  Delver archive with no dependencies (PNG, WAV, map renders, prop tables, VM-object dumps).
  It matches delvmod on all 1,558 resources. New findings (ANALYSIS §3): the resource
  ID's high byte is the TOC page, encryption is chosen by the caller (the VM always decrypts),
  `asnd` rates are Fixed, `8EFF` is a sized image, and the scenario `clut` differs
  from delvmod's palette in 4 entries.
- 2026-09-30 (audio): ambient, spot and positional sounds are the game's own SoundTool mixer;
  the Toolbox side is one double-buffered channel and the output volume (how it works:
  [ANALYSIS.md §2.4](ANALYSIS.md#24-audio)). Checked with WAV captures (volumes, panning,
  a moving ambient loop). Fixes: the output volume has left/right levels and scales the music
  as on the Mac (so music is quieter at lower effects volumes); `TuneSetVolume` sets only that
  tune; tune players' synth channels no longer overlap or overrun their arrays; linear
  resampling; `SndGetInfo`/`SndChannelStatus` answers. `tests/scripts/volume.txt`.
- 2026-09-30 (timing): ticks were right (the model: [ANALYSIS.md §2.5](ANALYSIS.md#25-timing)); what
  surrounds them was fixed: `WaitNextEvent` sleeps the requested time, autoKey comes from
  KeyThresh/KeyRepThresh rather than host repeats, input is stamped when it happens,
  DoubleTime/CaretTime are the factory values, `GetDateTime` follows the emulated clock. No
  more vsync (it blocked the emulated CPU) and one present per tick. Busy-wait detection covers
  status-polling loops: the intro went from 44 s to 3.4 s of CPU, in-game idle from 21% to 13%
  of a core, and the scripted suite from ~10 to ~3 minutes.
- 2026-09-30 (Toolbox sweep): [TOOLBOX.md](TOOLBOX.md) classifies all 563 imports and lists the
  findings. The main ones: StdCLib's string functions were missing (empty dialog labels, file
  names, player name), a crash opening a game from within a game, the missing pop-up control
  (CDEF 63), InitZone, save previews. New: `--trap-stats`, `ppcdis.py xref`.
- 2026-09-29 (CPU use): gamma fades spun millions of times per second because the video
  driver's cscSetGamma returned at once; it now waits for the next vertical blank like the
  hardware (fades run at 60 steps/s). TickCount busy-waits sleep until the next tick. A
  minute of startup + loading went from 33 s to 11 s of CPU; idle play costs ~27% of a core,
  spent in the game's own map renderer (lighting, dithering).
- 2026-09-29 (status): verified by scripts: quitting (save prompt, ExitToShell), the tutorial
  mechanics listed below, the Register app. Not yet verified, because it's out of reach of cheap
  scripted play: combat, sleeping, shops, travel between maps, magic. The Omen's Test crate
  puzzle is solved by the tutorial script; the spiral maze's egg spawners didn't produce
  monsters when walked past (probably conditional). All InterfaceLib imports are
  implemented; the missing ones (Navigation Services, InputSprocket, Contextual Menu Manager,
  Control Strip) are weak imports of libraries the game treats as absent.
- 2026-09-29 (tests): CPU unit tests (`make test`, ~37.8k checks against reference models)
  found a real bug: `sraw`/`srawi` computed XER[CA] from the result when rA == rS, which
  broke CodeWarrior's signed divide-by-power-of-two idiom (`srawi` + `addze`: -8/4 gave -1).
  The scripted test runner is now hermetic (temporary System Folder and Saved Games, built
  from `tests/fixture.txt`) and scripts can assert on-screen text with `expect`.
- 2026-09-29 (tutorial): Omen's Test plays through, scripted: reading notes, lever and gate,
  secret door, containers, taking items (dropped on the roster portrait), key on a lock,
  ladder, lighting and throwing a bomb (turn-based fuse), sliding crates. `CYTHERA_WATCH`
  watchpoints and `tools/delv_props.py` make it practical to follow game logic.
- 2026-09-29 (pictures): QuickDraw picture recording (OpenPicture) now records text,
  lines and rects. In-game notes and books (TWScrollText) record their text into a
  picture, so they were blank before. Shape drawing honors a hidden pen, as in real
  QuickDraw. Script command `trace on|off` limits trap tracing to part of a run.
- 2026-09-29 (registration): the separate "Register Cythera" PPC app runs unmodified
  (`--app "Register Cythera"`) and validates codes. Added the Scrap Manager
  (synced with the host clipboard) and fixed thin TrueType stems vanishing
  at small sizes.
- 2026-09-29 (later): in-game play works (movement, lighting, contextual menus, conversations,
  save/load, hidden menu bar, music via QTMA+SoundFont, sound effects). Deterministic test mode added.
- 2026-09-29: M0-M4 done. Title, main menu, character creation, intro slideshow and
  the first in-game conversation run. Notable findings:
  - The game needs QuickTime 3 and Apple Events ('evnt') to start.
  - App WDEFs read `portBits.bounds.topLeft` at port+8/+10 even for color windows, so
    window ports mirror it there (grafVars kept host-side).
  - Fonts: the Seldane NFNT has a truncated owTable; ArgosANouveau is a TrueType font
    with only a Mac Roman cmap (vendored stb_truetype patched to accept it).
  - A video driver (refnum -50) with gamma-table calls is emulated (fades work).
  - Thread switching via OS threads + condvars was catastrophically slow; switched to
    coroutines.
