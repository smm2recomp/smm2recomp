// The next Endless courses' files, fetched ahead (course_preload.h). One host thread fetches; the entries are the
// queue (oldest queued first). A fetch's result goes to the entry that asked for it (its ticket): an entry dropped
// meanwhile (a new list, a clear) throws the result away. Waiting callers sleep on the condvar, their core given up.
#include "course_preload.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

uint64_t plat_now_ns(void);  // (runtime/platform.h)
void kernel_host_wait_begin(void);
void kernel_host_wait_end(void);
extern volatile int g_rt_unwind;
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define LOG(...) rt_log(__VA_ARGS__)
#define VLOG(...) do { if (cp_verbose) LOG("[mod] " __VA_ARGS__); } while (0)

volatile bool cp_verbose;
void (*cp_name)(uint32_t id, char out[12]);

typedef struct {
    uint32_t id;
    int kind, state;
    uint64_t ticket, queued_at;  // (queued_at: the order of the queue)
    uint64_t t_start, t_done;    // ns (fetch), s (age)
    CpFetch fetch;
    uint8_t *data;
    size_t len;
} Entry;

static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;  // (any change: entries, the queue, the fetcher idle)
static Entry E[CP_SLOTS];
static uint32_t list[64];
static int nlist, list_d = -1;
static uint64_t tickets;
static bool started, busy;

static uint64_t mono_s(void) { return plat_now_ns() / 1000000000ull; }
static uint64_t (*clock_s)(void) = mono_s;
void cp_test_clock(uint64_t (*now_s)(void)) { clock_s = now_s ? now_s : mono_s; }

static void name_of(uint32_t id, char out[12]) {
    if (cp_name) cp_name(id, out);
    else snprintf(out, 12, "%u", id);
}

static void drop(Entry *e) {  // (lk held)
    free(e->data);
    memset(e, 0, sizeof *e);
}

static int list_index(uint32_t id) {  // (lk held)
    for (int i = 0; i < nlist; i++)
        if (list[i] == id) return i;
    return -1;
}

static Entry *find(uint32_t id, int kind) {  // (lk held) also ages entries out
    uint64_t now = clock_s();
    for (int i = 0; i < CP_SLOTS; i++)
        if (E[i].state && now - E[i].t_done > CP_MAX_AGE_S && E[i].state != CP_LOADING) {
            char code[12];
            name_of(E[i].id, code);
            VLOG("preload %s: dropped (older than %d min)\n", code, CP_MAX_AGE_S / 60);
            drop(&E[i]);
        }
    for (int i = 0; i < CP_SLOTS; i++)
        if (E[i].state && E[i].id == id && (!kind || E[i].kind == kind)) return &E[i];
    return NULL;
}

static void *fetcher(void *arg);

static bool add(uint32_t id, int kind, CpFetch fetch) {  // (lk held) queue a fetch of id unless it is held already
    if (!id || !fetch || find(id, 0)) return false;
    Entry *e = NULL;
    for (int i = 0; i < CP_SLOTS && !e; i++)
        if (!E[i].state) e = &E[i];
    if (!e) return false;  // (full: the ones held are ahead of the player)
    e->id = id, e->kind = kind, e->fetch = fetch, e->state = CP_QUEUED;
    e->ticket = ++tickets, e->queued_at = tickets, e->t_done = clock_s();  // (a stuck queue ages out too)
    if (!started) {
        pthread_t th;
        started = pthread_create(&th, NULL, fetcher, NULL) == 0;
        if (started) pthread_detach(th);
    }
    pthread_cond_broadcast(&cv);
    return true;
}

static void *fetcher(void *arg) {
    (void)arg;
    pthread_mutex_lock(&lk);
    for (;;) {
        Entry *e = NULL;
        for (int i = 0; i < CP_SLOTS; i++)
            if (E[i].state == CP_QUEUED && (!e || E[i].queued_at < e->queued_at)) e = &E[i];
        if (!e) {
            busy = false;
            pthread_cond_broadcast(&cv);
            pthread_cond_wait(&cv, &lk);
            continue;
        }
        busy = true;
        e->state = CP_LOADING, e->t_start = plat_now_ns();
        uint64_t ticket = e->ticket, t0 = e->t_start;
        uint32_t id = e->id;
        int kind = e->kind;
        CpFetch fetch = e->fetch;
        pthread_mutex_unlock(&lk);
        size_t len = 0;
        uint8_t *d = fetch(id, &len);  // (no lock held: the network)
        bool ok = d && cp_valid(kind, d, len);
        uint64_t ms = (plat_now_ns() - t0) / 1000000;
        char code[12];
        name_of(id, code);
        static int failures;  // (the first one always, the rest with the log switch: a changed URL fails each time)
        if (!ok && (failures++ == 0 || cp_verbose))
            LOG("[mod] preload %s: %s after %llu ms (the game downloads it itself)\n", code,
                !d ? "fetch failed" : "not a course file (size / header)", (unsigned long long)ms);
        pthread_mutex_lock(&lk);
        e = NULL;
        for (int i = 0; i < CP_SLOTS; i++)
            if (E[i].state == CP_LOADING && E[i].ticket == ticket) e = &E[i];
        if (!e) {
            VLOG("preload %s: no longer wanted (%zu bytes in %llu ms)\n", code, len, (unsigned long long)ms);
            free(d);
        } else if (ok) {
            e->state = CP_READY, e->data = d, e->len = len, e->t_done = clock_s();
            VLOG("preload %s: %zu bytes in %llu ms\n", code, len, (unsigned long long)ms);
        } else {
            free(d);
            e->state = CP_FAILED, e->t_done = clock_s();
        }
        pthread_cond_broadcast(&cv);
    }
    return NULL;
}

