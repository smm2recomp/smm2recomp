// Unit tests for the iPhone / iPad close shortcut (runtime/hle/close_key.h): Control-W or Command-W closes the app
// as it does other iPadOS apps; the game takes every other key, W alone included (the left stick).
// usage: make test-close-key
#include <stdbool.h>
#include <stdio.h>

#include "../runtime/hle/close_key.h"

static int failures;
#define CHECK(name, cond) \
    do { if (!(cond)) { printf("FAIL %s: %s (line %d)\n", name, #cond, __LINE__); failures++; } } while (0)

int main(void) {
    //                       w      ctrl   cmd    alt    shift  repeat
    CHECK("control-w", close_shortcut(true, true, false, false, false, false));
    CHECK("command-w", close_shortcut(true, false, true, false, false, false));
    CHECK("control-command-w", close_shortcut(true, true, true, false, false, false));
    CHECK("w alone is the game's", !close_shortcut(true, false, false, false, false, false));
    CHECK("control-q isn't", !close_shortcut(false, true, false, false, false, false));
    CHECK("command-shift-w isn't", !close_shortcut(true, false, true, false, true, false));
    CHECK("control-option-w isn't", !close_shortcut(true, true, false, true, false, false));
    CHECK("a held key repeating doesn't", !close_shortcut(true, true, false, false, false, true));
    if (failures) return 1;
    printf("PASS test_close_key\n");
    return 0;
}
