#pragma once

#include "tree.h"

#include <limits.h>

/*
 * The ordered set of archives being edited. The session owns identity (path,
 * display label, load order); unsaved state lives in the model's per-archive
 * flags, so archive index i here is model slot i there.
 */

typedef struct {
    char path[PATH_MAX];        /* path as given on the command line */
    char resolved[PATH_MAX];    /* realpath, for duplicate detection */
    char label[256];            /* basename, for display */
} le_archive;

typedef struct {
    le_archive archives[LE_MAX_ARCHIVES];
    int        count;
} le_session;

/* Append an archive. Returns 0, or -1 with err set when the session is full,
 * the path cannot be resolved, or it is already loaded. */
int session_add(le_session *s, const char *path, char *err, size_t errlen);

const char *session_path(const le_session *s, int idx);
const char *session_label(const le_session *s, int idx);
