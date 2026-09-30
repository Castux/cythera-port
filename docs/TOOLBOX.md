# Toolbox inventory

Every call the game imports (`python tools/pef.py gamedata/Cythera imports`: 563
imports from 10 libraries) and how complete its implementation in `src/os/` is.
Status:

- **full**: does what Inside Macintosh describes, for everything the game uses.
- **partial**: works for the game's uses, with a documented limitation.
- **stub**: no-op or constant result. Each one says why that is right, or harmless,
  for this game.
- **absent**: weak import of a library we don't provide. It resolves to NULL, and
  the game checks for that and takes its fallback path.
- **audio**: Sound Manager and QuickTime Music (`sound.c`, `music.c`). Only
  inventoried here.

**Run** marks the calls reached by the scripted scenarios (`tests/run.sh`) plus a
free-form session (saves, Save As/Backup As, closing and reopening games,
windows, menus, preferences) and three 500-action random "monkey" sessions,
counted with `--trap-stats`. 413 of the 524 bound calls were reached. To
repeat the count:
`CYTHERA_ARGS="--trap-stats stats.txt" tests/run.sh`.

| Status | Calls | Reached |
|---|---|---|
| full | 423 | 353 |
| partial | 32 | 27 |
| stub | 49 | 17 |
| audio | 20 | 16 |
| absent (5 weak libraries) | 39 | – |

## Findings of the M7 sweep

Stubs and gaps that were reached and mattered, now fixed:

- **StdCLib was missing.** The MSL runtime binds `PLstrcpy`, `PLstrcat`, `PLstrcmp`
  and 10 other Pascal-string functions at run time with
  `GetSharedLibrary("StdCLib")` and `FindSymbol`, and does nothing if they're
  missing. Mac OS always has StdCLib. Without it, the preferences dialog had no
  group box titles or slider labels, Save As and Backup As proposed garbage
  names, the title screen didn't show the player's name, the combat strategy
  names were empty, and `SameFileSpec` treated all names as equal.
  `GetSharedLibrary` also wrote its error string through the wrong argument.
