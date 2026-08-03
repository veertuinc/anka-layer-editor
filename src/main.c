#include "archive.h"
#include "session.h"
#include "tree.h"
#include "tui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s <archive1.tar.gz> [archive2.tar.gz archive3.tar.gz ...]\n"
            "\n"
            "Interactive editor for filesystem-layer tar.gz archives.\n"
            "Browse the hierarchy, mark paths for removal, add host files,\n"
            "and save. With several archives loaded (up to %d), each row shows\n"
            "which archives hold that path. Press '?' inside for keybindings.\n",
            prog, LE_MAX_ARCHIVES);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        }

    le_session sess;
    memset(&sess, 0, sizeof(sess));
    char err[512] = "";
    for (int i = 1; i < argc; i++)
        if (session_add(&sess, argv[i], err, sizeof(err)) != 0) {
            fprintf(stderr, "%s: %s\n", argv[0], err);
            return 2;
        }

    le_model *m = model_new(sess.count);
    if (!m) {
        fprintf(stderr, "%s: out of memory\n", argv[0]);
        return 1;
    }

    for (int i = 0; i < sess.count; i++)
        if (archive_load(session_path(&sess, i), m, i, err, sizeof(err)) != 0) {
            fprintf(stderr, "%s: %s\n", argv[0], err);
            model_free(m);
            return 1;
        }

    int rc = tui_run(&sess, m);
    model_free(m);
    return rc;
}
