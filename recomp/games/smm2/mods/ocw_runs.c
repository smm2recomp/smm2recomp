// The Endless Challenge's and 100 Mario's runs for backend=ocw (mods/hybrid.h): OCW has neither, so the runs are kept
// here, in the game, and the courses are drawn from OCW's public course list. The mod's Endless overrides (endless.c)
// speak to a course server in a small REST dialect (core/crates/server /api/endless ...); with backend=ocw
// server.c hands those requests to ocw_runs_request() instead of a socket, and this file answers them:
//
//   GET  /api/me                       the player's name
//   GET  /api/endless                  the four runs of each mode (runs: SMM2's; hundred_runs: 100 Mario's) and the best clears
//   POST /api/endless/<d>/start        New Game (?ruleset=hundred | smm2): the start lives, a fresh queue of courses
//   GET  /api/endless/<d>/next?count=  the first courses of the run's queue
//   POST /api/endless/<d>/result       {"code", "result": clear | death | start_over | skip | broken, "pending"}
//   POST /api/endless/<d>/suspend | finish
//   GET  /api/endless/<d>/history      the run's clears and skips (the map's dots, the ending's credits)
//   GET  /api/rankings/hundred_<d>     completed runs of this difficulty (unlocks Expert and Super Expert)
//   GET  /api/rankings/endless_<d>     SMM2's best clears on this difficulty
//   GET  /api/courses/<code>           a course served in this session (its record)
//
// Both Endless modes are served, each in its own run slots (the request's ruleset=): 100 Mario and SMM2's Endless
// Challenge. The rules are the server's (core/crates/modes/src/hundred.rs, endless.rs).
//   100 Mario: 100 lives; a death costs one; a clear adds the banked 1-Ups (at most 3); a skip is free; the run is
//   complete at the difficulty's goal (8, 16, 16, 6 clears) and over at no lives. Courses by SMM1's fail-rate bands
//   (Easy 0-34, Normal 0-74, Expert 75-95, Super Expert 96-100) over the courses rated by their attempts, widening to
//   any rated one, then unrated ones (the server's pick_hundred).
//   SMM2: 5 lives (at most 99), no goal; a clear adds the banked 1-Ups; a skip costs a life only during the miss
//   demo; the best clears per difficulty are kept. Courses by clear-rate band (Easy 34% and up, Normal 12%, Expert
//   3%, else Super Expert), widening to the bands one, two, three steps away, then unrated ones (pick).
// The courses come from OCW's /api/latest/courses. Their files are the game's own downloads from OCW (endless.c leaves
// GetObject to the game), so a course is only ever named here by its code.
//
// The runs, the completed-run counts and the best clears are kept in ocw-100mario.json in the data folder.
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mod.h"
#include "ocw.h"
#include "../../../runtime/net/http.h"
#include "../../../runtime/net/json.h"

#define QUEUE_AHEAD 12      // courses held ahead of the run's position
#define HISTORY_MAX 48
#define SEEN_MAX 400        // recently played courses remembered across runs
#define FRESH_WANTED 6      // unplayed courses of the run's own tier to queue before settling for others
#define FETCHES_PER_REFILL 2
#define POOL_MAX 1500
#define PAGE 100            // OCW's list page size
#define PAGES_PER_FILL 3    // pages fetched when a run needs courses
#define MIN_RATED 20        // attempts before a course has a fail rate
#define PENDING_CAP 3
#define REST_TIMEOUT_MS 20000
#define SAVE_FILE "ocw-100mario.json"

static const char *const DIFF[4] = {"easy", "normal", "expert", "super_expert"};
static const int GOAL[4] = {8, 16, 16, 6};
static const int BAND[4][2] = {{0, 34}, {0, 74}, {75, 95}, {96, 100}};

typedef struct {
    char code[12];
    char name[66], maker[24], maker_id[16];
    int style, theme;           // 0 SMB1 .. 4 3D World; the course format's theme number
    unsigned uploaded, plays, attempts, clears, likes;
    bool endless;               // OCW's own "may appear in Endless"
} Course;

