// SMM1 title letters (a mod, off by default): on SMM2's title screen each letter of the logo does what that letter did
// on SMM1's title screen when tapped or clicked. Letters map by letter to SMM2's logo panes (P_Super_00, P_LogoM_00
// ...); SMM2's "2" is no letter. Hooks and state: title_letters.c; hit-testing: hit.c (pure, unit-tested by
// tools/test_title_letters.c); the SMM1 animations: anim.c; the sounds, screen effects, K's sepia and 8-bit music:
// effects.c; actors on the title course: spawn.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "../smm1/lyt.h"

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
// the overlay (smm1/flow.c chains it after the SMW goal wipe's): RGBA8 1280 x 720 or NULL, as smw_goal_image
const uint8_t *title_letters_image(int *w, int *h, int *full, uint64_t *seq);

// ---- anim.c (ticks: 60 Hz, one clock for both threads)
bool tl_anim_start(int letter, int tick);      // false: ignored (the letter plays, or its layout is missing)
bool tl_anim_ready(int letter, int tick);      // a tap now would start it
bool tl_anim_on(int letter, int tick);         // a toggle (I, K) turned on (its DecideA playing or held)
bool tl_anim_busy(int letter, int tick);       // SMM1's letter is drawn: SMM2's pane stays hidden
void tl_anim_box(int letter, const TlBox *b);  // where SMM2's pane is (each frame: the logo may move)
void tl_anim_reset(void);
bool tl_anim_draw(int tick, LQuads *out);      // (the renderers' thread) false: nothing to draw

// ---- effects.c: sounds, screen effects, K's sepia and 8-bit music, the parakeet's stay (game thread unless said)
void tl_effects_load(void);                   // (the SMM1 dump's thread, once it is open) the sounds, the music
void tl_effect_touch(int letter);             // a finger came down on a letter (SMM1's TouchSelect sound)
void tl_effect_untouch(int letter);           // ... and slid off it (TouchCancel)
void tl_effect_decide(int letter, bool on, const TlBox *box, int tick);  // its animation started (on: a toggle's)
void tl_effect_drag(float x, float y, int tick, bool first);  // a finger held on the course behind the title
void tl_effect_release(float x, float y, int tick);
void tl_effects_frame(int tick);              // each title frame
void tl_effects_reset(void);                  // left the title: K's picture and music back, sprites gone
void tl_effects_lost(void);                   // (the renderers' thread) the title is gone: the same, if anything is on
bool tl_effects_draw(int tick, LQuads *out, bool front);  // (the renderers') A's stars (behind the letters) / the dust

// ---- spawn.c (the game thread, inside the title's hooks: between tl_spawn_frame and tl_spawn_done; screen points
// in game pixels, velocities in course px per frame, y up). Each kind is the style's own actor: the Galoomba in SMW,
// Junior's Clown Car in NSMBU, the Shoe in SMB1 / SMB3 and Yoshi's Egg in SMW / NSMBU; 3D World has no Clown Car,
// Buzzy Shell, Shoe or Egg; the Mystery Mushroom is the costumes mod's item (SMB1, that mod on).
struct Cpu;
enum TlActor { TA_GOOMBA, TA_KOOPA, TA_BOWSER, TA_COIN, TA_TRAMPOLINE, TA_CLOWN_CAR, TA_BUZZY_SHELL, TA_SPINY_SHELL,
               TA_SHOE, TA_YOSHI_EGG, TA_MYSTERY_MUSHROOM, TA_STAR, TA_COUNT };
bool tl_spawn(int actor, float screen_x, float screen_y, float vx, float vy);  // false: the style lacks it, or capped
void tl_defeat_enemies(void);                 // every enemy on screen, as a Super Star's touch
void tl_clear_screen(void);                   // every enemy, item and coin on screen vanishes in a puff
const char *tl_actor_name(int actor);         // "Goomba" ... (logs)
int tl_game_style(void);                      // the title course's style: 0 SMB1 .. 4 3D World, -1 none
int tl_course_theme(bool *night);             // the player's area's theme (0 ground .. 4 underwater .. 9 forest), -1
void tl_background_tap(float sx, float sy);   // a tap off the letters: a Goomba, a Koopa or rarely Bowser
void tl_spawn_frame(struct Cpu *c);           // each title frame, before the taps: the guest CPU, live counts
void tl_spawn_done(void);
void tl_spawn_reset(void);                    // left the title (the actors go with the course)
