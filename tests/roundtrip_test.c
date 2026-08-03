/*
 * Headless round-trip tests for archive load/save and model edit semantics.
 * Links against tar/tree/archive (no TUI).
 */
#include "../src/archive.h"
#include "../src/session.h"
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

/* Copy one host file into an open writer under the given guest path. */
static int write_member(tar_t *out, const char *guest_path,
                        const char *host_file) {
    struct stat st;
    if (stat(host_file, &st) == -1)
        return -1;
    tar_entry e;
    memset(&e, 0, sizeof(e));
    strlcpy(e.path, guest_path, sizeof(e.path));
    e.type = TAR_FILE;
    e.mode = S_IFREG | 0644;
    e.size = st.st_size;
    e.mtime = st.st_mtime;
    if (tar_write_header(out, &e) == -1)
        return -1;
    FILE *f = fopen(host_file, "rb");
    if (!f)
        return -1;
    char buf[4096];
    size_t rd;
    int rc = 0;
    while ((rd = fread(buf, 1, sizeof(buf), f)) > 0)
        if (tar_write_data(out, buf, rd) == -1) {
            rc = -1;
            break;
        }
    fclose(f);
    return rc;
}

/* Build a tiny .tar.gz holding the given guest paths, all with the same body. */
static int build_archive(const char *archive_path,
                         const char *const *guest_paths, int nguests,
                         const char *host_file) {
    tar_t *out = tar_wopen(archive_path, 6);
    if (!out)
        return -1;
    int rc = 0;
    for (int i = 0; i < nguests && rc == 0; i++)
        rc = write_member(out, guest_paths[i], host_file);
    if (tar_close(out) == -1)
        rc = -1;
    return rc;
}

static int build_seed_archive(const char *archive_path, const char *guest_path,
                              const char *host_file) {
    return build_archive(archive_path, &guest_path, 1, host_file);
}

