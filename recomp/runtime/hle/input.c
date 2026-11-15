// Controller input: SDL keyboard/gamepad -> nn::hid Npad state.
//
// Both go through the player's bindings (runtime/hle/bindings.c, defaults there: keyboard WASD left stick, arrows
// d-pad, K/J/L/I = A/B/X/Y, Q/E = L/R, 1/3 = ZL/ZR, Enter / Backspace = Plus / Minus, numpad 8/4/5/6 right stick;
// controller by position, Switch A = east).
#include <pthread.h>
#include <SDL3/SDL.h>
#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../nvn/nvn.h"
#include "../nvn/renderer.h"
#include "../settings.h"
#include "../input_bind.h"
#include "../padscript.h"
#include "stick_deadzone.h"
#include "pad_order.h"

enum {
    BTN_A = 0, BTN_B, BTN_X, BTN_Y, BTN_LSTICK, BTN_RSTICK, BTN_L, BTN_R, BTN_ZL, BTN_ZR, BTN_PLUS, BTN_MINUS,
    BTN_LEFT, BTN_UP, BTN_RIGHT, BTN_DOWN
};

_Static_assert(BTN_DOWN == BIND_DOWN && BTN_ZL == BIND_ZL, "bindings follow the button bits");

static _Atomic uint64_t key_buttons, pad_buttons;
static _Atomic int32_t key_lx, key_ly, key_rx, key_ry, pad_lx, pad_ly, pad_rx, pad_ry;
static uint8_t keys_down[SDL_SCANCODE_COUNT];
static SDL_Gamepad *pad;
static _Atomic int64_t sampling;

// Players 2-4 (local multiplayer: Play Together): each further controller is the next player, Npad No. 2..4 (ids
// 1..3), through that player's controller bindings (player 1's until it has its own: runtime/hle/bindings.c
// bind_pad_table). Player 1 keeps the keyboard, the on-screen controller and the first controller. LARECOMP_INPUT2
// (same form as LARECOMP_INPUT) is a scripted player 2 for tests.
#define EXTRA_PADS 3
static SDL_Gamepad *extra_pad[EXTRA_PADS];
static _Atomic uint64_t extra_buttons[EXTRA_PADS];
static _Atomic int32_t extra_lx[EXTRA_PADS], extra_ly[EXTRA_PADS], extra_rx[EXTRA_PADS], extra_ry[EXTRA_PADS];
static _Atomic uint64_t script2_buttons;
static _Atomic int32_t script2_lx, script2_ly;
static bool script2_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_INPUT2") != NULL;
    return on;
}
static bool extra_connected(int i) { return extra_pad[i] || (i == 0 && script2_on()); }
static void style_changed(void);
static void note_players(void);
static bool real_gamepad(SDL_JoystickID id);

// Host keys / buttons held by a test script (LARECOMP_HOST_INPUT, input_frame): they go through the bindings
static uint8_t test_keys[SDL_SCANCODE_COUNT];
static _Atomic uint32_t test_pad;
static _Atomic int32_t test_lx, test_ly;  // (a resting stick's drift: "lstick" in LARECOMP_HOST_INPUT)
static bool host_test;  // (then a real controller is left out: the player's own pad must not spoil the test)

static int held_key(int sc) { return sc > 0 && sc < SDL_SCANCODE_COUNT && (keys_down[sc] || test_keys[sc]); }

// Through the bindings (runtime/hle/bindings.c: g_bind_key), read on every key event, so a rebind applies at once.
static void recompute_keyboard(void) {
    uint64_t b = 0;
    for (int i = 0; i < BIND_PAD_COUNT; i++)
        if (held_key(g_bind_key[i])) b |= 1ull << i;
    key_buttons = b;
    key_lx = (held_key(g_bind_key[BIND_LS_RIGHT]) - held_key(g_bind_key[BIND_LS_LEFT])) * 32767;
    key_ly = (held_key(g_bind_key[BIND_LS_UP]) - held_key(g_bind_key[BIND_LS_DOWN])) * 32767;
    key_rx = (held_key(g_bind_key[BIND_RS_RIGHT]) - held_key(g_bind_key[BIND_RS_LEFT])) * 32767;
    key_ry = (held_key(g_bind_key[BIND_RS_UP]) - held_key(g_bind_key[BIND_RS_DOWN])) * 32767;
}

// Controller buttons (and the triggers as buttons) -> Switch buttons through the player's table (bind_pad_table); the
// sticks stay the sticks.
static void recompute_pad(void) {
    uint32_t host = test_pad;
    if (pad && !host_test) {
        for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
            if (SDL_GetGamepadButton(pad, (SDL_GamepadButton)i)) host |= 1u << i;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) host |= 1u << PADB_LTRIGGER;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) host |= 1u << PADB_RTRIGGER;
    }
    const int16_t *t = bind_pad_table(1);
    uint64_t b = 0;
    for (int i = 0; i < BIND_PAD_COUNT; i++)
        if (t[i] >= 0 && t[i] < PADB_HOST_COUNT && (host >> t[i] & 1)) b |= 1ull << i;
    pad_buttons = b;
    if (!pad || host_test) { pad_lx = test_lx, pad_ly = test_ly, pad_rx = pad_ry = 0; return; }
    pad_lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
    pad_ly = -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    pad_rx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
    pad_ry = -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
}

static void recompute_extra(int i) {
    SDL_Gamepad *g = extra_pad[i];
    uint32_t host = 0;
    if (g) {
        for (int k = 0; k < SDL_GAMEPAD_BUTTON_COUNT; k++)
            if (SDL_GetGamepadButton(g, (SDL_GamepadButton)k)) host |= 1u << k;
        if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) host |= 1u << PADB_LTRIGGER;
        if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) host |= 1u << PADB_RTRIGGER;
    }
    const int16_t *t = bind_pad_table(i + 2);
    uint64_t b = 0;
    for (int k = 0; k < BIND_PAD_COUNT; k++)
        if (t[k] >= 0 && t[k] < PADB_HOST_COUNT && (host >> t[k] & 1)) b |= 1ull << k;
    extra_buttons[i] = b;
    extra_lx[i] = g ? SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX) : 0;
    extra_ly[i] = g ? -SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY) : 0;
    extra_rx[i] = g ? SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX) : 0;
    extra_ry[i] = g ? -SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY) : 0;
}

// A controller that isn't player 1's: the next free player's (logged; the game hears of it by its style set event)
static void open_extra(SDL_JoystickID id) {
    if ((pad && SDL_GetGamepadID(pad) == id) || !real_gamepad(id)) return;
    for (int i = 0; i < EXTRA_PADS; i++)
        if (extra_pad[i] && SDL_GetGamepadID(extra_pad[i]) == id) return;
    for (int i = 0; i < EXTRA_PADS; i++)
        if (!extra_pad[i]) {
            extra_pad[i] = SDL_OpenGamepad(id);
            if (!extra_pad[i]) return;
            LOG("[input] player %d: %s\n", i + 2, SDL_GetGamepadName(extra_pad[i]));
            note_players();
            recompute_extra(i);
            style_changed();
            return;
        }
}

// The "Controllers start at player 2" setting: no controller is player 1's (the keyboard and the on-screen controls
// are); every one is the next free player's
static bool pads_from_p2(void) { return g_settings.pads_from_p2; }
static void note_family(void);
static bool scanned;  // pads present at startup: their ADDED event may have gone to the start screen's loop

// The setting changed: every controller let go and taken again under the new rule (the game hears of the players
// coming and going by its style set events)
void input_pads_reassign(void) {
    if (pad) SDL_CloseGamepad(pad), pad = NULL;
    for (int i = 0; i < EXTRA_PADS; i++)
        if (extra_pad[i]) {
            SDL_CloseGamepad(extra_pad[i]);
            extra_pad[i] = NULL;
            recompute_extra(i);
        }
    style_changed();
    note_family();
    recompute_pad();
    scanned = false;  // (taken again on the next event)
}

// Forget held keys (the menu took focus); the gamepad is re-read on its next event.
void input_reset(void) {
    memset(keys_down, 0, sizeof keys_down);
    recompute_keyboard();
}

volatile int g_debug_frame_request;  // F9: dump the next frame's passes (build/pass_*.png) + draw log

// Events a test pushes (LARECOMP_HOST_INPUT type: / sdlkey: / sdlpad:) carry this device id: they go the whole way, menu first.
#define TEST_DEVICE 0x7E57u
static bool test_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: return e->key.which == TEST_DEVICE;
    case SDL_EVENT_TEXT_INPUT: return e->text.windowID == TEST_DEVICE;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: return e->gbutton.which == TEST_DEVICE;
    // ("finger" tests: the mouse SDL makes up from a touch, marked with the test's window id)
    case SDL_EVENT_MOUSE_MOTION: return e->motion.which == SDL_TOUCH_MOUSEID && e->motion.windowID == TEST_DEVICE;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        return e->button.which == SDL_TOUCH_MOUSEID && e->button.windowID == TEST_DEVICE;
    default: return false;
    }
}

