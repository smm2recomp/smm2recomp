// A null renderer backend (HEADLESS=1 builds): no window, no GPU, no Vulkan. The NVN layer (nvn/objects.c,
// nvn/cmdbuf.c) records command lists as with any backend; this one draws nothing and does only what the game's CPU
// side can see of the GPU's work, as the Vulkan backend does it (nvn/vulkan/exec.c exec_list):
// - report values and counters written to guest memory (CMD_REPORT_VALUE, CMD_REPORT_COUNTER). The game times its GPU
//   work with the counters and drops to 30 fps when that looks slow (exec.c), so the counters here advance by a
//   fixed, small step: the game keeps 60 fps and runs repeat, whatever the host's speed;
// - buffer fills, buffer copies and uniform updates into guest memory (CMD_CLEAR_BUFFER, CMD_COPY_BUFFER_TO_BUFFER,
//   CMD_UPDATE_UNIFORM_BUFFER), and fence syncs (CMD_FENCE_SYNC);
// - nested lists (CMD_CALL, CMD_JUMP).
// Every submission is complete at once: fences are signalled by number, waits return.
//
// A present counts the frame and runs the per-frame hooks the Vulkan backend's present runs after its own work
// (nvn/vulkan/device.c backend_present: input, the function profiler, warps, the reboot test, the save editor).
// Pacing: run with RECOMP_NO_DISPLAY_PACE=1 to go as fast as the host (the game's logic is a fixed step per frame).
//
// The window loop (ui_run) polls SDL's events with no window (SDL's dummy video driver) and returns on a reboot
// request. The setup screen and the overlay menu (runtime/ui/*.mm) are not built: the game files must be set up
// beforehand (games/<game>/setup.py, sysdata.py), as in a source build.
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "../../rt.h"
#include "../nvn.h"

static uint64_t total_frames;
static uint64_t submissions;
static uint64_t counter_ticks;  // the fixed GPU clock of the reports (19.2 MHz ticks)

// ---- the command lists
static void walk(uint64_t handle, int depth) {
    uint64_t p = handle;
    for (int guard = 0; guard < 10000000; guard++) {
        CmdRec *r = (CmdRec *)(uintptr_t)p;
        const void *x = r->extra;
        switch (r->op) {
        case CMD_END: return;
        case CMD_JUMP: p = r->x[0]; continue;
        case CMD_CALL: {
            const uint64_t *h = x;
            for (uint64_t i = 0; i < r->x[0] && depth < 8; i++) walk(h[i], depth + 1);
            break;
        }
        case CMD_REPORT_VALUE: {  // {value, timestamp}
            uint64_t *out = (uint64_t *)(uintptr_t)r->x[1];
            counter_ticks += 16;
            if (out) { out[0] = (uint32_t)r->x[0]; out[1] = counter_ticks; }
            break;
        }
        case CMD_REPORT_COUNTER: {  // {0, timestamp}: a GPU frame of 16 ticks (under a microsecond)
            uint64_t *out = (uint64_t *)(uintptr_t)r->x[1];
            counter_ticks += 16;
            if (out) { out[0] = 0; out[1] = counter_ticks; }
            break;
        }
        case CMD_CLEAR_BUFFER: {  // ClearBuffer(address, size, value): 32-bit fill
            uint32_t *out = (uint32_t *)(uintptr_t)r->x[0];
            if (out)
                for (uint64_t i = 0; i < r->x[1] / 4; i++) out[i] = (uint32_t)r->x[2];
            break;
        }
        case CMD_COPY_BUFFER_TO_BUFFER: memmove((void *)(uintptr_t)r->x[1], (void *)(uintptr_t)r->x[0], r->x[2]); break;
        case CMD_UPDATE_UNIFORM_BUFFER: memcpy((void *)(uintptr_t)(r->x[0] + r->x[2]), x, r->x[3]); break;
        case CMD_FENCE_SYNC: {
            extern void nvn_sync_set_seq(uint64_t guest, uint64_t seq);
            if (r->x[1]) nvn_sync_set_seq(r->x[1], submissions + 1);
            break;
        }
        default: break;  // draws, clears, binds, texture copies, dispatches: GPU-only
        }
        p += r->size;
    }
}

