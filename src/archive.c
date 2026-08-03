#include "archive.h"
#include "tar.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <libgen.h>
#include <sys/stat.h>

#define COPY_BUF (1 << 16)

int archive_load(const char *path, le_model *m, int arch_idx,
                 char *err, size_t errlen) {
    if (arch_idx < 0 || arch_idx >= m->narchives) {
        le_seterr(err, errlen, "%s: invalid archive slot %d", path, arch_idx);
        return -1;
    }

    tar_t *in = tar_ropen(path);
    if (!in) {
        le_seterr(err, errlen, "%s: cannot open: %s", path, strerror(errno));
        return -1;
    }

    tar_entry e;
    int r;
    while ((r = tar_read_next(in, &e)) == 1) {
        int is_dir = (e.type == TAR_DIR);
        int is_sym = (e.type == TAR_SYMLINK);
        /* Root / empty paths are skipped by model_insert (NULL is OK). */
        const char *p = e.path;
        while (*p == '/')
            p++;
        int skip_insert = (p[0] == '\0' || (p[0] == '.' && p[1] == '\0'));
        if (!skip_insert) {
            le_node *n = model_insert(m, e.path, arch_idx, is_dir, is_sym,
                                      e.size, e.mode, is_sym ? e.link : NULL);
            if (!n) {
                le_seterr(err, errlen, "%s: failed to insert path '%s'", path,
                          e.path);
                tar_close(in);
                return -1;
            }
        }
        if (tar_skip_data(in) == -1) {
            le_seterr(err, errlen, "%s: skip error", path);
            tar_close(in);
            return -1;
        }
    }

    tar_close(in);
    if (r < 0) {
        le_seterr(err, errlen, "%s: read error", path);
        return -1;
    }
    return 0;
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

/* Write one staged addition (regular file or symlink) from the host. */
static int write_addition(tar_t *out, const le_node *n, int arch_idx,
                          char *err, size_t errlen) {
    const char *host_path = n->arch[arch_idx].host_path;
    char guest[PATH_MAX];
    if (node_path(n, guest, sizeof(guest)) == -1) {
        le_seterr(err, errlen, "path too deep for node '%s'", n->name);
        return -1;
    }

    struct stat st;
    if (lstat(host_path, &st) == -1) {
        le_seterr(err, errlen, "%s: %s", host_path, strerror(errno));
        return -1;
    }

    tar_entry e;
    memset(&e, 0, sizeof(e));
    strlcpy(e.path, guest, sizeof(e.path));
    e.mode = st.st_mode;
    e.uid = st.st_uid;
    e.gid = st.st_gid;
    e.mtime = st.st_mtime;

    if (S_ISLNK(st.st_mode)) {
        char tgt[PATH_MAX];
        ssize_t l = readlink(host_path, tgt, sizeof(tgt) - 1);
        if (l == -1) {
            le_seterr(err, errlen, "%s: %s", host_path, strerror(errno));
            return -1;
        }
        tgt[l] = '\0';
        e.type = TAR_SYMLINK;
        strlcpy(e.link, tgt, sizeof(e.link));
        return tar_write_header(out, &e);
    }

    if (!S_ISREG(st.st_mode)) {
        le_seterr(err, errlen, "%s: not a regular file or symlink", host_path);
        return -1;
    }

    e.type = TAR_FILE;
    e.size = st.st_size;
    if (tar_write_header(out, &e) == -1)
        return -1;

    FILE *f = fopen(host_path, "rb");
    if (!f) {
        le_seterr(err, errlen, "%s: %s", host_path, strerror(errno));
        return -1;
    }
    char buf[COPY_BUF];
    size_t rd;
    off_t written = 0;
    int rc = 0;
    while ((rd = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (tar_write_data(out, buf, rd) == -1) {
            rc = -1;
            break;
        }
        written += (off_t)rd;
    }
    if (rc == 0 && ferror(f))
        rc = -1;
    fclose(f);
    if (rc == 0 && written != st.st_size) {
        le_seterr(err, errlen, "%s: size changed during read (%lld -> %lld)",
                  host_path, (long long)st.st_size, (long long)written);
        return -1;
    }
    return rc;
}

static int append_additions(tar_t *out, le_node *node, int arch_idx,
                            char *err, size_t errlen) {
    for (size_t i = 0; i < node->nchildren; i++) {
        le_node *c = node->children[i];
        if (node_effectively_removed(c, arch_idx))
            continue;
        if (c->arch[arch_idx].added && c->arch[arch_idx].host_path &&
            write_addition(out, c, arch_idx, err, errlen) == -1)
            return -1;
        if (append_additions(out, c, arch_idx, err, errlen) == -1)
            return -1;
    }
    return 0;
}

/* Skip an original member when it is marked removed (incl. via ancestor) or
 * replaced by a staged host addition at the same path, in this archive. */
static int should_skip_original(const le_node *n, int arch_idx) {
    return n && (node_effectively_removed(n, arch_idx) ||
                 n->arch[arch_idx].added);
}

int archive_save(const char *path, le_model *m, int arch_idx,
                 char *err, size_t errlen) {
    if (arch_idx < 0 || arch_idx >= m->narchives) {
        le_seterr(err, errlen, "%s: invalid archive slot %d", path, arch_idx);
        return -1;
    }

    /* Temp file in the same directory for an atomic rename. */
    char dirbuf[PATH_MAX];
    strlcpy(dirbuf, path, sizeof(dirbuf));
    char *dir = dirname(dirbuf);
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s/.anka-layer-editor.%d.tmp", dir, (int)getpid());

    tar_t *out = tar_wopen(tmp, 6);
    if (!out) {
        le_seterr(err, errlen, "%s: cannot create temp: %s", tmp, strerror(errno));
        return -1;
    }
    tar_t *in = tar_ropen(path);
    if (!in) {
        le_seterr(err, errlen, "%s: cannot reopen: %s", path, strerror(errno));
        tar_close(out);
        unlink(tmp);
        return -1;
    }

    /* Stream the original: keep entries that are not removed or replaced. */
    tar_entry e;
    int r, failed = 0;
    while ((r = tar_read_next(in, &e)) == 1) {
        le_node *n = model_lookup(m, e.path);
        if (should_skip_original(n, arch_idx)) {
            if (tar_skip_data(in) == -1) {
                le_seterr(err, errlen, "%s: skip error", path);
                failed = 1;
                break;
            }
            continue;
        }
        if (tar_write_header(out, &e) == -1) {
            le_seterr(err, errlen, "%s: write error", tmp);
            failed = 1;
            break;
        }
        /* Copy any payload bytes (files, and rare non-file sized entries). */
        if (e.size > 0 && e.type != TAR_SYMLINK) {
            if (copy_data(in, out) == -1) {
                le_seterr(err, errlen, "%s: copy error", tmp);
                failed = 1;
                break;
            }
        } else if (tar_skip_data(in) == -1) {
            le_seterr(err, errlen, "%s: skip error", path);
            failed = 1;
            break;
        }
    }
    if (r < 0) {
        le_seterr(err, errlen, "%s: read error", path);
        failed = 1;
    }
    tar_close(in);

    if (!failed && append_additions(out, m->root, arch_idx, err, errlen) == -1)
        failed = 1;

    if (tar_close(out) == -1 && !failed) {
        le_seterr(err, errlen, "%s: finalize error", tmp);
        failed = 1;
    }

    if (failed) {
        unlink(tmp);
        return -1;
    }

    if (rename(tmp, path) == -1) {
        le_seterr(err, errlen, "%s -> %s: %s", tmp, path, strerror(errno));
        unlink(tmp);
        return -1;
    }

    model_reconcile_after_save(m, arch_idx);
    return 0;
}
