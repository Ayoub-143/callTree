/*
 * calltree.c  –  "heuristic"(or whatever it's called) C/C++ call-tree printer
 *
 * Build standalone:  gcc -O2 -o calltree calltree.c
 * Build as library:  gcc -O2 -DCALLTREE_LIB -c -o calltree.o calltree.c
 * Use:               ./calltree -e main -d 5 src/*.c
 * made with AI's asistance (sad indeed)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── tunables ─────────────────────────────────────────── */
#define MAX_FUNCS   2048 //just definitions for already known/assumed maximums
#define MAX_CALLS    256
#define MAX_NAME     128
#define MAX_LINE    2048
#define DEFAULT_DEPTH  6

/* ── data model ───────────────────────────────────────── */
typedef struct {
    char name[MAX_NAME];
    char calls[MAX_CALLS][MAX_NAME]; //call table/matrix
    int  ncalls;
} Func;

static Func funcs[MAX_FUNCS]; //almost everything is static btw
static int  nfuncs = 0;

/* ── graph helpers ────────────────────────────────────── */
static Func *find_func(const char *n) //it just finds it in the array
{
    for (int i = 0; i < nfuncs; i++)
        if (!strcmp(funcs[i].name, n)) return &funcs[i];
    return NULL;
}

static Func *get_or_add(const char *n) //uses find func to create a new entry
{
    Func *f = find_func(n);
    if (f) return f;
    if (nfuncs >= MAX_FUNCS) return NULL;
    f = &funcs[nfuncs++];
    strncpy(f->name, n, MAX_NAME-1);
    f->ncalls = 0;
    return f;
}

static void add_call(Func *caller, const char *callee)//new call, who dis?
{
    if (!caller || caller->ncalls >= MAX_CALLS) return;
    for (int i = 0; i < caller->ncalls; i++)
        if (!strcmp(caller->calls[i], callee)) return;
    strncpy(caller->calls[caller->ncalls++], callee, MAX_NAME-1);
}

// keywords that look like calls
static const char *KW[] = {
    /* C keywords */
    "if","else","for","while","do","switch","case","return",
    "sizeof","typeof","__typeof__","offsetof","goto","break",
    "continue","typedef","struct","union","enum","static",
    "extern","inline","const","volatile","register","auto",
    "int","char","short","long","unsigned","signed","void",
    "float","double","bool","NULL",
    /* C++ keywords */
    "catch","try","throw","new","delete","class","namespace",
    "using","this","operator","template","typename","public",
    "private","protected","virtual","override","final",
    "explicit","friend","mutable","constexpr","consteval",
    "constinit","noexcept","nullptr","static_assert",
    "static_cast","dynamic_cast","reinterpret_cast","const_cast",
    "and","or","not","xor","bitand","bitor","compl",
    "and_eq","or_eq","xor_eq","not_eq",
    "true","false","export","import","module","co_await",
    "co_return","co_yield","requires","concept","decltype",
    "alignas","alignof","typeid","thread_local",
    NULL
};

static int is_kw(const char *w)//keyword check
{
    for (int i = 0; KW[i]; i++) if (!strcmp(KW[i],w)) return 1;
    return 0;
}

static int is_idc(char c) { return isalnum((unsigned char)c)||c=='_'; }

// tokeniser by ai
static char *read_file(const char *path, size_t *outsz)
{
    FILE *fp = fopen(path, "r");
    if (!fp) { perror(path); return NULL; }
    fseek(fp, 0, SEEK_END);
    size_t sz = (size_t)ftell(fp);
    rewind(fp);
    char *buf = malloc(sz + 2);
    if (!buf) { fclose(fp); return NULL; }
    sz = fread(buf, 1, sz, fp);
    buf[sz] = buf[sz+1] = '\0';
    fclose(fp);
    *outsz = sz;
    return buf;
}

static void scrub(char *s, size_t n) //a "comments remover" by ai too cuz look at that thing! u want me to go do this brainf**k (that is the name of the lang respectfully) looking code?? pass
{
    size_t i = 0;
    while (i < n) {
        if (s[i]=='/' && i+1<n && s[i+1]=='*') {
            i += 2;
            while (i+1 < n && !(s[i]=='*' && s[i+1]=='/')) {
                if (s[i] != '\n') s[i] = ' ';
                i++;
            }
            if (i+1 < n) i += 2;
            continue;
        }
        if (s[i]=='/' && i+1<n && s[i+1]=='/') {
            i += 2;
            while (i < n && s[i] != '\n') { s[i++] = ' '; }
            continue;
        }
        if (s[i] == '"') {
            i++;
            while (i < n && s[i] != '"') {
                if (s[i]=='\\') i++;
                if (s[i]!='\n') s[i] = ' ';
                i++;
            }
            if (i < n) i++;
            continue;
        }
        if (s[i] == '\'') {
            i++;
            while (i < n && s[i] != '\'') {
                if (s[i]=='\\') i++;
                s[i++] = ' ';
            }
            if (i < n) i++;
            continue;
        }
        i++;
    }
}

