// Unit tests for the SMM1 title letters' hit-testing (games/smm2/mods/title_letters/hit.c).
// usage: make test-title-letters
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../games/smm2/mods/title_letters/title_letters.h"

static int failures;
#define CHECK(name, cond) \
    do { if (!(cond)) { printf("FAIL %s: %s (line %d)\n", name, #cond, __LINE__); failures++; return; } } while (0)

// eleven boxes apart from each other (SUPER above, ten letters in a row below), as on SMM2's title
static void boxes_apart(TlBox b[TL_COUNT]) {
    b[TL_SUPER] = (TlBox){255, 135, 223, 62, true};
    for (int i = TL_M1; i < TL_COUNT; i++) b[i] = (TlBox){(float)(250 + 80 * (i - TL_M1)), 204, 70, 77, true};
}

static void test_centres(void) {
    TlBox b[TL_COUNT];
    boxes_apart(b);
    for (int i = 0; i < TL_COUNT; i++)
        CHECK("centre", tl_hit(b, (int)(b[i].x + b[i].w / 2), (int)(b[i].y + b[i].h / 2)) == i);
    printf("ok centres\n");
}

static void test_edges(void) {
    TlBox b[TL_COUNT];
    boxes_apart(b);
    for (int i = 0; i < TL_COUNT; i++) {
        int x0 = (int)b[i].x, y0 = (int)b[i].y, x1 = (int)(b[i].x + b[i].w), y1 = (int)(b[i].y + b[i].h);
        int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
        CHECK("inside corner", tl_hit(b, x0, y0) == i);
        CHECK("inside corner", tl_hit(b, x1 - 1, y1 - 1) == i);
        CHECK("left", tl_hit(b, x0 - 1, cy) == TL_NONE);
        CHECK("right", tl_hit(b, x1, cy) == TL_NONE);
        CHECK("top", tl_hit(b, cx, y0 - 1) == TL_NONE);
        CHECK("bottom", tl_hit(b, cx, y1) == TL_NONE);
    }
    CHECK("empty", tl_hit(b, 640, 600) == TL_NONE);
    printf("ok edges\n");
}

static void test_hidden(void) {
    TlBox b[TL_COUNT];
    boxes_apart(b);
    b[TL_K].visible = false;
    CHECK("hidden", tl_hit(b, (int)(b[TL_K].x + 10), (int)(b[TL_K].y + 10)) == TL_NONE);
    CHECK("others", tl_hit(b, (int)(b[TL_E].x + 10), (int)(b[TL_E].y + 10)) == TL_E);
    printf("ok hidden\n");
}

static void test_overlap(void) {
    TlBox b[TL_COUNT];
    boxes_apart(b);
    b[TL_I] = (TlBox){400, 204, 77, 77, true};  // R1 (410..480) under I
    b[TL_R1] = (TlBox){410, 204, 70, 77, true};
    CHECK("later wins", tl_hit(b, 450, 240) == TL_I);
    CHECK("only the earlier", tl_hit(b, 478, 240) == TL_R1);  // (I is 400..477: 478 is R1's alone)
    printf("ok overlap\n");
}

static void test_trim(void) {
    TlBox b[TL_COUNT];
    boxes_apart(b);
    // R and I as on the title: centres 46 px apart, 76.8 px wide
    b[TL_R1] = (TlBox){423 - 38.4f, 204, 76.8f, 76.8f, true};
    b[TL_I] = (TlBox){469 - 38.4f, 204, 76.8f, 76.8f, true};
    b[TL_O] = (TlBox){522 - 38.4f, 204, 76.8f, 76.8f, true};
    b[TL_A1] = (TlBox){100, 204, 70, 77, true};  // (apart: unchanged)
    tl_trim(b);
    CHECK("R right = I left", fabsf(b[TL_R1].x + b[TL_R1].w - b[TL_I].x) < 0.01f);
    CHECK("cut in the middle", fabsf(b[TL_I].x - 446) < 0.01f);
    CHECK("I right = O left", fabsf(b[TL_I].x + b[TL_I].w - b[TL_O].x) < 0.01f);
    CHECK("R left kept", b[TL_R1].x == 423 - 38.4f);
    CHECK("apart kept", b[TL_A1].x == 100 && b[TL_A1].w == 70);
    CHECK("SUPER kept", b[TL_SUPER].x == 255 && b[TL_SUPER].w == 223);
    CHECK("R side", tl_hit(b, 445, 240) == TL_R1);
    CHECK("I side", tl_hit(b, 447, 240) == TL_I);
    printf("ok trim\n");
}

static void test_names(void) {
    CHECK("names", !strcmp(tl_letter_name[TL_SUPER], "SUPER") && !strcmp(tl_letter_name[TL_R2], "R2"));
    printf("ok names\n");
}

int main(void) {
    test_centres();
    test_edges();
    test_hidden();
    test_overlap();
    test_trim();
    test_names();
    if (failures) { printf("%d failed\n", failures); return 1; }
    printf("title letters: all passed\n");
    return 0;
}
