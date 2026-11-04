// SMM1 title letters (a mod, off by default): on SMM2's title screen each letter of the logo does what that letter did
// on SMM1's title screen when tapped or clicked. Letters map by letter to SMM2's logo panes (P_Super_00, P_LogoM_00
// ...); SMM2's "2" is no letter. Hooks and state: title_letters.c; hit-testing: hit.c (pure, unit-tested by
// tools/test_title_letters.c); the SMM1 animations: anim.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// in SMM1's reading order (SMM1's own ids put I and K last: not used here)
enum TitleLetter { TL_NONE = -1, TL_SUPER, TL_M1, TL_A1, TL_R1, TL_I, TL_O, TL_M2, TL_A2, TL_K, TL_E, TL_R2, TL_COUNT };

typedef struct { float x, y, w, h; bool visible; } TlBox;  // game pixels, 1280 x 720, top-left origin

extern const char *const tl_letter_name[TL_COUNT];  // "SUPER", "M", ... "R2" (logs)

// ---- hit.c
int tl_hit(const TlBox boxes[TL_COUNT], int x, int y);  // the letter at (x, y), TL_NONE; the later box wins overlaps
// The logo's letters stand closer than their panes are wide (R's and I's 77-px panes are 46 px apart): neighbouring
// boxes on one row are cut where they overlap, at its middle, so each tap has one letter.
void tl_trim(TlBox boxes[TL_COUNT]);

// ---- title_letters.c (the game thread's hooks; the box and the pane calls are for the game thread)
extern bool g_title_letters;          // the switch (smm1_extras.c: smm1_title_letters=1)
bool smm1_title_letters(void);        // the switch, read from the ini on first use (smm1_extras.c)
bool title_letters_active(void);      // switch on and the title scene running
bool title_letters_box(int letter, TlBox *out);  // the SMM2 pane's box this frame
void title_letters_hide(int letter, bool hidden);
void title_letters_reset(void);       // on leaving the title scene: panes shown again, animations dropped
