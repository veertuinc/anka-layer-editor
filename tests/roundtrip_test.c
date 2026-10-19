/*
 * Headless round-trip tests for archive load/save and model edit semantics.
 * Links against tar/tree/archive (no TUI).
 */
#include "test_support.h"
#include "../src/archive.h"
#include "../src/session.h"
#include "../src/tar.h"
#include "../src/tree.h"
#include "../src/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <zlib.h>

static void test_save_preserves_change_kind(const char *dir) {
    char archive[512];
    snprintf(archive, sizeof(archive), "%s/kind.tar.gz", dir);
    const raw_member_spec specs[] = {
        {"new.txt", '0', NULL, 1, "new", 0, 0},
        {"changed.txt", '0', NULL, 2, "changed", 0, 0},
        {".wh.deleted.txt", '0', NULL, 4, NULL, 0, 0},
        {"drop.txt", '0', NULL, 1, "drop", 0, 0},
    };
    EXPECT(build_raw_archive(archive, specs, 4) == 0, "build");
    le_model *m = load_single(archive);
    remove_and_save(m, archive, "drop.txt");

    raw_member_found found;
    EXPECT(raw_find_member(archive, "new.txt", &found) == 1 &&
           found.devmajor == 1, "new.txt keeps kind 1, got %lu", found.devmajor);
    EXPECT(raw_find_member(archive, "changed.txt", &found) == 1 &&
           found.devmajor == 2, "changed.txt keeps kind 2, got %lu", found.devmajor);
    EXPECT(raw_find_member(archive, ".wh.deleted.txt", &found) == 1 &&
           found.devmajor == 4, "whiteout keeps kind 4, got %lu", found.devmajor);
    model_free(m);
}

static void test_save_preserves_hardlink(const char *dir) {
    char archive[512];
    snprintf(archive, sizeof(archive), "%s/hardlink.tar.gz", dir);
    const raw_member_spec specs[] = {
        {"target.bin", '0', NULL, 0, "payload", 0, 0},
        {"hardlink.bin", '1', "target.bin", 0, NULL, 0, 0},
        {"drop.txt", '0', NULL, 0, "drop", 0, 0},
    };
    EXPECT(build_raw_archive(archive, specs, 3) == 0, "build");
    le_model *m = load_single(archive);
    remove_and_save(m, archive, "drop.txt");

    raw_member_found found;
    EXPECT(raw_find_member(archive, "hardlink.bin", &found) == 1, "hardlink kept");
    EXPECT(found.typeflag == '1', "hardlink typeflag want '1' got '%c'",
           found.typeflag);
    EXPECT(!strcmp(found.linkname, "target.bin"), "hardlink target '%s'",
           found.linkname);
    model_free(m);
}

static void test_save_rejects_sized_unsupported_type(const char *dir) {
    char archive[512], err[256];
    snprintf(archive, sizeof(archive), "%s/sparse.tar.gz", dir);
    const raw_member_spec specs[] = {
        {"sparse.bin", 'S', NULL, 0, "sparse-data", 0, 0},
        {"drop.txt", '0', NULL, 0, "drop", 0, 0},
    };
    EXPECT(build_raw_archive(archive, specs, 2) == 0, "build");
    struct stat before;
    stat(archive, &before);

    le_model *m = load_single(archive);
    model_toggle_removed(m, model_lookup(m, "drop.txt"), 0);
    EXPECT(archive_save(archive, m, 0, err, sizeof(err)) == -1,
           "save must fail closed on a sized entry it cannot rewrite");
    struct stat after;
    stat(archive, &after);
    EXPECT(before.st_size == after.st_size && before.st_ino == after.st_ino,
           "original archive untouched");
    model_free(m);
}

