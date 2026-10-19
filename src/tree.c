#define _GNU_SOURCE /* strcasestr on glibc */
#include "tree.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>

static int valid_arch(const le_model *m, int arch_idx) {
    return arch_idx >= 0 && arch_idx < m->narchives;
}

static void touch(le_model *m) {
    m->generation++;
}

/* ---- path components ---- */

/* Copy the next component of *p into comp and advance past it. Slashes and
 * "." components are skipped, so "./a//b", "/a/b" and "a/b" read the same.
 * Returns 0 when no components remain. */
static int next_component(const char **p, char *comp, size_t cap) {
    for (;;) {
        while (**p == '/')
            (*p)++;
        if (!**p)
            return 0;
        size_t clen = strcspn(*p, "/");
        if (clen == 1 && (*p)[0] == '.') {
            *p += 1;
            continue;
        }
        size_t copy_len = clen < cap - 1 ? clen : cap - 1;
        memcpy(comp, *p, copy_len);
        comp[copy_len] = '\0';
        *p += clen;
        return 1;
    }
}

static int path_has_more(const char *p) {
    char scratch[PATH_MAX];
    return next_component(&p, scratch, sizeof(scratch));
}

static int has_whiteout_prefix(const char *name) {
    return strncmp(name, LE_WHITEOUT_PREFIX, sizeof(LE_WHITEOUT_PREFIX) - 1) == 0;
}

/* Split into parent path (normalized, "" at the top) and last component. */
static int split_path(const char *path, char *parent, size_t parent_len,
                      char *base, size_t base_len) {
    char comp[PATH_MAX];
    const char *p = path;
    size_t off = 0;
    parent[0] = '\0';
    base[0] = '\0';
    while (next_component(&p, comp, sizeof(comp))) {
        if (base[0]) {
            int wrote = snprintf(parent + off, parent_len - off, "%s%s",
                                 off ? "/" : "", base);
            if (wrote < 0 || (size_t)wrote >= parent_len - off)
                return -1;
            off += (size_t)wrote;
        }
        if (strlcpy(base, comp, base_len) >= base_len)
            return -1;
    }
    return base[0] ? 0 : -1;
}

int model_check_guest_path(const char *path, char *err, size_t errlen) {
    if (strlen(path) >= PATH_MAX) {
        le_seterr(err, errlen, "path is too long");
        return -1;
    }
    int ncomponents = 0;
    for (const char *p = path; *p; ) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        size_t clen = strcspn(p, "/");
        if ((clen == 1 && p[0] == '.') ||
            (clen == 2 && p[0] == '.' && p[1] == '.')) {
            le_seterr(err, errlen, "'%s': '.' and '..' are not allowed", path);
            return -1;
        }
        ncomponents++;
        p += clen;
    }
    if (!ncomponents) {
        le_seterr(err, errlen, "empty path");
        return -1;
    }
    return 0;
}

/* ---- metadata ---- */

static void meta_assign(le_meta *dst, const le_meta *src) {
    char *link = src->link ? strdup(src->link) : NULL;
    free(dst->link);
    *dst = *src;
    dst->link = link;
}

static void meta_clear(le_meta *meta) {
    free(meta->link);
    memset(meta, 0, sizeof(*meta));
}

static int meta_conflicts(const le_meta *a, const le_meta *b) {
    return a->kind != b->kind || (a->kind != LE_KIND_DIR && a->size != b->size);
}

static void staged_free(le_staged *s) {
    if (!s)
        return;
    free(s->host_path);
    meta_clear(&s->meta);
    free(s);
}

/* ---- children (sorted array) ---- */

