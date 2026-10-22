// Executes recorded NVN command lists with Metal.
#include <pthread.h>
#include <dirent.h>
#include <unistd.h>
#import <Metal/Metal.h>
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#define DEPTH24_STENCIL8_FORMAT MTLPixelFormatInvalid  // (macOS only)
#else
#define DEPTH24_STENCIL8_FORMAT MTLPixelFormatDepth24Unorm_Stencil8
#endif
#include <mach/mach_time.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <math.h>
#include <sys/stat.h>
#include <SDL3/SDL_timer.h>

#include "maxwell/shader.h"
#include "nvn.h"
#include "renderer.h"
#include "buildsync.h"

static id<MTLDevice> dev;
static id<MTLCommandBuffer> cmdbuf;
static id<MTLRenderCommandEncoder> enc;
static id<MTLRenderPipelineState> clear_pipes[64];
static NSMutableDictionary<NSString *, id> *cache;
// (stats) GPU wait time by cause: 0 = guest fence (nvnSyncWait), 1 = full flush (CPU reads GPU results / ring), 2 = sync all
static uint64_t stat_wait_by[3], stat_wait_n[3];
static uint64_t stat_draws, stat_skipped, stat_passes, stat_dispatches, stat_wait_ns, stat_flushes, stat_feedback_copies;
static double stat_gpu_s;
uint64_t g_gpu_total_ns;  // GPU time of all completed command buffers (dynamic resolution: output_scale.c)
// (forward: texture-lifetime cleanup and stats, defined further down)
static int g_views_live;
static void views_forget(void *base);
static NSMutableDictionary *g_snap_copies;
static NvnTexture *snap_tex[8];
static bool snap_ok[8];
static uint64_t op_counts[CMD_COUNT];
bool g_debug_frame;
static bool debug_draw_fs_match(uint64_t fs) {  // LARECOMP_DEBUG_DRAW_FS (unset: every draw)
    const char *e = getenv("LARECOMP_DEBUG_DRAW_FS");
    if (!e || !*e) return true;
    for (const char *p = e; *p;) {
        char *end;
        unsigned long long v = strtoull(p, &end, 16);
        if (end == p) break;
        if (v == fs) return true;
        p = *end == ',' ? end + 1 : end;
    }
    return false;
}
void renderer_save_texture(id<MTLTexture> tex, const char *path);
void renderer_save_texture_alpha(id<MTLTexture> tex, const char *path);
static char stats_text[320];

// ------------------------------------------------------------------ state
typedef struct {
    NvnTexture *color[8];
    NvnTextureView cview[8];
    uint8_t has_cview[8];
    int ncolor;
    NvnTexture *depth;
    NvnTextureView dview;
    uint8_t has_dview;
} RtState;

typedef struct { uint64_t addr, size; } Range;

typedef struct {
    RtState rt;
    bool rt_dirty;
    NvnProgram *program;
    uint8_t blend[8][8];
    uint32_t channel_mask;
    uint8_t color_state[4], ds[8], ms[0x18], poly[4];
    float poly_offset[3];  // factor, units, clamp (nvnCommandBufferSetPolygonOffsetClamp)
    bool depth_clamp;      // nvnCommandBufferSetDepthClamp: depth clamped instead of clipped (shadow maps)
    uint32_t attrib[16];
    int nattrib;
    uint32_t stream[16][2];
    int nstream;
    Range vbuf[16], ubuf[6][18], sbuf[6][16];
    uint64_t tex[6][32], img[6][8];
    NvnDescPool *tpool, *spool;
    float viewport[16][4];
    int32_t scissor[16][4];
    float depth_range[16][2];
    float blend_color[4];
    uint32_t stencil_ref[2], stencil_mask[2], stencil_vmask[2];
    bool prim_restart;
    bool render_off;  // SetRenderEnable(false) / a failed SetRenderEnableConditional: draws and clears are skipped
    uint32_t restart_index;
    uint64_t copy_row_stride, copy_image_stride;
} ExecState;
static ExecState st;

extern uint32_t g_window_origin_lower_left_hack;
uint32_t nvn_window_origin(void);
bool renderer_origin_lower_left(void) { return nvn_window_origin() == 0; }

static double stat_compile_ms;  // time spent building shaders/pipelines (first-use stutter)
static int stat_compiles;
static int stat_precompiled;  // shaders loaded precompiled (cache/metallib/)
static int stat_cached;  // pipelines taken from the pipeline cache, and the time that took
static double stat_cached_ms;
static inline double now_ms(void) { return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6; }
#define TIMED_COMPILE(expr) ({ double t0_ = now_ms(); __auto_type r_ = (expr); stat_compile_ms += now_ms() - t0_; stat_compiles++; r_; })

static void pipeline_cache_tick(void);
const char *renderer_stats(void) {
    pipeline_cache_tick();
    static int dumps;
    if (dumps++ < 3) {
        LOG("[metal] command counts:");
        for (int i = 0; i < CMD_COUNT; i++) if (op_counts[i]) LOG(" %d:%llu", i, op_counts[i]);
        LOG("\n");
    }
    extern int g_tex_live;
    extern uint64_t g_tex_live_bytes;
    struct task_vm_info vmi;
    mach_msg_type_number_t vn = TASK_VM_INFO_COUNT;
    uint64_t foot = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vmi, &vn) == KERN_SUCCESS ? vmi.phys_footprint : 0;
    snprintf(stats_text, sizeof stats_text, "%llu draws, %llu skipped, %llu passes, %llu dispatches, %llu flushes, gpu %.0fms, wait %.0fms, compile %d/%.0fms, cached %d/%.0fms, precompiled %d, fbcopy %llu | tex %d (%.0f MB) views %d snaps %lu mem %.0f MB | waits fence %.0fms/%llu flush %.0fms/%llu all %.0fms/%llu", stat_draws, stat_skipped,
             stat_passes, stat_dispatches, stat_flushes, stat_gpu_s * 1000.0, (double)stat_wait_ns / 1e6 * 125.0 / 3.0, stat_compiles, stat_compile_ms, stat_cached, stat_cached_ms, stat_precompiled, stat_feedback_copies,
             g_tex_live, g_tex_live_bytes / 1048576.0, g_views_live, (unsigned long)(g_snap_copies ? g_snap_copies.count : 0), foot / 1048576.0, stat_wait_by[0] / 24e3, (unsigned long long)stat_wait_n[0], stat_wait_by[1] / 24e3, (unsigned long long)stat_wait_n[1], stat_wait_by[2] / 24e3, (unsigned long long)stat_wait_n[2]);
    stat_compiles = 0;
    stat_feedback_copies = 0;
    stat_compile_ms = 0;
    stat_cached = stat_precompiled = 0;
    stat_cached_ms = 0;
    stat_draws = stat_skipped = stat_passes = stat_dispatches = stat_flushes = stat_wait_ns = 0;
    memset(stat_wait_by, 0, sizeof stat_wait_by); memset(stat_wait_n, 0, sizeof stat_wait_n);
    stat_gpu_s = 0;
    return stats_text;
}

static void st_defaults(void) {
    memset(&st, 0, sizeof st);
    for (int i = 0; i < 16; i++) {
        st.viewport[i][2] = 1280; st.viewport[i][3] = 720;
        st.scissor[i][2] = 16384; st.scissor[i][3] = 16384;
        st.depth_range[i][1] = 1.0f;
    }
    st.channel_mask = 0xFFFFFFFF;
    st.stencil_mask[0] = st.stencil_mask[1] = 0xFF;
    st.stencil_vmask[0] = st.stencil_vmask[1] = 0xFF;
    st.ds[1] = 1; st.ds[2] = 2;
    st.poly[1] = 1; st.poly[2] = 2;
    for (int i = 0; i < 8; i++) { st.blend[i][1] = 2; st.blend[i][2] = 1; st.blend[i][3] = 2; st.blend[i][4] = 1; st.blend[i][5] = 1; st.blend[i][6] = 1; }
}

void renderer_init(id<MTLDevice> d) {
    dev = d;
    cache = [NSMutableDictionary new];
    st_defaults();
}

static int g_passes_in_cb;  // render passes encoded into the open command buffer (chunked commits)
static id<MTLCommandBuffer> cb(void) {
    if (!cmdbuf) cmdbuf = [renderer_queue() commandBuffer];
    return cmdbuf;
}

void renderer_texture_scale(NvnTexture *t, float *sx, float *sy);
void renderer_get_scale(float *sx, float *sy);
static float pass_sx = 1, pass_sy = 1;  // allocation scale of the current pass's attachments
bool renderer_texture_presented(NvnTexture *t);
uint64_t renderer_present_count(void);
// Widescreen UI: 2D draws into the swapchain image use a centred 16:9 region (uniform scale) instead of
// the stretched wide mapping. vp_* is the mapping for the current draw: physical = guest * s + off.
static float vp_sx = 1, vp_sy = 1, vp_ox = 0, vp_oy = 0;
static float tex_sc[2][64][2];          // per bound texture slot: allocation scale (for shaders' texel math)
static uint32_t tex_wh[2][64][2];       // per bound texture slot: guest size
static int draw_bottle;                 // a fragment texture is the bottle's item icon (1) / model map (2): Escape Bottle
static int draw_heart;                  // ... a HUD / file select heart's: heart colour (Cosmetics tab)
static bool draw_item_cursor;           // a fragment texture is the Items page's cursor brackets
#define RSCALE_BUFFER_INDEX 18

static int debug_pass_no;
static void end_pass(void) {
    if (enc) {
        [enc endEncoding];
        enc = nil;
        static int no_pass_png = -1;  // LARECOMP_DEBUG_NO_PASSES=1: the debug frame's log without its pass images
        if (no_pass_png < 0) no_pass_png = getenv("LARECOMP_DEBUG_NO_PASSES") != NULL;
        if (g_debug_frame && !no_pass_png && st.rt.ncolor && st.rt.color[0] && st.rt.color[0]->backend) {
            id<MTLTexture> t = renderer_texture(st.rt.color[0], nil, true);
            id<MTLCommandBuffer> c = cmdbuf;
            cmdbuf = nil;
            [c commit];
            [c waitUntilCompleted];
            char path[256];
            snprintf(path, sizeof path, "build/pass_%03d.png", debug_pass_no);
            renderer_save_texture(t, path);
            snprintf(path, sizeof path, "build/pass_%03d_a.png", debug_pass_no++);
            renderer_save_texture_alpha(t, path);
        }
    }
}

static void ring_reset(void);
// Submitted work runs asynchronously; last_cb is the most recently committed command buffer (the queue
// executes in order, so waiting for it waits for everything before it). Other guest threads (nvnSyncWait)
// read last_cb, so it is guarded by cb_lock; everything else here runs on the submitting thread.
static id<MTLCommandBuffer> last_cb;
static pthread_mutex_t cb_lock = PTHREAD_MUTEX_INITIALIZER;
// Fences: each committed command buffer gets a sequence number; a fence remembers the number of the submission
// that contains it, and nvnSyncWait waits only for that one (not for everything submitted since), so the game's
// CPU work on frame N+1 overlaps the GPU's frame N as on the Switch.
#define CB_HIST 256
static uint64_t submit_seq;
static id<MTLCommandBuffer> cb_hist[CB_HIST];

static int g_wait_kind;
static void wait_cb(id<MTLCommandBuffer> c) {
    if (!c) return;
    uint64_t t0 = mach_absolute_time();
    [c waitUntilCompleted];
    uint64_t d = mach_absolute_time() - t0;
    stat_wait_ns += d;
    stat_wait_by[g_wait_kind] += d;
    if (d > 24000) stat_wait_n[g_wait_kind]++;  // (> ~1 ms counts as a real wait)
}

// Textures the game destroyed (any thread): their cached views and snapshot copies are dropped on the render thread.
static pthread_mutex_t gone_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { void *nvn, *mtl; } gone[4096];
static int ngone;
void renderer_texture_gone(void *nvn_tex, void *mtl_tex) {
    pthread_mutex_lock(&gone_lock);
    if (ngone < 4096) { gone[ngone].nvn = nvn_tex; gone[ngone].mtl = mtl_tex; ngone++; }
    pthread_mutex_unlock(&gone_lock);
}
static void drain_gone(void) {
    pthread_mutex_lock(&gone_lock);
    int n = ngone;
    static struct { void *nvn, *mtl; } local[4096];
    memcpy(local, gone, sizeof local[0] * n);
    ngone = 0;
    pthread_mutex_unlock(&gone_lock);
    for (int i = 0; i < n; i++) {
        if (local[i].mtl) views_forget(local[i].mtl);
        if (local[i].nvn) {
            if (g_snap_copies) [g_snap_copies removeObjectForKey:[NSValue valueWithPointer:local[i].nvn]];
            for (int k = 0; k < 8; k++) if (snap_tex[k] == local[i].nvn) { snap_tex[k] = NULL; snap_ok[k] = false; }
        }
    }
}

// Commit recorded work without waiting.
void renderer_submit(void) {
    end_pass();
    drain_gone();
    if (cmdbuf) {
        id<MTLCommandBuffer> c = cmdbuf;
        cmdbuf = nil;
        [c addCompletedHandler:^(id<MTLCommandBuffer> done) {
            stat_gpu_s += done.GPUEndTime - done.GPUStartTime;
            __atomic_add_fetch(&g_gpu_total_ns, (uint64_t)((done.GPUEndTime - done.GPUStartTime) * 1e9), __ATOMIC_RELAXED);
            if (done.error) LOG("[metal] command buffer error: %s\n", done.error.localizedDescription.UTF8String);
        }];
        [c commit];
        g_passes_in_cb = 0;
        pthread_mutex_lock(&cb_lock);
        last_cb = c;
        submit_seq++;
        cb_hist[submit_seq % CB_HIST] = c;
        pthread_mutex_unlock(&cb_lock);
        stat_flushes++;
    }
}

// Guest memory the GPU reads when it executes, not when we encode: vertex, index and storage buffers (bound in
// place in the heap). Commands the guest GPU runs in order but we apply on the CPU while encoding
// (UpdateUniformBuffer) must not overwrite such memory under draws that are encoded and not yet run: SMM2 updates a
// buffer between draws that read it, and every draw saw the last update (3D World terrain stretched across the
// screen). Marked in 64 KB pages since the last renderer_flush (which waits for the GPU).
#define GPU_READ_PAGE_SHIFT 16
static uint64_t gpu_read_bits[(HEAP_MAX >> GPU_READ_PAGE_SHIFT) / 64 + 1];
static bool gpu_read_any;
static void gpu_read_mark(uint64_t addr, uint64_t size) {
    if (!size || addr < HEAP_BASE || addr >= HEAP_BASE + HEAP_MAX) return;
    uint64_t end = addr + size > HEAP_BASE + HEAP_MAX ? HEAP_BASE + HEAP_MAX : addr + size;
    for (uint64_t pg = (addr - HEAP_BASE) >> GPU_READ_PAGE_SHIFT; pg <= (end - 1 - HEAP_BASE) >> GPU_READ_PAGE_SHIFT; pg++)
        gpu_read_bits[pg >> 6] |= 1ull << (pg & 63);
    gpu_read_any = true;
}
static bool gpu_read_pending(uint64_t addr, uint64_t size) {
    if (!gpu_read_any || !size || addr < HEAP_BASE || addr >= HEAP_BASE + HEAP_MAX) return false;
    uint64_t end = addr + size > HEAP_BASE + HEAP_MAX ? HEAP_BASE + HEAP_MAX : addr + size;
    for (uint64_t pg = (addr - HEAP_BASE) >> GPU_READ_PAGE_SHIFT; pg <= (end - 1 - HEAP_BASE) >> GPU_READ_PAGE_SHIFT; pg++)
        if (gpu_read_bits[pg >> 6] & (1ull << (pg & 63))) return true;
    return false;
}
static void gpu_read_clear(void) {
    if (gpu_read_any) memset(gpu_read_bits, 0, sizeof gpu_read_bits);
    gpu_read_any = false;
}

// Finish all recorded GPU work (the CPU is about to read GPU results, or reuse the ring).
void renderer_flush(void) {
    renderer_submit();
    pthread_mutex_lock(&cb_lock);
    id<MTLCommandBuffer> c = last_cb;
    pthread_mutex_unlock(&cb_lock);
    g_wait_kind = 1;
    wait_cb(c);
    g_wait_kind = 0;
    ring_reset();
    gpu_read_clear();
}

// Wait for submitted work without committing anything new (nvnSyncWait / QueueFinish).
// Callable from any thread: everything submitted so far was committed by renderer_submit.
void renderer_sync(void) {
    pthread_mutex_lock(&cb_lock);
    id<MTLCommandBuffer> c = last_cb;
    pthread_mutex_unlock(&cb_lock);
    g_wait_kind = 2;
    wait_cb(c);
    g_wait_kind = 0;
}

// Sequence number a fence placed now belongs to: the pending (uncommitted) work's submission, else the last one.
uint64_t renderer_fence_seq(void) {
    pthread_mutex_lock(&cb_lock);
    uint64_t q = submit_seq + (cmdbuf ? 1 : 0);
    pthread_mutex_unlock(&cb_lock);
    return q;
}

// Wait until submission `seq` has completed (0 = nothing to wait for).
void renderer_wait_seq(uint64_t seq) {
    if (!seq) return;
    if (ENV("LARECOMP_LOG_FENCES")) {  // (debug) how far behind the newest submission the waited fence is
        extern uint64_t renderer_present_count(void);
        static uint64_t f0, hist[6], frames_seen;
        uint64_t f = renderer_present_count();
        uint64_t pend = submit_seq + (cmdbuf ? 1 : 0);
        uint64_t age = pend > seq ? pend - seq : 0;
        bool done_already = false;
        pthread_mutex_lock(&cb_lock);
        if (seq <= submit_seq && submit_seq - seq < CB_HIST && cb_hist[seq % CB_HIST])
            done_already = cb_hist[seq % CB_HIST].status >= MTLCommandBufferStatusCompleted;
        pthread_mutex_unlock(&cb_lock);
        hist[age > 4 ? 4 : age]++;
        if (done_already) hist[5]++;
        if (f - f0 >= 300) {
            frames_seen = f - f0;
            LOG("[fences] per %llu frames: waits by age (submissions behind newest) 0:%llu 1:%llu 2:%llu 3:%llu 4+:%llu, already done %llu; submissions now %llu\n",
                (unsigned long long)frames_seen, hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], (unsigned long long)submit_seq);
            memset(hist, 0, sizeof hist);
            f0 = f;
        }
    }
    pthread_mutex_lock(&cb_lock);
    id<MTLCommandBuffer> c = nil;
    if (seq > submit_seq) c = last_cb;  // not committed yet (other thread): wait for everything committed so far
    else if (submit_seq - seq < CB_HIST) c = cb_hist[seq % CB_HIST];
    pthread_mutex_unlock(&cb_lock);
    wait_cb(c);
}

// ------------------------------------------------------------------ textures with views
// Texture view cache: views are made per bind otherwise (an allocation per draw). Keyed by the base texture and
// the view parameters; a cached view retains its base, so a key's base pointer can't be reused while cached.
#define VIEW_TAB 16384
static struct { uint64_t key; void *base; void *view; } view_tab[VIEW_TAB];

// Views keep their base texture alive: when the game frees a texture its views must go too (they piled up -
// render targets of a 32:9 window held long after the game dropped them - and a new texture at the same address
// could get a stale view). Entries are tombstoned (key ~0) so probe chains stay intact.
static void views_forget(void *base) {
    for (int i = 0; i < VIEW_TAB; i++)
        if (view_tab[i].base == base && view_tab[i].view) {
            CFRelease(view_tab[i].view);
            view_tab[i].view = NULL;
            view_tab[i].base = NULL;
            view_tab[i].key = ~0ull;
            g_views_live--;
        }
}

static id<MTLTexture> view_cached(id<MTLTexture> base, uint64_t params, id<MTLTexture> (^make)(void)) {
    uint64_t k = ((uint64_t)(uintptr_t)(__bridge void *)base * 0x9E3779B97F4A7C15ull) ^ (params * 0xC2B2AE3D27D4EB4Full);
    if (!k) k = 1;
    if (k == ~0ull) k = 2;
    // up to 8 probes; a miss is stored in the first free or tombstoned slot of those, else over the first one.
    // (It used to go to the slot past the 8 probes - never found again - and tombstones were never reused: on the
    // overworld, where textures stream in and out, more and more binds made a new view every time, the slowdown
    // that grew with play time until a level change - user report 2026-10-08, 32:9.)
    uint32_t start = (uint32_t)(k ^ (k >> 32)) & (VIEW_TAB - 1), slot = start;
    int free_slot = -1;
    for (int probe = 0; probe < 8; probe++, slot = (slot + 1) & (VIEW_TAB - 1)) {
        if (view_tab[slot].key == k && view_tab[slot].base == (__bridge void *)base) return (__bridge id<MTLTexture>)view_tab[slot].view;
        if (!view_tab[slot].key || view_tab[slot].key == ~0ull) {
            if (free_slot < 0) free_slot = (int)slot;
            if (!view_tab[slot].key) break;  // (end of the chain)
        }
    }
    slot = free_slot >= 0 ? (uint32_t)free_slot : start;
    id<MTLTexture> v = make();
    if (!v) return base;
    if (ENV("LARECOMP_LOG_VIEWCREATE")) {  // (debug: what keeps making new views - overworld soak, views pile up)
        extern uint64_t renderer_present_count(void);
        static int n;
        if (renderer_present_count() > (uint64_t)atol(ENV("LARECOMP_LOG_VIEWCREATE")) && n++ < 400)
            LOG("[viewnew] f%llu base %p %s %lux%lu fmt %lu type %lu params %#llx live %d\n", (unsigned long long)renderer_present_count(),
                (__bridge void *)base, base.label.UTF8String, (unsigned long)base.width, (unsigned long)base.height,
                (unsigned long)base.pixelFormat, (unsigned long)base.textureType, (unsigned long long)params, g_views_live);
    }
    if (view_tab[slot].view) { CFRelease(view_tab[slot].view); g_views_live--; }  // evict (8 probes all taken)
    g_views_live++;
    view_tab[slot].key = k;
    view_tab[slot].base = (__bridge void *)base;
    view_tab[slot].view = (void *)CFBridgingRetain(v);
    return v;
}

// A view's pixel format over a texture of format base (view_texture's rule; pass_layout predicts with it).
static MTLPixelFormat view_pixel_format(MTLPixelFormat base, const NvnTextureView *v, bool log) {
    MTLPixelFormat pf = (v->setmask & 4) ? renderer_pixel_format(v->format) : base;
    // Metal can't view a block-compressed texture as an uncompressed format or the other way round (it asserts):
    // such a view keeps the texture's own format. (Seen: the Course World top screen's Maker panel, an RGBA8 sRGB
    // texture viewed as ASTC 4x4.)
    bool pf_bc = pf >= MTLPixelFormatBC1_RGBA && pf <= 235, base_bc = base >= MTLPixelFormatBC1_RGBA && base <= 235;
    // (between two compressed formats only an sRGB / linear twin works: BC n and n_sRGB, ASTC LDR and sRGB of one size)
    #define BLOCK_FAMILY(f) ((f) >= 204 && (f) <= 217 ? 1000 + (int)(f) - 204 : (f) >= 186 && (f) <= 199 ? 1000 + (int)(f) - 186 \
                             : (f) >= 130 && (f) <= 143 ? 2000 + ((int)(f) - 130) / 2 : (int)(f))
    if (pf_bc != base_bc || (pf_bc && pf != base && BLOCK_FAMILY(pf) != BLOCK_FAMILY(base))) {
        static int nmix;
        if (log && nmix++ < 5) LOG("[view] compressed / uncompressed view %lu -> %lu kept as %lu\n", (unsigned long)base, (unsigned long)pf, (unsigned long)base);
        pf = base;
    }
    return pf;
}

static id<MTLTexture> view_texture(NvnTexture *t, const NvnTextureView *v, bool has_view, bool render,
                                   id<MTLBlitCommandEncoder> blit) {
    id<MTLTexture> base = renderer_texture(t, blit, render);
    if (!has_view || !v->setmask) return base;
    MTLPixelFormat pf = view_pixel_format(base.pixelFormat, v, true);
    NSUInteger lv0 = (v->setmask & 1) ? v->base_level : 0;
    NSUInteger nlv = (v->setmask & 1) && v->num_levels ? v->num_levels : base.mipmapLevelCount - lv0;
    NSUInteger ly0 = (v->setmask & 2) ? v->min_layer : 0;
    // (a cube has 6 slices; Metal's arrayLength is 1 for it)
    NSUInteger total = base.textureType == MTLTextureTypeCube ? 6 : base.textureType == MTLTextureTypeCubeArray ? base.arrayLength * 6 : base.arrayLength;
    if (ly0 >= total) ly0 = total - 1;
    NSUInteger nly = (v->setmask & 2) && v->num_layers ? v->num_layers : total - ly0;
    if (ly0 + nly > total) nly = total - ly0;
    MTLTextureType type = base.textureType;
    if (type == MTLTextureTypeCube && nly != 6) type = nly == 1 ? MTLTextureType2D : MTLTextureType2DArray;  // (some faces)
    if (render && (type == MTLTextureType2DArray || type == MTLTextureTypeCube) && nly == 1) type = MTLTextureType2D;
    if (lv0 + nlv > base.mipmapLevelCount) nlv = base.mipmapLevelCount - lv0;
    if (pf == base.pixelFormat && lv0 == 0 && nlv == base.mipmapLevelCount && ly0 == 0 && type == base.textureType)
        return base;
    static int vlog;  // LARECOMP_LOG_VIEWS: texture views that change the pixel format
    if (pf != base.pixelFormat && vlog < 20 && getenv("LARECOMP_LOG_VIEWS")) {
        vlog++;
        LOG("[view] tex fmt %#x (mtl %lu) viewed as nvn %#x (mtl %lu)\n", t->format, (unsigned long)base.pixelFormat, v->format, (unsigned long)pf);
    }
    NSUInteger ns = type == MTLTextureType2D ? 1 : nly;
    uint64_t params = (uint64_t)pf | (uint64_t)type << 16 | (uint64_t)lv0 << 24 | (uint64_t)nlv << 32 | (uint64_t)ly0 << 40 | (uint64_t)ns << 52;
    id<MTLTexture> got = view_cached(base, params, ^{
        return [base newTextureViewWithPixelFormat:pf textureType:type levels:NSMakeRange(lv0, nlv) slices:NSMakeRange(ly0, ns)];
    });
    if (got == base && pf != base.pixelFormat) {
        static int nfail;
        if (nfail++ < 10) LOG("[view] FAILED %lu -> %lu (nvn %#x -> %#x)\n", (unsigned long)base.pixelFormat, (unsigned long)pf, t->format, v->format);
    }
    return got;
}

// ------------------------------------------------------------------ render passes
static __unsafe_unretained id last_bias_enc;  // encoder the depth bias was last set on

// The iOS Simulator's Metal allows 32 bytes of colour targets per sample (devices: 64): there, targets past that
// budget are left out of the pass and the pipelines (their writes are lost: the Simulator shows a little less).
static bool rt_dropped_in(const ExecState *s, int index) {
#if TARGET_OS_SIMULATOR
    uint32_t bytes = 0;
    for (int i = 0; i <= index && i < s->rt.ncolor; i++) {
        if (!s->rt.color[i]) continue;
        MTLPixelFormat pf = renderer_pixel_format(s->rt.color[i]->format);
        uint32_t b = pf == MTLPixelFormatRGBA32Float || pf == MTLPixelFormatRGBA32Uint || pf == MTLPixelFormatRGBA32Sint ? 16
                   : pf == MTLPixelFormatRGBA16Float || pf == MTLPixelFormatRG32Float || pf == MTLPixelFormatRGBA16Unorm ||
                     pf == MTLPixelFormatRGBA16Uint || pf == MTLPixelFormatRGBA16Sint || pf == MTLPixelFormatRG32Uint ||
                     pf == MTLPixelFormatRGBA8Unorm_sRGB || pf == MTLPixelFormatBGRA8Unorm_sRGB || pf == MTLPixelFormatRG11B10Float ||
                     pf == MTLPixelFormatRGB10A2Unorm || pf == MTLPixelFormatRGB10A2Uint || pf == MTLPixelFormatRGB9E5Float ? 8 : 4;
        // (Apple GPU tile storage: sRGB 8-bit, packed float and 10-bit formats take 8 bytes)
        bytes = (bytes + b - 1) / b * b + b;  // (each target aligned to its own size)
    }
    if (bytes > 32) {
        static int nlog;
        if (nlog++ < 3) LOG("[metal] Simulator: colour target %d dropped (%u bytes per sample)\n", index, bytes);
        return true;
    }
#else
    (void)s, (void)index;
#endif
    return false;
}
static bool rt_dropped(int index) { return rt_dropped_in(&st, index); }

