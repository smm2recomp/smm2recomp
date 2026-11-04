// SMM1 extras: settings that each turn one SMM1 behaviour on in the otherwise exact SMM2 game. All off by default,
// so the game is SMM2 unless the player opts in. One switch each in the overlay menu's Mods tab (settings.c lists
// them after the 100 Mario mod's), kept in smm2mods.ini; an environment variable wins over the ini:
//
//   RECOMP_SMM1_SHELL_JUMPS=1   smm1_shell_jumps=1     mid-air shell jumps as in SMM1
//   RECOMP_SMM1_PIPES=1         smm1_overlap_pipes=1   pipes may overlap pipes (Course Maker and courses)
//   RECOMP_HUNDRED_SMM1_CARD=1  hundred_smm1_card=1    100 Mario: SMM1's start card at a course's start (off: SMM2's
//                                                      own course-in card, as after a miss; mods/smm1/flow.c)
//   RECOMP_SMM1_POUND_HOP=1     smm1_pound_hop=1       a ground pound rises a little first (SMW, NSMBU), as in SMM1
//   RECOMP_SMM1_SPIN_CATCH=1    smm1_spin_catch=1      SMW's spin jump may grab / catch an item, as in SMM1
//   RECOMP_SMM1_LONG_DOWN=1     smm1_long_down=1       the death fanfare plays to its end after a pit fall, as in SMM1
//   RECOMP_SMM1_ONEWAY_LEDGE=1  smm1_oneway_ledge=1    a sideways one-way wall has SMM1's ledge on its blocked side
//   RECOMP_SMM1_FIRE_CLIMB=1    smm1_fire_climb=1      NSMBU: a throw ends in Fall even mid-twirl (the wall climb)
//   RECOMP_SMM1_TITLE_LETTERS=1 smm1_title_letters=1   the title logo's letters do what SMM1's did when tapped
//                                                      (mods/title_letters)
//
// The behaviour comes from the game's own code: host hooks after single instructions (games/smm2/mods/smm1_extras.py
// lists them) change a register or the flags while a setting is on, and do nothing otherwise. What SMM1 does was
// read from SMM1's own code (Block.rpx v208); both are described in docs/mods.md ("SMM1 extras").
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mod.h"
#include "romfs_overlay.h"
#include "smm1/smm1_audio.h"

typedef struct { const char *label, *desc; bool *value; bool def; } GameModToggle;  // (runtime/game_hooks.c)

static bool shell_jumps, overlap_pipes, smm1_card, pound_hop, spin_catch, long_down, oneway_ledge, fire_climb;
bool g_title_letters;  // (mods/title_letters/title_letters.h)

static const struct Setting {
    const char *key, *env;
    bool *value;
} settings[] = {
    {"smm1_shell_jumps", "RECOMP_SMM1_SHELL_JUMPS", &shell_jumps},
    {"smm1_overlap_pipes", "RECOMP_SMM1_PIPES", &overlap_pipes},
    {"hundred_smm1_card", "RECOMP_HUNDRED_SMM1_CARD", &smm1_card},
    {"smm1_pound_hop", "RECOMP_SMM1_POUND_HOP", &pound_hop},
    {"smm1_spin_catch", "RECOMP_SMM1_SPIN_CATCH", &spin_catch},
    {"smm1_long_down", "RECOMP_SMM1_LONG_DOWN", &long_down},
    {"smm1_oneway_ledge", "RECOMP_SMM1_ONEWAY_LEDGE", &oneway_ledge},
    {"smm1_fire_climb", "RECOMP_SMM1_FIRE_CLIMB", &fire_climb},
    {"smm1_title_letters", "RECOMP_SMM1_TITLE_LETTERS", &g_title_letters},
};
#define NSETTINGS (sizeof settings / sizeof *settings)

static const char *ini_path(void) { return getenv("RECOMP_MOD_INI") ? getenv("RECOMP_MOD_INI") : "smm2mods.ini"; }

