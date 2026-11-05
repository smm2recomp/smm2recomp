// The setup screen (start screen), shown in place of the game until it is set up: nothing of the game ships with the
// app, so the player points it at their own dump, and the app prepares everything from it on their machine, then runs
// the game. Modelled on melee-pc's launcher (DECISIONS.md, "Releases: a launcher plus recompiler per platform"); the
// window, render loop and update check follow LAS Recomp's start screen (UPSTREAM.md).
//
//   1. Your game: Super Mario Maker 2 with the 3.0.3 update - its NSP / XCI files, or folders of the extracted ExeFS
//      and RomFS. games/smm2/setup.py checks the version (build ids and hashes of the player's own files against
//      games/smm2/versions.py), extracts NSP / XCI with hactool and the player's prod.keys, and lays the files out in
//      <data>/extracted.
//   2. prod.keys and the firmware (the console's system NCAs): games/smm2/sysdata.py copies the shared fonts (the game
//      stops at the title screen without them) and a few system data archives into <data>/fonts, <data>/sysdata.
//      The keys are read where they are, by hactool only: never copied, logged or shown.
//   3. Optional: Super Mario Maker (Wii U), for the SMM1 mods (games/smm2/mods/smm1: RECOMP_SMM1_DUMP).
//   Then the game code is recompiled from the player's ExeFS (runtime/gamebuild.c: the bundled Python runs the
//   generator, the bundled zig compiles it), and the game boots in the same window.
//
// What the player chose is kept in <data>/launcher.ini (paths only). Once everything is there the game starts straight
// away; `--setup`, or the overlay menu's Quit > Quit to Start Screen, brings this screen back. RECOMP_NO_LAUNCHER=1:
// never (the game starts, or says what's missing, as before).
//
// The update check asks GitHub for the project's latest release (a background curl, which macOS, Windows 10+ and
// Linux distributions ship) and only says that there is one, with a link to its page: nothing is downloaded.
//
// Tests: RECOMP_LAUNCHER_AUTO="game=<path>|game=<path>|keys=<file>|firmware=<dir>|smm1=<dir>" fills the page in and
// presses Set up after a moment (as the player would); RECOMP_LAUNCHER_SHOTS=<dir> saves the window as PNG whenever
// the screen changes (and every 30 s while a step runs); RECOMP_NO_UPDATE_CHECK=1; RECOMP_UPDATE_TAGS=<tag>: pretend
// that release exists (no network).
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "game.h"
#include "paths.h"
}

#if (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE) || defined(__ANDROID__)
// Mobile builds carry the game code in the app (made on a desktop from the player's files): no setup screen.
extern "C" bool launcher_needed(const char *, bool) { return false; }
extern "C" void launcher_run(const char *) {}
extern "C" void launcher_boot_env(void) {}
extern "C" void launcher_update_check_start(void) {}
extern "C" const char *launcher_update_tag(void) { return NULL; }
extern "C" bool launcher_settings_rows(void) { return false; }
extern "C" bool launcher_has_settings(void) { return false; }
#else

#ifdef __APPLE__
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <sys/xattr.h>
#import <AppKit/AppKit.h>
#endif
#include <SDL3/SDL.h>
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"
#ifdef __APPLE__
#include "imgui_impl_metal.h"
#else
#include "imgui_impl_vulkan.h"
#endif
#include "option_rows.h"
#include "osk.h"

extern "C" {
#include "gamebuild.h"
#include "gamelib.h"
#ifdef __APPLE__
#include "iosbuild.h"
#endif
#include "platform.h"
void rt_log(const char *fmt, ...);  // runtime/rt.h LOG
void ui_window_init(void);
SDL_Window *ui_window(void);
void menu_apply_window_requests(void *sdl_window);
#ifdef __APPLE__
void *ui_metal_layer(void);
void *ui_metal_device(void);
#else
bool vk_frame_begin(const float clear[4]);
void vk_frame_end(const char *shot_path);
void *vk_frame_cmd(void);
bool vk_imgui_backend_init(void);
void vk_imgui_backend_shutdown(void);
#endif
}

#ifdef _WIN32
#define SEP "\\"
static void env_set(const char *k, const char *v) { _putenv_s(k, v ? v : ""); }
#else
#define SEP "/"
static void env_set(const char *k, const char *v) {
    if (v) setenv(k, v, 1);
    else unsetenv(k);
}
#endif

#define LOG(...) rt_log(__VA_ARGS__)
#ifndef RECOMP_VERSION
#define RECOMP_VERSION "dev"
#endif
#define RELEASES_API "https://api.github.com/repos/smm2recomp/smm2recomp/releases/latest"
#define RELEASES_PAGE "https://github.com/smm2recomp/smm2recomp/releases"

static const char MAIN_303[] = "c2dc405ac414c37c8b1c50219c7a0f0c";  // (games/smm2/versions.py: v3.0.3 main build id)
static const char MAIN_100[] = "3ebb1eb2fffb029a8b87579b961a7b";    // (v1.0.0: the base game without the update)
static const char *FONT_STD = "fonts" SEP "nintendo_udsg-r_std_003.bfttf";

