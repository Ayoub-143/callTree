/*
 * tui.c  –  ncurses front-end for calltree
 *
 * Build:
 *   gcc -O2 -DCALLTREE_LIB -c -o calltree.o calltree.c
 *   gcc -O2 -o calltui tui.c calltree.o -lncurses
 *
 * Keys (file browser):
 *   ↑/↓ k/j    navigate
 *   Enter       enter directory
 *   Backspace   go up one directory
 *   Space       toggle file selection
 *   a           select/deselect all .c files in current dir
 *   t           switch to tree panel
 *   q           quit
 *
 * Keys (tree panel):
 *   ↑/↓ k/j    navigate nodes
 *   Enter/Space toggle expand/collapse
 *   e           set entry point (opens prompt)
 *   +/-         increase/decrease max depth
 *   r           re-parse selected files
 *   f           switch back to file browser
 *   q           quit
 */

#define _GNU_SOURCE
#include <ncurses.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include "calltree.h"

/* ── limits ───────────────────────────────────────────── */
#define MAX_FILES     1024
#define MAX_PATH       512
#define MAX_TREE_NODES 4096
#define MAX_SEL_FILES  256

/* ── colour pairs ─────────────────────────────────────── */
#define CP_NORMAL   1
#define CP_SELECTED 2
#define CP_HEADER   3
#define CP_CURSOR   4
#define CP_DIM      5
#define CP_BRANCH   6
#define CP_ENTRY    7
#define CP_UNKNOWN  8
#define CP_CYCLE    9
#define CP_KEY     10

/* ── file browser entry ───────────────────────────────── */
typedef struct {
    char name[256];
    char fullpath[MAX_PATH];
    int  is_dir;
    int  selected;   /* only meaningful for files */
} DirEntry;

/* ── tree node ────────────────────────────────────────── */
typedef struct {
    char name[MAX_NAME];
    int  depth;
    int  expanded;    /* user toggled open */
    int  has_children;
    int  is_cycle;
    int  is_unknown;
    int  parent;      /* index of parent node, -1 for root */
    int  visible;
} TreeNode;

/* ── global state ─────────────────────────────────────── */
static char cwd[MAX_PATH];

static DirEntry dir_entries[MAX_FILES];
static int      ndir = 0;
static int      dir_cursor = 0;
static int      dir_scroll = 0;

static char sel_files[MAX_SEL_FILES][MAX_PATH];
static int  nsel = 0;

static TreeNode tree[MAX_TREE_NODES];
static int      ntree = 0;
static int      tree_cursor = 0;
static int      tree_scroll = 0;

static char  entry_func[MAX_NAME] = "main";
static int   max_depth = 6;
static int   parsed = 0;   /* have we run a parse yet? */

static int active_panel = 0;  /* 0=files, 1=tree */

static WINDOW *win_files;
static WINDOW *win_tree;
static WINDOW *win_status;

/* ── helpers ──────────────────────────────────────────── */
static int ends_with_c(const char *s)
{
    size_t n = strlen(s);
    return (n >= 2 && s[n-2] == '.' && (s[n-1] == 'c' || s[n-1] == 'h'));
}

static int cmp_dir(const void *a, const void *b)
{
    const DirEntry *da = a, *db = b;
    if (da->is_dir != db->is_dir) return db->is_dir - da->is_dir;
    return strcasecmp(da->name, db->name);
}

static void load_dir(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return;

    ndir = 0;
    struct dirent *de;
    while ((de = readdir(d)) && ndir < MAX_FILES) {
        if (!strcmp(de->d_name, ".")) continue;
        DirEntry *e = &dir_entries[ndir];
        strncpy(e->name, de->d_name, 255);
        snprintf(e->fullpath, MAX_PATH, "%s/%s", path, de->d_name);
        struct stat st;
        stat(e->fullpath, &st);
        e->is_dir = S_ISDIR(st.st_mode);
        e->selected = 0;
        /* preserve selection across reloads */
        if (!e->is_dir) {
            for (int i = 0; i < nsel; i++)
                if (!strcmp(sel_files[i], e->fullpath)) { e->selected = 1; break; }
        }
        ndir++;
    }
    closedir(d);
    qsort(dir_entries, ndir, sizeof(DirEntry), cmp_dir);
    dir_cursor = 0;
    dir_scroll = 0;
    strncpy(cwd, path, MAX_PATH-1);
}

