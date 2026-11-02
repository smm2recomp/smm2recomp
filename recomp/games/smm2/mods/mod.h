// SMM2 v3.0.3 mods: shared declarations (games/smm2/mods/*.c).
//
// The 100 Mario Challenge mod keeps SMM2's Endless Challenge (its screens, run logic, HUD) and answers the game's
// Course World network calls from our own server (core/crates/server: /api/endless, /api/courses) instead of
// Nintendo's. The calls are replaced one level above the network library: the game's own request functions
// ("wrappers", each reached through a function pointer) get host overrides (hle_override) that talk HTTP and write
// the plain records the game's callbacks read. Addresses are v3.0.3; addresses are offsets in main (MAIN_ADDR).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../../runtime/hle.h"

#define MAIN_ADDR(off) (MAIN_BASE + (off))  // an offset in main -> its address (runtime/rt.h: moves on iOS devices)

// ---- settings (settings.c): environment variables, else smm2mods.ini in the data folder
typedef struct {
    bool hundred;        // RECOMP_MOD_100MARIO / hundred=1: the 100 Mario Challenge mod is on
    char server[256];    // RECOMP_MOD_SERVER / server=: our server's base URL ("http://127.0.0.1:8080")
    char token[160];     // RECOMP_MOD_TOKEN / token=: the account token (`mmk register`)
    char ruleset[16];    // RECOMP_MOD_RULESET / ruleset=: "hundred" (SMM1's rules, default) or "smm2"
    char courses[16];    // RECOMP_MOD_COURSES / courses=: "server" (server= above) or "tgrcode" (a server built into the
                         // game, fed from TGRCode's course archive; iOS default)
    int timeout_ms;      // RECOMP_MOD_TIMEOUT_MS / timeout_ms=: per HTTP request (default 8000)
    bool log;            // RECOMP_MOD_LOG / log=1: log every request and override
    bool mode_endless;   // challenge_mode=endless: Challenge Modes opens on SMM2's Endless Challenge (the last one used)
    char course_world[16];  // RECOMP_MOD_COURSE_WORLD / course_world=: with another backend than ours, who answers
                            // Course World: "mod" or "backend" ("" unset: nextendo -> the mod; hybrid.h)
    bool courses_set;    // courses= was given (ini or environment); unset, hybrid takes its courses from TGRCode
} ModSettings;
extern ModSettings g_mod;
void mod_settings_load(void);   // once (idempotent)
void mod_server_ready(void);    // before the first request: with courses=tgrcode, start the built-in server (once)
bool mod_hundred(void);         // the mod answers Course World (the 100 Mario mod is on; hybrid too)
bool mod_endless(void);         // the mod answers the Endless Challenge / 100 Mario runs (Course World, or ocw: those alone)
bool mod_local_runs(void);      // ... and the runs are kept in the game (backend ocw: mods/ocw_runs.c), not on a server
bool mod_hybrid(void);          // ... while the game's own NEX client is logged in to the backend (hybrid.h)
bool mod_answers_login(void);   // the mod answers the login too (backend ours: no NEX connection)
bool mod_hundred_offered(void); // ... and 100 Mario is offered (ruleset "hundred": the Endless button opens it)
bool mod_hundred_rules(void);   // ... and the player is in the 100 Mario part now: SMM1's rules
bool mod_smm2_session(void);    // Challenge Modes is on SMM2's own Endless Challenge (SMM2's rules)
void mod_set_smm2_session(bool on);
void mod_remember_mode(bool endless);  // Challenge Modes' mode, kept in smm2mods.ini (challenge_mode=)
#define MLOG(...) do { if (g_mod.log) LOG("[mod] " __VA_ARGS__); } while (0)

// ---- server calls (server.c): JSON over HTTP; NULL on any failure (logged)
typedef struct Json Json;
Json *srv_call(const char *method, const char *path, const char *body, int *status);
Json *srv_call_slow(const char *method, const char *path, const char *body, int *status, int timeout_ms);
void *srv_get_bytes(const char *path, size_t *len);  // raw body (course data); malloc'd
void *srv_get_bytes_slow(const char *path, size_t *len, int timeout_ms);  // ... the server may wait on (?wait_ms=)
void *srv_get_bytes_status(const char *path, size_t *len, int *status);  // ... and the status (202: coming)
void srv_quiet_thread(void);  // this thread's answers aren't logged (a host thread asking often)
Json *srv_post_bytes(const char *path, const void *data, size_t len, int *status);  // raw body up (a course)
Json *srv_send_bytes(const char *method, const char *path, const void *data, size_t len, int *status);  // ... PUT