static bool file_exists(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
static bool dir_exists(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
static std::string home_dir(void) {
#ifdef _WIN32
    const char *h = getenv("USERPROFILE");
#else
    const char *h = getenv("HOME");
#endif
    return h ? h : "";
}
static std::string lower(std::string s) {
    for (auto &c : s) c = (char)tolower((unsigned char)c);
    return s;
}
static bool ends_with(const std::string &s, const char *e) {
    size_t n = strlen(e);
    return s.size() >= n && lower(s.substr(s.size() - n)) == e;
}
static std::string base_name(const std::string &p) {
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

// The build id of an NSO (hex, trailing zero bytes dropped), "" if it isn't one
static std::string nso_build_id(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return "";
    uint8_t h[0x60] = {0};
    size_t n = fread(h, 1, sizeof h, f);
    fclose(f);
    if (n != sizeof h || memcmp(h, "NSO0", 4)) return "";
    int end = 0x60;
    while (end > 0x40 && !h[end - 1]) end--;
    std::string s;
    char b[3];
    for (int i = 0x40; i < end; i++) snprintf(b, sizeof b, "%02x", h[i]), s += b;
    return s;
}
static bool starts_with(const std::string &s, const char *p) { return s.compare(0, strlen(p), p) == 0; }

// ------------------------------------------------------------------ what's ready
static std::string g_root;  // <data>/extracted

static bool game_files_ok(const std::string &root) {
    return starts_with(nso_build_id(root + SEP "exefs" SEP "main"), MAIN_303) && dir_exists(root + SEP "romfs") &&
           file_exists(root + SEP "exefs" SEP "main.npdm");
}
static bool fonts_ok(void) { return file_exists(FONT_STD); }  // (relative: the data root is the working directory)

extern "C" bool launcher_needed(const char *root, bool asked) {
    if (getenv("RECOMP_NO_LAUNCHER") && atoi(getenv("RECOMP_NO_LAUNCHER"))) return false;
    if (getenv("LARECOMP_REBOOT_AT")) asked = false;  // (reboot tests: straight back into the game, as before)
    if (asked || getenv("RECOMP_LAUNCHER_AUTO")) return true;
    // (a source checkout started in its data folder: its own fonts or none, as before)
    return !game_files_ok(root) || !gamelib_present(root) || (!fonts_ok() && !rt_dev_mode());
}

// ------------------------------------------------------------------ launcher.ini (paths only, never key contents)
struct Config {
    std::vector<std::string> game;  // NSP / XCI files and folders with the extracted game
    std::string keys, firmware, smm1;
    bool update_check = true;
};
static Config g_cfg;
static const char *INI = "launcher.ini";

static std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') i++;
    return s.substr(i);
}
static void config_load(void) {
    g_cfg = Config();
    FILE *f = fopen(INI, "r");
    if (!f) return;
    char line[4200];
    while (fgets(line, sizeof line, f)) {
        std::string l = line;
        size_t eq = l.find('=');
        if (l[0] == '#' || eq == std::string::npos) continue;
        std::string k = trim(l.substr(0, eq)), v = trim(l.substr(eq + 1));
        if (k == "game" && !v.empty()) g_cfg.game.push_back(v);
        else if (k == "keys") g_cfg.keys = v;
        else if (k == "firmware") g_cfg.firmware = v;
        else if (k == "smm1") g_cfg.smm1 = v;
        else if (k == "update_check") g_cfg.update_check = atoi(v.c_str()) != 0;
    }
    fclose(f);
}
static void config_save(void) {
    FILE *f = fopen(INI, "w");
    if (!f) return;
    fprintf(f, "# " GAME_TITLE " setup screen: where your own files are (paths only)\n");
    for (auto &g : g_cfg.game) fprintf(f, "game=%s\n", g.c_str());
    fprintf(f, "keys=%s\nfirmware=%s\nsmm1=%s\nupdate_check=%d\n", g_cfg.keys.c_str(), g_cfg.firmware.c_str(),
            g_cfg.smm1.c_str(), g_cfg.update_check ? 1 : 0);
    fclose(f);
}

// Before every boot: the SMM1 dump the player chose, for the SMM1 mods (games/smm2/mods/smm1/smm1_data.c)
extern "C" void launcher_boot_env(void) {
    config_load();
    if (!g_cfg.smm1.empty() && !getenv("RECOMP_SMM1_DUMP")) env_set("RECOMP_SMM1_DUMP", g_cfg.smm1.c_str());
}

// Where emulators keep the keys and the firmware they were given (suggestions; the player can choose others)
static std::vector<std::string> keys_candidates(void) {
    std::string h = home_dir();
    std::vector<std::string> v;
#ifdef _WIN32
    const char *ad = getenv("APPDATA");
    std::string a = ad ? ad : h + "\\AppData\\Roaming";
    v = {a + "\\Ryujinx\\system\\prod.keys", a + "\\yuzu\\keys\\prod.keys", a + "\\suyu\\keys\\prod.keys",
         a + "\\eden\\keys\\prod.keys", a + "\\citron\\keys\\prod.keys", h + "\\.switch\\prod.keys"};
#elif defined(__APPLE__)
    std::string a = h + "/Library/Application Support";
    v = {a + "/Ryujinx/system/prod.keys", a + "/yuzu/keys/prod.keys", a + "/eden/keys/prod.keys", h + "/.switch/prod.keys"};
#else
    const char *xc = getenv("XDG_CONFIG_HOME"), *xd = getenv("XDG_DATA_HOME");
    std::string c = xc && *xc ? xc : h + "/.config", d = xd && *xd ? xd : h + "/.local/share";
    v = {c + "/Ryujinx/system/prod.keys", d + "/yuzu/keys/prod.keys", d + "/suyu/keys/prod.keys",
         d + "/eden/keys/prod.keys", h + "/.var/app/io.github.ryubing.Ryujinx/config/Ryujinx/system/prod.keys",
         h + "/.switch/prod.keys"};
#endif
    return v;
}
static std::vector<std::string> firmware_candidates(void) {
    std::string h = home_dir();
#ifdef _WIN32
    const char *ad = getenv("APPDATA");
    std::string a = ad ? ad : h + "\\AppData\\Roaming";
    return {a + "\\Ryujinx\\bis\\system\\Contents\\registered", a + "\\yuzu\\nand\\system\\Contents\\registered",
            a + "\\eden\\nand\\system\\Contents\\registered"};
#elif defined(__APPLE__)
    std::string a = h + "/Library/Application Support";
    return {a + "/Ryujinx/bis/system/Contents/registered", a + "/yuzu/nand/system/Contents/registered"};
#else
    const char *xc = getenv("XDG_CONFIG_HOME"), *xd = getenv("XDG_DATA_HOME");
    std::string c = xc && *xc ? xc : h + "/.config", d = xd && *xd ? xd : h + "/.local/share";
    return {c + "/Ryujinx/bis/system/Contents/registered", d + "/yuzu/nand/system/Contents/registered",
            d + "/eden/nand/system/Contents/registered"};
#endif
}

// ------------------------------------------------------------------ checks of what was chosen (cheap, no keys shown)
struct Check {
    bool ok = false, warn = false;
    std::string text;
};

// A source of game files: an NSP / XCI, or a folder (the version of an ExeFS in it, if there is one near the top)
static Check check_game_source(const std::string &p) {
    Check c;
    if (file_exists(p)) {
        if (ends_with(p, ".nsp") || ends_with(p, ".xci")) {
            c.ok = true;
            c.text = std::string(ends_with(p, ".nsp") ? "NSP" : "XCI") + " file, checked and extracted when set up";
        } else if (ends_with(p, ".nsz") || ends_with(p, ".xcz")) {
            c.text = "compressed (NSZ / XCZ): decompress it to an NSP / XCI first";
        } else {
            c.text = "not an NSP, XCI or folder";
        }
        return c;
    }
    if (!dir_exists(p)) { c.text = "not found"; return c; }
    c.ok = true;
    const char *places[] = {"", SEP "exefs", SEP "ExeFS"};
    for (const char *pl : places) {
        std::string id = nso_build_id(p + pl + SEP "main");
        if (id.empty()) continue;
        if (starts_with(id, MAIN_303)) c.text = "folder: game code 3.0.3";
        else if (starts_with(id, MAIN_100)) c.text = "folder: game code 1.0.0 - also add the 3.0.3 update", c.warn = true;
        else c.text = "folder: game code of another version", c.warn = true;
        return c;
    }
    c.text = "folder, searched when set up";
    return c;
}

static Check check_keys(const std::string &p) {
    Check c;
    if (p.empty()) { c.text = "needed for the fonts (and to extract an NSP / XCI)"; return c; }
    FILE *f = fopen(p.c_str(), "r");
    if (!f) { c.text = "not found"; return c; }
    // only the names are looked at (each line "name = value"); nothing is kept
    char line[512];
    int n = 0;
    bool header = false, master = false;
    while (fgets(line, sizeof line, f) && n < 4000) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        n++;
        if (!strncmp(line, "header_key", 10)) header = true;
        if (!strncmp(line, "master_key_", 11)) master = true;
    }
    fclose(f);
    memset(line, 0, sizeof line);
    c.ok = header && master;
    c.text = c.ok ? "keys file found (read in place, never copied)" : "doesn't look like a prod.keys (no header_key / master keys)";
    return c;
}

static Check check_firmware(const std::string &p) {
    Check c;
    if (p.empty()) { c.text = "needed: the console's fonts come from it"; return c; }
    DIR *d = opendir(p.c_str());
    if (!d) { c.text = "not found"; return c; }
    int n = 0;
    while (struct dirent *e = readdir(d)) n += ends_with(e->d_name, ".nca");
    closedir(d);
    c.ok = n > 0;
    char b[200];
    snprintf(b, sizeof b, n ? "%d system files" : "no NCA files in this folder (choose the folder with the firmware's .nca files)", n);
    c.text = b;
    return c;
}

// SMM1 (Wii U): decrypted titles - meta/meta.xml with Super Mario Maker's title id next to content/ (as smm1_data.c)
static void smm1_scan(const std::string &dir, int depth, std::vector<int> &versions) {
    std::string meta = dir + SEP "meta" SEP "meta.xml";
    if (FILE *f = fopen(meta.c_str(), "rb")) {
        char buf[8192];
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = 0;
        std::string x = lower(buf);
        size_t t = x.find("<title_id");
        if (t != std::string::npos && (t = x.find('>', t)) != std::string::npos && dir_exists(dir + SEP "content")) {
            std::string id = x.substr(t + 1, 16);
            if (id.size() == 16 && (id.substr(8) == "1018dc00" || id.substr(8) == "1018dd00" || id.substr(8) == "1014b700")) {
                size_t v = x.find("<title_version");
                versions.push_back(v != std::string::npos && (v = x.find('>', v)) != std::string::npos ? atoi(x.c_str() + v + 1) : 0);
                return;
            }
        }
    }
    if (depth <= 0) return;
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> sub;
    while (struct dirent *e = readdir(d))
        if (e->d_name[0] != '.' && dir_exists(dir + SEP + e->d_name)) sub.push_back(dir + SEP + e->d_name);
    closedir(d);
    for (auto &s : sub) smm1_scan(s, depth - 1, versions);
}
static Check check_smm1(const std::string &p) {
    Check c;
    if (p.empty()) { c.ok = true; c.text = "optional"; return c; }
    std::vector<int> v;
    smm1_scan(p, 3, v);
    if (v.empty()) { c.text = "no decrypted Super Mario Maker (Wii U) title found here"; return c; }
    c.ok = true;
    c.text = "found: ";
    for (size_t i = 0; i < v.size(); i++) c.text += (i ? ", v" : "v") + std::to_string(v[i]);
    if (v.size() == 1 && v[0] < 208) c.text += " (add update v208 too, for Super Expert)", c.warn = true;
    return c;
}

// The checks of what's chosen, at most every 2 s per path (the screen draws 60 times a second; files can appear)
static Check cached_check(char kind, const std::string &p, Check (*fn)(const std::string &)) {
    struct Entry { double t; Check c; };
    static std::vector<std::pair<std::string, Entry>> cache;
    std::string key = std::string(1, kind) + p;
    double now = SDL_GetTicks() / 1000.0;
    for (auto &e : cache)
        if (e.first == key) {
            if (now - e.second.t > 2) e.second = {now, fn(p)};
            return e.second.c;
        }
    if (cache.size() > 64) cache.clear();
    cache.push_back({key, {now, fn(p)}});
    return cache.back().second.c;
}

// ------------------------------------------------------------------ the tools
struct Tools {
    std::string python, setup, sysdata, hactool;
};
static bool find_tools(Tools &t, std::string &why) {
    char b[1100];
    if (!gamebuild_python(b, sizeof b)) { why = "the Python that comes with this program is missing"; return false; }
    t.python = b;
    if (!rt_resource("tools" SEP "games" SEP "smm2" SEP "setup.py", "games/smm2/setup.py", b, sizeof b)) {
        why = "tools/games/smm2/setup.py is missing";
        return false;
    }
    t.setup = b;
    if (!rt_resource("tools" SEP "games" SEP "smm2" SEP "sysdata.py", "games/smm2/sysdata.py", b, sizeof b)) {
        why = "tools/games/smm2/sysdata.py is missing";
        return false;
    }
    t.sysdata = b;
    if (rt_resource("hactool" SEP "hactool" PLAT_EXE_SUFFIX, NULL, b, sizeof b)) t.hactool = b;
    else t.hactool.clear();  // (a source checkout: the scripts look on PATH and in ~/smm-work/tools/bin)
    return true;
}

// ------------------------------------------------------------------ the setup job (background thread)
enum StepState { PENDING, RUNNING, DONE, SKIPPED, FAILED };
struct Step {
    const char *name;
    StepState state;
    std::string detail;
};
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static Step g_steps[3] = {{"Check and lay out your game files", PENDING, ""},
                          {"Copy the fonts and system data from your firmware", PENDING, ""},
                          {"Recompile the game code", PENDING, ""}};
static std::atomic<float> g_progress{0};
static std::atomic<int> g_job{0};  // 0 idle, 1 running, 2 done (play), 3 failed
static std::atomic<bool> g_built_wait{false};  // the game code was just built on a small Linux machine: Play, not a start
static std::string g_err_what, g_err_hint, g_err_log;
static std::vector<std::string> g_found;
static std::atomic<bool> g_cancel{false};
static PlatProc g_child;

static void set_step(int i, StepState s, const std::string &detail = "") {
    pthread_mutex_lock(&g_lock);
    g_steps[i].state = s;
    if (!detail.empty() || s != RUNNING) g_steps[i].detail = detail;
    pthread_mutex_unlock(&g_lock);
}
static void fail(int step, const std::string &what, const std::string &hint, const std::string &log = "") {
    pthread_mutex_lock(&g_lock);
    g_steps[step].state = FAILED;
    g_err_what = what, g_err_hint = hint, g_err_log = log;
    pthread_mutex_unlock(&g_lock);
    LOG("[launcher] %s failed: %s\n", g_steps[step].name, what.c_str());
    g_job = 3;
}

static std::string read_text(const std::string &p) {
    std::string s;
    if (FILE *f = fopen(p.c_str(), "rb")) {
        char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0 && s.size() < (4u << 20)) s.append(buf, n);
        fclose(f);
    }
    return s;
}

