// Direct boot into Coursebot play (RECOMP_BOOT_COURSE): from the title screen, a course played the way Coursebot's
// Play plays one, without walking the menus. The same steps as smm2-hooks' direct boot in an emulator (its
// src/directboot.cpp, docs/direct-boot.md), so a run here and a recording made there start a course alike.
//
//   RECOMP_BOOT_COURSE=<file>   the course: a .bcd's 0x10-byte header and its decrypted body (0x5bfd0 bytes, "SCDL" at
//                               +0xc), as smm2-hooks' tools/directboot_cfg.py boot_bin_from_bcd writes it
//   RECOMP_BOOT_SLOT=<n>        the Coursebot slot the request names (default 0); play reads no course data from it
//   RECOMP_BOOT_KIND=<n>        the scene change's reason (default 4, cMyCourseToNormalPlay; 3 cRoboToEdit: the
//                               Course Maker)
//   RECOMP_BOOT_SETTLE=<n>      frames of title before the first request (default 150)
//   RECOMP_BOOT_KEEP=1          keep running once the course has ended (by default the run ends there)
//   RECOMP_BOOT_INTRO=1         keep the title's opening cutscene (by default a boot skips it, boot_skip_intro: the
//                               title takes the request about 140 frames sooner)
//
// Once the title is up (the scene mode 6) and has settled, the course goes into the save buffer's slot copy (+0x6c000,
// the copy Coursebot's own Play prepares and the course loader reads: games/smm2/mods/edit.c), and the four calls of
// My Courses' play start (main+0x16e5c10) run: the request's start, the play stats, the course source and the
// commit, which asks for the scene. A request the game does not take is made again every 90 frames, eight times at
// most. While the boot is on its way (boot_holds) the pad script's clock, the trace and the probes wait: the title
// screen plays a course of its own with a player in it, and the run starts at the first player update in the course
// (the scene mode 7). A boot that does not get there ends the program (exit 3), so a scripted run never records the
// title.
//
// When the course ends, so does the run (exit 0, the probe log flushed and a call graph written), as a recording made in
// an emulator ends with the course, on the first of:
// - no player update for 120 frames once the course has run (SMB1, SMB3, SMW: after a clear, the walk into the castle
//   is the last update; the scene mode stays 7 through the results);
// - after a clear (the player in a goal state, 121-128: GoalPoleClimb .. GoalDead), the player neither moving nor
//   changing state for 30 frames, out of the goal's own motion (121-127: the climb, the slide and the wait at the
//   pole's foot, the back jump, the poses; the wait alone is 90 frames, 3D World's pose about 170). In NSMBU and
//   3D World the player goes on updating after the walk, standing, so the first rule never comes; in lesson_002 the
//   crash below came 60 frames after the walk ended. goal-pole-smb1, -nsmbu and -3dw end with as many goal rows as
//   their Eden recordings (122, 213, 371 of 372);
// - the scene mode leaving 7.
// A pad script's clock counts player updates, so after a clear it may never reach its end (RECOMP_PAD_EXIT). On the
// title the clear goes back to the title course and its player; here, about 1300 frames after the clear, the game
// heads back to a Coursebot scene this boot never set up, registers a style's effects again into the full
// EffectStaticResInstance heap (12 MB, 1259 blocks in use), and a failed allocation for an emitter set
// (Enm_KoopaClown_Charge in lesson_002) is used as the emitters' array: a write through null (+0x58 in main+0x2beb60,
// from main+0x2d1520 on a loader thread). That is where NSMBU and 3D World runs crashed before the second rule.
// RECOMP_BOOT_KEEP=1 goes on anyway. Addresses are SMM2 v3.0.3, offsets in
// main.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mod.h"

