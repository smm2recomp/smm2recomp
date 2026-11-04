// SMM1 title letters: actors on the title course (title_letters.h). The title course is SMM2's own course play, so a
// letter's Goomba, coin or Clown Car is SMM2's own actor, made with the game's general create (main+0xe50830: an area
// id and 0x78 bytes of create args, filled as the course loader and the Bill Blaster fill theirs) in the area the
// player is in, at a depth in the course loader's bands. Addresses v3.0.3; research in research/specs/
// smm2-title-scene.md (private). Actor ids are v3.0.3's own (the profile table was renumbered after v1.0.0: id 87,
// v1.0.0's Goomba, is the Superball in v3.0.3).
//
// SMM1's limits (smm1-title-letters.md): fewer than 100 live enemy-type spawns (Goombas, Koopas, Bowsers, Clown Cars,
// shells, Trampolines, Yoshi's Eggs), fewer than 100 items (Stars, the Mystery Mushroom, the Shoe), at most 100 coins;
// counted over the actors this mod made that are still in the area's live list. The background tap: a Goomba
// (Galoomba in SMW) or, half the time, a Koopa; Bowser instead with a chance of 1 in 50, or for sure on the 30th tap
// of a cycle of 30 that had none, while fewer than 3 are alive; 20 frames between taps.
//
// Defeating: SMM2's own attack on each enemy on screen (the enemy's receive-attack, attack "star": what a Super Star's
// touch does, each enemy reacting as its EnemyDB row says). Clearing: SMM2's own death with the "vanish" reaction
// (the puff an enemy leaves when it is erased) for every enemy, item and object of the enemy kind on screen.
//
// Called from the title's state hooks only (the game thread, between the game's state function and its return):
// tl_spawn_frame() gives this file the guest CPU for the frame, tl_spawn_done() takes it back.
// Tests: RECOMP_TITLE_LETTERS_SPAWN=<kind>[:<vx>:<vy>],... (TlActor numbers): background taps make these in turn;
// RECOMP_TITLE_LETTERS_DO=defeat@<n>,clear@<n>: tl_defeat_enemies / tl_clear_screen at title frame n.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../mod.h"
#include "title_letters.h"

#define CREATE_IN_AREA MAIN_ADDR(0xe50830)  // (area id, create args) -> the actor, or 0 (not made / parked)
#define PROFILES MAIN_ADDR(0x2a560f0)       // the actor profiles, 8 bytes per id (+0x10 id, +0x14 kind: 1 enemy,
#define NPROFILES 0x1d0                     //   2 item, 3 object, 0 player / Yoshi, 5 system)
#define SIZE_DEFAULT MAIN_ADDR(0x233a404)   // the create args' default width / height (1, 1)
#define PLAYERS MAIN_ADDR(0x2c19290)        // +0x28: player 1
#define CAMERAS MAIN_ADDR(0x2c55080)        // +0x88: the view (+0x0c left, +0x10 bottom, +0x14 right, +0x18 top)
#define PLAY_INFO MAIN_ADDR(0x2c57d58)      // -> +0x30: +0x1c the course's style
#define ENEMY_ATTACKED MAIN_ADDR(0x128f500) // the enemy base's receive-attack: (enemy, type, attacker, direction,
                                            //   float pos[2], 0) -> handled
#define ENEMY_DIE MAIN_ADDR(0x128e8f0)      // the enemy base's death: (enemy, death info)
#define ENEMY_STATES_VT MAIN_ADDR(0x2850b88) // the enemy base's state machine (at +0x3f8): its vtable [run: Goomba,
                                            //   Koopa, Bowser, Coin, JumpStep, Clown Car, shells, Shoe, Star]
#define DEATH_INFO_VT MAIN_ADDR(0x28b4558)  // the death info's vtable, as the receive-attack builds it
#define ATTACK_STAR 5                       // EnemyDB attack columns: stamp, stamp_spin, ..., fire, star (5)
#define REACT_VANISH 2                      // reactions: DieFall, DieStamp, DieVanish (2), ...
#define A_PROFILE 0x38                      // actor: its profile [run]
#define A_AREA 0x48                         // actor: its area (the actor manager's; +0x20 the area's id)
#define A_SERIAL 0x30                       // actor: its number, new for each actor made [run]
#define A_PARKED 0x52                       // actor: set when it leaves the live list
#define A_POS 0x230                         // actor: x, y, z (course px, y up; feet); +0x23c / +0x240 vx, vy [run]
#define AREA_ID 0x20
#define AREA_LIVE 0x118                     // area: its actors, a list {prev, next} at +0x118, count +0x128, the
                                            //   link's offset in an actor +0x12c [bin 0x7100d642e0] (+0x48: those
                                            //   made this frame)