// Runs a setup script, following its "@" lines (step text, progress lo..hi, what it found, its error). true: it worked.
static bool run_script(int step, const std::vector<std::string> &args, const std::string &log, float lo, float hi) {
    std::vector<const char *> av;
    for (auto &a : args) av.push_back(a.c_str());
    av.push_back(NULL);
    PlatProc p = plat_spawn(av.data(), log.c_str(), false);
    if (!p) {
        fail(step, "The setup tool didn't start", "Reinstall " GAME_TITLE ": its tools folder may be damaged.", log);
        return false;
    }
    pthread_mutex_lock(&g_lock);
    g_child = p;
    pthread_mutex_unlock(&g_lock);
    int rc;
    size_t seen = 0;
    std::string err;
    for (;;) {
        rc = plat_proc_wait(p, false);
        std::string out = read_text(log);
        for (size_t nl; (nl = out.find('\n', seen)) != std::string::npos; seen = nl + 1) {
            std::string l = out.substr(seen, nl - seen);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (starts_with(l, "@step ")) set_step(step, RUNNING, l.substr(6));
            else if (starts_with(l, "@progress ")) g_progress = lo + (hi - lo) * (float)atof(l.c_str() + 10);
            else if (starts_with(l, "@found ")) {
                pthread_mutex_lock(&g_lock);
                g_found.push_back(l.substr(7));
                pthread_mutex_unlock(&g_lock);
            } else if (starts_with(l, "@error ")) err = l.substr(7);
        }
        if (rc >= 0) break;
        SDL_Delay(100);
    }
    pthread_mutex_lock(&g_lock);
    g_child = 0;
    pthread_mutex_unlock(&g_lock);
    if (rc == 0 && err.empty()) return true;
    if (g_cancel) return false;
    std::string what = "It stopped with an error", hint = "The log has the details.";
    if (!err.empty()) {  // "<code>\t<what>\t<what to do>"
        size_t a = err.find('\t'), b = a == std::string::npos ? a : err.find('\t', a + 1);
        if (a != std::string::npos) what = err.substr(a + 1, b == std::string::npos ? b : b - a - 1);
        if (b != std::string::npos) hint = err.substr(b + 1);
    } else {  // (a Python error: its last line; the scripts never print key values)
        std::string out = read_text(log);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        size_t nl = out.find_last_of('\n');
        what = out.substr(nl == std::string::npos ? 0 : nl + 1);
        if (what.size() > 300) what = what.substr(0, 300);
    }
    fail(step, what, hint, log);
    return false;
}

static void build_progress(double f, const char *what) {
    g_progress = 0.25f + 0.75f * (float)f;
    static const char *last;
    if (what != last) last = what, set_step(2, RUNNING, what);
}

static std::string setup_stamp(void) {  // what the game files were set up from (setup.json's sources)
    return read_text(g_root + SEP "setup.json");
}