static void load_once(void) {
    FILE *f = fopen(ini_path(), "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            char *eq = strchr(line, '=');
            if (line[0] == '#' || !eq) continue;
            *eq = 0;
            for (size_t i = 0; i < NSETTINGS; i++)
                if (!strcmp(line, settings[i].key)) *settings[i].value = atoi(eq + 1) != 0;
        }
        fclose(f);
    }
    for (size_t i = 0; i < NSETTINGS; i++) {
        if (getenv(settings[i].env)) *settings[i].value = atoi(getenv(settings[i].env)) != 0;
        if (*settings[i].value) LOG("[mod] SMM1 extras: %s on\n", settings[i].key);
    }
}
static void load(void) {  // (the game's threads and the menu: once)
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, load_once);
}

void mod_ini_set(const char *key, const char *val);  // (settings.c: replace or add one key=value line)

bool hundred_smm1_card(void) {  // (hundred.h; the flow asks on its render thread)
    load();
    return smm1_card;
}
bool smm1_title_letters(void) {  // (mods/title_letters: its hooks ask on the game thread)
    load();
    return g_title_letters;
}

// ---- the Mods tab (settings.c appends these to its own)
static bool shown[NSETTINGS];  // what the tab last showed, to see which one changed

int smm1_extras_toggles(GameModToggle *out, int room) {
    static const GameModToggle toggles[] = {
        {"Mid-air shell jumps (SMM1)",
         "SMM1's timing for landing on a shell: after a mid-air kick off a wall only SMM1's short no-touch time (5 "
         "frames, SMM2 16), so the shell coming back off the wall can be landed on, and a flat stomp window when "
         "falling (SMM2 narrows it by up to 3 px at slow falls). Immediately.",
         &shown[0], false},
        {"Overlapping pipes (SMM1)",
         "Pipes may overlap other pipes, in the Course Maker and in courses, as in SMM1 (SMM2 refuses the placement "
         "and treats such a course as broken). From the next course start.",
         &shown[1], false},
        {"SMM1 course card (100 Mario)",
         "In the 100 Mario Challenge, SMM1's course card (the course, its maker and the lives) after the map instead "
         "of SMM2's own course-in card. From the next course.",
         &shown[2], false},
        {"Ground pound hop (SMM1)",
         "In Super Mario World and New Super Mario Bros. U style courses a ground pound rises a little (about half "
         "a block) while Mario curls up, before it drops, as in SMM1 (SMM2: he hangs still). Immediately.",
         &shown[3], false},
        {"Catch while spin jumping (SMM1)",
         "In Super Mario World style courses a spin jump can grab or catch an item, as in SMM1: throw it up, spin "
         "jump, catch it while spinning and keep spin-bouncing off enemies with it (SMM2: a spin jump can't "
         "grab). Immediately.",
         &shown[4], false},
        {"Full death fanfare in pits (SMM1)",
         "The death fanfare always plays to its end, as in SMM1: SMM2 cuts it short after about a second and a half "
         "when Mario falls into a pit, as the course starts again. Not with Quick level return. Immediately.",
         &shown[5], false},
        {"One-way wall ledge (SMM1)",
         "A sideways one-way wall has SMM1's ledge: once Mario is past it, on the side it blocks, the wall is an 8-px "
         "solid pillar there that he can stand on and wall-kick off (SMM2: only the one-way line). From the next "
         "course start.",
         &shown[6], false},
        {"Fire Flower wall climb (SMM1)",
         "In New Super Mario Bros. U style courses, SMM1's Fire Flower wall climb works again: wall jump, throw a "
         "fireball at once, twirl twice at the top and grab the same wall higher up. A throw now ends as SMM1's does "
         "even during a twirl, so the wall can be grabbed mid-twirl (SMM2 kept Mario in the throw until the twirl "
         "was over). Immediately.",
         &shown[7], false},
        {"SMM1 title letters",
         "On the title screen, tap or click a letter of the logo and it does what that letter did on SMM1's title "
         "screen. Needs your SMM1 dump (without it the letters do nothing). Immediately.",
         &shown[8], false},
    };
    load();
    int n = 0;
    for (size_t i = 0; i < NSETTINGS && n < room; i++) {
        shown[i] = *settings[i].value;
        out[n++] = toggles[i];
    }
    return n;
}