#define MAX_TRACK 320

static bool on_heap(uint64_t a) { return a >= HEAP_BASE && a < HEAP_BASE + HEAP_MAX - 0x1000 && !(a & 7); }
static float g32f(uint64_t a) { float f; uint32_t w = g32(a); memcpy(&f, &w, 4); return f; }
static void s32f(uint64_t a, float f) { uint32_t w; memcpy(&w, &f, 4); s32(a, w); }

bool costumes_on(void);  // costume.c: the Mystery Mushroom costumes mod is on

// ---- the kinds: v3.0.3 actor ids per style (SMB1, SMB3, SMW, NSMBU, 3D World; -1: the style has none), SMM1's
// count (A enemy-type, B item, C coin) and the course-object flags beyond the default 0x06000040 (0x4 alternate)
typedef struct { const char *name; char count; int16_t id[5]; uint32_t flags; } Kind;
static const Kind kinds[TA_COUNT] = {
    [TA_GOOMBA] = {"Goomba", 'A', {95, 95, 99, 95, 97}, 0},  // (Kuribo; Kuribon, the Galoomba; Kuribo3W)
    [TA_KOOPA] = {"Koopa", 'A', {105, 105, 105, 105, 105}, 0},  // (NokonokoGreen)
    [TA_BOWSER] = {"Bowser", 'A', {195, 197, 199, 201, 203}, 0},  // (KoopaM1 / M3 / MW / WU, Koopa3W)
    [TA_COIN] = {"coin", 'C', {75, 75, 75, 75, 75}, 0},  // (Coin: the actor coin)
    [TA_TRAMPOLINE] = {"Trampoline", 'A', {48, 48, 48, 48, 48}, 0},  // (JumpStep)
    [TA_CLOWN_CAR] = {"Koopa Clown Car", 'A', {80, 80, 80, 80, -1}, 0},  // (KoopaClown: NSMBU's is Junior's)
    [TA_BUZZY_SHELL] = {"Buzzy Shell", 'A', {56, 56, 56, 56, -1}, 0x4},  // (Met, alternate: the shell; no 3W model)
    [TA_SPINY_SHELL] = {"Spiny Shell", 'A', {138, 138, 138, 138, 138}, 0x4},  // (Togezo, alternate)
    [TA_SHOE] = {"Goomba's Shoe", 'B', {252, 252, -1, -1, -1}, 0},  // (Shoes: the empty shoe)
    [TA_YOSHI_EGG] = {"Yoshi's Egg", 'A', {-1, -1, 267, 267, -1}, 0},  // (YoshiEgg)
    [TA_MYSTERY_MUSHROOM] = {"Mystery Mushroom", 'B', {253, -1, -1, -1, -1}, 0},  // (BigKinoko: the costumes mod's)
    [TA_STAR] = {"Super Star", 'B', {244, 244, 244, 244, 244}, 0},  // (SuperStar)
};
const char *tl_actor_name(int actor) { return actor >= 0 && actor < TA_COUNT ? kinds[actor].name : "?"; }

static Cpu *g_cpu;   // this frame's guest CPU (tl_spawn_frame), NULL outside the title's hooks
static int g_log = -1;
static struct { uint64_t actor, serial; int kind; } g_track[MAX_TRACK];  // the actors made here, while alive
static int g_ntrack;
static uint64_t g_frame, g_last_tap;  // title frames; the last background tap that spawned
static int g_taps, g_bowser_cycle;    // background taps in this cycle of 30; a Bowser came in it
static uint64_t g_rng;

static uint32_t rnd(uint32_t n) {  // (xorshift; 0 .. n-1)
    if (!g_rng) g_rng = (uint64_t)time(NULL) * 0x9e3779b97f4a7c15ull | 1;
    g_rng ^= g_rng << 13, g_rng ^= g_rng >> 7, g_rng ^= g_rng << 17;
    return (uint32_t)((g_rng >> 11) % n);
}

