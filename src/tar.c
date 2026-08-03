#include "tar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <zlib.h>

/* EFTYPE is BSD ("inappropriate file type or format"); Linux has no equivalent. */
#ifndef EFTYPE
#define EFTYPE EINVAL
#endif

#define TByte 512
#define PAD512(n) ((TByte - ((n) % TByte)) % TByte)

struct posix_header {          /* byte offset */
    char name[100];            /*   0 */
    char mode[8];              /* 100 */
    char uid[8];               /* 108 */
    char gid[8];               /* 116 */
    char size[12];             /* 124 */
    char mtime[12];            /* 136 */
    char chksum[8];            /* 148 */
    char typeflag;             /* 156 */
    char linkname[100];        /* 157 */
    char magic[6];             /* 257 */
    char version[2];           /* 263 */
    char uname[32];            /* 265 */
    char gname[32];            /* 297 */
    char devmajor[8];          /* 329 */
    char devminor[8];          /* 337 */
    char prefix[155];          /* 345 */
    char padding[12];          /* 500 */
};

_Static_assert(sizeof(struct posix_header) == TByte, "tar header must be 512 bytes");

struct tar {
    gzFile gz;
    int    writing;
    /* read: bytes of current entry not yet consumed + trailing padding */
    off_t  rdata;
    off_t  rpad;
    /* write: declared size, bytes written so far, and whether a sized
     * file entry is open (so finish_data can detect short/long writes). */
    off_t  wsize;
    off_t  wdata;
    int    wactive;
};

/* ---- low-level gzip helpers ---- */

static int gz_readfull(gzFile f, void *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        int r = gzread(f, (char *)buf + off, (unsigned)(len - off));
        if (r < 0)
            return -1;
        if (r == 0)
            break;              /* EOF */
        off += (size_t)r;
    }
    return (int)off;
}

static int gz_writefull(gzFile f, const void *buf, size_t len) {
    if (len == 0)
        return 0;
    return gzwrite(f, buf, (unsigned)len) == (int)len ? 0 : -1;
}

static int gz_writezeros(gzFile f, size_t len) {
    char z[TByte];
    memset(z, 0, sizeof(z));
    while (len) {
        size_t n = len < sizeof(z) ? len : sizeof(z);
        if (-1 == gz_writefull(f, z, n))
            return -1;
        len -= n;
    }
    return 0;
}

/* ---- numeric field parsing ---- */

static long long parse_octal(const char *p, size_t n) {
    long long v = 0;
    size_t i = 0;
    while (i < n && (p[i] == ' ' || p[i] == '\0'))
        i++;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++)
        v = (v << 3) + (p[i] - '0');
    return v;
}

static off_t parse_size(const char *field, size_t n) {
    if (field[0] & 0x80) {      /* GNU base-256 */
        unsigned long long v = (unsigned char)field[0] & 0x7f;
        for (size_t i = 1; i < n; i++)
            v = (v << 8) | (unsigned char)field[i];
        return (off_t)v;
    }
    return (off_t)parse_octal(field, n);
}

static int is_zero_block(const unsigned char *b) {
    for (int i = 0; i < TByte; i++)
        if (b[i])
            return 0;
    return 1;
}

/* ---- numeric field writing ---- */

static void put_octal(char *field, size_t n, unsigned long long v) {
    /* n-1 octal digits, NUL-terminated */
    snprintf(field, n, "%0*llo", (int)(n - 1), v);
}

static void put_size(char field[12], off_t size) {
    if (size <= 077777777777LL) {
        snprintf(field, 12, "%011llo", (unsigned long long)size);
    } else {
        /* GNU base-256 with high bit set on the first byte */
        field[0] = (char)0x80;
        for (int i = 11; i >= 1; i--) {
            field[i] = (char)((unsigned long long)size & 0xff);
            size = (off_t)((unsigned long long)size >> 8);
        }
    }
}

static unsigned header_checksum(const struct posix_header *h) {
    const unsigned char *b = (const unsigned char *)h;
    unsigned s = 0;
    for (int i = 0; i < TByte; i++)
        s += (i >= 148 && i < 156) ? ' ' : b[i];
    return s;
}

