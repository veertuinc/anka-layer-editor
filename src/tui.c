#include "tui.h"
#include "archive.h"
#include "util.h"

#include <ncurses.h>
#include <glob.h>
#include <locale.h>
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

/* Color pairs: paths in more than one archive (and their parents), and
 * whiteout entries. */
#define PAIR_SHARED   1
#define PAIR_WHITEOUT 2
static int have_colors;

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
                         "arrows = move | Space = mark/undo | a = add host file"
                         " | w = whiteout | / = filter | s = save | q = quit"
                         " | ? = more");
    if (multi(st))
        str_append(buf, buflen, off, " | yellow = file in multiple archives");
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

/* Mark files that belong to 2+ archives, and paint their ancestors yellow so
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

/* Fixed-width membership column: one slot per archive, "1".."8" when the
 * archive holds the path, '+' when it is a staged add, '.' when absent. */
static void archive_badge(const tui_state *st, const le_node *n,
                          char *buf, size_t len) {
    buf[0] = '\0';
    if (!multi(st) || len < (size_t)st->m->narchives + 5)
        return;
    size_t off = 0;
    buf[off++] = '[';
    for (int i = 0; i < st->m->narchives; i++) {
        if (n->arch[i].staged)
            buf[off++] = '+';
        else if (n->arch[i].present)
            buf[off++] = (char)('1' + i);
        else
            buf[off++] = '.';
    }
    buf[off++] = ']';
    buf[off++] = n->mismatch ? '!' : ' ';
    buf[off++] = ' ';
    buf[off] = '\0';
}

typedef struct {
    int explicit_removed;       /* marked in at least one archive */
    int removed_somewhere;      /* removed (directly or via a parent) somewhere */
    int removed_everywhere;     /* every archive holding it drops it */
    int staged;                 /* staged in at least one archive */
} row_state;