static void test_save_preserves_pax_records(const char *dir) {
    char archive[512];
    snprintf(archive, sizeof(archive), "%s/pax.tar.gz", dir);
    gzFile gz = gzopen(archive, "wb");
    const char *global_records[] = {"SCHILY.xattr.user.global=g"};
    const char *local_records[] = {"LIBARCHIVE.creationtime=1600000000",
                                   "mtime=1700000000.25"};
    const raw_member_spec kept = {"kept.txt", '0', NULL, 0, "kept", 0, 0};
    const raw_member_spec drop = {"drop.txt", '0', NULL, 0, "drop", 0, 0};
    EXPECT(raw_write_pax(gz, 'g', global_records, 1) == 0, "global pax");
    EXPECT(raw_write_pax(gz, 'x', local_records, 2) == 0, "local pax");
    EXPECT(raw_write_member(gz, &kept) == 0, "kept");
    EXPECT(raw_write_member(gz, &drop) == 0, "drop");
    EXPECT(raw_finish(gz) == 0, "finish");

    le_model *m = load_single(archive);
    remove_and_save(m, archive, "drop.txt");

    raw_member_found found;
    EXPECT(raw_find_member(archive, "kept.txt", &found) == 1, "kept present");
    EXPECT(strstr(found.local_pax, "LIBARCHIVE.creationtime=1600000000\n") != NULL,
           "creationtime kept, pax was '%s'", found.local_pax);
    EXPECT(strstr(found.local_pax, "SCHILY.xattr.user.global=g\n") != NULL,
           "global record applied, pax was '%s'", found.local_pax);
    EXPECT(strstr(found.local_pax, "mtime=1700000000.25") != NULL,
           "sub-second mtime kept, pax was '%s'", found.local_pax);
    model_free(m);
}

static void test_save_preserves_archive_mode(const char *dir) {
    char archive[512], host[512];
    snprintf(archive, sizeof(archive), "%s/mode.tar.gz", dir);
    snprintf(host, sizeof(host), "%s/mode.txt", dir);
    EXPECT(write_host_file(host, "mode") == 0, "write");
    const char *members[] = {"keep.txt", "drop.txt"};
    EXPECT(build_archive(archive, members, 2, host) == 0, "build");
    EXPECT(chmod(archive, 0600) == 0, "chmod");

    le_model *m = load_single(archive);
    remove_and_save(m, archive, "drop.txt");
    struct stat st;
    EXPECT(stat(archive, &st) == 0 && (st.st_mode & 07777) == 0600,
           "mode want 0600 got %o", st.st_mode & 07777);
    model_free(m);
}

static void test_add_rejects_invalid_destinations(const char *dir) {
    char archive[512], host[512];
    snprintf(archive, sizeof(archive), "%s/dest.tar.gz", dir);
    snprintf(host, sizeof(host), "%s/dest.txt", dir);
    EXPECT(write_host_file(host, "dest") == 0, "write");
    const raw_member_spec specs[] = {
        {"dir", '5', NULL, 0, NULL, 0, 0},
        {"dir/child.txt", '0', NULL, 0, "child", 0, 0},
        {"file.txt", '0', NULL, 0, "file", 0, 0},
    };
    EXPECT(build_raw_archive(archive, specs, 3) == 0, "build");
    le_model *m = load_single(archive);

    const char *invalid[] = {"dir", "file.txt/inside", "a/../b", "../escape",
                             "a/./b", "dir/.."};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        EXPECT(stage_host(m, invalid[i], 0, host) == NULL,
               "destination '%s' must be rejected", invalid[i]);
    EXPECT(stage_host(m, ".wh.sneaky", 0, host) == NULL,
           "whiteout names are not host additions");
    EXPECT(m->added == 0 && model_lookup(m, "a") == NULL,
           "rejected adds leave no trace in the model");
    EXPECT(stage_host(m, "dir/new.txt", 0, host) != NULL,
           "a new file inside an existing directory is fine");

    model_toggle_removed(m, model_lookup(m, "dir"), 0);
    EXPECT(stage_host(m, "dir/other.txt", 0, host) == NULL,
           "adding under a path marked for removal is rejected");
    model_free(m);
}

static void expect_raw_member(const char *archive, const char *name,
                              char typeflag, unsigned long devmajor) {
    raw_member_found found;
    memset(&found, 0, sizeof(found));
    EXPECT(raw_find_member(archive, name, &found) == 1, "%s present", name);
    EXPECT(found.typeflag == typeflag, "%s typeflag want '%c' got '%c'", name,
           typeflag, found.typeflag);
    EXPECT(found.devmajor == devmajor, "%s devmajor want %lu got %lu", name,
           devmajor, found.devmajor);
}