/* Lower-bound index for name; sets *found when an exact match exists. */
static size_t child_lb(const le_node *p, const char *name, int *found) {
    size_t lo = 0, hi = p->nchildren;
    *found = 0;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(p->children[mid]->name, name);
        if (c == 0) { *found = 1; return mid; }
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static le_node *child_find(const le_node *p, const char *name) {
    int found;
    size_t idx = child_lb(p, name, &found);
    return found ? p->children[idx] : NULL;
}

static le_node *node_new(const char *name, le_node *parent) {
    le_node *n = calloc(1, sizeof(*n));
    if (!n)
        return NULL;
    n->name = strdup(name);
    if (!n->name) {
        free(n);
        return NULL;
    }
    n->parent = parent;
    return n;
}

static le_node *child_find_or_add(le_node *p, const char *name, int *created) {
    int found;
    size_t idx = child_lb(p, name, &found);
    *created = 0;
    if (found)
        return p->children[idx];

    if (p->nchildren == p->cap) {
        size_t ncap = p->cap ? p->cap * 2 : 8;
        le_node **nc = realloc(p->children, ncap * sizeof(*nc));
        if (!nc)
            return NULL;
        p->children = nc;
        p->cap = ncap;
    }
    le_node *child = node_new(name, p);
    if (!child)
        return NULL;
    memmove(&p->children[idx + 1], &p->children[idx],
            (p->nchildren - idx) * sizeof(*p->children));
    p->children[idx] = child;
    p->nchildren++;
    *created = 1;
    return child;
}

/* ---- model lifecycle ---- */

le_model *model_new(int narchives) {
    le_model *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->root = node_new("", NULL);
    if (!m->root) {
        free(m);
        return NULL;
    }
    if (narchives < 1)
        narchives = 1;
    if (narchives > LE_MAX_ARCHIVES)
        narchives = LE_MAX_ARCHIVES;
    m->narchives = narchives;
    m->root->meta.kind = LE_KIND_DIR;
    m->root->expanded = 1;
    return m;
}

static void node_free(le_node *n) {
    for (size_t i = 0; i < n->nchildren; i++)
        node_free(n->children[i]);
    free(n->children);
    free(n->name);
    meta_clear(&n->meta);
    for (int i = 0; i < LE_MAX_ARCHIVES; i++)
        staged_free(n->arch[i].staged);
    free(n);
}

void model_free(le_model *m) {
    if (!m)
        return;
    node_free(m->root);
    free(m);
}

/* ---- counters ---- */

static void count_change(size_t *arch_counter, size_t *total, int delta) {
    if (delta > 0) {
        (*arch_counter)++;
        (*total)++;
        return;
    }
    if (*arch_counter)
        (*arch_counter)--;
    if (*total)
        (*total)--;
}

int model_arch_dirty(const le_model *m, int arch_idx) {
    return valid_arch(m, arch_idx) &&
           (m->arch_marked[arch_idx] || m->arch_added[arch_idx]);
}

int model_dirty(const le_model *m) {
    for (int i = 0; i < m->narchives; i++)
        if (model_arch_dirty(m, i))
            return 1;
    return 0;
}

/* ---- insertion ---- */

/* Create every node along path, returning the leaf. Interior components become
 * directories. When mark_present, each component is recorded as belonging to
 * arch_idx (staged additions do not exist in the archive yet, so they don't). */
static le_node *path_create(le_model *m, const char *path, int arch_idx,
                            int mark_present) {
    le_node *cur = m->root;
    const char *p = path;
    char comp[PATH_MAX];
    while (next_component(&p, comp, sizeof(comp))) {
        int created;
        cur = child_find_or_add(cur, comp, &created);
        if (!cur)
            return NULL;
        if (created)
            m->total++;
        if (mark_present)
            cur->arch[arch_idx].present = 1;
        if (path_has_more(p)) {          /* interior component is a directory */
            if (cur->meta_set && !node_is_dir(cur))
                cur->mismatch = 1;       /* another archive has a file here */
            cur->meta.kind = LE_KIND_DIR;
        }
    }
    return cur == m->root ? NULL : cur;  /* empty/degenerate path */
}

/* Adopt display metadata from an explicit archive entry, or flag a conflict
 * when another archive already described this path differently. */
static void apply_metadata(le_node *n, const le_meta *meta) {
    if (n->meta_set) {
        if (meta_conflicts(&n->meta, meta))
            n->mismatch = 1;
        return;
    }
    int inferred_dir = node_is_dir(n) && n->nchildren > 0;
    meta_assign(&n->meta, meta);
    if (inferred_dir && meta->kind != LE_KIND_DIR) {
        n->meta.kind = LE_KIND_DIR;      /* children elsewhere need a dir */
        n->mismatch = 1;
    }
    n->meta_set = 1;
}

le_node *model_insert(le_model *m, const char *path, int arch_idx,
                      const le_meta *meta, le_change_kind change_kind) {
    if (!valid_arch(m, arch_idx))
        return NULL;
    le_node *n = path_create(m, path, arch_idx, 1);
    if (!n)
        return NULL;
    apply_metadata(n, meta);
    n->arch[arch_idx].change_kind = (unsigned char)change_kind;
    touch(m);
    return n;
}

/* ---- lookup ---- */

le_node *model_lookup(le_model *m, const char *path) {
    le_node *cur = m->root;
    const char *p = path;
    char comp[PATH_MAX];
    while (cur && next_component(&p, comp, sizeof(comp)))
        cur = child_find(cur, comp);
    return cur == m->root ? NULL : cur;
}

int node_in_archive(const le_node *n, int arch_idx) {
    if (!n || arch_idx < 0 || arch_idx >= LE_MAX_ARCHIVES)
        return 0;
    return n->arch[arch_idx].present || n->arch[arch_idx].staged;
}

int node_archives(const le_node *n, int narchives, int *out) {
    int count = 0;
    if (narchives > LE_MAX_ARCHIVES)
        narchives = LE_MAX_ARCHIVES;
    for (int i = 0; i < narchives; i++)
        if (node_in_archive(n, i))
            out[count++] = i;
    return count;
}

int node_is_dir(const le_node *n) {
    return n->meta.kind == LE_KIND_DIR;
}

int node_is_whiteout(const le_node *n) {
    return n->parent && has_whiteout_prefix(n->name);
}

const char *node_whiteout_target(const le_node *n) {
    if (!strcmp(n->name, LE_OPAQUE_WHITEOUT))
        return "";
    return n->name + sizeof(LE_WHITEOUT_PREFIX) - 1;
}

const le_meta *node_display_meta(const le_node *n, int narchives) {
    for (int i = 0; i < narchives && i < LE_MAX_ARCHIVES; i++)
        if (n->arch[i].staged)
            return &n->arch[i].staged->meta;
    return &n->meta;
}

int node_effectively_removed(const le_node *n, int arch_idx) {
    if (arch_idx < 0 || arch_idx >= LE_MAX_ARCHIVES)
        return 0;
    for (const le_node *c = n; c; c = c->parent)
        if (c->arch[arch_idx].removed)
            return 1;
    return 0;
}

int node_path(const le_node *n, char *buf, size_t len) {
    if (!buf || len == 0)
        return -1;
    buf[0] = '\0';
    if (!n || !n->parent)
        return 0;

    size_t depth = 0;
    for (const le_node *c = n; c && c->parent; c = c->parent)
        depth++;

    const le_node **stack = malloc(depth * sizeof(*stack));
    if (!stack)
        return -1;
    size_t cnt = 0;
    for (const le_node *c = n; c && c->parent; c = c->parent)
        stack[cnt++] = c;

    size_t off = 0;
    int truncated = 0;
    for (size_t i = cnt; i > 0; i--) {
        const char *name = stack[i - 1]->name;
        int wrote = snprintf(buf + off, len - off, "%s%s",
                             name, i > 1 ? "/" : "");
        if (wrote < 0 || (size_t)wrote >= len - off) {
            truncated = 1;
            break;
        }
        off += (size_t)wrote;
    }
    free(stack);
    return truncated ? -1 : 0;
}

void model_default_owner(le_model *m, const char *dest, uid_t *uid, gid_t *gid) {
    const le_node *cur = m->root, *nearest_explicit = NULL;
    const char *p = dest;
    char comp[PATH_MAX];
    while (cur && next_component(&p, comp, sizeof(comp))) {
        cur = child_find(cur, comp);
        if (cur && cur->meta_set)
            nearest_explicit = cur;
    }
    *uid = nearest_explicit ? nearest_explicit->meta.uid : 0;
    *gid = nearest_explicit ? nearest_explicit->meta.gid : 0;
}

/* ---- pruning ---- */

static void child_remove_at(le_node *parent, size_t idx) {
    le_node *victim = parent->children[idx];
    memmove(&parent->children[idx], &parent->children[idx + 1],
            (parent->nchildren - idx - 1) * sizeof(*parent->children));
    parent->nchildren--;
    node_free(victim);
}

/* True while any archive still holds this path or one below it. */
static int subtree_in_use(const le_node *n, int narchives) {
    for (int i = 0; i < narchives; i++)
        if (node_in_archive(n, i))
            return 1;
    for (size_t i = 0; i < n->nchildren; i++)
        if (subtree_in_use(n->children[i], narchives))
            return 1;
    return 0;
}

static size_t count_members(const le_node *n) {
    size_t total = 0;
    for (size_t i = 0; i < n->nchildren; i++)
        total += 1 + count_members(n->children[i]);
    return total;
}

/* Free n and every parent that no archive holds any more. */
static void prune_unused(le_model *m, le_node *n) {
    while (n && n->parent && !subtree_in_use(n, m->narchives)) {
        le_node *parent = n->parent;
        int found;
        size_t idx = child_lb(parent, n->name, &found);
        if (!found)
            return;
        size_t removed = 1 + count_members(n);
        m->total = m->total > removed ? m->total - removed : 0;
        child_remove_at(parent, idx);
        n = parent;
    }
}

/* Free every child subtree that no archive holds any more. */
static void drop_unused_children(le_model *m, le_node *n) {
    for (size_t i = n->nchildren; i-- > 0;) {
        le_node *c = n->children[i];
        if (subtree_in_use(c, m->narchives)) {
            drop_unused_children(m, c);
            continue;
        }
        size_t removed = 1 + count_members(c);
        m->total = m->total > removed ? m->total - removed : 0;
        child_remove_at(n, i);
    }
}

static void set_removed(le_model *m, le_node *n, int arch_idx, int removed) {
    if (!!n->arch[arch_idx].removed == !!removed)
        return;
    n->arch[arch_idx].removed = removed ? 1 : 0;
    count_change(&m->arch_marked[arch_idx], &m->marked, removed ? +1 : -1);
    touch(m);
}

/* ---- staging ---- */

/* Check that path can take a new non-directory entry in arch_idx. */
static int check_stage_target(le_model *m, const char *path, int arch_idx,
                              char *err, size_t errlen) {
    if (model_check_guest_path(path, err, errlen) == -1)
        return -1;
    const le_node *cur = m->root;
    const char *p = path;
    char comp[PATH_MAX], where[PATH_MAX];
    while (next_component(&p, comp, sizeof(comp))) {
        const le_node *next = child_find(cur, comp);
        if (!next)
            return 0;                    /* the rest of the path is new */
        int is_leaf = !path_has_more(p);
        node_path(next, where, sizeof(where));
        if (is_leaf && node_is_dir(next)) {
            le_seterr(err, errlen, "'%s' is a directory", where);
            return -1;
        }
        if (!is_leaf && !node_is_dir(next)) {
            le_seterr(err, errlen, "'%s' is not a directory", where);
            return -1;
        }
        if (next->arch[arch_idx].removed) {
            le_seterr(err, errlen,
                      "'%s' is marked for removal in this archive; unmark it first",
                      where);
            return -1;
        }
        cur = next;
    }
    return 0;
}

static void stage(le_model *m, le_node *n, int arch_idx, le_staged *s) {
    if (n->arch[arch_idx].staged)
        staged_free(n->arch[arch_idx].staged);
    else
        count_change(&m->arch_added[arch_idx], &m->added, +1);
    n->arch[arch_idx].staged = s;
    for (le_node *a = n->parent; a && a->parent; a = a->parent)
        a->expanded = 1;                 /* reveal the addition */
    touch(m);
}

static le_staged *staged_new(le_staged_source source, const char *host_path,
                             const le_meta *meta, le_change_kind change_kind) {
    le_staged *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->source = source;
    s->change_kind = change_kind;
    meta_assign(&s->meta, meta);
    if (host_path && !(s->host_path = strdup(host_path))) {
        staged_free(s);
        return NULL;
    }
    return s;
}

static le_node *stage_at(le_model *m, const char *path, int arch_idx,
                         le_staged *s, char *err, size_t errlen) {
    le_node *n = s ? path_create(m, path, arch_idx, 0) : NULL;
    if (!n) {
        staged_free(s);
        le_seterr(err, errlen, "out of memory");
        return NULL;
    }
    stage(m, n, arch_idx, s);
    return n;
}

le_node *model_stage_host_file(le_model *m, const char *dest, int arch_idx,
                               const char *host_path, const le_meta *meta,
                               char *err, size_t errlen) {
    if (!valid_arch(m, arch_idx)) {
        le_seterr(err, errlen, "invalid archive slot %d", arch_idx);
        return NULL;
    }
    if (check_stage_target(m, dest, arch_idx, err, errlen) == -1)
        return NULL;
    char parent[PATH_MAX], base[PATH_MAX];
    if (split_path(dest, parent, sizeof(parent), base, sizeof(base)) == -1) {
        le_seterr(err, errlen, "'%s': invalid path", dest);
        return NULL;
    }
    if (has_whiteout_prefix(base)) {
        le_seterr(err, errlen, "'%s' is a whiteout name; use w to stage a whiteout",
                  base);
        return NULL;
    }
    le_node *existing = model_lookup(m, dest);
    le_change_kind kind = existing && existing->arch[arch_idx].present
                              ? existing->arch[arch_idx].change_kind
                              : LE_CHANGE_UNTAGGED;
    return stage_at(m, dest, arch_idx,
                    staged_new(LE_STAGED_HOST_FILE, host_path, meta, kind),
                    err, errlen);
}

int model_whiteout_path(const char *target_path, char *whiteout_path,
                        size_t len, char *err, size_t errlen) {
    char parent[PATH_MAX], base[PATH_MAX];
    if (split_path(target_path, parent, sizeof(parent), base, sizeof(base)) == -1) {
        le_seterr(err, errlen, "'%s': invalid path", target_path);
        return -1;
    }
    if (has_whiteout_prefix(base)) {
        le_seterr(err, errlen, "'%s' is already a whiteout", base);
        return -1;
    }
    int wrote = snprintf(whiteout_path, len, "%s%s%s%s", parent,
                         parent[0] ? "/" : "", LE_WHITEOUT_PREFIX, base);
    if (wrote < 0 || (size_t)wrote >= len) {
        le_seterr(err, errlen, "path is too long");
        return -1;
    }
    return 0;
}

le_node *model_stage_whiteout(le_model *m, const char *target_path,
                              int arch_idx, int target_is_dir,
                              char *err, size_t errlen) {
    if (!valid_arch(m, arch_idx)) {
        le_seterr(err, errlen, "invalid archive slot %d", arch_idx);
        return NULL;
    }
    if (model_check_guest_path(target_path, err, errlen) == -1)
        return NULL;
    char whiteout_path[PATH_MAX];
    if (model_whiteout_path(target_path, whiteout_path, sizeof(whiteout_path),
                            err, errlen) == -1)
        return NULL;

    le_node *existing = model_lookup(m, whiteout_path);
    if (existing && existing->arch[arch_idx].staged) {
        le_seterr(err, errlen, "a whiteout for '%s' is already staged",
                  target_path);
        return NULL;
    }
    if (existing && existing->arch[arch_idx].present) {
        le_seterr(err, errlen, "this archive already has a whiteout for '%s'",
                  target_path);
        return NULL;
    }
    if (check_stage_target(m, whiteout_path, arch_idx, err, errlen) == -1)
        return NULL;

    le_meta meta = {
        .kind = target_is_dir ? LE_KIND_DIR : LE_KIND_FILE,
        .mode = target_is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644),
    };
    model_default_owner(m, target_path, &meta.uid, &meta.gid);
    le_node *n = stage_at(m, whiteout_path, arch_idx,
                          staged_new(LE_STAGED_WHITEOUT, NULL, &meta,
                                     LE_CHANGE_WHITEOUT),
                          err, errlen);
    if (!n)
        return NULL;

    /* The layer should not also carry what it deletes. */
    le_node *target = model_lookup(m, target_path);
    if (target && target->arch[arch_idx].staged) {
        model_unstage(m, target, arch_idx);
    } else if (target && target->arch[arch_idx].present &&
               !target->arch[arch_idx].removed) {
        set_removed(m, target, arch_idx, 1);
        n->arch[arch_idx].staged->marked_target = 1;
    }
    return n;
}

