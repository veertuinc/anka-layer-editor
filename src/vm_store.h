#pragma once

#include "store.h"

#include <stddef.h>

/*
 * A store for the top layer of a VM's current tag. Load reads the layer from
 * "anka export" and checks the working disk for changes that are not in a
 * tag. Save applies the edits to the stopped VM's working disk with
 * "anka import -v" and commits them as a new local tag, which is then loaded.
 */

/* NULL with err set when anka fails, the VM is not stopped, or expected_tag
 * (optional) is not the VM's current tag. */
le_store *vm_store_new(const char *vm, const char *expected_tag, char *err,
                       size_t errlen);
