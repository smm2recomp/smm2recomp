// Builds the game code library on the player's machine (runtime/gamebuild.h).
#include "gamebuild.h"
#include "game.h"

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "gamelib.h"
#include "paths.h"
#include "platform.h"
#include "rt.h"
#if defined(__x86_64__) || defined(__i386__)
#include "platform/x86_cpu.h"
#endif

// From the Makefile: the flags the game code is compiled and linked with (as `make` builds it in a source checkout)
#ifndef GAMELIB_CFLAGS
#define GAMELIB_CFLAGS "-O1 -fno-strict-aliasing -fvisibility=hidden"
#endif
#ifndef GAMELIB_SHARED
#define GAMELIB_SHARED "-shared"
#endif
#ifndef GAMEBUILD_HLE
#define GAMEBUILD_HLE "runtime/hle/hle_symbols.txt"  // (the game's HLE list in a source checkout: Makefile GAME_HLE)
#endif
#ifndef GAMEBUILD_PATCHES
#define GAMEBUILD_PATCHES "games/patches.py"  // (the game's generator patches in a source checkout: Makefile GAME_PATCHES)
#endif

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

// ------------------------------------------------------------------ child processes
// Every one running, so that quitting mid-build ends them too (atexit) instead of leaving compilers behind.
#define MAX_PROCS 80
static pthread_mutex_t g_plock = PTHREAD_MUTEX_INITIALIZER;
static PlatProc g_procs[MAX_PROCS];
static int g_nprocs;
static bool g_quitting;
static volatile sig_atomic_t g_cancel;  // gamebuild_cancel (a signal handler may set it)

void gamebuild_cancel(void) { g_cancel = 1; }
void gamebuild_cancel_clear(void) { g_cancel = 0; }
bool gamebuild_cancelled(void) { return g_cancel != 0; }

static void end_all(void) {
    pthread_mutex_lock(&g_plock);
    g_quitting = true;
    for (int i = 0; i < g_nprocs; i++) plat_proc_kill(g_procs[i]);
    g_nprocs = 0;
    pthread_mutex_unlock(&g_plock);
}

static PlatProc spawn(char **argv, const char *log) {
    pthread_mutex_lock(&g_plock);
    PlatProc p = g_quitting || g_cancel ? 0 : plat_spawn((const char *const *)argv, log, false);
    if (p && g_nprocs < MAX_PROCS) g_procs[g_nprocs++] = p;
    pthread_mutex_unlock(&g_plock);
    return p;
}

// plat_proc_wait without blocking, then forgotten once it has ended
static int finished(PlatProc p) {
    pthread_mutex_lock(&g_plock);
    int r = plat_proc_wait(p, false);
    if (r >= 0)
        for (int i = 0; i < g_nprocs; i++)
            if (g_procs[i] == p) { g_procs[i] = g_procs[--g_nprocs]; break; }
    pthread_mutex_unlock(&g_plock);
    return r;
}

static void stop(PlatProc p) {  // ends one that is still running, then forgotten
    pthread_mutex_lock(&g_plock);
    bool ours = false;
    for (int i = 0; i < g_nprocs; i++)
        if (g_procs[i] == p) { g_procs[i] = g_procs[--g_nprocs]; ours = true; break; }
    pthread_mutex_unlock(&g_plock);
    if (ours) plat_proc_kill(p);
}

static void sleep_ms(int ms) { plat_sleep_until_ns(plat_now_ns() + (uint64_t)ms * 1000000ull); }

static int wait_for(PlatProc p) {  // (-1: cancelled, the child ended)
    int r;
    while ((r = finished(p)) < 0) {
        if (g_cancel) { stop(p); return -1; }
        sleep_ms(20);
    }
    return r;
}

int gamebuild_exec(const char *const *argv, const char *log) {
    static bool registered;
    if (!registered) registered = true, atexit(end_all);
    PlatProc p = spawn((char **)argv, log);
    return p ? wait_for(p) : -1;
}

