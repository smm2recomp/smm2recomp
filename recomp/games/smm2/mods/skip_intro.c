// Boot intro skip: skip the Mario logo presentation animation directly to the title screen.
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <stdatomic.h>

#include "mod.h"
#include "../../../runtime/settings.h"

#define STATE_CHANGE MAIN_ADDR(0x8b9320)

extern uint64_t renderer_present_count(void);
extern uint64_t g_test_buttons, g_test_buttons_until;  // runtime/game_hooks.c, read by runtime/hle/input.c

static volatile uint64_t g_boot_hook_at, g_boot_hook_first, g_boot_state;  // the logo's hooks last ran / first ran / state
static void boot_note(Cpu *c) {
    uint64_t now = renderer_present_count();
    g_boot_hook_at = now;
    if (!g_boot_hook_first) g_boot_hook_first = now;
    g_boot_state = g32(c->x[0] + 0x240);
}
static void boot_cappear(Cpu *c) {
    boot_note(c);
    if (!g_settings.skip_intro || g_settings.skip_mode_select) {  // (the two aren't used together: mode select wins)
        guest_call_orig(c, MAIN_ADDR(0x17E4340));
        return;
    }
    uint64_t seq = c->x[0];
    uint32_t step = g32(seq + 0x240);
    if (step == 0) {
        guest_call_orig(c, MAIN_ADDR(0x17E4340));
        s8(seq + 0x28d, 1);
        c->x[0] = seq + 0x238;
        c->x[1] = 4;
        guest_call(c, STATE_CHANGE);
    } else {
        c->x[0] = seq + 0x238;
        c->x[1] = 4;
        guest_call(c, STATE_CHANGE);
    }
    c->x[0] = 0;
}

static void boot_cinmain(Cpu *c) {
    boot_note(c);
    if (!g_settings.skip_intro || g_settings.skip_mode_select) {  // (the two aren't used together: mode select wins)
        guest_call_orig(c, MAIN_ADDR(0x17E49F0));
        return;
    }
    uint64_t seq = c->x[0];
    uint32_t step = g32(seq + 0x240);
    if (step == 0) {
        guest_call_orig(c, MAIN_ADDR(0x17E49F0));
        s8(seq + 0x28d, 1);
        c->x[0] = seq + 0x238;
        c->x[1] = 4;
        guest_call(c, STATE_CHANGE);
    } else {
        c->x[0] = seq + 0x238;
        c->x[1] = 4;
        guest_call(c, STATE_CHANGE);
    }
    c->x[0] = 0;
}

// ---- Skip mode select: from the title screen straight into Course World, as a player would go: L+R opens the Make /
// Play menu, then Play, then Course World. The presses go through the same injection the 100 Mario driver uses
// (g_test_buttons), so the game sees an ordinary controller. Each press waits for the scene manager to be quiet
// (no scene change under way) and for the screen to have settled, so animations and fades never eat a press.
#define SCENE_MGR MAIN_ADDR(0x2a390d8)  // *: the scene manager; +0x33C target scene (u32), +0x344 change under way
enum { BTN_A = 1 << 0, BTN_L = 1 << 6, BTN_R = 1 << 7, BTN_LEFT = 1 << 12, BTN_UP = 1 << 13, BTN_RIGHT = 1 << 14,
       BTN_DOWN = 1 << 15 };  // nn::hid button bits (runtime/hle/input.c)

typedef struct { uint64_t buttons; int settle; const char *what; } MsStep;
static const MsStep g_ms_steps[] = {
    {BTN_L | BTN_R, 0,   "L+R on the title screen"},
    {BTN_RIGHT,     200, "Play"},
    {BTN_A,         60,  "open Play"},
    {BTN_DOWN,      300, "Course World"},
    {BTN_A,         60,  "open Course World"},
};

extern _Atomic int g_present_hidden;  // runtime/nvn/objects.c: frames run unpaced and unseen (counts down per frame)
extern volatile int g_audio_muted;  // runtime/services/audio.c
extern uint64_t input_player_buttons(void);  // runtime/hle/input.c: the player's own buttons

static double ms_wall(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}

