// LAS Recomp entry: load modules, set up the kernel, run nn::init::Start on the main thread.
#include <SDL3/SDL.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#if (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE) || defined(__ANDROID__)
#include <SDL3/SDL_main.h>  // (iOS, Android: SDL starts the app and calls main() inside it - SDL_main in libmain.so)
#endif
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "gamebuild.h"
#include "gamelib.h"
#include "paths.h"
#include "settings.h"
#include "hle.h"
#include "kernel.h"
#include "mods.h"

const char *g_romfs_dir;

// rtld normally passes these to nn::init::Start: run every module's init/fini arrays.
static void run_array(Cpu *c, uint64_t arr, uint64_t size) {
    for (uint64_t i = 0; i < size / 8; i++) {
        uint64_t fn = ((uint64_t *)arr)[i];
        if (fn) guest_call(c, fn);
    }
}

// nn::init::Start calls x2 right after nnosInitialize and x3 after nninitStartup, just before
// nnMain: SDK constructors run early, the game's run once nninitStartup has set up its heap.
// rtld runs a module's DT_INIT, which registers it (and its TLS template) with musl via
// __nnmusl_init_dso and then runs its init_array; fall back to the array if there is no DT_INIT.
static void module_init(Cpu *c, Module *m) {
    LOG("init %s (DT_INIT %llx, %llu init_array entries)\n", m->name, m->init, m->init_array_sz / 8);
    if (m->init) guest_call(c, m->init);
    else run_array(c, m->init_array, m->init_array_sz);
}

static void rtld_init_sdk(Cpu *c) { module_init(c, &g_modules[1]); }
static void rtld_init_main(Cpu *c) { module_init(c, &g_modules[0]); }

// Claim the guest address range before anything else can map there. The host allocator places
// regions at random addresses in the same window; on a collision, re-exec for a fresh layout.
// (Android: when libmain.so is loaded, before SDL_main)
static bool g_guest_unreserved;  // (iOS, Android: no re-exec - main() says why and stops)
__attribute__((constructor(101))) static void reserve_guest_space(void) {
    if (plat_reserve_guest(GUEST_LO, GUEST_HI)) return;
#if (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE) || defined(__ANDROID__)
    g_guest_unreserved = true;
    return;
#endif
    const char *tries = getenv("LARECOMP_REEXEC");
    int n = tries ? atoi(tries) : 0;
    if (n >= 20) {
        fprintf(stderr, "FATAL: guest address range %llx-%llx is not free\n", GUEST_LO, GUEST_HI);
        exit(1);
    }
    char buf[16];
    snprintf(buf, sizeof buf, "%d", n + 1);
    setenv("LARECOMP_REEXEC", buf, 1);
    plat_exec(NULL);
    perror("re-exec");
    exit(1);
}

// ------------------------------------------------------------------ boot / in-place reboot
// Switching seeds (or vanilla) and going back to the start screen reboot the game inside this process: the window,
// GPU device, settings and shader caches stay. rt_request_reboot (menu, any thread) makes ui_run return on the
// main thread, which then: stops every guest thread (kernel_stop_threads: each unwinds at its next svc / HLE call /
// wait), resets the renderer (textures, address-keyed caches) and all per-boot host state (RT_ON_RESET hooks:
// services, HLE features, seed data, audio), empties the kernel, wipes guest memory, and boots again.
static _Atomic int g_reboot;
void rt_request_reboot(int kind) {
    LOG("[reboot] requested (%s)\n", kind == RT_REBOOT_TITLE ? "start screen" : "boot");
    g_reboot = kind;
}
int rt_reboot_requested(void) { return g_reboot; }

// LARECOMP_REBOOT_AT=<frame>:<seed | vanilla | title>[,...] (test): at that presented frame, switch to the seed /
// vanilla in place, or go back to the start screen (with LARECOMP_LAUNCHER_AUTOPLAY it plays the saved choice).
// Frames count on across reboots. Called by the presenter every frame.
void reboot_test_frame(uint64_t frame) {
    static const char *next;
    static int init;
    if (!init) { init = 1; next = getenv("LARECOMP_REBOOT_AT"); }
    long at;
    char what[128];
    if (!next || !*next || sscanf(next, "%ld:%127[^,]", &at, what) != 2 || (long)frame < at) return;
    next = strchr(next, ',');
    if (next) next++;
    if (!strcmp(what, "title")) { rt_request_reboot(RT_REBOOT_TITLE); return; }
    rt_request_reboot(RT_REBOOT_GAME);
}

