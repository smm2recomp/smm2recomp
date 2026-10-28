// Course World against our server (games/smm2/mods/mod.h): the course lists, course details, makers, likes,
// comments, uploads and leaderboards that the game would ask Nintendo's servers for. Each request function
// ("wrapper", this = the network-session object) is answered from core/crates/server's /api routes, writing what
// the game's own converters would write from a NEX answer. Names of the requests (DataStore SMM2 methods) come
// from the client vtable slot each wrapper calls (v3.0.3).
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mod.h"
#include "ocw.h"
#include "../../../runtime/net/json.h"

void mod_fill_course(uint64_t e, Json *m, uint32_t data_id, uint32_t difficulty);  // endless.c
bool mod_maker_id(uint64_t pid, char out[16]);
const char *mod_own_maker_id(void);
uint64_t mod_player_pid(void);

#define NO_STATUS 0x60
#define NO_COURSES 0x30780       // course list ring: entries (0x548 bytes), +8 capacity, +0xc head, +0x10 count
#define COURSE_ENTRY 0x548
#define NO_PAGE 0x737a8          // u32: the list page (the request asks for 100 from page * 100)
#define NO_MORE 0x737ac          // u8: the answer: a next page follows
#define NO_SEARCH_TYPE 0x746e8   // u32: which search
#define NO_SEARCH_DIFF 0x746ec   // u32: preferred difficulty (Popular)
#define NO_SEARCH_PID 0x746f0    // u64: the maker (posted by / liked by / played by ...)
#define NO_ADV_DIFF 0x746fc      // u32: detailed search: difficulty, u8 style +0x74704, u8 theme +0x74705, sort +0x7470c
#define NO_ADV_STYLE 0x74704
#define NO_ADV_THEME 0x74705
#define NO_ADV_SORT 0x7470c

static void finish(Cpu *c, uint64_t no, bool ok, uint32_t status) {
    s32(no + NO_STATUS, ok ? 0 : status);
    c->x[0] = ok ? wrapper_ok() : wrapper_fail();
}

static const char *const STYLES[5] = {"M1", "M3", "MW", "WU", "3W"};

// The difficulty band of a course from the server's difficulty value (0-100, its fail rate model): Easy, Normal,
// Expert, Super Expert as the game shows them.
static uint32_t band(Json *m) {
    uint32_t v = (uint32_t)json_num(json_get(m, "difficulty"), 50);
    return v < 35 ? 0 : v < 75 ? 1 : v < 96 ? 2 : 3;
}

// The search types of the course-list wrapper (main+0x14d8550, NetObj +0x746e8) and the DataStore method each
// asks for: 0 PickUp (84), 1 PointRanking (71, "Popular", by difficulty), 2 Latest (73, "New"), 3 FolloweePostedBy
// (82), 4 Advanced (72, detailed search), 5 TermsRanking (83), 6 PostedBy (74), 7 PositiveRatedBy (75), 8 BestTime
// (81), 9 FirstClear (80), 10 PlayedBy (76), 11-13 Battle (77, 78), 14 EndlessMode (79, endless.c).
static const char *const SEARCH_NAMES[14] = {"pick up", "popular", "new", "followed makers", "detailed search",
                                             "terms ranking", "posted by", "liked by", "world records",
                                             "first clears", "played by", "versus", "versus", "versus"};

// GET /api/makers/<maker id>, only for real maker ids (XXX-XXX-XXX: courses from TGRCode's archive may carry a maker
// name instead, which the server can't look up), and once per unknown id (a 404 is remembered): the course lists
// ask for every maker on them, and each miss used to cost a request.
static Json *maker_get(const char *id) {
    static char unknown[128][16];
    static int nunknown;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    if (!id || strlen(id) != 11 || id[3] != '-' || id[7] != '-') return NULL;
    for (int i = 0; i < 11; i++)
        if (i != 3 && i != 7 && !((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'A' && id[i] <= 'Z'))) return NULL;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < nunknown && i < 128; i++)
        if (!strcmp(unknown[i], id)) return pthread_mutex_unlock(&lock), NULL;
    pthread_mutex_unlock(&lock);
    char path[64];
    snprintf(path, sizeof path, "/api/makers/%s", id);
    int status = 0;
    Json *j = srv_call("GET", path, NULL, &status);
    if (!j && status == 404) {
        pthread_mutex_lock(&lock);
        snprintf(unknown[nunknown++ % 128], 16, "%s", id);
        pthread_mutex_unlock(&lock);
    }
    return j;
}

void world_search(Cpu *c, uint64_t no, uint32_t type) {
    uint32_t page = g32(no + NO_PAGE), want_band = 4, want_style = 5;
    char path[256], id[16] = "";
    const char *sort = "popular", *player_list = NULL;
    bool by_maker = false, none = false;
    switch (type) {
    case 0: sort = "hot"; break;                                // pick up (Hot): popular, the latest uploads first
    case 5: break;                                              // terms ranking: popular
    case 1: want_band = g32(no + NO_SEARCH_DIFF); break;        // popular, by difficulty
    case 2: sort = "new"; break;                                // new
    case 4:                                                     // detailed search
        want_band = g32(no + NO_ADV_DIFF);
        want_style = g8(no + NO_ADV_STYLE);
        if (g32(no + NO_ADV_SORT) == 1) sort = "new";
        break;
    case 6: by_maker = true; break;                             // a maker's courses
    case 8: case 9: by_maker = true; break;                     // records / first clears by a maker: filtered below
    case 7: player_list = "liked"; break;                       // liked by a player
    case 10: player_list = "played"; break;                     // played by a player
    default: none = true; break;                                // followed makers, versus: none kept
    }
    Json *j = NULL;
    // (a player id that isn't one of ours, which all start 0x4 / 0x5: the console's own account, as the Super World
    // upload's check of the player's posted courses sends it)
    uint64_t search_pid = g64(no + NO_SEARCH_PID);
    if (by_maker && !mod_maker_id(search_pid, id) && (search_pid >> 60) != 4 && (search_pid >> 60) != 5) {
        mod_player_pid();
        snprintf(id, sizeof id, "%s", mod_own_maker_id());
    }
    if (by_maker) {
        if (*id) j = maker_get(id);
    } else if (player_list) {
        if (mod_maker_id(g64(no + NO_SEARCH_PID), id) && page == 0) {
            snprintf(path, sizeof path, "/api/makers/%s/%s", id, player_list);
            j = srv_call("GET", path, NULL, NULL);
        }
    } else if (!none) {
        snprintf(path, sizeof path, "/api/courses?sort=%s&offset=%u&limit=100%s%s", sort, page * 100,
                 want_style < 5 ? "&style=" : "", want_style < 5 ? STYLES[want_style] : "");
        j = srv_call("GET", path, NULL, NULL);
    }
    Json *list = json_get(j, "courses");
    netobj_lock(c);
    uint64_t ring = no + NO_COURSES, entries = g64(ring);
    uint32_t cap = g32(ring + 8), count = 0;
    s64(ring + 0xc, 0);
    for (size_t i = 0; i < json_len(list) && count < cap && entries; i++) {
        Json *m = json_at(list, i);
        uint32_t data_id;
        if (!code_to_data_id(json_str(json_get(m, "code"), ""), &data_id)) continue;
        if (want_band < 4 && band(m) != want_band) continue;
        if (type == 8 && strcmp(json_str(json_get(m, "record_holder"), ""), id)) continue;
        if (type == 9 && strcmp(json_str(json_get(m, "first_clear"), ""), id)) continue;
        mod_fill_course(entries + (uint64_t)COURSE_ENTRY * count++, m, data_id, band(m));
    }
    s32(ring + 0x10, count);
    // whether a next page follows (the search's own out-flag, which the original passes to the NEX call): the server
    // answered a whole page. Detailed Search's page arrows go by it [obs: never set, they stayed grey]; Hot, Popular
    // and New keep theirs grey with it set [obs] (something else limits them)
    s8(no + NO_MORE, !none && !by_maker && !player_list && json_len(list) >= 100);
    netobj_unlock(c);
    LOG("[mod] course list (%s%s%s, page %u): %u courses\n", type < 14 ? SEARCH_NAMES[type] : "?", id[0] ? " " : "",
        id, page, count);
    bool ok = none || by_maker || player_list || j;
    json_free(j);
    finish(c, no, ok, 2);
}