// Scripted test runs ignore the user's own input (a key or stick touched while a test drives Link spoils it):
// LARECOMP_NO_USER_INPUT=1, on by default whenever LARECOMP_INPUT / LARECOMP_DOORJUMP_TEST / LARECOMP_PLACE is set
// (or LARECOMP_HOST_INPUT / LARECOMP_BIND_TEST; LARECOMP_NO_USER_INPUT=0 lets it through again).
bool input_user_blocked(const SDL_Event *e) {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("LARECOMP_NO_USER_INPUT");
        on = v ? atoi(v) != 0 : (getenv("LARECOMP_INPUT") || getenv("LARECOMP_DOORJUMP_TEST") || getenv("LARECOMP_PLACE") ||
                                 getenv("LARECOMP_HOST_INPUT") || getenv("LARECOMP_BIND_TEST") || padscript_active());
        if (on) LOG("[input] scripted run: keyboard, mouse and controllers ignored\n");
    }
    if (!on) return false;
    if (test_event(e)) return false;  // (LARECOMP_HOST_INPUT type: / sdlkey: / sdlpad: - the test's own presses)
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: case SDL_EVENT_TEXT_INPUT: case SDL_EVENT_TEXT_EDITING:
    case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_GAMEPAD_ADDED: case SDL_EVENT_GAMEPAD_AXIS_MOTION: case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: case SDL_EVENT_JOYSTICK_ADDED: case SDL_EVENT_JOYSTICK_AXIS_MOTION:
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN: case SDL_EVENT_JOYSTICK_BUTTON_UP:
        return true;
    default:
        return false;
    }
}

// Controller family for the button prompts (runtime/nvn/texpatch.c): 0 Nintendo / other / none, 1 Xbox, 2 PlayStation.
// LARECOMP_PAD_TYPE=xbox|playstation|nintendo pretends one is attached (tests).
static _Atomic int pad_family;
static _Atomic bool pad_connected;  // a gamepad is open (the on-screen controller hides)
static int family_of(SDL_Gamepad *g) {
    if (g) switch (SDL_GetGamepadType(g)) {
        case SDL_GAMEPAD_TYPE_XBOX360: case SDL_GAMEPAD_TYPE_XBOXONE: return 1;
        case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5: return 2;
        default: break;
    }
    return 0;
}
static void note_family(void) {
    pad_connected = pad != NULL;
    int f = family_of(pad);
    if (f != pad_family) LOG("[input] button prompts: %s controller\n", f == 1 ? "Xbox" : f == 2 ? "PlayStation" : "Nintendo / other");
    pad_family = f;
    note_players();
}
int input_pad_family(void) {
    static int env = -2;
    if (env == -2) {
        const char *e = getenv("LARECOMP_PAD_TYPE");
        env = !e ? -1 : !strcmp(e, "xbox") ? 1 : !strcmp(e, "playstation") || !strcmp(e, "ps") ? 2 : 0;
    }
    return env >= 0 ? env : pad_family;
}

bool input_gamepad_connected(void) { return pad_connected; }

// Which controller is each player's, for the Controls tab and a binding capture (runtime/ui/controls_ui.mm,
// runtime/hle/bindings.c): player 1's is `pad` (none with "Controllers start at player 2"), player N's is
// extra_pad[N - 2]. Those run on other threads than the one that opens and closes the controllers, so they get the
// SDL joystick id and the family noted here each time one comes or goes, never the SDL_Gamepad itself.
_Static_assert(BIND_PLAYERS == EXTRA_PADS + 1, "player 1 and the extra controllers' players");
static _Atomic uint32_t player_pad_id[BIND_PLAYERS];
static _Atomic int player_family[BIND_PLAYERS];
static void note_players(void) {
    for (int p = 0; p < BIND_PLAYERS; p++) {
        SDL_Gamepad *g = p ? extra_pad[p - 1] : pad;
        player_family[p] = family_of(g);
        player_pad_id[p] = g ? SDL_GetGamepadID(g) : 0;
    }
}
uint32_t input_player_pad(int player) { return player >= 1 && player <= BIND_PLAYERS ? player_pad_id[player - 1] : 0; }
const char *input_player_pad_name(int player) {
    uint32_t id = input_player_pad(player);
    return id ? SDL_GetGamepadNameForID(id) : NULL;
}
// (player 1, and a player without a controller: as the editor always had it, LARECOMP_PAD_TYPE included)
int input_player_family(int player) {
    return player > 1 && input_player_pad(player) ? player_family[player - 1] : bind_label_family();
}
int input_pad_player(uint32_t id) {
    for (int p = 0; id && p < BIND_PLAYERS; p++)
        if (player_pad_id[p] == id) return p + 1;
    return -1;
}

// The Controls tab's stick preview (runtime/ui/menu.mm, #98): the left stick (right: the right one) of that player's
// controller as it is now, before the dead zone, y up as in recompute_pad; false when the player has no controller.
// Read from the controller itself: while the menu is up its events don't reach input_handle_event, so pad_lx and the
// rest stand still. The menu is drawn on the render thread and the main thread may close the controller meanwhile: it
// is looked up by its id (input_player_pad) under SDL's lock.
bool input_preview_stick(int player, int right, int32_t *x, int32_t *y) {
    SDL_JoystickID id = input_player_pad(player);
    SDL_LockJoysticks();
    SDL_Gamepad *g = id ? SDL_GetGamepadFromID(id) : NULL;
    if (g) {
        *x = SDL_GetGamepadAxis(g, right ? SDL_GAMEPAD_AXIS_RIGHTX : SDL_GAMEPAD_AXIS_LEFTX);
        *y = -SDL_GetGamepadAxis(g, right ? SDL_GAMEPAD_AXIS_RIGHTY : SDL_GAMEPAD_AXIS_LEFTY);
    }
    SDL_UnlockJoysticks();
    return g != NULL;
}

// Controls > Controller order (runtime/ui/controls_ui.mm), as the Switch's "Change Grip/Order" screen. Its page keeps
// a draft (pad_order.h): a controller that holds L + R joins in the lowest empty box, one that lets go and holds them
// again for two seconds leaves its box. Nothing changes until the draft is confirmed: by the menu's confirm button
// (A) on the controller in the lowest box, or Enter, and only once there is a player 1. Then the controllers that
// joined become their boxes' players and the others follow in the order they had; a controller takes its player's
// bindings, as when it is plugged in (bind_pad_table: they are the player's, not the controller's). The menu's back
// button (B) on that controller, or on any while nobody has joined, or Esc, leaves everything as it was. Nothing of
// the order is kept: the next start takes the controllers as SDL lists them again.
// The page is drawn on the render thread, which only asks (order_request) and reads the draft's copy (order_box,
// order_hold). The draft is kept and the controllers change players here, on the thread that opens and closes them:
// the main thread's input_pad_order_event (runtime/ui/menu.mm menu_handle_event) and input_pad_order_tick
// (menu_apply_window_requests).
_Static_assert(PAD_ORDER_SLOTS == BIND_PLAYERS, "a box per player");
static _Atomic int order_request;  // the page's: PAD_ORDER_OPEN / CANCEL / CONFIRM
static _Atomic bool order_open;
static PadOrder order;  // the draft (main thread)
static _Atomic uint32_t order_box[BIND_PLAYERS];   // its boxes and the holds to leave, for the page
static _Atomic uint64_t order_hold[BIND_PLAYERS];

static SDL_Gamepad **order_slot(int i) { return i ? &extra_pad[i - 1] : &pad; }
static SDL_Gamepad *order_pad(SDL_JoystickID id) {
    for (int i = 0; i < PAD_ORDER_SLOTS; i++)
        if (*order_slot(i) && SDL_GetGamepadID(*order_slot(i)) == id) return *order_slot(i);
    return NULL;
}
static int order_first(void) { return pads_from_p2() ? 1 : 0; }
static void order_publish(void) {
    for (int i = 0; i < BIND_PLAYERS; i++) order_box[i] = order.box[i], order_hold[i] = order.hold[i];
}

// The menu's confirm (else back) button, as menu.mm's key_from_pad has them
static SDL_GamepadButton order_button(bool confirm) {
    extern bool menu_ab_swapped(void);  // runtime/ui/menu.mm
    return confirm != menu_ab_swapped() ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_SOUTH;
}

