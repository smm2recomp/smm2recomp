// "Pretend online" for Course World (games/smm2/mods/mod.h): the game's network session logs in without a network,
// and Course World's login sequence gets neutral answers, so its menus open; the Endless Challenge requests then
// go to our server (endless.c). Every other Course World request runs the game's own code, which finds no NEX
// connection and answers "not connected" (status 1), and the game shows its usual message for that screen.
//
// The session (Nintendo's "enl" layer over NEX) is a state machine: cConnectNetwork -> cLogInReady (starts the NEX
// login) -> cLogIn, which is also the logged-in resting state (it waits there for the next command in +0xe4). With
// the mod, cLogInReady goes straight to cLogIn and cLogIn handles the commands without a NEX connection.
//
// Hybrid (hybrid.h: backend nextendo, or local-nex with course_world=mod): the game's own enl / NEX login runs and
// the login sequence's requests go to the backend (mod_answers_login() is false); Course World's other requests stay
// the mod's (mod_hundred()). Two answers are merged: the application config (the backend's, with the Endless
// Challenge switched on) and the player's own record (the backend's, with our Endless bests and unlocks).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mod.h"
#include "ocw.h"
#include "../../../runtime/net/netcfg.h"
#include "../../../runtime/net/nextendo.h"
#include "../../../runtime/net/json.h"

#define STATE_CHANGE MAIN_ADDR(0x8b9320)  // (state machine, new state)

// enl session states (v3.0.3 exec functions)
#define ENL_LOGIN_READY MAIN_ADDR(0x60fe20)
#define ENL_LOGIN MAIN_ADDR(0x6101d0)
enum { ENL_ST_LOGIN = 4, ENL_ST_LOGOUT_ONLY = 0x1b, ENL_ST_LOGOUT = 0x1c };

static void change_state(Cpu *c, uint64_t sm, uint32_t st) {
    c->x[0] = sm;
    c->x[1] = st;
    guest_call(c, STATE_CHANGE);
}

// Hybrid: whether the backend's login has succeeded this session. An error the game shows before it did means the
// login failed: the session then falls back to ours (runtime/net/netcfg.h net_backend_fall_back), so opening Course
// World again works - the mod answers the login and everything else, Network Play is off until the next start (owner,
// 2026-10-23: Course World never depends on Nextendo). An error after a login (a match not found...) changes nothing.
static volatile bool g_backend_logged_in;
bool game_online_error(char *msg, size_t cap) {  // (runtime/game_hooks.c: the game shows an error)
    if (!mod_hybrid() || g_backend_logged_in) return false;
    NetBackend b = net_backend();
    if (!net_backend_fall_back("its login failed")) return false;
    snprintf(msg, cap, "Couldn't log in to %s, so Network Play is off until the next start. Course World, the Endless "
             "Challenge and 100 Mario don't need it: open Course World again.",
             b == NET_BACKEND_NEXTENDO ? "Nextendo Network" : "the online test server");
    return true;
}

static void enl_login_ready(Cpu *c) {
    if (!mod_answers_login()) { guest_call_orig(c, ENL_LOGIN_READY); return; }
    uint64_t self = c->x[0];
    static bool told;  // (a backend set but not used this session: said once, as the game logs in)
    const char *why = net_backend_fallback_why();
    if (why[0] && !told) {
        told = true;
        extern void menu_notice(const char *text, double seconds);
        char t[400];
        snprintf(t, sizeof t, "%s, so Network Play is off until the next start. Course World, the Endless Challenge and "
                 "100 Mario work as usual.", why);
        menu_notice(t, 15);
    }
    MLOG("enl session %llx: logged in (no NEX connection)\n", (unsigned long long)self);
    if (g_mod.log) {
        uint64_t vt = g64(self);
        for (int i = 0x20; i <= 0xb0; i += 8) MLOG("  enl vtable +%#x = %llx\n", i, (unsigned long long)g64(vt + i));
    }
    s8(self + 0x19, 1);  // "logged in" (the session's own query reads this byte; NEX's login callback sets it)
    s8(self + 0x1a, 0);  // no error
    change_state(c, self + 0x30, ENL_ST_LOGIN);
}

