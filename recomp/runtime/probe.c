// Function probes in the style of smm2-hooks (its src/probe.cpp and docs/probe.md): any guest
// function, read after it returns, with fields at paths from its entry registers or from the main module's globals.
// The config is smm2-hooks' probe.txt and the output its probe.log, line for line, so its tools/probe.py decode reads
// a recomp run as it reads an Eden one.
//
//   RECOMP_PROBE=<probe.txt>        hooks and fields (below); read once at start
//   RECOMP_PROBE_OUT=<probe.log>    the log (default probe.log in the working directory)
//
//   hook <name> <0x71...> [every=N] [callers=N]     a function by its entry (the 0x71... addresses of the game's
//                                                   symbol tables; the main module is at 0x7100000000 there)
//   field <hook> <label> <u8|u16|u32|u64|f32> <path>
//     0x230 / 0x550>0x14     from x0 at entry; every step but the last dereferences
//     x2:0x70>0x8            from argument register x2 at entry
//     @0x7102C19290>0x28>0x4A0   from a main-module global (0x71... address), dereferenced
//     @@0x7012345678>0x10    from an absolute guest address, not dereferenced
//
// The log: V,1; one H line per hook (H,<i>,<name>,<addr>,ok,<label>:<type>...,lr<j>:u64...); per call an R line
// (R,<frame>,<i>,x0..x7 at entry in hex,<fields>...,<callers>...: '-' for a field whose path left guest memory); and a
// P line (P,<frame>,<buttons>,<lx>,<ly>) the first time a frame is logged: the pad as the game last read it (a pad
// script's state as hle/input.c hands it over, its stick directions included). The frame is the count of presented
// frames, one value for every row of a game frame (the pad script's clock ticks inside the player's update, halfway
// through one). The log is flushed at each new frame and before the runtime's _exit (probe_flush).
//
// How "after it returns" is caught: the generated code calls FN_ENTER at every function's entry and FN_AFTER after
// every call site (tools/gen.py). A probed function's entry pushes its registers, its return address (x30) and its
// sp; a call site that returns to that address at that sp pops it and logs it. A function reached by a tail jump
// returns through its caller's caller, whose call site has that return address too, as smm2-hooks' trampoline (which
// returns through x30) sees it. Entries left behind (a frame the code never returned through) are dropped when a
// shallower call site returns.
//
// callers=N: lr0 is the return address at entry, and the walk follows the frame records from the guest's x29 at
// entry, as smm2-hooks walks them from its trampoline's own frame (whose record holds lr0 again: lr1 repeats lr0
// there, and here too, so the columns mean the same).
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "rt.h"

volatile int g_probe;
volatile int g_probe_pending;

enum { MAX_HOOKS = 16, MAX_FIELDS = 24, MAX_CALLERS = 8, MAX_DEPTH = 6, MAX_PENDING = 256 };
#define SYM_BASE 0x7100000000ull   // the main module's base in the game's symbol tables (functions.csv)

enum { T_U8, T_U16, T_U32, T_U64, T_F32 };
typedef struct {
    char name[24];
    uint8_t type, depth, from, reg;   // from: 0 x0, 1 the main module (@), 2 an absolute address (@@), 3 register reg
    uint32_t path[MAX_DEPTH];
    uint64_t base;                    // from 2
} Field;
typedef struct {
    char name[24];
    uint64_t addr;                    // the guest address of its entry
    uint64_t sym;                     // as probe.txt gave it
    uint32_t every, calls;
    uint8_t callers;
    int nfields;
    Field fields[MAX_FIELDS];
} Hook;
typedef struct {
    int hook;
    uint64_t x[8], lr, fp, sp;
} Pending;

static Hook hooks[MAX_HOOKS];
static int nhooks;
static FILE *out;
static pthread_mutex_t out_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t last_pad_frame = -1;
static __thread Pending pending[MAX_PENDING];
static __thread int npending;

static int readable(uint64_t a, uint64_t n) { return a >= GUEST_LO && a + n <= GUEST_HI && a + n > a; }
extern uint64_t renderer_present_count(void);
static int64_t frame_now(void) { return (int64_t)renderer_present_count(); }
static uint64_t pad_buttons;
static int32_t pad_lx, pad_ly;
void probe_pad_read(uint64_t buttons, int32_t lx, int32_t ly) { pad_buttons = buttons, pad_lx = lx, pad_ly = ly; }