// ---- helpers

static void put_utf16(uint64_t buf, uint32_t cap, const char *s) {
    if (!buf || !cap) return;
    uint32_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && n + 1 < cap;) {
        uint32_t cp = *p++;
        if (cp >= 0xC0) {
            int extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : 1;
            cp &= 0x3F >> extra;
            while (extra-- && (*p & 0xC0) == 0x80) cp = cp << 6 | (*p++ & 0x3F);
            if (cp > 0xFFFF) cp = '?';
        }
        s16(buf + 2 * n++, (uint16_t)cp);
    }
    s16(buf + 2 * n, 0);
}

static void get_utf16(uint64_t buf, char *out, size_t cap) {  // (ASCII is all a code or an id needs)
    size_t n = 0;
    for (uint32_t i = 0; buf && n + 1 < cap; i++) {
        uint16_t ch = *(volatile uint16_t *)(uintptr_t)(buf + 2 * i);
        if (!ch) break;
        out[n++] = ch < 0x80 ? (char)ch : '?';
    }
    out[n] = 0;
}

static void course_code_of(uint64_t data_id, char out[12]) { data_id_to_code((uint32_t)data_id, out); }

static bool post_ok(const char *path, const char *body) {
    int st = 0;
    Json *j = srv_call("POST", path, body, &st);
    json_free(j);
    return j != NULL;
}

// ---- users (makers): the user record (0x1f0 bytes) as the game's converter (main+0x14d0a60) fills it from a NEX
// UserInfo: maker code +0x08, name +0x30 (both UTF-16, the code without dashes), player id +0x58, Mii +0x9c, play
// stats +0xfc.. (plays, clears, attempts, deaths), Endless best clears +0x114 + 4 * difficulty, maker stats +0x124
// (likes received) / +0x128 (maker points), versus / co-op +0x130.., the courses uploaded +0x190 and the most the
// maker may upload +0x194 (the NEX record's map +0x1a8, keys 0 / 1; My Info's "Courses uploaded" header shows the
// page's record's, profile +0x2264c0 + 0x190 / +0x194, main+0x1aa46a0), "valid" +0x1ec.
void user_entry_reset(uint64_t e);                                     // online.c
void mii_default_charinfo(uint8_t out[0x58], const char *nickname);  // runtime/services/mii.c
uint64_t mod_maker_pid(const char *maker_id, const char *name);
uint64_t mod_player_pid(void);
#define USER_RING 0x68
#define USER_ENTRY 0x1f0

// The maker's record from GET /api/makers/<id> (name, courses -> likes and maker points); NULL: unknown maker.
static Json *maker_profile(const char *maker_id) {
    return maker_get(maker_id);
}

// The maker's courses on our server and the most they may upload (the server's `upload_limit`, at least the count)
static void upload_counts(Json *prof, uint32_t *count, uint32_t *limit) {
    *count = (uint32_t)json_len(json_get(prof, "courses"));
    *limit = (uint32_t)json_num(json_get(prof, "upload_limit"), 100);
    if (*limit < *count) *limit = *count;
}

void mod_upload_counts(const char *maker_id, uint32_t *count, uint32_t *limit) {  // (online.c: the player's own)
    Json *prof = maker_profile(maker_id);
    upload_counts(prof, count, limit);
    json_free(prof);
}

static void fill_user(uint64_t e, uint64_t pid, const char *maker_id, const char *name_hint, Json *prof) {
    const char *name = json_str(json_get(json_get(prof, "maker"), "name"), name_hint && *name_hint ? name_hint : "Maker");
    uint32_t likes = 0, points = 0;
    Json *courses = json_get(prof, "courses");
    for (size_t i = 0; i < json_len(courses); i++) {
        int32_t l = (int32_t)json_num(json_get(json_at(courses, i), "likes"), 0);
        int32_t b = (int32_t)json_num(json_get(json_at(courses, i), "boos"), 0);
        likes += (uint32_t)l;
        points += l > b ? (uint32_t)(l - b) : 0;
    }
    char code[12] = "";
    if (maker_id) snprintf(code, sizeof code, "%.3s%.3s%.3s", maker_id, maker_id + 4, maker_id + 8);
    user_entry_reset(e);
    put_utf16(g64(e + 0x08), g32(e + 0x10), code);  // (the converter: NEX maker code -> +0x08, name -> +0x30)
    put_utf16(g64(e + 0x30), g32(e + 0x38), name);
    s64(e + 0x58, pid);
    s8(e + 0x98, 1);  // comments on (NEX UserInfo +0xf1; a course's record +0x830 -> course global +0x860, below)
    mii_default_charinfo((uint8_t *)(uintptr_t)(e + 0x9c), name);
    s32(e + 0x124, likes);
    s32(e + 0x128, points);
    uint32_t uploaded, limit;
    upload_counts(prof, &uploaded, &limit);
    s32(e + 0x190, uploaded);
    s32(e + 0x194, limit);
    const char *world = json_str(json_get(prof, "super_world_id"), "");  // the maker's Super World (UTF-8, +0x1b0)
    if (*world && g64(e + 0x1b0) && g32(e + 0x1b8) > strlen(world))
        memcpy((void *)(uintptr_t)g64(e + 0x1b0), world, strlen(world) + 1);
    s8(e + 0x1ec, 1);
}