// cLogIn, the logged-in state: +0xe4 is the command the game posted (1 log in: done; 0 log out; 0xf: log out and
// stop). Matchmaking commands (3..7, online multiplayer) are not available: they are dropped.
static void enl_login(Cpu *c) {
    uint64_t self = c->x[0];
    if (!mod_answers_login()) {
        guest_call_orig(c, ENL_LOGIN);
        if (g8(self + 0x19) && !g_backend_logged_in) {
            g_backend_logged_in = true;
            MLOG("enl session %llx: logged in to the backend\n", (unsigned long long)self);
        }
        return;
    }
    int32_t cmd = (int32_t)g32(self + 0xe4);
    s8(self + 0x19, 1);
    s8(self + 0x1a, 0);
    switch (cmd) {
    case 0: change_state(c, self + 0x30, ENL_ST_LOGOUT_ONLY); break;
    case 0xf: change_state(c, self + 0x30, ENL_ST_LOGOUT); break;
    case -1: break;
    default:
        MLOG("enl command %d done\n", cmd);
        s32(self + 0xe4, 0xffffffffu);
    }
}

// ---- Course World's login sequence: each state runs one request ("wrapper", this = the network-session object)
// on the network worker and goes on when the result byte it leaves at +0x10 says success. The answers here are
// neutral: no application-config overrides, the player's own user record, an unrestricted name, no Mii costumes,
// no notifications, no event course.

static void answered(Cpu *c, const char *what) {
    MLOG("%s: answered locally\n", what);
    s32(c->x[0] + 0x60, 0);
    c->x[0] = wrapper_ok();
}

#define LOCAL_WRAPPER(name, off, body)                                                           \
    static void name(Cpu *c) {                                                                   \
        if (!mod_answers_login()) { guest_call_orig(c, MAIN_ADDR(off)); return; }                \
        uint64_t no = c->x[0];                                                                   \
        (void)no;                                                                                \
        netobj_lock(c);                                                                          \
        body;                                                                                    \
        netobj_unlock(c);                                                                        \
        answered(c, #name);                                                                      \
    }

// The server's application config: 38 u32 values at +0x737b0 (copied to the UI cache at +0x28). Values 0-3 switch the
// Endless Challenge on per difficulty (> 0; with 0 the game says "This service is not active right now"), value 37
// Course World's Super Worlds (exactly 1; superworld.c); the rest stay 0 (features the mod doesn't serve). Hybrid: the backend's config (Utility.GetIntegerSettings(0): its keys
// 0..37 into the array, Network Play's switches among them) with the Endless Challenge's four and Super Worlds
// switched on, since the mod serves them.
#define APP_CONFIG 0x737b0

static void get_application_config(Cpu *c) {
    uint64_t no = c->x[0];
    if (mod_hybrid()) {
        guest_call_orig(c, MAIN_ADDR(0x14ce2f0));
        if (g32(no + 0x60) == 0 && (uint8_t)c->x[0] == wrapper_ok()) {  // (the game's own request succeeded)
            uint64_t ret = c->x[0];
            netobj_lock(c);
            for (int d = 0; d < 4; d++) s32(no + APP_CONFIG + 4 * d, 1);
            s32(no + APP_CONFIG + 4 * 37, 1);
            netobj_unlock(c);
            c->x[0] = ret;
            LOG("[mod] GetApplicationConfig: the backend's, Endless on\n");
        }
        return;
    }
    if (!mod_answers_login()) { guest_call_orig(c, MAIN_ADDR(0x14ce2f0)); return; }
    netobj_lock(c);
    for (int d = 0; d < 4; d++) s32(no + APP_CONFIG + 4 * d, 1);
    s32(no + APP_CONFIG + 4 * 37, 1);
    netobj_unlock(c);
    answered(c, "get_application_config");
}
LOCAL_WRAPPER(get_user_name_ng_type, 0x14f2cc0, s32(no + 0x141608, 0))  // the name is fine
// (sync_user_profile: below, after the player's own record)
LOCAL_WRAPPER(get_mii_clothes, 0x14d8360, s32(no + 0x73a58, 0))      // no costume rewards (ring count)
LOCAL_WRAPPER(login_check, 0x14d7d20, {})
LOCAL_WRAPPER(update_last_login, 0x14d7e00, {})
LOCAL_WRAPPER(post_activity_quest, 0x14f0b30, {})
LOCAL_WRAPPER(get_new_notification, 0x14edee0, { s8(no + 0xeac12, 0); s8(no + 0xeac14, 0); })
LOCAL_WRAPPER(get_event_course_status, 0x14f3ab0, {
    s64(no + 0x1a0810, 0); s8(no + 0x1a0818, 0); s64(no + 0x1a0820, 0); s8(no + 0x1a0828, 0);
})
// Ninji Speedruns' event count (+0x1a0808): none, so the screen shows SMM2's own "No active speedruns" instead of a
// communication error (TGRCode has past events: ninji_info, a later step)
LOCAL_WRAPPER(get_event_course_count, 0x14f3980, s32(no + 0x1a0808, 0))
// ... and its list of events (a ring of 0xf38-byte entries at +0x141610: head and count at +0x14161c): empty
LOCAL_WRAPPER(get_event_courses, 0x14f3770, s64(no + 0x14161c, 0))

// The player's own user record (GetUsers for the signed-in player): one entry of 0x1f0 bytes in the ring at +0x68
// (entries, +0x70 capacity, +0x74 head, +0x78 count). Name and best Endless clears come from our server. The entry
// is reset the way the game's own converter resets one (strings empty, statistics 0, an invalid Mii, which the game
// replaces with a default Mii), then filled.
#define USER_RING 0x68
#define USER_ENTRY 0x1f0

static void put_utf16(uint64_t buf, uint32_t cap, const char *s) {
    if (!buf || !cap) return;
    uint32_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && n + 1 < cap; ) {
        uint32_t cp = *p++;
        if (cp >= 0xC0) {  // UTF-8 -> code point (BMP only; anything else becomes '?')
            int extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : 1;
            cp &= 0x3F >> extra;
            while (extra-- && (*p & 0xC0) == 0x80) cp = cp << 6 | (*p++ & 0x3F);
            if (cp > 0xFFFF) cp = '?';
        }
        s16(buf + 2 * n++, (uint16_t)cp);
    }
    s16(buf + 2 * n, 0);
}