// ------------------------------------------------------------------ argument lists
typedef struct { char **v; int n, cap; } Args;
static void add(Args *a, const char *s) {
    if (a->n + 2 > a->cap) {
        a->cap = a->cap ? a->cap * 2 : 32;
        a->v = (char **)realloc(a->v, sizeof *a->v * a->cap);
    }
    a->v[a->n++] = strdup(s);
    a->v[a->n] = NULL;
}
static void add_split(Args *a, const char *flags) {  // (space-separated, no quoting)
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", flags);
    for (char *t = strtok(buf, " "); t; t = strtok(NULL, " ")) add(a, t);
}
static void args_free(Args *a) {
    for (int i = 0; i < a->n; i++) free(a->v[i]);
    free(a->v);
    memset(a, 0, sizeof *a);
}

// ------------------------------------------------------------------ the tools
typedef struct {
    char gen[1100], gen_script[1100];  // generator: lasgen, or (source checkout) the venv's python + tools/gen.py
    char cc[1100];                     // compiler: zig (as `zig cc`), or LARECOMP_CC / clang
    bool zig;
    char inc[1100], entry[1100], hle[1100];  // headers the game code includes, tools/gamelib_entry.c, HLE names
    char patches[1100];                       // the game's per-instruction patches for main ("" = none)
#ifdef _WIN32
    char implib[1100];  // the .exe's import library (the runtime functions the game code calls)
    char zig_rt[1100];  // the release's zig: its runtime built ahead of time (zig/rt, tools/zig_slim.sh), "" = none
#endif
} Tools;

static bool file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static void env_put(const char *k, const char *v) {
#ifdef _WIN32
    _putenv_s(k, v ? v : "");
#else
    if (v) setenv(k, v, 1);
    else unsetenv(k);
#endif
}

bool gamebuild_python(char *out, size_t n) {
#ifdef _WIN32
    const char *bundled = "python" SEP "python.exe";
#else
    const char *bundled = "python" SEP "bin" SEP "python3";
#endif
    // the package's own Python (tools/fetch_python.sh), else a venv: tools/venv next to the program or in the source
    if (!rt_resource(bundled, NULL, out, n) && !rt_resource("tools" SEP PLAT_VENV_PYTHON, "tools/" PLAT_VENV_PYTHON, out, n))
        return false;
    env_put("PYTHONHOME", NULL);  // (the player's own Python settings don't apply to ours)
    env_put("PYTHONPATH", NULL);
    env_put("PYTHONNOUSERSITE", "1");
    env_put("PYTHONDONTWRITEBYTECODE", "1");  // (nothing written next to the scripts: the .app stays as signed)
    env_put("PYTHONUTF8", "1");
    env_put("PYTHONUNBUFFERED", "1");
    return true;
}

static bool find_tools(Tools *t, bool need_cc) {
    memset(t, 0, sizeof *t);
    const char *e = getenv("LARECOMP_GEN");
    if (e && *e) snprintf(t->gen, sizeof t->gen, "%s", e);
    else if (!rt_resource("lasgen" SEP "lasgen" PLAT_EXE_SUFFIX, NULL, t->gen, sizeof t->gen) &&
             !(gamebuild_python(t->gen, sizeof t->gen) &&
               rt_resource("tools" SEP "gen.py", "tools/gen.py", t->gen_script, sizeof t->gen_script)))
        // (packages: python/ and tools/ next to the program, or tools/venv made by the player - recomp/README.md)
        return false;
    e = getenv("LARECOMP_CC");
    if (e && *e) snprintf(t->cc, sizeof t->cc, "%s", e);
    else if (rt_resource("zig" SEP "zig" PLAT_EXE_SUFFIX, NULL, t->cc, sizeof t->cc)) t->zig = true;
#ifndef _WIN32
    else if (rt_dev_mode() && file_exists("/usr/bin/clang")) snprintf(t->cc, sizeof t->cc, "/usr/bin/clang");
#endif
    else if (need_cc) return false;
    if (!rt_resource("gamelib-src" SEP "cpu.h", "runtime/cpu.h", t->inc, sizeof t->inc) ||
        !rt_resource("gamelib-src" SEP "gamelib_entry.c", "tools/gamelib_entry.c", t->entry, sizeof t->entry) ||
        !rt_resource("gamelib-src" SEP "hle_symbols.txt", GAMEBUILD_HLE, t->hle, sizeof t->hle))
        return false;
    if (!rt_resource("gamelib-src" SEP "patches.py", GAMEBUILD_PATCHES, t->patches, sizeof t->patches)) t->patches[0] = 0;
    char *s = strrchr(t->inc, '/'), *b = strrchr(t->inc, '\\');
    if (b > s) s = b;
    if (s) *s = 0;
#ifdef _WIN32
    if (!rt_resource(GAME_EXE ".dll.a", "build/" GAME_EXE ".dll.a", t->implib, sizeof t->implib)) return false;
    t->zig_rt[0] = 0;
    if (t->zig && rt_resource("zig" SEP "rt" SEP "link.rsp", NULL, t->zig_rt, sizeof t->zig_rt)) {
        char *s = strrchr(t->zig_rt, '/'), *b = strrchr(t->zig_rt, '\\');
        if (b > s) s = b;
        if (s) *s = 0;
        else t->zig_rt[0] = 0;
    }
#endif
    return true;
}

