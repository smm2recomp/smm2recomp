// The Endless Challenge's network requests answered by our server (games/smm2/mods/mod.h).
//
// Each request function ("wrapper", this = the network-session object NetObj) reads its parameters from NetObj,
// asks the server, writes the answer where the game's own code would (the run records, the history rings, the
// course list), sets the status word +0x60 (0 ok, 1 not connected, 2 error) and returns the game's success or failure
// byte. They run on the game's network worker thread, which waits for them, so the HTTP calls block there. Every
// write is made under the lock the originals take (NetObj + 0x20).
//
// Server routes (core/crates/server): GET /api/endless, POST /api/endless/<d>/start | result | suspend | finish,
// GET /api/endless/<d>/next?count=10 | history, GET /api/rankings/<board>, GET /api/me, GET /api/courses/<code>/data.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mod.h"
#include "../../../runtime/net/json.h"
#include "course_preload.h"
#include "hundred.h"
#include <unistd.h>

void bridge_event(const char *what, uint32_t d);  // bridge.c
bool endless_run_complete(uint32_t d);
int style_index(const char *s);
bool bridge_take_start_over(void);  // bridge.c: the life being posted is a Start Over from the pause
void endless_prefetch(int d);

static const char *const DIFF[4] = {"easy", "normal", "expert", "super_expert"};

// NetObj fields (v3.0.3)
#define NO_STATUS 0x60
#define NO_RECORDS_COUNT 0xe9970   // u32: run records (one per difficulty, 0x38 bytes) ...
#define NO_RECORDS 0xe9978         // ... and their array
#define NO_HISTORY 0xe9980         // per difficulty (stride 0x498): entries, +8 capacity, +0xc head, +0x10 count
#define HISTORY_STRIDE 0x498
#define NO_DIFF 0xeabe0            // u32 difficulty of the request
#define NO_CLEAR_ID 0xeabe8        // u64 course data id (clear report)
#define NO_PENDING 0xeabf4         // u8 pending 1-Ups (clear report)
#define NO_COINS 0xeabf5           // u8 coins (clear report)
#define NO_SCORE 0xeabf8           // u32 score with the time bonus (clear report)
#define NO_RANK 0xeac00            // u32 global rank (answer)
#define NO_COURSE_ID 0xeac08       // u64 course data id (start / skip)
#define NO_STYLE 0xeac10           // u8 style, u8 theme
#define NO_SEARCH_TYPE 0x746e8     // u32 search type (0xE: Endless)
#define NO_SEARCH_DIFF 0x746ec     // u32 difficulty of the search
#define NO_COURSES 0x30780         // course list: entries (0x548 bytes each), +8 capacity, +0xc head, +0x10 count
#define COURSE_ENTRY 0x548
#define NO_DL_BUFFER 0x8e5e8       // ptr: where a downloaded course file goes (0x5c000 bytes)
#define NO_DL_ID 0x8e5f0           // u64: data id of the course to download

// ---- the signed-in account and its runs

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    uint32_t coins, score;   // what the server holds for the run (the clear report sends deltas)
    uint64_t started;        // data id of the course the run is on (StartCourse seen, no result yet)
    bool complete;           // the server said the run reached its goal (100 Mario)
    bool active;             // the server's run: can go on / lives / clears / goal (0: endless)
    uint32_t lives, clears, goal;
    uint64_t current;        // the course being (or about to be) played
    uint64_t last_done;      // the last course cleared (its play result comes after)
    int ncredits;            // the run's cleared courses (for the ending's credits)
    struct hundred_look look;  // the players at the last goal (the ending's rescue scene)
    struct hundred_credit credits[32];
} runs[4];

static volatile int last_search_d = -1;  // the difficulty of the last Endless course list

// Names and makers of the courses served (data id -> credit), for the ending's credits.
static struct { uint64_t id; struct hundred_credit c; int style, theme; } course_names[64];
static int ncourse_names;

static void note_course(uint64_t id, const char *name, const char *maker, int style, int theme) {
    pthread_mutex_lock(&state_lock);
    int i = 0;
    while (i < ncourse_names && course_names[i].id != id) i++;
    if (i == ncourse_names) i = ncourse_names < 64 ? ncourse_names++ : (int)(id % 64);
    course_names[i].id = id;
    course_names[i].style = style, course_names[i].theme = theme;
    snprintf(course_names[i].c.name, sizeof course_names[i].c.name, "%s", name);
    snprintf(course_names[i].c.maker, sizeof course_names[i].c.maker, "%s", maker);
    pthread_mutex_unlock(&state_lock);
}

static void add_credit(uint32_t d, uint64_t id) {  // (state_lock held)
    if (runs[d].ncredits >= 32) return;
    struct hundred_credit *c = &runs[d].credits[runs[d].ncredits++];
    snprintf(c->name, sizeof c->name, "?");
    c->maker[0] = 0;
    for (int i = 0; i < ncourse_names; i++)
        if (course_names[i].id == id) *c = course_names[i].c;
    data_id_to_code((uint32_t)id, c->code);
}

static char own_maker_id[16];
static uint64_t pid_of_maker_id(const char *maker_id);
static volatile uint64_t backend_pid;  // hybrid: the player id in the backend's own record of the player (secret)

void mod_set_backend_pid(uint64_t pid) {
    // Hybrid (online.c GetUsers (own)): the game's NEX client is logged in, and its own record of the player carries
    // the backend's player id. Course World compares it with the owners of courses ("my course") and the rows of
    // lists ("me"), so the player's own maker on our server gets that id too (pid_of_maker_id). Never logged.
    if (mod_hybrid() && pid) backend_pid = pid;
}

uint64_t mod_player_pid(void) {
    // The signed-in account's player id: the account id the game holds (acc_account_id; Course World compares course
    // owners and user records with it: own courses, "me"), once /api/me named the own maker. Hybrid: the backend's id,
    // once known.
    static volatile int known;
    if (!known) {
        Json *me = srv_call("GET", "/api/me", NULL, NULL);
        const char *id = json_str(json_get(me, "maker_id"), NULL);
        if (id) snprintf(own_maker_id, sizeof own_maker_id, "%s", id), known = 1;
        json_free(me);
        if (!known && backend_pid) return backend_pid;
        if (!known) {  // (no server answer: a stable id from the token, for this session)
            uint64_t h = 1469598103934665603ull;
            for (const char *p = g_mod.token; *p; p++) h = (h ^ (uint8_t)*p) * 1099511628211ull;
            return 0x4000000000000000ull | (h & 0x0000ffffffffffffull) | 1;
        }
    }
    return pid_of_maker_id(own_maker_id);
}

// Makers of the courses served (fake player ids -> names), for the user records the game asks for next.
// A maker's player id is a hash of their maker ID on our server (the player's own: the game's account id, once /api/me
// answered), so the game's "is this my course / me" comparisons work; the table maps ids back to maker IDs and names.
#define NMAKERS 512
static struct { uint64_t pid; char name[48]; char maker_id[16]; } makers[NMAKERS];
static int nmakers;

uint64_t acc_account_id(void);  // runtime/services/acc.c: the network service account id the game is given

static uint64_t pid_of_maker_id(const char *maker_id) {
    if (own_maker_id[0] && maker_id && !strcmp(maker_id, own_maker_id)) {
        if (backend_pid) return backend_pid;  // (hybrid)
        // The player's own maker: the account id the game itself holds (GetNetworkServiceAccountId). Besides the
        // owner comparisons above, some own-checks compare a user record's player id with that account id directly
        // (My Info's Super World panel shows Delete only for it, main+0x1aa46a0), as on a console where they agree.
        uint64_t a = acc_account_id();
        if (a) return a;
    }
    uint64_t h = 1469598103934665603ull;
    for (const char *p = maker_id ? maker_id : ""; *p; p++) h = (h ^ (uint8_t)*p) * 1099511628211ull;
    return 0x5000000000000000ull | (h & 0x0000ffffffffffffull) | 1;
}

static uint64_t maker_pid(const char *maker_id, const char *name) {
    uint64_t pid = pid_of_maker_id(maker_id);
    pthread_mutex_lock(&state_lock);
    int i = 0;
    while (i < nmakers && makers[i].pid != pid) i++;
    if (i == nmakers) {
        i = nmakers < NMAKERS ? nmakers++ : (int)(pid % NMAKERS);
        makers[i].pid = pid;
    }
    if (name && *name) snprintf(makers[i].name, sizeof makers[i].name, "%s", name);
    else if (!makers[i].name[0]) snprintf(makers[i].name, sizeof makers[i].name, "Maker");
    snprintf(makers[i].maker_id, sizeof makers[i].maker_id, "%s", maker_id ? maker_id : "");
    pthread_mutex_unlock(&state_lock);
    return pid;
}

static void maker_name(uint64_t pid, char *out, size_t cap) {
    pthread_mutex_lock(&state_lock);
    snprintf(out, cap, "Maker");
    for (int i = 0; i < nmakers; i++)
        if (makers[i].pid == pid) snprintf(out, cap, "%s", makers[i].name);
    pthread_mutex_unlock(&state_lock);
}

// for world.c
uint64_t mod_maker_pid(const char *maker_id, const char *name) { return maker_pid(maker_id, name); }
void mod_maker_name(uint64_t pid, char *out, size_t cap) { maker_name(pid, out, cap); }
bool mod_maker_id(uint64_t pid, char out[16]) {  // the maker ID behind a player id ("" unknown)
    pthread_mutex_lock(&state_lock);
    out[0] = 0;
    if (pid == mod_player_pid()) snprintf(out, 16, "%s", own_maker_id);
    for (int i = 0; i < nmakers && !out[0]; i++)
        if (makers[i].pid == pid) snprintf(out, 16, "%s", makers[i].maker_id);
    pthread_mutex_unlock(&state_lock);
    return out[0] != 0;
}
const char *mod_own_maker_id(void) { return own_maker_id; }

static bool hundred_rules(void) { return mod_hundred_rules(); }
// the server keeps a run slot per ruleset: every run request says which (?ruleset=)
static const char *RULESET(void) { return hundred_rules() ? "hundred" : "smm2"; }
static Json *post_result(const char *path, const char *body, uint32_t d, const char *code);

