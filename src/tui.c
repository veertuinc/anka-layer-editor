#include "tui.h"
#include "archive.h"
#include "member_read.h"
#include "preview.h"
#include "unified.h"
#include "util.h"

#include <ncurses.h>
#include <glob.h>
#include <locale.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <libgen.h>
#include <sys/stat.h>

typedef struct {
    const le_session *sess;
    le_model   *m;
    le_row     *rows;
    size_t      nrows;
    size_t      rowcap;
    size_t      cursor;      /* selected row index */
    size_t      top;         /* first visible row */
    char        filter[128];
    int         filtering;
    char        msg[512];    /* transient status message */
    int         show_help;
    unsigned long shared_warn_generation;
    int         shared_warn_valid;
} tui_state;

/* Results of the archive picker that are not an archive index. */
#define PICK_CANCEL (-1)
#define PICK_ALL    (-2)

#define KEY_ESCAPE 27
#define KEY_CTRL(c) ((c) & 0x1f)
/* read_key returns this when SIGINT or SIGTERM asked the editor to leave.
 * 75 is the process status for SIGTERM, which `make dev` uses to restart. */
#define KEY_STOP (-2)
#define RELOAD_EXIT 75

/* 1 = leave (SIGINT), 2 = leave and ask `make dev` to restart (SIGTERM). */
static volatile sig_atomic_t stop_requested;

static void request_stop(int sig) {
    stop_requested = (sig == SIGTERM) ? 2 : 1;
}

/* getch waits, so a reload signal would not be seen until the next key.
 * A short timeout lets the loop notice the signal and call endwin. */
static int read_key(void) {
    for (;;) {
        if (stop_requested)
            return KEY_STOP;
        int ch = getch();
        if (ch != ERR)
            return ch;
    }
}

/* Color pairs from the Veertu site palette (https://veertu.com/):
 * shared paths: amber #FCB900, selected row: light purple #9B51E0, whiteouts:
 * rose #E21E79. 256-color terminals get the nearest cube color; 8-color
 * terminals get yellow / magenta / red. Black text on the purple bar so the
 * letters stay readable. */
#define PAIR_SHARED   1
#define PAIR_WHITEOUT 2
#define PAIR_SELECTED 3
#define VEERTU_AMBER_256  214   /* #FFAF00 ~ #FCB900 */
#define VEERTU_PURPLE_256 141   /* #AF87FF ~ #9B51E0 */
#define VEERTU_ROSE_256   162   /* #D70087 ~ #E21E79 */
static int have_colors;
static int have_select_color;

static int brand_color(int color_256, int color_8) {
    return COLORS >= 256 ? color_256 : color_8;
}

#define HELP_MAX_LINES 4
#define HELP_LINE_CAP  256

/* ---- text helpers ---- */

/* Append to a fixed-size buffer, returning the new offset. Once the buffer is
 * full the offset stops advancing, so callers can chain appends of unbounded
 * text (archive labels, deep paths) without overrunning it. */
static int str_append(char *buf, size_t cap, int off, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static int str_append(char *buf, size_t cap, int off, const char *fmt, ...) {
    if (off < 0 || (size_t)off + 1 >= cap)
        return (int)cap - 1;
    va_list ap;
    va_start(ap, fmt);
    int wrote = vsnprintf(buf + off, cap - (size_t)off, fmt, ap);
    va_end(ap);
    if (wrote < 0)
        return off;
    off += wrote;
    return (size_t)off + 1 >= cap ? (int)cap - 1 : off;
}

/* Append text from an archive or the host with control characters shown as
 * '?', so names such as macOS "Icon\r" cannot move the cursor. */
static int str_append_safe(char *buf, size_t cap, int off, const char *text) {
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if ((size_t)off + 1 >= cap)
            return (int)cap - 1;
        buf[off++] = (*p < 0x20 || *p == 0x7f) ? '?' : (char)*p;
    }
    buf[off] = '\0';
    return off;
}

static void human_size(off_t s, char *buf, size_t n) {
    const char *unit[] = {"B", "K", "M", "G", "T"};
    double v = (double)s;
    int i = 0;
    while (v >= 1024.0 && i < 4) {
        v /= 1024.0;
        i++;
    }
    if (i == 0)
        snprintf(buf, n, "%lldB", (long long)s);
    else
        snprintf(buf, n, "%.1f%s", v, unit[i]);
}

static const char *change_kind_label(unsigned char kind) {
    switch (kind) {
    case LE_CHANGE_ADDED:    return "added";
    case LE_CHANGE_MODIFIED: return "modified";
    case LE_CHANGE_WHITEOUT: return "whiteout";
    default:                 return NULL;
    }
}

static int multi(const tui_state *st) {
    return st->m->narchives > 1;
}

static void set_msg(tui_state *st, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void set_msg(tui_state *st, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(st->msg, sizeof(st->msg), fmt, ap);
    va_end(ap);
}

/* ---- host paths ---- */

static void expand_tilde(const char *in, char *out, size_t len) {
    const char *home = getenv("HOME");
    if (home && in[0] == '~' && (in[1] == '/' || in[1] == '\0'))
        snprintf(out, len, "%s%s", home, in + 1);
    else
        strlcpy(out, in, len);
}

/* Absolute host path without resolving a final symlink (it may be the thing
 * being added). Returns -1 when the result does not fit. */
static int absolute_host_path(const char *in, char *out, size_t len) {
    char expanded[PATH_MAX], cwd[PATH_MAX];
    expand_tilde(in, expanded, sizeof(expanded));
    size_t needed;
    if (expanded[0] == '/' || !getcwd(cwd, sizeof(cwd)))
        needed = strlcpy(out, expanded, len);
    else
        needed = (size_t)snprintf(out, len, "%s/%s", cwd, expanded);
    return needed < len ? 0 : -1;
}

/* Tab completion: extend buf to the longest prefix shared by matching host
 * paths; directories get a trailing slash. */
static void complete_host_path(char *buf, size_t len) {
    char expanded[PATH_MAX], pattern[PATH_MAX + 2];
    expand_tilde(buf, expanded, sizeof(expanded));
    snprintf(pattern, sizeof(pattern), "%s*", expanded);
    glob_t matches;
    if (glob(pattern, GLOB_MARK, NULL, &matches) != 0 || matches.gl_pathc == 0) {
        globfree(&matches);
        beep();
        return;
    }
    size_t common = strlen(matches.gl_pathv[0]);
    for (size_t i = 1; i < matches.gl_pathc; i++) {
        size_t j = 0;
        while (j < common && matches.gl_pathv[i][j] == matches.gl_pathv[0][j])
            j++;
        common = j;
    }
    if (common > strlen(expanded) && common < len)
        snprintf(buf, len, "%.*s", (int)common, matches.gl_pathv[0]);
    else if (matches.gl_pathc > 1)
        beep();
    globfree(&matches);
}

/* ---- layout ---- */

/* Build the keybinding footer text (single logical line, may wrap on draw). */
static void format_help(const tui_state *st, char *buf, size_t buflen) {
    int off = str_append(buf, buflen, 0,
                         "arrows = move | Enter = open file | d = diff"
                         " | x = drop from layer | u = undo drop or staged"
                         " | a = add host file"
                         " | w = whiteout | / = filter | s = save | q = quit"
                         " | ? = more");
    if (multi(st))
        str_append(buf, buflen, off, " | gold = file in multiple archives");
}

/* Wrap text to width, breaking on spaces when possible. Returns line count. */
static int wrap_text(const char *text, int width,
                     char out[][HELP_LINE_CAP], int max_lines) {
    if (width < 1)
        width = 1;
    if (width >= HELP_LINE_CAP)
        width = HELP_LINE_CAP - 1;

    int nlines = 0;
    const char *p = text;
    while (*p && nlines < max_lines) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;

        int remaining = (int)strlen(p);
        if (remaining <= width || nlines == max_lines - 1) {
            /* Last available line: take what fits (truncate if needed). */
            int take = remaining > width ? width : remaining;
            snprintf(out[nlines], HELP_LINE_CAP, "%.*s", take, p);
            nlines++;
            break;
        }

        /* Prefer a " | " boundary, then any space, then a hard cut. */
        int brk = -1;
        for (int i = width; i >= 3; i--) {
            if (p[i - 3] == ' ' && p[i - 2] == '|' && p[i - 1] == ' ') {
                brk = i - 3;
                break;
            }
        }
        if (brk <= 0) {
            for (int i = width; i > 0; i--) {
                if (p[i] == ' ') {
                    brk = i;
                    break;
                }
            }
        }
        if (brk <= 0)
            brk = width;
        snprintf(out[nlines], HELP_LINE_CAP, "%.*s", brk, p);
        p += brk;
        if (!strncmp(p, " | ", 3))
            p += 3;
        nlines++;
    }
    return nlines < 1 ? 1 : nlines;
}