// Users by player id into the user ring: a list of ids (ring at +0x72b10: entries, +8 capacity, +0xc head, +0x10
// count; GetUsers main+0x14d02d0 / +0x14d7780) or the one id at +0x73a30 (the maker page, main+0x14d2f00). At most
// MAX_USERS (a Super World list names up to 50 owners). `world`: the first maker's Super World id ("" none), if
// wanted.
#define MAX_USERS 64
static void users_answer(Cpu *c, uint64_t no, const uint64_t *pids, uint32_t n, const char *what, char world[64]) {
    char ids[MAX_USERS][16], names[MAX_USERS][48];
    uint32_t k = 0;
    for (uint32_t i = 0; i < n && k < MAX_USERS; i++) {
        if (!pids[i]) continue;
        mod_maker_id(pids[i], ids[k]);
        extern void mod_maker_name(uint64_t pid, char *out, size_t cap);
        mod_maker_name(pids[i], names[k], sizeof names[k]);
        k++;
    }
    Json *prof[MAX_USERS] = {0};
    for (uint32_t i = 0; i < k; i++) prof[i] = maker_profile(ids[i]);  // (outside the lock: HTTP)
    if (world) snprintf(world, 64, "%s", k ? json_str(json_get(prof[0], "super_world_id"), "") : "");
    netobj_lock(c);
    uint64_t out = no + USER_RING;
    uint32_t cap = g32(out + 8), m = 0;
    s64(out + 0xc, 0);
    for (uint32_t i = 0, j = 0; i < n && m < cap && j < k; i++) {
        if (!pids[i]) continue;
        fill_user(g64(out) + (uint64_t)USER_ENTRY * m++, pids[i], ids[j], names[j], prof[j]);
        j++;
    }
    s32(out + 0x10, m);
    netobj_unlock(c);
    for (uint32_t i = 0; i < k; i++) json_free(prof[i]);
    MLOG("%s: %u users\n", what, m);
    finish(c, no, true, 0);
}

static uint32_t pid_list(uint64_t no, uint64_t pids[MAX_USERS]) {
    uint64_t req = no + 0x72b10;
    uint32_t rcap = g32(req + 8), rhead = g32(req + 0xc), rcount = g32(req + 0x10), n = 0;
    for (uint32_t i = 0; i < rcount && rcap && n < MAX_USERS; i++)
        pids[n++] = g64(g64(req) + 8ull * ((rhead + i) % rcap));
    return n;
}

static void get_users_list(Cpu *c) {  // main+0x14d02d0 and +0x14d7780 (GetUsers for listed players)
    if (!mod_hundred()) { guest_call_orig(c, c->pc); return; }
    uint64_t no = c->x[0], pids[MAX_USERS];
    netobj_lock(c);
    uint32_t n = pid_list(no, pids);
    netobj_unlock(c);
    users_answer(c, no, pids, n, "GetUsers", NULL);
}

// main+0x14d2f00: a maker's page: GetUsers, and when the maker has a Super World (our server's players' uploads),
// GetWorldMap into the detail record as the original does (superworld.c)
bool superworld_fill_detail(Cpu *c, uint64_t no, const char *id);
static void get_user_page(Cpu *c) {
    if (!mod_hundred()) {
        uint64_t self = c->x[0];
        guest_call_orig(c, c->pc);
        ocw_fill_play_stats(c, self, c->x[0]);  // (OCW: the Play History from its REST API; else nothing)
        return;
    }
    uint64_t no = c->x[0], pid = g64(no + 0x73a30);
    char world[64];
    users_answer(c, no, &pid, 1, "maker page", world);
    if (*world) superworld_fill_detail(c, no, world);
}

// main+0x14d74a0: GetUsers (option 0x40) for the user at +0x73a30, only the NEX record's map +0x190 (keys 0 / 1 to
// +0x73a3c / +0x73a40; main+0x1513b60 copies them to the user records' +0x188 / +0x18c, meaning unknown: not the
// "Courses uploaded" header, which reads +0x190 / +0x194 [obs]): 0
static void get_user_counts(Cpu *c) {
    if (!mod_hundred()) { guest_call_orig(c, c->pc); return; }
    uint64_t no = c->x[0];
    netobj_lock(c);
    s64(no + 0x73a3c, 0);
    netobj_unlock(c);
    finish(c, no, true, 0);
}

