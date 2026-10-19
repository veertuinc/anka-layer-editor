/*
 * Headless tests for VM mode: layer loading from a stream, the change set,
 * and the anka CLI wrapper (driven by the fake scripts in tests/fake-anka).
 */
#include "test_support.h"
#include "../src/anka_cli.h"
#include "../src/archive.h"
#include "../src/change_set.h"
#include "../src/proc.h"
#include "../src/tar.h"
#include "../src/tree.h"
#include "../src/util.h"
#include "../src/vm_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

/* Feed a file into a pipe from a child, so the reader sees a real stream. */
static int open_file_as_pipe(const char *path, pid_t *writer_pid) {
    int pipe_fds[2];
    if (pipe(pipe_fds) == -1)
        return -1;
    pid_t pid = fork();
    if (pid == 0) {
        close(pipe_fds[0]);
        int in = open(path, O_RDONLY);
        char buf[4096];
        ssize_t n;
        while (in != -1 && (n = read(in, buf, sizeof(buf))) > 0)
            if (write(pipe_fds[1], buf, (size_t)n) != n)
                break;
        _exit(0);
    }
    close(pipe_fds[1]);
    *writer_pid = pid;
    return pipe_fds[0];
}

static void test_load_layer_from_pipe(const char *dir) {
    char layer[512], err[256];
    snprintf(layer, sizeof(layer), "%s/pipe-layer.tar.gz", dir);
    const raw_member_spec specs[] = {
        {"Users/anka/new", '5', NULL, 1, NULL, 501, 20},
        {"Users/anka/new/file.txt", '0', NULL, 1, "a body bigger than nothing", 501, 20},
        {"private/etc/hosts", '0', NULL, 2, "hosts", 0, 0},
        {"Users/anka/Music/.wh..localized", '0', NULL, 4, NULL, 501, 20},
    };
    EXPECT(build_raw_archive(layer, specs, 4) == 0, "build layer");

    pid_t writer_pid = -1;
    int read_fd = open_file_as_pipe(layer, &writer_pid);
    tar_t *in = read_fd == -1 ? NULL : tar_rdopen(read_fd);
    EXPECT(in != NULL, "open reader on pipe");
    le_model *m = model_new(1);
    EXPECT(in && archive_load_tar(in, "pipe", m, 0, err, sizeof(err)) == 0,
           "load from pipe: %s", err);
    if (in)
        tar_close(in);
    waitpid(writer_pid, NULL, 0);

    le_node *file = model_lookup(m, "Users/anka/new/file.txt");
    EXPECT(file && file->arch[0].change_kind == LE_CHANGE_ADDED &&
           file->meta.size == 26, "added file loaded with its size");
    le_node *hosts = model_lookup(m, "private/etc/hosts");
    EXPECT(hosts && hosts->arch[0].change_kind == LE_CHANGE_MODIFIED,
           "modified file keeps its kind");
    le_node *whiteout = model_lookup(m, "Users/anka/Music/.wh..localized");
    EXPECT(whiteout && node_is_whiteout(whiteout), "whiteout loaded");
    model_free(m);
}