typedef struct {
    Course c;
    bool clear;                 // else skipped
    unsigned seq;
} Done;

typedef struct {
    bool active, complete, exists;
    unsigned lives, clears, coins, score, seq;
    Course queue[QUEUE_AHEAD];
    int nqueue;
    Done hist[HISTORY_MAX];
    int nhist;
} Run;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static Run runs[4];        // 100 Mario's run per difficulty
static Run smm2_runs[4];   // SMM2's Endless Challenge's
static unsigned completed[4];    // completed 100 Mario runs
static unsigned best_clears[4];  // SMM2's best clears
static Run *slots(bool hundred) { return hundred ? runs : smm2_runs; }
static int goal_of(bool hundred, int d) { return hundred ? GOAL[d] : 0; }
// The band of a rated course by its clear rate (SMM2's: Easy 34% and up, Normal 12%, Expert 3%, else Super Expert);
// -1: unrated
static int smm2_band(const Course *c) {
    if (c->attempts < MIN_RATED) return -1;
    unsigned bp = (unsigned)((c->clears < c->attempts ? c->clears : c->attempts) * 10000ull / c->attempts);
    return bp >= 3400 ? 0 : bp >= 1200 ? 1 : bp >= 300 ? 2 : 3;
}
static bool loaded;

// ---- OCW's course list

static Course *pool;
static int npool, cappool;
static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long long rng_state;

static unsigned rnd(unsigned n) {
    if (!rng_state) rng_state = (unsigned long long)time(NULL) * 6364136223846793005ull + 1442695040888963407ull;
    rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull;
    return n ? (unsigned)((rng_state >> 33) % n) : 0;
}

static int style_of(const char *s) {
    static const char *const names[5] = {"SMB1", "SMB3", "SMW", "NSMBU", "SM3DW"};
    for (int i = 0; i < 5; i++)
        if (!strcmp(s, names[i])) return i;
    return 0;
}

static int theme_of(const char *s) {
    static const char *const names[10] = {"Ground", "Underground", "Castle", "Airship", "Underwater",
                                          "Ghost House", "Snow", "Desert", "Sky", "Forest"};
    for (int i = 0; i < 10; i++)
        if (!strcmp(s, names[i])) return i;
    return 0;
}

