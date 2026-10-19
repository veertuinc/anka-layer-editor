#pragma once

#include "store.h"
#include "tree.h"

#include <limits.h>

/*
 * The ordered set of stores being edited. The session owns identity (store,
 * load order, duplicate detection); unsaved state lives in the model's
 * per-archive flags, so slot i here is model slot i there.
 */

typedef struct {
    le_store *store;
    char      identity[PATH_MAX];   /* unique key, e.g. the realpath */
    char      given_name[PATH_MAX]; /* as given on the command line */
} le_session_slot;

typedef struct {
    le_session_slot slots[LE_MAX_ARCHIVES];
    int             count;
} le_session;

/* Append an archive file. Returns 0, or -1 with err set when the session is
 * full, the path cannot be resolved, or it is already loaded. */
int session_add(le_session *s, const char *path, char *err, size_t errlen);

/* Append a store; the session owns it from now on, also on failure. identity
 * detects duplicates. 0, or -1 with err set. */
int session_add_store(le_session *s, le_store *store, const char *identity,
                      const char *given_name, char *err, size_t errlen);

le_store   *session_store(const le_session *s, int idx);
const char *session_label(const le_session *s, int idx);

/* Free every store. */
void session_free(le_session *s);