static void *job_thread(void *) {
    Tools t;
    std::string why;
    std::string root = g_root;
    g_progress = 0;
    pthread_mutex_lock(&g_lock);
    for (auto &s : g_steps) s.state = PENDING, s.detail.clear();
    g_found.clear();
    pthread_mutex_unlock(&g_lock);
    bool need_files = !game_files_ok(root), need_fonts = !fonts_ok();
    // chosen files other than those the game was set up from: set it up again
    if (!need_files) {
        std::string stamp = setup_stamp();
        for (auto &g : g_cfg.game) {
            std::string esc;  // (as setup.py wrote it: JSON, the path as given)
            for (char c : g) esc += c == '\\' || c == '"' ? std::string("\\") + c : std::string(1, c);
            if (!stamp.empty() && stamp.find("\"" + esc + "\"") == std::string::npos) need_files = true;
        }
    }
    if ((need_files || need_fonts) && !find_tools(t, why)) {
        fail(need_files ? 0 : 1, "The setup tools aren't there (" + why + ")",
             "Download " GAME_TITLE " again and unpack all of it.");
        return NULL;
    }
    std::string logs = std::string(rt_data_root()) + SEP "logs";
    mkdir(logs.c_str(), 0755);

    // 1. the game files
    if (need_files) {
        set_step(0, RUNNING);
        LOG("[launcher] setting up the game files from %zu source(s)\n", g_cfg.game.size());
        std::vector<std::string> a = {t.python, t.setup};
        for (auto &g : g_cfg.game) a.push_back("--source"), a.push_back(g);
        if (!g_cfg.keys.empty()) a.push_back("--keys"), a.push_back(g_cfg.keys);
        if (!t.hactool.empty()) a.push_back("--hactool"), a.push_back(t.hactool);
        a.push_back("--out"), a.push_back(root);
        if (!run_script(0, a, logs + SEP "setup.log", 0, 0.2f)) return NULL;
        if (!game_files_ok(root)) {
            fail(0, "The game files weren't set up", "Check the files you chose and try again.", logs + SEP "setup.log");
            return NULL;
        }
        set_step(0, DONE, "Super Mario Maker 2 3.0.3");
    } else {
        set_step(0, SKIPPED, "already set up: Super Mario Maker 2 3.0.3");
    }
    g_progress = 0.2f;

    // 2. fonts and system data
    if (need_fonts) {
        set_step(1, RUNNING);
        LOG("[launcher] copying the fonts and system data from the firmware\n");
        std::vector<std::string> a = {t.python, t.sysdata, "--firmware", g_cfg.firmware, "--keys", g_cfg.keys,
                                      "--data", rt_data_root()};
        if (!t.hactool.empty()) a.push_back("--hactool"), a.push_back(t.hactool);
        if (!run_script(1, a, logs + SEP "sysdata.log", 0.2f, 0.25f)) return NULL;
        if (!fonts_ok()) {
            fail(1, "The fonts weren't copied", "Check the firmware folder and prod.keys.", logs + SEP "sysdata.log");
            return NULL;
        }
        set_step(1, DONE, "shared fonts and system data copied");
    } else {
        set_step(1, SKIPPED, "already there");
    }
    g_progress = 0.25f;

    // 3. the game code (setup.py checked the ExeFS against the known 3.0.3 hashes: a library built from it before is
    // still the right one, wherever the files came from)
    if (!gamelib_present(root.c_str())) {
        set_step(2, RUNNING, "Starting...");
        if (!gamebuild_available()) {
            fail(2, "The recompiler isn't there", "Download " GAME_TITLE " again and unpack all of it (zig, tools, python).");
            return NULL;
        }
        int cores = SDL_GetNumLogicalCPUCores(), ram_gb = SDL_GetSystemRAM() / 1024;
        // (a compiler takes up to ~1 GB, and the generators, which run meanwhile, ~4 GB together)
        int jobs = cores < ram_gb - 6 ? cores : ram_gb - 6;
        if (getenv("RECOMP_BUILD_JOBS") && atoi(getenv("RECOMP_BUILD_JOBS")) > 0) jobs = atoi(getenv("RECOMP_BUILD_JOBS"));
        GameBuildOpts o = {build_progress, jobs > 1 ? jobs : 1};
        char w[512] = "";
        LOG("[launcher] recompiling the game code (%d jobs)\n", o.jobs);
        if (!gamebuild_run(root.c_str(), &o, w, sizeof w)) {
            if (g_cancel) return NULL;
            fail(2, std::string("The game code didn't build: ") + w,
                 "Make sure there are about 4 GB of disk space and 4 GB of memory free, then try again. If it keeps happening, report it on GitHub "
                 "with the log.", std::string(rt_data_root()) + SEP "smm2recomp.log");
            return NULL;
        }
        set_step(2, DONE, "built");
#if defined(__linux__) && !defined(__ANDROID__)
        // A Steam Deck (16 GB) froze starting the game straight after the build in desktop mode (#13): the build and
        // the first start together are too much for it there. Back to the screen instead: Play (or Gaming Mode)
        if (ram_gb <= 16) {
            LOG("[launcher] built on a %d GB machine: the game starts with Play, not straight away\n", ram_gb);
            g_built_wait = true;
            g_job = 0;
            return NULL;
        }
#endif
    } else {
        set_step(2, SKIPPED, "already built");
    }
    g_progress = 1;
    g_job = 2;
    return NULL;
}

// ------------------------------------------------------------------ update check (notifies only)
static std::string g_update_tag;
static pthread_mutex_t g_update_lock = PTHREAD_MUTEX_INITIALIZER;

static long long tag_rank(const std::string &t) {  // v<major>.<minor>.<patch> (anything after: the same release)
    int a, b, c;
    if (sscanf(t.c_str(), "v%d.%d.%d", &a, &b, &c) == 3 && a >= 0 && b >= 0 && c >= 0 && b < 4096 && c < 4096)
        return ((long long)a << 24) + ((long long)b << 12) + c;
    return -1;  // (dev builds: any release is newer)
}
extern "C" void menu_notice(const char *text, double seconds);  // (menu.mm: a notice over the game)
static void *update_thread(void *) {
    std::string body;
    if (const char *t = getenv("RECOMP_UPDATE_TAGS")) {
        body = std::string("\"tag_name\": \"") + t + "\"";
    } else {
#ifdef _WIN32
        const char *sr = getenv("SystemRoot");
        std::string curl = std::string(sr ? sr : "C:\\Windows") + "\\System32\\curl.exe";
#else
        std::string curl = file_exists("/usr/bin/curl") ? "/usr/bin/curl" : "/bin/curl";
#endif
        std::string dir = std::string(rt_data_root()) + SEP "cache";
        mkdir(dir.c_str(), 0755);
        std::string out = dir + SEP "latest-release.json";
        const char *av[] = {curl.c_str(), "-fsSL", "--max-time", "15", "-H", "Accept: application/vnd.github+json",
                            "-A", "smm2recomp/" RECOMP_VERSION, RELEASES_API, NULL};
        PlatProc p = file_exists(curl) ? plat_spawn(av, out.c_str(), false) : 0;
        if (!p || plat_proc_wait(p, true) != 0) { remove(out.c_str()); return NULL; }
        body = read_text(out);
        remove(out.c_str());
    }
    size_t at = body.find("\"tag_name\"");
    if (at == std::string::npos) return NULL;
    size_t q1 = body.find('"', body.find(':', at) + 1), q2 = q1 == std::string::npos ? q1 : body.find('"', q1 + 1);
    if (q2 == std::string::npos) return NULL;
    std::string tag = body.substr(q1 + 1, q2 - q1 - 1);
    if (tag_rank(tag) > tag_rank(RECOMP_VERSION)) {
        LOG("[launcher] a newer release is out: %s (this is %s): %s\n", tag.c_str(), RECOMP_VERSION, RELEASES_PAGE);
        pthread_mutex_lock(&g_update_lock);
        g_update_tag = tag;
        pthread_mutex_unlock(&g_update_lock);
        // said over the game too, once it's past its start logos: the Mods tab's row alone went unseen (issue #25)
        while (SDL_GetTicks() < 25000) SDL_Delay(500);
        char note[200];
        snprintf(note, sizeof note, "Version %s is out (this is %s). Menu (F1) > Online: New version.",
                 tag.c_str(), RECOMP_VERSION);
        menu_notice(note, 12);
    }
    return NULL;
}
extern "C" void launcher_update_check_start(void) {
    static bool started;
    if (started || (getenv("RECOMP_NO_UPDATE_CHECK") && !getenv("RECOMP_UPDATE_TAGS"))) return;
    config_load();
    if (!g_cfg.update_check) return;
    started = true;
    pthread_t th;
    if (pthread_create(&th, NULL, update_thread, NULL) == 0) pthread_detach(th);
}
extern "C" const char *launcher_update_tag(void) {
    static std::string copy;
    pthread_mutex_lock(&g_update_lock);
    copy = g_update_tag;
    pthread_mutex_unlock(&g_update_lock);
    return copy.empty() ? NULL : copy.c_str();
}

// The overlay menu's Mods tab, its app section (runtime/ui/menu.mm tab_mods): updates and the game files. true: drawn.
extern "C" void rt_request_reboot(int kind);
extern "C" void menu_toggle(void);
extern "C" bool launcher_has_settings(void) { return true; }
extern "C" bool launcher_settings_rows(void) {
    static bool loaded;
    if (!loaded) loaded = true, config_load();
    opt_section("App");
    bool uc = g_cfg.update_check;
    if (opt_toggle("Check for updates", &uc, true,
                   "Asks GitHub for the newest release of " GAME_TITLE " at each start and says when there is one. "
                   "Nothing is downloaded or installed by itself.")) {
        g_cfg.update_check = uc;
        config_save();
        if (uc) launcher_update_check_start();
    }
    if (const char *tag = launcher_update_tag()) {
        char d[300];
        snprintf(d, sizeof d, "Version %s is out (this is %s). Opens its download page in your browser.", tag, RECOMP_VERSION);
        if (opt_action("New version available", tag, d)) SDL_OpenURL(RELEASES_PAGE);
    }
    if (opt_action("Game files", "Set up...", "Back to the setup screen: choose other game files, keys, firmware or "
                   "the SMM1 dump. Progress since your last save is lost."))
        rt_request_reboot(2 /* RT_REBOOT_TITLE */), menu_toggle();
    return true;
}

