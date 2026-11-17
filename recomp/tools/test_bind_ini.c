// Unit tests for the players' controller bindings in larecomp.ini (runtime/input_bind.h bind_pad_key_player, #100):
// which player a bind_pad_* key is for, and the name of the input it binds; and which button confirms in the menus
// (bind_menu_ab_swapped, #84): the same as in the game's menus.
// usage: make test-bind-ini
#include <stdio.h>
#include <string.h>

#include "../runtime/input_bind.h"

static int failures;
#define CHECK(name, cond) \
    do { if (!(cond)) { printf("FAIL %s: %s (line %d)\n", name, #cond, __LINE__); failures++; return; } } while (0)

// the player of `key`, with its input name compared to `input`
static int player_of(const char *key, const char *input) {
    const char *name = NULL;
    int p = bind_pad_key_player(key, &name);
    return name && !strcmp(name, input) ? p : -1;
}

static void test_player1(void) {
    CHECK("an input", player_of("bind_pad_a", "a") == 1);
    CHECK("Plus is not a player", player_of("bind_pad_plus", "plus") == 1);
    CHECK("an input with an underscore", player_of("bind_pad_ls_up", "ls_up") == 1);
    CHECK("no player 1 prefix", player_of("bind_pad_p1_a", "p1_a") == 1);
    CHECK("no player 5", player_of("bind_pad_p5_a", "p5_a") == 1);
    CHECK("p2 needs the underscore", player_of("bind_pad_p2a", "p2a") == 1);
    CHECK("nothing after the prefix", player_of("bind_pad_", "") == 1);
}

static void test_marker(void) {
    CHECK("player 2's own table", player_of("bind_pad_p2", "") == 2);
    CHECK("player 3's own table", player_of("bind_pad_p3", "") == 3);
    CHECK("player 4's own table", player_of("bind_pad_p4", "") == 4);
}

static void test_players(void) {
    CHECK("player 2's input", player_of("bind_pad_p2_a", "a") == 2);
    CHECK("player 3's input", player_of("bind_pad_p3_zl", "zl") == 3);
    CHECK("player 4's input", player_of("bind_pad_p4_rstick", "rstick") == 4);
    CHECK("player 2's Plus", player_of("bind_pad_p2_plus", "plus") == 2);
    CHECK("player 2, no input", player_of("bind_pad_p2_", "") == 2);
}

// The four cases of bindings and "A and B swapped in menus" (true = the bottom face button confirms), and the
// setting doing nothing with "Controllers start at player 2", as in the game's menus
static void test_menu_ab(void) {
    CHECK("default bindings, setting off: right", !bind_menu_ab_swapped(false, false, false));
    CHECK("default bindings, setting on: bottom", bind_menu_ab_swapped(false, true, false));
    CHECK("A and B bound swapped, setting off: bottom", bind_menu_ab_swapped(true, false, false));
    CHECK("A and B bound swapped, setting on: the two swaps cancel out", !bind_menu_ab_swapped(true, true, false));
    CHECK("controllers from player 2: the setting changes nothing", !bind_menu_ab_swapped(false, true, true));
    CHECK("controllers from player 2: the bindings still count", bind_menu_ab_swapped(true, true, true));
}

int main(void) {
    test_player1();
    test_marker();
    test_players();
    test_menu_ab();
    if (failures) { printf("%d FAILED\n", failures); return 1; }
    printf("bindings ini keys: all passed\n");
    return 0;
}