/* Fill name/prefix if the path fits ustar; return 1 on success, 0 otherwise. */
static int name_fits(const char *path, char name[100], char prefix[155]) {
    size_t len = strlen(path);
    if (len <= 100) {
        memcpy(name, path, len);
        return 1;
    }
    if (len > 100 + 1 + 155)
        return 0;
    /* Find a '/' such that the tail fits name[100] and head fits prefix[155]. */
    for (size_t i = (len > 100 ? len - 100 : 0); i < len; i++) {
        if (path[i] != '/')
            continue;
        size_t plen = i;
        size_t nlen = len - i - 1;
        if (plen <= 155 && nlen > 0 && nlen <= 100) {
            memcpy(prefix, path, plen);
            memcpy(name, path + i + 1, nlen);
            return 1;
        }
    }
    return 0;
}

/* ---- pax record building ---- */

static int num_digits(size_t n) {
    int d = 1;
    while (n >= 10) { n /= 10; d++; }
    return d;
}

/* Append one pax record. Returns 0 on success, -1 if it would not fit. */
static int pax_add(char *buf, size_t cap, size_t *len,
                   const char *key, const char *val) {
    size_t body = strlen(key) + 1 + strlen(val) + 1;   /* key=val\n */
    int digits = 1;
    size_t total;
    for (;;) {
        total = (size_t)digits + 1 + body;             /* digits + ' ' + body */
        int d = num_digits(total);
        if (d == digits)
            break;
        digits = d;
    }
    if (*len + total >= cap)
        return -1;
    int wrote = snprintf(buf + *len, cap - *len, "%zu %s=%s\n",
                         total, key, val);
    if (wrote < 0 || (size_t)wrote != total)
        return -1;
    *len += (size_t)wrote;
    return 0;
}

/* True when an unsigned value cannot be stored in an n-byte octal field
 * (n-1 digits + NUL). */
static int octal_overflows(unsigned long long v, size_t field_len) {
    if (field_len < 2)
        return 1;
    int digits = (int)(field_len - 1);
    unsigned long long max = 0;
    for (int i = 0; i < digits; i++)
        max = (max << 3) | 7ULL;
    return v > max;
}

/* ==== reader ==== */

tar_t *tar_ropen(const char *path) {
    gzFile gz = gzopen(path, "rb");
    if (!gz)
        return NULL;
    struct tar *t = calloc(1, sizeof(*t));
    if (!t) {
        gzclose(gz);
        return NULL;
    }
    t->gz = gz;
    t->writing = 0;
    return t;
}

/* Read `size` bytes of extended-header data into out (NUL-terminated) and
 * consume the trailing 512-padding. out may be NULL to discard. */
static int read_ext_data(tar_t *t, off_t size, char *out, size_t outcap) {
    off_t pad = PAD512(size);
    off_t got = 0;
    char chunk[4096];
    while (got < size) {
        size_t want = (size_t)((size - got) < (off_t)sizeof(chunk)
                               ? (size - got) : (off_t)sizeof(chunk));
        int r = gz_readfull(t->gz, chunk, want);
        if (r <= 0)
            return -1;
        if (out && (size_t)got < outcap - 1) {
            size_t cp = (size_t)r;
            if ((size_t)got + cp > outcap - 1)
                cp = outcap - 1 - (size_t)got;
            memcpy(out + got, chunk, cp);
        }
        got += r;
    }
    if (out)
        out[(size_t)size < outcap ? (size_t)size : outcap - 1] = '\0';
    if (pad) {
        char skip[TByte];
        if (gz_readfull(t->gz, skip, (size_t)pad) != (int)pad)
            return -1;
    }
    return 0;
}

