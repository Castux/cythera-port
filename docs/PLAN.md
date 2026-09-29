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
- [ ] `tools/setup_gamedata.sh`: reproducible extraction into `gamedata/`
- [ ] Archive dumper for `Cythera Data` (decrypt, list, export) for debugging

## M1 — Runtime skeleton
- [ ] Build system (Makefile + pkg-config SDL2), `src/` layout
- [ ] Guest memory (flat BE buffer, low-mem area, allocator for OS structures)
- [ ] PEF loader: sections, pidata unpack, relocations, import binding to trap TVectors
- [ ] PPC interpreter: integer, branch, CR, load/store (incl. lmw/stmw/string), FPU
- [ ] Trap dispatch + `guest_call` for callbacks; unimplemented-trap logging with symbolized backtraces
- [ ] CPU unit tests
- **Exit:** `main` runs until the first unimplemented Toolbox call, with a clean trace.

## M2 — Core OS services (get through startup)
- [ ] Memory Manager: pointers, handles (master pointers, lock/purge/resource flags, SetHandleSize), zones, stats
- [ ] Resource Manager: open app/scenario/prefs forks, resource chain, Get/Get1/GetNamed/Count/Ind, Release/Detach, Add/Write/Changed/Update
- [ ] File Manager: FSSpec, open DF/RF, read/write/seek/EOF, create/delete/exchange, PBGetCatInfo, volumes, FindFolder, aliases (minimal)
- [ ] Gestalt, Process Manager, Time Manager, TickCount, Random (exact QD algorithm), misc stubs
- [ ] Thread Manager (host threads + baton)
- **Exit:** the app reaches event-loop setup (menus/windows creation).

## M3 — Graphics & windows (first pixels)
- [ ] SDL host: 8-bit screen, palette, scaling, cursor, event pump
- [ ] QuickDraw core: GrafPort/CGrafPort/GDevice/PixMap structs, ports, colours, pen, clip/vis
- [ ] Regions (real Mac region format + full algebra), rects, points
- [ ] CopyBits/CopyMask/CopyDeepMask (modes, masks, scaling, colour mapping), GWorlds
- [ ] Shapes: rect/roundrect/oval/line/poly/region paint/frame/fill/erase/invert, patterns, ppat
- [ ] Text: NFNT/FOND bitmap fonts, TrueType (stb_truetype), styles, widths, font substitution
- [ ] PICT v1/v2 decoding (DrawPicture), cicn/crsr/icons
- [ ] Window Manager (window list, custom WDEF via PPC callbacks, a standard WDEF, update/activate regions)
- [ ] Menu Manager (menu bar, pulldowns, MenuSelect/MenuKey, popups)
- [ ] Event Manager (queue, WaitNextEvent, keys/KeyMap, mouse, autoKey, update/activate generation)
- **Exit:** splash screen / title screen visible in a headless PNG screenshot.

## M4 — Dialogs & controls (menus, new game)
- [ ] Control Manager (standard + app CDEFs), Dialog Manager (DLOG/DITL/ALRT, ModalDialog, filters, TE items)
- [ ] List Manager (ListRec layout, app LDEF via refCon proc)
- [ ] TextEdit (TERec, styled text, TETextBox, keyboard editing)
- [ ] StandardFile get/put dialogs (Navigation Services absent)
- **Exit:** create a new character and enter the game world.

## M5 — Gameplay
- [ ] Main map view, roster, text log, character/inventory windows work
- [ ] Keyboard/mouse movement, take/use/talk, conversations, journal, to-do
- [ ] Save / load games, preferences
- [ ] Timing correctness (ticks, animation, heartbeat)
- **Exit:** play through the opening of the game (Odemia) with save/load.

## M6 — Audio
- [ ] Sound Manager (snd resources, SndDoCommand/Immediate, SndPlay, double buffer, callbacks)
- [ ] QuickTime Music: tune header/sequence parsing, note allocator (NAPlayNote), GM synth
- [ ] Ambient/spot sounds, volume prefs
- **Exit:** music and sound effects play correctly.

## M7 — Completeness & polish
- [ ] Sweep every imported call for correctness; replace all stubs used at runtime
- [ ] Slideshows, cutscenes, end-game, credits
- [ ] Window scaling, fullscreen, HiDPI, configurable screen size
- [ ] Performance (predecoded instruction cache or block JIT if needed)
- [ ] Portability: Linux/Windows builds (CI-free manual check), no host-endianness assumptions
- [ ] User documentation (README: setup, controls, config)
- **Exit:** full-feature playable port.

## Working notes
- Game data never goes into git. `gamedata/` and `work/` are ignored.
- Commit and push after each significant chunk.
- Headless screenshot tests are the main autonomous verification tool.