static void test_whiteout_staging(const char *dir) {
    char archive[512], err[256];
    snprintf(archive, sizeof(archive), "%s/whiteout.tar.gz", dir);
    const raw_member_spec specs[] = {
        {"Users/anka", '5', NULL, 0, NULL, 501, 20},
        {"Users/anka/keep.txt", '0', NULL, 1, "keep", 501, 20},
        {"Users/anka/tool", '0', NULL, 1, "tool", 501, 20},
    };
    EXPECT(build_raw_archive(archive, specs, 3) == 0, "build");
    le_model *m = load_single(archive);

    le_node *tool_whiteout = model_stage_whiteout(m, "Users/anka/tool", 0, 0,
                                                  err, sizeof(err));
    EXPECT(tool_whiteout != NULL, "stage whiteout for a layer file: %s", err);
    EXPECT(tool_whiteout && node_is_whiteout(tool_whiteout) &&
           !strcmp(node_whiteout_target(tool_whiteout), "tool"),
           "whiteout node names its target");
    EXPECT(model_lookup(m, "Users/anka/tool")->arch[0].removed,
           "the whited-out layer file is marked for removal");
    EXPECT(model_stage_whiteout(m, "Users/anka/base-only.txt", 0, 0, err,
                                sizeof(err)) != NULL,
           "stage whiteout for a base-only path: %s", err);
    EXPECT(model_stage_whiteout(m, "/Users/anka/olddir", 0, 1, err,
                                sizeof(err)) != NULL,
           "stage directory whiteout: %s", err);

    EXPECT(model_stage_whiteout(m, "Users/anka/tool", 0, 0, err, sizeof(err)) == NULL,
           "staging the same whiteout twice is rejected");
    EXPECT(model_stage_whiteout(m, "Users/anka/.wh.tool", 0, 0, err,
                                sizeof(err)) == NULL,
           "a whiteout of a whiteout is rejected");
    EXPECT(model_stage_whiteout(m, "Users/../etc", 0, 0, err, sizeof(err)) == NULL,
           "'..' is rejected for whiteouts");

    EXPECT(archive_save(archive, m, 0, err, sizeof(err)) == 0, "save: %s", err);
    expect_raw_member(archive, "Users/anka/.wh.tool", '0', LE_CHANGE_WHITEOUT);
    expect_raw_member(archive, "Users/anka/.wh.base-only.txt", '0',
                      LE_CHANGE_WHITEOUT);
    expect_raw_member(archive, "Users/anka/.wh.olddir", '5', LE_CHANGE_WHITEOUT);
    expect_raw_member(archive, "Users/anka/keep.txt", '0', LE_CHANGE_ADDED);
    EXPECT(count_path_occurrences(archive, "Users/anka/tool") == 0,
           "whited-out file left the layer");

    le_node *saved = model_lookup(m, "Users/anka/.wh.olddir");
    EXPECT(saved && saved->arch[0].present &&
           saved->arch[0].change_kind == LE_CHANGE_WHITEOUT,
           "saved whiteout is a tagged member");
    model_free(m);

    le_model *reloaded = load_single(archive);
    le_node *loaded = model_lookup(reloaded, "Users/anka/.wh.base-only.txt");
    EXPECT(loaded && node_is_whiteout(loaded) &&
           loaded->arch[0].change_kind == LE_CHANGE_WHITEOUT,
           "loaded whiteout keeps its kind");
    model_free(reloaded);
}

static void test_unstage_restores_and_prunes(const char *dir) {
    char archive[512], host[512], err[256];
    snprintf(archive, sizeof(archive), "%s/unstage.tar.gz", dir);
    snprintf(host, sizeof(host), "%s/unstage-host.txt", dir);
    EXPECT(write_host_file(host, "a much longer replacement body") == 0, "write");
    const raw_member_spec specs[] = {{"a/orig.txt", '0', NULL, 0, "orig", 0, 0}};
    EXPECT(build_raw_archive(archive, specs, 1) == 0, "build");
    le_model *m = load_single(archive);
    size_t total_before = m->total;

    le_node *orig = stage_host(m, "a/orig.txt", 0, host);
    EXPECT(orig && node_display_meta(orig, 1)->size == 30, "shows staged size");
    EXPECT(model_toggle_mark(m, orig, 0) == LE_MARK_UNSTAGED, "space unstages");
    orig = model_lookup(m, "a/orig.txt");
    EXPECT(orig && node_display_meta(orig, 1)->size == 4 && !orig->arch[0].removed,
           "original entry is back and not marked");
    EXPECT(m->added == 0 && !model_dirty(m), "clean after unstage");

    le_node *deep = stage_host(m, "new/deep/file.txt", 0, host);
    EXPECT(deep != NULL, "stage deep");
    EXPECT(model_toggle_mark(m, deep, 0) == LE_MARK_UNSTAGED, "unstage deep");
    EXPECT(model_lookup(m, "new") == NULL, "parents created for it are gone");

    le_node *whiteout = model_stage_whiteout(m, "a/gone", 0, 0, err, sizeof(err));
    EXPECT(whiteout != NULL, "stage whiteout: %s", err);
    EXPECT(model_toggle_mark(m, whiteout, 0) == LE_MARK_UNSTAGED,
           "space unstages a whiteout");
    EXPECT(model_lookup(m, "a/.wh.gone") == NULL, "whiteout node gone");
    EXPECT(m->total == total_before, "total back to %zu, got %zu", total_before,
           m->total);

    whiteout = model_stage_whiteout(m, "a/orig.txt", 0, 0, err, sizeof(err));
    EXPECT(whiteout && model_lookup(m, "a/orig.txt")->arch[0].removed,
           "whiteout of a layer file marks it: %s", err);
    EXPECT(model_toggle_mark(m, whiteout, 0) == LE_MARK_UNSTAGED,
           "space unstages the whiteout");
    EXPECT(!model_lookup(m, "a/orig.txt")->arch[0].removed && m->marked == 0,
           "undoing the whiteout unmarks the file it marked");

    model_toggle_removed(m, model_lookup(m, "a/orig.txt"), 0);
    whiteout = model_stage_whiteout(m, "a/orig.txt", 0, 0, err, sizeof(err));
    EXPECT(whiteout != NULL, "whiteout of a marked file: %s", err);
    model_unstage(m, whiteout, 0);
    EXPECT(model_lookup(m, "a/orig.txt")->arch[0].removed,
           "a mark the user set before the whiteout stays");
    model_toggle_removed(m, model_lookup(m, "a/orig.txt"), 0);

    model_toggle_removed(m, model_lookup(m, "a/orig.txt"), 0);
    model_toggle_removed(m, model_lookup(m, "a/orig.txt"), 0);
    EXPECT(!model_dirty(m), "mark then unmark is clean");
    model_free(m);
}