static void test_double_save_no_duplicate_additions(const char *dir) {
    char archive[512], host[512], host2[512], err[256];
    snprintf(archive, sizeof(archive), "%s/layer.tar.gz", dir);
    snprintf(host, sizeof(host), "%s/seed.txt", dir);
    snprintf(host2, sizeof(host2), "%s/added.txt", dir);
    EXPECT(write_host_file(host, "seed") == 0, "write seed");
    EXPECT(write_host_file(host2, "added-body") == 0, "write added");
    EXPECT(build_seed_archive(archive, "seed.txt", host) == 0, "seed archive");

    le_model *m = model_new(1);
    EXPECT(m != NULL, "model_new");
    EXPECT(archive_load(archive, m, 0, err, sizeof(err)) == 0, "load: %s", err);
    EXPECT(model_add_host(m, "new/file.txt", 0, host2, 0, 10, S_IFREG | 0644,
                          NULL) != NULL,
           "add host");
    EXPECT(archive_save(archive, m, 0, err, sizeof(err)) == 0, "save1: %s", err);
    EXPECT(m->dirty == 0, "dirty cleared");
    EXPECT(m->added == 0, "added counter cleared after reconcile");
    EXPECT(archive_save(archive, m, 0, err, sizeof(err)) == 0, "save2: %s", err);
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

    le_model *m = model_new(1);
    EXPECT(archive_load(archive, m, 0, err, sizeof(err)) == 0, "load: %s", err);
    EXPECT(model_add_host(m, "path/file.txt", 0, replacement, 0, 8,
                          S_IFREG | 0644, NULL) != NULL,
           "replace add");
    EXPECT(archive_save(archive, m, 0, err, sizeof(err)) == 0, "save: %s", err);
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

    le_model *m = model_new(1);
    EXPECT(archive_load(archive, m, 0, err, sizeof(err)) == 0, "load: %s", err);
    le_node *n = model_lookup(m, "doomed.txt");
    EXPECT(n != NULL, "lookup");
    model_toggle_removed(m, n, 0);
    EXPECT(archive_save(archive, m, 0, err, sizeof(err)) == 0, "save: %s", err);
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

    /* Seed with a placeholder member, then swap it for the long path. */
    EXPECT(build_seed_archive(archive, "placeholder", host) == 0, "seed");

    le_model *m = model_new(1);
    EXPECT(archive_load(archive, m, 0, err, sizeof(err)) == 0, "load: %s", err);
    le_node *ph = model_lookup(m, "placeholder");
    EXPECT(ph != NULL, "placeholder");
    model_toggle_removed(m, ph, 0);
    EXPECT(model_add_host(m, longpath, 0, host, 0, 4, S_IFREG | 0644,
                          NULL) != NULL,
           "add long path");
    EXPECT(archive_save(archive, m, 0, err, sizeof(err)) == 0, "save: %s", err);
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

    le_model *m = model_new(1);
    EXPECT(archive_load(archive, m, 0, err, sizeof(err)) == 0, "load: %s", err);
    EXPECT(model_add_host(m, "link", 0, linkpath, 1, 0, S_IFLNK | 0755,
                          "target.txt") != NULL,
           "add symlink");
    le_node *n = model_lookup(m, "link");
    EXPECT(n && n->link && !strcmp(n->link, "target.txt"), "link target in model");
    EXPECT(archive_save(archive, m, 0, err, sizeof(err)) == 0, "save: %s", err);

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

/* Load two archives that share one path into a single model. */
static le_model *load_pair(const char *dir, const char *a_path,
                           const char *b_path) {
    char host[512], err[256];
    snprintf(host, sizeof(host), "%s/pair-body.txt", dir);
    if (write_host_file(host, "body") != 0)
        return NULL;

    const char *a_members[] = {"shared.txt", "only-a.txt"};
    const char *b_members[] = {"shared.txt", "only-b.txt"};
    EXPECT(build_archive(a_path, a_members, 2, host) == 0, "build a");
    EXPECT(build_archive(b_path, b_members, 2, host) == 0, "build b");

    le_model *m = model_new(2);
    EXPECT(m != NULL, "model_new(2)");
    EXPECT(archive_load(a_path, m, 0, err, sizeof(err)) == 0, "load a: %s", err);
    EXPECT(archive_load(b_path, m, 1, err, sizeof(err)) == 0, "load b: %s", err);
    return m;
}

static void test_multi_membership(const char *dir) {
    char a[512], b[512];
    snprintf(a, sizeof(a), "%s/mem-a.tar.gz", dir);
    snprintf(b, sizeof(b), "%s/mem-b.tar.gz", dir);
    le_model *m = load_pair(dir, a, b);

    le_node *shared = model_lookup(m, "shared.txt");
    EXPECT(shared != NULL, "shared path in model");
    EXPECT(node_in_archive(shared, 0) && node_in_archive(shared, 1),
           "shared path belongs to both archives");

    le_node *only_a = model_lookup(m, "only-a.txt");
    EXPECT(only_a && node_in_archive(only_a, 0) && !node_in_archive(only_a, 1),
           "only-a.txt belongs to the first archive alone");

    int slots[LE_MAX_ARCHIVES];
    EXPECT(node_archives(shared, m->narchives, slots) == 2, "two owners");
    EXPECT(node_archives(only_a, m->narchives, slots) == 1, "one owner");
    model_free(m);
}

static void test_multi_remove_scoped_to_one_archive(const char *dir) {
    char a[512], b[512], err[256];
    snprintf(a, sizeof(a), "%s/scoped-a.tar.gz", dir);
    snprintf(b, sizeof(b), "%s/scoped-b.tar.gz", dir);
    le_model *m = load_pair(dir, a, b);

    le_node *shared = model_lookup(m, "shared.txt");
    model_toggle_removed(m, shared, 0);
    EXPECT(m->arch_dirty[0] == 1, "first archive dirty");
    EXPECT(m->arch_dirty[1] == 0, "second archive untouched");
    EXPECT(m->dirty == 1, "session dirty");

    /* Saving the untouched archive too must not carry the removal across. */
    EXPECT(archive_save(a, m, 0, err, sizeof(err)) == 0, "save a: %s", err);
    EXPECT(archive_save(b, m, 1, err, sizeof(err)) == 0, "save b: %s", err);
    EXPECT(count_path_occurrences(a, "shared.txt") == 0, "dropped from a");
    EXPECT(count_path_occurrences(b, "shared.txt") == 1, "kept in b");

    le_node *still = model_lookup(m, "shared.txt");
    EXPECT(still != NULL, "path survives for the other archive");
    EXPECT(still && !node_in_archive(still, 0) && node_in_archive(still, 1),
           "membership now only the second archive");
    EXPECT(m->dirty == 0, "no dirty archives left");
    model_free(m);
}

static void test_multi_remove_from_all_archives(const char *dir) {
    char a[512], b[512], err[256];
    snprintf(a, sizeof(a), "%s/all-a.tar.gz", dir);
    snprintf(b, sizeof(b), "%s/all-b.tar.gz", dir);
    le_model *m = load_pair(dir, a, b);

    model_toggle_removed_all(m, model_lookup(m, "shared.txt"));
    EXPECT(m->arch_dirty[0] && m->arch_dirty[1], "both archives dirty");
    EXPECT(m->marked == 2, "marked counted per archive, got %zu", m->marked);

    EXPECT(archive_save(a, m, 0, err, sizeof(err)) == 0, "save a: %s", err);
    EXPECT(archive_save(b, m, 1, err, sizeof(err)) == 0, "save b: %s", err);
    EXPECT(count_path_occurrences(a, "shared.txt") == 0, "dropped from a");
    EXPECT(count_path_occurrences(b, "shared.txt") == 0, "dropped from b");
    EXPECT(model_lookup(m, "shared.txt") == NULL, "dropped from the model");
    EXPECT(model_lookup(m, "only-a.txt") != NULL, "unrelated path kept");
    EXPECT(m->marked == 0 && m->dirty == 0, "counters cleared");
    model_free(m);
}

static void test_multi_add_targets_one_archive(const char *dir) {
    char a[512], b[512], host[512], err[256];
    snprintf(a, sizeof(a), "%s/add-a.tar.gz", dir);
    snprintf(b, sizeof(b), "%s/add-b.tar.gz", dir);
    snprintf(host, sizeof(host), "%s/add-body.txt", dir);
    EXPECT(write_host_file(host, "new-body") == 0, "write host");
    le_model *m = load_pair(dir, a, b);

    EXPECT(model_add_host(m, "extra/new.txt", 1, host, 0, 8, S_IFREG | 0644,
                          NULL) != NULL,
           "stage into second archive");
    EXPECT(m->arch_dirty[0] == 0 && m->arch_dirty[1] == 1,
           "only the target archive is dirty");

    EXPECT(archive_save(b, m, 1, err, sizeof(err)) == 0, "save b: %s", err);
    EXPECT(count_path_occurrences(b, "extra/new.txt") == 1, "added to b");
    EXPECT(count_path_occurrences(a, "extra/new.txt") == 0, "absent from a");

    le_node *added = model_lookup(m, "extra/new.txt");
    EXPECT(added && node_in_archive(added, 1) && !node_in_archive(added, 0),
           "member of the second archive only");
    EXPECT(added && added->arch[1].added == 0, "staged flag cleared after save");
    model_free(m);
}

static void test_session_rejects_duplicates_and_overflow(const char *dir) {
    char err[256], path[512];
    le_session s;
    memset(&s, 0, sizeof(s));

    snprintf(path, sizeof(path), "%s/sess-0.tar.gz", dir);
    EXPECT(write_host_file(path, "x") == 0, "write archive stand-in");
    EXPECT(session_add(&s, path, err, sizeof(err)) == 0, "first add: %s", err);
    EXPECT(session_add(&s, path, err, sizeof(err)) == -1, "duplicate rejected");
    EXPECT(s.count == 1, "duplicate not recorded");
    EXPECT(!strcmp(session_label(&s, 0), "sess-0.tar.gz"), "label is basename");

    for (int i = 1; i < LE_MAX_ARCHIVES; i++) {
        snprintf(path, sizeof(path), "%s/sess-%d.tar.gz", dir, i);
        EXPECT(write_host_file(path, "x") == 0, "write %d", i);
        EXPECT(session_add(&s, path, err, sizeof(err)) == 0, "add %d: %s", i, err);
    }
    EXPECT(s.count == LE_MAX_ARCHIVES, "session full");

    snprintf(path, sizeof(path), "%s/sess-overflow.tar.gz", dir);
    EXPECT(write_host_file(path, "x") == 0, "write overflow");
    EXPECT(session_add(&s, path, err, sizeof(err)) == -1, "overflow rejected");

    EXPECT(session_add(&s, "/nonexistent/nope.tar.gz", err, sizeof(err)) == -1,
           "missing path rejected");
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
    test_multi_membership(dir);
    test_multi_remove_scoped_to_one_archive(dir);
    test_multi_remove_from_all_archives(dir);
    test_multi_add_targets_one_archive(dir);
    test_session_rejects_duplicates_and_overflow(dir);

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