#ifdef __APPLE__
extern const ResetFn __start_resets[] __asm__("section$start$__DATA$__rt_reset");
extern const ResetFn __stop_resets[] __asm__("section$end$__DATA$__rt_reset");
#elif defined(__linux__)
extern const ResetFn __start_resets[], __stop_resets[];  // (ELF: defined by the linker)
#else
// PE/COFF: .rst$a < .rst$m (entries, hle.h) < .rst$z (the linker sorts by the text after '$')
__attribute__((section(".rst$a"), used)) static const ResetFn __start_resets[1];
__attribute__((section(".rst$z"), used)) static const ResetFn __stop_resets[1];
#endif

void backend_reset(void);             // renderer: GPU idle, game textures and address-keyed caches dropped
void backend_guest_memory_reset(void);  // renderer: guest memory was replaced (re-wrap the heap)
void backend_clear_window(void);      // a black frame while the game is down

static void reboot_idle(void) {  // (main thread, while guest threads stop) keep the window alive
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_EVENT_QUIT) { LOG("window closed\n"); exit(0); }
}

static void teardown(void) {
    uint64_t t0 = plat_now_ns();
    kernel_stop_threads(reboot_idle);
    backend_clear_window();
    backend_reset();
    const ResetFn *start = __start_resets, *stop = __stop_resets;
    __asm__("" : "+r"(start), "+r"(stop));  // separate objects: keep the compiler from reasoning about them
    int n = 0;
    for (const ResetFn *p = start; p < stop; p++)
        if (*p) { (*p)(); n++; }
    kernel_reset();
    if (!plat_reset_guest(GUEST_LO, GUEST_HI)) FATAL("cannot reset guest memory\n");
    backend_guest_memory_reset();
    g_nmodules = 0;
    g_reboot = RT_REBOOT_NONE;
    LOG("[reboot] game torn down in %llums (%d reset hooks)\n", (unsigned long long)((plat_now_ns() - t0) / 1000000), n);
}

// (iOS and Android: the game code comes with the app, built on the Mac; nothing to build on the device)
#if !defined(GAMELIB_STATIC) && !defined(__ANDROID__)
#define RT_FIRST_LAUNCH_BUILD 1
static void build_progress(double fraction, const char *step) {
    static int last = -100;
    int pct = (int)(fraction * 100);
    if (pct >= last + 5) { last = pct; LOG("[gamebuild] %d%% %s\n", pct, step); }
}

// First launch of a packaged build (no start screen): the game code library isn't there yet, the player's game files
// are. It is built from them once (runtime/gamebuild.c: the generator, then the bundled compiler), into <root>.
static void first_launch_build(const char *root) {
    char main_nso[1100];
    snprintf(main_nso, sizeof main_nso, "%s/exefs/main", root);
    FILE *f = fopen(main_nso, "rb");
    if (!f) return;  // (no game files: gamelib_load says so)
    fclose(f);
    if (!gamebuild_available()) {
        LOG("[gamebuild] the game code isn't built and the builder (tools/venv + tools/gen.py, zig/) isn't here\n");
        return;
    }
    LOG("[gamebuild] first launch: building the game code from %s (a few minutes to half an hour)\n", root);
#ifdef _WIN32
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, GAME_TITLE,
                             "First launch: " GAME_TITLE " now recompiles the game code from your own game files. This "
                             "takes from a few minutes to half an hour; the game starts by itself when it's done (progress in the "
                             "console window).", NULL);
#endif
    int jobs = SDL_GetNumLogicalCPUCores();
    GameBuildOpts o = {build_progress, jobs > 1 ? jobs : 1};
    char why[512];
    if (!gamebuild_run(root, &o, why, sizeof why)) FATAL("building the game code failed: %s (details above)\n", why);
    LOG("[gamebuild] done\n");
}
#endif