// The game's own lives after a skip (research/specs/endless-challenge.md §2.4): its course end puts the counters back
// to the snapshot taken when the course started and takes a life only when its own flag (run +0x48) says the skip came
// during a miss demo - so a skip after a miss was posted showed the lives from before it (owner, 2026-10-30: 100 shown,
// 99 on the server). The server's lives go into both the current and the snapshot counters (lives[4] at counters +8,
// one per player) and the flag is cleared, so the course end lands on the server's number. Run = *(main+0x2c56bb0):
// +0x30 the current counters, +0x40 the snapshot.
static void run_lives_set(uint32_t lives) {
    uint64_t run = g64(MAIN_ADDR(0x2c56bb0));
    if (run < 0x7000000000ull) return;
    for (int k = 0; k < 2; k++) {
        uint64_t ct = g64(run + (k ? 0x40 : 0x30)), lp = ct > 0x7000000000ull ? g64(ct + 8) : 0;
        if (lp < 0x7000000000ull) continue;
        for (int i = 0; i < 4; i++) s32(lp + 4 * i, lives);
    }
    s8(run + 0x48, 0);
}  // (a refused result: below)

// Name and best per difficulty for the player's own record (online.c GetUsers). Best = the best clears of SMM2's
// rules, or with SMM1's rules the number of completed 100 Mario runs on that difficulty (the server's hundred_<d>
// boards): the menu's "High Score" slot shows that.
// SMM1's unlocks: Expert opens after a completed Normal run, Super Expert after a completed Expert run (from the
// server's hundred_<d> boards). RECOMP_MOD_UNLOCK_ALL=1 opens everything (tests); SMM2's rules have no locks.
static volatile bool unlocked[4] = {true, true, false, false};

static bool difficulty_open(int d) {
    if (!hundred_rules() || (getenv("RECOMP_MOD_UNLOCK_ALL") && atoi(getenv("RECOMP_MOD_UNLOCK_ALL")))) return true;
    return d < 2 || unlocked[d];
}

void mod_account_info(char *name, size_t cap, uint32_t best[4]) {
    Json *me = srv_call("GET", "/api/me", NULL, NULL);
    if (me && json_str(json_get(me, "name"), NULL)) snprintf(name, cap, "%s", json_str(json_get(me, "name"), ""));
    json_free(me);
    Json *st = srv_call("GET", "/api/endless", NULL, NULL);
    for (int d = 0; d < 4; d++) best[d] = (uint32_t)json_num(json_get(json_get(st, "best"), DIFF[d]), 0);
    json_free(st);
    if (hundred_rules())
        for (int d = 0; d < 4; d++) {
            char path[96];
            snprintf(path, sizeof path, "/api/rankings/hundred_%s?limit=1", DIFF[d]);
            Json *r = srv_call("GET", path, NULL, NULL);
            best[d] = (uint32_t)json_num(json_get(json_get(r, "me"), "score"), 0);
            json_free(r);
        }
    unlocked[2] = best[1] > 0, unlocked[3] = best[2] > 0;
}

// The menu's Expert / Super Expert tabs: a locked one doesn't open. The tab handlers (main+0x1a22260 / +0x1a22320 /
// +0x1a223e0 / +0x1a224a0, Easy .. Super Expert) also finish the menu's tab step (an event 0x7c queued, the map's
// +0x290 set), so a locked press runs the handler of the tab already chosen (the last difficulty, main+0x2c3f618)
// instead: the menu stays where it is and keeps taking input. Left out entirely, the menu stopped answering on
// Windows with the SMM1 screens off (#77).
static void tab_handler(Cpu *c, int d, uint64_t addr) {
    if (!mod_hundred() || difficulty_open(d)) { guest_call_orig(c, addr); return; }
    static const char *const names[4] = {"Easy", "Normal", "Expert", "Super Expert"};
    static const uint64_t handlers[4] = {0x1a22260, 0x1a22320, 0x1a223e0, 0x1a224a0};
    int32_t cur = (int32_t)g32(MAIN_ADDR(0x2c3f618));
    if (cur < 0 || cur > 3 || !difficulty_open(cur)) cur = 0;
    LOG("[mod] %s is locked: complete a %s run first (%s stays)\n", names[d], names[d - 1], names[cur]);
    guest_call_orig(c, MAIN_ADDR(handlers[cur]));
}
static void tab_expert(Cpu *c) { tab_handler(c, 2, MAIN_ADDR(0x1a223e0)); }
static void tab_super_expert(Cpu *c) { tab_handler(c, 3, MAIN_ADDR(0x1a224a0)); }

// ---- helpers

static uint32_t req_diff(uint64_t no, uint64_t field) {
    uint32_t d = g32(no + field);
    return d < 4 ? d : 0;
}

static uint64_t record(uint64_t no, uint32_t d) {
    uint64_t base = g64(no + NO_RECORDS);
    return d < g32(no + NO_RECORDS_COUNT) ? base + 0x38ull * d : base;
}

static void finish(Cpu *c, uint64_t no, bool ok, uint32_t status) {
    s32(no + NO_STATUS, ok ? 0 : status);
    c->x[0] = ok ? wrapper_ok() : wrapper_fail();
}

static void code_of(uint64_t data_id, char out[12]) { data_id_to_code((uint32_t)data_id, out); }

// The server's answer {lives, clears} into the run record of difficulty d (what the game's answers set).
static void set_play(Cpu *c, uint64_t no, uint32_t d, uint32_t lives, uint32_t clears) {
    pthread_mutex_lock(&state_lock);
    runs[d].lives = lives, runs[d].clears = clears, runs[d].active = lives > 0;
    pthread_mutex_unlock(&state_lock);
    netobj_lock(c);
    uint64_t r = record(no, d);
    s8(r + 0x28, 1);
    s32(r + 0x2c, clears);
    s8(r + 0x30, (uint8_t)(lives > 255 ? 255 : lives));
    netobj_unlock(c);
}

static uint32_t lives_now(uint64_t no, uint32_t d) { return g8(record(no, d) + 0x30); }
static uint32_t clears_now(uint64_t no, uint32_t d) { return g32(record(no, d) + 0x2c); }

// ---- around every request below, the mod's answer or the game's own (another backend): how long it took (the mod's
// log switch: issue #61's waits), and the preload's hooks (course_preload.h): the Endless list once a search is
// answered, the files of the courses after the one downloaded, everything dropped when the run ends.
extern uint64_t plat_now_ns(void);
void srv_wait_notice(bool on);  // server.c
void ocw_preload_before(uint32_t id);  // ocw.c: the game's own download (backend ocw)
void ocw_preload_after(uint32_t id, bool ok);

static uint64_t t_base;  // (endless_register: start-up)
static double t_since(uint64_t t) { return (double)(t - t_base) / 1e9; }
void mod_timing(const char *what) {  // a moment of the Endless play, for the log: "timing: <what> @<s since start>"
    if (g_mod.log) LOG("[mod] timing: %s @%.3f\n", what, t_since(plat_now_ns()));
}

static uint8_t *server_fetch(uint32_t id, size_t *len);

#define RESERVE_SLOT 1
static struct { bool on; uint32_t id; int d; uint64_t entries; uint32_t cap; } reserve;
static int reserve_lost;   // reserves written over in a row ...
static bool reserve_off;   // ... and after two, none for the session (each search would ask twice for nothing)
static bool reserve_test(void) {
    static int v = -1;
    if (v < 0) v = getenv("RECOMP_MOD_TEST_RESERVE") && atoi(getenv("RECOMP_MOD_TEST_RESERVE")) ? 1 : 0;
    return v;
}
bool ocw_active(void);  // ocw.c
void endless_reserve_drop(const char *why);

// The list the game plays (NO_COURSES after an Endless search, either backend): its ids in order from the head
static void capture_list(Cpu *c, uint64_t no) {
    uint32_t ids[64];
    int n = 0;
    netobj_lock(c);
    uint64_t ring = no + NO_COURSES, entries = g64(ring);
    uint32_t cap = g32(ring + 8), head = g32(ring + 0xc), count = g32(ring + 0x10);
    for (uint32_t i = 0; entries && cap && i < count && i < cap && n < 64; i++)
        ids[n++] = (uint32_t)g64(entries + (uint64_t)COURSE_ENTRY * ((head + i) % cap));
    netobj_unlock(c);
    MLOG("timing: course list (%s): %d of 10%s\n", DIFF[req_diff(no, NO_SEARCH_DIFF)], n,  // (fewer: more searches)
         reserve.on ? ", and one in reserve" : "");
    if (reserve.on && n < 64) ids[n++] = reserve.id;  // (the course the next search hands the game: fetched after these)
    cp_set_list(ids, n, (int)req_diff(no, NO_SEARCH_DIFF));
}

static uint64_t wrapper_subject(uint64_t off, uint64_t no) {  // the course a request is about (0: none)
    switch (off) {
    case 0x14de010: return g64(no + NO_DL_ID);
    case 0x14ecf30: case 0x14ed5e0: return g64(no + NO_COURSE_ID);
    case 0x14ed120: return g64(no + NO_CLEAR_ID);
    default: return 0;
    }
}

// GetCourses (0x14dbae0) writes the course list from the head too (on OCW the game asks for it after each skip): the
// reserve is moved to the list's last entry while it runs, and back after, so it isn't written over.
static bool reserve_parked;
static void reserve_park(Cpu *c, uint64_t no, bool park) {
    uint64_t ring = no + NO_COURSES, ents = g64(ring);
    uint32_t cap = g32(ring + 8);
    if (park) reserve_parked = false;
    if (!reserve.on || ents != reserve.entries || cap != reserve.cap || cap <= RESERVE_SLOT + 1) return;
    uint64_t e1 = ents + (uint64_t)COURSE_ENTRY * RESERVE_SLOT, far = ents + (uint64_t)COURSE_ENTRY * (cap - 1);
    if (park ? (uint32_t)g64(e1) != reserve.id : !reserve_parked) return;
    if (!park && (uint32_t)g64(far) != reserve.id) { reserve_parked = false; return; }  // (written over: dropped later)
    netobj_lock(c);
    bool moved = cp_swap_entries((void *)(uintptr_t)e1, (void *)(uintptr_t)far, COURSE_ENTRY);
    netobj_unlock(c);
    reserve_parked = park && moved;
}

