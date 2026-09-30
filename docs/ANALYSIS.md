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
  Saved games use the same format (§3.1).
- **Maps (80xx):** 32-byte header (u16 width, height, 0, two roof-layer
  sizes; u8 ×2 edge propagation; u16 ×4 exit zoneports N/E/S/W; zero
  padding). Then `0x40·(roof1+roof2)` bytes of roof data and `w·h` u16 tile
  ids. **Prop lists (81xx):** 16-byte records: flags, x:12 y:12 (or the
  holder, see §3.1), u16 aspect<<10|type, u16 persistence d1:d2, u16,
  u32, u16.
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

### 3.1 Saved games

A saved game ("Saved Games/Hero") is a Delver archive in the same format
whose resources override the scenario's; its resource fork holds the
Finder preview (`PICT`/`pnot`) and `SCEN` 128, an alias record naming
"Cythera Data". Its header has the player's name at 0x20.
There is no checksum: `TSegFile::Verify` only checks that TOC entries lie
inside the file and don't overlap, and `SegFileHeader::CompatibleVersions`
the u16 at 0x42 (2). `TDelverApp::SaveToFile` writes, in order, the preview,
`SaveLevelProps` (current zone), the to-do list, macros, `THeap::Save`, then
the game state stream, and copies every segment of the "Delver Temp"
archive (segments changed since loading) into the file. `RestoreModel`
reads it back: F009, F00E, then the hero's zone from F009, its map and props,
the heap, F306, the stream, to-do, journal, macros. `tools/delv_save.py`
decodes and edits all this (verified by loading edited saves in the game).

| Resource | Contents |
|---|---|
| 0400 | Game state stream: chunks of 4-char tag, u32 length (counting itself), data: `Char`, `Mons`, `FXQ `, `Wind`, `Grem` (below) |
| 0401 | To-do list: 256 × (u8 done, u8, u16 day added, u32 text: VM value `0x3kkk021A` = quest text *k*, `none` if unused); scripts pick the slot |
| 0404 | F-key macros, 10 × u16 (`FFFF` none) |
| 81zz | Props of each visited zone, scenario format (records from list index 0x100) |
| 82zz | Explored map of each visited zone: a bit per tile, rows of (w+7)/8 bytes, LSB first |
| 8800 | The hero's portrait (DCG, 64×64) |
| E0xx | Journal pages (not decoded) |
| F009 | Character table, 512 × 32 bytes (only 0–255 are used; guest 0x228558) |
| F00E | Room fields: 1024 × u16, the Room objects' field 0x14 |
| F306 | The current zone's 256 character slots (prop records, list indexes 0–255) |
| F307 | VM heap, 256 KB: blocks of u32 size, u16 handle, u8 (bits 4–6 kind, 0 = free; bit 3 adds 4 bytes), data padded to 4 |
| F308 | Prop frames, 8 KB (`PropItem::AllocateFrame`) |

**`Char` chunk** (110 bytes, `WriteData` formats; stream offsets in brackets):

| Offset | Type | Contents (VM global number, `GetGlobal__Fs`) |
|---|---|---|
| 0 [08] | h | karma (12), 55 at start |
| 2 | h | languages known (14) |
| 4 | h | difficulty 0–4 (monster strength in `TActiveMonster::__ct`), 2 |
| 6 | h | next unique name (`cbNewUniqueName`), 0x800 |
| 8 [10] | 32 × b | story state values QV 0–31 (syscalls DC get, DD set) |
| 40 [30] | 8 × l | story flags QF 0–255, bit *n*&31 of long *n*>>5 (DE get, DF set) |
| 72 [50] | l | clock: 0x1000 per hour, hour = clock>>12 (global 0); the day rolls over at 0x18000 |
| 76 [54] | h | day (15), 1 at start |
| 78 | b | automap enabled (viewer+0xd, `cbEnableAutoMap`) |
| 79 [57] | l | seconds played (`GetDateTime` differences) |
| 83 | 27 × b | zero |

**Other chunks.** `Mons`, the active monsters (`TActiveMonster::SaveMonsters`):
per monster a byte (property 0x37, the kind: 9/10 crawler, 11 dragon, 12
octopus), then `Save`: h prop index, h, h; for level monsters (index ≥
0x100) h and 32 bytes of monster data; b facing (0–3) and 4 × h; h queue
length and b h h l per queued activity; crawlers add h *n* and *n* × h (their
segments), dragons 4 × h, octopuses 8 × h. `FXQ `:
timed spell effects, 3 × h each. `Wind`: open inventory and character
windows (`TInventoryWindow::MarshalAll`), empty unless one was open.
`Grem`: 1024 raw bytes of `TGremlin` state.

**Character record** (F009; offsets = the VM's Character fields in
`SetField__Fsss5VAddr`, field numbers in brackets):

| Offset | Contents | Offset | Contents |
|---|---|---|---|
| 0 | zone | 1–3 | x:12 y:12 [1, 2] |
| 4–5 | aspect<<10 \| type (facing in the aspect) [0x24] | 6–7 | status: bit 0 alive [0x14] |
| 8 | character flags: bit *n* = the scripts' flag *n* (`SetCharacterFlag`); 6 in the party [0x13] | 9, 10, 11 | body, reflex, mind [0x17–0x19] |
| 12–13 | experience [0x1A] | 14, 15 | health, maximum [0x1C, 0x1D] |
| 16, 17 | magic, maximum [0x1E, 0x1F] | 18 | timing [0x23] |
| 19 | level [0x1B] | 20–21 | [0x25] |
| 22 | behaviour: 2 player, 1 party [0x15] | 23 | [0x27] |
| 27 | nutrition [0x28] | 28 | training points [0x21] |
| 29 | [0x20] | 30 | behaviour 2 [0x16] |

