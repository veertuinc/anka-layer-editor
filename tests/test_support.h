#pragma once

#include "../src/tree.h"

#include <stdio.h>
#include <zlib.h>

extern int test_failures;

#define EXPECT(cond, fmt, ...)                                                 \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: " fmt "\n", __FILE__, __LINE__,       \
                    ##__VA_ARGS__);                                            \
            test_failures++;                                                   \
        }                                                                      \
    } while (0)

char *make_tmpdir(void);
int   write_host_file(const char *path, const char *contents);

/* Archives written and read with the tar code under test. */
int count_path_occurrences(const char *archive, const char *want);
int read_file_entry_contents(const char *archive, const char *want,
                             char *out, size_t outlen);
/* A .tar.gz holding the given guest paths, all with the body of host_file. */
int build_archive(const char *archive_path, const char *const *guest_paths,
                  int nguests, const char *host_file);
int build_seed_archive(const char *archive_path, const char *guest_path,
                       const char *host_file);

/* Raw tar fixtures, independent of the tar code under test. */
typedef struct {
    const char *name;
    char        typeflag;
    const char *linkname;
    unsigned    devmajor;
    const char *body;
    unsigned    uid;
    unsigned    gid;
} raw_member_spec;

typedef struct {
    char          typeflag;
    char          linkname[101];
    unsigned long devmajor;
    char          local_pax[4096];      /* records of the preceding 'x' */
} raw_member_found;

/* Write a pax 'x' or 'g' header holding the given "key=value" records. */
int raw_write_pax(gzFile gz, char typeflag, const char *const *records,
                  int nrecords);
int raw_write_member(gzFile gz, const raw_member_spec *spec);
/* Write the end-of-archive marker and close. */
int raw_finish(gzFile gz);
int build_raw_archive(const char *archive_path, const raw_member_spec *specs,
                      int nspecs);
/* Find a member by header name; returns 1 found, 0 absent, -1 error. */
int raw_find_member(const char *archive, const char *want,
                    raw_member_found *out);

/* Model helpers. */
le_model *load_single(const char *archive);
/* Stage a host file the way the TUI does. */
le_node  *stage_host(le_model *m, const char *dest, int arch_idx,
                     const char *host);
void      remove_and_save(le_model *m, const char *archive,
                          const char *path_to_remove);
