#include "member_read.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

void member_bytes_free(le_member_bytes *bytes) {
    if (!bytes)
        return;
    free(bytes->data);
    bytes->data = NULL;
    bytes->len = 0;
    bytes->truncated = 0;
}

static const char *guest_path_skip_prefix(const char *path) {
    if (path[0] == '.' && path[1] == '/')
        path += 2;
    while (*path == '/')
        path++;
    return path;
}

static int guest_path_matches(const char *entry, const char *want) {
    return !strcmp(guest_path_skip_prefix(entry), guest_path_skip_prefix(want));
}

int member_read_current(tar_t *in, size_t max_bytes, le_member_bytes *out,
                        char *err, size_t errlen) {
    char *data = malloc(max_bytes + 1);
    if (!data) {
        le_seterr(err, errlen, "out of memory");
        return -1;
    }
    size_t got = 0;
    while (got < max_bytes) {
        ssize_t n = tar_read_data(in, data + got, max_bytes - got);
        if (n < 0) {
            free(data);
            le_seterr(err, errlen, "read error: %s", strerror(errno));
            return -1;
        }
        if (n == 0)
            break;
        got += (size_t)n;
    }
    int truncated = 0;
    if (got == max_bytes) {
        char extra;
        ssize_t n = tar_read_data(in, &extra, 1);
        if (n < 0) {
            free(data);
            le_seterr(err, errlen, "read error: %s", strerror(errno));
            return -1;
        }
        if (n > 0)
            truncated = 1;
    }
    if (tar_skip_data(in) == -1) {
        free(data);
        le_seterr(err, errlen, "read error: %s", strerror(errno));
        return -1;
    }
    data[got] = '\0';
    out->data = data;
    out->len = got;
    out->truncated = truncated;
    return 0;
}

int member_read_from_tar(tar_t *in, const char *guest_path, size_t max_bytes,
                         le_member_bytes *out, char *err, size_t errlen) {
    tar_entry e;
    int r;
    while ((r = tar_read_next(in, &e)) == 1) {
        if (!guest_path_matches(e.path, guest_path)) {
            if (tar_skip_data(in) == -1) {
                le_seterr(err, errlen, "%s: read error", guest_path);
                return -1;
            }
            continue;
        }
        if (e.type != TAR_FILE) {
            tar_skip_data(in);
            le_seterr(err, errlen, "%s: not a regular file", guest_path);
            return -1;
        }
        return member_read_current(in, max_bytes, out, err, errlen);
    }
    if (r < 0) {
        le_seterr(err, errlen, "%s: read error: %s", guest_path, strerror(errno));
        return -1;
    }
    le_seterr(err, errlen, "%s: not in the archive", guest_path);
    return -1;
}

int member_read_from_archive(const char *archive_path, const char *guest_path,
                             size_t max_bytes, le_member_bytes *out,
                             char *err, size_t errlen) {
    tar_t *in = tar_ropen(archive_path);
    if (!in) {
        le_seterr(err, errlen, "%s: cannot open: %s", archive_path,
                  strerror(errno));
        return -1;
    }
    int rc = member_read_from_tar(in, guest_path, max_bytes, out, err, errlen);
    tar_close(in);
    return rc;
}

int member_read_from_host(const char *host_path, size_t max_bytes,
                          le_member_bytes *out, char *err, size_t errlen) {
    int fd = open(host_path, O_RDONLY);
    if (fd == -1) {
        le_seterr(err, errlen, "%s: %s", host_path, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) == -1) {
        le_seterr(err, errlen, "%s: %s", host_path, strerror(errno));
        close(fd);
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        le_seterr(err, errlen, "%s: not a regular file", host_path);
        close(fd);
        return -1;
    }
    char *data = malloc(max_bytes + 1);
    if (!data) {
        le_seterr(err, errlen, "out of memory");
        close(fd);
        return -1;
    }
    size_t want = (size_t)st.st_size;
    int truncated = 0;
    if (want > max_bytes) {
        want = max_bytes;
        truncated = 1;
    }
    size_t got = 0;
    while (got < want) {
        ssize_t n = read(fd, data + got, want - got);
        if (n < 0) {
            le_seterr(err, errlen, "%s: %s", host_path, strerror(errno));
            free(data);
            close(fd);
            return -1;
        }
        if (n == 0)
            break;
        got += (size_t)n;
    }
    close(fd);
    data[got] = '\0';
    out->data = data;
    out->len = got;
    out->truncated = truncated;
    return 0;
}

int member_bytes_is_binary(const le_member_bytes *bytes) {
    if (!bytes || !bytes->data)
        return 0;
    size_t n = bytes->len < MEMBER_BINARY_PROBE ? bytes->len : MEMBER_BINARY_PROBE;
    return memchr(bytes->data, '\0', n) != NULL;
}
