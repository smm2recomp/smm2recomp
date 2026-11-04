// Audio > Mute in background (#76): the output gain while the window is unfocused / minimized / hidden or the app is
// in the background, faded. Header-only: runtime/services/audio.c (the SDL events, the output streams' gain) and
// tools/test_bg_mute.c. Nothing of the game's audio state changes: only the final output gain.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Why the game is away (any set = muted when the setting is on); set and cleared by paired SDL events
enum { BGM_UNFOCUSED = 1, BGM_MINIMIZED = 2, BGM_HIDDEN = 4, BGM_BACKGROUND = 8 };
#define BGM_FADE_NS 150000000ull  // (150 ms from full to silent, and back)
#define BGM_STEP_MAX_NS 50000000ull  // (a step counts at most 50 ms: an output held a while still fades back in)

static inline float bgmute_target(bool enabled, unsigned away) { return enabled && away ? 0.0f : 1.0f; }

// One output's fade: the gain now and when it was last stepped (zeroed: the first step jumps to the target)
typedef struct { float gain; uint64_t t; bool started; } BgFade;

// Moves the gain toward the target at 1 per BGM_FADE_NS of now_ns (any monotonic clock) and returns it
static inline float bgmute_step(BgFade *f, float target, uint64_t now_ns) {
    if (!f->started) { f->started = true, f->t = now_ns, f->gain = target; return target; }
    uint64_t dt = now_ns > f->t ? now_ns - f->t : 0;
    float d = (float)(dt < BGM_STEP_MAX_NS ? dt : BGM_STEP_MAX_NS) / (float)BGM_FADE_NS;
    f->t = now_ns;
    if (f->gain < target) f->gain = f->gain + d < target ? f->gain + d : target;
    else if (f->gain > target) f->gain = f->gain - d > target ? f->gain - d : target;
    return f->gain;
}