static void begin_pass(void) {
    if (enc && !st.rt_dirty) return;
    end_pass();
    st.rt_dirty = false;
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    int w = 0, h = 0;
    for (int i = 0; i < st.rt.ncolor; i++) {
        NvnTexture *t = st.rt.color[i];
        if (!t) continue;
        renderer_mark_gpu_owned(t);
        if (rt_dropped(i)) continue;
        id<MTLTexture> tex = view_texture(t, &st.rt.cview[i], st.rt.has_cview[i], true, nil);
        rp.colorAttachments[i].texture = tex;
        // 3D target: the view's layer is a depth slice (SMM2 fills its colour-correction LUT one slice per target)
        if (tex.textureType == MTLTextureType3D && st.rt.has_cview[i] && (st.rt.cview[i].setmask & 2)) {
            NSUInteger d = tex.depth ? tex.depth : 1, plane = st.rt.cview[i].min_layer;
            rp.colorAttachments[i].depthPlane = plane < d ? plane : d - 1;
        }
        if (g_debug_frame) LOG("[rt] pass color %d: nvn %p mtl %p (%lux%lu fmt %lu)\n", i, (void *)t, (__bridge void *)tex, (unsigned long)tex.width, (unsigned long)tex.height, (unsigned long)tex.pixelFormat);
        rp.colorAttachments[i].loadAction = MTLLoadActionLoad;
        // reshaped output: areas outside the game's 16:9 drawing (interiors, fades) start black each frame
        extern bool renderer_texture_first_write(NvnTexture *t);
        extern bool renderer_texture_level_fresh(NvnTexture *t);
        extern bool renderer_texture_frame_fresh(NvnTexture *t);
        bool fresh = renderer_texture_level_fresh(t) | renderer_texture_frame_fresh(t);
        if (renderer_texture_first_write(t) || fresh) {
            rp.colorAttachments[i].loadAction = MTLLoadActionClear;
            rp.colorAttachments[i].clearColor = MTLClearColorMake(0, 0, 0, fresh ? 0 : 1);
        }
        rp.colorAttachments[i].storeAction = MTLStoreActionStore;
        if (!w) renderer_texture_scale(t, &pass_sx, &pass_sy);
        w = (int)tex.width; h = (int)tex.height;
    }
    if (st.rt.depth) {
        renderer_mark_gpu_owned(st.rt.depth);
        id<MTLTexture> tex = view_texture(st.rt.depth, &st.rt.dview, st.rt.has_dview, true, nil);
        if (renderer_format_is_depth(st.rt.depth->format) && st.rt.depth->format != 0x31) {
            rp.depthAttachment.texture = tex;
            rp.depthAttachment.loadAction = MTLLoadActionLoad;
            rp.depthAttachment.storeAction = MTLStoreActionStore;
        }
        if (renderer_format_has_stencil(st.rt.depth->format)) {
            rp.stencilAttachment.texture = tex;
            rp.stencilAttachment.loadAction = MTLLoadActionLoad;
            rp.stencilAttachment.storeAction = MTLStoreActionStore;
        }
        if (!w) { renderer_texture_scale(st.rt.depth, &pass_sx, &pass_sy); w = (int)tex.width; h = (int)tex.height; }
    }
    if (!w) {
        pass_sx = pass_sy = 1;
        rp.renderTargetWidth = 16; rp.renderTargetHeight = 16;
        rp.defaultRasterSampleCount = 1;
    }
    // Commit the frame in chunks: the GPU starts on the first passes while the CPU still encodes the rest. The
    // game waits for its previous frame before it submits the next one, so with one command buffer per frame the
    // GPU sat idle during our encoding and a frame cost GPU + encoding time (5120x1440 with the GPU clocked down:
    // ~15.5 + a few ms -> missed vsync -> 30 fps). LARECOMP_CHUNK_PASSES=<n> (0 = one buffer per frame).
    static int chunk = -1;
    if (chunk < 0) { const char *e = ENV("LARECOMP_CHUNK_PASSES"); chunk = e ? atoi(e) : 8; }
    if (chunk > 0 && cmdbuf && ++g_passes_in_cb >= chunk) renderer_submit();
    enc = [cb() renderCommandEncoderWithDescriptor:rp];
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    stat_passes++;
}

// Physical size of the current pass (guest size times the attachments' allocation scale).
static void pass_size(int *w, int *h) {
    *w = *h = 0;
    for (int i = 0; i < st.rt.ncolor && !*w; i++)
        if (st.rt.color[i]) { *w = (int)st.rt.color[i]->width; *h = (int)st.rt.color[i]->height; }
    if (!*w && st.rt.depth) { *w = (int)st.rt.depth->width; *h = (int)st.rt.depth->height; }
    *w = (int)lroundf(*w * pass_sx);
    *h = (int)lroundf(*h * pass_sy);
}

static void apply_scissor(void) {
    int w, h;
    pass_size(&w, &h);
    if (!w) return;
    int x = st.scissor[0][0], y = st.scissor[0][1], sw = st.scissor[0][2], sh = st.scissor[0][3];
    if (vp_sx != 1 || vp_sy != 1 || vp_ox != 0 || vp_oy != 0) {
        double x0 = floor(x * (double)vp_sx + vp_ox), y0 = floor(y * (double)vp_sy + vp_oy);
        double x1 = ceil(((double)x + sw) * vp_sx + vp_ox), y1 = ceil(((double)y + sh) * vp_sy + vp_oy);
        if (sw >= 16384) x1 = 1e9;  // "unbounded" scissors stay unbounded
        if (sh >= 16384) y1 = 1e9;
        if (x1 > 1e9) x1 = 1e9;
        if (y1 > 1e9) y1 = 1e9;
        x = (int)x0; y = (int)y0; sw = (int)(x1 - x0); sh = (int)(y1 - y0);
    }
    if (x < 0) { sw += x; x = 0; }
    if (y < 0) { sh += y; y = 0; }
    if (x >= w || y >= h) { x = y = 0; sw = sh = 0; }
    if (x + sw > w) sw = w - x;
    if (y + sh > h) sh = h - y;
    if (sw < 0) sw = 0;
    if (sh < 0) sh = 0;
    [enc setScissorRect:(MTLScissorRect){(NSUInteger)x, (NSUInteger)y, (NSUInteger)sw, (NSUInteger)sh}];
}

// ------------------------------------------------------------------ clears (full-screen triangle)
static const char *clear_src =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct V { float4 pos [[position]]; };\n"
    "vertex V vs(uint vid [[vertex_id]], constant float4 &d [[buffer(0)]]) {\n"
    "  float2 p = float2((vid << 1) & 2, vid & 2); V o; o.pos = float4(p * 2.0 - 1.0, d.x, 1); return o; }\n"
    "struct OF { float4 c [[color(INDEX)]]; };\n"
    "fragment OF fs_f(V in [[stage_in]], constant float4 &c [[buffer(0)]]) { OF o; o.c = c; return o; }\n"
    "struct OI { int4 c [[color(INDEX)]]; };\n"
    "fragment OI fs_i(V in [[stage_in]], constant int4 &c [[buffer(0)]]) { OI o; o.c = c; return o; }\n"
    "struct OU { uint4 c [[color(INDEX)]]; };\n"
    "fragment OU fs_u(V in [[stage_in]], constant uint4 &c [[buffer(0)]]) { OU o; o.c = c; return o; }\n"
    "fragment void fs_none(V in [[stage_in]]) {}\n";

static id<MTLLibrary> clear_lib(int index) {
    static id<MTLLibrary> libs[8];
    if (!libs[index]) {
        NSString *src = [@(clear_src) stringByReplacingOccurrencesOfString:@"INDEX" withString:[NSString stringWithFormat:@"%d", index]];
        NSError *err;
        libs[index] = TIMED_COMPILE([dev newLibraryWithSource:src options:nil error:&err]);
        if (!libs[index]) FATAL("clear shader: %s\n", err.localizedDescription.UTF8String);
    }
    return libs[index];
}

// The attachments' formats a pipeline is built for (a dropped target: Invalid).
typedef struct { MTLPixelFormat color[8], depth, stencil; uint8_t dropped; NSUInteger samples; } PassLayout;

static void layout_depth_samples(const ExecState *s, PassLayout *out) {
    if (s->rt.depth) {
        MTLPixelFormat pf = renderer_pixel_format(s->rt.depth->format);
        if (renderer_format_is_depth(s->rt.depth->format) && s->rt.depth->format != 0x31) out->depth = pf;
        if (renderer_format_has_stencil(s->rt.depth->format)) out->stencil = pf;
    }
    out->samples = 1;
    for (int i = 0; i < s->rt.ncolor && i < 8; i++)
        if (s->rt.color[i] && s->rt.color[i]->samples > 1) out->samples = s->rt.color[i]->samples;
}
// The current state's, from its targets' textures (made if need be: as begin_pass does)
static void pass_layout_actual(PassLayout *out) {
    memset(out, 0, sizeof *out);
    for (int i = 0; i < st.rt.ncolor && i < 8; i++) {
        if (!st.rt.color[i]) continue;
        if (rt_dropped(i)) { out->dropped |= 1u << i; continue; }
        out->color[i] = view_texture(st.rt.color[i], &st.rt.cview[i], st.rt.has_cview[i], true, nil).pixelFormat;
    }
    layout_depth_samples(&st, out);
}
// The same, predicted for any state: no texture made, no pass begun (the prefetch scan's; a texture not made yet is
// predicted from its NVN format)
static void pass_layout(const ExecState *s, PassLayout *out) {
    memset(out, 0, sizeof *out);
    for (int i = 0; i < s->rt.ncolor && i < 8; i++) {
        NvnTexture *t = s->rt.color[i];
        if (!t) continue;
        if (rt_dropped_in(s, i)) { out->dropped |= 1u << i; continue; }
        MTLPixelFormat base = renderer_texture_pixel_format(t);
        out->color[i] = s->rt.has_cview[i] && s->rt.cview[i].setmask ? view_pixel_format(base, &s->rt.cview[i], false) : base;
    }
    layout_depth_samples(s, out);
    static int wrong = -1;  // (test) LARECOMP_DEBUG_MISPREDICT=1: a wrong prediction, which mustn't change what's drawn
    if (wrong < 0) wrong = getenv("LARECOMP_DEBUG_MISPREDICT") != NULL;
    if (wrong && out->color[0]) out->color[0] = MTLPixelFormatRGBA32Float;
}
static void layout_apply(MTLRenderPipelineDescriptor *pd, const PassLayout *pl) {
    for (int i = 0; i < 8; i++) if (pl->color[i]) pd.colorAttachments[i].pixelFormat = pl->color[i];
    if (pl->depth) pd.depthAttachmentPixelFormat = pl->depth;
    if (pl->stencil) pd.stencilAttachmentPixelFormat = pl->stencil;
    if (pl->samples > 1) pd.rasterSampleCount = pl->samples;
}
static void attachment_formats(MTLRenderPipelineDescriptor *pd) {
    PassLayout pl;
    pass_layout_actual(&pl);
    layout_apply(pd, &pl);
}

static void clear(int index, int kind, const void *color, uint32_t mask, bool depth, float dval, bool stencil,
                  uint32_t sval, uint32_t smask) {
    begin_pass();
    if (g_debug_frame) {
        int pw, ph;
        pass_size(&pw, &ph);
        const float *cf = color;
        LOG("[clear] pass %d idx %d depth %d (%g) stencil %d | pass %dx%d scissor %d,%d %dx%d | kind %d mask %#x colour %g %g %g %g\n",
            debug_pass_no, index, depth, dval, stencil, pw, ph, st.scissor[0][0], st.scissor[0][1], st.scissor[0][2],
            st.scissor[0][3], kind, mask, cf ? cf[0] : 0, cf ? cf[1] : 0, cf ? cf[2] : 0, cf ? cf[3] : 0);
    }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    id<MTLLibrary> lib = clear_lib(index < 0 ? 0 : index);
    pd.vertexFunction = [lib newFunctionWithName:@"vs"];
    static NSString *const fs[] = {@"fs_f", @"fs_i", @"fs_u"};
    pd.fragmentFunction = [lib newFunctionWithName:index >= 0 ? fs[kind] : @"fs_none"];
    attachment_formats(pd);
    for (int i = 0; i < 8; i++) pd.colorAttachments[i].writeMask = MTLColorWriteMaskNone;
    if (index >= 0) {
        MTLColorWriteMask m = 0;
        if (mask & 1) m |= MTLColorWriteMaskRed;
        if (mask & 2) m |= MTLColorWriteMaskGreen;
        if (mask & 4) m |= MTLColorWriteMaskBlue;
        if (mask & 8) m |= MTLColorWriteMaskAlpha;
        pd.colorAttachments[index].writeMask = m;
    }
    NSError *err;
    NSString *key = [NSString stringWithFormat:@"clear %d %d %u %d %d %lu %lu %lu", index, kind, mask, depth, stencil,
                     (unsigned long)pd.colorAttachments[index < 0 ? 0 : index].pixelFormat,
                     (unsigned long)pd.depthAttachmentPixelFormat, (unsigned long)pd.stencilAttachmentPixelFormat];
    id<MTLRenderPipelineState> pipe = cache[key];
    if (!pipe) {
        pipe = TIMED_COMPILE([dev newRenderPipelineStateWithDescriptor:pd error:&err]);
        if (!pipe) { LOG("[metal] clear pipeline: %s\n", err.localizedDescription.UTF8String); return; }
        cache[key] = pipe;
    }
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = MTLCompareFunctionAlways;
    dd.depthWriteEnabled = depth;
    if (stencil) {
        MTLStencilDescriptor *sd = [MTLStencilDescriptor new];
        sd.stencilCompareFunction = MTLCompareFunctionAlways;
        sd.depthStencilPassOperation = MTLStencilOperationReplace;
        sd.writeMask = smask;
        dd.frontFaceStencil = sd;
        dd.backFaceStencil = sd;
    }
    [enc setRenderPipelineState:pipe];
    [enc setDepthStencilState:[dev newDepthStencilStateWithDescriptor:dd]];
    [enc setStencilReferenceValue:sval];
    [enc setCullMode:MTLCullModeNone];
    int w, h;
    pass_size(&w, &h);
    [enc setViewport:(MTLViewport){0, 0, w, h, 0, 1}];
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    apply_scissor();
    float d4[4] = {dval, 0, 0, 0};
    [enc setVertexBytes:d4 length:16 atIndex:0];
    if (index >= 0) [enc setFragmentBytes:color length:16 atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
}

// ------------------------------------------------------------------ shaders (debug dump)
static void dump_program(NvnProgram *p) {
    if (!getenv("LARECOMP_DUMP_SHADERS")) return;
    static int n;
    mkdir("build/shaders", 0755);
    for (int s = 0; s < 6; s++) {
        if (!(p->stage_mask & (1u << s))) continue;
        char path[256];
        snprintf(path, sizeof path, "build/shaders/%llx_s%d_code.bin", p->stages[s].code_addr, s);
        FILE *f = fopen(path, "wb");
        fwrite((void *)(uintptr_t)p->stages[s].code_addr, 1, 0x10000, f);
        fclose(f);
        snprintf(path, sizeof path, "build/shaders/%llx_s%d_ctrl.bin", p->stages[s].code_addr, s);
        f = fopen(path, "wb");
        fwrite(p->stages[s].control, 1, 0x4000, f);
        fclose(f);
    }
    n++;
}

// ------------------------------------------------------------------ draw support
uint32_t nvn_window_origin(void);
uint32_t nvn_depth_mode(void);
static int cbuf_base = 3, tex_base = 8;

@interface Prog : NSObject {
@public
    id<MTLFunction> vs, fs;
    ShaderInfo *vi, *fi;
    ShaderInfo *gi;  // geometry stage (vi->msl is then the combined vertex function: maxwell/shader.h)
    _Atomic int state;         // BUILD_PENDING / READY (vs, fs set) / FAILED (buildsync.h)
    NSMutableArray *waiting;   // pipelines the prefetch scan asked for before the program was ready (@synchronized)
    uint64_t vs_code;
}
@end
@implementation Prog
@end

// A pipeline the prefetch scan wants once its program is compiled (in Prog.waiting)
@interface PipeReq : NSObject {
@public
    MTLRenderPipelineDescriptor *pd;
    NSString *key;
    uint64_t khash;  // (pipe_key_hash of key)
}
@end
@implementation PipeReq
@end
@class MPipe;
static MPipe *pipe_request(Prog *pr, MTLRenderPipelineDescriptor *pd, NSString *key, uint64_t khash, bool *created);
static uint64_t g_prog_creates;  // programs made (game thread): the scan's and the draws' share of them

static id<MTLBuffer> ring;
static uint64_t ring_off;
#define RING_SIZE (64u << 20)

static uint64_t ring_alloc(const void *data, uint64_t size) {
    if (!ring) ring = [dev newBufferWithLength:RING_SIZE options:MTLResourceStorageModeShared];
    uint64_t aligned = (ring_off + 255) & ~255ull;
    if (aligned + size > RING_SIZE) {
        // Start a fresh ring; command buffers in flight keep the old one alive until they complete.
        ring = [dev newBufferWithLength:(size > RING_SIZE ? size : RING_SIZE) options:MTLResourceStorageModeShared];
        aligned = 0;
    }
    memcpy((uint8_t *)ring.contents + aligned, data, size);
    ring_off = aligned + size;
    return aligned;
}

static void ring_reset(void) { ring_off = 0; }

// Bind guest memory [addr, addr+size) as a buffer: zero-copy inside the heap, copied otherwise.
static void bind_guest(id<MTLRenderCommandEncoder> e, bool vertex_stage, uint64_t addr, uint64_t size, int index) {
    uint64_t goff;
    id<MTLBuffer> gbuf = (addr & 3) == 0 ? guest_buffer_chunk(addr, size, &goff) : nil;
    if (gbuf) {
        gpu_read_mark(addr, size);
        if (vertex_stage) [e setVertexBuffer:gbuf offset:goff atIndex:index];
        else [e setFragmentBuffer:gbuf offset:goff atIndex:index];
        return;
    }
    uint64_t off = ring_alloc((void *)(uintptr_t)addr, size);
    if (vertex_stage) [e setVertexBuffer:ring offset:off atIndex:index];
    else [e setFragmentBuffer:ring offset:off atIndex:index];
}

static int attr_kind(uint32_t fmt) {
    switch (fmt) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x1B: case 0x20: case 0x23:
    case 0x27: case 0x2C: case 0x2F: case 0x3E:
        return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x1C: case 0x21: case 0x24:
    case 0x28: case 0x2D: case 0x30:
        return 1;
    default: return 0;
    }
}

static MTLVertexFormat vertex_format(uint32_t f) {
    switch (f) {
    case 0x01: return MTLVertexFormatUCharNormalized;
    case 0x02: return MTLVertexFormatCharNormalized;
    case 0x03: return MTLVertexFormatUChar;
    case 0x04: return MTLVertexFormatChar;
    case 0x05: return MTLVertexFormatHalf;
    case 0x06: return MTLVertexFormatUShortNormalized;
    case 0x07: return MTLVertexFormatShortNormalized;
    case 0x08: return MTLVertexFormatUShort;
    case 0x09: return MTLVertexFormatShort;
    case 0x0A: return MTLVertexFormatFloat;
    case 0x0B: return MTLVertexFormatUInt;
    case 0x0C: return MTLVertexFormatInt;
    case 0x0D: return MTLVertexFormatUChar2Normalized;
    case 0x0E: return MTLVertexFormatChar2Normalized;
    case 0x0F: return MTLVertexFormatUChar2;
    case 0x10: return MTLVertexFormatChar2;
    case 0x11: return MTLVertexFormatHalf2;
    case 0x12: return MTLVertexFormatUShort2Normalized;
    case 0x13: return MTLVertexFormatShort2Normalized;
    case 0x14: return MTLVertexFormatUShort2;
    case 0x15: return MTLVertexFormatShort2;
    case 0x16: return MTLVertexFormatFloat2;
    case 0x17: return MTLVertexFormatUInt2;
    case 0x18: return MTLVertexFormatInt2;
    case 0x19: return MTLVertexFormatUChar3Normalized;
    case 0x1A: return MTLVertexFormatChar3Normalized;
    case 0x1B: return MTLVertexFormatUChar3;
    case 0x1C: return MTLVertexFormatChar3;
    case 0x1D: return MTLVertexFormatHalf3;
    case 0x1E: return MTLVertexFormatUShort3Normalized;
    case 0x1F: return MTLVertexFormatShort3Normalized;
    case 0x20: return MTLVertexFormatUShort3;
    case 0x21: return MTLVertexFormatShort3;
    case 0x22: return MTLVertexFormatFloat3;
    case 0x23: return MTLVertexFormatUInt3;
    case 0x24: return MTLVertexFormatInt3;
    case 0x25: return MTLVertexFormatUChar4Normalized;
    case 0x26: return MTLVertexFormatChar4Normalized;
    case 0x27: return MTLVertexFormatUChar4;
    case 0x28: return MTLVertexFormatChar4;
    case 0x29: return MTLVertexFormatHalf4;
    case 0x2A: return MTLVertexFormatUShort4Normalized;
    case 0x2B: return MTLVertexFormatShort4Normalized;
    case 0x2C: return MTLVertexFormatUShort4;
    case 0x2D: return MTLVertexFormatShort4;
    case 0x2E: return MTLVertexFormatFloat4;
    case 0x2F: return MTLVertexFormatUInt4;
    case 0x30: return MTLVertexFormatInt4;
    case 0x3D: return MTLVertexFormatUInt1010102Normalized;
    case 0x69: return MTLVertexFormatInt1010102Normalized;
    case 0x3F: return MTLVertexFormatFloatRG11B10;
    case 0x40: return MTLVertexFormatFloatRGB9E5;
    // integer-to-float ("I2F") formats: converted to float by the vertex fetch
    case 0x51: return MTLVertexFormatUChar;   // these read as raw integers converted in shader
    default:
        return MTLVertexFormatFloat4;
    }
}

static bool is_int_format(uint32_t f) { return attr_kind(f) != 0; }

static MTLBlendFactor blend_factor(uint8_t f) {
    switch (f) {
    case 1: return MTLBlendFactorZero;
    case 2: return MTLBlendFactorOne;
    case 3: return MTLBlendFactorSourceColor;
    case 4: return MTLBlendFactorOneMinusSourceColor;
    case 5: return MTLBlendFactorSourceAlpha;
    case 6: return MTLBlendFactorOneMinusSourceAlpha;
    case 7: return MTLBlendFactorDestinationAlpha;
    case 8: return MTLBlendFactorOneMinusDestinationAlpha;
    case 9: return MTLBlendFactorDestinationColor;
    case 10: return MTLBlendFactorOneMinusDestinationColor;
    case 11: return MTLBlendFactorSourceAlphaSaturated;
    case 0x10: return MTLBlendFactorSource1Color;
    case 0x11: return MTLBlendFactorOneMinusSource1Color;
    case 0x12: return MTLBlendFactorSource1Alpha;
    case 0x13: return MTLBlendFactorOneMinusSource1Alpha;
    case 0x61: return MTLBlendFactorBlendColor;
    case 0x62: return MTLBlendFactorOneMinusBlendColor;
    case 0x63: return MTLBlendFactorBlendAlpha;
    case 0x64: return MTLBlendFactorOneMinusBlendAlpha;
    default: {
        static uint8_t warned[256];
        if (!warned[f]++) LOG("[metal] unknown blend factor %#x\n", f);
        return MTLBlendFactorOne;
    }
    }
}

static MTLBlendOperation blend_op(uint8_t e) {
    switch (e) {
    case 2: return MTLBlendOperationSubtract;
    case 3: return MTLBlendOperationReverseSubtract;
    case 4: return MTLBlendOperationMin;
    case 5: return MTLBlendOperationMax;
    default: return MTLBlendOperationAdd;
    }
}

static MTLCompareFunction compare_fn(uint8_t f) {
    switch (f) {
    case 1: return MTLCompareFunctionNever;
    case 2: return MTLCompareFunctionLess;
    case 3: return MTLCompareFunctionEqual;
    case 4: return MTLCompareFunctionLessEqual;
    case 5: return MTLCompareFunctionGreater;
    case 6: return MTLCompareFunctionNotEqual;
    case 7: return MTLCompareFunctionGreaterEqual;
    default: return MTLCompareFunctionAlways;
    }
}

static MTLStencilOperation stencil_op(uint8_t o) {
    switch (o) {
    case 2: return MTLStencilOperationZero;
    case 3: return MTLStencilOperationReplace;
    case 4: return MTLStencilOperationIncrementClamp;
    case 5: return MTLStencilOperationDecrementClamp;
    case 6: return MTLStencilOperationInvert;
    case 7: return MTLStencilOperationIncrementWrap;
    case 8: return MTLStencilOperationDecrementWrap;
    default: return MTLStencilOperationKeep;
    }
}

static int rt_kind(uint32_t f) {
    switch (f) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x27: case 0x2C: case 0x2F: case 0x3E: return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x28: case 0x2D: case 0x30: return 1;
    default: return 0;
    }
}

#define PROG_TAB 4096
static struct { uint64_t key; void *prog; } prog_tab[PROG_TAB];

// Shader caches, in the data folder's cache/ (delete it to reset; docs/smm2-bringup.md "First-use shader compiles"):
// - cache/shaders/: every translated shader's MSL (v_/f_/c_<hash>.metal). At startup the ones not precompiled are
//   compiled once on a background queue, which fills Metal's own on-disk shader cache so that a later first use
//   in-game is cheap - while Metal keeps them: that cache is shared and size-limited, and evicts.
// - cache/metallib/ (and metallib/ in an iOS app): those shaders precompiled (tools/precompile_shaders.sh), loaded
//   instead of compiling the source.
// - cache/pipelines/: the pipelines built, with their GPU code (Metal binary archives).
static uint64_t fnv64(const char *t) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (; *t; t++) { h ^= (uint8_t)*t; h *= 0x100000001B3ull; }
    return h;
}

static void shader_cache_note1(char kind, const char *src) {  // kind: v(ertex), f(ragment), c(ompute)
    static int made;
    if (!made) { made = 1; mkdir("cache", 0755); mkdir("cache/shaders", 0755); }
    char path[96];
    snprintf(path, sizeof path, "cache/shaders/%c_%016llx.metal", kind, (unsigned long long)fnv64(src));
    if (access(path, F_OK) == 0) return;
    FILE *f = fopen(path, "w");
    if (f) { fputs(src, f); fclose(f); }
}
static void shader_cache_note(const char *vs, const char *fs) { shader_cache_note1('v', vs); shader_cache_note1('f', fs); }

// Each shader's entry point is named after its kind and source hash (v_/f_/c_<hash>) rather than the translator's
// main0: a pipeline archive packs its pipelines' functions by name, and with every one called main0 its saves failed
// ("expecting 'fragment' stage" / "missing 'vertex' stage in pipeline no. N").
static NSString *entry_name(char kind, const char *msl) {
    return [NSString stringWithFormat:@"%c_%016llx", kind, (unsigned long long)fnv64(msl)];
}
static NSString *entry_source(char kind, const char *msl, NSString *salt) {
    NSString *src = [@(msl) stringByReplacingOccurrencesOfString:@" main0("
                                                      withString:[NSString stringWithFormat:@" %@(", entry_name(kind, msl)]];
    return salt.length ? [src stringByAppendingString:salt] : src;
}

// Shaders compiled ahead of time from cache/shaders/ (tools/precompile_shaders.sh: Metal's offline compiler, which
// runs on a Mac only) are loaded instead of compiling the source: no front-end compile at all, which is most of a
// first use's cost. Looked for in cache/metallib/ (made for this machine) and metallib/ (an iOS app carries them in
// its bundle and links them there). Keyed by the source's hash, so a changed translator just misses.
// LARECOMP_NO_PRECOMPILED=1: off.
static id<MTLLibrary> precompiled_library(char kind, const char *msl) {
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_NO_PRECOMPILED") != NULL;
    if (off) return nil;
    static const char *const dirs[] = {"cache/metallib", "metallib"};
    unsigned long long h = (unsigned long long)fnv64(msl);
    for (int i = 0; i < 2; i++) {
        char path[96];
        snprintf(path, sizeof path, "%s/%c_%016llx.metallib", dirs[i], kind, h);
        if (access(path, R_OK) != 0) continue;
        id<MTLLibrary> l = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(path)] error:nil];
        if (l) { __atomic_fetch_add(&stat_precompiled, 1, __ATOMIC_RELAXED); return l; }
    }
    return nil;
}

// Pipeline cache: the render / compute pipelines built are saved with their GPU code in Metal binary archives
// (cache/pipelines/NNNN.bin), and looked up there first: a pipeline found there costs no back-end compile, and the
// files outlive Metal's own shader cache (shared, size-limited, and an app update can clear it). New pipelines are
// saved from a background queue a few seconds after the last one appeared (a game can be killed rather than quit,
// e.g. on iOS) and at exit, each time into a new file from a new archive: adding to an archive that had been saved
// or loaded before and saving it again failed on macOS 27 ("Failed to generate machO: ... expecting 'fragment' stage
// in pipeline no. N", and the archive came out empty), while the same pipelines saved from a fresh archive at once.
// Each file has its pipelines' descriptions next to it (NNNN.txt: shader names and fixed-function state, no game
// data beyond what cache/shaders already holds). Archives can't be read back or merged as such, so once there are
// more than PIPELINE_FILES_MERGE files, the next start-up rebuilds every described pipeline into one new archive on
// the background queue (the files stay in use meanwhile) and the files it replaces are deleted at the start-up
// after that: loading stays a few files.
// LARECOMP_NO_PIPELINE_CACHE=1: off. Delete cache/ to reset.
#define PIPELINE_FILES_MAX 256
#define PIPELINE_FILES_MERGE 8
static NSArray *g_archives;                 // the saved files (read only)
static dispatch_queue_t g_archive_q;
static NSMutableArray *g_archive_pending;   // descriptors not saved yet (g_archive_q only)
static int g_archive_next, g_archive_hits, g_archive_fails;
static double g_archive_saved, g_archive_added, g_archive_first;  // (last save, last addition, oldest unsaved)
static NSArray *pipeline_archives(void) { return g_archives.count ? g_archives : nil; }
static void pipeline_cache_save_now(void);

