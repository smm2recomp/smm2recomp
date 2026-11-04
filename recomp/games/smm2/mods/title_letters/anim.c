// SMM1 title letters: each letter's SMM1 button layout (Play_Title<X>Btn_0n, from the player's SMM1 dump) played over
// SMM2's logo, placed and scaled so that its letter covers SMM2's letter pane. While it plays SMM2's pane is hidden
// (title_letters.c asks tl_anim_busy); after it, SMM2's letter shows again.
//
// One animation per letter: a tap while the letter plays is ignored (SMM1's buttons take no new decide until their
// Decide animation is over). I and K are toggles in SMM1: the first tap plays DecideA and the letter stays SMM1's
// (the parakeet on the I, K pressed in) until the second tap plays DecideB.
//
// Threads: tl_anim_start / tl_anim_busy / tl_anim_box come from the game thread, tl_anim_draw from the renderers'
// (where the layouts are loaded: the layout player is not shared between threads). Ticks are 60 Hz on one clock.
// Pure layout code: tools/test_smm1ui --title-letter renders it without the game.
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "../smm1/lay.h"
#include "title_letters.h"

static const struct { const char *layout; bool toggle; } LETTER[TL_COUNT] = {
    {"Play_TitleSuperBtn_00", false}, {"Play_TitleMBtn_00", false}, {"Play_TitleABtn_00", false},
    {"Play_TitleRBtn_00", false},     {"Play_TitleIBtn_00", true},  {"Play_TitleOBtn_00", false},
    {"Play_TitleMBtn_01", false},     {"Play_TitleABtn_01", false}, {"Play_TitleKBtn_00", true},
    {"Play_TitleEBtn_00", false},     {"Play_TitleRBtn_01", false},
};
#define BOUNDS "B_Btn_00"  // every button layout's touch box: the letter at rest (R's pieces, M2's offset included)

enum { P_IDLE, P_PLAY, P_ON, P_OFF };  // P_PLAY: Decide / DecideA; P_ON: a toggle held on; P_OFF: DecideB

typedef struct {
    int phase, start;
    TlBox box;            // SMM2's pane (game pixels)
    bool have_box;
    // the renderers' side
    Lay *lay;
    bool tried, missing;
    int len_play, len_off;  // frames (0: not known yet)
    float gx, gy, gw, gh;   // the letter's rest rectangle in its layout (centre, size)
    int bound;              // the phase whose animation is bound
} Anim;
static Anim A[TL_COUNT];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *play_anim(int i) { return LETTER[i].toggle ? "DecideA" : "Decide"; }

static void advance(int i, int tick) {  // (the lock held)
    Anim *a = &A[i];
    if (a->phase == P_PLAY && a->len_play > 0 && tick - a->start >= a->len_play)
        a->phase = LETTER[i].toggle ? P_ON : P_IDLE;
    else if (a->phase == P_OFF && a->len_off > 0 && tick - a->start >= a->len_off)
        a->phase = P_IDLE;
}

bool tl_anim_start(int letter, int tick) {
    if (letter < 0 || letter >= TL_COUNT) return false;
    pthread_mutex_lock(&g_lock);
    Anim *a = &A[letter];
    advance(letter, tick);
    bool ok = !a->missing && (a->phase == P_IDLE || a->phase == P_ON);
    if (ok) a->phase = a->phase == P_ON ? P_OFF : P_PLAY, a->start = tick;
    pthread_mutex_unlock(&g_lock);
    return ok;
}

bool tl_anim_busy(int letter, int tick) {
    if (letter < 0 || letter >= TL_COUNT) return false;
    pthread_mutex_lock(&g_lock);
    Anim *a = &A[letter];
    advance(letter, tick);
    bool busy = a->phase != P_IDLE && !a->missing;
    pthread_mutex_unlock(&g_lock);
    return busy;
}