/* block kinds tracked on the brace stack, so namespace/class bodies
 * don't get mistaken for function bodies (and so calls inside them
 * still get attributed to the right enclosing function, or none) */
#define BLK_OTHER     0   /* if/for/while/switch/lambda/unknown block */
#define BLK_NAMESPACE 1
#define BLK_CLASS     2   /* class/struct/union/enum body */
#define BLK_FUNC      3

#define MAX_PARSE_DEPTH 512

static void parse_buf(const char *buf, size_t n) //the parser with depth + namespace/class detection
{
    int   depth   = 0;
    Func *cur     = NULL;
    char  cand[MAX_NAME] = "";   /* candidate function name (leaf, unqualified) */

    int   blocktype[MAX_PARSE_DEPTH];
    Func *saved_cur[MAX_PARSE_DEPTH];
    blocktype[0] = BLK_OTHER;
    saved_cur[0] = NULL;

    /* what kind of block the *next* '{' should open, based on the
     * most recently seen leading keyword (namespace/class/struct/union) */
    int pending_blk = BLK_OTHER;

    /* paren tracking, so we can recognize a ctor member-initializer
     * list: "Note(...) : id(id), subject(subject) {" — once we've
     * closed a candidate's own parameter list and see a lone ':'
     * (not '::') right after, everything up to the '{' is init-list
     * entries like "id(id)", not a new function candidate. */
    int  paren_depth = 0;
    int  in_init_list = 0;

    size_t i = 0;
    while (i < n) {
        char c = buf[i];

        if (c == '(') {
            paren_depth++;
            i++; continue;
        }
        if (c == ')') {
            if (paren_depth > 0) paren_depth--;
            if (paren_depth == 0 && cand[0] && cur == NULL && !in_init_list) {
                size_t k = i+1;
                while (k < n && (buf[k]==' '||buf[k]=='\t'||buf[k]=='\n'||buf[k]=='\r')) k++;
                if (k < n && buf[k] == ':' && !(k+1 < n && buf[k+1] == ':'))
                    in_init_list = 1;
            }
            i++; continue;
        }
        if (c == '{') {
            int newtype = BLK_OTHER;
            if (pending_blk != BLK_OTHER) {
                newtype = pending_blk;
            } else if (cand[0] && cur == NULL) {
                newtype = BLK_FUNC;
            }

            if (depth + 1 < MAX_PARSE_DEPTH) depth++;
            blocktype[depth] = newtype;
            saved_cur[depth]  = cur;

            if (newtype == BLK_FUNC) {
                cur = get_or_add(cand);
            }
            /* BLK_NAMESPACE / BLK_CLASS / BLK_OTHER: leave cur as-is,
             * so calls inside namespaces/classes-without-a-func, or
             * inside nested if/for/lambda blocks, attribute correctly */

            cand[0] = '\0';
            pending_blk = BLK_OTHER;
            in_init_list = 0;
            i++; continue;
        }
        if (c == '}') {
            if (depth > 0) {
                int type = blocktype[depth];
                if (type == BLK_FUNC) cur = saved_cur[depth];
                depth--;
            } else {
                cur = NULL;
            }
            cand[0] = '\0';
            in_init_list = 0;
            i++; continue;
        }
        if (c == ';') {
            if (cur == NULL) cand[0] = '\0';
            pending_blk = BLK_OTHER;
            in_init_list = 0;
            i++; continue;
        }

        /* "::" scope resolution – just skip over it. Functions are keyed
         * by their leaf name (e.g. "paint" for both "Widget::paint(){...}"
         * and a call made as "w.paint()" or "Widget::paint()"), so a
         * definition and its call sites always agree on the name. Do NOT
         * clear `cand` here: "::" also shows up in unrelated type names
         * inside a signature (e.g. "const std::string& x"), and clearing
         * would wipe out an already-captured function-name candidate
         * before its "{" is ever reached. */
        if (c == ':' && i+1 < n && buf[i+1] == ':') {
            i += 2; continue;
        }

        if (is_idc(c)) {
            char word[MAX_NAME]; int wlen = 0;
            while (i < n && is_idc(buf[i]) && wlen < MAX_NAME-1)
                word[wlen++] = buf[i++];
            word[wlen] = '\0';

            if (!strcmp(word, "namespace")) {
                pending_blk = BLK_NAMESPACE;
                cand[0] = '\0';
                continue;
            }
            if (!strcmp(word,"class") || !strcmp(word,"struct") ||
                !strcmp(word,"union") || !strcmp(word,"enum")) {
                pending_blk = BLK_CLASS;
                cand[0] = '\0';
                continue;
            }

            size_t j = i;
            while (j < n && (buf[j]==' '||buf[j]=='\t'||buf[j]=='\n'||buf[j]=='\r')) j++;

            if (j < n && buf[j] == '(') {
                if (cur == NULL && !in_init_list && !is_kw(word)) {
                    strncpy(cand, word, MAX_NAME-1);
                } else if (cur && !is_kw(word)) {
                    add_call(cur, word);
                }
            }
            continue;
        }

        i++;
    }
}

