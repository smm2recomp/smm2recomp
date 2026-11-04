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
// A tapped letter plays its SMM1 animation (anim.c) over SMM2's letter, which is hidden meanwhile; the animations are
// drawn by the renderers' thread into the overlay (title_letters_image, after the 100 Mario flow's and the SMW goal
// wipe's: smm1/flow.c), with the letters' screen effects (effects.c: A's stars behind them, the drag's dust over them).
// The SMM1 dump is opened on a thread of its own the first time the title shows, and the letters' sounds with it. If
// the title goes without any of the hooks below noticing, the overlay's next frame finds it gone and turns K's sepia
// and 8-bit music off (tl_effects_lost).
//
// A tap that starts and ends on the course behind the title (off the logo, its "2" and SMM2's buttons of the state:
// the "Press L + R" bar in Wait, Make / Play in Top, the three Play choices in PlayMenu) spawns there (spawn.c).
//
// RECOMP_TITLE_LETTERS_LOG=1: each letter's box when the panes are found; spawns, defeats and clears.
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../mod.h"
#include "../smm1/smm1_data.h"
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

// the letters, then the panes a background tap must miss: the "2", and SMM2's own buttons in the states that show
// them (block_in: bit 0 Wait, 1 Top, 2 PlayMenu)
enum { NBLOCK = 7, NPANES = TL_COUNT + NBLOCK };
static const char *const pane_name[NPANES] = {"P_Super_00", "P_LogoM_00", "P_LogoA_00", "P_LogoR_00", "P_LogoI_00",
                                              "P_LogoO_00", "P_LogoM_01", "P_LogoA_01", "P_LogoK_00", "P_LogoE_00",
                                              "P_LogoR_01", "P_Logo2_00", "W_PressKey_00", "L_EditBtn_00",
                                              "L_PlayBtn_00", "L_QuestBtn_00", "L_NetworkBtn_00",
                                              "L_CourseRobotPlayBtn_00"};
static const uint8_t block_in[NBLOCK] = {7, 1, 2, 2, 4, 4, 4};
enum { ST_WAIT = 1, ST_TOP = 2, ST_PLAYMENU = 4 };
#define MAX_DEPTH 10

extern uint64_t renderer_present_count(void);
extern bool touch_first(int32_t *x, int32_t *y);  // runtime/hle/touch.c

static bool on_heap(uint64_t a) { return a >= HEAP_BASE && a < HEAP_BASE + HEAP_MAX - 0x1000 && !(a & 7); }
static float g32f(uint64_t a) { float f; uint32_t w = g32(a); memcpy(&f, &w, 4); return f; }

// ---- state (written by the game thread; boxes read by the renderers' thread under the lock)
static struct {
    uint64_t page;
    uint64_t path[NPANES][MAX_DEPTH];    // RootPane ... the letter's pane (then the blockers')
    int depth[NPANES];
    TlBox box[TL_COUNT];                 // for taps (neighbours trimmed: tl_trim)
    TlBox pane[TL_COUNT];                // the panes' own boxes
    bool hidden[NPANES];                 // hidden by us (its visible bit was set; letters only)
    TlBox block[NBLOCK], logo;           // the blockers' boxes; the logo's (letters and "2")
    bool down;                           // the touch, as last seen
    int start;                           // the letter the touch went down on
    bool bg;                             // ... or the background
    bool off;                            // it slid off its letter (or the letter plays: no sounds)
    int32_t x, y;
} T;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_seen;          // presented frame of the last state call (0: none)
static _Atomic bool g_on_title;
static _Atomic int g_data;               // the SMM1 dump: 0 not opened yet, 1 open, -1 none

static int now_tick(void) {  // (60 Hz, both threads: the renderers may present more frames than the game runs)
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int)((double)t.tv_sec * 60 + (double)t.tv_nsec * 60e-9);
}

bool title_letters_active(void) {
    uint64_t seen = g_seen;
    return g_on_title && seen && renderer_present_count() - seen < GONE_AFTER;
}