// ---- leaderboards (SearchUsers*, main+0x14d1820): kind +0x73a14: 0 maker points (50), 1 Endless (51, difficulty
// +0x73a18, when +0x73a1c is 3..6) or the clear ranking (57, +0x73a20), 2 versus (52), 4 terms (58), 5 official
// (147), 6 friends (167), 7..9 players of a course (+0x73a28). Our server: /api/rankings/<kind>.
static void search_users(Cpu *c) {
    if (!mod_hundred()) { guest_call_orig(c, c->pc); return; }
    uint64_t no = c->x[0];
    static const char *const DIFF[4] = {"easy", "normal", "expert", "super_expert"};
    uint32_t kind = g32(no + 0x73a14), page = g32(no + 0x737a8);
    char board[48] = "";
    int endless_d = -1;
    if (kind == 0 || kind == 4) snprintf(board, sizeof board, "maker_points");  // (4: the period's ranking)
    else if (kind == 1 && g32(no + 0x73a1c) - 3 < 4) {
        endless_d = (int)(g32(no + 0x73a18) & 3);
        snprintf(board, sizeof board, "endless_%s", DIFF[endless_d]);
    } else if (kind == 1) snprintf(board, sizeof board, "clears");
    Json *j = NULL;
    if (*board) {
        char path[96];
        snprintf(path, sizeof path, "/api/rankings/%s?limit=%u", board, (page + 1) * 100);
        j = srv_call("GET", path, NULL, NULL);
    }
    Json *rows = json_get(j, "rankings");
    size_t n = json_len(rows), first = (size_t)page * 100;
    uint32_t cnt = 0;
    uint64_t pids[100];
    char ids[100][16];
    uint32_t scores[100];
    for (size_t i = first; i < n && cnt < 100; i++) {
        Json *r = json_at(rows, i);
        snprintf(ids[cnt], 16, "%s", json_str(json_get(r, "maker_id"), ""));
        pids[cnt] = mod_maker_pid(ids[cnt], json_str(json_get(r, "name"), ""));
        scores[cnt++] = (uint32_t)json_num(json_get(r, "score"), 0);
    }
    netobj_lock(c);
    uint64_t out = no + USER_RING;
    uint32_t cap = g32(out + 8), m = 0;
    s64(out + 0xc, 0);
    for (uint32_t i = 0; i < cnt && m < cap; i++) {
        uint64_t e = g64(out) + (uint64_t)USER_ENTRY * m++;
        fill_user(e, pids[i], ids[i], json_str(json_get(json_at(rows, first + i), "name"), ""), NULL);
        if (kind == 0 || kind == 4) s32(e + 0x128, scores[i]), s32(e + 0x110, scores[i]);  // maker points [? +0x110: the period's]
        else if (endless_d >= 0) s32(e + 0x114 + 4 * endless_d, scores[i]);  // Endless best
        else if (kind == 1) s32(e + 0x100, scores[i]);              // courses cleared (play stats key 1)
    }
    s32(out + 0x10, m);
    netobj_unlock(c);
    LOG("[mod] leaderboard %s (kind %u, page %u): %u makers\n", *board ? board : "-", kind, page, m);
    json_free(j);
    finish(c, no, true, 0);
}

// ---- a course or a maker by ID (GetUserOrCourse 131, main+0x14dc0d0): the code at +0x74728 (UTF-16); a course
// goes to the course list ring, a maker to the user ring.
static void get_user_or_course(Cpu *c) {
    if (!mod_hundred()) { guest_call_orig(c, c->pc); return; }
    uint64_t no = c->x[0];
    char raw[32], code[16];
    get_utf16(g64(no + 0x74728), raw, sizeof raw);
    size_t k = 0;
    for (const char *p = raw; *p && k < 9; p++)
        if (*p != '-' && *p != ' ') code[k++] = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
    code[k] = 0;
    char dashed[16];
    snprintf(dashed, sizeof dashed, "%.3s-%.3s-%.3s", code, code + 3, code + 6);
    char path[64];
    snprintf(path, sizeof path, "/api/courses/%s", dashed);
    Json *course = srv_call("GET", path, NULL, NULL);
    Json *maker = course ? NULL : maker_profile(dashed);
    netobj_lock(c);
    s64(no + NO_COURSES + 0xc, 0);
    s64(no + USER_RING + 0xc, 0);
    uint32_t id;
    if (course && code_to_data_id(dashed, &id) && g32(no + NO_COURSES + 8)) {
        mod_fill_course(g64(no + NO_COURSES), course, id, band(course));
        s32(no + NO_COURSES + 0x10, 1);
    } else if (maker && g32(no + USER_RING + 8)) {
        fill_user(g64(no + USER_RING), mod_maker_pid(dashed, json_str(json_get(json_get(maker, "maker"), "name"), "")),
                  dashed, "", maker);
        s32(no + USER_RING + 0x10, 1);
    }
    netobj_unlock(c);
    LOG("[mod] ID search %s: %s\n", dashed, course ? "course" : maker ? "maker" : "nothing");
    json_free(course);
    json_free(maker);
    finish(c, no, true, 0);
}

// ---- likes and boos, comments allowed

// CanPostRatingAndComment (61, main+0x14ebc60): data id +0xe8578; two answers, rating +0xe8580 and comment +0xe8530,
// each {u64 id, u8 +8, u32 state +0xc, u8 map[0] +0x10, u8 map[1] +0x11, valid +0x12}, copied by main+0x1520d10 to
// the course-play globals (*(main+0x2c115e0) +0x1b9c40 rating, +0x1b8950 comment). The clear screen's Boo / I like it
// (main+0x1be73b0) take input when the rating's u8 is 0 and its state isn't 2, and the player's own relation to the
// course (course record +0x188, mod_fill_course) is played or cleared [obs: u8 0, state 2 greyed them; u8 1, state 0
// with relation 0 left them dead]; the readers that go on to the comment step with state 2 (main+0x1b6af70,
// +0x1be6d30, +0x1be9860) are taken then as "already rated" [inf]. Answered: 0 / 0, so rating is open. The comment
// button (main+0x1be7d30, +0x1b6bb48) also wants the comment answer's two allowances (map[0] / map[1]) above the
// player's comment count on the course (course +0x7896c), and the course's "comments on" flag (course global
// main+0x2c4b710 +0x860: the maker's profile setting, copied into the course list's entries +0x830, from the maker's
// user record +0x98: fill_user sets it) [obs: 0 kept the button grey]. Our server takes likes and comments on every
// course.
static void can_rate(Cpu *c) {
    if (!mod_hundred()) { guest_call_orig(c, c->pc); return; }
    uint64_t no = c->x[0], id = g64(no + 0xe8578);
    netobj_lock(c);
    for (int i = 0; i < 2; i++) {
        uint64_t r = no + (i ? 0xe8530 : 0xe8580);
        s64(r, id), s8(r + 8, 0), s32(r + 0xc, 0), s16(r + 0x10, i ? 0x0a0a : 0), s8(r + 0x12, 1);
    }
    netobj_unlock(c);
    MLOG("rating / comments allowed for %llu\n", (unsigned long long)id);
    finish(c, no, true, 0);
}

// PostRatingInfo (104, main+0x14e6b90): data id +0xe8548, u32 +0xe8550 the rating (1 like, 2 boo, 0 take back: its
// answer, main+0x151fd00, sets the course's own-relation value to 3 / 4 / 2 and counts a like on the game's copies),
// u32 +0xe8554. -> POST /api/courses/<code>/like | boo, DELETE /api/courses/<code>/vote.
static void post_rating(Cpu *c) {
    if (!mod_hundred()) { guest_call_orig(c, c->pc); return; }
    uint64_t no = c->x[0], id = g64(no + 0xe8548);
    uint32_t a = g32(no + 0xe8550), b = g32(no + 0xe8554);
    char code[12], path[64];
    course_code_of(id, code);
    bool ok = true;
    if (a == 1 || a == 2) {
        snprintf(path, sizeof path, "/api/courses/%s/%s", code, a == 1 ? "like" : "boo");
        ok = post_ok(path, "{}");
    } else if (a == 0) {
        snprintf(path, sizeof path, "/api/courses/%s/vote", code);
        Json *j = srv_call("DELETE", path, NULL, NULL);
        ok = j != NULL;
        json_free(j);
    }
    LOG("[mod] rating %s: %u / %u%s\n", code, a, b, a == 1 ? " (like)" : a == 2 ? " (boo)" : a == 0 ? " (taken back)" : "");
    finish(c, no, ok, 2);
}

