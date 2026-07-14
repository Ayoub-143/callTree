#pragma once
/*
 * calltree.h  –  public API for calltree.c when built as a library
 *
 * Compile calltree.c with -DCALLTREE_LIB to suppress its own main().
 */

#ifndef MAX_NAME
#define MAX_NAME 128
#endif

/* opaque forward declaration – callers use pointers only */
typedef struct Func_s Func;

/* reset all state (call before re-parsing a new set of files) */
void        ct_reset(void);

/* parse a single C/C++ source file into the global call graph */
void        ct_parse_file(const char *path);

/* query the graph */
int         ct_func_count(void);
const char *ct_func_name(int index);          /* iterate all funcs  */
const Func *ct_find(const char *name);        /* look up by name    */
int         ct_ncalls(const Func *f);
const char *ct_call(const Func *f, int i);    /* i-th callee name   */