// macOS host layer (runtime/platform.h), also iOS / iPadOS (TARGET_OS_IPHONE: no mach_vm.h, no child processes).
#include <TargetConditionals.h>
#include <crt_externs.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#if !TARGET_OS_IPHONE
#include <mach/mach_vm.h>
#endif
#include <mach/thread_policy.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include "../platform.h"

static mach_timebase_info_data_t tb;

// SIGTERM / SIGINT / SIGHUP: say who sent it (test runs were being stopped from outside), then quit as SDL would.
static void on_term(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    char msg[160];
    int n = snprintf(msg, sizeof msg, "[platform] signal %d from pid %d (uid %d)\n", sig, (int)si->si_pid,
                     (int)si->si_uid);
    if (n > 0) write(2, msg, (size_t)n);
    extern void kernel_dump_threads(void);
    kernel_dump_threads();
    if (sig != SIGUSR1) _exit(0);
}

#if defined(__x86_64__)
#include "x86_cpu.h"
// The recompiled game code is built for x86-64-v3 (Makefile GENARCH: AVX2, FMA, BMI2): say so instead of crashing
// with an illegal instruction on an older CPU.
static void require_cpu(void) {
    if (x86_cpu_has_v3()) return;
    fprintf(stderr, "FATAL: " GAME_TITLE " needs a CPU with AVX2 (Intel Haswell / 2013 or newer, AMD Excavator / Zen or "
                    "newer).\n");
    exit(1);
}
#else
static void require_cpu(void) {}
#endif

void plat_init(void) {
    require_cpu();
#if TARGET_OS_IPHONE
    {   // iOS: no terminal - the log goes to Documents/smm2recomp.log (Files app / Finder)
        const char *home = getenv("HOME");
        char path[1024];
        snprintf(path, sizeof path, "%s/Documents", home ? home : "/tmp");
        mkdir(path, 0755);
        snprintf(path, sizeof path, "%s/Documents/smm2recomp.log", home ? home : "/tmp");
        if (freopen(path, "w", stderr)) { setvbuf(stderr, NULL, _IOLBF, 0); dup2(fileno(stderr), 1); }
    }
#else
    {   // the .app started from Finder (no terminal): the log goes to the data folder's smm2recomp.log, as on Linux
        char exe[1024];
        uint32_t n = sizeof exe;
        const char *home = getenv("HOME"), *env = getenv("LARECOMP_DATA_ROOT");
        if (!isatty(STDERR_FILENO) && _NSGetExecutablePath(exe, &n) == 0 && strstr(exe, ".app/Contents/MacOS/")) {
            char dir[1024], path[1100];
            if (env && *env) snprintf(dir, sizeof dir, "%s", env);
            else snprintf(dir, sizeof dir, "%s/Library/Application Support/" GAME_BUNDLE_ID, home ? home : "/tmp");
            for (char *c = dir + 1; *c; c++)
                if (*c == '/') { *c = 0; mkdir(dir, 0755); *c = '/'; }
            mkdir(dir, 0755);
            snprintf(path, sizeof path, "%s/" GAME_ID "recomp.log", dir);
            bool append = getenv("LASRECOMP_LOG_APPEND") != NULL;  // (a re-exec for the guest window: the same log)
            if (freopen(path, append ? "a" : "w", stderr)) {
                setvbuf(stderr, NULL, _IOLBF, 0);
                dup2(fileno(stderr), 1);
                setenv("LASRECOMP_LOG_APPEND", "1", 1);
            }
        }
    }
#endif
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_term;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);  // (thread dump, keeps running)
}

#if TARGET_OS_IPHONE
// iOS: vm_allocate (64-bit vm_address_t). The guest window is far above iOS's default address space limit: the app
// needs com.apple.developer.kernel.extended-virtual-addressing (tools/make_ios_app.sh).
#define mach_vm_address_t vm_address_t
#define mach_vm_allocate vm_allocate
#endif

bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    // Everything readable/writable: macOS pages are 16K but the guest maps at 4K granularity.
    // Untouched pages cost nothing.
    mach_vm_address_t a = lo;
    kern_return_t r = mach_vm_allocate(mach_task_self(), &a, hi - lo, VM_FLAGS_FIXED);
    if (r != KERN_SUCCESS) fprintf(stderr, "[platform] guest memory %#llx..%#llx not reserved (kern %d)\n", (unsigned long long)lo,
                                   (unsigned long long)hi, r);
    return r == KERN_SUCCESS;
}