// The draft confirmed: the controllers to their players (pad_order_apply). None is opened or closed.
static void order_apply(void) {
    SDL_Gamepad *was[PAD_ORDER_SLOTS], *now[PAD_ORDER_SLOTS] = {NULL};
    uint32_t id[PAD_ORDER_SLOTS];
    int to[PAD_ORDER_SLOTS];
    bool moved = false;
    for (int i = 0; i < PAD_ORDER_SLOTS; i++) {
        was[i] = *order_slot(i);
        id[i] = was[i] ? SDL_GetGamepadID(was[i]) : 0;
    }
    pad_order_apply(&order, id, order_first(), to);
    for (int i = 0; i < PAD_ORDER_SLOTS; i++) {
        if (to[i] < 0) continue;
        now[to[i]] = was[i];
        if (to[i] == i) continue;
        moved = true;
        LOG("[input] controller order: player %d: %s\n", to[i] + 1, SDL_GetGamepadName(was[i]));
    }
    if (!moved) return;  // (they were those players already)
    for (int i = 0; i < PAD_ORDER_SLOTS; i++) *order_slot(i) = now[i];
    note_family();  // (and note_players)
    recompute_pad();
    for (int i = 0; i < EXTRA_PADS; i++) recompute_extra(i);
    style_changed();
}

static void order_close(bool confirm) {
    if (confirm && !pad_order_ready(&order, order_first())) return;  // (no player 1 yet: Esc / B to leave)
    if (confirm) order_apply();
    order_open = false;
}

// Main thread, before the menu sees the event (as bind_capture_event): true = the page took it. With the page up a
// controller's buttons and sticks are its own, so L and R don't switch the menu's tabs and nothing moves its cursor;
// a release still goes on to the menu, which then never keeps a button held from before the page as down. Guide and
// Back + Start go on to the menu too, so a controller can still close it (input_pad_order_tick then cancels the
// page). Enter and Esc are the page's; the other keys and the mouse stay the menu's. A controller unplugged is out of
// the draft (the event goes on to input_handle_event, which closes it).
bool input_pad_order_event(const SDL_Event *e) {
    if (!order_open) return false;
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        if (e->key.scancode == SDL_SCANCODE_ESCAPE || e->key.scancode == SDL_SCANCODE_AC_BACK) order_close(false);
        else if (e->key.scancode != SDL_SCANCODE_RETURN && e->key.scancode != SDL_SCANCODE_KP_ENTER) return false;
        else if (!e->key.repeat) order_close(true);
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        bool down = e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE || e->gbutton.button == SDL_GAMEPAD_BUTTON_BACK ||
            e->gbutton.button == SDL_GAMEPAD_BUTTON_START) return false;  // (the menu's open / close: menu.mm)
        SDL_Gamepad *g = order_pad(e->gbutton.which);
        int lead = pad_order_lead(&order);
        if (g && (e->gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER ||
                  e->gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) {
            pad_order_lr(&order, e->gbutton.which, SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) +
                         SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER), SDL_GetTicks(), order_first());
            order_publish();
        } else if (down && e->gbutton.button == order_button(false)) {
            if (lead < 0 || order.box[lead] == e->gbutton.which) order_close(false);
        } else if (down && e->gbutton.button == order_button(true)) {
            if (lead >= 0 && order.box[lead] == e->gbutton.which) order_close(true);
        }
        return down;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        return true;
    case SDL_EVENT_GAMEPAD_REMOVED:
        pad_order_out(&order, e->gdevice.which, true);
        order_publish();
        return false;
    default:
        return false;
    }
}

// Main thread, once per round of events: the page's requests, the menu closed under it, and the holds to leave
void input_pad_order_tick(bool menu_open) {
    int req = atomic_exchange(&order_request, 0);
    if (!order_open) {
        if (req != PAD_ORDER_OPEN || !menu_open) return;
        memset(&order, 0, sizeof order);  // (a new draft: every box empty)
        order_publish();
        order_open = true;
        return;
    }
    if (req == PAD_ORDER_CANCEL || !menu_open) { order_close(false); return; }
    if (req == PAD_ORDER_CONFIRM) { order_close(true); return; }
    if (pad_order_tick(&order, SDL_GetTicks())) order_publish();
}

// The page's side, from any thread
void input_pad_order_request(int what) { order_request = what; }
bool input_pad_order_active(void) { return order_open; }
uint32_t input_pad_order_box(int player, float *leaving) {
    if (player < 1 || player > BIND_PLAYERS) return 0;
    uint64_t since = order_hold[player - 1], now = SDL_GetTicks();
    float part = since ? (float)(now - since) / PAD_ORDER_LEAVE_MS : 0;
    if (leaving) *leaving = part > 1 ? 1 : part;
    return order_box[player - 1];
}
int input_pad_order_lead(void) {
    for (int p = 1; p <= BIND_PLAYERS; p++)
        if (order_box[p - 1]) return p;
    return 0;
}
bool input_pad_order_ready(void) { return pads_from_p2() || order_box[0]; }
int input_pad_order_button(bool confirm) { return order_button(confirm); }
// What a controller holds right now, for the page's drawing of it: *held has a bit per PADB_* value (the triggers as
// in recompute_pad), stick[] the left and the right stick's x and y (y up), *family its button names' (as
// input_pad_family); false when it has gone. Read from the controller itself under SDL's lock, as
// input_preview_stick.
bool input_pad_order_state(uint32_t id, uint32_t *held, int32_t stick[4], int *family) {
    SDL_LockJoysticks();
    SDL_Gamepad *g = id ? SDL_GetGamepadFromID(id) : NULL;
    if (g) {
        uint32_t host = 0;
        for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
            if (SDL_GetGamepadButton(g, (SDL_GamepadButton)i)) host |= 1u << i;
        if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) host |= 1u << PADB_LTRIGGER;
        if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) host |= 1u << PADB_RTRIGGER;
        *held = host;
        stick[0] = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX);
        stick[1] = -SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
        stick[2] = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX);
        stick[3] = -SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY);
        *family = family_of(g);
    }
    SDL_UnlockJoysticks();
    return g != NULL;
}

// (Android: the emulator's own keyboard, "qwerty2", has gamepad keys and SDL lists it as a gamepad; it would hide
// the on-screen controller)
static bool real_gamepad(SDL_JoystickID id) {
#ifdef __ANDROID__
    const char *n = SDL_GetGamepadNameForID(id);
    if (n && !strcmp(n, "qwerty2")) return false;
#else
    (void)id;
#endif
    return true;
}

// A game's PC controls (runtime/game_hooks.c game_pc_*): keys with Ctrl / Cmd, the middle and right mouse buttons, the
// wheel. A key taken when pressed is taken when released too, so the game never sees half of it.
static bool pc_event(const SDL_Event *e) {
    extern bool game_pc_key(int scancode, bool cmd, bool shift, bool down, bool repeat);
    extern bool game_pc_mouse(int button, int act, float x, float y);
    extern bool game_pc_wheel(float dx, float dy, bool shift);
    static bool taken[SDL_SCANCODE_COUNT];
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
        int sc = e->key.scancode;
        if (sc <= 0 || sc >= SDL_SCANCODE_COUNT) return false;
        bool down = e->type == SDL_EVENT_KEY_DOWN;
        if (!down) {
            if (!taken[sc]) return false;
            taken[sc] = false;
            game_pc_key(sc, false, false, false, false);
            return true;
        }
#ifdef __APPLE__
        bool cmd = (e->key.mod & SDL_KMOD_GUI) != 0;
#else
        bool cmd = (e->key.mod & SDL_KMOD_CTRL) != 0;
#endif
        if (!game_pc_key(sc, cmd, (e->key.mod & SDL_KMOD_SHIFT) != 0, true, e->key.repeat)) return false;
        taken[sc] = true;
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: case SDL_EVENT_MOUSE_MOTION: {
        if (e->button.which == SDL_TOUCH_MOUSEID) return false;
        int b = e->type == SDL_EVENT_MOUSE_MOTION
                    ? (e->motion.state & SDL_BUTTON_RMASK ? 3 : e->motion.state & SDL_BUTTON_MMASK ? 2 : 0)
                    : e->button.button == SDL_BUTTON_RIGHT ? 3 : e->button.button == SDL_BUTTON_MIDDLE ? 2 : 0;
        if (!b) return false;
        SDL_Window *w = SDL_GetWindowFromID(e->type == SDL_EVENT_MOUSE_MOTION ? e->motion.windowID : e->button.windowID);
        int ww = 1, wh = 1;
        if (w) SDL_GetWindowSize(w, &ww, &wh);
        float x = e->type == SDL_EVENT_MOUSE_MOTION ? e->motion.x : e->button.x;
        float y = e->type == SDL_EVENT_MOUSE_MOTION ? e->motion.y : e->button.y;
        return game_pc_mouse(b, e->type == SDL_EVENT_MOUSE_BUTTON_DOWN ? 0 : e->type == SDL_EVENT_MOUSE_MOTION ? 1 : 2,
                             x / (ww > 0 ? ww : 1), y / (wh > 0 ? wh : 1));
    }
    case SDL_EVENT_MOUSE_WHEEL:
        return game_pc_wheel(e->wheel.x, e->wheel.y, (SDL_GetModState() & SDL_KMOD_SHIFT) != 0);
    default:
        return false;
    }
}

