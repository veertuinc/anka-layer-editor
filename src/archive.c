#include "archive.h"
#include "staged_write.h"
#include "tar.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <libgen.h>
#include <sys/stat.h>

#define COPY_BUF (1 << 16)

int archive_host_meta(const char *host_path, le_meta *meta, char *link,
                      size_t linklen, char *err, size_t errlen) {
    struct stat st;
    if (lstat(host_path, &st) == -1) {
        le_seterr(err, errlen, "%s: %s", host_path, strerror(errno));
        return -1;
    }
    memset(meta, 0, sizeof(*meta));
    meta->mode = st.st_mode;
    meta->uid = st.st_uid;
    meta->gid = st.st_gid;
    if (S_ISREG(st.st_mode)) {
        meta->kind = LE_KIND_FILE;
        meta->size = st.st_size;
        return 0;
    }
    if (!S_ISLNK(st.st_mode)) {
        le_seterr(err, errlen, "%s: not a regular file or symlink", host_path);
        return -1;
    }
    ssize_t l = readlink(host_path, link, linklen - 1);
    if (l == -1) {
        le_seterr(err, errlen, "%s: %s", host_path, strerror(errno));
        return -1;
    }
    link[l] = '\0';
    meta->kind = LE_KIND_SYMLINK;
    meta->link = link;
    return 0;
}

static le_entry_kind kind_for_tar_type(tar_type type) {
    switch (type) {
    case TAR_FILE:     return LE_KIND_FILE;
    case TAR_DIR:      return LE_KIND_DIR;
    case TAR_SYMLINK:  return LE_KIND_SYMLINK;
    case TAR_HARDLINK: return LE_KIND_HARDLINK;
    default:           return LE_KIND_OTHER;
    }
}

static le_change_kind change_kind_for_devmajor(unsigned long devmajor) {
    switch (devmajor) {
    case LE_CHANGE_ADDED:
    case LE_CHANGE_MODIFIED:
    case LE_CHANGE_WHITEOUT:
        return (le_change_kind)devmajor;
    default:
        return LE_CHANGE_UNTAGGED;
    }
}

static int is_archive_root(const char *path) {
    while (*path == '/')
        path++;
    return path[0] == '\0' || (path[0] == '.' && path[1] == '\0');
}

int archive_load_tar(tar_t *in, const char *source_label, le_model *m,
                     int arch_idx, char *err, size_t errlen) {
    if (arch_idx < 0 || arch_idx >= m->narchives) {
        le_seterr(err, errlen, "%s: invalid archive slot %d", source_label,
                  arch_idx);
        return -1;
    }
    tar_entry e;
    int r;
    while ((r = tar_read_next(in, &e)) == 1) {
        if (!is_archive_root(e.path)) {
            int has_link = e.type == TAR_SYMLINK || e.type == TAR_HARDLINK;
            le_meta meta = {
                .kind = kind_for_tar_type(e.type),
                .size = e.size,
                .mode = e.mode,
                .uid = e.uid,
                .gid = e.gid,
                .link = has_link ? e.link : NULL,
            };
            if (!model_insert(m, e.path, arch_idx, &meta,
                              change_kind_for_devmajor(e.devmajor))) {
                le_seterr(err, errlen, "%s: failed to insert path '%s'",
                          source_label, e.path);
                return -1;
            }
        }
        if (tar_skip_data(in) == -1) {
            le_seterr(err, errlen, "%s: read error in '%s'", source_label,
                      e.path);
            return -1;
        }
    }
    if (r < 0) {
        le_seterr(err, errlen, "%s: read error: %s", source_label,
                  strerror(errno));
        return -1;
    }
    return 0;
}

int archive_load(const char *path, le_model *m, int arch_idx,
                 char *err, size_t errlen) {
    tar_t *in = tar_ropen(path);
    if (!in) {
        le_seterr(err, errlen, "%s: cannot open: %s", path, strerror(errno));
        return -1;
    }
    int rc = archive_load_tar(in, path, m, arch_idx, err, errlen);
    tar_close(in);
    return rc;
}

/* Copy the current reader entry's data into the writer. */
static int copy_data(tar_t *in, tar_t *out) {
    char buf[COPY_BUF];
    ssize_t n;
    while ((n = tar_read_data(in, buf, sizeof(buf))) > 0)
        if (tar_write_data(out, buf, (size_t)n) == -1)
            return -1;
    return n < 0 ? -1 : 0;
}

/* Skip an original member when it is marked removed (incl. via ancestor) or
 * replaced by a staged addition at the same path, in this archive. */
static int should_skip_original(const le_node *n, int arch_idx) {
    return n && (node_effectively_removed(n, arch_idx) ||
                 n->arch[arch_idx].staged);
}

