#pragma once

#include "tar.h"
#include "tree.h"

/*
 * Write staged edits into a tar writer: host files and symlinks (read from
 * the host now) and whiteout entries.
 */

/* Write every staged addition and whiteout of slot arch_idx under root, in
 * tree order, skipping subtrees removed in that slot. 0, or -1 with err set. */
int staged_write_all(tar_t *out, const le_node *root, int arch_idx,
                     char *err, size_t errlen);

/* Write one empty whiteout entry at whiteout_path (".wh." name included),
 * typed as a directory when is_dir, with change kind LE_CHANGE_WHITEOUT. */
int staged_write_whiteout_entry(tar_t *out, const char *whiteout_path,
                                int is_dir, uid_t uid, gid_t gid,
                                char *err, size_t errlen);