static void wrapper_before(Cpu *c, uint64_t off, uint64_t no, bool native) {
    if (native && off == 0x14de010) ocw_preload_before((uint32_t)g64(no + NO_DL_ID));  // (no guest call: x0.. stay)
    if (off == 0x14dbae0) reserve_park(c, no, true);
}

static void wrapper_after(Cpu *c, const char *name, uint64_t off, uint64_t no, uint64_t t0, bool native) {
    uint32_t status = g32(no + NO_STATUS);
    bool ok = status == 0 && (uint8_t)c->x[0] == wrapper_ok();
    if (off == 0x14dbae0) reserve_park(c, no, false);
    if (native && off == 0x14de010) ocw_preload_after((uint32_t)g64(no + NO_DL_ID), ok);
    if (off == 0x14d8550 && g32(no + NO_SEARCH_TYPE) == 0xE && ok) capture_list(c, no);
    else if (off == 0x14d8550) cp_clear("Course World"), endless_reserve_drop("Course World");  // (Endless was left)
    if (off == 0x14ed360 || off == 0x14ed4a0) {
        cp_clear(off == 0x14ed360 ? "quit" : "game over");
        endless_reserve_drop(off == 0x14ed360 ? "quit" : "game over");
    }
    if (!g_mod.log) return;
    uint64_t id = wrapper_subject(off, no);
    char code[12] = "-";
    if (id) code_of(id, code);
    MLOG("timing: %s %s: %llu ms, status %u (%s) @%.3f\n", name, code,
         (unsigned long long)((plat_now_ns() - t0) / 1000000), status, native ? "the game's own" : "the mod's",
         t_since(t0));
}

#define WRAPPER(name, off)                                                                       \
    static void name##_impl(Cpu *c, uint64_t no);                                                \
    static void name(Cpu *c) {                                                                   \
        uint64_t no_ = c->x[0], t0_ = plat_now_ns();                                             \
        bool native_ = !mod_hundred();                                                           \
        wrapper_before(c, off, no_, native_);                                                    \
        if (native_) guest_call_orig(c, MAIN_ADDR(off));                                         \
        else name##_impl(c, no_);                                                                \
        wrapper_after(c, #name, off, no_, t0_, native_);                                         \
    }                                                                                            \
    static void name##_impl(Cpu *c, uint64_t no)

// ---- GetEndlessModeStatus: the four run records

// The server keeps a run slot per ruleset and difficulty ("runs": SMM2's, "hundred_runs": 100 Mario's); the last
// status of both, so a Challenge Modes switch shows the other mode's runs at once.
static struct slot { bool held, complete; uint32_t lives, clears, coins, score, goal; } slots[2][4];

static void read_slot(Json *run, struct slot *s) {
    bool active = json_bool(json_get(run, "active"), false);
    s->complete = json_bool(json_get(run, "complete"), false);
    s->lives = (uint32_t)json_num(json_get(run, "lives"), 0);
    s->clears = (uint32_t)json_num(json_get(run, "clears"), 0);
    s->coins = (uint32_t)json_num(json_get(run, "coins"), 0);
    s->score = (uint32_t)json_num(json_get(run, "score"), 0);
    s->goal = (uint32_t)json_num(json_get(run, "goal"), 0);
    s->held = active && s->lives > 0 && !s->complete;
}

// One run record (NetObj's, or the menu's copy in the UI cache: the same 0x38-byte layout) from the current
// Challenge Mode's slot for difficulty d (the run state kept here follows it).
// The run state +4 is 1 for a run that can go on: the menu offers Continue only for state 1 or 2 with lives (+0x30)
// and a history [obs 0x7101a26850]; 0 resets its map to an empty run. +0x11: the start lives New Game reads (the
// game has no default).
static void write_record(uint64_t r, uint32_t d) {
    pthread_mutex_lock(&state_lock);
    struct slot s = slots[hundred_rules()][d];
    runs[d].coins = s.coins, runs[d].score = s.score, runs[d].complete = s.complete, runs[d].goal = s.goal;
    runs[d].lives = s.lives, runs[d].clears = s.clears, runs[d].active = s.held;
    pthread_mutex_unlock(&state_lock);
    bool cont = s.held;
    uint8_t lv = (uint8_t)(s.lives > 255 ? 255 : s.lives);
    s8(r + 0x00, 1);
    s32(r + 0x04, cont ? 1 : 0);
    s8(r + 0x08, cont ? (uint8_t)(s.coins % 100) : 0);
    s32(r + 0x0c, cont ? s.score : 0);
    s8(r + 0x10, cont ? lv : 0);
    s8(r + 0x11, hundred_rules() ? 100 : 5);
    s64(r + 0x18, 0);
    s64(r + 0x20, 0);
    s8(r + 0x28, cont ? 1 : 0);  // (the play part: a run that can go on has lives; none shows none)
    s32(r + 0x2c, cont ? s.clears : 0);
    s8(r + 0x30, cont ? lv : 0);
}

WRAPPER(get_status, 0x14ec2d0) {
    Json *st = srv_call("GET", "/api/endless", NULL, NULL);
    if (!st) { finish(c, no, false, 2); return; }
    bool both = json_get(st, "hundred_runs") != NULL;  // (an older server: one slot, its run says the ruleset)
    pthread_mutex_lock(&state_lock);
    for (uint32_t d = 0; d < 4; d++) {
        struct slot s[2] = {{0}};
        if (both) {
            read_slot(json_get(json_get(st, "runs"), DIFF[d]), &s[0]);
            read_slot(json_get(json_get(st, "hundred_runs"), DIFF[d]), &s[1]);
        } else {
            Json *run = json_get(json_get(st, "runs"), DIFF[d]);
            bool h = !strcmp(json_str(json_get(run, "ruleset"), "smm2"), "hundred");
            read_slot(run, &s[h]);
        }
        slots[0][d] = s[0], slots[1][d] = s[1];
        MLOG("status %s: SMM2 %s lives %u, 100 Mario %s lives %u%s\n", DIFF[d], s[0].held ? "run" : "no run",
             s[0].lives, s[1].held ? "run" : "no run", s[1].lives, s[1].complete ? " (complete)" : "");
    }
    pthread_mutex_unlock(&state_lock);
    netobj_lock(c);
    for (uint32_t d = 0; d < 4; d++) write_record(record(no, d), d);
    netobj_unlock(c);
    json_free(st);
    finish(c, no, true, 0);
}

// (the UI cache: Cache +0x15e598 holds the menu's copy of the records [obs 0x7101520fd0])
#define UI_CACHE MAIN_ADDR(0x2c115e0)
// A Challenge Modes switch: the run records again as the other mode sees them (the game's and the menu's copies).
void endless_apply_mode(void) {
    uint64_t no = netobj(), cache = g64(UI_CACHE);
    for (uint32_t d = 0; d < 4; d++) {
        if (no && g64(no + NO_RECORDS) && d < g32(no + NO_RECORDS_COUNT)) write_record(record(no, d), d);
        if (cache && g64(cache + 0x15e598) && d < g32(cache + 0x15e590))
            write_record(g64(cache + 0x15e598) + 0x38ull * d, d);
    }
}

// Whether SMM2's Endless menu shows Continue for difficulty d, from the menu's copy of the record: a run that can go
// on (+0x04 state 1 or 2) with lives (+0x30) and a history, i.e. a course cleared (+0x2c) [inf: a run with lives
// and no clear shows New Game alone, research/specs/endless-challenge.md 1.2]. -1: no copy yet.
int endless_menu_offers_continue(int d) {
    uint64_t cache = g64(UI_CACHE);
    if (d < 0 || d > 3 || !cache || !g64(cache + 0x15e598) || (uint32_t)d >= g32(cache + 0x15e590)) return -1;
    uint64_t r = g64(cache + 0x15e598) + 0x38ull * (uint32_t)d;
    uint32_t state = g32(r + 0x04);
    return (state == 1 || state == 2) && g8(r + 0x30) != 0 && g32(r + 0x2c) != 0;
}

// ---- the next course, known before the game asks for it (the flow's start card shows it, as SMM1's does): the
// server's queue only moves on a result, so asking for its head now gives what the game's own search gets later.

// The run's next courses: the server may first gather them (TGRCode upstream: about a minute for a difficulty's first
// batch), so the request may take long; meanwhile hundred_run_status().loading says so (the flow shows it).
static volatile int loading;
static Json *next_courses(const char *path) {
    __sync_fetch_and_add(&loading, 1);
    Json *j = srv_call_slow("GET", path, NULL, NULL, 180000);
    __sync_fetch_and_sub(&loading, 1);
    return j;
}

static void *prefetch_thread(void *arg) {
    int d = (int)(intptr_t)arg;
    char path[96];
    snprintf(path, sizeof path, "/api/endless/%s/next?count=1&ruleset=%s", DIFF[d], RULESET());
    Json *j = next_courses(path);
    Json *m = json_at(json_get(j, "courses"), 0);
    uint32_t id;
    if (m && code_to_data_id(json_str(json_get(m, "code"), ""), &id)) {
        const char *maker = json_str(json_get(m, "maker"), "");
        note_course(id, json_str(json_get(m, "name"), ""), maker, style_index(json_str(json_get(m, "style"), "")),
                    json_get(m, "theme") ? (int)json_num(json_get(m, "theme"), 0) : -1);
        extern void smm2_shader_hint_course(const char *style, int theme_main, int theme_sub);  // (shader_rank.c)
        smm2_shader_hint_course(json_str(json_get(m, "style"), ""), (int)json_num(json_get(m, "theme"), 0), -1);
        pthread_mutex_lock(&state_lock);
        if (!runs[d].started) runs[d].current = id;
        pthread_mutex_unlock(&state_lock);
        MLOG("next course (%s): %s %s\n", DIFF[d], json_str(json_get(m, "code"), ""), json_str(json_get(m, "name"), ""));
    }
    json_free(j);
    return NULL;
}

void endless_prefetch(int d) {
    if (d < 0 || d > 3) return;
    pthread_t th;
    pthread_create(&th, NULL, prefetch_thread, (void *)(intptr_t)d);
    pthread_detach(th);
}

// A run the flow starts (hundred_run_start): a new one has the start lives at once (the status the start card reads
// before the game's New Game request is answered); a continued one has the server's.
// A continued run's credits: the courses cleared before (the server's history of the run, with names and makers) in
// place of what this session saw, so the ending lists the whole run after a Save & Quit or a restart.
static void *credits_thread(void *arg) {
    int d = (int)(intptr_t)arg;
    char path[96];
    snprintf(path, sizeof path, "/api/endless/%s/history?ruleset=%s", DIFF[d], RULESET());
    int st;
    Json *j = srv_call("GET", path, NULL, &st);
    Json *list = json_get(j, "history");
    if (list) {
        pthread_mutex_lock(&state_lock);
        runs[d].ncredits = 0;
        for (size_t i = 0; i < json_len(list) && runs[d].ncredits < 32; i++) {
            Json *e = json_at(list, i);
            if (strcmp(json_str(json_get(e, "result"), ""), "clear")) continue;
            struct hundred_credit *c = &runs[d].credits[runs[d].ncredits++];
            snprintf(c->name, sizeof c->name, "%s", json_str(json_get(e, "name"), "?"));
            snprintf(c->maker, sizeof c->maker, "%s", json_str(json_get(e, "maker"), ""));
            snprintf(c->code, sizeof c->code, "%s", json_str(json_get(e, "code"), ""));
        }
        MLOG("credits of the continued run (%s): %d courses\n", DIFF[d], runs[d].ncredits);
        pthread_mutex_unlock(&state_lock);
    }
    json_free(j);
    return NULL;
}

void endless_prepare_run(int d, bool resume) {
    if (d < 0 || d > 3) return;
    static const uint32_t totals[4] = {8, 16, 16, 6};
    pthread_mutex_lock(&state_lock);
    if (!resume) {
        memset(&runs[d], 0, sizeof runs[d]);
        runs[d].active = true, runs[d].lives = hundred_rules() ? 100 : 5, runs[d].goal = totals[d];
    }
    runs[d].current = 0;
    pthread_mutex_unlock(&state_lock);
    if (resume) {
        endless_prefetch(d);
        pthread_t th;
        if (pthread_create(&th, NULL, credits_thread, (void *)(intptr_t)d) == 0) pthread_detach(th);
    }
}

// ---- InitEndlessMode: New Game

WRAPPER(init_run, 0x14ece20) {
    uint32_t d = req_diff(no, NO_DIFF);
    char path[64], body[64];
    snprintf(path, sizeof path, "/api/endless/%s/start?ruleset=%s", DIFF[d], RULESET());
    snprintf(body, sizeof body, "{\"ruleset\":\"%s\"}", hundred_rules() ? "hundred" : "smm2");
    int st;
    Json *j = srv_call("POST", path, body, &st);
    bool ok = j != NULL;
    if (ok) {
        pthread_mutex_lock(&state_lock);
        memset(&runs[d], 0, sizeof runs[d]);
        Json *r = json_get(j, "run");
        runs[d].active = true, runs[d].lives = (uint32_t)json_num(json_get(r, "lives"), 0);
        runs[d].goal = (uint32_t)json_num(json_get(r, "goal"), 0);
        pthread_mutex_unlock(&state_lock);
        bridge_event("new", d);
        LOG("[mod] New Game: %s, %s rules, %u lives\n", DIFF[d], hundred_rules() ? "hundred" : "smm2",
            (uint32_t)json_num(json_get(json_get(j, "run"), "lives"), 0));
        endless_prefetch(d);
    }
    json_free(j);
    finish(c, no, ok, 2);
}

// ---- GetEndlessModePlayInfo: the history rings (the map's dots)

WRAPPER(get_play_info, 0x14ed750) {
    Json *h[4] = {0};
    for (int d = 0; d < 4; d++) {
        char path[64];
        snprintf(path, sizeof path, "/api/endless/%s/history?ruleset=%s", DIFF[d], RULESET());
        int st;
        h[d] = srv_call("GET", path, NULL, &st);  // (404: no run on that difficulty)
    }
    netobj_lock(c);
    for (int d = 0; d < 4; d++) {
        uint64_t ring = no + NO_HISTORY + (uint64_t)HISTORY_STRIDE * d;
        uint64_t entries = g64(ring);
        uint32_t cap = g32(ring + 8);
        s64(ring + 0xc, 0);  // head 0, count 0
        Json *list = json_get(h[d], "history");
        size_t n = json_len(list), first = n > cap ? n - cap : 0;
        uint32_t count = 0;
        for (size_t i = first; i < n && entries; i++) {
            Json *e = json_at(list, i);
            uint32_t id;
            if (!code_to_data_id(json_str(json_get(e, "code"), ""), &id)) continue;
            uint64_t p = entries + 0x18ull * count++;
            s64(p, id);
            s32(p + 8, strcmp(json_str(json_get(e, "result"), ""), "clear") ? 1 : 0);  // 0 clear, 1 skip [inf]
            s32(p + 0xc, (uint32_t)json_num(json_get(e, "seq"), 0));
            extern int style_index(const char *);
            s8(p + 0x10, (uint8_t)style_index(json_str(json_get(e, "style"), "")));
            s8(p + 0x11, (uint8_t)json_num(json_get(e, "theme"), 0));
        }
        s32(ring + 0x10, count);
    }
    netobj_unlock(c);
    for (int d = 0; d < 4; d++) json_free(h[d]);
    finish(c, no, true, 0);
}

// ---- GetEndlessModeRank: the player's place on the difficulty's board

WRAPPER(get_rank, 0x14eddc0) {
    uint32_t d = req_diff(no, NO_DIFF);
    char path[96];
    snprintf(path, sizeof path, "/api/rankings/%s_%s?limit=1", hundred_rules() ? "hundred" : "endless", DIFF[d]);
    Json *j = srv_call("GET", path, NULL, NULL);
    uint32_t rank = (uint32_t)json_num(json_get(json_get(j, "me"), "rank"), 0);
    json_free(j);
    netobj_lock(c);
    s32(no + NO_RANK, rank ? rank : 0xffffffffu);
    netobj_unlock(c);
    finish(c, no, true, 0);
}

// ---- the course list (SearchCoursesEndlessMode) and the course download (GetObject)

int style_index(const char *s) {
    static const char *const styles[5] = {"M1", "M3", "MW", "WU", "3W"};
    for (int i = 0; i < 5; i++)
        if (!strcmp(s, styles[i])) return i;
    return 0;
}

static void put_utf16(uint64_t buf, uint32_t cap, const char *s) {
    if (!buf || !cap) return;
    uint32_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && n + 1 < cap;) {
        uint32_t cp = *p++;
        if (cp >= 0xC0) {
            int extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : 1;
            cp &= 0x3F >> extra;
            while (extra-- && (*p & 0xC0) == 0x80) cp = cp << 6 | (*p++ & 0x3F);
            if (cp > 0xFFFF) cp = '?';
        }
        s16(buf + 2 * n++, (uint16_t)cp);
    }
    s16(buf + 2 * n, 0);
}

