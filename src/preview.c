#include "preview.h"
#include "util.h"

#include <regex.h>
#include <stdlib.h>
#include <string.h>

int preview_put_byte(unsigned char c, char *out) {
    if (c == '\n')
        return 0;
    if (c == '\t') {
        out[0] = '\t';
        return 1;
    }
    if (c < 0x20) {
        out[0] = '^';
        out[1] = (char)(c + '@');
        return 2;
    }
    if (c == 0x7f) {
        out[0] = '^';
        out[1] = '?';
        return 2;
    }
    out[0] = (char)c;
    return 1;
}

static int display_width(unsigned char c) {
    char tmp[2];
    return preview_put_byte(c, tmp);
}

static int push_row(preview_layout *lay, size_t off) {
    if (lay->nrows == lay->cap) {
        size_t cap = lay->cap ? lay->cap * 2 : 32;
        size_t *row_off = realloc(lay->row_off, cap * sizeof(*row_off));
        if (!row_off)
            return -1;
        lay->row_off = row_off;
        lay->cap = cap;
    }
    lay->row_off[lay->nrows++] = off;
    return 0;
}

int preview_layout_build(preview_layout *lay, const char *bytes, size_t len,
                         int width) {
    preview_layout_free(lay);
    lay->bytes = bytes;
    lay->len = len;
    lay->width = width < 1 ? 1 : width;
    if (push_row(lay, 0) == -1)
        return -1;
    int col = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)bytes[i];
        if (c == '\n') {
            if (push_row(lay, i + 1) == -1)
                return -1;
            col = 0;
            continue;
        }
        int w = display_width(c);
        if (col + w > lay->width && col > 0) {
            if (push_row(lay, i) == -1)
                return -1;
            col = 0;
        }
        col += w;
    }
    return 0;
}

void preview_layout_free(preview_layout *lay) {
    if (!lay)
        return;
    free(lay->row_off);
    lay->row_off = NULL;
    lay->nrows = 0;
    lay->cap = 0;
    lay->bytes = NULL;
    lay->len = 0;
    lay->width = 0;
}

static size_t row_end_off(const preview_layout *lay, size_t row) {
    if (row + 1 < lay->nrows)
        return lay->row_off[row + 1];
    return lay->len;
}

int preview_row_render(const preview_layout *lay, size_t row,
                       size_t match_off, size_t match_len, int has_match,
                       char *out, size_t out_cap, size_t *out_len,
                       int *hl_col, int *hl_n) {
    *out_len = 0;
    *hl_col = 0;
    *hl_n = 0;
    if (!lay || row >= lay->nrows)
        return -1;
    size_t start = lay->row_off[row];
    size_t end = row_end_off(lay, row);
    int col = 0;
    int hl_started = 0;
    for (size_t i = start; i < end; i++) {
        unsigned char c = (unsigned char)lay->bytes[i];
        if (c == '\n')
            break;
        char tmp[2];
        int n = preview_put_byte(c, tmp);
        if (n <= 0)
            continue;
        if (*out_len + (size_t)n >= out_cap)
            break;
        memcpy(out + *out_len, tmp, (size_t)n);
        int in_match = has_match && i >= match_off && i < match_off + match_len;
        if (in_match) {
            if (!hl_started) {
                *hl_col = col;
                hl_started = 1;
            }
            *hl_n += n;
        }
        *out_len += (size_t)n;
        col += n;
    }
    if (*out_len < out_cap)
        out[*out_len] = '\0';
    return 0;
}

static size_t row_for_offset(const preview_view *v, size_t off) {
    size_t row = 0;
    for (size_t i = 0; i < v->layout.nrows; i++) {
        if (v->layout.row_off[i] <= off)
            row = i;
        else
            break;
    }
    return row;
}

static void clamp_top(preview_view *v, int page_rows) {
    if (page_rows < 1)
        page_rows = 1;
    if (v->layout.nrows <= (size_t)page_rows)
        v->top = 0;
    else if (v->top > v->layout.nrows - (size_t)page_rows)
        v->top = v->layout.nrows - (size_t)page_rows;
}

void preview_view_init(preview_view *v, const char *bytes, size_t len) {
    memset(v, 0, sizeof(*v));
    v->bytes = bytes;
    v->len = len;
}

