#pragma once

#include "tree.h"

/*
 * Archive engine: load a .tar.gz into the model, and save an edited model back
 * to a .tar.gz by streaming the original (skipping removed entries) and
 * appending staged additions, then atomically replacing the original.
 */

/* Load archive at path into m. Returns 0 on success, -1 on error
 * (err, if non-NULL, receives a human-readable message). */
int archive_load(const char *path, le_model *m, char *err, size_t errlen);

/* Save the edited model to path (atomic replace). Returns 0 / -1. */
int archive_save(const char *path, le_model *m, char *err, size_t errlen);