void smm1_extras_toggles_changed(void) {
    for (size_t i = 0; i < NSETTINGS; i++) {
        if (shown[i] == *settings[i].value) continue;
        *settings[i].value = shown[i];
        mod_ini_set(settings[i].key, shown[i] ? "1" : "0");
        LOG("[mod] SMM1 extras: %s %s\n", settings[i].key, shown[i] ? "on" : "off");
    }
}

// ---- the hooks (smm1_extras.py)

// A shell: an enemy in its shell mode (its shell state object's mode 1; SMM2's enemy contact picks the still / sliding
// shell profiles on the same test).
static bool on_heap(uint64_t a) { return a >= HEAP_BASE && a < HEAP_BASE + HEAP_MAX - 0x1000 && !(a & 7); }
static bool in_shell(uint64_t enemy) {
    if (!on_heap(enemy)) return false;
    uint64_t st = g64(enemy + 0x5c8);
    return on_heap(st) && g32(st + 0x28) == 1;
}

// RECOMP_SMM1_LOG: after a mid-air kick without a window, the kicker's no-hit window on that shell is logged at each of
// the player's next 20 updates (smm1_extras_player_frame, from games/smm2/trace.c), so the 5 / 16 can be seen counting
static uint64_t watch_tab;
static int watch_idx, watch_left;

void smm1_extras_player_frame(void) {
    if (watch_left <= 0) return;
    watch_left--;
    if (!on_heap(watch_tab)) return;
    LOG("[smm1]   kicker's no-hit window on that shell: %d\n", (int)g32(watch_tab + 0x1f8 + 4 * (uint64_t)watch_idx));
}

static float f32v(Cpu *c, int r) { float f; memcpy(&f, &c->v[r], 4); return f; }
static float g32f(uint64_t a) { float f; uint32_t w = g32(a); memcpy(&f, &w, 4); return f; }

void smm1_extras_after(Cpu *c, uint64_t addr) {
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("RECOMP_SMM1_LOG") != NULL;
    load();
    switch (addr - MAIN_ADDR(0)) {
    case 0x1267ce8:  // stomp test, cmp w11, #1 ("falling") before SMM2's step for ordinary-size targets
        if (!in_shell(g64(c->x[20] + 0xa8))) break;
        if (dbg) {  // the window both ways: s0 the table's, s8 the extra; SMM2 adds -3 + the player's fall this frame,
                    // at most 3 (fmax(dy, -3)), so its window is never wider than SMM1's
            uint64_t p = c->x[19], shell = g64(c->x[20] + 0xa8);
            float w = f32v(c, 0), extra = f32v(c, 8), dy = g32f(p + 0x234) - g32f(p + 0x308);
            float fall = dy < 0 ? (-dy < 3 ? -dy : 3) : 0;
            bool step = FLAG_Z(c) && (uint32_t)c->x[9] == 0;  // falling, ordinary-size target
            bool minus3 = (c->x[8] & 1) || (int32_t)c->x[10] < 1;  // (w8 | w10 < 1: the -3, as the next lines test)
            LOG("[smm1] shell stomp test: falling %d, window SMM1 %.3f, SMM2 %.3f (player x %.3f y %.3f dy %.3f, "
                "shell x %.3f y %.3f vx %.3f)%s\n"
                "[smm1]   centres: player %+.2f %+.2f shell %+.2f %+.2f, shell state %d, grounded %d\n",
                (int)FLAG_Z(c), w + extra, step ? w - (minus3 ? 3 : 0) + extra + fall : w + extra, g32f(p + 0x230),
                g32f(p + 0x234),
                dy, g32f(shell + 0x230), g32f(shell + 0x234), g32f(shell + 0x23c), shell_jumps ? " -> SMM1" : "",
                g32f(p + 0x2f8), g32f(p + 0x2fc), g32f(shell + 0x2f8), g32f(shell + 0x2fc), (int)g32(shell + 0x3f8),
                (int)(g32(shell + 0x658) & 1));
        }
        if (shell_jumps) (void)subs32(c, 0, 1);  // -> b.ne: the flat window
        break;
    case 0xfe827c: {  // overlap rule: x19 the pair (this part's record at +0, the other's at +0x28), w0 "other solid"
        uint16_t self = (uint16_t)g32(c->x[19] + 0x20), other = (uint16_t)g32(c->x[19] + 0x48);
        bool smm1 = overlap_pipes && self == 9 && other == 9;
        if (dbg && (self == 9 || other == 9))
            LOG("[smm1] overlap rule: part %u against part %u: %s%s\n", self, other, (c->x[0] & 1) ? "checked" : "free",
                smm1 ? " -> free (SMM1)" : "");
        if (smm1) c->x[0] = 0;
        break;
    }
    case 0x15c1da8:  // Fire exec: ldrb w8, [x19, #0x488] (bit 2: a twirl) - SMM1 goes on to Fall regardless
    case 0x15c1db0: {  // ... ldr w8, [x19, #0x283c] (the twirl's frames)
        if (!fire_climb) break;
        uint64_t h = g64(MAIN_ADDR(0x2c57d58)), info = h ? g64(h + 0x30) : 0;
        if (!info || g32(info + 0x1c) != 3) break;  // (NSMBU style)
        if (dbg && (addr - MAIN_ADDR(0) == 0x15c1db0 ? (uint32_t)c->x[8] : (c->x[8] & 4)))
            LOG("[smm1] fire climb: the throw ends in Fall mid-twirl (SMM1)\n");
        c->x[8] = addr - MAIN_ADDR(0) == 0x15c1db0 ? 0 : c->x[8] & ~4ull;
        break;
    }
    case 0x1202c88:  // enemy contact, kick accepted with no window written: mov w9, #16
        if (!in_shell(g64(c->x[23]))) break;
        if (dbg) {
            uint64_t p = c->x[22], shell = g64(c->x[23]);
            // (x21 holds the same per-player no-hit table, [shell+0x6b0], where the contact's epilogue, main+0x1203868,
            // turns an empty window into 5)
            LOG("[smm1] mid-air kick without a no-hit window: %s (player x %.3f y %.3f, shell x %.3f y %.3f%s)\n",
                shell_jumps ? "5 frames (SMM1)" : "16 frames (SMM2)", g32f(p + 0x230), g32f(p + 0x234),
                g32f(shell + 0x230), g32f(shell + 0x234), c->x[21] == c->x[24] ? "" : ", other no-hit table");
            watch_tab = c->x[24], watch_idx = (int8_t)g8(p + 0x288), watch_left = 20;
        }
        if (shell_jumps) c->x[9] = 0;  // -> the epilogue's 5 frames
        break;
    }
}

