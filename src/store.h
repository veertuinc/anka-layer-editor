#pragma once

#include "tree.h"

#include <stddef.h>

/*
 * Where the edits of one session slot come from and go to. The TUI and main
 * talk only to this interface. Operations marked optional may be NULL.
 */

typedef struct le_store le_store;

typedef struct {
    /* Text for the header and messages. */
    const char *(*label)(const le_store *store);
    /* Load the store's entries into slot arch_idx. 0, or -1 with err set. */
    int  (*load)(le_store *store, le_model *m, int arch_idx,
                 char *err, size_t errlen);
    /* Write the slot's edits. save_name is the answer to save_name_prompt, or
     * NULL. 0, or -1 with err set. */
    int  (*save)(le_store *store, le_model *m, int arch_idx,
                 const char *save_name, char *err, size_t errlen);
    /* Optional. 0 when Space may mark n for removal, else -1 with err set. */
    int  (*may_remove)(const le_store *store, const le_node *n, int arch_idx,
                       char *err, size_t errlen);
    /* Optional. Fill question with a yes/no question to ask before save.
     * Returns 1 when there is one. */
    int  (*pre_save_question)(const le_store *store, char *question, size_t len);
    /* Optional. Fill prompt and default_name when save needs a name. Returns 1
     * when it does. */
    int  (*save_name_prompt)(const le_store *store, char *prompt,
                             size_t prompt_len, char *default_name,
                             size_t default_len);
    /* Optional. 0 when name can be used for save, else -1 with err set. */
    int  (*check_save_name)(const le_store *store, const char *name,
                            char *err, size_t errlen);
    void (*free)(le_store *store);
} le_store_ops;

struct le_store {
    const le_store_ops *ops;
};

static inline const char *store_label(const le_store *store) {
    return store->ops->label(store);
}

static inline int store_load(le_store *store, le_model *m, int arch_idx,
                             char *err, size_t errlen) {
    return store->ops->load(store, m, arch_idx, err, errlen);
}

static inline int store_save(le_store *store, le_model *m, int arch_idx,
                             const char *save_name, char *err, size_t errlen) {
    return store->ops->save(store, m, arch_idx, save_name, err, errlen);
}

static inline int store_may_remove(const le_store *store, const le_node *n,
                                   int arch_idx, char *err, size_t errlen) {
    return store->ops->may_remove
               ? store->ops->may_remove(store, n, arch_idx, err, errlen)
               : 0;
}

static inline int store_pre_save_question(const le_store *store, char *question,
                                          size_t len) {
    return store->ops->pre_save_question &&
           store->ops->pre_save_question(store, question, len);
}

static inline int store_save_name_prompt(const le_store *store, char *prompt,
                                         size_t prompt_len, char *default_name,
                                         size_t default_len) {
    return store->ops->save_name_prompt &&
           store->ops->save_name_prompt(store, prompt, prompt_len,
                                        default_name, default_len);
}

static inline int store_check_save_name(const le_store *store, const char *name,
                                        char *err, size_t errlen) {
    return store->ops->check_save_name
               ? store->ops->check_save_name(store, name, err, errlen)
               : 0;
}

static inline void store_free(le_store *store) {
    if (store)
        store->ops->free(store);
}
