// Scripted pad for input replays: see runtime/padscript.h.
#include "../padscript.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "../rt.h"

// A game with a clock of its own (a per-update hook) defines this as true (games/<game>/; weak default: none).
__attribute__((weak)) const bool g_game_pad_clock = false;

static uint64_t *frames;  // buttons per script frame
static int32_t (*sticks)[4];  // and the sticks (lx, ly, rx, ry; LX=n LY=n RX=n RY=n on the line, 0 when not given)
static int64_t nframes, start_tick;
static _Atomic int64_t ticks;       // clock ticks seen
static _Atomic uint64_t current;    // what the game reads
static _Atomic int64_t current_l, current_r;   // its sticks, (x << 32) | (uint32_t)y
static int loaded = -1;             // -1: not looked at yet
static bool game_clock;

int padscript_parse_button(const char *n, uint64_t *bit) {
    static const struct { const char *name; int b; } names[] = {
        {"A", 0}, {"B", 1}, {"X", 2}, {"Y", 3}, {"LSTICK", 4}, {"RSTICK", 5}, {"L", 6}, {"R", 7}, {"ZL", 8},
        {"ZR", 9}, {"PLUS", 10}, {"MINUS", 11}, {"LEFT", 12}, {"UP", 13}, {"RIGHT", 14}, {"DOWN", 15},
        {"JUMP", 0}, {"RUN", 3}, {"SPIN", 9},  // the trace scripts' names (Ryujinx tooling: JUMP=A RUN=Y SPIN=ZR)
    };
    if (!strcmp(n, "-")) { *bit = 0; return 1; }
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (!strcasecmp(n, names[i].name)) { *bit = 1ull << names[i].b; return 1; }
    return 0;
}

static void load(void) {
    loaded = 0;
    const char *path = getenv("RECOMP_PAD_SCRIPT");
    if (!path || !*path) return;
    FILE *f = fopen(path, "r");
    if (!f) FATAL("[pad] RECOMP_PAD_SCRIPT %s: cannot open\n", path);
    char line[1024];
    int64_t cap = 0, ln = 0;
    while (fgets(line, sizeof line, f)) {
        ln++;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *save = NULL, *tok = strtok_r(line, " \t\r\n", &save);
        if (!tok) continue;
        char *end;
        long n = strtol(tok, &end, 10);
        if (*end || n < 0) FATAL("[pad] %s:%lld: frame count expected, got \"%s\"\n", path, (long long)ln, tok);
        uint64_t b = 0;
        int32_t st[4] = {0, 0, 0, 0};
        while ((tok = strtok_r(NULL, " \t\r\n", &save))) {
            uint64_t bit;
            static const char *axes[4] = {"LX=", "LY=", "RX=", "RY="};
            int axis = -1;
            for (int k = 0; k < 4; k++)
                if (!strncasecmp(tok, axes[k], 3)) axis = k;
            if (axis >= 0) {
                long v = strtol(tok + 3, &end, 10);
                if (*end || v < -32768 || v > 32767) FATAL("[pad] %s:%lld: bad stick value \"%s\"\n", path, (long long)ln, tok);
                st[axis] = (int32_t)v;
                continue;
            }
            if (!padscript_parse_button(tok, &bit)) FATAL("[pad] %s:%lld: unknown button \"%s\"\n", path, (long long)ln, tok);
            b |= bit;
        }
        if (nframes + n > cap) {
            cap = (nframes + n) * 2 + 64;
            frames = realloc(frames, (size_t)cap * sizeof *frames);
            sticks = realloc(sticks, (size_t)cap * sizeof *sticks);
        }
        for (long i = 0; i < n; i++) { memcpy(sticks[nframes], st, sizeof st); frames[nframes++] = b; }
    }
    fclose(f);
    const char *clk = getenv("RECOMP_PAD_CLOCK");
    game_clock = clk ? !strcmp(clk, "game") : g_game_pad_clock;
    if (clk && strcmp(clk, "game") && strcmp(clk, "frame")) FATAL("[pad] RECOMP_PAD_CLOCK=%s: game or frame\n", clk);
    if (game_clock && !g_game_pad_clock) FATAL("[pad] RECOMP_PAD_CLOCK=game: this game has no clock hook\n");
    const char *st = getenv("RECOMP_PAD_START");
    start_tick = st ? strtoll(st, NULL, 0) : 0;
    loaded = 1;
    LOG("[pad] script %s: %lld frames, one per %s from tick %lld; keyboard and controllers left out\n", path,
        (long long)nframes, game_clock ? "game update" : "presented frame", (long long)start_tick);
}

bool padscript_active(void) {
    if (loaded < 0) load();
    return loaded == 1;
}

bool padscript_game_clock(void) { return padscript_active() && game_clock; }

uint64_t padscript_buttons(void) { return padscript_active() ? current : 0; }

void padscript_sticks(int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    int64_t l = padscript_active() ? current_l : 0, r = padscript_active() ? current_r : 0;
    *lx = (int32_t)(l >> 32), *ly = (int32_t)(uint32_t)l, *rx = (int32_t)(r >> 32), *ry = (int32_t)(uint32_t)r;
}

// RECOMP_PAD_ORIGIN=player: the presented-frame clock starts at the game's first player update (the same game frame
// in every runtime that hooks it: the Ryujinx trace build counts its frames from there too), not at boot.
static _Atomic int origin_set;
static _Atomic int64_t frames_since_origin = -1;
void padscript_origin(void) {
    if (!origin_set) { origin_set = 1; frames_since_origin = 0; LOG("[pad] frame clock origin: first player update\n"); }
}
int64_t padscript_frames(void) { return frames_since_origin; }
void padscript_frame(void) {  // each presented frame (runtime/hle/input.c)
    if (origin_set) frames_since_origin++;
    static int wait = -1;
    if (wait < 0) wait = getenv("RECOMP_PAD_ORIGIN") && !strcmp(getenv("RECOMP_PAD_ORIGIN"), "player");
    if (padscript_active() && !game_clock && (!wait || origin_set)) padscript_tick();
}

void padscript_tick(void) {
    if (!padscript_active()) return;
    int64_t t = ticks++;
    int64_t i = t - start_tick;
    bool in = i >= 0 && i < nframes;
    current_l = in ? ((int64_t)sticks[i][0] << 32) | (uint32_t)sticks[i][1] : 0;
    current_r = in ? ((int64_t)sticks[i][2] << 32) | (uint32_t)sticks[i][3] : 0;
    current = in ? frames[i] : 0;
    if (i == nframes) {
        LOG("[pad] script done (%lld frames)\n", (long long)nframes);
        if (getenv("RECOMP_PAD_EXIT")) { fflush(stderr); _exit(0); }  // (scripted runs that end with the script)
    }
}

int64_t padscript_pos(void) {
    int64_t p = ticks - start_tick;
    return p < 0 ? 0 : p > nframes ? nframes : p;
}
int64_t padscript_len(void) { return nframes; }
bool padscript_done(void) { return padscript_active() && ticks - start_tick >= nframes; }
