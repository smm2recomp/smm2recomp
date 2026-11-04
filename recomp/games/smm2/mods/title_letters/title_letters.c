// SMM1 title letters: the title scene's hooks, the logo's panes and the taps (title_letters.h).
//
// SMM2's title page runs one function per state, every frame, with the state owner in x0 (its +0x244 is 0 on a state's
// first frame): Wait (the "Press L + R" prompt), ToTop, Top (Make / Play), ToPlayMenu, PlayMenu, ToEdit. Each is
// overridden here: the game's own runs first, then, with the switch on, the mod reads the logo's panes and follows the
// touch. Leaving
// for the Course Maker (ToEdit) or another scene (Coursebot, Course World, Story Mode) shows the panes again; if the
// title goes some other way, the mod notices that no state ran for a while and forgets the panes without touching
// them (they went with the page). Addresses v3.0.3; research in research/specs/smm2-title-scene.md (private).
//
// RECOMP_TITLE_LETTERS_LOG=1: each letter's box when the panes are found.
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../mod.h"
#include "title_letters.h"

#define T_WAIT MAIN_ADDR(0x1867580)      // title page states (x0 the state owner)
#define T_TOTOP MAIN_ADDR(0x1867690)
#define T_TOP MAIN_ADDR(0x1867920)
#define T_TOEDIT MAIN_ADDR(0x1867e40)
#define T_TOPLAYMENU MAIN_ADDR(0x1867f90)
#define T_PLAYMENU MAIN_ADDR(0x1868110)
#define T_TO_ROBOT MAIN_ADDR(0x1868520)  // the page's scene requests: Coursebot, Course World, Story Mode
#define T_TO_NETWORK MAIN_ADDR(0x18685e0)
#define T_TO_QUEST MAIN_ADDR(0x1868690)
#define OWNER_PAGE 0x288                 // state owner -> the layout page
#define PAGE_LAYOUT 0x1e8                // page -> the layout whose RootPane (+0x380) holds the logo
#define LAYOUT_ROOT 0x380
#define PANE_CHILDREN 0x20               // pane: child list head (each child linked at its +0x08)
#define PANE_T 0x30                      // translate x, y (centre origin, y up)
#define PANE_S 0x48                      // scale x, y
#define PANE_SIZE 0x50                   // width, height
#define PANE_FLAGS 0x58                  // bit 0 visible
#define PANE_NAME 0xb0
#define GONE_AFTER 30                    // presented frames without a state call: the title is gone

static const char *const pane_name[TL_COUNT] = {"P_Super_00", "P_LogoM_00", "P_LogoA_00", "P_LogoR_00", "P_LogoI_00",
                                                "P_LogoO_00", "P_LogoM_01", "P_LogoA_01", "P_LogoK_00", "P_LogoE_00",
                                                "P_LogoR_01"};
#define MAX_DEPTH 10

extern uint64_t renderer_present_count(void);
extern bool touch_first(int32_t *x, int32_t *y);  // runtime/hle/touch.c

static bool on_heap(uint64_t a) { return a >= HEAP_BASE && a < HEAP_BASE + HEAP_MAX - 0x1000 && !(a & 7); }
static float g32f(uint64_t a) { float f; uint32_t w = g32(a); memcpy(&f, &w, 4); return f; }

// ---- state (written by the game thread; boxes read by the renderers' thread under the lock)
static struct {
    uint64_t page;
    uint64_t path[TL_COUNT][MAX_DEPTH];  // RootPane ... the letter's pane
    int depth[TL_COUNT];
    TlBox box[TL_COUNT];
    bool hidden[TL_COUNT];               // hidden by us (its visible bit was set)
    bool down;                           // the touch, as last seen
    int start;                           // the letter the touch went down on
    int32_t x, y;
} T;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_seen;          // presented frame of the last state call (0: none)
static _Atomic bool g_on_title;

bool title_letters_active(void) {
    uint64_t seen = g_seen;
    return g_on_title && seen && renderer_present_count() - seen < GONE_AFTER;
}

bool title_letters_box(int letter, TlBox *out) {
    if (letter < 0 || letter >= TL_COUNT || !title_letters_active()) return false;
    pthread_mutex_lock(&g_lock);
    *out = T.box[letter];
    bool ok = T.depth[letter] > 0;
    pthread_mutex_unlock(&g_lock);
    return ok;
}

static uint64_t letter_pane(int letter) { return T.depth[letter] > 0 ? T.path[letter][T.depth[letter] - 1] : 0; }