static int footer_lines(const tui_state *st, char lines[][HELP_LINE_CAP]) {
    char buf[1024];
    format_help(st, buf, sizeof(buf));
    /* Leave one column for the leading pad space on each footer line. */
    int width = (COLS > 1 ? COLS : 80) - 1;
    return wrap_text(buf, width, lines, HELP_MAX_LINES);
}

static int list_height(const tui_state *st) {
    char lines[HELP_MAX_LINES][HELP_LINE_CAP];
    /* header + details + status bar + wrapped keybinding footer */
    int h = LINES - 3 - footer_lines(st, lines);
    return h < 1 ? 1 : h;
}

static void rebuild_rows(tui_state *st) {
    st->nrows = model_flatten(st->m, st->filtering ? st->filter : "",
                              &st->rows, &st->rowcap);
    if (st->nrows == 0)
        st->cursor = 0;
    else if (st->cursor >= st->nrows)
        st->cursor = st->nrows - 1;
}

/* Keep the selected row within the visible window. */
static void clamp_scroll(tui_state *st) {
    size_t h = (size_t)list_height(st);
    if (st->cursor < st->top)
        st->top = st->cursor;
    else if (st->cursor >= st->top + h)
        st->top = st->cursor - h + 1;
}

/* Draw a full-width reverse-video bar (header, status, prompts). */
static void draw_bar(int y, const char *text) {
    move(y, 0);
    clrtoeol();
    attron(A_REVERSE);
    mvaddnstr(y, 0, text, COLS);
    int len = (int)strlen(text);
    if (len > COLS)
        len = COLS;
    for (int x = len; x < COLS; x++)
        mvaddch(y, x, ' ');
    attroff(A_REVERSE);
}

/* ---- rendering ---- */

/* Mark files that belong to 2+ archives, and paint their ancestors so
 * collapsed folders point the way. Directories themselves never warn just for
 * being present in several archives. */
static int mark_shared_warn(le_node *n, int narchives) {
    int slots[LE_MAX_ARCHIVES];
    int self = !node_is_dir(n) && node_archives(n, narchives, slots) > 1;
    int below = 0;
    for (size_t i = 0; i < n->nchildren; i++)
        if (mark_shared_warn(n->children[i], narchives))
            below = 1;
    n->shared_warn = self || below;
    return n->shared_warn;
}

static void refresh_shared_warn(tui_state *st) {
    if (!multi(st))
        return;
    if (st->shared_warn_valid && st->shared_warn_generation == st->m->generation)
        return;
    mark_shared_warn(st->m->root, st->m->narchives);
    st->m->root->shared_warn = 0;   /* root is not a visible row */
    st->shared_warn_generation = st->m->generation;
    st->shared_warn_valid = 1;
}

/* Fixed-width membership column: one slot per archive. See
 * node_archive_slot_char. */
static void archive_badge(const tui_state *st, const le_node *n,
                          char *buf, size_t len) {
    buf[0] = '\0';
    if (!multi(st) || len < (size_t)st->m->narchives + 5)
        return;
    size_t off = 0;
    buf[off++] = '[';
    for (int i = 0; i < st->m->narchives; i++)
        buf[off++] = node_archive_slot_char(n, i);
    buf[off++] = ']';
    buf[off++] = n->mismatch ? '!' : ' ';
    buf[off++] = ' ';
    buf[off] = '\0';
}

typedef struct {
    int removed_somewhere;      /* dropped (directly or via a parent) somewhere */
    int removed_everywhere;     /* every archive holding it drops it */
    int staged;                 /* staged in at least one archive */
} row_state;

static row_state row_state_for(const tui_state *st, const le_node *n) {
    int slots[LE_MAX_ARCHIVES];
    int nslots = node_archives(n, st->m->narchives, slots);
    row_state rs = {0, nslots > 0, 0};
    for (int i = 0; i < nslots; i++) {
        int a = slots[i];
        if (n->arch[a].staged)
            rs.staged = 1;
        if (node_effectively_removed(n, a))
            rs.removed_somewhere = 1;
        else
            rs.removed_everywhere = 0;
    }
    return rs;
}

static int append_whiteout_text(char *line, size_t cap, int off,
                                const le_node *n, const le_meta *meta) {
    off = str_append(line, cap, off, "[wh] ");
    if (!strcmp(n->name, LE_OPAQUE_WHITEOUT)) {
        off = str_append_safe(line, cap, off, n->name);
        return str_append(line, cap, off, "   (opaque marker; Anka ignores it)");
    }
    off = str_append_safe(line, cap, off, node_whiteout_target(n));
    if (meta->kind == LE_KIND_DIR)
        return str_append(line, cap, off,
                          "/   (deletes this directory from the base, if empty)");
    return str_append(line, cap, off, "   (deletes it from the base image)");
}

static int append_entry_text(char *line, size_t cap, int off,
                             const le_node *n, const le_meta *meta) {
    off = str_append_safe(line, cap, off, n->name);
    switch (meta->kind) {
    case LE_KIND_DIR:
        return str_append(line, cap, off, "/");
    case LE_KIND_SYMLINK:
    case LE_KIND_HARDLINK:
        off = str_append(line, cap, off, meta->kind == LE_KIND_SYMLINK
                                             ? " -> " : " => ");
        return str_append_safe(line, cap, off, meta->link ? meta->link : "");
    case LE_KIND_OTHER:
        return str_append(line, cap, off, "   (special file)");
    default: {
        char sz[32];
        human_size(meta->size, sz, sizeof(sz));
        return str_append(line, cap, off, "   (%s)", sz);
    }
    }
}

static void draw_row(tui_state *st, int screen_y, size_t idx) {
    le_row *r = &st->rows[idx];
    le_node *n = r->node;
    int selected = (idx == st->cursor);
    int whiteout = node_is_whiteout(n);
    const le_meta *meta = node_display_meta(n, st->m->narchives);
    row_state rs = row_state_for(st, n);

    char badge[LE_MAX_ARCHIVES + 8];
    archive_badge(st, n, badge, sizeof(badge));

    char line[4096];
    int off;
    if (multi(st))
        off = str_append(line, sizeof(line), 0, "%s", badge);
    else {
        char flag = rs.removed_somewhere ? 'X' : ' ';
        off = str_append(line, sizeof(line), 0, "%c %s", flag, badge);
    }
    for (int d = 0; d < r->depth; d++)
        off = str_append(line, sizeof(line), off, "  ");

    const char *marker = "  ";
    if (!whiteout && node_is_dir(n))
        marker = (n->expanded || st->filtering) ? "v " : "> ";
    off = str_append(line, sizeof(line), off, "%s%s", marker,
                     rs.staged ? "[+] " : "");
    if (whiteout)
        append_whiteout_text(line, sizeof(line), off, n, meta);
    else
        append_entry_text(line, sizeof(line), off, n, meta);

    int attr = 0;
    int fill_row = selected;
    if (selected && have_select_color)
        attr = COLOR_PAIR(PAIR_SELECTED) | A_BOLD;
    else if (selected)
        attr = A_REVERSE | A_BOLD;
    else if (rs.removed_everywhere)
        attr = A_DIM;
    else {
        int color_pair = whiteout ? PAIR_WHITEOUT
                         : n->shared_warn ? PAIR_SHARED : 0;
        if (color_pair && have_colors) {
            attr = COLOR_PAIR(color_pair);
            if (color_pair == PAIR_SHARED)
                fill_row = 1;
        } else if (color_pair)
            attr = A_BOLD;          /* no color: keep these rows obvious */
    }

    if (attr)
        attron(attr);
    mvaddnstr(screen_y, 0, line, COLS);
    if (fill_row) {                 /* fill row for a solid highlight bar */
        int len = (int)strlen(line);
        for (int x = len; x < COLS; x++)
            addch(' ');
    }
    if (attr)
        attroff(attr);
}

