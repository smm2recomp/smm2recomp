// Play recording (RECOMP_PLAY_RECORD=<dir>): every course attempt the player plays, as a folder that replays it.
//
// What a folder holds, so a recorded attempt can be played again headless (direct boot + RECOMP_PAD_SCRIPT) and
// compared with itself or another runtime:
//   course.dat   the course as the loader took it: the .bcd's 0x10-byte header (u32 1, u16 0x10, the body's CRC32,
//                "SCDL") and the decrypted 0x5bfc0-byte body, read from the save buffer's prepared slot (+0x6c000:
//                games/smm2/mods/edit.c; Coursebot's Play and a downloaded course both play from it). It is what
//                RECOMP_BOOT_COURSE takes. Missing when that slot holds no valid course (meta.json course_found).
//   inputs.pad   the pad as a RECOMP_PAD_SCRIPT: one script frame per update of the player, the game's own clock
//                (games/smm2/trace.c), "N BUTTON ... LX=n LY=n RX=n RY=n" runs
//   inputs.csv   the same, one line per change: frame (player updates from the attempt's first), buttons (nn::hid bits,
//                the stick directions 16-23 included), stick_lx, stick_ly, stick_rx, stick_ry
//   player.csv   the player at the start of every update: update, x, y (+0x230, +0x234), state (+0x3f8); a replay that
//                matches it update for update played the same (tools/play_replay.py --check)
//   restarts.csv each restart after a miss: restart (the update the new life starts at), death (the update the miss
//                began at, -1 when not seen), x, y, state (the player there), gap (presented frames without an update)
//   meta.json    the course (style, name, version, CRC), the seeds, the two random generators' state at the first
//                update, the player's start (place, state), the end (why, the last state and place, updates)
//
// The clock. A pad script's frame k takes effect at the k-th player update and is what the game reads until the next
// one (runtime/hle/padscript.c, games/smm2/trace.c). So frame k here is the last pad state the game read between
// update k and update k+1 (runtime/hle/input.c hands each read to game_pad_read): the read the next update acts on.
//
// An attempt is one player object: it starts at that player's first update and ends when another player object is
// updated while the followed one has not been for a few presented frames (the next course; local multiplayer's other
// players are updated alongside and change nothing) or at exit. A miss usually does NOT end it: the game builds the
// restarted course's player at the same address (a recording of 7380 updates with nine restarts stayed one folder), and
// that is what replay wants: one boot, and the replay restarts where the game did (each miss's timing is the game's,
// or the Quick level return mod's when it is on: meta.json "mods"). The title screen's course (scene mode 6) is never
// recorded: it has a player of its own.
//
// Restarts. A miss does not end the attempt (the restarted course's player is built at the same address), and the
// updates a death takes, from the miss to the course built again, can differ between builds by a few (a first
// restart at a checkpoint: 222 on macOS, 228 headless) while the player takes no input. So each restart is kept
// (restarts.csv), and a replay given them (RECOMP_PAD_SYNC=<restarts.csv>) moves the script to the recorded restart's
// frame at each of its own (runtime/hle/padscript.c padscript_seek): the input after a restart lines up with the
// restart. A restart is the first player update after more than two presented frames without one, in a life that
// went down (cState_DownPose .. cState_RestartWait), and whose state is cState_Restart (16): a pause stops the updates
// too, and one taken while he is down resumes him still down, which is no restart.
//
// Held from before. A button already held when the attempt starts (run, or a direction, kept down through the course's
// start; a press the game took before the first update) is held, not pressed, at update 0: the game sees no new press.
// meta.json's held_before is the last pad state the game read before the first update, and RECOMP_PAD_HELD shows it
// to the game until the script's first frame (runtime/hle/padscript.c), once the title is gone (game_pad_held_now).
//
// Not in the inputs: Plus and Minus. Pausing stops the player's updates, so the script clock does not move through the
// pause menu; a replayed Plus would pause the replay with its clock stopped. The pause and its menu are left out (the
// game does not move while paused); meta.json counts the updates that had them held.
//
// Randomness. The game's sead::Random generators seed themselves from the system tick (games/smm2/seed.c:
// SMM2_SEED, SMM2_SEED_ALL pin them). The global generator has drawn through every menu before the course, which a
// direct boot does not repeat, so its state and the area's (area +0x100 -> +8) are saved at the first update, and
// RECOMP_PLAY_RANDOM="g0,g1,g2,g3[,a0,a1,a2,a3]" puts them back at the first player update of a run (the replay).
//
// Zero cost when RECOMP_PLAY_RECORD is unset. An IO error stops the recording (logged); the game goes on. inputs.csv and
// player.csv are flushed every update and meta.json rewritten every 60 ("end": "running"), so a run that ends without
// exit() (a crash, _exit) keeps them; inputs.pad is complete once the attempt has closed (tools/play_replay.py makes
// its script from inputs.csv).
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#define playrec_mkdir(path) _mkdir(path)
#else
#define playrec_mkdir(path) mkdir((path), 0755)
#endif

