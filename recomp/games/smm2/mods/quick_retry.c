// Quick level return: after a miss, the course is back at its start (or the checkpoint) in about 1 s instead of SMM2's
// 4.5-10 (M-series Mac, 2026-11-06: 100 Mario about 1.1 s, Course World about 1 s; most of what is left is the course's
// rebuild, about 0.8 s of the game's own work). The game itself does the miss, the lives and the restart. The frames
// from the death fanfare until the player is updated again in the restarted course run as fast as the machine can,
// without being shown (runtime/nvn/objects.c g_present_hidden), and the parts of SMM2's miss that only animate or wait
// are cut short (below: "SMM2's own miss, shortened"). Lives count as before (100 Mario and Endless: a miss still costs
// one) and clear rules don't change; the fanfare is only heard for as long as it lasts.
//
// The miss is the death fanfare (music state 7, mods/costume.c down_fanfare). The restart is, in Endless and 100 Mario,
// the Endless scene's play state running again (after the lives card; mods/bridge.c), elsewhere the first player
// update after a stretch of frames without one (the course is torn down and built again). A cap ends the
// fast-forward whatever happens (a game over, a menu), and outside Endless it lapses within a second where no player
// is updated (test play's miss goes back to the Course Maker), so the screen never stays still for long.
//
// One switch in the Mods tab (off by default), kept in smm2mods.ini; an environment variable wins over the ini:
//   RECOMP_QUICK_RETRY=0/1   quick_retry=0/1
// Off, nothing at all changes. (Tests: RECOMP_QUICK_RETRY_SHORT=0 keeps SMM2's own timing, only fast-forwarded; =alt
// shortens every other miss, for side-by-side timings in one run.)
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mod.h"
#include "../../../runtime/rt.h"

typedef struct { const char *label, *desc; bool *value; bool def; } GameModToggle;  // (runtime/game_hooks.c)
void mod_ini_set(const char *key, const char *val);                                 // (settings.c)
extern _Atomic int g_present_hidden;                                                // runtime/nvn/objects.c
extern uint64_t renderer_present_count(void);

#define CAP_FRAMES 360  // at most this many frames after the miss are fast-forwarded (SMM2's own miss to restart: ~300)
#define CAP_ENDLESS 900 // ... in Endless and 100 Mario, lives card included (SMM2's own: ~545)
#define LEASE_FRAMES 60 // the fast-forward lasts this long past the last player update (none: a scene without a player)
#define GAP_FRAMES 3    // frames without a player update: the course is being built again

// ---------------------------------------------------------------- the setting
static bool g_on, g_shown;
static void load_once(void) {
    const char *ini = getenv("RECOMP_MOD_INI") ? getenv("RECOMP_MOD_INI") : "smm2mods.ini";
    FILE *f = fopen(ini, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "quick_retry=", 12)) g_on = atoi(line + 12) != 0;
        fclose(f);
    }
    if (getenv("RECOMP_QUICK_RETRY")) g_on = atoi(getenv("RECOMP_QUICK_RETRY")) != 0;
    LOG("[mod] quick level return %s\n", g_on ? "on" : "off");
}
static bool retry_on(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, load_once);
    return g_on;
}

int quick_retry_toggles(GameModToggle *out, int room) {  // (settings.c appends it to the Mods tab)
    static const GameModToggle t = {
        "Quick level return",
        "After a miss the course starts again (from its start or the checkpoint) in about a second: the miss, the "
        "lives card and the restart are fast-forwarded and cut short, unseen. Lives still count as before (100 Mario, Endless) and "
        "clears don't change. Off: SMM2's own miss. Also in test play. Immediately.",
        &g_shown, false};
    if (room < 1) return 0;
    g_shown = retry_on();
    out[0] = t;
    return 1;
}
void quick_retry_toggles_changed(void) {
    if (g_shown == retry_on()) return;
    g_on = g_shown;
    mod_ini_set("quick_retry", g_on ? "1" : "0");
    LOG("[mod] quick level return %s\n", g_on ? "on" : "off");
}

// ---------------------------------------------------------------- the fast-forward
// (presented-frame clock) the miss; the last player update; the last frame of the Endless scene's play state
static bool g_ff, g_endless;
static uint64_t g_ff_from, g_player_at, g_play_at;
static uint64_t g_ff_ns, g_player_ns;  // (wall clock) the miss; the last player update (the log's split)
extern uint64_t plat_now_ns(void);