static unsigned iso_to_unix(const char *t) {  // "2025-12-19T22:19:44.58Z" -> seconds (0 if unreadable)
    int y, mo, d, h, mi, s;
    if (sscanf(t, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) return 0;
    static const int cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    long days = 365L * (y - 1970) + (y - 1969) / 4 - (y - 1901) / 100 + (y - 1601) / 400 + cum[(mo - 1) % 12] + d - 1;
    if (mo > 2 && (y % 4 == 0 && (y % 100 || y % 400 == 0))) days++;
    return (unsigned)(days * 86400L + h * 3600L + mi * 60L + s);
}

static bool course_from_ocw(const Json *j, Course *c) {
    const char *id = json_str(json_get(j, "id"), "");
    if (strlen(id) != 11 || json_bool(json_get(j, "deleted"), false)) return false;
    memset(c, 0, sizeof *c);
    snprintf(c->code, sizeof c->code, "%s", id);
    snprintf(c->name, sizeof c->name, "%s", json_str(json_get(j, "title"), ""));
    const Json *owner = json_get(j, "owner");
    snprintf(c->maker, sizeof c->maker, "%s", json_str(json_get(owner, "username"), "Maker"));
    snprintf(c->maker_id, sizeof c->maker_id, "%s", json_str(json_get(j, "owner_id"), ""));
    for (char *p = c->maker; *p; p++)  // (the lists show a maker name on one line)
        if (*p == '\n' || *p == '\r') *p = ' ';
    c->style = style_of(json_str(json_get(j, "style"), ""));
    c->theme = theme_of(json_str(json_get(j, "theme"), ""));
    c->uploaded = iso_to_unix(json_str(json_get(j, "created"), ""));
    c->plays = (unsigned)json_num(json_get(j, "plays"), 0);
    c->attempts = (unsigned)json_num(json_get(j, "tries"), 0);
    c->clears = (unsigned)json_num(json_get(j, "clears"), 0);
    c->likes = (unsigned)json_num(json_get(j, "likes"), 0);
    c->endless = json_bool(json_get(j, "endless"), true);
    return true;
}

static void pool_add(const Course *c) {
    for (int i = 0; i < npool; i++)
        if (!strcmp(pool[i].code, c->code)) return;
    if (npool == cappool) {
        cappool = cappool ? cappool * 2 : 256;
        pool = realloc(pool, (size_t)cappool * sizeof *pool);
    }
    pool[npool++] = *c;
}

// Fetch some pages of OCW's newest courses at random offsets into the pool (pool_lock held).
static void pool_fill(void) {
    static unsigned span = 30;  // pages to pick from; shrinks when a page comes back short
    const char *domain = ocw_domain();
    if (!*domain) {
        LOG("[mod] OCW 100 Mario: no domain (ocw-config.json): no courses\n");
        return;
    }
    for (int k = 0; k < PAGES_PER_FILL; k++) {
        unsigned page = rnd(span);
        for (int attempt = 0; attempt < 2; attempt++) {
            char url[200];
            snprintf(url, sizeof url, "https://%s/api/latest/courses?offset=%u&size=%d", domain, page * PAGE, PAGE);
            HttpResponse r;
            int st = http_request("GET", url, "Accept: application/json\r\n", NULL, 0, REST_TIMEOUT_MS, &r);
            Json *j = st == 200 ? json_parse(r.body, r.len) : NULL;
            size_t n = json_len(j);
            for (size_t i = 0; i < n; i++) {
                Course c;
                if (course_from_ocw(json_at(j, i), &c)) pool_add(&c);
            }
            if (!j) LOG("[mod] OCW 100 Mario: course list page %u: %s\n", page, st ? "bad answer" : r.error);
            json_free(j);
            http_free(&r);
            if (n >= PAGE) break;
            if (n == 0 && page > 0) span = page < 2 ? 1 : page, page = rnd(span);  // (past the end: nearer the start)
            else break;
        }
    }
    MLOG("OCW course pool: %d courses\n", npool);
}

static bool in_run(const Run *r, const char *code) {
    for (int i = 0; i < r->nqueue; i++)
        if (!strcmp(r->queue[i].code, code)) return true;
    for (int i = 0; i < r->nhist; i++)
        if (!strcmp(r->hist[i].c.code, code)) return true;
    return false;
}

// Courses this player has played lately (any run, both modes), oldest first in a ring; refill prefers the others.
static char seen[SEEN_MAX][12];
static int nseen, seen_head;

static bool seen_has(const char *code) {
    for (int i = 0; i < nseen; i++)
        if (!strcmp(seen[i], code)) return true;
    return false;
}

static void seen_add(const char *code) {
    if (seen_has(code)) return;
    snprintf(seen[seen_head], sizeof seen[0], "%s", code);
    seen_head = (seen_head + 1) % SEEN_MAX;
    if (nseen < SEEN_MAX) nseen++;
}

// Does the pool course belong to the tier for this run? 100 Mario: pick_hundred's tiers (SMM1's fail-rate band, any
// rated course, unrated ones). SMM2: pick's (the difficulty's band, the bands one, two, three steps away, then unrated).
static bool in_tier(const Course *c, int d, bool hundred, int tier) {
    if (hundred) {
        bool rated = c->attempts >= MIN_RATED;
        int fail = rated ? 100 - (int)(c->clears * 100ull / c->attempts) : 0;
        return tier == 0 ? rated && fail >= BAND[d][0] && fail <= BAND[d][1] : tier == 1 ? rated : !rated;
    }
    int band = smm2_band(c);
    return tier == 4 ? band < 0 : band >= 0 && (band == d - tier || band == d + tier);
}

// Queue random pool courses of the tier that are (not) among the recently played (pool_lock held).
static void take(Run *r, int d, bool hundred, int tier, bool played) {
    int idx[2048], n = 0;
    for (int i = 0; i < npool && n < 2048; i++) {
        const Course *c = &pool[i];
        if (c->endless && !in_run(r, c->code) && seen_has(c->code) == played && in_tier(c, d, hundred, tier)) idx[n++] = i;
    }
    while (n > 0 && r->nqueue < QUEUE_AHEAD) {
        int k = (int)rnd((unsigned)n);
        r->queue[r->nqueue++] = pool[idx[k]];
        idx[k] = idx[--n];
    }
}

// Top the run's queue up from the pool (lock held; may fetch). Fresh courses of the difficulty's own tier come first, and
// more of OCW's list is fetched while there are few of them; only then the ones played lately, then wider tiers.
static void refill(Run *r, int d, bool hundred) {
    if (r->nqueue >= QUEUE_AHEAD) return;
    int tiers = hundred ? 3 : 5;
    pthread_mutex_lock(&pool_lock);
    for (int fetch = 0;; fetch++) {
        take(r, d, hundred, 0, false);
        if (r->nqueue >= FRESH_WANTED || fetch == FETCHES_PER_REFILL || npool >= POOL_MAX) break;
        pool_fill();
    }
    for (int round = 0; r->nqueue < 2 && round < 3; round++) {
        take(r, d, hundred, 0, true);
        for (int tier = 1; tier < tiers && r->nqueue < 2; tier++) {
            take(r, d, hundred, tier, false);
            take(r, d, hundred, tier, true);
        }
        if (r->nqueue < 2 && round < 2) pool_fill();
    }
    pthread_mutex_unlock(&pool_lock);
}

// ---- persistence

static void put_str(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') fprintf(f, "\\%c", *p);
        else if (*p < 0x20) fprintf(f, "\\u%04x", *p);
        else fputc(*p, f);
    }
    fputc('"', f);
}

