/*
 * Helpers shared by the test programs: expectations, temp files, and raw tar
 * fixtures written without the tar code under test.
 */
#include "test_support.h"

#include "../src/archive.h"
#include "../src/tar.h"
#include "../src/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <zlib.h>

int test_failures = 0;

char *make_tmpdir(void) {
    char *tmpl = strdup("/tmp/anka-oci-tag-editor-test.XXXXXX");
    if (!tmpl || !mkdtemp(tmpl)) {
        free(tmpl);
        return NULL;
    }
    return tmpl;
}

int write_host_file(const char *path, const char *contents) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    size_t n = strlen(contents);
    int ok = fwrite(contents, 1, n, f) == n;
    fclose(f);
    return ok ? 0 : -1;
}

int count_path_occurrences(const char *archive, const char *want) {
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

int read_file_entry_contents(const char *archive, const char *want,
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
int build_archive(const char *archive_path,
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

int build_seed_archive(const char *archive_path, const char *guest_path,
                              const char *host_file) {
    return build_archive(archive_path, &guest_path, 1, host_file);
}

/* ---- raw tar fixtures, independent of the tar code under test ---- */

/* Zero-padded octal with a NUL terminator; fixture values always fit. */
static void raw_octal(char *field, size_t field_len, unsigned long long value) {
    field[field_len - 1] = '\0';
    for (size_t i = field_len - 1; i > 0; i--, value >>= 3)
        field[i - 1] = (char)('0' + (value & 7));
}

static int raw_write_header(gzFile gz, const char *name, char typeflag,
                            const char *linkname, unsigned devmajor,
                            unsigned uid, unsigned gid, size_t body_len) {
    unsigned char header[512];
    memset(header, 0, sizeof(header));
    memcpy(header, name, strnlen(name, 100));
    raw_octal((char *)header + 100, 8, typeflag == '5' ? 0755 : 0644);
    raw_octal((char *)header + 108, 8, uid);
    raw_octal((char *)header + 116, 8, gid);
    raw_octal((char *)header + 124, 12, body_len);
    raw_octal((char *)header + 136, 12, 1700000000);
    header[156] = (unsigned char)typeflag;
    if (linkname)
        memcpy(header + 157, linkname, strnlen(linkname, 100));
    memcpy(header + 257, "ustar", 6);
    memcpy(header + 263, "00", 2);
    raw_octal((char *)header + 329, 8, devmajor);
    raw_octal((char *)header + 337, 8, 0);
    memset(header + 148, ' ', 8);
    unsigned checksum = 0;
    for (int i = 0; i < 512; i++)
        checksum += header[i];
    raw_octal((char *)header + 148, 7, checksum);
    header[155] = ' ';
    return gzwrite(gz, header, sizeof(header)) == (int)sizeof(header) ? 0 : -1;
}

static int raw_write_body(gzFile gz, const char *body, size_t body_len) {
    static const char zeros[512];
    if (body_len && gzwrite(gz, body, (unsigned)body_len) != (int)body_len)
        return -1;
    size_t pad = (512 - body_len % 512) % 512;
    if (pad && gzwrite(gz, zeros, (unsigned)pad) != (int)pad)
        return -1;
    return 0;
}

/* Write a pax 'x' or 'g' header holding the given "key=value" records. */
int raw_write_pax(gzFile gz, char typeflag, const char *const *records,
                         int nrecords) {
    char body[2048];
    size_t body_len = 0;
    for (int i = 0; i < nrecords; i++) {
        size_t record_len = strlen(records[i]) + 2;      /* ' ' + '\n' */
        size_t total = record_len + 1;
        while (snprintf(NULL, 0, "%zu", total) + record_len != total)
            total = (size_t)snprintf(NULL, 0, "%zu", total) + record_len;
        body_len += (size_t)snprintf(body + body_len, sizeof(body) - body_len,
                                     "%zu %s\n", total, records[i]);
    }
    if (raw_write_header(gz, "PaxHeaders", typeflag, NULL, 0, 0, 0, body_len) == -1)
        return -1;
    return raw_write_body(gz, body, body_len);
}

int raw_write_member(gzFile gz, const raw_member_spec *spec) {
    size_t body_len = spec->body ? strlen(spec->body) : 0;
    if (raw_write_header(gz, spec->name, spec->typeflag, spec->linkname,
                         spec->devmajor, spec->uid, spec->gid, body_len) == -1)
        return -1;
    return raw_write_body(gz, spec->body, body_len);
}

int raw_finish(gzFile gz) {
    static const char zeros[1024];
    int rc = gzwrite(gz, zeros, sizeof(zeros)) == (int)sizeof(zeros) ? 0 : -1;
    return gzclose(gz) == Z_OK ? rc : -1;
}

int build_raw_archive(const char *archive_path,
                             const raw_member_spec *specs, int nspecs) {
    gzFile gz = gzopen(archive_path, "wb");
    if (!gz)
        return -1;
    int rc = 0;
    for (int i = 0; i < nspecs && rc == 0; i++)
        rc = raw_write_member(gz, &specs[i]);
    return raw_finish(gz) == 0 ? rc : -1;
}

static unsigned long long raw_parse_octal(const unsigned char *field,
                                          size_t len) {
    unsigned long long value = 0;
    for (size_t i = 0; i < len && field[i] >= '0' && field[i] <= '7'; i++)
        value = (value << 3) | (unsigned)(field[i] - '0');
    return value;
}

/* Find a member by header name; returns 1 found, 0 absent, -1 error. */
int raw_find_member(const char *archive, const char *want,
                           raw_member_found *out) {
    gzFile gz = gzopen(archive, "rb");
    if (!gz)
        return -1;
    char pending_pax[4096] = "";
    int result = 0;
    unsigned char header[512];
    while (gzread(gz, header, sizeof(header)) == (int)sizeof(header)) {
        int all_zero = 1;
        for (int i = 0; i < 512 && all_zero; i++)
            all_zero = header[i] == 0;
        if (all_zero)
            break;
        size_t size = (size_t)raw_parse_octal(header + 124, 12);
        size_t padded = size + (512 - size % 512) % 512;
        char *data = malloc(padded + 1);
        if (!data || (padded && gzread(gz, data, (unsigned)padded) != (int)padded)) {
            free(data);
            result = -1;
            break;
        }
        data[size] = '\0';
        char typeflag = (char)header[156];
        if (typeflag == 'x') {
            snprintf(pending_pax, sizeof(pending_pax), "%s", data);
            free(data);
            continue;
        }
        free(data);
        if (typeflag == 'g')
            continue;
        char name[101];
        snprintf(name, sizeof(name), "%.100s", (const char *)header);
        size_t name_len = strlen(name);
        if (name_len > 1 && name[name_len - 1] == '/')
            name[name_len - 1] = '\0';
        if (!strcmp(name, want)) {
            out->typeflag = typeflag;
            snprintf(out->linkname, sizeof(out->linkname), "%.100s",
                     (const char *)header + 157);
            out->devmajor = (unsigned long)raw_parse_octal(header + 329, 8);
            snprintf(out->local_pax, sizeof(out->local_pax), "%s", pending_pax);
            result = 1;
            break;
        }
        pending_pax[0] = '\0';
    }
    gzclose(gz);
    return result;
}

le_model *load_single(const char *archive) {
    char err[256];
    le_model *m = model_new(1);
    EXPECT(archive_load(archive, m, 0, err, sizeof(err)) == 0, "load: %s", err);
    return m;
}

/* Stage a host file the way the TUI does; err may be NULL. */
static le_node *stage_host_err(le_model *m, const char *dest, int arch_idx,
                               const char *host, char *err, size_t errlen) {
    le_meta meta;
    char link[PATH_MAX];
    if (archive_host_meta(host, &meta, link, sizeof(link), err, errlen) == -1)
        return NULL;
    return model_stage_host_file(m, dest, arch_idx, host, &meta, err, errlen);
}

le_node *stage_host(le_model *m, const char *dest, int arch_idx,
                           const char *host) {
    return stage_host_err(m, dest, arch_idx, host, NULL, 0);
}

void remove_and_save(le_model *m, const char *archive,
                            const char *path_to_remove) {
    char err[256];
    le_node *n = model_lookup(m, path_to_remove);
    EXPECT(n != NULL, "lookup %s", path_to_remove);
    model_toggle_removed(m, n, 0);
    EXPECT(archive_save(archive, m, 0, err, sizeof(err)) == 0, "save: %s", err);
}
