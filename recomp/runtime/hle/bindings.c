// Control remapping: tables runtime/hle/input.c reads on every input event (so a change applies at once), the
// capture used by the "click a binding, then press" UI, and their larecomp.ini lines (bind_pad_<input>, bind_pad_pN
// and bind_pad_pN_<input> for a player 2-4's own controller table, bind_key_<input>). See runtime/input_bind.h.
#include <SDL3/SDL.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../input_bind.h"

_Static_assert(PADB_LTRIGGER == SDL_GAMEPAD_BUTTON_COUNT, "trigger pseudo-buttons follow SDL's buttons");

// Defaults: Nintendo layout by position (east = A, south = B, north = X, west = Y), triggers = ZL / ZR;
// keyboard WASD left stick, arrows d-pad, K/J/L/I = A/B/X/Y, Q/E = L/R, 1/3 = ZL/ZR, Enter / Backspace = Plus /
// Minus, F / numpad 0 = stick clicks, numpad 8/5/4/6 right stick.
#define DEF_PAD { \
    SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_WEST, \
    SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, \
    SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PADB_LTRIGGER, PADB_RTRIGGER, SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK, \
    SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_DOWN}
#define DEF_KEY { \
    SDL_SCANCODE_K, SDL_SCANCODE_J, SDL_SCANCODE_L, SDL_SCANCODE_I, SDL_SCANCODE_F, SDL_SCANCODE_KP_0, \
    SDL_SCANCODE_Q, SDL_SCANCODE_E, SDL_SCANCODE_1, SDL_SCANCODE_3, SDL_SCANCODE_RETURN, SDL_SCANCODE_BACKSPACE, \
    SDL_SCANCODE_LEFT, SDL_SCANCODE_UP, SDL_SCANCODE_RIGHT, SDL_SCANCODE_DOWN, \
    SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_A, SDL_SCANCODE_D, SDL_SCANCODE_KP_8, SDL_SCANCODE_KP_5, \
    SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_6}
static const int16_t k_def_pad[BIND_PAD_COUNT] = DEF_PAD, k_def_key[BIND_KEY_COUNT] = DEF_KEY;
int16_t g_bind_key[BIND_KEY_COUNT] = DEF_KEY;
// The controller tables, one per player (#100). Players 2-4 follow player 1's until one is changed for them
// (pad_custom): from then on they have their own.
_Static_assert(BIND_PLAYERS == 4, "one DEF_PAD per player");
static int16_t pad_tables[BIND_PLAYERS][BIND_PAD_COUNT] = {DEF_PAD, DEF_PAD, DEF_PAD, DEF_PAD};
static bool pad_custom[BIND_PLAYERS] = {true};

static bool valid_player(int player) { return player >= 1 && player <= BIND_PLAYERS; }
const int16_t *bind_pad_table(int player) {
    return pad_tables[valid_player(player) && pad_custom[player - 1] ? player - 1 : 0];
}
bool bind_pad_custom(int player) { return valid_player(player) && pad_custom[player - 1]; }
// "controller" / "keyboard" for the log, players 2-4's controllers by their number
static const char *dev_name(int dev, int player) {
    static const char *const pads[BIND_PLAYERS] = {"controller", "player 2 controller", "player 3 controller",
                                                   "player 4 controller"};
    return dev == BIND_DEV_PAD ? pads[valid_player(player) ? player - 1 : 0] : "keyboard";
}

static const char *const k_ini[BIND_KEY_COUNT] = {
    "a", "b", "x", "y", "lstick", "rstick", "l", "r", "zl", "zr", "plus", "minus", "left", "up", "right", "down",
    "ls_up", "ls_down", "ls_left", "ls_right", "rs_up", "rs_down", "rs_left", "rs_right",
};
static const char *const k_names[BIND_KEY_COUNT] = {
    "A", "B", "X", "Y", "Left stick press", "Right stick press", "L", "R", "ZL", "ZR", "Plus", "Minus",
    "D-pad Left", "D-pad Up", "D-pad Right", "D-pad Down", "Left stick Up", "Left stick Down", "Left stick Left",
    "Left stick Right", "Right stick Up", "Right stick Down", "Right stick Left", "Right stick Right",
};
// controller buttons by position (larecomp.ini)
static const char *const k_pad_ini[PADB_HOST_COUNT] = {
    "south", "east", "west", "north", "back", "guide", "start", "lstick", "rstick", "lshoulder", "rshoulder",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right", "misc1", "rpaddle1", "lpaddle1", "rpaddle2", "lpaddle2",
    "touchpad", "misc2", "misc3", "misc4", "misc5", "misc6", "ltrigger", "rtrigger",
};