static void test_clear_slot_keeps_other_slots(const char *dir) {
    char host[512];
    snprintf(host, sizeof(host), "%s/clear-host.txt", dir);
    EXPECT(write_host_file(host, "x") == 0, "write host");
    le_meta file_meta = {.kind = LE_KIND_FILE, .mode = S_IFREG | 0644};
    le_model *m = model_new(2);
    model_insert(m, "a/only-first", 0, &file_meta, LE_CHANGE_ADDED);
    model_insert(m, "a/shared", 0, &file_meta, LE_CHANGE_ADDED);
    model_insert(m, "a/shared", 1, &file_meta, LE_CHANGE_ADDED);
    model_insert(m, "b/only-second", 1, &file_meta, LE_CHANGE_ADDED);
    model_toggle_removed(m, model_lookup(m, "a/shared"), 0);
    EXPECT(stage_host(m, "a/staged", 0, host) != NULL, "stage");
    model_toggle_removed(m, model_lookup(m, "b/only-second"), 1);

    model_clear_slot(m, 0);
    EXPECT(model_lookup(m, "a/only-first") == NULL, "first-only path gone");
    EXPECT(model_lookup(m, "a/staged") == NULL, "staged path gone");
    le_node *shared = model_lookup(m, "a/shared");
    EXPECT(shared && !shared->arch[0].present && !shared->arch[0].removed &&
           shared->arch[1].present, "shared path kept for the second slot");
    EXPECT(model_lookup(m, "b/only-second") != NULL, "second-only path kept");
    EXPECT(m->arch_marked[0] == 0 && m->arch_added[0] == 0 && m->marked == 1 &&
           m->added == 0, "counters: marked %zu added %zu", m->marked, m->added);
    EXPECT(m->total == 4, "total counts a, a/shared, b, b/only-second: %zu",
           m->total);
    EXPECT(!model_arch_dirty(m, 0) && model_arch_dirty(m, 1),
           "only the second slot stays dirty");
    model_free(m);
}

/* One slot that holds every change kind:
 *   added-dir/{g, sub/f}  all kind 1
 *   mixed-dir/changed     dir kind 1, file kind 2
 *   changed (2), .wh.gone (4), untagged (0), keep (1)
 *   implicit/added        kind 1 under a directory with no entry
 *   anka-dir/added        dir kind 0, file kind 1 */
static le_model *change_set_fixture(void) {
    le_meta dir_meta = {.kind = LE_KIND_DIR, .mode = S_IFDIR | 0755,
                        .uid = 501, .gid = 20};
    le_meta file_meta = {.kind = LE_KIND_FILE, .mode = S_IFREG | 0644,
                         .uid = 501, .gid = 20};
    le_model *m = model_new(1);
    model_insert(m, "added-dir", 0, &dir_meta, LE_CHANGE_ADDED);
    model_insert(m, "added-dir/g", 0, &file_meta, LE_CHANGE_ADDED);
    model_insert(m, "added-dir/sub", 0, &dir_meta, LE_CHANGE_ADDED);
    model_insert(m, "added-dir/sub/f", 0, &file_meta, LE_CHANGE_ADDED);
    model_insert(m, "mixed-dir", 0, &dir_meta, LE_CHANGE_ADDED);
    model_insert(m, "mixed-dir/changed", 0, &file_meta, LE_CHANGE_MODIFIED);
    model_insert(m, "changed", 0, &file_meta, LE_CHANGE_MODIFIED);
    model_insert(m, ".wh.gone", 0, &file_meta, LE_CHANGE_WHITEOUT);
    model_insert(m, "untagged", 0, &file_meta, LE_CHANGE_UNTAGGED);
    model_insert(m, "keep", 0, &file_meta, LE_CHANGE_ADDED);
    model_insert(m, "implicit/added", 0, &file_meta, LE_CHANGE_ADDED);
    model_insert(m, "anka-dir", 0, &dir_meta, LE_CHANGE_UNTAGGED);
    model_insert(m, "anka-dir/added", 0, &file_meta, LE_CHANGE_ADDED);
    return m;
}

static int may_remove_path(le_model *m, const char *path) {
    le_node *n = model_lookup(m, path);
    return n && change_set_may_remove(n, 0);
}

static void test_change_set_may_remove(void) {
    le_model *m = change_set_fixture();
    EXPECT(may_remove_path(m, "added-dir"), "added dir subtree may go");
    EXPECT(may_remove_path(m, "added-dir/sub/f"), "added file may go");
    EXPECT(may_remove_path(m, "implicit/added"), "added file under implicit dir");
    EXPECT(!may_remove_path(m, "mixed-dir"), "dir with a changed file blocked");
    EXPECT(!may_remove_path(m, "mixed-dir/changed"), "changed file blocked");
    EXPECT(!may_remove_path(m, "changed"), "kind 2 blocked");
    EXPECT(!may_remove_path(m, ".wh.gone"), "kind 4 blocked");
    EXPECT(!may_remove_path(m, "untagged"), "kind 0 blocked");
    EXPECT(!may_remove_path(m, "implicit"), "dir with no entry blocked");
    EXPECT(may_remove_path(m, "anka-dir"),
           "kind 0 dir (how Anka's diff writes a new dir) may go");
    model_free(m);
}

