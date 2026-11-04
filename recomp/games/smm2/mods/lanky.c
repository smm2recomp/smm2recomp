// Lanky Mario: SMM1's Weird Mushroom form (Slim Mario) in SMB1-style courses (research: lanky-smm2-v303.md, the
// v3.0.3 side; smm1-extras-lanky.md, SMM1's behaviour).
//
// A lanky player is Super Mario (form 1) for the game's logic: body, sensors, speeds, gravity, damage (a hit gives
// small Mario, as SMM2's damage does for form 1, SMM1's for Lanky) and the power-up rules (a Super Mushroom is refused,
// a Fire Flower gives Fire). What changes:
//   - Look: the model holder's builder (player_model.c) loads, for the SMB1 style, Slim Mario into the spare form slot
//     8 (Builder: size class 2 and Super's animation class, unused in SMB1) by pointing that slot's file at
//     "Player_SlimMarioMdl" and its FMDL at "Su_mario_model" for the call; the visual slot setter shows slot 8 where the
//     game asks for 1 while the player is lanky, so the grow and damage flickers show small <-> Lanky. The models are
//     made from the player's dumps (smm1/costume_gen.c, generator "lanky": SMM2's Super Mario with SMM1's Slim frames;
//     Luigi's recoloured, Toad and Toadette wear Mario's) and laid over the romfs.
//   - Jump: +0.5 px/frame on a computed ground jump (SMM1's Jump-state entry adds it after the common launch), at the
//     hook point after the launch returns into the Jump state's begin (main+0x15c8d54): the jump request's vy
//     (+0x248c) is NaN for a computed jump. Preset jumps (stomp bounces 3.9095, springs, Yoshi), crouch jumps, spin
//     and shoe jumps don't pass there or keep their own vy.
//   - Fuwa cue: SMM1's 20-frame timer from the take-off with a "rising" flag (cleared when jump is let go, vy <= 1.0 or
//     on the ground); when the timer reads 1 with the flag still set, h_fuyu plays (a held jump, never a tap).
//   - Sounds: SMM2 still ships SMM1's lanky sounds (SLink user M1_Player: power_up_Funny -> M1_HEN_LARGE,
//     jump_3_Funny -> M1_HEN_JUMP, h_fuyu -> M1_HEN_JUMP_FUWA); they are played by name with the game's own call
//     (0x7100666af0, the player's sound user *(*(player + 0x28) + 0x50)), from the player's next update.
//   - Weird Mushrooms (SMM1's easter egg, smm1-extras-lanky.md §5.2): each Super Mushroom that comes out of a block or
//     a container in an SMB1 course counts (its create call, 0x71013547c0, overridden: the create args' spawn kind,
//     byte 2 of +0x3c, is 1 for a mushroom placed in the course and 4 for one out of a ? block [obs at run time]; 9 is
//     the other course-object kind of the spawner 0x7100e4f140); while fewer than 100 were counted and none was weird
//     yet, each is weird with a chance of 1 in 20; the 100th is weird if none was, and the count starts again. Taking a
//     weird one as small or Super Mario makes the player lanky (the item pickup's form load, main+0x13502c4). The
//     mushroom itself still looks like a Super Mushroom (SMM1's M1_Item_kinokofunny: not done).
//   - "Lanky Mario now" in the overlay menu's Try it now (triggers.c): player 1 becomes lanky (small: through the
//     game's own power-up request, so the grow demo plays; Super: at once). As the Weird Mushroom, only from small or
//     Super Mario (greyed out with another power-up); not in an Endless / 100 Mario run (they keep records) and not
//     outside an SMB1 course being played.
//   - The player's model holders are at +0x2f90 and +0x2f98 (the builder and the setter get them; [obs at run time]),
//     both shown with Slim Mario.
//
// Settings: lanky=1 in smm2mods.ini (or RECOMP_MOD_LANKY=1), the Mods tab's switch; off by default, the models are made
// at the next start. Tests: RECOMP_LANKY=1 makes player 1 lanky 90 updates into each SMB1 course (=<n>: n updates);
// RECOMP_LANKY_WEIRD=1 makes every eligible Super Mushroom weird; RECOMP_LANKY_TRY=<frame> presses "Lanky Mario now" at
// that presented frame (from the player's update); RECOMP_LANKY_DEBUG=1 logs (jumps with their vy and peak height,
// mushrooms, forms, sounds).
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mod.h"
#include "romfs_overlay.h"