static void toggle_select(int idx)
{
    DirEntry *e = &dir_entries[idx];
    if (e->is_dir) return;

    if (e->selected) {
        /* remove from sel_files */
        for (int i = 0; i < nsel; i++) {
            if (!strcmp(sel_files[i], e->fullpath)) {
                memmove(sel_files[i], sel_files[i+1],
                        (nsel-i-1)*MAX_PATH);
                nsel--;
                break;
            }
        }
        e->selected = 0;
    } else {
        if (nsel < MAX_SEL_FILES) {
            strncpy(sel_files[nsel++], e->fullpath, MAX_PATH-1);
            e->selected = 1;
        }
    }
}

static void select_all_c(void)
{
    /* check if all .c files are already selected */
    int all = 1;
    for (int i = 0; i < ndir; i++)
        if (!dir_entries[i].is_dir && ends_with_c(dir_entries[i].name) && !dir_entries[i].selected)
            { all = 0; break; }

    for (int i = 0; i < ndir; i++) {
        DirEntry *e = &dir_entries[i];
        if (e->is_dir || !ends_with_c(e->name)) continue;
        if (all) {
            /* deselect */
            if (e->selected) toggle_select(i);
        } else {
            /* select */
            if (!e->selected) toggle_select(i);
        }
    }
}

/* ── tree builder ─────────────────────────────────────── */

/* visited stack for cycle detection while building */
static char vstack[MAX_TREE_NODES][MAX_NAME];
static int  vdepth = 0;

static int in_vstack(const char *name)
{
    for (int i = 0; i < vdepth; i++)
        if (!strcmp(vstack[i], name)) return 1;
    return 0;
}

static void build_tree_rec(const char *name, int depth, int maxd, int parent)
{
    if (ntree >= MAX_TREE_NODES) return;

    int idx = ntree++;
    TreeNode *nd = &tree[idx];
    strncpy(nd->name, name, MAX_NAME-1);
    nd->depth     = depth;
    nd->expanded  = (depth < 2);  /* auto-expand first 2 levels */
    nd->is_cycle  = 0;
    nd->is_unknown= 0;
    nd->parent    = parent;
    nd->visible   = 1;

    if (in_vstack(name)) {
        nd->is_cycle    = 1;
        nd->has_children= 0;
        return;
    }

    const Func *f = ct_find(name);
    if (!f) {
        nd->is_unknown  = 1;
        nd->has_children= 0;
        return;
    }

    int nc = ct_ncalls(f);
    nd->has_children = (nc > 0);

    if (depth >= maxd || nc == 0) return;

    strncpy(vstack[vdepth++], name, MAX_NAME-1);
    for (int i = 0; i < nc; i++)
        build_tree_rec(ct_call(f, i), depth+1, maxd, idx);
    vdepth--;
}

static void rebuild_tree(void)
{
    ntree  = 0;
    vdepth = 0;
    memset(tree, 0, sizeof(tree));
    build_tree_rec(entry_func, 0, max_depth, -1);
    tree_cursor = 0;
    tree_scroll = 0;
}

/* compute which nodes are visible given expand/collapse state */
static void update_visibility(void)
{
    /* mark all invisible first */
    for (int i = 0; i < ntree; i++) tree[i].visible = 0;
    if (ntree == 0) return;
    tree[0].visible = 1;

    for (int i = 0; i < ntree; i++) {
        if (!tree[i].visible) continue;
        /* children are visible only if this node is expanded */
        if (!tree[i].expanded) continue;
        /* find direct children (next nodes with depth == tree[i].depth+1
         * that have tree[i] as parent) */
        for (int j = i+1; j < ntree; j++) {
            if (tree[j].parent == i)
                tree[j].visible = 1;
        }
    }
}