// One course-list entry (0x548 bytes), as the game's converter fills it from a NEX CourseInfo. The string objects
// inside the entry already own their buffers: only their contents are written.
static void fill_course(uint64_t e, Json *m, uint32_t data_id, uint32_t difficulty) {
    char code[12], bare[10];
    data_id_to_code(data_id, code);
    snprintf(bare, sizeof bare, "%.3s%.3s%.3s", code, code + 4, code + 8);  // (the game adds the dashes itself)
    s64(e + 0x000, data_id);
    put_utf16(g64(e + 0x010), g32(e + 0x018), bare);
    const char *maker = json_str(json_get(m, "maker"), "");
    note_course(data_id, json_str(json_get(m, "name"), ""), maker, style_index(json_str(json_get(m, "style"), "")),
                json_get(m, "theme") ? (int)json_num(json_get(m, "theme"), 0) : -1);
    s64(e + 0x030, maker_pid(json_str(json_get(m, "maker_id"), ""), maker));
    put_utf16(g64(e + 0x040), g32(e + 0x048), json_str(json_get(m, "name"), ""));
    char desc[400];  // (an edited copy of someone else's course: the credit line, edit.c)
    extern const char *edit_listing_description(Json *m, char *buf, size_t cap);
    put_utf16(g64(e + 0x098), g32(e + 0x0a0), edit_listing_description(m, desc, sizeof desc));
    s8(e + 0x170, (uint8_t)style_index(json_str(json_get(m, "style"), "")));
    s8(e + 0x171, (uint8_t)json_num(json_get(m, "theme"), 0));
    s64(e + 0x178, (uint64_t)json_num(json_get(m, "uploaded"), 0));
    s32(e + 0x180, difficulty);
    // The player's own standing with the course, from the server's `you` (signed in; 0 for both without it):
    // +0x184 their play state (the larger of NEX CourseInfo +0x178 / +0x17a, converter main+0x14daf60): 1 not played,
    // 2 played without a clear, 3 cleared. +0x188 their rating (NEX +0x179; course global main+0x2c4b710 +0x1b8): 1 not
    // played, 2 played (may rate), 3 liked, 4 booed. After a play the game asks for the record again and moves both on
    // itself in case the server is behind (main+0x151a520): state 1 -> 2, or 3 on a clear, 2 -> 3 on a clear; rating
    // 1 -> 2 (and PostRatingInfo's answer, main+0x151fd00: 3 / 4, or 2 taken back). Course cards show the state (3: the
    // red cleared flag, 0: a white flag) and a heart at rating 0 [obs]. The clear screen's Boo / I like it
    // (main+0x1be73b0) take input at rating 1 or 2 and show 3 / 4 as chosen. Comments with "Require course clear to
    // view" are shown to a player at state 3 (in the course, main+0x17f2a30 / main+0x17f39e0 via the display's
    // +0x356, main+0x17f5fd4; the comment list, main+0x171b850), besides their poster and the course's maker.
    Json *you = json_get(m, "you");
    int vote = (int)json_num(json_get(you, "vote"), 0);
    bool cleared = json_bool(json_get(you, "cleared"), false), played = cleared || json_bool(json_get(you, "played"), false);
    uint32_t state = !you ? 0 : cleared ? 3 : played ? 2 : 1;
    uint32_t mine = !you ? 0 : vote > 0 ? 3 : vote < 0 ? 4 : played ? 2 : 1;
    s32(e + 0x184, state), s32(e + 0x188, mine), s32(e + 0x18c, 0);
    s8(e + 0x190, 0), s8(e + 0x191, 0);
    s32(e + 0x194, 0), s32(e + 0x198, 0), s32(e + 0x19c, 0), s32(e + 0x1a0, 0);  // no clear condition
    s32(e + 0x1a4, 0);
    uint32_t plays = (uint32_t)json_num(json_get(m, "plays"), 0), clears = (uint32_t)json_num(json_get(m, "clears"), 0),
             attempts = (uint32_t)json_num(json_get(m, "attempts"), 0);
    // play statistics (keys 0-4: plays, attempts, ?, clears, versus plays), two unknown counters, the ratings
    // (key 0: likes) [obs converter 0x71014daf60: +0x1a8.. play stats, +0x1bc/+0x1c0 map 0x40, +0x1c4.. ratings]
    uint32_t stats[10] = {plays, attempts, 0, clears, 0, 0, 0, (uint32_t)json_num(json_get(m, "likes"), 0)};
    for (int i = 0; i < 10; i++) s32(e + 0x1a8 + 4 * i, stats[i]);
    // time stats: first clear, world record holder (player ids), world record and the maker's time (ms)
    const char *fc = json_str(json_get(m, "first_clear"), ""), *wr = json_str(json_get(m, "record_holder"), "");
    s64(e + 0x1d0, *fc ? maker_pid(fc, NULL) : 0), s64(e + 0x1d8, *wr ? maker_pid(wr, NULL) : 0);
    s32(e + 0x1e0, (uint32_t)json_num(json_get(m, "record_ms"), 0)), s32(e + 0x1e4, 0);
    s32(e + 0x1e8, (uint32_t)json_num(json_get(m, "comments"), 0));
    // thumbnails / record holders: none (empty addresses: the game downloads nothing for them)
    s64(e + 0x1f0, data_id);
    if (g64(e + 0x200)) s8(g64(e + 0x200), 0);
    s8(e + 0x310, 0), s32(e + 0x314, 0);
    if (g64(e + 0x320)) s8(g64(e + 0x320), 0);
    s64(e + 0x398, data_id);
    if (g64(e + 0x3a8)) s8(g64(e + 0x3a8), 0);
    s8(e + 0x4b8, 0), s32(e + 0x4bc, 0);
    if (g64(e + 0x4c8)) s8(g64(e + 0x4c8), 0);
    s8(e + 0x540, 1);
}