// A pipeline's description: its shaders' entry-point names (v_/f_/c_<hash>: the source in cache/shaders/, precompiled
// in cache/metallib/) and the state the draw path sets.
static NSString *pipeline_spec(id desc) {
    if ([desc isKindOfClass:[MTLComputePipelineDescriptor class]])
        return [NSString stringWithFormat:@"C %@", ((MTLComputePipelineDescriptor *)desc).computeFunction.name];
    MTLRenderPipelineDescriptor *pd = desc;
    NSMutableString *t = [NSMutableString stringWithFormat:@"R %@ %@ %lu %lu %lu %d", pd.vertexFunction.name, pd.fragmentFunction.name,
                          (unsigned long)pd.depthAttachmentPixelFormat, (unsigned long)pd.stencilAttachmentPixelFormat,
                          (unsigned long)pd.rasterSampleCount, pd.alphaToCoverageEnabled];
    for (int i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *ca = pd.colorAttachments[i];
        [t appendFormat:@" %lu/%lu/%d/%lu/%lu/%lu/%lu/%lu/%lu", (unsigned long)ca.pixelFormat, (unsigned long)ca.writeMask,
                        ca.blendingEnabled, (unsigned long)ca.sourceRGBBlendFactor, (unsigned long)ca.destinationRGBBlendFactor,
                        (unsigned long)ca.sourceAlphaBlendFactor, (unsigned long)ca.destinationAlphaBlendFactor,
                        (unsigned long)ca.rgbBlendOperation, (unsigned long)ca.alphaBlendOperation];
    }
    MTLVertexDescriptor *vd = pd.vertexDescriptor;
    for (int i = 0; i < 31; i++)
        if (vd.attributes[i].format != MTLVertexFormatInvalid)
            [t appendFormat:@" a%d/%lu/%lu/%lu", i, (unsigned long)vd.attributes[i].format, (unsigned long)vd.attributes[i].offset,
                            (unsigned long)vd.attributes[i].bufferIndex];
    for (int i = 0; i < 31; i++)
        if (vd.layouts[i].stride || vd.layouts[i].stepFunction != MTLVertexStepFunctionPerVertex)
            [t appendFormat:@" l%d/%lu/%lu/%lu", i, (unsigned long)vd.layouts[i].stride, (unsigned long)vd.layouts[i].stepFunction,
                            (unsigned long)vd.layouts[i].stepRate];
    return t;
}
static id<MTLFunction> cached_function(NSString *name, NSMutableDictionary *seen) {  // (merging: as the draw path would)
    id<MTLFunction> f = seen[name];
    if (f || name.length != 18) return f;
    for (NSString *dir in @[@"cache/metallib", @"metallib"]) {
        if (getenv("LARECOMP_NO_PRECOMPILED")) break;
        NSString *path = [NSString stringWithFormat:@"%@/%@.metallib", dir, name];
        id<MTLLibrary> l = access(path.fileSystemRepresentation, R_OK) == 0 ? [dev newLibraryWithURL:[NSURL fileURLWithPath:path] error:nil] : nil;
        if ((f = [l newFunctionWithName:name])) break;
    }
    if (!f) {
        NSString *src = [NSString stringWithContentsOfFile:[NSString stringWithFormat:@"cache/shaders/%@.metal", name]
                                                  encoding:NSUTF8StringEncoding error:nil];
        MTLCompileOptions *o = [MTLCompileOptions new];
        if ([name hasPrefix:@"v"]) o.preserveInvariance = YES;
        src = [src stringByReplacingOccurrencesOfString:@" main0(" withString:[NSString stringWithFormat:@" %@(", name]];
        f = src ? [[dev newLibraryWithSource:src options:o error:nil] newFunctionWithName:name] : nil;
    }
    if (f) seen[name] = f;
    return f;
}
static id pipeline_from_spec(NSString *line, NSMutableDictionary *seen) {
    NSArray *w = [line componentsSeparatedByString:@" "];
    if (w.count == 2 && [w[0] isEqual:@"C"]) {
        id<MTLFunction> f = cached_function(w[1], seen);
        if (!f) return nil;
        MTLComputePipelineDescriptor *cd = [MTLComputePipelineDescriptor new];
        cd.computeFunction = f;
        return cd;
    }
    if (w.count < 15 || ![w[0] isEqual:@"R"]) return nil;
    id<MTLFunction> vf = cached_function(w[1], seen), ff = cached_function(w[2], seen);
    if (!vf || !ff) return nil;
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = vf;
    pd.fragmentFunction = ff;
    pd.depthAttachmentPixelFormat = (MTLPixelFormat)[w[3] integerValue];
    pd.stencilAttachmentPixelFormat = (MTLPixelFormat)[w[4] integerValue];
    pd.rasterSampleCount = (NSUInteger)[w[5] integerValue];
    pd.alphaToCoverageEnabled = [w[6] intValue] != 0;
    for (int i = 0; i < 8; i++) {
        unsigned long v[9];
        if (sscanf([w[7 + i] UTF8String], "%lu/%lu/%lu/%lu/%lu/%lu/%lu/%lu/%lu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8]) != 9)
            return nil;
        MTLRenderPipelineColorAttachmentDescriptor *ca = pd.colorAttachments[i];
        ca.pixelFormat = v[0]; ca.writeMask = v[1]; ca.blendingEnabled = v[2] != 0;
        ca.sourceRGBBlendFactor = v[3]; ca.destinationRGBBlendFactor = v[4];
        ca.sourceAlphaBlendFactor = v[5]; ca.destinationAlphaBlendFactor = v[6];
        ca.rgbBlendOperation = v[7]; ca.alphaBlendOperation = v[8];
    }
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    for (NSUInteger k = 15; k < w.count; k++) {
        const char *x = [w[k] UTF8String];
        unsigned long i, a, b, c;
        if (sscanf(x + 1, "%lu/%lu/%lu/%lu", &i, &a, &b, &c) != 4 || i > 30) return nil;
        if (x[0] == 'a') { vd.attributes[i].format = a; vd.attributes[i].offset = b; vd.attributes[i].bufferIndex = c; }
        else if (x[0] == 'l') { vd.layouts[i].stride = a; vd.layouts[i].stepFunction = b; vd.layouts[i].stepRate = c; }
        else return nil;
    }
    pd.vertexDescriptor = vd;
    return pd;
}
// (start-up: a merge finished last time - its marker names the files it replaced)
static void pipeline_cache_merged(void) {
    int upto, into;
    FILE *f = fopen("cache/pipelines/merged", "r");
    if (!f) return;
    bool ok = fscanf(f, "%d %d", &upto, &into) == 2;
    fclose(f);
    char path[64];
    snprintf(path, sizeof path, "cache/pipelines/%04d.bin", into);
    if (ok && access(path, R_OK) == 0)
        for (NSString *n in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:@"cache/pipelines" error:nil])
            if (([n hasSuffix:@".bin"] || [n hasSuffix:@".txt"]) && n.intValue <= upto)
                unlink([@"cache/pipelines" stringByAppendingPathComponent:n].fileSystemRepresentation);
    unlink("cache/pipelines/merged");
}
static void pipeline_cache_merge(int upto, int into) {  // (on g_archive_q)
    double t0 = now_ms();
    NSMutableOrderedSet *specs = [NSMutableOrderedSet new];
    for (NSString *n in [[[NSFileManager defaultManager] contentsOfDirectoryAtPath:@"cache/pipelines" error:nil]
             sortedArrayUsingSelector:@selector(compare:)]) {
        if (![n hasSuffix:@".txt"] || n.intValue > upto) continue;
        NSString *t = [NSString stringWithContentsOfFile:[@"cache/pipelines" stringByAppendingPathComponent:n]
                                                encoding:NSUTF8StringEncoding error:nil];
        for (NSString *l in [t componentsSeparatedByString:@"\n"]) if (l.length) [specs addObject:l];
    }
    int n = 0, lost = 0;
    NSMutableString *kept = [NSMutableString new];
    @autoreleasepool {
        NSMutableDictionary *seen = [NSMutableDictionary new];
        id<MTLBinaryArchive> a = [dev newBinaryArchiveWithDescriptor:[MTLBinaryArchiveDescriptor new] error:nil];
        for (NSString *l in specs) {
            id d = pipeline_from_spec(l, seen);
            bool ok = d && ([d isKindOfClass:[MTLComputePipelineDescriptor class]] ? [a addComputePipelineFunctionsWithDescriptor:d error:nil]
                                                                                  : [a addRenderPipelineFunctionsWithDescriptor:d error:nil]);
            if (ok) { n++; [kept appendFormat:@"%@\n", l]; } else lost++;
        }
        char path[64], tmp[72];
        snprintf(path, sizeof path, "cache/pipelines/%04d.bin", into);
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        NSError *e = nil;
        if (!n || ![a serializeToURL:[NSURL fileURLWithPath:@(tmp)] error:&e]) {
            LOG("[shader] pipeline cache not merged: %s\n", n ? e.localizedDescription.UTF8String : "nothing to rebuild");
            unlink(tmp);
            return;
        }
        snprintf(tmp, sizeof tmp, "cache/pipelines/%04d.txt", into);
        [kept writeToFile:@(tmp) atomically:YES encoding:NSUTF8StringEncoding error:nil];
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        rename(tmp, path);
        FILE *f = fopen("cache/pipelines/merged", "w");
        if (f) { fprintf(f, "%d %d\n", upto, into); fclose(f); }
    }
    LOG("[shader] pipeline cache merged: %d pipelines into one file (%d not rebuilt; %.0f ms); the old files go at the next start\n",
        n, lost, now_ms() - t0);
}

// Metal binary archives are serialized for a target GPU architecture that the legacy driver of Intel Macs' NVIDIA GPUs
// doesn't have: saving one there aborts ("MTLLoader sliceIDForDevice: failed assertion Target device architecture is
// nil"), so those machines keep no pipeline cache (the shaders are still compiled once into cache/shaders).
static bool pipeline_archives_usable(void) {
    static int ok = -1;
    if (ok < 0) {
        ok = [dev supportsFamily:MTLGPUFamilyMac2] ? 1 : 0;
        if (!ok) LOG("[shader] this driver has no Metal binary archives: pipelines are compiled on each run\n");
    }
    return ok == 1;
}

static void pipeline_cache_open(void) {
    static int done;
    if (done++ || getenv("LARECOMP_NO_PIPELINE_CACHE") || !pipeline_archives_usable()) return;
    mkdir("cache", 0755);
    mkdir("cache/pipelines", 0755);
    double t0 = now_ms();
    pipeline_cache_merged();
    NSMutableArray *got = [NSMutableArray new];
    NSArray *names = [[[NSFileManager defaultManager] contentsOfDirectoryAtPath:@"cache/pipelines" error:nil]
        sortedArrayUsingSelector:@selector(compare:)];
    for (NSString *n in names) {
        if (![n hasSuffix:@".bin"]) continue;
        g_archive_next = MAX(g_archive_next, n.intValue + 1);
        if (![names containsObject:[[n stringByDeletingPathExtension] stringByAppendingString:@".txt"]]) {
            // (from before the descriptions: it could never be merged)
            unlink([@"cache/pipelines" stringByAppendingPathComponent:n].fileSystemRepresentation);
            continue;
        }
        if (got.count >= PIPELINE_FILES_MAX) continue;
        MTLBinaryArchiveDescriptor *d = [MTLBinaryArchiveDescriptor new];
        d.url = [NSURL fileURLWithPath:[@"cache/pipelines" stringByAppendingPathComponent:n]];
        NSError *e = nil;
        id<MTLBinaryArchive> a = [dev newBinaryArchiveWithDescriptor:d error:&e];
        if (a) [got addObject:a];
        else LOG("[shader] pipeline cache file %s unreadable (%s): removed\n", n.UTF8String, e.localizedDescription.UTF8String),
             unlink(d.url.fileSystemRepresentation);
    }
    g_archives = got;
    if (got.count) LOG("[shader] pipeline cache: %lu files loaded (%.0f ms)\n", (unsigned long)got.count, now_ms() - t0);
    g_archive_pending = [NSMutableArray new];
    g_archive_q = dispatch_queue_create("recomp.pipeline-cache", DISPATCH_QUEUE_SERIAL);
    atexit(pipeline_cache_save_now);
    int merge_at = getenv("LARECOMP_PIPELINE_MERGE") ? atoi(getenv("LARECOMP_PIPELINE_MERGE")) : PIPELINE_FILES_MERGE;
    if ((int)got.count > merge_at) {  // (LARECOMP_PIPELINE_MERGE=<n>: tests)
        int upto = g_archive_next - 1, into = g_archive_next++;
        dispatch_async(g_archive_q, ^{ pipeline_cache_merge(upto, into); });
    }
}
// (a pipeline from the cache, or nil when it isn't there)
static id<MTLRenderPipelineState> pipeline_cache_find_render(MTLRenderPipelineDescriptor *pd) {
    if (!g_archives.count) return nil;
    pd.binaryArchives = g_archives;
    double t0 = now_ms();
    id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                                                   reflection:nil error:nil];
    pd.binaryArchives = nil;
    @synchronized(cache) {  // (also called from pipeline builds off the game thread)
        stat_cached_ms += now_ms() - t0;
        if (ps) stat_cached++;
    }
    if (ps) __atomic_fetch_add(&g_archive_hits, 1, __ATOMIC_RELAXED);
    return ps;
}
static void pipeline_cache_add(id desc) {
    if (!g_archive_q || g_archive_fails >= 4) return;  // (off, or saving keeps failing: given up for this run)
    dispatch_async(g_archive_q, ^{
        if (!g_archive_pending.count) g_archive_first = now_ms();
        [g_archive_pending addObject:desc];
        g_archive_added = now_ms();
    });
}
static void pipeline_cache_add_render(MTLRenderPipelineDescriptor *pd) {
    if (!pd.fragmentFunction) return;
    MTLRenderPipelineDescriptor *c = [pd copy];
    c.binaryArchives = nil;
    pipeline_cache_add(c);
}
static void pipeline_cache_add_compute(MTLComputePipelineDescriptor *cd) {
    MTLComputePipelineDescriptor *c = [cd copy];
    c.binaryArchives = nil;
    pipeline_cache_add(c);
}
static void pipeline_cache_save_q(void) {  // (on g_archive_q)
    if (!g_archive_pending.count) return;
    double t0 = now_ms();
    g_archive_saved = t0;
    NSArray *todo = [g_archive_pending copy];
    [g_archive_pending removeAllObjects];
    @autoreleasepool {
        id<MTLBinaryArchive> a = [dev newBinaryArchiveWithDescriptor:[MTLBinaryArchiveDescriptor new] error:nil];
        int n = 0;
        NSMutableString *specs = [NSMutableString new];
        for (id d in todo) {  // (compiles each pipeline once more, here off the game thread)
            bool ok = [d isKindOfClass:[MTLComputePipelineDescriptor class]] ? [a addComputePipelineFunctionsWithDescriptor:d error:nil]
                                                                             : [a addRenderPipelineFunctionsWithDescriptor:d error:nil];
            if (ok) { n++; [specs appendFormat:@"%@\n", pipeline_spec(d)]; }
        }
        if (!n) return;
        char path[64], tmp[72];
        snprintf(path, sizeof path, "cache/pipelines/%04d.bin", g_archive_next);
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        NSError *e = nil;
        if (![a serializeToURL:[NSURL fileURLWithPath:@(tmp)] error:&e]) {
            if (g_archive_fails++ < 4) LOG("[shader] pipeline cache not saved: %s\n", e.localizedDescription.UTF8String);
            unlink(tmp);
            return;
        }
        snprintf(tmp, sizeof tmp, "cache/pipelines/%04d.txt", g_archive_next);
        [specs writeToFile:@(tmp) atomically:YES encoding:NSUTF8StringEncoding error:nil];
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        rename(tmp, path);
        g_archive_next++;
        LOG("[shader] pipeline cache saved: %d new pipelines (%.0f ms)\n", n, now_ms() - t0);
    }
}
static void pipeline_cache_save_now(void) {  // (at exit: waits for the additions queued so far)
    if (g_archive_q) dispatch_sync(g_archive_q, ^{ pipeline_cache_save_q(); });
    LOG("[shader] pipeline cache: %d pipelines found in it this run\n", g_archive_hits);
}
// (every couple of seconds from the stats line: a save 3 s after the last addition, or once the oldest unsaved one
// is 20 s old while new ones keep coming; at most every 10 s)
static void pipeline_cache_tick(void) {
    if (!g_archive_q) return;
    dispatch_async(g_archive_q, ^{
        double t = now_ms();
        if (g_archive_pending.count && t - g_archive_saved > 10000 && (t - g_archive_added > 3000 || t - g_archive_first > 20000))
            pipeline_cache_save_q();
    });
}

void shader_cache_warm(void) {
    static int started;
    if (started++ || getenv("LARECOMP_NO_SHADER_WARM")) return;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        DIR *d = opendir("cache/shaders");
        if (!d) return;
        MTLCompileOptions *vopts = [MTLCompileOptions new];
        vopts.preserveInvariance = YES;
        double t0 = now_ms();
        int n = 0;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] != 'v' && e->d_name[0] != 'f' && e->d_name[0] != 'c') continue;
            char path[300];
            NSString *base = [@(e->d_name) stringByDeletingPathExtension];
            snprintf(path, sizeof path, "cache/metallib/%s.metallib", base.UTF8String);
            bool pre = !getenv("LARECOMP_NO_PRECOMPILED") && access(path, R_OK) == 0;
            snprintf(path, sizeof path, "metallib/%s.metallib", base.UTF8String);
            pre = pre || (!getenv("LARECOMP_NO_PRECOMPILED") && access(path, R_OK) == 0);
            if (pre) continue;  // (precompiled: nothing to warm)
            snprintf(path, sizeof path, "cache/shaders/%s", e->d_name);
            NSString *src = [NSString stringWithContentsOfFile:@(path) encoding:NSUTF8StringEncoding error:nil];
            if (!src) continue;
            {   // (named as the first uses name them, so that the source is the same)
                NSString *nm = [@(e->d_name) stringByDeletingPathExtension];
                src = [src stringByReplacingOccurrencesOfString:@" main0(" withString:[NSString stringWithFormat:@" %@(", nm]];
            }
            if (getenv("LARECOMP_SHADER_SALT"))  // (measurements: the same salted sources the first uses compile)
                src = [src stringByAppendingFormat:@"\n// %s\n", getenv("LARECOMP_SHADER_SALT")];
            @autoreleasepool { (void)[dev newLibraryWithSource:src options:e->d_name[0] == 'v' ? vopts : nil error:nil]; }
            n++;
        }
        closedir(d);
        LOG("[shader] warm-up: %d cached shaders compiled in %.0f ms\n", n, now_ms() - t0);
    });
}

// Programs and pipelines build off the game thread and draws wait for theirs (shader_mode(), buildsync.h): SMM2
// renders some things exactly once (its colour-correction LUT, light probes), so a draw skipped while its program
// compiled in the background was never redrawn and the scene stayed black. LARECOMP_NO_SHADER_PREFETCH=1: build on
// first use, inline; LARECOMP_ASYNC_SHADERS=1: in the background, skipping the draws meanwhile (LAS's behaviour).
// The shader caches above keep all of it short after the first run.
static int compile_program(Prog *pr) {  // (BUILD_READY or BUILD_FAILED; the caller publishes it in pr->state)
    static uint64_t fail_vs = 1;  // (test) LARECOMP_DEBUG_FAIL_VS=<code address, hex>: that program fails to compile
    if (fail_vs == 1) { const char *e = getenv("LARECOMP_DEBUG_FAIL_VS"); fail_vs = e ? strtoull(e, NULL, 16) : 0; }
    if (fail_vs && pr->vs_code == fail_vs) { LOG("[shader] VS %llx fails on purpose (LARECOMP_DEBUG_FAIL_VS)\n", fail_vs); return BUILD_FAILED; }
    NSError *err = nil;
    static MTLCompileOptions *vopts;
    static dispatch_once_t once;
    static MTLCompileOptions *fopts;  // (nil: the defaults)
    dispatch_once(&once, ^{
        vopts = [MTLCompileOptions new];
        vopts.preserveInvariance = YES;
        if (getenv("LARECOMP_SAFE_MATH")) {  // (test: IEEE maths instead of fast maths, both stages)
            if (@available(macOS 15.0, iOS 18.0, *)) vopts.mathMode = MTLMathModeSafe, (fopts = [MTLCompileOptions new]).mathMode = MTLMathModeSafe;
            LOG("[shader] safe maths (LARECOMP_SAFE_MATH)\n");
        }
    });
    double t0 = now_ms();
    // LARECOMP_SHADER_SALT=<text>: tag sources so Metal's disk cache misses (measure cold first-use compiles)
    NSString *salt = getenv("LARECOMP_SHADER_SALT") ? [NSString stringWithFormat:@"\n// %s\n", getenv("LARECOMP_SHADER_SALT")] : @"";
    // (precompiled when tools/precompile_shaders.sh made them; else the two stages compile side by side: Metal's
    // compiler service runs them in parallel)
    __block id<MTLLibrary> vl = precompiled_library('v', pr->vi->msl);
    id<MTLLibrary> fl = precompiled_library('f', pr->fi->msl);
    __block NSError *verr = nil;
    dispatch_semaphore_t vdone = vl ? nil : dispatch_semaphore_create(0);
    if (vdone)
        [dev newLibraryWithSource:entry_source('v', pr->vi->msl, salt) options:vopts
                completionHandler:^(id<MTLLibrary> l, NSError *e) { vl = l; verr = e; dispatch_semaphore_signal(vdone); }];
    if (!fl) fl = [dev newLibraryWithSource:entry_source('f', pr->fi->msl, salt) options:fopts error:&err];
    if (vdone) dispatch_semaphore_wait(vdone, DISPATCH_TIME_FOREVER);
    if (!vl) { LOG("[shader] VS compile failed: %s\n%s\n", verr.localizedDescription.UTF8String, pr->vi->msl); return BUILD_FAILED; }
    if (!fl) { LOG("[shader] FS compile failed: %s\n%s\n", err.localizedDescription.UTF8String, pr->fi->msl); return BUILD_FAILED; }
    pr->vs = [vl newFunctionWithName:entry_name('v', pr->vi->msl)];
    pr->fs = [fl newFunctionWithName:entry_name('f', pr->fi->msl)];
    __atomic_fetch_add(&stat_compiles, 1, __ATOMIC_RELAXED);
    double dt = now_ms() - t0;
    @synchronized(cache) { stat_compile_ms += dt; }
    if (dt > 250) LOG("[shader] slow program: %.0f ms (VS %016llx %zu bytes, FS %016llx %zu bytes of MSL)\n", dt,
                      (unsigned long long)fnv64(pr->vi->msl), strlen(pr->vi->msl), (unsigned long long)fnv64(pr->fi->msl), strlen(pr->fi->msl));
    return BUILD_READY;
}

// A program draws can use now: in prefetch and sync mode after waiting for its build (sync: already built)
static bool prog_usable(Prog *pr) {
    if (!pr) return false;
    if (shader_mode() == SHADERS_SKIP) return atomic_load(&pr->state) == BUILD_READY;
    return build_wait(&pr->state, "program") == BUILD_READY;
}

static Prog *get_program(NvnProgram *p, ShaderKey *vkey, ShaderKey *fkey) {
    shader_cache_warm();
    if (!(p->stage_mask & 1) || !(p->stage_mask & 2)) return nil;
    // fast path: hash of the code addresses and keys
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 3; i++) { uint64_t a = p->stages[i].code_addr; for (int k = 0; k < 8; k++) { h ^= (a >> (k * 8)) & 0xFF; h *= 0x100000001B3ull; } }
    const uint8_t *vb = (const uint8_t *)vkey;
    for (size_t i = 0; i < sizeof *vkey; i++) { h ^= vb[i]; h *= 0x100000001B3ull; }
    for (int i = 0; i < 8; i++) { h ^= fkey->rt_kind[i]; h *= 0x100000001B3ull; }
    if (!h) h = 1;
    uint32_t slot = (uint32_t)(h ^ (h >> 31)) & (PROG_TAB - 1);
    while (prog_tab[slot].key && prog_tab[slot].key != h) slot = (slot + 1) & (PROG_TAB - 1);
    if (prog_tab[slot].key) { Prog *fp = (__bridge Prog *)prog_tab[slot].prog; return atomic_load(&fp->state) == BUILD_FAILED ? nil : fp; }
    NSString *key = [NSString stringWithFormat:@"%llx/%llx/%@/%@%s", p->stages[0].code_addr, p->stages[1].code_addr,
                     [NSData dataWithBytes:vkey length:sizeof *vkey], [NSData dataWithBytes:fkey->rt_kind length:8],
                     vkey->flip_x ? "/mirror" : ""];  // (NSData's description elides the middle of the key's bytes)
    Prog *pr = cache[key];
    if (pr) {
        prog_tab[slot].key = h;
        prog_tab[slot].prog = (__bridge void *)pr;
        return atomic_load(&pr->state) == BUILD_FAILED ? nil : pr;
    }
    pr = [Prog new];
    pr->vs_code = p->stages[0].code_addr;
    g_prog_creates++;
    if (getenv("LARECOMP_PROG_STATS")) {  // (measurement: how far ahead its shaders were set, whether the key is plain)
        extern uint64_t nvn_program_set_ns(const void *p);
        uint64_t set = nvn_program_set_ns(p), now = clock_gettime_nsec_np(CLOCK_MONOTONIC);
        bool plain = true;
        for (int i = 0; i < 32; i++) plain = plain && !vkey->attr_kind[i];
        for (int i = 0; i < 8; i++) plain = plain && !fkey->rt_kind[i];
        LOG("[progstat] new program %llx/%llx: shaders set %.0f ms before, key %s%s\n", p->stages[0].code_addr,
            p->stages[1].code_addr, set ? (now - set) / 1e6 : -1.0, plain ? "plain" : "typed", vkey->vs_fn ? " gs" : "");
    }
    cache[key] = pr;
    prog_tab[slot].key = h;
    prog_tab[slot].prog = (__bridge void *)pr;  // retained by the cache dictionary
    dump_program(p);
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    {
        // LARECOMP_DBG_FS=<fs code addr hex>:<pc hex>:<reg>
        static int parsed;
        static unsigned long long dbg_addr;
        static unsigned dbg_pc;
        static int dbg_reg;
        if (!parsed) { parsed = 1; const char *e = getenv("LARECOMP_DBG_FS"); if (e) sscanf(e, "%llx:%x:%d", &dbg_addr, &dbg_pc, &dbg_reg); }
        maxwell_dbg_code = dbg_addr ? (const uint8_t *)(uintptr_t)dbg_addr : NULL;
        static unsigned long long dbg_vs;
        static int vparsed;
        if (!vparsed) { vparsed = 1; const char *e = getenv("LARECOMP_DBG_VS"); if (e) dbg_vs = strtoull(e, NULL, 16); }
        maxwell_dbg_vs = dbg_vs ? (const uint8_t *)(uintptr_t)dbg_vs : NULL;
        maxwell_dbg_pc = dbg_pc;
        maxwell_dbg_reg = dbg_reg;
    }
    // the compiler constants follow the code, so they bound the instruction scan
    size_t vmax = p->stages[0].const_offset ? p->stages[0].const_offset : 0x10000;
    size_t fmax = p->stages[1].const_offset ? p->stages[1].const_offset : 0x10000;
    ShaderKey gvkey;
    if (p->stage_mask & 4) {  // geometry stage: translated first (the vertex function carries its varyings)
        size_t gmax = p->stages[2].const_offset ? p->stages[2].const_offset : 0x10000;
        pr->gi = maxwell_translate((const uint8_t *)(uintptr_t)p->stages[2].code_addr, gmax, STAGE_GEOMETRY, vkey);
        if (!pr->gi->msl) {
            LOG("[shader] GS translate failed: %s (code %llx)\n", pr->gi->error, p->stages[2].code_addr);
            atomic_store(&pr->state, BUILD_FAILED);
            return nil;
        }
        gvkey = *vkey;
        gvkey.vs_fn = 1;
        gvkey.gs_out_mask = pr->gi->out_mask;
        vkey = &gvkey;
    }
    pr->vi = maxwell_translate((const uint8_t *)(uintptr_t)p->stages[0].code_addr, vmax, STAGE_VERTEX, vkey);
    if (pr->vi->msl && pr->gi) {  // the combined vertex function: vertex shader for the point, then the geometry stage
        if ((pr->vi->cbuf_mask >> GS_CBUF_BASE) & pr->gi->cbuf_mask) {
            LOG("[shader] GS cbufs clash with the vertex shader's (code %llx)\n", p->stages[2].code_addr);
            atomic_store(&pr->state, BUILD_FAILED);
            return nil;
        }
        NSMutableString *m = [NSMutableString stringWithUTF8String:pr->vi->msl];
        [m appendFormat:@"vertex VOut main0(VIn in [[stage_in]], uint vid [[vertex_id]], uint iid [[instance_id]]%s%s) {\n",
               pr->vi->vs_entry_params, pr->gi->gs_params];
        [m appendFormat:@"  VOut vso = vs_main(in, iid, iid%s);\n", pr->vi->vs_call_args];
        [m appendString:@"  uint tri = vid / 3u, corner = vid % 3u;  // triangle tri of the emitted strip\n"
                   @"  uint want = (tri & 1u) ? (corner == 0u ? tri + 1u : corner == 1u ? tri : tri + 2u) : tri + corner;\n"
                   @"  VOut out = vso;\n"];
        [m appendString:@(pr->gi->msl)];
        if (vkey->flip_y) [m appendString:@"  out.pos.y = -out.pos.y;\n"];
        if (vkey->flip_x) [m appendString:@"  out.pos.x = -out.pos.x;\n"];
        if (vkey->depth_minus_one) [m appendString:@"  out.pos.z = (out.pos.z + out.pos.w) * 0.5f;\n"];
        [m appendString:@"  return out;\n}\n"];
        free(pr->vi->msl);
        pr->vi->msl = strdup(m.UTF8String);
        pr->vi->out_mask = pr->gi->out_mask;  // (what the fragment stage gets)
    }
    if (!pr->vi->msl) {
        LOG("[shader] VS translate failed: %s (code %llx)\n", pr->vi->error, p->stages[0].code_addr);
        char path[128];
        snprintf(path, sizeof path, "build/shaders/fail_%llx_s0_code.bin", p->stages[0].code_addr);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite((void *)(uintptr_t)p->stages[0].code_addr, 1, 0x4000, f); fclose(f); }
        atomic_store(&pr->state, BUILD_FAILED);
        return nil;
    }
    fkey->vs_out_mask = pr->vi->out_mask;
    pr->fi = maxwell_translate((const uint8_t *)(uintptr_t)p->stages[1].code_addr, fmax, STAGE_FRAGMENT, fkey);
    if (!pr->fi->msl) { LOG("[shader] FS translate failed: %s\n", pr->fi->error); atomic_store(&pr->state, BUILD_FAILED); return nil; }
    if (getenv("LARECOMP_SAVE_MSL")) {
        mkdir("build/msl", 0755);
        char path[256];
        snprintf(path, sizeof path, "build/msl/%llx_vs.metal", p->stages[0].code_addr);
        FILE *f = fopen(path, "w"); fputs(pr->vi->msl, f); fclose(f);
        snprintf(path, sizeof path, "build/msl/%llx_fs.metal", p->stages[1].code_addr);
        f = fopen(path, "w"); fputs(pr->fi->msl, f); fclose(f);
    }
    if (getenv("LARECOMP_SAVE_GLSL") && !pr->gi) {  // (debug) the same programs as the Vulkan backend translates them
        ShaderKey gv = *vkey, gf = *fkey;
        gv.glsl = gf.glsl = 1;
        ShaderInfo *a = maxwell_translate((const uint8_t *)(uintptr_t)p->stages[0].code_addr, vmax, STAGE_VERTEX, &gv);
        gf.vs_out_mask = a->out_mask;
        ShaderInfo *b = a->msl ? maxwell_translate((const uint8_t *)(uintptr_t)p->stages[1].code_addr, fmax, STAGE_FRAGMENT, &gf) : NULL;
        mkdir("build/glsl", 0755);
        char path[256];
        snprintf(path, sizeof path, "build/glsl/%llx_vs.glsl", p->stages[0].code_addr);
        FILE *f = fopen(path, "w"); if (f) { fputs(a->msl ? a->msl : a->error, f); fclose(f); }
        snprintf(path, sizeof path, "build/glsl/%llx_fs.glsl", p->stages[1].code_addr);
        f = fopen(path, "w"); if (f) { fputs(b && b->msl ? b->msl : b ? b->error : "", f); fclose(f); }
    }
    shader_cache_note(pr->vi->msl, pr->fi->msl);
    if (shader_mode() == SHADERS_SYNC) {
        atomic_store(&pr->state, compile_program(pr));
        return atomic_load(&pr->state) == BUILD_FAILED ? nil : pr;
    }
    // First-time compiles take ~40 ms each on Metal: built off the game thread, side by side
    static dispatch_queue_t q;
    if (!q) q = dispatch_queue_create("larecomp.shaders", DISPATCH_QUEUE_CONCURRENT);
    build_begin();
    dispatch_async(q, ^{
        int r = compile_program(pr);
        // The scan's pipeline requests go out before the program shows ready (all under its lock, which the scan's
        // check-and-add takes too): a draw that sees it ready finds them, and they're in flight before it is not.
        @synchronized(pr) {
            NSArray *w = pr->waiting;
            pr->waiting = nil;
            build_test_delay();
            if (r == BUILD_READY) for (PipeReq *q in w) pipe_request(pr, q->pd, q->key, q->khash, NULL);
            build_finish(&pr->state, r);
        }
    });
    return pr;
}