static int visible_index(int nth)
{
    int count = 0;
    for (int i = 0; i < ntree; i++) {
        if (tree[i].visible) {
            if (count == nth) return i;
            count++;
        }
    }
    return -1;
}

static int visible_count(void)
{
    int c = 0;
    for (int i = 0; i < ntree; i++) if (tree[i].visible) c++;
    return c;
}

/* ── parse & build ────────────────────────────────────── */
static void do_parse(void)
{
    ct_reset();
    for (int i = 0; i < nsel; i++)
        ct_parse_file(sel_files[i]);
    parsed = 1;
    rebuild_tree();
    update_visibility();
    active_panel = 1;
}

/* ── drawing ──────────────────────────────────────────── */
static void draw_header(WINDOW *w, const char *title, int active)
{
    int wide = getmaxx(w);
    wattron(w, COLOR_PAIR(CP_HEADER) | (active ? A_BOLD : A_NORMAL));
    mvwhline(w, 0, 0, ' ', wide);
    mvwprintw(w, 0, 1, " %s ", title);
    wattroff(w, COLOR_PAIR(CP_HEADER) | A_BOLD);
}

static void draw_files_panel(void)
{
    WINDOW *w = win_files;
    int rows, cols;
    getmaxyx(w, rows, cols);
    werase(w);

    char title[64];
    snprintf(title, sizeof(title), "[ Files (%d selected) ]", nsel);
    draw_header(w, active_panel==0 ? title : "  Files  ", active_panel==0);

    /* current dir, truncated */
    wattron(w, COLOR_PAIR(CP_DIM));
    char disp[MAX_PATH];
    snprintf(disp, cols-1, "%s", cwd);
    mvwprintw(w, 1, 1, "%-*.*s", cols-2, cols-2, disp);
    wattroff(w, COLOR_PAIR(CP_DIM));

    int list_rows = rows - 3;  /* header + dir line */
    /* clamp scroll */
    if (dir_cursor < dir_scroll) dir_scroll = dir_cursor;
    if (dir_cursor >= dir_scroll + list_rows) dir_scroll = dir_cursor - list_rows + 1;

    for (int r = 0; r < list_rows && (r + dir_scroll) < ndir; r++) {
        int idx = r + dir_scroll;
        DirEntry *e = &dir_entries[idx];
        int is_cur = (idx == dir_cursor);

        if (is_cur) wattron(w, COLOR_PAIR(CP_CURSOR) | A_BOLD);
        else if (e->selected) wattron(w, COLOR_PAIR(CP_SELECTED));
        else if (e->is_dir)   wattron(w, COLOR_PAIR(CP_BRANCH));
        else                  wattron(w, COLOR_PAIR(CP_NORMAL));

        char line[256];
        if (e->is_dir)
            snprintf(line, sizeof(line), " ▶ %s/", e->name);
        else
            snprintf(line, sizeof(line), " %s %s",
                     e->selected ? "●" : "○", e->name);

        mvwprintw(w, r+2, 0, "%-*.*s", cols, cols, line);
        wattroff(w, COLOR_PAIR(CP_CURSOR)|COLOR_PAIR(CP_SELECTED)|
                    COLOR_PAIR(CP_BRANCH)|COLOR_PAIR(CP_NORMAL)|A_BOLD);
    }

    box(w, 0, 0);
    wrefresh(w);
}

/* box-drawing prefixes */
static void draw_tree_prefix(WINDOW *w, int row, int col,
                             int depth, int is_last_child,
                             int *last_at_depth)
{
    /* draw vertical bars for each ancestor level */
    for (int d = 0; d < depth; d++) {
        wattron(w, COLOR_PAIR(CP_BRANCH));
        if (d == depth-1) {
            mvwprintw(w, row, col + d*3,
                      is_last_child ? "└─ " : "├─ ");
        } else {
            mvwprintw(w, row, col + d*3,
                      last_at_depth[d] ? "   " : "│  ");
        }
        wattroff(w, COLOR_PAIR(CP_BRANCH));
    }
}

