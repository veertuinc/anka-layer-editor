#include "json_scan.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char *skip_space(const char *p) {
    while (isspace((unsigned char)*p))
        p++;
    return p;
}

/* Copy the string that starts after the opening quote at p. Returns a pointer
 * after the closing quote, or NULL when the string does not end. */
static void append_char(char *value, size_t value_len, size_t *used, char c) {
    if (*used + 1 < value_len)
        value[(*used)++] = c;
}

static const char *copy_string_value(const char *p, char *value,
                                     size_t value_len) {
    size_t used = 0;
    for (; *p && *p != '"'; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p != '"' && *p != '\\' && *p != '/')
                append_char(value, value_len, &used, '\\');
        }
        append_char(value, value_len, &used, *p);
    }
    if (value_len)
        value[used] = '\0';
    return *p == '"' ? p + 1 : NULL;
}

const char *json_scan_string(const char *json, const char *key, char *value,
                             size_t value_len) {
    if (!json)
        return NULL;
    char quoted_key[256];
    int wrote = snprintf(quoted_key, sizeof(quoted_key), "\"%s\"", key);
    if (wrote < 0 || (size_t)wrote >= sizeof(quoted_key))
        return NULL;
    for (const char *hit = strstr(json, quoted_key); hit;
         hit = strstr(hit + 1, quoted_key)) {
        const char *p = skip_space(hit + wrote);
        if (*p != ':')
            continue;
        p = skip_space(p + 1);
        if (*p != '"')
            continue;
        return copy_string_value(p + 1, value, value_len);
    }
    return NULL;
}

const char *json_scan_body(const char *json) {
    const char *body = json ? strstr(json, "\"body\"") : NULL;
    return body ? body + strlen("\"body\"") : NULL;
}