#include "../../runtime/hle.h"
#include "../../runtime/padscript.h"

#ifndef RECOMP_VERSION
#define RECOMP_VERSION "dev"
#endif

#define SAVE_MGR (MAIN_BASE + 0x2a39088ull)   // *: +0x20 the save buffer (games/smm2/mods/edit.c)
#define SLOT_COURSE 0x6c000ull                // save buffer: the prepared slot's course (header + body)
#define COURSE_BODY 0x5bfc0ull
#define GLOBAL_RANDOM (MAIN_BASE + 0x2a24fe0ull)  // *: sead's global sead::Random (4 words)
#define GPM (MAIN_BASE + 0x2c57d58ull)           // GamePhaseManager*: [+0x30]+0x14 the scene mode (6 the title)
#define SCENE_TITLE 6
#define GOAL_FIRST 121   // cState_GoalPoleClimb .. cState_GoalDead: the player has cleared the course
#define GOAL_LAST 128
#define DOWN_FIRST 113  // cState_DownPose .. cState_RestartWait: the player has missed (a restart follows)
#define DOWN_LAST 119
#define RESTART_STATE 16  // cState_Restart: a restarted life's first update (restarts.csv's rows)
#define NO_PAUSE_KEYS (~((1ull << 10) | (1ull << 11)))  // Plus, Minus (see above)

static int on = -1;          // -1 not looked at; 0 off; 1 recording
static const char *root;
static uint64_t rd_b;        // the last pad state the game read (game_pad_read)
static int32_t rd_st[4];

static struct {
    int open;
    char dir[1024];
    FILE *csv, *pad, *ply;
    uint64_t player;
    int64_t k;                // player updates so far
    uint64_t seen;            // the presented frame of the followed player's last update
    uint32_t scene;
    int cleared;
    uint64_t run_b;           // the current run of identical script frames
    int32_t run_st[4];
    int64_t run_n;
    uint64_t csv_b;           // the last line in inputs.csv
    int32_t csv_st[4];
    int64_t pause_keys;       // updates with Plus or Minus held (left out)
    FILE *rst;                // restarts.csv
    int64_t death;            // the update the current life went down at (-1: not yet)
    int64_t restarts;
    float x0, y0, x, y;
    uint32_t state0, state;
    uint64_t held_b;          // the pad before the first update (held_before)
    int32_t held_st[4];
    uint32_t grand[4], arand[4];
    int grand_ok, arand_ok;
    int course_found;
    uint32_t course_crc;
    char style[4], name[80];
    uint32_t version;
} a;
static int seq;

extern uint64_t renderer_present_count(void);

static int readable(uint64_t p, uint64_t n) { return p >= GUEST_LO && p + n <= GUEST_HI && p + n > p; }

static uint32_t scene_mode(void) {
    if (!readable(GPM, 8)) return 0;
    uint64_t gpm = LD64(GPM);
    if (!gpm || !readable(gpm + 0x30, 8)) return 0;
    uint64_t inner = LD64(gpm + 0x30);
    return inner && readable(inner + 0x14, 4) ? LD32(inner + 0x14) : 0;
}

static uint32_t crc32_of(const uint8_t *p, size_t n) {
    uint32_t c = 0xffffffffu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}

