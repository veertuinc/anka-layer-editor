#include "util.h"

#include <stdio.h>
#include <stdarg.h>

void le_seterr(char *err, size_t errlen, const char *fmt, ...) {
    if (!err || !errlen)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

#ifdef LE_NEED_STRLCPY
size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t src_len = strlen(src);
    if (size) {
        size_t n = src_len < size - 1 ? src_len : size - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return src_len;
}
#endif
