#include "archive_store.h"
#include "archive.h"
#include "member_read.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <libgen.h>

typedef struct {
    le_store base;
    char     path[PATH_MAX];
    char     label[256];
} archive_store;

static const char *archive_store_label(const le_store *store) {
    return ((const archive_store *)store)->label;
}

static int archive_store_load(le_store *store, le_model *m, int arch_idx,
                              char *err, size_t errlen) {
    return archive_load(((archive_store *)store)->path, m, arch_idx, err, errlen);
}

static int archive_store_save(le_store *store, le_model *m, int arch_idx,
                              const char *save_name, char *err, size_t errlen) {
    return archive_save(((archive_store *)store)->path, m, arch_idx, err, errlen);
}

static int archive_store_read_member(const le_store *store,
                                     const char *guest_path,
                                     le_member_bytes *out, char *err,
                                     size_t errlen) {
    return member_read_from_archive(((const archive_store *)store)->path,
                                    guest_path, MEMBER_PREVIEW_MAX, out, err,
                                    errlen);
}

static void archive_store_free(le_store *store) {
    free(store);
}

static const le_store_ops archive_store_ops = {
    .label = archive_store_label,
    .load = archive_store_load,
    .save = archive_store_save,
    .read_member = archive_store_read_member,
    .free = archive_store_free,
};

le_store *archive_store_new(const char *path) {
    archive_store *store = calloc(1, sizeof(*store));
    if (!store)
        return NULL;
    store->base.ops = &archive_store_ops;
    strlcpy(store->path, path, sizeof(store->path));
    char label_source[PATH_MAX];
    strlcpy(label_source, path, sizeof(label_source));
    strlcpy(store->label, basename(label_source), sizeof(store->label));
    return &store->base;
}