static void pax_parse(const char *buf, size_t len,
                      char *o_path, char *o_link, off_t *o_size,
                      long *o_uid, long *o_gid, long long *o_mtime) {
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        size_t rec = 0;
        while (j < len && buf[j] >= '0' && buf[j] <= '9') {
            rec = rec * 10 + (size_t)(buf[j] - '0');
            j++;
        }
        if (j >= len || buf[j] != ' ' || rec == 0 || i + rec > len)
            break;
        j++;                                /* skip space */
        const char *kv = buf + j;
        size_t kvlen = (i + rec) - j;        /* includes trailing '\n' */
        if (kvlen && kv[kvlen - 1] == '\n')
            kvlen--;
        const char *eq = memchr(kv, '=', kvlen);
        if (eq) {
            size_t klen = (size_t)(eq - kv);
            const char *val = eq + 1;
            size_t vlen = kvlen - klen - 1;
            char vbuf[PATH_MAX];
            if (vlen >= sizeof(vbuf))
                vlen = sizeof(vbuf) - 1;
            memcpy(vbuf, val, vlen);
            vbuf[vlen] = '\0';
            if (klen == 4 && !strncmp(kv, "path", 4))
                strlcpy(o_path, vbuf, PATH_MAX);
            else if (klen == 8 && !strncmp(kv, "linkpath", 8))
                strlcpy(o_link, vbuf, PATH_MAX);
            else if (klen == 4 && !strncmp(kv, "size", 4))
                *o_size = (off_t)strtoll(vbuf, NULL, 10);
            else if (klen == 3 && !strncmp(kv, "uid", 3))
                *o_uid = strtol(vbuf, NULL, 10);
            else if (klen == 3 && !strncmp(kv, "gid", 3))
                *o_gid = strtol(vbuf, NULL, 10);
            else if (klen == 5 && !strncmp(kv, "mtime", 5))
                *o_mtime = strtoll(vbuf, NULL, 10);
        }
        i += rec;
    }
}

int tar_read_next(tar_t *t, tar_entry *e) {
    if (t->rdata > 0 || t->rpad > 0)
        tar_skip_data(t);

    char pax_path[PATH_MAX] = "";
    char pax_link[PATH_MAX] = "";
    char gnu_name[PATH_MAX] = "";
    char gnu_link[PATH_MAX] = "";
    off_t     pax_size = -1;
    long      pax_uid = -1, pax_gid = -1;
    long long pax_mtime = -1;

    for (;;) {
        unsigned char blk[TByte];
        int r = gz_readfull(t->gz, blk, TByte);
        if (r == 0)
            return 0;                        /* clean EOF */
        if (r != TByte)
            return -1;
        if (is_zero_block(blk))
            return 0;                        /* end-of-archive marker */

        struct posix_header *h = (struct posix_header *)blk;
        if (strncmp(h->magic, "ustar", 5) != 0) {
            errno = EFTYPE;
            return -1;
        }

        off_t size = parse_size(h->size, sizeof(h->size));
        char tf = h->typeflag;

        if (tf == 'x' || tf == 'g') {
            char paxbuf[1 << 16];
            if (read_ext_data(t, size, paxbuf, sizeof(paxbuf)) == -1)
                return -1;
            size_t plen = (size_t)size < sizeof(paxbuf) ? (size_t)size
                                                        : sizeof(paxbuf) - 1;
            pax_parse(paxbuf, plen, pax_path, pax_link, &pax_size,
                      &pax_uid, &pax_gid, &pax_mtime);
            continue;
        }
        if (tf == 'L') {                     /* GNU long name */
            if (read_ext_data(t, size, gnu_name, sizeof(gnu_name)) == -1)
                return -1;
            continue;
        }
        if (tf == 'K') {                     /* GNU long link */
            if (read_ext_data(t, size, gnu_link, sizeof(gnu_link)) == -1)
                return -1;
            continue;
        }

        /* Real entry. Resolve the path. */
        char path[PATH_MAX];
        if (pax_path[0])
            strlcpy(path, pax_path, sizeof(path));
        else if (gnu_name[0])
            strlcpy(path, gnu_name, sizeof(path));
        else if (h->prefix[0])
            snprintf(path, sizeof(path), "%.155s/%.100s", h->prefix, h->name);
        else
            snprintf(path, sizeof(path), "%.100s", h->name);

        /* Normalize: drop a single trailing slash (directory entries). */
        size_t pl = strlen(path);
        if (pl > 1 && path[pl - 1] == '/')
            path[pl - 1] = '\0';

        memset(e, 0, sizeof(*e));
        strlcpy(e->path, path, sizeof(e->path));

        mode_t perms = (mode_t)parse_octal(h->mode, sizeof(h->mode)) & 07777;
        if (tf == '5') {
            e->type = TAR_DIR;
            e->mode = S_IFDIR | perms;
        } else if (tf == '2') {
            e->type = TAR_SYMLINK;
            e->mode = S_IFLNK | perms;
            if (pax_link[0])
                strlcpy(e->link, pax_link, sizeof(e->link));
            else if (gnu_link[0])
                strlcpy(e->link, gnu_link, sizeof(e->link));
            else
                snprintf(e->link, sizeof(e->link), "%.100s", h->linkname);
        } else if (tf == '0' || tf == '\0') {
            e->type = TAR_FILE;
            e->mode = S_IFREG | perms;
        } else {
            e->type = TAR_OTHER;
            e->mode = perms;
        }

        e->uid = (uid_t)(pax_uid >= 0 ? pax_uid
                                      : parse_octal(h->uid, sizeof(h->uid)));
        e->gid = (gid_t)(pax_gid >= 0 ? pax_gid
                                      : parse_octal(h->gid, sizeof(h->gid)));
        e->mtime = (time_t)(pax_mtime >= 0 ? pax_mtime
                                           : parse_octal(h->mtime, sizeof(h->mtime)));
        e->size = pax_size >= 0 ? pax_size : size;

        /* Symlinks carry their target in the header/linkname, not as payload.
         * Any other entry with a non-zero size has trailing data we must
         * consume so the stream stays aligned. */
        if (e->type != TAR_SYMLINK && e->size > 0) {
            t->rdata = e->size;
            t->rpad = PAD512(e->size);
        } else {
            t->rdata = 0;
            t->rpad = 0;
        }
        return 1;
    }
}

