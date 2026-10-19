#include "session.h"
#include "tree.h"
#include "tui.h"
#include "vm_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s <archive1.tar.gz> [archive2.tar.gz archive3.tar.gz ...]\n"
            "       %s --vm <name> [--tag <tag>]\n"
            "\n"
            "Interactive editor for filesystem-layer tar.gz archives.\n"
            "Browse the hierarchy, mark paths for removal, add host files,\n"
            "and save. With several archives loaded (up to %d), each row shows\n"
            "which archives hold that path. Press '?' inside for keybindings.\n"
            "\n"
            "--vm edits the top layer of the stopped VM's current OCI tag (--tag\n"
            "must name that OCI tag). Save commits the edits as a new local OCI\n"
            "tag. About OCI tags:\n"
            "http://docs.veertu.com/anka/templates-and-registries/using-an-oci-registry/oci-tags/\n",
            prog, prog, LE_MAX_ARCHIVES);
}

typedef struct {
    const char  *vm;
    const char  *expected_tag;
    const char **archives;
    int          narchives;
} command_line;

/* 0, or -1 when the arguments do not fit either form. */
static int parse_command_line(int argc, char **argv, command_line *cmd) {
    memset(cmd, 0, sizeof(*cmd));
    cmd->archives = calloc((size_t)argc, sizeof(*cmd->archives));
    if (!cmd->archives)
        return -1;
    for (int i = 1; i < argc; i++) {
        int has_value = i + 1 < argc;
        if (!strcmp(argv[i], "--vm") && has_value)
            cmd->vm = argv[++i];
        else if (!strcmp(argv[i], "--tag") && has_value)
            cmd->expected_tag = argv[++i];
        else if (argv[i][0] == '-' && argv[i][1] == '-')
            return -1;
        else
            cmd->archives[cmd->narchives++] = argv[i];
    }
    if (cmd->vm)
        return cmd->narchives == 0 ? 0 : -1;
    return cmd->narchives > 0 && !cmd->expected_tag ? 0 : -1;
}

static int fill_session(le_session *sess, const command_line *cmd, char *err,
                        size_t errlen) {
    if (cmd->vm) {
        le_store *store = vm_store_new(cmd->vm, cmd->expected_tag, err, errlen);
        return store ? session_add_store(sess, store, cmd->vm, cmd->vm, err,
                                         errlen)
                     : -1;
    }
    for (int i = 0; i < cmd->narchives; i++)
        if (session_add(sess, cmd->archives[i], err, errlen) != 0)
            return -1;
    return 0;
}

static int load_all_stores(le_session *sess, le_model *m, char *err,
                           size_t errlen) {
    for (int i = 0; i < sess->count; i++) {
        le_store *store = session_store(sess, i);
        fprintf(stderr, "loading %s ...\n", store_label(store));
        if (store_load(store, m, i, err, errlen) != 0)
            return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        }
    command_line cmd;
    if (parse_command_line(argc, argv, &cmd) != 0) {
        usage(argv[0]);
        free(cmd.archives);
        return 2;
    }

    le_session sess;
    memset(&sess, 0, sizeof(sess));
    char err[1024] = "";
    int filled = fill_session(&sess, &cmd, err, sizeof(err));
    free(cmd.archives);
    if (filled != 0) {
        fprintf(stderr, "%s: %s\n", argv[0], err);
        session_free(&sess);
        return 2;
    }

    le_model *m = model_new(sess.count);
    if (!m) {
        fprintf(stderr, "%s: out of memory\n", argv[0]);
        session_free(&sess);
        return 1;
    }

    int rc;
    if (load_all_stores(&sess, m, err, sizeof(err)) != 0) {
        fprintf(stderr, "%s: %s\n", argv[0], err);
        rc = 1;
    } else {
        rc = tui_run(&sess, m);
    }
    model_free(m);
    session_free(&sess);
    return rc;
}
