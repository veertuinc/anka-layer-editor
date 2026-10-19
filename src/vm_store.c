#include "vm_store.h"
#include "anka_cli.h"
#include "archive.h"
#include "change_set.h"
#include "util.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHANGE_TAR_COMPRESSION 1

typedef struct {
    le_store     base;
    anka_vm_info info;
    long         uncommitted_change_count;
    char         label[600];
} vm_store;

static void update_label(vm_store *store) {
    snprintf(store->label, sizeof(store->label), "%s (tag %s)", store->info.vm,
             store->info.current_tag);
}

/* info for the VM, refused unless it is stopped and has a current tag. */
static int read_stopped_vm_info(const char *vm, anka_vm_info *info, char *err,
                                size_t errlen) {
    if (anka_cli_vm_info(vm, info, err, errlen) == -1)
        return -1;
    if (strcmp(info->status, "stopped") != 0) {
        le_seterr(err, errlen, "%s is %s; stop the VM first", vm, info->status);
        return -1;
    }
    if (!info->current_tag[0]) {
        le_seterr(err, errlen, "%s has no tag to edit", vm);
        return -1;
    }
    return 0;
}

static const char *vm_store_label(const le_store *base) {
    return ((const vm_store *)base)->label;
}

/* Load the current tag's layer from the export stream into slot arch_idx. */
static int load_layer(vm_store *store, le_model *m, int arch_idx, char *err,
                      size_t errlen) {
    proc_stream stream;
    if (anka_cli_export_start(&store->info, &stream, err, errlen) == -1)
        return -1;
    int stdout_fd = stream.stdout_fd;
    tar_t *in = tar_rdopen(stdout_fd);
    int load_rc = -1;
    if (in) {
        load_rc = archive_load_tar(in, store->label, m, arch_idx, err, errlen);
        tar_close(in);
    } else {
        le_seterr(err, errlen, "%s: cannot read the export", store->label);
        close(stdout_fd);
    }
    char export_err[512];
    if (anka_cli_export_finish(&stream, export_err, sizeof(export_err)) == -1) {
        le_seterr(err, errlen, "%s", export_err);
        return -1;
    }
    return load_rc;
}

static int vm_store_load(le_store *base, le_model *m, int arch_idx, char *err,
                         size_t errlen) {
    vm_store *store = (vm_store *)base;
    if (load_layer(store, m, arch_idx, err, errlen) == -1)
        return -1;
    return anka_cli_dirty_count(&store->info, &store->uncommitted_change_count,
                                err, errlen);
}

static int vm_store_may_remove(const le_store *base, const le_node *n,
                               int arch_idx, char *err, size_t errlen) {
    if (change_set_may_remove(n, arch_idx))
        return 0;
    le_seterr(err, errlen,
              "This entry has a base version; use w to delete it from the image");
    return -1;
}

static int vm_store_pre_save_question(const le_store *base, char *question,
                                      size_t len) {
    const vm_store *store = (const vm_store *)base;
    if (store->uncommitted_change_count == 0)
        return 0;
    snprintf(question, len,
             "The VM disk has %ld changes that are not in a tag. Put them in "
             "the new tag too?", store->uncommitted_change_count);
    return 1;
}

static int vm_store_save_name_prompt(const le_store *base, char *prompt,
                                     size_t prompt_len, char *default_name,
                                     size_t default_len) {
    const vm_store *store = (const vm_store *)base;
    snprintf(prompt, prompt_len, " New tag name: ");
    snprintf(default_name, default_len, "%s-edit", store->info.current_tag);
    return 1;
}

static int vm_store_check_save_name(const le_store *base, const char *name,
                                    char *err, size_t errlen) {
    const vm_store *store = (const vm_store *)base;
    if (!name[0]) {
        le_seterr(err, errlen, "the tag name is empty");
        return -1;
    }
    int exists;
    if (anka_cli_tag_exists(&store->info, name, &exists, err, errlen) == -1)
        return -1;
    if (exists) {
        le_seterr(err, errlen, "tag '%s' already exists", name);
        return -1;
    }
    return 0;
}