ssize_t tar_read_data(tar_t *t, void *buf, size_t len) {
    if (t->rdata == 0)
        return 0;
    if ((off_t)len > t->rdata)
        len = (size_t)t->rdata;
    int r = gzread(t->gz, buf, (unsigned)len);
    if (r < 0)
        return -1;
    t->rdata -= r;
    return r;
}

int tar_skip_data(tar_t *t) {
    char buf[4096];
    off_t rem = t->rdata + t->rpad;
    while (rem > 0) {
        size_t want = rem < (off_t)sizeof(buf) ? (size_t)rem : sizeof(buf);
        int r = gz_readfull(t->gz, buf, want);
        if (r <= 0)
            return -1;
        rem -= r;
    }
    t->rdata = 0;
    t->rpad = 0;
    return 0;
}

/* ==== writer ==== */

tar_t *tar_wopen(const char *path, int level) {
    char mode[8];
    if (level < 0 || level > 9)
        level = 6;
    snprintf(mode, sizeof(mode), "wb%d", level);
    gzFile gz = gzopen(path, mode);
    if (!gz)
        return NULL;
    struct tar *t = calloc(1, sizeof(*t));
    if (!t) {
        gzclose(gz);
        return NULL;
    }
    t->gz = gz;
    t->writing = 1;
    return t;
}

/* Pad out the previous entry's data to a 512 boundary.
 * Fails closed if a sized file entry was short or long relative to its header. */
static int finish_data(tar_t *t) {
    if (t->wactive && t->wdata != t->wsize) {
        errno = EIO;
        t->wactive = 0;
        t->wdata = 0;
        t->wsize = 0;
        return -1;
    }
    off_t pad = PAD512(t->wdata);
    if (pad && -1 == gz_writezeros(t->gz, (size_t)pad))
        return -1;
    t->wactive = 0;
    t->wdata = 0;
    t->wsize = 0;
    return 0;
}

static int emit_header(tar_t *t, struct posix_header *h) {
    memcpy(h->magic, "ustar", 5);            /* leaves magic[5] = '\0' */
    memcpy(h->version, "00", 2);
    memset(h->chksum, ' ', sizeof(h->chksum));
    unsigned sum = header_checksum(h);
    snprintf(h->chksum, 7, "%06o", sum & 0777777);
    h->chksum[6] = '\0';
    h->chksum[7] = ' ';
    return gz_writefull(t->gz, h, TByte);
}