// ------------------------------------------------------------------ file dialogs (SDL calls back on any thread)
enum PickTarget { PICK_GAME, PICK_KEYS, PICK_FIRMWARE, PICK_SMM1 };
static std::vector<std::pair<int, std::string>> g_picked;
static void SDLCALL picked(void *target, const char *const *files, int) {
    if (!files) return;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; files[i]; i++) g_picked.push_back({(int)(intptr_t)target, files[i]});
    pthread_mutex_unlock(&g_lock);
}
static std::string dialog_start(const std::string &p) {
    if (dir_exists(p)) return p;
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? home_dir() : p.substr(0, s);
}

// ------------------------------------------------------------------ screenshots (tests: RECOMP_LAUNCHER_SHOTS)
#ifdef __APPLE__
static void save_png(id<MTLTexture> tex, const char *path) {
    size_t w = tex.width, h = tex.height;
    std::vector<uint8_t> px(w * h * 4);
    [tex getBytes:px.data() bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(px.data(), w, h, 8, w * 4, cs, kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, (__bridge CFStringRef)UTTypePNG.identifier, 1, NULL);
    if (dest) {
        CGImageDestinationAddImage(dest, img, NULL);
        CGImageDestinationFinalize(dest);
        CFRelease(dest);
    }
    CFRelease(url), CGImageRelease(img), CGContextRelease(ctx), CGColorSpaceRelease(cs);
}
#endif

// ------------------------------------------------------------------ drawing
static ImFont *g_font, *g_font_big, *g_font_title;
static const ImVec4 GOLD(0.93f, 0.76f, 0.33f, 1), DIM(0.62f, 0.64f, 0.58f, 1), GOOD(0.45f, 0.85f, 0.55f, 1),
    WARN(0.98f, 0.75f, 0.35f, 1), BAD(1.0f, 0.45f, 0.40f, 1);

static void style_init(void) {
    ImGuiStyle &s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowRounding = 14, s.FrameRounding = 8, s.GrabRounding = 8, s.ChildRounding = 12;
    s.WindowPadding = ImVec2(28, 22), s.FramePadding = ImVec2(14, 8), s.ItemSpacing = ImVec2(12, 10);
    s.WindowBorderSize = 0, s.ChildBorderSize = 0, s.ScrollbarSize = 16;
    ImVec4 *c = s.Colors;
    ImVec4 green(0.15f, 0.36f, 0.25f, 1);
    c[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ChildBg] = ImVec4(0.07f, 0.11f, 0.10f, 0.92f);
    c[ImGuiCol_PopupBg] = ImVec4(0.07f, 0.10f, 0.09f, 0.98f);
    c[ImGuiCol_Text] = ImVec4(0.96f, 0.94f, 0.88f, 1);
    c[ImGuiCol_TextDisabled] = DIM;
    c[ImGuiCol_FrameBg] = ImVec4(0.13f, 0.19f, 0.16f, 1);
    c[ImGuiCol_FrameBgHovered] = green;
    c[ImGuiCol_FrameBgActive] = ImVec4(0.19f, 0.45f, 0.31f, 1);
    c[ImGuiCol_Button] = ImVec4(0.16f, 0.26f, 0.21f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.21f, 0.42f, 0.30f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.24f, 0.52f, 0.36f, 1);
    c[ImGuiCol_CheckMark] = GOLD;
    c[ImGuiCol_NavCursor] = GOLD;
    c[ImGuiCol_PlotHistogram] = ImVec4(0.30f, 0.70f, 0.45f, 1);
    c[ImGuiCol_Separator] = ImVec4(0.30f, 0.36f, 0.30f, 1);
}

static void wrapped(const ImVec4 &col, const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(buf);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

static void status_line(const Check &c, float indent) {
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);  // (under the path field)
    ImGui::PushStyleColor(ImGuiCol_Text, c.ok ? (c.warn ? WARN : GOOD) : BAD);
    ImGui::TextUnformatted(c.ok ? (c.warn ? "!" : "OK") : "X");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    wrapped(c.ok ? DIM : BAD, "%s", c.text.c_str());
}

// A path row: the field (typed or pasted) and Browse. true when it changed.
static bool path_row(const char *id, std::string &path, const char *hint, PickTarget target, bool folder, float s) {
    ImGui::PushID(id);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(id);
    ImGui::SameLine(130 * s);
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", path.c_str());
    float bw = 150 * s;
    ImGui::SetNextItemWidth(-bw - ImGui::GetStyle().ItemSpacing.x);
    bool changed = false;
    if (ui_input_text("##path", buf, sizeof buf, 0, hint, id)) path = trim(buf), changed = true;
    ImGui::SameLine();
    if (ImGui::Button("Browse...", ImVec2(bw, 0))) {
        std::string start = dialog_start(path.empty() ? home_dir() : path);
        if (folder) SDL_ShowOpenFolderDialog(picked, (void *)(intptr_t)target, ui_window(), start.c_str(), false);
        else {
            static const SDL_DialogFileFilter keys_filter[] = {{"Keys", "keys;txt"}, {"All files", "*"}};
            SDL_ShowOpenFileDialog(picked, (void *)(intptr_t)target, ui_window(), keys_filter, 2, start.c_str(), false);
        }
    }
    ImGui::PopID();
    return changed;
}

static void card_begin(const char *id, const char *num, const char *title, float s) {
    ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NavFlattened);
    ImGui::PushFont(g_font_big);
    if (*num) {
        ImGui::TextColored(GOLD, "%s", num);
        ImGui::SameLine();
    }
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    (void)s;
}

static bool big_button(const char *label, bool enabled, float w, float h) {
    ImGui::BeginDisabled(!enabled);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.80f, 0.55f, 0.12f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.92f, 0.66f, 0.20f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.98f, 0.74f, 0.28f, 1));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.10f, 0.07f, 0.02f, 1));
    ImGui::PushFont(g_font_big);
    bool r = ImGui::Button(label, ImVec2(w, h));
    ImGui::PopFont();
    ImGui::PopStyleColor(4);
    ImGui::EndDisabled();
    return r;
}

// ------------------------------------------------------------------ Build for iPhone / iPad (macOS: runtime/iosbuild.h)
#ifdef __APPLE__
// its own state and thread, apart from the setup job's (g_steps / g_job)
static pthread_mutex_t g_ios_lock = PTHREAD_MUTEX_INITIALIZER;
static std::atomic<int> g_ios_avail{0};  // 0 being checked, 1 yes, 2 no (g_ios_why), 3 no kit: no iPhone section
static std::atomic<int> g_ios_job{0};    // 0 idle, 1 running, 2 done (g_ios_out), 3 failed (g_ios_why), 4 cancelled
static std::atomic<float> g_ios_frac{0};
static std::string g_ios_why, g_ios_step, g_ios_out;
static bool g_ios_files = true;  // "Include the game files"

static void *ios_check_thread(void *) {  // (xcrun takes a moment: not on the UI thread)
    char why[300] = "";
    bool ok = iosbuild_available(why, sizeof why);
    pthread_mutex_lock(&g_ios_lock);
    g_ios_why = why;
    pthread_mutex_unlock(&g_ios_lock);
    g_ios_avail = ok ? 1 : strstr(why, "no iPhone kit") ? 3 : 2;
    return NULL;
}

static void ios_progress(const char *step, float frac) {
    pthread_mutex_lock(&g_ios_lock);
    g_ios_step = step;
    pthread_mutex_unlock(&g_ios_lock);
    g_ios_frac = frac;
}

static void *ios_build_thread(void *) {
    char out[1100], why[512];
    iosbuild_default_out(out, sizeof out);
    IosBuildOpts o = {g_ios_files, out, ios_progress};
    bool ok = iosbuild_run(rt_data_root(), &o, why, sizeof why);
    pthread_mutex_lock(&g_ios_lock);
    g_ios_out = out, g_ios_why = why;
    pthread_mutex_unlock(&g_ios_lock);
    g_ios_job = ok ? 2 : gamebuild_cancelled() ? 4 : 3;
    return NULL;
}

