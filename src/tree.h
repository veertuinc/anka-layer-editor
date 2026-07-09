#pragma once

#include <sys/types.h>
#include <stddef.h>

/*
 * In-memory path tree for the archive contents.
 *
 * Directories are inferred from entry paths. Each node carries archive
 * metadata plus edit state (marked-for-removal, staged addition) and UI state
 * (expanded). The root is a nameless sentinel; its children render at depth 0.
 */

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

    int               removed;      /* explicitly marked for removal */
    int               added;        /* staged addition sourced from host */
    char             *host_path;    /* source path on host (added nodes) */
    char             *link;         /* symlink target */

    int               expanded;     /* UI: directory expansion state */
    int               filter_hit;   /* transient: subtree matches active filter */
} le_node;

typedef struct {
    le_node *root;
    size_t   total;    /* archive members + staged additions */
    size_t   marked;   /* nodes explicitly marked for removal */
    size_t   added;    /* staged additions */
    int      dirty;    /* unsaved changes present */
} le_model;

typedef struct {
    le_node *node;
    int      depth;
} le_row;

le_model *model_new(void);
void      model_free(le_model *m);

/* Insert an archive member; returns the leaf node (creating inferred dirs). */
le_node  *model_insert(le_model *m, const char *path, int is_dir,
                       int is_symlink, off_t size, mode_t mode,
                       const char *link);

/* Stage a host file at a destination path inside the archive. */
le_node  *model_add_host(le_model *m, const char *dest, const char *host_path,
                         int is_symlink, off_t size, mode_t mode,
                         const char *link);

le_node  *model_lookup(le_model *m, const char *path);
int       node_effectively_removed(const le_node *n);
void      model_toggle_removed(le_model *m, le_node *n);
/* Rebuild guest path into buf. Returns 0, or -1 if truncated. */
int       node_path(const le_node *n, char *buf, size_t len);

/* After a successful save: drop removed subtrees, clear staged-add flags,
 * and reset counters so the in-memory model matches the written archive. */
void      model_reconcile_after_save(le_model *m);

/* Flatten visible rows (respecting expansion and optional substring filter).
 * The out and cap args form a reusable growable buffer owned by the caller.
 * Returns the number of rows written. */
size_t    model_flatten(le_model *m, const char *filter,
                        le_row **out, size_t *cap);
