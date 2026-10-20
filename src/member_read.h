#pragma once

#include "tar.h"

#include <stddef.h>

/* Cap for file preview. Larger files load this many bytes and set truncated. */
#define MEMBER_PREVIEW_MAX ((size_t)8 * 1024 * 1024)
/* Probe this many loaded bytes for a NUL (binary). */
#define MEMBER_BINARY_PROBE 512

/* Heap buffer of one file's payload. data is NUL-terminated; len is the
 * payload length and does not count that terminator. Caller frees with
 * member_bytes_free. */
typedef struct {
    char  *data;
    size_t len;
    int    truncated;
} le_member_bytes;

void member_bytes_free(le_member_bytes *bytes);

/* Copy the current tar entry's data, up to max_bytes. 0, or -1 with err set. */
int member_read_current(tar_t *in, size_t max_bytes, le_member_bytes *out,
                        char *err, size_t errlen);

/* Walk in until guest_path matches a regular file. 0, or -1 with err set. */
int member_read_from_tar(tar_t *in, const char *guest_path, size_t max_bytes,
                         le_member_bytes *out, char *err, size_t errlen);

int member_read_from_archive(const char *archive_path, const char *guest_path,
                             size_t max_bytes, le_member_bytes *out,
                             char *err, size_t errlen);

int member_read_from_host(const char *host_path, size_t max_bytes,
                          le_member_bytes *out, char *err, size_t errlen);

/* True when a NUL sits in the first probe bytes of the payload. */
int member_bytes_is_binary(const le_member_bytes *bytes);