/* Create a private temp file beside path, carrying path's mode and owner, so
 * the rename keeps them. Returns the fd, or -1 with err set. */
static int open_temp_beside(const char *path, char *tmp, size_t tmplen,
                            char *err, size_t errlen) {
    char dirbuf[PATH_MAX], basebuf[PATH_MAX];
    strlcpy(dirbuf, path, sizeof(dirbuf));
    strlcpy(basebuf, path, sizeof(basebuf));
    int wrote = snprintf(tmp, tmplen, "%s/.%s.XXXXXX", dirname(dirbuf),
                         basename(basebuf));
    if (wrote < 0 || (size_t)wrote >= tmplen) {
        le_seterr(err, errlen, "%s: path too long", path);
        return -1;
    }
    struct stat st;
    if (stat(path, &st) == -1) {
        le_seterr(err, errlen, "%s: %s", path, strerror(errno));
        return -1;
    }
    int fd = mkstemp(tmp);
    if (fd == -1) {
        le_seterr(err, errlen, "%s: cannot create temp: %s", tmp, strerror(errno));
        return -1;
    }
    (void)!fchown(fd, st.st_uid, st.st_gid);    /* best effort when not owner */
    if (fchmod(fd, st.st_mode & 07777) == -1) {
        le_seterr(err, errlen, "%s: %s", tmp, strerror(errno));
        close(fd);
        unlink(tmp);
        return -1;
    }
    return fd;
}

/* Stream the original into out, keeping entries not removed or replaced. */
static int copy_kept_entries(const char *path, le_model *m, int arch_idx,
                             tar_t *out, char *err, size_t errlen) {
    tar_t *in = tar_ropen(path);
    if (!in) {
        le_seterr(err, errlen, "%s: cannot reopen: %s", path, strerror(errno));
        return -1;
    }
    tar_entry e;
    int r = 0, failed = 0;
    while (!failed && (r = tar_read_next(in, &e)) == 1) {
        if (should_skip_original(model_lookup(m, e.path), arch_idx)) {
            if (tar_skip_data(in) == -1) {
                le_seterr(err, errlen, "%s: skip error", path);
                failed = 1;
            }
            continue;
        }
        if (tar_write_header(out, &e) == -1) {
            if (e.type != TAR_FILE && e.size > 0)
                le_seterr(err, errlen,
                          "%s: cannot rewrite '%s' (type '%c' with data)",
                          path, e.path, e.typeflag);
            else
                le_seterr(err, errlen, "%s: write error: %s", e.path,
                          strerror(errno));
            failed = 1;
        } else if (e.type == TAR_FILE ? copy_data(in, out) == -1
                                      : tar_skip_data(in) == -1) {
            le_seterr(err, errlen, "%s: copy error at '%s'", path, e.path);
            failed = 1;
        }
    }
    if (!failed && r < 0) {
        le_seterr(err, errlen, "%s: read error: %s", path, strerror(errno));
        failed = 1;
    }
    tar_close(in);
    return failed ? -1 : 0;
}

int archive_save(const char *path, le_model *m, int arch_idx,
                 char *err, size_t errlen) {
    if (arch_idx < 0 || arch_idx >= m->narchives) {
        le_seterr(err, errlen, "%s: invalid archive slot %d", path, arch_idx);
        return -1;
    }

    char tmp[PATH_MAX];
    int fd = open_temp_beside(path, tmp, sizeof(tmp), err, errlen);
    if (fd == -1)
        return -1;
    int writer_fd = dup(fd);
    tar_t *out = writer_fd == -1 ? NULL : tar_wdopen(writer_fd, 6);
    if (!out) {
        le_seterr(err, errlen, "%s: cannot open writer: %s", tmp, strerror(errno));
        if (writer_fd != -1)
            close(writer_fd);
        close(fd);
        unlink(tmp);
        return -1;
    }

    int failed = copy_kept_entries(path, m, arch_idx, out, err, errlen) == -1 ||
                 staged_write_all(out, m->root, arch_idx, err, errlen) == -1;
    if (tar_close(out) == -1 && !failed) {
        le_seterr(err, errlen, "%s: finalize error", tmp);
        failed = 1;
    }
    if (!failed && fsync(fd) == -1) {
        le_seterr(err, errlen, "%s: fsync: %s", tmp, strerror(errno));
        failed = 1;
    }
    close(fd);
    if (!failed && rename(tmp, path) == -1) {
        le_seterr(err, errlen, "%s -> %s: %s", tmp, path, strerror(errno));
        failed = 1;
    }
    if (failed) {
        unlink(tmp);
        return -1;
    }

    model_reconcile_after_save(m, arch_idx);
    return 0;
}