// PostPlayResult (96, main+0x14e30e0): data id +0x8e618, cleared +0x8e620, time ms +0x8e624, u8 +0x8e628, u64 +0x8e630,
// death position +0x8e638 / +0x8e63c, ... (plus a recording uploaded first [inf]). Our server counts a play, and a
// clear with its time. Endless plays are counted by the Endless reports.
bool endless_has_course(uint64_t id);  // endless.c
static void post_play_result(Cpu *c) {
    if (!mod_hundred()) { guest_call_orig(c, c->pc); return; }
    uint64_t no = c->x[0], id = g64(no + 0x8e618);
    // [obs: a clear in 12.851 s posted 1 / 12851 / 1]: +0x8e620 cleared, +0x8e624 the time in ms
    uint32_t cleared = g32(no + 0x8e620), time_ms = g32(no + 0x8e624), v2 = g8(no + 0x8e628);
    char code[12], path[64], body[64];
    course_code_of(id, code);
    LOG("[mod] play result %s: %s, %u ms (%u)\n", code, cleared == 1 ? "clear" : "no clear", time_ms, v2);
    if (!endless_has_course(id)) {
        snprintf(path, sizeof path, "/api/courses/%s/play", code);
        post_ok(path, "{}");
        if (cleared == 1) {
            snprintf(path, sizeof path, "/api/courses/%s/clear", code);
            snprintf(body, sizeof body, "{\"time_ms\":%u}", time_ms);
            post_ok(path, body);
        }
    }
    finish(c, no, true, 0);
}

// ---- comments

static size_t utf16_to_utf8(uint64_t buf, uint32_t max, char *out, size_t cap) {
    size_t n = 0;
    for (uint32_t i = 0; buf && i < max; i++) {
        uint32_t ch = *(volatile uint16_t *)(uintptr_t)(buf + 2 * i);
        if (!ch) break;
        if (ch < 0x80 && n + 1 < cap) out[n++] = (char)ch;
        else if (ch < 0x800 && n + 2 < cap) out[n++] = (char)(0xC0 | ch >> 6), out[n++] = (char)(0x80 | (ch & 0x3F));
        else if (n + 3 < cap)
            out[n++] = (char)(0xE0 | ch >> 12), out[n++] = (char)(0x80 | (ch >> 6 & 0x3F)), out[n++] = (char)(0x80 | (ch & 0x3F));
    }
    out[n] = 0;
    return n;
}

static void json_escape(const char *s, char *out, size_t cap) {
    size_t n = 0;
    for (; *s && n + 7 < cap; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') out[n++] = '\\', out[n++] = (char)ch;
        else if (ch < 0x20) n += (size_t)snprintf(out + n, cap - n, "\\u%04x", ch);
        else out[n++] = (char)ch;
    }
    out[n] = 0;
}

// A comment as SMM2 keeps one: the 0x390-byte record its converter (main+0x14b7720) fills from a NEX CommentInfo,
// and the game's own copy of a comment it just posted (main+0x14b8030, from the post's request) shows which request
// field lands where: id (UTF-8) +0x08; +0x58 posted from the clear screen (request +0x8e690 [obs: 1 from the clear
// screen, 0 from the pause menu]): those show under the clear screen and not in the course, the others in the course
// (main+0x17f2a30 takes +0x58 == 0; the pause menu's Display Comments counts them, main+0x1861360); +0x5c type (0
// drawing, 1 text, 2 stamp: the screen's three tabs, request +0x8e694, main+0x17f88e0); the poster's player id +0x60
// (the course's maker gets the framed bubble, the poster their own Delete); where the player was, x / y +0x68 /
// +0x6c in pixels, 16 to a block, y up (request +0x8e698 / +0x8e69c; shown at least 70 / 3 in and inside the area
// [obs: (0, 0) at 4.4 blocks]); +0x70 in the sub area (+0x8e6a0); +0x71 the face picked for the Mii (+0x8e6a1);
// +0x72 "Require course clear to view" (+0x8e6a2) [obs: ticked, 1]: the game itself hides such a comment, in the
// course and in the list ("You can view comments for this course after clearing it."), from a player whose own play
// state isn't 3 (course record +0x184, endless.c fill_course) unless they posted it or made the course; posted time
// +0x78 (Unix seconds [obs: "5 minutes ago"]); the text (UTF-16) +0x88; the stamp's number +0x130 (+0x8e758); a
// drawing's picture (a relation object, +0x140); the poster's name (UTF-16) +0x2c8 and Mii +0x2f0; +0x388 valid. A
// comment with type 0 has its picture fetched (main+0x14b8930 picks records with +0x5c == 0, main+0x14eaf30
// downloads them). While a course plays (SearchComments mode 1 at its start, +0x8e678; the list asks mode 0, pages
// of 100 from +0x737a8) the game copies them to the course-play globals (main+0x150bd10, *(main+0x2c115e0)
// +0x15f818, drawings' pictures +0x1b8940) and shows them as bubbles that open one by one, none near the player.
#define COMMENT_PICTURE 0x1c200  // a drawn comment's picture as the game keeps it
static void comment_fields(uint64_t no, char *out, size_t cap) {
    snprintf(out, cap, "\"x\":%d,\"y\":%d,\"sub_area\":%s,\"face\":%u,\"clear_required\":%s,\"cleared\":%s",
             (int32_t)g32(no + 0x8e698), (int32_t)g32(no + 0x8e69c), g8(no + 0x8e6a0) ? "true" : "false",
             g8(no + 0x8e6a1), g8(no + 0x8e6a2) ? "true" : "false", g32(no + 0x8e690) ? "true" : "false");
}

