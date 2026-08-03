#include "tui.h"
#include "archive.h"

#include <ncurses.h>
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
    char        msg[256];    /* transient status message */
    int         show_help;
} tui_state;

/* Results of the archive picker that are not an archive index. */
#define PICK_CANCEL (-1)
#define PICK_ALL    (-2)

/* ---- helpers ---- */

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

static int multi(const tui_state *st) {
    return st->m->narchives > 1;
}

static int list_height(void) {
    int h = LINES - 2;              /* header + status bar */
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
    int h = list_height();
    if (st->cursor < st->top)
        st->top = st->cursor;
    else if (st->cursor >= st->top + (size_t)h)
        st->top = st->cursor - (size_t)h + 1;
}

/* Draw a full-width reverse-video bar (header, status, prompts). */
static void draw_bar(int y, const char *text) {
    attron(A_REVERSE);
    mvaddnstr(y, 0, text, COLS);
    for (int x = (int)strlen(text); x < COLS; x++)
        mvaddch(y, x, ' ');
    attroff(A_REVERSE);
}

/* ---- rendering ---- */

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
        if (n->arch[i].added)
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

static void draw_row(tui_state *st, int screen_y, size_t idx) {
    le_row *r = &st->rows[idx];
    le_node *n = r->node;
    int selected = (idx == st->cursor);

    int slots[LE_MAX_ARCHIVES];
    int nslots = node_archives(n, st->m->narchives, slots);
    int explicit_removed = 0, removed_somewhere = 0, staged = 0;
    int removed_everywhere = nslots > 0;
    for (int i = 0; i < nslots; i++) {
        int a = slots[i];
        if (n->arch[a].removed)
            explicit_removed = 1;
        if (n->arch[a].added)
            staged = 1;
        if (node_effectively_removed(n, a))
            removed_somewhere = 1;
        else
            removed_everywhere = 0;
    }

    char flag = ' ';
    if (explicit_removed)
        flag = 'X';                 /* explicitly marked */
    else if (removed_somewhere)
        flag = '~';                 /* removed via ancestor */

    char badge[LE_MAX_ARCHIVES + 8];
    archive_badge(st, n, badge, sizeof(badge));

    char line[4096];
    int off = str_append(line, sizeof(line), 0, "%c %s", flag, badge);
    for (int d = 0; d < r->depth; d++)
        off = str_append(line, sizeof(line), off, "  ");

    const char *marker = "  ";
    if (n->is_dir)
        marker = n->expanded ? "v " : "> ";
    off = str_append(line, sizeof(line), off, "%s", marker);

    if (staged)
        off = str_append(line, sizeof(line), off, "[+] ");

    off = str_append(line, sizeof(line), off, "%s%s",
                     n->name, n->is_dir ? "/" : "");

    if (n->is_symlink)
        str_append(line, sizeof(line), off, " -> %s", n->link);
    else if (!n->is_dir) {
        char sz[32];
        human_size(n->size, sz, sizeof(sz));
        str_append(line, sizeof(line), off, "   (%s)", sz);
    }

    int attr = 0;
    if (selected)
        attr = A_REVERSE;
    else if (removed_everywhere)
        attr = A_DIM;
    else if (nslots > 1)
        attr = A_BOLD;              /* path shared between archives */

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

static void draw_help(const tui_state *st) {
    const char *lines[] = {
        "  anka-layer-editor - help",
        "",
        "  Up/Down, j/k    Move selection",
        "  Right / l       Expand directory (or step into)",
        "  Left / h        Collapse directory (or jump to parent)",
        "  PgUp/PgDn       Page up / down",
        "  g / G           Jump to top / bottom",
        "  Space           Toggle mark-for-removal (subtree)",
        "  a               Add a host file at a destination path",
        "  /               Filter by substring (empty clears)",
        "  s               Save every changed archive in place",
        "  q               Quit (confirms if unsaved)",
        "  ?               Toggle this help",
        "",
        "  Legend: 'X' marked, '~' removed via parent, '[+]' staged add",
        NULL,
    };
    const char *multi_lines[] = {
        "",
        "  With several archives loaded, each row carries a membership column:",
        "  [12] in both, [1.] only the first, [.+] staged into the second.",
        "  Shared paths are bold; '!' means the entries disagree on type/size.",
        "  Removing a shared path asks which archive (or all of them);",
        "  adding a file asks which archive receives it.",
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

    /* Header: the archive (or archives) being edited, '*' when unsaved. */
    char title[1024];
    int off = str_append(title, sizeof(title), 0, " anka-layer-editor - ");
    if (multi(st)) {
        for (int i = 0; i < st->sess->count; i++)
            off = str_append(title, sizeof(title), off, "%d:%s%s  ",
                             i + 1, session_label(st->sess, i),
                             st->m->arch_dirty[i] ? "*" : "");
    } else {
        str_append(title, sizeof(title), off, "%s%s ",
                   session_label(st->sess, 0),
                   st->m->dirty ? "  [modified]" : "");
    }
    draw_bar(0, title);

    /* List. */
    int h = list_height();
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

    /* Status bar. */
    char status[1024];
    off = str_append(status, sizeof(status), 0,
                     " %zu paths | marked: %zu | staged: %zu",
                     st->m->total, st->m->marked, st->m->added);
    if (st->filtering)
        off = str_append(status, sizeof(status), off, " | filter: \"%s\"",
                         st->filter);
    if (st->msg[0])
        str_append(status, sizeof(status), off, " | %s", st->msg);
    draw_bar(LINES - 1, status);

    refresh();
}

/* ---- input dialogs ---- */

/* Read a line from the status bar. Returns 1 on confirm, 0 on empty/cancel. */
static int prompt(const char *label, char *out, size_t len) {
    int y = LINES - 1;
    move(y, 0);
    clrtoeol();
    attron(A_REVERSE);
    mvaddnstr(y, 0, label, COLS);
    attroff(A_REVERSE);
    int lx = (int)strlen(label);
    if (lx > COLS - 1)
        lx = COLS - 1;
    move(y, lx);
    echo();
    curs_set(1);
    out[0] = '\0';
    int r = getnstr(out, (int)len - 1);
    noecho();
    curs_set(0);
    return (r == OK || r == KEY_ENTER) && out[0] != '\0';
}

static int confirm(const char *question) {
    int y = LINES - 1;
    move(y, 0);
    clrtoeol();
    attron(A_REVERSE);
    mvaddnstr(y, 0, question, COLS);
    attroff(A_REVERSE);
    int ch = getch();
    return ch == 'y' || ch == 'Y';
}

/* Ask which of the candidate archives an action applies to. Returns an archive
 * index, PICK_ALL when allow_all and the user chose all, or PICK_CANCEL. */
static int prompt_archive(tui_state *st, const char *action, const int *slots,
                          int nslots, int allow_all) {
    char line[1024];
    int off = str_append(line, sizeof(line), 0, " %s: ", action);
    for (int i = 0; i < nslots; i++)
        off = str_append(line, sizeof(line), off, "[%d] %s  ",
                         slots[i] + 1, session_label(st->sess, slots[i]));
    if (allow_all)
        off = str_append(line, sizeof(line), off, "[a] all  ");
    str_append(line, sizeof(line), off, "(Esc cancels)");

    move(LINES - 1, 0);
    clrtoeol();
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

/* ---- actions ---- */

static void action_toggle_removed(tui_state *st, le_node *n) {
    int slots[LE_MAX_ARCHIVES];
    int nslots = node_archives(n, st->m->narchives, slots);
    if (nslots == 0) {
        snprintf(st->msg, sizeof(st->msg), "not present in any archive");
        return;
    }
    if (nslots == 1) {
        model_toggle_removed(st->m, n, slots[0]);
        snprintf(st->msg, sizeof(st->msg), "%s",
                 n->arch[slots[0]].removed ? "marked for removal" : "unmarked");
        return;
    }

    int pick = prompt_archive(st, "Mark for removal in", slots, nslots, 1);
    if (pick == PICK_CANCEL) {
        snprintf(st->msg, sizeof(st->msg), "cancelled");
        return;
    }
    if (pick == PICK_ALL) {
        model_toggle_removed_all(st->m, n);
        snprintf(st->msg, sizeof(st->msg), "%s in all %d archives",
                 n->arch[slots[0]].removed ? "marked" : "unmarked", nslots);
        return;
    }
    model_toggle_removed(st->m, n, pick);
    snprintf(st->msg, sizeof(st->msg), "%s in %s",
             n->arch[pick].removed ? "marked" : "unmarked",
             session_label(st->sess, pick));
}

static void action_add(tui_state *st) {
    char host[PATH_MAX], dest[PATH_MAX];
    if (!prompt("Add - host file path: ", host, sizeof(host))) {
        snprintf(st->msg, sizeof(st->msg), "add cancelled");
        return;
    }
    struct stat sb;
    if (lstat(host, &sb) == -1) {
        snprintf(st->msg, sizeof(st->msg), "host file not found: %s", host);
        return;
    }
    if (!S_ISREG(sb.st_mode) && !S_ISLNK(sb.st_mode)) {
        snprintf(st->msg, sizeof(st->msg), "not a regular file or symlink");
        return;
    }
    if (!prompt("Add - destination path in archive: ", dest, sizeof(dest))) {
        snprintf(st->msg, sizeof(st->msg), "add cancelled");
        return;
    }

    int target = 0;
    if (multi(st)) {
        int slots[LE_MAX_ARCHIVES];
        for (int i = 0; i < st->m->narchives; i++)
            slots[i] = i;
        target = prompt_archive(st, "Add to which archive", slots,
                                st->m->narchives, 0);
        if (target < 0) {
            snprintf(st->msg, sizeof(st->msg), "add cancelled");
            return;
        }
    }

    int sym = S_ISLNK(sb.st_mode);
    char linktgt[PATH_MAX] = "";
    if (sym) {
        ssize_t l = readlink(host, linktgt, sizeof(linktgt) - 1);
        if (l == -1) {
            snprintf(st->msg, sizeof(st->msg), "readlink failed: %s", host);
            return;
        }
        linktgt[l] = '\0';
    }
    le_node *n = model_add_host(st->m, dest, target, host, sym,
                                sym ? 0 : sb.st_size, sb.st_mode,
                                sym ? linktgt : NULL);
    if (!n) {
        snprintf(st->msg, sizeof(st->msg), "invalid destination path");
        return;
    }
    if (st->filtering) {                /* additions may not match the filter */
        st->filtering = 0;
        st->filter[0] = '\0';
    }
    rebuild_rows(st);
    select_node(st, n);
    if (multi(st))
        snprintf(st->msg, sizeof(st->msg), "staged in %s: %s",
                 session_label(st->sess, target), dest);
    else
        snprintf(st->msg, sizeof(st->msg), "staged: %s", dest);
}

static void action_filter(tui_state *st) {
    char f[sizeof(st->filter)];
    if (prompt("Filter (empty to clear): ", f, sizeof(f))) {
        strlcpy(st->filter, f, sizeof(st->filter));
        st->filtering = 1;
        snprintf(st->msg, sizeof(st->msg), "filter applied");
    } else {
        st->filter[0] = '\0';
        st->filtering = 0;
        snprintf(st->msg, sizeof(st->msg), "filter cleared");
    }
    st->cursor = 0;
    st->top = 0;
    rebuild_rows(st);
}

/* Save every archive with unsaved changes, stopping at the first failure so a
 * broken archive does not hide behind later successes. */
static void action_save(tui_state *st) {
    char err[256] = "";
    int saved = 0, failed = -1;

    for (int i = 0; i < st->m->narchives; i++) {
        if (!st->m->arch_dirty[i])
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
        snprintf(st->msg, sizeof(st->msg), "saved %d, SAVE FAILED %s: %s",
                 saved, session_label(st->sess, failed), err);
    else if (saved == 0)
        snprintf(st->msg, sizeof(st->msg), "nothing to save");
    else if (multi(st))
        snprintf(st->msg, sizeof(st->msg), "saved %d archive%s", saved,
                 saved == 1 ? "" : "s");
    else
        snprintf(st->msg, sizeof(st->msg), "saved %zu entries", st->m->total);
}

/* ---- main loop ---- */

int tui_run(const le_session *sess, le_model *m) {
    tui_state st;
    memset(&st, 0, sizeof(st));
    st.sess = sess;
    st.m = m;

    if (!initscr())
        return 1;
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

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
            if (st.cursor > 0)
                st.cursor--;
            break;
        case KEY_DOWN:
        case 'j':
            if (st.cursor + 1 < st.nrows)
                st.cursor++;
            break;
        case KEY_NPAGE:
            st.cursor += (size_t)list_height();
            if (st.cursor >= st.nrows && st.nrows)
                st.cursor = st.nrows - 1;
            break;
        case KEY_PPAGE: {
            size_t step = (size_t)list_height();
            st.cursor = st.cursor > step ? st.cursor - step : 0;
            break;
        }
        case 'g':
            st.cursor = 0;
            break;
        case 'G':
            st.cursor = st.nrows ? st.nrows - 1 : 0;
            break;
        case KEY_RIGHT:
        case 'l':
            if (cur && cur->is_dir) {
                if (!cur->expanded) {
                    cur->expanded = 1;
                    rebuild_rows(&st);
                } else if (st.cursor + 1 < st.nrows) {
                    st.cursor++;        /* step into first child */
                }
            }
            break;
        case KEY_LEFT:
        case 'h':
            if (cur && cur->is_dir && cur->expanded) {
                cur->expanded = 0;
                rebuild_rows(&st);
            } else {
                move_to_parent(&st);
            }
            break;
        case ' ':
            if (cur)
                action_toggle_removed(&st, cur);
            break;
        case 'a':
            action_add(&st);
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
            if (!st.m->dirty ||
                confirm("Unsaved changes - quit without saving? (y/N) "))
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