typedef struct {
    char          path[256];
    tar_type      type;
    unsigned long devmajor;
    uid_t         uid;
} written_member;

static int read_written_members(const char *archive, written_member *members,
                                int max_members) {
    tar_t *in = tar_ropen(archive);
    if (!in)
        return -1;
    tar_entry e;
    int count = 0;
    while (count < max_members && tar_read_next(in, &e) == 1) {
        size_t len = strlen(e.path);
        if (len && e.path[len - 1] == '/')
            e.path[len - 1] = '\0';
        strlcpy(members[count].path, e.path, sizeof(members[count].path));
        members[count].type = e.type;
        members[count].devmajor = e.devmajor;
        members[count].uid = e.uid;
        count++;
    }
    tar_close(in);
    return count;
}

static int write_change_set_file(const char *archive, le_model *m, char *err,
                                 size_t errlen) {
    tar_t *out = tar_wopen(archive, 6);
    if (!out)
        return -1;
    int rc = change_set_write(out, m, 0, err, errlen);
    return tar_close(out) == -1 ? -1 : rc;
}

static void test_change_set_write_order(const char *dir) {
    char host[512], archive[512], err[256] = "";
    snprintf(host, sizeof(host), "%s/change-set-host.txt", dir);
    snprintf(archive, sizeof(archive), "%s/change-set.tar.gz", dir);
    EXPECT(write_host_file(host, "new\n") == 0, "write host");

    le_model *m = change_set_fixture();
    model_toggle_mark(m, model_lookup(m, "added-dir"), 0);
    EXPECT(stage_host(m, "new.txt", 0, host) != NULL, "stage new.txt");
    EXPECT(write_change_set_file(archive, m, err, sizeof(err)) == 0,
           "write change set: %s", err);

    static const struct {
        const char *path;
        tar_type    type;
    } expected[] = {
        {"added-dir/.wh.g", TAR_FILE},
        {"added-dir/sub/.wh.f", TAR_FILE},
        {"added-dir/.wh.sub", TAR_DIR},
        {".wh.added-dir", TAR_DIR},
        {"new.txt", TAR_FILE},
    };
    int nexpected = (int)(sizeof(expected) / sizeof(expected[0]));
    written_member members[16];
    int count = read_written_members(archive, members, 16);
    EXPECT(count == nexpected, "member count %d", count);
    for (int i = 0; i < count && i < nexpected; i++) {
        EXPECT(!strcmp(members[i].path, expected[i].path),
               "member %d is '%s', want '%s'", i, members[i].path,
               expected[i].path);
        EXPECT(members[i].type == expected[i].type, "member %d type", i);
        if (i < nexpected - 1)
            EXPECT(members[i].devmajor == LE_CHANGE_WHITEOUT &&
                   members[i].uid == 501, "member %d is a kind 4 whiteout "
                   "owned by 501", i);
    }
    model_free(m);
}

static void test_change_set_write_refuses_base_version(const char *dir) {
    char archive[512], err[256] = "";
    snprintf(archive, sizeof(archive), "%s/change-set-refused.tar.gz", dir);
    le_model *m = change_set_fixture();
    model_toggle_mark(m, model_lookup(m, "mixed-dir"), 0);
    EXPECT(write_change_set_file(archive, m, err, sizeof(err)) == -1,
           "removing a dir with a changed file is refused");
    EXPECT(strstr(err, "mixed-dir") != NULL, "error names the path: %s", err);
    model_free(m);
}

