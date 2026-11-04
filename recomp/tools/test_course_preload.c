// Unit tests for the next-course preload's rules (games/smm2/mods/course_preload.c): which courses are fetched, what
// is served, joining a fetch under way, invalidation, age, the game's HTTP request answered from the cache.
// usage: make test-course-preload
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../games/smm2/mods/course_preload.h"

// ---- what the module needs from the runtime
uint64_t plat_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static int released, reacquired;
void kernel_host_wait_begin(void) { released++; }
void kernel_host_wait_end(void) { reacquired++; }
volatile int g_rt_unwind;
void rt_log(const char *fmt, ...) {
    if (!getenv("CP_TEST_LOG")) return;
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
}

static int failures;
#define CHECK(name, cond) \
    do { if (!(cond)) { printf("FAIL %s: %s (line %d)\n", name, #cond, __LINE__); failures++; return; } } while (0)

// ---- fetches
static int fetches;
static uint8_t *make_file(uint32_t id, size_t *len) {
    uint8_t *d = calloc(1, CP_FILE_SIZE);
    d[0] = 1, d[4] = 0x10, memcpy(d + 0xc, "SCDL", 4);
    memcpy(d + 0x10, &id, 4);  // (which course: checked by the tests)
    *len = CP_FILE_SIZE;
    return d;
}
static uint8_t *fetch_file(uint32_t id, size_t *len) { fetches++; return make_file(id, len); }
static uint8_t *fetch_plain(uint32_t id, size_t *len) {
    fetches++;
    uint8_t *d = calloc(1, CP_PLAIN_SIZE);
    memcpy(d, &id, 4);
    *len = CP_PLAIN_SIZE;
    return d;
}
static uint8_t *fetch_fail(uint32_t id, size_t *len) { (void)id, (void)len; fetches++; return NULL; }
static uint8_t *fetch_short(uint32_t id, size_t *len) {
    fetches++;
    uint8_t *d = make_file(id, len);
    *len = CP_FILE_SIZE - 1;
    return d;
}
static uint8_t *fetch_bad_magic(uint32_t id, size_t *len) {
    fetches++;
    uint8_t *d = make_file(id, len);
    d[0xc] = 'X';
    return d;
}
// (a fetch held until the test opens the gate)
static pthread_mutex_t gm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gc = PTHREAD_COND_INITIALIZER;
static int gate_open, gate_entered;
static uint8_t *fetch_gated(uint32_t id, size_t *len) {
    fetches++;
    pthread_mutex_lock(&gm);
    gate_entered++;
    pthread_cond_broadcast(&gc);
    while (!gate_open) pthread_cond_wait(&gc, &gm);
    pthread_mutex_unlock(&gm);
    return make_file(id, len);
}
static void gate(int open) {
    pthread_mutex_lock(&gm);
    gate_open = open;
    if (!open) gate_entered = 0;
    pthread_cond_broadcast(&gc);
    pthread_mutex_unlock(&gm);
}
static void gate_wait_entered(void) {
    pthread_mutex_lock(&gm);
    while (!gate_entered) pthread_cond_wait(&gc, &gm);
    pthread_mutex_unlock(&gm);
}
static void *open_later(void *arg) {
    usleep((useconds_t)(intptr_t)arg * 1000);
    gate(1);
    return NULL;
}

static const uint32_t L10[10] = {101, 102, 103, 104, 105, 106, 107, 108, 109, 110};
static void reset(void) {
    gate(1);
    cp_idle();
    cp_clear("test");
    cp_test_clock(NULL);
    fetches = released = reacquired = 0;
}
static uint32_t file_id(const uint8_t *d) { uint32_t v; memcpy(&v, d + 0x10, 4); return v; }

// ---- tests
static void test_valid(void) {
    size_t n;
    uint8_t *f = make_file(1, &n);
    CHECK("valid file", cp_valid(CP_FILE, f, n));
    CHECK("file short", !cp_valid(CP_FILE, f, n - 1));
    CHECK("file as plain", !cp_valid(CP_PLAIN, f, n));
    f[0] = 2;
    CHECK("file magic", !cp_valid(CP_FILE, f, n));
    f[0] = 1, f[4] = 0x20;
    CHECK("file header size", !cp_valid(CP_FILE, f, n));
    f[4] = 0x10, f[0xf] = 'm';
    CHECK("file SCDL", !cp_valid(CP_FILE, f, n));
    free(f);
    uint8_t *p = calloc(1, CP_PLAIN_SIZE);
    CHECK("valid plain", cp_valid(CP_PLAIN, p, CP_PLAIN_SIZE));
    CHECK("plain size", !cp_valid(CP_PLAIN, p, CP_PLAIN_SIZE + 1));
    CHECK("null", !cp_valid(CP_FILE, NULL, CP_FILE_SIZE));
    free(p);
    printf("ok valid\n");
}

static void test_next_in_list(void) {
    reset();
    cp_set_list(L10, 10, 0);
    CHECK("next known", cp_next_known(101) && cp_next_known(109) && !cp_next_known(110) && !cp_next_known(5));
    CHECK("after listed", cp_after(101, CP_FILE, fetch_file));
    cp_idle();
    CHECK("two ahead", cp_state(102, CP_FILE) == CP_READY && cp_state(103, CP_FILE) == CP_READY);
    CHECK("not three", cp_state(104, CP_FILE) == CP_NONE && fetches == 2);
    uint8_t *d;
    size_t n;
    int w;
    CHECK("hit", cp_take(102, CP_FILE, 1000, &d, &n, &w) == CP_HIT && n == CP_FILE_SIZE && file_id(d) == 102);
    free(d);
    CHECK("taken once", cp_take(102, CP_FILE, 0, &d, &n, &w) == CP_MISS && !d);
    CHECK("no wait on a hit", released == 0);
    cp_after(102, CP_FILE, fetch_file);  // (103 held already: only 104 more)
    cp_idle();
    CHECK("moves on", fetches == 3 && cp_state(104, CP_FILE) == CP_READY && cp_held() == 2);
    cp_after(104, CP_FILE, fetch_file);  // (a skip past 103: it goes, 105 and 106 come)
    cp_idle();
    CHECK("passed ones go", cp_state(103, 0) == CP_NONE);
    CHECK("next two", cp_state(105, 0) == CP_READY && cp_state(106, 0) == CP_READY);
    CHECK("not listed", !cp_after(999, CP_FILE, fetch_file));
    cp_after(109, CP_FILE, fetch_file);
    cp_idle();
    CHECK("list end", cp_state(110, 0) == CP_READY && cp_held() == 1);
    printf("ok next in list\n");
}

static void test_new_list(void) {
    reset();
    cp_set_list(L10, 10, 1);
    cp_after(101, CP_PLAIN, fetch_plain);
    cp_want(201, CP_PLAIN, fetch_plain);  // (our server's queue: the next list's first, not in this list)
    cp_idle();
    CHECK("three held", cp_held() == 3);
    uint32_t next[3] = {201, 202, 203};
    cp_set_list(next, 3, 1);
    CHECK("new list keeps its own", cp_held() == 1 && cp_state(201, CP_PLAIN) == CP_READY);
    cp_set_list(next, 3, 2);
    CHECK("other difficulty: all go", cp_held() == 0);
    printf("ok new list\n");
}

static void test_join(void) {
    reset();
    gate(0);
    cp_set_list(L10, 10, 0);
    cp_after(101, CP_FILE, fetch_gated);
    gate_wait_entered();
    CHECK("loading", cp_state(102, CP_FILE) == CP_LOADING && cp_state(103, CP_FILE) == CP_QUEUED);
    pthread_t th;
    pthread_create(&th, NULL, open_later, (void *)(intptr_t)200);
    uint8_t *d;
    size_t n;
    int w = 0;
    int how = cp_take(102, CP_FILE, 5000, &d, &n, &w);
    pthread_join(th, NULL);
    CHECK("joined", how == CP_JOINED && d && file_id(d) == 102 && w >= 150 && w < 4000);
    free(d);
    CHECK("core given up once", released == 1 && reacquired == 1);
    CHECK("one fetch each", (cp_idle(), fetches == 2));
    // a queued one (not started yet) is joined too
    gate(0);
    cp_after(103, CP_FILE, fetch_gated);  // (104 and 105)
    gate_wait_entered();
    pthread_create(&th, NULL, open_later, (void *)(intptr_t)100);
    how = cp_take(105, CP_FILE, 5000, &d, &n, &w);
    pthread_join(th, NULL);
    CHECK("queued joined", how == CP_JOINED && d && file_id(d) == 105);
    free(d);
    printf("ok join\n");
}

static void test_wait_limit(void) {
    reset();
    gate(0);
    cp_set_list(L10, 10, 0);
    cp_after(101, CP_FILE, fetch_gated);
    gate_wait_entered();
    uint8_t *d;
    size_t n;
    int w = 0;
    CHECK("gives up", cp_take(102, CP_FILE, 120, &d, &n, &w) == CP_MISS && !d && w >= 100 && w < 2000);
    CHECK("kept loading", cp_state(102, CP_FILE) == CP_LOADING);
    CHECK("no wait: miss", cp_take(102, CP_FILE, 0, &d, &n, &w) == CP_MISS);
    gate(1);
    cp_idle();
    CHECK("then ready", cp_state(102, CP_FILE) == CP_READY);
    CHECK("cp_wait: nothing to wait on", cp_wait(102, CP_FILE, 1000) < 50 && cp_wait(555, CP_FILE, 1000) < 50);
    g_rt_unwind = 1;  // (a reboot: waits end at once)
    gate(0);
    cp_after(102, CP_FILE, fetch_gated);  // (104)
    gate_wait_entered();
    CHECK("unwind", cp_take(104, CP_FILE, 3000, &d, &n, &w) == CP_MISS && w < 500);
    g_rt_unwind = 0;
    printf("ok wait limit\n");
}

static void test_failures(void) {
    reset();
    cp_set_list(L10, 10, 0);
    uint8_t *d;
    size_t n;
    int w;
    cp_after(101, CP_FILE, fetch_fail);
    cp_idle();
    CHECK("failed", cp_state(102, CP_FILE) == CP_FAILED);
    CHECK("no wait on a failed one", cp_take(102, CP_FILE, 5000, &d, &n, &w) == CP_MISS && w < 50);
    cp_after(101, CP_FILE, fetch_fail);
    cp_idle();
    CHECK("not again", fetches == 2);  // (a failed one stays failed for this list: no fetch again)
    reset();
    cp_set_list(L10, 10, 0);
    cp_after(101, CP_FILE, fetch_short);
    cp_idle();
    CHECK("short file", cp_state(102, CP_FILE) == CP_FAILED);
    reset();
    cp_set_list(L10, 10, 0);
    cp_after(101, CP_FILE, fetch_bad_magic);
    cp_idle();
    CHECK("bad header", cp_state(102, CP_FILE) == CP_FAILED);
    reset();
    cp_set_list(L10, 10, 0);
    cp_after(101, CP_PLAIN, fetch_plain);
    cp_idle();
    CHECK("other kind", cp_take(102, CP_FILE, 0, &d, &n, &w) == CP_MISS && cp_state(102, CP_PLAIN) == CP_READY);
    printf("ok failures\n");
}

static void test_clear_in_flight(void) {
    reset();
    gate(0);
    cp_set_list(L10, 10, 0);
    cp_after(101, CP_FILE, fetch_gated);
    gate_wait_entered();
    cp_clear("game over");
    CHECK("cleared", cp_held() == 0 && !cp_next_known(101));
    gate(1);
    cp_idle();
    CHECK("result thrown away", cp_held() == 0 && cp_state(102, 0) == CP_NONE);
    CHECK("no list: nothing", !cp_after(101, CP_FILE, fetch_file));
    printf("ok clear in flight\n");
}

static uint64_t fake_now;
static uint64_t fake_clock(void) { return fake_now; }
static void test_age(void) {
    reset();
    fake_now = 1000;
    cp_test_clock(fake_clock);
    cp_set_list(L10, 10, 0);
    cp_after(101, CP_FILE, fetch_file);
    cp_idle();
    fake_now += 14 * 60;
    CHECK("14 min: kept", cp_state(102, CP_FILE) == CP_READY);
    fake_now += 2 * 60;
    uint8_t *d;
    size_t n;
    int w;
    CHECK("16 min: gone", cp_take(102, CP_FILE, 0, &d, &n, &w) == CP_MISS && cp_held() == 0);
    cp_test_clock(NULL);
    printf("ok age\n");
}

static void test_capacity(void) {
    reset();
    cp_set_list(L10, 10, 0);
    CHECK("want 1", cp_want(301, CP_FILE, fetch_file));
    CHECK("want again: no", !cp_want(301, CP_FILE, fetch_file));
    CHECK("want 2", cp_want(302, CP_FILE, fetch_file) && cp_want(303, CP_FILE, fetch_file));
    CHECK("full", !cp_want(304, CP_FILE, fetch_file));
    cp_idle();
    CHECK("three", cp_held() == CP_SLOTS && fetches == 3);
    printf("ok capacity\n");
}

static void test_http(void) {
    reset();
    uint32_t ids[2] = {1000024641, 1000024642};
    cp_set_list(ids, 2, 0);
    cp_after(ids[0], CP_FILE, fetch_file);
    cp_idle();
    const char *host = "server.opencourse.world";
    uint8_t *r = NULL;
    size_t rl = 0;
    uint32_t id = 0;
    const char *ok = "GET /level_data/1000024642 HTTP/1.1\r\nHost: server.opencourse.world\r\nAccept: */*\r\n\r\n";
    CHECK("other host", !cp_http_answer("example.org", ok, strlen(ok), &r, &rl, &id));
    const char *wrong[] = {
        "GET /level_data/1000024642 HTTP/1.1\r\nAccept: */*\r\n\r\n",                                // no Host
        "GET /level_data/1000024642 HTTP/1.1\r\nHost: server.opencourse.world.evil\r\n\r\n",  // longer host
        "POST /level_data/1000024642 HTTP/1.1\r\nHost: server.opencourse.world\r\n\r\n",
        "GET /level_data/10000x4642 HTTP/1.1\r\nHost: server.opencourse.world\r\n\r\n",
        "GET /level_data/ HTTP/1.1\r\nHost: server.opencourse.world\r\n\r\n",
        "GET /level_data/99999999999 HTTP/1.1\r\nHost: server.opencourse.world\r\n\r\n",
        "GET /one_screen_thumbnail/1000024642 HTTP/1.1\r\nHost: server.opencourse.world\r\n\r\n",
        "GET /level_data/1000024643 HTTP/1.1\r\nHost: server.opencourse.world\r\n\r\n",          // not held
    };
    for (size_t i = 0; i < sizeof wrong / sizeof *wrong; i++)
        CHECK("not answered", !cp_http_answer(host, wrong[i], strlen(wrong[i]), &r, &rl, &id));
    CHECK("still held", cp_state(ids[1], CP_FILE) == CP_READY);
    const char *lower = "GET /level_data/1000024642 HTTP/1.1\r\nhost: Server.OpenCourse.World:80\r\n\r\n";
    CHECK("answered", cp_http_answer(host, lower, strlen(lower), &r, &rl, &id) && id == ids[1]);
    const char *hdr = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: 376832\r\n"
                      "Connection: close\r\n\r\n";
    CHECK("header", rl == strlen(hdr) + CP_FILE_SIZE && !memcmp(r, hdr, strlen(hdr)));
    CHECK("body", cp_valid(CP_FILE, r + strlen(hdr), CP_FILE_SIZE) && file_id(r + strlen(hdr)) == ids[1]);
    free(r);
    CHECK("once", !cp_http_answer(host, ok, strlen(ok), &r, &rl, &id));
    // (a plain course of our server is never an OCW answer)
    cp_set_list(ids, 2, 0);
    cp_want(ids[1], CP_PLAIN, fetch_plain);
    cp_idle();
    CHECK("plain not served", !cp_http_answer(host, ok, strlen(ok), &r, &rl, &id));
    printf("ok http\n");
}

// Two course-list entries swapped in place (the OCW reserve, endless.c): what each holds moves to the other, and a
// pointer into the entry itself (a string object's buffer inside the entry) is moved with it, so it still points into
// the entry it is in; a pointer elsewhere moves as it is.
static void test_swap_entries(void) {
    enum { N = 0x548 };
    static _Alignas(8) uint8_t ring[2 * N];
    static uint8_t outside;
    uint8_t *a = ring, *b = ring + N;
    uint64_t v;
    memset(ring, 0, sizeof ring);
    v = 111, memcpy(a + 0x00, &v, 8);                          // data id
    v = (uint64_t)(uintptr_t)(a + 0x100), memcpy(a + 0x10, &v, 8);  // the code's buffer, inside the entry
    v = (uint64_t)(uintptr_t)&outside, memcpy(a + 0x20, &v, 8);     // something elsewhere
    memcpy(a + 0x100, "AAA", 4);
    v = 222, memcpy(b + 0x00, &v, 8);
    v = (uint64_t)(uintptr_t)(b + 0x100), memcpy(b + 0x10, &v, 8);
    v = 0, memcpy(b + 0x20, &v, 8);
    memcpy(b + 0x100, "BBB", 4);
    v = (uint64_t)(uintptr_t)(b + N), memcpy(b + 0x28, &v, 8);  // just past the entry: not inside it
    CHECK("swap: done", cp_swap_entries(a, b, N));
    memcpy(&v, a + 0x00, 8);
    CHECK("swap: ids", v == 222);
    memcpy(&v, b + 0x00, 8);
    CHECK("swap: ids", v == 111);
    memcpy(&v, a + 0x10, 8);
    CHECK("swap: a's buffer in a", v == (uint64_t)(uintptr_t)(a + 0x100));
    memcpy(&v, b + 0x10, 8);
    CHECK("swap: b's buffer in b", v == (uint64_t)(uintptr_t)(b + 0x100));
    CHECK("swap: contents", !memcmp(a + 0x100, "BBB", 4) && !memcmp(b + 0x100, "AAA", 4));
    memcpy(&v, b + 0x20, 8);
    CHECK("swap: outside pointer moves as it is", v == (uint64_t)(uintptr_t)&outside);
    memcpy(&v, a + 0x28, 8);
    CHECK("swap: past the end is not rebased", v == (uint64_t)(uintptr_t)(b + N));
    cp_swap_entries(a, b, N);
    memcpy(&v, a + 0x10, 8);
    CHECK("swap back", v == (uint64_t)(uintptr_t)(a + 0x100) && !memcmp(a + 0x100, "AAA", 4));
    printf("ok swap entries\n");
}

int main(void) {
    test_valid();
    test_next_in_list();
    test_new_list();
    test_join();
    test_wait_limit();
    test_failures();
    test_clear_in_flight();
    test_age();
    test_capacity();
    test_http();
    test_swap_entries();
    reset();
    if (failures) { printf("%d FAILED\n", failures); return 1; }
    printf("course preload: all passed\n");
    return 0;
}