// ---- SMM1 moves (research/specs/smm1-mods-moves.md), function overrides: each runs the game's own function and,
// with its setting on, changes what that function left in the player (x0 in all three)
#define STYLE_PTR MAIN_ADDR(0x2c57d58)            // *(*(this) + 0x30) + 0x1c: the course's style (2 SMW, 3 NSMBU)
#define HIP_ENTER MAIN_ADDR(0x15c6540)            // HipAttack (the ground pound) begin
#define HIP_EXEC MAIN_ADDR(0x15c6790)             // ... its exec
#define SPIN_ENTER MAIN_ADDR(0x15df8a0)           // SpinJump begin
#define P_VY 0x240
#define P_HIP_ARG 0x2488   // the pound's argument: 0 a normal pound (1: straight into the dive)
#define P_HIP_PHASE 0x2478 // 0 the wind-up (hip_start), 1 the hold, 2 the dive
#define P_STATE_FLAGS 0x494  // bit 22: the state lets the player grab (SMM1 +0x42c bit 8)

static int course_style(void) {
    load();
    uint64_t h = g64(STYLE_PTR), info = h ? g64(h + 0x30) : 0;
    return info ? (int)g32(info + 0x1c) : -1;
}

// SMM1's pound begin sets vy +1.0 with the gravity off, so Mario rises 1 px a frame while `hip_start` plays (about
// 9 frames in NSMBU), then the hold puts vy back to 0 [obs v208 0x026bf3e0, 0x026bf640]. SMM2's begin zeroes vy and
// gravity and its wind-up never touches them: the begin's vy is set to 1.0, and set back to 0 when the wind-up ends.
static void hip_enter(Cpu *c) {
    uint64_t p = c->x[0];
    guest_call_orig(c, HIP_ENTER);
    int st = course_style();
    if (getenv("RECOMP_SMM1_LOG"))
        LOG("[smm1] ground pound begin: style %d, argument %u%s\n", st, g32(p + P_HIP_ARG),
            pound_hop && (st == 2 || st == 3) && !g32(p + P_HIP_ARG) ? " -> rises (SMM1)" : "");
    if (pound_hop && (st == 2 || st == 3) && g32(p + P_HIP_ARG) == 0) {
        float one = 1.0f;
        uint32_t w;
        memcpy(&w, &one, 4);
        s32(p + P_VY, w);
    }
}
static void hip_exec(Cpu *c) {
    uint64_t p = c->x[0];
    uint32_t before = g32(p + P_HIP_PHASE);
    guest_call_orig(c, HIP_EXEC);
    load();
    if (getenv("RECOMP_SMM1_LOG") && before == 0)
        LOG("[smm1] ground pound wind-up: y %.2f vy %.2f, phase now %u\n", g32f(p + 0x234), g32f(p + P_VY),
            g32(p + P_HIP_PHASE));
    if (pound_hop && before == 0 && g32(p + P_HIP_PHASE) == 1) s32(p + P_VY, 0);
}
// SMM1's spin begin lets the player grab in SMW style (+0x42c |= 0x100 with the style's mw_spin_jump) [obs v208
// 0x026ce914]; SMM2's sets only the other flag (+0x494 |= 0x800000) [obs v3.0.3 main+0x15df924].
static void spin_enter(Cpu *c) {
    uint64_t p = c->x[0];
    guest_call_orig(c, SPIN_ENTER);
    int st = course_style();
    if (getenv("RECOMP_SMM1_LOG"))
        LOG("[smm1] spin jump begin: style %d, state flags %#x%s\n", st, g32(p + P_STATE_FLAGS),
            spin_catch && st == 2 ? " -> may grab (SMM1)" : "");
    if (spin_catch && st == 2) s32(p + P_STATE_FLAGS, g32(p + P_STATE_FLAGS) | 0x400000u);
}