static void put_course(FILE *f, const Course *c) {
    fprintf(f, "{\"code\":\"%s\",\"name\":", c->code);
    put_str(f, c->name);
    fputs(",\"maker\":", f);
    put_str(f, c->maker);
    fputs(",\"maker_id\":", f);
    put_str(f, c->maker_id);
    fprintf(f, ",\"style\":%d,\"theme\":%d,\"uploaded\":%u,\"plays\":%u,\"attempts\":%u,\"clears\":%u,\"likes\":%u}",
            c->style, c->theme, c->uploaded, c->plays, c->attempts, c->clears, c->likes);
}

static void put_runs(FILE *f, const Run *rs) {
    fputc('[', f);
    for (int d = 0; d < 4; d++) {
        const Run *r = &rs[d];
        fprintf(f, "%s{\"exists\":%d,\"active\":%d,\"complete\":%d,\"lives\":%u,\"clears\":%u,\"coins\":%u,\"score\":%u,"
                   "\"seq\":%u,\"queue\":[", d ? "," : "", r->exists, r->active, r->complete, r->lives, r->clears,
                r->coins, r->score, r->seq);
        for (int i = 0; i < r->nqueue; i++) {
            if (i) fputc(',', f);
            put_course(f, &r->queue[i]);
        }
        fputs("],\"history\":[", f);
        for (int i = 0; i < r->nhist; i++) {
            if (i) fputc(',', f);
            fprintf(f, "{\"clear\":%d,\"seq\":%u,\"c\":", r->hist[i].clear, r->hist[i].seq);
            put_course(f, &r->hist[i].c);
            fputc('}', f);
        }
        fputs("]}", f);
    }
    fputc(']', f);
}