void user_entry_reset(uint64_t e) {
    s16(g64(e + 0x08), 0);  // name (UTF-16)
    s16(g64(e + 0x30), 0);  // second UTF-16 string
    s64(e + 0x58, 0);       // player id
    s8(g64(e + 0x68), 0);   // UTF-8 string (country)
    s16(e + 0x80, 0xff);
    for (uint64_t o = 0x88; o < 0x168; o += 8) s64(e + o, 0);
    s8(e + 0x168, 0);
    static const uint8_t flags[29] = {1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0};
    for (int i = 0; i < 29; i++) s8(e + 0x169 + i, flags[i]);
    for (uint64_t o = 0x188; o < 0x1a8; o += 8) s64(e + o, 0);
    s8(g64(e + 0x1b0), 0);
    s32(e + 0x1e8, 0);
    s8(e + 0x1ec, 0);
}

uint64_t mod_player_pid(void);  // endless.c: the fake player id the overrides use for the signed-in account
extern void mod_account_info(char *name, size_t cap, uint32_t best[4]);

// ---- hybrid: the player's name. A save whose Maker avatar has no name (empty or only spaces, as one made without
// the console's Mii editor) would have the login's SyncUserProfile send a blank name, and the backend would show
// the player as nameless. The backend's account name stands in: nextendo's user name, or (local-nex) the name in
// the backend's record of the player. A name the player set is never replaced, and the save is not written.
#define NAME_MAX16 11  // UTF-16 units of a Maker name with its terminator (10 characters, as the game sends)

static bool blank_utf16(uint64_t buf, uint32_t cap) {
    for (uint32_t i = 0; buf && i < cap; i++) {
        uint16_t ch = *(volatile uint16_t *)(uintptr_t)(buf + 2 * i);
        if (!ch) return true;
        if (ch != ' ' && ch != 0x3000) return false;  // (ASCII and ideographic spaces)
    }
    return true;
}