typedef struct { const char *label, *desc; bool *value; bool def; } GameModToggle;  // (runtime/game_hooks.c)
void mod_ini_set(const char *key, const char *val);                                 // (settings.c)
extern uint64_t renderer_present_count(void);

#define NAME_TABLE MAIN_ADDR(0x2c17638)   // model file names: SafeString {vtable, char *} [style * 16 + slot]
#define FMDL_TABLE MAIN_ADDR(0x2c17b38)   // FMDL names, the same layout
#define PLAYERS MAIN_ADDR(0x2c19290)      // ptr: the players at +0x28 + 8 * i (i < 4)
#define FORM_REQUEST MAIN_ADDR(0x15a94a0) // (player, int *form): 0 accepted, 1 the same change pending, 2 refused
#define SOUND_PLAY MAIN_ADDR(0x666af0)    // (user, const char *key, flags; x8: the 16-byte handle returned): an SLink
                                          // key by name [obs: it loads [x0 + 0x10], the handle is stored through x8]
#define MUSHROOM_CREATE MAIN_ADDR(0x13547c0)  // the Super Mushroom's create (create args): the new actor
#define SLIM_SLOT 8
#define P_FORM 0x4a8      // the player's form (0 small, 1 Super, 2 Fire, ...)
#define P_PENDING 0x4b0   // the form a change asked for
#define P_HOLDER 0x2f90   // the model holder (the builder's and the setter's x0) [obs at run time]
#define P_HOLDER2 0x2f98  // a second one, built and set with it [obs at run time; its use: ?]
#define P_VY 0x240        // float, up positive
#define P_JUMP_VY 0x248c  // the jump request's vy: NaN = computed from the speed (a ground jump)
#define P_GROUND 0x2308   // bit 0: on the ground
#define P_INPUT 0x550     // pointer to the input block: +0x16 / +0x1e jump held
#define H_STYLE 0x470     // the holder's style (0 SMB1)
#define H_SLOT 0x478      // the holder's visual slot
#define I_KIND 0xd20      // the item's kind (0 Super Mushroom)

// ---------------------------------------------------------------- the setting
static bool g_on, g_shown;
static int g_test, g_weird_test, g_debug;
static void load_once(void) {
    const char *ini = getenv("RECOMP_MOD_INI") ? getenv("RECOMP_MOD_INI") : "smm2mods.ini";
    FILE *f = fopen(ini, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "lanky=", 6)) g_on = atoi(line + 6) != 0;
        fclose(f);
    }
    if (getenv("RECOMP_MOD_LANKY")) g_on = atoi(getenv("RECOMP_MOD_LANKY")) != 0;
    g_test = getenv("RECOMP_LANKY") ? atoi(getenv("RECOMP_LANKY")) : 0;  // (1: 90 updates in; n > 1: n updates in)
    g_weird_test = getenv("RECOMP_LANKY_WEIRD") && atoi(getenv("RECOMP_LANKY_WEIRD"));
    g_debug = getenv("RECOMP_LANKY_DEBUG") && atoi(getenv("RECOMP_LANKY_DEBUG"));
    g_shown = g_on;
    LOG("[lanky] Lanky Mario (Weird Mushroom) %s\n", g_on ? "on" : "off");
}
bool lanky_on(void) {  // (as at start-up: the Slim models are made then)
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, load_once);
    return g_on;
}

int lanky_toggles(GameModToggle *out, int room) {  // (settings.c appends it to the Mods tab)
    static const GameModToggle t = {
        "Lanky Mario (Weird Mushroom)",
        "SMM1's Weird Mushroom in Super Mario Bros. style courses: now and then a Super Mushroom out of a block is a "
        "weird one, and it makes Mario lanky: a higher jump, SMM1's funny sounds, Super Mario otherwise (a hit makes "
        "him small). Also on demand under Try it now. The model comes from your SMM1 dump. Takes effect at the next "
        "start.",
        &g_shown, false};
    if (room < 1) return 0;
    lanky_on();
    out[0] = t;
    return 1;
}
void lanky_toggles_changed(void) {
    static int saved = -1;  // (the switch as last saved; the mod itself follows at the next start)
    if (saved < 0) saved = lanky_on();
    if (g_shown == (saved != 0)) return;
    saved = g_shown;
    mod_ini_set("lanky", g_shown ? "1" : "0");
    LOG("[lanky] Lanky Mario %s (at the next start)\n", g_shown ? "on" : "off");
}