static void boot(const char *root) {
    char exefs[1024], romfs[1024], npdm_path[1024];
    snprintf(exefs, sizeof exefs, "%s/exefs", root);
    snprintf(romfs, sizeof romfs, "%s/romfs", root);
    snprintf(npdm_path, sizeof npdm_path, "%s/main.npdm", exefs);
    if (!g_romfs_dir) g_romfs_dir = strdup(romfs);

    char why[512];
#ifdef RT_FIRST_LAUNCH_BUILD
    if (!gamelib_present(root)) first_launch_build(root);
#endif
    if (!gamelib_load(root, why, sizeof why)) FATAL("%s\n", why);
    loader_load(exefs);
    { extern void embed_server_boot(void); embed_server_boot(); }  // (runtime/net/embed_server.h: before the mods' settings)
    { extern void net_config_reload(void); net_config_reload(); }  // (a backend chosen since the last boot: before
                                                                    // the mods, which look at it - ocw.c)
    mods_boot();
    { extern void settings_load(void); settings_load(); }
    { extern void online_sign_in_at_start(void); online_sign_in_at_start(); }  // (runtime/ui/online_rows.mm)
    kernel_init();
    dispatch_init();

    uint32_t stack_size = 0x100000;
    int prio = 44;
    FILE *f = fopen(npdm_path, "rb");
    if (f) {
        uint8_t meta[0x80];
        if (fread(meta, 1, sizeof meta, f) == sizeof meta && !memcmp(meta, "META", 4)) {
            prio = meta[0xE];
            stack_size = *(uint32_t *)(meta + 0x1C);
        }
        fclose(f);
    }
    LOG("main thread: prio %d, stack 0x%x\n", prio, stack_size);

    uint64_t start = loader_find_symbol("_ZN2nn4init5StartEmmPFvvES2_");
    if (!start) FATAL("nn::init::Start not found\n");
    uint64_t init_fn = hle_register("<rtld_init_sdk>", rtld_init_sdk);
    uint64_t fini_fn = hle_register("<rtld_init_main>", rtld_init_main);
    KThread *t = kernel_create_main_thread(start, stack_size, prio);
    kernel_run_main(t, t->handle, 0, init_fn, fini_fn);  // nn::init::Start(thread_handle, args_region, init, fini)
}

#if defined(__APPLE__) && !(defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
#include <signal.h>
#include <unistd.h>
#include "iosbuild.h"
// smm2recomp --build-ios [--no-game-files] [<out.ipa>]: the setup screen's "Build for iPhone / iPad" without it
// (runtime/iosbuild.h). Ctrl-C / SIGTERM cancel it (its compilers end, its work folder is removed).
static void ios_cli_progress(const char *step, float frac) {
    static char last[64];
    static int last_pct = -100;
    int pct = (int)(frac * 100);
    if (strcmp(step, last) != 0 || pct >= last_pct + 5) {
        snprintf(last, sizeof last, "%s", step);
        last_pct = pct;
        printf("%3d%% %s\n", pct, step);
        fflush(stdout);
    }
}
static void ios_cli_cancel(int sig) { (void)sig; gamebuild_cancel(); }
static int build_ios_cli(int argc, char **argv) {
    bool files = true;
    char out[1100] = "";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--build-ios")) continue;
        else if (!strcmp(argv[i], "--no-game-files")) files = false;
        else if (argv[i][0] == '-') { fprintf(stderr, "usage: %s --build-ios [--no-game-files] [<out.ipa>]\n", argv[0]); return 1; }
        else if (argv[i][0] == '/') snprintf(out, sizeof out, "%s", argv[i]);
        else {  // (relative to where it was started: rt_paths_init changes the working directory)
            char cwd[900];
            snprintf(out, sizeof out, "%s/%s", getcwd(cwd, sizeof cwd) ? cwd : ".", argv[i]);
        }
    }
    rt_paths_init();
    if (!out[0]) iosbuild_default_out(out, sizeof out);
    signal(SIGINT, ios_cli_cancel);
    signal(SIGTERM, ios_cli_cancel);
    IosBuildOpts o = {files, out, ios_cli_progress};
    char why[512];
    if (!iosbuild_run(rt_data_root(), &o, why, sizeof why)) {
        fprintf(stderr, "%s\n", why);
        return 1;
    }
    printf("Saved %s\n", out);
    return 0;
}
#endif

