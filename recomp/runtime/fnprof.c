// Function-entry profiler. LARECOMP_FNPROF=<from>:<to> counts guest function entries (every generated
// function calls FN_ENTER) between those presented frames, then writes build/fnprof.txt (addr count, most
// called first). Diffing two runs shows which game functions a change brings in; counts also show hot code.
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "hle.h"
#include "platform.h"

volatile int g_fnprof;

#define NSLOT (1 << 18)
static struct { _Atomic uint64_t addr; _Atomic uint64_t count; } slots[NSLOT];

// LARECOMP_FNPROF_BT=<hex addr>[,...]: also print the host backtrace (guest functions are f_<addr>) the first time
// each listed function is entered inside the window - who calls it
static uint64_t bt_addrs[256];
static int bt_n = -1;
static _Atomic uint8_t bt_done[256];

uint64_t g_bt_first_x0;

static void bt_check(uint64_t a, uint64_t lr, uint64_t x0) {
    if (bt_n < 0) {
        bt_n = 0;
        const char *e = getenv("LARECOMP_FNPROF_BT");
        for (const char *p = e; p && *p && bt_n < 256;) {
            bt_addrs[bt_n++] = strtoull(p, (char **)&p, 16);
            while (*p == ',' || *p == ' ') p++;
        }
    }
    // LARECOMP_FNPROF_BT_FROM=<presented frame>: only entries from that frame on (a call in play, not a create's)
    static long bt_from = -1;
    if (bt_from < 0) bt_from = getenv("LARECOMP_FNPROF_BT_FROM") ? atol(getenv("LARECOMP_FNPROF_BT_FROM")) : 0;
    if (bt_from) {
        extern uint64_t renderer_present_count(void);
        if (renderer_present_count() < (uint64_t)bt_from) return;
    }
    // LARECOMP_FNPROF_BT_X0_AT=<hex addr>,<hex off>: only entries whose x0 is the pointer at addr + off (one object's)
    static int x0_set = -1;
    static uint64_t x0_addr, x0_off;
    if (x0_set < 0) {
        const char *e = getenv("LARECOMP_FNPROF_BT_X0_AT");
        x0_set = e != NULL;
        if (e) { char *p; x0_addr = strtoull(e, &p, 16); x0_off = *p ? strtoull(p + 1, NULL, 16) : 0; }
    }
    if (x0_set) {
        uint64_t at = x0_addr + x0_off;
        if (at < GUEST_LO || at + 8 > GUEST_HI || x0 != *(uint64_t *)(uintptr_t)at) return;
    }
    for (int i = 0; i < bt_n; i++)
        if (bt_addrs[i] == a && !atomic_exchange(&bt_done[i], 1)) {
            if (i == 0) g_bt_first_x0 = x0;
            uint64_t vt = x0 >= GUEST_LO && x0 < GUEST_HI ? *(uint64_t *)(uintptr_t)x0 : 0;
            uint64_t v100 = vt >= GUEST_LO && vt < GUEST_HI ? *(uint64_t *)(uintptr_t)(vt + 0x100) : 0;
            uint64_t p8 = x0 >= GUEST_LO && x0 < GUEST_HI ? *(uint64_t *)(uintptr_t)(x0 + 8) : 0;
            fprintf(stderr, "[fnprof-bt] %llx entered from guest lr %llx x0 %llx (vtable %llx, +0x100 %llx, [x0+8] %llx):\n",
                    (unsigned long long)a, (unsigned long long)lr, (unsigned long long)x0, (unsigned long long)vt,
                    (unsigned long long)v100, (unsigned long long)p8);
            plat_print_backtrace();  // (execinfo is macOS / glibc only)
        }
}

// RECOMP_CALL_RING=<n>: every thread keeps its last n guest function entries (address, caller, x0); a fatal error
// prints the crashing thread's (fnprof_dump_ring, from rt_fatal_trace). Turns the profiler hook on from the start.
#define RING_MAX 65536
static int ring_n = -1;
static __thread struct { uint64_t a, lr, x0; } *ring;
static __thread uint64_t ring_pos;

void fnprof_dump_ring(void) {
    if (!ring || ring_n <= 0) return;
    uint64_t n = ring_pos < (uint64_t)ring_n ? ring_pos : (uint64_t)ring_n;
    fprintf(stderr, "last %llu guest calls on this thread (oldest first):\n", (unsigned long long)n);
    for (uint64_t i = ring_pos - n; i < ring_pos; i++) {
        uint64_t k = i % (uint64_t)ring_n;
        fprintf(stderr, "  f_%llx from %llx x0=%llx\n", (unsigned long long)ring[k].a, (unsigned long long)ring[k].lr,
                (unsigned long long)ring[k].x0);
    }
}