bool title_letters_box(int letter, TlBox *out) {
    if (letter < 0 || letter >= TL_COUNT || !title_letters_active()) return false;
    pthread_mutex_lock(&g_lock);
    *out = T.pane[letter];
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
    tl_letters_reset();
    tl_anim_reset();
    tl_spawn_reset();
    tl_effects_reset();
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
    for (int i = 0; i < NPANES; i++)
        if (find_path(root, pane_name[i], T.path[i], 0, &T.depth[i])) found += i < TL_COUNT;
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
    TlBox b[TL_COUNT], p[TL_COUNT];
    for (int i = 0; i < TL_COUNT; i++) p[i] = b[i] = T.depth[i] ? box_of(i) : (TlBox){0};
    tl_trim(b);
    pthread_mutex_lock(&g_lock);
    memcpy(T.box, b, sizeof b), memcpy(T.pane, p, sizeof p);
    pthread_mutex_unlock(&g_lock);
    for (int i = 0; i < TL_COUNT; i++) tl_anim_box(i, &p[i]);
    float x0 = 1280, y0 = 720, x1 = 0, y1 = 0;  // (the logo: the gaps between its letters aren't background)
    for (int i = 0; i < NBLOCK; i++) {
        TlBox q = T.block[i] = T.depth[TL_COUNT + i] ? box_of(TL_COUNT + i) : (TlBox){0};
        if (i) continue;
        x0 = fminf(x0, q.x), y0 = fminf(y0, q.y), x1 = fmaxf(x1, q.x + q.w), y1 = fmaxf(y1, q.y + q.h);
    }
    for (int i = 0; i < TL_COUNT; i++)
        if (p[i].w > 0) x0 = fminf(x0, p[i].x), y0 = fminf(y0, p[i].y), x1 = fmaxf(x1, p[i].x + p[i].w),
                        y1 = fmaxf(y1, p[i].y + p[i].h);
    T.logo = (TlBox){x0, y0, x1 - x0, y1 - y0, x1 > x0};
}

static bool in_box(const TlBox *b, int32_t x, int32_t y) {
    return b->visible && x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h;
}

// a tap on the course behind the title: off the logo and off SMM2's buttons in this state
static bool background(int state, int32_t x, int32_t y) {
    if (!state || in_box(&T.logo, x, y)) return false;
    for (int i = 0; i < NBLOCK; i++)
        if ((block_in[i] & state) && in_box(&T.block[i], x, y)) return false;
    return true;
}

// ---- the touch: a letter is tapped when the touch goes down and comes up on it (as SMM1's buttons: sliding off
// cancels)
static void letter_tapped(int letter, int32_t x, int32_t y) {
    int tick = now_tick();
    bool plays = g_data == 1 && tl_anim_start(letter, tick);
    LOG("[title] letter %s at %d,%d%s\n", tl_letter_name[letter], x, y,
        plays ? "" : g_data == 1 ? " (still playing: no new one)" : " (no SMM1 dump: nothing to play)");
    if (plays) tl_letter_decide(letter, tl_anim_on(letter, tick), &T.pane[letter], tick);
}

static void *open_dump(void *arg) {
    (void)arg;
    bool open = smm1_data_open();
    if (open) tl_effects_load();
    g_data = open ? 1 : -1;
    LOG("[title] SMM1 dump %s\n", g_data == 1 ? "open: the letters play" : "not found: the letters do nothing");
    return NULL;
}

static void follow_touch(int state) {
    bool taps = state != 0, ready = g_data == 1;
    int32_t x = T.x, y = T.y;
    int tick = now_tick();
    bool down = touch_first(&x, &y);
    if (down && !T.down && ready && taps && tl_anim_deciding(tick)) {  // (SMM1's title: none while a letter plays)
        LOG("[title] touch at %d,%d ignored: a letter plays\n", x, y);
        taps = false, state = 0;
    }
    if (down && !T.down) {
        T.start = taps ? tl_hit(T.box, x, y) : TL_NONE;
        T.bg = T.start == TL_NONE && background(state, x, y);  // (a tap that starts and ends on the course)
        T.off = false;
        if (T.start != TL_NONE && ready && tl_anim_ready(T.start, tick)) tl_effect_touch(T.start);
        else if (T.start != TL_NONE) T.off = true;  // (it plays: no sounds for this touch)
    }
    if (down && T.start != TL_NONE && !T.off && tl_hit(T.box, x, y) != T.start)  // (slid off: SMM1's cancel)
        T.off = true, tl_effect_untouch(T.start);
    if (down && T.bg && ready) tl_effect_drag((float)x, (float)y, tick, !T.down);
    if (!down && T.down && T.start != TL_NONE && tl_hit(T.box, T.x, T.y) == T.start) letter_tapped(T.start, T.x, T.y);
    if (!down && T.down && T.bg && ready) tl_effect_release((float)T.x, (float)T.y, tick);
    if (!down && T.down && T.bg && background(state, T.x, T.y)) tl_background_tap((float)T.x, (float)T.y);
    if (!down) T.start = TL_NONE, T.bg = false;
    T.down = down;
    if (down) T.x = x, T.y = y;
}