void mod_fill_course(uint64_t e, Json *m, uint32_t data_id, uint32_t difficulty) {
    fill_course(e, m, data_id, difficulty);
}

// ---- One course in reserve (issue #61). OpenCourse World answers each Endless search with one course, so the game
// searches again after every course, and the next course isn't known until then: its download can't start early.
// Here each search hands the game the course the last search brought (held in the list's second entry, its file
// fetched while the course before it was played: course_preload.c), and the course the search brings now takes its
// place. The first search of a run asks twice. OCW keeps no record of which course a search gave out: the course the
// game starts is the run's course, and clear / skip / game over go by that (ocw-server db/dyna/endless_type_normal.go),
// so a course held one search longer plays exactly as one just found. Each search still goes to OCW, the game's own.
// Dropped (a fresh pair at the next search) on anything else: another difficulty, a Course World list, quit, game over,
// a list the search wrote over, a backend restart. A course the reserve would repeat back to back is dropped too (OCW
// counts a course started twice in a row as a lost life).
// RECOMP_MOD_TEST_RESERVE=1 (tests): the same on our own server, whose searches then answer one course each.
static bool reserve_wanted(bool native) { return g_mod.preload && (native ? ocw_active() : reserve_test()); }

void endless_reserve_drop(const char *why) {
    if (!strcmp(why, "backend restart")) reserve_off = false, reserve_lost = 0;
    if (reserve.on) {
        char code[12];
        code_of(reserve.id, code);
        MLOG("reserve %s: dropped (%s)\n", code, why);
    }
    reserve.on = false;
}

static void search_courses_impl(Cpu *c, uint64_t no);
static void search_call(Cpu *c, uint64_t no, bool native, const uint64_t *args) {
    memcpy(c->x, args, 8 * sizeof *args);  // (the request's own registers, for a second call)
    if (native) guest_call_orig(c, MAIN_ADDR(0x14d8550));
    else search_courses_impl(c, no);
}
static bool search_one(Cpu *c, uint64_t no, uint64_t ring) {  // the search ended well with one course at the head
    return g32(no + NO_STATUS) == 0 && (uint8_t)c->x[0] == wrapper_ok() && g32(ring + 0xc) == 0 && g32(ring + 0x10) == 1;
}

static void search_with_reserve(Cpu *c, uint64_t no, bool native) {
    uint64_t args[8];
    memcpy(args, c->x, sizeof args);
    uint32_t d = req_diff(no, NO_SEARCH_DIFF);
    uint64_t ring = no + NO_COURSES, ents = g64(ring);
    uint32_t cap = g32(ring + 8);
    uint64_t e0 = ents, e1 = ents + (uint64_t)COURSE_ENTRY * RESERVE_SLOT;
    bool have = reserve.on && reserve.d == (int)d && reserve.entries == ents && reserve.cap == cap && ents &&
                cap > RESERVE_SLOT && (uint32_t)g64(e1) == reserve.id;
    if (reserve.on && !have) {
        bool lost = reserve.d == (int)d;  // (same difficulty: something else wrote over it)
        endless_reserve_drop(lost ? "the list changed" : "another difficulty");
        if (lost && ++reserve_lost >= 2) {
            reserve_off = true;
            LOG("[mod] reserve: off for this session (the game wrote over the held course twice)\n");
        }
    }
    if (reserve_off) {
        search_call(c, no, native, args);
        return;
    }
    if (have) {  // (the new course goes to the head, the reserve stays where it is)
        netobj_lock(c);
        s32(ring + 0xc, 0), s32(ring + 0x10, 0);
        netobj_unlock(c);
    }
    search_call(c, no, native, args);
    if (!ents || cap <= RESERVE_SLOT || !search_one(c, no, ring)) {
        // (a failure: the game's own error and retry, the reserve kept for it; more than one course: nothing to hold)
        if (g32(no + NO_STATUS) == 0 && g32(ring + 0x10) != 1) endless_reserve_drop("the search brought more than one");
        return;
    }
    uint32_t got = (uint32_t)g64(e0);
    char a[12], b[12];
    if (have) {
        if ((uint32_t)g64(e1) != reserve.id) { endless_reserve_drop("the search wrote over it"); return; }
        netobj_lock(c);
        bool moved = cp_swap_entries((void *)(uintptr_t)e0, (void *)(uintptr_t)e1, COURSE_ENTRY);
        netobj_unlock(c);
        if (!moved) { endless_reserve_drop("no memory"); return; }
        uint32_t now = reserve.id;
        reserve.id = got, reserve_lost = 0;
        code_of(now, a), code_of(got, b);
        MLOG("reserve: %s now (held since the last search), %s held for the next\n", a, b);
        if (got == now) endless_reserve_drop("the same course again");
        return;
    }
    // no reserve yet: this course goes to the second entry, and a second search brings the one played now
    uint64_t x0 = c->x[0];
    uint32_t st = g32(no + NO_STATUS);
    netobj_lock(c);
    bool moved = cp_swap_entries((void *)(uintptr_t)e0, (void *)(uintptr_t)e1, COURSE_ENTRY);
    s32(ring + 0xc, 0), s32(ring + 0x10, 0);
    netobj_unlock(c);
    if (!moved) {
        netobj_lock(c);
        s32(ring + 0x10, 1);
        netobj_unlock(c);
        return;
    }
    search_call(c, no, native, args);
    uint32_t now = (uint32_t)g64(e0);
    bool second = search_one(c, no, ring), first_kept = (uint32_t)g64(e1) == got;
    if (second && first_kept && now != got) {
        reserve.on = true, reserve.id = got, reserve.d = (int)d, reserve.entries = ents, reserve.cap = cap;
        code_of(now, a), code_of(got, b);
        MLOG("reserve: %s now, %s held for the next (two searches: the run's first)\n", a, b);
        return;
    }
    if (first_kept) {  // (the second search failed or brought the same course: the first one's answer, as if alone)
        netobj_lock(c);
        cp_swap_entries((void *)(uintptr_t)e0, (void *)(uintptr_t)e1, COURSE_ENTRY);
        s32(ring + 0xc, 0), s32(ring + 0x10, 1);
        netobj_unlock(c);
        s32(no + NO_STATUS, st);
        c->x[0] = x0;
        MLOG("reserve: none this time (the second search %s)\n", second ? "brought the same course" : "failed");
        return;
    }
    // (the second search wrote over the first one's course: its own answer stands, a course or the game's error
    // and retry, with nothing stale in the list)
    if (!second) {
        netobj_lock(c);
        s32(ring + 0xc, 0), s32(ring + 0x10, 0);
        netobj_unlock(c);
    }
    MLOG("reserve: none this time (the second search wrote over the first)\n");
}

static void search_courses(Cpu *c) {
    uint64_t no = c->x[0], t0 = plat_now_ns();
    bool native = !mod_hundred();
    wrapper_before(c, 0x14d8550, no, native);
    if (g32(no + NO_SEARCH_TYPE) == 0xE && reserve_wanted(native)) search_with_reserve(c, no, native);
    else if (native) guest_call_orig(c, MAIN_ADDR(0x14d8550));
    else search_courses_impl(c, no);
    wrapper_after(c, "search_courses", 0x14d8550, no, t0, native);
}

