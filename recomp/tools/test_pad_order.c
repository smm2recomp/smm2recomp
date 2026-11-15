// Unit tests for Controls > Controller order (runtime/hle/pad_order.h): a controller joins the lowest empty box with
// L + R and leaves it by letting go and holding them again for two seconds, nobody moves up into an empty box, the
// lowest box's controller has the say and only once there is a player 1, and confirming puts the ones that joined in
// their boxes and the others after them in the order they had.
// usage: make test-pad-order
#include <stdio.h>
#include <string.h>

#include "../runtime/hle/pad_order.h"

static int failures;
#define CHECK(name, cond) \
    do { if (!(cond)) { printf("FAIL %s: %s (line %d)\n", name, #cond, __LINE__); failures++; return; } } while (0)

// Controllers are letters ('a' = id 1...); the draft's boxes as a string, '.' = empty: "b.a."
static PadOrder o;
static const char *boxes(void) {
    static char s[PAD_ORDER_SLOTS + 1];
    for (int i = 0; i < PAD_ORDER_SLOTS; i++) s[i] = o.box[i] ? (char)('a' + o.box[i] - 1) : '.';
    return s;
}
static uint32_t id_of(char c) { return c == '.' ? 0 : (uint32_t)(c - 'a' + 1); }
static void fresh(void) { memset(&o, 0, sizeof o); }
// `who` holds L + R at time `now` / lets go of both / leaves by a hold that is over
static void press(char who, uint64_t now, int first) { pad_order_lr(&o, id_of(who), 2, now, first); }
static void release(char who) { pad_order_lr(&o, id_of(who), 0, 0, 0); }
static void leave(char who) {
    release(who);
    press(who, 1, 0);
    pad_order_tick(&o, 1 + PAD_ORDER_LEAVE_MS);
}

static void test_join(void) {
    fresh();
    pad_order_lr(&o, id_of('c'), 1, 0, 0);
    CHECK("L alone joins nobody", !strcmp(boxes(), "...."));
    press('c', 10, 0);
    CHECK("the first to hold L + R is player 1", !strcmp(boxes(), "c..."));
    press('a', 20, 0);
    press('b', 30, 0);
    CHECK("the next ones follow", !strcmp(boxes(), "cab."));
    press('a', 40, 0);
    CHECK("one that has joined stays", !strcmp(boxes(), "cab."));
    press('d', 50, 0);
    press('e', 60, 0);
    CHECK("no box for a fifth", !strcmp(boxes(), "cabd"));
    pad_order_lr(&o, 0, 2, 70, 0);
    CHECK("no controller", !strcmp(boxes(), "cabd"));
}

static void test_leave(void) {
    fresh();
    press('a', 0, 0);
    press('b', 0, 0);
    press('c', 0, 0);
    CHECK("kept holding since it joined: stays", !pad_order_tick(&o, 5000) && !strcmp(boxes(), "abc."));
    release('b');
    press('b', 6000, 0);
    CHECK("holding to leave", o.hold[1] == 6000 && !pad_order_tick(&o, 7999) && !strcmp(boxes(), "abc."));
    pad_order_lr(&o, id_of('b'), 1, 0, 0);
    CHECK("let go of one before the two seconds: stays", !o.hold[1] && !pad_order_tick(&o, 9000));
    press('b', 9000, 0);
    CHECK("the two seconds start again", !pad_order_tick(&o, 10999) && !strcmp(boxes(), "abc."));
    CHECK("two seconds: out, nobody moves up", pad_order_tick(&o, 11000) && !strcmp(boxes(), "a.c."));
    press('b', 11001, 0);
    CHECK("still holding: not back in", !strcmp(boxes(), "a.c."));
    pad_order_lr(&o, id_of('b'), 1, 0, 0);
    press('b', 11002, 0);
    CHECK("let go of one only: not back in", !strcmp(boxes(), "a.c."));
    press('d', 11003, 0);
    CHECK("the next to join takes the lowest empty box", !strcmp(boxes(), "adc."));
    release('b');
    press('b', 11004, 0);
    CHECK("let go of both, then L + R: back in", !strcmp(boxes(), "adcb"));
    fresh();
    press('a', 0, 0);
    release('a');
    press('a', 0, 0);
    CHECK("a hold that begins at time 0", pad_order_tick(&o, PAD_ORDER_LEAVE_MS + 1) && !strcmp(boxes(), "...."));
}

static void test_unplugged(void) {
    fresh();
    press('a', 0, 0);
    press('b', 0, 0);
    pad_order_out(&o, id_of('a'), true);
    CHECK("its box empties, the other stays", !strcmp(boxes(), ".b.."));
    pad_order_out(&o, id_of('z'), true);
    CHECK("one that never joined", !strcmp(boxes(), ".b.."));
    press('c', 0, 0);
    CHECK("a new one joins there", !strcmp(boxes(), "cb.."));
    release('b');
    press('b', 100, 0);
    pad_order_out(&o, id_of('b'), true);
    CHECK("unplugged while holding to leave", !o.hold[1] && !pad_order_tick(&o, 5000) && !strcmp(boxes(), "c..."));
}

static void test_say(void) {
    fresh();
    CHECK("nobody: no lead, not ready", pad_order_lead(&o) == -1 && !pad_order_ready(&o, 0));
    press('b', 0, 0);
    press('a', 0, 0);
    CHECK("player 1 has the say", pad_order_lead(&o) == 0 && o.box[0] == id_of('b') && pad_order_ready(&o, 0));
    leave('b');
    CHECK("player 1 left: the next one cancels, nobody confirms", pad_order_lead(&o) == 1 && !pad_order_ready(&o, 0));
    fresh();
    CHECK("from player 2: the keyboard is player 1", pad_order_lead(&o) == -1 && pad_order_ready(&o, 1));
    press('a', 0, 1);
    CHECK("from player 2: the first to join", !strcmp(boxes(), ".a..") && pad_order_lead(&o) == 1);
    CHECK("from player 2: still ready", pad_order_ready(&o, 1));
}

// Confirm with the controllers `now` in the players' slots ("ab.c"): the slots afterwards
static const char *apply(const char *now, int first) {
    static char s[PAD_ORDER_SLOTS + 1];
    uint32_t id[PAD_ORDER_SLOTS];
    int to[PAD_ORDER_SLOTS];
    for (int i = 0; i < PAD_ORDER_SLOTS; i++) id[i] = id_of(now[i]);
    pad_order_apply(&o, id, first, to);
    strcpy(s, "....");
    for (int i = 0; i < PAD_ORDER_SLOTS; i++)
        if (to[i] >= 0) s[to[i]] = s[to[i]] == '.' ? now[i] : '!';  // ('!': two in one slot)
    for (int i = 0; i < PAD_ORDER_SLOTS; i++)
        if ((to[i] >= 0) != (now[i] != '.')) return "lost";
    return s;
}

static void test_apply(void) {
    fresh();
    CHECK("nobody joined: as it was", !strcmp(apply("abc.", 0), "abc."));
    CHECK("nobody joined: the gaps close", !strcmp(apply("a.c.", 0), "ac.."));
    press('c', 0, 0);
    CHECK("the one that joined first, the others in their order", !strcmp(apply("abc.", 0), "cab."));
    press('a', 0, 0);
    CHECK("two joined", !strcmp(apply("abcd", 0), "cabd"));
    fresh();
    press('d', 0, 0), press('c', 0, 0), press('b', 0, 0), press('a', 0, 0);
    CHECK("all four, the other way round", !strcmp(apply("abcd", 0), "dcba"));
    fresh();
    press('a', 0, 0), press('b', 0, 0), press('c', 0, 0);
    leave('b');
    CHECK("an empty box between two: the ones that didn't join fill it", !strcmp(apply("abcd", 0), "abcd"));
    CHECK("in their order", !strcmp(apply("dbac", 0), "adcb"));
    CHECK("nobody for it: it stays empty", !strcmp(apply("a.c.", 0), "a.c."));
}

static void test_from_player2(void) {  // ("Controllers start at player 2")
    fresh();
    CHECK("nobody joined: none becomes player 1", !strcmp(apply(".ab.", 1), ".ab."));
    CHECK("the gaps close from player 2", !strcmp(apply("..ab", 1), ".ab."));
    press('b', 0, 1);
    CHECK("the first to join is player 2", !strcmp(boxes(), ".b.."));
    CHECK("the others after it", !strcmp(apply(".abc", 1), ".bac"));
    press('c', 0, 1), press('a', 0, 1), press('d', 0, 1);
    CHECK("three boxes: no fourth controller", !strcmp(boxes(), ".bca"));
    CHECK("three joined", !strcmp(apply(".abc", 1), ".bca"));
    fresh();
    press('b', 0, 1);
    CHECK("a controller left as player 1 stays, unless it joined", !strcmp(apply("ab..", 1), "ab.."));
    press('a', 0, 1);
    CHECK("it joined", !strcmp(apply("ab..", 1), ".ba."));
}

int main(void) {
    test_join();
    test_leave();
    test_unplugged();
    test_say();
    test_apply();
    test_from_player2();
    if (failures) { printf("%d FAILED\n", failures); return 1; }
    printf("controller order: all passed\n");
    return 0;
}