static void draw_tree_panel(void)
{
    WINDOW *w = win_tree;
    int rows, cols;
    getmaxyx(w, rows, cols);
    werase(w);

    draw_header(w, active_panel==1 ? "[ Call Tree ]" : "  Call Tree  ", active_panel==1);

    /* info line */
    wattron(w, COLOR_PAIR(CP_DIM));
    mvwprintw(w, 1, 1, "entry: ");
    wattroff(w, COLOR_PAIR(CP_DIM));
    wattron(w, COLOR_PAIR(CP_ENTRY) | A_BOLD);
    wprintw(w, "%-20s", entry_func);
    wattroff(w, COLOR_PAIR(CP_ENTRY) | A_BOLD);
    wattron(w, COLOR_PAIR(CP_DIM));
    wprintw(w, "  depth: ");
    wattroff(w, COLOR_PAIR(CP_DIM));
    wattron(w, COLOR_PAIR(CP_ENTRY) | A_BOLD);
    wprintw(w, "%d", max_depth);
    wattroff(w, COLOR_PAIR(CP_ENTRY) | A_BOLD);
    wattron(w, COLOR_PAIR(CP_DIM));
    wprintw(w, "  funcs: %d", ct_func_count());
    wattroff(w, COLOR_PAIR(CP_DIM));

    if (!parsed) {
        wattron(w, COLOR_PAIR(CP_DIM));
        mvwprintw(w, 3, 2, "No tree yet.");
        mvwprintw(w, 4, 2, "1. Press f to go to the file panel");
        mvwprintw(w, 5, 2, "2. Space to select .c/.cpp/.h files");
        mvwprintw(w, 6, 2, "3. Press t, then r here to build the tree");
        wattroff(w, COLOR_PAIR(CP_DIM));
        box(w, 0, 0);
        wrefresh(w);
        return;
    }

    update_visibility();
    int vc = visible_count();
    int list_rows = rows - 3;

    if (tree_cursor < tree_scroll) tree_scroll = tree_cursor;
    if (tree_cursor >= tree_scroll + list_rows) tree_scroll = tree_cursor - list_rows + 1;

    /* precompute last-child flags per depth for tree drawing */
    /* we'll recompute per row */
    int last_at_depth[64] = {0};

    int row = 0;
    int vis_idx = 0;
    for (int i = 0; i < ntree && row < list_rows; i++) {
        if (!tree[i].visible) continue;
        int cur_vis = vis_idx++;
        if (cur_vis < tree_scroll) continue;

        TreeNode *nd = &tree[i];
        int scr_row = cur_vis - tree_scroll + 2;
        int is_cur  = (cur_vis == tree_cursor);

        /* determine if this node is the last sibling */
        int is_last = 1;
        for (int j = i+1; j < ntree; j++) {
            if (tree[j].parent == nd->parent && tree[j].visible) {
                is_last = 0; break;
            }
        }
        if (nd->depth < 64) last_at_depth[nd->depth] = is_last;

        /* cursor highlight */
        if (is_cur) wattron(w, COLOR_PAIR(CP_CURSOR) | A_BOLD);

        /* draw tree prefix */
        draw_tree_prefix(w, scr_row, 1, nd->depth, is_last, last_at_depth);

        /* expand/collapse indicator */
        int prefix_end = 1 + nd->depth * 3;
        if (nd->has_children && !nd->is_cycle && !nd->is_unknown) {
            wattron(w, COLOR_PAIR(CP_BRANCH));
            mvwprintw(w, scr_row, prefix_end, nd->expanded ? "▼ " : "▶ ");
            wattroff(w, COLOR_PAIR(CP_BRANCH));
        } else {
            mvwprintw(w, scr_row, prefix_end, "  ");
        }

        /* function name */
        int name_col = prefix_end + 2;
        if (is_cur) {
            /* already have CP_CURSOR */
        } else if (nd->is_cycle)   wattron(w, COLOR_PAIR(CP_CYCLE));
        else if (nd->is_unknown)   wattron(w, COLOR_PAIR(CP_UNKNOWN));
        else if (nd->depth == 0)   wattron(w, COLOR_PAIR(CP_ENTRY) | A_BOLD);
        else                       wattron(w, COLOR_PAIR(CP_NORMAL));

        int avail = cols - name_col - 2;
        if (avail < 4) avail = 4;
        mvwprintw(w, scr_row, name_col, "%-*.*s", avail, avail, nd->name);

        /* annotation */
        if (nd->is_cycle)
            wprintw(w, " ↺");
        else if (nd->is_unknown)
            wprintw(w, " ?");

        wattroff(w, COLOR_PAIR(CP_CURSOR)|COLOR_PAIR(CP_CYCLE)|
                    COLOR_PAIR(CP_UNKNOWN)|COLOR_PAIR(CP_ENTRY)|
                    COLOR_PAIR(CP_NORMAL)|A_BOLD);

        row++;
    }

    /* scroll indicator */
    if (vc > list_rows) {
        wattron(w, COLOR_PAIR(CP_DIM));
        mvwprintw(w, 0, cols-10, "%d/%d", tree_cursor+1, vc);
        wattroff(w, COLOR_PAIR(CP_DIM));
    }

    box(w, 0, 0);
    wrefresh(w);
}

