// The Mac app's "Build for iPhone / iPad" (runtime/iosbuild.h). macOS only (the Makefile leaves it out elsewhere).
#include "iosbuild.h"

#include <copyfile.h>
#include <errno.h>
#include <ftw.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "gamebuild.h"
#include "paths.h"
#include "rt.h"

#ifndef GAMELIB_ID
#error "GAMELIB_ID comes from the Makefile"
#endif

#define IOS_TARGET "arm64-apple-ios17.0"
#define NEED_GB_WITH_FILES 8   // the game files cloned into the app (no space on APFS) and zipped (~3 GB), the build
#define NEED_GB_NO_FILES 3

static bool exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

// a command's first line of output (popen: xcrun with the environment's DEVELOPER_DIR). false: it failed.
static bool run_line(const char *cmd, char *out, size_t n) {
    FILE *f = popen(cmd, "r");
    if (!f) return false;
    out[0] = 0;
    if (!fgets(out, (int)n, f)) out[0] = 0;
    char rest[256];
    while (fgets(rest, sizeof rest, f)) {}
    int st = pclose(f);
    out[strcspn(out, "\r\n")] = 0;
    return st == 0 && out[0];
}

static bool kit_dir(char *out, size_t n) {
    char p[1100];
    if (!rt_resource("ios-kit/kit.json", "build/ios-kit/kit.json", p, sizeof p)) return false;
    snprintf(out, n, "%s", dirname(p));
    return true;
}

// a string field of kit.json ({"gamelib_id": "...", "ios_min": "...", "version": "..."}: written by make_ios_kit.sh)
static bool kit_field(const char *kit, const char *key, char *out, size_t n) {
    char p[1200], buf[1024], pat[64];
    snprintf(p, sizeof p, "%s/kit.json", kit);
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    size_t k = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[k] = 0;
    snprintf(pat, sizeof pat, "\"%s\": \"", key);
    const char *s = strstr(buf, pat);
    if (!s) return false;
    s += strlen(pat);
    const char *e = strchr(s, '"');
    if (!e || (size_t)(e - s) >= n) return false;
    snprintf(out, n, "%.*s", (int)(e - s), s);
    return true;
}

// Xcode's developer folder, without running xcrun first: on a Mac with no developer tools at all, /usr/bin/xcrun
// would put up macOS's "install the command line tools" dialog
static bool have_developer_dir(void) {
    const char *d = getenv("DEVELOPER_DIR");
    if (d && *d) return exists(d);
    char p[1100];
    return run_line("/usr/bin/xcode-select -p 2>/dev/null", p, sizeof p) && exists(p);
}

bool iosbuild_available(char *why, size_t n) {
    int arm = 0;
    size_t sz = sizeof arm;
    if (sysctlbyname("hw.optional.arm64", &arm, &sz, NULL, 0) != 0 || !arm) {
        snprintf(why, n, "Building for iPhone and iPad needs a Mac with Apple silicon.");
        return false;
    }
    char kit[1100], id[64];
    if (!kit_dir(kit, sizeof kit)) {
        snprintf(why, n, "This copy of the app has no iPhone kit.");
        return false;
    }
    if (!kit_field(kit, "gamelib_id", id, sizeof id) || strcmp(id, GAMELIB_ID) != 0) {
        snprintf(why, n, "This app's iPhone kit doesn't match it: reinstall the app.");
        return false;
    }
    char sdk[1100];
    if (!have_developer_dir() || !run_line("/usr/bin/xcrun --sdk iphoneos --show-sdk-path 2>/dev/null", sdk, sizeof sdk)) {
        snprintf(why, n, "Install Xcode from the App Store, open it once, and add iOS under Xcode > Settings > Components.");
        return false;
    }
    return true;
}

void iosbuild_default_out(char *out, size_t n) {
    char kit[1100], v[64] = "";
    if (kit_dir(kit, sizeof kit)) kit_field(kit, "version", v, sizeof v);
    const char *home = getenv("HOME");
    snprintf(out, n, "%s/Downloads/SMMaker2%s%s.ipa", home && *home ? home : "/tmp", v[0] ? "-" : "",
             v[0] == 'v' ? v + 1 : v);
}