bool gamebuild_available(void) {
    Tools t;
    return find_tools(&t, true);
}

static void compiler(Args *a, const Tools *t) {
    add(a, t->cc);
    if (t->zig) add(a, "cc");
}

// ------------------------------------------------------------------ generator progress (its log)
typedef struct {
    const char *name;
    char log[1100], out[1100];
    PlatProc p;
    int exit;                // -1 while running
    int files;               // chunk files written so far (complete)
    unsigned long long ci, n;  // functions written / all
} Gen;

static void read_gen_log(Gen *g) {
    FILE *f = fopen(g->log, "rb");
    if (!f) return;
    static char buf[1 << 16];
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long from = size > (long)sizeof buf - 1 ? size - (long)sizeof buf + 1 : 0;
    fseek(f, from, SEEK_SET);
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    const char *last = NULL;
    for (const char *p = strstr(buf, ": progress "); p; p = strstr(p + 1, ": progress ")) last = p;
    unsigned long long ci, all;
    int files;
    if (last && sscanf(last, ": progress %llu/%llu files %d", &ci, &all, &files) == 3) {
        g->ci = ci, g->n = all;
        if (files > g->files) g->files = files;
    }
}

static void log_tail(const char *path, char *out, size_t n) {  // the last line of a log (an error message)
    out[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char buf[4096];
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, size > (long)sizeof buf - 1 ? size - (long)sizeof buf + 1 : 0, SEEK_SET);
    size_t k = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[k] = 0;
    while (k && (buf[k - 1] == '\n' || buf[k - 1] == '\r')) buf[--k] = 0;
    char *nl = strrchr(buf, '\n');
    snprintf(out, n, "%s", nl ? nl + 1 : buf);
}

// ------------------------------------------------------------------ compiling
typedef struct {
    char src[1100], obj[1100], log[1100];
    unsigned long long bytes;
} Unit;

typedef struct {
    Unit *u;
    int n, cap;
} Units;

static void add_unit(Units *us, const char *src, const char *work, const char *stem) {
    if (us->n == us->cap) {
        us->cap = us->cap ? us->cap * 2 : 64;
        us->u = (Unit *)realloc(us->u, sizeof *us->u * us->cap);
    }
    Unit *u = &us->u[us->n++];
    snprintf(u->src, sizeof u->src, "%s", src);
    snprintf(u->obj, sizeof u->obj, "%s" SEP "%s.o", work, stem);
    snprintf(u->log, sizeof u->log, "%s" SEP "%s.log", work, stem);
    struct stat st;
    u->bytes = stat(src, &st) == 0 ? (unsigned long long)st.st_size : 0;
}

