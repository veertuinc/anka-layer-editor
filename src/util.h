#pragma once

#include <stddef.h>
#include <string.h>

/* glibc gained strlcpy in 2.38; provide it for older Linux toolchains. */
#if defined(__GLIBC__) && \
    (__GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 38))
#define LE_NEED_STRLCPY 1
size_t strlcpy(char *dst, const char *src, size_t size);
#endif

/* Format a human-readable message into an optional caller-supplied buffer. */
void le_seterr(char *err, size_t errlen, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