/* ── public API (used by tui.c) ───────────────────────── */
//idk how tui's work have fun from here

void ct_reset(void)
{
    nfuncs = 0;
    memset(funcs, 0, sizeof(funcs));
}

void ct_parse_file(const char *path)
{
    size_t n;
    char *buf = read_file(path, &n);
    if (!buf) return;
    scrub(buf, n);
    parse_buf(buf, n);
    free(buf);
}

int ct_func_count(void) { return nfuncs; }

const char *ct_func_name(int i)
{
    if (i < 0 || i >= nfuncs) return NULL;
    return funcs[i].name;
}

/* Returns the Func pointer (opaque to caller via header typedef) */
const Func *ct_find(const char *name) { return find_func(name); }

int ct_ncalls(const Func *f)          { return f ? f->ncalls : 0; }
const char *ct_call(const Func *f, int i)
{
    if (!f || i < 0 || i >= f->ncalls) return NULL;
    return f->calls[i];
}

/* ── standalone main (stripped when -DCALLTREE_LIB) ──── */
#ifndef CALLTREE_LIB

static void print_tree(const char *name, int depth, int maxd,
                        char visited[][MAX_NAME], int *nv)
{
    for (int i = 0; i < depth; i++) fputs("  ", stdout);
    for (int i = 0; i < *nv; i++) {
        if (!strcmp(visited[i], name)) {
            printf("%s  [seen]\n", name);
            return;
        }
    }
    Func *f = find_func(name);
    if (!f) { printf("%s  (unknown)\n", name); return; }
    printf("%s\n", name);
    if (depth >= maxd) {
        if (f->ncalls) {
            for (int i = 0; i < depth+1; i++) fputs("  ", stdout);
            puts("...");
        }
        return;
    }
    strncpy(visited[*nv], name, MAX_NAME-1);
    (*nv)++;
    for (int i = 0; i < f->ncalls; i++)
        print_tree(f->calls[i], depth+1, maxd, visited, nv);
    (*nv)--;
}

static void list_funcs(void)
{
    printf("%d functions found:\n", nfuncs);
    for (int i = 0; i < nfuncs; i++)
        printf("  %s\n", funcs[i].name);
}

static void usage(const char *p)//tutu
{
    fprintf(stderr,
        "Usage: %s [options] file.c [file2.c ...]\n"
        "  -e <func>   entry point (default: main)\n"
        "  -d <n>      max depth   (default: %d)\n"
        "  -l          list all discovered functions\n"
        "  -h          help\n", p, DEFAULT_DEPTH);
    exit(1);
}

int main(int argc, char **argv)
{
    const char *entry = "main";
    int maxd = DEFAULT_DEPTH, do_list = 0;

    int i = 1;
    for (; i < argc && argv[i][0]=='-'; i++) {
        if      (!strcmp(argv[i],"-e") && i+1<argc) entry = argv[++i];
        else if (!strcmp(argv[i],"-d") && i+1<argc) maxd  = atoi(argv[++i]);
        else if (!strcmp(argv[i],"-l")) do_list = 1;
        else usage(argv[0]);
    }
    if (i >= argc) usage(argv[0]);

    for (; i < argc; i++) ct_parse_file(argv[i]);

    if (do_list) { list_funcs(); return 0; }

    if (!find_func(entry)) {
        fprintf(stderr, "'%s' not found. Try -l to list functions.\n", entry);
        return 1;
    }

    char visited[MAX_FUNCS][MAX_NAME];
    int  nv = 0;
    print_tree(entry, 0, maxd, visited, &nv);
    return 0;
}

#endif /* CALLTREE_LIB */