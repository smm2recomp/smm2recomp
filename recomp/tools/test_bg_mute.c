// Unit tests for Audio > Mute in background (runtime/services/bg_mute.h, #76): the target gain from the setting and
// the away flags, and the fade toward it (length, reversal mid-fade, a held output, the clock).
// usage: make test-bg-mute
#include <math.h>
#include <stdio.h>

#include "../runtime/services/bg_mute.h"

static int failures;
#define CHECK(name, cond) \
    do { if (!(cond)) { printf("FAIL %s: %s (line %d)\n", name, #cond, __LINE__); failures++; return; } } while (0)
#define NEAR(a, b) (fabsf((a) - (b)) < 1e-4f)
#define MS 1000000ull

static void test_target(void) {
    CHECK("off, focused", bgmute_target(false, 0) == 1);
    CHECK("off, away: the setting is off", bgmute_target(false, BGM_UNFOCUSED | BGM_BACKGROUND) == 1);
    CHECK("on, focused", bgmute_target(true, 0) == 1);
    CHECK("on, unfocused", bgmute_target(true, BGM_UNFOCUSED) == 0);
    CHECK("on, minimized", bgmute_target(true, BGM_MINIMIZED) == 0);
    CHECK("on, hidden", bgmute_target(true, BGM_HIDDEN) == 0);
    CHECK("on, background", bgmute_target(true, BGM_BACKGROUND) == 0);
}

static void test_start(void) {
    BgFade f = {0};
    CHECK("first step jumps (muted at start)", bgmute_step(&f, 0, 1000 * MS) == 0);
    BgFade g = {0};
    CHECK("first step jumps (full at start)", bgmute_step(&g, 1, 0) == 1);
    CHECK("stays", bgmute_step(&g, 1, 5 * MS) == 1);
}

static void test_fade(void) {
    BgFade f = {0};
    uint64_t t = 1000 * MS;
    bgmute_step(&f, 1, t);
    float g = 1;
    int steps = 0;
    for (; g > 0 && steps < 1000; steps++) g = bgmute_step(&f, 0, t += 5 * MS);  // (5 ms audio frames)
    CHECK("fades out to silence", g == 0);
    CHECK("in 150 ms (30 frames; +1 for rounding)", steps == 30 || steps == 31);
    CHECK("stays silent", bgmute_step(&f, 0, t += 5 * MS) == 0);
    for (steps = 0; g < 1 && steps < 1000; steps++) g = bgmute_step(&f, 1, t += 5 * MS);
    CHECK("fades back in", g == 1 && (steps == 30 || steps == 31));
    CHECK("never above 1", bgmute_step(&f, 1, t += 400 * MS) == 1);
}

static void test_halfway(void) {
    BgFade f = {0};
    uint64_t t = 0;
    bgmute_step(&f, 1, t);
    float g = 0;
    for (int i = 0; i < 3; i++) g = bgmute_step(&f, 0, t += 25 * MS);
    CHECK("75 ms: half", NEAR(g, 0.5f));
    g = bgmute_step(&f, 1, t += 30 * MS);
    CHECK("reversed mid-fade: back up from where it was", NEAR(g, 0.7f));
    g = bgmute_step(&f, 1, t += 46 * MS);
    CHECK("then full", g == 1);
}

static void test_held(void) {  // (an output thread held, e.g. iOS in the background: no jump back to full)
    BgFade f = {0};
    uint64_t t = 0;
    bgmute_step(&f, 0, t);
    float g = bgmute_step(&f, 1, t += 10000 * MS);
    CHECK("a long gap counts as 50 ms", NEAR(g, 1.0f / 3));
    g = bgmute_step(&f, 1, t += 5 * MS);
    CHECK("then fades on", NEAR(g, 1.0f / 3 + 5.0f / 150));
}

static void test_clock(void) {
    BgFade f = {0};
    bgmute_step(&f, 1, 500 * MS);
    CHECK("a clock going back: no step", bgmute_step(&f, 0, 400 * MS) == 1);
    CHECK("then from there", NEAR(bgmute_step(&f, 0, 430 * MS), 0.8f));
    CHECK("same time: no step", NEAR(bgmute_step(&f, 0, 430 * MS), 0.8f));
}

int main(void) {
    test_target();
    test_start();
    test_fade();
    test_halfway();
    test_held();
    test_clock();
    if (failures) { printf("%d FAILED\n", failures); return 1; }
    printf("background mute: all passed\n");
    return 0;
}