void model_unstage(le_model *m, le_node *n, int arch_idx) {
    if (!n || !valid_arch(m, arch_idx) || !n->arch[arch_idx].staged)
        return;
    if (n->arch[arch_idx].staged->marked_target) {
        le_node *target = child_find(n->parent, node_whiteout_target(n));
        if (target)
            set_removed(m, target, arch_idx, 0);
    }
    staged_free(n->arch[arch_idx].staged);
    n->arch[arch_idx].staged = NULL;
    count_change(&m->arch_added[arch_idx], &m->added, -1);
    touch(m);
    prune_unused(m, n);
}

/* ---- removal ---- */

void model_toggle_removed(le_model *m, le_node *n, int arch_idx) {
    if (!n || n == m->root || !valid_arch(m, arch_idx))
        return;
    if (!n->arch[arch_idx].present)
        return;
    set_removed(m, n, arch_idx, !n->arch[arch_idx].removed);
}

void model_toggle_removed_all(le_model *m, le_node *n) {
    if (!n || n == m->root)
        return;
    int any_removed = 0;
    for (int i = 0; i < m->narchives; i++)
        if (n->arch[i].present && n->arch[i].removed)
            any_removed = 1;
    for (int i = 0; i < m->narchives; i++)
        if (n->arch[i].present)
            set_removed(m, n, i, !any_removed);
}

