// SMM1 title letters: which letter a tap is on (pure: tools/test_title_letters.c).
#include "title_letters.h"

const char *const tl_letter_name[TL_COUNT] = {"SUPER", "M", "A", "R", "I", "O", "M2", "A2", "K", "E", "R2"};

static bool inside(const TlBox *b, int x, int y) {
    return b->visible && (float)x >= b->x && (float)x < b->x + b->w && (float)y >= b->y && (float)y < b->y + b->h;
}

int tl_hit(const TlBox boxes[TL_COUNT], int x, int y) {
    for (int i = TL_COUNT - 1; i >= 0; i--)  // (the later box is drawn over the earlier)
        if (inside(&boxes[i], x, y)) return i;
    return TL_NONE;
}

void tl_trim(TlBox boxes[TL_COUNT]) {
    for (int i = TL_M1; i + 1 < TL_COUNT; i++) {  // (the row M ... R2, left to right; SUPER is above it)
        TlBox *a = &boxes[i], *b = &boxes[i + 1];
        float ar = a->x + a->w, br = b->x + b->w;
        if (ar <= b->x || b->x <= a->x || ar >= br) continue;  // (apart, or not side by side)
        float mid = (b->x + ar) / 2;
        a->w = mid - a->x;
        b->w = br - mid, b->x = mid;
    }
}
