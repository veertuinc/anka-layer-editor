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

le_model *model_new(void) {
    le_model *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->root = node_new("", NULL);
    if (!m->root) {
        free(m);
        return NULL;
    }
    m->root->is_dir = 1;
    m->root->expanded = 1;
    return m;
}

static void node_free(le_node *n) {
    for (size_t i = 0; i < n->nchildren; i++)
        node_free(n->children[i]);
    free(n->children);
    free(n->name);
    free(n->host_path);
    free(n->link);
    free(n);
}

void model_free(le_model *m) {
    if (!m)
        return;
    node_free(m->root);
    free(m);
}

/* ---- insertion ---- */

le_node *model_insert(le_model *m, const char *path, int is_dir,
                      int is_symlink, off_t size, mode_t mode,
                      const char *link) {
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

        int final = !slash || slash[1] == '\0';
        int created = 0;
        cur = child_find_or_add(cur, comp, &created);
        if (!cur)
            return NULL;
        if (created)
            m->total++;
        if (!final)
            cur->is_dir = 1;             /* interior component is a directory */
        if (!slash)
            break;
        p = slash + 1;
    }

    if (cur == m->root)                  /* empty/degenerate path */
        return NULL;

    if (is_dir)
        cur->is_dir = 1;
    cur->is_symlink = is_symlink;
    cur->size = size;
    cur->mode = mode;
    if (link) {
        free(cur->link);
        cur->link = strdup(link);
    }
    return cur;
}

le_node *model_add_host(le_model *m, const char *dest, const char *host_path,
                        int is_symlink, off_t size, mode_t mode,
                        const char *link) {
    le_node *n = model_insert(m, dest, 0, is_symlink, size, mode, link);
    if (!n)
        return NULL;
    if (!n->added)
        m->added++;
    n->added = 1;
    free(n->host_path);
    n->host_path = strdup(host_path);
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

int node_effectively_removed(const le_node *n) {
    for (const le_node *c = n; c; c = c->parent)
        if (c->removed)
            return 1;
    return 0;
}

void model_toggle_removed(le_model *m, le_node *n) {
    if (n == m->root)
        return;
    n->removed = !n->removed;
    if (n->removed)
        m->marked++;
    else if (m->marked)
        m->marked--;
    m->dirty = 1;
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

static void reconcile_node(le_node *n) {
    /* Walk children first so removals are bottom-up safe. */
    for (size_t i = 0; i < n->nchildren; ) {
        le_node *c = n->children[i];
        if (c->removed) {
            child_remove_at(n, i);
            continue;
        }
        reconcile_node(c);
        i++;
    }
    if (n->added) {
        n->added = 0;
        free(n->host_path);
        n->host_path = NULL;
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

void model_reconcile_after_save(le_model *m) {
    if (!m || !m->root)
        return;
    reconcile_node(m->root);
    m->marked = 0;
    m->added = 0;
    m->total = count_members(m->root);
    m->dirty = 0;
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