void input_handle_event(const SDL_Event *e) {
    extern bool touchpad_handle_event(const SDL_Event *e);  // runtime/ui/touchpad.mm (the on-screen controller)
    extern bool touch_handle_event(const SDL_Event *e);  // runtime/hle/touch.c (touches, the mouse as a finger)
    // The overlay menu or a name entry up: its touches and clicks are its own (it takes the mouse SDL makes from a touch,
    // menu_handle_event); the fingers themselves would otherwise reach the on-screen controller and the game's touch
    // screen behind it (owner, 2026-10-23: a tap on a menu option also tapped the game). A release still goes through,
    // so a touch held when the menu opened ends.
    extern bool menu_input_captured(void);  // runtime/ui/menu.mm
    if (menu_input_captured()) switch (e->type) {
        case SDL_EVENT_FINGER_DOWN: case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_WHEEL:
            return;
        default: break;
    }
    if (touchpad_handle_event(e) || touch_handle_event(e)) return;
    if (pc_event(e)) return;  // (a game's own PC controls: game_pc_key / mouse / wheel)
    if (!scanned) {
        scanned = true;
        int n = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&n);
        if (n > 0 && !pad && !pads_from_p2() && real_gamepad(ids[0])) { pad = SDL_OpenGamepad(ids[0]); LOG("[input] gamepad: %s\n", SDL_GetGamepadName(pad)); note_family(); }
        for (int i = pad ? 1 : 0; i < n; i++) open_extra(ids[i]);
        SDL_free(ids);
    }
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP:
        if (e->type == SDL_EVENT_KEY_DOWN && e->key.scancode == SDL_SCANCODE_F9 && !e->key.repeat) g_debug_frame_request = 1;
        if (e->key.scancode < SDL_SCANCODE_COUNT) keys_down[e->key.scancode] = e->type == SDL_EVENT_KEY_DOWN;
        recompute_keyboard();
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        if (!pad && !pads_from_p2() && real_gamepad(e->gdevice.which)) { pad = SDL_OpenGamepad(e->gdevice.which); LOG("[input] gamepad: %s\n", SDL_GetGamepadName(pad)); note_family(); }
        else open_extra(e->gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (pad && SDL_GetGamepadID(pad) == e->gdevice.which) { SDL_CloseGamepad(pad); pad = NULL; note_family(); }
        for (int i = 0; i < EXTRA_PADS; i++)
            if (extra_pad[i] && SDL_GetGamepadID(extra_pad[i]) == e->gdevice.which) {
                LOG("[input] player %d: controller removed\n", i + 2);
                SDL_CloseGamepad(extra_pad[i]);
                extra_pad[i] = NULL;
                note_players();
                recompute_extra(i);
                style_changed();
            }
        recompute_pad();
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP:
        recompute_pad();
        for (int i = 0; i < EXTRA_PADS; i++)
            if (extra_pad[i]) recompute_extra(i);
        break;
    }
}

// Scripted input for testing: LARECOMP_INPUT="frame:BUTTON:frames,..." (BUTTON = A,B,X,Y,L,R,ZL,ZR,PLUS,
// MINUS,LEFT,UP,RIGHT,DOWN, or STICK_UP/DOWN/LEFT/RIGHT, RSTICK_UP/DOWN/LEFT/RIGHT, STICK_<degrees>: full tilt at
// that angle, 0 = right, 90 = up).
static _Atomic uint64_t script_buttons;
static _Atomic int32_t script_lx, script_ly, script_rx, script_ry;

// Tests of the bindings, through them like a real player:
// LARECOMP_HOST_INPUT="frame:key:<SDL key name>:frames,frame:pad:<button>:frames,..." holds a host key / controller
//   button (names as in larecomp.ini's bind_* lines: "Z", "Keypad 0", "south", "ltrigger"...);
//   "frame:lstick:<x>/<y>:frames" holds the controller's left stick there (raw SDL units, y up: a drifting stick).
// "frame:type:<text>" types <text> as real key events (down, text, up per character), through the overlay menu / name
//   entry first like a player's typing; "{Escape}" (any SDL key name) presses that key. No ',' or ':' in <text>.
// "frame:sdlpad:<button>:frames" / "frame:sdlkey:<key>:frames" hold a controller button / key as real events (down
//   at frame, up `frames` later): they reach menus and the on-screen keyboard first, as a player's would.
// "frame:click:<x>/<y>:frames" holds the left mouse button at window point x, y (a real event: the mouse as a finger).
// "frame:tap:<x>/<y>:frames" touches the touch screen at game pixel x, y (1280x720) for that long.
// "frame:finger:<x>/<y>[/<dy>]:frames" a finger on the window at point x, y as SDL reports one on a phone / tablet (the
//   mouse it makes up: moved there and pressed in the same instant, released `frames` later), sliding dy points down
//   over that time: the overlay menu's touch input.
// "frame:shot:<name>" saves the window (game + menus) to build/<name>.png.
// LARECOMP_BIND_TEST="frame:pad|key:<input>:<host>,..." starts a rebind capture of <input> (bind_* name: a, zl,
//   ls_up...) at that frame (the Controls tab shows it waiting) and presses <host> 90 frames later ("esc" cancels,
//   "wait" lets it time out); "frame:reset:pad|key" resets to the defaults. pad2 / pad3 / pad4 in place of pad:
//   that player's controller bindings (pad is player 1's).
static void push_key(SDL_Scancode sc, SDL_Keymod mod, bool down) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.which = TEST_DEVICE, e.key.scancode = sc, e.key.mod = mod, e.key.down = down;
    e.key.key = SDL_GetKeyFromScancode(sc, mod, false);
    SDL_PushEvent(&e);
}
// a real key event by SDL key name, as a player's ("Left", "K"): for a mod's tests that act on what is on screen
void input_test_key(const char *name, bool down) { push_key(SDL_GetScancodeFromName(name), SDL_KMOD_NONE, down); }

// "frame:kbd:<name>:0": a thread asks for text as a game's software keyboard does (runtime/hle/swkbd.c) while the
// game goes on presenting (the race of the dialog's re-present with the game's, runtime/nvn/objects.c nvn_represent)
static void *kbd_test_thread(void *arg) {
    extern int menu_keyboard_run(const char *initial, int max_len, char *out, int cap);  // runtime/ui/menu.mm
    char text[256] = "";
    int ok = menu_keyboard_run((const char *)arg, 32, text, sizeof text);
    LOG("[input] test keyboard: %s \"%s\"\n", ok ? "entered" : "cancelled", text);
    return NULL;
}