/* An unlinked temp file holding the change tar, at offset 0. -1 on error. */
static int write_change_tar(const le_model *m, int arch_idx, char *err,
                            size_t errlen) {
    const char *tmpdir = getenv("TMPDIR");
    char path[1024];
    snprintf(path, sizeof(path), "%s/anka-layer-editor-XXXXXX",
             tmpdir && tmpdir[0] ? tmpdir : "/tmp");
    int fd = mkstemp(path);
    if (fd == -1) {
        le_seterr(err, errlen, "%s: %s", path, strerror(errno));
        return -1;
    }
    unlink(path);
    int writer_fd = dup(fd);
    tar_t *out = writer_fd == -1 ? NULL
                                 : tar_wdopen(writer_fd, CHANGE_TAR_COMPRESSION);
    if (!out) {
        le_seterr(err, errlen, "change tar: %s", strerror(errno));
        if (writer_fd != -1)
            close(writer_fd);
        close(fd);
        return -1;
    }
    int rc = change_set_write(out, m, arch_idx, err, errlen);
    if (tar_close(out) == -1 && rc == 0) {
        le_seterr(err, errlen, "change tar: write error");
        rc = -1;
    }
    if (rc == 0 && lseek(fd, 0, SEEK_SET) == -1) {
        le_seterr(err, errlen, "change tar: %s", strerror(errno));
        rc = -1;
    }
    if (rc == -1) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Import the change tar and commit it as tag. A Ctrl-C here would leave the
 * disk half changed, so SIGINT is ignored until both are done. */
static int apply_and_commit(vm_store *store, int change_tar_fd,
                            const char *tag, char *err, size_t errlen) {
    struct sigaction ignore = {.sa_handler = SIG_IGN}, previous;
    sigaction(SIGINT, &ignore, &previous);
    char anka_err[512];
    int rc = 0;
    if (anka_cli_import(&store->info, change_tar_fd, anka_err,
                        sizeof(anka_err)) == -1) {
        le_seterr(err, errlen, "%s. The working disk can be partly changed",
                  anka_err);
        rc = -1;
    } else if (anka_cli_commit(&store->info, tag, anka_err,
                               sizeof(anka_err)) == -1) {
        le_seterr(err, errlen, "%s. Your changes are on the working disk. To "
                  "commit them, run: anka push -l -t %s %s", anka_err, tag,
                  store->info.vm);
        rc = -1;
    }
    sigaction(SIGINT, &previous, NULL);
    return rc;
}

static int vm_store_save(le_store *base, le_model *m, int arch_idx,
                         const char *save_name, char *err, size_t errlen) {
    vm_store *store = (vm_store *)base;
    if (!save_name) {
        le_seterr(err, errlen, "no tag name for the save");
        return -1;
    }
    anka_vm_info fresh_info;
    if (read_stopped_vm_info(store->info.vm, &fresh_info, err, errlen) == -1)
        return -1;
    if (strcmp(fresh_info.current_tag, store->info.current_tag) != 0) {
        le_seterr(err, errlen, "the current tag of %s is now '%s'; open it again",
                  store->info.vm, fresh_info.current_tag);
        return -1;
    }

    int change_tar_fd = write_change_tar(m, arch_idx, err, errlen);
    if (change_tar_fd == -1)
        return -1;
    int rc = apply_and_commit(store, change_tar_fd, save_name, err, errlen);
    close(change_tar_fd);
    if (rc == -1)
        return -1;

    strlcpy(store->info.current_tag, save_name, sizeof(store->info.current_tag));
    store->uncommitted_change_count = 0;
    update_label(store);
    model_clear_slot(m, arch_idx);
    char load_err[512];
    if (load_layer(store, m, arch_idx, load_err, sizeof(load_err)) == -1) {
        le_seterr(err, errlen, "saved tag '%s', but cannot load it: %s",
                  save_name, load_err);
        return -1;
    }
    return 0;
}

static void vm_store_free(le_store *base) {
    free(base);
}

static const le_store_ops vm_store_ops = {
    .label = vm_store_label,
    .load = vm_store_load,
    .save = vm_store_save,
    .may_remove = vm_store_may_remove,
    .pre_save_question = vm_store_pre_save_question,
    .save_name_prompt = vm_store_save_name_prompt,
    .check_save_name = vm_store_check_save_name,
    .free = vm_store_free,
};

le_store *vm_store_new(const char *vm, const char *expected_tag, char *err,
                       size_t errlen) {
    vm_store *store = calloc(1, sizeof(*store));
    if (!store) {
        le_seterr(err, errlen, "out of memory");
        return NULL;
    }
    store->base.ops = &vm_store_ops;
    if (read_stopped_vm_info(vm, &store->info, err, errlen) == -1) {
        free(store);
        return NULL;
    }
    if (expected_tag && strcmp(expected_tag, store->info.current_tag) != 0) {
        le_seterr(err, errlen, "%s: the current tag is '%s', not '%s'. VM mode "
                  "edits only the current tag", vm, store->info.current_tag,
                  expected_tag);
        free(store);
        return NULL;
    }
    update_label(store);
    return &store->base;
}
