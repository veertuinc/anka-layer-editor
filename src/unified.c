#include "unified.h"
#include "preview.h"
#include "util.h"

#include <regex.h>
#include <stdlib.h>
#include <string.h>

#define SIDE_DIFF_DP_CELLS 4000000u

enum { ED_KEEP, ED_DEL, ED_INS };

typedef struct {
    const char *p;
    size_t      n;
    int         has_nl;
} line_span;

static int split_lines(const char *s, size_t len, line_span **out,
                       size_t *nlines) {
    size_t count = 0;
    for (size_t i = 0; i < len; ) {
        count++;
        const char *nl = memchr(s + i, '\n', len - i);
        i = nl ? (size_t)(nl - s) + 1 : len;
    }
    line_span *lines = count ? calloc(count, sizeof(*lines)) : NULL;
    if (count && !lines)
        return -1;
    size_t n = 0;
    for (size_t i = 0; i < len; ) {
        const char *nl = memchr(s + i, '\n', len - i);
        if (nl) {
            lines[n].p = s + i;
            lines[n].n = (size_t)(nl - (s + i));
            lines[n].has_nl = 1;
            n++;
            i = (size_t)(nl - s) + 1;
        } else {
            lines[n].p = s + i;
            lines[n].n = len - i;
            lines[n].has_nl = 0;
            n++;
            break;
        }
    }
    *out = lines;
    *nlines = n;
    return 0;
}

static int line_equal(line_span a, line_span b) {
    return a.n == b.n && a.has_nl == b.has_nl &&
           memcmp(a.p, b.p, a.n) == 0;
}

/* Build a reverse edit script via LCS DP, then reverse it into ops. */
static int lcs_ops(line_span *a, size_t na, line_span *b, size_t nb,
                   unsigned char **ops_out, size_t *nops_out) {
    size_t cols = nb + 1;
    size_t cells = (na + 1) * cols;
    unsigned int *dp = calloc(cells, sizeof(*dp));
    if (!dp)
        return -1;
    for (size_t i = 0; i < na; i++)
        for (size_t j = 0; j < nb; j++) {
            if (line_equal(a[i], b[j]))
                dp[(i + 1) * cols + (j + 1)] = dp[i * cols + j] + 1;
            else {
                unsigned int up = dp[i * cols + (j + 1)];
                unsigned int left = dp[(i + 1) * cols + j];
                dp[(i + 1) * cols + (j + 1)] = up > left ? up : left;
            }
        }

    size_t cap = na + nb + 1;
    unsigned char *rev = malloc(cap);
    if (!rev) {
        free(dp);
        return -1;
    }
    size_t nrev = 0;
    size_t i = na, j = nb;
    while (i > 0 && j > 0) {
        if (line_equal(a[i - 1], b[j - 1]) &&
            dp[i * cols + j] == dp[(i - 1) * cols + (j - 1)] + 1) {
            rev[nrev++] = ED_KEEP;
            i--;
            j--;
        } else if (dp[(i - 1) * cols + j] >= dp[i * cols + (j - 1)]) {
            rev[nrev++] = ED_DEL;
            i--;
        } else {
            rev[nrev++] = ED_INS;
            j--;
        }
    }
    while (i > 0) {
        rev[nrev++] = ED_DEL;
        i--;
    }
    while (j > 0) {
        rev[nrev++] = ED_INS;
        j--;
    }
    free(dp);

    unsigned char *ops = NULL;
    if (nrev) {
        ops = malloc(nrev);
        if (!ops) {
            free(rev);
            return -1;
        }
        for (size_t k = 0; k < nrev; k++)
            ops[k] = rev[nrev - 1 - k];
    }
    free(rev);
    *ops_out = ops;
    *nops_out = nrev;
    return 0;
}

/* All deletes then all inserts. Used when the DP table would be too large. */
static int whole_replace_ops(size_t na, size_t nb, unsigned char **ops_out,
                             size_t *nops_out) {
    size_t nops = na + nb;
    unsigned char *ops = NULL;
    if (nops) {
        ops = malloc(nops);
        if (!ops)
            return -1;
        for (size_t i = 0; i < na; i++)
            ops[i] = ED_DEL;
        for (size_t i = 0; i < nb; i++)
            ops[na + i] = ED_INS;
    }
    *ops_out = ops;
    *nops_out = nops;
    return 0;
}

