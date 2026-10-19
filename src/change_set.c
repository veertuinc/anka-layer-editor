#include "change_set.h"
#include "staged_write.h"
#include "util.h"

#include <limits.h>

/* Anka's diff writes a directory only when the base image does not have it,
 * and leaves its kind at 0. A directory with no entry of its own is implied by
 * the paths below it and is in the base image. VM mode has one slot, so
 * meta_set tells whether this slot has the entry. */
static int added_by_layer(const le_node *n, int arch_idx) {
    unsigned char change_kind = n->arch[arch_idx].change_kind;
    return change_kind == LE_CHANGE_ADDED ||
           (change_kind == LE_CHANGE_UNTAGGED && n->meta.kind == LE_KIND_DIR &&
            n->meta_set);
}

static int present_members_all_added(const le_node *n, int arch_idx) {
    if (n->arch[arch_idx].present && !added_by_layer(n, arch_idx))
        return 0;
    for (size_t i = 0; i < n->nchildren; i++)
        if (!present_members_all_added(n->children[i], arch_idx))
            return 0;
    return 1;
}

int change_set_may_remove(const le_node *n, int arch_idx) {
    return n->arch[arch_idx].present && present_members_all_added(n, arch_idx);
}

/* True when n is on the disk the layer was applied to: it is a member, or a
 * directory above one. Staged-only paths are not. */
static int on_applied_disk(const le_node *n, int arch_idx) {
    if (n->arch[arch_idx].present)
        return 1;
    for (size_t i = 0; i < n->nchildren; i++)
        if (on_applied_disk(n->children[i], arch_idx))
            return 1;
    return 0;
}

static int guest_path_of(const le_node *n, char *guest, size_t len,
                         char *err, size_t errlen) {
    if (node_path(n, guest, len) == 0)
        return 0;
    le_seterr(err, errlen, "path too deep for node '%s'", n->name);
    return -1;
}

static int write_whiteout_for(tar_t *out, const le_node *n, char *err,
                              size_t errlen) {
    char guest[PATH_MAX], whiteout_path[PATH_MAX];
    if (guest_path_of(n, guest, sizeof(guest), err, errlen) == -1 ||
        model_whiteout_path(guest, whiteout_path, sizeof(whiteout_path), err,
                            errlen) == -1)
        return -1;
    return staged_write_whiteout_entry(out, whiteout_path,
                                       n->meta.kind == LE_KIND_DIR,
                                       n->meta.uid, n->meta.gid, err, errlen);
}

static int write_marked_subtree(tar_t *out, const le_node *n, int arch_idx,
                                char *err, size_t errlen) {
    for (size_t i = 0; i < n->nchildren; i++) {
        const le_node *c = n->children[i];
        if (on_applied_disk(c, arch_idx) &&
            write_marked_subtree(out, c, arch_idx, err, errlen) == -1)
            return -1;
    }
    return write_whiteout_for(out, n, err, errlen);
}

static int write_removals(tar_t *out, const le_node *n, int arch_idx,
                          char *err, size_t errlen) {
    for (size_t i = 0; i < n->nchildren; i++) {
        const le_node *c = n->children[i];
        if (!c->arch[arch_idx].removed) {
            if (write_removals(out, c, arch_idx, err, errlen) == -1)
                return -1;
            continue;
        }
        if (!change_set_may_remove(c, arch_idx)) {
            char guest[PATH_MAX];
            if (guest_path_of(c, guest, sizeof(guest), err, errlen) == 0)
                le_seterr(err, errlen, "'%s' holds a base version, which a "
                          "whiteout cannot bring back", guest);
            return -1;
        }
        if (write_marked_subtree(out, c, arch_idx, err, errlen) == -1)
            return -1;
    }
    return 0;
}

int change_set_write(tar_t *out, const le_model *m, int arch_idx,
                     char *err, size_t errlen) {
    if (write_removals(out, m->root, arch_idx, err, errlen) == -1)
        return -1;
    return staged_write_all(out, m->root, arch_idx, err, errlen);
}