static void test_added_entries_change_kind(const char *dir) {
    char archive[512], host[512], err[256];
    snprintf(archive, sizeof(archive), "%s/addkind.tar.gz", dir);
    snprintf(host, sizeof(host), "%s/addkind.txt", dir);
    EXPECT(write_host_file(host, "replacement") == 0, "write");
    const raw_member_spec specs[] = {{"changed.txt", '0', NULL, 2, "changed", 0, 0}};
    EXPECT(build_raw_archive(archive, specs, 1) == 0, "build");
    le_model *m = load_single(archive);
    EXPECT(stage_host(m, "changed.txt", 0, host) != NULL, "replace");
    EXPECT(stage_host(m, "brand-new.txt", 0, host) != NULL, "new");
    EXPECT(archive_save(archive, m, 0, err, sizeof(err)) == 0, "save: %s", err);
    expect_raw_member(archive, "changed.txt", '0', LE_CHANGE_MODIFIED);
    expect_raw_member(archive, "brand-new.txt", '0', LE_CHANGE_UNTAGGED);
    model_free(m);
}

static void test_default_owner(const char *dir) {
    char archive[512];
    snprintf(archive, sizeof(archive), "%s/owner.tar.gz", dir);
    const raw_member_spec specs[] = {
        {"Users/anka", '5', NULL, 0, NULL, 501, 20},
        {"Users/anka/Library/file", '0', NULL, 0, "f", 501, 20},
    };
    EXPECT(build_raw_archive(archive, specs, 2) == 0, "build");
    le_model *m = load_single(archive);
    uid_t uid;
    gid_t gid;
    model_default_owner(m, "Users/anka/Library/new.txt", &uid, &gid);
    EXPECT(uid == 501 && gid == 20, "inherits nearest entry owner, got %u:%u",
           (unsigned)uid, (unsigned)gid);
    model_default_owner(m, "opt/tool", &uid, &gid);
    EXPECT(uid == 0 && gid == 0, "falls back to 0:0, got %u:%u", (unsigned)uid,
           (unsigned)gid);
    model_free(m);
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
    EXPECT(stage_host(m, "new/file.txt", 0, host2) != NULL, "add host");
    EXPECT(archive_save(archive, m, 0, err, sizeof(err)) == 0, "save1: %s", err);
    EXPECT(!model_dirty(m), "dirty cleared");
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
    EXPECT(stage_host(m, "path/file.txt", 0, replacement) != NULL, "replace add");
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
    EXPECT(!model_dirty(m), "dirty cleared");
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
    EXPECT(stage_host(m, longpath, 0, host) != NULL, "add long path");
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
    EXPECT(stage_host(m, "link", 0, linkpath) != NULL, "add symlink");
    le_node *n = model_lookup(m, "link");
    const le_meta *shown = n ? node_display_meta(n, 1) : NULL;
    EXPECT(shown && shown->link && !strcmp(shown->link, "target.txt"),
           "link target in model");
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
    EXPECT(model_arch_dirty(m, 0), "first archive dirty");
    EXPECT(!model_arch_dirty(m, 1), "second archive untouched");
    EXPECT(model_dirty(m), "session dirty");

    /* Saving the untouched archive too must not carry the removal across. */
    EXPECT(archive_save(a, m, 0, err, sizeof(err)) == 0, "save a: %s", err);
    EXPECT(archive_save(b, m, 1, err, sizeof(err)) == 0, "save b: %s", err);
    EXPECT(count_path_occurrences(a, "shared.txt") == 0, "dropped from a");
    EXPECT(count_path_occurrences(b, "shared.txt") == 1, "kept in b");

    le_node *still = model_lookup(m, "shared.txt");
    EXPECT(still != NULL, "path survives for the other archive");
    EXPECT(still && !node_in_archive(still, 0) && node_in_archive(still, 1),
           "membership now only the second archive");
    EXPECT(!model_dirty(m), "no dirty archives left");
    model_free(m);
}