static void draw_status(const char *msg, int active_panel, int parsed, int nsel)
{
    WINDOW *w = win_status;
    int wide = getmaxx(w);
    werase(w);

    /* line 0: context-aware hint or transient status message */
    wattron(w, COLOR_PAIR(CP_ENTRY) | A_BOLD);
    if (msg) {
        mvwprintw(w, 0, 0, "%-*.*s", wide, wide, msg);
    } else if (active_panel == 0) {
        if (nsel == 0)
            mvwprintw(w, 0, 0, "%-*.*s", wide, wide,
                "→ Space to select a file, then t + r to build the tree");
        else
            mvwprintw(w, 0, 0, "%-*.*s", wide, wide,
                "→ Press t then r to build the tree from your selection");
    } else {
        if (!parsed)
            mvwprintw(w, 0, 0, "%-*.*s", wide, wide,
                "→ Press f to pick files first, then come back and press r");
        else
            mvwprintw(w, 0, 0, "%-*.*s", wide, wide,
                "→ f to pick different files  |  e to change entry function");
    }
    wattroff(w, COLOR_PAIR(CP_ENTRY) | A_BOLD);

    /* line 1: always-on key legend, changes per panel */
    wattron(w, COLOR_PAIR(CP_DIM));
    if (active_panel == 0)
        mvwprintw(w, 1, 0, "Space:select  a:select-all  Enter:open dir  Bksp:up  t:tree  ?:help  q:quit");
    else
        mvwprintw(w, 1, 0, "Space/Enter:expand  e:entry  +/-:depth  r:reparse  f:files  ?:help  q:quit");
    wattroff(w, COLOR_PAIR(CP_DIM));

    wrefresh(w);
}

static void draw_help(void)
{
    int rows = LINES, cols = COLS;
    int h = 16, w = 58;
    int y = (rows-h)/2, x = (cols-w)/2;
    WINDOW *hw = newwin(h, w, y, x);
    wbkgd(hw, COLOR_PAIR(CP_NORMAL));
    box(hw, 0, 0);
    wattron(hw, COLOR_PAIR(CP_HEADER) | A_BOLD);
    mvwprintw(hw, 0, 2, " Help ");
    wattroff(hw, COLOR_PAIR(CP_HEADER) | A_BOLD);

    int r = 2;
    wattron(hw, A_BOLD); mvwprintw(hw, r++, 2, "File panel"); wattroff(hw, A_BOLD);
    mvwprintw(hw, r++, 2, "  Space    select/deselect a file");
    mvwprintw(hw, r++, 2, "  a        select/deselect all .c/.h here");
    mvwprintw(hw, r++, 2, "  Enter    open directory");
    mvwprintw(hw, r++, 2, "  Bksp     go up one directory");
    mvwprintw(hw, r++, 2, "  t        go to call tree panel");
    r++;
    wattron(hw, A_BOLD); mvwprintw(hw, r++, 2, "Tree panel"); wattroff(hw, A_BOLD);
    mvwprintw(hw, r++, 2, "  r        (re)build tree from selected files");
    mvwprintw(hw, r++, 2, "  Space    expand/collapse a node");
    mvwprintw(hw, r++, 2, "  e        set entry function (default: main)");
    mvwprintw(hw, r++, 2, "  +/-      increase/decrease tree depth");
    mvwprintw(hw, r++, 2, "  f        back to file panel");
    r++;
    mvwprintw(hw, r++, 2, "Press any key to close this help");

    wrefresh(hw);
    wgetch(hw);
    delwin(hw);
}