static uint16_t record_name[NAME_MAX16];  // local-nex: the backend's name for the player (from its record)

// The backend's name for the player as UTF-16 into buf (cap units); false when there is none.
static bool backend_name(uint64_t buf, uint32_t cap) {
    if (!buf || cap < 2) return false;
    if (cap > NAME_MAX16) cap = NAME_MAX16;
    char nx[64];
    if (net_backend() == NET_BACKEND_NEXTENDO) {
        if (!nextendo_username(nx, sizeof nx) || !nx[0]) return false;
        put_utf16(buf, cap, nx);
        return true;
    }
    if (!record_name[0]) return false;
    uint32_t i = 0;
    for (; i + 1 < cap && record_name[i]; i++) s16(buf + 2 * i, record_name[i]);
    s16(buf + 2 * i, 0);
    return true;
}

// Hybrid: the backend's record of the player (its name, Mii, maker code, registration and player id) stays as the
// game's own request wrote it; our server's Endless bests go into it (mod_account_info also sets the 100 Mario
// unlocks), and its player id becomes the player's own id in the mod's answers (mod_set_backend_pid). Neither our
// server's account name nor a Maker avatar name is written: the name is the backend's (SyncUserProfile, the next
// request of the login, sends it the player's own avatar name from the save).
static void get_my_user_hybrid(Cpu *c) {
    uint64_t no = c->x[0];
    guest_call_orig(c, MAIN_ADDR(0x14d5cd0));
    uint64_t ret = c->x[0];
    // (the game's own request succeeded and wrote at least one entry: the first is the player's, the ring's head is 0)
    if (g32(no + 0x60) != 0 || (uint8_t)ret != wrapper_ok() || (int32_t)g32(no + USER_RING + 0x10) < 1) return;
    char name[64] = "";
    uint32_t best[4] = {0};
    mod_account_info(name, sizeof name, best);  // (our server; before the lock: it may take a while)
    bool named = false;
    netobj_lock(c);
    uint64_t e = g64(no + USER_RING);
    if (e) {
        for (int d = 0; d < 4; d++) s32(e + 0x114 + 4 * d, best[d]);
        mod_set_backend_pid(g64(e + 0x58));
        // the name (entry +0x30, the converter's copy of the UserInfo's name; +0x38 its capacity): kept for
        // SyncUserProfile when it is a real one, else (nextendo) the account's user name, so the card shows it
        uint64_t nb = g64(e + 0x30);
        uint32_t cap = g32(e + 0x38);
        if (!blank_utf16(nb, cap)) {
            for (uint32_t i = 0; i < NAME_MAX16; i++)
                record_name[i] = i + 1 < NAME_MAX16 && i < cap ? *(volatile uint16_t *)(uintptr_t)(nb + 2 * i) : 0;
        } else if (net_backend() == NET_BACKEND_NEXTENDO) {
            named = backend_name(nb, cap);
        }
    }
    netobj_unlock(c);
    (void)mod_player_pid();  // (the own maker ID from our server, before any list is answered)
    if (named) LOG("[mod] GetUsers (own): the backend's record had no name; the Nextendo user name stands in\n");
    c->x[0] = ret;
    LOG("[mod] GetUsers (own): the backend's record, our Endless bests %u/%u/%u/%u\n", best[0], best[1], best[2],
        best[3]);
}
void mii_default_charinfo(uint8_t out[0x58], const char *nickname);  // runtime/services/mii.c

