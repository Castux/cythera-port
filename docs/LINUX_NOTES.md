# Linux portability notes

Found while trying the build on Linux early (WSL2, Ubuntu 24.04, gcc 13.3,
glibc, SDL2 via WSLg), at commit `675f8e1`. Related to the PLAN.md
"Portability: Linux/Windows builds" item.

## 1. Startup abort: `*** buffer overflow detected ***` (fix required)

### Symptom

```
$ make all && ./build/cythera
[I] symbols: 1985 functions from traceback tables
[I] loaded code 0xcd280 bytes @00100000, data 0x7934e bytes @00200000, 563 imports
*** buffer overflow detected ***: terminated
Aborted (core dumped)
```

### Cause

`files_init()` in `src/os/files.c` calls `realpath()` with destination buffers
smaller than `PATH_MAX`:

```c
char abs[1024];                               /* line ~467 */
if (!realpath(root, abs)) ...
...
char sabs[1100];                              /* line ~481 */
if (!realpath(saves, sabs)) ...
```

Ubuntu's gcc turns on `_FORTIFY_SOURCE` by default at `-O2`. glibc's fortified
`realpath()` (`__realpath_chk`) aborts **whenever** the destination's known
size is `< PATH_MAX`, whatever path it actually resolves. `PATH_MAX` is 4096
on Linux and 1024 on macOS, so on macOS `abs[1024]` is exactly big enough,
which is why the bug never shows up there.

This is a fortify check, not real memory corruption: an ASan+UBSan build with
`-D_FORTIFY_SOURCE=0` runs cleanly past this point.

### Fix

In `src/os/files.c`, size both buffers with `PATH_MAX` (add
`#include <limits.h>` if it isn't already pulled in; it built fine without it
on glibc):

```c
char abs[PATH_MAX];
...
char sabs[PATH_MAX];
```

Those are the only two `realpath` calls in `src/`. Any `realpath` added later
must also get a `PATH_MAX` buffer, or use `realpath(p, NULL)` + `free()`.

### Verified

With only this change, the normal `make all` build on WSL2 gets through
`volume root ...`, loads the SoundFont, starts the PPC code, handles
`aevt/oapp` and keeps running until the process is killed. (An exit on
SIGTERM shows up as `aevt/quit` → `ExitToShell`, because SDL turns SIGTERM
into `SDL_QUIT`. That's expected, not a bug.)

## 2. Minor observations (no action needed now)

- UBSan warns in the third-party SoundFont lib:
  `third_party/tsf.h:627: left shift of negative value -1`. It's harmless on
  gcc/clang; ignore it unless we patch tsf.
- `[I] GetSharedLibrary(StdCLib) -> not found` shows up on Linux too, so it
  isn't platform-specific.
- Fonts fall back to DejaVu (`/usr/share/fonts/truetype/dejavu/...`) on
  Ubuntu. The substitution code already handles this.
- Build tip: `make CFLAGS=...` on the command line **replaces** the Makefile's
  `CFLAGS += ... $(SDL_CFLAGS)` (so `SDL.h` isn't found). Pass extra flags
  through the environment instead, e.g.
  `CFLAGS="-O1 -g -fsanitize=address,undefined" LDFLAGS="-fsanitize=address,undefined" make`.
  You could also change the Makefile to `override CFLAGS += ...`.