le_mark_result model_toggle_mark(le_model *m, le_node *n, int arch_idx) {
    if (!n || n == m->root || !valid_arch(m, arch_idx))
        return LE_MARK_NONE;
    if (n->arch[arch_idx].staged) {
        model_unstage(m, n, arch_idx);
        return LE_MARK_UNSTAGED;
    }
    if (!n->arch[arch_idx].present)
        return LE_MARK_NONE;
    set_removed(m, n, arch_idx, !n->arch[arch_idx].removed);
    return n->arch[arch_idx].removed ? LE_MARK_REMOVED : LE_MARK_UNMARKED;
}

le_mark_result model_toggle_mark_all(le_model *m, le_node *n) {
    if (!n || n == m->root)
        return LE_MARK_NONE;
    int staged_slots[LE_MAX_ARCHIVES], nstaged = 0, first_present = -1;
    for (int i = 0; i < m->narchives; i++) {
        if (n->arch[i].staged)
            staged_slots[nstaged++] = i;
        else if (n->arch[i].present && first_present < 0)
            first_present = i;
    }
    if (nstaged) {
        /* Later staged slots keep n alive until the last unstage. */
        for (int i = 0; i < nstaged; i++)
            model_unstage(m, n, staged_slots[i]);
        return LE_MARK_UNSTAGED;
    }
    if (first_present < 0)
        return LE_MARK_NONE;
    model_toggle_removed_all(m, n);
    return n->arch[first_present].removed ? LE_MARK_REMOVED : LE_MARK_UNMARKED;
}