static void get_my_user(Cpu *c) {
    if (mod_hybrid()) { get_my_user_hybrid(c); return; }
    if (!mod_answers_login()) {
        uint64_t self = c->x[0];
        guest_call_orig(c, MAIN_ADDR(0x14d5cd0));
        ocw_fill_play_stats(c, self, c->x[0]);  // (OCW: the Play History from its REST API; else nothing)
        return;
    }
    uint64_t no = c->x[0];
    char name[64];
    snprintf(name, sizeof name, "%s", getenv("RECOMP_USER_NAME") ? getenv("RECOMP_USER_NAME") : "Player");
    uint32_t best[4] = {0};
    mod_account_info(name, sizeof name, best);
    char world[64] = "";
    Json *me = srv_call("GET", "/api/me", NULL, NULL);
    snprintf(world, sizeof world, "%s", json_str(json_get(me, "super_world_id"), ""));
    json_free(me);
    // the courses the player uploaded and the most they may (My Info's "Courses uploaded"; world.c)
    extern const char *mod_own_maker_id(void);
    extern void mod_upload_counts(const char *maker_id, uint32_t *count, uint32_t *limit);
    uint32_t uploaded, limit;
    (void)mod_player_pid();  // (the own maker ID, from /api/me once)
    mod_upload_counts(mod_own_maker_id(), &uploaded, &limit);
    netobj_lock(c);
    s64(no + USER_RING + 0xc, 0);  // head 0, count 0
    if (g32(no + USER_RING + 8) > 0) {
        uint64_t e = g64(no + USER_RING);
        user_entry_reset(e);
        uint64_t pid = mod_player_pid();
        const char *mid = mod_own_maker_id();
        char code[12] = "";
        if (strlen(mid) == 11) snprintf(code, sizeof code, "%.3s%.3s%.3s", mid, mid + 4, mid + 8);
        put_utf16(g64(e + 0x08), g32(e + 0x10), code);  // maker code (the converter: NEX code -> +0x08)
        put_utf16(g64(e + 0x30), g32(e + 0x38), name);  // name
        s64(e + 0x58, pid);
        // The Maker avatar: a valid Mii (the default one, named after the account). Without one Course World asks
        // for a new avatar, which needs the console's Mii editor.
        mii_default_charinfo((uint8_t *)(uintptr_t)(e + 0x9c), name);
        for (int d = 0; d < 4; d++) s32(e + 0x114 + 4 * d, best[d]);
        s32(e + 0x190, uploaded);  // (world.c fill_user)
        s32(e + 0x194, limit);
        s8(e + 0x1ec, 1);  // a registered Course World user (Course World's top screen checks it, with the id)
        // the player's uploaded Super World (+0x1b0, UTF-8; the upload scene reads it back after an upload)
        if (*world && g64(e + 0x1b0) && g32(e + 0x1b8) > strlen(world))
            memcpy((void *)(uintptr_t)g64(e + 0x1b0), world, strlen(world) + 1);
        s32(no + USER_RING + 0x10, 1);
    }
    netobj_unlock(c);
    // ... and, as the original asks GetWorldMap for it, its detail record (superworld.c)
    if (*world) {
        extern bool superworld_fill_detail(Cpu * c, uint64_t no, const char *id);
        superworld_fill_detail(c, no, world);
    }
    // The Maker avatar's name in the player's settings (an empty one sends Course World to "Create Maker Avatar",
    // which needs the console's Mii editor): the account's name when there is none yet.
    uint64_t settings = g64(MAIN_ADDR(0x2a57d60));
    if (settings && g64(settings + 0x2b0) && *(volatile uint16_t *)(uintptr_t)g64(settings + 0x2b0) == 0) {
        put_utf16(g64(settings + 0x2b0), 11, name);
        LOG("[mod] Maker avatar name set to \"%s\" (default Mii)\n", name);
    }
    // ... and its Mii (the save's user settings, *(main+0x2a39088) +0x20: CharInfo at +0x60). The login's
    // SyncUserProfile (main+0x14b09e0) copies it into the profile the menus draw the avatar from (the icon at the
    // top left); without one the icon stays a white box until a maker page brings the record's Mii.
    uint64_t user_settings = g64(MAIN_ADDR(0x2a39088)) ? g64(g64(MAIN_ADDR(0x2a39088)) + 0x20) : 0;
    if (user_settings) {
        uint64_t create_id[2];
        memcpy(create_id, (const void *)(uintptr_t)(user_settings + 0x60), 16);
        if (!create_id[0] && !create_id[1]) {
            mii_default_charinfo((uint8_t *)(uintptr_t)(user_settings + 0x60), name);
            LOG("[mod] Maker avatar Mii set to the default Mii\n");
        }
    }
    MLOG("GetUsers (own): %s, best %u/%u/%u/%u, courses uploaded %u of %u\n", name, best[0], best[1], best[2], best[3],
         uploaded, limit);
    s32(no + 0x60, 0);
    c->x[0] = wrapper_ok();
}

