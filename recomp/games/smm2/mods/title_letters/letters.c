// SMM1 title letters: what each letter does on the title course, at SMM1's moments (title_letters.h). The sounds and
// the screen effects are effects.c's, the actors spawn.c's; this is the table that ties them to the letters.
//
// - SUPER: five Super Stars at the letter, at the tap.
// - M: a Mystery Mushroom with Wario's costume at the letter (SMB1 style only; needs the costumes mod).
// - A: the enemies on screen at the tap are defeated: from 18 frames on a 32nd of them (+1) a frame, the rest when
//   A's animation ends (then its stars fall: effects.c).
// - R: a coin from the end of the R's leg (its layout's N_Pos_00).
// - O: a Koopa Clown Car (Junior's in NSMBU). M2: an empty Buzzy Shell, 1 time in 10 a Spiny Shell. E: a Trampoline.
// - A2: when its rocket is back (the 380-frame animation's end), everything on screen but the players vanishes (SMM1
//   used its Reset Rocket's sweep; SMM2 has none: spawn.c's).
// - R2: from its leg's end, an empty Goomba's Shoe (SMB1, SMB3) or a Yoshi's Egg (SMW, NSMBU).
// - I, K: nothing on the course (the parakeet, the sepia: effects.c).
//
// One actor per tap, as SMM1: the course acts on the frame a letter is decided (SUPER's five stars are five actors of
// one call). SMM1's title takes no other letter and no background touch while a letter's Decide plays
// (title_letters.c asks tl_anim_deciding), which also keeps A's and A2's sweeps apart from new spawns. 3D World title
// courses lack the Clown Car, the Buzzy Shell, the Shoe and the Egg (SMM1 had no such style): those letters only play
// their animation and sounds there. SMM2's 3D World has the Spiny Shell, so M2's 1 in 10 still gives one.
#include <stdlib.h>
#include <time.h>

#include "../mod.h"
#include "title_letters.h"

void costume_pickup_gives(const char *res);  // costume.c: every Mystery Mushroom pickup gives res ("" a draw)

#define A_FROM 18  // title frames after A's tap before its first defeats (SMM1: its counter passes 17)
#define A_SPAN 32  // A defeats a 32nd of the enemies (+1) each frame

static struct {
    bool a_on, a2_on;  // A's defeats / A2's clear wait for their animation's end
    int a_frames, a_per;
} L;

static uint32_t rnd(uint32_t n) {  // (xorshift; 0 .. n-1)
    static uint64_t r;
    if (!r) r = (uint64_t)time(NULL) * 0xd1342543de82ef95ull | 1;
    r ^= r << 13, r ^= r >> 7, r ^= r << 17;
    return (uint32_t)((r >> 11) % n);
}

// the letter's point: its centre (SMM1: the letter part's position), R's and R2's the end of their leg
static void point(int letter, const TlBox *b, float *x, float *y) {
    *x = b->x + b->w / 2, *y = b->y + b->h / 2;
    if ((letter == TL_R1 || letter == TL_R2) && !tl_anim_point(letter, b, x, y))
        *x = b->x + b->w * 0.8f, *y = b->y - b->h * 0.4f;  // (its layout not loaded yet: about there)
}

void tl_letter_decide(int letter, bool on, const TlBox *box, int tick) {
    tl_effect_decide(letter, on, box, tick);
    float x, y;
    point(letter, box, &x, &y);
    int style = tl_game_style();
    switch (letter) {
    case TL_SUPER: {
        static const float vx[5] = {-2, -1, 0, 1, 2};  // (SMM1 numbers its five stars; here each its own way)
        int made = 0;
        for (int i = 0; i < 5; i++) made += tl_spawn_free(TA_STAR, x, y, vx[i], 1.5f);
        LOG("[title] SUPER: %d of 5 Super Stars\n", made);
        break;
    }
    case TL_M1:
        if (style != 0) {
            LOG("[title] M: no Mystery Mushroom outside the SMB1 style (SMM1 too)\n");
            break;
        }
        if (tl_spawn_free(TA_MYSTERY_MUSHROOM, x, y, 0, 0)) costume_pickup_gives("Wario");
        break;
    case TL_A1:
        L.a_on = true, L.a_frames = 0;
        L.a_per = tl_defeat_begin() / A_SPAN + 1;
        break;
    case TL_R1: tl_spawn_free(TA_COIN, x, y, (float)rnd(200) / 100.f - 1, 2.5f); break;  // (a pop, as from a blaster)
    case TL_O: tl_spawn_free(TA_CLOWN_CAR, x, y, 0, 0); break;
    case TL_M2: tl_spawn_free(rnd(10) == 1 ? TA_SPINY_SHELL : TA_BUZZY_SHELL, x, y, 0, 0); break;
    case TL_A2:
        L.a2_on = true;
        LOG("[title] A2: the screen clears when the rocket is back\n");
        break;
    case TL_E: tl_spawn_free(TA_TRAMPOLINE, x, y, 0, 0); break;
    case TL_R2:
        if (style == 0 || style == 1) tl_spawn_free(TA_SHOE, x, y, 0, 0);
        else if (style == 2 || style == 3) tl_spawn_free(TA_YOSHI_EGG, x, y, 0, 0);
        else LOG("[title] R2: no Shoe or Egg in this style\n");
        break;
    }
}

void tl_letters_frame(int tick) {
    if (L.a_on) {
        bool over = !tl_anim_busy(TL_A1, tick);
        int n = ++L.a_frames >= A_FROM ? tl_defeat_some(L.a_per) : 0;
        if (n) LOG("[title] A: %d defeated (frame %d after the tap)\n", n, L.a_frames);
        if (over) {
            int rest = tl_defeat_some(1 << 20);
            LOG("[title] A: its animation is over, %d frames on (%d defeated then)\n", L.a_frames, rest);
            L.a_on = false;
        }
    }
    if (L.a2_on && !tl_anim_busy(TL_A2, tick)) {
        LOG("[title] A2: the rocket is back: the screen clears\n");
        tl_clear_screen();
        L.a2_on = false;
    }
}

void tl_letters_reset(void) {
    L.a_on = L.a2_on = false;
    costume_pickup_gives(NULL);
}
