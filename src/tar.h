#pragma once

#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>

/*
 * Minimal gzip-compressed tar reader/writer.
 *
 * Reads POSIX ustar archives with pax ('x'/'g') extension headers and GNU
 * large-size / long-name ('L'/'K') entries. Writes ustar headers, emitting pax
 * records only when a value does not fit the header, plus any pax records the
 * reader kept but does not interpret (xattrs, atime, creation time, ...), so a
 * read-then-write round trip keeps the entry's metadata.
 *
 * Depends only on zlib.
 */

typedef enum {
    TAR_FILE,
    TAR_DIR,
    TAR_SYMLINK,
    TAR_HARDLINK,
    TAR_OTHER
} tar_type;

typedef struct {
    char          path[PATH_MAX];   /* entry path (no leading slash normalization) */
    char          link[PATH_MAX];   /* symlink or hard link target */
    tar_type      type;
    char          typeflag;         /* raw ustar type; 0 derives it from type */
    mode_t        mode;             /* full mode incl. type bits */
    uid_t         uid;
    gid_t         gid;
    char          uname[33];
    char          gname[33];
    off_t         size;             /* data size (files only) */
    time_t        mtime;
    long          mtime_nsec;
    unsigned long devmajor;         /* Anka layers store the change kind here */
    unsigned long devminor;
    /* Uninterpreted pax records ("len key=value\n" ...). Entries returned by
     * tar_read_next point into the reader, valid until the next read. */
    const char   *pax_extra;
    size_t        pax_extra_len;
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
/* Like tar_wopen on an open descriptor. On success the writer owns fd. */
tar_t  *tar_wdopen(int fd, int level);
/* Fails with EFTYPE for an entry that carries data but is not a regular file
 * (for example GNU sparse), since its layout cannot be rewritten faithfully. */
int     tar_write_header(tar_t *t, const tar_entry *e);
/* Writes len bytes of the current file entry's data. Returns len or -1. */
ssize_t tar_write_data(tar_t *t, const void *buf, size_t len);

/* Closes a reader or writer (writer emits the end-of-archive marker). */
int     tar_close(tar_t *t);
