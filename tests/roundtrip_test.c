/*
 * Headless round-trip tests for archive load/save and model edit semantics.
 * Links against tar/tree/archive (no TUI).
 */
#include "../src/archive.h"
#include "../src/tar.h"
#include "../src/tree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

static int failures = 0;

#define EXPECT(cond, fmt, ...)                                                 \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: " fmt "\n", __FILE__, __LINE__,       \
                    ##__VA_ARGS__);                                            \
            failures++;                                                        \
        }                                                                      \
    } while (0)

static char *make_tmpdir(void) {
    char *tmpl = strdup("/tmp/anka-layer-editor-test.XXXXXX");
    if (!tmpl || !mkdtemp(tmpl)) {
        free(tmpl);
        return NULL;
    }
    return tmpl;
}

static int write_host_file(const char *path, const char *contents) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    size_t n = strlen(contents);
    int ok = fwrite(contents, 1, n, f) == n;
    fclose(f);
    return ok ? 0 : -1;
}

static int count_path_occurrences(const char *archive, const char *want) {
    tar_t *in = tar_ropen(archive);
    if (!in)
        return -1;
    tar_entry e;
    int r, count = 0;
    while ((r = tar_read_next(in, &e)) == 1) {
        if (!strcmp(e.path, want))
            count++;
        tar_skip_data(in);
    }
    tar_close(in);
    return r < 0 ? -1 : count;
}

static int read_file_entry_contents(const char *archive, const char *want,
                                    char *out, size_t outlen) {
    tar_t *in = tar_ropen(archive);
    if (!in)
        return -1;
    tar_entry e;
    int r, found = 0;
    while ((r = tar_read_next(in, &e)) == 1) {
        if (!strcmp(e.path, want) && e.type == TAR_FILE) {
            size_t got = 0;
            while (got + 1 < outlen) {
                ssize_t n = tar_read_data(in, out + got, outlen - 1 - got);
                if (n < 0) {
                    tar_close(in);
                    return -1;
                }
                if (n == 0)
                    break;
                got += (size_t)n;
            }
            out[got] = '\0';
            found = 1;
            break;
        }
        tar_skip_data(in);
    }
    tar_close(in);
    if (r < 0)
        return -1;
    return found ? 0 : -1;
}