static void logf_locked(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_locked(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
}

static int read_field(const uint64_t *x, const Field *f, uint64_t *v) {
    uint64_t p = f->from == 3 ? x[f->reg] : f->from == 2 ? f->base : f->from == 1 ? MAIN_BASE : x[0];
    for (int i = 0; i + 1 < f->depth; i++) {
        p += f->path[i];
        if (!readable(p, 8) || (p & 7)) return 0;
        p = LD64(p);
    }
    p += f->path[f->depth - 1];
    switch (f->type) {
    case T_U8: if (!readable(p, 1)) return 0; *v = LD8(p); return 1;
    case T_U16: if (!readable(p, 2) || (p & 1)) return 0; *v = LD16(p); return 1;
    case T_U32: case T_F32: if (!readable(p, 4) || (p & 3)) return 0; *v = LD32(p); return 1;
    case T_U64: if (!readable(p, 8) || (p & 7)) return 0; *v = LD64(p); return 1;
    }
    return 0;
}

static uint64_t module_relative(uint64_t a) {
    return a >= MAIN_BASE && a < MAIN_BASE + 0x2000000ull ? a - MAIN_BASE + SYM_BASE : 0;
}

static void log_call(const Pending *e) {
    Hook *h = &hooks[e->hook];
    h->calls++;
    if (h->every > 1 && h->calls % h->every) return;
    pthread_mutex_lock(&out_lock);
    const int64_t f = frame_now();
    if (f != last_pad_frame) {
        if (last_pad_frame >= 0) fflush(out);
        last_pad_frame = f;
        logf_locked("P,%lld,%llx,%d,%d\n", (long long)f, (unsigned long long)pad_buttons, pad_lx, pad_ly);
    }
    logf_locked("R,%lld,%d", (long long)f, e->hook);
    for (int i = 0; i < 8; i++) logf_locked(",%llx", (unsigned long long)e->x[i]);
    for (int i = 0; i < h->nfields; i++) {
        uint64_t v;
        if (read_field(e->x, &h->fields[i], &v)) logf_locked(",%llx", (unsigned long long)v);
        else logf_locked(",-");
    }
    // lr0, then the frame records from x29 at entry behind a record that holds lr0 (smm2-hooks' trampoline frame)
    uint64_t lr = e->lr, fp = e->fp;
    for (int i = 0; i < h->callers; i++) {
        const uint64_t rel = module_relative(lr);
        if (rel) logf_locked(",%llx", (unsigned long long)rel); else logf_locked(",-");
        if (i == 0) continue;   // the trampoline's own record: lr0 again, then the game's from x29
        if (!readable(fp, 16) || (fp & 7)) { lr = 0; fp = 0; continue; }
        lr = LD64(fp + 8);
        fp = LD64(fp);
    }
    logf_locked("\n");
    pthread_mutex_unlock(&out_lock);
}

void probe_enter(uint64_t addr, Cpu *c) {
    for (int i = 0; i < nhooks; i++) {
        if (hooks[i].addr != addr) continue;
        if (npending == MAX_PENDING) return;   // deeper than any frame the game keeps: dropped
        Pending *e = &pending[npending++];
        e->hook = i;
        memcpy(e->x, c->x, sizeof e->x);
        e->lr = c->x[30], e->fp = c->x[29], e->sp = c->sp;
        __atomic_add_fetch(&g_probe_pending, 1, __ATOMIC_RELAXED);
        return;
    }
}

void probe_after(uint64_t ret, Cpu *c) {
    while (npending) {
        Pending *e = &pending[npending - 1];
        if (e->sp < c->sp) {   // a deeper frame the code did not return through
            npending--;
            __atomic_sub_fetch(&g_probe_pending, 1, __ATOMIC_RELAXED);
            continue;
        }
        if (e->sp != c->sp || e->lr != ret) return;
        npending--;
        __atomic_sub_fetch(&g_probe_pending, 1, __ATOMIC_RELAXED);
        log_call(e);
    }
}

// Both hooks, from FN_ENTER (cpu.h): the profiler's and the probes'.
void fn_enter_hook(uint64_t addr, Cpu *c) {
    if (g_fnprof) fnprof_hit(addr, c->x[30], c->x[0]);
    extern bool boot_holds(void);  // (runtime/game_hooks.c: a direct boot not yet in its course: nothing to probe yet)
    if (g_probe && !boot_holds()) probe_enter(addr, c);
}

// ---- probe.txt

static char *token(char **s) {
    while (**s == ' ' || **s == '\t') (*s)++;
    if (!**s) return NULL;
    char *t = *s;
    while (**s && **s != ' ' && **s != '\t') (*s)++;
    if (**s) *(*s)++ = 0;
    return t;
}

static int parse_type(const char *t, uint8_t *v) {
    static const char *names[] = {"u8", "u16", "u32", "u64", "f32"};
    for (int i = 0; i < 5; i++)
        if (!strcmp(t, names[i])) return *v = (uint8_t)i, 1;
    return 0;
}

static Hook *find_hook(const char *name) {
    for (int i = 0; i < nhooks; i++)
        if (!strcmp(hooks[i].name, name)) return &hooks[i];
    return NULL;
}

static void parse_hook(char *s) {
    if (nhooks == MAX_HOOKS) { fprintf(out, "E,too many hooks (max %d)\n", MAX_HOOKS); return; }
    char *name = token(&s), *addr = token(&s);
    if (!name || !addr) { fprintf(out, "E,hook needs <name> <addr>\n"); return; }
    Hook *h = &hooks[nhooks];
    memset(h, 0, sizeof *h);
    snprintf(h->name, sizeof h->name, "%s", name);
    h->sym = strtoull(addr, NULL, 16);
    h->every = 1;
    for (char *o = token(&s); o; o = token(&s)) {
        if (!strncmp(o, "every=", 6)) h->every = (uint32_t)strtoul(o + 6, NULL, 10);
        else if (!strncmp(o, "callers=", 8)) {
            unsigned long n = strtoul(o + 8, NULL, 10);
            h->callers = (uint8_t)(n > MAX_CALLERS ? MAX_CALLERS : n);
        }
    }
    if (h->sym < SYM_BASE || h->sym >= SYM_BASE + 0x2000000ull) {
        fprintf(out, "E,%s: address %s outside main\n", h->name, addr);
        return;
    }
    h->addr = h->sym - SYM_BASE + MAIN_BASE;
    nhooks++;
}

static void parse_field(char *s) {
    char *hname = token(&s), *label = token(&s), *type = token(&s), *path = token(&s);
    if (!hname || !label || !type || !path) { fprintf(out, "E,field needs <hook> <label> <type> <path>\n"); return; }
    Hook *h = find_hook(hname);
    if (!h) { fprintf(out, "E,field %s: unknown hook %s\n", label, hname); return; }
    if (h->nfields == MAX_FIELDS) { fprintf(out, "E,%s: too many fields (max %d)\n", hname, MAX_FIELDS); return; }
    Field *f = &h->fields[h->nfields];
    memset(f, 0, sizeof *f);
    snprintf(f->name, sizeof f->name, "%s", label);
    if (!parse_type(type, &f->type)) { fprintf(out, "E,field %s: bad type %s\n", label, type); return; }
    char *p = path;
    if (p[0] == 'x' && p[1] >= '0' && p[1] <= '7' && p[2] == ':') {
        f->from = 3, f->reg = (uint8_t)(p[1] - '0');
        p += 3;
    } else if (p[0] == '@' && p[1] == '@') {
        f->from = 2;
        p += 2;
        char *sep = strchr(p, '>');
        if (sep) *sep = 0;
        f->base = strtoull(p, NULL, 16);
        if (!f->base) { fprintf(out, "E,field %s: bad @@address\n", label); return; }
        p = sep ? sep + 1 : NULL;
        if (!p || !*p) f->path[f->depth++] = 0;
    } else if (*p == '@') {
        f->from = 1;
        p++;
        char *sep = strchr(p, '>');
        if (sep) *sep = 0;
        unsigned long long va = strtoull(p, NULL, 16);
        if (va < SYM_BASE) { fprintf(out, "E,field %s: @address below main\n", label); return; }
        f->path[f->depth++] = (uint32_t)(va - SYM_BASE);
        p = sep ? sep + 1 : NULL;
    }
    while (p && *p) {
        if (f->depth == MAX_DEPTH) { fprintf(out, "E,field %s: path deeper than %d\n", label, MAX_DEPTH); return; }
        char *sep = strchr(p, '>');
        if (sep) *sep = 0;
        f->path[f->depth++] = (uint32_t)strtoul(p, NULL, 16);
        p = sep ? sep + 1 : NULL;
    }
    if (!f->depth) { fprintf(out, "E,field %s: empty path\n", label); return; }
    h->nfields++;
}

void probe_flush(void) {
    if (!out) return;
    pthread_mutex_lock(&out_lock);
    fflush(out);
    pthread_mutex_unlock(&out_lock);
}

__attribute__((constructor)) static void probe_init(void) {
    const char *cfg = getenv("RECOMP_PROBE");
    if (!cfg || !*cfg) return;
    const char *path = getenv("RECOMP_PROBE_OUT");
    out = fopen(path && *path ? path : "probe.log", "w");
    if (!out) { fprintf(stderr, "[probe] cannot write %s\n", path && *path ? path : "probe.log"); return; }
    fprintf(out, "V,1\n");
    FILE *f = fopen(cfg, "r");
    if (!f) { fprintf(out, "E,cannot read %s\n", cfg); fflush(out); return; }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *s = line, *kw = token(&s);
        if (!kw || *kw == '#') continue;
        if (!strcmp(kw, "hook")) parse_hook(s);
        else if (!strcmp(kw, "field")) parse_field(s);
        else fprintf(out, "E,unknown keyword %s\n", kw);
    }
    fclose(f);
    for (int i = 0; i < nhooks; i++) {
        const Hook *h = &hooks[i];
        fprintf(out, "H,%d,%s,%llx,ok", i, h->name, (unsigned long long)h->sym);
        for (int j = 0; j < h->nfields; j++) {
            static const char *names[] = {"u8", "u16", "u32", "u64", "f32"};
            fprintf(out, ",%s:%s", h->fields[j].name, names[h->fields[j].type]);
        }
        for (int j = 0; j < h->callers; j++) fprintf(out, ",lr%d:u64", j);
        fprintf(out, "\n");
    }
    fflush(out);
    atexit(probe_flush);
    if (nhooks) g_probe = 1;
    fprintf(stderr, "[probe] %d hooks from %s\n", nhooks, cfg);
}