__attribute__((constructor)) static void smm1_moves_register(void) {
    hle_override(HIP_ENTER, hip_enter, "SMM1 extras: ground pound begin (hop)");
    hle_override(HIP_EXEC, hip_exec, "SMM1 extras: ground pound exec (hop ends)");
    hle_override(SPIN_ENTER, spin_enter, "SMM1 extras: spin jump begin (catch)");
}

// ---- the full death fanfare (mods/costume.c down_fanfare, at the course music's state 7). SMM2 plays the style's
// FanfareDown stream (Sound/Resource/Stream/<style>_Fanfare_PlayerDown.bwav) and, after a pit fall, cuts it at about
// 1.6 s when the course is taken down to start again [obs: the game's audio recorded through pit misses in an NSMBU
// course, whose fanfare sounds for 2.2 s]: its last notes are lost. SMM1 let it play out. With the setting on the
// game's own is left out (state 0, as a costume's death sound does) and the same stream plays through the host mixer,
// which the course's restart doesn't stop. Not for the special characters' own fanfares (SMB2 Mushroom, Superball,
// Link: the game's) nor with quick level return (its restart comes before the end).
#define LONG_DOWN_VOL 0.24f  // matched to the game's own in a recording (RMS of the first 1.5 s; SLink's 0.38 was
                             // 1.6 times louder through the host mixer)
bool quick_retry_on(void);
bool smm1_long_down_fanfare(void) {  // (mods/costume.c: true: played here, the game's to be left out)
    load();
    if (!long_down || quick_retry_on()) return false;
    uint64_t pm = g64(MAIN_ADDR(0x2c19290)), pl = pm ? g64(pm + 0x28) : 0;
    uint32_t kind = pl ? g32(pl + 0x4ac) : 0;  // (the fanfare's own choice: 9 / 10 / 15 their own)
    if (kind == 9 || kind == 10 || kind == 15) return false;
    static const char *const names[5] = {"M1", "M3", "MW", "WU", "3W"};
    static SPcm *pcm[5];
    static bool tried[5];
    int st = course_style();
    if (st < 0 || st > 4) return false;
    if (!tried[st]) {
        tried[st] = true;
        char rel[96];
        snprintf(rel, sizeof rel, "Sound/Resource/Stream/%s_Fanfare_PlayerDown.bwav", names[st]);
        RovBuf b;
        if (rov_read_game(rel, &b)) pcm[st] = smm1_bwav_decode(b.d, b.n), rov_free(&b);
        if (!pcm[st]) LOG("[mod] SMM1 extras: %s not readable: the game's own death fanfare\n", rel);
    }
    if (!pcm[st]) return false;
    smm1_play_vol(pcm[st], LONG_DOWN_VOL);
    if (getenv("RECOMP_SMM1_LOG")) LOG("[smm1] death fanfare (%s) played in full by the host\n", names[st]);
    return true;
}