/* What one archive holds and plans for the node, e.g. "added, dropped".
 * Empty when the archive only holds the path, with no change kind and no
 * edit. */
static int format_arch_state(char *buf, size_t cap, const le_node *n,
                             int arch_idx) {
    const le_arch_state *a = &n->arch[arch_idx];
    int off = 0, wrote_any = 0;
    buf[0] = '\0';
#define STATE_PART(text) \
    do { off = str_append(buf, cap, off, "%s%s", wrote_any ? ", " : "", text); \
         wrote_any = 1; } while (0)
    const char *kind = a->present ? change_kind_label(a->change_kind) : NULL;
    if (kind)
        STATE_PART(kind);
    if (a->staged)
        STATE_PART(a->staged->source == LE_STAGED_WHITEOUT ? "staged whiteout"
                                                           : "staged");
    if (a->removed)
        STATE_PART("dropped");
    else if (node_effectively_removed(n, arch_idx))
        STATE_PART("dropped with parent");
#undef STATE_PART
    return off;
}

/* Details for the selected row: full path, the archives that hold it with
 * their state, mode, and owner. One archive: only the state, no name. */
static void format_details(const tui_state *st, char *buf, size_t cap) {
    buf[0] = '\0';
    if (st->nrows == 0)
        return;
    const le_node *n = st->rows[st->cursor].node;
    char path[PATH_MAX];
    node_path(n, path, sizeof(path));
    int off = str_append(buf, cap, 0, " ");
    off = str_append_safe(buf, cap, off, path);

    int slots[LE_MAX_ARCHIVES];
    int nslots = node_archives(n, st->m->narchives, slots);
    char state[256];
    if (multi(st)) {
        for (int i = 0; i < nslots; i++) {
            off = str_append(buf, cap, off, "%s", i == 0 ? "   in " : ", ");
            off = str_append_safe(buf, cap, off,
                                  session_label(st->sess, slots[i]));
            if (format_arch_state(state, sizeof(state), n, slots[i]))
                off = str_append(buf, cap, off, " (%s)", state);
        }
    } else if (nslots == 1 && format_arch_state(state, sizeof(state), n, slots[0])) {
        off = str_append(buf, cap, off, "   %s", state);
    }

    const le_meta *meta = node_display_meta(n, st->m->narchives);
    if (n->meta_set || meta != &n->meta)
        str_append(buf, cap, off, "   mode %04o   owner %u:%u",
                   (unsigned)(meta->mode & 07777), (unsigned)meta->uid,
                   (unsigned)meta->gid);
}

static void draw_help(const tui_state *st) {
    const char *lines[] = {
        "  anka-oci-tag-editor - help",
        "",
        "  Up/Down, j/k    Move selection",
        "  Right / l       Expand directory (or step into)",
        "  Left / h        Collapse directory (or jump to parent)",
        "  PgUp/PgDn       Page up / down",
        "  g / G           Jump to top / bottom",
        "  Enter           Open the selected file (less-like preview)",
        "  d               Side-by-side diff of the file across two archives",
        "  x               Drop the path from the layer on save (toggles X)",
        "  u               Undo a drop (X), or cancel a staged add or whiteout",
        "  a               Add a host file at a destination path",
        "  w               Stage a whiteout: delete a path from the base image",
        "  /               Filter by substring (empty clears)",
        "  s               Save every changed archive in place",
        "  q               Quit (confirms if unsaved)",
        "  ?               Toggle this help",
        "",
        "  In prompts: Enter accepts, Esc cancels, Tab completes host paths.",
        "  Legend: 'X' dropped on save, '[+]' staged, '[wh]' whiteout (rose).",
        NULL,
    };
    const char *multi_lines[] = {
        "",
        "  Membership: [12] both, [1.] first only, [X2] dropped from first,",
        "  [.+] staged into the second. Gold bar: file in more than one archive.",
        "  '!' means the entries disagree on type/size.",
        "  x on a shared path asks which archive (or all of them);",
        "  adding a file or a whiteout asks which archive receives it.",
        NULL,
    };
    erase();
    int y = 1;
    for (int i = 0; lines[i]; i++)
        mvaddnstr(y++, 2, lines[i], COLS - 4);
    if (multi(st))
        for (int i = 0; multi_lines[i]; i++)
            mvaddnstr(y++, 2, multi_lines[i], COLS - 4);
    mvaddnstr(y + 1, 2, "  Press any key to close help", COLS - 4);
    refresh();
}

static void draw(tui_state *st) {
    if (st->show_help) {
        draw_help(st);
        return;
    }

    erase();
    refresh_shared_warn(st);

    /* Header: the archive (or archives) being edited, '*' when unsaved. */
    char title[1024];
    int off = str_append(title, sizeof(title), 0, " anka-oci-tag-editor - ");
    if (multi(st)) {
        for (int i = 0; i < st->sess->count; i++) {
            off = str_append(title, sizeof(title), off, "%d:", i + 1);
            off = str_append_safe(title, sizeof(title), off,
                                  session_label(st->sess, i));
            off = str_append(title, sizeof(title), off, "%s  ",
                             model_arch_dirty(st->m, i) ? "*" : "");
        }
    } else {
        off = str_append_safe(title, sizeof(title), off, session_label(st->sess, 0));
        str_append(title, sizeof(title), off, "%s ",
                   model_dirty(st->m) ? "  [modified]" : "");
    }
    draw_bar(0, title);

    char footer[HELP_MAX_LINES][HELP_LINE_CAP];
    int footer_n = footer_lines(st, footer);

    /* List. */
    int h = list_height(st);
    if (st->nrows == 0) {
        mvaddnstr(1, 2, st->filtering ? "(no matches)" : "(empty archive)",
                  COLS - 2);
    } else {
        for (int i = 0; i < h; i++) {
            size_t idx = st->top + (size_t)i;
            if (idx >= st->nrows)
                break;
            draw_row(st, i + 1, idx);
        }
    }

    /* Details of the selection, then counts + transient message. */
    char details[1024];
    format_details(st, details, sizeof(details));
    draw_bar(LINES - 2 - footer_n, details);

    char status[1024];
    off = str_append(status, sizeof(status), 0,
                     " %zu paths | drop: %zu | staged: %zu",
                     st->m->total, st->m->marked, st->m->added);
    if (st->filtering) {
        off = str_append(status, sizeof(status), off, " | filter: \"");
        off = str_append_safe(status, sizeof(status), off, st->filter);
        off = str_append(status, sizeof(status), off, "\"");
    }
    if (st->msg[0]) {
        off = str_append(status, sizeof(status), off, " | ");
        str_append_safe(status, sizeof(status), off, st->msg);
    }
    draw_bar(LINES - 1 - footer_n, status);

    /* Wrapped keybinding footer (leading space matches the status bar). */
    for (int i = 0; i < footer_n; i++) {
        char padded[HELP_LINE_CAP + 1];
        str_append(padded, sizeof(padded), 0, " %s", footer[i]);
        draw_bar(LINES - footer_n + i, padded);
    }

    refresh();
}

/* ---- input dialogs ---- */

static int is_utf8_continuation(char c) {
    return ((unsigned char)c & 0xC0) == 0x80;
}

typedef void (*prompt_completer)(char *buf, size_t len);

/* Edit buf (which holds the initial text) on the bottom row. Enter accepts,
 * Esc cancels, Tab calls complete when given. Returns 1 when accepted, 0 on
 * cancel. Empty Enter is accepted only when accept_empty is set. */
