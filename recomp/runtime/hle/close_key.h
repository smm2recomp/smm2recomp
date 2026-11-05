// The iPhone / iPad close shortcut: Control-W or Command-W (W pressed with either, no Shift or Option, not a key
// repeat). The game takes every key it gets, so iPadOS never sees its own Command-W; runtime/hle/input.c checks this
// first and closes the app (ios_close_app, runtime/ui/touchpad.mm).
#pragma once
#include <stdbool.h>

static inline bool close_shortcut(bool key_w, bool ctrl, bool cmd, bool alt, bool shift, bool repeat) {
    return key_w && (ctrl || cmd) && !alt && !shift && !repeat;
}