int tl_game_style(void) {
    uint64_t g = g64(PLAY_INFO), pi = on_heap(g) ? g64(g + 0x30) : 0;
    if (!on_heap(pi)) return -1;
    uint32_t s = g32(pi + 0x1c);
    return s <= 4 ? (int)s : -1;
}

static uint64_t player1(void) {
    uint64_t pl = g64(PLAYERS), p = on_heap(pl) ? g64(pl + 0x28) : 0;
    return on_heap(p) ? p : 0;
}

static uint64_t player_area(void) {
    uint64_t p = player1(), a = p ? g64(p + A_AREA) : 0;
    return on_heap(a) ? a : 0;
}

static bool view(float *l, float *b, float *r, float *t) {
    uint64_t h = g64(CAMERAS), v = on_heap(h) ? g64(h + 0x88) : 0;
    if (!on_heap(v)) return false;
    *l = g32f(v + 0xc), *b = g32f(v + 0x10), *r = g32f(v + 0x14), *t = g32f(v + 0x18);
    return *r - *l > 1 && *t - *b > 1;
}

// the area's live actors, each through its list link (at +offset in the actor)
static int live_actors(uint64_t area, uint64_t *out, int max) {
    if (!area) return 0;
    uint64_t head = area + AREA_LIVE;
    int32_t off = (int32_t)g32(area + AREA_LIVE + 0x14), n = 0, guard = 0;
    for (uint64_t l = g64(head + 8); on_heap(l) && l != head && n < max && guard < 8192; l = g64(l + 8), guard++)
        out[n++] = l - (uint64_t)(int64_t)off;
    return n;
}

static bool main_ptr(uint64_t p) { return p >= MAIN_ADDR(0) && p < MAIN_ADDR(0x2e74000); }
static int profile_kind(uint64_t actor) {  // (the profile's kind, -1 unknown)
    uint64_t pr = g64(actor + A_PROFILE);
    return main_ptr(pr) ? (int)g32(pr + 0x14) : -1;
}

// an actor of the enemy base (enemies, items, coins, springs ...): the base's state machine at +0x3f8
static bool enemy_base(uint64_t actor) { return g64(actor + 0x3f8) == ENEMY_STATES_VT; }

static bool on_screen(uint64_t actor, float margin) {
    float l, b, r, t, x = g32f(actor + A_POS), y = g32f(actor + A_POS + 4);
    return view(&l, &b, &r, &t) && x >= l - margin && x <= r + margin && y >= b - margin && y <= t + margin;
}

static void guest_save(Cpu *c, Cpu *save) { memcpy(save, c, sizeof *save); c->sp = (c->sp - 0x200) & ~0xfull; }
static void guest_restore(Cpu *c, const Cpu *save) { memcpy(c, save, sizeof *save); }

// ---- the create
// depth: the course loader's bands by the profile's draw flags (main+0xe4f140), each spawn a step nearer
static float depth_for(uint64_t prof) {
    static int step;
    uint64_t d = g64(prof + 0x18);
    uint32_t f = main_ptr(d) ? g32(d + 0x10) : 0;
    step = (step + 1) % 400;
    if (f & 0x10) return 6360.f + (float)step;
    if (f & 1) return 8113.f - (float)step;
    return 10865.f - (float)step;
}

static uint64_t create(uint64_t area, int id, float x, float y, uint32_t flags) {
    Cpu *c = g_cpu, save;
    uint64_t prof = id >= 0 && id < NPROFILES ? g64(PROFILES + 8ull * (uint64_t)id) : 0;
    if (!c || !area || !main_ptr(prof) || g32(prof + 0x10) != (uint32_t)id) return 0;
    guest_save(c, &save);
    uint64_t a = c->sp;
    memset((void *)(uintptr_t)a, 0, 0x78);
    s64(a + 0x10, prof);
    s32f(a + 0x18, x), s32f(a + 0x1c, y), s32f(a + 0x20, depth_for(prof));
    s64(a + 0x40, g64(SIZE_DEFAULT));
    s32(a + 0x48, 0x6000040 | flags);
    s32(a + 0x58, 0x6000040);
    s32(a + 0x5c, 0xffffffff);
    c->x[0] = g32(area + AREA_ID), c->x[1] = a;
    guest_call(c, CREATE_IN_AREA);
    uint64_t actor = c->x[0];
    guest_restore(c, &save);
    return on_heap(actor) ? actor : 0;
}