// ---- the one-way wall ledge (research/specs/smm1-mods-moves.md §2, §7). SMM1's HalfHitWall (object 67) builds, for the
// sideways walls (d = 0, 2), a second collider: a solid box 8 px thick and 32 tall against the one-way line on its far
// side, in the course only while player 1 is wholly on the side the wall blocks (added when his near edge is 16 px
// past the wall, removed when his far edge is back within 8) [obs v208 0x025fd2c4, 0x025fd1a0]. SMM2's has only the
// line. With the setting on, the same box is built at the wall's create the way SMM2's own boxes are (MapObjDashBlock,
// inlined there: the base collider, the box vtable, its vertex / edge arrays, the init with a descriptor), as a
// HardBlock (type 9: plain solid; SMM1's was solid to players only, through a sensor mask SMM2 lacks: here it is solid
// to everything, while player 1 is on that side), added to and taken out of the BG grid each frame by player 1's
// side, and destroyed before the wall is. Its memory comes from the game's own operator new.
#define HHW_CREATE MAIN_ADDR(0x13b1b50)     // MapObjHalfHitWall create (vtable slot 6)
#define HHW_EXEC MAIN_ADDR(0x13b1ea0)       // ... per frame (slot 9)
#define HHW_DTOR MAIN_ADDR(0x13b1ad0)       // ... complete destructor (slot 3)
#define HHW_DTOR_DEL MAIN_ADDR(0x13b1b10)   // ... deleting destructor (slot 4)
#define COL_BASE_CTOR MAIN_ADDR(0xe24160)   // collider base constructor (c)
#define COL_INIT MAIN_ADDR(0xe310d0)        // collider init (c, init, descriptor)
#define COL_ADD MAIN_ADDR(0xe265e0)         // into the BG grid (c)
#define COL_REMOVE MAIN_ADDR(0xe26430)      // out of it (c)
#define VT_POLY4 MAIN_ADDR(0x28b96f8)       // 4-vertex polygon collider vtable
#define VT_BOX MAIN_ADDR(0x28695f8)         // box collider vtable (slot 0: its complete destructor)
#define EDGE_INIT MAIN_ADDR(0x233a3ec)      // 16 bytes each edge record takes ({0, 0, 1.0, 0})
#define COL_TYPES MAIN_ADDR(0x2a651d0)      // {u32 attr, char *name} by collider type
#define OP_NEW MAIN_ADDR(0x262790)          // operator new (size)
#define OP_DELETE MAIN_ADDR(0x262c20)       // operator delete (p)
#define BOX_SIZE 0x4c0
#define TYPE_HARD 9
static struct { uint64_t wall, box; int d; bool added; } ledges[512];
static pthread_mutex_t ledge_lock = PTHREAD_MUTEX_INITIALIZER;

static int ledge_find(uint64_t wall) {
    for (int i = 0; i < (int)(sizeof ledges / sizeof *ledges); i++)
        if (ledges[i].wall == wall) return i;
    return -1;
}
static void gcall(Cpu *c, uint64_t fn) {
    uint64_t lr = c->x[30];
    guest_call(c, fn);
    c->x[30] = lr;
}
static void sf32(uint64_t a, float f) { uint32_t w; memcpy(&w, &f, 4); s32(a, w); }