#define GPM MAIN_ADDR(0x2c57d58)          // GamePhaseManager*: [+0x30]+0x14 the scene mode (6 title, 7 course play)
#define SAVE_MGR MAIN_ADDR(0x2a39088)     // *: +0x20 the save buffer
#define SLOT_COURSE 0x6c000               // save buffer: the slot copy the course loader reads
#define COURSE_BYTES (0x10 + 0x5bfc0)
#define REQ_BEGIN MAIN_ADDR(0x1792070)    // (reason, &{u64 -1, u32 -1})
#define REQ_PLAY_STATS MAIN_ADDR(0x1790480)
#define REQ_COURSE MAIN_ADDR(0x1792890)   // (reason, course number, &{...})
#define REQ_COMMIT MAIN_ADDR(0x1791020)   // (0)
#define MODE_TITLE 6
#define MODE_PLAY 7
#define RETRY_EVERY 90
#define MAX_TRIES 8

enum { OFF, WAITING, IN_COURSE };
static int state = -1;
static uint8_t *course;
static int slot, kind = 4, settle = 150;
static uint32_t title_frames, since_try, tries;
#define ENDED_AFTER 120   // frames without a player update: the course is over
static uint64_t last_update;   // the presented frame of the last player update in the course (0: none yet)
static bool cleared;           // the player has been in a goal state in the course
static uint64_t last_change;   // after the clear: the presented frame the player last moved or changed state
#define PLAYER_POS 0x230       // player: f32 x, y
#define PLAYER_STATE 0x3f8     // player: u32 the state (smm2-hooks' "player" preset)
#define GOAL_FIRST 121         // cState_GoalPoleClimb
#define GOAL_LAST 128          // cState_GoalDead
#define GOAL_MOTION_LAST 127   // cState_GoalPosing3W: 121-127 move or wait on their own (the pole, the jump, the poses)
#define STANDING_AFTER 30      // frames still after a clear: the course is over
extern uint64_t renderer_present_count(void);

// Each player update in the course (games/smm2/trace.c's player hook, `player` the player object).
void boot_player_updated(uint64_t player) {
    if (state != IN_COURSE) return;
    const uint64_t now = renderer_present_count();
    last_update = now;
    const uint64_t pos = g64(player + PLAYER_POS);
    const uint32_t st = g32(player + PLAYER_STATE);
    static uint64_t last_pos;
    static uint32_t last_state;
    if (st >= GOAL_FIRST && st <= GOAL_LAST && !cleared) {
        cleared = true;
        LOG("[boot] the course is cleared (player state %u)\n", st);
    }
    const bool pole = st >= GOAL_FIRST && st <= GOAL_MOTION_LAST;
    if (pos != last_pos || st != last_state || pole || !last_change) last_change = now;
    last_pos = pos;
    last_state = st;
}

static uint32_t scene_mode(void) {
    uint64_t gpm = g64(GPM);
    if (gpm < GUEST_LO || gpm >= GUEST_HI) return 0;
    uint64_t inner = g64(gpm + 0x30);
    if (inner < GUEST_LO || inner >= GUEST_HI) return 0;
    return g32(inner + 0x14);
}

static void fail(const char *why) {
    LOG("[boot] %s\n", why);
    fflush(stderr);
    _exit(3);
}

static void configure(void) {
    state = OFF;
    const char *path = getenv("RECOMP_BOOT_COURSE");
    if (!path || !*path) return;
    const char *e;
    if ((e = getenv("RECOMP_BOOT_SLOT"))) slot = atoi(e);
    if ((e = getenv("RECOMP_BOOT_KIND"))) kind = atoi(e);
    if ((e = getenv("RECOMP_BOOT_SETTLE"))) settle = atoi(e);
    FILE *f = fopen(path, "rb");
    if (!f) fail("cannot read RECOMP_BOOT_COURSE");
    course = malloc(COURSE_BYTES);
    size_t n = fread(course, 1, COURSE_BYTES, f);
    int more = fgetc(f) != EOF;
    fclose(f);
    if (n != COURSE_BYTES || more || memcmp(course + 0xc, "SCDL", 4))
        fail("RECOMP_BOOT_COURSE: not a course (a .bcd's 0x10-byte header and its decrypted body, SCDL at +0xc)");
    state = WAITING;
    LOG("[boot] %s into Coursebot play (slot %d, reason %d) after %d frames of title\n", path, slot, kind, settle);
}

