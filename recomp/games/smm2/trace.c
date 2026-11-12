// The player's per-frame update as a clock and a trace point, for input replays against Ryujinx.
//
// games/smm2/patches.py calls smm2_player_hook() at the entry of the player's per-frame update (main+0x158a5c4,
// v3.0.3: the instruction that keeps the player object in a callee-saved register). The same instruction is the one
// the Ryujinx trace build turns into `svc #0x7E` (research/tools/ryujinx-trace), so both runtimes tick and sample at
// the same point of the game's frame.
//
// - Clock: a scripted pad (RECOMP_PAD_SCRIPT, runtime/padscript.h) advances one script frame per update of the player.
// - Trace: RECOMP_TRACE_OUT=<file> writes one record per update, in the Ryujinx TraceHook format, so the same tools
//   read both: u32 'TRC1', i32 script frame, u64 player address, u32 buttons in effect, i32 length, the player's bytes,
//   i32 n, then n x (u64 pointer, i32 length, bytes) for RECOMP_TRACE_DEREF="off,len[ off,len...]" (pointers read
//   from the player bytes; default 0x550,0x200: the input block). RECOMP_TRACE_LEN (default 0x3000) bytes of the
//   player. The record at update k is written before script frame k takes effect, as TraceHook does.
// - Recording and the script's clock start together at the first update (RECOMP_TRACE_START=<n>: the n-th), and
//   follow the player object seen then (updates of another player are not counted; RECOMP_PAD_START=<n> delays the
//   script by n of these updates). The trace ends one record after the script's last frame (or after
//   RECOMP_TRACE_FRAMES records without a script), or when the followed player is gone (120 updates of other players
//   in a row: a death restarts the course with a new player object, and the script could never move on; Ryujinx's
//   recording stalls at the same point). RECOMP_TRACE_EXIT=1 then quits.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../runtime/hle.h"
#include "../../runtime/padscript.h"

const bool g_game_pad_clock = true;  // (runtime/hle/padscript.c: the player update is SMM2's script clock)

static FILE *out;
static int state;  // 0 not started, 1 recording / scripting, 2 done
static uint64_t tracked;
static int64_t hooks, start_at, rec_frames, n, others;
static int rec_len = 0x3000;
static int nder;
static struct { int off, len; } der[8];

static void begin(void) {
    const char *s = getenv("RECOMP_TRACE_START");
    start_at = s ? strtoll(s, NULL, 0) : 0;
    const char *path = getenv("RECOMP_TRACE_OUT");
    if (path && *path) {
        out = fopen(path, "wb");
        if (!out) FATAL("[trace] %s: cannot create\n", path);
        if ((s = getenv("RECOMP_TRACE_LEN"))) rec_len = (int)strtol(s, NULL, 0);
        const char *d = getenv("RECOMP_TRACE_DEREF");
        if (!d) d = "0x550,0x200";
        char buf[256];
        snprintf(buf, sizeof buf, "%s", d);
        char *save = NULL;
        for (char *t = strtok_r(buf, " ", &save); t && nder < 8; t = strtok_r(NULL, " ", &save)) {
            char *comma = strchr(t, ',');
            if (!comma) continue;
            der[nder].off = (int)strtol(t, NULL, 0);
            der[nder].len = (int)strtol(comma + 1, NULL, 0);
            nder++;
        }
        if ((s = getenv("RECOMP_TRACE_FRAMES"))) rec_frames = strtoll(s, NULL, 0);
        LOG("[trace] player updates to %s from update %lld (%#x bytes, %d derefs)\n", path, (long long)start_at,
            rec_len, nder);
    }
}

static int readable(uint64_t a, uint64_t n) { return a >= GUEST_LO && a + n <= GUEST_HI && a + n > a; }

static void put(const void *p, size_t n) { fwrite(p, 1, n, out); }

static void record(uint64_t self, int32_t idx, uint32_t inp) {
    uint32_t magic = 0x31435254u;
    put(&magic, 4), put(&idx, 4), put(&self, 8), put(&inp, 4), put(&rec_len, 4);
    static uint8_t zero[0x10000];
    put(readable(self, (uint64_t)rec_len) ? (const void *)(uintptr_t)self : zero, (size_t)rec_len);
    put(&nder, 4);
    for (int i = 0; i < nder; i++) {
        uint64_t ptr = der[i].off + 8 <= rec_len && readable(self + der[i].off, 8) ? LD64(self + der[i].off) : 0;
        put(&ptr, 8), put(&der[i].len, 4);
        put(ptr && readable(ptr, (uint64_t)der[i].len) ? (const void *)(uintptr_t)ptr : zero, (size_t)der[i].len);
    }
}