static void save(void) {  // (lock held)
    FILE *f = fopen(SAVE_FILE, "wb");
    if (!f) return;
    fprintf(f, "{\"completed\":[%u,%u,%u,%u],\"best\":[%u,%u,%u,%u],\"runs\":", completed[0], completed[1], completed[2],
            completed[3], best_clears[0], best_clears[1], best_clears[2], best_clears[3]);
    put_runs(f, runs);
    fputs(",\"smm2_runs\":", f);
    put_runs(f, smm2_runs);
    fputs("}\n", f);
    fclose(f);
}

static void course_from_save(const Json *j, Course *c) {
    memset(c, 0, sizeof *c);
    snprintf(c->code, sizeof c->code, "%s", json_str(json_get(j, "code"), ""));
    snprintf(c->name, sizeof c->name, "%s", json_str(json_get(j, "name"), ""));
    snprintf(c->maker, sizeof c->maker, "%s", json_str(json_get(j, "maker"), ""));
    snprintf(c->maker_id, sizeof c->maker_id, "%s", json_str(json_get(j, "maker_id"), ""));
    c->style = (int)json_num(json_get(j, "style"), 0), c->theme = (int)json_num(json_get(j, "theme"), 0);
    c->uploaded = (unsigned)json_num(json_get(j, "uploaded"), 0), c->plays = (unsigned)json_num(json_get(j, "plays"), 0);
    c->attempts = (unsigned)json_num(json_get(j, "attempts"), 0), c->clears = (unsigned)json_num(json_get(j, "clears"), 0);
    c->likes = (unsigned)json_num(json_get(j, "likes"), 0);
    c->endless = true;
}

static void get_runs(const Json *arr, Run *rs) {
    for (int d = 0; d < 4; d++) {
        Json *o = json_at(arr, (size_t)d);
        if (!o) continue;
        Run *r = &rs[d];
        r->exists = json_bool(json_get(o, "exists"), false), r->active = json_bool(json_get(o, "active"), false);
        r->complete = json_bool(json_get(o, "complete"), false);
        r->lives = (unsigned)json_num(json_get(o, "lives"), 0), r->clears = (unsigned)json_num(json_get(o, "clears"), 0);
        r->coins = (unsigned)json_num(json_get(o, "coins"), 0), r->score = (unsigned)json_num(json_get(o, "score"), 0);
        r->seq = (unsigned)json_num(json_get(o, "seq"), 0);
        Json *q = json_get(o, "queue"), *h = json_get(o, "history");
        for (size_t i = 0; i < json_len(q) && r->nqueue < QUEUE_AHEAD; i++) course_from_save(json_at(q, i), &r->queue[r->nqueue++]);
        for (size_t i = 0; i < json_len(h) && r->nhist < HISTORY_MAX; i++) {
            Json *e = json_at(h, i);
            Done *dn = &r->hist[r->nhist++];
            course_from_save(json_get(e, "c"), &dn->c);
            dn->clear = json_bool(json_get(e, "clear"), false), dn->seq = (unsigned)json_num(json_get(e, "seq"), 0);
        }
    }
}

static void load(void) {  // (lock held, once)
    if (loaded) return;
    loaded = true;
    FILE *f = fopen(SAVE_FILE, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = n > 0 && n < (16 << 20) ? malloc((size_t)n + 1) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) {
        Json *j = json_parse(buf, (size_t)n);
        for (int d = 0; d < 4; d++) {
            completed[d] = (unsigned)json_num(json_at(json_get(j, "completed"), (size_t)d), 0);
            best_clears[d] = (unsigned)json_num(json_at(json_get(j, "best"), (size_t)d), 0);
        }
        get_runs(json_get(j, "runs"), runs);
        get_runs(json_get(j, "smm2_runs"), smm2_runs);
        Json *sn = json_get(j, "seen");
        for (size_t i = 0; i < json_len(sn); i++)
            if (*json_str(json_at(sn, i), "")) seen_add(json_str(json_at(sn, i), ""));
        json_free(j);
    }
    free(buf);
    fclose(f);
}