/* ── mini prompt (bottom of screen) ──────────────────── */
static int prompt(const char *label, char *buf, int bufsz)
{
    int rows = LINES;
    int cols = COLS;
    WINDOW *pw = newwin(1, cols, rows-1, 0);
    echo();
    curs_set(1);
    wattron(pw, COLOR_PAIR(CP_ENTRY) | A_BOLD);
    mvwprintw(pw, 0, 0, "%s", label);
    wattroff(pw, COLOR_PAIR(CP_ENTRY) | A_BOLD);
    wattron(pw, COLOR_PAIR(CP_NORMAL));
    int llen = strlen(label);
    wmove(pw, 0, llen);
    wgetnstr(pw, buf, bufsz-1);
    wattroff(pw, COLOR_PAIR(CP_NORMAL));
    noecho();
    curs_set(0);
    delwin(pw);
    return strlen(buf) > 0;
}

/* ── layout ───────────────────────────────────────────── */
static void create_windows(void)
{
    int rows = LINES, cols = COLS;
    int file_w = cols / 3;
    int tree_w = cols - file_w;
    int content_h = rows - 2;

    win_files  = newwin(content_h, file_w, 0, 0);
    win_tree   = newwin(content_h, tree_w, 0, file_w);
    win_status = newwin(2, cols, rows-2, 0);

    keypad(win_files, TRUE);
    keypad(win_tree,  TRUE);
}

static void destroy_windows(void)
{
    delwin(win_files);
    delwin(win_tree);
    delwin(win_status);
}

static void resize_windows(void)
{
    destroy_windows();
    create_windows();
}

