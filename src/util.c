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
