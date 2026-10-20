/*
 * Headless tests for file preview: member bytes, binary detection, wrap, search.
 */
#include "test_support.h"
#include "../src/archive.h"
#include "../src/archive_store.h"
#include "../src/member_read.h"
#include "../src/preview.h"
#include "../src/unified.h"
#include "../src/store.h"
#include "../src/tar.h"
#include "../src/tree.h"
#include "../src/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_read_archive_member(const char *dir) {
    char archive[512], err[256] = "";
    snprintf(archive, sizeof(archive), "%s/preview.tar.gz", dir);
    const raw_member_spec specs[] = {
        {"usr/local/README", '0', NULL, 1, "hello from the layer\n", 0, 0},
        {"usr/local/empty", '0', NULL, 1, "", 0, 0},
        {"usr/local/bin", '5', NULL, 1, NULL, 0, 0},
    };
    EXPECT(build_raw_archive(archive, specs, 3) == 0, "build");

    le_member_bytes bytes = {0};
    EXPECT(member_read_from_archive(archive, "usr/local/README",
                                    MEMBER_PREVIEW_MAX, &bytes, err,
                                    sizeof(err)) == 0,
           "read README: %s", err);
    EXPECT(bytes.len == 21 && !strcmp(bytes.data, "hello from the layer\n"),
           "README body '%s' len %zu", bytes.data ? bytes.data : "", bytes.len);
    EXPECT(!bytes.truncated, "README not truncated");
    EXPECT(!member_bytes_is_binary(&bytes), "text is not binary");
    member_bytes_free(&bytes);

    EXPECT(member_read_from_archive(archive, "usr/local/empty",
                                    MEMBER_PREVIEW_MAX, &bytes, err,
                                    sizeof(err)) == 0,
           "read empty: %s", err);
    EXPECT(bytes.len == 0 && bytes.data && bytes.data[0] == '\0',
           "empty file is a zero-length buffer");
    member_bytes_free(&bytes);

    EXPECT(member_read_from_archive(archive, "usr/local/bin", MEMBER_PREVIEW_MAX,
                                    &bytes, err, sizeof(err)) == -1 &&
           strstr(err, "not a regular file"),
           "directory refused: %s", err);

    EXPECT(member_read_from_archive(archive, "no/such", MEMBER_PREVIEW_MAX,
                                    &bytes, err, sizeof(err)) == -1 &&
           strstr(err, "not in the archive"),
           "missing path: %s", err);
}

static void test_read_truncated_and_binary(const char *dir) {
    char archive[512], host[512], err[256] = "";
    snprintf(archive, sizeof(archive), "%s/cap.tar.gz", dir);
    const raw_member_spec specs[] = {
        {"big.txt", '0', NULL, 1, "abcdefghij", 0, 0},
    };
    EXPECT(build_raw_archive(archive, specs, 1) == 0, "build cap");

    le_member_bytes bytes = {0};
    EXPECT(member_read_from_archive(archive, "big.txt", 4, &bytes, err,
                                    sizeof(err)) == 0,
           "capped read: %s", err);
    EXPECT(bytes.len == 4 && !memcmp(bytes.data, "abcd", 4) && bytes.truncated,
           "first 4 bytes, truncated flag");
    member_bytes_free(&bytes);

    EXPECT(member_read_from_archive(archive, "big.txt", 10, &bytes, err,
                                    sizeof(err)) == 0,
           "exact size: %s", err);
    EXPECT(bytes.len == 10 && !bytes.truncated, "exact size is not truncated");
    member_bytes_free(&bytes);

    snprintf(host, sizeof(host), "%s/nul.bin", dir);
    FILE *f = fopen(host, "wb");
    EXPECT(f && fwrite("aa\0bb", 1, 5, f) == 5, "write NUL file");
    if (f)
        fclose(f);
    EXPECT(member_read_from_host(host, MEMBER_PREVIEW_MAX, &bytes, err,
                                 sizeof(err)) == 0,
           "nul host: %s", err);
    EXPECT(bytes.len == 5 && member_bytes_is_binary(&bytes),
           "NUL in first 512 bytes is binary");
    member_bytes_free(&bytes);
}

