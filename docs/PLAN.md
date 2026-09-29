# Porting Plan

Strategy (see [ANALYSIS.md §4](ANALYSIS.md#4-porting-strategy-evaluation)): run the original
PowerPC engine code on a PPC interpreter and replace the Mac OS Toolbox with a
native C/SDL2 implementation (high-level emulation). Game scripts and data stay
untouched.

Milestones are ordered so that each ends with something runnable and testable.
Status markers: `[ ]` todo, `[~]` in progress, `[x]` done.

## M0 — Analysis & tooling
- [x] Extract `.sit` → Installer VISE → game files (unar + installer-vise)
- [x] Resource fork parser (`tools/rsrc.py`), PEF parser (`tools/pef.py`)
- [x] Symbolizing PPC disassembler with traceback names (`tools/ppcdis.py`)
- [x] Technical analysis, strategy decision
- [x] `tools/setup_gamedata.sh`: reproducible extraction into `gamedata/`
- [ ] Archive dumper for `Cythera Data` (decrypt, list, export) for debugging

## M1 — Runtime skeleton
- [x] Build system (Makefile + pkg-config SDL2), `src/` layout
- [x] Guest memory (flat BE buffer, low-mem area, allocator for OS structures)
- [x] PEF loader: sections, pidata unpack, relocations, import binding to trap TVectors
- [x] PPC interpreter: integer, branch, CR, load/store (incl. lmw/stmw/string), FPU
- [x] Trap dispatch + `guest_call` for callbacks; unimplemented-trap logging with symbolized backtraces
- [ ] CPU unit tests (deferred; the game itself exercises the CPU heavily)
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
- [~] Keyboard/mouse movement, take/use/talk, conversations, journal, to-do (movement, contextual menu, conversations verified)
- [x] Save / load games (verified); [ ] preferences dialog
- [ ] Timing correctness (ticks, animation, heartbeat)
- **Exit:** play through the opening of the game (Odemia) with save/load.

## M6 — Audio
- [x] Sound Manager (snd resources, SndDoCommand/Immediate, SndPlay, double buffer, callbacks)
- [x] QuickTime Music: tune header/sequence parsing, note allocator (NAPlayNote), GM synth
- [ ] Ambient/spot sounds, volume prefs
- **Exit:** music and sound effects play correctly.

## M7 — Completeness & polish
- [ ] Sweep every imported call for correctness; replace all stubs used at runtime
- [ ] Slideshows, cutscenes, end-game, credits
- [ ] Window scaling, fullscreen, HiDPI, configurable screen size
- [ ] Performance (predecoded instruction cache or block JIT if needed)
- [ ] Portability: Linux/Windows builds (CI-free manual check), no host-endianness assumptions
  (early Linux findings and a required fix: [LINUX_NOTES.md](LINUX_NOTES.md))
- [x] User documentation (README: setup, controls, config)
- **Exit:** full-feature playable port.

## Working notes
- Game data never goes into git. `gamedata/` and `work/` are ignored.
- Commit and push after each significant chunk.
- Headless screenshot tests are the main autonomous verification tool.

## Progress log
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