// The flags the game code was built with (GAMELIB_CFLAGS, part of its id). zig cc reads -march=<cpu> with its own CPU
// names (x86_64_v3) and takes "-<feature>" off the end, so x86-64-v3 would be CPU "x86": the same CPU as -mcpu there.
//
// x86-64 dispatch (method 1): GAMELIB_CFLAGS carries the v2 baseline (Makefile GENARCH), and the build upgrades to
// v3 where this CPU has AVX2/FMA/BMI2, or downgrades an explicit v3 back to v2 where it doesn't. Modern machines lose
// nothing; pre-Haswell machines (back to Nehalem, x86-64-v2) still run: without hardware FMA the lifted code's
// __builtin_elementwise_fma lowers to correctly-rounded fmaf/fma libcalls (slower), with it to vfmadd.
// LARECOMP_GENARCH=<flag> forces an arch flag verbatim (e.g. -march=x86-64-v2), for testing.
static bool gamebuild_cpu_has_v3(void) {
#if defined(__x86_64__)
    return x86_cpu_has_v3();
#else
    return false;
#endif
}

static void add_arch(Args *a, const Tools *t, bool want_v3) {
    if (t->zig) add(a, want_v3 ? "-mcpu=x86_64_v3" : "-mcpu=x86_64_v2");
    else add(a, want_v3 ? "-march=x86-64-v3" : "-march=x86-64-v2");
}

static void add_cflags(Args *a, const Tools *t) {
    bool has_v3 = gamebuild_cpu_has_v3();
    const char *forced = getenv("LARECOMP_GENARCH");
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", GAMELIB_CFLAGS);
    for (char *f = strtok(buf, " "); f; f = strtok(NULL, " ")) {
        bool is_arch = (!strncmp(f, "-march=", 7) || !strncmp(f, "-mcpu=", 6)) &&
                       (strstr(f, "x86-64-v") || strstr(f, "x86_64_v"));
        if (is_arch) {
            if (forced && *forced) {
                if (t->zig && !strncmp(forced, "-march=", 7)) {
                    char cpu[128];
                    snprintf(cpu, sizeof cpu, "-mcpu=%s", forced + 7);
                    for (char *c = cpu + 6; *c; c++) if (*c == '-') *c = '_';
                    add(a, cpu);
                } else {
                    add(a, forced);
                }
            } else {
                add_arch(a, t, has_v3);
            }
            continue;
        }
        if (t->zig && !strncmp(f, "-march=", 7)) {
            char cpu[128];
            snprintf(cpu, sizeof cpu, "-mcpu=%s", f + 7);
            for (char *c = cpu + 6; *c; c++) if (*c == '-') *c = '_';
            add(a, cpu);
        } else {
            add(a, f);
        }
    }
}

// cc / cflags (NULL-terminated): another compiler and its flags (the iPhone build: runtime/iosbuild.c), else ours
static PlatProc start_compile(const Tools *t, const Unit *u, const char *const *cc, const char *const *cflags) {
    Args a = {0};
    if (cc) {
        for (; *cc; cc++) add(&a, *cc);
        for (; cflags && *cflags; cflags++) add(&a, *cflags);
    } else {
        compiler(&a, t);
        add_cflags(&a, t);
        char inc[1200];
        snprintf(inc, sizeof inc, "-I%s", t->inc);
        add(&a, inc);
        add(&a, "-DGAMELIB_ID=\"" GAMELIB_ID "\"");
    }
    add(&a, "-c");
    add(&a, u->src);
    add(&a, "-o");
    add(&a, u->obj);
    PlatProc p = spawn(a.v, u->log);
    args_free(&a);
    return p;
}