// ------------------------------------------------------------------ files
static int rm_one(const char *p, const struct stat *st, int flag, struct FTW *ftw) {
    (void)st, (void)flag, (void)ftw;
    if (remove(p) != 0 && errno != ENOENT) LOG("[iosbuild] can't remove %s: %s\n", p, strerror(errno));
    return 0;
}

// removes the work folder (links inside are removed, never followed); only a folder named ios-build
static void rm_work(const char *work) {
    size_t k = strlen(work);
    if (k < 10 || strcmp(work + k - 10, "/ios-build") != 0) return;
    if (exists(work)) nftw(work, rm_one, 32, FTW_DEPTH | FTW_PHYS);
}

// clone_tree: src (links followed: the set-up romfs may be links into the update) into dst, file by file as APFS
// clones where it can (no space taken), else copies. nftw has no user data: one build at a time.
static const char *g_src, *g_dst;
static int clone_one(const char *p, const struct stat *st, int flag, struct FTW *ftw) {
    (void)st, (void)ftw;
    if (gamebuild_cancelled()) return 1;
    char d[2200];
    snprintf(d, sizeof d, "%s%s", g_dst, p + strlen(g_src));
    if (flag == FTW_D) return mkdir(d, 0755) == 0 || errno == EEXIST ? 0 : (LOG("[iosbuild] mkdir %s: %s\n", d, strerror(errno)), 1);
    if (flag != FTW_F) return 0;  // (a dangling link: nothing to copy)
    char real[1100];
    if (!realpath(p, real) || copyfile(real, d, NULL, COPYFILE_CLONE) != 0) {
        LOG("[iosbuild] can't copy %s: %s\n", p, strerror(errno));
        return 1;
    }
    return 0;
}
static bool clone_tree(const char *src, const char *dst) {
    g_src = src, g_dst = dst;
    return nftw(src, clone_one, 32, 0) == 0;
}

static bool copy_file(const char *src, const char *dst) { return copyfile(src, dst, NULL, COPYFILE_CLONE) == 0; }

// ------------------------------------------------------------------ the build
static const IosBuildOpts *g_opts;
static char g_work[1100], g_part[1200];  // what quitting mid-build removes

static void step(const char *s, float f) {
    LOG("[iosbuild] %s (%d%%)\n", s, (int)(f * 100));
    if (g_opts->progress) g_opts->progress(s, f);
}

static void compile_progress(double frac, const char *what) {  // gamebuild's share: 5% to 80%
    (void)what;
    if (g_opts->progress) g_opts->progress("Building the game code", 0.05f + 0.75f * (float)frac);
}

static void at_quit(void) {  // (quitting mid-build: the compilers end through gamebuild's own atexit)
    if (!g_work[0]) return;
    gamebuild_cancel();
    rm_work(g_work);
    if (g_part[0]) remove(g_part);
}

static void log_tail(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) LOG("[iosbuild]   %s", line);
    fclose(f);
}

typedef struct { char **v; int n, cap; } List;
static void push(List *l, const char *s) {
    if (l->n + 2 > l->cap) l->cap = l->cap ? l->cap * 2 : 32, l->v = (char **)realloc(l->v, sizeof *l->v * l->cap);
    l->v[l->n++] = strdup(s);
    l->v[l->n] = NULL;
}
static void list_free(List *l) {
    for (int i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    memset(l, 0, sizeof *l);
}
static bool push_lines(List *l, const char *path) {  // one argument per line (game_cflags.txt)
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0]) push(l, line);
    }
    fclose(f);
    return true;
}

