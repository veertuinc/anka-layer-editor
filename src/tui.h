#pragma once

#include "session.h"
#include "tree.h"

/* Run the interactive editor over an already-loaded session and model.
 * Returns 0 on normal exit, non-zero on fatal UI error. */
int tui_run(const le_session *sess, le_model *m);
