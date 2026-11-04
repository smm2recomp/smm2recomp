// OpenCourse World: the game's own online client against the player's own OCW account, as the Atmosphère / Ryujinx
// mod does it on a console (_vendor/ocw-client-mod). The player signs in at opencourse.world, accepts its terms and
// is given a domain and a personal token; those go in ocw-config.json in the data folder, next to smm2mods.ini:
//
//     { "domain": "server.opencourse.world", "token": "<22 characters>", "ip": "10.0.0.5" }
//
//   domain  the host written into the game, so the game looks it up, sends it as Host: and names it for SNI
//           (at most 29 characters: the string slot in the game's own binary). May itself be an address.
//   token   the account's token, exactly 22 characters, carried in the WebSocket handshake
//   ip      optional: the address `domain` resolves to, instead of asking the host resolver. This is how a
//           self-hosted or local OCW instance is reached, and how an IP-only set-up keeps a real name for SNI.
//
// Only two of the console mod's ten patches are needed here. Its other eight stub nn::account calls that reach real
// Nintendo services on a console; in the recomp those reach runtime/services/acc.c, which already reports a network
// service account and hands out a local ID token whenever the game's own sockets are on.
//
//   1. the domain, written over the game's own host string once the modules are loaded (below; the whole mapping is
//      read/write, runtime/loader.c)
//   2. the WebSocket handshake, built by the host instead of the game's own snprintf so the token rides along
//      (smm2_ocw_header, reached from the one patch in mods/patches.py)
//
// Nothing here does anything unless backend=ocw is chosen (smm2mods.ini, or the overlay's Mods tab) *and* a usable
// ocw-config.json is present: the game is then unmodified and goes nowhere, exactly as before.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../../../runtime/game_rows.h"
#include "../../../runtime/net/json.h"
#include "../../../runtime/net/netcfg.h"
#include "../../../runtime/net/http.h"
#include "../../../runtime/kernel.h"
#include "course_preload.h"
#include "mod.h"
#include "ocw.h"

// The game's own host string (v3.0.3). The console mod writes the same 29 bytes at main+0x22cc307.
#define OCW_DOMAIN_OFF 0x22cc307
#define OCW_DOMAIN_MAX 29  // the slot's size: the domain is zero-padded to it, as the mod does
#define OCW_TOKEN_LEN 22   // the mod rejects any other length
#define OCW_MOD_VERSION "0.2"
#define OCW_GAME_VERSION "3.0.3"

static struct {
    bool active;                       // config read and backend=ocw: the hook and the domain write are live
    char domain[OCW_DOMAIN_MAX + 1];
    char token[OCW_TOKEN_LEN + 1];
    char ip[64];
} g_ocw;

static const char *config_path(void) {
    const char *e = getenv("RECOMP_OCW_CONFIG");
    return e && *e ? e : "ocw-config.json";
}