// ---------------------------------------------------------------- SMM2's own miss, shortened
// While a miss is fast-forwarded, the parts of SMM2's miss that only animate or wait are cut short as well (SMM2
// v3.0.3; nothing of the game's code is changed, only frame counters and states it reads each frame):
//   - the player's death event (player +0x2438, or +0x2448 after a car ride; its state machine's current state at
//     +0x70, a frame timer at +0xb0 the event counts down): the Pose (45 frames) and the Fall (140; 20 in test play)
//     end at once. They only animate: the miss post (Endless), the scene to go to, the lives and the checkpoint are
//     handled at the event's start and at the scene change. The Pose is cut only for the last player standing
//     (+0x50): in co-op the others still get their bubble at the game's time.
//   - the scene fade (the UISceneFadeSeq page; its state at +0x240: 2 out, 4 in): the fade out is ended on its next
//     frame (+0x28c, the type it reads each frame, := 8, the instant cut) and the fade in is instant (+0x290 := 8), up
//     to the course's own fade in after the restart. The page is resident: it is looked up once (the game's own
//     lookup by its path, from the player update) and kept.
//   - the scene manager's leaving phase waits for the master volume's fade out (about 0.5 s; *(*0x2a39150 + 0x7e0)):
//     the volume is put at its target at once (the fast-forward is silent anyway).
//   - Endless and 100 Mario's CourseIn scene (the lives card between the miss and the course again; the scene
//     manager's current scene, *(*0x2a390d8 + 0x330)): its main state 5 waits for the card page's appear animation
//     (about 180 frames) to say it's done: it goes on to 8 once the fade in is over (+0x3bf), as it would (states 6
//     and 8 have nothing to do in Endless: no 60-frame minimum there). The card page's appear (3) and leave (5) states
//     go on at once too (single-function states: a state written is a state taken); its leave would wait for the
//     appear animation's end. The page is the CourseIn scene's +0x3d0, a new one with each scene.
#define EV_STATE 0x70
#define EV_TIMER 0xb0
#define EV_LAST 0x50
#define PAGE_LOOKUP MAIN_ADDR(0x1c989e0)  // the UI page lookup by path: x0 a sead::SafeString {vtable, chars}
#define SAFESTRING_VT MAIN_ADDR(0x28522b0)
#define PAGE_STATE 0x240
#define PAGE_OUT_TYPE 0x28c
#define PAGE_IN_TYPE 0x290
#define SCENE_MGR MAIN_ADDR(0x2a390d8)    // ptr: the scene manager (+0x330 the current scene)
#define COURSE_IN_VT MAIN_ADDR(0x295fc58)     // the CourseIn scene's vtable (its +0)
#define MGR_PHASE 0x690                       // the scene manager's phase: 2 the old scene leaving (its waits)
#define AUDIO MAIN_ADDR(0x2a39150)            // ptr: the audio system; +0x7e0 the master volume V
#define CI_FADE_SEEN 0x3bf                    // byte: CourseIn's WaitFadeToCourseIn saw the fade in end (and posted)
#define CI_PAGE 0x3d0                         // ptr: the lives card's UI page (Play_CourseIn_00; state +0x240)
#define CI_MAIN 0x2a8
static const uint32_t k_events[] = {0x2438, 0x2448};  // the player's death events with a Pose (0) and a Fall (1)
static uint64_t g_page;                               // the fade page (0: not looked up yet; 1: not found)
static int g_tail;                                    // frames left to watch the course's fade in after the restart
static bool g_short = true;  // this miss is shortened (RECOMP_QUICK_RETRY_SHORT=alt: every other one)
static int short_mode(void) {  // 0 off, 1 on, 2 every other miss
    static int v = -1;
    const char *e = getenv("RECOMP_QUICK_RETRY_SHORT");
    if (v < 0) v = !e ? 1 : !strcmp(e, "alt") ? 2 : atoi(e) != 0;
    return v;
}
static bool short_on(void) { return short_mode() && g_short; }
static bool heap_ptr(uint64_t a) { return a >= HEAP_BASE && a < HEAP_BASE + HEAP_MAX - 0x1000 && !(a & 7); }

