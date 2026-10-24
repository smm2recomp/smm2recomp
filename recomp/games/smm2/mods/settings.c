// Mod settings (games/smm2/mods/mod.h) and start-up: environment variables win over smm2mods.ini (key=value lines
// in the data folder, next to larecomp.ini). Nothing is on unless asked for.
//
//   RECOMP_MOD_100MARIO=1        hundred=1          100 Mario Challenge on SMM2's Endless Challenge
//   RECOMP_MOD_SERVER=<url>      server=<url>       our course server, plain http (default http://127.0.0.1:8080)
//   RECOMP_MOD_TOKEN=<token>     token=<token>      the account's token (`mmk register NAME --server URL`)
//   RECOMP_MOD_RULESET=<name>    ruleset=<name>     hundred (SMM1's 100 lives, 8/16/16/6 clears; default) or smm2
//   RECOMP_MOD_TIMEOUT_MS=<n>    timeout_ms=<n>     per request (default 8000)
//   RECOMP_MOD_LOG=1             log=1              log requests and overrides
//   RECOMP_MOD_COURSES=<src>     courses=<src>      server (the server above) or tgrcode (a server built into the
//                                                   game, fed from TGRCode's archive of SMM2 courses; iOS default;
//                                                   hybrid's default)
//   RECOMP_MOD_COURSE_WORLD=<w>  course_world=<w>   with another backend than ours: mod (the mod answers Course
//                                                   World, the Endless Challenge and 100 Mario while the game's NEX
//                                                   client logs in to the backend: hybrid; nextendo's default) or
//                                                   backend (all of Course World from the backend; hybrid.h)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>

#include "mod.h"
#include "../../../runtime/game_rows.h"
#include "hybrid.h"
#include "../../../runtime/net/netcfg.h"
#include "../../../runtime/net/nextendo.h"
#include "../../../runtime/net/online.h"

ModSettings g_mod;
static volatile bool smm2_session;  // Challenge Modes is on SMM2's Endless Challenge (bridge.c)

static void set(const char *key, const char *val) {
    if (!strcmp(key, "hundred")) g_mod.hundred = atoi(val) != 0;
    else if (!strcmp(key, "courses"))
        snprintf(g_mod.courses, sizeof g_mod.courses, "%s", val), g_mod.courses_set = true;
    else if (!strcmp(key, "course_world")) snprintf(g_mod.course_world, sizeof g_mod.course_world, "%s", val);
    else if (!strcmp(key, "server")) snprintf(g_mod.server, sizeof g_mod.server, "%s", val);
    else if (!strcmp(key, "token")) snprintf(g_mod.token, sizeof g_mod.token, "%s", val);
    else if (!strcmp(key, "ruleset")) snprintf(g_mod.ruleset, sizeof g_mod.ruleset, "%s", val);
    else if (!strcmp(key, "timeout_ms")) g_mod.timeout_ms = atoi(val);
    else if (!strcmp(key, "log")) g_mod.log = atoi(val) != 0;
    else if (!strcmp(key, "challenge_mode")) g_mod.mode_endless = !strcmp(val, "endless");
}