/* ---- anka CLI, driven by tests/fake-anka ---- */

static char fake_state_dir[512];

static void write_fake_state(const char *name, const char *contents) {
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", fake_state_dir, name);
    EXPECT(write_host_file(path, contents) == 0, "write fake state %s", name);
}

static int read_fake_state(const char *name, char *contents, size_t len) {
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", fake_state_dir, name);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    size_t got = fread(contents, 1, len - 1, f);
    contents[got] = '\0';
    fclose(f);
    return 0;
}

/* Fresh state for one VM "myvm" on tag "base", fakes first on PATH. */
static void fake_anka_reset(const char *dir) {
    snprintf(fake_state_dir, sizeof(fake_state_dir), "%s/fake-state", dir);
    char command[2 * sizeof(fake_state_dir) + 64];
    snprintf(command, sizeof(command), "rm -rf '%s' && mkdir -p '%s'",
             fake_state_dir, fake_state_dir);
    EXPECT(system(command) == 0, "reset fake state");
    write_fake_state("working.img", "");
    write_fake_state("tag.ank", "");
    setenv("FAKE_ANKA_DIR", fake_state_dir, 1);
    unsetenv("FAKE_ANKA_FAIL");
    unsetenv("FAKE_ANKA_DIRTY");
}

static void put_fakes_on_path(void) {
    char cwd[512], path[4096];
    EXPECT(getcwd(cwd, sizeof(cwd)) != NULL, "getcwd");
    snprintf(path, sizeof(path), "%s/tests/fake-anka:%s", cwd, getenv("PATH"));
    setenv("PATH", path, 1);
}

static void test_anka_cli_vm_info(const char *dir) {
    fake_anka_reset(dir);
    anka_vm_info info;
    char err[512] = "";
    EXPECT(anka_cli_vm_info("myvm", &info, err, sizeof(err)) == 0,
           "vm info: %s", err);
    EXPECT(!strcmp(info.uuid, "fake-uuid-myvm"), "uuid '%s'", info.uuid);
    EXPECT(!strcmp(info.status, "stopped"), "status '%s'", info.status);
    EXPECT(!strcmp(info.current_tag, "base"), "tag '%s'", info.current_tag);
    EXPECT(!strcmp(info.working_disk_file, "working.img"), "working disk");
    EXPECT(!strcmp(info.tag_image_file, "tag.ank"), "tag image");
    EXPECT(!strcmp(info.img_lib_dir, fake_state_dir), "img_lib_dir '%s'",
           info.img_lib_dir);
    EXPECT(strstr(info.anka_image_tool, "fake-anka/anka_image") != NULL,
           "anka_image '%s'", info.anka_image_tool);
}

static void test_anka_cli_reports_anka_error(const char *dir) {
    fake_anka_reset(dir);
    setenv("FAKE_ANKA_FAIL", "show", 1);
    anka_vm_info info;
    char err[512] = "";
    EXPECT(anka_cli_vm_info("myvm", &info, err, sizeof(err)) == -1,
           "failing show is an error");
    EXPECT(!strcmp(err, "anka -j show myvm: fake show failure"), "err '%s'", err);
}

static void test_anka_cli_tag_exists_and_dirty_count(const char *dir) {
    fake_anka_reset(dir);
    write_fake_state("tags", "base\nother\n");
    anka_vm_info info;
    char err[512] = "";
    EXPECT(anka_cli_vm_info("myvm", &info, err, sizeof(err)) == 0, "info: %s", err);
    int exists = -1;
    EXPECT(anka_cli_tag_exists(&info, "other", &exists, err, sizeof(err)) == 0 &&
           exists == 1, "tag 'other' exists: %s", err);
    EXPECT(anka_cli_tag_exists(&info, "nope", &exists, err, sizeof(err)) == 0 &&
           exists == 0, "tag 'nope' does not exist");

    long dirty_count = -1;
    EXPECT(anka_cli_dirty_count(&info, &dirty_count, err, sizeof(err)) == 0 &&
           dirty_count == 0, "clean disk: %ld %s", dirty_count, err);
    setenv("FAKE_ANKA_DIRTY", "3", 1);
    EXPECT(anka_cli_dirty_count(&info, &dirty_count, err, sizeof(err)) == 0 &&
           dirty_count == 3, "dirty disk: %ld %s", dirty_count, err);
}

