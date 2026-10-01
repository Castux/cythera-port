/* Host platform shims: the few POSIX calls the runtime uses, for Windows
   (MinGW-w64). Paths use '/', which Windows accepts. */
#ifndef PLAT_H
#define PLAT_H
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#ifndef PATH_MAX
#define PATH_MAX 260
#endif
#define plat_mkdir(p) _mkdir(p)
#define PLAT_NULL_DEVICE "NUL"
static inline char *plat_realpath(const char *p, char *out) { return _fullpath(out, p, PATH_MAX); }
/* per-user data directory */
static inline const char *plat_home(void) {
    const char *h = getenv("APPDATA");
    return h ? h : getenv("USERPROFILE");
}
static inline int plat_ftruncate(int fd, long long len) { return _chsize_s(fd, len); }
/* rename() that replaces an existing target, as on POSIX */
static inline int plat_rename_replace(const char *from, const char *to) {
    if (rename(from, to) == 0) return 0;
    _unlink(to);
    return rename(from, to);
}
/* local time's offset from UTC, in seconds, at time t */
static inline long plat_gmtoff(time_t t) {
    struct tm lt;
    if (localtime_s(&lt, &t)) return 0;
    return (long)(_mkgmtime(&lt) - t);
}
#else
#include <unistd.h>
#define plat_mkdir(p) mkdir((p), 0755)
#define PLAT_NULL_DEVICE "/dev/null"
static inline char *plat_realpath(const char *p, char *out) { return realpath(p, out); }
static inline const char *plat_home(void) { return getenv("HOME"); }
static inline int plat_ftruncate(int fd, long long len) { return ftruncate(fd, (off_t)len); }
#define plat_rename_replace(from, to) rename((from), (to))
static inline long plat_gmtoff(time_t t) {
    struct tm lt;
    localtime_r(&t, &lt);
    return lt.tm_gmtoff;
}
#endif

#endif