static void search_courses_impl(Cpu *c, uint64_t no) {
    if (g32(no + NO_SEARCH_TYPE) != 0xE) {  // Course World's lists (world.c)
        extern void world_search(Cpu * c, uint64_t no, uint32_t type);
        world_search(c, no, g32(no + NO_SEARCH_TYPE));
        return;
    }
    uint32_t d = req_diff(no, NO_SEARCH_DIFF);
    char path[96];
    snprintf(path, sizeof path, "/api/endless/%s/next?count=10&ruleset=%s", DIFF[d], RULESET());
    Json *j = next_courses(path);
    if (!j) { finish(c, no, false, 2); return; }
    Json *list = json_get(j, "courses");
    // (the reserve test: one course a search, picked at random from the ten, as OCW answers)
    size_t pick = reserve_test() && json_len(list) ? (size_t)rand() % json_len(list) : 0;
    netobj_lock(c);
    uint64_t ring = no + NO_COURSES, entries = g64(ring);
    uint32_t cap = g32(ring + 8), count = 0;
    s64(ring + 0xc, 0);
    for (size_t i = pick; i < json_len(list) && count < cap && entries && !(reserve_test() && count); i++) {
        Json *m = json_at(list, i);
        uint32_t id;
        if (!code_to_data_id(json_str(json_get(m, "code"), ""), &id)) continue;
        fill_course(entries + (uint64_t)COURSE_ENTRY * count++, m, id, d);
        MLOG("  course %u: %s %s\n", count, json_str(json_get(m, "code"), ""), json_str(json_get(m, "name"), ""));
    }
    s32(ring + 0x10, count);
    netobj_unlock(c);
    last_search_d = (int)d;
    bridge_event("list", d);
    LOG("[mod] course list (%s): %u courses%s\n", DIFF[d], count, json_bool(json_get(j, "widened"), false) ? " (band widened)" : "");
    json_free(j);
    finish(c, no, count > 0, 2);
}

// Course details the game asks for around a course start (names [?]; outputs as the originals write them): two
// player records about the course (+0xe8530 and +0xe8580, each with a "valid" byte; [inf] world record / first
// clear), a list for a course (ring at +0xe85a0, 0x18-byte entries), and a check on an id (+0x141480 -> u32 result
// at +0x141604, 0 = fine). Answered empty: no records, an empty list, the check passes. Not connected, the game
// shows a communication error at times (it doesn't need them to play).
WRAPPER(course_list_info, 0x14ec0d0) {
    netobj_lock(c);
    s64(no + 0xe85ac, 0);  // head 0, count 0
    netobj_unlock(c);
    finish(c, no, true, 0);
}

WRAPPER(id_check, 0x14f17a0) {
    netobj_lock(c);
    s32(no + 0x141604, 0);
    netobj_unlock(c);
    finish(c, no, true, 0);
}

// The course's comments (SearchCommentsInOrder, read while a course plays): none. Ring of 0x390-byte comments at
// +0x8f410; +0xe8528 / +0xe852c / +0xe852d: the request it answered and "answered".
WRAPPER(course_comments, 0x14e8ec0) {
    netobj_lock(c);
    s64(no + 0x8f41c, 0);  // head 0, count 0
    s32(no + 0xe8528, g32(no + 0x737a8));
    s8(no + 0xe852c, g8(no + 0x737ac));
    s8(no + 0xe852d, 1);
    netobj_unlock(c);
    finish(c, no, true, 0);
}

// (CanPostRatingAndComment, PostRatingInfo, PostPlayResult and the GetUsers of listed players: world.c)

// Course records by data id (asked for after a clear, for the map): the ids are in a linked list at +0x74748 (count
// +0x74760); the answer goes to the course list ring at +0x30780, as for a search. From GET /api/courses/<code>.
// (A Super World has up to 40 courses.)
#define GET_COURSES_MAX 64
WRAPPER(get_courses, 0x14dbae0) {
    uint64_t ids[GET_COURSES_MAX];
    uint32_t n = 0;
    netobj_lock(c);
    // (a node's link is at p: its id at p - 8, the next link at p + 8; the list's own link, at +0x74750, ends it. The
    // first id is the first node's, as the original reads them [obs main+0x14dbb8c].)
    uint64_t end = no + 0x74750, p = g64(no + 0x74758);
    for (int guard = 0; p && p != end && guard < 4 * GET_COURSES_MAX && n < GET_COURSES_MAX; guard++) {
        ids[n++] = g64(p - 8);
        p = g64(p + 8);
    }
    netobj_unlock(c);
    Json *metas[GET_COURSES_MAX] = {0};
    for (uint32_t i = 0; i < n; i++) {
        char code[12], path[96];
        code_of(ids[i], code);
        snprintf(path, sizeof path, "/api/courses/%s", code);
        metas[i] = srv_call("GET", path, NULL, NULL);
    }
    netobj_lock(c);
    uint64_t ring = no + NO_COURSES, entries = g64(ring);
    uint32_t cap = g32(ring + 8), count = 0;
    s64(ring + 0xc, 0);
    for (uint32_t i = 0; i < n && count < cap && entries; i++) {
        if (!metas[i]) continue;
        uint32_t diff = (uint32_t)json_num(json_get(metas[i], "difficulty"), 50);
        fill_course(entries + (uint64_t)COURSE_ENTRY * count++, metas[i], (uint32_t)ids[i],
                    diff < 35 ? 0 : diff < 75 ? 1 : diff < 96 ? 2 : 3);
    }
    s32(ring + 0x10, count);
    netobj_unlock(c);
    for (uint32_t i = 0; i < n; i++) json_free(metas[i]);
    MLOG("GetCourses: %u of %u\n", count, n);
    finish(c, no, count > 0 || n == 0, 2);
}

// ReportFromBugDetection (142): data id +0x141480, a kind +0x141518, a text +0x141528. The game posts it when a
// downloaded course fails its own validity check (main+0xfcab10 in the download job; then "The course data is
// corrupt."). In an Endless run the course is reported to the server as broken (it leaves the run's queue) and the
// bridge answers the window and goes on.
WRAPPER(course_post, 0x14f2910) {
    uint64_t id = g64(no + 0x141480);
    LOG("[mod] course %llu reported broken by the game (kind %u)\n", (unsigned long long)id, g8(no + 0x141518));
    int d = -1;
    pthread_mutex_lock(&state_lock);
    for (int i = 0; i < 4 && d < 0; i++)  // (the run whose next course this is: the head of its server queue)
        if (runs[i].current == id || runs[i].started == id) d = i;
    pthread_mutex_unlock(&state_lock);
    if (d < 0 && last_search_d >= 0 && runs[last_search_d].active) d = last_search_d;
    if (d >= 0) {
        char code[12], path[64], body[96];
        code_of(id, code);
        snprintf(path, sizeof path, "/api/endless/%s/result?ruleset=%s", DIFF[d], RULESET());
        snprintf(body, sizeof body, "{\"code\":\"%s\",\"result\":\"broken\"}", code);
        Json *j = srv_call("POST", path, body, NULL);
        json_free(j);
        bridge_event("broken", (uint32_t)d);
    }
    finish(c, no, true, 0);
}

// The next courses' files, fetched while the current one is played (course_preload.h): the ones after it in the
// game's list as soon as its own download is done (get_course_file), so a skip right after a course starts finds the
// next one held or under way (it waits on that fetch, never starting a second one of the same course: issue #61). At
// the list's end the server's queue names the next one (the course after the one at its head; start_course).
//
// A course's data: a listed TGRCode course the server doesn't hold yet is fetched first, in TGRCode's turn (about one
// request in 5-7 s, behind any other client's): waited for up to 20 s (the server's most), so the first try works
// (owner, 2026-10-23: "course ID not found" on the first try, the second worked)
#define DATA_WAIT_MS 20000
static uint8_t *server_fetch(uint32_t id, size_t *len) {  // (the preload's thread)
    char code[12], path[96];
    code_of(id, code);
    snprintf(path, sizeof path, "/api/courses/%s/data?wait_ms=%d", code, DATA_WAIT_MS);
    return srv_get_bytes_slow(path, len, DATA_WAIT_MS + 6000);
}

static void *prefetch_data_thread(void *arg) {
    int d = (int)(intptr_t)arg;
    char path[96];
    snprintf(path, sizeof path, "/api/endless/%s/next?count=2&ruleset=%s", DIFF[d], RULESET());
    Json *j = next_courses(path);
    Json *m = json_at(json_get(j, "courses"), 1);
    uint32_t id;
    if (m && code_to_data_id(json_str(json_get(m, "code"), ""), &id) && cp_want(id, CP_PLAIN, server_fetch))
        MLOG("preload %s: the server's next, past the list's end\n", json_str(json_get(m, "code"), ""));
    json_free(j);
    return NULL;
}

static void prefetch_data(uint32_t d) {
    pthread_t th;
    pthread_create(&th, NULL, prefetch_data_thread, (void *)(intptr_t)d);
    pthread_detach(th);
}

// The course file the game expects: a 0x10-byte header {u32 1, u16 0x10, u16 0, u32 CRC32 of the course, "SCDL"},
// the course (0x5bfc0 bytes) encrypted with the game's own course encryption (main+0x8b3070, AES-128-CBC with a key
// from the game's key table, IV and CMAC in the 0x30-byte block that follows). Nothing of the key leaves the game.
#define COURSE_SIZE 0x5bfc0
#define FILE_SIZE 0x5c000
#define ENCRYPT_FN MAIN_ADDR(0x8b3070)
#define KEY_TABLE MAIN_ADDR(0x23d68c0)

