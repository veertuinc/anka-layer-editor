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

/* A final path component with this prefix deletes the rest of the name from
 * the image the layer is applied to (Anka, OCI). */
#define LE_WHITEOUT_PREFIX ".wh."
#define LE_OPAQUE_WHITEOUT ".wh..wh..opq"

/* Why an Anka diff layer holds an entry (stored in the tar devmajor field). */
typedef enum {
    LE_CHANGE_UNTAGGED = 0,
    LE_CHANGE_ADDED    = 1,
    LE_CHANGE_MODIFIED = 2,
    LE_CHANGE_WHITEOUT = 4,
} le_change_kind;

typedef enum {
    LE_KIND_FILE,
    LE_KIND_DIR,
    LE_KIND_SYMLINK,
    LE_KIND_HARDLINK,
    LE_KIND_OTHER,
} le_entry_kind;

/* What one archive entry (or staged addition) looks like. API arguments only
 * borrow link; nodes keep their own copy. */
typedef struct {
    le_entry_kind kind;
    off_t         size;
    mode_t        mode;         /* full mode incl. type bits */
    uid_t         uid;
    gid_t         gid;
    char         *link;         /* symlink / hard link target, or NULL */
} le_meta;

typedef enum {
    LE_STAGED_HOST_FILE,        /* copy a host file or symlink at save */
    LE_STAGED_WHITEOUT,         /* write an empty whiteout entry at save */
} le_staged_source;

typedef struct {
    le_staged_source source;
    char            *host_path;     /* LE_STAGED_HOST_FILE only */
    le_meta          meta;          /* what the new entry will hold */
    le_change_kind   change_kind;   /* devmajor written for the new entry */
    int              marked_target; /* whiteout marked its target for removal;
                                     * unstaging unmarks it */
} le_staged;

/* State of one path within one archive. */
typedef struct {
    unsigned char present;      /* path exists in this archive */
    unsigned char removed;      /* explicitly marked for removal here */
    unsigned char change_kind;  /* le_change_kind of the archive's entry */
    le_staged    *staged;       /* staged addition, or NULL */
} le_arch_state;

typedef struct le_node {
    char             *name;        /* single path component ("" for root) */
    struct le_node   *parent;
    struct le_node  **children;    /* sorted by name */
    size_t            nchildren;
    size_t            cap;

    le_meta           meta;         /* from the first explicit entry seen */
    int               meta_set;     /* metadata adopted from an explicit entry */
    int               mismatch;     /* type/size differs between archives */

    le_arch_state     arch[LE_MAX_ARCHIVES];

    int               expanded;     /* UI: directory expansion state */
    int               filter_hit;   /* transient: subtree matches active filter */
    int               shared_warn;  /* transient: a non-directory under this
                                     * path (or this file) is in 2+ archives */
} le_node;

typedef struct {
    le_node      *root;
    int           narchives;                     /* archive slots in use (>= 1) */
    size_t        total;                         /* distinct paths across archives */
    size_t        marked;                        /* explicit removals, all archives */
    size_t        added;                         /* staged additions, all archives */
    size_t        arch_marked[LE_MAX_ARCHIVES];
    size_t        arch_added[LE_MAX_ARCHIVES];
    unsigned long generation;                    /* bumped on every change */
} le_model;

typedef struct {
    le_node *node;
    int      depth;
} le_row;

/* Result of the toggle-mark action, for user feedback. */
typedef enum {
    LE_MARK_NONE,
    LE_MARK_REMOVED,
    LE_MARK_UNMARKED,
    LE_MARK_UNSTAGED,
} le_mark_result;

le_model *model_new(int narchives);
void      model_free(le_model *m);

/* Insert a member of archive arch_idx; returns the leaf node (creating
 * inferred dirs). Display metadata comes from the first explicit entry seen;
 * a later archive whose entry disagrees sets the node's mismatch flag. */
le_node  *model_insert(le_model *m, const char *path, int arch_idx,
                       const le_meta *meta, le_change_kind change_kind);