static int prompt_edit_ex(const char *label, char *buf, size_t len,
                          prompt_completer complete, int accept_empty) {
    size_t n = strlen(buf), pos = n;
    int y = LINES - 1, accepted = -1;
    curs_set(1);
    while (accepted < 0) {
        draw_bar(y, label);
        int label_cols = (int)strlen(label);
        if (label_cols > COLS - 2)
            label_cols = COLS - 2;
        size_t visible = (size_t)(COLS - label_cols - 1);
        size_t start = pos > visible ? pos - visible : 0;
        move(y, label_cols);
        clrtoeol();
        mvaddnstr(y, label_cols, buf + start, (int)visible);
        move(y, label_cols + (int)(pos - start));
        refresh();

        int ch = read_key();
        if (ch == KEY_STOP) {
            accepted = 0;
            break;
        }
        switch (ch) {
        case KEY_ESCAPE:
            accepted = 0;
            break;
        case '\n': case '\r': case KEY_ENTER:
            accepted = accept_empty || buf[0] != '\0';
            break;
        case KEY_BACKSPACE: case 127: case KEY_CTRL('h'):
            while (pos > 0) {
                char removed = buf[pos - 1];
                memmove(buf + pos - 1, buf + pos, n - pos + 1);
                pos--;
                n--;
                if (!is_utf8_continuation(removed))
                    break;
            }
            break;
        case KEY_DC:
            while (pos < n) {
                memmove(buf + pos, buf + pos + 1, n - pos);
                n--;
                if (pos >= n || !is_utf8_continuation(buf[pos]))
                    break;
            }
            break;
        case KEY_LEFT:
            while (pos > 0 && is_utf8_continuation(buf[--pos]))
                ;
            break;
        case KEY_RIGHT:
            while (pos < n && is_utf8_continuation(buf[++pos]))
                ;
            break;
        case KEY_HOME: case KEY_CTRL('a'):
            pos = 0;
            break;
        case KEY_END: case KEY_CTRL('e'):
            pos = n;
            break;
        case KEY_CTRL('u'):
            buf[0] = '\0';
            n = pos = 0;
            break;
        case '\t':
            if (complete) {
                complete(buf, len);
                n = pos = strlen(buf);
            }
            break;
        default:
            if (ch >= 0x20 && ch < 0x100 && ch != 0x7f && n + 1 < len) {
                memmove(buf + pos + 1, buf + pos, n - pos + 1);
                buf[pos++] = (char)ch;
                n++;
            }
            break;
        }
    }
    curs_set(0);
    return accepted;
}

static int prompt_edit(const char *label, char *buf, size_t len,
                       prompt_completer complete) {
    return prompt_edit_ex(label, buf, len, complete, 0);
}

static int confirm(const char *question) {
    draw_bar(LINES - 1, question);
    int ch = read_key();
    return ch == 'y' || ch == 'Y';
}

/* Ask which of the candidate archives an action applies to. Returns an archive
 * index, PICK_ALL when allow_all and the user chose all, or PICK_CANCEL. */
static int prompt_archive(tui_state *st, const char *action, const int *slots,
                          int nslots, int allow_all) {
    char line[1024];
    int off = str_append(line, sizeof(line), 0, " %s: ", action);
    for (int i = 0; i < nslots; i++) {
        off = str_append(line, sizeof(line), off, "[%d] ", slots[i] + 1);
        off = str_append_safe(line, sizeof(line), off,
                              session_label(st->sess, slots[i]));
        off = str_append(line, sizeof(line), off, "  ");
    }
    if (allow_all)
        off = str_append(line, sizeof(line), off, "[a] all  ");
    str_append(line, sizeof(line), off, "(Esc cancels)");
    draw_bar(LINES - 1, line);

    int ch = read_key();
    if (ch == KEY_STOP)
        return PICK_CANCEL;
    if (allow_all && (ch == 'a' || ch == 'A'))
        return PICK_ALL;
    if (ch >= '1' && ch <= '9') {
        int idx = ch - '1';
        for (int i = 0; i < nslots; i++)
            if (slots[i] == idx)
                return idx;
    }
    return PICK_CANCEL;
}

/* Archive that receives a new entry: the only one, or the user's pick. */
static int pick_receiving_archive(tui_state *st, const char *action) {
    if (!multi(st))
        return 0;
    int slots[LE_MAX_ARCHIVES];
    for (int i = 0; i < st->m->narchives; i++)
        slots[i] = i;
    return prompt_archive(st, action, slots, st->m->narchives, 0);
}

/* ---- navigation ---- */

static void move_to_parent(tui_state *st) {
    if (st->nrows == 0)
        return;
    le_node *parent = st->rows[st->cursor].node->parent;
    if (!parent || !parent->parent)     /* top-level: no visible parent */
        return;
    for (size_t i = 0; i < st->cursor; i++)
        if (st->rows[i].node == parent) {
            st->cursor = i;
            return;
        }
}

static void select_node(tui_state *st, le_node *target) {
    for (size_t i = 0; i < st->nrows; i++)
        if (st->rows[i].node == target) {
            st->cursor = i;
            return;
        }
}

/* Show a newly staged node: clear a filter it may not match, then select it. */
static void reveal_node(tui_state *st, le_node *n) {
    st->filtering = 0;
    st->filter[0] = '\0';
    rebuild_rows(st);
    select_node(st, n);
}

/* Directory a new entry goes into by default: the selected directory, or the
 * parent of the selected entry. */
static void selected_directory(const le_node *cur, char *buf, size_t len) {
    buf[0] = '\0';
    if (!cur)
        return;
    const le_node *dir = (node_is_dir(cur) && !node_is_whiteout(cur)) ? cur
                                                                        : cur->parent;
    node_path(dir, buf, len);
}

static void join_guest_path(const char *dir, const char *name, char *out,
                            size_t len) {
    snprintf(out, len, "%s%s%s", dir, dir[0] ? "/" : "", name);
}

static void append_slash(char *path, size_t len) {
    size_t used = strlen(path);
    if (used + 1 < len) {
        path[used] = '/';
        path[used + 1] = '\0';
    }
}

/* Remove trailing slashes; returns 1 when there were any. */
static int strip_trailing_slashes(char *path) {
    size_t len = strlen(path);
    int had_slash = 0;
    while (len > 1 && path[len - 1] == '/') {
        path[--len] = '\0';
        had_slash = 1;
    }
    return had_slash;
}

/* ---- actions ---- */

static int preview_page_rows(void) {
    int h = LINES - 2;
    return h < 1 ? 1 : h;
}

static void draw_preview(const char *path, const char *label, int truncated,
                         preview_view *v, const char *status) {
    erase();
    char header[1024];
    int off = str_append(header, sizeof(header), 0, " %s", path);
    if (label && label[0]) {
        off = str_append(header, sizeof(header), off, "  ");
        off = str_append_safe(header, sizeof(header), off, label);
    }
    if (truncated)
        str_append(header, sizeof(header), off, "  (truncated)");
    draw_bar(0, header);

    int page = preview_page_rows();
    char line[4096];
    for (int i = 0; i < page; i++) {
        size_t row = v->top + (size_t)i;
        if (row >= v->layout.nrows)
            break;
        size_t n = 0;
        int hl_col = 0, hl_n = 0;
        preview_row_render(&v->layout, row, v->match_off, v->match_len,
                           v->has_match, line, sizeof(line), &n, &hl_col, &hl_n);
        mvaddnstr(1 + i, 0, line, COLS);
        if (hl_n > 0 && hl_col < COLS) {
            attron(A_REVERSE);
            mvaddnstr(1 + i, hl_col, line + hl_col, hl_n);
            attroff(A_REVERSE);
        }
    }

    char footer[1024];
    if (status && status[0])
        snprintf(footer, sizeof(footer), " %s", status);
    else
        snprintf(footer, sizeof(footer),
                 " q close | / search | n next | j/k line | space page");
    draw_bar(LINES - 1, footer);
    refresh();
}

static void preview_apply_search(preview_view *v, const char *pattern,
                                 int forward, char *status, size_t status_len) {
    char err[256] = "";
    int rc = preview_view_search(v, pattern, forward, 1, err, sizeof(err));
    if (rc == -1)
        strlcpy(status, err, status_len);
    else if (rc == 1)
        strlcpy(status, "Pattern not found", status_len);
    else
        status[0] = '\0';
}