static void test_read_host_file(const char *dir) {
    char host[512], err[256] = "";
    snprintf(host, sizeof(host), "%s/staged.txt", dir);
    EXPECT(write_host_file(host, "from the host") == 0, "write host");

    le_member_bytes bytes = {0};
    EXPECT(member_read_from_host(host, MEMBER_PREVIEW_MAX, &bytes, err,
                                 sizeof(err)) == 0,
           "host read: %s", err);
    EXPECT(bytes.len == 13 && !strcmp(bytes.data, "from the host"),
           "host body '%s'", bytes.data ? bytes.data : "");
    member_bytes_free(&bytes);

    EXPECT(member_read_from_host(host, 4, &bytes, err, sizeof(err)) == 0 &&
           bytes.truncated && bytes.len == 4,
           "host cap");
    member_bytes_free(&bytes);

    EXPECT(member_read_from_host("/no/such/host-file", MEMBER_PREVIEW_MAX,
                                 &bytes, err, sizeof(err)) == -1,
           "missing host file");
}

static void test_archive_store_read_member(const char *dir) {
    char archive[512], err[256] = "";
    snprintf(archive, sizeof(archive), "%s/store.tar.gz", dir);
    const raw_member_spec specs[] = {
        {"a.txt", '0', NULL, 1, "store-body", 0, 0},
    };
    EXPECT(build_raw_archive(archive, specs, 1) == 0, "build store");
    le_store *store = archive_store_new(archive);
    EXPECT(store != NULL, "archive store");
    le_member_bytes bytes = {0};
    EXPECT(store_read_member(store, "a.txt", &bytes, err, sizeof(err)) == 0 &&
           bytes.len == 10 && !strcmp(bytes.data, "store-body"),
           "store read: %s", err);
    member_bytes_free(&bytes);
    store_free(store);
}

static void test_preview_put_byte(void) {
    char buf[4];
    EXPECT(preview_put_byte('A', buf) == 1 && buf[0] == 'A', "plain");
    EXPECT(preview_put_byte('\t', buf) == 1 && buf[0] == '\t', "tab stays");
    EXPECT(preview_put_byte('\n', buf) == 0, "newline is a row break");
    EXPECT(preview_put_byte(1, buf) == 2 && buf[0] == '^' && buf[1] == 'A',
           "SOH as ^A");
    EXPECT(preview_put_byte(127, buf) == 2 && buf[0] == '^' && buf[1] == '?',
           "DEL as ^?");
}

static void test_preview_wrap(void) {
    preview_layout lay = {0};
    EXPECT(preview_layout_build(&lay, "abcdefghij", 10, 4) == 0, "build wrap");
    EXPECT(lay.nrows == 3, "3 rows, got %zu", lay.nrows);
    EXPECT(lay.row_off[0] == 0 && lay.row_off[1] == 4 && lay.row_off[2] == 8,
           "row offsets");

    char row[64];
    size_t n = 0;
    int hl_col = 0, hl_n = 0;
    EXPECT(preview_row_render(&lay, 0, 0, 0, 0, row, sizeof(row), &n, &hl_col,
                              &hl_n) == 0 &&
           n == 4 && !memcmp(row, "abcd", 4),
           "row 0");
    EXPECT(preview_row_render(&lay, 2, 0, 0, 0, row, sizeof(row), &n, &hl_col,
                              &hl_n) == 0 &&
           n == 2 && !memcmp(row, "ij", 2),
           "row 2");
    preview_layout_free(&lay);

    EXPECT(preview_layout_build(&lay, "a\x01""b", 3, 80) == 0, "control wrap");
    EXPECT(preview_row_render(&lay, 0, 0, 0, 0, row, sizeof(row), &n, &hl_col,
                              &hl_n) == 0 &&
           n == 4 && !memcmp(row, "a^Ab", 4),
           "control expands on the row");
    preview_layout_free(&lay);

    EXPECT(preview_layout_build(&lay, "", 0, 80) == 0 && lay.nrows == 1,
           "empty file still has one row");
    preview_layout_free(&lay);
}