static void stop(const char *what) {
    LOG("[playrec] %s: recording stopped\n", what);
    if (a.csv) fclose(a.csv);
    if (a.pad) fclose(a.pad);
    if (a.ply) fclose(a.ply);
    if (a.rst) fclose(a.rst);
    a.csv = a.pad = a.ply = a.rst = NULL;
    a.open = 0;
    on = 0;
}

// The course name (UTF-16 at body +0xF4, 0x21 characters) as a folder-safe ASCII word.
static void course_name(const uint8_t *body) {
    size_t o = 0;
    for (int i = 0; i < 0x21 && o + 1 < sizeof a.name; i++) {
        uint16_t ch = (uint16_t)(body[0xf4 + 2 * i] | body[0xf5 + 2 * i] << 8);
        if (!ch) break;
        char c = ch < 0x80 && ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) ? (char)ch
                  : ch < 0x80 && (ch == '-' || ch == '_') ? (char)ch : '_';
        if (c == '_' && o && a.name[o - 1] == '_') continue;
        a.name[o++] = c;
    }
    while (o && a.name[o - 1] == '_') o--;
    a.name[o] = 0;
    if (!o) snprintf(a.name, sizeof a.name, "course");
}

// The prepared slot's course, if it holds one: written as course.dat.
static void take_course(void) {
    a.course_found = 0;
    snprintf(a.name, sizeof a.name, "unknown");
    a.style[0] = 0;
    if (!readable(SAVE_MGR, 8)) return;
    uint64_t mgr = LD64(SAVE_MGR);
    if (!mgr || !readable(mgr + 0x20, 8)) return;
    uint64_t buf = LD64(mgr + 0x20);
    uint64_t slot = buf + SLOT_COURSE;
    if (!buf || !readable(slot, 0x10 + COURSE_BODY)) return;
    const uint8_t *p = (const uint8_t *)(uintptr_t)slot;
    if (LD32(slot) != 1 || LD16(slot + 4) != 0x10 || memcmp(p + 0xc, "SCDL", 4)) return;
    uint32_t crc = crc32_of(p + 0x10, COURSE_BODY);
    if (crc != LD32(slot + 8)) { LOG("[playrec] the prepared slot's CRC does not match: course not taken\n"); return; }
    a.course_found = 1;
    a.course_crc = crc;
    const uint8_t *body = p + 0x10;
    memcpy(a.style, body + 0xf1, 2);
    a.style[2] = 0;
    a.version = (uint32_t)(body[0x14] | body[0x15] << 8 | body[0x16] << 16 | (uint32_t)body[0x17] << 24);
    course_name(body);
}

static void put_course(void) {
    if (!a.course_found || !readable(LD64(LD64(SAVE_MGR) + 0x20) + SLOT_COURSE, 0x10 + COURSE_BODY)) return;
    char path[1100];
    snprintf(path, sizeof path, "%s/course.dat", a.dir);
    FILE *f = fopen(path, "wb");
    if (!f) { stop("cannot write course.dat"); return; }
    const uint8_t *p = (const uint8_t *)(uintptr_t)(LD64(LD64(SAVE_MGR) + 0x20) + SLOT_COURSE);
    size_t w = fwrite(p, 1, 0x10 + COURSE_BODY, f);
    if (fclose(f) || w != 0x10 + COURSE_BODY) stop("cannot write course.dat");
}

// A generator's four words at p (sead::Random: x, y, z, w).
static int random_words(uint64_t p, uint32_t w[4]) {
    if (!p || !readable(p, 16)) return 0;
    for (int i = 0; i < 4; i++) w[i] = LD32(p + 4 * i);
    return 1;
}
static uint64_t area_random(uint64_t player) {  // player +0x48 -> +0x140: its area; +0x100 -> +8: the area's generator
    if (!readable(player + 0x48, 8)) return 0;
    uint64_t o = LD64(player + 0x48);
    if (!o || !readable(o + 0x140, 8)) return 0;
    uint64_t area = LD64(o + 0x140);
    if (!area || !readable(area + 0x100, 8)) return 0;
    uint64_t g = LD64(area + 0x100);
    return g ? g + 8 : 0;
}