static void preview_do_search(preview_view *v, int forward, char *last_pat,
                              size_t last_len, int *last_fwd, char *status,
                              size_t status_len) {
    char pat[256] = "";
    if (!prompt_edit_ex(forward ? "/" : "?", pat, sizeof(pat), NULL, 1)) {
        status[0] = '\0';
        return;
    }
    if (!pat[0]) {
        if (!last_pat[0]) {
            strlcpy(status, "no previous pattern", status_len);
            return;
        }
        strlcpy(pat, last_pat, sizeof(pat));
    } else {
        strlcpy(last_pat, pat, last_len);
    }
    *last_fwd = forward;
    preview_apply_search(v, pat, forward, status, status_len);
}

static void preview_search_again(preview_view *v, int reverse,
                                 const char *last_pat, int last_fwd,
                                 char *status, size_t status_len) {
    if (!last_pat[0]) {
        strlcpy(status, "no previous pattern", status_len);
        return;
    }
    preview_apply_search(v, last_pat, reverse ? !last_fwd : last_fwd, status,
                         status_len);
}

static void run_file_preview(const char *path, const char *label,
                             le_member_bytes *bytes) {
    preview_view view;
    preview_view_init(&view, bytes->data, bytes->len);
    char last_pat[256] = "";
    int last_fwd = 1;
    char status[256] = "";
    for (;;) {
        int page = preview_page_rows();
        int width = COLS < 1 ? 1 : COLS;
        if (view.layout.width != width)
            preview_view_set_width(&view, width);
        preview_view_move_line(&view, 0, page);
        draw_preview(path, label, bytes->truncated, &view, status);

        int ch = read_key();
        if (ch == KEY_STOP)
            break;
        status[0] = '\0';
        switch (ch) {
        case KEY_DOWN:
        case 'j':
        case '\n':
        case '\r':
        case KEY_ENTER:
            preview_view_move_line(&view, 1, page);
            break;
        case KEY_UP:
        case 'k':
            preview_view_move_line(&view, -1, page);
            break;
        case ' ':
        case 'f':
        case KEY_NPAGE:
            preview_view_page(&view, 1, page);
            break;
        case 'b':
        case KEY_PPAGE:
            preview_view_page(&view, -1, page);
            break;
        case 'g':
            preview_view_home(&view);
            break;
        case 'G':
            preview_view_end(&view, page);
            break;
        case '/':
            preview_do_search(&view, 1, last_pat, sizeof(last_pat), &last_fwd,
                              status, sizeof(status));
            break;
        case '?':
            preview_do_search(&view, 0, last_pat, sizeof(last_pat), &last_fwd,
                              status, sizeof(status));
            break;
        case 'n':
            preview_search_again(&view, 0, last_pat, last_fwd, status,
                                 sizeof(status));
            break;
        case 'N':
            preview_search_again(&view, 1, last_pat, last_fwd, status,
                                 sizeof(status));
            break;
        case 'q':
        case KEY_ESCAPE:
            preview_view_free(&view);
            return;
        default:
            break;
        }
    }
    preview_view_free(&view);
}

static int side_page_rows(void) {
    int h = LINES - 3;
    return h < 1 ? 1 : h;
}

static int side_pair_at(const side_view *v, size_t disp, size_t *pair,
                        int *wrap_i) {
    size_t npair = v->diff ? v->diff->nrows : 0;
    if (!npair || !v->pair_disp0 || disp >= v->ndisp)
        return 0;
    size_t lo = 0, hi = npair;
    while (lo + 1 < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (v->pair_disp0[mid] <= disp)
            lo = mid;
        else
            hi = mid;
    }
    *pair = lo;
    *wrap_i = (int)(disp - v->pair_disp0[lo]);
    return 1;
}

static void draw_side_cell(int y, int x, int width, const char *text,
                           int wrap_i, int color, int has_match,
                           size_t match_off, size_t match_len) {
    if (width < 1)
        return;
    preview_layout lay = {0};
    const char *s = text ? text : "";
    preview_layout_build(&lay, s, strlen(s), width);
    char line[4096];
    size_t n = 0;
    int hl_col = 0, hl_n = 0;
    if (wrap_i >= 0 && (size_t)wrap_i < lay.nrows)
        preview_row_render(&lay, (size_t)wrap_i, match_off, match_len,
                           has_match, line, sizeof(line), &n, &hl_col, &hl_n);
    if (color && have_colors)
        attron(COLOR_PAIR(color));
    move(y, x);
    int shown = (int)n;
    if (shown > width)
        shown = width;
    if (shown > 0)
        addnstr(line, shown);
    for (int i = shown; i < width; i++)
        addch(' ');
    if (color && have_colors)
        attroff(COLOR_PAIR(color));
    if (hl_n > 0 && hl_col < width) {
        int hn = hl_n;
        if (hl_col + hn > width)
            hn = width - hl_col;
        if (hn > 0) {
            attron(A_REVERSE);
            mvaddnstr(y, x + hl_col, line + hl_col, hn);
            attroff(A_REVERSE);
        }
    }
    preview_layout_free(&lay);
}

static void draw_side_label(int y, int x, int width, const char *label) {
    const char *s = label ? label : "";
    attron(A_BOLD);
    mvaddnstr(y, x, s, width);
    int n = (int)strlen(s);
    if (n > width)
        n = width;
    for (int i = n; i < width; i++)
        mvaddch(y, x + i, ' ');
    attroff(A_BOLD);
}

static void draw_side_preview(const char *path, const char *left_label,
                              const char *right_label, int truncated,
                              side_view *v, const char *status) {
    erase();
    char header[1024];
    int off = str_append(header, sizeof(header), 0, " %s", path);
    if (truncated)
        str_append(header, sizeof(header), off, "  (truncated)");
    draw_bar(0, header);
    draw_side_label(1, 0, v->left_width, left_label);
    mvaddch(1, v->left_width, '|');
    draw_side_label(1, v->left_width + 1, v->right_width, right_label);

    int page = side_page_rows();
    for (int i = 0; i < page; i++) {
        size_t disp = v->top + (size_t)i;
        size_t pair = 0;
        int wrap_i = 0;
        int y = 2 + i;
        if (!side_pair_at(v, disp, &pair, &wrap_i))
            break;
        const side_row *row = &v->diff->rows[pair];
        int left_color = 0, right_color = 0;
        if (row->kind == SIDE_DEL || row->kind == SIDE_CHG)
            left_color = PAIR_WHITEOUT;
        if (row->kind == SIDE_INS || row->kind == SIDE_CHG)
            right_color = PAIR_SHARED;
        int hit_left = v->has_match && v->match_pair == pair &&
                       v->match_side == 0;
        int hit_right = v->has_match && v->match_pair == pair &&
                        v->match_side == 1;
        draw_side_cell(y, 0, v->left_width, row->left, wrap_i, left_color,
                       hit_left, v->match_off, v->match_len);
        mvaddch(y, v->left_width, '|');
        draw_side_cell(y, v->left_width + 1, v->right_width, row->right, wrap_i,
                       right_color, hit_right, v->match_off, v->match_len);
    }

    char footer[1024];
    if (status && status[0])
        snprintf(footer, sizeof(footer), " %s", status);
    else
        snprintf(footer, sizeof(footer),
                 " q close | / search | n next | j/k line | space page");
    draw_bar(LINES - 1, footer);
    refresh();
}

static void side_apply_search(side_view *v, const char *pattern, int forward,
                              char *status, size_t status_len) {
    char err[256] = "";
    int rc = side_view_search(v, pattern, forward, 1, err, sizeof(err));
    if (rc == -1)
        strlcpy(status, err, status_len);
    else if (rc == 1)
        strlcpy(status, "Pattern not found", status_len);
    else
        status[0] = '\0';
}

static void side_do_search(side_view *v, int forward, char *last_pat,
                          size_t last_len, int *last_fwd, char *status,
                          size_t status_len) {
    char pat[256] = "";
    if (!prompt_edit_ex(forward ? "/" : "?", pat, sizeof(pat), NULL, 1)) {
        status[0] = '\0';
        return;
    }
    if (!pat[0]) {
        if (!last_pat[0]) {
            strlcpy(status, "no previous pattern", status_len);
            return;
        }
        strlcpy(pat, last_pat, sizeof(pat));
    } else {
        strlcpy(last_pat, pat, last_len);
    }
    *last_fwd = forward;
    side_apply_search(v, pat, forward, status, status_len);
}

