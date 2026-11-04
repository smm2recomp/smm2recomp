// The next Endless courses' files, fetched while the current one is played (issue #61): a small cache keyed by the
// course's data id, filled by one host thread, read by the game's network worker when the game asks for a course.
//
// Which courses: the game plays its Endless list (10 a search) in order, so once the list is known (cp_set_list) the
// courses after the one being played are known too (cp_after). Our server's own queue can name one past the list's
// end (cp_want). Nothing is served that doesn't pass cp_valid; a miss of any kind leaves the game's own path.
// Plain C, no guest headers: tools/test_course_preload.c tests the rules on their own.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// What a fetch brings: our server's plain course (0x5bfc0 bytes; endless.c encrypts it as the game's file) or a
// course file as the game downloads it (0x5c000 bytes: {u32 1, u16 0x10, u16 0, u32 CRC, "SCDL"}, OCW's level_data).
enum { CP_PLAIN = 1, CP_FILE = 2 };
#define CP_PLAIN_SIZE 0x5bfc0
#define CP_FILE_SIZE 0x5c000
#define CP_DEPTH 2           // courses fetched ahead of the one played
#define CP_SLOTS 3           // at most this many files held (~1.1 MB)
#define CP_MAX_AGE_S (15 * 60)

// A fetch, run on the preload's own thread (never the game's): the bytes (malloc'd) or NULL.
typedef uint8_t *(*CpFetch)(uint32_t id, size_t *len);

enum { CP_MISS, CP_HIT, CP_JOINED };                                   // cp_take
enum { CP_NONE, CP_QUEUED, CP_LOADING, CP_READY, CP_FAILED };          // cp_state

void cp_set_list(const uint32_t *ids, int n, int difficulty);  // the game's Endless list, in play order (a new search)
bool cp_after(uint32_t id, int kind, CpFetch fetch);  // id is downloaded now: fetch the next CP_DEPTH (false: unlisted)
bool cp_want(uint32_t id, int kind, CpFetch fetch);   // fetch this one (our server's queue named it)
bool cp_next_known(uint32_t id);                       // the list has a course after id
int cp_state(uint32_t id, int kind);
// The file of id: CP_HIT (held), CP_JOINED (its fetch was under way and ended well within wait_ms; *waited_ms says how
// long) or CP_MISS. A hit hands the bytes over (*data, malloc'd) and forgets them: each course is downloaded once.
// The caller's core is given up while it waits (kernel_host_wait_begin).
int cp_take(uint32_t id, int kind, int wait_ms, uint8_t **data, size_t *len, int *waited_ms);
int cp_wait(uint32_t id, int kind, int wait_ms);  // waits while id is fetched (no take): ms waited
void cp_clear(const char *why);                   // all of it goes (game over, quit, backend restart ...)
bool cp_valid(int kind, const uint8_t *d, size_t n);
// A plain-HTTP request of the game's, answered from the cache (runtime/services/bsd_host.c game_http_answer): true for
// "GET /level_data/<id> HTTP/1.x" with "Host: <host>" whose file is held; *resp (malloc'd) is the whole answer.
bool cp_http_answer(const char *host, const char *req, size_t len, uint8_t **resp, size_t *rlen, uint32_t *id);

extern volatile bool cp_verbose;                    // log each fetch and hit (the mod's log switch)
extern void (*cp_name)(uint32_t id, char out[12]);  // a data id as a course code, for the log (NULL: the number)
// tests: the clock the age rule reads (seconds; NULL: the monotonic clock); wait until no fetch is queued or running
void cp_test_clock(uint64_t (*now_s)(void));
void cp_idle(void);
int cp_held(void);  // entries of any state
