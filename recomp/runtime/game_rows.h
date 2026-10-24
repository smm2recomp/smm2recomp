// A game's mod settings that are not switches, in the overlay menu's Mods tab after the switches
// (runtime/game_hooks.c for the neutral defaults, runtime/ui/menu.mm for the drawing, a game's own code to fill
// them in - games/smm2/mods/ocw.c).
//
// One row is a section title (value and text both NULL), a switch (value), or a text field (text; A or a click
// turns it into a field, with the on-screen keyboard for a controller).
//
// A field marked `secret` is not shown: the row says "set" or "not set", and activating it once shows the field so a
// new value can be typed. It is hidden again when the menu closes, so a token is not sitting on screen whenever the
// tab is opened (it never reaches a log either: runtime/net/netcfg.h net_log_secret). Revealing on demand rather
// than masking in place is deliberate - a mask would mean swapping the buffer under a live text field.
//
// The rows are shown whether or not any mod is on, so a setting that has to be entered *before* its mod can work is
// reachable (GameModToggle's rows are hidden when nothing is on).
//
// This struct is shared rather than repeated per file: GameModToggle is copied into each of its users, which is
// survivable for four fields and not for these nine.
#pragma once
#include <stdbool.h>

typedef struct {
    const char *label, *desc;
    bool *value;        // a switch (text NULL)
    bool def;           // its default
    char *text;         // a text field, the game's own buffer (value NULL)
    unsigned long cap;  // its size
    const char *hint;   // shown when the field is empty ("not set")
    bool secret;        // not shown until the row is activated (and hidden again when the menu closes)
    int max_len;        // longest value accepted (0: the buffer's size decides)
} GameModRow;

// The secret rows are hidden again (runtime/ui/menu.mm calls this when the menu closes).
void game_mod_rows_hide(void);

#ifdef __cplusplus
extern "C" {
#endif
int game_mod_rows(const GameModRow **out);  // how many, and where (runtime/game_hooks.c: none)
void game_mod_rows_changed(void);           // one of them was changed in the menu
#ifdef __cplusplus
}
#endif