bool cp_valid(int kind, const uint8_t *d, size_t n) {
    if (!d) return false;
    if (kind == CP_PLAIN) return n == CP_PLAIN_SIZE;
    if (kind != CP_FILE || n != CP_FILE_SIZE) return false;
    uint32_t magic = (uint32_t)d[0] | (uint32_t)d[1] << 8 | (uint32_t)d[2] << 16 | (uint32_t)d[3] << 24;
    return magic == 1 && d[4] == 0x10 && d[5] == 0 && !memcmp(d + 0xc, "SCDL", 4);
}

void cp_set_list(const uint32_t *ids, int n, int difficulty) {
    pthread_mutex_lock(&lk);
    if (n > (int)(sizeof list / sizeof *list)) n = (int)(sizeof list / sizeof *list);
    bool other = difficulty != list_d;
    nlist = n < 0 ? 0 : n, list_d = difficulty;
    memcpy(list, ids, (size_t)nlist * sizeof *list);
    int kept = 0;
    for (int i = 0; i < CP_SLOTS; i++) {  // (a new list keeps what it still plays: our server's look past the old end)
        if (!E[i].state) continue;
        if (other || list_index(E[i].id) < 0) drop(&E[i]);
        else kept++;
    }
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&lk);
    VLOG("preload: list of %d courses (difficulty %d), %d held kept\n", nlist, difficulty, kept);
}

bool cp_after(uint32_t id, int kind, CpFetch fetch) {
    pthread_mutex_lock(&lk);
    int at = list_index(id);
    if (at >= 0) {
        for (int i = 0; i < CP_SLOTS; i++) {  // (the courses up to this one are behind the player)
            int k = E[i].state ? list_index(E[i].id) : -1;
            if (E[i].state && k >= 0 && k <= at) drop(&E[i]);
        }
        for (int k = 1; k <= CP_DEPTH && at + k < nlist; k++) add(list[at + k], kind, fetch);
    }
    pthread_mutex_unlock(&lk);
    return at >= 0;
}

bool cp_want(uint32_t id, int kind, CpFetch fetch) {
    pthread_mutex_lock(&lk);
    bool v = add(id, kind, fetch);
    pthread_mutex_unlock(&lk);
    return v;
}

bool cp_next_known(uint32_t id) {
    pthread_mutex_lock(&lk);
    int at = list_index(id);
    bool v = at >= 0 && at + 1 < nlist;
    pthread_mutex_unlock(&lk);
    return v;
}

int cp_state(uint32_t id, int kind) {
    pthread_mutex_lock(&lk);
    Entry *e = find(id, kind);
    int v = e ? e->state : CP_NONE;
    pthread_mutex_unlock(&lk);
    return v;
}

// Wait (lk held) while id's fetch is queued or under way, at most wait_ms, the core given up; the entry after.
static Entry *wait_for(uint32_t id, int kind, int wait_ms, int *waited) {
    uint64_t t0 = plat_now_ns(), end = t0 + (uint64_t)(wait_ms > 0 ? wait_ms : 0) * 1000000;
    bool released = false;
    Entry *e;
    while ((e = find(id, kind)) && (e->state == CP_QUEUED || e->state == CP_LOADING) && wait_ms > 0 && !g_rt_unwind) {
        uint64_t now = plat_now_ns();
        if (now >= end) break;
        if (!released) {  // (not under lk: the kernel's lock comes first)
            pthread_mutex_unlock(&lk);
            kernel_host_wait_begin();
            pthread_mutex_lock(&lk);
            released = true;
            continue;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t slice = end - now < 100000000ull ? end - now : 100000000ull;  // (slices: g_rt_unwind is seen)
        ts.tv_nsec += (long)slice;
        ts.tv_sec += ts.tv_nsec / 1000000000L, ts.tv_nsec %= 1000000000L;
        pthread_cond_timedwait(&cv, &lk, &ts);
    }
    if (released) {
        pthread_mutex_unlock(&lk);
        kernel_host_wait_end();
        pthread_mutex_lock(&lk);
        e = find(id, kind);
    }
    *waited = (int)((plat_now_ns() - t0) / 1000000);
    return e;
}

int cp_take(uint32_t id, int kind, int wait_ms, uint8_t **data, size_t *len, int *waited_ms) {
    int waited = 0, how = CP_MISS;
    *data = NULL, *len = 0;
    pthread_mutex_lock(&lk);
    Entry *e = find(id, kind);
    bool was_ready = e && e->state == CP_READY;
    if (e && !was_ready) e = wait_for(id, kind, wait_ms, &waited);
    if (e && e->state == CP_READY) {
        how = was_ready ? CP_HIT : CP_JOINED;
        *data = e->data, *len = e->len;
        e->data = NULL;
        drop(e);
    }
    pthread_mutex_unlock(&lk);
    if (waited_ms) *waited_ms = waited;
    return how;
}

int cp_wait(uint32_t id, int kind, int wait_ms) {
    int waited = 0;
    pthread_mutex_lock(&lk);
    wait_for(id, kind, wait_ms, &waited);
    pthread_mutex_unlock(&lk);
    return waited;
}

void cp_clear(const char *why) {
    pthread_mutex_lock(&lk);
    int n = 0;
    for (int i = 0; i < CP_SLOTS; i++)
        if (E[i].state) drop(&E[i]), n++;
    nlist = 0, list_d = -1;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&lk);
    if (n) VLOG("preload: %d dropped (%s)\n", n, why);
}