/* ---- reconcile after save ---- */

/* Forget everything one archive knew about this subtree. */
static void clear_arch_subtree(le_node *n, int arch_idx) {
    n->arch[arch_idx].present = 0;
    n->arch[arch_idx].removed = 0;
    n->arch[arch_idx].change_kind = 0;
    staged_free(n->arch[arch_idx].staged);
    n->arch[arch_idx].staged = NULL;
    for (size_t i = 0; i < n->nchildren; i++)
        clear_arch_subtree(n->children[i], arch_idx);
}

void model_clear_slot(le_model *m, int arch_idx) {
    if (!valid_arch(m, arch_idx))
        return;
    clear_arch_subtree(m->root, arch_idx);
    drop_unused_children(m, m->root);
    m->marked -= m->arch_marked[arch_idx];
    m->added -= m->arch_added[arch_idx];
    m->arch_marked[arch_idx] = 0;
    m->arch_added[arch_idx] = 0;
    touch(m);
}

/* The staged entry is now a member of that archive. */
static void promote_staged(le_node *n, int arch_idx) {
    le_staged *s = n->arch[arch_idx].staged;
    meta_assign(&n->meta, &s->meta);
    n->meta_set = 1;
    n->arch[arch_idx].present = 1;
    n->arch[arch_idx].change_kind = (unsigned char)s->change_kind;
    staged_free(s);
    n->arch[arch_idx].staged = NULL;
    for (le_node *a = n->parent; a && a->parent; a = a->parent)
        a->arch[arch_idx].present = 1;
}

