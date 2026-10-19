#pragma once

#include "store.h"

/* A store backed by a .tar.gz file: load reads it, save rewrites it in place.
 * Returns NULL when out of memory. */
le_store *archive_store_new(const char *path);
