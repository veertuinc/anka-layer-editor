#include "anka_cli.h"
#include "json_scan.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ANKA_COMMAND "anka"

static void command_text(const char *const argv[], char *text, size_t len) {
    size_t used = 0;
    text[0] = '\0';
    for (int i = 0; argv[i] && used < len; i++)
        used += (size_t)snprintf(text + used, len - used, "%s%s", i ? " " : "",
                                 argv[i]);
}

static int last_line(const char *text, char *line, size_t len) {
    const char *end = text + strlen(text);
    while (end > text && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' '))
        end--;
    const char *start = end;
    while (start > text && start[-1] != '\n')
        start--;
    if (start == end)
        return 0;
    snprintf(line, len, "%.*s", (int)(end - start), start);
    return 1;
}

/* anka's JSON "message", else the last line of stderr, else the exit status. */
static void set_command_error(const char *command, int status,
                              const char *stdout_text, const char *stderr_text,
                              char *err, size_t errlen) {
    char message[512];
    if (!json_scan_string(stdout_text, "message", message, sizeof(message)) &&
        !(stderr_text && last_line(stderr_text, message, sizeof(message))))
        snprintf(message, sizeof(message), "exit status %d", status);
    le_seterr(err, errlen, "%s: %s", command, message);
}

/* Run to the end and keep the output only on success. 0, or -1 with err set. */
static int run_command(const proc_spec *spec, char **stdout_text,
                       char **stderr_text, char *err, size_t errlen) {
    int status = proc_run(spec, stdout_text, stderr_text, err, errlen);
    if (status == 0)
        return 0;
    if (status > 0) {
        char command[1024];
        command_text(spec->argv, command, sizeof(command));
        set_command_error(command, status, *stdout_text, *stderr_text, err,
                          errlen);
    }
    free(*stdout_text);
    free(*stderr_text);
    *stdout_text = *stderr_text = NULL;
    return -1;
}

static int run_anka(const char *const argv[], int stdin_fd, char **reply,
                    char *err, size_t errlen) {
    proc_spec spec = {.argv = argv, .stdin_fd = stdin_fd};
    char *stderr_text;
    if (run_command(&spec, reply, &stderr_text, err, errlen) == -1)
        return -1;
    free(stderr_text);
    return 0;
}

static int run_anka_discard_reply(const char *const argv[], int stdin_fd,
                                  char *err, size_t errlen) {
    char *reply;
    if (run_anka(argv, stdin_fd, &reply, err, errlen) == -1)
        return -1;
    free(reply);
    return 0;
}

typedef struct {
    const char *key;
    char       *value;
    size_t      value_len;
    int         required;
} reply_field;

static int read_reply_fields(const char *const argv[], reply_field *fields,
                             size_t nfields, char *err, size_t errlen) {
    char *reply;
    if (run_anka(argv, -1, &reply, err, errlen) == -1)
        return -1;
    const char *body = json_scan_body(reply);
    int rc = 0;
    for (size_t i = 0; i < nfields && rc == 0; i++) {
        if (json_scan_string(body, fields[i].key, fields[i].value,
                             fields[i].value_len) || !fields[i].required)
            continue;
        char command[1024];
        command_text(argv, command, sizeof(command));
        le_seterr(err, errlen, "%s: the reply has no \"%s\"", command,
                  fields[i].key);
        rc = -1;
    }
    free(reply);
    return rc;
}

#define FIELD(key, buffer, required) {key, buffer, sizeof(buffer), required}

int anka_cli_vm_info(const char *vm, anka_vm_info *info, char *err,
                     size_t errlen) {
    memset(info, 0, sizeof(*info));
    strlcpy(info->vm, vm, sizeof(info->vm));

    const char *show_argv[] = {ANKA_COMMAND, "-j", "show", vm, NULL};
    reply_field show_fields[] = {
        FIELD("uuid", info->uuid, 1),
        FIELD("status", info->status, 1),
        FIELD("version", info->current_tag, 0),
    };
    const char *disk_argv[] = {ANKA_COMMAND, "-j", "show", vm, "disk", NULL};
    reply_field disk_fields[] = {
        FIELD("file", info->working_disk_file, 1),
        FIELD("base", info->tag_image_file, 0),
    };
    const char *config_argv[] = {ANKA_COMMAND, "-j", "config", NULL};
    reply_field config_fields[] = {
        FIELD("img_lib_dir", info->img_lib_dir, 1),
        FIELD("anka_image_maker_executable", info->anka_image_tool, 1),
    };
    if (read_reply_fields(show_argv, show_fields, LE_COUNT_OF(show_fields),
                          err, errlen) == -1 ||
        read_reply_fields(disk_argv, disk_fields, LE_COUNT_OF(disk_fields),
                          err, errlen) == -1 ||
        read_reply_fields(config_argv, config_fields,
                          LE_COUNT_OF(config_fields), err, errlen) == -1)
        return -1;
    return 0;
}