// ---------------------------------------------------------------- per player (by model holder: one per player per course)
typedef struct {
    uint64_t holder, holder2, player;
    bool lanky, rising;
    int timer, test_in;
    uint32_t last_form, last_pending;  // (debug)
    const char *sound[2];  // played at the player's next update
} Lanky;
static Lanky g_l[8];
static int g_next;
static volatile bool g_request[4];          // "Lanky Mario now" (the overlay menu's thread): player i
static volatile uint64_t g_last_update;     // presented frame of the last player update
uint64_t lanky_last_player_update(void) { return g_last_update; }

static Lanky *by_holder(uint64_t holder) {  // (the player's first holder: its state)
    for (int i = 0; i < 8; i++)
        if (holder && g_l[i].holder == holder) return &g_l[i];
    return NULL;
}
static Lanky *by_any_holder(uint64_t holder) {  // (either of the player's holders: the state of the player)
    for (int i = 0; i < 8; i++)
        if (holder && g_l[i].holder2 == holder && g_l[i].player) return &g_l[i];
    return by_holder(holder);
}
// both holders show Slim Mario for Super while lanky, Super again when not
static void show(Lanky *l, uint32_t form) {
    uint64_t h[2] = {l->holder, l->holder2};
    for (int k = 0; k < 2; k++) {
        if (!h[k]) continue;
        uint32_t shown = g32(h[k] + H_SLOT);
        if (l->lanky && form == 1 && shown == 1) s32(h[k] + H_SLOT, SLIM_SLOT);
        if (!l->lanky && shown == SLIM_SLOT) s32(h[k] + H_SLOT, 1);
    }
}
static int player_index(uint64_t player) {
    uint64_t pm = g64(PLAYERS);
    for (int i = 0; pm && i < 4; i++)
        if (g64(pm + 0x28 + 8 * (uint64_t)i) == player) return i;
    return -1;
}
static void queue_sound(Lanky *l, const char *key) {
    if (!l->sound[0]) l->sound[0] = key;
    else if (!l->sound[1]) l->sound[1] = key;
}

// the Slim model is in the overlay (generated now or at an earlier start); looked at once, on the game's thread (the
// menu's thread sees the answer once there is one)
static volatile int g_ready = -1;
static bool model_ready(bool look) {
    if (g_ready < 0 && look) {
        extern const char *game_romfs_overlay(void);
        game_romfs_overlay();  // (generated at the RomFS's opening, with or without RECOMP_MOD_DIR)
        RovSarc *s = rov_sarc_load("Pack/M1_Model.pack");
        int r = 0;
        for (int i = 0; s && rov_sarc_name(s, i); i++) r |= !strcmp(rov_sarc_name(s, i), "Model/M1_Player_SlimMarioMdl.Nin_NX_NVN.zs");
        rov_sarc_store(s, NULL);
        g_ready = r;
        LOG("[lanky] Slim Mario model %s\n", r ? "ready" : "MISSING (no SMM1 dump?): Lanky Mario stays off");
    }
    return g_ready > 0;
}
static bool active(void) { return lanky_on() && model_ready(true); }
static bool active_ui(void) { return lanky_on() && model_ready(false); }  // (the menu's thread)

// ---------------------------------------------------------------- Weird Mushrooms
static uint64_t g_weird[32];
static int g_nweird;
static bool is_weird(uint64_t item) {
    for (int i = 0; i < g_nweird; i++)
        if (g_weird[i] == item) return true;
    return false;
}
static void forget_weird(uint64_t item) {
    for (int i = 0; i < g_nweird; i++)
        if (g_weird[i] == item) g_weird[i--] = g_weird[--g_nweird];
}
static unsigned rnd(void) {
    static unsigned s;
    if (!s) s = (unsigned)time(NULL) * 2654435761u | 1;
    s ^= s << 13, s ^= s >> 17, s ^= s << 5;
    return s;
}
// SMM1: one count of eligible Super Mushrooms for the session; at most one weird per 100, at least one
static bool roll_weird(void) {
    static int count;
    static bool seen;
    bool w = false;
    if (++count >= 100) {
        count = 0;
        w = !seen;  // (the 100th: weird if none was)
        seen = false;
    } else if (!seen && rnd() % 20 == 0) {
        w = seen = true;
    }
    if (g_debug) LOG("[lanky] eligible Super Mushroom %d of 100%s\n", count, w ? ": WEIRD" : "");
    return w || g_weird_test;
}
static void mushroom_create(Cpu *c) {
    uint64_t args = c->x[0];
    uint32_t kind = args ? g32(args + 0x3c) >> 16 & 0xff : 0;
    guest_call_orig(c, MUSHROOM_CREATE);
    uint64_t item = c->x[0];
    forget_weird(item);  // (another actor's address before)
    if (!item || !active() || !smb1_course()) return;
    bool eligible = kind != 1 && kind != 9;  // (not placed in the course: out of a block or a container)
    if (g_debug) LOG("[lanky] Super Mushroom %#llx created, spawn kind %u%s\n", (unsigned long long)item, kind, eligible ? " (out of a block)" : "");
    if (eligible && roll_weird() && g_nweird < 32) {
        g_weird[g_nweird++] = item;
        LOG("[lanky] a Weird Mushroom (%#llx)\n", (unsigned long long)item);
    }
}