static void test_preview_search(void) {
    const char *text = "hello\nfoo\nhello again\n";
    preview_view v;
    preview_view_init(&v, text, strlen(text));
    EXPECT(preview_view_set_width(&v, 80) == 0, "width");

    char err[128] = "";
    EXPECT(preview_view_search(&v, "hello", 1, 1, err, sizeof(err)) == 0 &&
           v.has_match && v.match_off == 0 && v.match_len == 5,
           "first hello");
    EXPECT(preview_view_search(&v, "hello", 1, 1, err, sizeof(err)) == 0 &&
           v.match_off == 10,
           "second hello at %zu", v.match_off);
    EXPECT(preview_view_search(&v, "hello", 1, 1, err, sizeof(err)) == 0 &&
           v.match_off == 0,
           "wrap to first hello at %zu", v.match_off);

    EXPECT(preview_view_search(&v, "zzz", 1, 1, err, sizeof(err)) == 1,
           "pattern not found");
    EXPECT(preview_view_search(&v, "[", 1, 1, err, sizeof(err)) == -1 &&
           err[0],
           "bad regex: %s", err);

    preview_view_search(&v, "hello", 1, 1, err, sizeof(err));
    EXPECT(preview_view_search(&v, "hello", 0, 1, err, sizeof(err)) == 0 &&
           v.match_off == 10,
           "backward from after wrap lands on second");
    preview_view_free(&v);
}

static void test_preview_scroll(void) {
    char text[64];
    memset(text, 'x', 20);
    text[4] = '\n';
    text[9] = '\n';
    text[14] = '\n';
    text[19] = '\n';
    preview_view v;
    preview_view_init(&v, text, 20);
    preview_view_set_width(&v, 80);
    EXPECT(v.layout.nrows == 5, "5 lines got %zu", v.layout.nrows);

    preview_view_move_line(&v, 1, 2);
    EXPECT(v.top == 1, "j moves the window one line");
    preview_view_move_line(&v, 10, 2);
    EXPECT(v.top == v.layout.nrows - 2, "clamped to last page, top %zu", v.top);
    preview_view_home(&v);
    EXPECT(v.top == 0, "home");
    preview_view_end(&v, 2);
    EXPECT(v.top == v.layout.nrows - 2, "end top %zu", v.top);
    preview_view_page(&v, -1, 2);
    EXPECT(v.top < v.layout.nrows - 2, "page up moved");
    preview_view_free(&v);
}

static void test_shared_directory_does_not_ask_which_archive(const char *dir) {
    char a_path[512], b_path[512], host[512], err[256] = "";
    snprintf(a_path, sizeof(a_path), "%s/a.tar.gz", dir);
    snprintf(b_path, sizeof(b_path), "%s/b.tar.gz", dir);
    snprintf(host, sizeof(host), "%s/readme.txt", dir);
    EXPECT(write_host_file(host, "readme") == 0, "host");
    const char *guests[] = {"usr/local/README"};
    EXPECT(build_archive(a_path, guests, 1, host) == 0, "build a");
    EXPECT(build_archive(b_path, guests, 1, host) == 0, "build b");

    le_model *m = model_new(2);
    EXPECT(archive_load(a_path, m, 0, err, sizeof(err)) == 0, "load a: %s", err);
    EXPECT(archive_load(b_path, m, 1, err, sizeof(err)) == 0, "load b: %s", err);

    le_node *usr = model_lookup(m, "usr");
    int slots[LE_MAX_ARCHIVES];
    int nslots = usr ? node_archives(usr, 2, slots) : 0;
    EXPECT(usr && nslots == 2, "usr is in both archives");
    EXPECT(!preview_needs_archive_choice(usr, nslots),
           "Enter on a shared directory must not ask which archive");
    const char *reason = preview_block_reason(usr);
    EXPECT(reason && strstr(reason, "directory"),
           "shared directory refuse: %s", reason ? reason : "(null)");

    le_node *file = model_lookup(m, "usr/local/README");
    nslots = file ? node_archives(file, 2, slots) : 0;
    EXPECT(file && nslots == 2, "file is in both archives");
    EXPECT(preview_needs_archive_choice(file, nslots),
           "Enter on a shared file still asks which archive");
    EXPECT(preview_block_reason(file) == NULL, "shared file is openable");
    EXPECT(preview_diff_block(file, nslots) == NULL,
           "two holders may diff");
    EXPECT(preview_diff_block(file, 3) == NULL,
           "three holders may diff (TUI asks which two)");
    model_free(m);
}