static id<MTLSamplerState> sampler_for(NvnSamplerDesc *d) {
    if (d->backend) return (__bridge id<MTLSamplerState>)d->backend;
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.magFilter = d->mag_filter == 1 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    uint32_t mf = d->min_filter;
    sd.minFilter = (mf == 1 || mf == 3 || mf == 5) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.mipFilter = mf <= 1 ? MTLSamplerMipFilterNotMipmapped : (mf == 2 || mf == 3) ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
    MTLSamplerAddressMode modes[3];
    for (int i = 0; i < 3; i++) {
        switch (d->wrap[i]) {
        case 0: case 7: modes[i] = MTLSamplerAddressModeClampToEdge; break;
        case 1: modes[i] = MTLSamplerAddressModeRepeat; break;
        case 2: case 3: modes[i] = MTLSamplerAddressModeMirrorClampToEdge; break;
        case 4: case 5: modes[i] = MTLSamplerAddressModeClampToBorderColor; break;
        case 6: modes[i] = MTLSamplerAddressModeMirrorRepeat; break;
        default: modes[i] = MTLSamplerAddressModeRepeat;
        }
    }
    sd.sAddressMode = modes[0]; sd.tAddressMode = modes[1]; sd.rAddressMode = modes[2];
    // Border colour (clamp-to-border): Metal has transparent black, opaque black and opaque white; the guest's colour
    // goes to the nearest. A depth-compare sampler compares against the red component: SMM2's 3D World shadow maps
    // use white (1.0: lit) outside the map, so the near edge of the lava was in shadow with Metal's default black.
    {
        const float *bc = d->border;
        sd.borderColor = bc[3] < 0.5f ? MTLSamplerBorderColorTransparentBlack
                       : (bc[0] + bc[1] + bc[2]) >= 1.5f ? MTLSamplerBorderColorOpaqueWhite : MTLSamplerBorderColorOpaqueBlack;
    }
    sd.lodMinClamp = d->lod_min;
    sd.lodMaxClamp = d->lod_max > 1000 ? FLT_MAX : d->lod_max;
    if (d->max_aniso > 1) sd.maxAnisotropy = (NSUInteger)fminf(d->max_aniso, 16);
    if (d->lod_bias != 0 && getenv("LARECOMP_LOG_BIAS"))
        LOG("[sampler] lod bias %g (min filter %u, aniso %g)\n", d->lod_bias, mf, d->max_aniso);
    static float lodmin = -1;  // LARECOMP_LOD_MIN=<n>: skip the n sharpest mips everywhere (aliasing experiments)
    if (lodmin < 0) lodmin = getenv("LARECOMP_LOD_MIN") ? (float)atof(getenv("LARECOMP_LOD_MIN")) : 0;
    if (lodmin > 0 && mf > 1) sd.lodMinClamp = fmaxf(sd.lodMinClamp, lodmin);
    static int stest = -1;  // LARECOMP_SAMPLER_TEST: full mip range + 16x aniso on mipmapped samplers (moire test)
    if (stest < 0) stest = getenv("LARECOMP_SAMPLER_TEST") != NULL;
    if (stest && mf > 1) { sd.lodMinClamp = 0; sd.lodMaxClamp = FLT_MAX; sd.maxAnisotropy = 16; }
    if (getenv("LARECOMP_LOG_SAMPLERS"))
        LOG("[sampler] min %u mag %u lod %g..%g aniso %g wrap %u %u\n", mf, d->mag_filter, d->lod_min, d->lod_max, d->max_aniso,
            d->wrap[0], d->wrap[1]);
    if (d->compare_mode) sd.compareFunction = getenv("LARECOMP_NO_SHADOW") ? MTLCompareFunctionAlways : compare_fn((uint8_t)d->compare_func);
    if (d->compare_mode && getenv("LARECOMP_LOG_SAMPLERS")) LOG("[sampler] compare func %u (min %u mag %u)\n", d->compare_func, mf, d->mag_filter);
    sd.supportArgumentBuffers = NO;
    id<MTLSamplerState> s = [dev newSamplerStateWithDescriptor:sd];
    d->backend = (void *)CFBridgingRetain(s);
    return s;
}

static id<MTLSamplerState> default_sampler(void) {
    static id<MTLSamplerState> s;
    if (!s) {
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        s = [dev newSamplerStateWithDescriptor:sd];
    }
    return s;
}

static id<MTLTexture> dummy_texture(int type) {
    static id<MTLTexture> t[10];
    if (!t[type]) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor new];
        d.pixelFormat = MTLPixelFormatRGBA8Unorm;
        d.width = d.height = 1;
        d.textureType = type == TEXTYPE_CUBE ? MTLTextureTypeCube : type == TEXTYPE_3D ? MTLTextureType3D
                      : type == TEXTYPE_2D_ARRAY ? MTLTextureType2DArray : type == TEXTYPE_1D ? MTLTextureType1D
                      : MTLTextureType2D;
        t[type] = [dev newTextureWithDescriptor:d];
    }
    return t[type];
}

static MTLTextureType mtl_type_for(int t) {
    switch (t) {
    case TEXTYPE_1D: return MTLTextureType1D;
    case TEXTYPE_3D: return MTLTextureType3D;
    case TEXTYPE_CUBE: return MTLTextureTypeCube;
    case TEXTYPE_2D_ARRAY: return MTLTextureType2DArray;
    case TEXTYPE_CUBE_ARRAY: return MTLTextureTypeCubeArray;
    case TEXTYPE_1D_ARRAY: return MTLTextureType1DArray;
    default: return MTLTextureType2D;
    }
}

// The texture view with a channel swizzle is declared since macOS 10.15, but Intel Macs' NVIDIA driver (Metal 2 only)
// does not implement it (nor the descriptor's swizzle): there the translated shaders apply it instead, from the table
// renderer_swizzle_row fills (see TSWZ_BUFFER_INDEX).
static bool swizzle_view_supported(void) {
    static int ok = -1;
    if (ok < 0) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor new];
        d.pixelFormat = MTLPixelFormatRGBA8Unorm;
        d.width = d.height = 1;
        id<MTLTexture> t = [dev newTextureWithDescriptor:d];
        ok = [t respondsToSelector:@selector(newTextureViewWithPixelFormat:textureType:levels:slices:swizzle:)] ? 1 : 0;
        if (!ok) LOG("[tex] this driver has no swizzle texture views: the shaders apply them\n");
    }
    return ok == 1;
}

// The channel swizzle a bound texture is sampled with: the view's if it sets one, else the texture's (NVN's enum,
// the same values as MTLTextureSwizzle). Returns false when none was ever set (identity).
static bool bind_swizzle(NvnTexture *t, const NvnTextureView *v, bool has_view, uint8_t c[4]) {
    const uint8_t *sw = (has_view && (v->setmask & 8)) ? v->swizzle : t->swizzle;
    for (int i = 0; i < 4; i++) c[i] = sw[i] <= 5 ? sw[i] : (uint8_t)(2 + i);
    return c[0] || c[1] || c[2] || c[3];
}

// NVN texture/view component swizzle (same enum values as MTLTextureSwizzle), applied as a view.
static id<MTLTexture> apply_swizzle(id<MTLTexture> tex, NvnTexture *t, const NvnTextureView *v, bool has_view) {
    uint8_t c[4];
    if (!bind_swizzle(t, v, has_view, c)) return tex;  // never set: identity
    if (!swizzle_view_supported()) return tex;  // (the shader applies it there: renderer_swizzle_row)
    if (renderer_format_alpha_one(t->format))
        for (int i = 0; i < 4; i++) if (c[i] == MTLTextureSwizzleAlpha) c[i] = MTLTextureSwizzleOne;
    if (c[0] == 2 && c[1] == 3 && c[2] == 4 && c[3] == (renderer_format_alpha_one(t->format) ? 1 : 5)) return tex;
    if (tex.sampleCount > 1 || renderer_format_is_depth(t->format)) return tex;
    uint32_t swz = c[0] | c[1] << 8 | c[2] << 16 | (uint32_t)c[3] << 24;
    MTLTextureSwizzleChannels chans = MTLTextureSwizzleChannelsMake(c[0], c[1], c[2], c[3]);
    id<MTLTexture> view = view_cached(tex, 0x5000000000000000ull | swz, ^{
        NSUInteger slices = tex.textureType == MTLTextureType3D ? 1 : tex.arrayLength * (tex.textureType == MTLTextureTypeCube || tex.textureType == MTLTextureTypeCubeArray ? 6 : 1);
        return [tex newTextureViewWithPixelFormat:tex.pixelFormat textureType:tex.textureType
                                           levels:NSMakeRange(0, tex.mipmapLevelCount) slices:NSMakeRange(0, slices)
                                          swizzle:chans];
    });
    return view;
}

// What the shader must apply for a bound texture (TSWZ_BUFFER_INDEX, write_tex_result in maxwell/translate.c): identity
// where the swizzle is a texture view, the real channels where the driver has no swizzle views (Intel Macs' NVIDIA
// driver, Metal 2 only: its textures answer neither the view with a swizzle nor the descriptor's swizzle).
static void renderer_swizzle_row(NvnTexture *t, const NvnTextureView *v, bool has_view, uint32_t row[4]) {
    row[0] = MTLTextureSwizzleRed;
    row[1] = MTLTextureSwizzleGreen;
    row[2] = MTLTextureSwizzleBlue;
    row[3] = MTLTextureSwizzleAlpha;
    if (!t || swizzle_view_supported() || renderer_format_is_depth(t->format)) return;
    uint8_t c[4];
    if (!bind_swizzle(t, v, has_view, c)) return;
    if (c[0] == 2 && c[1] == 3 && c[2] == 4 && c[3] == 5) return;  // identity: nothing to do
    for (int i = 0; i < 4; i++) row[i] = c[i];
    static uint32_t seen[16];  // (one line per distinct swizzle, not per draw)
    static int nseen;
    uint32_t key = c[0] | c[1] << 8 | c[2] << 16 | (uint32_t)c[3] << 24;
    for (int i = 0; i < nseen; i++) if (seen[i] == key) return;
    if (nseen < 16) seen[nseen++] = key;
    LOG("[tex] shader swizzle %d%d%d%d (%ux%u fmt %#x)\n", c[0], c[1], c[2], c[3], t->width, t->height, t->format);
}

// Upload helper: a separate command buffer, committed now so it runs before the current pass.
static id<MTLTexture> sampled_texture(NvnTexture *t, const NvnTextureView *v, bool has_view, int want_type) {
    // uploads (if the CPU-side contents changed) go into their own command buffer, committed before
    // the current one so they land first
    id<MTLTexture> tex = view_texture(t, v, has_view, false, nil);
    tex = apply_swizzle(tex, t, v, has_view);
    MTLTextureType want = mtl_type_for(want_type);
#if TARGET_OS_SIMULATOR
    if (want == MTLTextureTypeCubeArray) return nil;  // (the Simulator has no cube arrays: those lookups read zero there)
#endif
    if (tex.textureType != want && tex.textureType != MTLTextureType2DMultisample) {
        id<MTLTexture> src = tex;
        if (want == MTLTextureType2D && (tex.textureType == MTLTextureType2DArray || tex.textureType == MTLTextureTypeCube))
            tex = view_cached(src, 0x6000000000000000ull | 1, ^{ return [src newTextureViewWithPixelFormat:src.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(0, src.mipmapLevelCount) slices:NSMakeRange(0, 1)]; });
        else if (want == MTLTextureType2DArray && tex.textureType == MTLTextureType2D)
            tex = view_cached(src, 0x6000000000000000ull | 2, ^{ return [src newTextureViewWithPixelFormat:src.pixelFormat textureType:MTLTextureType2DArray levels:NSMakeRange(0, src.mipmapLevelCount) slices:NSMakeRange(0, 1)]; });
        else if (want == MTLTextureTypeCube && tex.textureType == MTLTextureType2DArray && tex.arrayLength >= 6)
            tex = view_cached(src, 0x6000000000000000ull | 3, ^{ return [src newTextureViewWithPixelFormat:src.pixelFormat textureType:MTLTextureTypeCube levels:NSMakeRange(0, src.mipmapLevelCount) slices:NSMakeRange(0, 6)]; });
        else if (want == MTLTextureTypeCubeArray && tex.textureType == MTLTextureType2DArray && tex.arrayLength >= 6)
            // (a 2D array sampled as cubes, six layers each: SMM2's character reflection maps, 72 layers)
            tex = view_cached(src, 0x6000000000000000ull | 4, ^{ return [src newTextureViewWithPixelFormat:src.pixelFormat textureType:MTLTextureTypeCubeArray levels:NSMakeRange(0, src.mipmapLevelCount) slices:NSMakeRange(0, src.arrayLength / 6 * 6)]; });
        else if (want == MTLTextureTypeCubeArray && tex.textureType == MTLTextureTypeCube)
            tex = view_cached(src, 0x6000000000000000ull | 5, ^{ return [src newTextureViewWithPixelFormat:src.pixelFormat textureType:MTLTextureTypeCubeArray levels:NSMakeRange(0, src.mipmapLevelCount) slices:NSMakeRange(0, 6)]; });
        else {
            static int nlog;
            if (nlog++ < 30) LOG("[tex] type mismatch: want %lu have %lu (%ux%ux%u target %u fmt %#x arr %lu)\n", (unsigned long)want,
                                 (unsigned long)tex.textureType, t->width, t->height, t->depth, t->target, t->format, (unsigned long)tex.arrayLength);
            return dummy_texture(want_type);
        }
    }
    return tex;
}


// ---- render-target feedback: a draw that samples one of the pass's own attachments (e.g. depth
// bound for testing while the shader reads it) gets a snapshot copy, as sampling an attachment of the
// current Metal pass is undefined.
static NvnTexture *feedback_tex[4];
static id<MTLTexture> feedback_copy[4];
static int nfeedback;
// Snapshots stay valid until their texture is written again (a draw writing that attachment, a clear, copy or
// compute). Soft particles (grass-cut leaves, smoke) sample the depth buffer they're tested against but don't
// write it, so a burst of them shares one snapshot instead of a pass break + full-screen copy per draw.
static uint64_t g_cur_fs_hash;
static const char *snap_why[8];
static const char *snap_reason = "?";
static void snapshots_invalidate(NvnTexture *t) {  // NULL = all
    for (int i = 0; i < 8; i++) if ((!t || snap_tex[i] == t) && snap_ok[i]) { snap_ok[i] = false; snap_why[i] = snap_reason; }
}

static NvnTexture *shader_texture_object(int stage, ShaderTexture *st_) {
    int slot = ((int)st_->handle_offset - 0x20) / 8;
    uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
    uint32_t tic = h & 0xFFFFF;
    if (!h || !st.tpool || tic >= (uint32_t)st.tpool->count) return NULL;
    return ((NvnTexEntry *)st.tpool->entries)[tic].tex;
}

static bool is_attachment(NvnTexture *t) {
    if (!t) return false;
    if (t == st.rt.depth) return true;
    for (int i = 0; i < st.rt.ncolor; i++) if (t == st.rt.color[i]) return true;
    return false;
}

static void resolve_feedback(Prog *pr) {
    nfeedback = 0;
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_NO_FEEDBACK") != NULL;
    if (off) return;
    ShaderInfo *infos[2] = {pr->vi, pr->fi};
    for (int sg = 0; sg < 2; sg++)
        for (int i = 0; i < infos[sg]->ntex; i++) {
            NvnTexture *t = shader_texture_object(sg, &infos[sg]->tex[i]);
            if (!is_attachment(t)) continue;
            bool have = false;
            for (int k = 0; k < nfeedback; k++) have |= feedback_tex[k] == t;
            if (have || nfeedback == 4) continue;
            if (!g_snap_copies) g_snap_copies = [NSMutableDictionary new];
            NSMutableDictionary *copies = g_snap_copies;
            NSValue *key = [NSValue valueWithPointer:t];
            static int nosnap = -1;
            if (nosnap < 0) nosnap = getenv("LARECOMP_NO_SNAPSHOT_REUSE") != NULL;
            int si = -1;
            for (int k = 0; k < 8; k++) if (snap_tex[k] == t) si = k;
            // A draw that writes the same depth buffer it samples can't depend on what it reads (that is undefined on the
            // Switch too): seen in the shadow-map pass, whose material shader still declares the shadow map from the
            // main pass. Any earlier snapshot does; no copy, no pass break.
            bool self_write = t == st.rt.depth && st.ds[0] && st.ds[1];
            if (!nosnap && si >= 0 && (snap_ok[si] || self_write) && copies[key]) {  // unchanged since the last snapshot
                feedback_tex[nfeedback] = t;
                feedback_copy[nfeedback++] = copies[key];
                continue;
            }
            stat_feedback_copies++;
            static int logfb = -1;
            if (logfb < 0) logfb = getenv("LARECOMP_LOG_FEEDBACK") != NULL;
            if (logfb && stat_feedback_copies % 50 == 1)
                LOG("[feedback] copy %s %ux%u fmt %#x stage %d fs %016llx (last invalidated by %s) ds %d/%d/%d stencil %d ncolor %d c0 %ux%u fmt %#x mask %x\n", t == st.rt.depth ? "DEPTH" : "COLOR",
                    t->width, t->height, t->format, sg, (unsigned long long)g_cur_fs_hash, si >= 0 ? snap_why[si] : "new",
                    st.ds[0], st.ds[1], st.ds[2], st.ds[3], st.rt.ncolor, st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->width : 0,
                    st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->height : 0, st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->format : 0, st.channel_mask);
            end_pass();
            id<MTLTexture> src = renderer_texture(t, nil, true);
            id<MTLTexture> dst = copies[key];
            if (!dst || dst.width != src.width || dst.height != src.height || dst.pixelFormat != src.pixelFormat) {
                MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:src.pixelFormat width:src.width
                                                                                           height:src.height mipmapped:NO];
                d.textureType = src.textureType == MTLTextureType2DMultisample ? MTLTextureType2DMultisample : MTLTextureType2D;
                d.sampleCount = src.sampleCount;
                d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
                d.storageMode = MTLStorageModePrivate;
                dst = [dev newTextureWithDescriptor:d];
                copies[key] = dst;
            }
            id<MTLBlitCommandEncoder> b = [cb() blitCommandEncoder];
            [b copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                    sourceSize:MTLSizeMake(src.width, src.height, 1) toTexture:dst destinationSlice:0 destinationLevel:0
             destinationOrigin:MTLOriginMake(0, 0, 0)];
            [b endEncoding];
            feedback_tex[nfeedback] = t;
            feedback_copy[nfeedback++] = dst;
            if (si < 0) { static int rr; si = rr++ & 7; snap_tex[si] = t; }
            snap_ok[si] = true;
            st.rt_dirty = true;  // the pass resumes (loading) after the copy
        }
}

static void bind_textures(int stage, ShaderInfo *si) {
    if (stage == 1) draw_bottle = draw_heart = 0, draw_item_cursor = false;
    uint32_t swz[64][4];  // (the shader's channel swizzles: see renderer_swizzle_row)
    for (int i = 0; i < si->ntex && i < 64; i++) renderer_swizzle_row(NULL, NULL, false, swz[i]);
    for (int i = 0; i < si->ntex; i++) {
        ShaderTexture *st_ = &si->tex[i];
        int slot = ((int)st_->handle_offset - 0x20) / 8;  // 64-bit handles from offset 0x20
        uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
        id<MTLTexture> tex = nil;
        id<MTLSamplerState> smp = nil;
        uint32_t tic = h & 0xFFFFF, tsc = (uint32_t)(h >> 20) & 0xFFF;
        tex_sc[stage][i][0] = tex_sc[stage][i][1] = 1;
        if (i < 64) tex_wh[stage][i][0] = tex_wh[stage][i][1] = 0;
        if (g_debug_frame)
            LOG("[tx] stage %d tex %d: handle offset %#x slot %d h %llx tic %u pool %p count %d\n", stage, i, st_->handle_offset, slot,
                (unsigned long long)h, tic, (void *)st.tpool, st.tpool ? st.tpool->count : -1);
        if (h && st.tpool && tic < (uint32_t)st.tpool->count) {
            NvnTexEntry *e = &((NvnTexEntry *)st.tpool->entries)[tic];
            if (e->tex) tex = sampled_texture(e->tex, &e->view, e->has_view, st_->type);
            if (i < 64) renderer_swizzle_row(e->tex, &e->view, e->has_view, swz[i]);
            if (g_debug_frame && e->tex) LOG("[tx] stage %d tex %d slot %d: nvn %p mtl %p (%lux%lu fmt %lu type %lu) view %d (mask %#x swz %d%d%d%d) tex swz %d%d%d%d mtl swz %08x\n", stage, i, slot, (void *)e->tex, (__bridge void *)tex, (unsigned long)tex.width, (unsigned long)tex.height, (unsigned long)tex.pixelFormat, (unsigned long)tex.textureType, e->has_view, e->view.setmask, e->view.swizzle[0], e->view.swizzle[1], e->view.swizzle[2], e->view.swizzle[3], e->tex->swizzle[0], e->tex->swizzle[1], e->tex->swizzle[2], e->tex->swizzle[3], (unsigned)(tex.swizzle.red | tex.swizzle.green << 8 | tex.swizzle.blue << 16 | tex.swizzle.alpha << 24));
            if (e->tex && stage == 1) {
                extern int renderer_texpatch_of(NvnTexture *t);
                extern int texpatch_bottle_kind(int id);
                extern int texpatch_heart_kind(int id);
                extern bool texpatch_is_item_cursor(int id);
                int id = renderer_texpatch_of(e->tex);
                if (!draw_bottle) draw_bottle = texpatch_bottle_kind(id);
                if (!draw_heart) draw_heart = texpatch_heart_kind(id);
                draw_item_cursor |= texpatch_is_item_cursor(id);
            }
            if (e->tex && i < 64) {
                renderer_texture_scale(e->tex, &tex_sc[stage][i][0], &tex_sc[stage][i][1]);
                tex_wh[stage][i][0] = e->tex->width; tex_wh[stage][i][1] = e->tex->height;
            }
            for (int k = 0; k < nfeedback; k++)
                if (e->tex == feedback_tex[k]) {
                    id<MTLTexture> fc = feedback_copy[k];
                    MTLTextureType want = mtl_type_for(st_->type);
                    if (fc.textureType != want && want == MTLTextureType2DArray)
                        fc = [fc newTextureViewWithPixelFormat:fc.pixelFormat textureType:MTLTextureType2DArray levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1)];
                    tex = fc;
                }
            if (g_debug_frame && e->tex && e->tex->addr >= GUEST_LO && e->tex->addr < GUEST_HI) {
                uint64_t total = tegra_total_size(&e->tex->layout), nz = 0;
                const uint8_t *p = (const uint8_t *)(uintptr_t)e->tex->addr;
                for (uint64_t k = 0; k < total; k++) nz += p[k] != 0;
                if (e->tex->width == 1024 && e->tex->height == 2048) {
                    static int once;
                    if (!once++ || g_debug_frame) {
                        id<MTLTexture> base = renderer_texture(e->tex, nil, false);
                        LOG("[dtex] glyph cache target %u depth %u layers %u levels %u stride %u flags %#x base type %lu arrlen %lu\n",
                            e->tex->target, e->tex->depth, e->tex->layout.layers, e->tex->levels, e->tex->stride, e->tex->flags,
                            (unsigned long)base.textureType, (unsigned long)base.arrayLength);
                        LOG("[dtex] glyph view: has %d mask %#x levels %u+%u layers %u+%u fmt %#x target %u\n", e->has_view, e->view.setmask,
                            e->view.base_level, e->view.num_levels, e->view.min_layer, e->view.num_layers, e->view.format, e->view.target);
                        LOG("[dtex] glyph cache: mtl fmt %lu type %lu swizzle tex %d %d %d %d view(%d) %d %d %d %d, tex.swz %08x\n",
                            (unsigned long)base.pixelFormat, (unsigned long)tex.textureType, e->tex->swizzle[0], e->tex->swizzle[1],
                            e->tex->swizzle[2], e->tex->swizzle[3], e->has_view && (e->view.setmask & 8), e->view.swizzle[0],
                            e->view.swizzle[1], e->view.swizzle[2], e->view.swizzle[3],
                            (unsigned)(tex.swizzle.red | tex.swizzle.green << 8 | tex.swizzle.blue << 16 | tex.swizzle.alpha << 24));
                        renderer_save_texture(base, "build/glyph_gpu.png");
                        NvnSamplerDesc *sd = (h && st.spool && tsc < (uint32_t)st.spool->count) ? &((NvnSamplerDesc *)st.spool->entries)[tsc] : NULL;
                        if (sd) { const uint32_t *w = (const uint32_t *)sd; LOG("[dtex] glyph sampler words %08x %08x %08x %08x %08x %08x %08x %08x\n", w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]); }
                    }
                }
                if (e->tex->width == 2 && e->tex->height == 2 && e->tex->depth == 6) {
                    static int once2;
                    if (!once2++) {
                        FILE *rf = fopen("build/raw_cube2.bin", "wb");
                        if (rf) { fwrite(p, 1, total, rf); fclose(rf); }
                        LOG("[dtex] cube2 levels %u layers %u bh %u layer_size %llu\n", e->tex->levels, e->tex->layout.layers,
                            e->tex->layout.block_height_log2, tegra_layer_size(&e->tex->layout));
                    }
                }
                if ((e->tex->flags & 0x10) && !getenv("NO_RAW")) {
                    static int nraw;
                    if (nraw++ < 2) {
                        char rp[64];
                        snprintf(rp, sizeof rp, "build/raw_%llx.bin", e->tex->addr);
                        FILE *rf = fopen(rp, "wb");
                        if (rf) { fwrite(p, 1, total, rf); fclose(rf); }
                        LOG("[dtex] linear texture stride %u -> %s\n", e->tex->stride, rp);
                    }
                }
                LOG("[dtex] slot %d tex %ux%u f%#x addr %llx size %llu nonzero %llu gpu_owned %d\n", i, e->tex->width,
                    e->tex->height, e->tex->format, e->tex->addr, total, nz, renderer_is_gpu_owned(e->tex));
            }
        }
        if (h && st.spool && tsc < (uint32_t)st.spool->count) smp = sampler_for(&((NvnSamplerDesc *)st.spool->entries)[tsc]);
        if (!tex) tex = dummy_texture(st_->type);
        if (!smp) smp = default_sampler();
        if (g_debug_frame && tex.textureType == MTLTextureType2D && stage == 1) {
            static int dn;
            char path[128];
            snprintf(path, sizeof path, "build/dtex_%03d_%d.png", dn++, i);
            renderer_save_texture(tex, path);
            snprintf(path, sizeof path, "build/dtex_%03d_%d_a.png", dn - 1, i);
            renderer_save_texture_alpha(tex, path);
            snprintf(path, sizeof path, "build/dtex_%03d_%d_alpha.png", dn - 1, i);  // (sampled alpha, any format)
            renderer_save_texture(tex, path);
        }
        if (stage == 0) { [enc setVertexTexture:tex atIndex:i]; [enc setVertexSamplerState:smp atIndex:i]; }
        else { [enc setFragmentTexture:tex atIndex:i]; [enc setFragmentSamplerState:smp atIndex:i]; }
    }
    if (si->ntex) {
        if (stage == 0) [enc setVertexBytes:swz length:(NSUInteger)si->ntex * 16 atIndex:TSWZ_BUFFER_INDEX];
        else [enc setFragmentBytes:swz length:(NSUInteger)si->ntex * 16 atIndex:TSWZ_BUFFER_INDEX];
    }
}