// SearchComments / InOrder (94 / 95, main+0x14e8ec0): data id +0x8e680; answer: the ring of comment records at
// +0x8f410, then +0xe8528 the page, +0xe852c, +0xe852d answered.
static void search_comments(Cpu *c) {
    if (!mod_hundred()) { guest_call_orig(c, c->pc); return; }
    uint64_t no = c->x[0], id = g64(no + 0x8e680);
    char code[12], path[64];
    course_code_of(id, code);
    snprintf(path, sizeof path, "/api/courses/%s/comments", code);
    Json *j = srv_call("GET", path, NULL, NULL);
    Json *list = json_get(j, "comments");
    uint64_t pids[64];
    size_t n = json_len(list) < 64 ? json_len(list) : 64;
    for (size_t i = 0; i < n; i++) {
        Json *m = json_at(list, i);
        pids[i] = mod_maker_pid(json_str(json_get(m, "maker_id"), ""), json_str(json_get(m, "name"), ""));
    }
    netobj_lock(c);
    uint64_t ring = no + 0x8f410, entries = g64(ring);
    uint32_t cap = g32(ring + 8), cnt = 0;
    s64(ring + 0xc, 0);
    for (size_t i = 0; i < n && cnt < cap && entries; i++) {
        Json *m = json_at(list, i);
        uint64_t e = entries + 0x390ull * cnt++;
        const char *cid = json_str(json_get(m, "id"), "");
        if (g64(e + 0x08) && g32(e + 0x10) > strlen(cid)) memcpy((void *)(uintptr_t)g64(e + 0x08), cid, strlen(cid) + 1);
        s32(e + 0x58, json_bool(json_get(m, "cleared"), false));
        s32(e + 0x5c, (uint32_t)json_num(json_get(m, "type"), 1));
        s64(e + 0x60, pids[i]);
        s32(e + 0x68, (uint32_t)(int32_t)json_num(json_get(m, "x"), 0));
        s32(e + 0x6c, (uint32_t)(int32_t)json_num(json_get(m, "y"), 0));
        s8(e + 0x70, json_bool(json_get(m, "sub_area"), false));
        s8(e + 0x71, (uint8_t)json_num(json_get(m, "face"), 0));
        s8(e + 0x72, json_bool(json_get(m, "clear_required"), false));
        s64(e + 0x78, (uint64_t)json_num(json_get(m, "posted"), 0));
        put_utf16(g64(e + 0x88), g32(e + 0x90), json_str(json_get(m, "text"), ""));
        s16(e + 0x130, (uint16_t)json_num(json_get(m, "stamp"), 0)), s32(e + 0x134, 0), s8(e + 0x138, 0);
        // a drawing's picture (relation object +0x140: the key +0x148, the name the game's cache keeps it under
        // +0x268, its size +0x2b8): the comment's id for both; comment_pictures fetches it by that
        bool drawn = json_num(json_get(m, "type"), 1) == 0;
        for (int k = 0; k < 2; k++) {
            uint64_t buf = g64(e + (k ? 0x268 : 0x148));
            uint32_t bcap = g32(e + (k ? 0x270 : 0x150));
            const char *v = drawn ? cid : "";
            if (buf && bcap > strlen(v)) memcpy((void *)(uintptr_t)buf, v, strlen(v) + 1);
        }
        s32(e + 0x2b8, drawn ? COMMENT_PICTURE : 0);
        put_utf16(g64(e + 0x2c8), g32(e + 0x2d0), json_str(json_get(m, "name"), ""));
        mii_default_charinfo((uint8_t *)(uintptr_t)(e + 0x2f0), json_str(json_get(m, "name"), ""));
        s8(e + 0x388, 1);
    }
    s32(ring + 0x10, cnt);
    s32(no + 0xe8528, g32(no + 0x737a8));
    s8(no + 0xe852c, g8(no + 0x737ac));
    s8(no + 0xe852d, 1);
    netobj_unlock(c);
    LOG("[mod] comments on %s: %u\n", code, cnt);
    json_free(j);
    finish(c, no, true, 0);
}

// A comment posted: POST /api/courses/<code>/comments with SMM2's fields; the server's id for it goes back into
// *(+0x8e768) (UTF-8, capacity +0x8e770), as the original's answer does.
static void post_comment_as(Cpu *c, const char *what, const char *body_fields) {
    uint64_t no = c->x[0];
    char code[12], path[64], body[900], fields[200];
    course_code_of(g64(no + 0x8e688), code);
    comment_fields(no, fields, sizeof fields);
    snprintf(path, sizeof path, "/api/courses/%s/comments", code);
    snprintf(body, sizeof body, "{%s,%s}", body_fields, fields);
    Json *j = srv_call("POST", path, body, NULL);
    const char *cid = json_str(json_get(j, "id"), "");
    netobj_lock(c);
    uint64_t out = g64(no + 0x8e768);
    if (out && g32(no + 0x8e770) > strlen(cid)) memcpy((void *)(uintptr_t)out, cid, strlen(cid) + 1);
    netobj_unlock(c);
    LOG("[mod] comment (%s) on %s: %s %s\n", what, code, j ? "posted" : "failed", body);
    finish(c, no, j != NULL, 2);
    json_free(j);
}

// PostCommentText (91, main+0x14e96b0): the comment's fields (above), the text (UTF-16, up to 75 characters) at
// *(+0x8e6b0).
static void post_comment(Cpu *c) {
    if (!mod_hundred()) { guest_call_orig(c, c->pc); return; }
    char text[256], esc[600], fields[640];
    utf16_to_utf8(g64(c->x[0] + 0x8e6b0), 75, text, sizeof text);
    json_escape(text, esc, sizeof esc);
    snprintf(fields, sizeof fields, "\"type\":1,\"text\":\"%s\"", esc);
    post_comment_as(c, "text", fields);
}

// The stamp comment (main+0x14eaa20, the text post's request with the stamp's number at +0x8e758 in place of the
// text; [obs: unanswered, "Unable to post comment."]).
static void post_comment_stamp(Cpu *c) {
    if (!mod_hundred()) { guest_call_orig(c, c->pc); return; }
    char fields[64];
    snprintf(fields, sizeof fields, "\"type\":2,\"stamp\":%u", g32(c->x[0] + 0x8e758) & 0xffff);
    post_comment_as(c, "stamp", fields);
}