static void side_search_again(side_view *v, int reverse, const char *last_pat,
                              int last_fwd, char *status, size_t status_len) {
    if (!last_pat[0]) {
        strlcpy(status, "no previous pattern", status_len);
        return;
    }
    side_apply_search(v, last_pat, reverse ? !last_fwd : last_fwd, status,
                      status_len);
}

static void run_side_preview(const char *path, const char *left_label,
                             const char *right_label, int truncated,
                             side_diff *diff) {
    side_view view;
    side_view_init(&view, diff);
    char last_pat[256] = "";
    int last_fwd = 1;
    char status[256] = "";
    for (;;) {
        int page = side_page_rows();
        int width = COLS < 1 ? 1 : COLS;
        if (view.total_cols != width)
            side_view_set_width(&view, width);
        side_view_move_line(&view, 0, page);
        draw_side_preview(path, left_label, right_label, truncated, &view,
                          status);

        int ch = read_key();
        if (ch == KEY_STOP)
            break;
        status[0] = '\0';
        switch (ch) {
        case KEY_DOWN:
        case 'j':
        case '\n':
        case '\r':
        case KEY_ENTER:
            side_view_move_line(&view, 1, page);
            break;
        case KEY_UP:
        case 'k':
            side_view_move_line(&view, -1, page);
            break;
        case ' ':
        case 'f':
        case KEY_NPAGE:
            side_view_page(&view, 1, page);
            break;
        case 'b':
        case KEY_PPAGE:
            side_view_page(&view, -1, page);
            break;
        case 'g':
            side_view_home(&view);
            break;
        case 'G':
            side_view_end(&view, page);
            break;
        case '/':
            side_do_search(&view, 1, last_pat, sizeof(last_pat), &last_fwd,
                           status, sizeof(status));
            break;
        case '?':
            side_do_search(&view, 0, last_pat, sizeof(last_pat), &last_fwd,
                           status, sizeof(status));
            break;
        case 'n':
            side_search_again(&view, 0, last_pat, last_fwd, status,
                              sizeof(status));
            break;
        case 'N':
            side_search_again(&view, 1, last_pat, last_fwd, status,
                              sizeof(status));
            break;
        case 'q':
        case KEY_ESCAPE:
            side_view_free(&view);
            return;
        default:
            break;
        }
    }
    side_view_free(&view);
}

static int load_preview_bytes(tui_state *st, const le_node *n, int slot,
                              const char *path, le_member_bytes *out,
                              char *err, size_t errlen) {
    const le_staged *s = n->arch[slot].staged;
    if (s) {
        if (s->source == LE_STAGED_WHITEOUT) {
            le_seterr(err, errlen, "cannot preview a whiteout");
            return -1;
        }
        if (!s->host_path) {
            le_seterr(err, errlen, "staged file has no host path");
            return -1;
        }
        return member_read_from_host(s->host_path, MEMBER_PREVIEW_MAX, out, err,
                                     errlen);
    }
    return store_read_member(session_store(st->sess, slot), path, out, err,
                             errlen);
}

static void action_preview(tui_state *st, le_node *n) {
    const char *block_reason = preview_block_reason(n);
    if (block_reason) {
        set_msg(st, "%s", block_reason);
        return;
    }
    int slots[LE_MAX_ARCHIVES];
    int nslots = node_archives(n, st->m->narchives, slots);
    if (nslots == 0) {
        set_msg(st, "not present in any archive");
        return;
    }
    int slot = slots[0];
    if (preview_needs_archive_choice(n, nslots)) {
        slot = prompt_archive(st, "Preview which archive", slots, nslots, 0);
        if (slot == PICK_CANCEL) {
            set_msg(st, "cancelled");
            return;
        }
    }
    char path[PATH_MAX];
    if (node_path(n, path, sizeof(path)) == -1) {
        set_msg(st, "path too deep");
        return;
    }
    le_member_bytes bytes = {0};
    char err[512] = "";
    if (load_preview_bytes(st, n, slot, path, &bytes, err, sizeof(err)) != 0) {
        set_msg(st, "%s", err);
        return;
    }
    if (member_bytes_is_binary(&bytes)) {
        member_bytes_free(&bytes);
        set_msg(st, "binary file; not shown");
        return;
    }
    run_file_preview(path, session_label(st->sess, slot), &bytes);
    member_bytes_free(&bytes);
}

static int pick_diff_slots(tui_state *st, const int *slots, int nslots,
                           int *left, int *right) {
    if (nslots == 2) {
        *left = slots[0];
        *right = slots[1];
        return 1;
    }
    int first = prompt_archive(st, "Diff left archive", slots, nslots, 0);
    if (first == PICK_CANCEL)
        return 0;
    int rest[LE_MAX_ARCHIVES], nrest = 0;
    for (int i = 0; i < nslots; i++)
        if (slots[i] != first)
            rest[nrest++] = slots[i];
    int second = prompt_archive(st, "Diff right archive", rest, nrest, 0);
    if (second == PICK_CANCEL)
        return 0;
    *left = first;
    *right = second;
    return 1;
}

static void action_diff(tui_state *st, le_node *n) {
    int slots[LE_MAX_ARCHIVES];
    int nslots = node_archives(n, st->m->narchives, slots);
    const char *block = preview_diff_block(n, nslots);
    if (block) {
        set_msg(st, "%s", block);
        return;
    }
    int left, right;
    if (!pick_diff_slots(st, slots, nslots, &left, &right)) {
        set_msg(st, "cancelled");
        return;
    }
    char path[PATH_MAX];
    if (node_path(n, path, sizeof(path)) == -1) {
        set_msg(st, "path too deep");
        return;
    }
    le_member_bytes a = {0}, b = {0};
    char err[512] = "";
    if (load_preview_bytes(st, n, left, path, &a, err, sizeof(err)) != 0) {
        set_msg(st, "%s", err);
        return;
    }
    if (load_preview_bytes(st, n, right, path, &b, err, sizeof(err)) != 0) {
        member_bytes_free(&a);
        set_msg(st, "%s", err);
        return;
    }
    if (member_bytes_is_binary(&a) || member_bytes_is_binary(&b)) {
        member_bytes_free(&a);
        member_bytes_free(&b);
        set_msg(st, "binary file; not shown");
        return;
    }
    int truncated = a.truncated || b.truncated;
    side_diff diff = {0};
    int rc = side_diff_build(a.data, a.len, b.data, b.len, &diff);
    member_bytes_free(&a);
    member_bytes_free(&b);
    if (rc == SIDE_DIFF_IDENTICAL) {
        set_msg(st, "files are identical");
        return;
    }
    if (rc != SIDE_DIFF_OK) {
        set_msg(st, "out of memory");
        return;
    }
    run_side_preview(path, session_label(st->sess, left),
                     session_label(st->sess, right), truncated, &diff);
    side_diff_free(&diff);
}

/* True, with the reason in the message bar, when x would drop n in slot
 * and the slot's store does not allow that. */
static int store_blocks_mark(tui_state *st, const le_node *n, int slot) {
    const le_arch_state *state = &n->arch[slot];
    if (state->staged || !state->present || state->removed)
        return 0;
    char err[512] = "";
    if (store_may_remove(session_store(st->sess, slot), n, slot, err,
                         sizeof(err)) == 0)
        return 0;
    set_msg(st, "%s", err);
    return 1;
}

static int slot_is_staged(const le_node *n, int slot) {
    return n->arch[slot].staged != NULL;
}

static int slot_is_inherited_drop(const le_node *n, int slot) {
    return !n->arch[slot].removed && node_drop_parent(n, slot) != NULL;
}

static int staged_is_whiteout(const le_node *n, int slot) {
    return n->arch[slot].staged &&
           n->arch[slot].staged->source == LE_STAGED_WHITEOUT;
}

static void set_action_msg(tui_state *st, const char *text, int pick,
                           int nslots) {
    if (pick == PICK_ALL)
        set_msg(st, "%s in all %d archives", text, nslots);
    else if (pick >= 0)
        set_msg(st, "%s in %s", text, session_label(st->sess, pick));
    else
        set_msg(st, "%s", text);
}