static void hhw_create(Cpu *c) {
    uint64_t wall = c->x[0];
    guest_call_orig(c, HHW_CREATE);
    uint64_t ret = c->x[0];
    load();
    int d = (int)g32(wall + 0x840);
    if (!oneway_ledge || (d != 0 && d != 2)) return;
    uint64_t bg = g64(g64(wall + 0x48) + 0x140);
    if (!bg) return;
    pthread_mutex_lock(&ledge_lock);
    int slot = ledge_find(0);
    pthread_mutex_unlock(&ledge_lock);
    if (slot < 0) return;
    uint64_t x[8];
    memcpy(x, c->x, sizeof x);
    c->x[0] = BOX_SIZE, gcall(c, OP_NEW);
    uint64_t box = c->x[0];
    if (!box) { memcpy(c->x, x, sizeof x); c->x[0] = ret; return; }
    c->x[0] = box, gcall(c, COL_BASE_CTOR);
    for (uint64_t o = 0x3b0; o < BOX_SIZE; o += 4) s32(box + o, 0);
    s64(box, VT_POLY4), s64(box, VT_BOX);
    s32(box + 0x3b0, 4), s64(box + 0x3b8, box + 0x3e0);  // 4 vertices
    s32(box + 0x3c0, 4), s64(box + 0x3c8, box + 0x400);  // 4 edge records
    s32(box + 0x3d0, 4), s64(box + 0x3d8, box + 0x460);  // 4 more
    for (int k = 0; k < 8; k++) {
        uint64_t r = box + (k < 4 ? 0x400 : 0x460) + 0x18ull * (uint64_t)(k & 3);
        s16(r, 8);
        s64(r + 4, g64(EDGE_INIT)), s64(r + 0xc, g64(EDGE_INIT + 8));
        s32(r + 0x14, 0);
    }
    uint64_t sp = c->sp;
    c->sp -= 0x80;
    uint64_t init = c->sp, desc = c->sp + 0x30;
    s64(init, wall + 0x230), s64(init + 8, wall + 0x304), s64(init + 0x10, wall + 0x288), s64(init + 0x18, bg);
    s64(init + 0x20, 1);
    // the box against the line on the blocked side: d = 0 x +8 ... +16, d = 2 x -16 ... -8; y 0 ... 32 (SMM1's)
    float l = d == 0 ? 8.0f : -16.0f, r = d == 0 ? 16.0f : -8.0f;
    const float dv[8] = {0, 0, 0, 16, l, 32, r, 0};
    for (int k = 0; k < 8; k++) sf32(desc + 4ull * (uint64_t)k, dv[k]);
    s32(desc + 0x20, 0);
    c->x[0] = box, c->x[1] = init, c->x[2] = desc, gcall(c, COL_INIT);
    c->sp = sp;
    s32(box + 0x2e4, 5), s64(box + 0x278, wall);
    s32(box + 0x360, TYPE_HARD), s32(box + 0x364, g32(COL_TYPES + 16ull * TYPE_HARD));
    pthread_mutex_lock(&ledge_lock);
    ledges[slot].wall = wall, ledges[slot].box = box, ledges[slot].d = d, ledges[slot].added = false;
    pthread_mutex_unlock(&ledge_lock);
    if (getenv("RECOMP_SMM1_LOG"))
        LOG("[smm1] one-way wall %#llx (d %d, x %.1f y %.1f): ledge box %#llx built\n", (unsigned long long)wall, d,
            g32f(wall + 0x230), g32f(wall + 0x234), (unsigned long long)box);
    memcpy(c->x, x, sizeof x);
    c->x[0] = ret;
}