static void test_push(uint64_t frame, unsigned long start, unsigned long len, const char *dev, const char *arg) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    if (!strcmp(dev, "kbd")) {
        static char initial[400];
        pthread_t th;
        if (frame != start) return;
        snprintf(initial, sizeof initial, "%s", arg);
        if (!pthread_create(&th, NULL, kbd_test_thread, initial)) pthread_detach(th);
        return;
    }
    if (!strcmp(dev, "shot")) {
        extern char g_window_shot_request[256];  // runtime/hle/hookshot.c
        if (frame == start) snprintf(g_window_shot_request, sizeof g_window_shot_request, "build/%s.png", arg);
        return;
    }
    if (!strcmp(dev, "click") || !strcmp(dev, "rclick") || !strcmp(dev, "mclick")) {
        // "frame:click:<x>/<y>:frames": the left mouse button held at window point x, y (rclick: the right, mclick: the
        // middle button)
        float x = 0, y = 0;
        sscanf(arg, "%f/%f", &x, &y);
        if (frame != start && frame != start + len) return;
        e.type = frame == start ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.which = 1, e.button.down = frame == start, e.button.x = x, e.button.y = y;
        e.button.button = dev[0] == 'r' ? SDL_BUTTON_RIGHT : dev[0] == 'm' ? SDL_BUTTON_MIDDLE : SDL_BUTTON_LEFT;
        SDL_Window *w = SDL_GetMouseFocus() ? SDL_GetMouseFocus() : SDL_GetKeyboardFocus();
        if (!w) { int n = 0; SDL_Window **ws = SDL_GetWindows(&n); if (n > 0) w = ws[0]; SDL_free(ws); }
        e.button.windowID = w ? SDL_GetWindowID(w) : 0;
        SDL_PushEvent(&e);
        return;
    }
    if (!strcmp(dev, "finger")) {  // "frame:finger:<x>/<y>[/<dy>]:frames" (SDL's mouse from a touch)
        float x = 0, y = 0, dy = 0;
        sscanf(arg, "%f/%f/%f", &x, &y, &dy);
        if (frame < start || frame > start + len) return;
        float fy = y + (len ? dy * (float)(frame - start) / (float)len : 0);
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.which = SDL_TOUCH_MOUSEID, e.motion.windowID = TEST_DEVICE, e.motion.x = x, e.motion.y = fy;
        if (frame == start || dy != 0) SDL_PushEvent(&e);
        if (frame != start && frame != start + len) return;
        memset(&e, 0, sizeof e);
        e.type = frame == start ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.which = SDL_TOUCH_MOUSEID, e.button.windowID = TEST_DEVICE, e.button.button = SDL_BUTTON_LEFT;
        e.button.down = frame == start, e.button.x = x, e.button.y = fy;
        SDL_PushEvent(&e);
        return;
    }
    if (!strcmp(dev, "touch")) {  // "frame:touch:<x>/<y>:frames": a real SDL finger (down, up after the frames) at
        float x = 0, y = 0;       // game pixel x, y (1280 x 720): it goes the whole way, the overlay menu first
        sscanf(arg, "%f/%f", &x, &y);
        if (frame != start && frame != start + len) return;
        e.type = frame == start ? SDL_EVENT_FINGER_DOWN : SDL_EVENT_FINGER_UP;
        e.tfinger.touchID = 1, e.tfinger.fingerID = 0x544f43, e.tfinger.x = x / 1280.0f, e.tfinger.y = y / 720.0f;
        int n = 0;
        SDL_Window **ws = SDL_GetWindows(&n);
        e.tfinger.windowID = n > 0 ? SDL_GetWindowID(ws[0]) : 0;
        SDL_free(ws);
        SDL_PushEvent(&e);
        return;
    }
    if (!strcmp(dev, "tap")) {  // "frame:tap:<x>/<y>:frames": a finger on the touch screen at game pixel x, y
        extern void touch_test(int32_t x, int32_t y, bool down);  // runtime/hle/touch.c
        int x = 0, y = 0;
        sscanf(arg, "%d/%d", &x, &y);
        if (frame == start || frame == start + len) touch_test(x, y, frame == start);
        return;
    }
    if (!strcmp(dev, "drag")) {  // "frame:drag:<x0>/<y0>/<x1>/<y1>:frames": a finger held 10 frames at x0, y0, moved to
        extern void touch_test(int32_t x, int32_t y, bool down);  // x1, y1, held there, lifted after the frames
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        sscanf(arg, "%d/%d/%d/%d", &x0, &y0, &x1, &y1);
        if (frame < start || frame > start + len) return;
        long k = (long)(frame - start) - 10, n = (long)len - 20 > 1 ? (long)len - 20 : 1;
        if (k < 0) k = 0;
        if (k > n) k = n;
        touch_test((int32_t)(x0 + (x1 - x0) * k / n), (int32_t)(y0 + (y1 - y0) * k / n), frame != start + len);
        return;
    }
    if (!strcmp(dev, "sdlkey")) {
        if (frame == start || frame == start + len) push_key(SDL_GetScancodeFromName(arg), SDL_KMOD_NONE, frame == start);
        return;
    }
    if (!strcmp(dev, "sdlpad")) {
        int b = bind_pad_from_name(arg);
        if (b < 0 || b >= SDL_GAMEPAD_BUTTON_COUNT || (frame != start && frame != start + len)) return;
        e.type = frame == start ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
        e.gbutton.which = TEST_DEVICE, e.gbutton.button = (Uint8)b, e.gbutton.down = frame == start;
        SDL_PushEvent(&e);
        return;
    }
    if (frame != start) return;
    LOG("[input] test: typing \"%s\"\n", arg);
    static char ascii[128][2];  // (a text event keeps a pointer to its text: these live on)
    for (const char *p = arg; *p; p++) {
        if (*p == '{' && strchr(p, '}')) {  // a named key
            char nm[32];
            size_t n = (size_t)(strchr(p, '}') - p - 1);
            snprintf(nm, sizeof nm, "%.*s", (int)(n < sizeof nm ? n : sizeof nm - 1), p + 1);
            SDL_Scancode sc = SDL_GetScancodeFromName(nm);
            push_key(sc, SDL_KMOD_NONE, true), push_key(sc, SDL_KMOD_NONE, false);
            p += n + 1;
            continue;
        }
        SDL_Keymod mod = SDL_KMOD_NONE;
        SDL_Scancode sc = SDL_GetScancodeFromKey((SDL_Keycode)tolower((unsigned char)*p), &mod);
        if (isupper((unsigned char)*p)) mod |= SDL_KMOD_LSHIFT;
        push_key(sc, mod, true);
        int c = *p & 0x7f;
        ascii[c][0] = (char)c;
        memset(&e, 0, sizeof e);
        e.type = SDL_EVENT_TEXT_INPUT, e.text.windowID = TEST_DEVICE, e.text.text = ascii[c];
        SDL_PushEvent(&e);
        push_key(sc, mod, false);
    }
}

static void host_test_frame(uint64_t frame) {
    static const char *hspec, *bspec;
    static int init;
    if (!init) { init = 1; hspec = getenv("LARECOMP_HOST_INPUT"); bspec = getenv("LARECOMP_BIND_TEST"); host_test = hspec; }
    char buf[2048];
    if (hspec) {
        uint8_t keys[SDL_SCANCODE_COUNT] = {0};
        uint32_t padm = 0;
        int32_t sx = 0, sy = 0;
        snprintf(buf, sizeof buf, "%s", hspec);
        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            unsigned long start = 0, len = 10;
            char dev[8] = "", name[400] = "";
            if (sscanf(tok, "%lu:%7[a-z]:%399[^:]:%lu", &start, dev, name, &len) < 3) continue;
            if (!strcmp(dev, "type") || !strcmp(dev, "kbd") || !strcmp(dev, "touch") || !strcmp(dev, "sdlpad") || !strcmp(dev, "sdlkey") || !strcmp(dev, "shot") || !strcmp(dev, "tap") || !strcmp(dev, "drag") || !strcmp(dev, "click") || !strcmp(dev, "rclick") || !strcmp(dev, "mclick") || !strcmp(dev, "finger")) {
                test_push(frame, start, len, dev, name);
                continue;
            }
            if (frame < start || frame >= start + len) continue;
            if (!strcmp(dev, "key")) { SDL_Scancode sc = SDL_GetScancodeFromName(name); if (sc) keys[sc] = 1; }
            else if (!strcmp(dev, "lstick")) sscanf(name, "%d/%d", &sx, &sy);
            else { int h = bind_pad_from_name(name); if (h >= 0) padm |= 1u << h; }
        }
        bool kch = memcmp(keys, test_keys, sizeof keys) != 0, pch = padm != test_pad || sx != test_lx || sy != test_ly;
        memcpy(test_keys, keys, sizeof keys);
        test_pad = padm;
        test_lx = sx, test_ly = sy;
        if (kch) recompute_keyboard();
        if (pch) recompute_pad();
    }
    if (bspec) {
        snprintf(buf, sizeof buf, "%s", bspec);
        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            unsigned long start = 0;
            char dev[8] = "", in[16] = "", host[40] = "";
            if (sscanf(tok, "%lu:%7[a-z0-9]:%15[a-z_0-9]:%39[^:]", &start, dev, in, host) < 3) continue;
            const char *pn = !strcmp(dev, "reset") ? in : dev;  // "pad2".."pad4": that player (else player 1)
            int player = !strncmp(pn, "pad", 3) && pn[3] >= '2' && pn[3] <= '0' + BIND_PLAYERS && !pn[4]
                             ? pn[3] - '0' : 1;
            if (!strcmp(dev, "reset")) {
                if (frame == start) bind_reset(!strcmp(in, "key") ? BIND_DEV_KEY : BIND_DEV_PAD, player);
                continue;
            }
            int d = !strcmp(dev, "key") ? BIND_DEV_KEY : BIND_DEV_PAD;
            if (frame == start) {
                LOG("[input] test: capture %s %s\n", dev, in);
                bind_capture_begin(d, player, bind_slot_from_name(in));
            }
            if (frame == start + 90 && strcmp(host, "wait")) {
                SDL_Event e;
                memset(&e, 0, sizeof e);
                if (!strcmp(host, "esc") || d == BIND_DEV_KEY) {
                    e.type = SDL_EVENT_KEY_DOWN;
                    e.key.scancode = !strcmp(host, "esc") ? SDL_SCANCODE_ESCAPE : SDL_GetScancodeFromName(host);
                } else {
                    int h = bind_pad_from_name(host);
                    if (h >= PADB_LTRIGGER) {
                        e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
                        e.gaxis.axis = h == PADB_LTRIGGER ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
                        e.gaxis.value = 32767;
                    } else {
                        e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
                        e.gbutton.button = (Uint8)h;
                    }
                }
                bind_capture_event(&e);
            }
        }
    }
}