int bind_slot_from_name(const char *name) {
    for (int i = 0; i < BIND_KEY_COUNT; i++) if (!strcmp(name, k_ini[i])) return i;
    return -1;
}
int bind_pad_from_name(const char *name) {
    for (int i = 0; i < PADB_HOST_COUNT; i++) if (!strcmp(name, k_pad_ini[i])) return i;
    return -1;
}

const char *bind_input_name(int slot) { return slot >= 0 && slot < BIND_KEY_COUNT ? k_names[slot] : "?"; }

// fam: 0 Nintendo (and pads SDL doesn't name), 1 Xbox, 2 PlayStation
const char *bind_pad_label(int host, int fam) {
    static const char *const nin[PADB_HOST_COUNT] = {
        "B", "A", "Y", "X", "Minus", "Home", "Plus", "Left stick press", "Right stick press", "L", "R",
        "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right", "Capture", "Right paddle 1", "Left paddle 1",
        "Right paddle 2", "Left paddle 2", "Touchpad", "Misc 2", "Misc 3", "Misc 4", "Misc 5", "Misc 6", "ZL", "ZR",
    };
    static const char *const xb[PADB_HOST_COUNT] = {
        "A", "B", "X", "Y", "View", "Xbox", "Menu", "LS (press)", "RS (press)", "LB", "RB",
        "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right", "Share", "Paddle P1", "Paddle P3",
        "Paddle P2", "Paddle P4", "Touchpad", "Misc 2", "Misc 3", "Misc 4", "Misc 5", "Misc 6", "LT", "RT",
    };
    static const char *const ps[PADB_HOST_COUNT] = {
        "Cross", "Circle", "Square", "Triangle", "Create", "PS", "Options", "L3", "R3", "L1", "R1",
        "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right", "Mute", "Right back button", "Left back button",
        "Right Fn", "Left Fn", "Touchpad", "Misc 2", "Misc 3", "Misc 4", "Misc 5", "Misc 6", "L2", "R2",
    };
    if (host < 0 || host >= PADB_HOST_COUNT) return "(none)";
    return fam == 1 ? xb[host] : fam == 2 ? ps[host] : nin[host];
}

const char *bind_key_label(int sc) {
    if (sc <= 0 || sc >= SDL_SCANCODE_COUNT) return "(none)";
    const char *n = SDL_GetScancodeName((SDL_Scancode)sc);
    return n && *n ? n : "(unnamed key)";
}

int bind_label_family(void) {
    extern int input_pad_family(void);  // runtime/hle/input.c (the game's open pad, or LARECOMP_PAD_TYPE)
    int f = input_pad_family();
    if (f) return f;
    int n = 0, t = 0;  // start screen: input.c hasn't opened a pad yet
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (ids && n > 0) t = SDL_GetGamepadTypeForID(ids[0]);
    SDL_free(ids);
    return t == SDL_GAMEPAD_TYPE_XBOX360 || t == SDL_GAMEPAD_TYPE_XBOXONE ? 1 :
           t == SDL_GAMEPAD_TYPE_PS3 || t == SDL_GAMEPAD_TYPE_PS4 || t == SDL_GAMEPAD_TYPE_PS5 ? 2 : 0;
}

static void result(int kind, int dev, int player, int slot, int other);

// (a player 2-4 that followed player 1 has its own table from here: the defaults, whatever player 1's are)
void bind_reset(int dev, int player) {
    if (dev == BIND_DEV_PAD) {
        if (!valid_player(player)) return;
        memcpy(pad_tables[player - 1], k_def_pad, sizeof k_def_pad);
        pad_custom[player - 1] = true;
    } else memcpy(g_bind_key, k_def_key, sizeof g_bind_key);
    LOG("[input] %s bindings reset to the defaults\n", dev_name(dev, player));
    result(5, dev, player, -1, -1);
}

void bind_pad_follow(int player) {
    if (player < 2 || player > BIND_PLAYERS) return;
    pad_custom[player - 1] = false;
    LOG("[input] %s bindings same as player 1\n", dev_name(BIND_DEV_PAD, player));
    result(6, BIND_DEV_PAD, player, -1, -1);
}