bool iosbuild_run(const char *data_root, const IosBuildOpts *o, char *why, size_t n) {
    static bool registered;
    if (!registered) registered = true, atexit(at_quit);
    gamebuild_cancel_clear();
    g_opts = o;
    why[0] = 0;
    step("Checking", 0);
    if (!iosbuild_available(why, n)) return false;
    char game[1100], p[2200];
    snprintf(game, sizeof game, "%s/extracted", data_root);
    snprintf(p, sizeof p, "%s/exefs/main", game);
    bool set_up = exists(p);
    if (o->game_files) {
        static const char *const need[] = {"extracted/romfs", "fonts", "sysdata"};
        for (size_t i = 0; i < sizeof need / sizeof *need; i++) {
            snprintf(p, sizeof p, "%s/%s", data_root, need[i]);
            set_up = set_up && exists(p);
        }
    }
    if (!set_up) {
        snprintf(why, n, "Set up the game first.");
        return false;
    }
    struct statfs fs;
    double free_gb = statfs(data_root, &fs) == 0 ? (double)fs.f_bavail * fs.f_bsize / 1e9 : 0;
    const char *test_free = getenv("IOSBUILD_TEST_FREE_GB");
    if (test_free && *test_free) free_gb = atof(test_free);
    int need_gb = o->game_files ? NEED_GB_WITH_FILES : NEED_GB_NO_FILES;
    if (free_gb < need_gb) {
        snprintf(why, n, "Not enough free disk space: the build needs about %d GB free, and %.1f GB is.", need_gb, free_gb);
        return false;
    }
    char kit[1100], inc[1100], sdk[1100], clang[1100], clangxx[1200];
    if (!kit_dir(kit, sizeof kit) || !rt_resource("gamelib-src/cpu.h", "runtime/cpu.h", inc, sizeof inc) ||
        !run_line("/usr/bin/xcrun --sdk iphoneos --show-sdk-path 2>/dev/null", sdk, sizeof sdk) ||
        !run_line("/usr/bin/xcrun --sdk iphoneos -f clang 2>/dev/null", clang, sizeof clang)) {
        snprintf(why, n, "the iPhone build's tools are missing from this copy of the app");
        return false;
    }
    snprintf(clangxx, sizeof clangxx, "%s++", clang);
    { char t[1100]; snprintf(t, sizeof t, "%s", inc); snprintf(inc, sizeof inc, "%s", dirname(t)); }

    snprintf(g_work, sizeof g_work, "%s/ios-build", data_root);
    snprintf(g_part, sizeof g_part, "%s.part", o->out);
    char obj[1200], app[1200], log[1200];
    snprintf(obj, sizeof obj, "%s/obj", g_work);
    snprintf(app, sizeof app, "%s/Payload/SMM2Recomp.app", g_work);
    rm_work(g_work);
    remove(g_part);
    snprintf(p, sizeof p, "%s/Payload", g_work);
    bool ok = mkdir(g_work, 0755) == 0 && mkdir(obj, 0755) == 0 && mkdir(p, 0755) == 0 && mkdir(app, 0755) == 0;
    if (!ok) snprintf(why, n, "can't make %s (%s)", g_work, strerror(errno));
    LOG("[iosbuild] kit %s, compiler %s, SDK %s, game files %s, to %s\n", kit, clang, sdk, o->game_files ? "in" : "out", o->out);

    // 1. the game code, with Xcode's compiler for iOS
    List cc = {0}, cflags = {0}, link = {0};
    char **objs = NULL;
    if (ok) {
        push(&cc, clang), push(&cc, "-isysroot"), push(&cc, sdk), push(&cc, "-target"), push(&cc, IOS_TARGET);
        snprintf(p, sizeof p, "%s/game_cflags.txt", kit);
        ok = push_lines(&cflags, p);
        snprintf(p, sizeof p, "-I%s", inc), push(&cflags, p);
        push(&cflags, "-DGAMELIB_ID=\"" GAMELIB_ID "\"");
        long cpus = sysconf(_SC_NPROCESSORS_ONLN);
        GameBuildOpts go = {compile_progress, cpus > 1 ? (int)cpus : 1};
        step("Building the game code", 0.05f);
        if (!ok) snprintf(why, n, "the iPhone kit is incomplete: reinstall the app");
        else ok = gamebuild_compile(game, obj, (const char *const *)cc.v, (const char *const *)cflags.v, &go, &objs, why, n);
    }
    // 2. linked with the kit
    if (ok) {
        step("Linking", 0.82f);
        snprintf(p, sizeof p, "%s/objs.txt", g_work);
        FILE *f = fopen(p, "wb");
        for (char **ob = objs; f && *ob; ob++) fprintf(f, "%s\n", *ob);
        if (f) fclose(f);
        push(&link, clangxx), push(&link, "-isysroot"), push(&link, sdk), push(&link, "-target"), push(&link, IOS_TARGET);
        char a[2400];
        snprintf(a, sizeof a, "%s/SMM2Recomp", app), push(&link, "-o"), push(&link, a);
        snprintf(a, sizeof a, "-Wl,-filelist,%s", p), push(&link, a);
        snprintf(a, sizeof a, "-Wl,-force_load,%s/lib/libsmm2rt.a", kit), push(&link, a);
        snprintf(a, sizeof a, "%s/lib/libSDL3.a", kit), push(&link, a);
        snprintf(a, sizeof a, "%s/lib/libmmk_embed.a", kit);
        if (exists(a)) push(&link, a);
        snprintf(a, sizeof a, "@%s/link.txt", kit), push(&link, a);
        snprintf(log, sizeof log, "%s/link.log", g_work);
        int r = f ? gamebuild_exec((const char *const *)link.v, log) : -1;
        if (r != 0) {
            if (!gamebuild_cancelled()) log_tail(log);
            snprintf(why, n, gamebuild_cancelled() ? "cancelled" : "the iPhone app didn't link");
            ok = false;
        }
    }
    // 3. the bundle: Info.plist, icon, the game files
    if (ok) {
        char d[2400];
        snprintf(p, sizeof p, "%s/Info.plist", kit), snprintf(d, sizeof d, "%s/Info.plist", app);
        ok = copy_file(p, d);
        snprintf(p, sizeof p, "%s/icons", kit);
        ok = ok && clone_tree(p, app);  // (icons/* into the app)
        snprintf(d, sizeof d, "%s/PkgInfo", app);
        FILE *f = fopen(d, "wb");
        ok = ok && f && fputs("APPL????", f) >= 0;
        if (f) fclose(f);
        if (!ok) snprintf(why, n, "the iPhone app couldn't be put together");
    }
    if (ok && o->game_files) {  // (runtime/paths.c links data/* into Documents at each launch)
        step("Copying the game files", 0.86f);
        static const char *const from[] = {"extracted/exefs", "extracted/romfs", "fonts", "sysdata"};
        char d[2400];
        snprintf(d, sizeof d, "%s/data", app), ok = mkdir(d, 0755) == 0;
        snprintf(d, sizeof d, "%s/data/extracted", app), ok = ok && mkdir(d, 0755) == 0;
        for (size_t i = 0; ok && i < sizeof from / sizeof *from; i++) {
            snprintf(p, sizeof p, "%s/%s", data_root, from[i]);
            snprintf(d, sizeof d, "%s/data/%s", app, from[i]);
            ok = clone_tree(p, d);
        }
        if (!ok) snprintf(why, n, gamebuild_cancelled() ? "cancelled" : "the game files couldn't be copied into the app");
    }
    // 4. signed ad hoc, with the entitlements the guest memory window needs (the player re-signs it with their own)
    if (ok) {
        step("Signing", 0.92f);
        char ent[1200];
        snprintf(ent, sizeof ent, "%s/entitlements.plist", kit);
        const char *argv[] = {"/usr/bin/codesign", "--force", "--sign", "-", "--entitlements", ent, "--timestamp=none", app, NULL};
        snprintf(log, sizeof log, "%s/codesign.log", g_work);
        if (gamebuild_exec(argv, log) != 0) {
            if (!gamebuild_cancelled()) log_tail(log);
            snprintf(why, n, gamebuild_cancelled() ? "cancelled" : "the iPhone app couldn't be signed");
            ok = false;
        }
    }
    // 5. the .ipa: Payload/ zipped (no extended attributes: ._ files would break the signature), then renamed over
    if (ok) {
        step("Packing", 0.95f);
        snprintf(p, sizeof p, "%s/Payload", g_work);
        const char *argv[] = {"/usr/bin/ditto", "-c", "-k", "--norsrc", "--noextattr", "--noacl", "--keepParent", p, g_part, NULL};
        snprintf(log, sizeof log, "%s/ditto.log", g_work);
        if (gamebuild_exec(argv, log) != 0) {
            if (!gamebuild_cancelled()) log_tail(log);
            snprintf(why, n, gamebuild_cancelled() ? "cancelled" : "the .ipa couldn't be written (is the disk full?)");
            ok = false;
        } else if (rename(g_part, o->out) != 0) {
            snprintf(why, n, "can't save %s (%s)", o->out, strerror(errno));
            ok = false;
        }
    }
    gamebuild_free_objs(objs);
    list_free(&cc), list_free(&cflags), list_free(&link);
    remove(g_part);
    rm_work(g_work);
    g_work[0] = g_part[0] = 0;
    if (ok) step("Done", 1);
    else LOG("[iosbuild] failed: %s\n", why);
    return ok;
}