// SyncUserProfile (the login's profile upload: name, Mii, region; this = NetObj). Its name is the UTF-16 string at
// +0x738e0 (buffer +0x738e8, capacity +0x738f0; the request converts 10 characters of it). Hybrid: a blank one is
// the backend's name for the player (above) for this request only; the original then sends it.
#define SYNC_NAME 0x738e0

static void sync_user_profile(Cpu *c) {
    uint64_t no = c->x[0];
    if (mod_hybrid()) {
        netobj_lock(c);
        uint64_t buf = g64(no + SYNC_NAME + 8);
        uint32_t cap = g32(no + SYNC_NAME + 0x10);
        bool filled = blank_utf16(buf, cap < NAME_MAX16 ? cap : NAME_MAX16) && backend_name(buf, cap);
        netobj_unlock(c);
        if (filled) LOG("[mod] SyncUserProfile: the save's Maker name is blank; sending the backend's name\n");
        guest_call_orig(c, MAIN_ADDR(0x14cef40));
        return;
    }
    if (!mod_answers_login()) { guest_call_orig(c, MAIN_ADDR(0x14cef40)); return; }
    answered(c, "sync_user_profile");
}

// RECOMP_MOD_TRACE=1: log each state entered by the machines above (their enter functions, reached through pointers)
static const struct { uint64_t off; const char *name; } traced[] = {
    {0x1532060, "CW login cIdle"}, {0x1532080, "CW login cWaitStartApplet"},
    {0x15320b0, "CW login cCheckParentalControl"}, {0x1532140, "CW login cLogin"},
    {0x1532260, "CW login cGetApplicationSetting"}, {0x15323d0, "CW login cGetMyUserInfo"},
    {0x15325e0, "CW login cGetUserNameNgType"}, {0x1532750, "CW login cSyncUserProfile"},
    {0x15328e0, "CW login cToSaveUserSetting"}, {0x1532910, "CW login cSaveUserSetting"},
    {0x1533010, "CW login cGetMiiClothes"}, {0x1533270, "CW login cLoginCheck"},
    {0x15333c0, "CW login cUpdateLastLoginInfo"}, {0x15334f0, "CW login cPostActivityQuest"},
    {0x1533620, "CW login cGetNewNotification"}, {0x15337e0, "CW login cGetEventCourseStatus"},
    {0x14940d0, "NetAppTask cIdle"}, {0x14941e0, "NetAppTask cReady"}, {0x1494250, "NetAppTask cLogIn"},
    {0x14942f0, "NetAppTask cEnlTask"}, {0x14945d0, "NetAppTask cError"}, {0x1494640, "NetAppTask cGetFriendList"},
};

static void trace_state(Cpu *c) {
    uint64_t pc = c->pc;
    for (size_t i = 0; i < sizeof traced / sizeof *traced; i++)
        if (MAIN_ADDR(traced[i].off) == pc) { LOG("[mod] state: %s\n", traced[i].name); break; }
    guest_call_orig(c, pc);
}