void mod_settings_load(void) {
    static int done;
    if (done) return;
    done = 1;
    snprintf(g_mod.server, sizeof g_mod.server, "http://127.0.0.1:8080");
    snprintf(g_mod.ruleset, sizeof g_mod.ruleset, "hundred");
    // Online (Course World, Challenge Modes) with courses from TGRCode through the built-in server unless the ini says
    // otherwise, on every platform: without an smm2mods.ini (a new player's data folder) the desktop builds used to
    // leave the mod off, and Course World then ran SMM2's own sign-in, asked for a Mii and fell back to the title
    // (user reports, 2026-10-22; iOS had these defaults already)
    g_mod.hundred = true;
    snprintf(g_mod.courses, sizeof g_mod.courses, "tgrcode");
    g_mod.timeout_ms = 8000;
    FILE *f = fopen(getenv("RECOMP_MOD_INI") ? getenv("RECOMP_MOD_INI") : "smm2mods.ini", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            char *eq = strchr(line, '='), *e;
            if (line[0] == '#' || !eq) continue;
            *eq = 0;
            for (e = eq + 1 + strlen(eq + 1); e > eq + 1 && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '); ) *--e = 0;
            set(line, eq + 1);
        }
        fclose(f);
    }
    static const struct { const char *env, *key; } envs[] = {
        {"RECOMP_MOD_100MARIO", "hundred"}, {"RECOMP_MOD_SERVER", "server"}, {"RECOMP_MOD_TOKEN", "token"},
        {"RECOMP_MOD_RULESET", "ruleset"},  {"RECOMP_MOD_TIMEOUT_MS", "timeout_ms"}, {"RECOMP_MOD_LOG", "log"},
        {"RECOMP_MOD_COURSES", "courses"},  {"RECOMP_MOD_COURSE_WORLD", "course_world"},
    };
    for (size_t i = 0; i < sizeof envs / sizeof *envs; i++)
        if (getenv(envs[i].env)) set(envs[i].key, getenv(envs[i].env));
    size_t n = strlen(g_mod.server);
    while (n && g_mod.server[n - 1] == '/') g_mod.server[--n] = 0;
    if (strcmp(g_mod.ruleset, "smm2") && strcmp(g_mod.ruleset, "hundred")) snprintf(g_mod.ruleset, sizeof g_mod.ruleset, "hundred");
    if (g_mod.timeout_ms <= 0) g_mod.timeout_ms = 8000;
    if (strcmp(g_mod.courses, "tgrcode")) snprintf(g_mod.courses, sizeof g_mod.courses, "server");
    smm2_session = g_mod.mode_endless;  // (the mode used last: its rules from the first menu request on)
    HyMode m = hy_mode(net_backend(), g_mod.hundred, g_mod.course_world);
    if (m.hybrid && !g_mod.courses_set) {  // (hybrid: no account on a server of ours needed)
        snprintf(g_mod.courses, sizeof g_mod.courses, "tgrcode");
        LOG("[mod] hybrid: courses from TGRCode (courses= not set)\n");
    }
    if (m.hybrid)
        LOG("[mod] hybrid: the game logs in to the %s backend (Network Play); Course World, the Endless Challenge and "
            "100 Mario from the mod\n", net_backend_key(net_backend()));
    if (m.world && !strcmp(g_mod.courses, "tgrcode"))
        LOG("[mod] 100 Mario Challenge on: courses from TGRCode (built-in server), ruleset %s\n", g_mod.ruleset);
    else if (m.world)
        LOG("[mod] 100 Mario Challenge on: server %s, ruleset %s, %s\n", g_mod.server, g_mod.ruleset,
            g_mod.token[0] ? "signed in" : "NO TOKEN (set RECOMP_MOD_TOKEN)");
}

// ---- courses=tgrcode: the course server built into the app (core/crates/embed, runtime/net/embed_server.h; it
// returns false when the build doesn't link it), on a loopback port with a local account, fed from TGRCode's course
// archive (it finds the course key table in the loaded game itself). The same server the runtime starts at boot
// on iOS (embed_server_boot: one server per process, a second start returns the first's address). If it can't
// start, the mod falls back to server= / token=.
#include "../../../runtime/net/embed_server.h"

static void start_builtin_server(void) {
    char url[256], token[160], nx_name[64];
    const char *name = getenv("RECOMP_USER_NAME") ? getenv("RECOMP_USER_NAME") : "Player";
    // (nextendo: the local account is named after the Nextendo account, the name Course World shows for the player)
    if (!getenv("RECOMP_USER_NAME") && net_backend() == NET_BACKEND_NEXTENDO &&
        nextendo_username(nx_name, sizeof nx_name) && nx_name[0])
        name = nx_name;
    if (embed_server_start("server", "tgrcode", name, url, sizeof url, token, sizeof token)) {
        snprintf(g_mod.server, sizeof g_mod.server, "%s", url);
        snprintf(g_mod.token, sizeof g_mod.token, "%s", token);
        LOG("[mod] built-in course server on %s (courses from TGRCode)\n", url);
    } else {
        LOG("[mod] the built-in course server didn't start; using server=%s\n", g_mod.server);
    }
}

void mod_server_ready(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    mod_settings_load();
    if (!strcmp(g_mod.courses, "tgrcode")) pthread_once(&once, start_builtin_server);
}

