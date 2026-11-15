// Controls > Controller order: which player each controller becomes, as on the Switch's "Change Grip/Order" screen.
// The page keeps a draft, one box per player (box 0 = player 1), and nothing changes until it is confirmed. A
// controller that holds L + R joins in the lowest empty box; one that has joined, lets go of L and R and then holds
// them again for PAD_ORDER_LEAVE_MS leaves its box, which stays empty for the next one to join (nobody moves up).
// Confirming puts the controllers that joined in their boxes' players, and the ones that didn't after them in the
// order they had. Header-only: runtime/hle/input.c (the page's draft, order_apply) and tools/test_pad_order.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define PAD_ORDER_SLOTS 4
#define PAD_ORDER_LEAVE_MS 2000

// The draft. Controllers go by an id that is never 0 (their SDL joystick id).
typedef struct {
    uint32_t box[PAD_ORDER_SLOTS];   // the controller in each box, 0 = empty
    uint64_t hold[PAD_ORDER_SLOTS];  // when the box's controller began holding L + R to leave, 0 = it isn't
    uint32_t wait[PAD_ORDER_SLOTS];  // controllers that joined or left by an L + R they haven't let go of yet
} PadOrder;

static inline int pad_order_box(const PadOrder *o, uint32_t id) {
    for (int i = 0; id && i < PAD_ORDER_SLOTS; i++)
        if (o->box[i] == id) return i;
    return -1;
}

// The controller with the say: the one in the lowest box. Its A confirms and its B cancels; -1 = nobody has joined.
static inline int pad_order_lead(const PadOrder *o) {
    for (int i = 0; i < PAD_ORDER_SLOTS; i++)
        if (o->box[i]) return i;
    return -1;
}

// first: the first box a controller may have (1 with "Controllers start at player 2", where player 1 is the
// keyboard; else 0). The draft can be confirmed once there is a player 1.
static inline bool pad_order_ready(const PadOrder *o, int first) { return first > 0 || o->box[0] != 0; }

// Controller `id` pressed or released L or R; held: how many of the two it holds now. Both held joins it, or begins
// its hold to leave; neither held lets its next L + R count again.
static inline void pad_order_lr(PadOrder *o, uint32_t id, int held, uint64_t now, int first) {
    int b = pad_order_box(o, id), w = -1;
    if (!id) return;
    for (int i = 0; i < PAD_ORDER_SLOTS; i++)
        if (o->wait[i] == id) w = i;
    if (held == 0 && w >= 0) o->wait[w] = 0, w = -1;
    if (held < 2) {
        if (b >= 0) o->hold[b] = 0;
        return;
    }
    if (w >= 0) return;
    if (b >= 0) {
        if (!o->hold[b]) o->hold[b] = now ? now : 1;
        return;
    }
    for (int i = first < 0 ? 0 : first; i < PAD_ORDER_SLOTS; i++) {
        if (o->box[i]) continue;
        o->box[i] = id, o->hold[i] = 0;
        for (int k = 0; k < PAD_ORDER_SLOTS; k++)
            if (!o->wait[k]) { o->wait[k] = id; break; }
        return;
    }
}

// Controller `id` out of its box: unplugged (gone, forgotten altogether), or its hold to leave is over (it has to let
// go of L and R before it can join again).
static inline void pad_order_out(PadOrder *o, uint32_t id, bool gone) {
    int b = pad_order_box(o, id), w = -1;
    for (int i = 0; i < PAD_ORDER_SLOTS; i++) {
        if (o->wait[i] == id) w = i;
        else if (!o->wait[i] && w < 0) w = i;
    }
    if (b >= 0) o->box[b] = 0, o->hold[b] = 0;
    if (!id || w < 0) return;
    o->wait[w] = gone ? 0 : id;
}

// Time passes: a controller that has held L + R for PAD_ORDER_LEAVE_MS leaves its box. True when one did.
static inline bool pad_order_tick(PadOrder *o, uint64_t now) {
    bool left = false;
    for (int i = 0; i < PAD_ORDER_SLOTS; i++)
        if (o->hold[i] && now - o->hold[i] >= PAD_ORDER_LEAVE_MS) pad_order_out(o, o->box[i], false), left = true;
    return left;
}

// Confirmed. id[i]: the controller that is player i + 1 now (0 = none). to[i] becomes the player slot it gets (-1:
// none there): its box when it joined, else the lowest slot from `first` that is no box's, in the order the
// controllers that didn't join have now. A controller before `first` that didn't join (there is none with
// "Controllers start at player 2") stays. Every controller keeps a slot: none is dropped.
static inline void pad_order_apply(const PadOrder *o, const uint32_t id[PAD_ORDER_SLOTS], int first,
                                   int to[PAD_ORDER_SLOTS]) {
    bool taken[PAD_ORDER_SLOTS] = {false};
    if (first < 0) first = 0;
    for (int i = 0; i < PAD_ORDER_SLOTS; i++) {
        to[i] = id[i] ? pad_order_box(o, id[i]) : -1;
        if (id[i] && to[i] < 0 && i < first) to[i] = i;
        if (to[i] >= 0) taken[to[i]] = true;
    }
    int next = first;
    for (int i = first; i < PAD_ORDER_SLOTS; i++) {
        if (!id[i] || to[i] >= 0) continue;
        while (next < PAD_ORDER_SLOTS && taken[next]) next++;
        if (next >= PAD_ORDER_SLOTS) { to[i] = i; continue; }  // (can't happen: no more controllers than slots)
        taken[next] = true;
        to[i] = next++;
    }
}