static bool tracked_alive(int i, const uint64_t *live, int nlive) {
    uint64_t a = g_track[i].actor;
    if (g8(a + A_PARKED) || g64(a + A_SERIAL) != g_track[i].serial) return false;
    for (int k = 0; k < nlive; k++)
        if (live[k] == a) return true;
    return false;
}

static int counted(char count) {
    int n = 0;
    for (int i = 0; i < g_ntrack; i++) n += kinds[g_track[i].kind].count == count;
    return n;
}

static int alive_of(int kind) {
    int n = 0;
    for (int i = 0; i < g_ntrack; i++) n += g_track[i].kind == kind;
    return n;
}

bool tl_spawn(int actor, float sx, float sy, float vx, float vy) {
    int style = tl_game_style();
    float l, b, r, t;
    if (actor < 0 || actor >= TA_COUNT || style < 0 || !g_cpu || !view(&l, &b, &r, &t)) return false;
    const Kind *k = &kinds[actor];
    int id = k->id[style];
    const char *why = NULL;
    if (id < 0) why = "this style has none";
    else if (actor == TA_MYSTERY_MUSHROOM && !costumes_on()) why = "the Mystery Mushroom costumes mod is off";
    else if (k->count == 'C' ? counted('C') > 100 : counted(k->count) >= 100) why = "SMM1's limit";
    else if (g_ntrack >= MAX_TRACK) why = "too many";
    uint64_t area = player_area(), a = 0;
    float x = l + sx * (r - l) / 1280.f, y = t - sy * (t - b) / 720.f;
    if (!why && !(a = create(area, id, x, y, k->flags))) why = "the game made none";
    if (g_log)
        LOG("[title] spawn %s (id %d, style %d) at %.0f,%.0f = course %.1f, %.1f: %s\n", k->name, id, style, sx, sy, x,
            y, why ? why : "made");
    if (why) return false;
    if (vx != 0 || vy != 0) s32f(a + A_POS + 0xc, vx), s32f(a + A_POS + 0x10, vy);
    g_track[g_ntrack].actor = a, g_track[g_ntrack].serial = g64(a + A_SERIAL), g_track[g_ntrack].kind = actor;
    g_ntrack++;
    return true;
}

// ---- the background tap
void tl_background_tap(float sx, float sy) {
    if (g_last_tap && g_frame - g_last_tap < 20) {
        if (g_log) LOG("[title] background tap at %.0f,%.0f: too soon\n", sx, sy);
        return;
    }
    const char *forced = getenv("RECOMP_TITLE_LETTERS_SPAWN");
    int kind;
    float vx = 0, vy = 0;
    static int nforced;
    if (forced && *forced) {  // (a list: one tap each, in turn)
        int n = 0;
        for (const char *f = forced; *f; f++) n += *f == ',';
        const char *f = forced;
        for (int k = nforced++ % (n + 1); k > 0; k--) f = strchr(f, ',') + 1;
        kind = atoi(f) % TA_COUNT;
        const char *v = strchr(f, ':'), *e = strchr(f, ',');
        if (v && (!e || v < e)) vx = strtof(v + 1, (char **)&v), vy = *v == ':' ? strtof(v + 1, NULL) : 0;
    } else {
        kind = rnd(10) & 1 ? TA_KOOPA : TA_GOOMBA;  // (SMM1: odd tenths, a Koopa)
        bool lucky = rnd(50) == 3;
        if (alive_of(TA_BOWSER) < 3 && !g_bowser_cycle && (lucky || g_taps >= 29)) kind = TA_BOWSER, g_bowser_cycle = 1;
    }
    if (counted('A') >= 100 && kinds[kind].count == 'A') {
        if (g_log) LOG("[title] background tap at %.0f,%.0f: SMM1's limit\n", sx, sy);
        return;
    }
    g_last_tap = g_frame;
    int tap = g_taps + 1;
    if (!forced && ++g_taps >= 30) g_taps = 0, g_bowser_cycle = 0;
    bool ok = tl_spawn(kind, sx, sy, vx, vy);
    LOG("[title] background tap at %.0f,%.0f: %s%s (tap %d of 30; %d Bowsers, %d enemy-type alive)\n", sx, sy,
        kinds[kind].name, ok ? "" : " (not made)", tap, alive_of(TA_BOWSER), counted('A'));
}

