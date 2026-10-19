#pragma once

#include "tar.h"
#include "tree.h"

/*
 * Archive engine: load a .tar.gz into one archive slot of the model, and save
 * an edited slot back to a .tar.gz by streaming the original (skipping entries
 * removed or replaced in that slot) and appending that slot's staged
 * additions and whiteouts, then atomically replacing the original.
 */

/* Load archive at path into slot arch_idx of m. Returns 0 on success, -1 on
 * error (err, if non-NULL, receives a human-readable message). */
int archive_load(const char *path, le_model *m, int arch_idx,
                 char *err, size_t errlen);

/* Load every entry of an open reader into slot arch_idx; source_label names
 * the source in messages. The caller closes the reader. 0 / -1. */
int archive_load_tar(tar_t *in, const char *source_label, le_model *m,
                     int arch_idx, char *err, size_t errlen);

/* Save slot arch_idx of the edited model to path (atomic replace that keeps
 * the archive's mode and owner). 0 / -1. */
int archive_save(const char *path, le_model *m, int arch_idx,
                 char *err, size_t errlen);

/* Describe a host regular file or symlink for staging. meta->link points into
 * the caller's link buffer. 0, or -1 with err set. */
int archive_host_meta(const char *host_path, le_meta *meta, char *link,
                      size_t linklen, char *err, size_t errlen);