// The drawn comment (main+0x14e9c10): the text post's request, and the drawing in two buffers: *(+0x8f400) (its size
// at +0x8e75c), which the game keeps as the comment's picture (its own copy after posting: main+0x14b8030 stores it
// under the comment's id), and *(+0x8f408) (0x1c200 bytes) [? a second form the original uploads as well]. The first
// goes to our server as the comment's picture: PUT /api/comments/<id>/picture.
static void post_comment_picture(Cpu *c) {
    if (!mod_hundred()) { guest_call_orig(c, c->pc); return; }
    uint64_t no = c->x[0];
    post_comment_as(c, "drawing", "\"type\":0");
    if (g32(no + NO_STATUS) != 0) return;
    char cid[64] = "", path[96];
    uint64_t s = g64(no + 0x8e768);
    for (size_t i = 0; s && i + 1 < sizeof cid && g8(s + i); i++) cid[i] = (char)g8(s + i), cid[i + 1] = 0;
    uint32_t len = g32(no + 0x8e75c);
    uint64_t pic = g64(no + 0x8f400);
    if (len > COMMENT_PICTURE) len = COMMENT_PICTURE;
    snprintf(path, sizeof path, "/api/comments/%s/picture", cid);
    int st = 0;
    Json *j = pic && len ? srv_send_bytes("PUT", path, (const void *)(uintptr_t)pic, len, &st) : NULL;
    LOG("[mod] comment %s: its drawing (%u bytes) %s\n", cid, len, j ? "kept by the server" : "not taken");
    if (!j) finish(c, no, false, 2);
    json_free(j);
}

// Drawn comments' pictures (main+0x14eaf30): the comments whose picture isn't in the game's cache yet are queued at
// +0x8ec58 (ring of 0x180-byte entries: +0x0c head, +0x08 capacity, count +0x8ec68; picked by main+0x14b8930, queued
// by main+0x1527ed0 from the comment record's +0x140..: the comment's id at +0x128); the original asks NEX where each
// is and downloads it into the buffers *(+0x8f3f8)[i] (0x1c200 bytes each, +0x8f3f0 of them), and its completion
// (main+0x151fbb0) puts each buffer in the cache under that id. From GET /api/comments/<id>/picture.
static void comment_pictures(Cpu *c) {
    if (!mod_hundred()) { guest_call_orig(c, c->pc); return; }
    uint64_t no = c->x[0];
    char ids[8][64];
    netobj_lock(c);
    uint32_t n = g32(no + 0x8ec68), cap = g32(no + 0x8ec60), head = g32(no + 0x8ec64), nbuf = g32(no + 0x8f3f0);
    if (n > nbuf) n = nbuf;
    if (n > 8) n = 8;
    for (uint32_t i = 0; i < n && cap; i++) {
        uint64_t e = g64(no + 0x8ec58) + 0x180ull * ((head + i) % cap), s = g64(e + 0x128);
        ids[i][0] = 0;
        for (size_t k = 0; s && k + 1 < sizeof ids[i] && g8(s + k); k++) ids[i][k] = (char)g8(s + k), ids[i][k + 1] = 0;
    }
    netobj_unlock(c);
    uint32_t got = 0;
    for (uint32_t i = 0; i < n; i++) {
        char path[96];
        snprintf(path, sizeof path, "/api/comments/%s/picture", ids[i]);
        size_t len = 0;
        uint8_t *b = ids[i][0] ? (uint8_t *)srv_get_bytes(path, &len) : NULL;
        uint64_t buf = g64(g64(no + 0x8f3f8) + 8ull * i);
        if (buf) {
            memset((void *)(uintptr_t)buf, 0, COMMENT_PICTURE);
            if (b) memcpy((void *)(uintptr_t)buf, b, len < COMMENT_PICTURE ? len : COMMENT_PICTURE), got++;
        }
        free(b);
    }
    LOG("[mod] drawn comments: %u of %u pictures from the server\n", got, n);
    finish(c, no, n > 0, 1);
}

// Deleting one's own comment (the comment list's Delete, main+0x14eadb0): the comment's id (UTF-8) at *(+0x8e7c0);
// answer: the status only (4 when the original's call fails with 0x8069000b [inf: no such comment], else 2).
// -> DELETE /api/comments/<id> (the poster, the course's uploader or the admin).
static void delete_comment(Cpu *c) {
    if (!mod_hundred()) { guest_call_orig(c, c->pc); return; }
    uint64_t no = c->x[0];
    char cid[64] = "", path[96];
    netobj_lock(c);
    uint64_t s = g64(no + 0x8e7c0);
    for (size_t i = 0; s && i + 1 < sizeof cid && g8(s + i); i++) cid[i] = (char)g8(s + i), cid[i + 1] = 0;
    netobj_unlock(c);
    snprintf(path, sizeof path, "/api/comments/%s", cid);
    int st = 0;
    Json *j = srv_call("DELETE", path, NULL, &st);
    LOG("[mod] comment %s deleted: %s\n", cid, j ? "yes" : "no");
    finish(c, no, j != NULL, st == 404 ? 4 : 2);
    json_free(j);
}

// ---- uploads from the Course Maker

// CanPostCourse (60, main+0x14e0d40): allowed. The wrapper keeps the call's answer at +0x8e610: the call's u32
// when its flag is clear, 0 when it is set (main+0x14e0d40), and the Coursebot's upload goes on only with 0;
// anything else is "You can't upload any more courses right now. Please wait a while and try again later." [obs: 100
// gave that message after a passed clear check, 0 uploads].
static void can_post_course(Cpu *c) {
    if (!mod_hundred()) { guest_call_orig(c, c->pc); return; }
    uint64_t no = c->x[0];
    netobj_lock(c);
    s32(no + 0x8e610, 0);
    netobj_unlock(c);
    MLOG("CanPostCourse: yes\n");
    finish(c, no, true, 0);
}

// The course's picture goes up after it: the game's thumbnail block follows the course in the upload buffer (+0x5c000,
// 0x1c000 bytes: the JPEG the Course Maker made, its size at +0x1bf9c) [obs: one JPEG there, none after it up to
// +0xb0000, so no whole-course map]. PUT /api/courses/<code>/thumbnail (the uploader's own course); the server keeps
// the JPEG and Course World shows it like any other course's picture.
#define UPLOAD_THUMB 0x5c000
static void post_pictures(const char *code, uint64_t src) {
    uint64_t block = src + UPLOAD_THUMB;
    uint32_t len = g32(block + 0x1bf9c);
    if (g8(block) != 0xff || g8(block + 1) != 0xd8 || !len || len > 0x1bf9c) {
        LOG("[mod] course upload %s: no picture with it\n", code);
        return;
    }
    char path[64];
    int st = 0;
    snprintf(path, sizeof path, "/api/courses/%s/thumbnail", code);
    Json *j = srv_send_bytes("PUT", path, (const void *)(uintptr_t)block, len, &st);
    LOG("[mod] course upload %s: its picture (%u bytes) %s\n", code, len, j ? "kept by the server" : "not taken");
    json_free(j);
}