// once per title frame, after the game's own state function
static void title_frame(Cpu *c, uint64_t owner, int state) {
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
        static bool opening;
        pthread_t th;
        if (!opening && !pthread_create(&th, NULL, open_dump, NULL)) opening = true, pthread_detach(th);
        read_boxes();
        if (getenv("RECOMP_TITLE_LETTERS_LOG"))
            for (int i = 0; i < TL_COUNT; i++)
                LOG("[title]   %-5s %s box %.1f %.1f %.1f x %.1f%s\n", tl_letter_name[i], pane_name[i], T.box[i].x,
                    T.box[i].y, T.box[i].w, T.box[i].h, T.box[i].visible ? "" : " (hidden)");
        if (getenv("RECOMP_TITLE_LETTERS_LOG")) {
            for (int i = 0; i < NBLOCK; i++)
                LOG("[title]   blocks %s box %.1f %.1f %.1f x %.1f%s\n", pane_name[TL_COUNT + i], T.block[i].x,
                    T.block[i].y, T.block[i].w, T.block[i].h, T.block[i].visible ? "" : " (hidden)");
            LOG("[title]   logo box %.1f %.1f %.1f x %.1f\n", T.logo.x, T.logo.y, T.logo.w, T.logo.h);
        }
    }
    g_seen = now;
    read_boxes();
    tl_spawn_frame(c);
    follow_touch(state);
    int tick = now_tick();
    for (int i = 0; i < TL_COUNT; i++)  // (SMM1's letter drawn: SMM2's hidden, whatever SMM2's animations set)
        title_letters_hide(i, tl_anim_busy(i, tick));
    if (g_data == 1) tl_effects_frame(tick), tl_letters_frame(tick);
    tl_spawn_done();
}

// ---- the overlay (the renderers' thread)
const uint8_t *title_letters_image(int *w, int *h, int *full, uint64_t *seq) {
    static LQuads q;
    static uint8_t *px;
    static uint64_t n;
    if (g_data != 1) return NULL;
    if (!title_letters_active()) {  // (the title went without one of our hooks noticing: K's picture and music back)
        tl_effects_lost();
        return NULL;
    }
    q.n = 0;
    tl_anim_preload();  // (the letters' layouts ahead of their first tap: R's leg tip is known then)
    int tick = now_tick();
    bool any = tl_effects_draw(tick, &q, false);  // (A's stars behind the letters, the dust over them)
    any |= tl_anim_draw(tick, &q);
    any |= tl_effects_draw(tick, &q, true);
    if (!any) return NULL;
    if (!px && !(px = malloc(1280 * 720 * 4))) return NULL;
    memset(px, 0, 1280 * 720 * 4);
    lyt_raster(&q, px, 1280, 720, 1280, 720);
    *w = 1280, *h = 720, *full = 0, *seq = 1ull << 48 | ++n;  // (its own numbers: the renderer uploads on a change)
    return px;
}

// ---- the hooks
static void t_wait(Cpu *c) { uint64_t o = c->x[0]; guest_call_orig(c, T_WAIT); title_frame(c, o, ST_WAIT); }
static void t_totop(Cpu *c) { uint64_t o = c->x[0]; guest_call_orig(c, T_TOTOP); title_frame(c, o, 0); }
static void t_top(Cpu *c) { uint64_t o = c->x[0]; guest_call_orig(c, T_TOP); title_frame(c, o, ST_TOP); }
static void t_toplaymenu(Cpu *c) { uint64_t o = c->x[0]; guest_call_orig(c, T_TOPLAYMENU); title_frame(c, o, 0); }
static void t_playmenu(Cpu *c) {
    uint64_t o = c->x[0];
    guest_call_orig(c, T_PLAYMENU);
    title_frame(c, o, ST_PLAYMENU);
}
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