static void hhw_exec(Cpu *c) {
    uint64_t wall = c->x[0];
    guest_call_orig(c, HHW_EXEC);
    if (!oneway_ledge) return;
    int i = ledge_find(wall);
    if (i < 0) return;
    uint64_t pm = g64(MAIN_ADDR(0x2c19290)), pl = pm ? g64(pm + 0x28) : 0;
    if (!pl) return;
    float px = g32f(pl + 0x230), wx = g32f(wall + 0x230), half = 7.9f;
    static uint64_t n;
    static int after_drop;  // (the drop test: every frame for a while after it)
    if (getenv("RECOMP_SMM1_LOG") && fabsf(px - wx) < 48 && (n++ % 3 == 0 || (after_drop > 0 && after_drop--)))
        LOG("[smm1]   near the one-way wall: player x %.1f y %.1f vy %.2f, wall x %.1f y %.1f, ledge %s\n", px,
            g32f(pl + 0x234), g32f(pl + 0x240), wx, g32f(wall + 0x234), ledges[i].added ? "in" : "out");
    static int drop = -1, dropped_at;  // (tests: RECOMP_SMM1_LEDGE_DROP=1 - with the ledge in for 90 frames, player 1
                                       // is put just above it, still: on a solid ledge he stands at its top)
    if (drop < 0) drop = getenv("RECOMP_SMM1_LEDGE_DROP") != NULL;
    if (drop && ledges[i].added && ++dropped_at == 90) {
        static float dx = -1;  // (RECOMP_SMM1_LEDGE_DROP=<x offset>; 12.6: his feet onto the ledge)
        if (dx < 0) dx = (float)atof(getenv("RECOMP_SMM1_LEDGE_DROP")), dx = dx > 1 ? dx : 17;
        sf32(pl + 0x230, ledges[i].d == 0 ? wx + 16 + dx - 16 + 6.9f - 1 : wx - 16 - dx + 16 - 6.9f + 1);
        sf32(pl + 0x234, g32f(wall + 0x234) + 36);
        sf32(pl + 0x240, 0), sf32(pl + 0x26c, 0);
        LOG("[smm1] (test) player 1 put above the ledge\n");
        after_drop = 24;
    }
    bool add = ledges[i].added;
    if (ledges[i].d == 0) {
        if (px - half >= wx + 16) add = true;
        else if (px + half <= wx + 8) add = false;
    } else {
        if (px + half <= wx - 16) add = true;
        else if (px - half >= wx - 8) add = false;
    }
    if (add == ledges[i].added) return;
    uint64_t x0 = c->x[0];
    c->x[0] = ledges[i].box, gcall(c, add ? COL_ADD : COL_REMOVE);
    c->x[0] = x0;
    ledges[i].added = add;
    if (getenv("RECOMP_SMM1_LOG")) {
        uint64_t ln = wall + 0x3f0, bx = ledges[i].box;  // (the colliders' bounding boxes, c+0x258: l, b, r, t)
        LOG("[smm1] one-way wall %#llx: ledge %s (player x %.1f y %.1f, wall x %.1f); line box %.1f %.1f %.1f %.1f, "
            "ledge box %.1f %.1f %.1f %.1f\n", (unsigned long long)wall, add ? "in" : "out", px, g32f(pl + 0x234), wx,
            g32f(ln + 0x258), g32f(ln + 0x25c), g32f(ln + 0x260), g32f(ln + 0x264), g32f(bx + 0x258), g32f(bx + 0x25c),
            g32f(bx + 0x260), g32f(bx + 0x264));
    }
}

static void ledge_destroy(Cpu *c, uint64_t wall) {
    pthread_mutex_lock(&ledge_lock);
    int i = ledge_find(wall);
    uint64_t box = i >= 0 ? ledges[i].box : 0;
    if (i >= 0) ledges[i].wall = 0, ledges[i].box = 0;
    pthread_mutex_unlock(&ledge_lock);
    if (!box) return;
    uint64_t x[3] = {c->x[0], c->x[1], c->x[2]};
    c->x[0] = box, gcall(c, g64(g64(box)));  // its complete destructor (vtable slot 0: out of the grid too)
    c->x[0] = box, gcall(c, OP_DELETE);
    c->x[0] = x[0], c->x[1] = x[1], c->x[2] = x[2];
}
static void hhw_dtor(Cpu *c) { ledge_destroy(c, c->x[0]); guest_call_orig(c, HHW_DTOR); }
static void hhw_dtor_del(Cpu *c) { ledge_destroy(c, c->x[0]); guest_call_orig(c, HHW_DTOR_DEL); }

__attribute__((constructor)) static void oneway_ledge_register(void) {
    hle_override(HHW_CREATE, hhw_create, "SMM1 extras: one-way wall create (ledge box)");
    hle_override(HHW_EXEC, hhw_exec, "SMM1 extras: one-way wall frame (ledge by player 1's side)");
    hle_override(HHW_DTOR, hhw_dtor, "SMM1 extras: one-way wall destructor (ledge box)");
    hle_override(HHW_DTOR_DEL, hhw_dtor_del, "SMM1 extras: one-way wall deleting destructor (ledge box)");
}
