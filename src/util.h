#pragma once

#include <stddef.h>

/* Format a human-readable message into an optional caller-supplied buffer. */
void le_seterr(char *err, size_t errlen, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