bool tl_anim_ready(int letter, int tick) {
    if (letter < 0 || letter >= TL_COUNT) return false;
    pthread_mutex_lock(&g_lock);
    Anim *a = &A[letter];
    advance(letter, tick);
    bool ok = !a->missing && (a->phase == P_IDLE || a->phase == P_ON);
    pthread_mutex_unlock(&g_lock);
    return ok;
}

bool tl_anim_on(int letter, int tick) {
    if (letter < 0 || letter >= TL_COUNT) return false;
    pthread_mutex_lock(&g_lock);
    Anim *a = &A[letter];
    advance(letter, tick);
    bool on = LETTER[letter].toggle && (a->phase == P_PLAY || a->phase == P_ON);
    pthread_mutex_unlock(&g_lock);
    return on;
}

void tl_anim_box(int letter, const TlBox *box) {
    if (letter < 0 || letter >= TL_COUNT) return;
    pthread_mutex_lock(&g_lock);
    A[letter].box = *box, A[letter].have_box = true;
    pthread_mutex_unlock(&g_lock);
}

void tl_anim_reset(void) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < TL_COUNT; i++) A[i].phase = P_IDLE, A[i].have_box = false;
    pthread_mutex_unlock(&g_lock);
}

// (the renderers' thread) the layout, its lengths and the glyph's rest rectangle, once
static bool open_letter(int i) {
    Anim *a = &A[i];
    if (a->tried) return a->lay != NULL;
    a->tried = true;
    Lay *l = lay_open(LETTER[i].layout);
    float c[2], s[2];
    if (!l || !lay_rect(l, BOUNDS, c, s, NULL) || s[0] < 1 || s[1] < 1) {
        fprintf(stderr, "[title] %s: not in the SMM1 dump\n", LETTER[i].layout);
        lay_free(l);
        pthread_mutex_lock(&g_lock);
        a->missing = true;
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    int lp = (int)ceilf(lay_length(l, play_anim(i))), lo = LETTER[i].toggle ? (int)ceilf(lay_length(l, "DecideB")) : 1;
    pthread_mutex_lock(&g_lock);
    a->lay = l, a->gx = c[0], a->gy = c[1], a->gw = s[0], a->gh = s[1];
    a->len_play = lp > 0 ? lp : 1, a->len_off = lo > 0 ? lo : 1;
    a->bound = P_IDLE;
    pthread_mutex_unlock(&g_lock);
    return true;
}

bool tl_anim_draw(int tick, LQuads *out) {
    bool any = false;
    for (int i = 0; i < TL_COUNT; i++) {
        pthread_mutex_lock(&g_lock);
        Anim *a = &A[i];
        advance(i, tick);
        int phase = a->phase, start = a->start;
        TlBox b = a->box;
        bool have = a->have_box;
        pthread_mutex_unlock(&g_lock);
        if (phase == P_IDLE || !have || !open_letter(i)) continue;
        Lay *l = a->lay;
        if (a->bound != phase) {  // (another animation: the previous one's values go)
            lay_stop(l, play_anim(i));
            if (LETTER[i].toggle) lay_stop(l, "DecideB");
            a->bound = phase;
        }
        float f = (float)(tick - start);
        if (phase == P_PLAY) lay_hold(l, play_anim(i), fminf(fmaxf(f, 0), (float)a->len_play));
        else if (phase == P_ON) lay_hold_end(l, play_anim(i));
        else lay_hold(l, "DecideB", fminf(fmaxf(f, 0), (float)a->len_off));
        // the letter's rest rectangle onto SMM2's pane, as large as fits (layout space: the screen's centre at 0, y up)
        float k = fminf(b.w / a->gw, b.h / a->gh), X = b.x + b.w / 2 - 640, Y = 360 - (b.y + b.h / 2);
        int from = out->n;
        lay_draw(l, 0, out);
        for (int q = from; q < out->n; q++)
            for (int v = 0; v < 4; v++) {
                float *p = out->q[q].pos[v];
                p[0] = X + k * (p[0] - a->gx), p[1] = Y + k * (p[1] - a->gy);
            }
        any |= out->n > from;
    }
    return any;
}