int anka_cli_tag_exists(const anka_vm_info *info, const char *tag, int *exists,
                        char *err, size_t errlen) {
    const char *argv[] = {ANKA_COMMAND, "-j", "show", info->vm, "tag", NULL};
    char *reply;
    if (run_anka(argv, -1, &reply, err, errlen) == -1)
        return -1;
    *exists = 0;
    char listed_tag[256];
    for (const char *p = json_scan_body(reply);
         (p = json_scan_string(p, "tag", listed_tag, sizeof(listed_tag)));)
        if (!strcmp(listed_tag, tag)) {
            *exists = 1;
            break;
        }
    free(reply);
    return 0;
}

int anka_cli_export_start(const anka_vm_info *info, proc_stream *stream,
                          char *err, size_t errlen) {
    const char *argv[] = {ANKA_COMMAND, "export", "-t", info->current_tag,
                          info->vm, NULL};
    proc_spec spec = {.argv = argv, .env_assign = "ANKA_PUSH_MODE=2",
                      .stdin_fd = -1};
    return proc_stream_start(&spec, stream, err, errlen);
}

int anka_cli_export_finish(proc_stream *stream, char *err, size_t errlen) {
    char *stderr_text;
    int status = proc_stream_finish(stream, &stderr_text, err, errlen);
    if (status > 0)
        set_command_error(ANKA_COMMAND " export", status, NULL, stderr_text,
                          err, errlen);
    free(stderr_text);
    return status == 0 ? 0 : -1;
}

/* Sum the CHANGED, ADDED, and DELETED totals that "anka_image diff -l" prints
 * last. Per-path log lines use a space after the colon, totals a tab. */
static int parse_diff_totals(const char *diff_output, long *count) {
    static const char *const total_labels[] = {"CHANGED:\t", "ADDED:\t",
                                               "DELETED:\t"};
    int totals_found = 0;
    *count = 0;
    for (const char *line = diff_output; line && *line;) {
        for (size_t i = 0; i < LE_COUNT_OF(total_labels); i++) {
            size_t label_len = strlen(total_labels[i]);
            if (!strncmp(line, total_labels[i], label_len)) {
                *count += strtol(line + label_len, NULL, 10);
                totals_found++;
            }
        }
        line = strchr(line, '\n');
        if (line)
            line++;
    }
    return totals_found == 3 ? 0 : -1;
}

int anka_cli_dirty_count(const anka_vm_info *info, long *count, char *err,
                         size_t errlen) {
    if (!info->tag_image_file[0]) {
        le_seterr(err, errlen, "%s: the working disk has no base image",
                  info->vm);
        return -1;
    }
    const char *argv[] = {info->anka_image_tool, "diff", "-ln",
                          info->tag_image_file, info->working_disk_file, NULL};
    proc_spec spec = {.argv = argv, .cwd = info->img_lib_dir, .stdin_fd = -1};
    char *stdout_text, *stderr_text;
    if (run_command(&spec, &stdout_text, &stderr_text, err, errlen) == -1)
        return -1;
    int rc = parse_diff_totals(stderr_text, count);
    if (rc == -1)
        le_seterr(err, errlen, "anka_image diff: no totals in its output");
    free(stdout_text);
    free(stderr_text);
    return rc;
}

int anka_cli_import(const anka_vm_info *info, int layer_fd, char *err,
                    size_t errlen) {
    const char *argv[] = {ANKA_COMMAND, "-j", "import", "-v", info->vm, NULL};
    return run_anka_discard_reply(argv, layer_fd, err, errlen);
}

int anka_cli_commit(const anka_vm_info *info, const char *tag, char *err,
                    size_t errlen) {
    const char *argv[] = {ANKA_COMMAND, "-j", "push", "-l", "-t", tag,
                          info->vm, NULL};
    return run_anka_discard_reply(argv, -1, err, errlen);
}