// Read and check ocw-config.json. False (logged, with what to do about it) leaves everything inert: unlike the
// console mod, which aborts the game when its config is missing, a bad config here must not stop the game.
static bool config_load(void) {
    const char *path = config_path();
    FILE *f = fopen(path, "rb");
    if (!f) {
        LOG("[ocw] no %s in the data folder: Course World stays off (backend=ocw needs it)\n", path);
        return false;
    }
    char text[2048];
    size_t n = fread(text, 1, sizeof text - 1, f);
    bool too_big = n == sizeof text - 1;
    fclose(f);
    if (too_big) {
        LOG("[ocw] %s is larger than %zu bytes: ignored\n", path, sizeof text - 1);
        return false;
    }
    text[n] = 0;
    Json *j = json_parse(text, n);
    if (!j) {
        LOG("[ocw] %s is not valid JSON: ignored\n", path);
        return false;
    }
    const char *domain = json_str(json_get(j, "domain"), "");
    const char *token = json_str(json_get(j, "token"), "");
    const char *ip = json_str(json_get(j, "ip"), "");
    bool ok = true;
    if (!*domain) {
        LOG("[ocw] %s has no \"domain\"\n", path);
        ok = false;
    } else if (strlen(domain) > OCW_DOMAIN_MAX) {
        LOG("[ocw] \"domain\" is %zu characters; the game's own slot holds %d\n", strlen(domain), OCW_DOMAIN_MAX);
        ok = false;
    }
    if (!*token) {
        LOG("[ocw] %s has no \"token\"\n", path);
        ok = false;
    } else if (strlen(token) != OCW_TOKEN_LEN) {
        LOG("[ocw] \"token\" is %zu characters; it should be %d\n", strlen(token), OCW_TOKEN_LEN);
        ok = false;
    }
    // Kept whatever the checks said: the menu shows and edits these buffers, and a half-filled config must not come
    // back as empty fields that are then written over the file. `ok` alone decides whether the game is touched.
    snprintf(g_ocw.domain, sizeof g_ocw.domain, "%s", domain);
    snprintf(g_ocw.token, sizeof g_ocw.token, "%s", token);
    snprintf(g_ocw.ip, sizeof g_ocw.ip, "%s", ip);
    json_free(j);
    return ok;
}

// The modules are loaded and nothing of the game has run yet (runtime/mods.c mods_boot): write the domain into the
// game's own host string, and tell the network stack which one host it may reach.
static void preload_reset(void);
static void preload_self_test(void);

void game_modules_loaded(void) {
    g_ocw.active = false;  // (an in-game reboot onto another backend: the hook makes the game's own call again)
    preload_reset();       // (the backend may be another one now: nothing fetched for the last is served)
    preload_self_test();
    if (net_backend() != NET_BACKEND_OCW) return;  // (the game is left exactly as it is)
    if (!config_load()) return;

    char *slot = (char *)(uintptr_t)MAIN_ADDR(OCW_DOMAIN_OFF);
    char was[OCW_DOMAIN_MAX + 1];
    memcpy(was, slot, OCW_DOMAIN_MAX);
    was[OCW_DOMAIN_MAX] = 0;
    for (int i = 0; i < OCW_DOMAIN_MAX; i++)
        if (was[i] && (was[i] < 0x20 || was[i] > 0x7e)) was[i] = '?';  // (log it, whatever it turned out to be)
    char buf[OCW_DOMAIN_MAX];
    memset(buf, 0, sizeof buf);
    memcpy(buf, g_ocw.domain, strlen(g_ocw.domain));  // zero-padded to the slot, as the console mod writes it
    memcpy(slot, buf, sizeof buf);
    LOG("[ocw] host \"%s\" written at main+%#x (was \"%s\")\n", g_ocw.domain, OCW_DOMAIN_OFF, was);

    net_log_secret(g_ocw.token);  // (never in a log: a log file is what a player attaches to a bug report)
    net_ocw_set_target(g_ocw.domain, g_ocw.ip[0] ? g_ocw.ip : NULL);
    g_ocw.active = true;
    LOG("[ocw] on: the game's own online client against %s\n", g_ocw.domain);
}

bool ocw_active(void) { return g_ocw.active; }

// ---- the next Endless courses ahead (course_preload.h, issue #61). The game asks OCW for 10 courses a search and
// plays them in order; each one's download is the game's own GetObject: DataStore PrepareGetObject over its NEX
// connection, then an HTTP GET of the file. While a course is played, the files of the next CP_DEPTH in its list are
// fetched from the same public address (http://<domain>/level_data/<data id>: no token, nothing of the account), and
// when the game's own GET for one comes, its answer is the file held (runtime/services/bsd_host.c game_http_answer).
//
// Why at the socket and not in place of the whole GetObject (as endless.c serves our own server's courses): the
// game's PrepareGetObject still goes to OCW for every course, so whatever OCW keeps of it (plays, its Endless
// bookkeeping) stays as it was; only the file's transfer is replaced, and that is what a skip waits on. Anything that
// doesn't match exactly (another URL, HTTPS, a file not held, a bad header) is left to the game's own path. If the
// game's download doesn't come as that GET at all, fetching ahead is no use: it stops for the session (logged).
// A fetch still under way when the game asks is waited on before the game's own call (its core given up), at most as
// long as a download of the game's own might take.
// "ip" in ocw-config.json (a self-hosted OCW at an address): off, since the fetch would ask the name's own address.
#define OCW_PRELOAD_TIMEOUT_MS 20000
extern uint64_t plat_now_ns(void);
static char preload_host[OCW_DOMAIN_MAX + 1];  // (the fetches' host: the domain, or the self-test's)
static volatile bool preload_stopped;
static volatile uint32_t served_id;  // the file the last answer of ours was
static int not_seen;                 // files held that the game's download went past