volatile bool g_lanky_requesting;  // (make_lanky's own form request: not a power-up item, costume.c leaves it alone)

// ---------------------------------------------------------------- guest calls from the player's update
// (pm_call, player_model.c: everything caller-saved is put back; the scratch goes below sp)
static int request_form(Cpu *c, uint64_t player, uint32_t form) {
    uint64_t sp = c->sp;
    c->sp -= 0x40;
    uint64_t arg = c->sp + 0x20;
    s32(arg, form);
    g_lanky_requesting = true;
    int r = (int)(uint32_t)pm_call(c, FORM_REQUEST, player, arg, 0, 0);
    g_lanky_requesting = false;
    c->sp = sp;
    return r;
}
static void play(Cpu *c, uint64_t player, const char *key) {
    uint64_t u = g64(player + 0x28);
    u = u ? g64(u + 0x50) : 0;
    if (!u || !g64(u + 0x10)) return;
    uint64_t sp = c->sp;
    c->sp -= 0x40;
    uint64_t handle = c->sp + 0x20;
    s64(handle, 0), s64(handle + 8, 0);
    pm_call(c, SOUND_PLAY, u, (uint64_t)(uintptr_t)key, 0, handle);
    if (g_debug) LOG("[lanky] sound %s: handle %#llx\n", key, (unsigned long long)g64(handle));
    c->sp = sp;
}

// ---------------------------------------------------------------- becoming lanky
static void make_lanky(Cpu *c, Lanky *l, uint64_t player, const char *why) {
    uint32_t form = g32(player + P_FORM);
    if (l->lanky) return;
    if (form > 1) {  // (as the Weird Mushroom: refused with another power-up)
        LOG("[lanky] %s: player %d has another power-up (form %u)\n", why, player_index(player), form);
        return;
    }
    l->lanky = true;
    if (form == 1) {
        show(l, form);
    } else {  // (small: the game's own change to Super, its demo shows small <-> Lanky)
        int r = request_form(c, player, 1);
        if (r == 2) {
            l->lanky = false;
            LOG("[lanky] %s: player %d can't change now (form %u)\n", why, player_index(player), form);
            return;
        }
    }
    queue_sound(l, "power_up_Funny");
    LOG("[lanky] %s: player %d is lanky (form %u)\n", why, player_index(player), form);
}

// the overlay menu (any thread): player i becomes lanky at its next update; false when that can't happen now
bool lanky_trigger(uint64_t player) {
    int i = player_index(player);
    if (i < 0 || !active_ui() || !smb1_course()) return false;
    Lanky *l = by_holder(g64(player + P_HOLDER));
    if (!l) return false;
    g_request[i] = true;
    return true;
}
bool lanky_is(uint64_t player) {
    Lanky *l = player ? by_holder(g64(player + P_HOLDER)) : NULL;
    return l && l->lanky;
}
void lanky_clear(uint64_t player) {
    Lanky *l = player ? by_holder(g64(player + P_HOLDER)) : NULL;
    if (!l) return;
    l->lanky = l->rising = false, l->timer = 0;
    show(l, 1);
}

