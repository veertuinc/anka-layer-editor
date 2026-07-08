#pragma once

#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>

/*
 * Minimal gzip-compressed tar reader/writer.
 *
 * Reads POSIX ustar archives with pax ('x'/'g') extension headers and GNU
 * large-size / long-name ('L'/'K') entries. Writes ustar headers, emitting pax
 * records only when a path/link is too long, and base-256 sizes for files that
 * exceed the octal field. This matches what Anka's importer accepts.
 *
 * Depends only on zlib.
 */

typedef enum {
    TAR_FILE,
    TAR_DIR,
    TAR_SYMLINK,
    TAR_OTHER
} tar_type;

typedef struct {
    char     path[PATH_MAX];   /* entry path (no leading slash normalization) */
    char     link[PATH_MAX];   /* symlink target (valid when type == TAR_SYMLINK) */
    tar_type type;
    mode_t   mode;             /* full mode incl. type bits */
    uid_t    uid;
    gid_t    gid;
    off_t    size;             /* data size (files only) */
    time_t   mtime;
} tar_entry;

typedef struct tar tar_t;

/* Reading. */
tar_t  *tar_ropen(const char *path);
/* Returns 1 when an entry is read, 0 at end of archive, -1 on error. */
int     tar_read_next(tar_t *t, tar_entry *e);
/* Reads up to len bytes of the current entry's data; 0 at entry end, -1 error. */
ssize_t tar_read_data(tar_t *t, void *buf, size_t len);
/* Discards any unread data of the current entry. */
int     tar_skip_data(tar_t *t);

/* Writing. level is a zlib compression level 0-9 (6 is a good default). */
tar_t  *tar_wopen(const char *path, int level);
int     tar_write_header(tar_t *t, const tar_entry *e);
/* Writes len bytes of the current file entry's data. Returns len or -1. */
ssize_t tar_write_data(tar_t *t, const void *buf, size_t len);

/* Closes a reader or writer (writer emits the end-of-archive marker). */
int     tar_close(tar_t *t);
