#include "tar.h"
#include "util.h"

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
#define EXT_DATA_MAX (16 << 20)      /* largest pax / GNU long-name payload */
#define NSEC_PER_SEC 1000000000L

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

#define USTAR_LINKNAME_LEN sizeof(((struct posix_header *)0)->linkname)

/* Growable buffer of pax records ("len key=value\n" ...), NUL-terminated. */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} pax_buf;

struct tar {
    gzFile  gz;
    int     writing;
    /* read: bytes of current entry not yet consumed + trailing padding */
    off_t   rdata;
    off_t   rpad;
    pax_buf global_records;    /* read: every 'g' record seen so far */
    pax_buf entry_extra;       /* read: uninterpreted records of the entry */
    pax_buf ext_data;          /* read: payload of the last extension header */
    /* write: declared size, bytes written so far, and whether a sized
     * file entry is open (so finish_data can detect short/long writes). */
    off_t   wsize;
    off_t   wdata;
    int     wactive;
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

/* ---- pax record buffers ---- */

static int pax_buf_reserve(pax_buf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap)
        return 0;
    size_t ncap = b->cap ? b->cap : 1024;
    while (ncap < b->len + extra + 1)
        ncap *= 2;
    char *nd = realloc(b->data, ncap);
    if (!nd)
        return -1;
    b->data = nd;
    b->cap = ncap;
    return 0;
}

static void pax_buf_free(pax_buf *b) {
    free(b->data);
    memset(b, 0, sizeof(*b));
}

static int pax_buf_append(pax_buf *b, const char *src, size_t len) {
    if (pax_buf_reserve(b, len) == -1)
        return -1;
    if (len)
        memcpy(b->data + b->len, src, len);
    b->len += len;
    b->data[b->len] = '\0';
    return 0;
}

static int num_digits(size_t n) {
    int d = 1;
    while (n >= 10) { n /= 10; d++; }
    return d;
}

/* Append one record; the length prefix counts its own digits. */
static int pax_buf_add(pax_buf *b, const char *key, size_t klen,
                       const char *val, size_t vlen) {
    size_t body = klen + 1 + vlen + 1;                 /* key=val\n */
    size_t total = 1 + 1 + body;                       /* digit + ' ' + body */
    while ((size_t)num_digits(total) + 1 + body != total)
        total = (size_t)num_digits(total) + 1 + body;
    char prefix[32];
    int prefix_len = snprintf(prefix, sizeof(prefix), "%zu ", total);
    if (pax_buf_reserve(b, total) == -1)
        return -1;
    char *w = b->data + b->len;
    memcpy(w, prefix, (size_t)prefix_len);
    w += prefix_len;
    memcpy(w, key, klen);
    w += klen;
    *w++ = '=';
    memcpy(w, val, vlen);
    w += vlen;
    *w++ = '\n';
    b->len += total;
    b->data[b->len] = '\0';
    return 0;
}

static int pax_buf_add_str(pax_buf *b, const char *key, const char *val) {
    return pax_buf_add(b, key, strlen(key), val, strlen(val));
}

typedef struct {
    const char *key;
    size_t      klen;
    const char *val;            /* not NUL-terminated; may hold binary */
    size_t      vlen;
    size_t      start;          /* offset of the record in the buffer */
    size_t      len;            /* full record length */
} pax_record;

/* Parse the record at *off. Returns 1 and advances, or 0 at the end or at the
 * first malformed record. */
static int pax_next(const char *buf, size_t len, size_t *off, pax_record *r) {
    size_t i = *off, j = *off, rec = 0;
    while (j < len && buf[j] >= '0' && buf[j] <= '9') {
        rec = rec * 10 + (size_t)(buf[j] - '0');
        if (rec > len)
            return 0;
        j++;
    }
    if (j >= len || buf[j] != ' ' || rec == 0 || rec > len - i)
        return 0;
    j++;
    const char *kv = buf + j;
    size_t kvlen = i + rec - j;
    if (kvlen == 0 || kv[kvlen - 1] != '\n')
        return 0;
    kvlen--;
    const char *eq = memchr(kv, '=', kvlen);
    if (!eq)
        return 0;
    r->key = kv;
    r->klen = (size_t)(eq - kv);
    r->val = eq + 1;
    r->vlen = kvlen - r->klen - 1;
    r->start = i;
    r->len = rec;
    *off = i + rec;
    return 1;
}

