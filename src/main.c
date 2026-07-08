#include "archive.h"
#include "tree.h"
#include "tui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s <archive.tar.gz>\n"
            "\n"
            "Interactive editor for a filesystem-layer tar.gz archive.\n"
            "Browse the hierarchy, mark paths for removal, add host files,\n"
            "and save. Press '?' inside the editor for keybindings.\n",
            prog);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        usage(argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        usage(argv[0]);
        return 0;
    }

    le_model *m = model_new();
    if (!m) {
        fprintf(stderr, "%s: out of memory\n", argv[0]);
        return 1;
    }

    char err[256] = "";
    if (archive_load(argv[1], m, err, sizeof(err)) != 0) {
        fprintf(stderr, "%s: %s\n", argv[0], err);
        model_free(m);
        return 1;
    }

    int rc = tui_run(argv[1], m);
    model_free(m);
    return rc;
}