static uint64_t g_pad_obj;
volatile int g_inject_plus;  // frames to hold Plus for the game (movie.c: A/B skip the opening on rando seeds)
RT_ON_RESET(input_reboot_reset) { g_pad_obj = 0; g_inject_plus = 0; }  // (reboot: the controller object was guest memory)
void input_frame(uint64_t frame) {
    if (g_pad_obj && getenv("LARECOMP_DUMP_PAD") && frame % 600 == 0) {
        const uint8_t *m = (const uint8_t *)(uintptr_t)(g_pad_obj - 0x98);
        LOG("[pad] mgr %llx 4e0=%u 4e4=%u 4e8=%u 548=%#x 54c..=%02x %02x %02x 56c=%02x\n", g_pad_obj - 0x98,
            *(uint32_t *)(m + 0x4e0), *(uint32_t *)(m + 0x4e4), *(uint32_t *)(m + 0x4e8), *(uint32_t *)(m + 0x548),
            m[0x54c], m[0x54d], m[0x54e], m[0x56c]);
        for (int i = 0; i < 9; i++) {
            const uint8_t *o = m + 0x98 + 0x48 * i;
            uint64_t player = *(uint64_t *)(m + 0x10 * i);
            LOG("[pad]  ctl%d id %#x style %#x btn %#x conn %d %d | player %llx [8]=%u\n", i, *(uint32_t *)(o + 8),
                *(uint32_t *)(o + 0xc), *(uint32_t *)(o + 0x14), o[0x44], o[0x45], player,
                player ? *(uint32_t *)(uintptr_t)(player + 8) : 0);
        }
    }
    extern void watch_arm(void);
    watch_arm();
    host_test_frame(frame);
    padscript_frame();  // the presented-frame clock (RECOMP_PAD_CLOCK=frame, RECOMP_FRAME_SHOT)
    {
        static const char *fs;
        static int init;
        if (!init) { init = 1; fs = getenv("RECOMP_FRAME_SHOT"); }
        int64_t k = padscript_frames();
        for (const char *p = fs; p && *p && k >= 0;) {
            char *end;
            long long v = strtoll(p, &end, 0);
            if (end == p) break;
            if (v == k) {
                extern char g_frame_shot_request[256];
                const char *dir = getenv("RECOMP_TRACE_SHOT_DIR");
                snprintf(g_frame_shot_request, sizeof g_frame_shot_request, "%s/recomp_frame_f%lld.png", dir ? dir : "build", v);
            }
            p = *end ? end + 1 : end;
        }
    }
    // (without a script the game's own presses below, g_test_buttons from a mod, still apply: e.g. the 100 Mario
    // mod driving SMM2's hidden Endless menu for a real player)
    const char *spec = getenv("LARECOMP_INPUT");
    static const char *const names[] = {"A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R", "ZL", "ZR", "PLUS",
                                        "MINUS", "LEFT", "UP", "RIGHT", "DOWN"};
    uint64_t b = 0;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    static char buf[16384];  // (long scripted sessions: the perf walk is ~4 KB)
    snprintf(buf, sizeof buf, "%s", spec ? spec : "");
    for (char *tok = strtok(buf, ","); spec && tok; tok = strtok(NULL, ",")) {
        unsigned long start = 0, len = 10;
        char name[32] = "";
        if (sscanf(tok, "%lu:%31[A-Z_0-9]:%lu", &start, name, &len) < 2) continue;
        if (frame < start || frame >= start + len) continue;
        if (!strcmp(name, "STICK_UP")) ly = 32767;
        else if (!strcmp(name, "STICK_DOWN")) ly = -32767;
        else if (!strcmp(name, "STICK_LEFT")) lx = -32767;
        else if (!strcmp(name, "STICK_RIGHT")) lx = 32767;
        else if (!strcmp(name, "RSTICK_UP")) ry = 32767;
        else if (!strcmp(name, "RSTICK_DOWN")) ry = -32767;
        else if (!strcmp(name, "RSTICK_LEFT")) rx = -32767;
        else if (!strcmp(name, "RSTICK_RIGHT")) rx = 32767;
        else if (!strncmp(name, "STICK_", 6) && name[6] >= '0' && name[6] <= '9') {
            double r = atoi(name + 6) * 3.14159265358979 / 180;
            lx = (int32_t)lround(cos(r) * 32767), ly = (int32_t)lround(sin(r) * 32767);
        }
        for (int i = 0; i < 16; i++) if (!strcmp(name, names[i])) b |= 1ull << i;
    }
    {  // test hooks steering the stick (runtime/hle/doorjump.c LARECOMP_DOORJUMP_TEST)
        extern int32_t g_test_lx, g_test_ly;
        extern uint64_t g_test_stick_until;
        if (frame < g_test_stick_until) { lx = g_test_lx; ly = g_test_ly; }
        extern int32_t g_test_rx, g_test_ry;  // (a mod's right stick: SMM2's Course Maker scrolled by the mouse)
        extern uint64_t g_test_rstick_until;
        if (frame < g_test_rstick_until) { rx = g_test_rx; ry = g_test_ry; }
        extern uint64_t g_test_buttons, g_test_buttons_until;  // (LARECOMP_PLACE action script)
        if (frame < g_test_buttons_until) b |= g_test_buttons;
    }
    if (script2_on()) {  // (a scripted player 2: buttons and the four stick directions)
        static char buf2[4096];
        uint64_t b2 = 0;
        int32_t x2 = 0, y2 = 0;
        snprintf(buf2, sizeof buf2, "%s", getenv("LARECOMP_INPUT2"));
        char *save = NULL;
        for (char *tok = strtok_r(buf2, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            unsigned long start = 0, len = 10;
            char name[32] = "";
            if (sscanf(tok, "%lu:%31[A-Z_0-9]:%lu", &start, name, &len) < 2 || frame < start || frame >= start + len) continue;
            if (!strcmp(name, "STICK_UP")) y2 = 32767;
            else if (!strcmp(name, "STICK_DOWN")) y2 = -32767;
            else if (!strcmp(name, "STICK_LEFT")) x2 = -32767;
            else if (!strcmp(name, "STICK_RIGHT")) x2 = 32767;
            for (int i = 0; i < 16; i++) if (!strcmp(name, names[i])) b2 |= 1ull << i;
        }
        script2_buttons = b2, script2_lx = x2, script2_ly = y2;
    }
    script_buttons = b;
    script_lx = lx;
    script_ly = ly;
    script_rx = rx;
    script_ry = ry;
}

extern bool menu_input_captured(void);  // runtime/ui/menu.mm: overlay menu or name entry up (it has the keys / pad)

// Buttons held right now (keyboard, gamepad, on-screen controller, test script), independent of whether the game
// polls (movies, a game's host-drawn screens). The on-screen controller has no d-pad: its stick is the d-pad here.
extern void touchpad_state(uint64_t *buttons, int32_t *lx, int32_t *ly);  // runtime/ui/touchpad.mm
static uint64_t touch_buttons_dpad(void) {
    uint64_t tb;
    int32_t tx, ty;
    touchpad_state(&tb, &tx, &ty);
    if (tx < -16000) tb |= 1ull << BTN_LEFT;
    if (tx > 16000) tb |= 1ull << BTN_RIGHT;
    if (ty > 16000) tb |= 1ull << BTN_UP;
    if (ty < -16000) tb |= 1ull << BTN_DOWN;
    return tb;
}

// The player's own buttons (keyboard, gamepad, on-screen controller), for a game's host-drawn screens (not the
// menu's, not test scripts).
uint64_t input_player_buttons(void) { return menu_input_captured() ? 0 : key_buttons | pad_buttons | touch_buttons_dpad(); }

uint64_t input_buttons_now(void) {
    return (menu_input_captured() ? 0 : key_buttons | pad_buttons | touch_buttons_dpad()) | script_buttons;
}

// The pad state player 1's game reads, each time it reads it (a game's play recorder: games/smm2/playrec.c).
__attribute__((weak)) void game_pad_read(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    (void)buttons, (void)lx, (void)ly, (void)rx, (void)ry;
}

static int32_t clamp_axis(int32_t v) { return v > 32767 ? 32767 : v < -32767 ? -32767 : v; }

// NpadFullKeyState / NpadHandheldState / NpadJoy*State share this layout (0x28 bytes):
// {int64 sampling, u64 buttons, AnalogStick L {x,y}, AnalogStick R {x,y}, u32 attributes}
// (single: GetNpadState, x1 = the id; else one entry of GetNpadStates)
static void fill_state(Cpu *c, uint8_t *s, uint32_t id, bool single) {
    memset(s, 0, 0x28);
    *(int64_t *)s = ++sampling;
    static int nlog;
    if (nlog < 6 && (sampling % 600) == 1) { nlog++; LOG("[input] GetNpadState id %#x lr %llx\n", id, c->x[30]); }
    if (id >= 1 && id <= EXTRA_PADS) {  // players 2-4: their own controllers (none: disconnected)
        int i = (int)id - 1;
        if (!extra_connected(i) || padscript_active()) return;
        bool menu = menu_input_captured();
        uint64_t b = menu ? 0 : extra_buttons[i] | (i == 0 ? script2_buttons : 0);
        // the controller's sticks past their dead zones (Controls > Sticks, #98; off: as they are), before the script's
        int32_t plx = extra_lx[i], ply = extra_ly[i], prx = extra_rx[i], pry = extra_ry[i];
        stick_deadzone(&plx, &ply, g_settings.stick_deadzone_l);
        stick_deadzone(&prx, &pry, g_settings.stick_deadzone_r);
        int32_t lx = menu ? 0 : clamp_axis(plx + (i == 0 ? script2_lx : 0));
        int32_t ly = menu ? 0 : clamp_axis(ply + (i == 0 ? script2_ly : 0));
        int32_t rx = menu ? 0 : prx, ry = menu ? 0 : pry;
        if (lx < -16000) b |= 1ull << 16;
        if (ly > 16000) b |= 1ull << 17;
        if (lx > 16000) b |= 1ull << 18;
        if (ly < -16000) b |= 1ull << 19;
        if (rx < -16000) b |= 1ull << 20;
        if (ry > 16000) b |= 1ull << 21;
        if (rx > 16000) b |= 1ull << 22;
        if (ry < -16000) b |= 1ull << 23;
        *(uint64_t *)(s + 8) = b;
        *(int32_t *)(s + 16) = lx, *(int32_t *)(s + 20) = ly, *(int32_t *)(s + 24) = rx, *(int32_t *)(s + 28) = ry;
        *(uint32_t *)(s + 32) = 1;  // IsConnected
        return;
    }
    if (id != 0) return;  // docked: Handheld stays disconnected
    {   // a direct boot's tick (games/smm2/mods/boot.c), once per presented frame, on the game's main thread
        extern void boot_game_tick(Cpu *c);
        extern uint64_t renderer_present_count(void);
        static uint64_t boot_frame = ~0ull;
        if (renderer_present_count() != boot_frame) { boot_frame = renderer_present_count(); boot_game_tick(c); }
    }
    if (padscript_active()) {  // a scripted replay (RECOMP_PAD_SCRIPT): the script's buttons and sticks alone
        uint64_t b = padscript_buttons();
        int32_t lx, ly, rx, ry;
        padscript_sticks(&lx, &ly, &rx, &ry);
        if (lx < -16000) b |= 1ull << 16;   // (the stick directions as above)
        if (ly > 16000) b |= 1ull << 17;
        if (lx > 16000) b |= 1ull << 18;
        if (ly < -16000) b |= 1ull << 19;
        if (rx < -16000) b |= 1ull << 20;
        if (ry > 16000) b |= 1ull << 21;
        if (rx > 16000) b |= 1ull << 22;
        if (ry < -16000) b |= 1ull << 23;
        *(uint64_t *)(s + 8) = b;
        *(int32_t *)(s + 16) = lx, *(int32_t *)(s + 20) = ly, *(int32_t *)(s + 24) = rx, *(int32_t *)(s + 28) = ry;
        *(uint32_t *)(s + 32) = 1;  // IsConnected
        game_pad_read(b, lx, ly, rx, ry);  // (what the game read, as for a controller: a replay records it again)
        return;
    }
    extern void warp_game_tick(Cpu *c);
    warp_game_tick(c);  // game main thread, once per frame: apply a queued warp
    { extern void give_item_tick(Cpu *c, uint64_t frame); extern uint64_t renderer_present_count(void);
      give_item_tick(c, renderer_present_count());
      extern void chamber_game_tick(Cpu *c, uint64_t frame);
      chamber_game_tick(c, renderer_present_count());
      extern void digs_game_tick(Cpu *c, uint64_t frame);
      digs_game_tick(c, renderer_present_count());
      extern void qol_tick(void);
      qol_tick();
      extern void pets_game_tick(Cpu *c);
      pets_game_tick(c); }
    extern uint64_t g_watch_lo, g_watch_hi;
    if (single && id == 0 && !g_pad_obj) g_pad_obj = c->x[1] - 8;
    if (single && id == 0 && !g_watch_lo && getenv("LARECOMP_WATCH_PAD")) {
        unsigned lo = 0x98 + 0x10, hi = 0x98 + 0x48;  // offsets from the controller manager
        sscanf(getenv("LARECOMP_WATCH_PAD"), "%x,%x", &lo, &hi);
        g_watch_lo = c->x[1] - 8 - 0x98 + lo;
        g_watch_hi = c->x[1] - 8 - 0x98 + hi;
        LOG("[input] watching controller object %llx\n", c->x[1] - 8);
    }
    // a game's host-drawn screen has the player's controls (runtime/game_hooks.c); test input (scripts, a mod's own
    // presses) still reaches the game. game_input_mask: buttons of the player's a host screen keeps for itself.
    extern bool game_overlay_captured(void);
    extern uint64_t game_input_mask(void);
    bool menu = menu_input_captured() || game_overlay_captured();  // the overlay menu / name entry has the controls (typing there isn't for the game)
    uint64_t touch_b = 0;  // the on-screen controller (runtime/ui/touchpad.mm)
    int32_t touch_lx = 0, touch_ly = 0;
    if (!menu) touchpad_state(&touch_b, &touch_lx, &touch_ly);
    uint64_t pb = pad_buttons;
    {   // "A and B swapped in menus" (Controls, #84): a controller's A and B trade places while the game is in a menu
        // (game_in_play false), so the bottom button confirms there; in play they stay as bound
        extern bool game_in_play(void);
        bool sw = g_settings.menu_ab_swap && !menu && !game_in_play();
        static bool was;
        if (sw != was) LOG("[input] A and B %s (menus)\n", sw ? "swapped" : "as bound");
        was = sw;
        if (sw) pb = (pb & ~3ull) | (pb >> BTN_A & 1) << BTN_B | (pb >> BTN_B & 1) << BTN_A;
    }
    uint64_t b = menu ? script_buttons : ((key_buttons | pb | touch_b) & game_input_mask()) | script_buttons;
    {   // injected Plus: held for ~10 presented frames (the game polls several times per frame)
        extern uint64_t renderer_present_count(void);
        static uint64_t until;
        if (g_inject_plus > 0) { until = renderer_present_count() + 10; g_inject_plus = 0; }
        if (renderer_present_count() < until) b |= 1ull << BTN_PLUS;
    }
    // the controller's sticks past their dead zones (Controls > Sticks, #98; off: as they are): the keyboard, the
    // on-screen stick and scripts have none
    int32_t plx = pad_lx, ply = pad_ly, prx = pad_rx, pry = pad_ry;
    stick_deadzone(&plx, &ply, g_settings.stick_deadzone_l);
    stick_deadzone(&prx, &pry, g_settings.stick_deadzone_r);
    int32_t lx = clamp_axis((menu ? 0 : key_lx + plx + touch_lx) + script_lx);
    int32_t ly = clamp_axis((menu ? 0 : key_ly + ply + touch_ly) + script_ly);
    int32_t rx = clamp_axis((menu ? 0 : key_rx + prx) + script_rx);
    int32_t ry = clamp_axis((menu ? 0 : key_ry + pry) + script_ry);
    extern bool mirror_input(void);  // mirror mode, the world on screen (runtime/hle/mirror.c): left and right swap
    bool mirrored = !menu && mirror_input();
    if (mirrored) {
        lx = -lx;
        uint64_t l = (b >> BTN_LEFT) & 1, r = (b >> BTN_RIGHT) & 1;
        b = (b & ~((1ull << BTN_LEFT) | (1ull << BTN_RIGHT))) | (l << BTN_RIGHT) | (r << BTN_LEFT);
    }
    // d-pad / arrow keys walk (setting) while the stick is at rest. "At rest" has a dead zone: a controller's stick
    // never reads exactly 0 (a DualSense rests at a few hundred), which used to turn the d-pad off altogether.
    if (!menu && g_settings.dpad_move && (int64_t)lx * lx + (int64_t)ly * ly < 8000ll * 8000) {
        uint64_t d = key_buttons | pad_buttons | script_buttons;
        int dx = (int)((d >> BTN_RIGHT) & 1) - (int)((d >> BTN_LEFT) & 1);
        if (mirrored) dx = -dx;
        int dy = (int)((d >> BTN_UP) & 1) - (int)((d >> BTN_DOWN) & 1);
        int32_t m = dx && dy ? 23170 : 32767;  // diagonals: same speed (32767 / sqrt 2)
        if (dx || dy) lx = dx * m, ly = dy * m;
    }
    {   // door jumps stop Link for a moment before moving him (runtime/hle/doorjump.c): stick + d-pad released
        extern int32_t g_stick_lx, g_stick_ly;  // (what the player holds, before that: doorjump.c reads it)
        g_stick_lx = lx, g_stick_ly = ly;
        extern uint64_t g_stick_freeze_until;
        extern uint64_t renderer_present_count(void);
        if (renderer_present_count() < g_stick_freeze_until) {
            lx = ly = 0;
            b &= ~(0xfull << 12);
        }
    }
    { extern uint64_t touch_gesture_buttons(void); b |= touch_gesture_buttons(); }  // (runtime/hle/touch.c: L + R by touch)
    // stick directions also appear as buttons (bits 16-23)
    if (lx < -16000) b |= 1ull << 16;
    if (ly > 16000) b |= 1ull << 17;
    if (lx > 16000) b |= 1ull << 18;
    if (ly < -16000) b |= 1ull << 19;
    if (rx < -16000) b |= 1ull << 20;
    if (ry > 16000) b |= 1ull << 21;
    if (rx > 16000) b |= 1ull << 22;
    if (ry < -16000) b |= 1ull << 23;
    {   // the pause menu's companion slot (runtime/hle/pets.c) takes the d-pad / sticks / A while the cursor is on it
        extern uint64_t pets_menu_input(uint64_t b, int32_t st[4]);
        int32_t st[4] = {lx, ly, rx, ry};
        b = pets_menu_input(b, st);
        lx = st[0], ly = st[1], rx = st[2], ry = st[3];
    }
    *(uint64_t *)(s + 8) = b;
    static uint64_t last_b;
    if (b != last_b && getenv("LARECOMP_INPUT")) LOG("[input] id %#x buttons %#llx (sampling %lld, lr %llx)\n", id, (unsigned long long)b, (long long)sampling, c->x[30]);
    last_b = b;
    *(int32_t *)(s + 16) = lx;
    *(int32_t *)(s + 20) = ly;
    { extern void input_note_rstick(int32_t ry); input_note_rstick(ry); }
    *(int32_t *)(s + 24) = rx;
    *(int32_t *)(s + 28) = ry;
    *(uint32_t *)(s + 32) = 1;  // IsConnected
    game_pad_read(b, lx, ly, rx, ry);
}

#define FILL1 fill_state(c, (uint8_t *)(uintptr_t)c->x[0], *(uint32_t *)(uintptr_t)c->x[1], true)
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", hid_state_fullkey) { FILL1; }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", hid_state_dual) { FILL1; }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj", hid_state_left) { FILL1; }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", hid_state_handheld) { FILL1; }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj", hid_state_right) { FILL1; }