Bytes 24–26 and 31 are unknown. The hero is character 1 (`RestoreModel`
sets the player to 1); names are `0201`, the hero's is the header's.

**Props of a zone in memory** are 256 character slots (list indexes 0–255;
F306 for the current zone) followed by the 81zz records (index 0x100 +
record number), which is what container references use. The flags byte:

| Flags | Meaning | Location field |
|---|---|---|
| 00 | on the map | x:12 y:12 |
| 09 | inside a container | container's list index |
| 10, 18 | carried, worn by a character | character (low 16 bits) |
| 11 | held by a character, but stays in this zone (NPCs' gear) | character |
| 1C | a skill or spell: level = aspect & 0xF (bit 0x10: not trained yet, below) | character |
| 42 | a character or monster "egg" to hatch; 04 once active | x:12 y:12 |
| FF | free (a chain of free records at run time) | |

Item counts (`GetItemCount`/`SetItemCount`) follow the Stacking field (0x28)
of the prop's class, resource 1000+type: 0x01 count in d2, 0x02 count in
d1:d2, 0x20 the aspect shows it (1, 2, 3, 4, <10, <20, <35, more). The
oboloi (type 0x82) have 0x22. Skills are classes 1Axx (Attack = 1AC0), whose
Look method returns their name.

**Everything the characters hold lives in the current zone's list**, for
all 256 characters. Changing zones (`TGameViewer::GoToLocation`) moves
them: `ShuffleUpPartyInventory` copies each held prop (flags 10/18/1C) and,
recursively, its contents (flags exactly 09) to the end of the list and frees
the originals (`CopyProp`), `SaveLevelProps` stores the old zone (character
slots write their position back to F009; trailing free records are dropped),
`LoadLevelProps(zone, n, 1)` loads the new list (the save's 81zz, else the
scenario's; props with flags 20/21 and some flag-00 props are refreshed from
the scenario) and appends the n carried props, renumbering their container
references. `MovePartyBetweenLevels` puts party members at the arrival
point (slot flags 42), `CueCharacters` places the characters whose F009
zone is the new one and frees the other slots, `RebuildParty` hatches the
party. The active monsters of the old zone are dropped (`LeavingLevel`).

**Story state.** The QF/QV bits and bytes of `Char` are the scripts' story
flags. Syscall opcode 0xA4+*n* is entry *n* of the engine's callback table
(`cbsetportrait` … `cbDebugStr`, 91 entries at data 0x10a000), so DC–DF are
`cbGetQV`/`cbSetQV`/`cbGetQF`/`cbSetQF` (and delvmod's
SetFlag/ClearFlag/TestFlag, C1/C2/C4, are Add/Remove/HasAbility). The scenario
names none of them (0101 lists engine methods and resources only), but the
scripts that test and set each one can be found (`delv_save.py flags
--refs`): QF 0 is set by Alaric (1802) at the first audience; QF 1 by Lindus
(1850) once he has trained the hero in magic. QV 1 is the opening's plot
stage, read by most townspeople: 0 at start; 1 when Odemia's gate guard (1864)
turns the hero away; 2 when Eudoxus (185C) dies while it is 1; 3 when Ariadne
(1835) joins the party; 4 when the guard sees her back (the gate opens,
signal 1) or Philinus (1832) does; 5–7 later (Antiphus 185E, zone scripts
1401/1402). The to-do list, the journal, prop states (d1:d2, positions) and VM
heap objects hold the rest of the story state.

**Schedules** (F00B) move the characters: 256 × u16 entry counts, then per
character 8-byte entries: u8 hour, u8 behaviour, u8 condition, u8 argument,
u32 zone<<24 | x:12 y:12 (zone 255: nowhere). `ScheduleOne` takes the first
entry whose condition holds (`EvalCondition`): 00 always, 01 never, 02/03
story flag *arg* set/clear, 20–38 random, 40–7F a character bit, 80+*n*
QV *n* == arg, A0+*n* ≥, C0+*n* ≠, E0+*n* < (e.g. Ariadne: nowhere while
QV 1 = 0, the farmhouse cellar while QV 1 < 3, then her day in Odemia).
They run when the hour changes and on zone changes (`ScheduleTime`), not when
a game is loaded.

**Skills** have a bit 0x10 in the aspect that the Abilities list shows in
italics: a class skill not trained yet. It counts as no skill at all where
scripts check (0EAC returns 0), e.g. the grimoire needs Casting without it;
trainers (0EB1) clear it.

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
  gamma-table calls makes fades work. The screen can change size while the
  game runs (the Large display modes): the frame buffer, device, `screenBits`
  and every port on the screen follow, then the game gets the Display
  Manager's notice (`'aevt'/'cnfg'` with `'dspl'` → items of `'dold'`/`'dnew'`
  records holding `'dmdd'` and `'dddr'`), which its `HandleDisplayNotice` →
  `TApp::DoMonitorChanged` handles by moving and stretching its windows (the
  status panel and backdrop follow the screen's edges). When the screen shrinks,
  the port first fits the game's other windows that wouldn't fit, the way its
  grow box does (`SizeWindow`, then the window's `ResizeRoutine`), and afterwards
  moves any left sticking out back onto the screen.
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