static void reconcile_node(le_node *n, int arch_idx, int narchives) {
    for (size_t i = 0; i < n->nchildren; ) {
        le_node *c = n->children[i];
        if (c->arch[arch_idx].removed) {
            clear_arch_subtree(c, arch_idx);
        } else {
            if (c->arch[arch_idx].staged)
                promote_staged(c, arch_idx);
            reconcile_node(c, arch_idx, narchives);
        }
        if (!subtree_in_use(c, narchives)) {
            child_remove_at(n, i);
            continue;
        }
        i++;
    }
}

void model_reconcile_after_save(le_model *m, int arch_idx) {
    if (!m || !m->root || !valid_arch(m, arch_idx))
        return;
    reconcile_node(m->root, arch_idx, m->narchives);

    m->arch_marked[arch_idx] = 0;
    m->arch_added[arch_idx] = 0;
    m->marked = 0;
    m->added = 0;
    for (int i = 0; i < m->narchives; i++) {
        m->marked += m->arch_marked[i];
        m->added += m->arch_added[i];
    }
    m->total = count_members(m->root);
    touch(m);
}

/* ---- flatten ---- */

/* Mark every node whose name matches filter, and propagate hits up to ancestors
 * so flatten can decide visibility in O(n) instead of re-walking subtrees. */