le_node  *model_lookup(le_model *m, const char *path);

/* Check a guest path typed by a user. Leading slashes are allowed; empty
 * paths and "." or ".." components are not. 0, or -1 with err set. */
int       model_check_guest_path(const char *path, char *err, size_t errlen);

/* Stage a host file or symlink at dest in archive arch_idx. Rejects a dest
 * that is a directory, sits under a non-directory or a path marked for
 * removal, or is a whiteout name. Returns the node, or NULL with err set. */
le_node  *model_stage_host_file(le_model *m, const char *dest, int arch_idx,
                                const char *host_path, const le_meta *meta,
                                char *err, size_t errlen);

/* Fill whiteout_path with "<parent>/.wh.<name>" for target_path. Returns 0,
 * or -1 with err set when target_path is already a whiteout or too long. */
int       model_whiteout_path(const char *target_path, char *whiteout_path,
                              size_t len, char *err, size_t errlen);

/* Stage a whiteout that deletes target_path from the image this layer is
 * applied to. A target held by this archive is marked for removal too, and
 * unstaging the whiteout clears that mark even if the user set it again in
 * the meantime. A staged addition at the target is undone and is not restored
 * by unstaging the whiteout. Returns the whiteout node, or NULL with err set. */
le_node  *model_stage_whiteout(le_model *m, const char *target_path,
                               int arch_idx, int target_is_dir,
                               char *err, size_t errlen);

/* Owner for a new entry at dest: the nearest explicit entry at or above dest,
 * else 0:0. */
void      model_default_owner(le_model *m, const char *dest,
                              uid_t *uid, gid_t *gid);

/* True when the path belongs to that archive, as a member or a staged add. */
int       node_in_archive(const le_node *n, int arch_idx);

/* Fill out (capacity LE_MAX_ARCHIVES) with the archives holding this path.
 * Returns how many were written. */
int       node_archives(const le_node *n, int narchives, int *out);

int       node_is_dir(const le_node *n);
int       node_is_whiteout(const le_node *n);
/* The name a whiteout node deletes ("" for the opaque marker). */
const char *node_whiteout_target(const le_node *n);
/* Metadata to show: the first staged addition's, else the node's. */
const le_meta *node_display_meta(const le_node *n, int narchives);

int       node_effectively_removed(const le_node *n, int arch_idx);
void      model_toggle_removed(le_model *m, le_node *n, int arch_idx);
/* Mark for removal in every archive holding the path; unmark all instead when
 * any of them is already marked. */
void      model_toggle_removed_all(le_model *m, le_node *n);

/* Undo a staged addition; the node and parents that only existed for it are
 * freed, so n must not be used afterwards. */
void      model_unstage(le_model *m, le_node *n, int arch_idx);

/* The Space action: undo a staged addition, otherwise toggle removal. May
 * free n (see model_unstage). */
le_mark_result model_toggle_mark(le_model *m, le_node *n, int arch_idx);
/* The Space action over every archive holding the path. May free n. */
le_mark_result model_toggle_mark_all(le_model *m, le_node *n);

int       model_arch_dirty(const le_model *m, int arch_idx);
int       model_dirty(const le_model *m);

/* Rebuild guest path into buf. Returns 0, or -1 if truncated. */
int       node_path(const le_node *n, char *buf, size_t len);

/* After a successful save of arch_idx: drop that archive's removed subtrees,
 * promote its staged additions to members, and reset its counters. Nodes still
 * held by another archive stay in the tree. */
void      model_reconcile_after_save(le_model *m, int arch_idx);

/* Forget everything slot arch_idx holds (members, marks, staged items) so it
 * can be loaded again. Paths that only that slot held are freed. */
void      model_clear_slot(le_model *m, int arch_idx);

/* Flatten visible rows (respecting expansion and optional substring filter).
 * The out and cap args form a reusable growable buffer owned by the caller.
 * Returns the number of rows written. */
size_t    model_flatten(le_model *m, const char *filter,
                        le_row **out, size_t *cap);