// int GetNpadStates(State *out, int count, const NpadIdType &id): the newest `count` samples, newest first. (SMM2 reads
// its pads this way; the SDK's own version reads hid shared memory, which nothing fills here.) Each call is one new
// sample; the older entries repeat it with the sampling numbers before it, so a game that walks the history since its
// last read sees one new sample per read.
static void fill_states(Cpu *c) {
    uint8_t *out = (uint8_t *)(uintptr_t)c->x[0];
    int n = (int)c->x[1];
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[2];
    if (n > 16) n = 16;
    if (n < 1) { c->x[0] = 0; return; }
    fill_state(c, out, id, false);
    int64_t smp = *(int64_t *)out;
    static int nlog;
    if (nlog < 4 && id == 0) { nlog++; LOG("[input] GetNpadStates id %#x count %d lr %llx\n", id, n, c->x[30]); }
    for (int i = 1; i < n; i++) {
        memcpy(out + 0x28 * i, out, 0x28);
        *(int64_t *)(out + 0x28 * i) = smp - i > 0 ? smp - i : 0;
    }
    c->x[0] = (uint64_t)n;
}
HLE_FUNC("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj", hid_states_fullkey) { fill_states(c); }
HLE_FUNC("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj", hid_states_dual) { fill_states(c); }
HLE_FUNC("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj", hid_states_left) { fill_states(c); }
HLE_FUNC("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj", hid_states_handheld) { fill_states(c); }
HLE_FUNC("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj", hid_states_right) { fill_states(c); }