static int emit_pair(side_diff *d, size_t *cap, unsigned char kind,
                     const line_span *left, const line_span *right) {
    if (d->nrows == *cap) {
        size_t next = *cap ? *cap * 2 : 16;
        side_row *rows = realloc(d->rows, next * sizeof(*rows));
        if (!rows)
            return -1;
        d->rows = rows;
        *cap = next;
    }
    size_t ln = left ? left->n : 0;
    size_t rn = right ? right->n : 0;
    char *lbuf = malloc(ln + 1);
    char *rbuf = malloc(rn + 1);
    if (!lbuf || !rbuf) {
        free(lbuf);
        free(rbuf);
        return -1;
    }
    if (left && ln)
        memcpy(lbuf, left->p, ln);
    lbuf[ln] = '\0';
    if (right && rn)
        memcpy(rbuf, right->p, rn);
    rbuf[rn] = '\0';
    d->rows[d->nrows].kind = kind;
    d->rows[d->nrows].left = lbuf;
    d->rows[d->nrows].right = rbuf;
    d->nrows++;
    return 0;
}

static int emit_pairs(side_diff *d, line_span *a, line_span *b,
                      unsigned char *ops, size_t nops) {
    size_t cap = 0;
    size_t ai = 0, bi = 0, k = 0;
    while (k < nops) {
        if (ops[k] == ED_KEEP) {
            if (emit_pair(d, &cap, SIDE_EQ, &a[ai], &b[bi]) == -1)
                return -1;
            ai++;
            bi++;
            k++;
            continue;
        }
        size_t nd = 0, ni = 0, t = k;
        while (t < nops && ops[t] != ED_KEEP) {
            if (ops[t] == ED_DEL)
                nd++;
            else
                ni++;
            t++;
        }
        size_t zip = nd < ni ? nd : ni;
        for (size_t i = 0; i < zip; i++) {
            if (emit_pair(d, &cap, SIDE_CHG, &a[ai + i], &b[bi + i]) == -1)
                return -1;
        }
        for (size_t i = zip; i < nd; i++) {
            if (emit_pair(d, &cap, SIDE_DEL, &a[ai + i], NULL) == -1)
                return -1;
        }
        for (size_t i = zip; i < ni; i++) {
            if (emit_pair(d, &cap, SIDE_INS, NULL, &b[bi + i]) == -1)
                return -1;
        }
        ai += nd;
        bi += ni;
        k = t;
    }
    return 0;
}

void side_diff_free(side_diff *d) {
    if (!d)
        return;
    for (size_t i = 0; i < d->nrows; i++) {
        free(d->rows[i].left);
        free(d->rows[i].right);
    }
    free(d->rows);
    d->rows = NULL;
    d->nrows = 0;
}

int side_diff_build(const char *a, size_t a_len, const char *b, size_t b_len,
                    side_diff *out) {
    memset(out, 0, sizeof(*out));
    if (!a) {
        a = "";
        a_len = 0;
    }
    if (!b) {
        b = "";
        b_len = 0;
    }
    if (a_len == b_len && memcmp(a, b, a_len) == 0)
        return SIDE_DIFF_IDENTICAL;

    line_span *la = NULL, *lb = NULL;
    size_t na = 0, nb = 0;
    if (split_lines(a, a_len, &la, &na) == -1 ||
        split_lines(b, b_len, &lb, &nb) == -1) {
        free(la);
        free(lb);
        return SIDE_DIFF_NOMEM;
    }

    unsigned char *ops = NULL;
    size_t nops = 0;
    int rc;
    if (na && nb && na > (SIDE_DIFF_DP_CELLS / nb))
        rc = whole_replace_ops(na, nb, &ops, &nops);
    else
        rc = lcs_ops(la, na, lb, nb, &ops, &nops);
    if (rc == -1) {
        free(la);
        free(lb);
        return SIDE_DIFF_NOMEM;
    }

    if (emit_pairs(out, la, lb, ops, nops) == -1) {
        side_diff_free(out);
        free(ops);
        free(la);
        free(lb);
        return SIDE_DIFF_NOMEM;
    }
    free(ops);
    free(la);
    free(lb);
    return SIDE_DIFF_OK;
}

