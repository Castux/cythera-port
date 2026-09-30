# Cythera — Technical Analysis

What the original program and its data are, and how the port runs them: the
reverse-engineering reference. Building and tools are in
[DEVELOPING.md](DEVELOPING.md), the reimplemented calls in [TOOLBOX.md](TOOLBOX.md),
status and history in [PLAN.md](PLAN.md).

## 1. The distribution archive

The game came as `Cythera Installer`, a classic Mac application (both forks),
distributed as a **StuffIt 5** archive (Arsenic compression,
`orig/Cythera_Installer.sit`) and as **MacBinary** (`orig/Cythera.bin`): the
installers inside are byte-identical (installer dated 1999-11-02).

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

`tools/setup_gamedata.sh` rebuilds this layout in `gamedata/`.

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
  Music Architecture tunes), CD-audio control (`PBControlSync`); see §2.4.

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

### 2.4 Audio

Sound effects go through Ambrosia's SoundTool, statically linked without
traceback names (around 0xb7600): a software mixer of 8 voices into one
stereo 8-bit double-buffered channel (`SndPlayDoubleBuffer`, at the rate
`SndGetInfo('srat')` reports, else 22254.54 Hz). Panning, distance
attenuation (`CalcStereo`: per ear, a linear pan times 256/distance in
tiles, capped at 128, over a 31x31 tile neighbourhood), random pitch (±12%),
looping ambient sounds (`TAudio::CalcAmbient`, `LoopCB`) and spot sounds that
follow moving objects (`TSoundTracker`) are all computed in that mixer; the
game never uses `SndDoCommand`. Ambient sources are props whose type has the
`SoundEffects` property (0x3B) and sound props added by `TViewer::SetStage`,
plus the `PlayAmbientSound` script call.

The sound-effects volume (0-8, or -1 for "System Volume") is applied with
`SetDefaultOutputVolume(v * 32)`, i.e. the Mac's output volume, which also
scales the music; the system volume is read with `GetDefaultOutputVolume` at
start and restored at quit. Volume 0 shuts SoundTool (and the music) down.
The music volume (0-8) is `TuneSetVolume(v << 13)`.