static void ios_start(void) {
    g_ios_frac = 0;
    ios_progress("Checking", 0);
    g_ios_job = 1;
    pthread_t th;
    if (pthread_create(&th, NULL, ios_build_thread, NULL) == 0) pthread_detach(th);
    else g_ios_job = 0;
}

static void reveal_in_finder(const std::string &path) {
    NSURL *u = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[ u ]];
}

// The card, under the game's (job 0 only). ready: the game is set up. Tests: RECOMP_LAUNCHER_IOS=show scrolls it into
// view, =build also presses the button once it can (as the player would), =cancel:<s> then presses Cancel <s>
// seconds later.
static void ios_card(bool ready, float s) {
    if (g_ios_avail == 3) return;  // (an app without the iOS kit: a source checkout's own build)
    ImGui::Spacing();
    card_begin("##c4", "", "iPhone and iPad", s);
    wrapped(DIM, "Make an app for your iPhone or iPad from your game. You sign and install it yourself (AltStore, "
                 "Sideloadly or Xcode).");
    pthread_mutex_lock(&g_ios_lock);
    std::string why = g_ios_why, step = g_ios_step, out = g_ios_out;
    pthread_mutex_unlock(&g_ios_lock);
    int job = g_ios_job, avail = g_ios_avail;
    const char *test = getenv("RECOMP_LAUNCHER_IOS");
    static double test_started;
    if (job == 1) {
        char pct[32];
        snprintf(pct, sizeof pct, "%.0f%%", g_ios_frac * 100);
        ImGui::TextColored(GOLD, "%s...", step.c_str());
        ImGui::ProgressBar(g_ios_frac, ImVec2(-1, 30 * s), pct);
        bool cancel = ImGui::Button("Cancel");
        if (test && !strncmp(test, "cancel:", 7) && SDL_GetTicks() / 1000.0 - test_started > atof(test + 7)) cancel = true;
        if (cancel) gamebuild_cancel();
        ImGui::EndChild();
        if (test) ImGui::SetScrollHereY(1.0f);
        return;
    }
    if (job == 2) {
        wrapped(GOOD, "Saved %s in %s.", base_name(out).c_str(),
                out.find("/Downloads/") != std::string::npos ? "Downloads" : out.substr(0, out.rfind('/')).c_str());
        wrapped(WARN, "It contains your game: don't share it.");
        if (ImGui::Button("Show in Finder")) reveal_in_finder(out);
        ImGui::SameLine();
    } else if (job == 3) {
        wrapped(BAD, "%s", why.c_str());
        if (ImGui::Button("Show log")) reveal_in_finder(std::string(rt_data_root()) + "/" GAME_ID "recomp.log");
        ImGui::SameLine();
    } else if (job == 4) {
        wrapped(DIM, "Cancelled.");
    }
    bool can = avail == 1 && ready;
    ImGui::Checkbox("Include the game files (about 3 GB)", &g_ios_files);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Without them the app is small, and you copy extracted/, fonts/ and sysdata/ into its "
                          "Documents folder yourself (Finder or the Files app).");
    ImGui::SameLine(0, 24 * s);
    ImGui::BeginDisabled(!can);
    bool go = ImGui::Button("Build for iPhone / iPad");
    ImGui::EndDisabled();
    const char *reason = avail == 0 ? "Checking for Xcode..." : avail == 2 ? why.c_str() : !ready ? "Set up the game first." : NULL;
    if (reason && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", reason);
    if (reason) wrapped(DIM, "%s", reason);
    if (can && test && job == 0 && (!strncmp(test, "build", 5) || !strncmp(test, "cancel", 6))) go = true;
    if (go) test_started = SDL_GetTicks() / 1000.0, ios_start();
    ImGui::EndChild();
    if (test) ImGui::SetScrollHereY(1.0f);  // (tests: the card in view for the screenshots)
}
#endif

// ------------------------------------------------------------------ the screen
extern "C" void launcher_run(const char *root) {
    g_root = root;
    config_load();
    // suggestions for what isn't chosen yet: the keys and firmware an emulator already has
    if (g_cfg.keys.empty())
        for (auto &k : keys_candidates()) if (file_exists(k)) { g_cfg.keys = k; break; }
    if (g_cfg.firmware.empty())
        for (auto &f : firmware_candidates()) if (dir_exists(f)) { g_cfg.firmware = f; break; }
    launcher_update_check_start();
#ifdef __APPLE__
    if (g_ios_avail == 0) {
        pthread_t th;
        if (pthread_create(&th, NULL, ios_check_thread, NULL) == 0) pthread_detach(th);
    }
#endif
    // the auto-fill of a test run
    const char *autof = getenv("RECOMP_LAUNCHER_AUTO");
    if (autof) {
        g_cfg.game.clear();
        std::string a = autof;
        for (size_t i = 0; i <= a.size();) {
            size_t e = a.find('|', i);
            if (e == std::string::npos) e = a.size();
            std::string kv = a.substr(i, e - i), k = kv.substr(0, kv.find('=')), v = kv.find('=') != std::string::npos ? kv.substr(kv.find('=') + 1) : "";
            if (k == "game") g_cfg.game.push_back(v);
            else if (k == "keys") g_cfg.keys = v;
            else if (k == "firmware") g_cfg.firmware = v;
            else if (k == "smm1") g_cfg.smm1 = v;
            i = e + 1;
        }
    }
    const char *shots = getenv("RECOMP_LAUNCHER_SHOTS");

    ui_window_init();
    SDL_Window *win = ui_window();
    SDL_SetWindowTitle(win, GAME_TITLE " - Setup");
#ifdef __APPLE__
    CAMetalLayer *layer = (__bridge CAMetalLayer *)ui_metal_layer();
    id<MTLDevice> dev = (__bridge id<MTLDevice>)ui_metal_device();
    id<MTLCommandQueue> queue = [dev newCommandQueue];
    if (shots) layer.framebufferOnly = NO;
#endif
    IMGUI_CHECKVERSION();
    ImGuiContext *prev = ImGui::GetCurrentContext();  // (the overlay menu's, after Quit to Start Screen)
    ImGuiContext *ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = NULL;
    {
        int ww, wh;
        SDL_GetWindowSizeInPixels(win, &ww, &wh);
        float atlas = wh > 1080 ? 2.0f : 1.0f;  // (sharp text on big / Retina windows)
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        const char *paths[] = {"/System/Library/Fonts/Avenir Next.ttc", "/System/Library/Fonts/Helvetica.ttc",
                               "C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                               "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
                               "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf"};
        g_font = g_font_big = g_font_title = NULL;
        for (const char *p : paths) {
            if (!file_exists(p)) continue;  // (a missing file is an assertion in ImGui)
            g_font = io.Fonts->AddFontFromFileTTF(p, 24 * atlas, &cfg);
            g_font_big = io.Fonts->AddFontFromFileTTF(p, 32 * atlas, &cfg);
            g_font_title = io.Fonts->AddFontFromFileTTF(p, 52 * atlas, &cfg);
            break;
        }
        if (!g_font) g_font = g_font_big = g_font_title = io.Fonts->AddFontDefault();
        io.FontGlobalScale = 1 / atlas;
    }
    style_init();
#ifdef __APPLE__
    ImGui_ImplSDL3_InitForMetal(win);
    ImGui_ImplMetal_Init(dev);
#else
    ImGui_ImplSDL3_InitForVulkan(win);
    vk_imgui_backend_init();
#endif
    ImGuiStyle base = ImGui::GetStyle();
    float style_scale = -1;

    bool ready_at_start = !launcher_needed(root, false);  // (back here from the game: everything is there)
    bool play = false, quit = false;
    int frame = 0, shot_n = 0, last_shot_key = -1;
    double last_shot_t = 0;
    g_job = 0;
    std::string add_path;
#ifdef __APPLE__
    bool quarantined = false;  // a downloaded app macOS hasn't cleared: its helpers may be refused (README: xattr)
    {
        char exe[1100];
        if (plat_exe_path(exe, sizeof exe)) quarantined = getxattr(exe, "com.apple.quarantine", NULL, 0, 0, 0) > 0;
    }
#endif

    while (!play) {
        // picks from the file dialogs
        pthread_mutex_lock(&g_lock);
        auto picks = g_picked;
        g_picked.clear();
        pthread_mutex_unlock(&g_lock);
        for (auto &p : picks) {
            if (p.first == PICK_GAME) g_cfg.game.push_back(p.second);
            if (p.first == PICK_KEYS) g_cfg.keys = p.second;
            if (p.first == PICK_FIRMWARE) g_cfg.firmware = p.second;
            if (p.first == PICK_SMM1) g_cfg.smm1 = p.second;
            config_save();
        }
        menu_apply_window_requests(win);
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) quit = true;
        }
        if (quit) {
#ifdef __APPLE__
            if (g_ios_job == 1) {  // the iPhone build: its compilers end and its work folder goes before the app does
                gamebuild_cancel();
                for (int i = 0; i < 300 && g_ios_job == 1; i++) SDL_Delay(50);
            }
#endif
            pthread_mutex_lock(&g_lock);
            g_cancel = true;
            if (g_child) plat_proc_kill(g_child), g_child = 0;
            pthread_mutex_unlock(&g_lock);
            LOG("window closed\n");
            exit(0);
        }
        if (g_job == 2) { play = true; break; }