// Every Course World request function of v3.0.3 (the callers of the session's "connected" check). With
// RECOMP_MOD_TRACE=1 the ones the mod doesn't answer are logged with their result (status word +0x60).
static const uint32_t all_wrappers[] = {
    0x14ce500, 0x14cef40, 0x14d02d0, 0x14d1820, 0x14d2f00, 0x14d5cd0, 0x14d74a0, 0x14d7780, 0x14d7b00, 0x14d7c30,
    0x14d7d20, 0x14d7e00, 0x14d7f50, 0x14d8360, 0x14d8550, 0x14dbae0, 0x14dbeb0, 0x14dc0d0, 0x14de010, 0x14de3f0,
    0x14e0c50, 0x14e0d40, 0x14e0e70, 0x14e2530, 0x14e2cf0, 0x14e30e0, 0x14e3f90, 0x14e5110, 0x14e5440, 0x14e55a0,
    0x14e58b0, 0x14e5c10, 0x14e5f20, 0x14e6060, 0x14e6b90, 0x14e6da0, 0x14e7240, 0x14e73d0, 0x14e7560, 0x14e7730,
    0x14e7af0, 0x14e8950, 0x14e8c60, 0x14e8d60, 0x14e8ec0, 0x14e96b0, 0x14e9c10, 0x14eaa20, 0x14eadb0, 0x14eaf30,
    0x14ebc60, 0x14ec0d0, 0x14ec2d0, 0x14ece20, 0x14ecf30, 0x14ed120, 0x14ed360, 0x14ed4a0, 0x14ed5e0, 0x14ed750,
    0x14eddc0, 0x14edee0, 0x14ee050, 0x14ef610, 0x14ef720, 0x14f0090, 0x14f0470, 0x14f0b30, 0x14f1210, 0x14f1330,
    0x14f15c0, 0x14f17a0, 0x14f1920, 0x14f1cf0, 0x14f22e0, 0x14f2910, 0x14f2cc0, 0x14f2de0, 0x14f3770, 0x14f3980,
    0x14f3ab0, 0x14f3cb0, 0x14f3da0, 0x14f4260, 0x14f4710, 0x14f4da0, 0x14f5940, 0x14f6100, 0x14f6600, 0x14f79a0,
    0x14f7c30, 0x14f7d10, 0x14f8260, 0x14f84d0, 0x14f8850,
};

static void trace_wrapper(Cpu *c) {
    uint64_t pc = c->pc, self = c->x[0];
    guest_call_orig(c, pc);
    LOG("[mod] wrapper main+%#llx (not answered by the mod): status %u\n", (unsigned long long)(pc - MAIN_ADDR(0)),
        g32(self + 0x60));
}

void online_register(void) {
    hle_override(ENL_LOGIN_READY, enl_login_ready, "mod: enl cLogInReady");
    hle_override(ENL_LOGIN, enl_login, "mod: enl cLogIn");
    hle_override(MAIN_ADDR(0x14ce2f0), get_application_config, "mod: GetApplicationConfig");
    hle_override(MAIN_ADDR(0x14d5cd0), get_my_user, "mod: GetUsers (own)");
    hle_override(MAIN_ADDR(0x14f2cc0), get_user_name_ng_type, "mod: GetUserNameNgType");
    hle_override(MAIN_ADDR(0x14cef40), sync_user_profile, "mod: SyncUserProfile");
    hle_override(MAIN_ADDR(0x14d8360), get_mii_clothes, "mod: GetMiiClothes");
    hle_override(MAIN_ADDR(0x14d7d20), login_check, "mod: LoginCheck");
    hle_override(MAIN_ADDR(0x14d7e00), update_last_login, "mod: UpdateLastLoginInfo");
    hle_override(MAIN_ADDR(0x14f0b30), post_activity_quest, "mod: PostActivityQuest");
    hle_override(MAIN_ADDR(0x14edee0), get_new_notification, "mod: GetNewNotification");
    hle_override(MAIN_ADDR(0x14f3ab0), get_event_course_status, "mod: GetEventCourseStatus");
    hle_override(MAIN_ADDR(0x14f3980), get_event_course_count, "mod: Ninji event count");
    hle_override(MAIN_ADDR(0x14f3770), get_event_courses, "mod: Ninji events");
    if (getenv("RECOMP_MOD_TRACE")) {
        for (size_t i = 0; i < sizeof traced / sizeof *traced; i++)
            hle_override(MAIN_ADDR(traced[i].off), trace_state, traced[i].name);
        extern void endless_register(void);
        endless_register();  // (the answered ones first: the first override of an address wins)
        for (size_t i = 0; i < sizeof all_wrappers / sizeof *all_wrappers; i++)
            hle_override(MAIN_ADDR(all_wrappers[i]), trace_wrapper, "mod: trace wrapper");
    }
}