static uint32_t crc32(const uint8_t *p, size_t n) {
    static uint32_t t[256];
    if (!t[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
    uint32_t c = 0xFFFFFFFFu;
    while (n--) c = t[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

WRAPPER(get_course_file, 0x14de010) {
    uint64_t id = g64(no + NO_DL_ID), dest = g64(no + NO_DL_BUFFER);
    char code[12], path[96];
    code_of(id, code);
    snprintf(path, sizeof path, "/api/courses/%s/data?wait_ms=%d", code, DATA_WAIT_MS);
    size_t len = 0;
    uint8_t *data = NULL;
    int waited = 0, was = g_mod.preload ? cp_state((uint32_t)id, CP_PLAIN) : CP_NONE, how = CP_MISS;
    if (was != CP_NONE) {  // (held, or its fetch under way: waited on, as long as a download of its own could take)
        srv_wait_notice(true);
        how = cp_take((uint32_t)id, CP_PLAIN, DATA_WAIT_MS + 6000, &data, &len, &waited);
        srv_wait_notice(false);
    }
    if (how == CP_HIT) MLOG("course %s: preloaded\n", code);
    else if (how == CP_JOINED) MLOG("course %s: preload joined, %d ms\n", code, waited);
    else {
        if (was != CP_NONE)
            MLOG("course %s: the preload %s: downloaded now\n", code, was == CP_FAILED ? "failed" : "didn't end");
        extern void srv_waited(bool on);  // (server.c: a slow answer says so over the game's black screen)
        srv_waited(true);
        data = srv_get_bytes_slow(path, &len, DATA_WAIT_MS + 6000);
        srv_waited(false);
    }
    if (g_mod.preload) cp_after((uint32_t)id, CP_PLAIN, server_fetch);  // (the next ones, while this one is played)
    if (!data || len != COURSE_SIZE || !dest) {
        LOG("[mod] course %s: %s\n", code, data ? "not a course (size)" : "download failed");
        free(data);
        finish(c, no, false, data ? 2 : 7);  // 7: retryable (the game retries once)
        return;
    }
    static uint8_t *plain;  // guest-visible buffers for the game's encryption (one download at a time: the worker)
    static uint64_t keyobj;
    if (!plain) {
        plain = hle_alloc(COURSE_SIZE);
        keyobj = (uint64_t)(uintptr_t)hle_alloc(16);
    }
    memcpy(plain, data, COURSE_SIZE);
    free(data);
    extern void smm2_shader_hint_course_data(const uint8_t *d, size_t n);  // (its shaders first: shader_rank.c)
    smm2_shader_hint_course_data(plain, COURSE_SIZE);
    {  // (tests: RECOMP_MOD_TEST_BREAK=<n>:<code> hands the game course <code>'s data in place of the n-th course,
       // e.g. a course the game finds broken)
        static int downloads;
        const char *tb = getenv("RECOMP_MOD_TEST_BREAK");
        const char *other = tb ? strchr(tb, ':') : NULL;
        if (other && ++downloads == atoi(tb)) {
            char p2[64];
            size_t n2 = 0;
            snprintf(p2, sizeof p2, "/api/courses/%s/data", other + 1);
            uint8_t *d2 = srv_get_bytes(p2, &n2);
            if (d2 && n2 == COURSE_SIZE) memcpy(plain, d2, COURSE_SIZE);
            free(d2);
            LOG("[mod] test: course %s's data in place of %s\n", other + 1, code);
        }
    }
    s64(keyobj, KEY_TABLE);
    uint8_t *out = (uint8_t *)(uintptr_t)dest;
    uint32_t crc = crc32(plain, COURSE_SIZE);
    uint64_t sx[8];
    for (int i = 0; i < 6; i++) sx[i] = c->x[i];
    c->x[0] = keyobj;                     // key table holder
    c->x[1] = dest + 0x10 + COURSE_SIZE;  // crypto block out (IV, seed, CMAC)
    c->x[2] = COURSE_SIZE;
    c->x[3] = dest + 0x10;                // encrypted course out
    c->x[4] = (uint64_t)(uintptr_t)plain;
    guest_call(c, ENCRYPT_FN);
    for (int i = 0; i < 6; i++) c->x[i] = sx[i];
    s32(dest + 0, 1);
    s16(dest + 4, 0x10);
    s16(dest + 6, 0);
    s32(dest + 8, crc);
    memcpy(out + 0xc, "SCDL", 4);
    MLOG("course %s downloaded (%u bytes), CRC %08x\n", code, FILE_SIZE, crc);
    finish(c, no, true, 0);
}

// ---- StartEndlessModeCourse: posted when a new course starts, and again during a miss demo (the life it costs).
// The first post for a course is the start; another post for the same course before its result is a miss.

// The miss demo has started (the death fanfare, mods/costume.c) and its miss isn't posted yet: a skip now costs the
// life (the server's rule, mmk_modes::endless mark_miss_demo; SMM1's own). The game posts the miss partway through the
// demo, so a skip pressed before that came with no miss at all, and the lives shown stayed as they were (owner, 2026-10-30).
static _Atomic bool g_miss_demo;
void endless_miss_demo(void) {
    if (!g_miss_demo) MLOG("miss demo: a skip now costs the life\n");
    g_miss_demo = true;
}

WRAPPER(start_course, 0x14ecf30) {
    uint32_t d = req_diff(no, NO_DIFF);
    uint64_t id = g64(no + NO_COURSE_ID);
    pthread_mutex_lock(&state_lock);
    bool miss = runs[d].started == id && id;
    runs[d].started = id;
    pthread_mutex_unlock(&state_lock);
    g_miss_demo = false;  // (a new course, or the miss posted: either way no skip owes a life now)
    char code[12];
    code_of(id, code);
    if (!miss) {  // a new course: the server's lives and clears as they are
        Json *st = srv_call("GET", "/api/endless", NULL, NULL);
        // (this mode's slot: "hundred_runs" for 100 Mario on a server with both; an older one has "runs" only)
        Json *slots = json_get(st, "runs");
        if (hundred_rules() && json_get(st, "hundred_runs")) slots = json_get(st, "hundred_runs");
        Json *run = json_get(slots, DIFF[d]);
        bool ok = run != NULL;
        if (ok) set_play(c, no, d, (uint32_t)json_num(json_get(run, "lives"), 0), (uint32_t)json_num(json_get(run, "clears"), 0));
        pthread_mutex_lock(&state_lock);
        runs[d].current = id;
        pthread_mutex_unlock(&state_lock);
        bridge_event("start", d);
        if (g_mod.preload && !cp_next_known((uint32_t)id)) prefetch_data(d);  // (past the list: the server's next)
        LOG("[mod] course start %s (%s): lives %u\n", code, DIFF[d], lives_now(no, d));
        json_free(st);
        finish(c, no, ok, 2);
        return;
    }
    // co-op: the game asks this for any player's miss (its Endless code knows one player); SMM2's local multiplayer
    // rule is that the course is only lost when every player is down (owner, 2026-10-21): while one stands, no life
    if (coop_in_course()) {
        int up = coop_players_standing();
        if (up > 0) {
            pthread_mutex_lock(&state_lock);
            uint32_t lives = runs[d].lives, clears = runs[d].clears;
            pthread_mutex_unlock(&state_lock);
            set_play(c, no, d, lives, clears);
            LOG("[mod] co-op: a player is down on %s, %d still standing: no life lost (lives %u)\n", code, up, lives);
            finish(c, no, true, 2);
            return;
        }
    }
    char path[64], body[160];
    snprintf(path, sizeof path, "/api/endless/%s/result?ruleset=%s", DIFF[d], RULESET());
    snprintf(body, sizeof body, "{\"code\":\"%s\",\"result\":\"%s\"}", code,
             bridge_take_start_over() ? "start_over" : "death");
    Json *j = post_result(path, body, d, code);
    if (j) {
        set_play(c, no, d, (uint32_t)json_num(json_get(j, "lives"), 0), (uint32_t)json_num(json_get(j, "clears"), 0));
        bridge_event("miss", d);
        LOG("[mod] miss on %s: lives %u%s\n", code, lives_now(no, d), json_bool(json_get(j, "game_over"), false) ? " (game over)" : "");
    }
    json_free(j);
    finish(c, no, j != NULL, 2);
}

// A course result the server refuses as not part of the run (the game played a course the run no longer has: v0.5.4's
// castle course could take the place of one already handed out, 2026-10-25): rather than a communication error that
// stops the run, the run as the server has it (lives, clears) is taken and play goes on; the game's next request gets
// the server's next course. Any other failure is a failure.
static Json *post_result(const char *path, const char *body, uint32_t d, const char *code) {
    int st = 0;
    Json *j = srv_call("POST", path, body, &st);
    if (j || st != 400) return j;
    Json *all = srv_call("GET", "/api/endless", NULL, NULL);
    Json *run = json_get(json_get(all, strcmp(RULESET(), "hundred") ? "runs" : "hundred_runs"), DIFF[d]);
    if (!run || !json_bool(json_get(run, "active"), true)) { json_free(all); return NULL; }
    char again[96];
    int n = snprintf(again, sizeof again, "{\"lives\":%u,\"clears\":%u}", (uint32_t)json_num(json_get(run, "lives"), 0),
                     (uint32_t)json_num(json_get(run, "clears"), 0));
    LOG("[mod] result for %s refused (not in this run): going on with the server's run (%s)\n", code, again);
    json_free(all);
    return json_parse(again, (size_t)n);
}

// The players at the goal (read when the clear is posted), for the ending: the player actors at *(main+0x2c19290) +
// 0x28 + 8 * slot (one, or in co-op one a player), each one's form at +0x4a8 (SMM2's enum "Normal, Super, Fire, ...,
// USA"), and what it rides: +0x4c0 set in a Goomba's Shoe or on Yoshi, +0x4c4 for the other kind (Stiletto / Red
// Yoshi) [obs v3.0.3 0x7101579290, the body-box choice: its shoe / Yoshi boxes by these bytes; inf: which byte is
// which kind]. False when no player is there.
static bool player_look(struct hundred_look *out) {
    uint64_t holder = g64(MAIN_ADDR(0x2c19290));
    if (!holder) return false;
    memset(out, 0, sizeof *out);
    for (int i = 0; i < 4; i++) {
        uint64_t pl = g64(holder + 0x28 + 8ull * (uint64_t)i);
        if (!pl) continue;
        uint32_t form = g32(pl + 0x4a8);
        out->form[i] = form < 16 ? (int)form : 1;
        out->mount[i] = !g8(pl + 0x4c0) ? 0 : g8(pl + 0x4c4) ? 2 : 1;
        out->players = i + 1;
    }
    return out->players > 0;
}

// ---- DominateEndlessModeCourse: a clear (pending 1-Ups, coins, score with the time bonus)

WRAPPER(clear_course, 0x14ed120) {
    uint32_t d = req_diff(no, NO_DIFF);
    uint64_t id = g64(no + NO_CLEAR_ID);
    uint32_t pending = g8(no + NO_PENDING), coins = g8(no + NO_COINS), score = g32(no + NO_SCORE);
    struct hundred_look look;
    bool have_look = player_look(&look);
    pthread_mutex_lock(&state_lock);
    uint32_t dcoins = (coins + 100 - runs[d].coins % 100) % 100;  // (the game sends coins below 100)
    uint32_t dscore = score >= runs[d].score ? score - runs[d].score : 0;
    pthread_mutex_unlock(&state_lock);
    char code[12], path[64], body[256];
    code_of(id, code);
    snprintf(path, sizeof path, "/api/endless/%s/result?ruleset=%s", DIFF[d], RULESET());
    snprintf(body, sizeof body,
             "{\"code\":\"%s\",\"result\":\"clear\",\"pending\":%u,\"coins\":%u,\"score\":%u,\"frames_left\":0}", code,
             pending, dcoins, dscore);
    Json *j = post_result(path, body, d, code);
    if (j) {
        uint32_t lives = (uint32_t)json_num(json_get(j, "lives"), 0), clears = (uint32_t)json_num(json_get(j, "clears"), 0);
        bool complete = json_bool(json_get(j, "run_complete"), false);
        set_play(c, no, d, lives, clears);
        pthread_mutex_lock(&state_lock);
        runs[d].coins += dcoins, runs[d].score += dscore, runs[d].started = 0, runs[d].last_done = id;
        if (have_look) {
            look.theme = -1;
            for (int i = 0; i < ncourse_names; i++)
                if (course_names[i].id == id) look.theme = course_names[i].theme;
            runs[d].look = look;
        }
        add_credit(d, id);
        if (complete) runs[d].complete = true;
        pthread_mutex_unlock(&state_lock);
        bridge_event(complete ? "clear_last" : "clear", d);
        if (!complete) endless_prefetch((int)d);
        LOG("[mod] clear %s (%s): lives %u, clears %u, pending %u, players %d (form %d, mount %d), theme %d%s\n", code,
            DIFF[d], lives, clears, pending, have_look ? look.players : 0, have_look ? look.form[0] : -1,
            have_look ? look.mount[0] : -1, have_look ? look.theme : -1, complete ? " -- 100 Mario Challenge complete!" : "");
    }
    json_free(j);
    finish(c, no, j != NULL, 2);
}

// ---- PassEndlessModeCourse: a skip (free under SMM1's rules; the server knows the ruleset)

WRAPPER(skip_course, 0x14ed5e0) {
    uint32_t d = req_diff(no, NO_DIFF);
    uint64_t id = g64(no + NO_COURSE_ID);
    char code[12], path[64], body[160];
    code_of(id, code);
    snprintf(path, sizeof path, "/api/endless/%s/result?ruleset=%s", DIFF[d], RULESET());
    bool during_miss = g_miss_demo;
    g_miss_demo = false;
    snprintf(body, sizeof body, "{\"code\":\"%s\",\"result\":\"skip\",\"during_miss\":%s}", code,
             during_miss ? "true" : "false");
    Json *j = post_result(path, body, d, code);
    if (j) {
        // (the lives as the server has them now: a skip during the miss demo cost one)
        uint32_t lives = (uint32_t)json_num(json_get(j, "lives"), 0);
        set_play(c, no, d, lives, (uint32_t)json_num(json_get(j, "clears"), 0));
        run_lives_set(lives);
        pthread_mutex_lock(&state_lock);
        runs[d].started = 0;
        pthread_mutex_unlock(&state_lock);
        bridge_event("skip", d);
        endless_prefetch((int)d);
        LOG("[mod] skip %s (%s): lives %u%s\n", code, DIFF[d], (uint32_t)json_num(json_get(j, "lives"), 0),
            during_miss ? " (during the miss: the life counted)" : "");
    }
    json_free(j);
    finish(c, no, j != NULL, 2);
}

// ---- SuspendEndlessMode (quit, run kept) and FinishEndlessMode (game over)

static void post_simple(Cpu *c, uint64_t no, const char *what) {
    uint32_t d = req_diff(no, NO_DIFF);
    char path[64];
    snprintf(path, sizeof path, "/api/endless/%s/%s?ruleset=%s", DIFF[d], what, RULESET());
    Json *j = srv_call("POST", path, "{}", NULL);
    if (j) {
        pthread_mutex_lock(&state_lock);
        runs[d].started = 0;
        pthread_mutex_unlock(&state_lock);
        LOG("[mod] %s (%s)\n", what, DIFF[d]);
    }
    json_free(j);
    finish(c, no, j != NULL, 2);
}

WRAPPER(suspend_run, 0x14ed360) {
    if (endless_run_complete(req_diff(no, NO_DIFF))) {  // (a won run left by its result screen's Quit: nothing to keep)
        LOG("[mod] suspend of a won run (%s): answered locally\n", DIFF[req_diff(no, NO_DIFF)]);
        finish(c, no, true, 0);
        bridge_event("suspend", req_diff(no, NO_DIFF));
        return;
    }
    post_simple(c, no, "suspend");
    bridge_event("suspend", req_diff(no, NO_DIFF));
}
WRAPPER(finish_run, 0x14ed4a0) { post_simple(c, no, "finish"); }

// The run as the flow sees it (hundred.h hundred_run_status): from the last server answers, no network here.
void endless_fill_status(struct hundred_status *out, int active_d) {
    memset(out, 0, sizeof *out);
    pthread_mutex_lock(&state_lock);
    out->difficulty = active_d;
    out->loading = loading > 0;
    for (int d = 0; d < 4; d++) {
        out->unlocked[d] = difficulty_open(d);
        out->can_continue[d] = runs[d].active && runs[d].lives > 0 && !runs[d].complete;
        out->continue_lives[d] = (int)runs[d].lives;
        out->continue_cleared[d] = (int)runs[d].clears;
    }
    if (active_d >= 0 && active_d < 4) {
        static const int totals[4] = {8, 16, 16, 6};
        out->lives = (int)runs[active_d].lives;
        out->cleared = (int)runs[active_d].clears;
        out->total = !hundred_rules() ? 0 : runs[active_d].goal ? (int)runs[active_d].goal : totals[active_d];
        out->ncredits = runs[active_d].ncredits < 16 ? runs[active_d].ncredits : 16;
        out->look = runs[active_d].look;
        memcpy(out->credits, runs[active_d].credits, sizeof out->credits);
        uint64_t id = runs[active_d].current;
        if (id) {
            data_id_to_code((uint32_t)id, out->current_code);
            for (int i = 0; i < ncourse_names; i++)
                if (course_names[i].id == id) out->current = course_names[i].c, out->style = course_names[i].style;
        }
    }
    pthread_mutex_unlock(&state_lock);
}

// The run is over (hundred_run_finish): forget its client side state.
void endless_forget_run(int d) {
    if (d < 0 || d > 3) return;
    pthread_mutex_lock(&state_lock);
    memset(&runs[d], 0, sizeof runs[d]);
    pthread_mutex_unlock(&state_lock);
    cp_clear("run over");
}

bool endless_has_course(uint64_t id) {  // world.c: is this course the one an Endless run is on
    pthread_mutex_lock(&state_lock);
    bool v = false;
    for (int d = 0; d < 4; d++) v |= id && (runs[d].current == id || runs[d].started == id || runs[d].last_done == id);
    pthread_mutex_unlock(&state_lock);
    return v;
}

// hooks.c (co-op): the style of the course a run is on (style_index: 0 SMB1 .. 4 3D World), -1 for no run's course,
// -2 for one whose style wasn't served with it
int endless_course_style(void) {
    pthread_mutex_lock(&state_lock);
    int v = -1;
    for (int d = 0; d < 4 && v == -1; d++)
        if (runs[d].current) {
            v = -2;
            for (int i = 0; i < ncourse_names; i++)
                if (course_names[i].id == runs[d].current) v = course_names[i].style;
        }
    pthread_mutex_unlock(&state_lock);
    return v;
}

bool endless_run_complete(uint32_t d) {
    pthread_mutex_lock(&state_lock);
    bool v = d < 4 && runs[d].complete;
    pthread_mutex_unlock(&state_lock);
    return v;
}

// The play scene's moments around a course change, for the log (timing; [obs research/specs/endless-challenge.md 4.3]):
// cSlidePass's enter (the skip is posted there) and the scene's exit (to the next course after a skip: scene 0x26).
// (cCourseListDownLoad / cCourseDataDownLoad: bridge.c's overrides log theirs.)
static void slide_pass_enter(Cpu *c) {
    mod_timing("state cSlidePass (skip)");
    guest_call_orig(c, MAIN_ADDR(0x1bbb3f0));
}
static void play_scene_exit(Cpu *c) {
    mod_timing("play scene exit");
    guest_call_orig(c, MAIN_ADDR(0x1bb9a10));
}

void endless_register(void) {
    cp_name = data_id_to_code;  // (cp_verbose: settings.c, once they are read)
    t_base = plat_now_ns();
    hle_override(MAIN_ADDR(0x1bbb3f0), slide_pass_enter, "mod: Endless play cSlidePass (timing)");
    hle_override(MAIN_ADDR(0x1bb9a10), play_scene_exit, "mod: Endless play scene exit (timing)");
    hle_override(MAIN_ADDR(0x14ec2d0), get_status, "mod: GetEndlessModeStatus");
    hle_override(MAIN_ADDR(0x1a223e0), tab_expert, "mod: Endless tab Expert");
    hle_override(MAIN_ADDR(0x1a224a0), tab_super_expert, "mod: Endless tab Super Expert");
    hle_override(MAIN_ADDR(0x14ece20), init_run, "mod: InitEndlessMode");
    hle_override(MAIN_ADDR(0x14ed750), get_play_info, "mod: GetEndlessModePlayInfo");
    hle_override(MAIN_ADDR(0x14eddc0), get_rank, "mod: GetEndlessModeRank");
    hle_override(MAIN_ADDR(0x14d8550), search_courses, "mod: SearchCourses (Endless)");
    hle_override(MAIN_ADDR(0x14de010), get_course_file, "mod: GetObject (course file)");
    hle_override(MAIN_ADDR(0x14ec0d0), course_list_info, "mod: course list info");
    hle_override(MAIN_ADDR(0x14f17a0), id_check, "mod: id check");
    hle_override(MAIN_ADDR(0x14e8ec0), course_comments, "mod: course comments");
    hle_override(MAIN_ADDR(0x14dbae0), get_courses, "mod: GetCourses (by id)");
    hle_override(MAIN_ADDR(0x14f2910), course_post, "mod: course post");
    hle_override(MAIN_ADDR(0x14ecf30), start_course, "mod: StartEndlessModeCourse");
    hle_override(MAIN_ADDR(0x14ed120), clear_course, "mod: DominateEndlessModeCourse");
    hle_override(MAIN_ADDR(0x14ed5e0), skip_course, "mod: PassEndlessModeCourse");
    hle_override(MAIN_ADDR(0x14ed360), suspend_run, "mod: SuspendEndlessMode");
    hle_override(MAIN_ADDR(0x14ed4a0), finish_run, "mod: FinishEndlessMode");
}

void endless_note_course(uint64_t data_id, const char *code) { (void)data_id, (void)code; }