int main(int argc, char **argv) {
    plat_init();
#if defined(__APPLE__) && !(defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--build-ios")) return build_ios_cli(argc, argv);
#endif
    if (g_guest_unreserved) {  // iOS without the extended virtual addressing entitlement (tools/make_ios_app.sh)
#if (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE) || defined(__ANDROID__)
        extern void plat_guest_report(uint64_t lo, uint64_t hi);  // runtime/platform/macos.c, linux.c
        plat_guest_report(GUEST_LO, GUEST_HI);
#endif
        char msg[400];
#ifdef __ANDROID__  // (something else already maps there: the Makefile's Android GUEST_ORIGIN would have to move)
        snprintf(msg, sizeof msg, "This copy of " GAME_TITLE " can't reserve the game's memory window (%#llx-%#llx) on this "
                 "device: something else is mapped there (details in the log).", GUEST_LO, GUEST_HI);
        fprintf(stderr, "FATAL: %s\n", msg);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, GAME_TITLE, msg, NULL);
        return 1;
#endif
        snprintf(msg, sizeof msg, "This copy of " GAME_TITLE " can't reserve the game's memory window (%#llx-%#llx): it "
                 "must be signed with the Extended Virtual Addressing and Increased Memory Limit entitlements.", GUEST_LO, GUEST_HI);
        fprintf(stderr, "FATAL: %s\n", msg);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, GAME_TITLE, msg, NULL);
        return 1;
    }
#ifdef __ANDROID__
    { extern void plat_guest_report(uint64_t lo, uint64_t hi); plat_guest_report(GUEST_LO, GUEST_HI); }  // (the log: the address space)
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");  // (Back opens the overlay menu, as Esc: runtime/ui/menu.mm)
#endif
#ifdef _WIN32
    setvbuf(stderr, NULL, _IONBF, 0);  // the Windows CRT treats _IOLBF as full buffering
#else
    setvbuf(stderr, NULL, _IOLBF, 0);
#endif
    const char *root = "extracted";  // (in the data root)
    static char arg_root[1024];
    bool setup = false;  // --setup: the setup screen even when everything is there
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--play")) continue;  // (no setup screen unless something is missing, as before)
        else if (!strcmp(argv[i], "--setup")) setup = true;
        else if (argv[i][0] != '-' && !realpath(argv[i], arg_root)) root = argv[i];  // (relative to where we started)
        else if (argv[i][0] != '-') root = arg_root;
    }
    rt_paths_init();  // the data root becomes the working directory (runtime/paths.h)
    {   // tests on a device without a shell (iOS): recomp-test.env in the data root, KEY=value lines, sets the environment
        FILE *f = fopen("recomp-test.env", "r");
        char line[1024];
        while (f && fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *eq = strchr(line, '=');
            if (line[0] == '#' || !eq || eq == line) continue;
            *eq = 0;
            setenv(line, eq + 1, 1);
            fprintf(stderr, "[main] recomp-test.env: %s set\n", line);
        }
        if (f) fclose(f);
    }
    // The setup screen (runtime/ui/launcher.mm): in place of the game until the player's files are set up, and after
    // Quit to Start Screen. Then the game boots in its window.
    extern bool launcher_needed(const char *root, bool asked);
    extern void launcher_run(const char *root);
    extern void launcher_boot_env(void);
    extern void launcher_update_check_start(void);
    launcher_update_check_start();  // (notifies only: the setup screen and the overlay menu's Mods tab)
    for (;;) {
        if (launcher_needed(root, setup)) launcher_run(root);
        else launcher_boot_env();
        setup = false;
        boot(root);
        int kind = ui_run();  // window and input on the main thread; returns when a reboot is requested
        teardown();
        if (kind == RT_REBOOT_TITLE) setup = true;  // (Quit to Start Screen: the setup screen)
    }
}