static void test_anka_cli_import_and_commit(const char *dir) {
    fake_anka_reset(dir);
    char layer[512], err[512] = "", contents[64];
    snprintf(layer, sizeof(layer), "%s/import-input.tar", dir);
    EXPECT(write_host_file(layer, "layer bytes") == 0, "write layer");
    anka_vm_info info;
    EXPECT(anka_cli_vm_info("myvm", &info, err, sizeof(err)) == 0, "info: %s", err);

    int layer_fd = open(layer, O_RDONLY);
    EXPECT(anka_cli_import(&info, layer_fd, err, sizeof(err)) == 0,
           "import: %s", err);
    close(layer_fd);
    EXPECT(read_fake_state("import.tar", contents, sizeof(contents)) == 0 &&
           !strcmp(contents, "layer bytes"), "import got the layer on stdin");

    EXPECT(anka_cli_commit(&info, "next", err, sizeof(err)) == 0,
           "commit: %s", err);
    EXPECT(read_fake_state("current_tag", contents, sizeof(contents)) == 0 &&
           !strcmp(contents, "next\n"), "commit made tag 'next'");
    EXPECT(anka_cli_commit(&info, "next", err, sizeof(err)) == -1 &&
           strstr(err, "tag already exists"), "second commit refused: %s", err);
}

static void test_anka_cli_export_streams_layer(const char *dir) {
    fake_anka_reset(dir);
    char layer[512], err[512] = "";
    snprintf(layer, sizeof(layer), "%s/export-layer.tar.gz", dir);
    const raw_member_spec specs[] = {
        {.name = "a.txt", .typeflag = '0', .devmajor = 1, .body = "a"},
        {.name = "b.txt", .typeflag = '0', .devmajor = 2, .body = "b"},
    };
    EXPECT(build_raw_archive(layer, specs, 2) == 0, "build layer");
    setenv("FAKE_ANKA_LAYER", layer, 1);
    anka_vm_info info;
    EXPECT(anka_cli_vm_info("myvm", &info, err, sizeof(err)) == 0, "info: %s", err);

    proc_stream stream;
    EXPECT(anka_cli_export_start(&info, &stream, err, sizeof(err)) == 0,
           "export start: %s", err);
    tar_t *in = tar_rdopen(stream.stdout_fd);
    tar_entry e;
    int members = 0;
    while (in && tar_read_next(in, &e) == 1)
        members++;
    if (in)
        tar_close(in);
    EXPECT(members == 2, "streamed %d members", members);
    EXPECT(anka_cli_export_finish(&stream, err, sizeof(err)) == 0,
           "export finish: %s", err);

    setenv("FAKE_ANKA_FAIL", "export", 1);
    EXPECT(anka_cli_export_start(&info, &stream, err, sizeof(err)) == 0,
           "failing export starts");
    close(stream.stdout_fd);
    EXPECT(anka_cli_export_finish(&stream, err, sizeof(err)) == -1 &&
           !strcmp(err, "anka export: fake export failure"), "err '%s'", err);
}

/* ---- VM store, driven by tests/fake-anka ---- */

/* Layer of tag "base": added.txt (kind 1) and changed.txt (kind 2). */
static void use_fake_layer(const char *dir) {
    static char layer[512];
    snprintf(layer, sizeof(layer), "%s/vm-layer.tar.gz", dir);
    const raw_member_spec specs[] = {
        {.name = "added.txt", .typeflag = '0', .devmajor = 1, .body = "a"},
        {.name = "changed.txt", .typeflag = '0', .devmajor = 2, .body = "c"},
    };
    EXPECT(build_raw_archive(layer, specs, 2) == 0, "build vm layer");
    setenv("FAKE_ANKA_LAYER", layer, 1);
}