// ---------------------------------------------------------------- the model holder (player_model.c)
int lanky_model_swaps(uint64_t holder, uint32_t style, uint32_t character, PmSwap *out) {
    if (style != 0 || !active()) return 0;
    Lanky *l = by_holder(holder);
    if (!l) l = &g_l[g_next++ % 8];
    memset(l, 0, sizeof *l);  // (a new course: not lanky)
    l->holder = holder;
    l->test_in = g_test && character == 0 ? (g_test > 1 ? g_test : 90) : 0;
    g_nweird = 0;
    static char file[] = "Player_SlimMarioMdl", fmdl[] = "Su_mario_model";
    out[0] = (PmSwap){NAME_TABLE + (uint64_t)SLIM_SLOT * 16 + 8, (uint64_t)(uintptr_t)file, 0};
    out[1] = (PmSwap){FMDL_TABLE + (uint64_t)SLIM_SLOT * 16 + 8, (uint64_t)(uintptr_t)fmdl, 0};
    if (g_debug) LOG("[lanky] holder %#llx (character %u): Slim Mario in slot %d\n", (unsigned long long)holder, character, SLIM_SLOT);
    return 2;
}
int32_t lanky_visual_slot(uint64_t holder, int32_t slot) {
    if (slot != 1 || g32(holder + H_STYLE) != 0) return slot;
    Lanky *l = by_any_holder(holder);
    return l && l->lanky ? SLIM_SLOT : slot;
}

// ---------------------------------------------------------------- every update of each player (games/smm2/trace.c)
void lanky_player_frame(Cpu *c, uint64_t player) {
    g_last_update = renderer_present_count();
    if (lanky_on() && g_debug) {  // (the height of every jump, lanky or not: y at +0x234 [obs: pos x/y/z +0x230..])
        static uint64_t who;
        static float top, from;
        static bool air;
        uint32_t wy = g32(player + 0x234), wv = g32(player + P_VY);
        float y, vy;
        memcpy(&y, &wy, 4), memcpy(&vy, &wv, 4);
        bool ground = g32(player + P_GROUND) & 1;
        if (who != player) who = player, air = false;
        if (!air && !ground && vy > 0.5f) air = true, from = top = y;
        if (air && y > top) top = y;
        if (air && ground) air = false, LOG("[lanky] jump peak %.1f px (form %u)\n", top - from, g32(player + P_FORM));
    }
    static long try_at = -2;  // (test: RECOMP_LANKY_TRY=<presented frame> presses "Lanky Mario now" then)
    if (try_at == -2) try_at = getenv("RECOMP_LANKY_TRY") ? atol(getenv("RECOMP_LANKY_TRY")) : -1;
    if (try_at > 0 && g_last_update >= (uint64_t)try_at) {
        try_at = -1;
        extern bool lanky_now_available(void);
        extern void lanky_now_trigger(int choice);
        LOG("[lanky] RECOMP_LANKY_TRY: Lanky Mario now is %s\n", lanky_now_available() ? "available" : "greyed out");
        lanky_now_trigger(0);
    }
    if (!active()) return;
    uint64_t holder = g64(player + P_HOLDER);
    Lanky *l = by_holder(holder);
    if (!l) return;
    if (l->player != player && g_debug) LOG("[lanky] player %#llx: holder %#llx\n", (unsigned long long)player, (unsigned long long)holder);
    l->player = player;
    l->holder2 = g64(player + P_HOLDER2);
    if (!smb1_course()) return;
    int i = player_index(player);
    if (i >= 0 && g_request[i]) g_request[i] = false, make_lanky(c, l, player, "Lanky Mario now");
    if (l->test_in > 0 && --l->test_in == 0) make_lanky(c, l, player, "RECOMP_LANKY");
    uint32_t form = g32(player + P_FORM), pending = g32(player + P_PENDING);
    if (g_debug && (form != l->last_form || pending != l->last_pending))
        LOG("[lanky] player %d form %u pending %u%s\n", i, form, pending, l->lanky ? " (lanky)" : "");
    l->last_form = form, l->last_pending = pending;
    if (l->lanky && form != 1 && pending != 1) {  // (a hit, a Fire Flower, a Big Mushroom...: lanky no more)
        l->lanky = l->rising = false, l->timer = 0;
        LOG("[lanky] player %d is no longer lanky (form %u)\n", i, form);
    }
    show(l, form);
    // the fuwa cue
    if (l->rising || l->timer) {
        uint64_t in = g64(player + P_INPUT);
        bool held = in && (g8(in + 0x16) || g8(in + 0x1e)), ground = g32(player + P_GROUND) & 1;
        uint32_t w = g32(player + P_VY);
        float vy;
        memcpy(&vy, &w, 4);
        if (l->rising && (!held || vy <= 1.0f || ground)) {
            l->rising = false;
            if (g_debug) LOG("[lanky] fuwa: rising ends at timer %d (held %d, vy %.2f, ground %d)\n", l->timer, held, vy, ground);
        }
        if (l->timer > 0 && --l->timer == 1 && l->rising) queue_sound(l, "h_fuyu");
    }
    for (int k = 0; k < 2; k++)
        if (l->sound[k]) play(c, player, l->sound[k]), l->sound[k] = NULL;
}

