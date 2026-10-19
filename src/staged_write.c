#include "staged_write.h"
#include "util.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#define COPY_BUF (1 << 16)

int staged_write_whiteout_entry(tar_t *out, const char *whiteout_path,
                                int is_dir, uid_t uid, gid_t gid,
                                char *err, size_t errlen) {
    tar_entry e;
    memset(&e, 0, sizeof(e));
    strlcpy(e.path, whiteout_path, sizeof(e.path));
    e.type = is_dir ? TAR_DIR : TAR_FILE;
    e.mode = is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    e.uid = uid;
    e.gid = gid;
    e.mtime = time(NULL);
    e.devmajor = LE_CHANGE_WHITEOUT;
    if (tar_write_header(out, &e) == -1) {
        le_seterr(err, errlen, "%s: write error: %s", whiteout_path,
                  strerror(errno));
        return -1;
    }
    return 0;
}

static void entry_from_staged(tar_entry *e, const char *guest,
                              const le_staged *s, tar_type type) {
    memset(e, 0, sizeof(*e));
    strlcpy(e->path, guest, sizeof(e->path));
    e->type = type;
    e->mode = s->meta.mode;
    e->uid = s->meta.uid;
    e->gid = s->meta.gid;
    e->mtime = time(NULL);
    e->devmajor = s->change_kind;
}

static int copy_host_file(tar_t *out, const char *host_path, off_t expected_size,
                          char *err, size_t errlen) {
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
            le_seterr(err, errlen, "%s: write error", host_path);
            rc = -1;
            break;
        }
        written += (off_t)rd;
    }
    if (rc == 0 && ferror(f)) {
        le_seterr(err, errlen, "%s: read error", host_path);
        rc = -1;
    }
    fclose(f);
    if (rc == 0 && written != expected_size) {
        le_seterr(err, errlen, "%s: size changed during read (%lld -> %lld)",
                  host_path, (long long)expected_size, (long long)written);
        rc = -1;
    }
    return rc;
}

/* Write a staged host file or symlink, read from the host now. */
static int write_staged_host_file(tar_t *out, const char *guest,
                                  const le_staged *s, char *err, size_t errlen) {
    struct stat st;
    if (lstat(s->host_path, &st) == -1) {
        le_seterr(err, errlen, "%s: %s", s->host_path, strerror(errno));
        return -1;
    }
    tar_entry e;
    if (S_ISLNK(st.st_mode)) {
        entry_from_staged(&e, guest, s, TAR_SYMLINK);
        ssize_t l = readlink(s->host_path, e.link, sizeof(e.link) - 1);
        if (l == -1) {
            le_seterr(err, errlen, "%s: %s", s->host_path, strerror(errno));
            return -1;
        }
        e.link[l] = '\0';
    } else if (S_ISREG(st.st_mode)) {
        entry_from_staged(&e, guest, s, TAR_FILE);
        e.size = st.st_size;
    } else {
        le_seterr(err, errlen, "%s: not a regular file or symlink", s->host_path);
        return -1;
    }
    e.mode = (st.st_mode & S_IFMT) | (s->meta.mode & 07777);
    e.mtime = st.st_mtime;

    if (tar_write_header(out, &e) == -1) {
        le_seterr(err, errlen, "%s: write error: %s", guest, strerror(errno));
        return -1;
    }
    return e.type == TAR_FILE
               ? copy_host_file(out, s->host_path, st.st_size, err, errlen)
               : 0;
}

static int write_staged(tar_t *out, const le_node *n, int arch_idx,
                        char *err, size_t errlen) {
    const le_staged *s = n->arch[arch_idx].staged;
    char guest[PATH_MAX];
    if (node_path(n, guest, sizeof(guest)) == -1) {
        le_seterr(err, errlen, "path too deep for node '%s'", n->name);
        return -1;
    }
    if (s->source == LE_STAGED_WHITEOUT)
        return staged_write_whiteout_entry(out, guest,
                                           s->meta.kind == LE_KIND_DIR,
                                           s->meta.uid, s->meta.gid,
                                           err, errlen);
    return write_staged_host_file(out, guest, s, err, errlen);
}

int staged_write_all(tar_t *out, const le_node *root, int arch_idx,
                     char *err, size_t errlen) {
    for (size_t i = 0; i < root->nchildren; i++) {
        const le_node *c = root->children[i];
        if (node_effectively_removed(c, arch_idx))
            continue;
        if (c->arch[arch_idx].staged &&
            write_staged(out, c, arch_idx, err, errlen) == -1)
            return -1;
        if (staged_write_all(out, c, arch_idx, err, errlen) == -1)
            return -1;
    }
    return 0;
}