void endless_reserve_drop(const char *why);  // endless.c

static void preload_reset(void) {
    cp_clear("backend restart");
    endless_reserve_drop("backend restart");
    preload_stopped = false, served_id = 0, not_seen = 0;
}

static const char *preload_domain(void) {
    if (!g_ocw.active || !g_mod.preload || preload_stopped) return NULL;
    if (g_ocw.ip[0]) {
        static bool said;
        if (!said) said = true, LOG("[ocw] next-course preload off: \"ip\" is set in ocw-config.json\n");
        return NULL;
    }
    return g_ocw.domain;
}

static uint8_t *ocw_fetch(uint32_t id, size_t *len) {  // (the preload's thread)
    char url[128];
    snprintf(url, sizeof url, "http://%s/level_data/%u", preload_host, id);
    HttpResponse r;
    int st = http_request("GET", url, "Accept: */*\r\n", NULL, 0, OCW_PRELOAD_TIMEOUT_MS, &r);
    if (st != 200) {
        MLOG("ocw: preload GET /level_data/%u: %s%d\n", id, st ? "HTTP " : r.error, st);
        http_free(&r);
        return NULL;
    }
    *len = r.len;
    return (uint8_t *)r.body;  // (the preload frees it)
}

static int joined_ms, state_before;

void ocw_preload_before(uint32_t id) {  // (endless.c: before the game's own GetObject)
    served_id = 0, joined_ms = -1, state_before = CP_NONE;
    if (!preload_domain()) return;
    state_before = cp_state(id, CP_FILE);
    if (state_before == CP_QUEUED || state_before == CP_LOADING)
        joined_ms = cp_wait(id, CP_FILE, OCW_PRELOAD_TIMEOUT_MS + 2000);
}

void ocw_preload_after(uint32_t id, bool ok) {  // (... and after it)
    const char *host = preload_domain();
    if (!host) return;
    char code[12];
    data_id_to_code(id, code);
    int st = cp_state(id, CP_FILE);
    if (served_id == id && joined_ms >= 0) MLOG("ocw: GetObject %s: preloaded, joined after %d ms\n", code, joined_ms);
    else if (served_id == id) MLOG("ocw: GetObject %s: preloaded\n", code);
    else if (st == CP_READY && ok) {  // (held, and the game downloaded it some other way)
        LOG("[ocw] GetObject %s: the game's own download (not GET http://%s/level_data/<id>)\n", code, host);
        if (++not_seen >= 2) {
            preload_stopped = true;
            cp_clear("not used");
            LOG("[ocw] next-course preload off for this session: the game downloads its courses another way\n");
            return;
        }
    } else {
        MLOG("ocw: GetObject %s: the game's own download (%s)\n", code,
             state_before == CP_NONE ? "not preloaded" : st == CP_FAILED ? "its preload failed"
             : st == CP_READY ? "the game's request failed" : "its preload didn't end");
    }
    snprintf(preload_host, sizeof preload_host, "%s", host);
    cp_after(id, CP_FILE, ocw_fetch);
}