void title_letters_hide(int letter, bool hidden) {
    uint64_t p = letter >= 0 && letter < TL_COUNT ? letter_pane(letter) : 0;
    if (!p) return;
    uint8_t f = g8(p + PANE_FLAGS);
    if (hidden && !T.hidden[letter]) {
        if (!(f & 1)) return;  // (SMM2 hides it itself: leave it so)
        T.hidden[letter] = true;
    } else if (!hidden && T.hidden[letter]) {
        T.hidden[letter] = false;
        s8(p + PANE_FLAGS, f | 1);
    }
    if (T.hidden[letter]) s8(p + PANE_FLAGS, f & ~1);
}

static void forget(void) {  // (no pane is touched: they may be gone)
    pthread_mutex_lock(&g_lock);
    memset(&T, 0, sizeof T);
    T.start = TL_NONE;
    pthread_mutex_unlock(&g_lock);
    g_on_title = false;
}

void title_letters_reset(void) {
    if (!g_on_title) return;
    for (int i = 0; i < TL_COUNT; i++) title_letters_hide(i, false);
    forget();
    LOG("[title] letters off: left the title\n");
}

// ---- the panes
static bool find_path(uint64_t pane, const char *name, uint64_t *path, int depth, int *out_depth) {
    if (!on_heap(pane) || depth >= MAX_DEPTH) return false;
    path[depth] = pane;
    if (!strncmp((const char *)(uintptr_t)(pane + PANE_NAME), name, 24)) { *out_depth = depth + 1; return true; }
    uint64_t head = pane + PANE_CHILDREN;
    int k = 0;
    for (uint64_t n = g64(head + 8); on_heap(n - 8) && n != head && k < 128; n = g64(n + 8), k++)
        if (find_path(n - 8, name, path, depth + 1, out_depth)) return true;
    return false;
}

static bool find_panes(uint64_t page) {
    uint64_t lay = g64(page + PAGE_LAYOUT), root = on_heap(lay) ? g64(lay + LAYOUT_ROOT) : 0;
    int found = 0;
    pthread_mutex_lock(&g_lock);
    memset(&T, 0, sizeof T);
    T.start = TL_NONE;
    T.page = page;
    for (int i = 0; i < TL_COUNT; i++)
        if (find_path(root, pane_name[i], T.path[i], 0, &T.depth[i])) found++;
        else T.depth[i] = 0;
    pthread_mutex_unlock(&g_lock);
    return found == TL_COUNT;
}

// a pane's box on the 1280 x 720 screen: the translations and scales down its path (the logo's panes aren't rotated)
static TlBox box_of(int letter) {
    float x = 0, y = 0, sx = 1, sy = 1;
    bool vis = true;
    for (int d = 0; d < T.depth[letter]; d++) {
        uint64_t p = T.path[letter][d];
        x += sx * g32f(p + PANE_T), y += sy * g32f(p + PANE_T + 4);
        sx *= g32f(p + PANE_S), sy *= g32f(p + PANE_S + 4);
        bool ours = d == T.depth[letter] - 1 && T.hidden[letter];
        if (!(g8(p + PANE_FLAGS) & 1) && !ours) vis = false;
    }
    uint64_t p = letter_pane(letter);
    float w = fabsf(sx * g32f(p + PANE_SIZE)), h = fabsf(sy * g32f(p + PANE_SIZE + 4));
    return (TlBox){640 + x - w / 2, 360 - y - h / 2, w, h, vis && w > 1 && h > 1};
}

static void read_boxes(void) {
    TlBox b[TL_COUNT];
    for (int i = 0; i < TL_COUNT; i++) b[i] = T.depth[i] ? box_of(i) : (TlBox){0};
    tl_trim(b);
    pthread_mutex_lock(&g_lock);
    memcpy(T.box, b, sizeof b);
    pthread_mutex_unlock(&g_lock);
}

// ---- the touch: a letter is tapped when the touch goes down and comes up on it (as SMM1's buttons: sliding off
// cancels)
static void letter_tapped(int letter, int32_t x, int32_t y) {
    LOG("[title] letter %s at %d,%d\n", tl_letter_name[letter], x, y);
}

