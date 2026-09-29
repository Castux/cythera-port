/* Host platform shims: the few POSIX calls the runtime uses, for Windows
   (MinGW-w64). Paths use '/', which Windows accepts. */
#ifndef PLAT_H
#define PLAT_H
#include <stdlib.h>
#include <limits.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#ifndef PATH_MAX
#define PATH_MAX 260
#endif
#define plat_mkdir(p) _mkdir(p)
static inline char *plat_realpath(const char *p, char *out) { return _fullpath(out, p, PATH_MAX); }
/* per-user data directory */
static inline const char *plat_home(void) {
    const char *h = getenv("APPDATA");
    return h ? h : getenv("USERPROFILE");
}
#else
#include <unistd.h>
#define plat_mkdir(p) mkdir((p), 0755)
static inline char *plat_realpath(const char *p, char *out) { return realpath(p, out); }
static inline const char *plat_home(void) { return getenv("HOME"); }
#endif

#endif