// Hardened against slow starts: a press only goes out once the game has run at a steady frame rate for 2 s (first-run
// shader compiles and asset loads stall the frames, and a press during a stall is lost), the settle delays count in
// presented frames (so they stretch with a slow machine), and the player's own buttons cancel the sequence.
static void *ms_driver(void *arg) {
    (void)arg;
    uint64_t next_at = 0, win_frames = 0, steady_since = 0;
    double win_t = 0;
    size_t step = 0;
    bool logging = getenv("RECOMP_SKIP_MODE_SELECT_LOG") != NULL, started = false;
    uint64_t last_target = ~0ull, done_at = 0;
    double t_start = 0;
    uint64_t g_ms_logged_at = 0;
    for (;;) {
        usleep(20000);
        uint64_t now = renderer_present_count();
        if (logging && now != g_ms_logged_at && now % 60 == 0) {
            g_ms_logged_at = now;
            LOG("[mod] skip mode select: (diag) frame %llu hooks first %llu last %llu state %llu\n", (unsigned long long)now,
                (unsigned long long)g_boot_hook_first, (unsigned long long)g_boot_hook_at, (unsigned long long)g_boot_state);
        }
        if (!g_settings.skip_mode_select) continue;
        uint64_t s = g64(SCENE_MGR);
        if (!s) continue;
        uint32_t target = g32(s + 0x33c);
        if (logging && target != last_target) LOG("[mod] skip mode select: scene 0x%x at frame %llu\n", target, (unsigned long long)now);
        last_target = target;
        // The presses run unseen and unpaced (the window stays black), then Course World appears: the lease is
        // renewed while the sequence runs and let go ~1.5 s of game time after the last press, or after 120 s.
        if (!t_start) t_start = ms_wall();
        if (ms_wall() - t_start > 120.0) { atomic_store(&g_present_hidden, 0), g_audio_muted = 0; break; }
        if (step >= sizeof g_ms_steps / sizeof *g_ms_steps) {
            if (!done_at) done_at = now;
            if (now - done_at >= 90) { atomic_store(&g_present_hidden, 0), g_audio_muted = 0; break; }
        }
        atomic_store(&g_present_hidden, 300), g_audio_muted = 1;
        if (step >= sizeof g_ms_steps / sizeof *g_ms_steps) continue;  // (all pressed: waiting for the screen)
        if (started && input_player_buttons()) {  // the player took over
            LOG("[mod] skip mode select: cancelled, the player pressed a button\n");
            g_test_buttons = 0;
            atomic_store(&g_present_hidden, 0), g_audio_muted = 0;
            break;
        }
        // frame rate over one-second windows: steady = at least 45 fps and no change pending in the scene manager
        double t = ms_wall();
        if (!win_t) win_t = t, win_frames = now;
        if (t - win_t >= 1.0) {
            bool ok = (double)(now - win_frames) >= 45.0 * (t - win_t) && !g8(s + 0x344);
            if (!ok) steady_since = 0, next_at = 0;
            else if (!steady_since) steady_since = now;
            win_t = t, win_frames = now;
        }
        // the title screen is up once the boot logo's hooks have run and gone quiet (with Skip intro off the logo plays
        // for ~1100 frames and any press before then would be lost); if they never run, go after 2400 frames
        bool title_up = (g_boot_hook_first && now - g_boot_hook_at >= 30) || now >= 2400;
        if (!title_up || !steady_since || now - steady_since < 120) continue;
        if (!next_at) next_at = now + g_ms_steps[step].settle;
        if (now < next_at) continue;
        LOG("[mod] skip mode select: %s (frame %llu, scene 0x%x)\n", g_ms_steps[step].what, (unsigned long long)now, target);
        g_test_buttons = g_ms_steps[step].buttons, g_test_buttons_until = now + 8;
        started = true;
        step++;
        next_at = step < sizeof g_ms_steps / sizeof *g_ms_steps ? now + g_ms_steps[step].settle + 8 : 0;
    }
    return NULL;
}

__attribute__((constructor)) static void skip_intro_register(void) {
    pthread_t th;
    if (pthread_create(&th, NULL, ms_driver, NULL) == 0) pthread_detach(th);
    hle_override(MAIN_ADDR(0x17E4340), boot_cappear, "mod: skip boot intro presentation (cAppear)");
    hle_override(MAIN_ADDR(0x17E49F0), boot_cinmain, "mod: skip boot intro presentation (cInMain)");
}

