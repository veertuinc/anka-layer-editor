#include "archive.h"
#include "tar.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <libgen.h>
#include <sys/stat.h>

#define COPY_BUF (1 << 16)

static void seterr(char *err, size_t errlen, const char *fmt, ...) {
    if (!err || !errlen)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

int archive_load(const char *path, le_model *m, char *err, size_t errlen) {
    tar_t *in = tar_ropen(path);
    if (!in) {
        seterr(err, errlen, "%s: cannot open: %s", path, strerror(errno));
        return -1;
    }

    tar_entry e;
    int r;
    while ((r = tar_read_next(in, &e)) == 1) {
        int is_dir = (e.type == TAR_DIR);
        int is_sym = (e.type == TAR_SYMLINK);
        model_insert(m, e.path, is_dir, is_sym, e.size, e.mode,
                     is_sym ? e.link : NULL);
        tar_skip_data(in);
    }

    tar_close(in);
    if (r < 0) {
        seterr(err, errlen, "%s: read error", path);
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
static int write_addition(tar_t *out, const le_node *n,
                          char *err, size_t errlen) {
    char guest[PATH_MAX];
    node_path(n, guest, sizeof(guest));

    struct stat st;
    if (lstat(n->host_path, &st) == -1) {
        seterr(err, errlen, "%s: %s", n->host_path, strerror(errno));
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
        ssize_t l = readlink(n->host_path, tgt, sizeof(tgt) - 1);
        if (l == -1) {
            seterr(err, errlen, "%s: %s", n->host_path, strerror(errno));
            return -1;
        }
        tgt[l] = '\0';
        e.type = TAR_SYMLINK;
        strlcpy(e.link, tgt, sizeof(e.link));
        return tar_write_header(out, &e);
    }

    if (!S_ISREG(st.st_mode)) {
        seterr(err, errlen, "%s: not a regular file or symlink", n->host_path);
        return -1;
    }

    e.type = TAR_FILE;
    e.size = st.st_size;
    if (tar_write_header(out, &e) == -1)
        return -1;

    FILE *f = fopen(n->host_path, "rb");
    if (!f) {
        seterr(err, errlen, "%s: %s", n->host_path, strerror(errno));
        return -1;
    }
    char buf[COPY_BUF];
    size_t rd;
    int rc = 0;
    while ((rd = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (tar_write_data(out, buf, rd) == -1) {
            rc = -1;
            break;
        }
    }
    if (rc == 0 && ferror(f))
        rc = -1;
    fclose(f);
    return rc;
}

static int append_additions(tar_t *out, le_node *node,
                            char *err, size_t errlen) {
    for (size_t i = 0; i < node->nchildren; i++) {
        le_node *c = node->children[i];
        if (node_effectively_removed(c))
            continue;
        if (c->added && c->host_path && write_addition(out, c, err, errlen) == -1)
            return -1;
        if (append_additions(out, c, err, errlen) == -1)
            return -1;
    }
    return 0;
}

int archive_save(const char *path, le_model *m, char *err, size_t errlen) {
    /* Temp file in the same directory for an atomic rename. */
    char dirbuf[PATH_MAX];
    strlcpy(dirbuf, path, sizeof(dirbuf));
    char *dir = dirname(dirbuf);
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s/.layeredit.%d.tmp", dir, (int)getpid());

    tar_t *out = tar_wopen(tmp, 6);
    if (!out) {
        seterr(err, errlen, "%s: cannot create temp: %s", tmp, strerror(errno));
        return -1;
    }
    tar_t *in = tar_ropen(path);
    if (!in) {
        seterr(err, errlen, "%s: cannot reopen: %s", path, strerror(errno));
        tar_close(out);
        unlink(tmp);
        return -1;
    }

    /* Stream the original: keep entries whose path is not effectively removed. */
    tar_entry e;
    int r, failed = 0;
    while ((r = tar_read_next(in, &e)) == 1) {
        le_node *n = model_lookup(m, e.path);
        if (n && node_effectively_removed(n)) {
            tar_skip_data(in);
            continue;
        }
        if (tar_write_header(out, &e) == -1) {
            seterr(err, errlen, "%s: write error", tmp);
            failed = 1;
            break;
        }
        if (e.type == TAR_FILE && copy_data(in, out) == -1) {
            seterr(err, errlen, "%s: copy error", tmp);
            failed = 1;
            break;
        }
    }
    if (r < 0) {
        seterr(err, errlen, "%s: read error", path);
        failed = 1;
    }
    tar_close(in);

    if (!failed && append_additions(out, m->root, err, errlen) == -1)
        failed = 1;

    if (tar_close(out) == -1 && !failed) {
        seterr(err, errlen, "%s: finalize error", tmp);
        failed = 1;
    }

    if (failed) {
        unlink(tmp);
        return -1;
    }

    if (rename(tmp, path) == -1) {
        seterr(err, errlen, "%s -> %s: %s", tmp, path, strerror(errno));
        unlink(tmp);
        return -1;
    }

    m->dirty = 0;
    return 0;
}
