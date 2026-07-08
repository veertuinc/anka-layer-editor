#pragma once

#include "tree.h"

/* Run the interactive editor over an already-loaded model.
 * Returns 0 on normal exit, non-zero on fatal UI error. */
int tui_run(const char *archive_path, le_model *m);