// runtime/services/bsd_host.c: a plain-HTTP request of the game's, on port 80 of the OCW host (the only one its sockets
// may reach with backend ocw)
bool game_http_answer(uint32_t ip, uint16_t port, const void *req, size_t len, uint8_t **resp, size_t *resp_len) {
    (void)ip;
    const char *host = port == 80 ? preload_domain() : NULL;
    uint32_t id = 0;
    if (!host || !cp_http_answer(host, (const char *)req, len, resp, resp_len, &id)) return false;
    served_id = id;
    return true;
}

// RECOMP_MOD_PRELOAD_TEST_OCW=<domain>/<data id> (tests, any backend, no account): the preload's own path for one
// public course file, at start-up on a thread of its own: the fetch, the checks, the cache, the answer the game's
// GET would get (twice: the second must miss). One log line with the outcome.
static void *self_test(void *arg) {
    const char *spec = arg;
    const char *slash = strrchr(spec, '/');
    uint32_t id = slash ? (uint32_t)strtoul(slash + 1, NULL, 10) : 0;
    if (!id || slash - spec > OCW_DOMAIN_MAX) { LOG("[ocw] preload self-test: <domain>/<data id>\n"); return NULL; }
    char host[OCW_DOMAIN_MAX + 1];
    snprintf(host, sizeof host, "%.*s", (int)(slash - spec), spec);
    snprintf(preload_host, sizeof preload_host, "%s", host);
    uint32_t ids[2] = {id - 1, id};
    cp_set_list(ids, 2, 0);
    uint64_t t0 = plat_now_ns();
    cp_after(id - 1, CP_FILE, ocw_fetch);
    cp_wait(id, CP_FILE, OCW_PRELOAD_TIMEOUT_MS + 2000);
    int st = cp_state(id, CP_FILE);
    unsigned ms = (unsigned)((plat_now_ns() - t0) / 1000000);
    char req[256];
    int n = snprintf(req, sizeof req, "GET /level_data/%u HTTP/1.1\r\nHost: %s\r\nUser-Agent: test\r\n\r\n", id, host);
    uint8_t *r = NULL, *r2 = NULL;
    size_t rl = 0, rl2 = 0;
    uint32_t got = 0;
    bool a1 = cp_http_answer(host, req, (size_t)n, &r, &rl, &got);
    bool a2 = cp_http_answer(host, req, (size_t)n, &r2, &rl2, NULL);
    const uint8_t *body = a1 ? (const uint8_t *)strstr((const char *)r, "\r\n\r\n") + 4 : NULL;
    bool body_ok = body && rl - (size_t)(body - r) == CP_FILE_SIZE && cp_valid(CP_FILE, body, CP_FILE_SIZE);
    LOG("[ocw] preload self-test %s/%u: %s in %u ms; answer %s (%zu bytes, file %s, id %u); again %s: %s\n", host, id,
        st == CP_READY ? "held" : st == CP_FAILED ? "FAILED" : "not held", ms, a1 ? "yes" : "no", rl,
        body_ok ? "valid" : "INVALID", got, a2 ? "yes" : "no",
        st == CP_READY && a1 && body_ok && !a2 ? "PASS" : "FAIL");
    free(r), free(r2);
    cp_clear("self-test");
    return NULL;
}

static void preload_self_test(void) {
    static bool once;
    const char *spec = getenv("RECOMP_MOD_PRELOAD_TEST_OCW");
    if (!spec || !*spec || once) return;
    once = true;
    pthread_t th;
    if (pthread_create(&th, NULL, self_test, (void *)spec) == 0) pthread_detach(th);
}

// ---- Play History (My Info and a maker's page): the game's own GetUsers record comes from OCW's NEX side with no play
// statistics, so they are filled in from OCW's REST API: the maker's own totals, GET https://<domain>/api/makers/<id>
// (stats), or, when that has none, the sum of their played list, GET .../played?offset=0&size=10000 (public: no token
// is sent). The user record (0x1f0 bytes, world.c) holds the four numbers the screen shows at +0x100: courses played,
// attempts, courses cleared, lives lost. The maker is read from the record the game just got (maker code +0x08, UTF-16
// without dashes), so the player's own record and any maker's page work alike. A failed request leaves the record
// exactly as the game wrote it.
#define OCW_USER_RING 0x68  // the user ring in the network-session object (online.c USER_RING)
#define OCW_STATS_OFF 0x100  // courses played, attempts, courses cleared, lives lost (u32 each; checked on screen)
#define OCW_REST_TIMEOUT_MS 8000