/* Build a tiny .tar.gz with one regular file via the writer. */
static int build_seed_archive(const char *archive_path, const char *guest_path,
                              const char *host_file) {
    struct stat st;
    if (stat(host_file, &st) == -1)
        return -1;
    tar_t *out = tar_wopen(archive_path, 6);
    if (!out)
        return -1;
    tar_entry e;
    memset(&e, 0, sizeof(e));
    strlcpy(e.path, guest_path, sizeof(e.path));
    e.type = TAR_FILE;
    e.mode = S_IFREG | 0644;
    e.size = st.st_size;
    e.mtime = st.st_mtime;
    if (tar_write_header(out, &e) == -1) {
        tar_close(out);
        return -1;
    }
    FILE *f = fopen(host_file, "rb");
    if (!f) {
        tar_close(out);
        return -1;
    }
    char buf[4096];
    size_t rd;
    int rc = 0;
    while ((rd = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (tar_write_data(out, buf, rd) == -1) {
            rc = -1;
            break;
        }
    }
    fclose(f);
    if (tar_close(out) == -1)
        rc = -1;
    return rc;
}

static void test_double_save_no_duplicate_additions(const char *dir) {
    char archive[512], host[512], host2[512], err[256];
    snprintf(archive, sizeof(archive), "%s/layer.tar.gz", dir);
    snprintf(host, sizeof(host), "%s/seed.txt", dir);
    snprintf(host2, sizeof(host2), "%s/added.txt", dir);
    EXPECT(write_host_file(host, "seed") == 0, "write seed");
    EXPECT(write_host_file(host2, "added-body") == 0, "write added");
    EXPECT(build_seed_archive(archive, "seed.txt", host) == 0, "seed archive");

    le_model *m = model_new();
    EXPECT(m != NULL, "model_new");
    EXPECT(archive_load(archive, m, err, sizeof(err)) == 0, "load: %s", err);
    EXPECT(model_add_host(m, "new/file.txt", host2, 0, 10, S_IFREG | 0644,
                          NULL) != NULL,
           "add host");
    EXPECT(archive_save(archive, m, err, sizeof(err)) == 0, "save1: %s", err);
    EXPECT(m->dirty == 0, "dirty cleared");
    EXPECT(m->added == 0, "added counter cleared after reconcile");
    EXPECT(archive_save(archive, m, err, sizeof(err)) == 0, "save2: %s", err);
    int n = count_path_occurrences(archive, "new/file.txt");
    EXPECT(n == 1, "expected 1 occurrence of new/file.txt, got %d", n);
    model_free(m);
}

static void test_add_over_existing_replaces(const char *dir) {
    char archive[512], host[512], replacement[512], err[256], body[64];
    snprintf(archive, sizeof(archive), "%s/replace.tar.gz", dir);
    snprintf(host, sizeof(host), "%s/orig.txt", dir);
    snprintf(replacement, sizeof(replacement), "%s/repl.txt", dir);
    EXPECT(write_host_file(host, "original") == 0, "write orig");
    EXPECT(write_host_file(replacement, "replaced") == 0, "write repl");
    EXPECT(build_seed_archive(archive, "path/file.txt", host) == 0, "seed");

    le_model *m = model_new();
    EXPECT(archive_load(archive, m, err, sizeof(err)) == 0, "load: %s", err);
    EXPECT(model_add_host(m, "path/file.txt", replacement, 0, 8,
                          S_IFREG | 0644, NULL) != NULL,
           "replace add");
    EXPECT(archive_save(archive, m, err, sizeof(err)) == 0, "save: %s", err);
    int n = count_path_occurrences(archive, "path/file.txt");
    EXPECT(n == 1, "expected single path/file.txt, got %d", n);
    EXPECT(read_file_entry_contents(archive, "path/file.txt", body,
                                    sizeof(body)) == 0,
           "read contents");
    EXPECT(!strcmp(body, "replaced"), "contents want 'replaced' got '%s'", body);
    model_free(m);
}

static void test_remove_reconciles_model(const char *dir) {
    char archive[512], host[512], err[256];
    snprintf(archive, sizeof(archive), "%s/rm.tar.gz", dir);
    snprintf(host, sizeof(host), "%s/rm.txt", dir);
    EXPECT(write_host_file(host, "gone") == 0, "write");
    EXPECT(build_seed_archive(archive, "doomed.txt", host) == 0, "seed");

    le_model *m = model_new();
    EXPECT(archive_load(archive, m, err, sizeof(err)) == 0, "load: %s", err);
    le_node *n = model_lookup(m, "doomed.txt");
    EXPECT(n != NULL, "lookup");
    model_toggle_removed(m, n);
    EXPECT(archive_save(archive, m, err, sizeof(err)) == 0, "save: %s", err);
    EXPECT(count_path_occurrences(archive, "doomed.txt") == 0, "removed from archive");
    EXPECT(model_lookup(m, "doomed.txt") == NULL, "removed from model");
    EXPECT(m->marked == 0, "marked counter cleared");
    EXPECT(m->dirty == 0, "dirty cleared");
    model_free(m);
}

static void test_long_path_roundtrip(const char *dir) {
    char archive[512], host[512], err[256];
    char longpath[256];
    memset(longpath, 'a', 140);
    longpath[140] = '\0';
    snprintf(longpath + 100, sizeof(longpath) - 100, "/file.txt");

    snprintf(archive, sizeof(archive), "%s/long.tar.gz", dir);
    snprintf(host, sizeof(host), "%s/long.txt", dir);
    EXPECT(write_host_file(host, "long") == 0, "write");

    le_model *m = model_new();
    EXPECT(model_add_host(m, longpath, host, 0, 4, S_IFREG | 0644, NULL) != NULL,
           "add long");
    /* Empty seed archive: write via save with only additions needs an existing file.
     * Create empty-ish archive with a tiny placeholder then remove it. */
    EXPECT(build_seed_archive(archive, "placeholder", host) == 0, "seed");
    EXPECT(archive_load(archive, m, err, sizeof(err)) == 0, "load: %s", err);
    /* Re-add long path after load (load wiped? no - same model already has add).
     * Fresh model is cleaner: */
    model_free(m);
    m = model_new();
    EXPECT(archive_load(archive, m, err, sizeof(err)) == 0, "load2: %s", err);
    le_node *ph = model_lookup(m, "placeholder");
    EXPECT(ph != NULL, "placeholder");
    model_toggle_removed(m, ph);
    EXPECT(model_add_host(m, longpath, host, 0, 4, S_IFREG | 0644, NULL) != NULL,
           "add long path");
    EXPECT(archive_save(archive, m, err, sizeof(err)) == 0, "save: %s", err);
    EXPECT(count_path_occurrences(archive, longpath) == 1, "long path present once");
    EXPECT(count_path_occurrences(archive, "placeholder") == 0, "placeholder gone");
    model_free(m);
}

static void test_symlink_add(const char *dir) {
    char archive[512], host[512], linkpath[512], err[256];
    snprintf(archive, sizeof(archive), "%s/sym.tar.gz", dir);
    snprintf(host, sizeof(host), "%s/target.txt", dir);
    snprintf(linkpath, sizeof(linkpath), "%s/thelink", dir);
    EXPECT(write_host_file(host, "tgt") == 0, "write target");
    unlink(linkpath);
    EXPECT(symlink("target.txt", linkpath) == 0, "symlink: %s", strerror(errno));
    EXPECT(build_seed_archive(archive, "keep.txt", host) == 0, "seed");

    le_model *m = model_new();
    EXPECT(archive_load(archive, m, err, sizeof(err)) == 0, "load: %s", err);
    EXPECT(model_add_host(m, "link", linkpath, 1, 0, S_IFLNK | 0755,
                          "target.txt") != NULL,
           "add symlink");
    le_node *n = model_lookup(m, "link");
    EXPECT(n && n->link && !strcmp(n->link, "target.txt"), "link target in model");
    EXPECT(archive_save(archive, m, err, sizeof(err)) == 0, "save: %s", err);

    tar_t *in = tar_ropen(archive);
    EXPECT(in != NULL, "reopen");
    tar_entry e;
    int r, found = 0;
    while ((r = tar_read_next(in, &e)) == 1) {
        if (!strcmp(e.path, "link")) {
            found = 1;
            EXPECT(e.type == TAR_SYMLINK, "type symlink");
            EXPECT(!strcmp(e.link, "target.txt"), "linkpath %s", e.link);
        }
        tar_skip_data(in);
    }
    tar_close(in);
    EXPECT(found, "symlink entry found");
    model_free(m);
}

static void test_write_byte_mismatch_fails(const char *dir) {
    /* Declared size larger than bytes written must fail closed on close/finish. */
    char archive[512];
    snprintf(archive, sizeof(archive), "%s/mismatch.tar.gz", dir);
    tar_t *out = tar_wopen(archive, 6);
    EXPECT(out != NULL, "wopen");
    tar_entry e;
    memset(&e, 0, sizeof(e));
    strlcpy(e.path, "short.txt", sizeof(e.path));
    e.type = TAR_FILE;
    e.mode = S_IFREG | 0644;
    e.size = 100; /* claim 100 bytes */
    EXPECT(tar_write_header(out, &e) == 0, "header");
    EXPECT(tar_write_data(out, "hi", 2) == 2, "partial write");
    EXPECT(tar_close(out) == -1, "close must fail on short write");
    unlink(archive);
}

int main(void) {
    char *dir = make_tmpdir();
    if (!dir) {
        fprintf(stderr, "mkdtemp failed\n");
        return 1;
    }

    test_double_save_no_duplicate_additions(dir);
    test_add_over_existing_replaces(dir);
    test_remove_reconciles_model(dir);
    test_long_path_roundtrip(dir);
    test_symlink_add(dir);
    test_write_byte_mismatch_fails(dir);

    /* Best-effort cleanup of temp dir contents is optional; leave for OS. */
    if (failures) {
        fprintf(stderr, "%d failure(s)  (tmpdir %s)\n", failures, dir);
        free(dir);
        return 1;
    }
    printf("ok\n");
    free(dir);
    return 0;
}