// ---- answers

typedef struct { char *p; size_t n, cap; } Buf;

static void bput(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void bput(Buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char tmp[1024];
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (b->n + (size_t)n + 1 > b->cap) {
        b->cap = (b->n + (size_t)n + 1) * 2;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
    b->n += (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1;
    b->p[b->n] = 0;
}

static void bstr(Buf *b, const char *s) {
    bput(b, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') bput(b, "\\%c", *p);
        else if (*p < 0x20) bput(b, "\\u%04x", *p);
        else bput(b, "%c", *p);
    }
    bput(b, "\"");
}

static void bcourse(Buf *b, const Course *c) {
    static const char *const styles[5] = {"M1", "M3", "MW", "WU", "3W"};
    bput(b, "{\"code\":\"%s\",\"name\":", c->code);
    bstr(b, c->name);
    bput(b, ",\"maker\":");
    bstr(b, c->maker);
    bput(b, ",\"maker_id\":");
    bstr(b, c->maker_id);
    bput(b, ",\"style\":\"%s\",\"theme\":%d,\"uploaded\":%u,\"plays\":%u,\"attempts\":%u,\"clears\":%u,\"likes\":%u}",
         styles[c->style % 5], c->theme, c->uploaded, c->plays, c->attempts, c->clears, c->likes);
}

// (the status says "active" only once a course is cleared: SMM2's Endless menu offers Continue only then [endless.c
// endless_menu_offers_continue], and the 100 Mario select must agree with it, or its Continue goes to a menu that
// shows New Game alone and the course never starts)
static void brun(Buf *b, const Run *r, int goal) {
    bput(b, "{\"active\":%s,\"complete\":%s,\"lives\":%u,\"clears\":%u,\"coins\":%u,\"score\":%u,\"goal\":%d}",
         r->active && r->clears ? "true" : "false", r->complete ? "true" : "false", r->lives, r->clears, r->coins, r->score, goal);
}

static int reply(HttpResponse *r, int status, Buf *b) {
    memset(r, 0, sizeof *r);
    r->status = status;
    r->body = b->p;
    r->len = b->n;
    return status;
}

static int fail(HttpResponse *r, int status, const char *msg) {
    Buf b = {0};
    bput(&b, "{\"error\":\"%s\"}", msg);
    return reply(r, status, &b);
}

static int parse_diff(const char *s) {
    for (int d = 0; d < 4; d++) {
        size_t n = strlen(DIFF[d]);
        if (!strncmp(s, DIFF[d], n) && (s[n] == '/' || s[n] == '?' || !s[n])) return d;
    }
    return -1;
}

static void push_done(Run *r, const Course *c, bool clear) {
    if (r->nhist == HISTORY_MAX) memmove(r->hist, r->hist + 1, sizeof(Done) * (HISTORY_MAX - 1)), r->nhist--;
    Done *dn = &r->hist[r->nhist++];
    dn->c = *c, dn->clear = clear, dn->seq = ++r->seq;
    seen_add(c->code);
}

static void queue_remove(Run *r, int i) {
    memmove(r->queue + i, r->queue + i + 1, sizeof(Course) * (size_t)(r->nqueue - i - 1));
    r->nqueue--;
}

static int run_result(Run *r, int d, bool hundred, const char *body, HttpResponse *resp) {
    Json *j = json_parse(body ? body : "", body ? strlen(body) : 0);
    const char *code = json_str(json_get(j, "code"), ""), *what = json_str(json_get(j, "result"), "");
    unsigned pending = (unsigned)json_num(json_get(j, "pending"), 0);
    unsigned coins = (unsigned)json_num(json_get(j, "coins"), 0), score = (unsigned)json_num(json_get(j, "score"), 0);
    if (!r->active) { json_free(j); return fail(resp, 400, "no run"); }
    int at = -1;
    for (int i = 0; i < r->nqueue; i++)
        if (!strcmp(r->queue[i].code, code)) at = i;
    if (at < 0) { json_free(j); return fail(resp, 400, "not in this run"); }
    Course c = r->queue[at];
    bool game_over = false, done = false;
    if (!strcmp(what, "clear")) {
        r->lives += pending < PENDING_CAP ? pending : PENDING_CAP;
        if (r->lives > (hundred ? 100u : 99u)) r->lives = hundred ? 100u : 99u;
        r->clears++, r->coins += coins, r->score += score;
        push_done(r, &c, true);
        queue_remove(r, at);
        done = hundred && r->clears >= (unsigned)GOAL[d];
    } else if (!strcmp(what, "death") || !strcmp(what, "start_over")) {
        seen_add(c.code);
        r->lives = r->lives ? r->lives - 1 : 0;  // (the same course again: it stays at the head)
        if (at) { Course t = r->queue[at]; memmove(r->queue + 1, r->queue, sizeof(Course) * (size_t)at); r->queue[0] = t; }
    } else if (!strcmp(what, "skip")) {
        // (free in SMM1's rules; SMM2's charges one only during the miss demo)
        if (!hundred && json_bool(json_get(j, "during_miss"), false) && r->lives) r->lives--;
        push_done(r, &c, false);
        queue_remove(r, at);
    } else {  // broken, or any other: the course leaves the run
        queue_remove(r, at);
    }
    if (done) r->complete = true, r->active = false, completed[d]++;
    if (!r->lives) r->active = false, game_over = true;
    Buf b = {0};
    bput(&b, "{\"lives\":%u,\"clears\":%u,\"passed\":%u,\"game_over\":%s,\"run_complete\":%s}", r->lives, r->clears,
         r->seq, game_over ? "true" : "false", done ? "true" : "false");
    json_free(j);
    save();
    return reply(resp, 200, &b);
}

int ocw_runs_request(const char *method, const char *path, const char *body, size_t blen, HttpResponse *resp) {
    (void)blen;
    memset(resp, 0, sizeof *resp);
    pthread_mutex_lock(&lock);
    load();
    Buf b = {0};
    int status = 404;
    bool get = !strcmp(method, "GET"), post = !strcmp(method, "POST");
    if (get && !strncmp(path, "/api/me", 7)) {
        const char *name = getenv("RECOMP_USER_NAME") ? getenv("RECOMP_USER_NAME") : "Player";
        bput(&b, "{\"name\":");
        bstr(&b, name);
        bput(&b, ",\"maker_id\":\"OCW-LOCAL\"}");
        status = reply(resp, 200, &b);
    } else if (get && !strncmp(path, "/api/endless", 12) && (!path[12] || path[12] == '?')) {
        bput(&b, "{\"runs\":{");
        for (int d = 0; d < 4; d++) {
            bput(&b, "%s\"%s\":", d ? "," : "", DIFF[d]);
            brun(&b, &smm2_runs[d], 0);
        }
        bput(&b, "},\"hundred_runs\":{");
        for (int d = 0; d < 4; d++) {
            bput(&b, "%s\"%s\":", d ? "," : "", DIFF[d]);
            brun(&b, &runs[d], GOAL[d]);
        }
        bput(&b, "},\"best\":{");
        for (int d = 0; d < 4; d++) bput(&b, "%s\"%s\":%u", d ? "," : "", DIFF[d], best_clears[d]);
        bput(&b, "}}");
        status = reply(resp, 200, &b);
    } else if (!strncmp(path, "/api/endless/", 13) && parse_diff(path + 13) >= 0) {
        int d = parse_diff(path + 13);
        bool hundred = !strstr(path, "ruleset=smm2");  // (the mod always says which)
        Run *r = &slots(hundred)[d];
        const char *verb = path + 13 + strlen(DIFF[d]);
        if (*verb == '/') verb++;
        if (post && !strncmp(verb, "start", 5)) {
            memset(r, 0, sizeof *r);
            r->exists = r->active = true, r->lives = hundred ? 100 : 5;
            refill(r, d, hundred);
            save();
            bput(&b, "{\"run\":{\"lives\":%u,\"goal\":%d}}", r->lives, goal_of(hundred, d));
            status = reply(resp, 200, &b);
        } else if (get && !strncmp(verb, "next", 4)) {
            const char *cp = strstr(path, "count=");
            int count = cp ? atoi(cp + 6) : 10;
            if (!r->active) {
                status = fail(resp, 404, "no run");
            } else {
                refill(r, d, hundred);
                if (count > r->nqueue) count = r->nqueue;
                bput(&b, "{\"courses\":[");
                for (int i = 0; i < count; i++) {
                    if (i) bput(&b, ",");
                    bcourse(&b, &r->queue[i]);
                }
                bput(&b, "],\"widened\":false}");
                save();
                status = reply(resp, 200, &b);
            }
        } else if (post && !strncmp(verb, "result", 6)) {
            status = run_result(r, d, hundred, body, resp);
            if (r->active) refill(r, d, hundred);
        } else if (post && (!strncmp(verb, "suspend", 7) || !strncmp(verb, "finish", 6))) {
            if (verb[0] == 'f') {
                r->active = false;
                if (!hundred && r->clears > best_clears[d]) best_clears[d] = r->clears;  // (SMM2's best clears)
            }
            save();
            bput(&b, "{}");
            status = reply(resp, 200, &b);
        } else if (get && !strncmp(verb, "history", 7)) {
            if (!r->exists) {
                status = fail(resp, 404, "no run");
            } else {
                static const char *const styles[5] = {"M1", "M3", "MW", "WU", "3W"};
                bput(&b, "{\"history\":[");
                for (int i = 0; i < r->nhist; i++) {
                    const Done *dn = &r->hist[i];
                    bput(&b, "%s{\"code\":\"%s\",\"result\":\"%s\",\"seq\":%u,\"style\":\"%s\",\"theme\":%d,\"name\":",
                         i ? "," : "", dn->c.code, dn->clear ? "clear" : "skip", dn->seq, styles[dn->c.style % 5],
                         dn->c.theme);
                    bstr(&b, dn->c.name);
                    bput(&b, ",\"maker\":");
                    bstr(&b, dn->c.maker);
                    bput(&b, "}");
                }
                bput(&b, "]}");
                status = reply(resp, 200, &b);
            }
        }
    } else if (get && (!strncmp(path, "/api/rankings/hundred_", 22) || !strncmp(path, "/api/rankings/endless_", 22)) &&
               parse_diff(path + 22) >= 0) {
        unsigned n = (path[15] == 'h' ? completed : best_clears)[parse_diff(path + 22)];
        bput(&b, "{\"me\":{\"score\":%u,\"rank\":%u}}", n, n ? 1u : 0u);
        status = reply(resp, 200, &b);
    } else if (get && !strncmp(path, "/api/courses/", 13) && !strchr(path + 13, '/')) {
        char code[12];
        snprintf(code, sizeof code, "%.11s", path + 13);
        const Course *found = NULL;
        for (int k = 0; k < 8 && !found; k++) {
            const Run *r = &(k < 4 ? runs : smm2_runs)[k & 3];
            for (int i = 0; i < r->nqueue && !found; i++)
                if (!strcmp(r->queue[i].code, code)) found = &r->queue[i];
            for (int i = 0; i < r->nhist && !found; i++)
                if (!strcmp(r->hist[i].c.code, code)) found = &r->hist[i].c;
        }
        if (found) bcourse(&b, found), status = reply(resp, 200, &b);
    }
    if (status == 404 && !resp->status) status = fail(resp, 404, "not served");
    pthread_mutex_unlock(&lock);
    return status;
}