#if TARGET_OS_IPHONE
// The guest window couldn't be reserved (runtime/main.c): what the address space looks like, into the log - the
// kernel's limits, what is mapped around the window, and which reservations would work.
void plat_guest_report(uint64_t lo, uint64_t hi) {
    vm_address_t a = lo;
    kern_return_t r = vm_allocate(mach_task_self(), &a, hi - lo, VM_FLAGS_FIXED);
    fprintf(stderr, "[platform] fixed %#llx + %#llx: kern %d\n", (unsigned long long)lo, (unsigned long long)(hi - lo), r);
    if (r == KERN_SUCCESS) { vm_deallocate(mach_task_self(), a, hi - lo); return; }
    task_vm_info_data_t vi;
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &n) == KERN_SUCCESS)
        fprintf(stderr, "[platform] address space %#llx..%#llx, footprint %llu MB, limit %d MB\n",
                (unsigned long long)vi.min_address, (unsigned long long)vi.max_address,
                (unsigned long long)vi.phys_footprint >> 20, vi.limit_bytes_remaining ? (int)((vi.phys_footprint + vi.limit_bytes_remaining) >> 20) : -1);
    vm_address_t at = 0x100000000ull;  // regions from the executable up
    for (int i = 0; i < 400; i++) {
        vm_size_t size = 0;
        natural_t depth = 0;
        struct vm_region_submap_info_64 info;
        mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (vm_region_recurse_64(mach_task_self(), &at, &size, &depth, (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS) break;
        if (size >= (64ull << 20) || (at < hi && at + size > lo))
            fprintf(stderr, "[platform] region %#llx..%#llx (%llu MB) prot %d tag %u\n", (unsigned long long)at,
                    (unsigned long long)(at + size), (unsigned long long)size >> 20, info.protection, info.user_tag);
        at += size;
    }
    static const uint64_t sizes[] = {0x320000000ull, 0x200000000ull, 0x100000000ull, 0x40000000ull};
    for (int i = 0; i < 4; i++) {  // anywhere: where would it go?
        vm_address_t b = 0;
        r = vm_allocate(mach_task_self(), &b, sizes[i], VM_FLAGS_ANYWHERE);
        fprintf(stderr, "[platform] anywhere %#llx: kern %d at %#llx\n", (unsigned long long)sizes[i], r, (unsigned long long)b);
        if (r == KERN_SUCCESS) vm_deallocate(mach_task_self(), b, sizes[i]);
    }
    static const uint64_t bases[] = {0x400000000ull, 0x600000000ull, 0xa00000000ull, 0xc00000000ull, 0x1000000000ull,
                                     0x2000000000ull, 0x7000000000ull};
    for (int i = 0; i < 7; i++) {  // the same window elsewhere
        vm_address_t b = bases[i];
        r = vm_allocate(mach_task_self(), &b, hi - lo, VM_FLAGS_FIXED);
        fprintf(stderr, "[platform] fixed %#llx + %#llx: kern %d\n", (unsigned long long)bases[i], (unsigned long long)(hi - lo), r);
        if (r == KERN_SUCCESS) vm_deallocate(mach_task_self(), b, hi - lo);
    }
}
#endif

// Fresh zero pages over the whole window (the old ones are released at once).
bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    mach_vm_address_t a = lo;
    return mach_vm_allocate(mach_task_self(), &a, hi - lo, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE) == KERN_SUCCESS;
}

void plat_commit(uint64_t addr, uint64_t size) { (void)addr; (void)size; }

int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {  // (no write tracking)
    (void)addr; (void)size; (void)pages; (void)max; (void)page;
    return -1;
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    int p = prot == PLAT_PROT_NONE ? PROT_NONE : prot == PLAT_PROT_READ ? PROT_READ : PROT_READ | PROT_WRITE;
    return mprotect((void *)(uintptr_t)addr, size, p);
}

uint64_t plat_now_ns(void) {
    if (!tb.denom) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;
}

void plat_sleep_until_ns(uint64_t t) {
    if (!tb.denom) mach_timebase_info(&tb);
    mach_wait_until(t * tb.denom / tb.numer);
}

