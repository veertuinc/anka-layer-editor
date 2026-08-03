#include "tree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* Skip leading slashes and "./" segments so that "./a/b", "/a/b" and "a/b"
 * all key to the same tree path. */
static const char *normalize_path(const char *p) {
    for (;;) {
        while (*p == '/')
            p++;
        if (p[0] == '.' && p[1] == '/')
            p += 2;
        else
            break;
    }
    return p;
}

/* True when a normalized path refers to the archive root itself ("." or ""). */
static int is_root_path(const char *p) {
    return p[0] == '\0' || (p[0] == '.' && p[1] == '\0');
}

static int valid_arch(const le_model *m, int arch_idx) {
    return arch_idx >= 0 && arch_idx < m->narchives;
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

static le_node *node_new(const char *name, le_node *parent) {
    le_node *n = calloc(1, sizeof(*n));
    if (!n)
        return NULL;
    n->name = strdup(name);
    n->parent = parent;
    return n;
}

static le_node *child_find_or_add(le_node *p, const char *name, int *created) {
    int found;
    size_t idx = child_lb(p, name, &found);
    if (found) {
        if (created)
            *created = 0;
        return p->children[idx];
    }

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
    if (created)
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
    m->root->is_dir = 1;
    m->root->expanded = 1;
    return m;
}

static void node_free(le_node *n) {
    for (size_t i = 0; i < n->nchildren; i++)
        node_free(n->children[i]);
    free(n->children);
    free(n->name);
    free(n->link);
    for (int i = 0; i < LE_MAX_ARCHIVES; i++)
        free(n->arch[i].host_path);
    free(n);
}

void model_free(le_model *m) {
    if (!m)
        return;
    node_free(m->root);
    free(m);
}

/* ---- insertion ---- */

/* Create every node along path, returning the leaf. Interior components become
 * directories. When mark_present, each component is recorded as belonging to
 * arch_idx (staged additions do not exist in the archive yet, so they don't). */
static le_node *path_create(le_model *m, const char *path, int arch_idx,
                            int mark_present) {
    le_node *cur = m->root;
    const char *p = normalize_path(path);
    if (is_root_path(p))
        return NULL;                     /* archive root, not an editable member */

    char comp[PATH_MAX];
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t clen = slash ? (size_t)(slash - p) : strlen(p);
        if (clen == 0) {                 /* collapse empty components */
            p = slash + 1;
            continue;
        }
        if (clen >= sizeof(comp))
            clen = sizeof(comp) - 1;
        memcpy(comp, p, clen);
        comp[clen] = '\0';

        int created = 0;
        cur = child_find_or_add(cur, comp, &created);
        if (!cur)
            return NULL;
        if (created)
            m->total++;
        if (mark_present)
            cur->arch[arch_idx].present = 1;
        if (slash && slash[1] != '\0') {  /* interior component is a directory */
            if (cur->meta_set && !cur->is_dir)
                cur->mismatch = 1;       /* another archive has a file here */
            cur->is_dir = 1;
        }
        if (!slash)
            break;
        p = slash + 1;
    }

    return cur == m->root ? NULL : cur;  /* empty/degenerate path */
}

/* Adopt display metadata from an explicit archive entry, or flag a conflict
 * when another archive already described this path differently. */
static void apply_metadata(le_node *n, int is_dir, int is_symlink, off_t size,
                           mode_t mode, const char *link) {
    if (n->meta_set) {
        if (!!is_dir != !!n->is_dir || !!is_symlink != !!n->is_symlink ||
            (!is_dir && size != n->size))
            n->mismatch = 1;
        return;
    }
    if (!is_dir && n->is_dir)
        n->mismatch = 1;                 /* inferred as a dir elsewhere */
    if (is_dir)
        n->is_dir = 1;
    n->is_symlink = is_symlink;
    n->size = size;
    n->mode = mode;
    if (link) {
        free(n->link);
        n->link = strdup(link);
    }
    n->meta_set = 1;
}

le_node *model_insert(le_model *m, const char *path, int arch_idx, int is_dir,
                      int is_symlink, off_t size, mode_t mode,
                      const char *link) {
    if (!valid_arch(m, arch_idx))
        return NULL;
    le_node *n = path_create(m, path, arch_idx, 1);
    if (!n)
        return NULL;
    apply_metadata(n, is_dir, is_symlink, size, mode, link);
    return n;
}

le_node *model_add_host(le_model *m, const char *dest, int arch_idx,
                        const char *host_path, int is_symlink, off_t size,
                        mode_t mode, const char *link) {
    if (!valid_arch(m, arch_idx))
        return NULL;
    le_node *n = path_create(m, dest, arch_idx, 0);
    if (!n)
        return NULL;

    /* An explicit add describes the file better than an inherited entry. */
    n->meta_set = 0;
    apply_metadata(n, 0, is_symlink, size, mode, link);

    if (!n->arch[arch_idx].added) {
        m->arch_added[arch_idx]++;
        m->added++;
    }
    n->arch[arch_idx].added = 1;
    free(n->arch[arch_idx].host_path);
    n->arch[arch_idx].host_path = strdup(host_path);
    m->arch_dirty[arch_idx] = 1;
    m->dirty = 1;
    for (le_node *a = n->parent; a && a->parent; a = a->parent)
        a->expanded = 1;                 /* reveal the addition */
    return n;
}

/* ---- lookup / removal ---- */