static void msg_inherited_drop(tui_state *st, const le_node *n, int slot) {
    const le_node *parent = node_drop_parent(n, slot);
    char path[PATH_MAX] = "";
    if (parent)
        node_path(parent, path, sizeof(path));
    if (path[0])
        set_msg(st, "dropped with parent %s; press u on the parent to keep it",
                path);
    else
        set_msg(st, "dropped with parent; press u on the parent to keep it");
}

static void action_drop(tui_state *st, le_node *n) {
    int slots[LE_MAX_ARCHIVES];
    int nslots = node_archives(n, st->m->narchives, slots);
    if (nslots == 0) {
        set_msg(st, "not present in any archive");
        return;
    }
    int pick;
    if (nslots == 1)
        pick = slots[0];
    else {
        pick = prompt_archive(st, "Drop from", slots, nslots, 1);
        if (pick == PICK_CANCEL) {
            set_msg(st, "cancelled");
            return;
        }
    }

    int apply[LE_MAX_ARCHIVES], napply = 0;
    for (int i = 0; i < nslots; i++)
        if (pick == PICK_ALL || pick == slots[i])
            apply[napply++] = slots[i];

    int nstaged = 0, nwhiteout = 0, ninherit = 0, ndrop = 0;
    int drop_slots[LE_MAX_ARCHIVES];
    int inherit_slot = -1;
    for (int i = 0; i < napply; i++) {
        int s = apply[i];
        if (slot_is_staged(n, s)) {
            nstaged++;
            if (staged_is_whiteout(n, s))
                nwhiteout++;
            continue;
        }
        if (slot_is_inherited_drop(n, s)) {
            ninherit++;
            if (inherit_slot < 0)
                inherit_slot = s;
            continue;
        }
        if (n->arch[s].present)
            drop_slots[ndrop++] = s;
    }

    if (ndrop == 0) {
        if (nstaged)
            set_msg(st, nwhiteout == nstaged
                            ? "press u to cancel this whiteout"
                            : "press u to cancel this add");
        else if (ninherit)
            msg_inherited_drop(st, n, inherit_slot);
        else
            set_msg(st, "nothing to change");
        return;
    }

    for (int i = 0; i < ndrop; i++)
        if (store_blocks_mark(st, n, drop_slots[i]))
            return;

    int want_remove = 1;
    for (int i = 0; i < ndrop; i++)
        if (n->arch[drop_slots[i]].removed) {
            want_remove = 0;
            break;
        }
    for (int i = 0; i < ndrop; i++)
        if (!!n->arch[drop_slots[i]].removed != want_remove)
            model_toggle_removed(st->m, n, drop_slots[i]);

    rebuild_rows(st);
    set_action_msg(st, want_remove ? "will drop from the layer on save"
                                   : "kept in the layer",
                   nslots == 1 ? -1 : pick, nslots);
}

static void action_undo(tui_state *st, le_node *n) {
    int undo_slots[LE_MAX_ARCHIVES], nundo = 0;
    for (int i = 0; i < st->m->narchives; i++)
        if (n->arch[i].staged || n->arch[i].removed)
            undo_slots[nundo++] = i;
    if (nundo == 0) {
        int inherit = -1;
        for (int i = 0; i < st->m->narchives; i++)
            if (slot_is_inherited_drop(n, i)) {
                inherit = i;
                break;
            }
        if (inherit >= 0)
            msg_inherited_drop(st, n, inherit);
        else
            set_msg(st, "press x to drop this path from the layer");
        return;
    }
    int pick;
    if (nundo == 1)
        pick = undo_slots[0];
    else {
        pick = prompt_archive(st, "Undo in", undo_slots, nundo, 1);
        if (pick == PICK_CANCEL) {
            set_msg(st, "cancelled");
            return;
        }
    }

    int do_slots[LE_MAX_ARCHIVES], ndo = 0;
    if (pick == PICK_ALL) {
        for (int i = 0; i < nundo; i++)
            do_slots[ndo++] = undo_slots[i];
    } else {
        do_slots[ndo++] = pick;
    }

    int undid_drop = 0, undid_stage = 0, undid_wh = 0;
    for (int i = 0; i < ndo; i++) {
        int s = do_slots[i];
        if (slot_is_staged(n, s)) {
            undid_stage++;
            if (staged_is_whiteout(n, s))
                undid_wh++;
        } else {
            undid_drop++;
        }
        model_undo(st->m, n, s);
    }
    rebuild_rows(st);
    const char *text;
    if (undid_stage && !undid_drop)
        text = undid_wh == undid_stage ? "staged whiteout cancelled"
                                       : "staged add cancelled";
    else if (undid_drop && !undid_stage)
        text = "kept in the layer";
    else
        text = "undone";
    set_action_msg(st, text, nundo == 1 ? -1 : pick, nundo);
}

static int parse_owner(const char *text, uid_t *uid, gid_t *gid) {
    unsigned long parsed_uid, parsed_gid;
    char extra;
    if (sscanf(text, "%lu:%lu%c", &parsed_uid, &parsed_gid, &extra) != 2)
        return -1;
    *uid = (uid_t)parsed_uid;
    *gid = (gid_t)parsed_gid;
    return 0;
}

static void action_add(tui_state *st, le_node *cur) {
    char host_input[PATH_MAX] = "", host[PATH_MAX], err[512];
    if (!prompt_edit("Add - host file (Tab completes): ", host_input,
                     sizeof(host_input), complete_host_path)) {
        set_msg(st, "add cancelled");
        return;
    }
    if (absolute_host_path(host_input, host, sizeof(host)) == -1) {
        set_msg(st, "host path is too long");
        return;
    }
    le_meta meta;
    char link[PATH_MAX];
    if (archive_host_meta(host, &meta, link, sizeof(link), err, sizeof(err)) == -1) {
        set_msg(st, "%s", err);
        return;
    }

    char host_copy[PATH_MAX], dir[PATH_MAX], dest[PATH_MAX];
    strlcpy(host_copy, host, sizeof(host_copy));
    const char *host_name = basename(host_copy);
    selected_directory(cur, dir, sizeof(dir));
    join_guest_path(dir, host_name, dest, sizeof(dest));
    if (!prompt_edit("Add - destination in archive: ", dest, sizeof(dest), NULL)) {
        set_msg(st, "add cancelled");
        return;
    }
    /* "dir/" or an existing directory means "inside it, same name". */
    le_node *dest_node = model_lookup(st->m, dest);
    if (strip_trailing_slashes(dest) ||
        (dest_node && node_is_dir(dest_node) && !node_is_whiteout(dest_node))) {
        strlcpy(dir, dest, sizeof(dir));
        join_guest_path(dir, host_name, dest, sizeof(dest));
    }

    int target = pick_receiving_archive(st, "Add to which archive");
    if (target < 0) {
        set_msg(st, "add cancelled");
        return;
    }

    char owner[64];
    model_default_owner(st->m, dest, &meta.uid, &meta.gid);
    snprintf(owner, sizeof(owner), "%u:%u", (unsigned)meta.uid, (unsigned)meta.gid);
    if (!prompt_edit("Add - owner uid:gid in the image: ", owner, sizeof(owner),
                     NULL)) {
        set_msg(st, "add cancelled");
        return;
    }
    if (parse_owner(owner, &meta.uid, &meta.gid) == -1) {
        set_msg(st, "owner must be uid:gid, for example 501:20");
        return;
    }

    le_node *n = model_stage_host_file(st->m, dest, target, host, &meta, err,
                                       sizeof(err));
    if (!n) {
        set_msg(st, "%s", err);
        return;
    }
    reveal_node(st, n);
    if (multi(st))
        set_msg(st, "staged in %s: %s", session_label(st->sess, target), dest);
    else
        set_msg(st, "staged: %s", dest);
}