// ------------------------------------------------------------------ the build
bool gamebuild_compile(const char *game_dir, const char *work, const char *const *cc, const char *const *cflags,
                       const GameBuildOpts *o, char ***objs, char *why, size_t why_size) {
    uint64_t t0 = plat_now_ns();
    static bool registered;
    if (!registered) registered = true, atexit(end_all);
    *objs = NULL;
    Tools t;
    if (!find_tools(&t, cc == NULL)) {
        snprintf(why, why_size, "the game code builder is missing from this copy of " GAME_TITLE);
        return false;
    }
    // 1. the generator, both modules at once; their finished files are compiled while it goes on
    Gen gens[2] = {{.name = "main"}, {.name = "sdk"}};
    static const char *const base[2] = {"0x7000000000", "0x7004000000"};
    bool ok = true;
    for (int i = 0; i < 2 && ok; i++) {
        Gen *g = &gens[i];
        g->exit = -1;
        snprintf(g->log, sizeof g->log, "%s" SEP "gen_%s.log", work, g->name);
        snprintf(g->out, sizeof g->out, "%s" SEP "%s", work, g->name);
        char nso[1100];
        snprintf(nso, sizeof nso, "%s" SEP "exefs" SEP "%s", game_dir, g->name);
        Args a = {0};
        add(&a, t.gen);
        if (t.gen_script[0]) add(&a, t.gen_script);
        add(&a, g->name);
        add(&a, nso);
        add(&a, base[i]);
        add(&a, g->out);
        if (i == 1) { add(&a, "--hle"); add(&a, t.hle); }
        if (i == 0 && t.patches[0]) { add(&a, "--patches"); add(&a, t.patches); }  // (as the Makefile's gen)
        g->p = spawn(a.v, g->log);
        args_free(&a);
        if (!g->p) { snprintf(why, why_size, "the game code builder didn't start"); ok = false; }
    }

    Units units = {0};
    int queued[2] = {0, 0};  // chunk files of each module put in units so far
    bool tables[2] = {false, false};
    int next = 0, jobs = o->jobs < 1 ? 1 : o->jobs > 64 ? 64 : o->jobs;
    struct { PlatProc p; int unit; } run[64];
    int nrun = 0;
    unsigned long long done_bytes = 0;
    double shown = 0;
    {
        add_unit(&units, t.entry, work, "gamelib_entry");
    }
    while (ok) {
        // generator progress, newly finished files
        bool gens_done = true;
        double gen_frac = 0;
        unsigned long long written = 0;
        for (int i = 0; i < 2; i++) {
            Gen *g = &gens[i];
            if (g->exit < 0) {
                read_gen_log(g);
                int r = finished(g->p);
                if (r >= 0) {
                    g->exit = r;
                    read_gen_log(g);
                    if (r != 0) {
                        char last[300];
                        log_tail(g->log, last, sizeof last);
                        LOG("[gamebuild] generator (%s) failed: %s\n", g->name, last);
                        if (strstr(last, "MemoryError"))
                            snprintf(why, why_size, "the computer ran out of memory preparing the game code (it needs "
                                                    "about 4 GB of free memory: close other programs and try again)");
                        else  // (the generator's own last line: what a report needs, issue #12)
                            snprintf(why, why_size, "the game code couldn't be prepared (%s: %s)", g->name,
                                     last[0] ? last : "no output");
                        ok = false;
                    }
                }
            }
            if (g->exit != 0) gens_done = false;
            for (; queued[i] < g->files; queued[i]++) {
                char src[1100], stem[64];
                snprintf(stem, sizeof stem, "%s_%03d", g->name, queued[i]);
                snprintf(src, sizeof src, "%s" SEP "%s.c", g->out, stem);
                add_unit(&units, src, work, stem);
            }
            if (g->exit == 0 && !tables[i]) {
                char src[1100], stem[64];
                snprintf(stem, sizeof stem, "%s_table", g->name);
                snprintf(src, sizeof src, "%s" SEP "%s.c", g->out, stem);
                add_unit(&units, src, work, stem);
                tables[i] = true;
            }
            gen_frac += g->exit == 0 ? 0.5 : g->n ? 0.5 * (double)g->ci / (double)g->n : 0;
        }
        if (!ok) break;
        if (g_cancel) { snprintf(why, why_size, "cancelled"); ok = false; break; }
        for (int k = 0; k < units.n; k++) written += units.u[k].bytes;

        // compiles
        while (nrun < jobs && next < units.n) {
            PlatProc p = start_compile(&t, &units.u[next], cc, cflags);
            if (!p) { snprintf(why, why_size, "the compiler didn't start"); ok = false; break; }
            run[nrun].p = p, run[nrun].unit = next++;
            nrun++;
        }
        for (int k = 0; k < nrun; k++) {
            int r = finished(run[k].p);
            if (r < 0) continue;
            Unit *u = &units.u[run[k].unit];
            if (r != 0) {
                char last[300];
                log_tail(u->log, last, sizeof last);
                LOG("[gamebuild] compiling %s failed: %s\n", u->src, last);
                snprintf(why, why_size, "the game code didn't compile");
                ok = false;
            }
            done_bytes += u->bytes;
            if (run[k].unit > 0) remove(u->src);  // (generated C, done with: not kept. Unit 0 is the shipped entry file)
            run[k--] = run[--nrun];
        }
        if (!ok) break;
        if (gens_done && next == units.n && nrun == 0) break;

        // progress: the generator's share (its functions written), the compiler's (bytes of C compiled, of an
        // estimate of all of it while the generator still writes)
        double est_total = gens_done ? (double)written : gen_frac > 0.02 ? (double)written / gen_frac : 0;
        double comp_frac = est_total > 0 ? (double)done_bytes / est_total : 0;
        if (comp_frac > 1) comp_frac = 1;
        double f = 0.2 * gen_frac + 0.75 * comp_frac;
        double secs = (double)(plat_now_ns() - t0) / 1e9;
        if (gen_frac == 0) f = 0.05 * (secs / (secs + 20));  // (the generator's first pass: no count yet)
        if (f > shown) shown = f;
        if (o->progress) o->progress(shown, gen_frac < 1 && !gens_done ? "Reading the game code..." : "Recompiling...");
        sleep_ms(50);
    }
    for (int k = 0; k < nrun; k++) stop(run[k].p);  // (a failure: the rest is of no use - the failure shows at once)
    for (int i = 0; i < 2; i++)
        if (gens[i].p && gens[i].exit < 0) stop(gens[i].p);

    if (ok) {
        *objs = (char **)calloc((size_t)units.n + 1, sizeof **objs);
        for (int k = 0; k < units.n; k++) (*objs)[k] = strdup(units.u[k].obj);
    }
    free(units.u);
    return ok;
}

