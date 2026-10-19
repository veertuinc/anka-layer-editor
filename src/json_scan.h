#pragma once

#include <stddef.h>

/*
 * Just enough JSON reading for "anka -j" output: find a "key": "string" pair
 * by text search. It does not track nesting, so start the search at the part
 * of the document that holds the key (for example after "body").
 */

/* Copy the string value of the first "key" at or after json into value.
 * \" \\ and \/ are decoded; other escapes are kept as they are. Returns a
 * pointer just after the value (to continue the search), or NULL when no such
 * key with a string value is found. */
const char *json_scan_string(const char *json, const char *key, char *value,
                             size_t value_len);

/* The part of an "anka -j" reply after its "body" key, or NULL. */
const char *json_scan_body(const char *json);
