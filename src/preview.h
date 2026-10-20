#pragma once

#include "tree.h"

#include <stddef.h>

/*
 * Headless pager model: wrap loaded text into display rows, expand control
 * characters, and search. The TUI draws this; tests drive it without ncurses.
 */

typedef struct {
    const char *bytes;
    size_t      len;
    int         width;
    size_t     *row_off;    /* byte offset of each display row */
    size_t      nrows;
    size_t      cap;
} preview_layout;

typedef struct {
    preview_layout layout;
    const char    *bytes;
    size_t         len;
    size_t         top;         /* first visible display row */
    size_t         match_off;
    size_t         match_len;
    int            has_match;
} preview_view;

/* Fill out with '^' + letter for a control byte, or the byte itself.
 * Returns how many chars were written (1 or 2), 0 for newline. */
int preview_put_byte(unsigned char c, char *out);

int  preview_layout_build(preview_layout *lay, const char *bytes, size_t len,
                          int width);
void preview_layout_free(preview_layout *lay);

/* Display text of one layout row, not including a terminator. *out_len is
 * chars written. If a search match overlaps the row, *hl_col and *hl_n are
 * the display columns to reverse (hl_n 0 when none). */
int preview_row_render(const preview_layout *lay, size_t row,
                       size_t match_off, size_t match_len, int has_match,
                       char *out, size_t out_cap, size_t *out_len,
                       int *hl_col, int *hl_n);

void preview_view_init(preview_view *v, const char *bytes, size_t len);
void preview_view_free(preview_view *v);
int  preview_view_set_width(preview_view *v, int width);

void preview_view_move_line(preview_view *v, int delta, int page_rows);
void preview_view_page(preview_view *v, int dir, int page_rows);
void preview_view_home(preview_view *v);
void preview_view_end(preview_view *v, int page_rows);

/* Compile pattern as POSIX extended regex and find the next match. wrap 1
 * searches the other end when this direction has no hit. 0 found, 1 not
 * found, -1 bad pattern with err set. */
int preview_view_search(preview_view *v, const char *pattern, int forward,
                        int wrap, char *err, size_t errlen);

/* Why Enter cannot open n, independent of which archive. NULL when a regular
 * file may open. */
const char *preview_block_reason(const le_node *n);

/* True when a regular file is in more than one archive, so Enter must ask
 * which one. Directories and other non-files never ask. */
int preview_needs_archive_choice(const le_node *n, int nslots);
