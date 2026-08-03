#pragma once

#include <sys/types.h>
#include <stddef.h>

/*
 * In-memory path tree for the contents of one or more archives.
 *
 * Directories are inferred from entry paths. A path that exists in several
 * archives is a single node carrying per-archive state (membership,
 * marked-for-removal, staged addition) plus shared display metadata and UI
 * state. The root is a nameless sentinel; its children render at depth 0.
 */

/* Archives editable in one session. Per-node state is a fixed-size array, so
 * this bound keeps nodes small. */
#define LE_MAX_ARCHIVES 8

/* State of one path within one archive. */
typedef struct {
    unsigned char present;      /* path exists in this archive */
    unsigned char removed;      /* explicitly marked for removal here */
    unsigned char added;        /* staged addition sourced from the host */
    char         *host_path;    /* source path on host (added only) */
} le_arch_state;

typedef struct le_node {
    char             *name;        /* single path component ("" for root) */
    struct le_node   *parent;
    struct le_node  **children;    /* sorted by name */
    size_t            nchildren;
    size_t            cap;

    int               is_dir;
    int               is_symlink;
    off_t             size;
    mode_t            mode;         /* full mode incl. type bits */
    char             *link;         /* symlink target */
    int               meta_set;     /* metadata adopted from an explicit entry */
    int               mismatch;     /* type/size differs between archives */

    le_arch_state     arch[LE_MAX_ARCHIVES];

    int               expanded;     /* UI: directory expansion state */
    int               filter_hit;   /* transient: subtree matches active filter */
} le_node;

typedef struct {
    le_node *root;
    int      narchives;                     /* archive slots in use (>= 1) */
    size_t   total;                         /* distinct paths across archives */
    size_t   marked;                        /* explicit removals, all archives */
    size_t   added;                         /* staged additions, all archives */
    int      dirty;                         /* any archive has unsaved changes */
    size_t   arch_marked[LE_MAX_ARCHIVES];
    size_t   arch_added[LE_MAX_ARCHIVES];
    int      arch_dirty[LE_MAX_ARCHIVES];   /* per-archive unsaved changes */
} le_model;

typedef struct {
    le_node *node;
    int      depth;
} le_row;

le_model *model_new(int narchives);
void      model_free(le_model *m);

/* Insert a member of archive arch_idx; returns the leaf node (creating
 * inferred dirs). Display metadata comes from the first explicit entry seen;
 * a later archive whose entry disagrees sets the node's mismatch flag. */
le_node  *model_insert(le_model *m, const char *path, int arch_idx, int is_dir,
                       int is_symlink, off_t size, mode_t mode,
                       const char *link);

/* Stage a host file at a destination path inside archive arch_idx. */
le_node  *model_add_host(le_model *m, const char *dest, int arch_idx,
                         const char *host_path, int is_symlink, off_t size,
                         mode_t mode, const char *link);

le_node  *model_lookup(le_model *m, const char *path);

/* True when the path belongs to that archive, as a member or a staged add. */
int       node_in_archive(const le_node *n, int arch_idx);

/* Fill out (capacity LE_MAX_ARCHIVES) with the archives holding this path.
 * Returns how many were written. */
int       node_archives(const le_node *n, int narchives, int *out);

int       node_effectively_removed(const le_node *n, int arch_idx);
void      model_toggle_removed(le_model *m, le_node *n, int arch_idx);
/* Mark for removal in every archive holding the path; unmark all instead when
 * any of them is already marked. */
void      model_toggle_removed_all(le_model *m, le_node *n);

/* Rebuild guest path into buf. Returns 0, or -1 if truncated. */
int       node_path(const le_node *n, char *buf, size_t len);

/* After a successful save of arch_idx: drop that archive's removed subtrees,
 * promote its staged additions to members, and reset its counters. Nodes still
 * held by another archive stay in the tree. */
void      model_reconcile_after_save(le_model *m, int arch_idx);

/* Flatten visible rows (respecting expansion and optional substring filter).
 * The out and cap args form a reusable growable buffer owned by the caller.
 * Returns the number of rows written. */
size_t    model_flatten(le_model *m, const char *filter,
                        le_row **out, size_t *cap);