// ---- game state the overrides share
uint64_t netobj(void);          // the network-session object (requests and answers of the wrappers)
uint8_t wrapper_ok(void);       // the byte a wrapper returns on success / failure (game globals)
uint8_t wrapper_fail(void);
void netobj_lock(Cpu *c);       // the lock the original wrappers take around their writes
void netobj_unlock(Cpu *c);
// guest helpers
static inline uint8_t g8(uint64_t a) { return *(volatile uint8_t *)(uintptr_t)a; }
static inline uint32_t g32(uint64_t a) { return *(volatile uint32_t *)(uintptr_t)a; }
static inline uint64_t g64(uint64_t a) { return *(volatile uint64_t *)(uintptr_t)a; }
static inline void s8(uint64_t a, uint8_t v) { *(volatile uint8_t *)(uintptr_t)a = v; }
static inline void s16(uint64_t a, uint16_t v) { *(volatile uint16_t *)(uintptr_t)a = v; }
static inline void s32(uint64_t a, uint32_t v) { *(volatile uint32_t *)(uintptr_t)a = v; }
static inline void s64(uint64_t a, uint64_t v) { *(volatile uint64_t *)(uintptr_t)a = v; }
// call a recompiled guest function directly (call-through for an overridden address: dispatch would come back here)
void guest_call_orig(Cpu *c, uint64_t addr);

// ---- per-mod registration (called once at start-up when the mod is on)
void online_register(void);     // online.c: pretend-online login and default "not connected" wrappers
void endless_register(void);    // endless.c: the Endless Challenge wrappers against our server
int coop_players(void);         // coop.c: players for an Endless / 100 Mario course (1 without co-op)
void coop_course(bool on);       // coop.c (hooks.c): the course starting is played in co-op, or not
bool coop_in_course(void);      // coop.c: the course being played is in co-op
int coop_players_standing(void); // coop.c: players in the course now who are neither down nor in a bubble
int endless_course_style(void);  // endless.c: a run's current course style (0 SMB1 .. 4 3D World), -1 none, -2 unknown
void endless_note_course(uint64_t data_id, const char *code);  // course.c -> endless.c: which code a data id is
// hybrid (online.c -> endless.c): the player id the backend's own record gives the player; from then on the player's
// own id everywhere (own courses, "me"). A secret: never logged.
void mod_set_backend_pid(uint64_t pid);

// ---- the overlay menu's "Try it now" rows (triggers.c; the type is runtime/game_hooks.c's): a mod adds its own from a
// constructor. available() and trigger() are called on the UI thread: trigger() only records a request, the mod
// applies it on the game's thread.
typedef struct {
    const char *label, *desc;
    bool (*available)(void);
    void (*trigger)(int choice);
    const char *choice_label;
    const char *const *choices;
    int nchoices;
    const char *(*why)(void);  // greyed out: a few words on why (shown on the row; NULL or "": none)
} GameModTrigger;
GameModTrigger *mod_trigger_add(const GameModTrigger *t);  // the stored row (its choices may be filled in later)

// ---- the player model holder's builder and visual slot setter (player_model.c, shared by costume.c and lanky.c: an
// address takes one override)
typedef struct { uint64_t addr, val, was; } PmSwap;  // a guest pointer set to val for the builder's call
// a guest function called from the player's update (games/smm2/trace.c: after its prologue only x0 and the argument
// registers are live): the caller-saved state is put back after; x8 where a returned struct goes. Its x0.
uint64_t pm_call(Cpu *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t x8);
bool smb1_course(void);          // costume.c: the course being played or edited is SMB1-style
bool lanky_on(void);             // lanky.c: Lanky Mario is on (the switch at start-up)
bool bridge_endless_course(void);  // bridge.c: the course being played is an Endless / 100 Mario run's

// ---- course codes (course.c)
bool code_to_data_id(const char *code, uint32_t *id);
void data_id_to_code(uint32_t id, char out[12]);