Music: `GMSTune::Play` sets the tune header and queues the sequence with
`kTuneStartNow`; `GMSTune::Idle` requeues it when `TuneGetStatus` reports an
empty queue (that's how tunes loop). Tunes are split into sections by
end-subtype markers with value 1, which QTMA ignores; only value 0 ends the
sequence. `GMSTune::Pause` (on suspend/deactivate) saves
`(tunePtr - tune) / 4` from the status as its resume point, and `Resume`
requeues at that offset.

### 2.5 Timing

The game paces itself with `TickCount` only (60.15 Hz; it doesn't use
Microseconds, VBL or Time Manager tasks, though InsTime/PrimeTime are
imported). Its main loop (`TApp::MEL`) yields to its threads, then calls
`WaitNextEvent` with a 3-tick sleep. A custom thread scheduler
(`TTaskMaster::MyScheduler`) runs the map animation thread
(`TMapWindow::AnimThread`: redraw, colour cycling, tile frames) when `next` is
due, then sets `next = now + speed`, where speed is a preference (bits 2-5 of
the first preferences byte, 6 ticks by default): 10 animation frames and at
most 10 steps per second (holding an arrow walks one tile per 6 ticks, polled
with `GetKeys`; `TGameSys::HeartBeat` runs once per turn). Monsters act as
turns pass. Screen effects wait in `while (TickCount() < t)` loops.
`GetDateTime` only dates saved games and measures time spent suspended.

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

## 4. Porting strategy

Two options were weighed:

- **Keep the engine, reimplement the OS (chosen).** Run the original PPC code
  on an interpreter and replace every imported Toolbox call with a native
  implementation. Behaviour is 1:1 by construction (lighting, AI,
  pathfinding, combat formulas, the VM, the save format and the UI layout are
  the original code), no specification is needed, and the work is bounded by
  the ~560 imported calls documented in *Inside Macintosh*. The catch: the
  Toolbox must be emulated down to its in-memory structures, because the
  game reads and writes `GrafPort`, `PixMap`, `WindowRecord`, `ListRec` and
  `TERec` fields directly. The CPU side is small (32-bit user-mode PPC,
  integer and FPU; no supervisor mode, MMU or AltiVec), and the original ran
  on a ~100 MHz 603e.
- **Rewrite the engine natively.** 840 KB of optimised PPC code to re-derive
  by hand, with 1:1 behaviour very hard to reach and fragile save and
  script-heap compatibility.

The first is the goal's "keep parts intact and replace only the system
calls", and it is testable step by step: run, hit an unimplemented call,
implement it, repeat. The named disassembly makes debugging tractable.

## 5. Runtime design

C11 and SDL2; the source layout is in [DEVELOPING.md](DEVELOPING.md#source-layout).

- **Guest address space:** one flat host buffer (256 MB) with big-endian
  accessors. 0–0x3FFF holds the low-memory globals (reads of NULL return 0,
  as on real Macs), then the import trap entries, the code and data sections,
  stacks, and the heap zones.
- **Imports:** import *i* resolves to a TVector `{TRAP_BASE+4i, i}`. When the
  interpreter's PC enters the trap range, it calls the native handler, sets
  r3/f1 and returns to LR. Weak libraries we don't provide resolve to NULL,
  and the game takes its fallback paths (no Appearance, Navigation Services,
  InputSprocket). StdCLib, which the MSL runtime looks up at run time, is
  provided natively.
- **Callbacks:** `guest_call(tvector, args)` runs a nested interpreter loop
  until it returns to a sentinel address: UPPs (routine descriptors built by
  our `NewRoutineDescriptor`), WDEF/CDEF/LDEF, dialog filters, thread entry
  points, completion routines.
- **Threads:** Thread Manager threads are coroutines (minicoro), switched
  cooperatively as the Mac did, even from deep inside nested callbacks. (Host
  threads with baton passing were far too slow.)
- **Display:** an emulated 8-bit indexed main GDevice (640×480 by default),
  presented once per tick in an SDL window scaled by whole multiples; the
  menu bar is drawn by our Menu Manager. A video driver (refnum -50) with
  gamma-table calls makes fades work.
- **Text:** the game's own fonts (NFNT bitmaps, and the ArgosANouveau
  TrueType font) are used as they are; TrueType is rasterised hinted and
  monochrome with FreeType, like the Mac's scaler. The system fonts it asks
  for (Chicago, Geneva) aren't shipped, so host TrueType fonts stand in
  (Verdana/Tahoma on Windows, DejaVu on Linux, Geneva on macOS).
- **Files:** a host directory is the Mac volume. Resource forks are
  `<name>.rsrc` sidecars, Finder info goes in `.finderinfo` files, and
  `FindFolder` maps the System Folder, Preferences and Saved Games to a
  per-user directory. A game folder found next to the program is read-only.
- **Audio:** the SDL audio thread mixes Sound Manager channels and renders the
  QuickTime tunes (TinySoundFont with a General MIDI SoundFont, or a small
  built-in synth), scaled by the output volume. It reads guest memory, so it
  must never fail hard.
- **Registration:** by default, the licence check routines are redirected to
  native ones that report a registration (`src/os/license.c`, applied only if
  the code matches).
- **Testing:** a headless, deterministic mode with scripted input and
  screenshots, plus call tracing with the recovered symbols, for autonomous
  regression testing.

## 6. Known gaps

- System font substitutes differ per platform, so text layout (line breaks,
  menu widths) differs slightly from the original and between platforms.
- Some Toolbox calls are partial or stubs, each documented as harmless for
  this game in [TOOLBOX.md](TOOLBOX.md); calls reached only in late-game
  content are less exercised.