static void action_whiteout(tui_state *st, le_node *cur) {
    char target[PATH_MAX] = "", err[512];
    if (cur && !node_is_whiteout(cur)) {
        node_path(cur, target, sizeof(target));
        if (node_is_dir(cur))
            append_slash(target, sizeof(target));
    } else {
        selected_directory(cur, target, sizeof(target));
        if (target[0])
            append_slash(target, sizeof(target));
    }
    if (!prompt_edit("Whiteout - path to delete from the base (dir ends in /): ",
                     target, sizeof(target), NULL)) {
        set_msg(st, "whiteout cancelled");
        return;
    }
    int target_is_dir = strip_trailing_slashes(target);
    le_node *target_node = model_lookup(st->m, target);
    if (target_node && node_is_dir(target_node))
        target_is_dir = 1;

    int arch_idx = pick_receiving_archive(st, "Whiteout in which archive");
    if (arch_idx < 0) {
        set_msg(st, "whiteout cancelled");
        return;
    }
    int layer_held_target = target_node && target_node->arch[arch_idx].present &&
                            !target_node->arch[arch_idx].removed;

    le_node *n = model_stage_whiteout(st->m, target, arch_idx, target_is_dir,
                                      err, sizeof(err));
    if (!n) {
        set_msg(st, "%s", err);
        return;
    }
    reveal_node(st, n);
    set_msg(st, "staged whiteout for %s%s", target,
            layer_held_target ? "; the layer copy is dropped" : "");
}

static void action_filter(tui_state *st) {
    char f[sizeof(st->filter)];
    strlcpy(f, st->filter, sizeof(f));
    if (prompt_edit("Filter (empty clears): ", f, sizeof(f), NULL)) {
        strlcpy(st->filter, f, sizeof(st->filter));
        st->filtering = 1;
        set_msg(st, "filter applied");
    } else {
        st->filter[0] = '\0';
        st->filtering = 0;
        set_msg(st, "filter cleared");
    }
    st->cursor = 0;
    st->top = 0;
    rebuild_rows(st);
}

enum { SAVE_DONE = 0, SAVE_FAILED = -1, SAVE_CANCELLED = 1 };

/* Ask what the slot's store needs to know, then save it. Long saves keep
 * "saving ..." on screen while they run. */
static int save_slot(tui_state *st, int slot, char *err, size_t errlen) {
    le_store *store = session_store(st->sess, slot);
    char question[512];
    if (store_pre_save_question(store, question, sizeof(question))) {
        str_append(question, sizeof(question), (int)strlen(question), " (y/N)");
        if (!confirm(question))
            return SAVE_CANCELLED;
    }

    char prompt[256], save_name[256];
    const char *accepted_save_name = NULL;
    if (store_save_name_prompt(store, prompt, sizeof(prompt), save_name,
                               sizeof(save_name))) {
        if (!prompt_edit(prompt, save_name, sizeof(save_name), NULL))
            return SAVE_CANCELLED;
        if (store_check_save_name(store, save_name, err, errlen) != 0)
            return SAVE_FAILED;
        accepted_save_name = save_name;
    }

    set_msg(st, "saving %s ...", store_label(store));
    draw(st);
    return store_save(store, st->m, slot, accepted_save_name, err, errlen) == 0
               ? SAVE_DONE
               : SAVE_FAILED;
}

/* Save every archive with unsaved changes, stopping at the first failure so a
 * broken archive does not hide behind later successes. */
static void action_save(tui_state *st) {
    char err[512] = "";
    int saved = 0, stopped_at = -1, stop_reason = SAVE_DONE;

    for (int i = 0; i < st->m->narchives; i++) {
        if (!model_arch_dirty(st->m, i))
            continue;
        stop_reason = save_slot(st, i, err, sizeof(err));
        if (stop_reason != SAVE_DONE) {
            stopped_at = i;
            break;
        }
        saved++;
    }
    rebuild_rows(st);

    if (stop_reason == SAVE_CANCELLED)
        set_msg(st, "saved %d, save of %s cancelled", saved,
                session_label(st->sess, stopped_at));
    else if (stop_reason == SAVE_FAILED)
        set_msg(st, "saved %d, SAVE FAILED %s: %s", saved,
                session_label(st->sess, stopped_at), err);
    else if (saved == 0)
        set_msg(st, "nothing to save");
    else if (multi(st))
        set_msg(st, "saved %d archive%s", saved, saved == 1 ? "" : "s");
    else
        set_msg(st, "saved %s", session_label(st->sess, 0));
}

/* ---- main loop ---- */

static void move_cursor(tui_state *st, long delta) {
    if (st->nrows == 0)
        return;
    long target = (long)st->cursor + delta;
    if (target < 0)
        target = 0;
    if ((size_t)target >= st->nrows)
        target = (long)st->nrows - 1;
    st->cursor = (size_t)target;
}

static int init_screen(void) {
    setlocale(LC_ALL, "");              /* UTF-8 names render as text */
    setenv("ESCDELAY", "25", 0);        /* Esc cancels without a long pause */
    if (!initscr())
        return -1;
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(200);                       /* read_key notices SIGTERM without a key */

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = request_stop;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    have_colors = 0;
    if (has_colors() && start_color() != ERR) {
        /* Prefer the terminal default background so most rows sit on the
         * normal list color. Shared, selected, and whiteout rows use the
         * Veertu amber, purple, and rose. */
        use_default_colors();
        have_colors = init_pair(PAIR_SHARED, COLOR_BLACK,
                                brand_color(VEERTU_AMBER_256, COLOR_YELLOW)) != ERR &&
                      init_pair(PAIR_WHITEOUT,
                                brand_color(VEERTU_ROSE_256, COLOR_RED), -1) != ERR;
        have_select_color = init_pair(PAIR_SELECTED, COLOR_BLACK,
                                      brand_color(VEERTU_PURPLE_256,
                                                  COLOR_MAGENTA)) != ERR;
    }
    return 0;
}

int tui_run(const le_session *sess, le_model *m) {
    tui_state st;
    memset(&st, 0, sizeof(st));
    st.sess = sess;
    st.m = m;

    if (init_screen() == -1)
        return 1;
    rebuild_rows(&st);

    int running = 1;
    while (running) {
        clamp_scroll(&st);
        draw(&st);
        int ch = read_key();
        if (ch == KEY_STOP)
            break;

        if (st.show_help) {             /* any key dismisses help */
            st.show_help = 0;
            continue;
        }
        st.msg[0] = '\0';               /* clear transient message on input */

        le_node *cur = st.nrows ? st.rows[st.cursor].node : NULL;

        switch (ch) {
        case KEY_UP:
        case 'k':
            move_cursor(&st, -1);
            break;
        case KEY_DOWN:
        case 'j':
            move_cursor(&st, 1);
            break;
        case KEY_NPAGE:
            move_cursor(&st, list_height(&st));
            break;
        case KEY_PPAGE:
            move_cursor(&st, -list_height(&st));
            break;
        case 'g':
            st.cursor = 0;
            break;
        case 'G':
            st.cursor = st.nrows ? st.nrows - 1 : 0;
            break;
        case KEY_RIGHT:
        case 'l':
            if (cur && node_is_dir(cur) && !node_is_whiteout(cur)) {
                if (!cur->expanded) {
                    cur->expanded = 1;
                    rebuild_rows(&st);
                } else {
                    move_cursor(&st, 1);    /* step into first child */
                }
            }
            break;
        case KEY_LEFT:
        case 'h':
            if (cur && node_is_dir(cur) && cur->expanded && !st.filtering) {
                cur->expanded = 0;
                rebuild_rows(&st);
            } else {
                move_to_parent(&st);
            }
            break;
        case '\n':
        case '\r':
        case KEY_ENTER:
            if (cur)
                action_preview(&st, cur);
            break;
        case 'd':
            if (cur)
                action_diff(&st, cur);
            break;
        case 'x':
            if (cur)
                action_drop(&st, cur);
            break;
        case 'u':
            if (cur)
                action_undo(&st, cur);
            break;
        case 'a':
            action_add(&st, cur);
            break;
        case 'w':
            action_whiteout(&st, cur);
            break;
        case '/':
            action_filter(&st);
            break;
        case 's':
            action_save(&st);
            break;
        case '?':
            st.show_help = 1;
            break;
        case 'q':
            if (!model_dirty(st.m) ||
                confirm(" Unsaved changes - quit without saving? (y/N) "))
                running = 0;
            break;
        case KEY_RESIZE:
        default:
            break;
        }
    }

    endwin();
    free(st.rows);
    return stop_requested == 2 ? RELOAD_EXIT : 0;
}