static void short_demo(uint64_t player) {
    if (!heap_ptr(player)) return;
    for (size_t i = 0; i < sizeof k_events / sizeof *k_events; i++) {
        uint64_t ev = g64(player + k_events[i]);
        if (!heap_ptr(ev)) continue;
        uint32_t st = g32(ev + EV_STATE);
        int32_t t = (int32_t)g32(ev + EV_TIMER);
        if (t <= 1 || st > 1 || (st == 0 && !g8(ev + EV_LAST))) continue;
        s32(ev + EV_TIMER, 1);
        LOG("[quick-retry] death event +%#x: %s cut short (%d frames left)\n", k_events[i], st ? "Fall" : "Pose", t);
    }
}

static void find_page(Cpu *c) {
    if (g_page) return;
    static uint64_t str, chars;
    static const char path[] = "ResidentUISequenceHolder/ResidentSeq/UISceneFadeSeq";
    if (!str) {
        str = (uint64_t)(uintptr_t)hle_alloc(16), chars = (uint64_t)(uintptr_t)hle_alloc(sizeof path);
        memcpy((void *)(uintptr_t)chars, path, sizeof path);
        s64(str, SAFESTRING_VT), s64(str + 8, chars);
    }
    Cpu save = *c;
    c->x[0] = str;
    guest_call(c, PAGE_LOOKUP);
    uint64_t page = c->x[0];
    *c = save;
    uint32_t st = heap_ptr(page) ? g32(page + PAGE_STATE) : 99;
    g_page = st <= 4 ? page : 1;
    LOG("[quick-retry] fade page %s (%#llx, state %u)\n", g_page > 1 ? "found" : "not found", (unsigned long long)page, st);
}

static uint64_t course_in(void) {  // the CourseIn scene when it is the current scene, else 0
    uint64_t m = g64(SCENE_MGR), sc = heap_ptr(m) ? g64(m + 0x330) : 0;
    return heap_ptr(sc) && g64(sc) == COURSE_IN_VT ? sc : 0;
}

// Each presented frame, hidden or not (from the overlay's tick: the render thread; the writes are single words the
// game reads each frame, made only in the states they end)
void quick_retry_frame(void) {
    if (!short_on()) return;
    bool active = g_ff && atomic_load(&g_present_hidden) > 0;  // (a fast-forward that lapsed with no player: over)
    if (active) g_tail = 120;
    else if (g_tail <= 0) return;
    uint64_t pg = g_page > 1 ? g_page : 0, m = g64(SCENE_MGR);
    if (!active) {  // after it: the restarted course's own fade in (after its first player update) instant too, then back
        if (pg && g32(pg + PAGE_STATE) == 4 && g32(pg + PAGE_IN_TYPE) != 8) s32(pg + PAGE_IN_TYPE, 8);
        if (--g_tail <= 0 || (pg && g32(pg + PAGE_STATE) == 1)) {
            if (pg && g32(pg + PAGE_IN_TYPE) == 8) s32(pg + PAGE_IN_TYPE, 0);  // (the In clears it; none came: here)
            g_tail = 0;
        }
        return;
    }
    // the scene fade: out ended on its next frame, in instant
    if (pg) {
        if (g32(pg + PAGE_STATE) == 2 && g32(pg + PAGE_OUT_TYPE) != 8) s32(pg + PAGE_OUT_TYPE, 8);
        if (g32(pg + PAGE_IN_TYPE) != 8) s32(pg + PAGE_IN_TYPE, 8);
    }
    // the scene manager's leaving phase waits for the master volume's fade (about 0.5 s): it's at its target at once
    // (the fast-forward is silent anyway)
    if (heap_ptr(m) && g32(m + MGR_PHASE) == 2) {
        uint64_t au = g64(AUDIO), v = heap_ptr(au) ? g64(au + 0x7e0) : 0;
        if (heap_ptr(v) && g32(v + 8) != g32(v + 0x10)) {
            uint32_t target = g32(v + 0x10);
            s32(v + 8, target), s32(v + 0xc, target), s32(v + 0x14, 0);
        }
    }
    // Endless / 100 Mario: the lives card's appear animation (about 180 frames) isn't waited for
    // (the card's page: 3 its appear animation, 4 shown; the scene goes on once it is shown and then leaves)
    uint64_t ci = course_in();
    if (ci && g32(ci + CI_MAIN) == 5 && g8(ci + CI_FADE_SEEN) == 1) {
        s32(ci + CI_MAIN, 8);
        LOG("[quick-retry] lives card: not waited for\n");
    }
    uint64_t card = ci ? g64(ci + CI_PAGE) : 0;
    if (ci && g32(ci + CI_MAIN) == 0xb && heap_ptr(card) && g32(card + PAGE_STATE) == 3 && g32(card + PAGE_STATE + 4) >= 1) {
        s32(card + PAGE_STATE, 4);
        LOG("[quick-retry] lives card: its appear animation not waited for\n");
    }
    if (ci && g32(ci + CI_MAIN) == 0xb && heap_ptr(card) && g32(card + PAGE_STATE) == 5 && g32(card + PAGE_STATE + 4) >= 1) {
        s32(card + PAGE_STATE, 6);  // (5: its leave animation, which waits for the appear animation's end)
        LOG("[quick-retry] lives card: its leave animation not waited for\n");
    }
}