#ifdef __APPLE__
        int pw, ph;
        SDL_GetWindowSizeInPixels(win, &pw, &ph);
        layer.drawableSize = CGSizeMake(pw, ph);
        @autoreleasepool {
            id<CAMetalDrawable> drawable = [layer nextDrawable];
            if (!drawable) { SDL_Delay(16); continue; }
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = drawable.texture;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0.04, 0.06, 0.07, 1);
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
            ImGui_ImplMetal_NewFrame(rp);
#else
        {
            // one frame per refresh: the swapchain presents without waiting, so this screen alone would spin
            static uint64_t next_frame;
            const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(win));
            uint64_t period = (uint64_t)(1e9 / (dm && dm->refresh_rate > 1 ? dm->refresh_rate : 60.0f)), now = plat_now_ns();
            bool on_time = next_frame > now && next_frame - now <= period;
            if (on_time) plat_sleep_until_ns(next_frame);
            next_frame = (on_time ? next_frame : now) + period;
            static const float clear_col[4] = {0.04f, 0.06f, 0.07f, 1};
            if (!vk_frame_begin(clear_col)) { SDL_Delay(16); continue; }
            ImGui_ImplVulkan_NewFrame();
#endif
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            ImVec2 ds = io.DisplaySize;
            float s = ds.y / 720.0f;
            if (s < 0.6f) s = 0.6f;
            if (fabsf(style_scale - s) > 0.01f) {
                ImGui::GetStyle() = base;
                ImGui::GetStyle().ScaleAllSizes(s);
                style_scale = s;
            }
            ImGui::GetBackgroundDrawList()->AddRectFilledMultiColor(ImVec2(0, 0), ds, IM_COL32(16, 36, 34, 255),
                                                                    IM_COL32(30, 62, 52, 255), IM_COL32(10, 18, 22, 255),
                                                                    IM_COL32(8, 12, 16, 255));
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ds);
            ImGui::Begin("##launcher", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
            ImGui::SetWindowFontScale(s);
            float contentw = fminf(ds.x - 2 * 28 * s, 1180 * s);
            float x0 = (ds.x - contentw) / 2;
            ImGui::SetCursorPosX(x0);
            ImGui::BeginGroup();
            ImGui::PushFont(g_font_title);
            ImGui::TextColored(GOLD, GAME_TITLE);
            ImGui::PopFont();
            ImGui::SameLine();
            ImGui::SetCursorPosX(x0 + contentw - ImGui::CalcTextSize(RECOMP_VERSION).x);
            ImGui::TextDisabled(RECOMP_VERSION);
            ImGui::PushTextWrapPos(x0 + contentw);
            ImGui::TextDisabled("Super Mario Maker 2 v3.0.3, recompiled on your computer from your own game. Nothing of "
                                "the game comes with this program: point it at your dump, and it does the rest.");
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
            ImGui::Spacing();

            float footer = 104 * s;
            ImGui::SetCursorPosX(x0);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
            ImGui::BeginChild("##body", ImVec2(contentw, ImGui::GetContentRegionAvail().y - footer), ImGuiChildFlags_None,
                              ImGuiWindowFlags_NavFlattened);
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.07f, 0.11f, 0.10f, 0.92f));
            int job = g_job;
            std::vector<Check> gchecks;
            for (auto &g : g_cfg.game) gchecks.push_back(cached_check('g', g, check_game_source));
            Check keys = cached_check('k', g_cfg.keys, check_keys), fw = cached_check('f', g_cfg.firmware, check_firmware),
                  sm = cached_check('s', g_cfg.smm1, check_smm1);
            bool games_ok = !gchecks.empty();
            for (auto &c : gchecks) games_ok &= c.ok;
            bool fonts_needed = !fonts_ok(), can_start = games_ok && (!fonts_needed || (keys.ok && fw.ok));
            bool files_ready = ready_at_start && g_cfg.game.empty() == false;
            if (job == 0) {
                // ---- 1. the game
                card_begin("##c1", "1", "Your game", s);
                wrapped(DIM, "Super Mario Maker 2 with the 3.0.3 update: the game's NSP or XCI and the update's NSP, or "
                             "folders with their extracted ExeFS and RomFS (e.g. an emulator's \"Extract RomFS / ExeFS\" "
                             "of the updated game). The version is checked against the known 3.0.3 files.");
                int remove_at = -1;
                for (size_t i = 0; i < g_cfg.game.size(); i++) {
                    // one line each: the check's mark, the name (the whole path on hover), what it is, Remove
                    ImGui::PushID((int)i);
                    const Check &c = gchecks[i];
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextColored(c.ok ? (c.warn ? WARN : GOOD) : BAD, "%s", c.ok ? (c.warn ? "!" : "OK") : "X");
                    ImGui::SameLine();
                    ImGui::TextUnformatted(base_name(g_cfg.game[i]).c_str());
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", g_cfg.game[i].c_str());
                    ImGui::SameLine();
                    ImGui::TextColored(c.ok ? DIM : BAD, "- %s", c.text.c_str());
                    float bw = 110 * s;
                    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - bw);
                    if (ImGui::Button("Remove", ImVec2(bw, 0))) remove_at = (int)i;
                    ImGui::PopID();
                }
                if (remove_at >= 0) g_cfg.game.erase(g_cfg.game.begin() + remove_at), config_save();
                if (g_cfg.game.empty()) wrapped(WARN, "Nothing chosen yet.");
                if (ImGui::Button("Add NSP / XCI...")) {
                    static const SDL_DialogFileFilter f[] = {{"Switch games", "nsp;xci"}, {"All files", "*"}};
                    std::string st = g_cfg.game.empty() ? home_dir() : dialog_start(g_cfg.game.back());
                    SDL_ShowOpenFileDialog(picked, (void *)(intptr_t)PICK_GAME, win, f, 2, st.c_str(), true);
                }
                ImGui::SameLine();
                if (ImGui::Button("Add folder...")) {
                    std::string st = g_cfg.game.empty() ? home_dir() : dialog_start(g_cfg.game.back());
                    SDL_ShowOpenFolderDialog(picked, (void *)(intptr_t)PICK_GAME, win, st.c_str(), true);
                }
                ImGui::SameLine();
                {
                    char buf[4096];
                    snprintf(buf, sizeof buf, "%s", add_path.c_str());
                    float aw = 120 * s;
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - aw - ImGui::GetStyle().ItemSpacing.x);
                    if (ui_input_text("##add", buf, sizeof buf, 0, "or type / paste a path", "Game files")) add_path = buf;
                    ImGui::SameLine();
                    ImGui::BeginDisabled(trim(add_path).empty());
                    if (ImGui::Button("Add", ImVec2(aw, 0))) g_cfg.game.push_back(trim(add_path)), add_path.clear(), config_save();
                    ImGui::EndDisabled();
                }
                ImGui::EndChild();
                ImGui::Spacing();

                // ---- 2. keys and firmware
                card_begin("##c2", "2", "Your keys and firmware", s);
                wrapped(DIM, "Your console's prod.keys and its firmware (the folder of its system .nca files): the game's "
                             "fonts come from the firmware, and it stops at the title screen without them. They're read "
                             "where they are and never copied; key values are never shown or written to the logs.%s",
                        fonts_needed ? "" : " (The fonts are already copied: only needed again for an NSP / XCI.)");
                if (path_row("prod.keys", g_cfg.keys, "prod.keys: its path", PICK_KEYS, false, s)) config_save();
                status_line(keys, 130 * s);
                if (path_row("Firmware", g_cfg.firmware, "firmware: e.g. Ryujinx's bis/system/Contents/registered", PICK_FIRMWARE, true, s))
                    config_save();
                status_line(fw, 130 * s);
                ImGui::EndChild();
                ImGui::Spacing();

                // ---- 3. SMM1
                card_begin("##c3", "3", "Super Mario Maker (Wii U), optional", s);
                wrapped(DIM, "Your own decrypted Super Mario Maker dump (the base game, and update v208 for Super "
                             "Expert), for the SMM1 mods: the 100 Mario Challenge and the costumes.");
                if (path_row("SMM1", g_cfg.smm1, "folder with the decrypted title(s)", PICK_SMM1, true, s)) config_save();
                status_line(sm, 130 * s);
                ImGui::EndChild();