/* A loaded VM store for "myvm" in a one-slot model, or NULL. */
static le_store *open_loaded_vm_store(const char *dir, le_model **m) {
    char err[512] = "";
    use_fake_layer(dir);
    le_store *store = vm_store_new("myvm", NULL, err, sizeof(err));
    EXPECT(store != NULL, "vm store: %s", err);
    *m = model_new(1);
    if (store)
        EXPECT(store_load(store, *m, 0, err, sizeof(err)) == 0, "load: %s", err);
    return store;
}

static void test_vm_store_refuses_running_vm_and_wrong_tag(const char *dir) {
    char err[512] = "";
    fake_anka_reset(dir);
    write_fake_state("status", "running");
    EXPECT(vm_store_new("myvm", NULL, err, sizeof(err)) == NULL &&
           strstr(err, "stop the VM first"), "running VM refused: %s", err);

    fake_anka_reset(dir);
    EXPECT(vm_store_new("myvm", "other", err, sizeof(err)) == NULL &&
           strstr(err, "current OCI tag is 'base'"), "wrong tag refused: %s", err);
    le_store *store = vm_store_new("myvm", "base", err, sizeof(err));
    EXPECT(store != NULL, "matching tag accepted: %s", err);
    store_free(store);
}

static void test_vm_store_load_and_may_remove(const char *dir) {
    fake_anka_reset(dir);
    le_model *m;
    le_store *store = open_loaded_vm_store(dir, &m);
    char err[512] = "";
    le_node *added = model_lookup(m, "added.txt");
    le_node *changed = model_lookup(m, "changed.txt");
    EXPECT(added && changed, "layer loaded");
    if (store && added && changed) {
        EXPECT(store_may_remove(store, added, 0, err, sizeof(err)) == 0,
               "added entry may go");
        EXPECT(store_may_remove(store, changed, 0, err, sizeof(err)) == -1 &&
               strstr(err, "base version"), "changed entry blocked: %s", err);
    }
    store_free(store);
    model_free(m);
}

static void test_vm_store_save_questions(const char *dir) {
    fake_anka_reset(dir);
    write_fake_state("tags", "base\ntaken\n");
    le_model *m;
    le_store *store = open_loaded_vm_store(dir, &m);
    char question[512], prompt[256], default_name[256], err[512] = "";
    if (store) {
        EXPECT(!store_pre_save_question(store, question, sizeof(question)),
               "no question for a clean disk");
        EXPECT(store_save_name_prompt(store, prompt, sizeof(prompt), default_name,
                                      sizeof(default_name)) &&
               !strcmp(default_name, "base-edit"), "default '%s'", default_name);
        EXPECT(store_check_save_name(store, "taken", err, sizeof(err)) == -1 &&
               strstr(err, "OCI tag 'taken' already exists"),
               "existing tag refused: %s", err);
        EXPECT(store_check_save_name(store, "", err, sizeof(err)) == -1,
               "empty tag refused");
        EXPECT(store_check_save_name(store, "fresh", err, sizeof(err)) == 0,
               "new tag accepted: %s", err);
    }
    store_free(store);
    model_free(m);

    setenv("FAKE_ANKA_DIRTY", "2", 1);
    store = open_loaded_vm_store(dir, &m);
    EXPECT(store && store_pre_save_question(store, question, sizeof(question)) &&
           strstr(question, "2 changes"), "dirty question: %s", question);
    store_free(store);
    model_free(m);
}

