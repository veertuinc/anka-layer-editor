#include "tui.h"
#include "archive.h"

#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <libgen.h>
#include <sys/stat.h>

typedef struct {
    const char *path;
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

/* ---- helpers ---- */

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

/* ---- rendering ---- */

static void draw_row(tui_state *st, int screen_y, size_t idx) {
    le_row *r = &st->rows[idx];
    le_node *n = r->node;
    int selected = (idx == st->cursor);
    int eff_removed = node_effectively_removed(n);

    char flag = ' ';
    if (n->removed)
        flag = 'X';                 /* explicitly marked */
    else if (eff_removed)
        flag = '~';                 /* removed via ancestor */

    char line[4096];
    int off = 0;
    off += snprintf(line + off, sizeof(line) - off, "%c ", flag);
    for (int d = 0; d < r->depth && off < (int)sizeof(line) - 4; d++)
        off += snprintf(line + off, sizeof(line) - off, "  ");

    const char *marker = "  ";
    if (n->is_dir)
        marker = n->expanded ? "v " : "> ";
    off += snprintf(line + off, sizeof(line) - off, "%s", marker);

    if (n->added)
        off += snprintf(line + off, sizeof(line) - off, "[+] ");

    off += snprintf(line + off, sizeof(line) - off, "%s%s",
                    n->name, n->is_dir ? "/" : "");

    if (n->is_symlink)
        off += snprintf(line + off, sizeof(line) - off, " -> %s", n->link);
    else if (!n->is_dir) {
        char sz[32];
        human_size(n->size, sz, sizeof(sz));
        off += snprintf(line + off, sizeof(line) - off, "   (%s)", sz);
    }

    int attr = 0;
    if (selected)
        attr = A_REVERSE;
    else if (eff_removed)
        attr = A_DIM;

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

static void draw_help(void) {
    const char *lines[] = {
        "  layeredit — help",
        "",
        "  Up/Down, j/k    Move selection",
        "  Right / l       Expand directory (or step into)",
        "  Left / h        Collapse directory (or jump to parent)",
        "  PgUp/PgDn       Page up / down",
        "  g / G           Jump to top / bottom",
        "  Space           Toggle mark-for-removal (subtree)",
        "  a               Add a host file at a destination path",
        "  /               Filter by substring (empty clears)",
        "  s               Save archive in place",
        "  q               Quit (confirms if unsaved)",
        "  ?               Toggle this help",
        "",
        "  Legend: 'X' marked, '~' removed via parent, '[+]' staged add",
        "",
        "  Press any key to close help",
        NULL,
    };
    erase();
    for (int i = 0; lines[i]; i++)
        mvaddnstr(i + 1, 2, lines[i], COLS - 4);
    refresh();
}

static void draw(tui_state *st) {
    if (st->show_help) {
        draw_help();
        return;
    }

    erase();

    /* Header. */
    char title[COLS + 1];
    char pathcopy[PATH_MAX];
    strlcpy(pathcopy, st->path, sizeof(pathcopy));
    snprintf(title, sizeof(title), " layeredit — %s%s ",
             basename(pathcopy), st->m->dirty ? "  [modified]" : "");
    attron(A_REVERSE);
    mvaddnstr(0, 0, title, COLS);
    for (int x = (int)strlen(title); x < COLS; x++)
        mvaddch(0, x, ' ');
    attroff(A_REVERSE);

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
    char status[COLS + 1];
    int off = snprintf(status, sizeof(status),
                       " %zu entries | marked: %zu | staged: %zu",
                       st->m->total, st->m->marked, st->m->added);
    if (st->filtering)
        off += snprintf(status + off, sizeof(status) - off,
                        " | filter: \"%s\"", st->filter);
    if (st->msg[0])
        off += snprintf(status + off, sizeof(status) - off, " | %s", st->msg);
    attron(A_REVERSE);
    mvaddnstr(LINES - 1, 0, status, COLS);
    for (int x = (int)strlen(status); x < COLS; x++)
        mvaddch(LINES - 1, x, ' ');
    attroff(A_REVERSE);

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

static void action_add(tui_state *st) {
    char host[PATH_MAX], dest[PATH_MAX];
    if (!prompt("Add — host file path: ", host, sizeof(host))) {
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
    if (!prompt("Add — destination path in archive: ", dest, sizeof(dest))) {
        snprintf(st->msg, sizeof(st->msg), "add cancelled");
        return;
    }
    int sym = S_ISLNK(sb.st_mode);
    le_node *n = model_add_host(st->m, dest, host, sym,
                                sym ? 0 : sb.st_size, sb.st_mode, NULL);
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

static void action_save(tui_state *st) {
    char err[256] = "";
    if (archive_save(st->path, st->m, err, sizeof(err)) == 0)
        snprintf(st->msg, sizeof(st->msg), "saved %zu entries", st->m->total);
    else
        snprintf(st->msg, sizeof(st->msg), "SAVE FAILED: %s", err);
}

/* ---- main loop ---- */

int tui_run(const char *archive_path, le_model *m) {
    tui_state st;
    memset(&st, 0, sizeof(st));
    st.path = archive_path;
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
            if (cur) {
                model_toggle_removed(st.m, cur);
                snprintf(st.msg, sizeof(st.msg),
                         cur->removed ? "marked for removal" : "unmarked");
            }
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
                confirm("Unsaved changes — quit without saving? (y/N) "))
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
