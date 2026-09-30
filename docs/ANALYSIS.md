# Cythera — Technical Analysis

Living document: the technical findings behind the port. The plan and progress
tracking are in [PLAN.md](PLAN.md).

## 1. The distribution archive

`Cythera_Installer.sit` is a **StuffIt 5** archive (Arsenic compression). It
holds one file, `Cythera Installer`, a classic Mac application with both a
resource fork and a data fork. `unar` (The Unarchiver) extracts it.

The installer's data fork (6.8 MB, magic `SVCT`, creator `VIS3`) is an
**Installer VISE 3.x** archive (MindVision). Its payload is "byte-substituted,
word-aligned DEFLATE". The open-source extractor
[mrmidi/installer-vise](https://github.com/mrmidi/installer-vise) handles it:
48 files, all CRC-verified. Resource forks come out as `<name>.rsrc` sidecar
files (raw resource-fork format).

Relevant payload:

| File | Type/Creator | Data fork | Rsrc fork | Contents |
|---|---|---|---|---|
| `Cythera` | APPL/Delv | 890,864 | 1,008,484 | The engine. **Fat app**: PPC PEF in data fork, 68k `CODE` in rsrc |
| `Cythera Data` | DelS/Delv | 5,608,688 | 1,247,331 | The scenario: Delver archive (data fork) + fonts, PICTs, lighting filters (rsrc) |
| `*.ai`, `AI Scripting Document` | TEXT | small | – | Combat AI scripts (text, compiled by the engine at runtime) |
| `Cythera Documentation` | APPL/Dk@P | 0 | 2.4 MB | DOCMaker manual |
| `Register Cythera` | APPL/Areg | – | – | Registration app (not needed) |
| `InputSprocket*`, `USBHID…` | shlb | | | Joystick drivers (not needed) |

The setup step (`tools/setup_gamedata.sh`) rebuilds this layout from the
user's `.sit` into `gamedata/`. Nothing copyrighted is committed.

## 2. Architecture of the original program

Cythera runs on the **Delver engine** by Glenn Andreas (1995–1999). There are
two layers:

1. **Engine** (`Cythera` app): C++ (Metrowerks CodeWarrior, MSL C++ runtime).
   It contains the bytecode VM, object heap, map renderer (lighting, LOS,
   roofs), pathfinding, monster movement, combat-AI compiler, UI windows,
   audio, the archive reader, and save/load.
2. **Scenario** (`Cythera Data`): almost all game *content and logic* is VM
   bytecode in the Delver archive. That covers item behaviour, conversations,
   quests, spells, skills, zone scripts, schedules and game mechanics
   (`GainExp`, `CastSpell`, ...).

So the scripts can stay untouched. Only the engine needs porting.

### 2.1 The PEF binary

`tools/pef.py` parses the container:

- Section 0: code, 0xCD280 bytes (840 KB)
- Section 1: data, pattern-initialised (pidata), 0x7934E bytes (496 KB, mostly BSS)
- Section 2: loader. `main` TVector is at data+0x3FA0. The TOC (r2) is data+0x8000.
- **563 imports** from 10 libraries:
  InterfaceLib 498 (strong), AppearanceLib 15, QuickTimeLib 17,
  InputSprocketLib 10, NavigationLib 9, ThreadsLib 7, ContextualMenu 3,
  ControlStripLib 2, MathLib 1 (`num2dec`), SoundLib 1.
  Everything except InterfaceLib is imported **weak**: the app checks for the
  library and falls back if it is missing.
- Only 2 of the 563 imports are never called directly.

**Symbols:** CodeWarrior left **traceback tables with mangled names** on every
function. `tools/ppcdis.py` recovers **1,981 named functions** plus **561
cross-TOC glue stubs** mapped to import names. Together they cover practically
all code; the unnamed gaps are the traceback tables themselves. Examples:
`TInterp::DoInterp`, `THeap::PerformGC`, `TGameViewer::CalcLighting`,
`TPathFinder::FindPath`, `SCombatAIEntry::CompileLine`,
`TSegFile::Decrypt`, `TConversation::ScanForHints`. This makes the binary
unusually easy to reverse engineer.

### 2.2 Engine class inventory (from symbols)

- **App / framework:** `TApp`, `TDelverApp`, `TWindow`, `TDialog`,
  `TDroppableWindow`, `TDrawerWindow`, custom window definitions (`TWDEF`,
  `TBorderWDEF`, `TDrawerWDEF`, `TPixsWDEF`, `TThinBorderWDEF`), custom
  controls (`TCDEF`, `TPushButtonCDEF`, `TScrollBarCDEF`, `TCheckButtonCDEF`,
  `TRadioButtonCDEF`, `TProgBarCDEF`, `TEditNumberCDEF`, ...), widget
  adapters (`TAppearanceAdapter` for Mac OS 8 Appearance, `T7Adapter` for
  System 7), `TListBox` (List Manager wrapper), `TPrefs`.
- **VM:** `TInterp` (`DoInterp`, `DoExpr`, `Dispatch`, `GetProperty`,
  `VAddrToPtr`), `VAddr`, `THeap` (GC, TOC, save/load), `THeapObj`,
  `THeapDict`, `THeapList`, `TGremlin` (per-prop script state), `TRegistry`.
  Exceptions: `XInterp`, `XIInvalidOpcode`, `XIInvalidVAddr`, `XIEndGame`.
- **World:** `TGameSys` (move/take/use/talk/attack commands, `HeartBeat`),
  `TViewer`/`TGameViewer` (rendering, `CalcLighting`, `LOS`, roofs,
  missiles, zone changes), `TMapWindow`, `TActiveMonster` (AI: roam,
  attack, retreat, follow), multi-tile monsters `TDragonMonster`,
  `TOctoMonster`, `TCrawlMonster`, `TPathFinder`, `THood` (neighbourhood
  activation), `TSpellFX` (timed effects), `TTaskMaster` (cooperative task
  scheduling on Thread Manager threads), `SCombatAIEntry` (compiles and runs
  the `.ai` text scripts).
- **UI:** `TStatusWindow` (roster, macros, F-keys), `TCharacterWindow`,
  `TInventoryWindow`, `TInventoryList`/`Pile`, `TConversation`, `TConvMode`,
  `TInteraction`, `TJournal`, `TToDo`, `TScriptedWindow` (VM-scripted
  windows with widgets `TWButton`, `TWText`, `TWInvent`, `TWAutoMap`, ...),
  `TBark` (speech balloons), `TCreatePlayerDialog`, `TRunStart`, slideshow
  and credits (`T*ImageObject`).
- **Data/IO:** `TSegFile`/`TCachedSegFiles` (Delver archive with TOC and
  per-resource encryption), `TPixCache*`, `TJournalSegment`, `TStream`.
- **Audio:** `TAudio` (sounds, ambient, spot sounds), `GMSTune` (QuickTime
  Music Architecture tunes), CD-audio control (`PBControlSync`).
  Sound effects go through Ambrosia's SoundTool, statically linked without
  traceback names (around 0xb7600): a software mixer of 8 voices into one
  stereo 8-bit double-buffered channel (`SndPlayDoubleBuffer`, at the rate
  `SndGetInfo('srat')` reports, else 22254.54 Hz). Panning, distance
  attenuation (`CalcStereo`: per ear, a linear pan times 256/distance in
  tiles, capped at 128, over a 31x31 tile neighbourhood), random
  pitch (±12%), looping ambient sounds (`TAudio::CalcAmbient`, `LoopCB`) and
  spot sounds that follow moving objects (`TSoundTracker`) are all computed
  in that mixer; the game never uses `SndDoCommand`. Ambient sources are
  props whose type has the `SoundEffects` property (0x3B) and sound props
  added by `TViewer::SetStage`, plus the `PlayAmbientSound` script call.
  The sound-effects volume (0-8, or -1 for "System Volume") is applied with
  `SetDefaultOutputVolume(v * 32)`, i.e. the Mac's output volume, which also
  scales the music; the system volume is read with `GetDefaultOutputVolume`
  at start and restored at quit. Volume 0 shuts SoundTool (and the music)
  down. The music volume (0-8) is `TuneSetVolume(v << 13)`.

### 2.3 Resource forks

The app rsrc fork has the usual UI resources (`MENU`, `MBAR`, `DLOG`,
`DITL`, `ALRT`, `WIND`, `CNTL`, `STR#`, `crsr`, `cicn`, `ppat`, `clut`,
`pltt`, `snd `) and some custom ones:

- `Lite` 128–158: light-radius masks.
- `TILE`, `DATA 0` (64 KB).
- `Page`: built-in help topics. `TMPL`, `TxSt`, `Audt`.
- `CODE` 0–8: the 68k build of the engine. Unused by us (the PPC build has symbols).
- `WDEF`/`CDEF` 1000+, `MDEF` 128: 6-byte `JMP $0` stubs, patched at runtime
  to jump to PPC routine descriptors (`TWDEFRegister`/`TCDEFRegister`).
- `LDEF 128`: a 68k trampoline. On `lInitMsg` it clears `ListRec+0x3C`,
  otherwise it calls the proc pointer stored at `ListRec+0x3C` with the LDEF
  arguments. The HLE List Manager does the same natively, so **no 68k code
  ever has to run**.
- Fonts: `FOND 128 "Rogue Font"`.

The scenario rsrc fork adds fonts (`FOND 128 "Seldane"`, `FOND 1046
"ArgosANouveau"`, two `NFNT`s, one TrueType `sfnt`), 19 `PICT`s (1.1 MB:
title, slideshows, backgrounds), `FILT` (lighting/colour filters), `PORT`,
`eBRS`/`eSTM`, `MSta`, `clut`.

## 3. Scenario archive format (Delver Archive)

From delvmod (`delv/archive.py`), checked against our copy and the engine
(`TSegFile::*`, `TCachedSegFiles::*`). `tools/delv_archive.py` implements all
of this (`info`, `list`, `dump`, `export`).

- Header (0x80 bytes): Pascal title at 0 ("Cythera: Fate of Alaric"),
  Pascal player name at 0x20 (empty in the scenario), unknown bytes at 0x40
  (`13 00 02 00 00 00 00 00 02`).
- TOC: page 0 is the master page, 256 × (offset u32, length u32) at 0x80.
  Its entry 0 is itself `(0x80, 0x800)`. `TSegFile::IsSegFile` checks exactly
  that word. Entry *p* ≠ 0 points to page *p*, another 256 × (offset,length).
  **Resource ID = `p<<8 | n`**: the high byte is the page number. (delvmod's
  "subindex" is `p-1`.) An offset of 0 means absent. The scenario has 34
  pages and 1,558 resources.
- **Encryption** is not stored anywhere in the archive. The engine decrypts
  when the caller asks (`GetEncryptedSegment`, used by the VM's
  `VAddrToPtr`/`DoExpr` for IDs < 0x8000). It XORs each byte with the low
  byte of an LCG: `key = id ^ id>>8; m = (id&0x3F)*4+1; b = (id>>6)&0xFF`,
  then per byte `key = key*m+b`. The engine masks `b` to 8 bits, delvmod
  doesn't, which matters only for IDs ≥ 0x4000 (none are encrypted). The
  engine's `Encrypt` has a skip count to start mid-segment. The encrypted
  pages are 02, 03, 05, 08–1E and 30, except `0210`. Pages 01 and 04 and
  everything ≥ 0x80 are clear. This matches delvmod's lists, and an entropy
  test agrees on every resource.
- Pages: 01 global symbol table, 02 string arrays (character names, signs,
  books, quests...), 03/05 AI and script data, 04 compiled combat AI
  (Pascal name + 8-byte records), 08–1E scripts (dialogue, potions,
  mechanics, objects, zones 14xx, characters 18xx, monsters, skills, areas),
  30 default methods, 80 maps, 81 prop lists, 84 landscapes 288×32,
  88 portraits 64×64, 8A skill icons (raw 32×16), 8E tile sheets 32×512,
  8F sized graphics, 90 music, 91 sounds, F0 general data (tile
  names/attributes/compositions, prop→tile, prop offsets, schedules,
  zoneports, monster stats, characters, symbol lists).
  Saved games use the same format (delvmod: pages 82 explored-area bitmaps
  and E0 journal).
- **Maps (80xx):** 32-byte header (u16 width, height, 0, two roof-layer
  sizes; u8 ×2 edge propagation; u16 ×4 exit zoneports N/E/S/W; zero
  padding). Then `0x40·(roof1+roof2)` bytes of roof data and `w·h` u16 tile
  ids. **Prop lists (81xx):** 16-byte records: flags, x:12 y:12 (or the
  container index + 0x100 if flags&0x18), u16 aspect<<10|type, u16
  persistence d1:d2, u16, u32, u16.
- **Sounds (91xx, `asnd`):** magic, u32 N (there are 512 + 1024·N samples),
  a **Fixed** sample rate (0x56220000 = 22050, 0x56EE8B9F = 22254.55,
  0x2B7745D0 = 11127.27; delvmod reads it as u16 rate + u16 flags). Then
  signed 8-bit samples stored as big-endian int16, always within -128..127.
  **Music (90xx):** QuickTime Music tunes (a `musi` tune-header atom first).

**Graphics:** Delver Compressed Graphics is an LZ77/RLE mix, 8-bit indexed.
Byte opcodes:
- `00–7F`: short copy (2 bytes).
- `80–BF`: long copy (3 bytes). Copies may be preceded by 0–3 literal bytes.
  A copy repeats its pattern if it overlaps.
- `Cx`: 4·(x+1) literals.
- `Dx`: x literals.
- `Ex`: run of x+3 of the next byte.
- `F0 n c`: run of n+3.
- `FF`: end.

The palette is `clut` 256 in the scenario's resource fork. delvmod's hard-coded
table differs in entries 16, 247, 252 and 253. 8Fxx images, and `8EFF`
(194×127, stored among the tile sheets), start with u16 width and u16
height. Their rows are padded to a multiple of 4. Tile *t* < 0x1000 is
image `t&15` of sheet `8E00|t>>4`. Tiles 0x1000–0x1FFF are composed from
F013: 16 u16 per tile, each `seg<<12 | sheet<<4 | tile`, which picks the 8×8
piece *seg* of that tile. Pieces are numbered down, then across. They are
placed across, then down. Colour index 0 is transparent for sprites.

**Checked against delvmod** (`work/verify_archive.py`, not committed): same
1,558 IDs, byte-identical decrypted data for all of them, all 441 images
decompress identically, all 8,192 tiles compose identically, and map
tiles/sizes, prop records and tile names match.

**VM objects** (script pages): a resource is either a class, or a bare object
at offset 0. A class has a u16 at offset 0 giving the offset of its field
table. A field table is `Ax`-style: count, then (u32 value, u16 key).
Object types:
- `81 argc nlocals code…`: function.
- `9n`: array; the count is the u16 & 0xFFF, followed by u32 values.
- `An`: table.
- anything else: a C string.

References inside the resource are `0x8000_0000 | id<<16 | offset`. Oddity:
`0201` (character names) refers to its own strings as `0x9165oooo`.

**Scripts:** a stack-ish bytecode (delvmod's rdasm/ddasm). Opcodes <0x80 are
expression ops (push local/arg/byte/short/word/string/data, arithmetic, field
get, cast, is_type). Opcodes ≥0x80 are statements (set, branch, switch,
if/if_not, print, return, conversation prompt/response, method call, call
resource/index). **0xA0–0xFF are ~96 engine "system calls"** (iterators,
Random, Create/Delete, ChangeZone, PlaySound/Music, Slideshow, AddQuest,
SetFlag, ...). Values are tagged 32-bit words: ints (28-bit), object refs
(`0x40ttnnnn`), resource refs, `true`/`false`/`none`/`empty` (`0x5000xxxx`),
and near/far data refs (`0x8…`).

Since the port reuses the original engine code, the VM does not have to be
reimplemented. This knowledge helps with debugging and verification.

## 4. Porting strategy: evaluation

### Option A: keep the engine, reimplement the OS (HLE)
Run the original PPC code on a PPC CPU interpreter and replace every imported
Toolbox function with a native C implementation built on SDL2.

- **Pros:** Exact 1:1 behaviour. All engine logic (lighting, AI, pathfinding,
  combat formulas, VM, save format, UI layout) is the original code. No
  specification is needed. The work is bounded by the ~560 imported calls,
  which are well documented in *Inside Macintosh*. **No Apple ROM or System
  software is needed**, unlike SheepShaver or Basilisk.
- **Cons:** QuickDraw, Window, Dialog, Control, List, Menu and TextEdit must
  be emulated faithfully, including their in-memory structures, because apps
  poke `GrafPort`, `PixMap`, `WindowRecord`, `ListRec` and `TERec` fields.
  PPC has to be interpreted (performance is fine: the original ran on a
  ~100 MHz 603e and a C interpreter reaches hundreds of MIPS).
- The CPU is small: 32-bit user-mode PPC, integer + FPU. No supervisor mode,
  no MMU, no AltiVec.

### Option B: rewrite the engine from scratch
Re-derive the whole engine (VM, heap/GC, renderer, AI, UI, save format) from
the disassembly and write it natively.

- **Pros:** "Clean" native code, no emulation layer.
- **Cons:** 840 KB of optimised PPC code to re-derive by hand. Getting 1:1
  behaviour is very hard (rendering, AI and timing are full of details).
  Save-file and script-heap compatibility are fragile. Very high risk.

### Decision
**Option A.** It is the option the goal describes as "keep parts intact and
replace only the system calls". It is the only realistic path to a true 1:1
feature match, and it is testable step by step: run, hit an unimplemented
call, implement it, repeat. The named disassembly makes debugging tractable.
Later, hot or awkward parts can be replaced by native code, and a static
PPC→C recompiler is a possible optimisation. Neither is needed for
correctness.

## 5. Runtime design (the "Delver runtime")

C11 + SDL2. No dependencies beyond SDL2 and bundled single-header libraries.

```
src/
  main.c             CLI, config, startup
  cpu/ppc.[ch]       PowerPC interpreter (UISA user mode + FPU)
  mem/               guest memory (flat, big-endian), Memory Manager (zones, handles)
  loader/pef.c       PEF loader: sections, pidata, relocations, import binding
  os/                HLE Toolbox, one file per manager:
    trap.c             import table -> native handler dispatch, guest callbacks (UPPs)
    resources.c        Resource Manager (read/write resource forks)
    files.c            File Manager over a host directory ("virtual volume")
    qd_*.c             QuickDraw: ports, GWorlds, pixmaps, regions, CopyBits,
                       shapes, patterns, text (NFNT + TrueType), PICT, cursors
    windows.c events.c menus.c dialogs.c controls.c lists.c textedit.c
    sound.c music.c    Sound Manager, QuickTime Music (tunes, note allocator)
    threads.c          Thread Manager (host threads, baton passing)
    misc.c             Gestalt, Time Manager, Process Manager, AppleEvents, stubs
  host/              SDL video (8-bit screen → texture), audio, input, clipboard
tools/               Python analysis tools (rsrc, pef, ppcdis) and setup scripts
```

Key mechanisms:

- **Guest address space:** one flat host buffer, big-endian accessors.
  - 0–0x3FFF: low-memory globals. Reads of NULL return 0, as on real Macs.
  - Import stubs (TVectors whose code address lies in a trap range).
  - Code and data sections, stacks, then the application heap zone.
- **Imports:** import *i* resolves to a TVector `{TRAP_BASE+4i, i}`. When the
  interpreter's PC enters the trap range, it calls the native handler, sets
  r3/f1, and returns to LR. Weak libraries we do not provide resolve to NULL,
  and the app takes its fallback paths (no Appearance, no Nav Services, no
  InputSprocket).
- **Callbacks:** `guest_call(tvector, args)` runs a nested interpreter loop
  until it returns to a sentinel address. It is used for UPPs (routine
  descriptors built by our `NewRoutineDescriptor`), WDEF/CDEF/LDEF, dialog
  filters, thread entry points and completion routines.
- **Threads:** each guest thread runs on its own host thread, and only the
  baton holder executes. This is cooperative, as the Mac Thread Manager was,
  and it works even when a yield happens deep inside a nested callback.
- **Display:** an emulated main GDevice (default 800×600, 8-bit indexed,
  palette from `SetEntries`/palettes) with an SDL window, scaled. The menu
  bar is drawn by our Menu Manager.
- **Files:** a host directory is the Mac volume. Resource forks are
  `<name>.rsrc` sidecars. Finder info goes in a metadata sidecar.
  `FindFolder(kPreferencesFolderType)` maps to a per-user directory.
- **Determinism and testing:** a headless mode with scripted input (wait,
  click, key, screenshot to PNG) plus API/function tracing using the
  recovered symbols. This allows autonomous regression testing.

## 6. Open questions / risks

- Exact semantics of QuickDraw transfer modes, CopyBits colour mapping and
  text styles. Verify visually against screenshots of the original (the
  installer includes a few).
- Font substitution for system fonts (Chicago/Geneva/Charcoal are not
  shipped). The game's own fonts are available.
- QuickTime Music: needs a General MIDI synthesizer. Plan: a bundled
  lightweight synth, or TinySoundFont with a user-supplied SoundFont.
- Registration: the licence check is replaced by default (src/os/license.c;
  `make LICENSE_BYPASS=0` keeps the original shareware flow).
- Timing: the game paces itself with `TickCount` only (60.15 Hz ticks from the
  host clock; it doesn't use Microseconds, VBL or Time Manager tasks). Its main
  loop (`TApp::MEL`) yields to its threads, then calls `WaitNextEvent` with a
  3-tick sleep. A custom thread scheduler (`TTaskMaster::MyScheduler`) runs the
  map animation thread (`TMapWindow::AnimThread`: redraw, colour cycling, tile
  frames) when `next` is due, then sets `next = now + speed`, where speed is a
  preference (bits 2-5 of the first preferences byte, 6 ticks by default): 10
  animation frames and at most 10 steps per second (holding an arrow walks one
  tile per 6 ticks; `TGameSys::HeartBeat` runs once per turn). Screen effects
  wait in `while (TickCount() < t)` loops. `GetDateTime` only dates saved games
  and measures time spent suspended.