static void bind_cbufs(int stage, ShaderInfo *si) {
    for (int k = 0; k < 18; k++) {
        if (!(si->cbuf_mask & (1u << k))) continue;
        if (k == 0) {
            // driver constants: storage buffer bindings {addr_lo, addr_hi, size, 0} in a 0x100-byte table
            // per hardware stage at 0x110 + 0x100 * stage (vertex 0, fragment 4)
            uint32_t drv[0x800 / 4] = {0};
            uint32_t base = 0x110 + 0x100 * (stage == 1 ? 4 : 0);
            for (int i = 0; i < 16; i++) {
                uint32_t *e = &drv[(base + 16 * i) / 4];
                e[0] = (uint32_t)st.sbuf[stage][i].addr;
                e[1] = (uint32_t)(st.sbuf[stage][i].addr >> 32);
                e[2] = (uint32_t)st.sbuf[stage][i].size;
                if (si->uses_gmem) gpu_read_mark(st.sbuf[stage][i].addr, st.sbuf[stage][i].size);
            }
            uint64_t size = si->cbuf_size[0] > sizeof drv ? sizeof drv : (si->cbuf_size[0] ? si->cbuf_size[0] : 256);
            uint64_t off = ring_alloc(drv, size);
            if (stage == 0) [enc setVertexBuffer:ring offset:off atIndex:0];
            else [enc setFragmentBuffer:ring offset:off atIndex:0];
            continue;
        }
        if (k == 1) {  // compiler constants stored after the shader code
            NvnShaderStage *ss = &st.program->stages[stage];
            uint64_t size = ss->const_size ? ss->const_size : 16;
            if (size < si->cbuf_size[1]) size = si->cbuf_size[1];
            uint64_t off = ring_alloc((void *)(uintptr_t)(ss->code_addr + ss->const_offset), size);
            if (stage == 0) [enc setVertexBuffer:ring offset:off atIndex:1];
            else [enc setFragmentBuffer:ring offset:off atIndex:1];
            continue;
        }
        int idx = k - cbuf_base;
        Range r = (idx >= 0 && idx < 18) ? st.ubuf[stage][idx] : (Range){0, 0};
        uint64_t size = si->cbuf_size[k];
        if (!r.addr) {
            static const uint8_t zeros[0x10000];
            uint64_t off = ring_alloc(zeros, size ? size : 256);
            if (stage == 0) [enc setVertexBuffer:ring offset:off atIndex:k];
            else [enc setFragmentBuffer:ring offset:off atIndex:k];
            continue;
        }
        if ((si->cbuf_dynamic & (1u << k)) && r.size) size = r.size > 0x10000 ? 0x10000 : r.size;  // indexed: whole buffer
        if (size > r.size && r.size) size = r.size;
        if (size < 16) size = 16;
        uint64_t off = ring_alloc((void *)(uintptr_t)r.addr, size);  // snapshot, as later CPU writes must not leak in
        if (stage == 0) [enc setVertexBuffer:ring offset:off atIndex:k];
        else [enc setFragmentBuffer:ring offset:off atIndex:k];
        if (g_debug_frame) {
            const float *f = (const float *)((uint8_t *)ring.contents + off);
            LOG("[cbuf] stage %d c%d <- ubo %d %llx size %llu (ring %p +%llu): %g %g %g %g\n", stage, k, idx,
                (unsigned long long)r.addr, (unsigned long long)size, (__bridge void *)ring, (unsigned long long)off, f[0], f[1],
                f[2], f[3]);
        }
    }
}


// ------------------------------------------------------------------ compute
// Compute driver constants (c0): 0x200 work group counts, 0x210 + 16*i uniform buffers (compute has
// few hardware cbuf slots, so the compiler reads the rest through global memory), 0x310 + 16*i
// storage buffers; each table entry is {addr_lo, addr_hi, size, 0}.
typedef struct { id<MTLComputePipelineState> pipe; ShaderInfo *si; uint32_t local[3]; _Atomic int state; uint64_t code; } CsProg;
static uint64_t g_cs_creates;  // compute programs made (game thread)
static NSMutableDictionary *g_cs_progs;  // code address -> CsProg

// A compute program's library and pipeline (BUILD_READY or BUILD_FAILED; any thread)
static int build_compute(CsProg *cp) {
    NSError *err = nil;
    NSString *csalt = getenv("LARECOMP_SHADER_SALT") ? [NSString stringWithFormat:@"\n// %s\n", getenv("LARECOMP_SHADER_SALT")] : @"";
    double ct0 = now_ms(), clib = 0, built = 0;
    bool chit = false;
    id<MTLLibrary> lib = precompiled_library('c', cp->si->msl);
    if (!lib) {
        lib = [dev newLibraryWithSource:entry_source('c', cp->si->msl, csalt) options:nil error:&err];
        built += now_ms() - ct0;
    }
    id<MTLFunction> f = lib ? [lib newFunctionWithName:entry_name('c', cp->si->msl)] : nil;
    clib = now_ms() - ct0;
    if (f) {
        MTLComputePipelineDescriptor *cd = [MTLComputePipelineDescriptor new];
        cd.computeFunction = f;
        if (pipeline_archives()) {
            cd.binaryArchives = pipeline_archives();
            double t0 = now_ms();
            cp->pipe = [dev newComputePipelineStateWithDescriptor:cd options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:nil];
            cd.binaryArchives = nil;
            @synchronized(cache) { stat_cached_ms += now_ms() - t0; if (cp->pipe) stat_cached++; }
            if (cp->pipe) { __atomic_fetch_add(&g_archive_hits, 1, __ATOMIC_RELAXED); chit = true; }
        }
        if (!cp->pipe) {
            double t0 = now_ms();
            cp->pipe = [dev newComputePipelineStateWithDescriptor:cd options:MTLPipelineOptionNone reflection:nil error:&err];
            built += now_ms() - t0;
            if (cp->pipe) pipeline_cache_add_compute(cd);
        }
    }
    if (built > 0) { @synchronized(cache) { stat_compile_ms += built; } __atomic_fetch_add(&stat_compiles, 1, __ATOMIC_RELAXED); }
    if (!cp->pipe) {
        LOG("[compute] compile failed: %s\n%s\n", err.localizedDescription.UTF8String, cp->si->msl);
        return BUILD_FAILED;
    }
    LOG("[compute] program %llx: local size %ux%ux%u, %d cbufs mask %#x (MSL %zu bytes: built in %.0f ms, library %.0f, pipeline %s)\n",
        cp->code, cp->local[0], cp->local[1], cp->local[2], 0, cp->si->cbuf_mask, strlen(cp->si->msl), now_ms() - ct0,
        clib, chit ? "from the cache" : "compiled");
    return BUILD_READY;
}

static CsProg *compute_program(NvnProgram *p) {
    NvnShaderStage *ss = &p->stages[5];
    if (!ss->code_addr) return NULL;
    NSValue *key = [NSValue valueWithPointer:(void *)(uintptr_t)ss->code_addr];
    NSMutableDictionary *progs = g_cs_progs;
    if (!progs) progs = g_cs_progs = [NSMutableDictionary new];
    NSValue *v = progs[key];
    if (v) { CsProg *cp = v.pointerValue; return atomic_load(&cp->state) == BUILD_FAILED ? NULL : cp; }
    CsProg *cp = calloc(1, sizeof *cp);
    progs[key] = [NSValue valueWithPointer:cp];
    g_cs_creates++;
    cp->code = ss->code_addr;
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    ShaderKey k = {0};
    cp->si = maxwell_translate((const uint8_t *)(uintptr_t)ss->code_addr, ss->const_offset ? ss->const_offset : 0x10000, STAGE_COMPUTE, &k);
    if (!cp->si->msl) { LOG("[compute] translate failed: %s (code %llx)\n", cp->si->error, ss->code_addr); atomic_store(&cp->state, BUILD_FAILED); return NULL; }
    for (int i = 0; i < 3; i++) {
        cp->local[i] = ss->control ? *(const uint32_t *)(ss->control + 0x748 + 4 * i) : 1;
        if (!cp->local[i]) cp->local[i] = 1;
    }
    shader_cache_note1('c', cp->si->msl);
    pipeline_cache_open();
    if (shader_mode() == SHADERS_PREFETCH) {  // (built off the thread; dispatch_compute waits for it)
        static dispatch_queue_t q;
        if (!q) q = dispatch_queue_create("larecomp.compute", DISPATCH_QUEUE_CONCURRENT);
        build_begin();
        dispatch_async(q, ^{ build_finish(&cp->state, build_compute(cp)); });
        return cp;
    }
    atomic_store(&cp->state, build_compute(cp));
    return atomic_load(&cp->state) == BUILD_FAILED ? NULL : cp;
}

static void dispatch_compute(uint32_t gx, uint32_t gy, uint32_t gz) {
    snap_reason = "compute"; snapshots_invalidate(NULL);
    if (!st.program || !gx || !gy || !gz) return;
    uint64_t made = g_cs_creates;
    CsProg *cp = compute_program(st.program);
    if (shader_mode() == SHADERS_PREFETCH) g_prefetch.unforeseen += g_cs_creates - made;
    if (!cp || build_wait(&cp->state, "compute") != BUILD_READY) return;
    static int trace = -1;
    if (trace < 0) trace = getenv("LARECOMP_TRACE_CS") != NULL;
#define CST(x) do { if (trace) { fprintf(stderr, "[cs] %s\n", x); fflush(stderr); } } while (0)
    CST("end_pass");
    end_pass();
    CST("encoder");
    id<MTLComputeCommandEncoder> ce = [cb() computeCommandEncoder];
    CST("pipeline");
    [ce setComputePipelineState:cp->pipe];
    ShaderInfo *si = cp->si;
    for (int k = 0; k < 18; k++) {
        if (!(si->cbuf_mask & (1u << k))) continue;
        uint64_t off;
        if (k == 0) {
            uint32_t drv[0x500 / 4] = {0};  // tables end at 0x310 + 16 * 16
            drv[0x200 / 4] = gx; drv[0x204 / 4] = gy; drv[0x208 / 4] = gz;
            for (int i = 0; i < 16; i++) {
                uint32_t *u = &drv[(0x210 + 16 * i) / 4];
                if (i < 18) { u[0] = (uint32_t)st.ubuf[5][i].addr; u[1] = (uint32_t)(st.ubuf[5][i].addr >> 32); u[2] = (uint32_t)st.ubuf[5][i].size; }
                uint32_t *e = &drv[(0x310 + 16 * i) / 4];
                e[0] = (uint32_t)st.sbuf[5][i].addr; e[1] = (uint32_t)(st.sbuf[5][i].addr >> 32); e[2] = (uint32_t)st.sbuf[5][i].size;
                gpu_read_mark(st.sbuf[5][i].addr, st.sbuf[5][i].size);  // (compute reads both in place)
                if (i < 18) gpu_read_mark(st.ubuf[5][i].addr, st.ubuf[5][i].size);
            }
            off = ring_alloc(drv, sizeof drv);
        } else if (k == 1) {
            NvnShaderStage *ss = &st.program->stages[5];
            uint64_t size = ss->const_size ? ss->const_size : 16;
            if (size < si->cbuf_size[1]) size = si->cbuf_size[1];
            off = ring_alloc((void *)(uintptr_t)(ss->code_addr + ss->const_offset), size);
        } else {
            int idx = k - cbuf_base;
            Range r = (idx >= 0 && idx < 18) ? st.ubuf[5][idx] : (Range){0, 0};
            uint64_t size = si->cbuf_size[k] ? si->cbuf_size[k] : 256;
            if (r.addr) {
                if ((si->cbuf_dynamic & (1u << k)) && r.size) size = r.size > 0x10000 ? 0x10000 : r.size;
                if (size > r.size && r.size) size = r.size;
                if (size < 16) size = 16;
                off = ring_alloc((void *)(uintptr_t)r.addr, size);
            } else {
                static const uint8_t zeros[0x10000];
                off = ring_alloc(zeros, size);
            }
        }
        [ce setBuffer:ring offset:off atIndex:k];
    }
    CST("gmem");
    if (si->uses_gmem) [ce setBuffer:g_guest_buffer offset:0 atIndex:GMEM_BUFFER_INDEX];
    CST("textures");
    uint32_t swz[64][4];
    for (int i = 0; i < si->ntex && i < 64; i++) renderer_swizzle_row(NULL, NULL, false, swz[i]);
    for (int i = 0; i < si->ntex; i++) {
        ShaderTexture *st_ = &si->tex[i];
        int slot = ((int)st_->handle_offset - 0x20) / 8;
        uint64_t h = (slot >= 0 && slot < 32) ? st.tex[5][slot] : 0;
        id<MTLTexture> tex = nil;
        id<MTLSamplerState> smp = nil;
        uint32_t tic = h & 0xFFFFF, tsc = (uint32_t)(h >> 20) & 0xFFF;
        tex_sc[0][i][0] = tex_sc[0][i][1] = 1;
        if (h && st.tpool && tic < (uint32_t)st.tpool->count) {
            NvnTexEntry *e = &((NvnTexEntry *)st.tpool->entries)[tic];
            if (e->tex) tex = sampled_texture(e->tex, &e->view, e->has_view, st_->type);
            if (i < 64) renderer_swizzle_row(e->tex, &e->view, e->has_view, swz[i]);
            if (e->tex && i < 64) renderer_texture_scale(e->tex, &tex_sc[0][i][0], &tex_sc[0][i][1]);
            for (int k = 0; k < nfeedback; k++)
                if (e->tex == feedback_tex[k]) {
                    id<MTLTexture> fc = feedback_copy[k];
                    MTLTextureType want = mtl_type_for(st_->type);
                    if (fc.textureType != want && want == MTLTextureType2DArray)
                        fc = [fc newTextureViewWithPixelFormat:fc.pixelFormat textureType:MTLTextureType2DArray levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1)];
                    tex = fc;
                }
        }
        if (h && st.spool && tsc < (uint32_t)st.spool->count) smp = sampler_for(&((NvnSamplerDesc *)st.spool->entries)[tsc]);
        [ce setTexture:tex ? tex : dummy_texture(st_->type) atIndex:i];
        [ce setSamplerState:smp ? smp : default_sampler() atIndex:i];
    }
    if (si->uses_rscale) {
        float rs[65][4] = {{1, 1, 0, 0}};
        for (int i = 0; i < si->ntex && i < 64; i++) { rs[1 + i][0] = tex_sc[0][i][0]; rs[1 + i][1] = tex_sc[0][i][1]; }
        [ce setBytes:rs length:(NSUInteger)(1 + si->ntex) * 16 atIndex:RSCALE_BUFFER_INDEX];
    }
    if (si->ntex) [ce setBytes:swz length:(NSUInteger)si->ntex * 16 atIndex:TSWZ_BUFFER_INDEX];
    CST("dispatch");
    extern bool g_debug_frame;
    if (g_debug_frame) {
        LOG("[dispatch] cs=%llx groups %ux%ux%u ssbo:", st.program->stages[5].code_addr, gx, gy, gz);
        for (int i = 0; i < 16; i++) if (st.sbuf[5][i].addr) LOG(" %d(%llx,%llu)", i, st.sbuf[5][i].addr, st.sbuf[5][i].size);
        LOG(" ubo:");
        for (int i = 0; i < 18; i++) if (st.ubuf[5][i].addr) LOG(" %d(%llx,%llu)", i, st.ubuf[5][i].addr, st.ubuf[5][i].size);
        LOG(" gmem %d\n", si->uses_gmem);
    }
    [ce dispatchThreadgroups:MTLSizeMake(gx, gy, gz) threadsPerThreadgroup:MTLSizeMake(cp->local[0], cp->local[1], cp->local[2])];
    [ce endEncoding];
    CST("done");
    stat_dispatches++;
    // What the dispatch writes (storage buffers in guest memory) is read by later draws as uniform buffers, which are
    // snapshotted from guest memory when they are encoded - before the GPU has run the dispatch. Finishing the GPU
    // work here puts the results in guest memory first (SMM2 computes its scene lighting this way: without it the
    // lights were zero and every frame black). RECOMP_NO_COMPUTE_FLUSH=1: off.
    static int no_flush = -1;
    if (no_flush < 0) no_flush = getenv("RECOMP_NO_COMPUTE_FLUSH") != NULL;
    if (!no_flush && si->uses_gmem) renderer_flush();
}

static MTLPrimitiveType prim_type(uint32_t mode, bool *ok) {
    *ok = true;
    switch (mode) {
    case 0: return MTLPrimitiveTypePoint;
    case 1: return MTLPrimitiveTypeLine;
    case 3: return MTLPrimitiveTypeLineStrip;
    case 4: return MTLPrimitiveTypeTriangle;
    case 5: return MTLPrimitiveTypeTriangleStrip;
    default: *ok = false; return MTLPrimitiveTypeTriangle;
    }
}

// Expand quads/fans/polygons into a triangle index list (32-bit) referencing the original vertices.
static id<MTLBuffer> expand_indices(uint32_t mode, uint32_t count, const void *indices, int itype, uint32_t first,
                                    uint32_t *out_count) {
    uint32_t tris = mode == 7 ? count / 4 * 2 : count >= 3 ? count - 2 : 0;
    uint32_t *out = malloc(tris * 3 * 4 + 4);
    uint32_t n = 0;
#define IDX(i) (indices ? (itype == 0 ? ((const uint8_t *)indices)[i] : itype == 1 ? ((const uint16_t *)indices)[i] : ((const uint32_t *)indices)[i]) : first + (i))
    if (mode == 7) {  // quads
        for (uint32_t q = 0; q + 3 < count; q += 4) {
            out[n++] = IDX(q); out[n++] = IDX(q + 1); out[n++] = IDX(q + 2);
            out[n++] = IDX(q); out[n++] = IDX(q + 2); out[n++] = IDX(q + 3);
        }
    } else {  // fan / polygon
        for (uint32_t i = 1; i + 1 < count; i++) { out[n++] = IDX(0); out[n++] = IDX(i); out[n++] = IDX(i + 1); }
    }
#undef IDX
    *out_count = n;
    id<MTLBuffer> b = [dev newBufferWithBytes:out length:n * 4 + 4 options:MTLResourceStorageModeShared];
    free(out);
    return b;
}

static void debug_draw(Prog *pr, CmdRec *r, bool indexed) {
    static uint64_t dump_fs = 1;
    if (dump_fs == 1) { const char *e = getenv("LARECOMP_DUMP_UBO"); dump_fs = e ? strtoull(e, NULL, 16) : 0; }
    if (dump_fs && st.program && st.program->stages[1].code_addr == dump_fs) {
        for (int i = 0; i < 18; i++) {
            if (!st.ubuf[1][i].addr) continue;
            const float *f = (const float *)(uintptr_t)st.ubuf[1][i].addr;
            LOG("[ubo] f%d (%llu):", i, st.ubuf[1][i].size);
            for (uint64_t k = 0; k < st.ubuf[1][i].size / 4 && k < 1200; k++) LOG(" %g", f[k]);
            LOG("\n");
        }
    }
    static int dump_vs = -1;  // LARECOMP_DUMP_VSUBO: every vertex-stage UBO (first 48 floats) of debug-frame draws
    if (dump_vs < 0) dump_vs = getenv("LARECOMP_DUMP_VSUBO") != NULL;
    for (int i = 0; dump_vs && i < 18; i++) {
        if (!st.ubuf[0][i].addr) continue;
        const float *f = (const float *)(uintptr_t)st.ubuf[0][i].addr;
        LOG("[vsubo] v%d (%llu):", i, st.ubuf[0][i].size);
        for (uint64_t k = 0; k < st.ubuf[0][i].size / 4 && k < 48; k++) LOG(" %g", f[k]);
        LOG("\n");
    }
    if (st.sbuf[0][0].addr && st.sbuf[0][0].size >= 64 && st.sbuf[0][0].size <= 8192) {
        int recw = (st.sbuf[0][0].size % 96 == 0 && st.sbuf[0][0].size % 64 != 0) ? 24 : 16;
        const float *f = (const float *)(uintptr_t)st.sbuf[0][0].addr;
        const uint32_t *u = (const uint32_t *)f;
        for (int g = 0; g < (int)(st.sbuf[0][0].size / (recw * 4)) && g < 6; g++) {
            LOG("[ssbo] rec%d:", g);
            for (int k = 0; k < recw; k++) LOG(" %g(%08x)", f[g * recw + k], u[g * recw + k]);
            LOG("\n");
        }
    }
    LOG("[draw] %s inst %llu", indexed ? "e" : "a", indexed ? r->x[6] : r->x[4]);
    LOG(" %s mode %llu count %llu prog vs=%llx fs=%llx | vs cb %#x fs cb %#x | ubufs:",
        indexed ? "elems" : "arrays", r->x[0], r->x[2], st.program->stages[0].code_addr, st.program->stages[1].code_addr,
        pr->vi->cbuf_mask, pr->fi->cbuf_mask);
    for (int s = 0; s < 2; s++)
        for (int i = 0; i < 18; i++)
            if (st.ubuf[s][i].addr) LOG(" %c%d(%llx,%llu)", s ? 'f' : 'v', i, st.ubuf[s][i].addr, st.ubuf[s][i].size);
    LOG(" | fubo0:");
    for (int i = 0; i < 6; i++)
        if (st.ubuf[1][i].addr) { const float *f = (const float *)(uintptr_t)st.ubuf[1][i].addr; LOG(" f%d=[%g %g %g %g]", i, f[0], f[1], f[2], f[3]); }
    LOG(" | ssbo:");
    for (int sg = 0; sg < 2; sg++)
        for (int i = 0; i < 16; i++)
            if (st.sbuf[sg][i].addr) LOG(" %c%d(%llx,%llu)", sg ? 'f' : 'v', i, st.sbuf[sg][i].addr, st.sbuf[sg][i].size);
    LOG(" | tex:");
    for (int i = 0; i < pr->fi->ntex; i++) LOG(" off%u", pr->fi->tex[i].handle_offset);
    LOG(" bound:");
    for (int i = 0; i < 32; i++) {
        if (!st.tex[1][i]) continue;
        uint32_t tic = st.tex[1][i] & 0xFFFFF;
        NvnTexture *t = st.tpool && tic < (uint32_t)st.tpool->count ? ((NvnTexEntry *)st.tpool->entries)[tic].tex : NULL;
        if (t) LOG(" %d=[%ux%ux%u t%u f%#x fl%#x %s]", i, t->width, t->height, t->depth, t->target, t->format, t->flags, t->label);
        else LOG(" %d=%llx", i, st.tex[1][i]);
        uint32_t tsc = (uint32_t)(st.tex[1][i] >> 20) & 0xFFF;
        if (i == 0 && st.spool && tsc < (uint32_t)st.spool->count) {  // (slot 0's sampler)
            const NvnSamplerDesc *sd = &((NvnSamplerDesc *)st.spool->entries)[tsc];
            LOG("{smp %u: min %u mag %u wrap %u,%u,%u cmp %u/%u red %u lod %g..%g bias %g}", tsc, sd->min_filter, sd->mag_filter,
                sd->wrap[0], sd->wrap[1], sd->wrap[2], sd->compare_mode, sd->compare_func, sd->reduction, sd->lod_min, sd->lod_max,
                sd->lod_bias);
        }
    }
    LOG(" | rt %d", st.rt.ncolor);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) LOG(" %ux%u/f%#x", st.rt.color[i]->width, st.rt.color[i]->height, st.rt.color[i]->format);
    if (st.rt.depth) LOG(" d%#x", st.rt.depth->format);
    LOG(" | vp %.0f %.0f %.0f %.0f | attrs %#x streams:", st.viewport[0][0], st.viewport[0][1], st.viewport[0][2], st.viewport[0][3], pr->vi->attr_mask);
    for (int i = 0; i < st.nattrib; i++) LOG(" a%d(f%#x s%u o%u)", i, st.attrib[i] & 0xFF, (st.attrib[i] >> 8) & 0x1F, st.attrib[i] >> 13);
    for (int i = 0; i < 12; i++) if (st.vbuf[i].addr) LOG(" vb%d(%llx,%llu,stride %u div %u)", i, st.vbuf[i].addr, st.vbuf[i].size, st.stream[i][0], st.stream[i][1]);
    if (st.vbuf[0].addr >= GUEST_LO && st.vbuf[0].addr < GUEST_HI) {  // (the first vertices, as floats)
        const float *v = (const float *)(uintptr_t)st.vbuf[0].addr;
        LOG(" vb0[0..15]:");
        for (int i = 0; i < 16 && (uint64_t)i * 4 < st.vbuf[0].size; i++) LOG(" %g", v[i]);
    }
    if (st.ubuf[0][0].addr && st.ubuf[0][0].size == 560) {
        const float *f = (const float *)(uintptr_t)st.ubuf[0][0].addr;
        LOG("\n  vs ubo0:");
        for (int i = 0; i < 36; i++) LOG("%s[%d] %g %g %g %g", i % 2 ? " " : "\n   ", i, f[i*4], f[i*4+1], f[i*4+2], f[i*4+3]);
    }
    LOG(" blend %#x ds %02x%02x%02x poly %02x%02x scissor %d,%d %dx%d chmask %08x stencil %02x%02x%02x%02x%02x ref %u/%u wm %x vm %x bf %d,%d,%d,%d,%d,%d\n", st.color_state[0], st.ds[0], st.ds[1], st.ds[2], st.poly[0], st.poly[1],
        st.scissor[0][0], st.scissor[0][1], st.scissor[0][2], st.scissor[0][3], st.channel_mask, st.ds[3], st.ds[4], st.ds[5], st.ds[6],
        st.ds[7], st.stencil_ref[0], st.stencil_ref[1], st.stencil_mask[0], st.stencil_vmask[0], st.blend[0][1], st.blend[0][2],
        st.blend[0][3], st.blend[0][4], st.blend[0][5], st.blend[0][6]);
}


// ---- pipeline cache keyed by a hash of the raw state that shapes the pipeline (avoids building a
// descriptor and a string key per draw)
// Pipelines are keyed by their shader functions' addresses: a reboot frees the old functions, so keys carry the
// boot generation (a build still in flight can't land on a new function at a reused address).
static unsigned g_boot_gen;
static NSMutableDictionary *g_ready_pipes;

static uint64_t hmix(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}
#define PIPE_TAB 8192
static struct { uint64_t key; void *pipe; } pipe_tab[PIPE_TAB];

static uint64_t pipeline_state_hash(const ExecState *s, Prog *pr) {
    uint64_t h = 0xcbf29ce484222325ull;
    h = hmix(h, &pr, sizeof pr);
    h = hmix(h, &s->rt.ncolor, sizeof s->rt.ncolor);
    for (int i = 0; i < s->rt.ncolor && i < 8; i++) {
        NvnTexture *t = s->rt.color[i];
        uint32_t v[4] = {t ? t->format : 0, t ? t->samples : 0, s->rt.has_cview[i], s->rt.has_cview[i] ? s->rt.cview[i].format | (s->rt.cview[i].setmask << 16) : 0};
        h = hmix(h, v, sizeof v);
        h = hmix(h, s->blend[i], 8);
    }
    uint32_t dfmt = s->rt.depth ? s->rt.depth->format : 0;
    h = hmix(h, &dfmt, 4);
    h = hmix(h, &s->nattrib, sizeof s->nattrib);
    h = hmix(h, s->attrib, sizeof s->attrib[0] * (size_t)(s->nattrib < 32 ? s->nattrib : 32));
    h = hmix(h, &s->nstream, sizeof s->nstream);
    h = hmix(h, s->stream, sizeof s->stream[0] * (size_t)(s->nstream < 16 ? s->nstream : 16));
    h = hmix(h, &s->channel_mask, sizeof s->channel_mask);
    h = hmix(h, &s->color_state[0], sizeof s->color_state[0]);
    h = hmix(h, &s->ms[0xC], 1);
    return h ? h : 1;
}

// The program keys a draw in state s compiles its shaders with (the mirror flip is the draw's own)
static void program_keys(const ExecState *s, ShaderKey *vkey, ShaderKey *fkey) {
    memset(vkey, 0, sizeof *vkey);
    memset(fkey, 0, sizeof *fkey);
    vkey->flip_y = fkey->flip_y = nvn_window_origin() == 0;
    vkey->depth_minus_one = nvn_depth_mode() == 0;
    for (int i = 0; i < s->nattrib; i++) vkey->attr_kind[i] = (uint8_t)attr_kind(s->attrib[i] & 0xFF);
    for (int i = 0; i < s->rt.ncolor; i++) if (s->rt.color[i]) fkey->rt_kind[i] = (uint8_t)rt_kind(s->rt.color[i]->format);
}

