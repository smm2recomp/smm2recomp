// Unit tests for Controls > Sticks (runtime/hle/stick_deadzone.h, #98): off leaves a stick untouched, a stick inside
// the dead zone reads 0, outside it the tilt is rescaled from the dead zone's edge, a full tilt stays full, a corner
// comes back onto the circle.
// usage: make test-stick-deadzone
#include <stdio.h>

#include "../runtime/hle/stick_deadzone.h"

static int failures;
#define CHECK(name, cond) \
    do { if (!(cond)) { printf("FAIL %s: %s (line %d)\n", name, #cond, __LINE__); failures++; return; } } while (0)

static int32_t ox, oy;  // the stick after its dead zone
static void dz(int32_t x, int32_t y, int pct) {
    ox = x, oy = y;
    stick_deadzone(&ox, &oy, pct);
}

static void test_off(void) {
    dz(32767, 32767, 0);
    CHECK("off: a corner as it is", ox == 32767 && oy == 32767);
    dz(-32768, 0, 0);
    CHECK("off: SDL's lowest value as it is", ox == -32768 && oy == 0);
    dz(1500, -1500, 0);
    CHECK("off: a drifting stick as it is", ox == 1500 && oy == -1500);
    dz(1500, -1500, -5);
    CHECK("below 0: off", ox == 1500 && oy == -1500);
}

static void test_inside(void) {  // (10%: 3276.7)
    dz(3276, 0, 10);
    CHECK("just inside", ox == 0 && oy == 0);
    dz(1500, -1500, 10);
    CHECK("a drifting stick (6.5%) rests", ox == 0 && oy == 0);
    dz(0, 0, 10);
    CHECK("the centre", ox == 0 && oy == 0);
}

static void test_outside(void) {
    dz(3277, 0, 10);
    CHECK("just outside: a third of a step, still 0", ox == 0 && oy == 0);
    dz(3300, 0, 10);
    CHECK("starts from 0 at the edge", ox == 26 && oy == 0);
    dz(4000, 0, 10);
    CHECK("rescaled", ox == 804 && oy == 0);
    dz(0, -4000, 10);
    CHECK("the same along y, down", ox == 0 && oy == -804);
    dz(32767, 0, 10);
    CHECK("a full tilt stays full", ox == 32767 && oy == 0);
    dz(-32768, 0, 10);
    CHECK("SDL's lowest value: full", ox == -32767 && oy == 0);
}

static void test_corner(void) {
    dz(32767, 32767, 10);
    CHECK("a corner keeps its direction", ox == oy);
    // on the circle: 32767 / sqrt 2 = 23169.8, each axis rounded to the nearest step
    CHECK("and comes back onto the circle", ox == 23170);
}

static void test_wide(void) {
    dz(32767, 0, 99);
    CHECK("99%: a full tilt is still full", ox == 32767 && oy == 0);
    dz(32767, 0, 100);
    CHECK("100 and over count as 99 (no division by 0)", ox == 32767 && oy == 0);
    dz(32000, 0, 99);
    CHECK("99%: inside", ox == 0 && oy == 0);
}

int main(void) {
    test_off();
    test_inside();
    test_outside();
    test_corner();
    test_wide();
    if (failures) { printf("%d FAILED\n", failures); return 1; }
    printf("stick dead zone: all passed\n");
    return 0;
}