#ifdef __APPLE__
                ios_card(game_files_ok(g_root) && !fonts_needed, s);
#endif
            } else {
                // ---- the steps
                card_begin("##run", "", job == 3 ? "Setup stopped" : "Setting up", s);
                pthread_mutex_lock(&g_lock);
                for (int i = 0; i < 3; i++) {
                    const Step &st = g_steps[i];
                    static const char *const mark[] = {"  ", "..", "OK", "OK", "X "};
                    ImVec4 col = st.state == FAILED ? BAD : st.state == RUNNING ? GOLD : st.state == PENDING ? DIM : GOOD;
                    ImGui::PushStyleColor(ImGuiCol_Text, col);
                    ImGui::Text("%s  %d. %s", mark[st.state], i + 1, st.name);
                    ImGui::PopStyleColor();
                    ImGui::Indent(48 * s);
                    if (!st.detail.empty()) wrapped(DIM, "%s", st.detail.c_str());
                    if (i == 0)  // (what setup.py found: the titles in an NSP / XCI, the version, the RomFS)
                        for (auto &f : g_found) wrapped(DIM, "%s", f.c_str());
                    ImGui::Unindent(48 * s);
                }
                std::string what = g_err_what, hint = g_err_hint, log = g_err_log;
                pthread_mutex_unlock(&g_lock);
                ImGui::Spacing();
                if (job == 1) {
                    char pct[32];
                    snprintf(pct, sizeof pct, "%.0f%%", g_progress * 100);
                    ImGui::ProgressBar(g_progress, ImVec2(-1, 34 * s), pct);
                    wrapped(DIM, "Recompiling takes from a few minutes to half an hour, depending on your computer. "
                                 "The game starts by itself when it's done; later starts skip all this.");
                } else {
                    ImGui::PushFont(g_font_big);
                    wrapped(BAD, "%s", what.c_str());
                    ImGui::PopFont();
                    wrapped(ImGui::GetStyle().Colors[ImGuiCol_Text], "%s", hint.c_str());
                    if (!log.empty()) wrapped(DIM, "Details: %s", log.c_str());
                    if (ImGui::Button("Back")) g_job = 0;
                }
                ImGui::EndChild();
            }
            ImGui::PopStyleColor();
            ImGui::EndChild();

            // ---- footer: the button, the update notice
            ImGui::Spacing();
            ImGui::SetCursorPosX(x0);
            ImGui::BeginGroup();
            float bh = 64 * s;
            if (job == 0) {
                const char *label = (files_ready || g_built_wait) && !fonts_needed ? "Play" : "Set up and play";
                bool ios_busy = false;
#ifdef __APPLE__
                ios_busy = g_ios_job == 1;  // (the two builds share the compilers and the cancel)
#endif
                bool go = big_button(label, (can_start || files_ready) && !ios_busy, 320 * s, bh);
                if (autof && frame == 45 && (can_start || files_ready) && !ios_busy) go = true;  // (test: as if pressed)
                if (go) {
                    config_save();
                    g_cancel = false;
                    g_job = 1;
                    pthread_t th;
                    if (pthread_create(&th, NULL, job_thread, NULL) == 0) pthread_detach(th);
                    else g_job = 0;
                }
                ImGui::SameLine(0, 24 * s);
                ImGui::BeginGroup();
                if (!can_start && !files_ready)
                    ImGui::TextColored(WARN, "%s", g_cfg.game.empty() ? "Choose your game files (1)."
                                                   : !games_ok ? "One of the game files can't be used (1)."
                                                               : "Choose prod.keys and the firmware (2): the fonts come from there.");
                else if (g_built_wait)
                    ImGui::TextColored(GOOD, "The game code is ready. Press Play (on a Steam Deck, Gaming Mode is best).");
                else
                    ImGui::TextColored(GOOD, "%s", files_ready && !fonts_needed ? "Everything is set up."
                                                   : "Ready to set up: the first time takes a while.");
                bool uc = g_cfg.update_check;
                if (ImGui::Checkbox("Check for updates", &uc)) {
                    g_cfg.update_check = uc, config_save();
                    if (uc) launcher_update_check_start();
                }
                const char *tag = launcher_update_tag();
                if (tag) {
                    ImGui::SameLine(0, 24 * s);
                    ImGui::TextColored(GOLD, "Version %s is out.", tag);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Open the download page")) SDL_OpenURL(RELEASES_PAGE);
                }
#ifdef __APPLE__
                if (quarantined) {
                    ImGui::SameLine();
                    ImGui::TextColored(WARN, "macOS may block the recompiler: see README (xattr).");
                }
#endif
                ImGui::EndGroup();
            } else if (job == 1) {
                ImGui::BeginDisabled(true);
                big_button("Setting up...", false, 320 * s, bh);
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextDisabled("Closing the window stops it; it starts over next time.");
            }
            ImGui::EndGroup();
            ImGui::End();
            osk_draw(g_font_big, s);
            ImGui::Render();

            // screenshots: whenever what's shown changes, and every 30 s while a step runs
            const char *shot = NULL;
            char shot_path[1200];
            if (shots) {
                int key = job * 100;
#ifdef __APPLE__
                key += g_ios_job * 1000 + g_ios_avail * 10000;
#endif
                pthread_mutex_lock(&g_lock);
                for (int i = 0; i < 3; i++) key += g_steps[i].state * (i == 0 ? 1 : i == 1 ? 5 : 25);
                pthread_mutex_unlock(&g_lock);
                double now = SDL_GetTicks() / 1000.0;
                if (frame >= 30 && (key != last_shot_key || (job == 1 && now - last_shot_t > 30))) {
                    last_shot_key = key, last_shot_t = now;
                    snprintf(shot_path, sizeof shot_path, "%s/launcher_%02d_%s.png", shots, shot_n++,
                             job == 0 ? "choose" : job == 1 ? "running" : job == 3 ? "error" : "done");
                    shot = shot_path;
                }
            }
#ifdef __APPLE__
            ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cb, enc);
            [enc endEncoding];
            id<MTLTexture> copy = nil;
            if (shot) {
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:drawable.texture.pixelFormat
                                             width:drawable.texture.width height:drawable.texture.height mipmapped:NO];
                td.storageMode = MTLStorageModeShared;
                copy = [dev newTextureWithDescriptor:td];
                id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
                [bl copyFromTexture:drawable.texture toTexture:copy];
                [bl endEncoding];
            }
            [cb presentDrawable:drawable];
            [cb commit];
            if (copy) {
                [cb waitUntilCompleted];
                save_png(copy, shot);
                LOG("[launcher] screenshot %s\n", shot);
            }
#else
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)vk_frame_cmd());
            vk_frame_end(shot);
            if (shot) LOG("[launcher] screenshot %s\n", shot);
#endif
        }
        frame++;
    }
    LOG("[launcher] ready: starting the game\n");
    config_save();
    launcher_boot_env();
#ifdef __APPLE__
    ImGui_ImplMetal_Shutdown();
#else
    vk_imgui_backend_shutdown();
#endif
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev);
    SDL_SetWindowTitle(win, GAME_TITLE);
}
#endif