// A draw's pipeline in state s with pass layout pl: everything but the shader functions (set from pr by the caller),
// and its key in the pipeline cache (*key).
static MTLRenderPipelineDescriptor *render_pipe_desc(const ExecState *s, const PassLayout *pl, Prog *pr, NSString **key) {
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    layout_apply(pd, pl);
    uint32_t used_streams = 0;
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    for (int i = 0; i < 32; i++) {
        if (!(pr->vi->attr_mask & (1u << i))) continue;
        uint32_t a = i < s->nattrib ? s->attrib[i] : 0x2E;
        uint32_t fmt = a & 0xFF, stream = (a >> 8) & 0x1F, off = a >> 13;
        if (i >= s->nattrib || fmt == 0) {
            // unbound attribute: read zeros from a constant stream
            vd.attributes[i].format = MTLVertexFormatFloat4;
            vd.attributes[i].offset = 0;
            vd.attributes[i].bufferIndex = 30;
            vd.layouts[30].stepFunction = MTLVertexStepFunctionConstant;
            vd.layouts[30].stepRate = 0;
            vd.layouts[30].stride = 16;
            used_streams |= 1u << 31;
            continue;
        }
        vd.attributes[i].format = vertex_format(fmt);
        vd.attributes[i].offset = off;
        vd.attributes[i].bufferIndex = 18 + stream;
        used_streams |= 1u << stream;
        (void)is_int_format;
    }
    for (int k = 0; k < 12; k++) {
        if (!(used_streams & (1u << k))) continue;
        uint32_t stride = k < s->nstream ? s->stream[k][0] : 0, div = k < s->nstream ? s->stream[k][1] : 0;
        if (stride == 0) { vd.layouts[18 + k].stepFunction = MTLVertexStepFunctionConstant; vd.layouts[18 + k].stepRate = 0; vd.layouts[18 + k].stride = 16; }
        else {
            vd.layouts[18 + k].stride = (stride + 3) & ~3u;
            if (div || pr->gi) { vd.layouts[18 + k].stepFunction = MTLVertexStepFunctionPerInstance; vd.layouts[18 + k].stepRate = div ? div : 1; }
        }
    }
    pd.vertexDescriptor = vd;
    for (int i = 0; i < s->rt.ncolor && i < 8; i++) {
        if (!s->rt.color[i] || (pl->dropped >> i & 1)) continue;
        MTLRenderPipelineColorAttachmentDescriptor *ca = pd.colorAttachments[i];
        uint32_t m = (s->channel_mask >> (i * 4)) & 0xF;
        ca.writeMask = (m & 1 ? MTLColorWriteMaskRed : 0) | (m & 2 ? MTLColorWriteMaskGreen : 0) |
                       (m & 4 ? MTLColorWriteMaskBlue : 0) | (m & 8 ? MTLColorWriteMaskAlpha : 0);
        bool blend_on = (s->color_state[0] >> i) & 1;
        if (blend_on && !rt_kind(s->rt.color[i]->format)) {
            const uint8_t *b = s->blend[i];
            ca.blendingEnabled = YES;
            ca.sourceRGBBlendFactor = blend_factor(b[1]);
            ca.destinationRGBBlendFactor = blend_factor(b[2]);
            ca.sourceAlphaBlendFactor = blend_factor(b[3]);
            ca.destinationAlphaBlendFactor = blend_factor(b[4]);
            ca.rgbBlendOperation = blend_op(b[5]);
            ca.alphaBlendOperation = blend_op(b[6]);
        }
    }
    pd.alphaToCoverageEnabled = s->ms[0xC] != 0;
    NSMutableString *pk = [NSMutableString stringWithFormat:@"%u %p %lu %lu", g_boot_gen, (__bridge void *)pr,
                           (unsigned long)pd.depthAttachmentPixelFormat, (unsigned long)pd.stencilAttachmentPixelFormat];
    for (int i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *ca = pd.colorAttachments[i];
        [pk appendFormat:@" %lu/%lu/%d/%lu/%lu/%lu/%lu/%lu/%lu", (unsigned long)ca.pixelFormat, (unsigned long)ca.writeMask,
                         ca.blendingEnabled, (unsigned long)ca.sourceRGBBlendFactor, (unsigned long)ca.destinationRGBBlendFactor,
                         (unsigned long)ca.sourceAlphaBlendFactor, (unsigned long)ca.destinationAlphaBlendFactor,
                         (unsigned long)ca.rgbBlendOperation, (unsigned long)ca.alphaBlendOperation];
    }
    for (int i = 0; i < 32; i++)
        if (pr->vi->attr_mask & (1u << i))
            [pk appendFormat:@" a%d:%lu/%lu/%lu", i, (unsigned long)vd.attributes[i].format, (unsigned long)vd.attributes[i].offset, (unsigned long)vd.attributes[i].bufferIndex];
    for (int k = 18; k < 31; k++)
        [pk appendFormat:@" l%lu/%lu/%lu", (unsigned long)vd.layouts[k].stride, (unsigned long)vd.layouts[k].stepFunction, (unsigned long)vd.layouts[k].stepRate];
    [pk appendFormat:@" s%lu a%d", (unsigned long)pd.rasterSampleCount, pd.alphaToCoverageEnabled];
    *key = pk;
    return pd;
}

// A pipeline built off the game thread (prefetch mode), held in the pending table (buildsync.h) until a draw takes it
@interface MPipe : NSObject {
@public
    _Atomic int state;  // BUILD_*
    id<MTLRenderPipelineState> pipe;
}
@end
@implementation MPipe
@end
static void mpipe_release(void *p) { CFRelease(p); }

// The pipeline for (pd, key) in the pending table, its build started if it's new (pr must be ready: its functions
// go into the pipeline). The archive lookup is part of the build, so a draw doesn't wait for that either.
// The pending table's key for a pipeline: its whole cache key (attachment formats, program, boot), so a pipeline the
// scan built from a wrong prediction is never a draw's
static uint64_t pipe_key_hash(NSString *key) {
    uint64_t h = fnv64(key.UTF8String);
    return h ? h : 1;
}

// (Also called from a program's compile completion, for the scan's requests: pipeline_cache_open ran before.)
static MPipe *pipe_request(Prog *pr, MTLRenderPipelineDescriptor *pd, NSString *key, uint64_t khash, bool *created) {
    MPipe *fresh = [MPipe new];
    void *fp = (__bridge_retained void *)fresh;
    void *got = pend_get_or_put(khash, fp);
    if (created) *created = got == fp;
    if (got != fp) { CFRelease(fp); return created ? (__bridge MPipe *)got : nil; }  // (nil: the caller doesn't hold it)
    pipeline_cache_open();
    static dispatch_queue_t q;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ q = dispatch_queue_create("larecomp.pipelines", DISPATCH_QUEUE_CONCURRENT); });
    MTLRenderPipelineDescriptor *pdc = [pd copy];
    pdc.vertexFunction = pr->vs;
    pdc.fragmentFunction = pr->fs;
    NSString *k = [key copy];
    build_begin();
    dispatch_async(q, ^{
        double t0 = now_ms();
        id<MTLRenderPipelineState> ps = pipeline_cache_find_render(pdc);
        if (!ps) {
            NSError *e = nil;
            ps = [dev newRenderPipelineStateWithDescriptor:pdc error:&e];
            if (ps) pipeline_cache_add_render(pdc);
            else { static int n; if (n++ < 10) LOG("[metal] pipeline failed: %s (%s)\n", e.localizedDescription.UTF8String, k.UTF8String); }
            @synchronized(cache) { stat_compile_ms += now_ms() - t0; }
            __atomic_fetch_add(&stat_compiles, 1, __ATOMIC_RELAXED);
        }
        fresh->pipe = ps;
        build_finish(&fresh->state, ps ? BUILD_READY : BUILD_FAILED);
    });
    return fresh;
}

static void draw(CmdRec *r, bool indexed) {
    if (!st.program) { stat_skipped++; return; }
    static bool logged_modes;
    if (!logged_modes) {
        logged_modes = true;
        const char *cb = getenv("LARECOMP_CBUF_BASE"), *tb = getenv("LARECOMP_TEX_BASE");
        if (cb) cbuf_base = atoi(cb);
        if (tb) tex_base = atoi(tb);
    }
    ShaderKey vkey, fkey;
    program_keys(&st, &vkey, &fkey);
    // Culling both faces (NVN Face FRONT_AND_BACK, polygon state 3) draws no polygons; Metal has no such mode. SMM2's
    // 3D World background keeps a clear pipe's yellow stand-in mesh switched off this way.
    if (st.poly[0] == 3 && (uint32_t)r->x[0] >= 4) { stat_skipped++; return; }
    uint64_t made = g_prog_creates;
    Prog *pr = get_program(st.program, &vkey, &fkey);
    if (shader_mode() == SHADERS_PREFETCH) g_prefetch.unforeseen += g_prog_creates - made;
    if (!prog_usable(pr)) { stat_skipped++; return; }
    {   // mirror mode (runtime/hle/mirror.c): the scene's composite into the window image is drawn mirrored - the
        // unblended full-screen draw sampling an image the window's size (the window images also serve as scratch
        // targets earlier in the frame: those draws sample smaller images; the HUD after it blends)
        extern bool mirror_draw(bool composite);
        bool composite = false;
        if (st.rt.ncolor && st.rt.color[0] && renderer_texture_presented(st.rt.color[0]) && !(st.color_state[0] & 1) &&
            r->x[2] <= 6 && st.viewport[0][0] == 0 && st.viewport[0][1] == 0 &&
            st.viewport[0][2] >= st.rt.color[0]->width && st.viewport[0][3] >= st.rt.color[0]->height && pr->fi->ntex) {
            NvnTexture *t0 = shader_texture_object(1, &pr->fi->tex[0]);
            composite = t0 && t0->width == st.rt.color[0]->width && t0->height == st.rt.color[0]->height;
        }
        if (mirror_draw(composite)) {  // (the vertex flip mirrors a composite drawn with uvs, the fragment one a
            vkey.flip_x = fkey.flip_x = true;  // composite reading pixel positions; get_program keys on vkey)
            pr = get_program(st.program, &vkey, &fkey);
            if (!prog_usable(pr)) { stat_skipped++; return; }
        }
    }
    {   // Interiors/dungeons draw a 16:9 backdrop quad over the still-rendered overworld: frames that
        // draw it are presented cropped to 16:9 (see backend_present).
        static uint64_t last_fs, last_hash;
        uint64_t fs = st.program->stages[1].code_addr;
        if (fs != last_fs) {
            last_fs = fs;
            last_hash = fs ? hmix(1469598103934665603ull, (const void *)(uintptr_t)(fs + 0x30), 0x200) : 0;
        }
        static uint64_t log_hash = 1;
        if (log_hash == 1) log_hash = getenv("LARECOMP_LOG_FSHASH") ? 0 : 2;
        if (!log_hash && g_debug_frame) LOG("[fshash] fs=%llx hash %016llx\n", fs, last_hash);
        {   // LARECOMP_SKIP_FSHASH=h1,h2,...: drop draws using these fragment shaders (find which draw makes what)
            static uint64_t skip[16];
            static int nskip = -1;
            if (nskip < 0) {
                nskip = 0;
                for (const char *p = getenv("LARECOMP_SKIP_FSHASH"); p && *p && nskip < 16;) {
                    skip[nskip++] = strtoull(p, (char **)&p, 16);
                    while (*p == ',') p++;
                }
            }
            for (int i = 0; i < nskip; i++) if (last_hash == skip[i]) { stat_skipped++; return; }
            extern bool pot_level_active(void);
            if (last_hash == 0xa2177dbb72197bd9ull && pot_level_active()) { stat_skipped++; return; }  // item twinkle
        }
        g_cur_fs_hash = last_hash;
        extern void widescreen_backdrop_seen(uint64_t hash);
        if (last_hash) widescreen_backdrop_seen(last_hash);
        extern void widescreen_scene_ubo(const float *u, uint64_t size);  // (the camera's view-projection)
        if (st.ubuf[0][5].addr) widescreen_scene_ubo((const float *)(uintptr_t)st.ubuf[0][5].addr, st.ubuf[0][5].size);
        if (last_hash == 0x71fe31bbf03c3515ull && getenv("LARECOMP_LOG_INDOOR")) {
            static uint64_t lf;
            extern uint64_t renderer_present_count(void);
            if (renderer_present_count() != lf) {
                lf = renderer_present_count();
                LOG("[backdrop] frame %llu count %llu rt %ux%u vp %.0f %.0f %.0f %.0f scissor %d %d %d %d\n", (unsigned long long)lf, r->x[2],
                    st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->width : 0, st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->height : 0,
                    st.viewport[0][0], st.viewport[0][1], st.viewport[0][2], st.viewport[0][3], st.scissor[0][0], st.scissor[0][1],
                    st.scissor[0][2], st.scissor[0][3]);
            }
        }
    }
    static int log_rt_w = -1, log_rt_h;
    if (log_rt_w < 0) { const char *e = getenv("LARECOMP_LOG_RT"); log_rt_w = 0; if (e) sscanf(e, "%dx%d", &log_rt_w, &log_rt_h); }
    static uint64_t log_rt_addr = 1;
    if (log_rt_addr == 1) { const char *e = getenv("LARECOMP_LOG_RT_ADDR"); log_rt_addr = e ? strtoull(e, NULL, 16) : 0; }
    bool addr_hit = false;
    for (int i = 0; log_rt_addr && i < st.rt.ncolor; i++)
        if (st.rt.color[i] && st.rt.color[i]->addr <= log_rt_addr + 0x40000 && st.rt.color[i]->addr + 0x40000 >= log_rt_addr) addr_hit = true;
    if (addr_hit) LOG("[rtaddr] rt0 addr %llx\n", st.rt.color[0] ? st.rt.color[0]->addr : 0);
    if (g_debug_frame || addr_hit || (log_rt_w && st.rt.ncolor && st.rt.color[0] && (int)st.rt.color[0]->width == log_rt_w &&
                          (int)st.rt.color[0]->height == log_rt_h))
        debug_draw(pr, r, indexed);
    resolve_feedback(pr);
    // this draw's writes make snapshots of its attachments stale
    snap_reason = "draw-depthwrite";
    if (st.rt.depth && st.ds[0] && st.ds[1]) snapshots_invalidate(st.rt.depth);  // stencil-only writes: shaders sample depth
    snap_reason = "draw-colorwrite";
    for (int i = 0; i < st.rt.ncolor; i++)
        if (st.rt.color[i] && ((st.channel_mask >> (i * 4)) & 0xF)) snapshots_invalidate(st.rt.color[i]);
    begin_pass();

    // vertex streams this draw reads (also computed inside the pipeline build on a cache miss)
    uint32_t used_streams = 0;
    for (int i = 0; i < 32; i++) {
        if (!(pr->vi->attr_mask & (1u << i))) continue;
        uint32_t a = i < st.nattrib ? st.attrib[i] : 0x2E;
        if (i >= st.nattrib || (a & 0xFF) == 0) used_streams |= 1u << 31;
        else used_streams |= 1u << ((a >> 8) & 0x1F);
    }
    // pipeline
    uint64_t phash = pipeline_state_hash(&st, pr);
    uint32_t pslot = (uint32_t)(phash ^ (phash >> 29)) & (PIPE_TAB - 1);
    while (pipe_tab[pslot].key && pipe_tab[pslot].key != phash) pslot = (pslot + 1) & (PIPE_TAB - 1);
    id<MTLRenderPipelineState> pipe = pipe_tab[pslot].key ? (__bridge id<MTLRenderPipelineState>)pipe_tab[pslot].pipe : nil;
    if (!pipe) {
    PassLayout pl;
    pass_layout_actual(&pl);
    {   // (the prefetch scan's prediction of it: a miss shows here once per pair of formats)
        PassLayout pp;
        pass_layout(&st, &pp);
        if (memcmp(&pl, &pp, sizeof pl)) {
            static int nlog;
            if (nlog++ < 10)
                LOG("[shader] pass_layout predicted %lu/%lu/%lu, the pass has %lu/%lu/%lu (colour 0, depth, samples)\n",
                    (unsigned long)pp.color[0], (unsigned long)pp.depth, (unsigned long)pp.samples, (unsigned long)pl.color[0],
                    (unsigned long)pl.depth, (unsigned long)pl.samples);
        }
    }
    NSString *pk;
    MTLRenderPipelineDescriptor *pd = render_pipe_desc(&st, &pl, pr, &pk);
    pd.vertexFunction = pr->vs;
    pd.fragmentFunction = pr->fs;
    id pobj = cache[pk];
    NSMutableDictionary *ready_pipes = g_ready_pipes;  // async pipeline builds finished on Metal's threads
    static id pending;
    if (!ready_pipes) { ready_pipes = g_ready_pipes = [NSMutableDictionary new]; pending = [NSObject new]; }
    if (pobj == pending) {
        @synchronized(ready_pipes) {
            id done = ready_pipes[pk];
            if (done) { [ready_pipes removeObjectForKey:pk]; cache[pk] = done; pobj = done; }
        }
        if (pobj == pending) { stat_skipped++; return; }
    }
    if (!pobj && shader_mode() == SHADERS_PREFETCH) {  // (built off the thread - maybe started by the scan - and waited for)
        bool created;
        uint64_t khash = pipe_key_hash(pk);
        MPipe *m = pipe_request(pr, pd, pk, khash, &created);
        if (created) g_prefetch.unforeseen++;  // (a build the scan didn't start)
        build_wait(&m->state, "pipeline");
        void *t = pend_take(khash);
        if (t) CFRelease(t);
        pobj = m->pipe ? (id)m->pipe : (id)[NSNull null];
        cache[pk] = pobj;
    }
    if (!pobj) {
        pipeline_cache_open();
        pobj = pipeline_cache_find_render(pd);  // (in the cache: no compile, and no background build either)
        if (pobj) {
            cache[pk] = pobj;
        } else if (shader_mode() == SHADERS_SYNC) {
            NSError *err;
            pobj = TIMED_COMPILE([dev newRenderPipelineStateWithDescriptor:pd error:&err]);
            if (pobj) pipeline_cache_add_render(pd);
            if (!pobj) {
                static int n;
                if (n++ < 10) LOG("[metal] pipeline failed: %s\n", err.localizedDescription.UTF8String);
                pobj = [NSNull null];
            }
            cache[pk] = pobj;
        } else {
            cache[pk] = pending;
            NSString *k = [pk copy];
            double t0 = now_ms();
            MTLRenderPipelineDescriptor *pdc = [pd copy];
            [dev newRenderPipelineStateWithDescriptor:pd completionHandler:^(id<MTLRenderPipelineState> ps, NSError *e) {
                if (!ps) { static int n; if (n++ < 10) LOG("[metal] pipeline failed: %s\n", e.localizedDescription.UTF8String); }
                else pipeline_cache_add_render(pdc);
                @synchronized(ready_pipes) { ready_pipes[k] = ps ? (id)ps : (id)[NSNull null]; }
                @synchronized(cache) { stat_compile_ms += now_ms() - t0; }
                __atomic_fetch_add(&stat_compiles, 1, __ATOMIC_RELAXED);
            }];
            stat_skipped++;
            return;
        }
    }
    if (pobj == [NSNull null]) { stat_skipped++; return; }
    pipe = pobj;
    pipe_tab[pslot].key = phash;
    pipe_tab[pslot].pipe = (__bridge void *)pipe;  // retained by the cache dictionary
    }
    [enc setRenderPipelineState:pipe];

    // depth / stencil
    static uint64_t last_dsk;
    static id<MTLDepthStencilState> last_dss;
    uint64_t dsk = hmix(hmix(hmix(0xcbf29ce484222325ull, st.ds, 8), st.stencil_mask, sizeof st.stencil_mask), st.stencil_vmask, sizeof st.stencil_vmask);
    id<MTLDepthStencilState> dss = dsk == last_dsk ? last_dss : nil;
    NSString *dk = nil;
    if (!dss) {
        dk = [NSString stringWithFormat:@"ds %02x%02x%02x%02x%02x%02x%02x%02x %x %x %x %x", st.ds[0], st.ds[1], st.ds[2], st.ds[3],
                    st.ds[4], st.ds[5], st.ds[6], st.ds[7], st.stencil_mask[0], st.stencil_vmask[0], st.stencil_mask[1], st.stencil_vmask[1]];
        dss = cache[dk];
    }
    if (!dss) {
        MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
        static int nodt = -1;
        if (nodt < 0) nodt = getenv("LARECOMP_NO_DEPTH_TEST") != NULL;
        if (st.ds[0] && !nodt) { dd.depthCompareFunction = compare_fn(st.ds[2]); dd.depthWriteEnabled = st.ds[1] != 0; }
        else { dd.depthCompareFunction = MTLCompareFunctionAlways; dd.depthWriteEnabled = NO; }
        if (st.ds[3]) {
            uint32_t ops = st.ds[5] | st.ds[6] << 8 | st.ds[7] << 16;
            for (int f = 0; f < 2; f++) {
                MTLStencilDescriptor *sd = [MTLStencilDescriptor new];
                sd.stencilCompareFunction = compare_fn(f ? st.ds[4] >> 4 : st.ds[4] & 0xF);
                uint32_t o = f ? ops >> 12 : ops;
                sd.stencilFailureOperation = stencil_op(o & 0xF);
                sd.depthFailureOperation = stencil_op((o >> 4) & 0xF);
                sd.depthStencilPassOperation = stencil_op((o >> 8) & 0xF);
                sd.readMask = st.stencil_vmask[f] & 0xFF;
                sd.writeMask = st.stencil_mask[f] & 0xFF;
                if (f) dd.backFaceStencil = sd; else dd.frontFaceStencil = sd;
            }
        }
        dss = [dev newDepthStencilStateWithDescriptor:dd];
        cache[dk] = dss;
    }
    [enc setDepthStencilState:dss];
    last_dsk = dsk;
    last_dss = dss;
    [enc setStencilFrontReferenceValue:st.stencil_ref[0] backReferenceValue:st.stencil_ref[1]];
    [enc setBlendColorRed:st.blend_color[0] green:st.blend_color[1] blue:st.blend_color[2] alpha:st.blend_color[3]];

    // rasterizer
    uint8_t cull = st.poly[0];
    if (vkey.flip_x) cull = 0;  // (mirror mode's composite: one full-screen quad, either winding)
    [enc setCullMode:cull == 1 ? MTLCullModeFront : cull == 2 ? MTLCullModeBack : MTLCullModeNone];
    bool ccw = st.poly[1] == 1;
    [enc setFrontFacingWinding:(ccw != vkey.flip_y) ? MTLWindingCounterClockwise : MTLWindingClockwise];
    [enc setTriangleFillMode:st.poly[2] == 1 ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
    {   // polygon offset (decals on floors: without it they z-fight). Enables: bit 2 = fill (NVN PolygonOffsetEnable)
        static float last[3] = {0, 0, 0};
        float f[3] = {0, 0, 0};
        static int off = -1;  // LARECOMP_NO_DEPTH_BIAS: ignore the game's polygon offset (A/B tests)
        if (off < 0) off = getenv("LARECOMP_NO_DEPTH_BIAS") != NULL;
        if ((st.poly[3] & 4) && !off) memcpy(f, st.poly_offset, sizeof f);
        static int nlog;
        if ((st.poly[3] & 4) && nlog < 3 && getenv("LARECOMP_LOG_BIAS")) { nlog++; LOG("[bias] factor %g units %g clamp %g\n", st.poly_offset[0], st.poly_offset[1], st.poly_offset[2]); }
        if (memcmp(f, last, sizeof f) || enc != last_bias_enc) {
            [enc setDepthBias:f[1] slopeScale:f[0] clamp:f[2]];
            memcpy(last, f, sizeof f);
            last_bias_enc = enc;
        }
    }
    {   // depth clamp (nvnCommandBufferSetDepthClamp, ignored before; SMM2 keeps it off as far as seen)
        static int last = -1;
        static __unsafe_unretained id last_enc;  // (as last_bias_enc)
        if ((int)st.depth_clamp != last || enc != last_enc) {
            [enc setDepthClipMode:st.depth_clamp ? MTLDepthClipModeClamp : MTLDepthClipModeClip];
            last = st.depth_clamp;
            last_enc = enc;
        }
    }
    float *vp = st.viewport[0];
    float zn = st.depth_range[0][0], zf = st.depth_range[0][1];

    // resources
    bind_cbufs(0, pr->vi);
    bind_cbufs(1, pr->fi);
    if (pr->vi->uses_gmem) [enc setVertexBuffer:g_guest_buffer offset:0 atIndex:GMEM_BUFFER_INDEX];
    static id<MTLBuffer> dbgvs;
    bool dbg_this = maxwell_dbg_vs && (const uint8_t *)(uintptr_t)st.program->stages[0].code_addr == maxwell_dbg_vs;
    if (dbg_this) {
        if (!dbgvs) dbgvs = [dev newBufferWithLength:2 * 64 * 8 * 16 options:MTLResourceStorageModeShared];
        memset(dbgvs.contents, 0xff, 2 * 64 * 8 * 16);
        [enc setVertexBuffer:dbgvs offset:0 atIndex:DBG_VS_BUFFER_INDEX];
    }
    if (pr->fi->uses_gmem) [enc setFragmentBuffer:g_guest_buffer offset:0 atIndex:GMEM_BUFFER_INDEX];
    void *enc_before_tex = (__bridge void *)enc;
    bind_textures(0, pr->vi);
    bind_textures(1, pr->fi);
    if (g_debug_frame && enc_before_tex != (__bridge void *)enc) LOG("[draw] encoder changed while binding textures\n");
    extern bool pets_hide_item_cursor(void);  // (the cursor is on the companion card: runtime/hle/pets.c)
    if (draw_item_cursor && pets_hide_item_cursor()) { stat_skipped++; return; }
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    if (fabsf(pass_sx - pass_sy) > 0.01f * pass_sy && st.rt.ncolor && renderer_texture_presented(st.rt.color[0])) {
        // Draws into the swapchain image: draws that sample the full-size reshaped scene (the final
        // composite, blurred menu backgrounds) and untextured full-screen fades fill the window; everything
        // else (HUD, text, menus, movie frames) goes into a centred 16:9 box.
        bool reads_scene = false;  // samples a reshaped texture the size of the framebuffer (the scene itself)
        for (int i = 0; i < pr->fi->ntex && i < 64; i++)
            if ((tex_sc[1][i][0] != 1 || tex_sc[1][i][1] != 1) && tex_wh[1][i][0] == st.rt.color[0]->width &&
                tex_wh[1][i][1] == st.rt.color[0]->height)
                reads_scene = true;
        // Full-screen overlays without textures (fades, flashes) cover the whole window too.
        bool fullscreen_quad = r->x[2] <= 6 && st.viewport[0][0] == 0 && st.viewport[0][1] == 0 &&
                               st.viewport[0][2] >= st.rt.color[0]->width && st.viewport[0][3] >= st.rt.color[0]->height;
        bool fullscreen_fill = pr->fi->ntex == 0 && fullscreen_quad;
        // A full-screen quad sampling a reshaped (screen-shaped) texture is a scene background too, even a
        // downscaled one: the blurred scene behind the pause menu reads a 192x108 copy.
        // (single-texture draws only: the menu's frosted panels sample the same blurred copy but are UI pieces)
        if (fullscreen_quad && !reads_scene && pr->fi->ntex == 1 && (tex_sc[1][0][0] != 1 || tex_sc[1][0][1] != 1))
            reads_scene = true;
        extern uint64_t renderer_present_count(void);
        static uint64_t wide_frame = ~0ull;  // the latest draw this frame filled the window (nothing boxed since)
        if (reads_scene) wide_frame = renderer_present_count();
        // Black or white fills are fades and flashes: always the whole window. Coloured ones (the file-select
        // tint) stay boxed unless they directly follow the composite. Colour = first fragment cbuf (rgb).
        bool fade_fill = false;
        if (fullscreen_fill && st.ubuf[1][0].addr && st.ubuf[1][0].size >= 12) {
            const float *c = (const float *)(uintptr_t)st.ubuf[1][0].addr;
            bool black = c[0] < 0.01f && c[1] < 0.01f && c[2] < 0.01f, white = c[0] > 0.99f && c[1] > 0.99f && c[2] > 0.99f;
            fade_fill = black || white;
        }
        bool composite = reads_scene || fade_fill || (fullscreen_fill && wide_frame == renderer_present_count());
        if (!composite) wide_frame = ~0ull;
        static int log_fills = -1;  // LARECOMP_LOG_FILLS: every full-screen untextured fill (fs hash, colour cbuf, decision)
        if (log_fills < 0) log_fills = getenv("LARECOMP_LOG_FILLS") != NULL;
        if (log_fills && fullscreen_fill) {
            static uint64_t lastf; static int nthis;
            uint64_t f = renderer_present_count();
            if (f != lastf) { lastf = f; nthis = 0; }
            if (nthis++ < 4 && f % 6 == 0) {
                const float *c = st.ubuf[1][0].addr ? (const float *)(uintptr_t)st.ubuf[1][0].addr : NULL;
                LOG("[fill] frame %llu fs=%llx composite %d cb0 %g %g %g %g | %g %g %g %g\n", (unsigned long long)f,
                    st.program->stages[1].code_addr, composite, c ? c[0] : -1, c ? c[1] : -1, c ? c[2] : -1, c ? c[3] : -1,
                    c ? c[4] : -1, c ? c[5] : -1, c ? c[6] : -1, c ? c[7] : -1);
            }
        }
        if (g_debug_frame && getenv("LARECOMP_LOG_UI")) {
            LOG("[ui2] fs=%llx count %llu ntex %d reads_scene %d fill %d quad %d -> %d |", st.program->stages[1].code_addr, r->x[2],
                pr->fi->ntex, reads_scene, fullscreen_fill, fullscreen_quad, composite);
            for (int i = 0; i < pr->fi->ntex && i < 4; i++)
                LOG(" t%d %dx%d sc %.2f,%.2f", i, tex_wh[1][i][0], tex_wh[1][i][1], tex_sc[1][i][0], tex_sc[1][i][1]);
            LOG(" | rt %ux%u vp %.0f,%.0f %.0fx%.0f\n", st.rt.color[0]->width, st.rt.color[0]->height, st.viewport[0][0],
                st.viewport[0][1], st.viewport[0][2], st.viewport[0][3]);
        }
        if (!composite) {  // centred 16:9 region with a uniform scale
            int pw, ph;
            pass_size(&pw, &ph);
            float u = fminf(pass_sx, pass_sy);
            vp_sx = vp_sy = u;
            vp_ox = (pw - st.rt.color[0]->width * u) * 0.5f;
            vp_oy = (ph - st.rt.color[0]->height * u) * 0.5f;
        }
    }
    {   // camera-attached effects sized for 16:9 are widened with the view (runtime/hle/widescreen.c)
        extern float widescreen_effect_stretch(uint64_t fs_hash);
        float k = widescreen_effect_stretch(g_cur_fs_hash);
        double vx = vp[0] * vp_sx + vp_ox, vw = vp[2] * vp_sx;
        if (k > 1.0f) { vx -= vw * (k - 1) / 2; vw *= k; }
        [enc setViewport:(MTLViewport){vx, vp[1] * vp_sy + vp_oy, vw, vp[3] * vp_sy, zn, zf}];
    }
    apply_scissor();
    if (pr->fi->uses_rscale) {  // [0] = render target scale, [1 + i] = scale of texture i
        float rs[66][4] = {{vp_sx, vp_sy, vp_ox, vp_oy}};
        for (int i = 0; i < pr->fi->ntex && i < 64; i++) { rs[1 + i][0] = tex_sc[1][i][0]; rs[1 + i][1] = tex_sc[1][i][1]; }
        if (fkey.flip_x) rs[65][0] = (float)st.rt.color[0]->width;  // (mirror mode: the composite's width, guest pixels)
        [enc setFragmentBytes:rs length:(NSUInteger)(fkey.flip_x ? 66 : 1 + pr->fi->ntex) * 16 atIndex:RSCALE_BUFFER_INDEX];
    }
    if (st.ubuf[0][2].addr && st.ubuf[0][2].size >= 48 && !(st.ubuf[0][2].addr & 3)) {  // cut bushes' items
        extern void pot_item_draw(float *m);                                                 // (runtime/hle/pot_glow.c)
        pot_item_draw((float *)(uintptr_t)st.ubuf[0][2].addr);
    }
    if (ENV("LARECOMP_POT_PROBE") && st.ubuf[0][2].addr && st.ubuf[0][2].size >= 48) {  // (debug: bush / pot draws)
        extern void pot_probe(const float *m, bool tintable, uint64_t mesh, uint32_t inst);
        uint32_t ninst = indexed ? (uint32_t)r->x[6] : (uint32_t)r->x[4];
        pot_probe((const float *)(uintptr_t)st.ubuf[0][2].addr, pr->fi->uses_tint, st.vbuf[0].addr, ninst);
        extern uint64_t renderer_present_count(void);
        static uint64_t pf; static int pn;
        if (renderer_present_count() != pf) { pf = renderer_present_count(); pn = 0; }
        if (ninst > 1 && pf % 300 == 0 && pn++ < 40) {
            const float *m = (const float *)(uintptr_t)st.ubuf[0][2].addr;
            LOG("[potprobe] instanced x%u count %u tint %d ubuf sizes %u %u %u %u %u %u  m2 t (%.2f %.2f %.2f)\n", ninst,
                (uint32_t)r->x[2], pr->fi->uses_tint, (unsigned)st.ubuf[0][0].size, (unsigned)st.ubuf[0][1].size,
                (unsigned)st.ubuf[0][2].size, (unsigned)st.ubuf[0][3].size, (unsigned)st.ubuf[0][4].size,
                (unsigned)st.ubuf[0][5].size, m[3], m[7], m[11]);
            for (int i = 0; i < st.nattrib; i++) LOG(" a%d(f%#x s%u o%u)", i, st.attrib[i] & 0xFF, (st.attrib[i] >> 8) & 0x1F, st.attrib[i] >> 13);
            for (int i = 0; i < 12; i++)
                if (st.vbuf[i].addr && i < st.nstream && st.stream[i][1]) {
                    const float *v = (const float *)(uintptr_t)st.vbuf[i].addr;
                    LOG(" vb%d(size %llu stride %u div %u: %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f)", i,
                        st.vbuf[i].size, st.stream[i][0], st.stream[i][1], v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11]);
                }
            LOG("\n");
            for (int i = 0; i < 4; i++)
                if (st.sbuf[0][i].addr) {
                    const float *f = (const float *)(uintptr_t)st.sbuf[0][i].addr;
                    LOG("   ssbo%d size %llu:", i, st.sbuf[0][i].size);
                    for (int k = 0; k < 48 && k * 4 < (int)st.sbuf[0][i].size; k++) LOG(" %.2f", f[k]);
                    LOG("\n");
                }
        }
    }
    if (pr->fi->uses_tint) {  // per-draw highlight tint (golden unchecked pots, runtime/hle/pot_glow.c)
        float tint[8] = {0};
        extern bool pot_tint(const float *m, float tint[8], uint64_t mesh);
        extern bool pot_glow_active(void);
        if (st.ubuf[0][2].addr && st.ubuf[0][2].size >= 48 && pot_glow_active())
            pot_tint((const float *)(uintptr_t)st.ubuf[0][2].addr, tint, st.vbuf[0].addr);
        if (draw_bottle) {  // the gold Escape Bottle (runtime/hle/escape.c): its item icon / held-up model
            extern void escape_bottle_tint(int kind, const float *ui, uint32_t ui_size, float tint[8]);
            int u = draw_bottle == 3 ? 3 : 0;  // (the fairy model: its bone matrices, for where it stands - souls.c)
            escape_bottle_tint(draw_bottle, (const float *)(uintptr_t)st.ubuf[0][u].addr, (uint32_t)st.ubuf[0][u].size, tint);
        }
        if (draw_heart) { extern void texpatch_heart_tint(int kind, float tint[8]); texpatch_heart_tint(draw_heart, tint); }
        [enc setFragmentBytes:tint length:sizeof tint atIndex:19];
    }
    if (pr->gi) {  // geometry stage: each input point is an instance drawing the emitted strip (maxwell/shader.h)
        uint32_t mode = (uint32_t)r->x[0], n = (uint32_t)r->x[2];
        uint32_t ninst = indexed ? (uint32_t)r->x[6] : (uint32_t)r->x[4];
        if (mode != 0 || ninst > 1 || pr->gi->gs_emits < 3 || !n) { stat_skipped++; return; }
        // the points' vertices, in draw order, into the ring (index lists resolved here)
        uint32_t *idx = malloc(n * sizeof *idx);
        if (indexed) {
            uint32_t itype = (uint32_t)r->x[1];
            int32_t bv = (int32_t)r->x[4];
            const uint8_t *ip = (const uint8_t *)(uintptr_t)r->x[3];
            for (uint32_t i = 0; i < n; i++)
                idx[i] = (uint32_t)((int32_t)(itype == 0 ? ip[i] : itype == 1 ? ((const uint16_t *)ip)[i] : ((const uint32_t *)ip)[i]) + bv);
        } else {
            for (uint32_t i = 0; i < n; i++) idx[i] = (uint32_t)r->x[1] + i;
        }
        for (int s = 0; s < 12; s++) {
            if (!(used_streams & (1u << s))) continue;
            Range v = st.vbuf[s];
            uint32_t stride = s < st.nstream ? st.stream[s][0] : 0;
            if (!v.addr) { static const float zero[4]; [enc setVertexBytes:zero length:16 atIndex:18 + s]; continue; }
            if (!stride) { bind_guest(enc, true, v.addr, v.size ? v.size : 16, 18 + s); continue; }
            uint32_t ps = (stride + 3) & ~3u;
            uint8_t *tmp = calloc(n, ps);
            const uint8_t *src = (const uint8_t *)(uintptr_t)v.addr;
            for (uint32_t i = 0; i < n; i++)
                if (!v.size || (uint64_t)idx[i] * stride + stride <= v.size) memcpy(tmp + (size_t)i * ps, src + (size_t)idx[i] * stride, stride);
            uint64_t off = ring_alloc(tmp, (uint64_t)n * ps);
            free(tmp);
            [enc setVertexBuffer:ring offset:off atIndex:18 + s];
        }
        free(idx);
        if (used_streams & (1u << 31)) { static const float zero[4] = {0, 0, 0, 1}; [enc setVertexBytes:zero length:16 atIndex:30]; }
        for (int k = 0; k < 5; k++) {  // the geometry stage's cbufs
            if (!(pr->gi->cbuf_mask & (1u << k))) continue;
            uint64_t size = pr->gi->cbuf_size[k] ? pr->gi->cbuf_size[k] : 256, off;
            if (k == 1) {
                NvnShaderStage *ss = &st.program->stages[2];
                off = ring_alloc((void *)(uintptr_t)(ss->code_addr + ss->const_offset), size);
            } else {
                Range u = k >= cbuf_base ? st.ubuf[2][k - cbuf_base] : (Range){0, 0};
                static const uint8_t zeros[0x10000];
                if ((pr->gi->cbuf_dynamic & (1u << k)) && u.size) size = u.size > 0x10000 ? 0x10000 : u.size;
                if (u.addr && u.size && size > u.size) size = u.size;
                off = ring_alloc(u.addr ? (void *)(uintptr_t)u.addr : (void *)zeros, size);
            }
            [enc setVertexBuffer:ring offset:off atIndex:GS_CBUF_BASE + k];
        }
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 * (pr->gi->gs_emits - 2) instanceCount:n baseInstance:0];
        stat_draws++;
        if (g_debug_frame && ENV("LARECOMP_DEBUG_EACH_DRAW")) {
            LOG("[draw] ^ saved as pass_%03d\n", debug_pass_no);
            end_pass();
            st.rt_dirty = true;
        }
        return;
    }
    for (int s = 0; s < 12; s++) {
        if (!(used_streams & (1u << s))) continue;
        Range v = st.vbuf[s];
        uint32_t stride = s < st.nstream ? st.stream[s][0] : 0;
        if (v.addr && stride % 4 && v.size) {
            // Metal wants vertex strides in multiples of 4 (the pipeline rounds them up): repack a 1- or 2-byte-stride
            // stream (SMM2's terrain: small per-vertex indices) into padded records, a snapshot in the ring
            uint32_t ps = (stride + 3) & ~3u;
            uint64_t n = v.size / stride;
            if (n > (1u << 20)) n = 1u << 20;
            static uint8_t *tmp;
            static uint64_t tmp_cap;
            if (n * ps > tmp_cap) { tmp_cap = n * ps + 4096; tmp = realloc(tmp, tmp_cap); }
            const uint8_t *src = (const uint8_t *)(uintptr_t)v.addr;
            for (uint64_t k = 0; k < n; k++) { memcpy(tmp + k * ps, src + k * stride, stride); memset(tmp + k * ps + stride, 0, ps - stride); }
            uint64_t off = ring_alloc(tmp, n * ps ? n * ps : 16);
            [enc setVertexBuffer:ring offset:off atIndex:18 + s];
        } else if (v.addr) bind_guest(enc, true, v.addr, v.size ? v.size : 16, 18 + s);
        else { static const float zero[4]; [enc setVertexBytes:zero length:16 atIndex:18 + s]; }
    }
    if (used_streams & (1u << 31)) { static const float zero[4] = {0, 0, 0, 1}; [enc setVertexBytes:zero length:16 atIndex:30]; }

    // Instanced draws with item spots among their instances (bushsanity: bushes are drawn in batches, each instance's
    // model matrix in vertex storage buffer 0, indexed by instance id): drawn as runs of instances, gold runs tinted.
    // (Metal's instance_id includes the base instance, so a run [b + k, b + k + n) draws exactly those instances.)
    uint32_t inst_all = indexed ? (uint32_t)r->x[6] : (uint32_t)r->x[4], base_all = indexed ? (uint32_t)r->x[5] : (uint32_t)r->x[3];
    if (!inst_all) inst_all = 1;
    enum { MAX_RUNS = 64 };
    uint32_t run_at[MAX_RUNS], run_n[MAX_RUNS];
    bool run_gold[MAX_RUNS];
    int nruns = 0;
    if (pr->fi->uses_tint && inst_all > 1 && st.sbuf[0][0].addr) {
        extern bool pot_glow_active(void);
        extern bool pot_instance_gold(const float *m);
        uint64_t recs = st.sbuf[0][0].size / 64;
        if (pot_glow_active() && (uint64_t)base_all + inst_all <= recs) {
            const float *m = (const float *)(uintptr_t)st.sbuf[0][0].addr;
            for (uint32_t k = 0; k < inst_all; k++) {
                bool g = pot_instance_gold(m + (size_t)(base_all + k) * 16);
                if (nruns && run_gold[nruns - 1] == g) { run_n[nruns - 1]++; continue; }
                if (nruns == MAX_RUNS) { nruns = 0; break; }  // (too fragmented: draw as is)
                run_at[nruns] = k; run_n[nruns] = 1; run_gold[nruns] = g; nruns++;
            }
            if (nruns == 1 && !run_gold[0]) nruns = 0;
        }
    }
    if (!nruns) { run_at[0] = 0; run_n[0] = inst_all; nruns = 1; run_gold[0] = false; }
    for (int ri = 0; ri < nruns; ri++) {
    if (nruns > 1 || run_gold[0]) {
        float tint[8] = {0};
        extern void pot_gold_tint(float tint[8]);
        if (run_gold[ri]) pot_gold_tint(tint);
        [enc setFragmentBytes:tint length:sizeof tint atIndex:19];
    }
    // draw
    bool native;
    uint32_t mode = (uint32_t)r->x[0];
    MTLPrimitiveType pt = prim_type(mode, &native);
    if (!indexed) {
        uint32_t first = (uint32_t)r->x[1], count = (uint32_t)r->x[2], base_inst = base_all + run_at[ri], inst = run_n[ri];
        if (native) {
            [enc drawPrimitives:pt vertexStart:first vertexCount:count instanceCount:inst baseInstance:base_inst];
        } else if (mode == 6 || mode == 7 || mode == 9) {
            uint32_t n;
            id<MTLBuffer> ib = expand_indices(mode, count, NULL, 0, first, &n);
            [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:n indexType:MTLIndexTypeUInt32 indexBuffer:ib
                     indexBufferOffset:0 instanceCount:inst baseVertex:0 baseInstance:base_inst];
        } else { stat_skipped++; return; }
    } else {
        uint32_t itype = (uint32_t)r->x[1], count = (uint32_t)r->x[2];
        uint64_t iaddr = r->x[3];
        int32_t base_vertex = (int32_t)r->x[4];
        uint32_t base_inst = base_all + run_at[ri], inst = run_n[ri];
        if (native && itype != 0) {
            MTLIndexType it = itype == 1 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
            uint32_t isz = itype == 1 ? 2 : 4;
            id<MTLBuffer> ib;
            uint64_t ioff;
            if ((iaddr % isz) == 0 && (ib = guest_buffer_chunk(iaddr, (uint64_t)count * isz, &ioff))) {
                gpu_read_mark(iaddr, (uint64_t)count * isz);
            } else {
                ioff = ring_alloc((void *)(uintptr_t)iaddr, (uint64_t)count * isz); ib = ring;
            }
            [enc drawIndexedPrimitives:pt indexCount:count indexType:it indexBuffer:ib indexBufferOffset:ioff
                         instanceCount:inst baseVertex:base_vertex baseInstance:base_inst];
        } else {
            uint32_t n;
            id<MTLBuffer> ib;
            if (native) {  // 8-bit indices: widen
                uint32_t *w = malloc(count * 4 + 4);
                for (uint32_t i = 0; i < count; i++) w[i] = ((const uint8_t *)(uintptr_t)iaddr)[i];
                ib = [dev newBufferWithBytes:w length:count * 4 + 4 options:MTLResourceStorageModeShared];
                free(w);
                n = count;
            } else {
                ib = expand_indices(mode, count, (const void *)(uintptr_t)iaddr, (int)itype, 0, &n);
                pt = MTLPrimitiveTypeTriangle;
            }
            [enc drawIndexedPrimitives:pt indexCount:n indexType:MTLIndexTypeUInt32 indexBuffer:ib indexBufferOffset:0
                         instanceCount:inst baseVertex:base_vertex baseInstance:base_inst];
        }
    }
    }  // runs
    stat_draws++;
    // (debug) the target after every draw: pass_NNN.png; LARECOMP_DEBUG_DRAW_FS=<hex>[,<hex>...]: only after draws
    // with those fragment programs (guest code addresses, as the draw log prints them)
    if (g_debug_frame && ENV("LARECOMP_DEBUG_EACH_DRAW") && debug_draw_fs_match(st.program ? st.program->stages[1].code_addr : 0)) {
        LOG("[draw] ^ saved as pass_%03d\n", debug_pass_no);
        end_pass();
        st.rt_dirty = true;
    }
    if (dbg_this) {
        static int nlog;
        if (nlog++ < 4) {
            renderer_flush();
            const float *f = dbgvs.contents;
            LOG("[dbgvs] draw %d:", nlog);
            for (int i = 0; i < 2; i++)
                for (int v = 0; v < 4; v++) LOG(" i%d v%d (%g %g %g %g) uv(%g %g %g)", i, v, f[(i * 8 + v) * 4], f[(i * 8 + v) * 4 + 1], f[(i * 8 + v) * 4 + 2], f[(i * 8 + v) * 4 + 3],
                                                f[(512 + i * 8 + v) * 4], f[(512 + i * 8 + v) * 4 + 1], f[(512 + i * 8 + v) * 4 + 2]);
            LOG("\n");
        }
    }
}