// RECOMP_TRACE_SHOT="k1,k2,...": a frame capture at those updates (record index k, as the trace numbers them): the
// game waits RECOMP_TRACE_SHOT_SETTLE ms (default 500, so the frame before is presented), then the next presented
// frame is saved raw, at its own size, to <RECOMP_TRACE_SHOT_DIR>/recomp_frame_<k>.png. The Ryujinx trace build does the same at the same
// updates (RYU_TRACE_SHOT): the two runtimes' frames then show the same game state, a clean render comparison.
static void shot_at(int64_t k) {
    static const char *spec;
    static int init;
    if (!init) { init = 1; spec = getenv("RECOMP_TRACE_SHOT"); }
    if (!spec) return;
    const char *p = spec;
    bool hit = false;
    while (*p && !hit) {
        char *end;
        long long v = strtoll(p, &end, 0);
        if (end == p) break;
        hit = v == k;
        p = *end ? end + 1 : end;
    }
    if (!hit) return;
    const char *dir = getenv("RECOMP_TRACE_SHOT_DIR"), *settle = getenv("RECOMP_TRACE_SHOT_SETTLE");
    usleep((useconds_t)(settle ? atoi(settle) : 500) * 1000);
    extern char g_frame_shot_request[256];  // (runtime/nvn: the next presented frame, at its own size)
    snprintf(g_frame_shot_request, sizeof g_frame_shot_request, "%s/recomp_frame_%lld.png", dir ? dir : "build", (long long)k);
    LOG("[trace] frame capture after update %lld -> %s\n", (long long)k, g_frame_shot_request);
    if (getenv("RECOMP_TRACE_SHOT_DEBUG")) {  // (debug) the captured frame is the debug frame (LARECOMP_DEBUG_*)
        extern bool g_debug_frame;
        g_debug_frame = true;
        LOG("===== debug frame (trace shot %lld) =====\n", (long long)k);
    }
}

static void finish(const char *why) {
    state = 2;
    if (out) { fclose(out); out = NULL; }
    LOG("[trace] end after %lld records (%s)\n", (long long)n, why);
    if (getenv("RECOMP_TRACE_EXIT")) { fflush(stderr); _exit(0); }
}

// In play or in a menu (runtime/hle/input.c's "A and B swapped in menus", #84): the player is updated each frame while
// a course runs, and not in the game's menus (Course World, the title's menus, the Course Maker while editing) or
// while it is paused. A few frames without an update: a menu.
static _Atomic uint64_t g_player_frame;
bool game_in_play(void) {
    extern uint64_t renderer_present_count(void);
    return renderer_present_count() < g_player_frame + 8;
}

void smm2_player_hook(Cpu *c) {
    uint64_t self = c->x[0];
    {
        extern uint64_t renderer_present_count(void);
        g_player_frame = renderer_present_count();
    }
    extern void costume_player_frame(Cpu *c, uint64_t player);  // (mods/costume.c: costumes, the taunt, the trigger)
    costume_player_frame(c, self);
    extern void lanky_player_frame(Cpu *c, uint64_t player);  // (mods/lanky.c: Lanky Mario's update)
    lanky_player_frame(c, self);
    extern void quick_retry_player(Cpu *c, uint64_t player);  // (mods/quick_retry.c: the restart after a miss)
    quick_retry_player(c, self);
    extern void smm1_extras_player_frame(void);  // (mods/smm1_extras.c: a debug log, RECOMP_SMM1_LOG)
    smm1_extras_player_frame();
    extern bool boot_holds(void);  // (mods/boot.c: a direct boot not yet in the course: the title's player is not it)
    if (boot_holds()) return;
    extern void boot_player_updated(uint64_t player);  // (mods/boot.c: when the course has ended)
    boot_player_updated(self);
    if (state == 2) return;
    if (state == 0) {
        static int init;
        if (!init) { init = 1; begin(); }
        if (!out && !padscript_active()) { state = 2; return; }
        if (hooks++ < start_at) return;
        state = 1;
        tracked = self;
        LOG("[trace] first update of player %llx\n", (unsigned long long)self);
        padscript_origin();  // (the presented-frame clock's origin when RECOMP_PAD_ORIGIN=player)
    }
    if (self != tracked) {
        if (++others == 120) finish("the followed player is no longer updated");
        return;
    }
    others = 0;
    shot_at(n);
    bool scripted = padscript_game_clock();
    int64_t pos = scripted ? padscript_pos() : 0;
    if (out) record(self, (int32_t)(scripted ? pos : n), (uint32_t)padscript_buttons());
    n++;
    bool done = scripted ? padscript_done() : rec_frames && n >= rec_frames;
    if (scripted) padscript_tick();
    if (out) fflush(out);  // (a run stopped from outside keeps every record so far)
    if (done) finish("done");
}