static void follow_touch(bool taps) {
    int32_t x = T.x, y = T.y;
    bool down = touch_first(&x, &y);
    if (down && !T.down) T.start = taps ? tl_hit(T.box, x, y) : TL_NONE;
    if (!down && T.down && T.start != TL_NONE && tl_hit(T.box, T.x, T.y) == T.start) letter_tapped(T.start, T.x, T.y);
    if (!down) T.start = TL_NONE;
    T.down = down;
    if (down) T.x = x, T.y = y;
}

// once per title frame, after the game's own state function
static void title_frame(uint64_t owner, bool taps) {
    if (!smm1_title_letters() || !on_heap(owner)) return;
    uint64_t page = g64(owner + OWNER_PAGE), now = renderer_present_count(), seen = g_seen;
    if (!on_heap(page)) return;
    if (!g_on_title || page != T.page || !seen || now - seen >= GONE_AFTER) {
        uint64_t was_hidden[TL_COUNT];  // (a state we don't follow ran a while, e.g. ToWait: the panes we hid, if
                                        // they are still the page's, are shown again)
        for (int i = 0; i < TL_COUNT; i++) was_hidden[i] = T.hidden[i] ? letter_pane(i) : 0;
        forget();
        bool all = find_panes(page);
        for (int i = 0; i < TL_COUNT; i++)
            if (was_hidden[i] && letter_pane(i) == was_hidden[i])
                s8(was_hidden[i] + PANE_FLAGS, g8(was_hidden[i] + PANE_FLAGS) | 1);
        if (!all) return;  // (not all there yet: again next frame)
        g_on_title = true;
        LOG("[title] letters on (page %#llx)\n", (unsigned long long)page);
        read_boxes();
        if (getenv("RECOMP_TITLE_LETTERS_LOG"))
            for (int i = 0; i < TL_COUNT; i++)
                LOG("[title]   %-5s %s box %.1f %.1f %.1f x %.1f%s\n", tl_letter_name[i], pane_name[i], T.box[i].x,
                    T.box[i].y, T.box[i].w, T.box[i].h, T.box[i].visible ? "" : " (hidden)");
    }
    g_seen = now;
    for (int i = 0; i < TL_COUNT; i++)  // (ours stay hidden whatever SMM2's animations set)
        if (T.hidden[i]) title_letters_hide(i, true);
    read_boxes();
    follow_touch(taps);
}

// ---- the hooks
static void t_wait(Cpu *c) { uint64_t o = c->x[0]; guest_call_orig(c, T_WAIT); title_frame(o, true); }
static void t_totop(Cpu *c) { uint64_t o = c->x[0]; guest_call_orig(c, T_TOTOP); title_frame(o, false); }
static void t_top(Cpu *c) { uint64_t o = c->x[0]; guest_call_orig(c, T_TOP); title_frame(o, true); }
static void t_toplaymenu(Cpu *c) { uint64_t o = c->x[0]; guest_call_orig(c, T_TOPLAYMENU); title_frame(o, false); }
static void t_playmenu(Cpu *c) { uint64_t o = c->x[0]; guest_call_orig(c, T_PLAYMENU); title_frame(o, true); }
static void t_toedit(Cpu *c) { title_letters_reset(); guest_call_orig(c, T_TOEDIT); }
static void t_to_robot(Cpu *c) { title_letters_reset(); guest_call_orig(c, T_TO_ROBOT); }
static void t_to_network(Cpu *c) { title_letters_reset(); guest_call_orig(c, T_TO_NETWORK); }
static void t_to_quest(Cpu *c) { title_letters_reset(); guest_call_orig(c, T_TO_QUEST); }

__attribute__((constructor)) static void title_letters_register(void) {
    forget();
    hle_override(T_WAIT, t_wait, "SMM1 title letters: title Wait");
    hle_override(T_TOTOP, t_totop, "SMM1 title letters: title ToTop");
    hle_override(T_TOP, t_top, "SMM1 title letters: title Top");
    hle_override(T_TOPLAYMENU, t_toplaymenu, "SMM1 title letters: title ToPlayMenu");
    hle_override(T_PLAYMENU, t_playmenu, "SMM1 title letters: title PlayMenu");
    hle_override(T_TOEDIT, t_toedit, "SMM1 title letters: title ToEdit (leaves)");
    hle_override(T_TO_ROBOT, t_to_robot, "SMM1 title letters: title to Coursebot (leaves)");
    hle_override(T_TO_NETWORK, t_to_network, "SMM1 title letters: title to Course World (leaves)");
    hle_override(T_TO_QUEST, t_to_quest, "SMM1 title letters: title to Story Mode (leaves)");
}