int tar_write_header(tar_t *t, const tar_entry *e) {
    if (-1 == finish_data(t))
        return -1;

    /* Directory entries carry a trailing slash by convention. */
    char pathbuf[PATH_MAX];
    const char *wpath = e->path;
    if (e->type == TAR_DIR) {
        size_t l = strlen(e->path);
        if (l == 0 || e->path[l - 1] != '/') {
            snprintf(pathbuf, sizeof(pathbuf), "%s/", e->path);
            wpath = pathbuf;
        }
    }

    int need_path = 0, need_link = 0;
    int need_uid = 0, need_gid = 0, need_mtime = 0;
    char name[100] = {0}, prefix[155] = {0};
    if (!name_fits(wpath, name, prefix))
        need_path = 1;
    if (e->type == TAR_SYMLINK && strlen(e->link) > 100)
        need_link = 1;
    if (octal_overflows((unsigned long long)e->uid, 8))
        need_uid = 1;
    if (octal_overflows((unsigned long long)e->gid, 8))
        need_gid = 1;
    if (octal_overflows((unsigned long long)e->mtime, 12))
        need_mtime = 1;

    if (need_path || need_link || need_uid || need_gid || need_mtime) {
        char pax[1 << 16];
        size_t plen = 0;
        char num[32];
        if (need_path && pax_add(pax, sizeof(pax), &plen, "path", wpath) == -1)
            return -1;
        if (need_link && pax_add(pax, sizeof(pax), &plen, "linkpath", e->link) == -1)
            return -1;
        if (need_uid) {
            snprintf(num, sizeof(num), "%llu", (unsigned long long)e->uid);
            if (pax_add(pax, sizeof(pax), &plen, "uid", num) == -1)
                return -1;
        }
        if (need_gid) {
            snprintf(num, sizeof(num), "%llu", (unsigned long long)e->gid);
            if (pax_add(pax, sizeof(pax), &plen, "gid", num) == -1)
                return -1;
        }
        if (need_mtime) {
            snprintf(num, sizeof(num), "%lld", (long long)e->mtime);
            if (pax_add(pax, sizeof(pax), &plen, "mtime", num) == -1)
                return -1;
        }

        struct posix_header xh;
        memset(&xh, 0, sizeof(xh));
        strncpy(xh.name, "PaxHeaders", sizeof(xh.name) - 1);
        put_octal(xh.mode, sizeof(xh.mode), 0644);
        put_octal(xh.uid, sizeof(xh.uid), 0);
        put_octal(xh.gid, sizeof(xh.gid), 0);
        put_size(xh.size, (off_t)plen);
        put_octal(xh.mtime, sizeof(xh.mtime), 0);
        xh.typeflag = 'x';
        if (-1 == emit_header(t, &xh))
            return -1;
        if (-1 == gz_writefull(t->gz, pax, plen))
            return -1;
        if (-1 == gz_writezeros(t->gz, (size_t)PAD512(plen)))
            return -1;
    }

    struct posix_header h;
    memset(&h, 0, sizeof(h));
    if (need_path)
        strncpy(h.name, wpath, sizeof(h.name));      /* reader prefers pax path */
    else {
        memcpy(h.name, name, strnlen(name, sizeof(h.name)));
        memcpy(h.prefix, prefix, strnlen(prefix, sizeof(h.prefix)));
    }

    put_octal(h.mode, sizeof(h.mode), e->mode & 07777);
    put_octal(h.uid, sizeof(h.uid), need_uid ? 0 : (unsigned long long)e->uid);
    put_octal(h.gid, sizeof(h.gid), need_gid ? 0 : (unsigned long long)e->gid);
    put_size(h.size, e->type == TAR_FILE ? e->size : 0);
    put_octal(h.mtime, sizeof(h.mtime),
              need_mtime ? 0 : (unsigned long long)e->mtime);

    switch (e->type) {
    case TAR_DIR:     h.typeflag = '5'; break;
    case TAR_SYMLINK: h.typeflag = '2';
        strncpy(h.linkname, e->link, sizeof(h.linkname));
        break;
    default:          h.typeflag = '0'; break;
    }

    if (-1 == emit_header(t, &h))
        return -1;

    if (e->type == TAR_FILE) {
        t->wactive = 1;
        t->wsize = e->size;
        t->wdata = 0;
    } else {
        t->wactive = 0;
        t->wsize = 0;
        t->wdata = 0;
    }
    return 0;
}

ssize_t tar_write_data(tar_t *t, const void *buf, size_t len) {
    if (len == 0)
        return 0;
    if (gzwrite(t->gz, buf, (unsigned)len) != (int)len)
        return -1;
    t->wdata += (off_t)len;
    return (ssize_t)len;
}

int tar_close(tar_t *t) {
    int res = 0;
    if (!t)
        return 0;
    if (t->writing) {
        if (-1 == finish_data(t))
            res = -1;
        if (-1 == gz_writezeros(t->gz, 2 * TByte))    /* end-of-archive */
            res = -1;
    }
    if (gzclose(t->gz) != Z_OK)
        res = -1;
    free(t);
    return res;
}