bool bind_is_default(int dev, int player) {
    return dev == BIND_DEV_PAD ? !memcmp(bind_pad_table(player), k_def_pad, sizeof k_def_pad) :
                                 !memcmp(g_bind_key, k_def_key, sizeof g_bind_key);
}

bool bind_is_default_slot(int dev, int player, int slot) {
    return dev == BIND_DEV_PAD ? bind_pad_table(player)[slot] == k_def_pad[slot] : g_bind_key[slot] == k_def_key[slot];
}

int bind_prompt_host(int slot) { return slot >= 0 && slot < BIND_PAD_COUNT ? bind_pad_table(1)[slot] : -1; }

// ------------------------------------------------------------------ capture
static _Atomic int cap_dev = -1, cap_player = 1, cap_slot = -1;
static _Atomic uint64_t cap_deadline;
static _Atomic int res_kind, res_dev, res_player, res_slot, res_other, res_serial;

static void result(int kind, int dev, int player, int slot, int other) {
    res_kind = kind, res_dev = dev, res_player = player, res_slot = slot, res_other = other;
    res_serial++;
}

void bind_capture_begin(int dev, int player, int slot) {
    if (slot < 0 || slot >= (dev == BIND_DEV_PAD ? BIND_PAD_COUNT : BIND_KEY_COUNT)) return;
    if (dev != BIND_DEV_PAD) player = 1;  // (the keyboard is player 1's)
    if (!valid_player(player)) return;
    cap_deadline = SDL_GetTicks() + BIND_CAPTURE_MS;
    cap_slot = slot;
    cap_player = player;
    cap_dev = dev;
}

void bind_capture_cancel(void) {
    if (cap_dev < 0) return;
    result(3, cap_dev, cap_player, cap_slot, -1);
    cap_dev = -1;
}

bool bind_capture_active(int *dev, int *player, int *slot, int *ms_left) {
    int d = cap_dev;
    if (d >= 0 && SDL_GetTicks() >= cap_deadline) { result(4, d, cap_player, cap_slot, -1); cap_dev = d = -1; }
    if (dev) *dev = d;
    if (player) *player = d >= 0 ? cap_player : -1;
    if (slot) *slot = d >= 0 ? cap_slot : -1;
    if (ms_left) *ms_left = d >= 0 ? (int)(cap_deadline - SDL_GetTicks()) : 0;
    return d >= 0;
}

int bind_last_result(int *dev, int *player, int *slot, int *other, int *serial) {
    if (dev) *dev = res_dev;
    if (player) *player = res_player;
    if (slot) *slot = res_slot;
    if (other) *other = res_other;
    if (serial) *serial = res_serial;
    return res_kind;
}

// Bind `host` to `slot`; an input already on `host` takes the slot's old binding (swap).
static void assign(int dev, int player, int slot, int host) {
    if (dev == BIND_DEV_PAD && !pad_custom[player - 1]) {  // following player 1 until now: its own table, from a copy
        memcpy(pad_tables[player - 1], pad_tables[0], sizeof pad_tables[0]);
        pad_custom[player - 1] = true;
    }
    int16_t *t = dev == BIND_DEV_PAD ? pad_tables[player - 1] : g_bind_key;
    int n = dev == BIND_DEV_PAD ? BIND_PAD_COUNT : BIND_KEY_COUNT, other = -1;
    for (int i = 0; i < n; i++)
        if (i != slot && t[i] == host) { other = i; t[i] = t[slot]; }
    t[slot] = (int16_t)host;
    LOG("[input] %s %s -> %s%s%s%s\n", dev_name(dev, player), k_names[slot],
        dev == BIND_DEV_PAD ? k_pad_ini[host] : bind_key_label(host), other >= 0 ? " (swapped with " : "",
        other >= 0 ? k_names[other] : "", other >= 0 ? ")" : "");
    if (other >= 0) LOG("[input]   %s -> %s\n", k_names[other], dev == BIND_DEV_PAD ? k_pad_ini[t[other]] : bind_key_label(t[other]));
    result(other >= 0 ? 2 : 1, dev, player, slot, other);
}

// A controller that is another player's: its presses are not for this capture. One that is nobody's (a test's
// events too) is taken as before, and so is any controller while the capture's player has none (player 1 with
// "Controllers start at player 2": its table is the one the players following it use).
static bool other_players_pad(SDL_JoystickID which) {
    int p = input_pad_player(which);
    return p >= 0 && p != cap_player && input_player_pad(cap_player);
}