static int wrap_rows(const char *text, int width) {
    preview_layout lay = {0};
    const char *s = text ? text : "";
    if (preview_layout_build(&lay, s, strlen(s), width < 1 ? 1 : width) == -1)
        return -1;
    int n = (int)lay.nrows;
    preview_layout_free(&lay);
    return n < 1 ? 1 : n;
}

int side_row_display_height(const side_row *row, int left_width,
                            int right_width) {
    if (!row)
        return 1;
    int lh = wrap_rows(row->left, left_width);
    int rh = wrap_rows(row->right, right_width);
    if (lh < 1 || rh < 1)
        return 1;
    return lh > rh ? lh : rh;
}

static void side_clamp_top(side_view *v, int page_rows) {
    if (page_rows < 1)
        page_rows = 1;
    if (v->ndisp <= (size_t)page_rows)
        v->top = 0;
    else if (v->top > v->ndisp - (size_t)page_rows)
        v->top = v->ndisp - (size_t)page_rows;
}

void side_view_init(side_view *v, const side_diff *d) {
    memset(v, 0, sizeof(*v));
    v->diff = d;
}

void side_view_free(side_view *v) {
    if (!v)
        return;
    free(v->pair_disp0);
    v->pair_disp0 = NULL;
    v->ndisp = 0;
}

int side_view_set_width(side_view *v, int total_cols) {
    int sep = 1;
    int inner = total_cols > sep ? total_cols - sep : 1;
    v->left_width = inner / 2;
    if (v->left_width < 1)
        v->left_width = 1;
    v->right_width = inner - v->left_width;
    if (v->right_width < 1)
        v->right_width = 1;

    size_t npair = v->diff ? v->diff->nrows : 0;
    size_t *off = calloc(npair ? npair : 1, sizeof(*off));
    if (!off)
        return -1;
    size_t disp = 0;
    for (size_t i = 0; i < npair; i++) {
        off[i] = disp;
        int h = side_row_display_height(&v->diff->rows[i], v->left_width,
                                        v->right_width);
        disp += (size_t)(h < 1 ? 1 : h);
    }
    free(v->pair_disp0);
    v->pair_disp0 = off;
    v->ndisp = disp ? disp : 1;
    v->total_cols = total_cols;
    return 0;
}

void side_view_move_line(side_view *v, int delta, int page_rows) {
    if (delta > 0)
        v->top += (size_t)delta;
    else if (delta < 0) {
        size_t back = (size_t)(-delta);
        v->top = back > v->top ? 0 : v->top - back;
    }
    side_clamp_top(v, page_rows);
}

void side_view_page(side_view *v, int dir, int page_rows) {
    if (page_rows < 1)
        page_rows = 1;
    side_view_move_line(v, dir * page_rows, page_rows);
}

void side_view_home(side_view *v) {
    v->top = 0;
}

void side_view_end(side_view *v, int page_rows) {
    v->top = v->ndisp;
    side_clamp_top(v, page_rows);
}

static size_t layout_row_for_off(const preview_layout *lay, size_t off) {
    size_t row = 0;
    for (size_t i = 0; i < lay->nrows; i++) {
        if (lay->row_off[i] <= off)
            row = i;
        else
            break;
    }
    return row;
}