static void ff_end(void) {
    g_tail = 120;  // (the course's own fade in, which comes after its first player update: instant too)
    g_ff = false;
}
static void stop(const char *why) {
    atomic_store(&g_present_hidden, 0);
    ff_end();
    uint64_t ns = plat_now_ns();
    LOG("[quick-retry] %s after %llu frames, %.2f s (the miss %llu frames, %.2f s; then %.2f s)\n", why,
        (unsigned long long)(renderer_present_count() - g_ff_from), (ns - g_ff_ns) / 1e9,
        (unsigned long long)(g_player_at - g_ff_from), (g_player_ns - g_ff_ns) / 1e9, (ns - g_player_ns) / 1e9);
}

bool quick_retry_on(void) { return retry_on(); }  // (smm1_extras.c: the full death fanfare is left out with it)
void quick_retry_miss(void) {  // mods/costume.c: the death fanfare starts
    if (!retry_on()) return;
    uint64_t now = renderer_present_count();
    // (Endless and 100 Mario: the play state ran until now; the course comes back to it after the lives card)
    g_endless = g_play_at && now - g_play_at <= GAP_FRAMES;
    g_ff = true, g_ff_from = g_player_at = now;
    g_ff_ns = g_player_ns = plat_now_ns();
    if (short_mode() == 2) {
        static int n;
        g_short = (n++ & 1) == 0;
        LOG("[quick-retry] this miss: %s\n", g_short ? "shortened" : "SMM2's own timing");
    }

    // (Endless and 100 Mario: no player while the lives card is up, so no lease; the play state's return ends it)
    atomic_store(&g_present_hidden, g_endless ? CAP_ENDLESS : LEASE_FRAMES);
    LOG("[quick-retry] miss at frame %llu: fast-forward\n", (unsigned long long)now);
}

// Every player update (games/smm2/trace.c). Each one keeps the fast-forward going (LEASE_FRAMES more), up to the cap:
// where no player comes (a menu, the Course Maker) it runs out by itself. Course World, Coursebot, test play...: the
// course is built again (no player for a few frames), and the first update after that is the restarted course.
void quick_retry_player(Cpu *c, uint64_t self) {
    if (!g_ff) return;
    if (short_on()) find_page(c), short_demo(self);
    uint64_t now = renderer_present_count();
    if (g_endless) {
        if (!atomic_load(&g_present_hidden)) ff_end(), LOG("[quick-retry] cap reached\n");
    } else if (now - g_player_at > GAP_FRAMES) {
        stop("restarted");
    } else if (now - g_ff_from >= CAP_FRAMES) {
        stop("cap reached");
    } else {
        atomic_store(&g_present_hidden, LEASE_FRAMES);
    }
    g_player_at = now, g_player_ns = plat_now_ns();
}

// Every frame of the Endless scene's play state (mods/bridge.c, after the 100 Mario flow lets the course go): Endless
// and 100 Mario keep the player through the lives card, the play state's return is the restart.
void quick_retry_play_state(void) {
    uint64_t now = renderer_present_count();
    if (g_ff && g_endless && now - g_play_at > GAP_FRAMES) stop("restarted (Endless)");
    g_play_at = now;
}