/* ── event loop ───────────────────────────────────────── */
int main(int argc, char **argv)
{
    /* determine starting directory */
    if (argc > 1) {
        struct stat st;
        if (stat(argv[1], &st) == 0 && S_ISDIR(st.st_mode))
            strncpy(cwd, argv[1], MAX_PATH-1);
        else {
            fprintf(stderr, "Usage: calltui [directory]\n");
            return 1;
        }
    } else {
        if (!getcwd(cwd, MAX_PATH)) strncpy(cwd, ".", MAX_PATH-1);
    }

    /* ncurses init */
    initscr();
    start_color();
    use_default_colors();
    noecho();
    cbreak();
    curs_set(0);

    /* colour palette */
    init_pair(CP_NORMAL,   COLOR_WHITE,   -1);
    init_pair(CP_SELECTED, COLOR_GREEN,   -1);
    init_pair(CP_HEADER,   COLOR_BLACK,   COLOR_CYAN);
    init_pair(CP_CURSOR,   COLOR_BLACK,   COLOR_WHITE);
    init_pair(CP_DIM,      COLOR_WHITE,   -1);   /* will use A_DIM */
    init_pair(CP_BRANCH,   COLOR_CYAN,    -1);
    init_pair(CP_ENTRY,    COLOR_YELLOW,  -1);
    init_pair(CP_UNKNOWN,  COLOR_RED,     -1);
    init_pair(CP_CYCLE,    COLOR_MAGENTA, -1);
    init_pair(CP_KEY,      COLOR_GREEN,   -1);

    create_windows();
    load_dir(cwd);

    const char *status = NULL;

    for (;;) {
        draw_files_panel();
        draw_tree_panel();
        draw_status(status, active_panel, parsed, nsel);

        WINDOW *w = active_panel == 0 ? win_files : win_tree;
        int ch = wgetch(w);

        if (ch == KEY_RESIZE) {
            resize_windows();
            continue;
        }

        if (ch == '?') {
            draw_help();
            continue;
        }

        status = NULL;  /* clear transient message on next keypress */

        /* ── file panel keys ── */
        if (active_panel == 0) {
            if (ch == 'q') break;

            if (ch == KEY_UP   || ch == 'k') {
                if (dir_cursor > 0) dir_cursor--;
            } else if (ch == KEY_DOWN || ch == 'j') {
                if (dir_cursor < ndir-1) dir_cursor++;
            } else if (ch == '\n' || ch == KEY_ENTER) {
                DirEntry *e = &dir_entries[dir_cursor];
                if (e->is_dir) {
                    char newpath[MAX_PATH];
                    if (!strcmp(e->name, "..")) {
                        /* go up */
                        char *slash = strrchr(cwd, '/');
                        if (slash && slash != cwd) {
                            *slash = '\0';
                            strncpy(newpath, cwd, MAX_PATH-1);
                        } else {
                            strncpy(newpath, "/", MAX_PATH-1);
                        }
                    } else {
                        snprintf(newpath, MAX_PATH, "%s", e->fullpath);
                    }
                    load_dir(newpath);
                }
            } else if (ch == KEY_BACKSPACE || ch == 127) {
                /* go up */
                char newpath[MAX_PATH];
                char *slash = strrchr(cwd, '/');
                if (slash && slash != cwd) {
                    strncpy(newpath, cwd, MAX_PATH-1);
                    newpath[slash - cwd] = '\0';
                } else {
                    strncpy(newpath, "/", MAX_PATH-1);
                }
                load_dir(newpath);
            } else if (ch == ' ') {
                toggle_select(dir_cursor);
                if (dir_cursor < ndir-1) dir_cursor++;
            } else if (ch == 'a') {
                select_all_c();
            } else if (ch == 't') {
                active_panel = 1;
            } else if (ch == 'r') {
                if (nsel == 0) status = "No files selected.";
                else { do_parse(); status = "Parsed."; }
            }

        /* ── tree panel keys ── */
        } else {
            if (ch == 'q') break;

            int vc = visible_count();

            if (ch == KEY_UP || ch == 'k') {
                if (tree_cursor > 0) tree_cursor--;
            } else if (ch == KEY_DOWN || ch == 'j') {
                if (tree_cursor < vc-1) tree_cursor++;
            } else if (ch == ' ' || ch == '\n' || ch == KEY_ENTER) {
                int ni = visible_index(tree_cursor);
                if (ni >= 0 && tree[ni].has_children &&
                    !tree[ni].is_cycle && !tree[ni].is_unknown) {
                    tree[ni].expanded = !tree[ni].expanded;
                    update_visibility();
                    /* keep cursor in bounds */
                    int nvc = visible_count();
                    if (tree_cursor >= nvc) tree_cursor = nvc-1;
                }
            } else if (ch == 'e') {
                char buf[MAX_NAME] = "";
                if (prompt("Entry function: ", buf, MAX_NAME)) {
                    strncpy(entry_func, buf, MAX_NAME-1);
                    if (parsed) { rebuild_tree(); update_visibility(); }
                }
            } else if (ch == '+' || ch == '=') {
                max_depth++;
                if (parsed) { rebuild_tree(); update_visibility(); }
            } else if (ch == '-') {
                if (max_depth > 1) max_depth--;
                if (parsed) { rebuild_tree(); update_visibility(); }
            } else if (ch == 'r') {
                if (nsel == 0) status = "No files selected.";
                else { do_parse(); status = "Parsed."; }
            } else if (ch == 'f') {
                active_panel = 0;
            }
        }
    }

    destroy_windows();
    endwin();
    return 0;
}