// ------------------------------------------------------------------ texture copies
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; } TexRefRec;

static const char *copy_src =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct V { float4 pos [[position]]; float2 uv; };\n"
    "vertex V vs(uint vid [[vertex_id]], constant float4 &r [[buffer(0)]]) {\n"
    "  float2 p = float2((vid << 1) & 2, vid & 2); V o; o.pos = float4(p.x * 2 - 1, 1 - p.y * 2, 0, 1);\n"
    "  o.uv = r.xy + p * r.zw; return o; }\n"
    "fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
    "  return t.sample(s, in.uv); }\n"
    "struct D { float d [[depth(any)]]; };\n"
    "fragment D fs_depth(V in [[stage_in]], depth2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
    "  D o; o.d = t.sample(s, in.uv); return o; }\n";

// Bytes per block of the Metal texture backing an NVN format (RGB8 is expanded to RGBA8 on the host).
static uint64_t renderer_pixel_format_bytes(MTLPixelFormat pf, uint64_t nvn_bpb) {
    (void)pf;
    return nvn_bpb == 3 ? 4 : nvn_bpb;
}

// Linear guest memory <-> texture region. Row/image strides of 0 mean tightly packed.
static void copy_region_dims(NvnTexture *t, const TexRefRec *ref, const FormatInfo **fi, int32_t rg[6], int *level,
                             uint64_t *row, uint64_t *img) {
    *fi = nvn_format_info(t->format);
    memcpy(rg, ref->region, sizeof ref->region);
    if (!rg[3]) rg[3] = (int32_t)t->width;
    if (!rg[4]) rg[4] = (int32_t)t->height;
    if (!rg[5]) rg[5] = 1;
    *level = ref->has_view && (ref->view.setmask & 1) ? ref->view.base_level : 0;
    uint32_t wb = ((uint32_t)rg[3] + (*fi)->bw - 1) / (*fi)->bw, hb = ((uint32_t)rg[4] + (*fi)->bh - 1) / (*fi)->bh;
    *row = st.copy_row_stride ? st.copy_row_stride : (uint64_t)wb * (*fi)->bpb;
    *img = st.copy_image_stride ? st.copy_image_stride : *row * hb;
}

static void copy_buffer_to_texture(uint64_t src, const TexRefRec *dst) {
    snap_reason = "buf2tex"; snapshots_invalidate(NULL);
    NvnTexture *t = (NvnTexture *)(uintptr_t)dst->tex;
    if (!t || !src) return;
    end_pass();
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row, img;
    copy_region_dims(t, dst, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb) return;
    static int nlog;
    if (nlog++ < 20) LOG("[copy] buf %llx -> tex %ux%u f%#x region %d,%d,%d %dx%dx%d row %llu lvl %d\n", src, t->width, t->height, t->format, rg[0], rg[1], rg[2], rg[3], rg[4], rg[5], row, level);
    id<MTLBlitCommandEncoder> b = [cb() blitCommandEncoder];
    id<MTLTexture> tex = renderer_texture(t, b, false);  // make sure prior CPU contents are uploaded first
    renderer_mark_gpu_owned(t);
    bool array = tex.textureType == MTLTextureType2DArray || tex.textureType == MTLTextureTypeCube;
    uint32_t hb = ((uint32_t)rg[4] + fi->bh - 1) / fi->bh, wb = ((uint32_t)rg[3] + fi->bw - 1) / fi->bw;
    uint64_t mtl_bpb = renderer_pixel_format_bytes(tex.pixelFormat, fi->bpb);
    for (int z = 0; z < (array ? rg[5] : 1); z++) {
        const uint8_t *s = (const uint8_t *)(uintptr_t)(src + (uint64_t)z * img);
        uint64_t out_row = (uint64_t)wb * mtl_bpb;
        uint8_t *data = malloc(out_row * hb * (array ? 1 : rg[5]));
        for (int d = 0; d < (array ? 1 : rg[5]); d++)
            for (uint32_t y = 0; y < hb; y++) {
                const uint8_t *sr = s + (uint64_t)d * img + (uint64_t)y * row;
                uint8_t *dr = data + ((uint64_t)d * hb + y) * out_row;
                if (mtl_bpb == fi->bpb) memcpy(dr, sr, out_row);
                else if (fi->bpb == 3 && mtl_bpb == 4)
                    for (uint32_t x = 0; x < wb; x++) { memcpy(dr + x * 4, sr + x * 3, 3); dr[x * 4 + 3] = 255; }
                else memset(dr, 0, out_row);
            }
        id<MTLBuffer> staging = [dev newBufferWithBytes:data length:out_row * hb * (array ? 1 : rg[5])
                                                options:MTLResourceStorageModeShared];
        free(data);
        [b copyFromBuffer:staging sourceOffset:0 sourceBytesPerRow:out_row sourceBytesPerImage:out_row * hb
               sourceSize:MTLSizeMake(rg[3], rg[4], array ? 1 : rg[5]) toTexture:tex
         destinationSlice:array ? rg[2] + z : 0 destinationLevel:level
        destinationOrigin:MTLOriginMake(rg[0], rg[1], array ? 0 : rg[2])];
    }
    [b endEncoding];
}

// Color copy with scaling: draw src rect into dst rect (physical texels).
static id<MTLTexture> copy_raw_view(id<MTLTexture> t) {
    MTLPixelFormat pf = t.pixelFormat;
    MTLPixelFormat raw = pf == MTLPixelFormatRGBA8Unorm_sRGB ? MTLPixelFormatRGBA8Unorm
                       : pf == MTLPixelFormatBGRA8Unorm_sRGB ? MTLPixelFormatBGRA8Unorm : pf;
    if (raw == pf) return t;
    id<MTLTexture> v = [t newTextureViewWithPixelFormat:raw];
    return v ? v : t;
}

static void draw_copy(id<MTLTexture> s, int sx0, int sy0, int sw, int sh, id<MTLTexture> d, int dx0, int dy0, int dw, int dh,
                      bool linear) {
    static int raw = -1;  // LARECOMP_COPY_RAW_SRGB: copies move values unchanged (no sRGB decode/encode on either side)
    if (raw < 0) raw = getenv("LARECOMP_COPY_RAW_SRGB") != NULL;
    if (raw) { s = copy_raw_view(s); d = copy_raw_view(d); }
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = d;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cb() renderCommandEncoderWithDescriptor:rp];
    NSString *key = [NSString stringWithFormat:@"copy %lu", (unsigned long)d.pixelFormat];
    id<MTLRenderPipelineState> pipe = cache[key];
    if (!pipe) {
        static id<MTLLibrary> lib;
        if (!lib) lib = TIMED_COMPILE([dev newLibraryWithSource:@(copy_src) options:nil error:nil]);
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"vs"];
        pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        pd.colorAttachments[0].pixelFormat = d.pixelFormat;
        pipe = TIMED_COMPILE([dev newRenderPipelineStateWithDescriptor:pd error:nil]);
        cache[key] = pipe;
    }
    static id<MTLSamplerState> lin, near;
    if (!lin) {
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        near = [dev newSamplerStateWithDescriptor:sd];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        lin = [dev newSamplerStateWithDescriptor:sd];
    }
    float r[4] = {(float)sx0 / s.width, (float)sy0 / s.height, (float)sw / s.width, (float)sh / s.height};
    [e setRenderPipelineState:pipe];
    [e setViewport:(MTLViewport){dx0, dy0, dw, dh, 0, 1}];
    [e setVertexBytes:r length:16 atIndex:0];
    [e setFragmentTexture:s atIndex:0];
    [e setFragmentSamplerState:linear ? lin : near atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    stat_passes++;
}

static void copy_texture_to_buffer(const TexRefRec *src, uint64_t dst) {
    NvnTexture *t = (NvnTexture *)(uintptr_t)src->tex;
    if (!t || !dst || dst < HEAP_BASE) return;
    end_pass();
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row, img;
    copy_region_dims(t, src, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb) return;
    {
        static int nlog;
        if (nlog++ < 12 || getenv("LARECOMP_LOG_READBACK"))
            LOG("[readback] tex %ux%u f%#x level %d region %d,%d %dx%d -> %llx row %llu\n", t->width, t->height,
                t->format, level, rg[0], rg[1], rg[3], rg[4], dst, row);
    }
    id<MTLTexture> tex = renderer_texture(t, nil, false);
    float tsx, tsy;
    renderer_texture_scale(t, &tsx, &tsy);
    if ((tsx != 1 || tsy != 1) && level == 0 && !renderer_format_is_depth(t->format)) {
        // the CPU expects guest-sized texels: filter the scaled allocation down first
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:tex.pixelFormat width:t->width
                                                                                      height:t->height mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> small = [dev newTextureWithDescriptor:td];
        draw_copy(tex, 0, 0, tex.width, tex.height, small, 0, 0, t->width, t->height, true);
        tex = small;
    }
    id<MTLBlitCommandEncoder> b = [cb() blitCommandEncoder];
    if (renderer_pixel_format_bytes(tex.pixelFormat, fi->bpb) != fi->bpb) { [b endEncoding]; return; }
    bool array = tex.textureType == MTLTextureType2DArray || tex.textureType == MTLTextureTypeCube;
    for (int z = 0; z < (array ? rg[5] : 1); z++) {
        uint64_t boff;
        id<MTLBuffer> bbuf = guest_buffer_chunk(dst + (uint64_t)z * img, img, &boff);
        if (bbuf)
            [b copyFromTexture:tex sourceSlice:array ? rg[2] + z : 0 sourceLevel:level
                  sourceOrigin:MTLOriginMake(rg[0], rg[1], array ? 0 : rg[2])
                    sourceSize:MTLSizeMake(rg[3], rg[4], array ? 1 : rg[5]) toBuffer:bbuf
             destinationOffset:boff destinationBytesPerRow:row destinationBytesPerImage:img];
    }
    [b endEncoding];
    renderer_flush();  // the CPU may read the result right after the fence
}