// ---- the call graph (LARECOMP_FNPROF_GRAPH=<file>)
// Every guest function entry as (return address, function), kind 0, and every indirect transfer out of a function as
// (site, target), kind 1 for a blr and 2 for a br (FN_EDGE, emitted by tools/gen.py), each pair once per thread with
// its count, the order it was first seen in (one sequence over all threads) and the followed player's update it was
// first seen in. The window is player updates: RECOMP_FNPROF_UPDATES=<from>:<to> (default 0:-1, to the end), counted
// by games/smm2/trace.c (fnprof_player_update); at <to> the file is written, and with LARECOMP_FNPROF_GRAPH_EXIT=1 the
// program ends. The thread that runs the player updates is marked "main". Per-thread tables, so no locks on the way.
#define GSLOTS (1 << 18)
typedef struct { uint64_t a, b, seq; int64_t upd; uint32_t kind, count; } GEdge;
typedef struct GTable { GEdge *e; int tid; uint64_t n; struct GTable *next; } GTable;
static volatile int g_graph;
static _Atomic uint64_t g_seq;
static volatile int64_t g_upd = -1;
static int g_main_tid = -1;
static __thread GTable *g_mine;
static GTable *g_tables;
static pthread_mutex_t g_tables_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_ntables;

static GTable *graph_table(void) {
    if (g_mine) return g_mine;
    GTable *t = calloc(1, sizeof *t);
    t->e = calloc(GSLOTS, sizeof(GEdge));
    pthread_mutex_lock(&g_tables_mu);
    t->tid = g_ntables++;
    t->next = g_tables;
    g_tables = t;
    pthread_mutex_unlock(&g_tables_mu);
    return g_mine = t;
}

static void graph_add(uint64_t a, uint64_t b, uint32_t kind) {
    GTable *t = graph_table();
    uint64_t h = ((a >> 2) * 0x9E3779B97F4A7C15ull) ^ ((b >> 2) * 0xC2B2AE3D27D4EB4Full) ^ kind;
    for (uint32_t n = 0, i = (uint32_t)(h >> 40) & (GSLOTS - 1); n < GSLOTS; n++, i = (i + 1) & (GSLOTS - 1)) {
        GEdge *e = &t->e[i];
        if (e->a == a && e->b == b && e->kind == kind) { e->count++; return; }
        if (e->a == 0) {
            e->a = a, e->b = b, e->kind = kind, e->count = 1;
            e->seq = atomic_fetch_add_explicit(&g_seq, 1, memory_order_relaxed);
            e->upd = g_upd;
            t->n++;
            return;
        }
    }
}

void fnprof_edge(uint64_t site, uint64_t target, int kind) {
    if (g_graph) graph_add(site, target, (uint32_t)kind);
}

void fnprof_graph_write(void);
static void graph_write(void) { fnprof_graph_write(); }
void fnprof_graph_write(void) {
    static int done;
    const char *out = getenv("LARECOMP_FNPROF_GRAPH");
    if (done || !out) return;
    done = 1;
    g_graph = 0;
    FILE *f = fopen(out, "w");
    if (!f) { LOG("[fnprof] graph: cannot write %s\n", out); return; }
    fprintf(f, "# fnprof graph v1: C tid return_address function count seq update | I tid site target kind count seq update\n");
    fprintf(f, "# main %d last_update %lld\n", g_main_tid, (long long)g_upd);
    uint64_t n = 0;
    for (GTable *t = g_tables; t; t = t->next)
        for (uint32_t i = 0; i < GSLOTS; i++) {
            GEdge *e = &t->e[i];
            if (!e->a) continue;
            if (e->kind == 0)
                fprintf(f, "C %d %llx %llx %u %llu %lld\n", t->tid, (unsigned long long)e->a, (unsigned long long)e->b,
                        e->count, (unsigned long long)e->seq, (long long)e->upd);
            else
                fprintf(f, "I %d %llx %llx %u %u %llu %lld\n", t->tid, (unsigned long long)e->a, (unsigned long long)e->b,
                        e->kind, e->count, (unsigned long long)e->seq, (long long)e->upd);
            n++;
        }
    fclose(f);
    LOG("[fnprof] graph: %llu edges over %d threads, written %s\n", (unsigned long long)n, g_ntables, out);
}