static void test_multi_remove_from_all_archives(const char *dir) {
    char a[512], b[512], err[256];
    snprintf(a, sizeof(a), "%s/all-a.tar.gz", dir);
    snprintf(b, sizeof(b), "%s/all-b.tar.gz", dir);
    le_model *m = load_pair(dir, a, b);

    model_toggle_removed_all(m, model_lookup(m, "shared.txt"));
    EXPECT(model_arch_dirty(m, 0) && model_arch_dirty(m, 1), "both archives dirty");
    EXPECT(m->marked == 2, "marked counted per archive, got %zu", m->marked);

    EXPECT(archive_save(a, m, 0, err, sizeof(err)) == 0, "save a: %s", err);
    EXPECT(archive_save(b, m, 1, err, sizeof(err)) == 0, "save b: %s", err);
    EXPECT(count_path_occurrences(a, "shared.txt") == 0, "dropped from a");
    EXPECT(count_path_occurrences(b, "shared.txt") == 0, "dropped from b");
    EXPECT(model_lookup(m, "shared.txt") == NULL, "dropped from the model");
    EXPECT(model_lookup(m, "only-a.txt") != NULL, "unrelated path kept");
    EXPECT(m->marked == 0 && !model_dirty(m), "counters cleared");
    model_free(m);
}

static void test_multi_add_targets_one_archive(const char *dir) {
    char a[512], b[512], host[512], err[256];
    snprintf(a, sizeof(a), "%s/add-a.tar.gz", dir);
    snprintf(b, sizeof(b), "%s/add-b.tar.gz", dir);
    snprintf(host, sizeof(host), "%s/add-body.txt", dir);
    EXPECT(write_host_file(host, "new-body") == 0, "write host");
    le_model *m = load_pair(dir, a, b);

    EXPECT(stage_host(m, "extra/new.txt", 1, host) != NULL,
           "stage into second archive");
    EXPECT(!model_arch_dirty(m, 0) && model_arch_dirty(m, 1),
           "only the target archive is dirty");

    EXPECT(archive_save(b, m, 1, err, sizeof(err)) == 0, "save b: %s", err);
    EXPECT(count_path_occurrences(b, "extra/new.txt") == 1, "added to b");
    EXPECT(count_path_occurrences(a, "extra/new.txt") == 0, "absent from a");

    le_node *added = model_lookup(m, "extra/new.txt");
    EXPECT(added && node_in_archive(added, 1) && !node_in_archive(added, 0),
           "member of the second archive only");
    EXPECT(added && added->arch[1].staged == NULL, "staged flag cleared after save");
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
    session_free(&s);
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
    test_save_preserves_change_kind(dir);
    test_save_preserves_hardlink(dir);
    test_save_rejects_sized_unsupported_type(dir);
    test_save_preserves_pax_records(dir);
    test_save_preserves_archive_mode(dir);
    test_add_rejects_invalid_destinations(dir);
    test_whiteout_staging(dir);
    test_unstage_restores_and_prunes(dir);
    test_added_entries_change_kind(dir);
    test_default_owner(dir);

    /* Best-effort cleanup of temp dir contents is optional; leave for OS. */
    if (test_failures) {
        fprintf(stderr, "%d failure(s)  (tmpdir %s)\n", test_failures, dir);
        free(dir);
        return 1;
    }
    printf("ok\n");
    free(dir);
    return 0;
}