void gamebuild_free_objs(char **objs) {
    for (char **p = objs; p && *p; p++) free(*p);
    free(objs);
}

bool gamebuild_run(const char *game_dir, const GameBuildOpts *o, char *why, size_t why_size) {
    uint64_t t0 = plat_now_ns();
    static bool registered;
    if (!registered) registered = true, atexit(end_all);
    gamebuild_cancel_clear();
    Tools t;
    if (!find_tools(&t, true)) {
        snprintf(why, why_size, "the game code builder is missing from this copy of " GAME_TITLE);
        return false;
    }
    char work[1100], lib[1100], tmp[1100];
    snprintf(work, sizeof work, "%s" SEP ".gamebuild", game_dir);
    snprintf(lib, sizeof lib, "%s" SEP "%s", game_dir, GAMELIB_NAME);
    snprintf(tmp, sizeof tmp, "%s" SEP "%s.tmp", work, GAMELIB_NAME);
    rt_remove_game_file(game_dir, ".gamebuild");
    rt_remove_game_file(game_dir, GAMELIB_NAME);
    rt_remove_game_file(game_dir, GAMELIB_NAME ".id");
    mkdir(work, 0755);
    if (t.zig) {  // its caches in the work folder too (deleted below), not ~/.cache: hundreds of MB, used once
        char zc[1200];
        snprintf(zc, sizeof zc, "%s" SEP "zig-cache", work);
#ifdef _WIN32
        _putenv_s("ZIG_GLOBAL_CACHE_DIR", zc);
        _putenv_s("ZIG_LOCAL_CACHE_DIR", zc);
#else
        setenv("ZIG_GLOBAL_CACHE_DIR", zc, 1);
        setenv("ZIG_LOCAL_CACHE_DIR", zc, 1);
#endif
    }
    LOG("[gamebuild] generator %s%s%s, compiler %s%s, %d jobs\n", t.gen, t.gen_script[0] ? " " : "", t.gen_script,
        t.cc, t.zig ? " cc" : "", o->jobs);
#if defined(__x86_64__)
    { const char *forced_arch = getenv("LARECOMP_GENARCH");
      LOG("[gamebuild] x86-64 game code: %s (%s)\n", gamebuild_cpu_has_v3() ? "x86-64-v3 (AVX2/FMA/BMI2)" : "x86-64-v2 baseline",
          forced_arch && *forced_arch ? "LARECOMP_GENARCH override" : "detected"); }
#endif

    char **objs = NULL;
    bool ok = gamebuild_compile(game_dir, work, NULL, NULL, o, &objs, why, why_size);

    // 2. link
    if (ok) {
        if (o->progress) o->progress(0.95, "Finishing...");
        char rsp[1100], log[1100];
        snprintf(rsp, sizeof rsp, "%s" SEP "link.rsp", work);
        snprintf(log, sizeof log, "%s" SEP "link.log", work);
        FILE *f = fopen(rsp, "wb");
        if (f) {
            for (char **ob = objs; *ob; ob++) {  // (forward slashes: the response file's backslashes are escapes)
                fputc('"', f);
                for (const char *c = *ob; *c; c++) fputc(*c == '\\' ? '/' : *c, f);
                fputs("\"\n", f);
            }
            fclose(f);
        }
        Args a = {0};
        char at[1200];
        snprintf(at, sizeof at, "@%s", rsp);
#ifdef _WIN32
        if (t.zig_rt[0]) {  // the linker itself, with the runtime built ahead of time (its flags and libraries: link.rsp)
            char opt[1200];
            add(&a, t.cc);
            add(&a, "lld-link");
            snprintf(opt, sizeof opt, "@%s" SEP "link.rsp", t.zig_rt);
            add(&a, opt);
            snprintf(opt, sizeof opt, "-LIBPATH:%s", t.zig_rt);
            add(&a, opt);
            snprintf(opt, sizeof opt, "-OUT:%s", tmp);
            add(&a, opt);
            add(&a, at);
            add(&a, t.implib);
        } else
#endif
        {
            compiler(&a, &t);
            add_split(&a, GAMELIB_SHARED);
            add(&a, "-o");
            add(&a, tmp);
            add(&a, at);
#ifdef _WIN32
            add(&a, t.implib);
#endif
        }
        PlatProc p = f ? spawn(a.v, log) : 0;
        args_free(&a);
        int r = p ? wait_for(p) : 255;
        if (r != 0) {
            char last[300];
            log_tail(log, last, sizeof last);
            LOG("[gamebuild] linking failed: %s\n", last);
            snprintf(why, why_size, "the game code didn't link");
            ok = false;
        }
    }
    if (ok) {
        remove(lib);
        char idp[1100];
        snprintf(idp, sizeof idp, "%s.id", lib);
        FILE *f = NULL;
        // (x86-64: the level it was built for follows the id, gamelib.c id_matches: a v3 build moved to a CPU without
        // AVX2 is built again instead of failing on its first AVX2 instruction)
        const char *level = "";
#if defined(__x86_64__)
        const char *forced = getenv("LARECOMP_GENARCH");
        level = forced && *forced ? " forced" : gamebuild_cpu_has_v3() ? " x86-64-v3" : " x86-64-v2";
#endif
        if (rename(tmp, lib) != 0 || !(f = fopen(idp, "wb")) || fprintf(f, "%s%s\n", GAMELIB_ID, level) < 0) {
            snprintf(why, why_size, "the game code couldn't be saved");
            ok = false;
        }
        if (f) fclose(f);
    }
    gamebuild_free_objs(objs);
    rt_remove_game_file(game_dir, ".gamebuild");  // the C, the objects, the logs
    LOG("[gamebuild] %s in %.0fs\n", ok ? "done" : "failed", (double)(plat_now_ns() - t0) / 1e9);
    if (ok && o->progress) o->progress(1, "Finishing...");
    return ok;
}