// ---------------------------------------------------------------- hook points (games/smm2/mods/hooks.c): true when handled
bool lanky_after(Cpu *c, uint64_t addr) {
    uint64_t off = addr - MAIN_ADDR(0);
    if (off == 0x15a94a0) return g_lanky_requesting;  // the form change request: ours
    if (off == 0x15c8d54) {  // the Jump state's begin, the launch done: x19 the player, vy final at +0x240
        uint64_t player = c->x[19];
        Lanky *l = active() ? by_holder(g64(player + P_HOLDER)) : NULL;
        if (!l || !l->lanky || g32(player + P_FORM) != 1 || !smb1_course()) return true;
        l->timer = 20, l->rising = true;
        queue_sound(l, "jump_3_Funny");
        uint32_t req = g32(player + P_JUMP_VY), w = g32(player + P_VY);
        if ((req & 0x7fffffffu) > 0x7f800000u) {  // (NaN: a computed jump)
            float vy;
            memcpy(&vy, &w, 4);
            vy += 0.5f;
            memcpy(&w, &vy, 4);
            s32(player + P_VY, w);
            if (g_debug) LOG("[lanky] jump: vy %.3f (+0.5)\n", vy);
        } else if (g_debug) {
            float vy;
            memcpy(&vy, &w, 4);
            LOG("[lanky] jump: preset vy %.3f (unchanged)\n", vy);
        }
        return true;
    }
    if (off == 0x13502c4) {  // the item pickup loads the player's form (w8): x19 the item, x20 the player
        uint64_t item = c->x[19], player = c->x[20];
        if (!active() || !is_weird(item) || g32(item + I_KIND) != 0 || !smb1_course()) return true;
        Lanky *l = by_holder(g64(player + P_HOLDER));
        uint32_t form = (uint32_t)c->x[8];
        if (!l || l->lanky || form > 1) return true;  // (Fire and the rest: refused, as in SMM1; the item is a mushroom)
        l->lanky = true;
        show(l, form);
        queue_sound(l, "power_up_Funny");
        LOG("[lanky] Weird Mushroom: player %d is lanky (form %u)\n", player_index(player), form);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- the overlay menu's "Lanky Mario now"
bool lanky_now_available(void) {
    if (!active_ui() || !smb1_course() || bridge_endless_course()) return false;
    if (renderer_present_count() - g_last_update > 30) return false;  // (no course being played)
    uint64_t pm = g64(PLAYERS), p = pm ? g64(pm + 0x28) : 0;
    return p && by_holder(g64(p + P_HOLDER)) && g32(p + P_FORM) <= 1;  // (small or Super, as the Weird Mushroom)
}
// why the row is greyed out, in the order a player would fix it (a few words: it sits beside the label)
static const char *lanky_now_why(void) {
    if (!active_ui()) return "Turn on Lanky Mario";
    if (renderer_present_count() - g_last_update > 30) return "Only in a course";
    if (bridge_endless_course()) return "Not in runs";
    if (!smb1_course()) return "SMB1 courses only";
    uint64_t pm = g64(PLAYERS), p = pm ? g64(pm + 0x28) : 0;
    if (p && g32(p + P_FORM) > 1) return "Small or Super only";
    return "";
}
void lanky_now_trigger(int choice) {
    (void)choice;
    uint64_t pm = g64(PLAYERS), p = pm ? g64(pm + 0x28) : 0;
    if (!lanky_now_available() || !p || !lanky_trigger(p)) LOG("[lanky] Lanky Mario now: not available\n");
}

__attribute__((constructor)) static void lanky_register(void) {
    hle_override(MUSHROOM_CREATE, mushroom_create, "lanky: Super Mushroom create (Weird Mushrooms)");
    static const GameModTrigger t = {
        "Lanky Mario now",
        "Player 1 becomes Lanky Mario (as with a Weird Mushroom: from small or Super Mario) in the Super Mario Bros. "
        "style course being played. Needs the Lanky Mario switch; not in Endless or 100 Mario runs.",
        lanky_now_available, lanky_now_trigger, NULL, NULL, 0, lanky_now_why};
    mod_trigger_add(&t);
}