// The maker ID ("AAA-BBB-CCC") of a user record, from its code; false when the record has none.
static bool record_maker_id(uint64_t e, char out[12]) {
    uint64_t buf = g64(e + 0x08);
    if (!buf || g32(e + 0x10) < 10) return false;
    char code[10];
    for (int i = 0; i < 9; i++) {
        uint16_t ch = *(volatile uint16_t *)(uintptr_t)(buf + 2 * i);
        if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z'))) return false;
        code[i] = (char)ch;
    }
    snprintf(out, 12, "%.3s-%.3s-%.3s", code, code + 3, code + 6);
    return true;
}

// {played, cleared, attempts, lives lost} from the maker's own record (GET /api/makers/<id>: stats plays / clears / tries /
// deaths, the server's totals over every course); false when the request fails or the record has no stats.
static bool fetch_maker_totals(const char *maker_id, uint32_t totals[4]) {
    char url[256];
    snprintf(url, sizeof url, "https://%s/api/makers/%s", g_ocw.domain, maker_id);
    HttpResponse r;
    kernel_host_wait_begin();
    int st = http_request("GET", url, "Accept: application/json\r\n", NULL, 0, OCW_REST_TIMEOUT_MS, &r);
    kernel_host_wait_end();
    Json *m = st == 200 ? json_parse(r.body, r.len) : NULL;
    http_free(&r);
    const Json *stats = m ? json_get(m, "stats") : NULL;
    bool ok = stats && stats->type == JSON_OBJ;
    if (ok) {
        totals[0] = (uint32_t)json_num(json_get(stats, "plays"), 0);
        totals[1] = (uint32_t)json_num(json_get(stats, "clears"), 0);
        totals[2] = (uint32_t)json_num(json_get(stats, "tries"), 0);
        totals[3] = (uint32_t)json_num(json_get(stats, "deaths"), 0);
    }
    json_free(m);
    return ok;
}

// Fallback: the totals of the maker's played list (the server returns at most 100 entries per request, so this undercounts
// a maker who played more).
static bool fetch_play_totals(const char *maker_id, uint32_t totals[4]) {
    char url[256];
    snprintf(url, sizeof url, "https://%s/api/makers/%s/played?offset=0&size=10000", g_ocw.domain, maker_id);
    HttpResponse r;
    kernel_host_wait_begin();  // (the calling guest thread gives its core up while it waits, as server.c does)
    int st = http_request("GET", url, "Accept: application/json\r\n", NULL, 0, OCW_REST_TIMEOUT_MS, &r);
    kernel_host_wait_end();
    if (st != 200) {
        LOG("[ocw] play history of %s: %s\n", maker_id, st ? "HTTP error" : r.error);
        http_free(&r);
        return false;
    }
    Json *list = json_parse(r.body, r.len);
    http_free(&r);
    if (!list || list->type != JSON_ARR) {
        LOG("[ocw] play history of %s: the answer is not a list\n", maker_id);
        json_free(list);
        return false;
    }
    uint32_t played = 0, cleared = 0, tries = 0, deaths = 0;
    for (const Json *p = list->child; p; p = p->next) {
        played++;
        tries += (uint32_t)json_num(json_get(p, "tries"), 0);
        deaths += (uint32_t)json_num(json_get(p, "deaths"), 0);
        if (json_bool(json_get(p, "cleared"), false) || json_num(json_get(p, "clears"), 0) > 0) cleared++;
    }
    json_free(list);
    totals[0] = played, totals[1] = cleared, totals[2] = tries, totals[3] = deaths;
    return true;
}