// FullKey for player 1, and for players 2-4 with a controller (we report docked mode, where Handheld is unavailable)
HLE_FUNC("_ZN2nn3hid15GetNpadStyleSetERKj", hid_style_set) {
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[0];
    static int nlog;
    if (nlog++ < 4) LOG("[input] GetNpadStyleSet id %#x\n", id);
    c->x[0] = id == 0 || (id >= 1 && id <= EXTRA_PADS && extra_connected((int)id - 1)) ? 1 : 0;
}

// BindNpadStyleSetUpdateEvent(id, SystemEventType*, clear mode): signaled once so the game sees a
// controller connected.
#include "../kernel.h"
static KEvent *style_events[16];
static int nstyle_events;
static void style_changed(void) {  // (UI thread: a controller came or went) every bound style set event
    pthread_mutex_lock(&g_klock);
    for (int i = 0; i < nstyle_events; i++) kernel_signal(style_events[i]);
    pthread_mutex_unlock(&g_klock);
}
HLE_FUNC("_ZN2nn3hid27BindNpadStyleSetUpdateEventERKjPNS_2os15SystemEventTypeENS3_14EventClearModeE", hid_bind_style_event) {
    uint32_t w, r;
    KEvent *ev = kernel_create_event_pair(&w, &r);
    pthread_mutex_lock(&g_klock);
    kernel_signal(ev);
    if (nstyle_events < 16) style_events[nstyle_events++] = ev;
    pthread_mutex_unlock(&g_klock);
    uint64_t clear_mode = c->x[2];
    c->x[0] = c->x[1];
    c->x[1] = r;
    c->x[2] = 0;
    c->x[3] = clear_mode;
    guest_call(c, guest_sym("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"));
    LOG("[input] style set update event bound\n");
}

// Vibration devices: accepted but inert (the SDK's vibration node graph stays recompiled).
HLE_FUNC("_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", hid_init_vibration) { (void)c; }
HLE_FUNC("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", hid_vibration_handles) {
    uint32_t *out = (uint32_t *)(uintptr_t)c->x[0];
    int n = (int)c->x[1] < 2 ? (int)c->x[1] : 2;
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[2];
    for (int i = 0; i < n; i++) out[i] = (id & 0xFF) << 8 | (uint32_t)i | 0x03000000u;  // type, player, position
    c->x[0] = (uint64_t)n;
}
HLE_FUNC("_ZN2nn3hid22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE", hid_vibration_info) {
    uint32_t *info = (uint32_t *)(uintptr_t)c->x[0];
    uint32_t h = *(uint32_t *)(uintptr_t)c->x[1];
    info[0] = 1;        // VibrationDeviceType_LinearResonantActuator
    info[1] = h & 1;    // position: left / right
}

// Controller support (the system's "change grip / order" screen): on the Switch nn::hid::ShowControllerSupport launches
// a library applet; our am service has none, so a game waiting for it (SMM2: Play Together) would hang. It is answered
// at once with the controllers as they are: player 1 and one more player per further controller (players 2-4).
// ControllerSupportArg: u8 min players, u8 max players, ...; ControllerSupportResultInfo: s8 players, u8 pad[3],
// u32 selected Npad id.
int input_players(void) {
    int n = 1;
    for (int i = 0; i < EXTRA_PADS; i++) n += extra_connected(i);
    return n;
}
HLE_FUNC("_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE",
         hid_show_controller_support) {
    uint64_t out = c->x[0], arg = c->x[1];
    int min = arg ? *(const int8_t *)(uintptr_t)arg : 0, max = arg ? *(const int8_t *)(uintptr_t)(arg + 1) : 0;
    int n = input_players();
    if (max > 0 && n > max) n = max;
    if (out) {
        *(int8_t *)(uintptr_t)out = (int8_t)n;
        *(uint32_t *)(uintptr_t)(out + 4) = 0;  // Npad No. 1
    }
    LOG("[hid] controller support (players %d..%d): %d player(s)%s\n", min, max, n,
        n < min ? " - fewer than the game asks for" : "");
    c->x[0] = 0;
}