bool quick_retry_on(void);  // (mods/quick_retry.c: Quick level return also shortens SMM2's own miss, so the timing of
                            // every death differs from the game's; a replay needs the same setting and the same mod)

static void write_meta(const char *end) {
    char path[1100];
    snprintf(path, sizeof path, "%s/meta.json", a.dir);
    FILE *f = fopen(path, "w");
    if (!f) { stop("cannot write meta.json"); return; }
    const char *seed = getenv("SMM2_SEED");
    fprintf(f, "{\n  \"format\": 1,\n  \"recomp\": \"%s\",\n", RECOMP_VERSION);
    fprintf(f, "  \"course_found\": %s,\n", a.course_found ? "true" : "false");
    if (a.course_found)
        fprintf(f, "  \"course\": {\"style\": \"%s\", \"name\": \"%s\", \"game_version\": %u, \"crc32\": \"%08x\"},\n",
                a.style, a.name, a.version, a.course_crc);
    fprintf(f, "  \"seed\": %s%s%s,\n  \"seed_all\": %s,\n", seed ? "\"" : "", seed ? seed : "null", seed ? "\"" : "",
            getenv("SMM2_SEED_ALL") ? "true" : "false");
    // The mods that change gameplay timing: a recording made with one on replays the same only with it on, in a build
    // whose mod does the same (tools/play_replay.py warns).
    fprintf(f, "  \"mods\": {\"quick_retry\": %s, \"quick_retry_short\": %s},\n", quick_retry_on() ? "true" : "false",
            getenv("RECOMP_QUICK_RETRY_SHORT") ? "\"set\"" : "null");
    if (a.grand_ok)
        fprintf(f, "  \"global_random\": [%u, %u, %u, %u],\n", a.grand[0], a.grand[1], a.grand[2], a.grand[3]);
    if (a.arand_ok)
        fprintf(f, "  \"area_random\": [%u, %u, %u, %u],\n", a.arand[0], a.arand[1], a.arand[2], a.arand[3]);
    fprintf(f, "  \"play_random\": \"");
    if (a.grand_ok) fprintf(f, "%u,%u,%u,%u", a.grand[0], a.grand[1], a.grand[2], a.grand[3]);
    if (a.grand_ok && a.arand_ok) fprintf(f, ",%u,%u,%u,%u", a.arand[0], a.arand[1], a.arand[2], a.arand[3]);
    fprintf(f, "\",\n");
    fprintf(f, "  \"scene_mode\": %u,\n", a.scene);
    {   // held_before, and the same as RECOMP_PAD_HELD's value
        static const char *nm[16] = {"A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R", "ZL", "ZR", "PLUS", "MINUS",
                                     "LEFT", "UP", "RIGHT", "DOWN"};
        char held[200] = "";
        size_t o = 0;
        uint64_t hb = a.held_b & NO_PAUSE_KEYS;
        for (int i = 0; i < 16; i++)
            if (hb >> i & 1) o += (size_t)snprintf(held + o, sizeof held - o, "%s%s", o ? " " : "", nm[i]);
        static const char *ax[4] = {"LX", "LY", "RX", "RY"};
        for (int i = 0; i < 4; i++)
            if (a.held_st[i]) o += (size_t)snprintf(held + o, sizeof held - o, "%s%s=%d", o ? " " : "", ax[i], a.held_st[i]);
        fprintf(f, "  \"held_before\": {\"buttons\": \"%#llx\", \"lx\": %d, \"ly\": %d, \"rx\": %d, \"ry\": %d},\n",
                (unsigned long long)hb, a.held_st[0], a.held_st[1], a.held_st[2], a.held_st[3]);
        fprintf(f, "  \"pad_held\": \"%s\",\n", held);
    }
    fprintf(f, "  \"start\": {\"x\": %.3f, \"y\": %.3f, \"state\": %u},\n", a.x0, a.y0, a.state0);
    fprintf(f, "  \"cleared\": %s,\n", a.cleared ? "true" : "false");
    fprintf(f, "  \"updates\": %lld,\n  \"pause_keys_updates\": %lld,\n  \"restarts\": %lld,\n", (long long)a.k,
            (long long)a.pause_keys, (long long)a.restarts);
    if (end)
        fprintf(f, "  \"end\": {\"why\": \"%s\", \"x\": %.3f, \"y\": %.3f, \"state\": %u}\n", end, a.x, a.y, a.state);
    else
        fprintf(f, "  \"end\": null\n");
    fprintf(f, "}\n");
    if (fclose(f)) stop("cannot write meta.json");
}

