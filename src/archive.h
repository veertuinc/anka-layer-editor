#pragma once

#include "tree.h"

/*
 * Archive engine: load a .tar.gz into one archive slot of the model, and save
 * an edited slot back to a .tar.gz by streaming the original (skipping entries
 * removed or replaced in that slot) and appending that slot's staged
 * additions, then atomically replacing the original.
 */

/* Load archive at path into slot arch_idx of m. Returns 0 on success, -1 on
 * error (err, if non-NULL, receives a human-readable message). */
int archive_load(const char *path, le_model *m, int arch_idx,
                 char *err, size_t errlen);

/* Save slot arch_idx of the edited model to path (atomic replace). 0 / -1. */
int archive_save(const char *path, le_model *m, int arch_idx,
                 char *err, size_t errlen);