static int mark_filter_hits(le_node *n, const char *filter) {
    int hit = filter && *filter && strcasestr(n->name, filter) != NULL;
    for (size_t i = 0; i < n->nchildren; i++)
        if (mark_filter_hits(n->children[i], filter))
            hit = 1;
    n->filter_hit = hit;
    return hit;
}

static void row_push(le_row **out, size_t *n, size_t *cap,
                     le_node *node, int depth) {
    if (*n == *cap) {
        size_t ncap = *cap ? *cap * 2 : 256;
        le_row *nr = realloc(*out, ncap * sizeof(*nr));
        if (!nr)
            return;
        *out = nr;
        *cap = ncap;
    }
    (*out)[*n].node = node;
    (*out)[*n].depth = depth;
    (*n)++;
}

static void flatten_rec(le_node *node, int depth, int filtering,
                        le_row **out, size_t *n, size_t *cap) {
    for (size_t i = 0; i < node->nchildren; i++) {
        le_node *c = node->children[i];
        if (filtering && !c->filter_hit)
            continue;
        row_push(out, n, cap, c, depth);
        if (node_is_dir(c) && (c->expanded || filtering))
            flatten_rec(c, depth + 1, filtering, out, n, cap);
    }
}

size_t model_flatten(le_model *m, const char *filter,
                     le_row **out, size_t *cap) {
    size_t n = 0;
    int filtering = filter && *filter;
    if (filtering)
        mark_filter_hits(m->root, filter);
    flatten_rec(m->root, 0, filtering, out, &n, cap);
    return n;
}