static row_state row_state_for(const tui_state *st, const le_node *n) {
    int slots[LE_MAX_ARCHIVES];
    int nslots = node_archives(n, st->m->narchives, slots);
    row_state rs = {0, 0, nslots > 0, 0};
    for (int i = 0; i < nslots; i++) {
        int a = slots[i];
        if (n->arch[a].removed)
            rs.explicit_removed = 1;
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

    char flag = ' ';
    if (rs.explicit_removed)
        flag = 'X';                 /* explicitly marked */
    else if (rs.removed_somewhere)
        flag = '~';                 /* removed via ancestor */

    char badge[LE_MAX_ARCHIVES + 8];
    archive_badge(st, n, badge, sizeof(badge));

    char line[4096];
    int off = str_append(line, sizeof(line), 0, "%c %s", flag, badge);
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
    if (selected)
        attr = A_REVERSE;
    else if (rs.removed_everywhere)
        attr = A_DIM;

    if (!rs.removed_everywhere) {
        int color_pair = whiteout ? PAIR_WHITEOUT
                         : n->shared_warn ? PAIR_SHARED : 0;
        if (color_pair && have_colors)
            attr |= COLOR_PAIR(color_pair);
        else if (color_pair && !selected)
            attr |= A_BOLD;         /* no color: keep these rows obvious */
    }

    if (attr)
        attron(attr);
    mvaddnstr(screen_y, 0, line, COLS);
    if (selected) {                 /* fill row for a solid highlight bar */
        int len = (int)strlen(line);
        for (int x = len; x < COLS; x++)
            addch(' ');
    }
    if (attr)
        attroff(attr);
}

/* What one archive holds and plans for the node, e.g. "added, marked". */
static int append_arch_state(char *buf, size_t cap, int off, const le_node *n,
                             int arch_idx) {
    const le_arch_state *a = &n->arch[arch_idx];
    int wrote_any = 0;
#define STATE_PART(text) \
    do { off = str_append(buf, cap, off, "%s%s", wrote_any ? ", " : "", text); \
         wrote_any = 1; } while (0)
    const char *kind = a->present ? change_kind_label(a->change_kind) : NULL;
    if (kind)
        STATE_PART(kind);
    else if (a->present)
        STATE_PART("member");
    if (a->staged)
        STATE_PART(a->staged->source == LE_STAGED_WHITEOUT ? "staged whiteout"
                                                           : "staged");
    if (a->removed)
        STATE_PART("marked");
    else if (node_effectively_removed(n, arch_idx))
        STATE_PART("removed via parent");
#undef STATE_PART
    return off;
}

/* Details for the selected row: full path, per-archive state, mode, owner. */
static void format_details(const tui_state *st, char *buf, size_t cap) {
    buf[0] = '\0';
    if (st->nrows == 0)
        return;
    const le_node *n = st->rows[st->cursor].node;
    char path[PATH_MAX];
    node_path(n, path, sizeof(path));
    int off = str_append(buf, cap, 0, " ");
    off = str_append_safe(buf, cap, off, path);
    off = str_append(buf, cap, off, " |");

    int slots[LE_MAX_ARCHIVES];
    int nslots = node_archives(n, st->m->narchives, slots);
    for (int i = 0; i < nslots; i++) {
        off = str_append(buf, cap, off, " ");
        if (multi(st))
            off = str_append(buf, cap, off, "%d: ", slots[i] + 1);
        off = append_arch_state(buf, cap, off, n, slots[i]);
        if (i + 1 < nslots)
            off = str_append(buf, cap, off, " ");
    }

    const le_meta *meta = node_display_meta(n, st->m->narchives);
    if (n->meta_set || meta != &n->meta)
        str_append(buf, cap, off, " | mode %04o owner %u:%u",
                   (unsigned)(meta->mode & 07777), (unsigned)meta->uid,
                   (unsigned)meta->gid);
}

static void draw_help(const tui_state *st) {
    const char *lines[] = {
        "  anka-layer-editor - help",
        "",
        "  Up/Down, j/k    Move selection",
        "  Right / l       Expand directory (or step into)",
        "  Left / h        Collapse directory (or jump to parent)",
        "  PgUp/PgDn       Page up / down",
        "  g / G           Jump to top / bottom",
        "  Space           Mark for removal (subtree), or undo a staged item",
        "  a               Add a host file at a destination path",
        "  w               Stage a whiteout: delete a path from the base image",
        "  /               Filter by substring (empty clears)",
        "  s               Save every changed archive in place",
        "  q               Quit (confirms if unsaved)",
        "  ?               Toggle this help",
        "",
        "  In prompts: Enter accepts, Esc cancels, Tab completes host paths.",
        "",
        "  Legend: 'X' marked, '~' removed via parent, '[+]' staged,",
        "  '[wh]' whiteout (red). The details line shows the Anka change kind",
        "  (added / modified / whiteout), mode, and owner of the selection.",
        NULL,
    };
    const char *multi_lines[] = {
        "",
        "  With several archives loaded, each row carries a membership column:",
        "  [12] in both, [1.] only the first, [.+] staged into the second.",
        "  Files in more than one archive are yellow; parents leading to them too.",
        "  '!' means the entries disagree on type/size.",
        "  Space on a shared path asks which archive (or all of them);",
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
    int off = str_append(title, sizeof(title), 0, " anka-layer-editor - ");
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
                     " %zu paths | marked: %zu | staged: %zu",
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
 * Esc cancels, Tab calls complete when given. Returns 1 when accepted with
 * text, 0 on cancel or empty. */
static int prompt_edit(const char *label, char *buf, size_t len,
                       prompt_completer complete) {
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

        int ch = getch();
        switch (ch) {
        case KEY_ESCAPE:
            accepted = 0;
            break;
        case '\n': case '\r': case KEY_ENTER:
            accepted = buf[0] != '\0';
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

static int confirm(const char *question) {
    draw_bar(LINES - 1, question);
    int ch = getch();
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

    int ch = getch();
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

static const char *mark_result_text(le_mark_result result) {
    switch (result) {
    case LE_MARK_REMOVED:  return "marked for removal";
    case LE_MARK_UNMARKED: return "unmarked";
    case LE_MARK_UNSTAGED: return "staged change undone";
    default:               return "nothing to change";
    }
}

static void action_toggle_mark(tui_state *st, le_node *n) {
    int slots[LE_MAX_ARCHIVES];
    int nslots = node_archives(n, st->m->narchives, slots);
    if (nslots == 0) {
        set_msg(st, "not present in any archive");
        return;
    }
    if (nslots == 1) {
        le_mark_result result = model_toggle_mark(st->m, n, slots[0]);
        rebuild_rows(st);
        set_msg(st, "%s", mark_result_text(result));
        return;
    }

    int pick = prompt_archive(st, "Space applies to", slots, nslots, 1);
    if (pick == PICK_CANCEL) {
        set_msg(st, "cancelled");
        return;
    }
    le_mark_result result = pick == PICK_ALL ? model_toggle_mark_all(st->m, n)
                                             : model_toggle_mark(st->m, n, pick);
    rebuild_rows(st);
    if (pick == PICK_ALL)
        set_msg(st, "%s in all %d archives", mark_result_text(result), nslots);
    else
        set_msg(st, "%s in %s", mark_result_text(result),
                session_label(st->sess, pick));
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
            layer_held_target ? "; the layer copy is marked for removal" : "");
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

/* Save every archive with unsaved changes, stopping at the first failure so a
 * broken archive does not hide behind later successes. */
static void action_save(tui_state *st) {
    char err[512] = "";
    int saved = 0, failed = -1;

    for (int i = 0; i < st->m->narchives; i++) {
        if (!model_arch_dirty(st->m, i))
            continue;
        if (archive_save(session_path(st->sess, i), st->m, i, err,
                         sizeof(err)) == 0)
            saved++;
        else {
            failed = i;
            break;
        }
    }
    rebuild_rows(st);

    if (failed >= 0)
        set_msg(st, "saved %d, SAVE FAILED %s: %s", saved,
                session_label(st->sess, failed), err);
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

    have_colors = 0;
    if (has_colors() && start_color() != ERR) {
        /* Prefer the terminal default background so colored rows sit on the
         * normal list color rather than a hard-coded black panel. */
        use_default_colors();
        have_colors = init_pair(PAIR_SHARED, COLOR_YELLOW, -1) != ERR &&
                      init_pair(PAIR_WHITEOUT, COLOR_RED, -1) != ERR;
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
        int ch = getch();

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
        case ' ':
            if (cur)
                action_toggle_mark(&st, cur);
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
    return 0;
}