// One script frame (the run-length pad script and the change list).
static void pad_line(uint64_t b, const int32_t st[4], int64_t n) {
    static const char *names[16] = {"A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R", "ZL", "ZR", "PLUS", "MINUS",
                                    "LEFT", "UP", "RIGHT", "DOWN"};
    fprintf(a.pad, "%lld", (long long)n);
    int any = 0;
    for (int i = 0; i < 16; i++)
        if (b >> i & 1) { fprintf(a.pad, " %s", names[i]); any = 1; }
    if (!any) fprintf(a.pad, " -");
    static const char *axes[4] = {"LX", "LY", "RX", "RY"};
    for (int i = 0; i < 4; i++)
        if (st[i]) fprintf(a.pad, " %s=%d", axes[i], st[i]);
    fputc('\n', a.pad);
}
static void frame(uint64_t b, const int32_t st[4]) {
    if (b & ~NO_PAUSE_KEYS) a.pause_keys++;
    b &= NO_PAUSE_KEYS;
    int64_t f = a.k - 1;
    if (f == 0 || b != a.csv_b || memcmp(st, a.csv_st, sizeof a.csv_st)) {
        fprintf(a.csv, "%lld,%#llx,%d,%d,%d,%d\n", (long long)f, (unsigned long long)b, st[0], st[1], st[2], st[3]);
        a.csv_b = b;
        memcpy(a.csv_st, st, sizeof a.csv_st);
    }
    uint64_t sb = b & 0xffffull;  // (the stick directions, 16-23, come back from the sticks: padscript.c)
    if (a.run_n && sb == a.run_b && !memcmp(st, a.run_st, sizeof a.run_st)) { a.run_n++; return; }
    if (a.run_n) pad_line(a.run_b, a.run_st, a.run_n);
    a.run_b = sb;
    memcpy(a.run_st, st, sizeof a.run_st);
    a.run_n = 1;
}

static void close_attempt(const char *why) {
    if (!a.open) return;
    if (a.k > 0) frame(rd_b, rd_st);  // the reads since the last update: its script frame
    if (a.run_n) pad_line(a.run_b, a.run_st, a.run_n);
    a.run_n = 0;
    int bad = ferror(a.csv) || ferror(a.pad) || ferror(a.ply) || ferror(a.rst);
    fclose(a.csv), fclose(a.pad), fclose(a.ply), fclose(a.rst);
    a.csv = a.pad = a.ply = a.rst = NULL;
    a.open = 0;
    if (bad) { stop("cannot write the inputs"); return; }
    write_meta(why);
    LOG("[playrec] %s: %lld updates (%s)\n", a.dir, (long long)a.k, why);
}

static void at_exit(void) { close_attempt("exit"); }

