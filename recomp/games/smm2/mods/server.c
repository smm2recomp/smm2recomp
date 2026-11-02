// Calls to our course server (core/crates/server) with the account's token, plus the shared game-state helpers.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pthread.h>
#include <unistd.h>

#include "mod.h"
#include "../../../runtime/kernel.h"
#include "../../../runtime/net/http.h"
#include "../../../runtime/net/json.h"

static __thread int slow_ms;  // (srv_call_slow: this thread's request may take this long)
static __thread int quiet;    // (srv_quiet_thread: this thread's answers aren't logged)

void srv_quiet_thread(void) { quiet = 1; }

// A request the game waits on (a course's data before the course: SMM2 holds a black screen meanwhile, 20 s at most,
// retried once) says so over the game once it has taken a while, so it doesn't look like a freeze (owner, 2026-10-30:
// "I got the sense the server wasn't replying"). Only those requests: srv_waited(true) around them (endless.c), not
// the background ones (prefetches, pictures). A watchdog thread looks twice a second.
#define SLOW_NOTICE_MS 4000
extern void menu_notice(const char *text, double seconds);  // runtime/ui/menu.mm
extern uint64_t plat_now_ns(void);
static _Atomic uint64_t g_wait_since;  // when the request the game waits on started (ns; 0: none)
static _Thread_local bool t_waited;
static void *slow_watch(void *arg) {
    (void)arg;
    for (;;) {
        usleep(500000);
        uint64_t since = g_wait_since;
        if (since && plat_now_ns() - since > (uint64_t)SLOW_NOTICE_MS * 1000000)
            menu_notice("Waiting for the course server...", 1.0);
    }
    return NULL;
}
static void watch_start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, slow_watch, NULL) == 0) pthread_detach(t);
}
void srv_waited(bool on) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, watch_start);
    t_waited = on;
}

static int request_typed(const char *method, const char *path, const char *body, size_t blen, const char *type,
                         HttpResponse *r) {
    char url[512], hdr[256];
    if (mod_local_runs()) {  // (backend ocw: the runs are kept in the game, ocw_runs.c)
        extern int ocw_runs_request(const char *method, const char *path, const char *body, size_t blen, HttpResponse *r);
        int st = ocw_runs_request(method, path, body, blen, r);
        if (!quiet) MLOG("%s %s -> %d (%zu bytes)\n", method, path, st, r->len);
        return st;
    }
    mod_server_ready();  // (courses=tgrcode: the built-in server starts before the first request)
    snprintf(url, sizeof url, "%s%s", g_mod.server, path);
    hdr[0] = 0;
    if (g_mod.token[0]) snprintf(hdr, sizeof hdr, "Authorization: Bearer %s\r\n", g_mod.token);
    if (body) {
        size_t n = strlen(hdr);
        snprintf(hdr + n, sizeof hdr - n, "Content-Type: %s\r\n", type);
    }
    // (the calling guest thread gives its core up while it waits, as the console's network wait does: the game's
    // worker threads share cores with the one that draws, and a picture the server waited seconds for froze the
    // frame for as long, 4 s at a time while Course World's lists scrolled)
    kernel_host_wait_begin();
    if (t_waited) g_wait_since = plat_now_ns();
    int st;
    // (tests: RECOMP_MOD_TEST_DOWN=<path part>[:<from n-th>]: such requests wait out the timeout and fail, as with a
    // server that stopped answering - from the n-th match on, so a run can start first)
    static const char *down = (const char *)-1;
    if (down == (const char *)-1) down = getenv("RECOMP_MOD_TEST_DOWN");
    static int down_seen;
    char part[128] = "";
    int from = 1;
    if (down) sscanf(down, "%127[^:]:%d", part, &from);
    if (down && part[0] && strstr(path, part) && ++down_seen >= from) {
        usleep((useconds_t)(slow_ms ? slow_ms : g_mod.timeout_ms) * 1000);
        memset(r, 0, sizeof *r);
        snprintf(r->error, sizeof r->error, "timed out (RECOMP_MOD_TEST_DOWN)");
        st = 0;
    } else {
        st = http_request(method, url, hdr, body, blen, slow_ms ? slow_ms : g_mod.timeout_ms, r);
    }
    if (t_waited) g_wait_since = 0;
    kernel_host_wait_end();
    bool text = body && !strcmp(type, "application/json");
    if (!st) LOG("[mod] %s %s: %s\n", method, path, r->error);
    else if (!quiet)
        MLOG("%s %s -> %d (%zu bytes)%s%.300s\n", method, path, st, r->len, text ? " body " : "", text ? body : "");
    return st;
}