void backend_init(void) { LOG("[null] headless renderer: nothing is drawn\n"); }
void backend_execute(uint64_t handle) {
    walk(handle, 0);
    submissions++;
}
void backend_prefetch(const uint64_t *handles, int n) { (void)handles; (void)n; }
void renderer_prefetch_verify(void) {}
void backend_texture_destroyed(NvnTexture *t) { (void)t; }
void backend_pool_written(uint64_t addr, uint64_t size) { (void)addr; (void)size; }
void backend_reset(void) {}
void backend_guest_memory_reset(void) {}
void backend_clear_window(void) {}

uint64_t renderer_fence_seq(void) { return submissions; }
void renderer_wait_seq(uint64_t seq) { (void)seq; }
void renderer_sync(void) {}
void renderer_flush(void) {}
uint64_t renderer_present_count(void) { return total_frames; }
void renderer_get_scale(float *sx, float *sy) { *sx = 1.0f, *sy = 1.0f; }
bool g_debug_frame;           // (a debug frame: nothing is drawn to debug; games/smm2/trace.c may set it)
void backend_represent(void) {}  // (a host dialog's re-present: no dialog headless)

void backend_present(NvnTexture *t) {
    (void)t;
    total_frames++;
    extern void input_frame(uint64_t frame);
    input_frame(total_frames);
    extern void fnprof_frame(uint64_t frame);
    fnprof_frame(total_frames);
    extern void warp_frame(uint64_t frame);
    warp_frame(total_frames);
    extern void reboot_test_frame(uint64_t frame);
    reboot_test_frame(total_frames);
    extern void save_editor_frame(uint64_t frame);
    save_editor_frame(total_frames);
    static long log_every = -1;  // RECOMP_NULL_LOG_EVERY=<n>: a log line every n frames (progress of a long run)
    if (log_every < 0) log_every = getenv("RECOMP_NULL_LOG_EVERY") ? atol(getenv("RECOMP_NULL_LOG_EVERY")) : 0;
    if (log_every > 0 && total_frames % (uint64_t)log_every == 0)
        LOG("[null] frame %llu\n", (unsigned long long)total_frames);
}

// ---- the window loop, without a window
int ui_run(void) {
    static bool inited;
    if (!inited) {
        inited = true;
        if (!SDL_Init(SDL_INIT_EVENTS)) FATAL("SDL_Init: %s\n", SDL_GetError());
    }
    for (;;) {
        if (rt_reboot_requested()) return rt_reboot_requested();
        SDL_Event e;
        while (SDL_WaitEventTimeout(&e, 16))
            if (e.type == SDL_EVENT_QUIT) { LOG("quit\n"); exit(0); }
    }
}

// ---- what runtime/ui/*.mm provides, not built headless: no setup screen (the game files are set up beforehand), no
// overlay menu, no on-screen controller
bool launcher_needed(const char *root, bool asked) { (void)root; (void)asked; return false; }
void launcher_run(const char *root) { (void)root; }
void launcher_boot_env(void) {}
void launcher_update_check_start(void) {}
bool menu_input_captured(void) { return false; }
int menu_keyboard_run(const char *initial, int max_len, char *out, int cap) {  // the name entry: cancelled
    (void)initial; (void)max_len; (void)out; (void)cap;
    return 0;
}
void menu_keyboard_title(const char *title) { (void)title; }
void menu_notice(const char *text, double seconds) { (void)seconds; LOG("[notice] %s\n", text ? text : ""); }
void online_sign_in_at_start(void) {}  // (no online sign-in at start headless)
void online_offline_notice(void) { LOG("[notice] offline\n"); }
bool touchpad_handle_event(const SDL_Event *e) { (void)e; return false; }
void touchpad_state(uint64_t *buttons, int32_t *lx, int32_t *ly) { *buttons = 0, *lx = 0, *ly = 0; }