static void open_attempt(uint64_t self) {
    memset(&a, 0, sizeof a);
    take_course();
    time_t t = time(NULL);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    snprintf(a.dir, sizeof a.dir, "%s/%s-%03d-%s", root, stamp, seq++, a.name);
    playrec_mkdir(root);
    if (playrec_mkdir(a.dir) && errno != EEXIST) { stop("cannot create the attempt's folder"); return; }
    char path[1100];
    snprintf(path, sizeof path, "%s/inputs.csv", a.dir);
    a.csv = fopen(path, "w");
    snprintf(path, sizeof path, "%s/inputs.pad", a.dir);
    a.pad = fopen(path, "w");
    snprintf(path, sizeof path, "%s/player.csv", a.dir);
    a.ply = fopen(path, "w");
    snprintf(path, sizeof path, "%s/restarts.csv", a.dir);
    a.rst = fopen(path, "w");
    if (!a.csv || !a.pad || !a.ply || !a.rst) { stop("cannot create the inputs"); return; }
    fprintf(a.ply, "update,x,y,state\n");
    fprintf(a.rst, "restart,death,x,y,state,gap\n");
    a.death = -1;
    fprintf(a.csv, "frame,buttons,stick_lx,stick_ly,stick_rx,stick_ry\n");
    fprintf(a.pad, "# recorded play (RECOMP_PLAY_RECORD): one frame per player update; RECOMP_PAD_SCRIPT plays it\n");
    a.open = 1;
    a.player = self;
    a.held_b = rd_b;
    memcpy(a.held_st, rd_st, sizeof a.held_st);
    if (readable(GLOBAL_RANDOM, 8)) a.grand_ok = random_words(LD64(GLOBAL_RANDOM), a.grand);
    a.arand_ok = random_words(area_random(self), a.arand);
    if (readable(self + 0x230, 8)) memcpy(&a.x0, (const void *)(uintptr_t)(self + 0x230), 4),
                                   memcpy(&a.y0, (const void *)(uintptr_t)(self + 0x234), 4);
    a.state0 = readable(self + 0x3f8, 4) ? LD32(self + 0x3f8) : 0;
    a.scene = scene_mode();
    put_course();
    if (on) write_meta(NULL);  // (a run that dies keeps what it had)
    LOG("[playrec] recording %s\n", a.dir);
}

// RECOMP_PAD_HELD shows once the title is gone (runtime/hle/padscript.c): the title's own player and menus never see it.
bool game_pad_held_now(void) {
    uint32_t m = scene_mode();
    return m != 0 && m != SCENE_TITLE;
}

// Each pad state player 1's game reads (runtime/hle/input.c).
void game_pad_read(uint64_t b, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    if (on == 0) return;  // (kept before the recorder looks at RECOMP_PLAY_RECORD too: the pad held_before is the last of them)
    rd_b = b, rd_st[0] = lx, rd_st[1] = ly, rd_st[2] = rx, rd_st[3] = ry;
}

// RECOMP_PLAY_RANDOM: the recorded generators' state back at the first player update (a replay).
static void restore_random(uint64_t self) {
    static int done;
    if (done) return;
    done = 1;
    const char *s = getenv("RECOMP_PLAY_RANDOM");
    if (!s || !*s) return;
    uint32_t w[8];
    int n = 0;
    for (const char *p = s; *p && n < 8;) {
        char *end;
        unsigned long v = strtoul(p, &end, 0);
        if (end == p) break;
        w[n++] = (uint32_t)v;
        p = *end ? end + 1 : end;
    }
    if (n >= 4 && readable(GLOBAL_RANDOM, 8)) {
        uint64_t g = LD64(GLOBAL_RANDOM);
        if (g && readable(g, 16)) memcpy((void *)(uintptr_t)g, w, 16);
    }
    uint64_t ar = area_random(self);
    if (n >= 8 && ar && readable(ar, 16)) memcpy((void *)(uintptr_t)ar, w + 4, 16);
    LOG("[playrec] random state restored at the first player update (%d words)\n", n);
}

// The followed player's lives (recording and replay alike): the presented frame of its last update and whether it went
// down since its last restart. Returns, at a restart (see above), the presented frames without an update before it.
static struct { uint64_t player, seen; int down, started; } life;
static uint64_t life_restart(uint64_t self, uint64_t now, uint32_t state) {
    if (life.started && self != life.player && now <= life.seen + 2) return 0;  // (local multiplayer's other players)
    uint64_t gap = 0;
    // A restart begins in cState_Restart (16); a pause while he is down stops the updates too, and he is still down
    // when they go on (a recording paused during a pit death took the resumed fall for a new life): no restart then.
    if (life.started && self == life.player && life.down && now > life.seen + 2 && state == RESTART_STATE)
        gap = now - life.seen, life.down = 0;
    if (!life.started || self != life.player) life.started = 1, life.player = self, life.down = 0;  // (another course)
    life.seen = now;
    if (state >= DOWN_FIRST && state <= DOWN_LAST) life.down = 1;
    return gap;
}