// Who answers what, per online backend (hybrid.h, backend.c): with ours the mod answers everything (hundred=1) or
// nothing; with nextendo the game's own NEX client logs in and the mod answers Course World (hybrid) unless
// course_world=backend; with local-nex only course_world=mod makes it hybrid.
static HyMode mode(void) { mod_settings_load(); return hy_mode(net_backend(), g_mod.hundred, g_mod.course_world); }
bool mod_hundred(void) { return mode().world; }
bool mod_hybrid(void) { return mode().hybrid; }
bool mod_answers_login(void) { return mode().login; }
// Challenge Modes: with 100 Mario offered (ruleset=hundred) Course World's Endless Challenge button (relabelled,
// relabel.c) opens the mode used last: SMM1's 100 Mario Challenge or SMM2's own Endless Challenge; ZL / ZR switch
// (bridge.c). SMM1's rules apply only in the 100 Mario part; the session flag says the player is in SMM2's.
bool mod_hundred_offered(void) { return mod_hundred() && !strcmp(g_mod.ruleset, "hundred"); }
bool mod_hundred_rules(void) { return mod_hundred_offered() && !mod_smm2_session(); }
bool mod_smm2_session(void) { mod_settings_load(); return smm2_session; }
void mod_set_smm2_session(bool on) {
    if (on == smm2_session) return;
    smm2_session = on;
    extern void endless_apply_mode(void);  // endless.c: the run records as this mode sees them (start lives too)
    endless_apply_mode();
    LOG("[mod] %s\n", on ? "SMM2's own Endless Challenge (SMM2's rules)" : "back to the 100 Mario Challenge");
}

// The system services report a connection while a mod answers the login (runtime/net/online.h; in hybrid the
// backend's real connection does).
bool net_pretend_online(void) { return mod_answers_login() || (getenv("RECOMP_PRETEND_ONLINE") && atoi(getenv("RECOMP_PRETEND_ONLINE"))); }

// ---- the overlay menu's Mods tab (runtime/ui/menu.mm): one switch per SMM1 feature, kept in smm2mods.ini. Only
// "100 Mario Challenge (SMM1)" so far (on: Course World's Endless Challenge button opens SMM1's 100 Mario Challenge;
// off: SMM2's own Endless Challenge, against our server under SMM2's rules); costumes, mid-air shell jumps and
// overlapping pipes will follow.
typedef struct { const char *label, *desc; bool *value; bool def; } GameModToggle;
static bool t_hundred = true, t_tgrcode;
static bool t_costumes;  // (mods/costume.c)
bool costumes_setting(void);
void costumes_set_setting(bool on);

void mod_ini_set(const char *key, const char *val) {  // replace or add one key=value line (also backend.c)
    const char *path = getenv("RECOMP_MOD_INI") ? getenv("RECOMP_MOD_INI") : "smm2mods.ini";
    char lines[64][512];
    int n = 0;
    bool done = false;
    FILE *f = fopen(path, "r");
    if (f) {
        while (n < 64 && fgets(lines[n], sizeof lines[n], f)) {
            size_t kl = strlen(key);
            if (!strncmp(lines[n], key, kl) && lines[n][kl] == '=') snprintf(lines[n], sizeof lines[n], "%s=%s\n", key, val), done = true;
            n++;
        }
        fclose(f);
    }
    if (!done && n < 64) snprintf(lines[n++], sizeof lines[0], "%s=%s\n", key, val);
    if ((f = fopen(path, "w"))) {
        for (int i = 0; i < n; i++) fputs(lines[i], f);
        fclose(f);
    }
}

void mod_remember_mode(bool endless) {
    mod_settings_load();
    if (g_mod.mode_endless == endless) return;
    g_mod.mode_endless = endless;
    mod_ini_set("challenge_mode", endless ? "endless" : "hundred");
}

static int mod_toggles(const GameModToggle **out) {
    static const GameModToggle toggles[] = {
        {"100 Mario Challenge (SMM1)",
         "Course World's Endless Challenge opens SMM1's 100 Mario Challenge: 100 lives, 8 / 16 / 16 / 6 courses, "
         "SMM1's rules and screens; B there leads on to SMM2's own Endless Challenge. Off: the button opens SMM2's "
         "own Endless Challenge only. Both play courses from your course server.",
         &t_hundred, true},
        {"Courses: TGRCode (off: my server)",
         "On: courses come from TGRCode's archive of SMM2 courses, through a course server built into the game "
         "(no account needed). Off: the course server set in smm2mods.ini (server=, token=). Takes effect at the "
         "next start.",
         &t_tgrcode, false},
        // (the backend, incl. OpenCourse World, is the Online section's: runtime/ui/online_rows.mm. OCW's domain
        // and token are its own rows: mods/ocw.c game_mod_rows)
        // (costumes: mods/costume.c)
        {"Mystery Mushroom costumes",
         "SMM1's costumes in Super Mario Bros. style courses: the Big Mushroom becomes the Mystery Mushroom, and a "
         "won 100 Mario Challenge gives a new costume. Models come from your SMM1 dump. Takes effect at the next start.",
         &t_costumes, false},
    };
    // (the mod is off and the backend is ours: nothing to switch)
    mod_settings_load();
    if (!g_mod.hundred && net_backend_setting() == NET_BACKEND_OURS) { *out = NULL; return 0; }
    t_hundred = !strcmp(g_mod.ruleset, "hundred");
    t_tgrcode = !strcmp(g_mod.courses, "tgrcode");
    t_costumes = costumes_setting();
    *out = toggles;
    return (int)(sizeof toggles / sizeof *toggles);
}