// Real-time (time-constraint) thread: exempt from the timer coalescing macOS applies to
// ordinary threads when the app isn't frontmost, which otherwise stretches 16ms waits to ~100ms.
void plat_thread_realtime(uint64_t period_ns) {
    if (!tb.denom) mach_timebase_info(&tb);
    uint64_t period = period_ns * tb.denom / tb.numer;
    thread_time_constraint_policy_data_t pol = {
        .period = (uint32_t)period,
        .computation = (uint32_t)(period / 20),
        .constraint = (uint32_t)(period / 4),
        .preemptible = 1,
    };
    thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&pol,
                      THREAD_TIME_CONSTRAINT_POLICY_COUNT);
}

bool plat_exe_path(char *buf, size_t size) {
    uint32_t len = (uint32_t)size;
    return _NSGetExecutablePath(buf, &len) == 0;
}

void plat_exec(char *const argv[]) {
    if (argv) { execv(argv[0], argv); return; }
    char path[4096];
    if (plat_exe_path(path, sizeof path)) execv(path, *_NSGetArgv());
}

const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    Dl_info di = {0};
    if (!dladdr((void *)(uintptr_t)pc, &di) || !di.dli_sname) return NULL;
    if (offset) *offset = pc - (uint64_t)(uintptr_t)di.dli_saddr;
    return di.dli_sname;
}

void plat_fatal_notice(void) {}  // (the log / terminal shows it)

void plat_print_backtrace(void) {
    void *bt[64];
    int n = backtrace(bt, 64);
    char **syms = backtrace_symbols(bt, n);
    for (int i = 0; i < n; i++) fprintf(stderr, "  %s\n", syms[i]);
}

static void crash_handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    static volatile int depth;
    if (sig == SIGABRT && depth++ > 0) { signal(SIGABRT, SIG_DFL); abort(); }  // (nested: just die - the report
                                                                               // itself aborting looped, 5.7 GB log)
    uint64_t fa = (uint64_t)si->si_addr;
#if defined(__aarch64__)
    uint64_t pc = uc->uc_mcontext->__ss.__pc, lr = uc->uc_mcontext->__ss.__lr;
#elif defined(__x86_64__)
    uint64_t pc = uc->uc_mcontext->__ss.__rip, lr = 0;  // (x86-64: no link register)
#else
    uint64_t pc = 0, lr = 0;
#endif
    if (rt_fault_watch(fa, pc)) return;
    char what[32];
    snprintf(what, sizeof what, "signal %d", sig);
    rt_fault_report(what, fa, pc, lr);
}

void plat_crash_handler_install(void) {
    stack_t ss = {.ss_sp = malloc(1 << 20), .ss_size = 1 << 20};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {0};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

// ---- child processes
#include <fcntl.h>
#include <ftw.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;

#if TARGET_OS_IPHONE
PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {  // (iOS starts no processes)
    (void)argv, (void)log_path, (void)append_log;
    return 0;
}
void plat_proc_kill(PlatProc p) { (void)p; }
int plat_proc_wait(PlatProc p, bool block) { (void)p, (void)block; return 255; }
#else
PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, log_path ? log_path : "/dev/null",
                                     O_WRONLY | O_CREAT | (append_log ? O_APPEND : O_TRUNC), 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    posix_spawnattr_t at;  // (its own process group: plat_proc_kill ends what it starts, e.g. a compiler's own child)
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    pid_t pid;
    int rc = posix_spawn(&pid, argv[0], &fa, &at, (char *const *)argv, environ);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    return rc ? 0 : (PlatProc)pid;
}

void plat_proc_kill(PlatProc p) {
    kill(-(pid_t)p, SIGKILL);
    waitpid((pid_t)p, NULL, 0);
}

int plat_proc_wait(PlatProc p, bool block) {
    int st = 0;
    pid_t r = waitpid((pid_t)p, &st, block ? 0 : WNOHANG);
    if (r == 0) return -1;
    if (r < 0) return 255;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 255;
}
#endif

static int rm_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw) {
    (void)sb, (void)flag, (void)ftw;
    return remove(path);
}
bool plat_remove_tree(const char *dir) { return nftw(dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS) == 0; }