// Whether the title skips its opening cutscene (games/smm2/patches.py's title phase): with a boot, unless
// RECOMP_BOOT_INTRO=1.
bool boot_skip_intro(void) {
    if (state < 0) configure();
    return state != OFF && !getenv("RECOMP_BOOT_INTRO");
}

bool boot_holds(void) {
    if (state < 0) configure();
    return state == WAITING;
}

// The request as My Courses' play start makes it (games/smm2/mods/edit.c request_edit, with this reason); the game's
// registers are kept around it, as the pad read it runs in returns to the game.
static void request(Cpu *c) {
    Cpu saved;
    memcpy(&saved, c, sizeof saved);
    uint64_t owner = g64(SAVE_MGR), buf = owner ? g64(owner + 0x20) : 0;
    if (buf < GUEST_LO || buf >= GUEST_HI) fail("the save buffer is not set up");
    memcpy((void *)(uintptr_t)(buf + SLOT_COURSE), course, COURSE_BYTES);
    c->sp -= 0x20;
    uint64_t params = c->sp;
    s64(params, ~0ull), s32(params + 8, ~0u);
    c->x[0] = (uint64_t)kind, c->x[1] = params, guest_call(c, REQ_BEGIN);
    c->x[0] = (uint64_t)kind, guest_call(c, REQ_PLAY_STATS);
    s64(params, ~0ull), s32(params + 8, ~0u);
    c->x[0] = (uint64_t)kind, c->x[1] = (uint32_t)slot, c->x[2] = params, guest_call(c, REQ_COURSE);
    c->x[0] = 0, guest_call(c, REQ_COMMIT);
    uint64_t asked = c->x[0];
    memcpy(c, &saved, sizeof saved);
    tries++;
    since_try = 0;
    LOG("[boot] requested (try %u, commit -> %llx)\n", tries, (unsigned long long)asked);
}

void boot_game_tick(Cpu *c) {
    if (boot_holds(), state == OFF) return;
    const uint32_t mode = scene_mode();
    static uint32_t last_mode = ~0u;
    if (mode != last_mode) {   // the scene modes the boot sees, in the log
        LOG("[boot] scene mode %u\n", mode);
        last_mode = mode;
    }
    const uint64_t now = renderer_present_count();
    const bool still = last_update && now - last_update > ENDED_AFTER;
    const bool standing = cleared && last_change && now - last_change > STANDING_AFTER;
    if (state == IN_COURSE && last_update && (still || standing || mode != MODE_PLAY) && !getenv("RECOMP_BOOT_KEEP")) {
        LOG("[boot] the course ended (%s): the run ends with it\n", still ? "no player update for 120 frames"
            : standing ? "cleared, the player still for 30 frames" : "a scene change");
        // _exit, not exit: the game's own threads are mid-course and its teardown is not wanted here. What a run
        // writes is written as it goes (a play recording flushes every update: games/smm2/playrec.c), so only this
        // process's own buffers are left.
        fflush(NULL);
        _exit(0);
    }
    if (state != WAITING) return;
    if (mode == MODE_PLAY && tries) {
        state = IN_COURSE;
        LOG("[boot] in Coursebot play after %u frames of title and %u tries\n", title_frames, tries);
        return;
    }
    if (mode != MODE_TITLE) {
        if (tries && ++since_try > 600) fail("the request left the title but no course play followed");
        return;
    }
    title_frames++;
    since_try++;
    if (title_frames < (uint32_t)settle) return;
    if (tries && since_try < RETRY_EVERY) return;
    if (tries >= MAX_TRIES) fail("the game took none of the requests");
    request(c);
}
