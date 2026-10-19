#pragma once

#include "proc.h"

#include <limits.h>
#include <stddef.h>

/*
 * The anka and anka_image commands VM mode needs, for one VM. Only the public
 * "anka" CLI is used, except the dirty check, which runs "anka_image diff",
 * because no anka command compares the working disk with its tag.
 */

typedef struct {
    char vm[256];
    char uuid[64];
    char status[32];                   /* "stopped", "running", ... */
    char current_tag[256];             /* empty when the VM has no tag */
    char working_disk_file[PATH_MAX];  /* in img_lib_dir */
    char tag_image_file[PATH_MAX];     /* in img_lib_dir; the current tag */
    char img_lib_dir[PATH_MAX];
    char anka_image_tool[PATH_MAX];
} anka_vm_info;

/* Fill info from "anka -j show", "show disk", and "config". 0, or -1 with
 * err set. */
int anka_cli_vm_info(const char *vm, anka_vm_info *info, char *err,
                     size_t errlen);

/* Set *exists to 1 when the VM has a local tag with this name. */
int anka_cli_tag_exists(const anka_vm_info *info, const char *tag, int *exists,
                        char *err, size_t errlen);

/* Start "anka export" of the current tag's layer as a tar on stream's stdout. */
int anka_cli_export_start(const anka_vm_info *info, proc_stream *stream,
                          char *err, size_t errlen);
/* Wait for the export. 0, or -1 with err set. */
int anka_cli_export_finish(proc_stream *stream, char *err, size_t errlen);

/* Number of paths that differ between the working disk and the current tag. */
int anka_cli_dirty_count(const anka_vm_info *info, long *count, char *err,
                         size_t errlen);

/* Apply the layer tar read from layer_fd onto the stopped VM's working disk. */
int anka_cli_import(const anka_vm_info *info, int layer_fd, char *err,
                    size_t errlen);

/* Commit the working disk as a new local tag. */
int anka_cli_commit(const anka_vm_info *info, const char *tag, char *err,
                    size_t errlen);