void cp_idle(void) {
    pthread_mutex_lock(&lk);
    for (;;) {
        bool queued = false;
        for (int i = 0; i < CP_SLOTS; i++) queued |= E[i].state == CP_QUEUED;
        if (!busy && !queued) break;
        pthread_cond_wait(&cv, &lk);
    }
    pthread_mutex_unlock(&lk);
}

int cp_held(void) {
    pthread_mutex_lock(&lk);
    int n = 0;
    for (int i = 0; i < CP_SLOTS; i++) n += E[i].state != CP_NONE;
    pthread_mutex_unlock(&lk);
    return n;
}

// ---- the game's own HTTP request for a course file (OCW: the URL PrepareGetObject hands out), answered from here

static bool prefix_ci(const char *s, const char *p) {
    for (; *p; s++, p++)
        if ((*s | 0x20) != (*p | 0x20)) return false;
    return true;
}

bool cp_http_answer(const char *host, const char *req, size_t len, uint8_t **resp, size_t *rlen, uint32_t *id) {
    char buf[2048];
    if (!host || !*host || len < 20) return false;
    size_t n = len < sizeof buf - 1 ? len : sizeof buf - 1;
    memcpy(buf, req, n);
    buf[n] = 0;
    static const char pre[] = "GET /level_data/";
    if (strncmp(buf, pre, sizeof pre - 1)) return false;
    const char *p = buf + sizeof pre - 1;
    uint64_t v = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9' && digits < 11) v = v * 10 + (uint64_t)(*p++ - '0'), digits++;
    if (!digits || !v || v > 0xffffffffull || strncmp(p, " HTTP/1.", 8)) return false;
    // (the Host header: this host's own request only)
    const char *h = NULL;
    for (const char *q = strstr(buf, "\r\n"); q && !h; q = strstr(q + 2, "\r\n"))
        if (prefix_ci(q + 2, "host:")) h = q + 7;
    if (!h) return false;
    while (*h == ' ' || *h == '\t') h++;
    size_t hl = strlen(host);
    if (!prefix_ci(h, host)) return false;
    const char *end = h + hl;
    if (!strncmp(end, ":80", 3)) end += 3;
    if (*end != '\r' && *end != ' ') return false;
    uint8_t *d;
    size_t dl;
    if (cp_take((uint32_t)v, CP_FILE, 0, &d, &dl, NULL) == CP_MISS) return false;
    char hdr[160];
    int hn = snprintf(hdr, sizeof hdr,
                      "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %zu\r\n"
                      "Connection: close\r\n\r\n", dl);
    uint8_t *out = malloc((size_t)hn + dl);
    if (!out) { free(d); return false; }
    memcpy(out, hdr, (size_t)hn);
    memcpy(out + hn, d, dl);
    free(d);
    *resp = out, *rlen = (size_t)hn + dl;
    if (id) *id = (uint32_t)v;
    return true;
}

static void rebase(uint8_t *buf, size_t n, uintptr_t from, uintptr_t to) {  // (pointers into [from, from+n) -> to)
    for (size_t o = 0; o + 8 <= n; o += 8) {
        uint64_t v;
        memcpy(&v, buf + o, 8);
        if (v >= from && v < from + n) v = v - from + to, memcpy(buf + o, &v, 8);
    }
}

bool cp_swap_entries(void *a, void *b, size_t n) {
    uint8_t *ta = malloc(n), *tb = malloc(n);
    if (!ta || !tb) { free(ta), free(tb); return false; }
    memcpy(ta, a, n), memcpy(tb, b, n);
    rebase(ta, n, (uintptr_t)a, (uintptr_t)b);
    rebase(tb, n, (uintptr_t)b, (uintptr_t)a);
    memcpy(a, tb, n), memcpy(b, ta, n);
    free(ta), free(tb);
    return true;
}