static void test_vm_store_save_imports_commits_and_reloads(const char *dir) {
    fake_anka_reset(dir);
    le_model *m;
    le_store *store = open_loaded_vm_store(dir, &m);
    char err[512] = "", contents[64], import_tar[600];
    if (!store) {
        model_free(m);
        return;
    }
    model_toggle_mark(m, model_lookup(m, "added.txt"), 0);
    EXPECT(store_save(store, m, 0, "fresh", err, sizeof(err)) == 0,
           "save: %s", err);
    snprintf(import_tar, sizeof(import_tar), "%s/import.tar", fake_state_dir);
    written_member members[4];
    int count = read_written_members(import_tar, members, 4);
    EXPECT(count == 1 && !strcmp(members[0].path, ".wh.added.txt"),
           "import got the whiteout (%d members)", count);
    EXPECT(read_fake_state("current_tag", contents, sizeof(contents)) == 0 &&
           !strcmp(contents, "fresh\n"), "tag 'fresh' committed");
    EXPECT(m->arch_marked[0] == 0 && model_lookup(m, "added.txt"),
           "new tag loaded with no marks");
    EXPECT(strstr(store_label(store), "fresh") != NULL, "label '%s'",
           store_label(store));
    store_free(store);
    model_free(m);
}

static void test_vm_store_save_failures(const char *dir) {
    fake_anka_reset(dir);
    le_model *m;
    le_store *store = open_loaded_vm_store(dir, &m);
    char err[512] = "";
    if (!store) {
        model_free(m);
        return;
    }
    model_toggle_mark(m, model_lookup(m, "added.txt"), 0);

    setenv("FAKE_ANKA_FAIL", "import", 1);
    EXPECT(store_save(store, m, 0, "fresh", err, sizeof(err)) == -1 &&
           strstr(err, "partly changed"), "import failure: %s", err);
    EXPECT(m->arch_marked[0] == 1, "edits kept after import failure");

    setenv("FAKE_ANKA_FAIL", "push", 1);
    EXPECT(store_save(store, m, 0, "fresh", err, sizeof(err)) == -1 &&
           strstr(err, "anka push -l -t fresh myvm"), "commit failure: %s", err);

    unsetenv("FAKE_ANKA_FAIL");
    write_fake_state("status", "running");
    EXPECT(store_save(store, m, 0, "fresh", err, sizeof(err)) == -1 &&
           strstr(err, "stop the VM first"), "running VM at save: %s", err);
    store_free(store);
    model_free(m);
}

static void test_proc_reports_missing_command(void) {
    const char *argv[] = {"le-no-such-command", NULL};
    proc_spec spec = {.argv = argv, .stdin_fd = -1};
    char *stdout_text, *stderr_text, err[256] = "";
    EXPECT(proc_run(&spec, &stdout_text, &stderr_text, err, sizeof(err)) == -1,
           "missing command is an error");
    EXPECT(strstr(err, "le-no-such-command") != NULL, "err '%s'", err);
    free(stdout_text);
    free(stderr_text);
}

int main(void) {
    char *dir = make_tmpdir();
    if (!dir) {
        fprintf(stderr, "mkdtemp failed\n");
        return 1;
    }

    test_load_layer_from_pipe(dir);
    test_clear_slot_keeps_other_slots(dir);
    test_change_set_may_remove();
    test_change_set_write_order(dir);
    test_change_set_write_refuses_base_version(dir);
    test_proc_reports_missing_command();
    put_fakes_on_path();
    test_anka_cli_vm_info(dir);
    test_anka_cli_reports_anka_error(dir);
    test_anka_cli_tag_exists_and_dirty_count(dir);
    test_anka_cli_import_and_commit(dir);
    test_anka_cli_export_streams_layer(dir);
    test_vm_store_refuses_running_vm_and_wrong_tag(dir);
    test_vm_store_load_and_may_remove(dir);
    test_vm_store_save_questions(dir);
    test_vm_store_save_imports_commits_and_reloads(dir);
    test_vm_store_save_failures(dir);

    if (test_failures) {
        fprintf(stderr, "%d failure(s)  (tmpdir %s)\n", test_failures, dir);
        free(dir);
        return 1;
    }
    printf("ok\n");
    free(dir);
    return 0;
}
