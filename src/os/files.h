/* File Manager internal API. */
#ifndef FILES_H
#define FILES_H
#include "os.h"

#define VOL_REFNUM   (-1)
#define ROOT_DIRID   2

void files_init(const char *root, const char *sysdir);

/* Resolve a (vRefNum, dirID, name) triple.  On success fills host path;
   returns noErr, fnfErr (dir exists, file missing) or dirNFErr/bdNamErr.
   *out_dir receives the directory ID and out_name the leaf Mac name. */
int vfs_resolve(s16 vref, s32 dirid, const char *macname, char *hostpath, size_t hsz,
                s32 *out_dir, char *out_name);
/* Host path of a directory, or NULL. */
const char *vfs_dir_path(s32 dirid);
s32 vfs_dir_parent(s32 dirid);
const char *vfs_dir_name(s32 dirid);
s32 vfs_dir_id_for(const char *hostpath, s32 parent, const char *macname);
bool vfs_is_dir(const char *hostpath);
bool vfs_exists(const char *hostpath);

/* FSSpec helpers (guest address of a 70-byte FSSpec) */
void fsspec_read(u32 spec, s16 *vref, s32 *parid, char *name);
void fsspec_write(u32 spec, s16 vref, s32 parid, const char *name);
int fsspec_hostpath(u32 spec, char *hostpath, size_t hsz);

/* Finder info */
void finfo_get(const char *hostpath, u32 *type, u32 *creator, u16 *flags);
void finfo_set(const char *hostpath, u32 type, u32 creator, u16 flags);

/* Mac Roman <-> UTF-8 */
void macroman_to_utf8(const char *in, char *out, size_t outsz);
void utf8_to_macroman(const char *in, char *out, size_t outsz);
bool mac_names_equal(const char *a, const char *b);

/* Host path of resource fork sidecar */
void rsrc_path(const char *datapath, char *out, size_t outsz);

/* Mac time (seconds since 1904) */
u32 mac_time_now(void);
u32 mac_time_from_unix(s64 t);

/* Open fork table (shared by the Resource Manager for refnums) */
int fcb_open(const char *hostpath, bool rsrc_fork, int perm, s16 *refnum);
int fcb_close(s16 refnum);
s16 fcb_alloc_refnum(void);
void fcb_free_refnum(s16 refnum);
#endif