static int request(const char *method, const char *path, const char *body, size_t blen, HttpResponse *r) {
    return request_typed(method, path, body, blen, "application/json", r);
}

Json *srv_send_bytes(const char *method, const char *path, const void *data, size_t len, int *status) {
    HttpResponse r;
    int st = request_typed(method, path, data, len, "application/octet-stream", &r);
    if (status) *status = st;
    Json *j = st >= 200 && st < 300 ? json_parse(r.body, r.len) : NULL;
    if (st && !j) LOG("[mod] %s %s: HTTP %d %.200s\n", method, path, st, r.body ? r.body : "");
    http_free(&r);
    return j;
}

Json *srv_post_bytes(const char *path, const void *data, size_t len, int *status) {
    return srv_send_bytes("POST", path, data, len, status);
}

Json *srv_call(const char *method, const char *path, const char *body, int *status) {
    HttpResponse r;
    int st = request(method, path, body, body ? strlen(body) : 0, &r);
    if (status) *status = st;
    Json *j = NULL;
    if (st >= 200 && st < 300) {
        j = json_parse(r.body, r.len);
        if (!j) LOG("[mod] %s %s: answer is not JSON\n", method, path);
    } else if (st) {
        LOG("[mod] %s %s: HTTP %d %.200s\n", method, path, st, r.body ? r.body : "");
    }
    if (j && g_mod.log) MLOG("  answer %.600s\n", r.body ? r.body : "");
    http_free(&r);
    return j;
}

// A request the server may take long over (TGRCode: the first courses of a difficulty, about a minute).
Json *srv_call_slow(const char *method, const char *path, const char *body, int *status, int timeout_ms) {
    slow_ms = timeout_ms;
    Json *j = srv_call(method, path, body, status);
    slow_ms = 0;
    return j;
}

void *srv_get_bytes(const char *path, size_t *len) {
    int st = 0;
    void *d = srv_get_bytes_status(path, len, &st);
    if (!d && st == 404) MLOG("GET %s: not there\n", path);
    else if (!d && st) LOG("[mod] GET %s: HTTP %d\n", path, st);
    return d;
}

// ... and the answer's status (0: none came)
void *srv_get_bytes_status(const char *path, size_t *len, int *status) {
    HttpResponse r;
    int st = request("GET", path, NULL, 0, &r);
    if (status) *status = st;
    if (st != 200) {
        http_free(&r);
        return NULL;
    }
    *len = r.len;
    return r.body;  // (the caller frees it)
}

// ... one the server may wait on (a listed TGRCode course it fetches first: TGRCode allows about one request in 5-7 s,
// so its turn can come after the default timeout and the game said "course ID not found" on the first try)
void *srv_get_bytes_slow(const char *path, size_t *len, int timeout_ms) {
    slow_ms = timeout_ms;
    void *d = srv_get_bytes(path, len);
    slow_ms = 0;
    return d;
}

// ---- game state

#define NETOBJ_HOLDER MAIN_ADDR(0x2a37380) // + 0x3e0: the network-session object
#define WRAPPER_TRUE MAIN_ADDR(0x2a17ad8) // the bytes the request functions return (success / failure)
#define WRAPPER_FALSE MAIN_ADDR(0x2a17ad9)
#define LOCK_FN MAIN_ADDR(0x292410)    // the lock / unlock the wrappers take on NetObj + 0x20
#define UNLOCK_FN MAIN_ADDR(0x292430)

uint64_t netobj(void) { return g64(g64(NETOBJ_HOLDER) + 0x3e0); }
uint8_t wrapper_ok(void) { return g8(WRAPPER_TRUE); }
uint8_t wrapper_fail(void) { return g8(WRAPPER_FALSE); }

void guest_call_orig(Cpu *c, uint64_t addr) {
    GuestFn f = dispatch_lookup(addr);
    if (!f) FATAL("[mod] no recompiled function at %llx\n", (unsigned long long)addr);
    uint64_t lr = c->x[30];
    f(c);
    c->x[30] = lr;
}

void netobj_lock(Cpu *c) {
    uint64_t save0 = c->x[0];
    c->x[0] = netobj() + 0x20;
    guest_call(c, LOCK_FN);
    c->x[0] = save0;
}

void netobj_unlock(Cpu *c) {
    uint64_t save0 = c->x[0];
    c->x[0] = netobj() + 0x20;
    guest_call(c, UNLOCK_FN);
    c->x[0] = save0;
}
