// Scripted pad for input replays (runtime/hle/padscript.c).
//
// RECOMP_PAD_SCRIPT=<file>: one line per run of frames, "N BUTTON BUTTON ..." ('#' starts a comment, "-" = nothing
// pressed); LX=n LY=n RX=n RY=n on a line hold the sticks there (-32768..32767; nn::hid's axes, y up), at rest when
// not given. The stick direction bits (16-23) are derived from them as for a controller. Buttons: A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT RIGHT LSTICK RSTICK, and the trace scripts' names
// JUMP (= A), RUN (= Y), SPIN (= ZR). The same files the Ryujinx trace tooling plays (its .pad and .txt scripts).
//
// One script frame per tick of the script's clock, which is the game's own count, never wall time:
//   RECOMP_PAD_CLOCK=game   a game hook ticks it (SMM2: each update of the player, games/smm2/trace.c); the default
//                           when the game has such a hook
//   RECOMP_PAD_CLOCK=frame  each frame the game presents
// RECOMP_PAD_START=<n> starts the script at tick n (default 0, the first). Nothing is pressed before the start or after
// the end; RECOMP_PAD_EXIT=1 quits when the script is done. While a script is loaded the keyboard, mouse and controllers are left out (the sticks stay at rest; the
// d-pad is the d-pad).
#pragma once
#include <stdbool.h>
#include <stdint.h>

bool padscript_active(void);          // a script is loaded
uint64_t padscript_buttons(void);     // nn::hid button bits the game reads now
void padscript_sticks(int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);  // and its sticks (LX=n LY=n RX=n RY=n)
void padscript_tick(void);            // the clock: the next script frame takes effect
bool padscript_game_clock(void);      // the clock is a game hook (else presented frames)
int64_t padscript_pos(void);          // script frames applied so far (from the start tick)
int64_t padscript_len(void);          // script frames in all
bool padscript_done(void);            // every frame applied
int padscript_parse_button(const char *name, uint64_t *bit);  // 0 if not a button name
// The presented-frame clock (RECOMP_PAD_CLOCK=frame): padscript_frame() each present; RECOMP_PAD_ORIGIN=player starts it
// at the first player update (padscript_origin(), from the game's hook). padscript_frames(): presents since then (-1:
// not yet). RECOMP_FRAME_SHOT="k1,k2,..." saves the presented frame k after the origin to
// <RECOMP_TRACE_SHOT_DIR>/recomp_frame_f<k>.png (menus and other screens without a player, in lockstep with Ryujinx).
void padscript_origin(void);
void padscript_frame(void);
int64_t padscript_frames(void);
