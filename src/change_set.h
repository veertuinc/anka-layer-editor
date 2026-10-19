#pragma once

#include "tar.h"
#include "tree.h"

#include <stddef.h>

/*
 * The edits of one slot as a layer to apply on top of the layer it loaded
 * (VM mode). A marked path becomes a whiteout, which deletes it. A whiteout
 * cannot bring back a base version, so only paths the loaded layer added may
 * be marked.
 */

/* 1 when n is in slot arch_idx and n and every member below it were added
 * by the loaded layer: kind 1, or a directory of kind 0. */
int change_set_may_remove(const le_node *n, int arch_idx);

/* Write a whiteout for each path in a marked subtree, deepest first so that
 * directories are empty when they are deleted, then the staged items in tree
 * order. Returns 0, or -1 with err set, also when a marked subtree holds a
 * member that change_set_may_remove refuses. */
int change_set_write(tar_t *out, const le_model *m, int arch_idx,
                     char *err, size_t errlen);
