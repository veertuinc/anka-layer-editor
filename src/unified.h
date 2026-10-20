#pragma once

#include "tree.h"

#include <stddef.h>

/* side_diff_build return values. */
enum {
    SIDE_DIFF_OK        = 0,  /* *out holds aligned pairs; caller frees */
    SIDE_DIFF_IDENTICAL = 1,  /* buffers match; *out is empty */
    SIDE_DIFF_NOMEM     = -1
};

enum {
    SIDE_EQ  = 0,  /* same text on both sides */
    SIDE_DEL = 1,  /* left only; right is empty */
    SIDE_INS = 2,  /* right only; left is empty */
    SIDE_CHG = 3   /* both sides, different */
};

typedef struct {
    unsigned char kind;
    char         *left;   /* owned, no newline */
    char         *right;
} side_row;

typedef struct {
    side_row *rows;
    size_t    nrows;
} side_diff;

/* One visual row per aligned pair. A replace block zips old lines with new
 * lines, then leftover deletes or inserts. */
int  side_diff_build(const char *a, size_t a_len, const char *b, size_t b_len,
                     side_diff *out);
void side_diff_free(side_diff *d);

/* Display rows this pair occupies when each side wraps at its column width.
 * The shorter side pads with blank rows. At least 1. */
int side_row_display_height(const side_row *row, int left_width,
                            int right_width);

typedef struct {
    const side_diff *diff;
    int              total_cols;
    int              left_width;
    int              right_width;
    size_t          *pair_disp0; /* first display row of each pair */
    size_t           ndisp;
    size_t           top;
    size_t           match_pair;
    int              match_side; /* 0 left, 1 right */
    size_t           match_off;
    size_t           match_len;
    int              has_match;
} side_view;

void side_view_init(side_view *v, const side_diff *d);
void side_view_free(side_view *v);
/* total_cols includes the 1-column separator. */
int  side_view_set_width(side_view *v, int total_cols);

void side_view_move_line(side_view *v, int delta, int page_rows);
void side_view_page(side_view *v, int dir, int page_rows);
void side_view_home(side_view *v);
void side_view_end(side_view *v, int page_rows);

/* Search left then right of each pair, POSIX ERE. 0 found, 1 not found,
 * -1 bad pattern. */
int side_view_search(side_view *v, const char *pattern, int forward, int wrap,
                     char *err, size_t errlen);

/* Why d cannot run, before any read. NULL when two holders and a regular
 * file. */
const char *preview_diff_block(const le_node *n, int nslots);