static void expect_side_row(const side_row *r, unsigned char kind,
                            const char *left, const char *right,
                            const char *what) {
    EXPECT(r && r->kind == kind, "%s kind %u", what, r ? r->kind : 99);
    EXPECT(r && r->left && !strcmp(r->left, left), "%s left '%s'", what,
           r && r->left ? r->left : "");
    EXPECT(r && r->right && !strcmp(r->right, right), "%s right '%s'", what,
           r && r->right ? r->right : "");
}

static void test_side_one_line_change(void) {
    const char *left = "foo\nbar\nbaz\n";
    const char *right = "foo\nqux\nbaz\n";
    side_diff d = {0};
    EXPECT(side_diff_build(left, strlen(left), right, strlen(right), &d) ==
               SIDE_DIFF_OK &&
           d.nrows == 3,
           "one-line change rows %zu", d.nrows);
    expect_side_row(&d.rows[0], SIDE_EQ, "foo", "foo", "eq foo");
    expect_side_row(&d.rows[1], SIDE_CHG, "bar", "qux", "chg bar/qux");
    expect_side_row(&d.rows[2], SIDE_EQ, "baz", "baz", "eq baz");
    side_diff_free(&d);
}

static void test_side_identical_and_empty(void) {
    side_diff d = {0};
    EXPECT(side_diff_build("x\n", 2, "x\n", 2, &d) == SIDE_DIFF_IDENTICAL &&
           d.rows == NULL,
           "identical");
    EXPECT(side_diff_build("", 0, "hello\n", 6, &d) == SIDE_DIFF_OK &&
           d.nrows == 1,
           "empty vs file rows %zu", d.nrows);
    expect_side_row(&d.rows[0], SIDE_INS, "", "hello", "ins hello");
    side_diff_free(&d);
}

static void test_side_no_newline(void) {
    side_diff d = {0};
    EXPECT(side_diff_build("foo", 3, "foo\n", 4, &d) == SIDE_DIFF_OK &&
           d.nrows == 1,
           "eof newline rows %zu", d.nrows);
    expect_side_row(&d.rows[0], SIDE_CHG, "foo", "foo", "chg same text");
    side_diff_free(&d);
}

static void test_side_replace_zip(void) {
    side_diff d = {0};
    EXPECT(side_diff_build("a\nb\nc\n", 6, "a\nX\nY\nc\n", 8, &d) ==
               SIDE_DIFF_OK &&
           d.nrows == 4,
           "zip rows %zu", d.nrows);
    expect_side_row(&d.rows[0], SIDE_EQ, "a", "a", "eq a");
    expect_side_row(&d.rows[1], SIDE_CHG, "b", "X", "chg b/X");
    expect_side_row(&d.rows[2], SIDE_INS, "", "Y", "ins Y");
    expect_side_row(&d.rows[3], SIDE_EQ, "c", "c", "eq c");
    side_diff_free(&d);
}

static void test_side_locked_wrap(void) {
    side_diff d = {0};
    EXPECT(side_diff_build("abcdefghij\n", 11, "x\n", 2, &d) == SIDE_DIFF_OK &&
           d.nrows == 1,
           "wrap pair");
    EXPECT(side_row_display_height(&d.rows[0], 4, 4) == 3,
           "pair height is max wrap");
    side_view v;
    side_view_init(&v, &d);
    EXPECT(side_view_set_width(&v, 9) == 0 && v.ndisp == 3,
           "9 cols -> 4|4, 3 display rows (got %zu)", v.ndisp);
    side_view_move_line(&v, 1, 2);
    EXPECT(v.top == 1, "scroll stays in the same pair (top %zu)", v.top);
    char err[64] = "";
    EXPECT(side_view_search(&v, "efgh", 1, 1, err, sizeof(err)) == 0 &&
           v.has_match && v.match_pair == 0 && v.match_side == 0 &&
           v.top == 1,
           "search scrolls to the wrap row of the hit (top %zu side %d)",
           v.top, v.match_side);
    side_view_free(&v);
    side_diff_free(&d);
}