static const char *side_text(const side_row *row, int side) {
    if (!row)
        return "";
    const char *s = side ? row->right : row->left;
    return s ? s : "";
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

static void side_view_goto_match(side_view *v, size_t pair, int side,
                                 size_t off, size_t n) {
    v->has_match = 1;
    v->match_pair = pair;
    v->match_side = side;
    v->match_off = off;
    v->match_len = n;
    const char *text = side_text(&v->diff->rows[pair], side);
    int width = side ? v->right_width : v->left_width;
    preview_layout lay = {0};
    if (preview_layout_build(&lay, text, strlen(text), width < 1 ? 1 : width) ==
        0) {
        size_t wrap = layout_row_for_off(&lay, off);
        preview_layout_free(&lay);
        v->top = (v->pair_disp0 ? v->pair_disp0[pair] : 0) + wrap;
    } else {
        preview_layout_free(&lay);
        v->top = v->pair_disp0 ? v->pair_disp0[pair] : 0;
    }
}

static int search_cells_forward(regex_t *re, const side_diff *d,
                                size_t start_pair, int start_side, size_t from,
                                size_t *hit_pair, int *hit_side,
                                size_t *hit_off, size_t *hit_n) {
    size_t npair = d ? d->nrows : 0;
    for (size_t p = start_pair; p < npair; p++) {
        int s0 = (p == start_pair) ? start_side : 0;
        for (int s = s0; s < 2; s++) {
            const char *text = side_text(&d->rows[p], s);
            size_t len = strlen(text);
            size_t start = (p == start_pair && s == start_side) ? from : 0;
            size_t off, n;
            if (search_forward(re, text, len, start, &off, &n)) {
                *hit_pair = p;
                *hit_side = s;
                *hit_off = off;
                *hit_n = n;
                return 1;
            }
        }
    }
    return 0;
}

static int search_cells_backward(regex_t *re, const side_diff *d,
                                 size_t start_pair, int start_side, size_t end,
                                 int use_end, size_t *hit_pair, int *hit_side,
                                 size_t *hit_off, size_t *hit_n) {
    size_t npair = d ? d->nrows : 0;
    if (!npair)
        return 0;
    size_t p = start_pair;
    int s = start_side;
    for (;;) {
        const char *text = side_text(&d->rows[p], s);
        size_t len = strlen(text);
        size_t limit = use_end ? end : len;
        size_t off, n;
        if (search_last_before(re, text, limit, &off, &n)) {
            *hit_pair = p;
            *hit_side = s;
            *hit_off = off;
            *hit_n = n;
            return 1;
        }
        use_end = 0;
        if (s > 0)
            s--;
        else if (p > 0) {
            p--;
            s = 1;
        } else
            return 0;
    }
}

int side_view_search(side_view *v, const char *pattern, int forward, int wrap,
                     char *err, size_t errlen) {
    regex_t re;
    int rc = regcomp(&re, pattern, REG_EXTENDED);
    if (rc != 0) {
        char msg[128];
        regerror(rc, &re, msg, sizeof(msg));
        le_seterr(err, errlen, "%s", msg);
        return -1;
    }

    size_t npair = v->diff ? v->diff->nrows : 0;
    int found = 0;
    size_t hit_pair = 0, hit_off = 0, hit_n = 0;
    int hit_side = 0;

    if (forward) {
        size_t start_pair = 0;
        int start_side = 0;
        size_t from = 0;
        if (v->has_match) {
            start_pair = v->match_pair;
            start_side = v->match_side;
            from = v->match_off + (v->match_len ? v->match_len : 1);
        }
        found = search_cells_forward(&re, v->diff, start_pair, start_side, from,
                                     &hit_pair, &hit_side, &hit_off, &hit_n);
        if (!found && wrap)
            found = search_cells_forward(&re, v->diff, 0, 0, 0, &hit_pair,
                                         &hit_side, &hit_off, &hit_n);
    } else if (npair) {
        size_t start_pair = npair - 1;
        int start_side = 1;
        size_t end = 0;
        int use_end = 0;
        if (v->has_match) {
            start_pair = v->match_pair;
            start_side = v->match_side;
            end = v->match_off;
            use_end = 1;
        }
        found = search_cells_backward(&re, v->diff, start_pair, start_side, end,
                                      use_end, &hit_pair, &hit_side, &hit_off,
                                      &hit_n);
        if (!found && wrap)
            found = search_cells_backward(&re, v->diff, npair - 1, 1, 0, 0,
                                          &hit_pair, &hit_side, &hit_off,
                                          &hit_n);
    }

    regfree(&re);
    if (!found) {
        v->has_match = 0;
        v->match_off = 0;
        v->match_len = 0;
        return 1;
    }
    side_view_goto_match(v, hit_pair, hit_side, hit_off, hit_n);
    return 0;
}

const char *preview_diff_block(const le_node *n, int nslots) {
    const char *reason = preview_block_reason(n);
    if (reason)
        return reason;
    if (nslots < 2)
        return "need two archives that hold this file";
    return NULL;
}