// After a GetUsers wrapper of the game ran (no = the network-session object, ret = what the wrapper returned): the
// first record of its user ring gets the maker's play history. Only with OCW on and the game's own request done.
void ocw_fill_play_stats(Cpu *c, uint64_t no, uint64_t ret) {
    if (!g_ocw.active) return;
    if (g32(no + 0x60) != 0 || (uint8_t)ret != wrapper_ok() || (int32_t)g32(no + OCW_USER_RING + 0x10) < 1) return;
    uint64_t keep = c->x[0];
    char id[12], again[12];
    netobj_lock(c);
    uint64_t e = g64(no + OCW_USER_RING);
    bool known = e && record_maker_id(e, id);
    netobj_unlock(c);
    uint32_t totals[4];
    if (known && (fetch_maker_totals(id, totals) || fetch_play_totals(id, totals))) {  // (outside the lock: HTTP)
        netobj_lock(c);
        e = g64(no + OCW_USER_RING);
        if (e && record_maker_id(e, again) && !strcmp(id, again))
            s32(e + OCW_STATS_OFF, totals[0]), s32(e + OCW_STATS_OFF + 4, totals[2]), s32(e + OCW_STATS_OFF + 8, totals[1]),
                s32(e + OCW_STATS_OFF + 12, totals[3]);
        netobj_unlock(c);
        LOG("[ocw] play history of %s: %u played, %u cleared, %u attempts, %u lives lost\n", id, totals[0], totals[1],
            totals[2], totals[3]);
    } else if (!known) {
        LOG("[ocw] play history: the record has no maker code\n");
    }
    c->x[0] = keep;
}

// ---- ocw-config.json written back (the overlay menu's Mods tab, below). The file stays the one place the settings
// live, so editing it by hand and setting them in the menu are the same thing.
static void config_save(void) {
    const char *path = config_path();
    FILE *f = fopen(path, "w");
    if (!f) {
        LOG("[ocw] cannot write %s\n", path);
        return;
    }
    fprintf(f, "{\n  \"domain\": \"%s\",\n  \"token\": \"%s\"", g_ocw.domain, g_ocw.token);
    if (g_ocw.ip[0]) fprintf(f, ",\n  \"ip\": \"%s\"", g_ocw.ip);
    fprintf(f, "\n}\n");
    fclose(f);
#ifndef _WIN32
    chmod(path, 0600);  // (the token is in it)
#endif
    LOG("[ocw] %s written (domain \"%s\"%s%s, token %s)\n", path, g_ocw.domain, g_ocw.ip[0] ? " at " : "", g_ocw.ip,
        g_ocw.token[0] ? "set" : "not set");
}

// ---- the Mods tab's own section (runtime/game_rows.h, drawn by runtime/ui/menu.mm), under the Online section's
// backend picker. It is always shown, so the domain and token can be entered before anything works - they used to
// have to be copied into ocw-config.json by hand. Changing any of them writes the file back, so the file stays the
// one place they live.
//
// Turning OCW on is the Online section's job (runtime/ui/online_rows.mm: "Online backend", from
// net_backend_options), not a switch of ours - two ways to choose a backend could disagree.
static bool rows_read;  // the config has been read into the buffers for the menu