static void test_diff_block_need_two(const char *dir) {
    char archive[512], host[512], err[256] = "";
    snprintf(archive, sizeof(archive), "%s/one.tar.gz", dir);
    snprintf(host, sizeof(host), "%s/only.txt", dir);
    EXPECT(write_host_file(host, "only") == 0, "host");
    const char *guests[] = {"only.txt"};
    EXPECT(build_archive(archive, guests, 1, host) == 0, "build");
    le_model *m = model_new(1);
    EXPECT(archive_load(archive, m, 0, err, sizeof(err)) == 0, "load: %s", err);
    le_node *file = model_lookup(m, "only.txt");
    int slots[LE_MAX_ARCHIVES];
    int nslots = file ? node_archives(file, 1, slots) : 0;
    const char *reason = preview_diff_block(file, nslots);
    EXPECT(reason && strstr(reason, "need two archives"),
           "one holder: %s", reason ? reason : "(null)");
    model_free(m);
}

static void test_diff_staged_vs_member(const char *dir) {
    char archive[512], host[512], staged[512], err[256] = "";
    snprintf(archive, sizeof(archive), "%s/base.tar.gz", dir);
    snprintf(host, sizeof(host), "%s/old.txt", dir);
    snprintf(staged, sizeof(staged), "%s/new.txt", dir);
    EXPECT(write_host_file(host, "old line\n") == 0, "old");
    EXPECT(write_host_file(staged, "new line\n") == 0, "new");
    const char *guests[] = {"file.txt"};
    EXPECT(build_archive(archive, guests, 1, host) == 0, "build");

    le_model *m = model_new(2);
    EXPECT(archive_load(archive, m, 0, err, sizeof(err)) == 0, "load 0: %s", err);
    EXPECT(archive_load(archive, m, 1, err, sizeof(err)) == 0, "load 1: %s", err);
    EXPECT(stage_host(m, "file.txt", 1, staged) != NULL, "stage into 1");

    le_member_bytes left = {0}, right = {0};
    EXPECT(member_read_from_archive(archive, "file.txt", MEMBER_PREVIEW_MAX,
                                    &left, err, sizeof(err)) == 0,
           "read left: %s", err);
    EXPECT(member_read_from_host(staged, MEMBER_PREVIEW_MAX, &right, err,
                                 sizeof(err)) == 0,
           "read staged: %s", err);
    side_diff d = {0};
    EXPECT(side_diff_build(left.data, left.len, right.data, right.len, &d) ==
               SIDE_DIFF_OK &&
           d.nrows == 1,
           "staged vs member rows %zu", d.nrows);
    expect_side_row(&d.rows[0], SIDE_CHG, "old line", "new line",
                    "staged vs member");
    side_diff_free(&d);
    member_bytes_free(&left);
    member_bytes_free(&right);
    model_free(m);
}

int main(void) {
    char *dir = make_tmpdir();
    if (!dir) {
        fprintf(stderr, "mkdtemp failed\n");
        return 1;
    }

    test_read_archive_member(dir);
    test_read_truncated_and_binary(dir);
    test_read_host_file(dir);
    test_archive_store_read_member(dir);
    test_preview_put_byte();
    test_preview_wrap();
    test_preview_search();
    test_preview_scroll();
    test_shared_directory_does_not_ask_which_archive(dir);
    test_side_one_line_change();
    test_side_identical_and_empty();
    test_side_no_newline();
    test_side_replace_zip();
    test_side_locked_wrap();
    test_diff_block_need_two(dir);
    test_diff_staged_vs_member(dir);

    if (test_failures) {
        fprintf(stderr, "%d failure(s)  (tmpdir %s)\n", test_failures, dir);
        free(dir);
        return 1;
    }
    printf("ok\n");
    free(dir);
    return 0;
}
