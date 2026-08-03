#include "session.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <libgen.h>

int session_add(le_session *s, const char *path, char *err, size_t errlen) {
    if (s->count >= LE_MAX_ARCHIVES) {
        le_seterr(err, errlen, "%s: at most %d archives can be edited at once",
                  path, LE_MAX_ARCHIVES);
        return -1;
    }

    char resolved[PATH_MAX];
    if (!realpath(path, resolved)) {
        le_seterr(err, errlen, "%s: %s", path, strerror(errno));
        return -1;
    }
    for (int i = 0; i < s->count; i++)
        if (!strcmp(resolved, s->archives[i].resolved)) {
            le_seterr(err, errlen, "%s: already loaded as '%s'", path,
                      s->archives[i].path);
            return -1;
        }

    le_archive *a = &s->archives[s->count];
    strlcpy(a->path, path, sizeof(a->path));
    strlcpy(a->resolved, resolved, sizeof(a->resolved));

    char labelbuf[PATH_MAX];
    strlcpy(labelbuf, path, sizeof(labelbuf));
    strlcpy(a->label, basename(labelbuf), sizeof(a->label));

    s->count++;
    return 0;
}

const char *session_path(const le_session *s, int idx) {
    if (idx < 0 || idx >= s->count)
        return "";
    return s->archives[idx].path;
}

const char *session_label(const le_session *s, int idx) {
    if (idx < 0 || idx >= s->count)
        return "";
    return s->archives[idx].label;
}