// RECOMP_PAD_SYNC=<restarts.csv>: a replay's restarts go on at the recorded restarts' script frames (see above).
static int64_t *sync_at;
static int64_t sync_n, sync_next;
static int sync_on = -1;
static void sync_load(void) {
    sync_on = 0;
    const char *path = getenv("RECOMP_PAD_SYNC");
    if (!path || !*path) return;
    FILE *f = fopen(path, "r");
    if (!f) { LOG("[playrec] RECOMP_PAD_SYNC %s: cannot open\n", path); return; }
    char line[256];
    int64_t cap = 0;
    while (fgets(line, sizeof line, f)) {
        char *end;
        long long v = strtoll(line, &end, 10);
        if (end == line) continue;  // (the header)
        if (sync_n == cap) cap = cap * 2 + 16, sync_at = realloc(sync_at, (size_t)cap * sizeof *sync_at);
        sync_at[sync_n++] = v;
    }
    fclose(f);
    sync_on = 1;
    LOG("[playrec] RECOMP_PAD_SYNC %s: %lld restarts\n", path, (long long)sync_n);
}
static void sync_restart(void) {
    if (sync_on < 0) sync_load();
    if (!sync_on || !padscript_active()) return;
    if (sync_next >= sync_n) { LOG("[playrec] a restart the recording does not have (%lld recorded)\n", (long long)sync_n); return; }
    int64_t want = sync_at[sync_next++], had = padscript_pos();
    padscript_seek(want);
    LOG("[playrec] restart %lld: script frame %lld -> %lld (%+lld)\n", (long long)sync_next, (long long)had,
        (long long)want, (long long)(want - had));
}

// At the start of every player update (games/smm2/trace.c smm2_player_hook), before anything of the update runs.
void smm2_playrec_player(uint64_t self) {
    if (scene_mode() == SCENE_TITLE) return;  // (the title's own course and player)
    restore_random(self);
    const uint32_t st0 = readable(self + 0x3f8, 4) ? LD32(self + 0x3f8) : 0;
    const uint64_t gap = life_restart(self, renderer_present_count(), st0);
    if (gap) sync_restart();
    if (on == 0) return;
    if (on < 0) {
        root = getenv("RECOMP_PLAY_RECORD");
        on = root && *root;
        if (!on) return;
        atexit(at_exit);
        LOG("[playrec] recording every course attempt into %s\n", root);
    }
    const uint64_t now = renderer_present_count();
    if (a.open && self != a.player) {
        if (now <= a.seen + 2) return;  // (another player updated alongside the followed one: local multiplayer)
        close_attempt("player gone");
    }
    if (!a.open) {
        open_attempt(self);
        if (!a.open) return;
    }
    a.seen = now;
    if (a.k > 0) frame(rd_b, rd_st);  // frame k-1: the last read since update k-1
    a.k++;
    if (readable(self + 0x230, 8)) memcpy(&a.x, (const void *)(uintptr_t)(self + 0x230), 4),
                                   memcpy(&a.y, (const void *)(uintptr_t)(self + 0x234), 4);
    a.state = readable(self + 0x3f8, 4) ? LD32(self + 0x3f8) : 0;
    if (a.state >= GOAL_FIRST && a.state <= GOAL_LAST) a.cleared = 1;
    if (gap && a.k > 1) {  // (this update starts a new life: restarts.csv)
        fprintf(a.rst, "%lld,%lld,%.9g,%.9g,%u,%llu\n", (long long)(a.k - 1), (long long)a.death, a.x, a.y, a.state,
                (unsigned long long)gap);
        fflush(a.rst);
        a.restarts++;
        a.death = -1;
    }
    if (a.state >= DOWN_FIRST && a.state <= DOWN_LAST && a.death < 0) a.death = a.k - 1;
    fprintf(a.ply, "%lld,%.9g,%.9g,%u\n", (long long)(a.k - 1), a.x, a.y, a.state);
    fflush(a.csv), fflush(a.ply);  // (a run that ends with _exit, as a scripted one does, keeps every update)
    if (a.k % 60 == 0) fflush(a.pad), write_meta("running");
}