static void copy_texture(const TexRefRec *src, const TexRefRec *dst, uint64_t flags) {
    snap_reason = "tex2tex"; snapshots_invalidate(NULL);
    NvnTexture *st_ = (NvnTexture *)(uintptr_t)src->tex, *dt = (NvnTexture *)(uintptr_t)dst->tex;
    if (!st_ || !dt) return;
    end_pass();
    id<MTLTexture> s = sampled_texture(st_, &src->view, src->has_view, TEXTYPE_2D);
    renderer_mark_gpu_owned(dt);
    id<MTLTexture> d = view_texture(dt, &dst->view, dst->has_view, true, nil);
    // a full copy into a swapchain image: the source is the frame's final framebuffer (UI is drawn there)
    extern void renderer_texture_mark_presented(NvnTexture *t);
    if (renderer_texture_presented(dt) && st_->width == dt->width && st_->height == dt->height)
        renderer_texture_mark_presented(st_);
    extern bool renderer_texture_first_write(NvnTexture *t);
    renderer_texture_first_write(dt);  // a copy fills the image: no clear afterwards this frame
    extern bool renderer_texture_level_fresh(NvnTexture *t);
    renderer_texture_level_fresh(dt);  // nor a level-change clear
    const int32_t *sr = src->region, *dr = dst->region;
    if (g_debug_frame) {
        static int nl;
        uint64_t total = tegra_total_size(&st_->layout), nz = 0;
        for (uint64_t k = 0; k < total && st_->addr; k++) nz += ((const uint8_t *)(uintptr_t)st_->addr)[k] != 0;
        if (nl < 400) LOG("[copytex] src addr %llx size %llu nonzero %llu tflags %#x stride %u bh %u\n", st_->addr, total, nz, st_->flags, st_->stride, st_->layout.block_height_log2);
        if (nl++ < 400) LOG("[copytex] %ux%u f%#x (%d,%d %dx%d) -> %ux%u f%#x (%d,%d %dx%d) flags %llx src_gpu %d\n", st_->width, st_->height, st_->format,
            sr[0], sr[1], sr[3], sr[4], dt->width, dt->height, dt->format, dr[0], dr[1], dr[3], dr[4], flags, renderer_is_gpu_owned(st_));
    }
    // regions are in guest texels; scaled allocations (output resolution) need physical texels
    float ssx, ssy, dsx, dsy;
    renderer_texture_scale(st_, &ssx, &ssy);
    renderer_texture_scale(dt, &dsx, &dsy);
    int sx0 = (int)lroundf(sr[0] * ssx), sy0 = (int)lroundf(sr[1] * ssy);
    int dx0 = (int)lroundf(dr[0] * dsx), dy0 = (int)lroundf(dr[1] * dsy);
    int sw = sr[3] ? (int)lroundf(sr[3] * ssx) : (int)s.width, sh = sr[4] ? (int)lroundf(sr[4] * ssy) : (int)s.height;
    int dw = dr[3] ? (int)lroundf(dr[3] * dsx) : (int)d.width, dh = dr[4] ? (int)lroundf(dr[4] * dsy) : (int)d.height;
    static int no_depth_scale = -1;
    if (no_depth_scale < 0) no_depth_scale = getenv("LARECOMP_NO_DEPTH_SCALE") != NULL;
    if (!no_depth_scale && renderer_format_is_depth(dt->format) && (sw != dw || sh != dh) && s.sampleCount == 1 && dt->format != 0x31) {
        // scaled depth copy (the game builds its depth pyramid this way): draw, writing fragment depth
        MTLRenderPassDescriptor *drp = [MTLRenderPassDescriptor renderPassDescriptor];
        drp.depthAttachment.texture = d;
        drp.depthAttachment.loadAction = MTLLoadActionLoad;
        drp.depthAttachment.storeAction = MTLStoreActionStore;
        if (renderer_format_has_stencil(dt->format)) {
            drp.stencilAttachment.texture = d;
            drp.stencilAttachment.loadAction = MTLLoadActionLoad;
            drp.stencilAttachment.storeAction = MTLStoreActionStore;
        }
        id<MTLRenderCommandEncoder> e = [cb() renderCommandEncoderWithDescriptor:drp];
        NSString *key = [NSString stringWithFormat:@"copydepth %lu", (unsigned long)d.pixelFormat];
        id<MTLRenderPipelineState> pipe = cache[key];
        if (!pipe) {
            static id<MTLLibrary> dlib;
            NSError *derr = nil;
            if (!dlib) dlib = TIMED_COMPILE([dev newLibraryWithSource:@(copy_src) options:nil error:&derr]);
            if (!dlib) LOG("[copy] depth copy library: %s\n", derr.localizedDescription.UTF8String);
            MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
            pd.vertexFunction = [dlib newFunctionWithName:@"vs"];
            pd.fragmentFunction = [dlib newFunctionWithName:@"fs_depth"];
            pd.depthAttachmentPixelFormat = d.pixelFormat;
            if (renderer_format_has_stencil(dt->format)) pd.stencilAttachmentPixelFormat = d.pixelFormat;
            NSError *perr = nil;
            pipe = TIMED_COMPILE([dev newRenderPipelineStateWithDescriptor:pd error:&perr]);
            if (!pipe) { LOG("[copy] depth copy pipeline: %s\n", perr.localizedDescription.UTF8String); return; }
            cache[key] = pipe;
        }
        static id<MTLDepthStencilState> always;
        if (!always) {
            MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
            dd.depthCompareFunction = MTLCompareFunctionAlways;
            dd.depthWriteEnabled = YES;
            always = [dev newDepthStencilStateWithDescriptor:dd];
        }
        static id<MTLSamplerState> dnear;
        if (!dnear) dnear = [dev newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
        id<MTLTexture> sdepth = s;
        if (s.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 || s.pixelFormat == DEPTH24_STENCIL8_FORMAT)
            sdepth = [s newTextureViewWithPixelFormat:s.pixelFormat];
        float r[4] = {(float)sx0 / s.width, (float)sy0 / s.height, (float)sw / s.width, (float)sh / s.height};
        [e setRenderPipelineState:pipe];
        [e setDepthStencilState:always];
        [e setViewport:(MTLViewport){dx0, dy0, dw, dh, 0, 1}];
        [e setVertexBytes:r length:16 atIndex:0];
        [e setFragmentTexture:sdepth atIndex:0];
        [e setFragmentSamplerState:dnear atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding];
        stat_passes++;
        static int dbgc = -1;
        if (dbgc < 0) dbgc = getenv("LARECOMP_DEBUG_DEPTHCOPY") != NULL;
        if (dbgc) {
            static int nlog;
            if (nlog++ < 6) {
                id<MTLBuffer> rb = [dev newBufferWithLength:64 options:MTLResourceStorageModeShared];
                id<MTLBlitCommandEncoder> bb = [cb() blitCommandEncoder];
                MTLBlitOption opt = (s.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone;
                [bb copyFromTexture:s sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(s.width / 2, s.height / 2, 0)
                         sourceSize:MTLSizeMake(1, 1, 1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:4 destinationBytesPerImage:4 options:opt];
                opt = (d.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone;
                [bb copyFromTexture:d sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(d.width / 2, d.height / 2, 0)
                         sourceSize:MTLSizeMake(1, 1, 1) toBuffer:rb destinationOffset:16 destinationBytesPerRow:4 destinationBytesPerImage:4 options:opt];
                [bb endEncoding];
                renderer_flush();
                const float *f = rb.contents;
                LOG("[copy] depth %lux%lu fmt %lu -> %lux%lu fmt %lu: centre src %g dst %g\n", (unsigned long)s.width, (unsigned long)s.height,
                    (unsigned long)s.pixelFormat, (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat, f[0], f[4]);
            }
        }
        return;
    }
    if (renderer_format_is_depth(dt->format) || (s.pixelFormat == d.pixelFormat && sw == dw && sh == dh && s.sampleCount == d.sampleCount)) {
        id<MTLBlitCommandEncoder> b = [cb() blitCommandEncoder];
        [b copyFromTexture:s sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(sx0, sy0, 0)
                sourceSize:MTLSizeMake(MIN(sw, dw), MIN(sh, dh), 1) toTexture:d destinationSlice:0 destinationLevel:0
         destinationOrigin:MTLOriginMake(dx0, dy0, 0)];
        [b endEncoding];
        return;
    }
    draw_copy(s, sx0, sy0, sw, sh, d, dx0, dy0, dw, dh, flags & 1);
}

// ------------------------------------------------------------------ executor
// Binding-state commands: s updated, nothing else touched (the executor and the prefetch scan share it). false:
// not one of them.
static bool apply_state(ExecState *s, const CmdRec *r) {
    void *x = (void *)r->extra;
    switch (r->op) {
        case CMD_BIND_PROGRAM: s->program = (NvnProgram *)(uintptr_t)r->x[0]; return true;
        case CMD_BIND_BLEND: { uint8_t *b = x; memcpy(s->blend[b[0] & 7], b, 8); return true; }
        case CMD_BIND_CHANNEL_MASK: memcpy(&s->channel_mask, x, 4); return true;
        case CMD_BIND_COLOR: memcpy(s->color_state, x, 4); return true;
        case CMD_BIND_DEPTH_STENCIL: memcpy(s->ds, x, 8); return true;
        case CMD_BIND_MULTISAMPLE: memcpy(s->ms, x, 0x18); return true;
        case CMD_BIND_POLYGON: memcpy(s->poly, x, 4); return true;
        case CMD_BIND_VERTEX_ATTRIB:
            s->nattrib = (int)r->x[0] > 16 ? 16 : (int)r->x[0];
            memcpy(s->attrib, x, s->nattrib * 4);
            return true;
        case CMD_BIND_VERTEX_STREAM:
            s->nstream = (int)r->x[0] > 16 ? 16 : (int)r->x[0];
            memcpy(s->stream, x, s->nstream * 8);
            return true;
        case CMD_BIND_VERTEX_BUFFERS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(&s->vbuf[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            return true;
        case CMD_BIND_UNIFORM_BUFFERS:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 18 && r->x[0] < 6; i++)
                memcpy(&s->ubuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16);
            return true;
        case CMD_BIND_STORAGE_BUFFERS:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 16 && r->x[0] < 6; i++)
                memcpy(&s->sbuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16);
            return true;
        case CMD_BIND_TEXTURES:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 32 && r->x[0] < 6; i++)
                memcpy(&s->tex[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8);
            return true;
        case CMD_BIND_IMAGES:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 8 && r->x[0] < 6; i++)
                memcpy(&s->img[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8);
            return true;
        case CMD_SET_TEXTURE_POOL: s->tpool = (NvnDescPool *)(uintptr_t)r->x[0]; return true;
        case CMD_SET_SAMPLER_POOL: s->spool = (NvnDescPool *)(uintptr_t)r->x[0]; return true;
        case CMD_SET_RENDER_TARGETS: {
            const struct { uint64_t tex[8]; NvnTextureView views[8]; uint8_t has_view[8]; uint64_t depth;
                           NvnTextureView dview; uint8_t has_dview; } *rt = x;
            memset(&s->rt, 0, sizeof s->rt);
            s->rt.ncolor = (int)r->x[0];
            for (int i = 0; i < s->rt.ncolor; i++) {
                s->rt.color[i] = (NvnTexture *)(uintptr_t)rt->tex[i];
                s->rt.cview[i] = rt->views[i];
                s->rt.has_cview[i] = rt->has_view[i];
            }
            s->rt.depth = (NvnTexture *)(uintptr_t)rt->depth;
            s->rt.dview = rt->dview;
            s->rt.has_dview = rt->has_dview;
            s->rt_dirty = true;
            return true;
        }
        case CMD_SET_VIEWPORTS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(s->viewport[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            return true;
        case CMD_SET_SCISSORS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(s->scissor[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            return true;
        case CMD_SET_DEPTH_RANGES:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(s->depth_range[r->x[0] + i], (uint8_t *)x + i * 8, 8);
            return true;
        case CMD_SET_BLEND_COLOR: memcpy(s->blend_color, x, 16); return true;
        case CMD_SET_STENCIL: {
            int face = (int)r->x[0];
            for (int f = 0; f < 2; f++) {
                if (!(face & (1 << f))) continue;
                if (r->x[6] == 0) s->stencil_ref[f] = (uint32_t)r->x[1];
                else if (r->x[6] == 1) s->stencil_mask[f] = (uint32_t)r->x[1];
                else s->stencil_vmask[f] = (uint32_t)r->x[1];
            }
            return true;
        }
        case CMD_SET_POLYGON_OFFSET: memcpy(s->poly_offset, r->f, sizeof s->poly_offset); return true;
        case CMD_SET_DEPTH_CLAMP: s->depth_clamp = (r->x[0] & 1) != 0; return true;
        case CMD_SET_PRIMITIVE_RESTART: s->prim_restart = r->x[0] != 0; s->restart_index = (uint32_t)r->x[1]; return true;
        case CMD_SET_COPY_STRIDES: s->copy_row_stride = r->x[0]; s->copy_image_stride = r->x[1]; return true;
        case CMD_SET_RENDER_ENABLE:
            if (r->x[2]) {  // conditional: the counters at x1 and x1 + 16 (mode 0: render if equal, 1: if different)
                const uint64_t *q = (const uint64_t *)(uintptr_t)r->x[1];
                bool eq = q ? q[0] == q[2] : true;
                s->render_off = r->x[0] == 0 ? !eq : eq;
            } else {
                s->render_off = !r->x[0];
            }
            return true;
    default: return false;
    }
}

static void exec_list(uint64_t handle, int depth) {
    uint64_t p = handle, prev = 0;
    for (int guard = 0; guard < 10000000; guard++) {
        if (p < 0x100000 || (p & 7) || ((CmdRec *)(uintptr_t)p)->op >= CMD_COUNT) {  // (a broken list: stop, say where)
            static int n;
            if (n++ < 10)
                LOG("[metal] command list %llx: bad record address %llx after %llx (op %u) at depth %d\n",
                    (unsigned long long)handle, (unsigned long long)p, (unsigned long long)prev,
                    prev ? ((CmdRec *)(uintptr_t)prev)->op : 0, depth);
            return;
        }
        prev = p;
        CmdRec *r = (CmdRec *)(uintptr_t)p;
        void *x = r->extra;
        if (r->op < CMD_COUNT) op_counts[r->op]++;
        switch (r->op) {
        case CMD_END: return;
        case CMD_JUMP: p = r->x[0]; continue;
        case CMD_CALL: {
            const uint64_t *h = x;
            for (uint64_t i = 0; i < r->x[0] && depth < 8; i++) {
                if (!h[i]) { static int n; if (n++ < 5) LOG("[metal] CallCommands: null command handle %llu of %llu\n", (unsigned long long)i, (unsigned long long)r->x[0]); continue; }
                exec_list(h[i], depth + 1);
            }
            break;
        }
        case CMD_SET_RENDER_TARGETS: {
            {
                const uint64_t *tx = x;
                for (int i = 0; i < (int)r->x[0] && i < 8; i++) {
                    NvnTexture *t = (NvnTexture *)(uintptr_t)tx[i];
                    static int nlog;
                    if (t && (t->flags & 2) && nlog++ < 5) LOG("[metal] video texture %p used as render target\n", t);
                }
            }
            apply_state(&st, r);
            if (ENV("LARECOMP_DEBUG_3D") && st.rt.ncolor && st.rt.color[0] && st.rt.color[0]->target == 2) {
                static int once;  // (debug) the first frame that renders into a 3D texture becomes the debug frame
                if (!once++) { g_debug_frame = true; LOG("===== debug frame (3D target) =====\n"); }
            }
            if (ENV("LARECOMP_LOG_RTS")) {  // (debug) every render target the first time it is bound
                static uint64_t seen[512];
                static int nseen;
                for (int i = 0; i < st.rt.ncolor; i++) {
                    NvnTexture *t = st.rt.color[i];
                    if (!t) continue;
                    uint64_t key = t->addr ^ ((uint64_t)st.rt.cview[i].min_layer << 56) ^ ((uint64_t)t->target << 48);
                    bool known = false;
                    for (int k = 0; k < nseen; k++) known |= seen[k] == key;
                    if (known || nseen == 512) continue;
                    seen[nseen++] = key;
                    LOG("[rtlog] f%llu target %d: %p %ux%ux%u tgt %u fmt %#x addr %llx %s | view %d mask %#x layer %u+%u tgt %u\n",
                        (unsigned long long)renderer_present_count(), i, (void *)t, t->width, t->height, t->depth, t->target,
                        t->format, (unsigned long long)t->addr, t->label, st.rt.has_cview[i], st.rt.cview[i].setmask,
                        st.rt.cview[i].min_layer, st.rt.cview[i].num_layers, st.rt.cview[i].target);
                }
            }
            if (g_debug_frame) {
                NvnTexture *t0 = st.rt.color[0];
                const NvnTextureView *v = &st.rt.cview[0];
                LOG("[setrt] %d colour target(s): %p (%ux%ux%u tgt %u lv %u addr %llx %s) view %d mask %#x lv %u+%u layer %u+%u fmt %#x tgt %u | depth %p\n",
                    st.rt.ncolor, (void *)t0, t0 ? t0->width : 0, t0 ? t0->height : 0, t0 ? t0->depth : 0, t0 ? t0->target : 0,
                    t0 ? t0->levels : 0, t0 ? (unsigned long long)t0->addr : 0ull, t0 ? t0->label : "", st.rt.has_cview[0], v->setmask,
                    v->base_level, v->num_levels, v->min_layer, v->num_layers, v->format, v->target, (void *)st.rt.depth);
            }
            break;
        }
        case CMD_REPORT_VALUE: {
            uint64_t *out = (uint64_t *)(uintptr_t)r->x[1];
            if (out) { out[0] = (uint32_t)r->x[0]; out[1] = (uint64_t)((unsigned __int128)plat_now_ns() * 384 / 625); }
            break;
        }
        case CMD_CLEAR_BUFFER: {  // ClearBuffer(address, size, value): 32-bit fill
            uint32_t *out = (uint32_t *)(uintptr_t)r->x[0];
            if (out) for (uint64_t i = 0; i < r->x[1] / 4; i++) out[i] = (uint32_t)r->x[2];
            break;
        }
        case CMD_CLEAR_COLOR: {
            if (st.render_off) break;
            snap_reason = "clear-color"; snapshots_invalidate(NULL);
            int index = (int)r->x[0];
            static int log_clears = -1;  // LARECOMP_LOG_CLEARS: every colour clear (frame, target size, mask, scissor)
            if (log_clears < 0) log_clears = getenv("LARECOMP_LOG_CLEARS") != NULL;
            if (log_clears && index < st.rt.ncolor && st.rt.color[index])
                LOG("[cclear] frame %llu idx %d %ux%u f%#x mask %#x scissor %d,%d %dx%d\n", (unsigned long long)renderer_present_count(),
                    index, st.rt.color[index]->width, st.rt.color[index]->height, st.rt.color[index]->format, (uint32_t)r->x[2],
                    st.scissor[0][0], st.scissor[0][1], st.scissor[0][2], st.scissor[0][3]);
            else if (log_clears) LOG("[cclear] frame %llu idx %d: no target bound (ncolor %d)\n", (unsigned long long)renderer_present_count(), index, st.rt.ncolor);
            if (index < st.rt.ncolor && st.rt.color[index]) clear(index, (int)r->x[5], x, (uint32_t)r->x[2], false, 0, false, 0, 0);
            break;
        }
        case CMD_CLEAR_TEXTURE: {
            // ClearTexture(cmd, texture, view, region, color, mask) (runtime/nvn/cmdbuf.c clear_texture). Was never
            // executed: the game clears its text-box textures this way before drawing a new message, so the old
            // text stayed behind the new one (user 2026-10-08: "Load this file?" over "Please select a file.").
            // Done as a render-target clear: bind the texture alone, scissor = region, clear(), restore.
            const struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; float color[4]; } *ct = x;
            NvnTexture *t = (NvnTexture *)(uintptr_t)ct->tex;
            if (!t) break;
            snap_reason = "clear-texture"; snapshots_invalidate(NULL);
            RtState saved = st.rt;
            int32_t sc[4];
            memcpy(sc, st.scissor[0], sizeof sc);
            end_pass();
            memset(&st.rt, 0, sizeof st.rt);
            st.rt.ncolor = 1;
            st.rt.color[0] = t;
            if (ct->has_view) { st.rt.cview[0] = ct->view; st.rt.has_cview[0] = 1; }
            st.rt_dirty = true;
            const int32_t *rg = ct->region;
            if (rg[3] > 0 && rg[4] > 0) { st.scissor[0][0] = rg[0]; st.scissor[0][1] = rg[1]; st.scissor[0][2] = rg[3]; st.scissor[0][3] = rg[4]; }
            else { st.scissor[0][0] = st.scissor[0][1] = 0; st.scissor[0][2] = st.scissor[0][3] = 16384; }
            static int nlog;
            if (nlog++ < 8 || g_debug_frame)
                LOG("[cleartex] %ux%u f%#x region %d,%d %dx%d color %g %g %g %g mask %#llx kind %llu\n", t->width, t->height, t->format,
                    rg[0], rg[1], rg[3], rg[4], ct->color[0], ct->color[1], ct->color[2], ct->color[3], r->x[4], r->x[6]);
            clear(0, (int)r->x[6], ct->color, (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 0xF, false, 0, false, 0, 0);
            end_pass();
            st.rt = saved;
            memcpy(st.scissor[0], sc, sizeof sc);
            st.rt_dirty = true;
            break;
        }
        case CMD_CLEAR_DEPTH_STENCIL:
            if (st.render_off) break;
            snap_reason = "clear-depth"; snapshots_invalidate(NULL);
            if (st.rt.depth) {
                // (depth s0, depthMask x1, stencil x2, stencilMask x3)
                bool has_stencil = renderer_format_has_stencil(st.rt.depth->format);
                clear(-1, 0, NULL, 0, r->x[0] != 0, r->f[0], has_stencil && r->x[2] != 0, (uint32_t)r->x[1],
                      (uint32_t)r->x[2]);
            }
            break;
        case CMD_DISPATCH:
            dispatch_compute((uint32_t)r->x[0], (uint32_t)r->x[1], (uint32_t)r->x[2]);
            if (st.program) {
                static uint64_t seen[16];
                static int nseen;
                uint64_t cs = st.program->stages[5].code_addr;
                bool known = false;
                for (int i = 0; i < nseen; i++) known |= seen[i] == cs;
                if (!known && nseen < 16) {
                    seen[nseen++] = cs;
                    char path[64];
                    snprintf(path, sizeof path, "build/cs_%d.bin", nseen - 1);
                    FILE *f = fopen(path, "wb");  // (no build/ outside a source checkout: the app's data root)
                    if (f) fwrite((void *)(uintptr_t)cs, 1, 0x4000, f), fclose(f);
                    LOG("[dispatch] new compute program %d: cs %llx groups %llu %llu %llu images:", nseen - 1, cs, r->x[0], r->x[1], r->x[2]);
                    for (int i = 0; i < 8; i++) if (st.img[5][i]) LOG(" %d=%llx", i, st.img[5][i]);
                    LOG(" tex:");
                    for (int i = 0; i < 8; i++) if (st.tex[5][i]) LOG(" %d=%llx", i, st.tex[5][i]);
                    LOG(" ubuf:");
                    for (int i = 0; i < 8; i++) if (st.ubuf[5][i].addr) LOG(" %d=%llx/%llu", i, st.ubuf[5][i].addr, st.ubuf[5][i].size);
                    LOG(" ssbo:");
                    for (int i = 0; i < 8; i++) if (st.sbuf[5][i].addr) LOG(" %d=%llx/%llu", i, st.sbuf[5][i].addr, st.sbuf[5][i].size);
                    LOG("\n");
                }
            }
            if (g_debug_frame && st.program) {
                LOG("[dispatch] %llu %llu %llu prog %p cs %llx\n", r->x[0], r->x[1], r->x[2], st.program, st.program->stages[5].code_addr);
                FILE *f = fopen("build/cs_code.bin", "wb");
                if (f) fwrite((void *)(uintptr_t)st.program->stages[5].code_addr, 1, 0x4000, f), fclose(f);
                f = fopen("build/cs_ctrl.bin", "wb");
                if (f) fwrite(st.program->stages[5].control, 1, 0x900, f), fclose(f);
                LOG("[dispatch] images:");
                for (int i = 0; i < 8; i++) if (st.img[5][i]) LOG(" %d=%llx", i, st.img[5][i]);
                LOG(" tex:");
                for (int i = 0; i < 8; i++) if (st.tex[5][i]) LOG(" %d=%llx", i, st.tex[5][i]);
                LOG(" ubuf:");
                for (int i = 0; i < 8; i++) if (st.ubuf[5][i].addr) LOG(" %d=%llx/%llu", i, st.ubuf[5][i].addr, st.ubuf[5][i].size);
                LOG("\n");
            }
            break;
        case CMD_DRAW_ARRAYS: if (!st.render_off) draw(r, false); break;
        case CMD_DRAW_ELEMENTS: if (!st.render_off) draw(r, true); break;
        case CMD_UPDATE_UNIFORM_BUFFER:
            // (buffer, size, offset, dataSize, data): apply the update in command order
            if (gpu_read_pending(r->x[0] + r->x[2], r->x[3])) renderer_flush();  // (see gpu_read_mark)
            memcpy((void *)(uintptr_t)(r->x[0] + r->x[2]), x, r->x[3]);
            break;
        case CMD_FENCE_SYNC: {
            // The CPU may read GPU-written memory (compute results) once the fence is signaled.
            static int fence_flush = -1;
            if (fence_flush < 0) fence_flush = getenv("LARECOMP_FENCE_FLUSH") ? 1 : 0;
            if (fence_flush) renderer_flush();
            extern void nvn_sync_set_seq(uint64_t guest, uint64_t seq);
            if (r->x[1]) nvn_sync_set_seq(r->x[1], renderer_fence_seq());
            break;
        }
        case CMD_REPORT_COUNTER: {
            uint64_t *out = (uint64_t *)(uintptr_t)r->x[1];
            static uint32_t seen;
            uint32_t type = (uint32_t)r->x[0];
            if (type < 32 && !(seen & (1u << type))) { seen |= 1u << type; LOG("[nvn] ReportCounter type %u\n", type); }
            // (Switch GPU ticks, 614.4 MHz: the game converts the difference to ns itself - see the Vulkan exec.c)
            if (out) { out[0] = 0; out[1] = (uint64_t)((unsigned __int128)plat_now_ns() * 384 / 625); }
            break;
        }
        case CMD_COPY_TEXTURE_TO_TEXTURE: {
            const TexRefRec *t = x;
            copy_texture(&t[0], &t[1], r->x[6]);
            break;
        }
        case CMD_DOWNSAMPLE: {
            const TexRefRec *t = x;
            copy_texture(&t[0], &t[1], 1);
            break;
        }
        case CMD_COPY_BUFFER_TO_TEXTURE: copy_buffer_to_texture(r->x[0], x); break;
        case CMD_COPY_TEXTURE_TO_BUFFER: copy_texture_to_buffer(x, r->x[3]); break;
        case CMD_COPY_BUFFER_TO_BUFFER:
            renderer_flush();
            memmove((void *)(uintptr_t)r->x[1], (void *)(uintptr_t)r->x[0], r->x[2]);
            break;
        case CMD_BARRIER: case CMD_SET_LINE_WIDTH: case CMD_SET_POINT_SIZE: case CMD_SET_DEPTH_BOUNDS: break;
        default:
            if (apply_state(&st, r)) break;
            if (g_debug_frame) LOG("[cmd] unhandled op %d x0 %llx x1 %llx\n", (int)r->op, (unsigned long long)r->x[0], (unsigned long long)r->x[1]);
            break;
        }
        p += r->size;
    }
}

// ------------------------------------------------------------------ shader prefetch
// Before a submission runs, its lists are walked on a copy of the state (the shadow) and the programs and pipelines
// their draws will need start building, side by side (docs/superpowers/specs/2026-10-20-shader-prefetch-design.md).
// Advisory only: a draw the scan got wrong builds its own and waits, as without it.
static ExecState g_shadow;

static bool pipe_known(uint64_t phash) {
    uint32_t slot = (uint32_t)(phash ^ (phash >> 29)) & (PIPE_TAB - 1);
    while (pipe_tab[slot].key && pipe_tab[slot].key != phash) slot = (slot + 1) & (PIPE_TAB - 1);
    return pipe_tab[slot].key != 0;
}

static void scan_draw(const CmdRec *r) {
    ExecState *s = &g_shadow;
    if (!s->program || (s->poly[0] == 3 && (uint32_t)r->x[0] >= 4)) return;  // (as draw() skips)
    g_prefetch.draws++;
    ShaderKey vkey, fkey;
    program_keys(s, &vkey, &fkey);
    uint64_t made = g_prog_creates;
    Prog *pr = get_program(s->program, &vkey, &fkey);  // (a new one starts compiling)
    g_prefetch.progs += g_prog_creates - made;
    if (!pr || atomic_load(&pr->state) == BUILD_FAILED) return;
    uint64_t phash = pipeline_state_hash(s, pr);
    if (pipe_known(phash)) return;
    PassLayout pl;
    pass_layout(s, &pl);
    NSString *pk;
    MTLRenderPipelineDescriptor *pd = render_pipe_desc(s, &pl, pr, &pk);
    if (cache[pk]) return;
    bool later = false;
    @synchronized(pr) {  // (compiling still: the pipeline is requested when it's done - see get_program)
        if (atomic_load(&pr->state) == BUILD_PENDING) {
            PipeReq *q = [PipeReq new];
            q->pd = pd, q->key = pk, q->khash = pipe_key_hash(pk);
            if (!pr->waiting) pr->waiting = [NSMutableArray new];
            [pr->waiting addObject:q];
            later = true;
            g_prefetch.pipes++;
        }
    }
    bool created = false;
    if (!later && atomic_load(&pr->state) == BUILD_READY) pipe_request(pr, pd, pk, pipe_key_hash(pk), &created);
    if (created) g_prefetch.pipes++;
}

static void scan_list(uint64_t handle, int depth) {
    uint64_t p = handle;
    for (int guard = 0; guard < 10000000; guard++) {
        if (p < 0x100000 || (p & 7) || ((CmdRec *)(uintptr_t)p)->op >= CMD_COUNT) return;  // (exec_list says where)
        CmdRec *r = (CmdRec *)(uintptr_t)p;
        switch (r->op) {
        case CMD_END: return;
        case CMD_JUMP: p = r->x[0]; continue;
        case CMD_CALL: {
            const uint64_t *h = (const uint64_t *)r->extra;
            for (uint64_t i = 0; i < r->x[0] && depth < 8; i++) if (h[i]) scan_list(h[i], depth + 1);
            break;
        }
        case CMD_DRAW_ARRAYS: case CMD_DRAW_ELEMENTS: scan_draw(r); break;  // (render-enable ignored: a build too many)
        case CMD_DISPATCH: {
            uint64_t made = g_cs_creates;
            if (g_shadow.program && r->x[0] && r->x[1] && r->x[2]) compute_program(g_shadow.program);
            g_prefetch.progs += g_cs_creates - made;
            break;
        }
        case CMD_CLEAR_TEXTURE: break;  // (binds its texture and puts the targets back)
        default: apply_state(&g_shadow, r); break;
        }
        p += r->size;
    }
}

void backend_prefetch(const uint64_t *handles, int n) {
    if (shader_mode() != SHADERS_PREFETCH) return;
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    pipeline_cache_open();
    g_shadow = st;
    for (int i = 0; i < n; i++) if (handles[i]) scan_list(handles[i], 0);
    g_prefetch.submits++;
    g_prefetch.scan_ns += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
}

// LARECOMP_PREFETCH_VERIFY=1: after a submission ran, the scan's final state should be the executor's
void renderer_prefetch_verify(void) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_PREFETCH_VERIFY") != NULL;
    if (!on || shader_mode() != SHADERS_PREFETCH) return;
    static int nlog;
    #define VERIFY(f, n) if (memcmp(&g_shadow.f, &st.f, n) && nlog++ < 20) LOG("[shader] prefetch verify: %s differs\n", #f)
    VERIFY(program, sizeof st.program);
    VERIFY(nattrib, sizeof st.nattrib);
    VERIFY(attrib, sizeof st.attrib[0] * (size_t)(st.nattrib < 16 ? st.nattrib : 16));
    VERIFY(nstream, sizeof st.nstream);
    VERIFY(stream, sizeof st.stream[0] * (size_t)(st.nstream < 16 ? st.nstream : 16));
    VERIFY(rt, sizeof st.rt);
    VERIFY(blend, sizeof st.blend);
    VERIFY(color_state, sizeof st.color_state);
    VERIFY(channel_mask, sizeof st.channel_mask);
    VERIFY(ms, sizeof st.ms);
    VERIFY(poly, sizeof st.poly);
    #undef VERIFY
}

// In-place reboot (runtime/nvn/metal.m backend_reset; GPU idle, the game's textures already destroyed): drop
// everything keyed by guest addresses or guest-owned objects - programs (by code address), pipelines (by program),
// views, snapshots, bound state - so the next boot can't hit a stale entry. Metal's own shader cache stays warm.
void renderer_reset(void) {
    build_drain();  // (builds in flight finish first: they write into the objects dropped below)
    pend_clear(mpipe_release);
    renderer_flush();
    pthread_mutex_lock(&gone_lock);
    ngone = 0;
    pthread_mutex_unlock(&gone_lock);
    for (int i = 0; i < VIEW_TAB; i++) {
        if (view_tab[i].view) CFRelease(view_tab[i].view);
        view_tab[i].view = view_tab[i].base = NULL;
        view_tab[i].key = 0;
    }
    g_views_live = 0;
    memset(prog_tab, 0, sizeof prog_tab);
    memset(pipe_tab, 0, sizeof pipe_tab);
    [cache removeAllObjects];
    [g_cs_progs removeAllObjects];
    @synchronized(g_ready_pipes) { [g_ready_pipes removeAllObjects]; }
    g_boot_gen++;
    [g_snap_copies removeAllObjects];
    memset(snap_tex, 0, sizeof snap_tex);
    memset(snap_ok, 0, sizeof snap_ok);
    for (int i = 0; i < 4; i++) { feedback_tex[i] = NULL; feedback_copy[i] = nil; }
    nfeedback = 0;
    g_cur_fs_hash = 0;
    last_bias_enc = nil;
    pthread_mutex_lock(&cb_lock);
    last_cb = nil;
    for (int i = 0; i < CB_HIST; i++) cb_hist[i] = nil;  // (they retain the old guest heap buffer)
    pthread_mutex_unlock(&cb_lock);
    st_defaults();
}

void backend_execute(uint64_t handle) {
    @autoreleasepool {
        exec_list(handle, 0);
        static int sync_submit = -1;
        if (sync_submit < 0) sync_submit = getenv("LARECOMP_SYNC_SUBMIT") ? 1 : 0;
        if (sync_submit) renderer_flush();  // old behaviour: finish the GPU work before returning
        else renderer_submit();  // the guest fences (nvnSyncWait) before reusing memory the GPU reads
    }
}
