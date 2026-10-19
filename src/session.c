#include "session.h"
#include "archive_store.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int check_room(const le_session *s, const char *given_name,
                      char *err, size_t errlen) {
    if (s->count < LE_MAX_ARCHIVES)
        return 0;
    le_seterr(err, errlen, "%s: at most %d archives can be edited at once",
              given_name, LE_MAX_ARCHIVES);
    return -1;
}

int session_add_store(le_session *s, le_store *store, const char *identity,
                      const char *given_name, char *err, size_t errlen) {
    if (!store) {
        le_seterr(err, errlen, "%s: out of memory", given_name);
        return -1;
    }
    if (check_room(s, given_name, err, errlen) == -1) {
        store_free(store);
        return -1;
    }
    for (int i = 0; i < s->count; i++)
        if (!strcmp(identity, s->slots[i].identity)) {
            le_seterr(err, errlen, "%s: already loaded as '%s'", given_name,
                      s->slots[i].given_name);
            store_free(store);
            return -1;
        }
    le_session_slot *slot = &s->slots[s->count++];
    slot->store = store;
    strlcpy(slot->identity, identity, sizeof(slot->identity));
    strlcpy(slot->given_name, given_name, sizeof(slot->given_name));
    return 0;
}

int session_add(le_session *s, const char *path, char *err, size_t errlen) {
    if (check_room(s, path, err, errlen) == -1)
        return -1;
    char resolved[PATH_MAX];
    if (!realpath(path, resolved)) {
        le_seterr(err, errlen, "%s: %s", path, strerror(errno));
        return -1;
    }
    return session_add_store(s, archive_store_new(path), resolved, path, err,
                             errlen);
}

le_store *session_store(const le_session *s, int idx) {
    return idx >= 0 && idx < s->count ? s->slots[idx].store : NULL;
}

const char *session_label(const le_session *s, int idx) {
    le_store *store = session_store(s, idx);
    return store ? store_label(store) : "";
}

void session_free(le_session *s) {
    for (int i = 0; i < s->count; i++)
        store_free(s->slots[i].store);
    s->count = 0;
}