- **Crash when opening a game from within a game** (after the "Save before
  closing?" prompt). StandardGetFile/StandardPutFile returned with their
  disposed dialog as the current port. Alert now restores the caller's port
  too. Covered by `tests/scripts/saveopen.txt`.
- **Pop-up menu control (CDEF 63) was missing.** The character window's
  strategy pop-up was drawn as a check box and had no menu in its
  `contrlData`, where the game looks for it. Menus taller than the screen
  (that pop-up has 34 items) now scroll.
- **InitZone was a no-op.** The game's segment cache builds a heap zone in a
  141 MB block and evicts from it when it's full. With the no-op, the block sat
  unused and the cache grew in the application zone.
- **Save previews.** `MakeThumbnailFromPixMap` and `AddFilePreview` were stubs.
  Saved games now carry a thumbnail, and the Open dialog shows it.
- **Menus ignored the low-memory system font.** The roster's "Do" pop-up is
  meant to be drawn in Geneva 9.
- **Windows:** Finder info updates after the first one in a folder were lost,
  because `rename()` doesn't replace an existing file on Windows. Backup As
  files didn't appear in the Open dialog.
- Smaller fixes: `ObscureCursor`, `PixPatChanged`, TextEdit up/down arrows,
  Standard File's 31-character name limit, Help Manager result codes.

Stubs and partials that are reached and left as they are (the table gives the
reason for each): the init calls (`InitFonts`, `InitDialogs`, `TEInit`), the
heap calls that only matter to a compacting heap (`MaxApplZone`, `MoreMasters`,
`MoveHHi`, `SetGrowZone`), `PortChanged`, the GWorld pixel-lock calls,
`AppendResMenu('DRVR')` (no desk accessories), Balloon Help (off),
`MakeFilePreview`, and the Apple Event parameter calls (only `oapp` and `quit`
are ever sent).

Not reached by any run, so less certain: 32 stubs (desktop database, catalog
search, Cursor Device Manager, `LaunchApplication`, unmounting, …), mostly
MoreFiles and library code the game links but doesn't use. Also a few paths
only a longer play-through would reach: combat, shops, travel between maps,
the end game.

| Call | Library | Status | Run | Notes |
|---|---|---|---|---|
| ActivatePalette | InterfaceLib | full | yes |  |
| AddResource | InterfaceLib | full | yes |  |
| AECountItems | InterfaceLib | stub |  | no parameters: the only Apple events sent are 'oapp' and 'quit' (no document lists) |
| AECreateDesc | InterfaceLib | full | yes |  |
| AEDisposeDesc | InterfaceLib | full |  |  |
| AEGetAttributePtr | InterfaceLib | stub | yes | errAEDescNotFound, the right answer for the missed-parameter check |
| AEGetNthDesc | InterfaceLib | stub |  | no parameters: the only Apple events sent are 'oapp' and 'quit' (no document lists) |
| AEGetNthPtr | InterfaceLib | stub |  | no parameters: the only Apple events sent are 'oapp' and 'quit' (no document lists) |
| AEGetParamDesc | InterfaceLib | stub |  | no parameters: the only Apple events sent are 'oapp' and 'quit' (no document lists) |
| AEGetParamPtr | InterfaceLib | stub |  | no parameters: the only Apple events sent are 'oapp' and 'quit' (no document lists) |
| AEInstallEventHandler | InterfaceLib | full | yes |  |
| AEProcessAppleEvent | InterfaceLib | partial | yes | dispatches to installed handlers with a parameterless event ('oapp', 'quit') |
| AESizeOfNthItem | InterfaceLib | stub |  | no parameters: the only Apple events sent are 'oapp' and 'quit' (no document lists) |
| Alert | InterfaceLib | full |  | Alert returns with the caller's port (fixed M7) |
| AppendMenu | InterfaceLib | full | yes |  |
| AppendResMenu | InterfaceLib | stub | yes | only called with 'DRVR' for the Apple menu: there are no desk accessories to list |
| BackColor | InterfaceLib | full | yes |  |
| BackPat | InterfaceLib | full | yes |  |
| BackPixPat | InterfaceLib | full | yes |  |
| BeginUpdate | InterfaceLib | full | yes |  |
| BitAnd | InterfaceLib | full |  |  |
| BitMapToRegion | InterfaceLib | full | yes |  |
| BlockMove | InterfaceLib | full | yes |  |
| BlockMoveData | InterfaceLib | full | yes |  |
| BringToFront | InterfaceLib | full | yes |  |
| Button | InterfaceLib | full | yes |  |
| c2pstr | InterfaceLib | full |  |  |
| CalcMask | InterfaceLib | full | yes |  |
| CalcMenuSize | InterfaceLib | full | yes |  |
| CalcVisBehind | InterfaceLib | full | yes |  |
| CallUniversalProc | InterfaceLib | partial | yes | PPC routine descriptors; parameter count from procInfo |
| ChangedResource | InterfaceLib | full | yes |  |
| CharExtra | InterfaceLib | full | yes |  |
| CharWidth | InterfaceLib | full |  |  |
| CheckItem | InterfaceLib | full |  |  |
| CheckUpdate | InterfaceLib | full |  |  |
| ClearMenuBar | InterfaceLib | full | yes |  |
| ClipRect | InterfaceLib | full | yes |  |
| CloseComponent | InterfaceLib | full | yes |  |
| ClosePicture | InterfaceLib | partial | yes | records text, lines and rects (what the game draws into pictures) |
| ClosePoly | InterfaceLib | full | yes |  |
| CloseResFile | InterfaceLib | full | yes |  |
| CloseRgn | InterfaceLib | full |  |  |
| Color2Index | InterfaceLib | full | yes |  |
| CopyBits | InterfaceLib | full | yes | modes, masks, scaling, colour mapping (not recorded into pictures) |
| CopyDeepMask | InterfaceLib | full | yes |  |
| CopyRgn | InterfaceLib | full | yes |  |
| CountDITL | InterfaceLib | full | yes |  |
| CountMItems | InterfaceLib | full | yes |  |
| CrsrDevButtonDown | InterfaceLib | stub |  | no Cursor Device Manager devices; unused |
| CrsrDevButtonUp | InterfaceLib | stub |  | no Cursor Device Manager devices; unused |
| CrsrDevDisposeDevice | InterfaceLib | stub |  | no Cursor Device Manager devices; unused |
| CrsrDevNewDevice | InterfaceLib | stub |  | no Cursor Device Manager devices; unused |
| CrsrDevNextDevice | InterfaceLib | stub |  | no Cursor Device Manager devices; unused |
| CrsrDevUnitsPerInch | InterfaceLib | stub |  | no Cursor Device Manager devices; unused |
| CTab2Palette | InterfaceLib | full | yes |  |
| CTabChanged | InterfaceLib | full | yes |  |
| CurResFile | InterfaceLib | full | yes |  |
| DebugStr | InterfaceLib | stub |  | logged |
| Delay | InterfaceLib | full | yes | timing: see ANALYSIS.md §2.5 |
| DeleteMenu | InterfaceLib | full | yes |  |
| DeleteMenuItem | InterfaceLib | full | yes |  |
| Dequeue | InterfaceLib | full |  |  |
| DetachResource | InterfaceLib | full | yes |  |
| DeviceLoop | InterfaceLib | partial | yes | one 8-bit device, drawing region ignored |
| DialogCopy | InterfaceLib | full |  |  |
| DialogCut | InterfaceLib | full |  |  |
| DialogDelete | InterfaceLib | full |  |  |
| DialogPaste | InterfaceLib | full |  |  |
| DialogSelect | InterfaceLib | full | yes |  |
| DiffRgn | InterfaceLib | full | yes |  |
| DisableItem | InterfaceLib | full | yes |  |
| DisposeCIcon | InterfaceLib | full | yes |  |
| DisposeControl | InterfaceLib | full | yes |  |
| DisposeCTable | InterfaceLib | full | yes |  |
| DisposeDialog | InterfaceLib | full | yes |  |
| DisposeGWorld | InterfaceLib | full | yes |  |
| DisposeHandle | InterfaceLib | full | yes |  |
| DisposePixPat | InterfaceLib | full |  |  |
| DisposePtr | InterfaceLib | full | yes |  |
| DisposeRgn | InterfaceLib | full | yes |  |
| DisposeRoutineDescriptor | InterfaceLib | full |  |  |
| DisposeWindow | InterfaceLib | full | yes |  |
| DragGrayRgn | InterfaceLib | full | yes |  |
| DragWindow | InterfaceLib | full |  |  |
| Draw1Control | InterfaceLib | full | yes |  |
| DrawChar | InterfaceLib | full |  |  |
| DrawControls | InterfaceLib | full | yes |  |
| DrawDialog | InterfaceLib | full | yes |  |
| DrawMenuBar | InterfaceLib | full | yes |  |
| DrawPicture | InterfaceLib | partial | yes | PICT v1/v2 interpreter; clip opcode ignored, oval/round-rect frames approximated |
| DrawString | InterfaceLib | full | yes |  |
| DrawText | InterfaceLib | full | yes |  |
| Eject | InterfaceLib | stub |  | the volume cannot be unmounted (fBsyErr); unused |
| EmptyRect | InterfaceLib | full | yes |  |
| EmptyRgn | InterfaceLib | full | yes |  |
| EnableItem | InterfaceLib | full | yes |  |
| EndUpdate | InterfaceLib | full | yes |  |
| Enqueue | InterfaceLib | full |  |  |
| EqualRect | InterfaceLib | full | yes |  |
| EqualString | InterfaceLib | full | yes |  |
| EraseRect | InterfaceLib | full | yes |  |
| EventAvail | InterfaceLib | full | yes |  |
| ExitToShell | InterfaceLib | full | yes |  |
| FillCRect | InterfaceLib | full | yes |  |
| FillRect | InterfaceLib | full | yes |  |
| FillRgn | InterfaceLib | full | yes |  |
| FindControl | InterfaceLib | full | yes |  |
| FindFolder | InterfaceLib | partial | yes | System Folder and Preferences; other types become System Folder subfolders |
| FindSymbol | InterfaceLib | partial | yes | StdCLib (PL string functions) provided; other libraries not found; fixed M7 |
| FindWindow | InterfaceLib | full | yes |  |
| FixDiv | InterfaceLib | full | yes |  |
| FixMul | InterfaceLib | full | yes |  |
| FlushEvents | InterfaceLib | full | yes |  |
| FlushVol | InterfaceLib | full | yes |  |
| ForeColor | InterfaceLib | full | yes |  |
| FrameRect | InterfaceLib | full | yes |  |
| FrameRgn | InterfaceLib | full |  |  |
| FrameRoundRect | InterfaceLib | full | yes |  |
| FreeMem | InterfaceLib | full |  |  |
| FrontWindow | InterfaceLib | full | yes |  |
| FSClose | InterfaceLib | full | yes |  |
| FSMakeFSSpec | InterfaceLib | full | yes |  |
| FSpCreate | InterfaceLib | full | yes |  |
| FSpCreateResFile | InterfaceLib | full | yes |  |
| FSpDelete | InterfaceLib | full | yes |  |
| FSpExchangeFiles | InterfaceLib | full | yes | swaps both forks; Finder info stays with the names |
| FSpGetFInfo | InterfaceLib | full | yes |  |
| FSpOpenDF | InterfaceLib | full | yes |  |
| FSpOpenResFile | InterfaceLib | full | yes |  |
| FSpOpenRF | InterfaceLib | full | yes |  |
| FSRead | InterfaceLib | full | yes |  |
| FSWrite | InterfaceLib | full | yes |  |
| Gestalt | InterfaceLib | partial | yes | fixed table (System 7.6.1, PPC, QuickTime 3, no Appearance) |
| Get1NamedResource | InterfaceLib | full | yes |  |
| Get1Resource | InterfaceLib | full |  |  |
| GetAuxiliaryControlRecord | InterfaceLib | partial | yes | synthesised records with default colour tables |
| GetAuxWin | InterfaceLib | partial | yes | synthesised records with default colour tables |
| GetBackColor | InterfaceLib | full | yes |  |
| GetCCursor | InterfaceLib | full | yes |  |
| GetCIcon | InterfaceLib | full | yes |  |
| GetClip | InterfaceLib | full | yes |  |
| GetControlAction | InterfaceLib | full | yes |  |
| GetControlMaximum | InterfaceLib | full | yes |  |
| GetControlMinimum | InterfaceLib | full | yes |  |
| GetControlReference | InterfaceLib | full | yes |  |
| GetControlTitle | InterfaceLib | full | yes |  |
| GetControlValue | InterfaceLib | full | yes |  |
| GetCTable | InterfaceLib | full | yes |  |
| GetCurrentProcess | InterfaceLib | partial | yes | one process |
| GetCursor | InterfaceLib | full | yes |  |
| GetCWMgrPort | InterfaceLib | full | yes |  |
| GetDateTime | InterfaceLib | full | yes |  |
| GetDCtlEntry | InterfaceLib | stub |  | returns NULL (no driver table); only the gamma library asks |
| GetDefaultOutputVolume | InterfaceLib | audio | yes | audio (sound.c / music.c): inventoried only |
| GetDeviceList | InterfaceLib | full | yes |  |
| GetDialogItem | InterfaceLib | full | yes |  |
| GetDialogItemText | InterfaceLib | full | yes |  |
| GetDrvQHdr | InterfaceLib | stub |  | empty drive queue; unused |
| GetEOF | InterfaceLib | full | yes |  |
| GetFNum | InterfaceLib | full | yes |  |
| GetFontInfo | InterfaceLib | full | yes |  |
| GetForeColor | InterfaceLib | full | yes |  |
| GetGDevice | InterfaceLib | full | yes |  |
| GetGray | InterfaceLib | partial | yes | average of the two colours (no device search) |
| GetGrayRgn | InterfaceLib | full | yes |  |
| GetGWorld | InterfaceLib | full | yes |  |
| GetGWorldPixMap | InterfaceLib | full | yes |  |
| GetHandleSize | InterfaceLib | full | yes |  |
| GetIndString | InterfaceLib | full | yes |  |
| GetItemMark | InterfaceLib | full |  |  |
| GetKeys | InterfaceLib | full | yes |  |
| GetMainDevice | InterfaceLib | full | yes |  |
| GetMBarHeight | InterfaceLib | full | yes |  |
| GetMenu | InterfaceLib | full | yes |  |
| GetMenuHandle | InterfaceLib | full | yes |  |
| GetMenuItemText | InterfaceLib | full |  |  |
| GetMouse | InterfaceLib | full | yes |  |
| GetNamedResource | InterfaceLib | full | yes |  |
| GetNewCWindow | InterfaceLib | full | yes |  |
| GetNewDialog | InterfaceLib | full | yes |  |
| GetNewPalette | InterfaceLib | full | yes |  |
| GetNewWindow | InterfaceLib | full |  |  |
| GetNextDevice | InterfaceLib | full | yes |  |
| GetOSEvent | InterfaceLib | full | yes |  |
| GetPenState | InterfaceLib | full | yes |  |
| GetPicture | InterfaceLib | full | yes |  |
| GetPixBaseAddr | InterfaceLib | full | yes |  |
| GetPixelsState | InterfaceLib | stub | yes | no pixel state kept (never purgeable, lock state not reported); only MyCopyGWorld uses them |
| GetPixPat | InterfaceLib | full |  |  |
| GetPort | InterfaceLib | full | yes |  |
| GetProcessInformation | InterfaceLib | partial | yes | one process |
| GetPtrSize | InterfaceLib | full | yes |  |
| GetResource | InterfaceLib | full | yes |  |
| GetSharedLibrary | InterfaceLib | partial | yes | StdCLib (PL string functions) provided; other libraries not found; fixed M7 |
| GetString | InterfaceLib | full | yes |  |
| GetSubTable | InterfaceLib | full | yes |  |
| GetToolboxTrapAddress | InterfaceLib | partial | yes | fake addresses: every trap reads as implemented |
| GetWMgrPort | InterfaceLib | full | yes |  |
| GetWRefCon | InterfaceLib | full | yes |  |
| GetWTitle | InterfaceLib | full | yes |  |
| GetZone | InterfaceLib | full | yes |  |
| GlobalToLocal | InterfaceLib | full | yes |  |
| GrowWindow | InterfaceLib | full | yes |  |
| HandToHand | InterfaceLib | partial | yes | new handle in the application zone, whatever the current zone |
| HasDepth | InterfaceLib | partial |  | the screen is 8-bit only, which is what the game asks for |
| HCreate | InterfaceLib | full | yes |  |
| HDelete | InterfaceLib | full |  |  |
| HGetFInfo | InterfaceLib | full | yes |  |
| HGetState | InterfaceLib | full | yes |  |
| HGetVol | InterfaceLib | full |  |  |
| HideControl | InterfaceLib | full | yes |  |
| HideCursor | InterfaceLib | full | yes |  |
| HidePen | InterfaceLib | full | yes |  |
| HideWindow | InterfaceLib | full | yes |  |
| HiliteControl | InterfaceLib | full | yes |  |
| HiliteMenu | InterfaceLib | full | yes |  |
| HiliteWindow | InterfaceLib | full | yes |  |
| HLock | InterfaceLib | full | yes |  |
| HLockHi | InterfaceLib | full | yes |  |
| HMRemoveBalloon | InterfaceLib | stub | yes | Balloon Help is off (no Help menu): hmHelpDisabled / hmNoBalloonUp |
| HMShowBalloon | InterfaceLib | stub | yes | Balloon Help is off (no Help menu): hmHelpDisabled / hmNoBalloonUp |
| HNoPurge | InterfaceLib | full | yes |  |
| HOpen | InterfaceLib | full | yes |  |
| HOpenResFile | InterfaceLib | full |  |  |
| HPurge | InterfaceLib | full | yes |  |
| HSetFInfo | InterfaceLib | full | yes |  |
| HSetState | InterfaceLib | full | yes |  |
| HSL2RGB | InterfaceLib | full | yes |  |
| HUnlock | InterfaceLib | full | yes |  |
| InitCursor | InterfaceLib | full | yes |  |
| InitDialogs | InterfaceLib | stub | yes | no-op: the runtime sets these managers up itself |
| InitFonts | InterfaceLib | stub | yes | no-op: the runtime sets these managers up itself |
| InitGraf | InterfaceLib | full | yes |  |
| InitMenus | InterfaceLib | full | yes |  |
| InitWindows | InterfaceLib | full | yes |  |
| InitZone | InterfaceLib | full | yes | real heap zone in the given block (the segment cache); fixed M7 |
| InlineGetHandleSize | InterfaceLib | full |  |  |
| InsertMenu | InterfaceLib | full | yes |  |
| InsetRect | InterfaceLib | full | yes |  |
| InsTime | InterfaceLib | full |  | timing: see ANALYSIS.md §2.5 |
| InvalRect | InterfaceLib | full | yes |  |
| InvertRect | InterfaceLib | full | yes |  |
| KillPicture | InterfaceLib | full | yes |  |
| KillPoly | InterfaceLib | full | yes |  |
| LAddColumn | InterfaceLib | full |  |  |
| LAddRow | InterfaceLib | full | yes |  |
| LAddToCell | InterfaceLib | full | yes |  |
| LaunchApplication | InterfaceLib | stub |  | procNotFound: other applications are not launched; unused |
| LClick | InterfaceLib | full | yes |  |
| LDelColumn | InterfaceLib | full |  |  |
| LDelRow | InterfaceLib | full | yes |  |
| LDispose | InterfaceLib | full | yes |  |
| LGetCell | InterfaceLib | full | yes |  |
| LGetCellDataLocation | InterfaceLib | full | yes |  |
| LGetSelect | InterfaceLib | full | yes |  |
| Line | InterfaceLib | full | yes |  |
| LineTo | InterfaceLib | full | yes |  |
| LMGetDoubleTime | InterfaceLib | full | yes |  |
| LMGetGrayRgn | InterfaceLib | full | yes |  |
| LMGetHiliteMode | InterfaceLib | full | yes |  |
| LMGetMenuHook | InterfaceLib | full |  |  |
| LMGetSysFontFam | InterfaceLib | full | yes |  |
| LMGetSysFontSize | InterfaceLib | full | yes |  |
| LMGetWindowList | InterfaceLib | full | yes |  |
| LMSetHiliteMode | InterfaceLib | full | yes |  |
| LMSetLastSPExtra | InterfaceLib | full | yes |  |
| LMSetMBarHeight | InterfaceLib | full | yes |  |
| LMSetMenuHook | InterfaceLib | full |  |  |
| LMSetPaintWhite | InterfaceLib | full | yes |  |
| LMSetSysFontFam | InterfaceLib | full | yes |  |
| LMSetSysFontSize | InterfaceLib | full | yes |  |
| LNew | InterfaceLib | partial | yes | hasGrow ignored |
| LocalToGlobal | InterfaceLib | full | yes |  |
| LockPixels | InterfaceLib | stub | yes | GWorld pixels are never purged: always locked |
| LRect | InterfaceLib | full | yes |  |
| LScroll | InterfaceLib | full | yes |  |
| LSearch | InterfaceLib | full |  |  |
| LSetCell | InterfaceLib | full | yes |  |
| LSetDrawingMode | InterfaceLib | full | yes |  |
| LSetSelect | InterfaceLib | full | yes |  |
| LSize | InterfaceLib | full | yes |  |
| LUpdate | InterfaceLib | full | yes |  |
| MapPt | InterfaceLib | full |  |  |
| MaxApplZone | InterfaceLib | stub | yes | no-op: the heap never moves blocks to compact; master pointers are preallocated |
| MaxBlock | InterfaceLib | full |  |  |
| MaxMem | InterfaceLib | full | yes |  |
| MemError | InterfaceLib | full | yes |  |
| MenuKey | InterfaceLib | full | yes |  |
| MenuSelect | InterfaceLib | full | yes |  |
| ModalDialog | InterfaceLib | full | yes | Alert returns with the caller's port (fixed M7) |
| MoreMasters | InterfaceLib | stub | yes | no-op: the heap never moves blocks to compact; master pointers are preallocated |
| Move | InterfaceLib | full | yes |  |
| MoveControl | InterfaceLib | full | yes |  |
| MoveHHi | InterfaceLib | stub | yes | no-op: the heap never moves blocks to compact; master pointers are preallocated |
| MoveTo | InterfaceLib | full | yes |  |
| MoveWindow | InterfaceLib | full | yes |  |
| Munger | InterfaceLib | full |  |  |
| NewAlias | InterfaceLib | partial | yes | an alias is a stored FSSpec |
| NewControl | InterfaceLib | full | yes | standard buttons, check boxes, radio buttons, scroll bars, pop-up menus (CDEF 63, fixed M7), app CDEFs |
| NewCWindow | InterfaceLib | full | yes |  |
| NewGWorld | InterfaceLib | full | yes |  |
| NewHandle | InterfaceLib | full | yes |  |
| NewHandleClear | InterfaceLib | full | yes |  |
| NewMenu | InterfaceLib | full | yes |  |
| NewPalette | InterfaceLib | full | yes |  |
| NewPixPat | InterfaceLib | full | yes |  |
| NewPtr | InterfaceLib | full | yes |  |
| NewPtrClear | InterfaceLib | full | yes |  |
| NewRgn | InterfaceLib | full | yes |  |
| NewRoutineDescriptor | InterfaceLib | full | yes | PPC routine descriptors (68k code never runs) |
| NGetTrapAddress | InterfaceLib | partial | yes | fake addresses: every trap reads as implemented |
| NMRemove | InterfaceLib | stub |  | the application is always frontmost: no notifications |
| NumToString | InterfaceLib | full | yes |  |
| ObscureCursor | InterfaceLib | full | yes | hides the cursor until the mouse moves; fixed M7 |
| OffsetRect | InterfaceLib | full | yes |  |
| OffsetRgn | InterfaceLib | full | yes |  |
| OpColor | InterfaceLib | full | yes |  |
| OpenDefaultComponent | InterfaceLib | full | yes |  |
| OpenDeskAcc | InterfaceLib | stub |  | no desk accessories |
| OpenPicture | InterfaceLib | partial | yes | records text, lines and rects (what the game draws into pictures) |
| OpenPoly | InterfaceLib | full | yes |  |
| OpenPort | InterfaceLib | full | yes |  |
| OpenRgn | InterfaceLib | full |  |  |
| OSEventAvail | InterfaceLib | full | yes |  |
| PaintBehind | InterfaceLib | full | yes |  |
| PaintPoly | InterfaceLib | full | yes |  |
| PaintRect | InterfaceLib | full | yes |  |
| PaintRoundRect | InterfaceLib | full | yes |  |
| ParamText | InterfaceLib | full |  |  |
| PBCatSearchSync | InterfaceLib | stub |  | paramErr: no catalog search; unused |
| PBCloseSync | InterfaceLib | full |  |  |
| PBControlAsync | InterfaceLib | partial |  | video driver (refnum -50: gamma tables, VBL wait) only; other drivers controlErr (CD audio absent) |
| PBControlSync | InterfaceLib | partial | yes | video driver (refnum -50: gamma tables, VBL wait) only; other drivers controlErr (CD audio absent) |
| PBDTGetCommentSync | InterfaceLib | stub |  | no desktop database (Finder comments); unused |
| PBDTGetPath | InterfaceLib | stub |  | no desktop database (Finder comments); unused |
| PBDTOpenInform | InterfaceLib | stub |  | no desktop database (Finder comments); unused |
| PBDTSetCommentSync | InterfaceLib | stub |  | no desktop database (Finder comments); unused |
| PBFlushFileSync | InterfaceLib | full | yes |  |
| PBGetCatInfoSync | InterfaceLib | full | yes |  |
| PBGetEOFAsync | InterfaceLib | full |  |  |
| PBGetEOFSync | InterfaceLib | full | yes |  |
| PBGetFCBInfoSync | InterfaceLib | full | yes |  |
| PBHCopyFileSync | InterfaceLib | stub |  | paramErr: no server-side copy (PBHGetVolParms says so); MoreFiles copies by reading and writing |
| PBHCreateSync | InterfaceLib | full | yes |  |
| PBHDeleteSync | InterfaceLib | full |  |  |
| PBHGetFInfoSync | InterfaceLib | full | yes |  |
| PBHGetVInfoSync | InterfaceLib | full | yes |  |
| PBHGetVolParmsSync | InterfaceLib | full | yes |  |
| PBHOpenDenySync | InterfaceLib | full |  |  |
| PBHOpenDFSync | InterfaceLib | full | yes |  |
| PBHOpenRFDenySync | InterfaceLib | full |  |  |
| PBHOpenRFSync | InterfaceLib | full | yes |  |
| PBHOpenSync | InterfaceLib | full |  |  |
| PBHSetFLockSync | InterfaceLib | stub |  | file locking ignored; unused |
| PBReadAsync | InterfaceLib | full |  |  |
| PBReadSync | InterfaceLib | full | yes |  |
| PBSetCatInfoSync | InterfaceLib | full | yes |  |
| PBSetEOFAsync | InterfaceLib | full |  |  |
| PBSetEOFSync | InterfaceLib | full | yes |  |
| PBSetFPosAsync | InterfaceLib | full |  |  |
| PBSetFPosSync | InterfaceLib | full | yes |  |
| PBStatusSync | InterfaceLib | partial | yes | video driver (refnum -50: gamma tables, VBL wait) only; other drivers controlErr (CD audio absent) |
| PBWriteAsync | InterfaceLib | full |  |  |
| PBWriteSync | InterfaceLib | full | yes |  |
| PenMode | InterfaceLib | full | yes |  |
| PenNormal | InterfaceLib | full | yes |  |
| PenPat | InterfaceLib | full | yes |  |
| PenSize | InterfaceLib | full |  |  |
| PixPatChanged | InterfaceLib | full | yes | marks patXValid; patterns are expanded when drawn |
| PlotCIcon | InterfaceLib | full | yes |  |
| PlotCIconHandle | InterfaceLib | full | yes |  |
| PopUpMenuSelect | InterfaceLib | full | yes | menus taller than the screen scroll (fixed M7) |
| PortChanged | InterfaceLib | stub | yes | no-op: port fields are always read from guest memory |
| PrimeTime | InterfaceLib | full |  | timing: see ANALYSIS.md §2.5 |
| PtInRect | InterfaceLib | full | yes |  |
| PtInRgn | InterfaceLib | full | yes |  |
| PtrAndHand | InterfaceLib | full |  |  |
| PtrToHand | InterfaceLib | partial |  | new handle in the application zone, whatever the current zone |
| Random | InterfaceLib | full | yes |  |
| RectRgn | InterfaceLib | full | yes |  |
| ReleaseResource | InterfaceLib | full | yes |  |
| RemoveResource | InterfaceLib | full | yes |  |
| ResError | InterfaceLib | full | yes |  |
| ResolveAlias | InterfaceLib | partial | yes | an alias is a stored FSSpec |
| RestoreDeviceClut | InterfaceLib | full | yes |  |
| RGB2HSL | InterfaceLib | full | yes |  |
| RGBBackColor | InterfaceLib | full | yes |  |
| RGBForeColor | InterfaceLib | full | yes |  |
| RmvTime | InterfaceLib | full |  | timing: see ANALYSIS.md §2.5 |
| ScalePt | InterfaceLib | full |  |  |
| SectRect | InterfaceLib | full | yes |  |
| SectRgn | InterfaceLib | full | yes |  |
| SeedFill | InterfaceLib | full | yes |  |
| SelectWindow | InterfaceLib | full | yes |  |
| SendBehind | InterfaceLib | full | yes |  |
| SetA5 | InterfaceLib | stub |  | no A5 world on PowerPC |
| SetCCursor | InterfaceLib | full | yes |  |
| SetClip | InterfaceLib | full | yes |  |
| SetControlAction | InterfaceLib | full | yes |  |
| SetControlMaximum | InterfaceLib | full | yes |  |
| SetControlMinimum | InterfaceLib | full |  |  |
| SetControlReference | InterfaceLib | full | yes |  |
| SetControlValue | InterfaceLib | full | yes |  |
| SetCurrentA5 | InterfaceLib | stub |  | no A5 world on PowerPC |
| SetCursor | InterfaceLib | full | yes |  |
| SetDefaultOutputVolume | InterfaceLib | audio | yes | audio (sound.c / music.c): inventoried only |
| SetDepth | InterfaceLib | partial |  | the screen is 8-bit only, which is what the game asks for |
| SetDialogItem | InterfaceLib | full | yes |  |
| SetEntries | InterfaceLib | full | yes |  |
| SetEntryUsage | InterfaceLib | full | yes |  |
| SetEOF | InterfaceLib | full | yes |  |
| SetEventMask | InterfaceLib | full | yes | filters what the OS event calls return (the game allows everything, keyUp included) |
| SetFPos | InterfaceLib | full | yes |  |
| SetGDevice | InterfaceLib | full | yes |  |
| SetGrowZone | InterfaceLib | stub | yes | ignored: the application zone (175 MB) never runs out, so the grow-zone proc would never be called |
| SetGWorld | InterfaceLib | full | yes |  |
| SetHandleSize | InterfaceLib | full | yes |  |
| SetItemMark | InterfaceLib | full | yes |  |
| SetMenuItemText | InterfaceLib | full | yes |  |
| SetOrigin | InterfaceLib | full | yes |  |
| SetPalette | InterfaceLib | full | yes |  |
| SetPenState | InterfaceLib | full | yes |  |
| SetPixelsState | InterfaceLib | stub | yes | no pixel state kept (never purgeable, lock state not reported); only MyCopyGWorld uses them |
| SetPort | InterfaceLib | full | yes |  |
| SetRect | InterfaceLib | full | yes |  |
| SetResLoad | InterfaceLib | full |  |  |
| SetWRefCon | InterfaceLib | full | yes |  |
| SetWTitle | InterfaceLib | full | yes |  |
| SetZone | InterfaceLib | full | yes |  |
| ShowControl | InterfaceLib | full |  |  |
| ShowCursor | InterfaceLib | full | yes |  |
| ShowHide | InterfaceLib | full | yes |  |
| ShowPen | InterfaceLib | full | yes |  |
| ShowWindow | InterfaceLib | full | yes |  |
| SizeControl | InterfaceLib | full |  |  |
| SizeWindow | InterfaceLib | partial | yes | fUpdate ignored |
| SndDisposeChannel | InterfaceLib | audio | yes | audio (sound.c / music.c): inventoried only |
| SndNewChannel | InterfaceLib | audio | yes | audio (sound.c / music.c): inventoried only |
| SndPlayDoubleBuffer | InterfaceLib | audio | yes | audio (sound.c / music.c): inventoried only |
| SndSoundManagerVersion | InterfaceLib | audio | yes | audio (sound.c / music.c): inventoried only |
| StandardGetFile | InterfaceLib | partial |  | Saved Games folder only, file filter ignored (the game passes none) |
| StandardPutFile | InterfaceLib | partial | yes | always saves in the Saved Games folder; 31-character names |
| StillDown | InterfaceLib | full | yes |  |
| StopAlert | InterfaceLib | full |  | Alert returns with the caller's port (fixed M7) |
| StringWidth | InterfaceLib | full | yes |  |
| StyledLineBreak | InterfaceLib | full | yes |  |
| SysBeep | InterfaceLib | audio | yes | audio (sound.c / music.c): inventoried only |
| SystemClick | InterfaceLib | stub |  | no desk accessories |
| SystemEdit | InterfaceLib | stub |  | no desk accessories |
| SystemTask | InterfaceLib | full |  |  |
| TEActivate | InterfaceLib | full | yes |  |
| TECalText | InterfaceLib | full | yes |  |
| TEClick | InterfaceLib | full |  |  |
| TEDeactivate | InterfaceLib | full | yes |  |
| TEDelete | InterfaceLib | full | yes |  |
| TEDispose | InterfaceLib | full | yes |  |
| TEGetText | InterfaceLib | full | yes |  |
| TEIdle | InterfaceLib | full | yes |  |
| TEInit | InterfaceLib | stub | yes | no-op: the runtime sets these managers up itself |
| TEInsert | InterfaceLib | full | yes |  |
| TEKey | InterfaceLib | full | yes | up/down arrows fixed M7 |
| TempNewHandle | InterfaceLib | full |  |  |
| TENew | InterfaceLib | full | yes |  |
| TESetSelect | InterfaceLib | full | yes |  |
| TESetText | InterfaceLib | full | yes |  |
| TestControl | InterfaceLib | full | yes |  |
| TestDeviceAttribute | InterfaceLib | full | yes |  |
| TEStyleInsert | InterfaceLib | full | yes |  |
| TEStyleNew | InterfaceLib | full | yes |  |
| TETextBox | InterfaceLib | full | yes |  |
| TEUpdate | InterfaceLib | full | yes |  |
| TextFace | InterfaceLib | full | yes |  |
| TextFont | InterfaceLib | full | yes |  |
| TextMode | InterfaceLib | full | yes |  |
| TextSize | InterfaceLib | full | yes |  |
| TextWidth | InterfaceLib | full | yes |  |
| TickCount | InterfaceLib | full | yes | timing: see ANALYSIS.md §2.5 |
| TrackBox | InterfaceLib | full |  |  |
| TrackControl | InterfaceLib | partial | yes | generic tracking loop (app CDEFs are not asked to autoTrack); pop-up menus pop up |
| TrackGoAway | InterfaceLib | full | yes |  |
| TruncString | InterfaceLib | full |  |  |
| UnionRect | InterfaceLib | full | yes |  |
| UnionRgn | InterfaceLib | full | yes |  |
| UniqueID | InterfaceLib | full | yes |  |
| UnlockPixels | InterfaceLib | stub | yes | GWorld pixels are never purged: always locked |
| UnmountVol | InterfaceLib | stub |  | the volume cannot be unmounted (fBsyErr); unused |
| UpdateGWorld | InterfaceLib | full |  |  |
| UpdateResFile | InterfaceLib | full | yes |  |
| UpperString | InterfaceLib | full |  |  |
| UseResFile | InterfaceLib | full | yes |  |
| ValidRect | InterfaceLib | full | yes |  |
| ValidRgn | InterfaceLib | full | yes |  |
| VisibleLength | InterfaceLib | full | yes |  |
| WaitMouseUp | InterfaceLib | full | yes |  |
| WaitNextEvent | InterfaceLib | full | yes | sleep and mouse region honoured |
| WriteResource | InterfaceLib | full | yes |  |
| ZoomWindow | InterfaceLib | full |  |  |
| GetCurrentThread | ThreadsLib | full | yes |  |
| GetThreadState | ThreadsLib | full | yes |  |
| NewThread | ThreadsLib | full | yes |  |
| SetThreadScheduler | ThreadsLib | full | yes |  |
| SetThreadState | ThreadsLib | full | yes |  |
| YieldToAnyThread | ThreadsLib | full | yes |  |
| YieldToThread | ThreadsLib | full |  |  |
| AddFilePreview | QuickTimeLib | full | yes | PICT + 'pnot' in the file's resource fork; fixed M7 |
| EnterMovies | QuickTimeLib | stub |  | nothing to initialise |
| MakeFilePreview | QuickTimeLib | stub | yes | previews movies/pictures only; a saved game is neither (the game adds its own thumbnail) |
| MakeThumbnailFromPixMap | QuickTimeLib | full | yes | box-filtered 8-bit PICT, longer side 80 px; fixed M7 |
| NADisposeNoteChannel | QuickTimeLib | audio |  | audio (sound.c / music.c): inventoried only |
| NANewNoteChannel | QuickTimeLib | audio |  | audio (sound.c / music.c): inventoried only |
| NAPlayNote | QuickTimeLib | audio |  | audio (sound.c / music.c): inventoried only |
| NAStuffToneDescription | QuickTimeLib | audio |  | audio (sound.c / music.c): inventoried only |
| StandardGetFilePreview | QuickTimeLib | partial | yes | Saved Games folder only, file filter ignored (the game passes none); shows the preview; fixed M7 |
| TuneGetStatus | QuickTimeLib | audio | yes | audio (sound.c / music.c): inventoried only |
| TunePreroll | QuickTimeLib | audio | yes | audio (sound.c / music.c): inventoried only |
| TuneQueue | QuickTimeLib | audio | yes | audio (sound.c / music.c): inventoried only |
| TuneSetHeader | QuickTimeLib | audio | yes | audio (sound.c / music.c): inventoried only |
| TuneSetTimeScale | QuickTimeLib | audio | yes | audio (sound.c / music.c): inventoried only |
| TuneSetVolume | QuickTimeLib | audio | yes | audio (sound.c / music.c): inventoried only |
| TuneStop | QuickTimeLib | audio | yes | audio (sound.c / music.c): inventoried only |
| TuneUnroll | QuickTimeLib | audio | yes | audio (sound.c / music.c): inventoried only |
| SndGetInfo | SoundLib | audio | yes | audio (sound.c / music.c): inventoried only |
| num2dec | MathLib | full |  | FIXEDDECIMAL and FLOATDECIMAL |
| (15 calls) | AppearanceLib | absent |  | weak, library absent: resolves to NULL, the game uses its System 7 widgets (T7Adapter); the TRAPs in controls.c/menus.c/windows.c are never bound |
| (3 calls) | ContextualMenu | absent |  | weak, library absent: the game has its own hold-to-open menus |
| (2 calls) | ControlStripLib | absent |  | weak, library absent |
| (10 calls) | InputSprocketLib | absent |  | weak, library absent: no joystick support |
| (9 calls) | NavigationLib | absent |  | weak, library absent: Standard File is used |