void preview_view_free(preview_view *v) {
    if (!v)
        return;
    preview_layout_free(&v->layout);
}

int preview_view_set_width(preview_view *v, int width) {
    return preview_layout_build(&v->layout, v->bytes, v->len, width);
}

void preview_view_move_line(preview_view *v, int delta, int page_rows) {
    if (delta > 0)
        v->top += (size_t)delta;
    else if (delta < 0) {
        size_t back = (size_t)(-delta);
        v->top = back > v->top ? 0 : v->top - back;
    }
    clamp_top(v, page_rows);
}

void preview_view_page(preview_view *v, int dir, int page_rows) {
    if (page_rows < 1)
        page_rows = 1;
    preview_view_move_line(v, dir * page_rows, page_rows);
}

void preview_view_home(preview_view *v) {
    v->top = 0;
}

void preview_view_end(preview_view *v, int page_rows) {
    if (v->layout.nrows == 0) {
        v->top = 0;
        return;
    }
    v->top = v->layout.nrows;
    clamp_top(v, page_rows);
}

static int search_forward(regex_t *re, const char *bytes, size_t len,
                          size_t from, size_t *match_off, size_t *match_len) {
    if (from > len)
        from = len;
    while (from <= len) {
        regmatch_t m;
        if (regexec(re, bytes + from, 1, &m, 0) != 0)
            return 0;
        if (m.rm_so < 0)
            return 0;
        *match_off = from + (size_t)m.rm_so;
        *match_len = (size_t)(m.rm_eo - m.rm_so);
        if (*match_off > len)
            return 0;
        if (*match_len == 0) {
            from = *match_off + 1;
            continue;
        }
        return 1;
    }
    return 0;
}

static int search_last_before(regex_t *re, const char *bytes, size_t end,
                              size_t *match_off, size_t *match_len) {
    size_t from = 0;
    int found = 0;
    while (from < end) {
        size_t off, n;
        if (!search_forward(re, bytes, end, from, &off, &n))
            break;
        if (off >= end)
            break;
        found = 1;
        *match_off = off;
        *match_len = n;
        from = off + (n ? n : 1);
    }
    return found;
}

int preview_view_search(preview_view *v, const char *pattern, int forward,
                        int wrap, char *err, size_t errlen) {
    regex_t re;
    int rc = regcomp(&re, pattern, REG_EXTENDED);
    if (rc != 0) {
        char msg[128];
        regerror(rc, &re, msg, sizeof(msg));
        le_seterr(err, errlen, "%s", msg);
        return -1;
    }

    size_t off = 0, n = 0;
    int found = 0;
    if (forward) {
        size_t from = v->has_match
                          ? v->match_off + (v->match_len ? v->match_len : 1)
                          : 0;
        found = search_forward(&re, v->bytes, v->len, from, &off, &n);
        if (!found && wrap)
            found = search_forward(&re, v->bytes, v->len, 0, &off, &n);
    } else {
        size_t end = v->has_match ? v->match_off : v->len;
        found = search_last_before(&re, v->bytes, end, &off, &n);
        if (!found && wrap)
            found = search_last_before(&re, v->bytes, v->len, &off, &n);
    }
    regfree(&re);
    if (!found) {
        v->has_match = 0;
        v->match_off = 0;
        v->match_len = 0;
        return 1;
    }
    v->has_match = 1;
    v->match_off = off;
    v->match_len = n;
    v->top = row_for_offset(v, off);
    return 0;
}

const char *preview_block_reason(const le_node *n) {
    if (!n)
        return "not present in any archive";
    if (node_is_whiteout(n))
        return "cannot preview a whiteout";
    const le_meta *meta = node_display_meta(n, LE_MAX_ARCHIVES);
    switch (meta->kind) {
    case LE_KIND_DIR:
        return "cannot preview a directory";
    case LE_KIND_SYMLINK:
        return "cannot preview a symlink";
    case LE_KIND_HARDLINK:
        return "cannot preview a hard link";
    case LE_KIND_OTHER:
        return "cannot preview a special file";
    default:
        return NULL;
    }
}

int preview_needs_archive_choice(const le_node *n, int nslots) {
    if (preview_block_reason(n))
        return 0;
    return nslots > 1;
}