// ---- defeating and clearing
static int screen_actors(uint64_t *out, int max) {
    uint64_t live[1024], p = player1();
    int n = live_actors(player_area(), live, 1024), m = 0;
    for (int i = 0; i < n && m < max; i++)
        if (live[i] != p && !g8(live[i] + A_PARKED) && on_screen(live[i], 16)) out[m++] = live[i];
    return m;
}

void tl_defeat_enemies(void) {
    Cpu *c = g_cpu, save;
    uint64_t act[512], p = player1();
    int n = c ? screen_actors(act, 512) : 0, done = 0;
    for (int i = 0; i < n; i++) {
        uint64_t a = act[i];
        if (profile_kind(a) != 1 || !enemy_base(a)) continue;
        float x = g32f(a + A_POS), y = g32f(a + A_POS + 4);
        guest_save(c, &save);
        uint64_t at = c->sp;
        s32f(at, x), s32f(at + 4, y);
        c->x[0] = a, c->x[1] = ATTACK_STAR, c->x[2] = 0;
        c->x[3] = p && g32f(p + A_POS) > x;  // (the hit's side: away from Mario)
        c->x[4] = at, c->x[5] = 0;
        guest_call(c, ENEMY_ATTACKED);
        guest_restore(c, &save);
        done++;
    }
    if (g_log) LOG("[title] defeat: %d of %d actors on screen\n", done, n);
}

void tl_clear_screen(void) {
    Cpu *c = g_cpu, save;
    uint64_t act[512];
    int n = c ? screen_actors(act, 512) : 0, done = 0;
    for (int i = 0; i < n; i++) {
        uint64_t a = act[i];
        int kind = profile_kind(a);
        if (kind < 1 || kind > 3 || !enemy_base(a)) continue;  // (enemies, items, objects; not the player, Yoshi)
        guest_save(c, &save);
        uint64_t info = c->sp;  // (the death info: vtable, attacker, reaction, attack, direction, x, y, flags, 0)
        memset((void *)(uintptr_t)info, 0, 0x30);
        s64(info, DEATH_INFO_VT);
        s32(info + 0x10, REACT_VANISH), s32(info + 0x14, 0xffffffff);
        s32f(info + 0x1c, g32f(a + A_POS)), s32f(info + 0x20, g32f(a + A_POS + 4));
        c->x[0] = a, c->x[1] = info;
        guest_call(c, ENEMY_DIE);
        guest_restore(c, &save);
        done++;
    }
    if (g_log) LOG("[title] clear: %d of %d actors on screen\n", done, n);
}

// ---- each title frame
void tl_spawn_frame(struct Cpu *c) {
    if (g_log < 0) g_log = getenv("RECOMP_TITLE_LETTERS_LOG") != NULL;
    g_cpu = c;
    g_frame++;
    static const char *todo = (const char *)1;  // (tests: RECOMP_TITLE_LETTERS_DO="defeat@<frame>,clear@<frame>")
    if (todo == (const char *)1) todo = getenv("RECOMP_TITLE_LETTERS_DO");
    for (const char *s = todo; s && *s; s = strchr(s, ',') ? strchr(s, ',') + 1 : "") {
        const char *at = strchr(s, '@');
        if (!at || strtoull(at + 1, NULL, 10) != g_frame) continue;
        LOG("[title] test: %.*s at title frame %llu\n", (int)(at - s), s, (unsigned long long)g_frame);
        if (!strncmp(s, "defeat", 6)) tl_defeat_enemies();
        else if (!strncmp(s, "clear", 5)) tl_clear_screen();
    }
    if (!g_ntrack) return;
    uint64_t live[1024];
    int nlive = live_actors(player_area(), live, 1024), kept = 0;
    for (int i = 0; i < g_ntrack; i++)
        if (tracked_alive(i, live, nlive)) g_track[kept++] = g_track[i];
        else if (g_log) LOG("[title] %s gone\n", kinds[g_track[i].kind].name);
    g_ntrack = kept;
}

void tl_spawn_done(void) { g_cpu = NULL; }

void tl_spawn_reset(void) {  // (the actors go with the title course)
    g_ntrack = 0, g_last_tap = 0, g_taps = 0, g_bowser_cycle = 0;
}