// The course upload (main+0x14de3f0: PreparePostObjectCourse, the course and thumbnail upload, CompletePostObject):
// the game's own course file is at *(+0x8e5e0) (header 0x10 with "SCDL" and the CRC32, then the course, 0x5bfc0
// bytes, not encrypted yet; the two thumbnails follow at +0x5c000). Our server takes the course itself (POST
// /api/courses, signed in) and gives back its code; the data id goes to +0x8e5f8, which the game keeps in the saved
// course as its upload.
static void post_course(Cpu *c) {
    if (!mod_hundred()) { guest_call_orig(c, c->pc); return; }
    uint64_t no = c->x[0], src = g64(no + 0x8e5e0);
    bool ok = false;
    char code[16] = "?";
    if (src && g32(src + 0xc) == 0x4c444353 && g32(src) == 1) {  // "SCDL", version 1
        int st = 0;
        char path[256], based_on[200];  // an edited copy of someone else's course credits the original (edit.c)
        extern void edit_upload_query(const uint8_t *course, char *out, size_t cap);
        edit_upload_query((const uint8_t *)(uintptr_t)(src + 0x10), based_on, sizeof based_on);
        snprintf(path, sizeof path, "/api/courses%s", based_on);
        if (based_on[0]) LOG("[mod] course upload: an edited copy (%s)\n", based_on + 1);
        Json *j = srv_post_bytes(path, (const void *)(uintptr_t)(src + 0x10), 0x5bfc0, &st);
        uint32_t id;
        if (j && code_to_data_id(json_str(json_get(j, "code"), ""), &id)) {
            snprintf(code, sizeof code, "%s", json_str(json_get(j, "code"), ""));
            netobj_lock(c);
            s64(no + 0x8e5f8, id);
            netobj_unlock(c);
            ok = true;
        }
        json_free(j);
    }
    LOG("[mod] course upload: %s\n", ok ? code : "failed");
    if (ok) post_pictures(code, src);
    finish(c, no, ok, 2);
}

// DeleteObject (DataStore, main+0x14e0c50): data id +0x8e5f0, the player's uploaded course -> DELETE
static void delete_course(Cpu *c) {
    if (!mod_hundred()) { guest_call_orig(c, c->pc); return; }
    uint64_t no = c->x[0];
    char code[12], path[64];
    course_code_of(g64(no + 0x8e5f0), code);
    snprintf(path, sizeof path, "/api/courses/%s", code);
    Json *j = srv_call("DELETE", path, NULL, NULL);
    LOG("[mod] course %s deleted: %s\n", code, j ? "yes" : "no");
    json_free(j);
    finish(c, no, true, 0);
}

// DataStore method 128 (main+0x14f0090): the player's courses the server took down, one 0xd8-byte entry each (data
// id, a reason byte, the course's name) into the ring at NetObj +0x136c08 (+0x136c10 capacity, +0x136c14 head, +0x136c18
// count); asked when My Info's Uploaded Courses opens [obs]. Our server takes no one's course down: none, ok (left
// unanswered it failed, and the tab gave "A communication error occurred").
static void deleted_courses(Cpu *c) {
    if (!mod_hundred()) { guest_call_orig(c, c->pc); return; }
    uint64_t no = c->x[0];
    netobj_lock(c);
    s32(no + 0x136c14, 0);
    s32(no + 0x136c18, 0);
    netobj_unlock(c);
    finish(c, no, true, 0);
}

// The server's time as the game keeps it: the NEX core object (*(main+0x2a17f08)) +0x110 known, +0x118 POSIX
// seconds, set on each tick of the NEX core (main+0x21710) from the connection's synchronized clock, and unknown with
// no connection. What counts from it stays as it is without it: a comment's "X minutes ago" (main+0x1a0b230, only for
// a post at or before it) kept its layout's placeholder [obs: "5分前" on every comment]. With our server the tick
// gives it this machine's clock.
static void nex_tick(Cpu *c) {
    uint64_t self = c->x[0];
    guest_call_orig(c, c->pc);
    if (!mod_hundred() || g8(self + 0x110)) return;
    s64(self + 0x118, (uint64_t)time(NULL));
    s8(self + 0x110, 1);
}

void world_register(void) {
    hle_override(MAIN_ADDR(0x21710), nex_tick, "mod: the server's time");
    hle_override(MAIN_ADDR(0x14f0090), deleted_courses, "mod: the player's courses taken down (none)");
    hle_override(MAIN_ADDR(0x14e8ec0), search_comments, "mod: SearchComments");
    hle_override(MAIN_ADDR(0x14e96b0), post_comment, "mod: PostCommentText");
    hle_override(MAIN_ADDR(0x14eaa20), post_comment_stamp, "mod: the stamp comment");
    hle_override(MAIN_ADDR(0x14eadb0), delete_comment, "mod: delete a comment");
    hle_override(MAIN_ADDR(0x14e9c10), post_comment_picture, "mod: the drawn comment");
    hle_override(MAIN_ADDR(0x14eaf30), comment_pictures, "mod: drawn comments' pictures");
    hle_override(MAIN_ADDR(0x14e0d40), can_post_course, "mod: CanPostCourse");
    hle_override(MAIN_ADDR(0x14de3f0), post_course, "mod: course upload");
    hle_override(MAIN_ADDR(0x14e0c50), delete_course, "mod: DeleteObject (course)");
    hle_override(MAIN_ADDR(0x14d02d0), get_users_list, "mod: GetUsers (players)");
    hle_override(MAIN_ADDR(0x14d7780), get_users_list, "mod: GetUsers (players, 2)");
    hle_override(MAIN_ADDR(0x14d2f00), get_user_page, "mod: GetUsers (maker page)");
    hle_override(MAIN_ADDR(0x14d74a0), get_user_counts, "mod: GetUsers (counts)");
    hle_override(MAIN_ADDR(0x14d1820), search_users, "mod: SearchUsers (leaderboards)");
    hle_override(MAIN_ADDR(0x14dc0d0), get_user_or_course, "mod: GetUserOrCourse");
    hle_override(MAIN_ADDR(0x14ebc60), can_rate, "mod: CanPostRatingAndComment");
    hle_override(MAIN_ADDR(0x14e6b90), post_rating, "mod: PostRatingInfo");
    hle_override(MAIN_ADDR(0x14e30e0), post_play_result, "mod: PostPlayResult");
}