bool bind_capture_event(const SDL_Event *e) {
    int dev = cap_dev;
    if (dev < 0) return false;
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        if (e->key.repeat) return true;
        if (e->key.scancode == SDL_SCANCODE_ESCAPE) { bind_capture_cancel(); return true; }
        // F1 (menu), F9 (frame dump), F11 (fullscreen) stay the overlay's; a controller capture ignores keys
        if (dev != BIND_DEV_KEY || e->key.scancode == SDL_SCANCODE_F1 || e->key.scancode == SDL_SCANCODE_F9 ||
            e->key.scancode == SDL_SCANCODE_F11 || e->key.scancode >= SDL_SCANCODE_COUNT) return true;
        cap_dev = -1;
        assign(dev, cap_player, cap_slot, e->key.scancode);
        return true;
    case SDL_EVENT_KEY_UP: case SDL_EVENT_TEXT_INPUT:
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        // Home / Guide opens the menu; a keyboard capture ignores the controller (it times out, or Esc)
        if (dev != BIND_DEV_PAD || e->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE || e->gbutton.button >= SDL_GAMEPAD_BUTTON_COUNT) return true;
        if (other_players_pad(e->gbutton.which)) return true;
        cap_dev = -1;
        assign(dev, cap_player, cap_slot, e->gbutton.button);
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        return true;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (dev == BIND_DEV_PAD && e->gaxis.value > 16000 && !other_players_pad(e->gaxis.which) &&
            (e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) {
            cap_dev = -1;
            assign(dev, cap_player, cap_slot,
                   e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? PADB_LTRIGGER : PADB_RTRIGGER);
        }
        return true;  // (sticks: no menu movement while waiting)
    default:
        return false;
    }
}

// ------------------------------------------------------------------ larecomp.ini
bool bind_settings_load(const char *key, const char *val) {
    bool pad = !strncmp(key, "bind_pad_", 9), kb = !strncmp(key, "bind_key_", 9);
    if (!pad && !kb) return false;
    // bind_pad_p2 / p3 / p4 (= custom) and bind_pad_p2_<input>...: that player's own table; any other key: player 1's
    const char *name = key + 9;
    int player = pad ? bind_pad_key_player(key, &name) : 1;
    if (player > 1 && !*name) {
        if (!strcmp(val, "custom")) pad_custom[player - 1] = true;
        return true;
    }
    for (int i = 0; i < (pad ? BIND_PAD_COUNT : BIND_KEY_COUNT); i++) {
        if (strcmp(name, k_ini[i])) continue;
        if (pad) {
            for (int h = 0; h < PADB_HOST_COUNT; h++)
                if (!strcmp(val, k_pad_ini[h])) pad_tables[player - 1][i] = (int16_t)h;
        } else {
            SDL_Scancode sc = strcmp(val, "none") ? SDL_GetScancodeFromName(val) : SDL_SCANCODE_UNKNOWN;
            if (sc != SDL_SCANCODE_UNKNOWN || !strcmp(val, "none")) g_bind_key[i] = (int16_t)sc;
        }
    }
    return true;
}

void bind_settings_save(FILE *f) {
    for (int i = 0; i < BIND_PAD_COUNT; i++)
        if (pad_tables[0][i] != k_def_pad[i] && pad_tables[0][i] >= 0 && pad_tables[0][i] < PADB_HOST_COUNT)
            fprintf(f, "bind_pad_%s = %s\n", k_ini[i], k_pad_ini[pad_tables[0][i]]);
    for (int p = 1; p < BIND_PLAYERS; p++) {  // (a player following player 1: no lines)
        if (!pad_custom[p]) continue;
        fprintf(f, "bind_pad_p%d = custom\n", p + 1);
        for (int i = 0; i < BIND_PAD_COUNT; i++)
            if (pad_tables[p][i] != k_def_pad[i] && pad_tables[p][i] >= 0 && pad_tables[p][i] < PADB_HOST_COUNT)
                fprintf(f, "bind_pad_p%d_%s = %s\n", p + 1, k_ini[i], k_pad_ini[pad_tables[p][i]]);
    }
    for (int i = 0; i < BIND_KEY_COUNT; i++)
        if (g_bind_key[i] != k_def_key[i])
            fprintf(f, "bind_key_%s = %s\n", k_ini[i], g_bind_key[i] ? SDL_GetScancodeName((SDL_Scancode)g_bind_key[i]) : "none");
}
