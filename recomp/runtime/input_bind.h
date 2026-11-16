// Control remapping (runtime/hle/bindings.c): which controller button / key drives each Switch input. Edited from
// the in-game menu's Controls tab (runtime/ui/controls_ui.mm), saved in larecomp.ini. Each player's controller has its
// own bindings (#100): players 2-4 use player 1's until one is changed for them. The keyboard is player 1's alone.
// Menus (the overlay, the start screen) never use these: they keep the fixed default layout, so no binding can lock
// the player out.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef union SDL_Event SDL_Event;

// Switch inputs, in nn::hid button-bit order (runtime/hle/input.c BTN_*), then the keyboard's stick directions.
enum {
    BIND_A, BIND_B, BIND_X, BIND_Y, BIND_LSTICK, BIND_RSTICK, BIND_L, BIND_R, BIND_ZL, BIND_ZR, BIND_PLUS, BIND_MINUS,
    BIND_LEFT, BIND_UP, BIND_RIGHT, BIND_DOWN,
    BIND_PAD_COUNT,                                      // controller: the 16 buttons (its sticks stay the sticks)
    BIND_LS_UP = BIND_PAD_COUNT, BIND_LS_DOWN, BIND_LS_LEFT, BIND_LS_RIGHT,
    BIND_RS_UP, BIND_RS_DOWN, BIND_RS_LEFT, BIND_RS_RIGHT,
    BIND_KEY_COUNT                                       // keyboard: buttons + both sticks
};
enum { BIND_DEV_PAD, BIND_DEV_KEY };
// Controller binding values: an SDL_GamepadButton, or the analog triggers as buttons
enum { PADB_LTRIGGER = 26 /* SDL_GAMEPAD_BUTTON_COUNT */, PADB_RTRIGGER, PADB_HOST_COUNT };
#define BIND_PLAYERS 4                      // `player` below: 1-4 (ignored for BIND_DEV_KEY)

// PADB value per Switch button, for that player's controller: its own table once it has one (bind_pad_custom), else
// player 1's, looked up anew each time so a change of player 1's reaches the players that follow it at once
const int16_t *bind_pad_table(int player);
bool bind_pad_custom(int player);           // player 1: always
void bind_pad_follow(int player);           // players 2-4: back to player 1's bindings
extern int16_t g_bind_key[BIND_KEY_COUNT];  // SDL_Scancode per input (0 = none)

const char *bind_input_name(int slot);         // "A", "D-pad Up", "Left stick Up"
const char *bind_pad_label(int host, int fam); // controller button in the pad family's own words (fam: input_pad_family)
const char *bind_key_label(int scancode);
int bind_label_family(void);                   // the connected controller's family (also on the start screen)
void bind_reset(int dev, int player);          // (players 2-4: the defaults, as that player's own table)
bool bind_is_default(int dev, int player);
bool bind_is_default_slot(int dev, int player, int slot);

// Which controller is whose (runtime/hle/input.c), from any thread. Player 1's is the first one, or none with
// "Controllers start at player 2"; players 2-4 have the further ones.
uint32_t input_player_pad(int player);         // its SDL_JoystickID, 0 = none connected
const char *input_player_pad_name(int player);  // NULL = none connected
int input_player_family(int player);           // as input_pad_family; none connected: bind_label_family
int input_pad_player(uint32_t id);             // the player an SDL_JoystickID belongs to, -1 = nobody's

// "Click a binding, then press": begun from the UI; the next press (any thread's SDL event) is taken. Escape cancels,
// as does BIND_CAPTURE_MS without a press. On a conflict the two inputs swap. A controller capture is for one
// player: a press on another player's controller is not taken, and a player still following player 1 gets its own
// table (a copy of player 1's) with the press.
#define BIND_CAPTURE_MS 5000
void bind_capture_begin(int dev, int player, int slot);
void bind_capture_cancel(void);
// capturing: dev / player / slot (else -1), ms left
bool bind_capture_active(int *dev, int *player, int *slot, int *ms_left);
// Main thread, before anything else sees the event: true = consumed by a capture
bool bind_capture_event(const SDL_Event *e);
// Last change for the UI: 0 none, 1 set, 2 swapped (with *other), 3 cancelled, 4 timed out, 5 reset, 6 back to
// player 1's (bind_pad_follow); *serial bumps
int bind_last_result(int *dev, int *player, int *slot, int *other, int *serial);

// larecomp.ini: bind_pad_<input> / bind_key_<input> (only the ones that differ from the defaults are written). A
// player 2-4 with its own controller table: bind_pad_pN = custom, then bind_pad_pN_<input> likewise; one that
// follows player 1 has no lines.
bool bind_settings_load(const char *key, const char *val);
void bind_settings_save(FILE *f);
// The player (1-4) a "bind_pad_..." key is for, *name = its <input> part ("" for bind_pad_pN itself). Only p2 / p3 /
// p4 alone or before '_' is a player: bind_pad_plus is player 1's Plus.
static inline int bind_pad_key_player(const char *key, const char **name) {
    const char *s = key + 9;  // (past "bind_pad_")
    if (s[0] == 'p' && s[1] >= '2' && s[1] <= '0' + BIND_PLAYERS && (s[2] == 0 || s[2] == '_')) {
        *name = s[2] ? s + 3 : s + 2;
        return s[1] - '0';
    }
    *name = s;
    return 1;
}

// bind_* names (larecomp.ini): Switch input "a", "zl", "ls_up"... -> slot; controller button "south"... -> PADB value
int bind_slot_from_name(const char *name);
int bind_pad_from_name(const char *name);

// Button prompts (runtime/nvn/texpatch.c): the controller button now driving Switch input `slot` (BIND_A..) for
// player 1
int bind_prompt_host(int slot);

#ifdef __cplusplus
}
#endif