int ocw_rows(GameModRow *out, int room) {
    if (net_backend_setting() != NET_BACKEND_OCW) return 0;  // (only with OpenCourse World chosen: owner, 2026-10-30)
    if (!rows_read) rows_read = true, config_load();  // (ignores its own complaints: empty fields are fine here)
    // (named fields, not positions: the row struct is shared and has grown once already)
    static const GameModRow rows[] = {
        {.label = "OpenCourse World"},  // (a section title: no value, no text)
        {.label = "Domain",
         .desc = "OCW's server, as opencourse.world gives it (at most 29 characters: the size of the slot in the "
                 "game's own code). May be an address instead. Takes effect at the next start.",
         .text = g_ocw.domain, .cap = sizeof g_ocw.domain, .hint = "not set", .max_len = OCW_DOMAIN_MAX},
        {.label = "Address",
         .desc = "Optional: the address the domain above should resolve to, instead of asking this computer's "
                 "resolver. For a self-hosted or local OCW; leave empty otherwise. Takes effect at the next start.",
         .text = g_ocw.ip, .cap = sizeof g_ocw.ip, .hint = "ask the resolver",
         .max_len = (int)sizeof g_ocw.ip - 1},
        {.label = "Token",
         .desc = "Your personal OCW token, 22 characters, from the package opencourse.world gives you. Not shown "
                 "until you activate this row, hidden again when the menu closes, and never written to a log. Takes "
                 "effect at the next start.",
         .text = g_ocw.token, .cap = sizeof g_ocw.token, .hint = "not set", .secret = true,
         .max_len = OCW_TOKEN_LEN},
    };
    int n = (int)(sizeof rows / sizeof *rows);
    if (room < n) return 0;
    for (int i = 0; i < n; i++) out[i] = rows[i];
    return n;
}

void ocw_rows_changed(void) {
    // (trim what a text field may have collected: the menu caps the length, these catch stray spaces)
    for (char *p = g_ocw.domain + strlen(g_ocw.domain); p > g_ocw.domain && p[-1] == ' ';) *--p = 0;
    for (char *p = g_ocw.ip + strlen(g_ocw.ip); p > g_ocw.ip && p[-1] == ' ';) *--p = 0;
    config_save();
    if (g_ocw.token[0] && strlen(g_ocw.token) != OCW_TOKEN_LEN)
        LOG("[ocw] the token is %zu characters; OCW's are %d - check it\n", strlen(g_ocw.token), OCW_TOKEN_LEN);
    if (net_backend_setting() == NET_BACKEND_OCW && !(g_ocw.domain[0] && g_ocw.token[0]))
        LOG("[ocw] the backend is \"ocw\" but the domain and the token are not both set yet\n");
}

// The WebSocket handshake (mods/patches.py: the one patch, in place of the game's own snprintf call). Same arguments
// as the call it replaces, in the same registers:
//   x0 buffer, x1 buffer length, x2 format (not used: the header is ours), x3 path, x4 host, x5 key, x6 protocol
// The protocol argument is used as the name of the header the token rides in, as the console mod does. Returns
// false when OCW is off, and then the generated code makes the game's own call instead.
bool smm2_ocw_header(Cpu *c) {
    if (!g_ocw.active) return false;
    char *buffer = (char *)(uintptr_t)c->x[0];
    size_t len = (size_t)c->x[1];
    const char *path = (const char *)(uintptr_t)c->x[3];
    const char *host = (const char *)(uintptr_t)c->x[4];
    const char *key = (const char *)(uintptr_t)c->x[5];
    const char *protocol = (const char *)(uintptr_t)c->x[6];
    if (!buffer || !len) return false;
    int wrote = snprintf(buffer, len,
                         "GET %s HTTP/1.1\r\n"
                         "Host: %s\r\n"
                         "Upgrade: websocket\r\n"
                         "Connection: upgrade\r\n"
                         "Sec-WebSocket-Key: %s\r\n"
                         "Sec-WebSocket-Version: 13\r\n"
                         "%s: %s\r\n"
                         "Mod-Version: %s\r\n"
                         "Game-Version: %s\r\n"
                         "\r\n",
                         path ? path : "/", host ? host : g_ocw.domain, key ? key : "", protocol ? protocol : "Token",
                         g_ocw.token, OCW_MOD_VERSION, OCW_GAME_VERSION);
    c->x[0] = (uint64_t)(uint32_t)wrote;  // (snprintf's own return: the length it would have written)
    MLOG("ocw: handshake for %s (%d bytes, token carried in \"%s\")\n", path ? path : "/", wrote,
         protocol ? protocol : "Token");
    return true;
}