le_node *model_lookup(le_model *m, const char *path) {
    le_node *cur = m->root;
    const char *p = normalize_path(path);
    if (is_root_path(p))
        return NULL;
    char comp[PATH_MAX];
    while (*p && cur) {
        const char *slash = strchr(p, '/');
        size_t clen = slash ? (size_t)(slash - p) : strlen(p);
        if (clen == 0) { p = slash + 1; continue; }
        if (clen >= sizeof(comp))
            clen = sizeof(comp) - 1;
        memcpy(comp, p, clen);
        comp[clen] = '\0';
        int found;
        size_t idx = child_lb(cur, comp, &found);
        if (!found)
            return NULL;
        cur = cur->children[idx];
        if (!slash)
            break;
        p = slash + 1;
    }
    return cur == m->root ? NULL : cur;
}

int node_in_archive(const le_node *n, int arch_idx) {
    if (!n || arch_idx < 0 || arch_idx >= LE_MAX_ARCHIVES)
        return 0;
    return n->arch[arch_idx].present || n->arch[arch_idx].added;
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

int node_effectively_removed(const le_node *n, int arch_idx) {
    if (arch_idx < 0 || arch_idx >= LE_MAX_ARCHIVES)
        return 0;
    for (const le_node *c = n; c; c = c->parent)
        if (c->arch[arch_idx].removed)
            return 1;
    return 0;
}

static void set_removed(le_model *m, le_node *n, int arch_idx, int removed) {
    if (!!n->arch[arch_idx].removed == !!removed)
        return;
    n->arch[arch_idx].removed = removed ? 1 : 0;
    if (removed) {
        m->arch_marked[arch_idx]++;
        m->marked++;
    } else {
        if (m->arch_marked[arch_idx])
            m->arch_marked[arch_idx]--;
        if (m->marked)
            m->marked--;
    }
    m->arch_dirty[arch_idx] = 1;
    m->dirty = 1;
}

void model_toggle_removed(le_model *m, le_node *n, int arch_idx) {
    if (!n || n == m->root || !valid_arch(m, arch_idx))
        return;
    if (!node_in_archive(n, arch_idx))
        return;
    set_removed(m, n, arch_idx, !n->arch[arch_idx].removed);
}

void model_toggle_removed_all(le_model *m, le_node *n) {
    if (!n || n == m->root)
        return;
    int slots[LE_MAX_ARCHIVES];
    int count = node_archives(n, m->narchives, slots);
    int any_removed = 0;
    for (int i = 0; i < count; i++)
        if (n->arch[slots[i]].removed)
            any_removed = 1;
    for (int i = 0; i < count; i++)
        set_removed(m, n, slots[i], !any_removed);
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

/* Detach child at index and free its subtree. */
static void child_remove_at(le_node *parent, size_t idx) {
    le_node *victim = parent->children[idx];
    memmove(&parent->children[idx], &parent->children[idx + 1],
            (parent->nchildren - idx - 1) * sizeof(*parent->children));
    parent->nchildren--;
    node_free(victim);
}

/* Forget everything one archive knew about this subtree. */
static void clear_arch_subtree(le_node *n, int arch_idx) {
    n->arch[arch_idx].present = 0;
    n->arch[arch_idx].removed = 0;
    n->arch[arch_idx].added = 0;
    free(n->arch[arch_idx].host_path);
    n->arch[arch_idx].host_path = NULL;
    for (size_t i = 0; i < n->nchildren; i++)
        clear_arch_subtree(n->children[i], arch_idx);
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

static void reconcile_node(le_node *n, int arch_idx, int narchives) {
    /* Walk children first so pruning is bottom-up safe. */
    for (size_t i = 0; i < n->nchildren; ) {
        le_node *c = n->children[i];
        if (c->arch[arch_idx].removed) {
            clear_arch_subtree(c, arch_idx);
        } else {
            if (c->arch[arch_idx].added) {
                /* The staged file is now a member of that archive. */
                c->arch[arch_idx].added = 0;
                free(c->arch[arch_idx].host_path);
                c->arch[arch_idx].host_path = NULL;
                c->arch[arch_idx].present = 1;
                for (le_node *a = c->parent; a && a->parent; a = a->parent)
                    a->arch[arch_idx].present = 1;
            }
            reconcile_node(c, arch_idx, narchives);
        }
        if (!subtree_in_use(c, narchives)) {
            child_remove_at(n, i);
            continue;
        }
        i++;
    }
}

static size_t count_members(const le_node *n) {
    size_t total = 0;
    for (size_t i = 0; i < n->nchildren; i++) {
        total++; /* every non-root node is a member (dir or leaf) */
        total += count_members(n->children[i]);
    }
    return total;
}

void model_reconcile_after_save(le_model *m, int arch_idx) {
    if (!m || !m->root || !valid_arch(m, arch_idx))
        return;
    reconcile_node(m->root, arch_idx, m->narchives);

    m->arch_marked[arch_idx] = 0;
    m->arch_added[arch_idx] = 0;
    m->arch_dirty[arch_idx] = 0;

    m->marked = 0;
    m->added = 0;
    m->dirty = 0;
    for (int i = 0; i < m->narchives; i++) {
        m->marked += m->arch_marked[i];
        m->added += m->arch_added[i];
        if (m->arch_dirty[i])
            m->dirty = 1;
    }
    m->total = count_members(m->root);
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
        if (c->is_dir && (c->expanded || filtering))
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