static int pax_key_is(const pax_record *r, const char *key) {
    size_t klen = strlen(key);
    return r->klen == klen && !memcmp(r->key, key, klen);
}

/* Replace any record with the same key (the last record for a key wins). */
static int pax_buf_put(pax_buf *b, const pax_record *rec) {
    size_t off = 0;
    pax_record r;
    while (b->len && pax_next(b->data, b->len, &off, &r)) {
        if (r.klen == rec->klen && !memcmp(r.key, rec->key, r.klen)) {
            memmove(b->data + r.start, b->data + r.start + r.len,
                    b->len - r.start - r.len);
            b->len -= r.len;
            b->data[b->len] = '\0';
            off = r.start;
        }
    }
    return pax_buf_add(b, rec->key, rec->klen, rec->val, rec->vlen);
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

/* "sec[.frac]" with an optional sign, as pax time values are written. */
static void parse_pax_time(const char *s, long long *sec, long *nsec) {
    char *end;
    long long whole = strtoll(s, &end, 10);
    long frac = 0;
    int digits = 0;
    if (*end == '.')
        for (const char *p = end + 1; *p >= '0' && *p <= '9'; p++)
            if (digits < 9) {
                frac = frac * 10 + (*p - '0');
                digits++;
            }
    for (; digits < 9; digits++)
        frac *= 10;
    while (*s == ' ')
        s++;
    if (*s == '-' && frac) {
        whole -= 1;
        frac = NSEC_PER_SEC - frac;
    }
    *sec = whole;
    *nsec = frac;
}

static void format_pax_time(long long sec, long nsec, char *out, size_t len) {
    if (nsec == 0) {
        snprintf(out, len, "%lld", sec);
        return;
    }
    const char *sign = "";
    long long whole = sec;
    long frac = nsec;
    if (sec < 0) {
        sign = "-";
        whole = -(sec + 1);
        frac = NSEC_PER_SEC - nsec;
    }
    char frac_digits[16];
    snprintf(frac_digits, sizeof(frac_digits), "%09ld", frac);
    for (size_t l = strlen(frac_digits); l > 1 && frac_digits[l - 1] == '0'; l--)
        frac_digits[l - 1] = '\0';
    snprintf(out, len, "%s%lld.%s", sign, whole, frac_digits);
}

static int is_zero_block(const unsigned char *b) {
    for (int i = 0; i < TByte; i++)
        if (b[i])
            return 0;
    return 1;
}

/* ---- numeric field writing ---- */

/* True when an unsigned value cannot be stored in an n-byte octal field
 * (n-1 digits + NUL). */
static int octal_overflows(unsigned long long v, size_t field_len) {
    if (field_len < 2)
        return 1;
    unsigned long long max = 0;
    for (size_t i = 0; i < field_len - 1; i++)
        max = (max << 3) | 7ULL;
    return v > max;
}

/* n-1 zero-padded octal digits, NUL-terminated. Callers check overflow. */
static void put_octal(char *field, size_t n, unsigned long long v) {
    field[n - 1] = '\0';
    for (size_t i = n - 1; i-- > 0; v >>= 3)
        field[i] = (char)('0' + (v & 7));
}

/* Octal when it fits, otherwise 0 (the value then travels in a pax record). */
static void put_octal_or_zero(char *field, size_t n, unsigned long long v) {
    put_octal(field, n, octal_overflows(v, n) ? 0 : v);
}

static void put_size(char field[12], off_t size) {
    if (!octal_overflows((unsigned long long)size, 12)) {
        put_octal(field, 12, (unsigned long long)size);
        return;
    }
    /* GNU base-256 with high bit set on the first byte */
    unsigned long long v = (unsigned long long)size;
    for (int i = 11; i >= 1; i--, v >>= 8)
        field[i] = (char)(v & 0xff);
    field[0] = (char)0x80;
}

static unsigned header_checksum(const struct posix_header *h) {
    const unsigned char *b = (const unsigned char *)h;
    unsigned s = 0;
    for (int i = 0; i < TByte; i++)
        s += (i >= 148 && i < 156) ? ' ' : b[i];
    return s;
}

/* Some historic writers summed signed bytes; accept either. */
static int header_checksum_ok(const struct posix_header *h) {
    long long stored = parse_octal(h->chksum, sizeof(h->chksum));
    const signed char *b = (const signed char *)h;
    int signed_sum = 0;
    for (int i = 0; i < TByte; i++)
        signed_sum += (i >= 148 && i < 156) ? ' ' : b[i];
    return stored == (long long)header_checksum(h) || stored == signed_sum;
}

static void copy_bounded(char *dst, size_t dstlen, const char *src, size_t srclen) {
    size_t n = strnlen(src, srclen);
    if (n >= dstlen)
        n = dstlen - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
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
    for (size_t i = len - 101; i < len; i++) {
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

/* ==== reader ==== */

static tar_t *tar_new(int writing) {
    struct tar *t = calloc(1, sizeof(*t));
    if (t)
        t->writing = writing;
    return t;
}

tar_t *tar_ropen(const char *path) {
    tar_t *t = tar_new(0);
    if (!t)
        return NULL;
    t->gz = gzopen(path, "rb");
    if (!t->gz) {
        free(t);
        return NULL;
    }
    return t;
}

tar_t *tar_rdopen(int fd) {
    tar_t *t = tar_new(0);
    if (!t)
        return NULL;
    t->gz = gzdopen(fd, "rb");
    if (!t->gz) {
        free(t);
        return NULL;
    }
    return t;
}

/* Read an extension payload into t->ext_data and consume its padding. */
static int read_ext_data(tar_t *t, off_t size) {
    if (size < 0 || size > EXT_DATA_MAX) {
        errno = EFTYPE;
        return -1;
    }
    t->ext_data.len = 0;
    if (pax_buf_reserve(&t->ext_data, (size_t)size + TByte) == -1)
        return -1;
    size_t padded = (size_t)size + (size_t)PAD512(size);
    if (gz_readfull(t->gz, t->ext_data.data, padded) != (int)padded) {
        errno = EIO;
        return -1;
    }
    t->ext_data.len = (size_t)size;
    t->ext_data.data[size] = '\0';
    return 0;
}

/* Pax values this reader turns into tar_entry fields. */
typedef struct {
    char      path[PATH_MAX];
    char      link[PATH_MAX];
    int       has_size, has_uid, has_gid, has_mtime;
    off_t     size;
    long long uid, gid;
    long long mtime_sec;
    long      mtime_nsec;
} pax_fields;

static void pax_value(const pax_record *r, char *out, size_t cap) {
    size_t n = r->vlen < cap - 1 ? r->vlen : cap - 1;
    memcpy(out, r->val, n);
    out[n] = '\0';
}

/* Apply records to the entry being read. Global records do not carry an
 * entry's path, link, or size. Everything not interpreted is kept in extra. */
static int pax_apply(const char *buf, size_t len, int is_global,
                     pax_fields *f, pax_buf *extra) {
    size_t off = 0;
    pax_record r;
    char num[64];
    while (len && pax_next(buf, len, &off, &r)) {
        int entry_only = pax_key_is(&r, "path") || pax_key_is(&r, "linkpath") ||
                         pax_key_is(&r, "size");
        if (entry_only && is_global)
            continue;
        if (pax_key_is(&r, "path")) {
            pax_value(&r, f->path, sizeof(f->path));
        } else if (pax_key_is(&r, "linkpath")) {
            pax_value(&r, f->link, sizeof(f->link));
        } else if (pax_key_is(&r, "size")) {
            pax_value(&r, num, sizeof(num));
            f->size = (off_t)strtoll(num, NULL, 10);
            f->has_size = 1;
        } else if (pax_key_is(&r, "uid")) {
            pax_value(&r, num, sizeof(num));
            f->uid = strtoll(num, NULL, 10);
            f->has_uid = 1;
        } else if (pax_key_is(&r, "gid")) {
            pax_value(&r, num, sizeof(num));
            f->gid = strtoll(num, NULL, 10);
            f->has_gid = 1;
        } else if (pax_key_is(&r, "mtime")) {
            pax_value(&r, num, sizeof(num));
            parse_pax_time(num, &f->mtime_sec, &f->mtime_nsec);
            f->has_mtime = 1;
        } else if (pax_buf_put(extra, &r) == -1) {
            return -1;
        }
    }
    return 0;
}

static tar_type type_for_typeflag(char typeflag) {
    switch (typeflag) {
    case '0': case '\0': case '7': return TAR_FILE;
    case '5':                      return TAR_DIR;
    case '2':                      return TAR_SYMLINK;
    case '1':                      return TAR_HARDLINK;
    default:                       return TAR_OTHER;
    }
}

static mode_t mode_bits_for_typeflag(char typeflag) {
    switch (type_for_typeflag(typeflag)) {
    case TAR_FILE: case TAR_HARDLINK: return S_IFREG;
    case TAR_DIR:                     return S_IFDIR;
    case TAR_SYMLINK:                 return S_IFLNK;
    default:
        break;
    }
    switch (typeflag) {
    case '3': return S_IFCHR;
    case '4': return S_IFBLK;
    case '6': return S_IFIFO;
    default:  return 0;
    }
}

static void fill_entry(tar_entry *e, const struct posix_header *h,
                       const pax_fields *f, const char *gnu_name,
                       const char *gnu_link) {
    memset(e, 0, sizeof(*e));
    if (f->path[0])
        strlcpy(e->path, f->path, sizeof(e->path));
    else if (gnu_name[0])
        strlcpy(e->path, gnu_name, sizeof(e->path));
    else if (h->prefix[0])
        snprintf(e->path, sizeof(e->path), "%.155s/%.100s", h->prefix, h->name);
    else
        snprintf(e->path, sizeof(e->path), "%.100s", h->name);

    /* Normalize: drop a single trailing slash (directory entries). */
    size_t pl = strlen(e->path);
    if (pl > 1 && e->path[pl - 1] == '/')
        e->path[pl - 1] = '\0';

    e->typeflag = h->typeflag;
    e->type = type_for_typeflag(h->typeflag);
    e->mode = mode_bits_for_typeflag(h->typeflag) |
              ((mode_t)parse_octal(h->mode, sizeof(h->mode)) & 07777);
    if (e->type == TAR_SYMLINK || e->type == TAR_HARDLINK) {
        if (f->link[0])
            strlcpy(e->link, f->link, sizeof(e->link));
        else if (gnu_link[0])
            strlcpy(e->link, gnu_link, sizeof(e->link));
        else
            snprintf(e->link, sizeof(e->link), "%.100s", h->linkname);
    }

    e->uid = (uid_t)(f->has_uid ? f->uid : parse_octal(h->uid, sizeof(h->uid)));
    e->gid = (gid_t)(f->has_gid ? f->gid : parse_octal(h->gid, sizeof(h->gid)));
    e->mtime = (time_t)(f->has_mtime ? f->mtime_sec
                                     : parse_octal(h->mtime, sizeof(h->mtime)));
    e->mtime_nsec = f->has_mtime ? f->mtime_nsec : 0;
    e->size = f->has_size ? f->size : parse_size(h->size, sizeof(h->size));
    copy_bounded(e->uname, sizeof(e->uname), h->uname, sizeof(h->uname));
    copy_bounded(e->gname, sizeof(e->gname), h->gname, sizeof(h->gname));
    e->devmajor = (unsigned long)parse_octal(h->devmajor, sizeof(h->devmajor));
    e->devminor = (unsigned long)parse_octal(h->devminor, sizeof(h->devminor));
}

int tar_read_next(tar_t *t, tar_entry *e) {
    if ((t->rdata > 0 || t->rpad > 0) && tar_skip_data(t) == -1)
        return -1;

    pax_fields f;
    memset(&f, 0, sizeof(f));
    char gnu_name[PATH_MAX] = "";
    char gnu_link[PATH_MAX] = "";

    t->entry_extra.len = 0;
    if (pax_apply(t->global_records.data, t->global_records.len, 1, &f,
                  &t->entry_extra) == -1)
        return -1;

    for (;;) {
        unsigned char blk[TByte];
        int r = gz_readfull(t->gz, blk, TByte);
        if (r == 0 || (r == TByte && is_zero_block(blk)))
            return 0;                        /* EOF or end-of-archive marker */
        if (r != TByte) {
            errno = EIO;
            return -1;
        }

        struct posix_header *h = (struct posix_header *)blk;
        if (strncmp(h->magic, "ustar", 5) != 0 || !header_checksum_ok(h)) {
            errno = EFTYPE;
            return -1;
        }

        off_t size = parse_size(h->size, sizeof(h->size));
        char tf = h->typeflag;

        if (tf == 'x' || tf == 'g') {
            if (read_ext_data(t, size) == -1)
                return -1;
            if (tf == 'g' && pax_buf_append(&t->global_records, t->ext_data.data,
                                            t->ext_data.len) == -1)
                return -1;
            if (pax_apply(t->ext_data.data, t->ext_data.len, tf == 'g', &f,
                          &t->entry_extra) == -1)
                return -1;
            continue;
        }
        if (tf == 'L' || tf == 'K') {        /* GNU long name / long link */
            if (read_ext_data(t, size) == -1)
                return -1;
            copy_bounded(tf == 'L' ? gnu_name : gnu_link, PATH_MAX,
                         t->ext_data.data, t->ext_data.len);
            continue;
        }

        fill_entry(e, h, &f, gnu_name, gnu_link);
        if (t->entry_extra.len) {
            e->pax_extra = t->entry_extra.data;
            e->pax_extra_len = t->entry_extra.len;
        }

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

static void write_mode(char mode[8], int level) {
    if (level < 0 || level > 9)
        level = 6;
    snprintf(mode, 8, "wb%d", level);
}

tar_t *tar_wopen(const char *path, int level) {
    char mode[8];
    write_mode(mode, level);
    tar_t *t = tar_new(1);
    if (!t)
        return NULL;
    t->gz = gzopen(path, mode);
    if (!t->gz) {
        free(t);
        return NULL;
    }
    return t;
}

tar_t *tar_wdopen(int fd, int level) {
    char mode[8];
    write_mode(mode, level);
    tar_t *t = tar_new(1);
    if (!t)
        return NULL;
    t->gz = gzdopen(fd, mode);
    if (!t->gz) {
        free(t);
        return NULL;
    }
    return t;
}

/* Pad out the previous entry's data to a 512 boundary.
 * Fails closed if a sized file entry was short or long relative to its header. */
static int finish_data(tar_t *t) {
    int mismatch = t->wactive && t->wdata != t->wsize;
    off_t pad = PAD512(t->wdata);
    t->wactive = 0;
    t->wdata = 0;
    t->wsize = 0;
    if (mismatch) {
        errno = EIO;
        return -1;
    }
    if (pad && -1 == gz_writezeros(t->gz, (size_t)pad))
        return -1;
    return 0;
}

static int emit_header(tar_t *t, struct posix_header *h) {
    memcpy(h->magic, "ustar", 6);
    memcpy(h->version, "00", 2);
    put_octal(h->chksum, 7, header_checksum(h) & 0777777);
    h->chksum[7] = ' ';
    return gz_writefull(t->gz, h, TByte);
}

static char typeflag_for_entry(const tar_entry *e) {
    if (e->typeflag)
        return e->typeflag;
    switch (e->type) {
    case TAR_FILE:     return '0';
    case TAR_DIR:      return '5';
    case TAR_SYMLINK:  return '2';
    case TAR_HARDLINK: return '1';
    default:           return '\0';
    }
}

static int entry_has_link(const tar_entry *e) {
    return e->type == TAR_SYMLINK || e->type == TAR_HARDLINK;
}

static int mtime_fits_header(const tar_entry *e) {
    return e->mtime >= 0 && !octal_overflows((unsigned long long)e->mtime, 12);
}

/* Emit a pax 'x' header for values the ustar header cannot hold, followed by
 * the entry's uninterpreted records. Writes nothing when there are none. */
static int write_pax_header(tar_t *t, const tar_entry *e, const char *wpath,
                            int need_path) {
    pax_buf pax = {0};
    char num[64];
    int rc = -1;

    if (need_path && pax_buf_add_str(&pax, "path", wpath) == -1)
        goto out;
    if (entry_has_link(e) && strlen(e->link) > USTAR_LINKNAME_LEN &&
        pax_buf_add_str(&pax, "linkpath", e->link) == -1)
        goto out;
    if (octal_overflows((unsigned long long)e->uid, 8)) {
        snprintf(num, sizeof(num), "%llu", (unsigned long long)e->uid);
        if (pax_buf_add_str(&pax, "uid", num) == -1)
            goto out;
    }
    if (octal_overflows((unsigned long long)e->gid, 8)) {
        snprintf(num, sizeof(num), "%llu", (unsigned long long)e->gid);
        if (pax_buf_add_str(&pax, "gid", num) == -1)
            goto out;
    }
    if (!mtime_fits_header(e) || e->mtime_nsec) {
        format_pax_time((long long)e->mtime, e->mtime_nsec, num, sizeof(num));
        if (pax_buf_add_str(&pax, "mtime", num) == -1)
            goto out;
    }
    if (e->pax_extra_len && pax_buf_append(&pax, e->pax_extra, e->pax_extra_len) == -1)
        goto out;

    rc = 0;
    if (pax.len == 0)
        goto out;

    struct posix_header xh;
    memset(&xh, 0, sizeof(xh));
    memcpy(xh.name, "PaxHeaders", sizeof("PaxHeaders") - 1);
    put_octal(xh.mode, sizeof(xh.mode), 0644);
    put_octal(xh.uid, sizeof(xh.uid), 0);
    put_octal(xh.gid, sizeof(xh.gid), 0);
    put_size(xh.size, (off_t)pax.len);
    put_octal(xh.mtime, sizeof(xh.mtime), 0);
    xh.typeflag = 'x';
    if (-1 == emit_header(t, &xh) ||
        -1 == gz_writefull(t->gz, pax.data, pax.len) ||
        -1 == gz_writezeros(t->gz, (size_t)PAD512(pax.len)))
        rc = -1;
out:
    pax_buf_free(&pax);
    return rc;
}

int tar_write_header(tar_t *t, const tar_entry *e) {
    if (-1 == finish_data(t))
        return -1;

    char typeflag = typeflag_for_entry(e);
    if (!typeflag || (e->type != TAR_FILE && e->size > 0)) {
        errno = EFTYPE;
        return -1;
    }

    /* Directory entries carry a trailing slash by convention. */
    char wpath[PATH_MAX + 1];
    size_t path_len = strlen(e->path);
    if (path_len + 2 > sizeof(wpath)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(wpath, e->path, path_len + 1);
    if (e->type == TAR_DIR && (path_len == 0 || wpath[path_len - 1] != '/')) {
        wpath[path_len] = '/';
        wpath[path_len + 1] = '\0';
    }

    struct posix_header h;
    memset(&h, 0, sizeof(h));
    int need_path = !name_fits(wpath, h.name, h.prefix);
    if (need_path)                           /* reader prefers the pax path */
        memcpy(h.name, wpath, strnlen(wpath, sizeof(h.name)));
    if (-1 == write_pax_header(t, e, wpath, need_path))
        return -1;

    put_octal(h.mode, sizeof(h.mode), e->mode & 07777);
    put_octal_or_zero(h.uid, sizeof(h.uid), (unsigned long long)e->uid);
    put_octal_or_zero(h.gid, sizeof(h.gid), (unsigned long long)e->gid);
    put_size(h.size, e->type == TAR_FILE ? e->size : 0);
    put_octal(h.mtime, sizeof(h.mtime),
              mtime_fits_header(e) ? (unsigned long long)e->mtime : 0);
    h.typeflag = typeflag;
    if (entry_has_link(e))
        memcpy(h.linkname, e->link, strnlen(e->link, sizeof(h.linkname)));
    memcpy(h.uname, e->uname, strnlen(e->uname, sizeof(h.uname)));
    memcpy(h.gname, e->gname, strnlen(e->gname, sizeof(h.gname)));
    put_octal_or_zero(h.devmajor, sizeof(h.devmajor), e->devmajor);
    put_octal_or_zero(h.devminor, sizeof(h.devminor), e->devminor);

    if (-1 == emit_header(t, &h))
        return -1;

    t->wactive = e->type == TAR_FILE;
    t->wsize = t->wactive ? e->size : 0;
    t->wdata = 0;
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
    pax_buf_free(&t->global_records);
    pax_buf_free(&t->entry_extra);
    pax_buf_free(&t->ext_data);
    free(t);
    return res;
}