static void mod_toggles_changed(void) {
    snprintf(g_mod.ruleset, sizeof g_mod.ruleset, "%s", t_hundred ? "hundred" : "smm2");
    mod_ini_set("ruleset", g_mod.ruleset);
    LOG("[mod] 100 Mario Challenge (SMM1) %s\n", t_hundred ? "on" : "off");
    const char *courses = t_tgrcode ? "tgrcode" : "server";
    if (strcmp(courses, g_mod.courses)) {  // (the server in use stays until the next start)
        mod_ini_set("courses", courses);
        LOG("[mod] courses from %s at the next start\n", t_tgrcode ? "TGRCode" : "your server");
    }

    costumes_set_setting(t_costumes);
}

// The tab: the switches above (when there is something to switch), then the SMM1 extras' (smm1_extras.c: one per SMM1
// feature, always there), then the SMW goal wipe's (smw_goal.c), the Muncher hit box's (muncher_hitbox.c), quick level return's
// (quick_retry.c), co-op's (coop.c) and Lanky Mario's (lanky.c).
int smm1_extras_toggles(GameModToggle *out, int room);
void smm1_extras_toggles_changed(void);
int smw_goal_toggles(GameModToggle *out, int room);
void smw_goal_toggles_changed(void);
int muncher_hitbox_toggles(GameModToggle *out, int room);
void muncher_hitbox_toggles_changed(void);
int quick_retry_toggles(GameModToggle *out, int room);
void quick_retry_toggles_changed(void);
int coop_toggles(GameModToggle *out, int room);
void coop_toggles_changed(void);
int lanky_toggles(GameModToggle *out, int room);
void lanky_toggles_changed(void);
int relax_toggles(GameModToggle *out, int room);
void relax_toggles_changed(void);
// OpenCourse World's own section, with its domain and token (mods/ocw.c): not switches, so it goes through the
// GameModRow hook instead (runtime/game_rows.h)
int ocw_rows(GameModRow *out, int room);
void ocw_rows_changed(void);
static int own_shown;  // (the tab showed the switches above: apply them on a change)
int game_mod_toggles(const GameModToggle **out) {
    static GameModToggle all[24];  // (each mod below is given the room that is left)
    const GameModToggle *own;
    int n = own_shown = mod_toggles(&own);
    for (int i = 0; i < n; i++) all[i] = own[i];
    n += smm1_extras_toggles(all + n, (int)(sizeof all / sizeof *all) - n);
    n += smw_goal_toggles(all + n, (int)(sizeof all / sizeof *all) - n);
    n += muncher_hitbox_toggles(all + n, (int)(sizeof all / sizeof *all) - n);
    n += quick_retry_toggles(all + n, (int)(sizeof all / sizeof *all) - n);
    n += coop_toggles(all + n, (int)(sizeof all / sizeof *all) - n);
    n += lanky_toggles(all + n, (int)(sizeof all / sizeof *all) - n);
    n += relax_toggles(all + n, (int)(sizeof all / sizeof *all) - n);
    *out = n ? all : NULL;
    return n;
}

// The non-switch rows of the same tab (runtime/game_hooks.c): OpenCourse World's section only, so far.
int game_mod_rows(const GameModRow **out) {
    static GameModRow all[8];
    int n = ocw_rows(all, (int)(sizeof all / sizeof *all));
    *out = n ? all : NULL;
    return n;
}

void game_mod_rows_changed(void) { ocw_rows_changed(); }

void game_mod_toggles_changed(void) {
    if (own_shown) mod_toggles_changed();
    smm1_extras_toggles_changed();
    smw_goal_toggles_changed();
    muncher_hitbox_toggles_changed();
    quick_retry_toggles_changed();
    coop_toggles_changed();
    lanky_toggles_changed();
    relax_toggles_changed();
}

// Overrides are registered before the game's first instruction runs (dispatch reads them on every first call). Each
// one checks the settings when it is called (they are read then, from the data folder) and runs the game's own
// function when its mod is off.
__attribute__((constructor)) static void mods_start(void) {
    extern void world_register(void);
    world_register();  // (first: the first override of an address wins, before the trace wrappers)
    extern void superworld_register(void);
    superworld_register();
    online_register();
    endless_register();
    extern void bridge_register(void);
    bridge_register();
}