static void graph_exit(void) { graph_write(); }

// games/smm2/trace.c: the followed player's update n is starting
void fnprof_player_update(int64_t n) {
    static int64_t from = -2, to = -1;
    if (from == -2) {
        from = -1;
        if (!getenv("LARECOMP_FNPROF_GRAPH")) return;
        from = 0;
        const char *e = getenv("RECOMP_FNPROF_UPDATES");
        if (e) sscanf(e, "%lld:%lld", (long long *)&from, (long long *)&to);
        atexit(graph_exit);
    }
    if (from < 0) return;
    g_upd = n;
    if (n == from) {
        g_main_tid = graph_table()->tid;
        g_graph = 1;
        g_fnprof = 1;
        LOG("[fnprof] graph on at player update %lld\n", (long long)n);
    }
    if (to >= 0 && n == to) {
        graph_write();
        if (getenv("LARECOMP_FNPROF_GRAPH_EXIT")) { fflush(stderr); _exit(0); }
    }
}

void fnprof_hit(uint64_t a, uint64_t lr, uint64_t x0) {
    if (ring_n) {
        if (ring_n < 0) {
            const char *e = getenv("RECOMP_CALL_RING");
            ring_n = e ? atoi(e) : 0;
            if (ring_n > RING_MAX) ring_n = RING_MAX;
        }
        if (ring_n > 0) {
            if (!ring) ring = calloc((size_t)ring_n, sizeof *ring);
            uint64_t k = ring_pos++ % (uint64_t)ring_n;
            ring[k].a = a, ring[k].lr = lr, ring[k].x0 = x0;
            if (!getenv("LARECOMP_FNPROF")) { if (bt_n) bt_check(a, lr, x0); return; }  // (ring only: no counting)
        }
    }
    if (bt_n) bt_check(a, lr, x0);
    if (g_graph) graph_add(lr, a, 0);
    uint32_t h = (uint32_t)((a >> 2) * 0x9E3779B1u) & (NSLOT - 1);
    for (int n = 0; n < NSLOT; n++, h = (h + 1) & (NSLOT - 1)) {
        uint64_t cur = atomic_load_explicit(&slots[h].addr, memory_order_relaxed);
        if (cur == a || (cur == 0 && atomic_compare_exchange_strong(&slots[h].addr, &cur, a)) || cur == a) {
            atomic_fetch_add_explicit(&slots[h].count, 1, memory_order_relaxed);
            return;
        }
    }
}

static int cmp(const void *x, const void *y) {
    uint64_t a = ((const uint64_t *)x)[1], b = ((const uint64_t *)y)[1];
    return a < b ? 1 : a > b ? -1 : 0;
}

void fnprof_frame(uint64_t frame) {
    static long from = -2, to;
    if (from == -2) {
        from = -1;
        const char *e = getenv("LARECOMP_FNPROF");
        if (e && sscanf(e, "%ld:%ld", &from, &to) != 2) from = -1;
    }
    if (from < 0) return;
    if ((long)frame == from) { g_fnprof = 1; LOG("[fnprof] on at frame %llu\n", (unsigned long long)frame); }
    if ((long)frame != to) return;
    g_fnprof = 0;
    uint64_t (*rows)[2] = malloc(sizeof(uint64_t[2]) * NSLOT);
    int n = 0;
    for (int i = 0; i < NSLOT; i++)
        if (slots[i].addr) { rows[n][0] = slots[i].addr; rows[n][1] = slots[i].count; n++; }
    qsort(rows, n, sizeof rows[0], cmp);
    const char *out = getenv("LARECOMP_FNPROF_OUT") ? getenv("LARECOMP_FNPROF_OUT") : "build/fnprof.txt";  // (parallel runs)
    FILE *f = fopen(out, "w");
    for (int i = 0; f && i < n; i++) fprintf(f, "%llx %llu\n", (unsigned long long)rows[i][0], (unsigned long long)rows[i][1]);
    if (f) fclose(f);
    free(rows);
    LOG("[fnprof] %d functions, written %s (frames %ld-%ld)\n", n, out, from, to);
    from = -1;
}

// LARECOMP_FNPROF=0:<to>: profile from process start (threads the game starts before the first frame)
__attribute__((constructor)) static void fnprof_early(void) {
    const char *e = getenv("LARECOMP_FNPROF");
    if (e && e[0] == '0' && e[1] == ':') g_fnprof = 1;
    if (getenv("RECOMP_CALL_RING")) g_fnprof = 1;
}
