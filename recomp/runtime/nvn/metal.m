#include <sys/mman.h>
#include "../game.h"
#include "paths.h"
#include "settings.h"
#include "ui/menu.h"
// Metal renderer for the NVN layer: window, textures, command execution, presentation.
#include <sys/stat.h>
#include <dlfcn.h>
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#else
#import <AppKit/AppKit.h>
#endif
#import <Metal/Metal.h>
#if __has_include(<MetalFX/MetalFX.h>)  // (not in the iOS Simulator SDK)
#import <MetalFX/MetalFX.h>
#define HAVE_METALFX 1
#endif
#include "output_scale.h"
#include "framegen.h"
#import <QuartzCore/CAMetalLayer.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
#include <stdatomic.h>

#include "nvn.h"
#include "renderer.h"
#include "texpatch.h"
#include "astc.h"
#include "bcsoft.h"

static id<MTLDevice> dev;
static id<MTLCommandQueue> queue;
static CAMetalLayer *layer;
static SDL_Window *window;
static atomic_bool ui_ready;
static id<MTLRenderPipelineState> present_pipe;
static id<MTLSamplerState> present_sampler;
id<MTLBuffer> g_guest_buffer;  // the guest heap's first chunk (GPU addresses map to buffer offsets)
uint64_t g_guest_buffer_base;
#define GUEST_CHUNK_MAX (2ull << 30)  // the NVIDIA/MetalOld driver reads zeros from no-copy buffers over 2 GiB
static id<MTLBuffer> g_guest_chunks[4];
static int g_guest_chunk_count;
static uint64_t g_guest_chunk_len;
static uint64_t g_guest_wrapped;

// ================================================================== formats
typedef struct { MTLPixelFormat pf; uint8_t expand_rgb; uint8_t force_alpha_one; } MtlFormat;

// GPUs without BC textures (the iOS Simulator, older iPhones; RECOMP_BC_SOFT=1 to try it on a Mac): BC1-7 are
// decoded on the CPU at upload (runtime/nvn/bcsoft.c) into RGBA8 / RGBA8 snorm / RGBA16F textures. Set in backend_init.
static bool g_bc_soft;
// GPUs without ASTC textures (Intel Macs' NVIDIA / AMD drivers, Metal 2 only; RECOMP_ASTC_SOFT=1 to try it on a Mac):
// ASTC is decoded on the CPU at upload (runtime/nvn/astc.c) into RGBA8, as the Vulkan path does. Creating an ASTC
// texture there aborts inside Metal ("invalid pixelFormat 204", the course menu's thumbnails, 2026-10-22).
static bool g_astc_soft;
static bool astc_format(uint32_t f) { return f >= 0x79 && f <= 0x94; }
static MtlFormat mtl_format_hw(uint32_t f);
static MtlFormat mtl_format(uint32_t f) {
    if (g_astc_soft && astc_format(f)) return (MtlFormat){f >= 0x87 ? MTLPixelFormatRGBA8Unorm_sRGB : MTLPixelFormatRGBA8Unorm};
    int k = g_bc_soft ? bcsoft_kind(f) : 0;
    if (!k) return mtl_format_hw(f);
    bool srgb = f == 0x45 || f == 0x46 || f == 0x47 || f == 0x48 || f == 0x4E;
    if (k == BCSOFT_BC6 || k == BCSOFT_BC6S) return (MtlFormat){MTLPixelFormatRGBA16Float};
    if (k == BCSOFT_BC4S || k == BCSOFT_BC5S) return (MtlFormat){MTLPixelFormatRGBA8Snorm};
    return (MtlFormat){srgb ? MTLPixelFormatRGBA8Unorm_sRGB : MTLPixelFormatRGBA8Unorm};
}

static MtlFormat mtl_format_hw(uint32_t f) {
    switch (f) {
    case 0x01: return (MtlFormat){MTLPixelFormatR8Unorm};
    case 0x02: return (MtlFormat){MTLPixelFormatR8Snorm};
    case 0x03: return (MtlFormat){MTLPixelFormatR8Uint};
    case 0x04: return (MtlFormat){MTLPixelFormatR8Sint};
    case 0x05: return (MtlFormat){MTLPixelFormatR16Float};
    case 0x06: return (MtlFormat){MTLPixelFormatR16Unorm};
    case 0x07: return (MtlFormat){MTLPixelFormatR16Snorm};
    case 0x08: return (MtlFormat){MTLPixelFormatR16Uint};
    case 0x09: return (MtlFormat){MTLPixelFormatR16Sint};
    case 0x0A: return (MtlFormat){MTLPixelFormatR32Float};
    case 0x0B: return (MtlFormat){MTLPixelFormatR32Uint};
    case 0x0C: return (MtlFormat){MTLPixelFormatR32Sint};
    case 0x0D: return (MtlFormat){MTLPixelFormatRG8Unorm};
    case 0x0E: return (MtlFormat){MTLPixelFormatRG8Snorm};
    case 0x0F: return (MtlFormat){MTLPixelFormatRG8Uint};
    case 0x10: return (MtlFormat){MTLPixelFormatRG8Sint};
    case 0x11: return (MtlFormat){MTLPixelFormatRG16Float};
    case 0x12: return (MtlFormat){MTLPixelFormatRG16Unorm};
    case 0x13: return (MtlFormat){MTLPixelFormatRG16Snorm};
    case 0x14: return (MtlFormat){MTLPixelFormatRG16Uint};
    case 0x15: return (MtlFormat){MTLPixelFormatRG16Sint};
    case 0x16: return (MtlFormat){MTLPixelFormatRG32Float};
    case 0x17: return (MtlFormat){MTLPixelFormatRG32Uint};
    case 0x18: return (MtlFormat){MTLPixelFormatRG32Sint};
    case 0x19: return (MtlFormat){MTLPixelFormatRGBA8Unorm, 1, 1};
    case 0x25: return (MtlFormat){MTLPixelFormatRGBA8Unorm};
    case 0x26: return (MtlFormat){MTLPixelFormatRGBA8Snorm};
    case 0x27: return (MtlFormat){MTLPixelFormatRGBA8Uint};
    case 0x28: return (MtlFormat){MTLPixelFormatRGBA8Sint};
    case 0x29: return (MtlFormat){MTLPixelFormatRGBA16Float};
    case 0x2A: return (MtlFormat){MTLPixelFormatRGBA16Unorm};
    case 0x2B: return (MtlFormat){MTLPixelFormatRGBA16Snorm};
    case 0x2C: return (MtlFormat){MTLPixelFormatRGBA16Uint};
    case 0x2D: return (MtlFormat){MTLPixelFormatRGBA16Sint};
    case 0x2E: return (MtlFormat){MTLPixelFormatRGBA32Float};
    case 0x2F: return (MtlFormat){MTLPixelFormatRGBA32Uint};
    case 0x30: return (MtlFormat){MTLPixelFormatRGBA32Sint};
    case 0x31: return (MtlFormat){MTLPixelFormatStencil8};
    case 0x32: return (MtlFormat){MTLPixelFormatDepth16Unorm};
    case 0x33: return (MtlFormat){MTLPixelFormatDepth32Float};
    case 0x34: return (MtlFormat){MTLPixelFormatDepth32Float};
    case 0x35: return (MtlFormat){MTLPixelFormatDepth32Float_Stencil8};
    case 0x36: return (MtlFormat){MTLPixelFormatDepth32Float_Stencil8};
    case 0x37: return (MtlFormat){MTLPixelFormatRGBA8Unorm_sRGB, 0, 1};
    case 0x38: return (MtlFormat){MTLPixelFormatRGBA8Unorm_sRGB};
    case 0x39: return (MtlFormat){MTLPixelFormatABGR4Unorm};
    case 0x3A: return (MtlFormat){MTLPixelFormatB5G6R5Unorm};
    case 0x3B: return (MtlFormat){MTLPixelFormatA1BGR5Unorm};
    case 0x3C: return (MtlFormat){MTLPixelFormatB5G6R5Unorm};
    case 0x3D: return (MtlFormat){MTLPixelFormatRGB10A2Unorm};
    case 0x3E: return (MtlFormat){MTLPixelFormatRGB10A2Uint};
    case 0x3F: return (MtlFormat){MTLPixelFormatRG11B10Float};
    case 0x40: return (MtlFormat){MTLPixelFormatRGB9E5Float};
    case 0x41: case 0x42: return (MtlFormat){MTLPixelFormatBC1_RGBA};
    case 0x43: return (MtlFormat){MTLPixelFormatBC2_RGBA};
    case 0x44: return (MtlFormat){MTLPixelFormatBC3_RGBA};
    case 0x45: case 0x46: return (MtlFormat){MTLPixelFormatBC1_RGBA_sRGB};
    case 0x47: return (MtlFormat){MTLPixelFormatBC2_RGBA_sRGB};
    case 0x48: return (MtlFormat){MTLPixelFormatBC3_RGBA_sRGB};
    case 0x49: return (MtlFormat){MTLPixelFormatBC4_RUnorm};
    case 0x4A: return (MtlFormat){MTLPixelFormatBC4_RSnorm};
    case 0x4B: return (MtlFormat){MTLPixelFormatBC5_RGUnorm};
    // 0x4C = signed RGTC2 (interior normal maps break if decoded unsigned). LARECOMP_BC5_UNORM=1 decodes it
    // unsigned: that removes the overworld grass hatch/moire (see notes/PLAN.md) but wrecks interiors.
    case 0x4C: return (MtlFormat){getenv("LARECOMP_BC5_UNORM") ? MTLPixelFormatBC5_RGUnorm : MTLPixelFormatBC5_RGSnorm};
    case 0x4D: return (MtlFormat){MTLPixelFormatBC7_RGBAUnorm};
    case 0x4E: return (MtlFormat){MTLPixelFormatBC7_RGBAUnorm_sRGB};
    case 0x4F: return (MtlFormat){MTLPixelFormatBC6H_RGBFloat};
    case 0x50: return (MtlFormat){MTLPixelFormatBC6H_RGBUfloat};
    case 0x6D: return (MtlFormat){MTLPixelFormatRGBA8Unorm, 0, 1};
    case 0x71: return (MtlFormat){MTLPixelFormatRGBA16Float, 0, 1};
    case 0x76: return (MtlFormat){MTLPixelFormatRGBA32Float, 0, 1};
    case 0x79: return (MtlFormat){MTLPixelFormatASTC_4x4_LDR};
    case 0x7A: return (MtlFormat){MTLPixelFormatASTC_5x4_LDR};
    case 0x7B: return (MtlFormat){MTLPixelFormatASTC_5x5_LDR};
    case 0x7C: return (MtlFormat){MTLPixelFormatASTC_6x5_LDR};
    case 0x7D: return (MtlFormat){MTLPixelFormatASTC_6x6_LDR};
    case 0x7E: return (MtlFormat){MTLPixelFormatASTC_8x5_LDR};
    case 0x7F: return (MtlFormat){MTLPixelFormatASTC_8x6_LDR};
    case 0x80: return (MtlFormat){MTLPixelFormatASTC_8x8_LDR};
    case 0x81: return (MtlFormat){MTLPixelFormatASTC_10x5_LDR};
    case 0x82: return (MtlFormat){MTLPixelFormatASTC_10x6_LDR};
    case 0x83: return (MtlFormat){MTLPixelFormatASTC_10x8_LDR};
    case 0x84: return (MtlFormat){MTLPixelFormatASTC_10x10_LDR};
    case 0x85: return (MtlFormat){MTLPixelFormatASTC_12x10_LDR};
    case 0x86: return (MtlFormat){MTLPixelFormatASTC_12x12_LDR};
    case 0x87: return (MtlFormat){MTLPixelFormatASTC_4x4_sRGB};
    case 0x88: return (MtlFormat){MTLPixelFormatASTC_5x4_sRGB};
    case 0x89: return (MtlFormat){MTLPixelFormatASTC_5x5_sRGB};
    case 0x8A: return (MtlFormat){MTLPixelFormatASTC_6x5_sRGB};
    case 0x8B: return (MtlFormat){MTLPixelFormatASTC_6x6_sRGB};
    case 0x8C: return (MtlFormat){MTLPixelFormatASTC_8x5_sRGB};
    case 0x8D: return (MtlFormat){MTLPixelFormatASTC_8x6_sRGB};
    case 0x8E: return (MtlFormat){MTLPixelFormatASTC_8x8_sRGB};
    case 0x8F: return (MtlFormat){MTLPixelFormatASTC_10x5_sRGB};
    case 0x90: return (MtlFormat){MTLPixelFormatASTC_10x6_sRGB};
    case 0x91: return (MtlFormat){MTLPixelFormatASTC_10x8_sRGB};
    case 0x92: return (MtlFormat){MTLPixelFormatASTC_10x10_sRGB};
    case 0x93: return (MtlFormat){MTLPixelFormatASTC_12x10_sRGB};
    case 0x94: return (MtlFormat){MTLPixelFormatASTC_12x12_sRGB};
    case 0x95: case 0x97: return (MtlFormat){MTLPixelFormatA1BGR5Unorm};
    case 0x96: return (MtlFormat){MTLPixelFormatA1BGR5Unorm};
    case 0x98: return (MtlFormat){MTLPixelFormatBGRA8Unorm, 0, 1};
    case 0x99: return (MtlFormat){MTLPixelFormatBGRA8Unorm};
    case 0x9A: return (MtlFormat){MTLPixelFormatBGRA8Unorm_sRGB, 0, 1};
    case 0x9B: return (MtlFormat){MTLPixelFormatBGRA8Unorm_sRGB};
    default:
        return (MtlFormat){MTLPixelFormatRGBA8Unorm};
    }
}

bool renderer_format_is_depth(uint32_t f) { return f >= 0x31 && f <= 0x36; }
bool renderer_format_has_stencil(uint32_t f) { return f == 0x31 || f == 0x35 || f == 0x36; }
MTLPixelFormat renderer_pixel_format(uint32_t f) { return mtl_format(f).pf; }
bool renderer_format_alpha_one(uint32_t f) { return mtl_format(f).force_alpha_one; }

// ================================================================== textures
typedef struct {
    __unsafe_unretained id<MTLTexture> tex;  // retained manually (CFBridgingRetain)
    bool gpu_owned;     // written by the GPU (render target / copy dest): never re-uploaded
    bool uploaded;
    bool dynamic;        // contents changed after the first upload: verify fully every frame
    uint64_t data_hash, sample_hash;
    uint64_t checked_frame, full_frame;
    bool scalable;       // screen-shaped render target: allocated at the output resolution
    bool presented;      // has been shown by backend_present (a swapchain image)
    uint64_t touched_frame;  // last frame it was written (cleared at the first write of a frame when reshaped)
    uint32_t level_gen;      // g_level_gen when last used as a render target
    uint64_t frame_cleared;  // LARECOMP_FRAME_CLEAR: frame it was last cleared at first use
    float sx, sy;        // allocation scale relative to the guest's size
    int reg;             // slot in g_reg (live textures, for the "largest textures" dump) + 1
    int tp;              // runtime/nvn/texpatch.c: 0 not checked yet, -1 none, else known UI texture + 1
    uint32_t tp_gen;     // texpatch_generation() it was last uploaded with
    int refs;            // texture objects sharing it (aliases of the same storage, see tex_backend)
} TexBackend;

// Live textures (diagnostics: the overworld slowdown at 32:9 - the same textures used 870 MB and 4x the GPU time
// until a window resize reallocated them; user 2026-10-09). Dumped when the frame rate stays low.
static NvnTexture *g_reg[16384];
static int g_reg_n;
static void reg_add(NvnTexture *t, TexBackend *b) {
    if (b->reg || g_reg_n >= 16384) return;
    g_reg[g_reg_n++] = t;
    b->reg = g_reg_n;
}
static void reg_remove(TexBackend *b) {
    if (!b->reg) return;
    int i = b->reg - 1;
    NvnTexture *last = g_reg[--g_reg_n];
    g_reg[i] = last;
    if (last && last->backend) ((TexBackend *)last->backend)->reg = i + 1;
    b->reg = 0;
}
static void dump_largest_textures(const char *why) {
    int idx[16];
    uint64_t sz[16];
    int n = 0;
    for (int i = 0; i < g_reg_n; i++) {
        TexBackend *b = g_reg[i] ? (TexBackend *)g_reg[i]->backend : NULL;
        if (!b || !b->tex) continue;
        uint64_t a = b->tex.allocatedSize;
        int k = n < 16 ? n++ : 15;
        if (k == 15 && n == 16 && a <= sz[15]) continue;
        while (k > 0 && sz[k - 1] < a) { sz[k] = sz[k - 1]; idx[k] = idx[k - 1]; k--; }
        sz[k] = a; idx[k] = i;
    }
    LOG("[texdump] %s: %d live textures, largest:\n", why, g_reg_n);
    for (int k = 0; k < n; k++) {
        NvnTexture *t = g_reg[idx[k]];
        TexBackend *b = (TexBackend *)t->backend;
        LOG("[texdump]   %6.1f MB  guest %ux%u f%#x tgt %u lv %u | mtl %lux%lu lv %lu | scalable %d scale %.2fx%.2f %s\n",
            sz[k] / 1048576.0, t->width, t->height, t->format, t->target, t->levels, (unsigned long)b->tex.width,
            (unsigned long)b->tex.height, (unsigned long)b->tex.mipmapLevelCount, b->scalable, b->sx, b->sy, t->label);
    }
}

static uint64_t frames, total_frames;
uint64_t renderer_present_count(void) { return total_frames; }

// Output resolution: render targets the game sizes for the 1920x1080 screen are allocated at
// (sx, sy) times their guest size. Shaders and the renderer keep working in guest ("logical") units.
static float g_rs_x = 1, g_rs_y = 1;
void renderer_get_scale(float *sx, float *sy) { *sx = g_rs_x; *sy = g_rs_y; }
void renderer_set_scale(float sx, float sy) { g_rs_x = sx; g_rs_y = sy; }

static bool tex_scalable(const NvnTexture *t) {
    if (getenv("LARECOMP_NO_SCALE")) return false;
    if (t->target != 1 || t->samples > 1 || !(t->flags & 8) || t->width < 32 || t->height < 16) return false;
    float a = (float)t->width / (float)t->height;
    return a > 1.74f && a < 1.81f;  // 16:9 (1920x1080 and its fractions, 704x396, 60x33...)
}

// First write this frame to a presented image while the output is reshaped: returns true once per frame.
bool renderer_texture_first_write(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    extern bool g_debug_frame;
    if (g_debug_frame && b && b->presented)
        LOG("[fw] tex %p %ux%u f%#x sx %.2f sy %.2f touched %llu now %llu\n", (void *)t, t->width, t->height, t->format,
            b->sx, b->sy, (unsigned long long)b->touched_frame, (unsigned long long)total_frames + 1);
    if (!b || !b->presented || (b->sx == b->sy) || b->touched_frame == total_frames + 1) return false;
    b->touched_frame = total_frames + 1;
    return true;
}

// A new level gets fresh render-target memory on the Switch (black); ours keep the previous level's pixels, which
// show wherever a level never draws (the phone booth's surroundings). Render targets are cleared at their first
// use after a level change (LARECOMP_KEEP_TARGETS=1 disables).
static uint32_t g_level_gen = 1;
static void level_change_check(void) {
    extern const char *game_level_name(void);
    static char last[64];
    const char *lv = game_level_name();
    if (strncmp(lv, last, sizeof last - 1) != 0) {
        strlcpy(last, lv, sizeof last);
        // only levels that never draw their surroundings: clearing everywhere also wipes targets other rooms
        // carry over (a house lost a band of its walls)
        if (!strncmp(lv, "TelephoneBox", 12) || getenv("LARECOMP_CLEAR_ALL_LEVELS")) g_level_gen++;
    }
}
// Scene render targets are cleared to transparent black at their first use in each frame (LARECOMP_NO_FRAME_CLEAR=1
// disables): pixels a frame never draws (inside pits, around generated rooms) otherwise keep older frames' content
// and smear (moving enemies over a pit left trails, the pit rim repeated downwards). Verified 2026-10-08 on
// rando/DevGen3 Tail Cave 07D: clean dark pit, no trails; overworld / dungeon rooms otherwise unchanged.
bool renderer_texture_frame_fresh(NvnTexture *t) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_NO_FRAME_CLEAR") == NULL;
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    if (!on || !b || !b->scalable || b->frame_cleared == total_frames + 1) return false;
    b->frame_cleared = total_frames + 1;
    return true;
}

bool renderer_texture_level_fresh(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    static int keep = -1;
    if (keep < 0) keep = getenv("LARECOMP_KEEP_TARGETS") != NULL;
    if (!b || keep || b->level_gen == g_level_gen) return false;
    if (!b->scalable) { b->level_gen = g_level_gen; return false; }  // scene buffers only: UI textures the game draws once
                                                                       // and reuses (rupee counter) must keep their contents
    bool fresh = b->level_gen != 0;  // first use ever: nothing stale to hide
    b->level_gen = g_level_gen;
    if (fresh && getenv("LARECOMP_LOG_LEVELCLEAR"))
        LOG("[levelclear] gen %u frame %llu tex %p %ux%u fmt %#x addr %llx\n", g_level_gen, (unsigned long long)total_frames, (void *)t,
            t->width, t->height, t->format, (unsigned long long)t->addr);
    return fresh;
}

void renderer_texture_mark_presented(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    if (b) b->presented = true;
}

bool renderer_texture_presented(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    return b && b->presented;
}

void renderer_texture_scale(NvnTexture *t, float *sx, float *sy) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    if (b && b->scalable) { *sx = b->sx; *sy = b->sy; }
    else { *sx = 1; *sy = 1; }
}

static MTLTextureType mtl_tex_type(uint32_t target, uint32_t samples) {
    switch (target) {
    case 0: return MTLTextureType1D;
    case 2: return MTLTextureType3D;
    case 3: return MTLTextureType1DArray;
    case 4: return MTLTextureType2DArray;
    case 5: return MTLTextureType2DMultisample;
    case 6: return MTLTextureType2DMultisampleArray;
    case 8: return MTLTextureTypeCube;
#if TARGET_OS_SIMULATOR
    case 9: return MTLTextureType2DArray;  // (no cube arrays in the Simulator: the faces as layers, never sampled)
#else
    case 9: return MTLTextureTypeCubeArray;
#endif
    default: return samples > 1 ? MTLTextureType2DMultisample : MTLTextureType2D;
    }
}

static void create_backend_texture(NvnTexture *t, TexBackend *b);
static void release_backend_texture(NvnTexture *t, TexBackend *b);
int g_tex_live;              // (stats: Metal textures alive and their size)
uint64_t g_tex_live_bytes;
void renderer_texture_gone(void *nvn_tex, void *mtl_tex);  // renderer.m

// ---- aliases. Engines create texture objects over the same memory again and again (SMM2's agl allocates its
// render targets per frame from a pool: a target drawn through one NVNtexture is sampled through another made over
// the same storage). On the console they are the same texels; here each object had its own Metal texture, and the
// one sampled was never drawn into (black frames). Textures over the same storage with the same shape now share one
// backend (reference counted). RECOMP_NO_TEX_ALIAS=1: off.
#define ALIAS_BUCKETS 4096
typedef struct Alias { uint64_t addr; uint32_t fmt, w, h, d, lv, tgt, samples; TexBackend *b; struct Alias *next; } Alias;
static Alias *g_alias[ALIAS_BUCKETS];
static unsigned alias_bucket(uint64_t addr) { return (unsigned)((addr >> 9) * 0x9E3779B1u) & (ALIAS_BUCKETS - 1); }
static bool alias_on(const NvnTexture *t) {
    static int off = -1;
    if (off < 0) off = getenv("RECOMP_NO_TEX_ALIAS") != NULL;
    return !off && t->addr >= GUEST_LO && t->addr < GUEST_HI;
}
static bool alias_match(const Alias *a, const NvnTexture *t) {
    return a->addr == t->addr && a->fmt == t->format && a->w == t->width && a->h == t->height && a->d == t->depth &&
           a->lv == t->levels && a->tgt == t->target && a->samples == t->samples;
}
static void alias_remove(TexBackend *b) {
    for (unsigned k = 0; k < ALIAS_BUCKETS; k++)
        for (Alias **pp = &g_alias[k]; *pp; pp = &(*pp)->next)
            if ((*pp)->b == b) { Alias *a = *pp; *pp = a->next; free(a); return; }
}

static TexBackend *tex_backend(NvnTexture *t) {
    if (t->backend) {
        TexBackend *b = (TexBackend *)t->backend;
        if (b->scalable && (b->sx != g_rs_x || b->sy != g_rs_y)) {  // output resolution changed: reallocate
            release_backend_texture(t, b);
            create_backend_texture(t, b);
        }
        return b;
    }
    if (alias_on(t)) {
        for (Alias *a = g_alias[alias_bucket(t->addr)]; a; a = a->next)
            if (alias_match(a, t)) {
                static int logged;
                if (logged++ < 5) LOG("[metal] texture %p (%ux%u f%#x %s) aliases storage %llx\n", (void *)t, t->width, t->height,
                                      t->format, t->label, (unsigned long long)t->addr);
                a->b->refs++;
                t->backend = a->b;
                return tex_backend(t);  // (the rescale check)
            }
    }
    TexBackend *b = calloc(1, sizeof *b);
    b->refs = 1;
    if (alias_on(t)) {
        Alias *a = calloc(1, sizeof *a);
        *a = (Alias){t->addr, t->format, t->width, t->height, t->depth, t->levels, t->target, t->samples, b, NULL};
        unsigned k = alias_bucket(t->addr);
        a->next = g_alias[k];
        g_alias[k] = a;
    }
    b->scalable = tex_scalable(t);
    if (t->width == 1920 && t->height == 1080 && getenv("LARECOMP_LOG_SCALABLE"))
        LOG("[scale] 1920x1080 tex %p f%#x flags %#x target %u samples %u levels %u -> scalable %d (%s)\n", (void *)t, t->format,
            t->flags, t->target, t->samples, t->levels, b->scalable, t->label);
    create_backend_texture(t, b);
    t->backend = b;
    reg_add(t, b);
    return b;
}

// runtime/nvn/texpatch.c id of a texture already in use (-1: none / not identified yet)
int renderer_texpatch_of(NvnTexture *t) {
    const TexBackend *b = (const TexBackend *)t->backend;
    return b && b->tp > 0 ? b->tp - 1 : -1;
}

// (test, LARECOMP_ZERO_TEXTURES=1) a new colour texture cleared to zero, every level and slice: Metal leaves a new
// texture's contents undefined (zeros on the Mac in practice, leftovers on iOS), where NVN's come from the game's
// memory pool
static void zero_texture(id<MTLTexture> tex, uint32_t nvn_format) {
    MTLPixelFormat pf = tex.pixelFormat;
    bool compressed = (pf >= 130 && pf <= 143) || (pf >= 160 && pf <= 240);
    if (compressed || renderer_format_is_depth(nvn_format) || !queue) return;
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    NSUInteger slices = tex.textureType == MTLTextureTypeCube ? 6 : tex.textureType == MTLTextureTypeCubeArray ?
                        tex.arrayLength * 6 : tex.arrayLength;
    for (NSUInteger l = 0; l < tex.mipmapLevelCount; l++) {
        NSUInteger planes = tex.textureType == MTLTextureType3D ? MAX(tex.depth >> l, 1u) : 1;
        for (NSUInteger sl = 0; sl < slices; sl++)
            for (NSUInteger z = 0; z < planes; z++) {
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture = tex;
                rp.colorAttachments[0].level = l;
                rp.colorAttachments[0].slice = sl;
                rp.colorAttachments[0].depthPlane = z;
                rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
                rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
            }
    }
    [cb commit];
}

static void create_backend_texture(NvnTexture *t, TexBackend *b) {
    MtlFormat mf = mtl_format(t->format);
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = mtl_tex_type(t->target, t->samples);
    d.pixelFormat = mf.pf;
    b->sx = b->scalable ? g_rs_x : 1;
    b->sy = b->scalable ? g_rs_y : 1;
    uint32_t tw = t->width ? t->width : 1, th = t->height ? t->height : 1;
    if (b->scalable) {
        tw = (uint32_t)lroundf(tw * b->sx); th = (uint32_t)lroundf(th * b->sy);
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
    }
    d.width = tw;
    d.height = (t->target == 0 || t->target == 3) ? 1 : th;
    d.depth = t->target == 2 ? (t->depth ? t->depth : 1) : 1;
    if (t->target == 3) d.arrayLength = t->height ? t->height : 1;
    else if (t->target == 4 || t->target == 6) d.arrayLength = t->depth ? t->depth : 1;
    else if (t->target == 9) d.arrayLength = (t->depth ? t->depth : 6) / (d.textureType == MTLTextureTypeCubeArray ? 6 : 1);
    d.mipmapLevelCount = t->levels ? t->levels : 1;
    if (b->scalable) {
        uint32_t m = 1, sz = tw > th ? tw : th;
        while (sz > 1) { sz >>= 1; m++; }
        if (d.mipmapLevelCount > m) d.mipmapLevelCount = m;
    }
    if (t->samples > 1) { d.sampleCount = t->samples; d.mipmapLevelCount = 1; }
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView | MTLTextureUsageRenderTarget |
              MTLTextureUsageShaderWrite;
    d.storageMode = MTLStorageModePrivate;
    // Shaders sample depth through ordinary texture2d bindings (Maxwell has no separate depth type);
    // keep depth uncompressed so those reads see the stored values.
    if (renderer_format_is_depth(t->format)) d.allowGPUOptimizedContents = NO;
    if (mf.force_alpha_one)
        d.swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleGreen,
                                                  MTLTextureSwizzleBlue, MTLTextureSwizzleOne);
    id<MTLTexture> tex = [dev newTextureWithDescriptor:d];
    if (!tex) {
        LOG("[metal] texture creation failed: fmt %#x %ux%ux%u target %u\n", t->format, t->width, t->height, t->depth,
            t->target);
        d.pixelFormat = MTLPixelFormatRGBA8Unorm;
        tex = [dev newTextureWithDescriptor:d];
    }
    tex.label = [NSString stringWithFormat:@"%s fmt%#x", t->label[0] ? t->label : "tex", t->format];
    static int zero = -1;
    if (zero < 0) zero = getenv("LARECOMP_ZERO_TEXTURES") && atoi(getenv("LARECOMP_ZERO_TEXTURES"));
    if (zero) zero_texture(tex, t->format);
    b->tex = tex;
    CFBridgingRetain(tex);
    g_tex_live++;
    g_tex_live_bytes += tex.allocatedSize;
}

static void release_backend_texture(NvnTexture *t, TexBackend *b) {  // (also drops the renderer's views of it)
    renderer_texture_gone(NULL, (__bridge void *)b->tex);
    g_tex_live--;
    g_tex_live_bytes -= b->tex.allocatedSize;
    CFBridgingRelease((__bridge CFTypeRef)b->tex);
    (void)t;
}

void backend_texture_destroyed(NvnTexture *t) {
    TexBackend *b = t->backend;
    renderer_texture_gone(t, NULL);  // its snapshot copy
    t->backend = NULL;
    if (!b) return;
    if (b->reg && g_reg[b->reg - 1] == t) reg_remove(b);  // (the registry names one owner: drop it with that owner)
    if (--b->refs > 0) return;  // (other textures over the same storage still use it: aliases, above)
    alias_remove(b);
    reg_remove(b);
    release_backend_texture(t, b);
    free(b);
}

static uint64_t g_frame_counter;  // presents so far (texture change detection runs once per frame)
static uint64_t full_hash_budget, full_hash_budget_frame;

static uint64_t hash_full(const uint8_t *p, uint64_t n) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
    uint64_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

static uint64_t fnv_sample(const uint8_t *p, uint64_t n) {
    // cheap change detector: hash a strided sample of the data
    uint64_t h = 1469598103934665603ull;
    uint64_t step = n > 4096 ? n / 4096 : 1;
    for (uint64_t i = 0; i < n; i += step) { h ^= p[i]; h *= 1099511628211ull; }
    return h ^ n;
}

// Upload all levels/layers from block-linear guest memory.
static void upload_texture(NvnTexture *t, TexBackend *b, id<MTLBlitCommandEncoder> blit) {
    if (!t->addr || t->target == 10) return;
    if (b->uploaded && b->checked_frame == g_frame_counter) return;  // (fast exits before any layout math)
    if (b->uploaded && !b->dynamic && ((g_frame_counter + ((uintptr_t)b >> 6)) & 7)) return;
    const FormatInfo *fi = t->layout.fmt;
    MtlFormat mf = mtl_format(t->format);
    uint64_t total = tegra_total_size(&t->layout);
    // CPU-written textures (e.g. font glyph caches) change sparsely, so a strided sample can miss
    // updates. Check at most once per frame; textures seen changing get a full hash every frame,
    // the rest a full hash on a rotating ~16 MB/frame budget plus a cheap sample otherwise.
    if (b->uploaded && b->checked_frame == g_frame_counter) return;
    // Textures that have never changed since upload (almost all: loaded from files) are re-checked every 8th
    // frame, staggered per texture; ones seen changing (glyph caches, video...) every frame.
    if (b->uploaded && !b->dynamic && ((g_frame_counter + ((uintptr_t)b >> 6)) & 7)) return;
    b->checked_frame = g_frame_counter;
    // linear textures are the CPU-written kind (the font glyph cache, 1024x2048 R8, video): checked in full every
    // frame from the start - a strided sample missed newly rasterized glyphs, so characters of new text popped in
    // seconds later (user 2026-10-08: file select "P ease se e t a i e.")
    if (t->flags & 0x10) b->dynamic = true;
    if (full_hash_budget_frame != g_frame_counter) { full_hash_budget_frame = g_frame_counter; full_hash_budget = 4 << 20; }
    const uint8_t *mem = (const uint8_t *)(uintptr_t)t->addr;
    static int always_full = -1;
    if (always_full < 0) always_full = getenv("LARECOMP_TEX_FULLHASH") ? 1 : 0;
    bool full = b->dynamic || !b->uploaded || always_full;
    if (!full && g_frame_counter - b->full_frame >= 120 && full_hash_budget >= total) { full = true; full_hash_budget -= total; }
    if (!b->tp) b->tp = texpatch_identify(t) + 1;  // (button prompts / Hero Mode icon: re-uploaded when their look changes)
    bool restyle = b->tp > 0 && b->uploaded && b->tp_gen != texpatch_generation();
    if (!full && !restyle) {
        uint64_t sh = fnv_sample(mem, total);
        if (sh == b->sample_hash) return;
    }
    uint64_t h = hash_full(mem, total);
    b->full_frame = g_frame_counter;
    b->sample_hash = fnv_sample(mem, total);
    if (b->uploaded && h == b->data_hash && !restyle) return;
    if (b->uploaded && h != b->data_hash) b->dynamic = true;
    if (b->tp > 0) b->tp_gen = texpatch_generation();
    b->data_hash = h;
    b->uploaded = true;
    // callers without an encoder get a private upload command buffer, created only when needed
    id<MTLCommandBuffer> own_cb = nil;
    if (!blit) { own_cb = [queue commandBuffer]; blit = [own_cb blitCommandEncoder]; }
    uint32_t layers = t->layout.layers ? t->layout.layers : 1;
    uint64_t layer_size = tegra_layer_size(&t->layout);
    bool linear = (t->flags & 0x10) != 0;
    for (uint32_t layer = 0; layer < layers; layer++) {
        for (uint32_t lv = 0; lv < t->layout.levels; lv++) {
            LevelLayout l = tegra_level(&t->layout, lv);
            uint32_t row = l.wb * fi->bpb;
            uint64_t sz = (uint64_t)row * l.hb * l.d;
            uint8_t *lin = malloc(sz);
            const uint8_t *src = (const uint8_t *)(uintptr_t)t->addr + layer * layer_size + l.offset;
            if (linear) {
                for (uint32_t y = 0; y < l.hb; y++) memcpy(lin + (uint64_t)y * row, src + y * t->stride, row);
            } else {
                tegra_deswizzle(src, lin, l.wb, l.hb, l.d, fi->bpb, l.block_height_log2, l.block_depth_log2);
            }
            if (b->tp > 0 && layer == 0) texpatch_apply(b->tp - 1, lv, lin, l.w, l.h, row);
            uint8_t *data = lin;
            uint32_t out_row = row, out_h = l.hb;  // (rows of blocks; of texels once decoded)
            int bck = g_bc_soft ? bcsoft_kind(t->format) : 0;
            if (bck) {  // BC on the CPU (see g_bc_soft)
                int ts = bcsoft_texel_size(bck);
                data = malloc((uint64_t)l.w * l.h * l.d * ts);
                bcsoft_decode(bck, lin, l.w, l.h, l.d, data);
                free(lin);
                lin = data;
                out_row = l.w * ts;
                out_h = l.h;
            }
            if (g_astc_soft && astc_format(t->format) && fi->bpb == 16) {  // ASTC on the CPU (see g_astc_soft)
                static int astc_log;
                if (!astc_log++) LOG("[metal] ASTC %#x %ux%u decoded on the CPU\n", t->format, l.w, l.h);
                uint32_t d = l.d ? l.d : 1;
                uint64_t bslice = (uint64_t)row * l.hb;  // (one depth slice of blocks)
                data = malloc((uint64_t)l.w * l.h * d * 4);
                for (uint32_t z = 0; z < d; z++)
                    astc_decode_ldr(lin + z * bslice, l.w, l.h, fi->bw, fi->bh, t->format >= 0x87, data + (uint64_t)z * l.w * l.h * 4);
                free(lin);
                lin = data;
                out_row = l.w * 4;
                out_h = l.h;
            }
            static int bc5log;  // LARECOMP_LOG_BC5: endpoint bytes of BC5 (0x4B/0x4C) textures (unorm ~128 vs snorm ~0)
            if ((t->format == 0x4B || t->format == 0x4C) && lv == 0 && bc5log < 12 && getenv("LARECOMP_LOG_BC5")) {
                bc5log++;
                int hist[4] = {0};
                for (uint64_t i = 0; i + 16 <= sz; i += 16) { hist[lin[i] >> 6]++; hist[lin[i + 8] >> 6]++; }
                LOG("[bc5] fmt %#x %ux%u endpoint0 quartiles (0-63,64-127,128-191,192-255): %d %d %d %d\n", t->format, l.w, l.h,
                    hist[0], hist[1], hist[2], hist[3]);
            }
            if (mf.expand_rgb && fi->bpb == 3) {  // RGB8 -> RGBA8
                uint64_t n = (uint64_t)l.wb * l.hb * l.d;
                data = malloc(n * 4);
                for (uint64_t i = 0; i < n; i++) {
                    data[i * 4] = lin[i * 3]; data[i * 4 + 1] = lin[i * 3 + 1]; data[i * 4 + 2] = lin[i * 3 + 2];
                    data[i * 4 + 3] = 255;
                }
                out_row = l.wb * 4;
                free(lin);
                lin = data;
            }
            uint64_t out_sz = (uint64_t)out_row * out_h * l.d;
            id<MTLBuffer> staging = [dev newBufferWithBytes:data length:out_sz options:MTLResourceStorageModeShared];
            free(lin);
            // the copy stays inside the texture's level: one allocated at the internal resolution (16:9, render
            // target sized - the save slots' screenshots on the title screen) can still be filled from the CPU, and its
            // full-size data overran the scaled texture (a GPU fault on Windows: runtime/nvn/vulkan/textures.c)
            NSUInteger lw = MAX(b->tex.width >> lv, 1), lh = MAX(b->tex.height >> lv, 1), ld = MAX(b->tex.depth >> lv, 1);
            MTLSize size = MTLSizeMake(MIN((NSUInteger)l.w, lw), MIN((NSUInteger)l.h, lh), MIN((NSUInteger)l.d, ld));
            NSUInteger slice = layer;
            [blit copyFromBuffer:staging sourceOffset:0 sourceBytesPerRow:out_row
                 sourceBytesPerImage:(NSUInteger)out_row * out_h sourceSize:size toTexture:b->tex
                    destinationSlice:slice destinationLevel:lv destinationOrigin:MTLOriginMake(0, 0, 0)];
        }
    }
    if (own_cb) { [blit endEncoding]; [own_cb commit]; }
}

// A texture rendered to before it was ever sampled starts from what its memory holds, as on the GPU: SMM2 loads
// each 2D style's tile sheet from a file and then draws the animated tiles into the same texture every frame;
// without the upload only the animated cells existed and the ground and blocks were missing.
static void first_render_use(NvnTexture *t, TexBackend *b, id<MTLBlitCommandEncoder> blit) {
    if (b->gpu_owned || b->uploaded || b->scalable || !t->addr || t->target == 10 || renderer_format_is_depth(t->format)) return;
    const uint64_t *m = (const uint64_t *)(uintptr_t)t->addr;
    uint64_t n = tegra_total_size(&t->layout) / 8;
    bool data = false;
    for (uint64_t i = 0; i < n && !data; i++) data = m[i] != 0;
    if (data) upload_texture(t, b, blit);
}

MTLPixelFormat renderer_texture_pixel_format(NvnTexture *t) {
    const TexBackend *b = (const TexBackend *)t->backend;
    return b && b->tex ? b->tex.pixelFormat : mtl_format(t->format).pf;
}

id<MTLTexture> renderer_texture(NvnTexture *t, id<MTLBlitCommandEncoder> blit, bool for_render) {
    TexBackend *b = tex_backend(t);
    if (for_render) { first_render_use(t, b, blit); b->gpu_owned = true; }
    else if (!b->gpu_owned) upload_texture(t, b, blit);
    return b->tex;
}

void renderer_mark_gpu_owned(NvnTexture *t) {
    TexBackend *b = tex_backend(t);
    first_render_use(t, b, nil);
    b->gpu_owned = true;
}
bool renderer_is_gpu_owned(NvnTexture *t) { return tex_backend(t)->gpu_owned; }

void backend_pool_written(uint64_t addr, uint64_t size) { (void)addr; (void)size; }

// Host-produced texel data (decoded video): upload straight into the Metal texture.
void renderer_upload_rgba(NvnTexture *t, const void *rgba, uint32_t w, uint32_t h, uint32_t stride) {
    TexBackend *b = tex_backend(t);
    b->gpu_owned = true;
    @autoreleasepool {
        id<MTLBuffer> staging = [dev newBufferWithBytes:rgba length:(NSUInteger)stride * h options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromBuffer:staging sourceOffset:0 sourceBytesPerRow:stride sourceBytesPerImage:(NSUInteger)stride * h
                  sourceSize:MTLSizeMake(MIN(w, b->tex.width), MIN(h, b->tex.height), 1) toTexture:b->tex
            destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        [cb commit];
    }
}

// ================================================================== init / window
static const char *present_src =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct V { float4 pos [[position]]; float2 uv; };\n"
    "vertex V vs(uint vid [[vertex_id]], constant float4 *pv [[buffer(0)]]) {\n"
    "  float2 p = float2((vid << 1) & 2, vid & 2);\n"
    "  V o; o.pos = float4(p * 2.0 - 1.0, 0, 1);\n"
    "  float2 uv = float2(p.x, pv[0].x > 0.5 ? p.y : 1.0 - p.y); o.uv = pv[1].xy + uv * pv[1].zw; return o; }\n"
    "fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]],\n"
    "                   constant float4 &solid [[buffer(1)]]) {\n"
    "  if (solid.w > 0.5) return solid;\n"  // LARECOMP_PRESENT_SOLID: draw a flat color (present-path test)
    "  return float4(t.sample(s, in.uv).rgb, 1.0); }\n"
    "fragment float4 fs_alpha(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
    "  return float4(float3(t.sample(s, in.uv).a), 1.0); }\n";

// The guest heap as Metal buffers, one per 2 GiB chunk: this driver (NVIDIA/MetalOld) silently reads zeros from a
// no-copy buffer larger than 2 GiB (a 3 GiB wrap gave every draw zeroed vertices, so the whole game image was black).
// A device whose buffers can't be that large (iOS) gets smaller chunks, or fewer than the heap's size: the log says so.
static void wrap_guest_heap(void) {
    for (int i = 0; i < 4; i++) g_guest_chunks[i] = nil;
    g_guest_chunk_count = 0;
    g_guest_wrapped = 0;
    uint64_t len = GUEST_CHUNK_MAX;
    if ((uint64_t)dev.maxBufferLength < len) len = (uint64_t)dev.maxBufferLength & ~0x3fffull;
    g_guest_chunk_len = len;
    while (len && g_guest_wrapped < HEAP_MAX && g_guest_chunk_count < 4) {
        uint64_t n = HEAP_MAX - g_guest_wrapped < len ? HEAP_MAX - g_guest_wrapped : len;
        id<MTLBuffer> b = [dev newBufferWithBytesNoCopy:(void *)(uintptr_t)(HEAP_BASE + g_guest_wrapped)
                                                length:n options:MTLResourceStorageModeShared deallocator:nil];
        if (!b) break;
        g_guest_chunks[g_guest_chunk_count++] = b;
        g_guest_wrapped += n;
    }
    g_guest_buffer = g_guest_chunk_count ? g_guest_chunks[0] : nil;
    g_guest_buffer_base = HEAP_BASE;
    if (g_guest_wrapped < HEAP_MAX)
        LOG("[metal] guest heap: %llu MB wrapped of %llu (%d chunks of at most %llu MB)\n", g_guest_wrapped >> 20,
            (uint64_t)HEAP_MAX >> 20, g_guest_chunk_count, len >> 20);
}

// The chunk holding guest range [addr, addr+size), and the offset to bind it with; nil when that range isn't inside
// one chunk of the wrapped heap (the caller copies such data instead).
id<MTLBuffer> guest_buffer_chunk(uint64_t addr, uint64_t size, uint64_t *offset) {
    if (!g_guest_chunk_len || addr < HEAP_BASE || addr - HEAP_BASE >= g_guest_wrapped) return nil;
    uint64_t rel = addr - HEAP_BASE, i = rel / g_guest_chunk_len, in = rel - i * g_guest_chunk_len;
    if (i >= (uint64_t)g_guest_chunk_count) return nil;
    uint64_t clen = i + 1 < (uint64_t)g_guest_chunk_count ? g_guest_chunk_len : g_guest_wrapped - i * g_guest_chunk_len;
    if (in + size > clen) return nil;
    *offset = in;
    return g_guest_chunks[i];
}

void backend_init(void) {
    while (!atomic_load(&ui_ready)) usleep(1000);
    queue = [dev newCommandQueue];
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(present_src) options:nil error:&err];
    if (!lib) FATAL("present shader: %s\n", err.localizedDescription.UTF8String);
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"vs"];
    pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
    pd.colorAttachments[0].pixelFormat = layer.pixelFormat;
    present_pipe = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
    present_sampler = [dev newSamplerStateWithDescriptor:sd];
    // Wrap the guest heap: memory pools live there, and GPU addresses are guest addresses.
    wrap_guest_heap();
    if (!g_guest_buffer) FATAL("cannot wrap guest heap in a Metal buffer (max %llu)\n", (uint64_t)dev.maxBufferLength);
    renderer_init(dev);
    if (getenv("LARECOMP_ASPECT_TEST")) {  // experiment: widen the game's 16:9 aspect constant
        float *k = (float *)(uintptr_t)0x70016afba4ull;
        mprotect((void *)((uintptr_t)k & ~0x3fffull), 0x4000, PROT_READ | PROT_WRITE);
        LOG("[aspect] constant %f -> %f\n", *k, (float)atof(getenv("LARECOMP_ASPECT_TEST")));
        *k = (float)atof(getenv("LARECOMP_ASPECT_TEST"));
    }
    LOG("[metal] %s, heap buffer %llu MB\n", dev.name.UTF8String, (uint64_t)HEAP_MAX >> 20);
}

id<MTLCommandQueue> renderer_queue(void) { return queue; }

// ---- in-place reboot (runtime/main.c teardown; main thread, guest threads stopped)
static NvnTexture *g_last_presented;
// Every game texture goes (the NvnTexture structs themselves are leaked: guest memory pointed at them), the
// renderer drops what is keyed by guest addresses / objects, and the heap wrapper is let go: guest memory is
// about to be replaced (backend_guest_memory_reset wraps the new pages).
void backend_reset(void) {
    if (!queue) return;  // the game never initialised NVN
    extern void renderer_reset(void);
    extern void renderer_flush(void);
    renderer_flush();  // (everything submitted has completed)
    int n = g_reg_n;
    while (g_reg_n > 0) backend_texture_destroyed(g_reg[g_reg_n - 1]);
    renderer_reset();
    g_last_presented = NULL;
    for (int i = 0; i < 4; i++) g_guest_chunks[i] = nil;
    g_guest_chunk_count = 0;
    g_guest_wrapped = 0;
    g_guest_buffer = nil;
    LOG("[metal] reboot: %d textures released (%d left alive, %.0f MB)\n", n, g_tex_live, g_tex_live_bytes / 1048576.0);
}

void backend_guest_memory_reset(void) {
    if (!queue) return;
    wrap_guest_heap();
    if (!g_guest_buffer) FATAL("cannot wrap guest heap in a Metal buffer\n");
}

// The window goes black while the game is down (instead of holding its last frame).
void backend_clear_window(void) {
    if (!layer || !dev) return;
    @autoreleasepool {
        id<CAMetalDrawable> drawable = [layer nextDrawable];
        if (!drawable) return;
        id<MTLCommandQueue> q = queue ? queue : [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = drawable.texture;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
        [cb presentDrawable:drawable];
        [cb commit];
        [cb waitUntilCompleted];
    }
}

// ================================================================== screenshots
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

void renderer_save_texture(id<MTLTexture> tex, const char *path);

// Scanout shows the bytes the game wrote: sample sRGB textures through a non-sRGB view so the
// present/screenshot shaders pass them through instead of decoding to linear.
static id<MTLTexture> raw_view(id<MTLTexture> t) {
    MTLPixelFormat pf = t.pixelFormat;
    MTLPixelFormat raw = pf == MTLPixelFormatRGBA8Unorm_sRGB ? MTLPixelFormatRGBA8Unorm
                       : pf == MTLPixelFormatBGRA8Unorm_sRGB ? MTLPixelFormatBGRA8Unorm : pf;
    if (raw == pf) return t;
    id<MTLTexture> v = [t newTextureViewWithPixelFormat:raw];
    return v ? v : t;
}

static void save_png(id<MTLTexture> tex, const char *path) { renderer_save_texture(tex, path); }

// MetalFX spatial upscaling (Video tab "Upscaler" = MetalFX). metalfx_upscale() decides (and sets up the scaler for
// this input / output size); metalfx_encode() runs it and returns the upscaled image.
#ifdef HAVE_METALFX
static id<MTLFXSpatialScaler> g_fx;
static id<MTLTexture> g_fx_out;
static NSUInteger g_fx_iw, g_fx_ih, g_fx_ow, g_fx_oh;
static MTLPixelFormat g_fx_fmt;
#endif
static id<MTLTexture> metalfx_upscale(id<MTLCommandBuffer> cb, id<MTLTexture> in, double ow_d, double oh_d) {
    (void)cb;
    if (g_settings.upscaler != UPSCALE_METALFX) return nil;
#ifndef HAVE_METALFX
    (void)in, (void)ow_d, (void)oh_d;
    return nil;
#else
    static int supported = -1;
    if (supported < 0) {
        supported = [MTLFXSpatialScalerDescriptor supportsDevice:dev] ? 1 : 0;
        LOG("[metalfx] spatial scaler %s\n", supported ? "available" : "not supported on this GPU");
    }
    if (!supported) return nil;
    NSUInteger ow = (NSUInteger)lround(ow_d), oh = (NSUInteger)lround(oh_d);
    if (ow <= in.width + 1 && oh <= in.height + 1) return nil;  // not an upscale: plain sampling is right
    if (ow > 16384) ow = 16384;
    if (oh > 16384) oh = 16384;
    if (!g_fx || g_fx_iw != in.width || g_fx_ih != in.height || g_fx_ow != ow || g_fx_oh != oh || g_fx_fmt != in.pixelFormat) {
        MTLFXSpatialScalerDescriptor *d = [MTLFXSpatialScalerDescriptor new];
        d.inputWidth = in.width; d.inputHeight = in.height;
        d.outputWidth = ow; d.outputHeight = oh;
        d.colorTextureFormat = in.pixelFormat;
        d.outputTextureFormat = in.pixelFormat;
        d.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;  // (gamma-encoded 8-bit image)
        g_fx = [d newSpatialScalerWithDevice:dev];
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:in.pixelFormat width:ow height:oh mipmapped:NO];
        td.usage = g_fx ? g_fx.outputTextureUsage | MTLTextureUsageShaderRead : MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        g_fx_out = [dev newTextureWithDescriptor:td];
        g_fx_iw = in.width; g_fx_ih = in.height; g_fx_ow = ow; g_fx_oh = oh; g_fx_fmt = in.pixelFormat;
        LOG("[metalfx] %lux%lu -> %lux%lu (%s)\n", (unsigned long)in.width, (unsigned long)in.height, (unsigned long)ow,
            (unsigned long)oh, g_fx ? "ok" : "FAILED");
    }
    return g_fx && g_fx_out ? g_fx_out : nil;
#endif
}
// SMAA 1x (Video tab "Anti-aliasing"; third_party/smaa, MIT; as runtime/nvn/vulkan/shaders.c / exec.c): luma edges ->
// blending weights -> neighbourhood blending, at the game image's size, before the present / MetalFX. The reference
// SMAA.hlsl has no Metal mode: it is built through its SMAA_CUSTOM_SL porting macros, with a few textual fixes for
// MSL (out / inout parameters and array parameters as references, no swizzles bound to them, no discard in a
// helper). The render-target size is a literal in the source (SMAA's preferred form): the library is compiled for
// each new image size, on first use - a problem there is logged once and turns SMAA off. Metal's texture origin is
// top-left, as SMAA expects.
static NSString *smaa_msl(NSUInteger w, NSUInteger h) {
    extern const char smaa_hlsl_src[];
    NSMutableString *s = [NSMutableString stringWithUTF8String:smaa_hlsl_src];
    struct { const char *from, *to; } fix[] = {
        {"inout float2 variable", "thread float2 &variable"},
        {"inout float4 variable", "thread float4 &variable"},
        {"SMAAMovc(cond.xy, variable.xy, value.xy);", "variable = select(variable, value, cond);"},
        {"SMAAMovc(cond.zw, variable.zw, value.zw);", ""},
        {"out float4 offset[3]", "thread float4 (&offset)[3]"},
        {"float4 offset[3]", "thread float4 (&offset)[3]"},
        {"thread thread float4 (&offset)[3]", "thread float4 (&offset)[3]"},
        {"out float2 pixcoord", "thread float2 &pixcoord"},
        {"out float4 offset)", "thread float4 &offset)"},
        {"out float2 e)", "thread float2 &e)"},
        {"inout float2 weights", "thread float2 &weights"},
        {"SMAADetectHorizontalCornerPattern(SMAATexturePass2D(edgesTex), weights.rg, coords.xyzy, d);",
         "{ float2 w_ = weights.rg; SMAADetectHorizontalCornerPattern(SMAATexturePass2D(edgesTex), w_, coords.xyzy, d); weights.rg = w_; }"},
        {"SMAADetectVerticalCornerPattern(SMAATexturePass2D(edgesTex), weights.ba, coords.xyxz, d);",
         "{ float2 w_ = weights.ba; SMAADetectVerticalCornerPattern(SMAATexturePass2D(edgesTex), w_, coords.xyxz, d); weights.ba = w_; }"},
        {"        discard;", "        return float2(0.0, 0.0);"},
    };
    for (size_t i = 0; i < sizeof fix / sizeof fix[0]; i++)
        [s replaceOccurrencesOfString:@(fix[i].from) withString:@(fix[i].to) options:0 range:NSMakeRange(0, s.length)];
    NSString *head = [NSString stringWithFormat:
        @"#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "#define SMAA_CUSTOM_SL\n"
        "#define SMAA_PRESET_HIGH\n"
        "#define SMAA_RT_METRICS float4(%.9g, %.9g, %.1f, %.1f)\n"
        "constexpr sampler LinearSampler(filter::linear, address::clamp_to_edge);\n"
        "constexpr sampler PointSampler(filter::nearest, address::clamp_to_edge);\n"
        "#define SMAATexture2D(tex) texture2d<float> tex\n"
        "#define SMAATexturePass2D(tex) tex\n"
        "#define SMAASampleLevelZero(tex, coord) tex.sample(LinearSampler, coord, level(0.0))\n"
        "#define SMAASampleLevelZeroPoint(tex, coord) tex.sample(PointSampler, coord, level(0.0))\n"
        "#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.sample(LinearSampler, coord, level(0.0), offset)\n"
        "#define SMAASample(tex, coord) tex.sample(LinearSampler, coord)\n"
        "#define SMAASamplePoint(tex, coord) tex.sample(PointSampler, coord)\n"
        "#define SMAASampleOffset(tex, coord, offset) tex.sample(LinearSampler, coord, offset)\n"
        "#define SMAA_FLATTEN\n"
        "#define SMAA_BRANCH\n"
        "#define lerp(a, b, t) mix(a, b, t)\n"
        "#define mad(a, b, c) fma(a, b, c)\n",
        1.0 / w, 1.0 / h, (double)w, (double)h];
    NSString *tail =
        @"struct V { float4 pos [[position]]; };\n"
        "vertex V smaa_vs(uint vid [[vertex_id]]) {\n"
        "  float2 p = float2((vid << 1) & 2, vid & 2);\n"
        "  V o; o.pos = float4(p * 2.0 - 1.0, 0, 1); return o; }\n"
        "fragment float4 smaa_edge(V in [[stage_in]], texture2d<float> t0 [[texture(0)]]) {\n"
        "  float2 uv = in.pos.xy * SMAA_RT_METRICS.xy;\n"
        "  float4 off[3];\n"
        "  SMAAEdgeDetectionVS(uv, off);\n"
        "  return float4(SMAALumaEdgeDetectionPS(uv, off, t0), 0.0, 0.0); }\n"
        "fragment float4 smaa_weight(V in [[stage_in]], texture2d<float> t0 [[texture(0)]], texture2d<float> t1 [[texture(1)]],\n"
        "                            texture2d<float> t2 [[texture(2)]]) {\n"
        "  float2 uv = in.pos.xy * SMAA_RT_METRICS.xy, pix;\n"
        "  float4 off[3];\n"
        "  SMAABlendingWeightCalculationVS(uv, pix, off);\n"
        "  return SMAABlendingWeightCalculationPS(uv, pix, off, t0, t1, t2, float4(0.0)); }\n"
        "fragment float4 smaa_blend(V in [[stage_in]], texture2d<float> t0 [[texture(0)]], texture2d<float> t1 [[texture(1)]]) {\n"
        "  float2 uv = in.pos.xy * SMAA_RT_METRICS.xy;\n"
        "  float4 off;\n"
        "  SMAANeighborhoodBlendingVS(uv, off);\n"
        "  return float4(SMAANeighborhoodBlendingPS(uv, off, t0, t1).rgb, 1.0); }\n";
    return [NSString stringWithFormat:@"%@%@%@", head, s, tail];
}

static id<MTLTexture> smaa_target(id<MTLTexture> old, NSUInteger w, NSUInteger h, MTLPixelFormat f) {
    if (old && old.width == w && old.height == h && old.pixelFormat == f) return old;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    return [dev newTextureWithDescriptor:td];
}

static void smaa_pass(id<MTLCommandBuffer> cb, id<MTLRenderPipelineState> p, id<MTLTexture> dst, bool clear,
                      id<MTLTexture> t0, id<MTLTexture> t1, id<MTLTexture> t2) {
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = dst;
    rp.colorAttachments[0].loadAction = clear ? MTLLoadActionClear : MTLLoadActionDontCare;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:p];
    [e setFragmentTexture:t0 atIndex:0];
    if (t1) [e setFragmentTexture:t1 atIndex:1];
    if (t2) [e setFragmentTexture:t2 atIndex:2];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
}

static id<MTLTexture> smaa_encode(id<MTLCommandBuffer> cb, id<MTLTexture> in) {
    static bool failed;  // (shader / pipeline error: logged once, SMAA stays off)
    static NSUInteger lw, lh;
    static MTLPixelFormat lf;
    static id<MTLRenderPipelineState> pe, pw, pb;
    static id<MTLTexture> area, search, edges, weights, out;
    if (failed) return in;
    if (!area) {  // the lookup textures (runtime/nvn/smaa_data.c), uploaded once
        extern const unsigned char *smaa_area_tex(void);
        extern const unsigned char *smaa_search_tex(void);
        MTLTextureDescriptor *ad = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG8Unorm width:160 height:560 mipmapped:NO];
        area = [dev newTextureWithDescriptor:ad];
        [area replaceRegion:MTLRegionMake2D(0, 0, 160, 560) mipmapLevel:0 withBytes:smaa_area_tex() bytesPerRow:160 * 2];
        MTLTextureDescriptor *sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:64 height:16 mipmapped:NO];
        search = [dev newTextureWithDescriptor:sd];
        [search replaceRegion:MTLRegionMake2D(0, 0, 64, 16) mipmapLevel:0 withBytes:smaa_search_tex() bytesPerRow:64];
    }
    if (!pe || lw != in.width || lh != in.height || lf != in.pixelFormat) {
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:smaa_msl(in.width, in.height) options:nil error:&err];
        if (!lib) { LOG("[smaa] shader: %s\n", err.localizedDescription.UTF8String); failed = true; return in; }
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"smaa_vs"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        pd.fragmentFunction = [lib newFunctionWithName:@"smaa_edge"];
        pe = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
        pd.fragmentFunction = [lib newFunctionWithName:@"smaa_weight"];
        pw = pe ? [dev newRenderPipelineStateWithDescriptor:pd error:&err] : nil;
        pd.fragmentFunction = [lib newFunctionWithName:@"smaa_blend"];
        pd.colorAttachments[0].pixelFormat = in.pixelFormat;
        pb = pw ? [dev newRenderPipelineStateWithDescriptor:pd error:&err] : nil;
        if (!pb) { LOG("[smaa] pipeline: %s\n", err.localizedDescription.UTF8String); failed = true; return in; }
        lw = in.width; lh = in.height; lf = in.pixelFormat;
        LOG("[smaa] %lux%lu\n", (unsigned long)in.width, (unsigned long)in.height);
    }
    edges = smaa_target(edges, in.width, in.height, MTLPixelFormatRGBA8Unorm);
    weights = smaa_target(weights, in.width, in.height, MTLPixelFormatRGBA8Unorm);
    out = smaa_target(out, in.width, in.height, in.pixelFormat);
    smaa_pass(cb, pe, edges, true, in, nil, nil);
    smaa_pass(cb, pw, weights, true, edges, area, search);
    smaa_pass(cb, pb, out, false, in, weights, nil);
    return out;
}

static id<MTLTexture> metalfx_encode(id<MTLCommandBuffer> cb, id<MTLTexture> in) {
#ifndef HAVE_METALFX
    (void)cb;
    return in;
#else
    g_fx.colorTexture = in;
    g_fx.inputContentWidth = in.width;
    g_fx.inputContentHeight = in.height;
    g_fx.outputTexture = g_fx_out;
    [g_fx encodeToCommandBuffer:cb];
    return g_fx_out;
#endif
}



// Debug: dump an 8-bit-per-channel 2D texture's raw bytes (alpha kept, written as a separate grey PNG).
void renderer_save_texture_alpha(id<MTLTexture> tex, const char *path) {
    if (tex.pixelFormat != MTLPixelFormatRGBA8Unorm && tex.pixelFormat != MTLPixelFormatRGBA8Unorm_sRGB &&
        tex.pixelFormat != MTLPixelFormatBGRA8Unorm)
        return;
    NSUInteger w = tex.width, h = tex.height;
    id<MTLBuffer> buf = [dev newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
              toBuffer:buf destinationOffset:0 destinationBytesPerRow:w * 4 destinationBytesPerImage:w * h * 4];
    [b endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    const uint8_t *px = buf.contents;
    uint8_t *a = malloc(w * h);
    unsigned amax = 0;
    for (NSUInteger i = 0; i < w * h; i++) { a[i] = px[i * 4 + 3]; if (a[i] > amax) amax = a[i]; }
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceGray();
    CGContextRef ctx = CGBitmapContextCreate(a, w, h, 8, w, cs, kCGImageAlphaNone);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, (__bridge CFStringRef)UTTypePNG.identifier, 1, NULL);
    if (dest) {  // (NULL: the folder isn't there)
        CGImageDestinationAddImage(dest, img, NULL);
        CGImageDestinationFinalize(dest);
        CFRelease(dest);
    }
    CFRelease(url); CGImageRelease(img); CGContextRelease(ctx); CGColorSpaceRelease(cs);
    free(a);
    LOG("[metal] saved alpha %s (%lux%lu, max %u)\n", path, (unsigned long)w, (unsigned long)h, amax);
}
static float g_shot_crop[4] = {0, 0, 1, 1};  // presented crop of the current frame (screenshots match it)
static bool g_save_upright;  // (set around saves of the window image: test shots under any name)
void renderer_save_texture(id<MTLTexture> tex, const char *path) {
    NSUInteger w = tex.width, h = tex.height;
    float shot_crop[4] = {0, 0, 1, 1};
    if (strstr(path, "frame_")) {
        memcpy(shot_crop, g_shot_crop, sizeof shot_crop);
        w = (NSUInteger)(w * shot_crop[2]); h = (NSUInteger)(h * shot_crop[3]);
    }
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                 width:w height:h mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModeShared;
    id<MTLTexture> dst = [dev newTextureWithDescriptor:d];
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = dst;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    static id<MTLRenderPipelineState> pipe, pipe_alpha;  // (a path ending in "_alpha.png": the alpha channel as grey)
    if (!pipe) {
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        id<MTLLibrary> lib = [dev newLibraryWithSource:@(present_src) options:nil error:nil];
        pd.vertexFunction = [lib newFunctionWithName:@"vs"];
        pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        pipe = [dev newRenderPipelineStateWithDescriptor:pd error:nil];
        pd.fragmentFunction = [lib newFunctionWithName:@"fs_alpha"];
        pipe_alpha = [dev newRenderPipelineStateWithDescriptor:pd error:nil];
    }
    bool want_alpha = strstr(path, "_alpha.png") != NULL;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    float flip = renderer_origin_lower_left() ? 1.0f : 0.0f;
    [e setRenderPipelineState:want_alpha && pipe_alpha ? pipe_alpha : pipe];
    if (g_save_upright || strstr(path, "window")) flip = 0;  // the window image is already upright
    float pv[8] = {flip, 0, 0, 0, shot_crop[0], shot_crop[1], shot_crop[2], shot_crop[3]};
    [e setVertexBytes:pv length:sizeof pv atIndex:0];
    { float zero[4] = {0, 0, 0, 0}; [e setFragmentBytes:zero length:sizeof zero atIndex:1]; }  // (fs: no solid override)
    [e setFragmentTexture:raw_view(tex) atIndex:0];
    [e setFragmentSamplerState:present_sampler atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    // Read back through a blit into a shared buffer: some drivers (the NVIDIA/MetalOld one) return zeros from
    // getBytes on a texture, which made every saved PNG black.
    id<MTLBuffer> rb = [dev newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> bl2 = [cb blitCommandEncoder];
    [bl2 copyFromTexture:dst sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
              sourceSize:MTLSizeMake(w, h, 1) toBuffer:rb destinationOffset:0
    destinationBytesPerRow:w * 4 destinationBytesPerImage:w * h * 4];
    [bl2 endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    uint8_t *px = malloc(w * h * 4);
    memcpy(px, rb.contents, w * h * 4);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(px, w, h, 8, w * 4, cs, kCGImageAlphaPremultipliedLast);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, (__bridge CFStringRef)UTTypePNG.identifier, 1, NULL);
    if (dest) {  // (NULL: the folder isn't there)
        CGImageDestinationAddImage(dest, img, NULL);
        CGImageDestinationFinalize(dest);
        CFRelease(dest);
    }
    CFRelease(url); CGImageRelease(img); CGContextRelease(ctx); CGColorSpaceRelease(cs);
    free(px);
    LOG("[metal] saved %s (%lux%lu)\n", path, (unsigned long)w, (unsigned long)h);
}

// ================================================================== present

static double last_fps_time;
static _Atomic float g_display_hz;  // the window's display refresh (ui_run)
static _Atomic uint64_t g_fg_shown;  // in-between frames the display actually showed (a presented handler with a time)

void backend_present(NvnTexture *t);
static bool g_held;          // (backend_present from backend_represent: no generated frame)
static bool g_back_from_bg;  // (the first present after the iOS background: none either)
// Present the last frame again (the game thread waits in a host dialog, e.g. the software keyboard): the overlay
// keeps drawing and taking input.
void backend_represent(void) {
    g_held = true;
    if (g_last_presented) backend_present(g_last_presented);
    else SDL_Delay(16);
    g_held = false;
}

// Our window's image from the window server (CGWindowListCreateImage, looked up at run time: it is deprecated in
// favour of ScreenCaptureKit, but a process may still capture its own windows with it) -> PNG.
#if !TARGET_OS_IPHONE
static void window_server_shot(const char *path) {
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    CGWindowID wid = 0;
    for (CFIndex i = 0; list && i < CFArrayGetCount(list) && !wid; i++) {
        CFDictionaryRef w = CFArrayGetValueAtIndex(list, i);
        int pid = 0, layer_no = -1;
        CFNumberRef n = CFDictionaryGetValue(w, kCGWindowOwnerPID);
        if (n) CFNumberGetValue(n, kCFNumberIntType, &pid);
        n = CFDictionaryGetValue(w, kCGWindowLayer);
        if (n) CFNumberGetValue(n, kCFNumberIntType, &layer_no);
        if (pid == getpid() && layer_no == 0 && (n = CFDictionaryGetValue(w, kCGWindowNumber)))
            CFNumberGetValue(n, kCFNumberIntType, &wid);
    }
    if (list) CFRelease(list);
    typedef CGImageRef (*WLCI)(CGRect, uint32_t, uint32_t, uint32_t);
    WLCI fn = (WLCI)dlsym(RTLD_DEFAULT, "CGWindowListCreateImage");
    CGImageRef img = wid && fn ? fn(CGRectNull, kCGWindowListOptionIncludingWindow, wid,
                                    kCGWindowImageBoundsIgnoreFraming | kCGWindowImageNominalResolution) : NULL;
    if (!img) { LOG("[metal] window server shot failed (window %u)\n", wid); return; }
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, (__bridge CFStringRef)UTTypePNG.identifier, 1, NULL);
    if (dest) { CGImageDestinationAddImage(dest, img, NULL); CGImageDestinationFinalize(dest); CFRelease(dest); }
    LOG("[metal] window server shot %s (%zux%zu)\n", path, CGImageGetWidth(img), CGImageGetHeight(img));
    CFRelease(url);
    CGImageRelease(img);
}
#else
static void window_server_shot(const char *path) { LOG("[metal] no window-server capture on iOS (%s)\n", path); }
#endif

#if TARGET_OS_IPHONE
// iOS refuses GPU work from an app in the background: the game holds at its next frame until the app is back
// (set from SDL's event watch, which runs as the app changes state, not when the main loop gets to the event)
static atomic_bool g_in_background;
static bool SDLCALL app_state_watch(void *userdata, SDL_Event *e) {
    (void)userdata;
    if (e->type == SDL_EVENT_WILL_ENTER_BACKGROUND) atomic_store(&g_in_background, true);
    else if (e->type == SDL_EVENT_DID_ENTER_FOREGROUND) atomic_store(&g_in_background, false);
    return true;
}
static void wait_while_in_background(void) {
    if (!atomic_load(&g_in_background)) return;
    g_back_from_bg = true;
    LOG("[metal] the app is in the background: the game waits\n");
    while (atomic_load(&g_in_background)) usleep(20000);
    LOG("[metal] back in the foreground\n");
}
#endif

void backend_present(NvnTexture *t) {
#if TARGET_OS_IPHONE
    wait_while_in_background();
#endif
    g_last_presented = t;
    extern void renderer_submit(void);
    renderer_submit();  // same queue: the present pass runs after the frame's work
    TexBackend *b = t->backend;
    if (b) b->presented = true;
    extern bool g_frame_hidden;  // (objects.c: a mod's fast-forward, no window image)
    if (!g_frame_hidden) @autoreleasepool {
        uint64_t nd0 = SDL_GetTicksNS();
        id<CAMetalDrawable> drawable = [layer nextDrawable];
        uint64_t nd = SDL_GetTicksNS() - nd0;
        if (nd > 30000000) {
            static int warned;
            if (warned++ < 20) LOG("[metal] nextDrawable blocked %llums\n", (unsigned long long)(nd / 1000000));
        }
        if (!drawable) return;
        __block id<MTLCommandBuffer> cb = [queue commandBuffer];  // (__block: frame generation commits it and starts another)
        // Frame generation (Video tab; runtime/nvn/framegen.h): the game image goes to fg_cur, and from there to an
        // in-between drawable (with fg_prev) and this frame's own
        static id<MTLTexture> fg_prev, fg_cur;
        bool fg_on = g_settings.frame_gen != FRAMEGEN_OFF;
        if (fg_on) {
            for (int i = 0; i < 2; i++) {
                id<MTLTexture> __strong *t = i ? &fg_cur : &fg_prev;
                if (*t && (*t).width == drawable.texture.width && (*t).height == drawable.texture.height &&
                    (*t).pixelFormat == drawable.texture.pixelFormat) continue;
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:drawable.texture.pixelFormat
                                             width:drawable.texture.width height:drawable.texture.height mipmapped:NO];
                td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                td.storageMode = MTLStorageModePrivate;
                *t = [dev newTextureWithDescriptor:td];
            }
        } else {
            fg_prev = fg_cur = nil;
        }
        id<MTLTexture> dst = fg_on ? fg_cur : drawable.texture;
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = dst;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        // Next frame's render resolution follows the window: the 1920x1080 image scaled to fit.
        double dw = drawable.texture.width, dh = drawable.texture.height;
        extern void widescreen_frame_end(uint64_t frame);
        widescreen_frame_end(total_frames);
        extern void widescreen_set_factor(float f);
        extern bool widescreen_allowed(void);
        // render scale + placement in the window: runtime/nvn/output_scale.c (Video tab "Internal resolution")
        extern uint64_t g_gpu_total_ns;
        static uint64_t gpu_prev;
        uint64_t gpu_now = __atomic_load_n(&g_gpu_total_ns, __ATOMIC_RELAXED);
        double gpu_ms = gpu_prev ? (gpu_now - gpu_prev) / 1e6 : -1;
        gpu_prev = gpu_now;
        OutScale os;
        output_scale(dw, dh, widescreen_allowed(), SDL_GetTicksNS() / 1e9, gpu_ms, &os);
        double sx = os.sx, sy = os.sy, vx = os.vx, vy = os.vy, vw = os.vw, vh = os.vh, shape = os.shape;
        bool reshape = shape != 1.0;
        // interiors / dungeons: their 16:9 centre, or the part inside a room wider than one screen
        extern void widescreen_present_crop(uint64_t, double, float *, double *, double *, double *, double *);
        float crop[4];  // uv origin and size
        widescreen_present_crop(total_frames, shape, crop, &vx, &vy, &vw, &vh);
        { extern void input_set_game_rect(double, double, double, double, double, double); input_set_game_rect(vx, vy, vw, vh, dw, dh); }
        widescreen_set_factor((float)shape);
        {   // (diagnostics) log output scale / drawable changes
            static double psx, psy, pdw, pdh;
            static int nlog;
            if ((sx != psx || sy != psy || dw != pdw || dh != pdh) && nlog++ < 200)
                LOG("[scale] frame %llu drawable %.0fx%.0f -> render scale %.3fx%.3f (reshape %d)\n",
                    (unsigned long long)total_frames, dw, dh, sx, sy, reshape);
            psx = sx, psy = sy, pdw = dw, pdh = dh;
        }
        renderer_set_scale((float)sx, (float)sy);
        // LARECOMP_PRESENT_SOLID=r,g,b,a: the present pass draws that flat color instead of the game image
        // (diagnostic: tells a black game image apart from a present pass that never reaches the drawable).
        static float present_solid[4] = {0, 0, 0, 0};
        static int solid_init;
        if (!solid_init) {
            solid_init = 1;
            const char *e = getenv("LARECOMP_PRESENT_SOLID");
            if (e) sscanf(e, "%f,%f,%f,%f", &present_solid[0], &present_solid[1], &present_solid[2], &present_solid[3]);
        }
        static int present_logged;
        if (!present_logged++)
            LOG("[metal] present: texture %p backend %p image %p\n", (void *)t, (void *)b, b ? (__bridge void *)b->tex : NULL);
        if ((b && b->tex) || present_solid[3] > 0.0f) {
            id<MTLTexture> src = (b && b->tex) ? raw_view(b->tex) : nil;
            // SMAA (Video tab "Anti-aliasing"): first, at the game image's size (MetalFX then upscales its result)
            if (src && g_settings.antialias == AA_SMAA) {
                [enc endEncoding];
                src = smaa_encode(cb, src);
                MTLRenderPassDescriptor *rp2 = [MTLRenderPassDescriptor renderPassDescriptor];
                rp2.colorAttachments[0].texture = dst;
                rp2.colorAttachments[0].loadAction = MTLLoadActionLoad;
                rp2.colorAttachments[0].storeAction = MTLStoreActionStore;
                enc = [cb renderCommandEncoderWithDescriptor:rp2];
            }
            // MetalFX (Video tab "Upscaler"): when the game image is smaller than its place in the window, Apple's
            // spatial upscaler brings it to the window's size first (the whole image, so the interior crop below
            // still applies); the present pass then samples it about 1:1. Its own pass: the drawable's pass is
            // ended and resumed (load) around it.
            id<MTLTexture> up = src ? metalfx_upscale(cb, src, vw / crop[2], vh / crop[3]) : nil;
            if (up) {
                [enc endEncoding];
                up = metalfx_encode(cb, src);
                MTLRenderPassDescriptor *rp2 = [MTLRenderPassDescriptor renderPassDescriptor];
                rp2.colorAttachments[0].texture = dst;
                rp2.colorAttachments[0].loadAction = MTLLoadActionLoad;
                rp2.colorAttachments[0].storeAction = MTLStoreActionStore;
                enc = [cb renderCommandEncoderWithDescriptor:rp2];
                src = up;
            }
            float flip = renderer_origin_lower_left() ? 1.0f : 0.0f;
            [enc setViewport:(MTLViewport){vx, vy, vw, vh, 0, 1}];
            [enc setRenderPipelineState:present_pipe];
            float pv[8] = {flip, 0, 0, 0, crop[0], crop[1], crop[2], crop[3]};
            memcpy(g_shot_crop, crop, sizeof crop);
            [enc setVertexBytes:pv length:sizeof pv atIndex:0];
            [enc setFragmentBytes:present_solid length:sizeof present_solid atIndex:1];
            [enc setFragmentTexture:src atIndex:0];
            [enc setFragmentSamplerState:present_sampler atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        }
        [enc endEncoding];
        static long menu_at = -1;  // test hook: open the menu at this frame (LARECOMP_MENU_AT)
        if (menu_at < 0) menu_at = getenv("LARECOMP_MENU_AT") ? atol(getenv("LARECOMP_MENU_AT")) : 0;
        if (menu_at && (long)total_frames + 1 == menu_at) { extern void menu_test_open(void); menu_test_open(); }
        static FgState fg_state;
        static bool fg_last_gen;
        FgFrame ff = {.w = (uint32_t)drawable.texture.width, .h = (uint32_t)drawable.texture.height,
                      .fmt = (uint32_t)drawable.texture.pixelFormat, .now_ns = plat_now_ns(),
                      .display_hz = atomic_load(&g_display_hz), .enabled = fg_on, .held = g_held || g_back_from_bg,
                      .any_pace = getenv("LARECOMP_FRAMEGEN_ANY_PACE") != NULL};
        memcpy(ff.crop, crop, sizeof crop);
        bool fg_gen = framegen_step(&fg_state, &ff);
        g_back_from_bg = false;
        static id<MTLTexture> fg_three;  // (LARECOMP_FRAMEGEN_SHOT: saved once this frame's work is done)
        if (fg_on) {
            static long fg_shot = -1;  // LARECOMP_FRAMEGEN_SHOT=<frame>: prev | generated | cur side by side (LARECOMP_SHOT_PATH)
            if (fg_shot < 0) fg_shot = getenv("LARECOMP_FRAMEGEN_SHOT") ? atol(getenv("LARECOMP_FRAMEGEN_SHOT")) : 0;
            bool shot3 = fg_gen && fg_shot && (long)total_frames + 1 >= fg_shot;
            id<MTLTexture> gen_tex = drawable.texture;
            if (shot3) {
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fg_cur.pixelFormat
                                             width:fg_cur.width height:fg_cur.height mipmapped:NO];
                td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                gen_tex = [dev newTextureWithDescriptor:td];
            }
            if (fg_gen && !fgm_encode(dev, cb, fg_prev, fg_cur, 0.5f, !fg_last_gen, gen_tex)) fg_gen = false;
            fg_last_gen = fg_gen;
            // (a drawable can only be drawn into: copies to it are draws)
            void (^copy_to)(id<MTLTexture>, id<MTLTexture>) = ^(id<MTLTexture> from, id<MTLTexture> to) {
                MTLRenderPassDescriptor *cp = [MTLRenderPassDescriptor renderPassDescriptor];
                cp.colorAttachments[0].texture = to;
                cp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                cp.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> ce = [cb renderCommandEncoderWithDescriptor:cp];
                float cv[8] = {0, 0, 0, 0, 0, 0, 1, 1};
                [ce setRenderPipelineState:present_pipe];
                [ce setVertexBytes:cv length:sizeof cv atIndex:0];
                [ce setFragmentTexture:from atIndex:0];
                [ce setFragmentSamplerState:present_sampler atIndex:0];
                [ce drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [ce endEncoding];
            };
            if (fg_gen) {
                static bool said;
                if (!said) { said = true; LOG("[framegen] on %lux%lu display %.0f Hz\n", (unsigned long)fg_cur.width,
                                              (unsigned long)fg_cur.height, ff.display_hz); }
                if (shot3) {
                    fg_shot = 0;
                    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fg_cur.pixelFormat
                                                 width:fg_cur.width * 3 height:fg_cur.height mipmapped:NO];
                    td.usage = MTLTextureUsageShaderRead;
                    id<MTLTexture> three = [dev newTextureWithDescriptor:td];
                    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
                    id<MTLTexture> parts[3] = {fg_prev, gen_tex, fg_cur};
                    for (int i = 0; i < 3; i++)
                        [bl copyFromTexture:parts[i] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                                 sourceSize:MTLSizeMake(fg_cur.width, fg_cur.height, 1) toTexture:three destinationSlice:0
                           destinationLevel:0 destinationOrigin:MTLOriginMake(fg_cur.width * i, 0, 0)];
                    [bl endEncoding];
                    copy_to(gen_tex, drawable.texture);
                    fg_three = three;
                }
                menu_render(dev, cb, drawable.texture);
                [drawable addPresentedHandler:^(id<MTLDrawable> d) {
                    if (d.presentedTime > 0) atomic_fetch_add(&g_fg_shown, 1);  // (0: replaced before it was shown)
                }];
                [cb presentDrawable:drawable];
                // (committed before the next drawable is asked for: that may wait for a display refresh, and the
                // GPU works on this meanwhile)
                [cb commit];
                cb = [queue commandBuffer];
                drawable = [layer nextDrawable];  // (this frame's own image: shown 1/120 s after the in-between one)
            }
            if (drawable) {
                copy_to(fg_cur, drawable.texture);
                if (fg_gen) menu_redraw(dev, cb, drawable.texture);
                else menu_render(dev, cb, drawable.texture);
            }
            id<MTLTexture> sw = fg_prev;
            fg_prev = fg_cur, fg_cur = sw;
        } else {
            menu_render(dev, cb, drawable.texture);
        }
        if (!drawable) [cb commit];  // (no image for this frame's own present: the in-between one still shows)
        if (drawable) {
        static long win_shot = -1;  // LARECOMP_WINDOW_SHOT=<frame>: save the final window image (game + menu)
        if (win_shot < 0) win_shot = getenv("LARECOMP_WINDOW_SHOT") ? atol(getenv("LARECOMP_WINDOW_SHOT")) : 0;
        id<MTLTexture> shot_tex = nil;
        extern char g_window_shot_request[256];  // test hooks (runtime/hle/hookshot.c): save the next frame here
        if ((win_shot && (long)total_frames + 1 == win_shot) || g_window_shot_request[0]) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:drawable.texture.pixelFormat
                                         width:drawable.texture.width height:drawable.texture.height mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead;
            shot_tex = [dev newTextureWithDescriptor:td];
            id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
            [bl copyFromTexture:drawable.texture toTexture:shot_tex];
            [bl endEncoding];
        }
        if (fg_gen) [cb presentDrawable:drawable afterMinimumDuration:FG_HALF_SLOT_NS / 1e9];
        else [cb presentDrawable:drawable];
        [cb commit];
        {   // LARECOMP_CG_SHOT=<frame> (+ LARECOMP_SHOT_PATH): capture our window as the window server composites it,
            // colour-managed into the display's space - the way the Ryujinx reference grabs its window, so the two
            // compare like for like
            // (LARECOMP_CG_SHOT=<first>:<last>[:<step>]: every step-th frame in between, LARECOMP_SHOT_PATH a printf
            // pattern taking the frame number, e.g. build/cg_%05d.png)
            static long cg_a = -1, cg_b, cg_step = 1;
            if (cg_a < 0) {
                const char *e = getenv("LARECOMP_CG_SHOT");
                cg_a = e ? atol(e) : 0;
                cg_b = cg_a;
                if (e && strchr(e, ':')) sscanf(e, "%ld:%ld:%ld", &cg_a, &cg_b, &cg_step);
                if (cg_step < 1) cg_step = 1;
            }
            long f = (long)total_frames;
            if (cg_a && f >= cg_a && f <= cg_b && (f - cg_a) % cg_step == 0) {
                [cb waitUntilScheduled];
                usleep(100000);  // (let the window server show this frame)
                char path[512];
                const char *pat = getenv("LARECOMP_SHOT_PATH") ? getenv("LARECOMP_SHOT_PATH") : "build/cg_window.png";
                if (strchr(pat, '%')) snprintf(path, sizeof path, pat, (int)f); else snprintf(path, sizeof path, "%s", pat);
                window_server_shot(path);
            }
        }
        if (shot_tex) {
            [cb waitUntilCompleted];
            // LARECOMP_SHOT_PATH: where (parallel test runs each write their own; default build/window.png)
            g_save_upright = true;
            if (g_window_shot_request[0]) {
                renderer_save_texture(shot_tex, g_window_shot_request);
                g_window_shot_request[0] = 0;
            } else {
                renderer_save_texture(shot_tex, getenv("LARECOMP_SHOT_PATH") ? getenv("LARECOMP_SHOT_PATH") : "build/window.png");
                if (getenv("LARECOMP_EXIT_AFTER_SHOT")) { fflush(stderr); _exit(0); }
            }
            g_save_upright = false;
        }
        }  // (drawable)
        if (fg_three) {
            [cb waitUntilCompleted];
            const char *pat = getenv("LARECOMP_SHOT_PATH");
            g_save_upright = true;
            renderer_save_texture(fg_three, pat ? pat : "build/framegen.png");
            g_save_upright = false;
            LOG("[framegen] shot %s\n", pat ? pat : "build/framegen.png");
            fg_three = nil;
        }
    }
    frames++;
    total_frames++;
    g_frame_counter++;
    extern void input_frame(uint64_t frame);
    input_frame(total_frames);
    static long slow_ms = -1;  // LARECOMP_SLOW_FRAME_MS=<ms>: sleep per presented frame (simulate a slow machine)
    if (slow_ms < 0) slow_ms = getenv("LARECOMP_SLOW_FRAME_MS") ? atol(getenv("LARECOMP_SLOW_FRAME_MS")) : 0;
    if (slow_ms > 0 && total_frames > 2600) usleep((useconds_t)slow_ms * 1000);
    extern void fnprof_frame(uint64_t frame);
    fnprof_frame(total_frames);
    extern void warp_frame(uint64_t frame);
    warp_frame(total_frames);
    extern void reboot_test_frame(uint64_t frame);  // runtime/main.c (LARECOMP_REBOOT_AT)
    reboot_test_frame(total_frames);
    extern void save_editor_frame(uint64_t frame);
    save_editor_frame(total_frames);
    extern void prefetch_stats_tick(uint64_t presented);
    prefetch_stats_tick(total_frames);
    static long shot_every = -1;
    if (shot_every < 0) { const char *e = getenv("LARECOMP_SCREENSHOT_EVERY"); shot_every = e ? atol(e) : 0; }
    extern bool g_debug_frame;
    static long debug_at = -1;
    if (debug_at < 0) { const char *e = getenv("LARECOMP_DEBUG_FRAME"); debug_at = e ? atol(e) : 0; }
    level_change_check();
    extern volatile int g_debug_frame_request;
    if (g_debug_frame_request) { g_debug_frame_request = 0; debug_at = (long)total_frames + 1; }
    g_debug_frame = debug_at && total_frames == (uint64_t)debug_at;
    if (g_debug_frame) LOG("===== debug frame %llu =====\n", total_frames);
    static long shot_from = -1;  // LARECOMP_SCREENSHOT_FROM=<frame>: periodic shots start there (into build/frames/)
    if (shot_from < 0) { const char *e = getenv("LARECOMP_SCREENSHOT_FROM"); shot_from = e ? atol(e) : 0; }
    if (shot_every && b && b->tex && total_frames % shot_every == 0 && (long)total_frames >= shot_from) {
        char path[256];
        if (shot_from) mkdir("build/frames", 0755);
        snprintf(path, sizeof path, shot_from ? "build/frames/frame_%05llu.png" : "build/frame_%05llu.png", total_frames);
        save_png(b->tex, path);
        if (getenv("LARECOMP_EXIT_AFTER_SHOT")) { fflush(stderr); _exit(0); }
    }
    {   // one capture of the presented frame at its own size, asked for by a game hook (games/smm2/trace.c
        // RECOMP_TRACE_SHOT; the path names "frame_" so the visible crop applies)
        extern char g_frame_shot_request[256];
        if (g_frame_shot_request[0] && b && b->tex) {
            save_png(b->tex, g_frame_shot_request);
            g_frame_shot_request[0] = 0;
        }
    }
    {   // RECOMP_DUMP_TEX=<frame>: every live 2D texture of at least 64x64 that the GPU drew into, as it is after that
        // frame's present, to build/tex_<frame>_<n>.png (with its label in the log): the state of each render target
        static long dump_at = -2;
        if (dump_at == -2) dump_at = getenv("RECOMP_DUMP_TEX") ? atol(getenv("RECOMP_DUMP_TEX")) : -1;
        if (dump_at >= 0 && (long)total_frames == dump_at) {
            renderer_flush();
            int n = 0;
            for (int i = 0; i < g_reg_n; i++) {
                NvnTexture *t = g_reg[i];
                TexBackend *tb = t ? (TexBackend *)t->backend : NULL;
                if (!tb || !tb->tex || !tb->gpu_owned || t->target != 1 || t->width < 64 || t->height < 64) continue;
                if (renderer_format_is_depth(t->format)) continue;
                char path[256];
                snprintf(path, sizeof path, "build/tex_%05ld_%03d.png", dump_at, n++);
                LOG("[texdump] %s: %ux%u f%#x %s\n", path, t->width, t->height, t->format, t->label);
                save_png(tb->tex, path);
            }
        }
    }
    double now = SDL_GetTicks() / 1000.0;
    static double fps_iv = -1, prev_present, max_gap;  // LARECOMP_FPS_INTERVAL=<s>: fps line period (default 2)
    if (fps_iv < 0) fps_iv = getenv("LARECOMP_FPS_INTERVAL") ? atof(getenv("LARECOMP_FPS_INTERVAL")) : 2.0;
    if (prev_present > 0 && now - prev_present > max_gap) max_gap = now - prev_present;
    prev_present = now;
    if (now - last_fps_time >= fps_iv) {
        extern const char *game_level_name(void);
        static uint64_t fg_real0, fg_gen0, fg_shown0;  // frame generation: generated (shown) / real since the last line
        char fgs[80] = "";
        uint64_t shown = atomic_load(&g_fg_shown);
        if (g_settings.frame_gen != FRAMEGEN_OFF)
            snprintf(fgs, sizeof fgs, " gen %llu/%llu shown %llu", (unsigned long long)(g_fg_generated - fg_gen0),
                     (unsigned long long)(g_fg_real - fg_real0), (unsigned long long)(shown - fg_shown0));
        fg_real0 = g_fg_real, fg_gen0 = g_fg_generated, fg_shown0 = shown;
        if (last_fps_time > 0) LOG("[metal] t=%.1f frame %llu %.1f fps%s max %.0fms level %s (%s)\n", now, (unsigned long long)total_frames,
                                   frames / (now - last_fps_time), fgs, max_gap * 1000.0, game_level_name(), renderer_stats());
        {   // (diagnostics) frame rate low for two periods in a row: what's allocated (at most once a minute)
            static int slow;
            static double last_dump = -100;
            double fps = last_fps_time > 0 ? frames / (now - last_fps_time) : 60;
            slow = fps < 40 ? slow + 1 : 0;
            if (slow >= 2 && now - last_dump > 60) { last_dump = now; dump_largest_textures("low frame rate"); }
            if (now - last_dump > 300 && now > 30) { last_dump = now; dump_largest_textures("periodic"); }
        }
        max_gap = 0;
        frames = 0;
        last_fps_time = now;
    }
}

// ================================================================== UI thread
// The window + Metal layer, created once: by the launcher (runtime/ui/launcher.mm, start screen before the game
// boots) or by ui_run; the game reuses the launcher's window.
void ui_window_init(void) {
    if (window) return;
    // (no SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS here: on macOS reading controllers in the background opens them
    // as HID devices, for which the system asks for Input Monitoring - "receive keystrokes from any application")
    settings_load();
#if TARGET_OS_IPHONE
    // iOS: the whole screen, in landscape, no status bar (a windowed SDL window keeps the size and orientation the app
    // started in: a portrait window on a landscape screen); the home indicator hides, edge swipes need a second swipe
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "2");
#endif
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) FATAL("SDL_Init: %s\n", SDL_GetError());
#if TARGET_OS_IPHONE
    SDL_AddEventWatch(app_state_watch, NULL);
#endif
    int win_w = 1280, win_h = 720;
    if (getenv("LARECOMP_WINDOW")) sscanf(getenv("LARECOMP_WINDOW"), "%dx%d", &win_w, &win_h);
    // LARECOMP_HIDDEN=1: never shown (automated test runs render off screen without taking the user's focus)
    SDL_WindowFlags wflags = SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (getenv("LARECOMP_HIDDEN")) wflags |= SDL_WINDOW_HIDDEN;
#if TARGET_OS_IPHONE
    wflags |= SDL_WINDOW_FULLSCREEN | SDL_WINDOW_BORDERLESS;
#endif
    window = SDL_CreateWindow(GAME_TITLE, win_w, win_h, wflags);
    if (!window) FATAL("SDL_CreateWindow: %s\n", SDL_GetError());
    rt_window_icon(window);
    // the Fullscreen setting applies from the start screen on (not only once the game runs); test windows stay windows
    if (g_settings.fullscreen && !getenv("LARECOMP_WINDOW") && !getenv("LARECOMP_HIDDEN")) SDL_SetWindowFullscreen(window, true);
    SDL_MetalView view = SDL_Metal_CreateView(window);
    layer = (__bridge CAMetalLayer *)SDL_Metal_GetLayer(view);
    dev = MTLCreateSystemDefaultDevice();
#if TARGET_OS_IPHONE
    if (@available(iOS 16.4, *)) g_bc_soft = !dev.supportsBCTextureCompression;
    else g_bc_soft = true;
#endif
    if (getenv("RECOMP_BC_SOFT")) g_bc_soft = atoi(getenv("RECOMP_BC_SOFT")) != 0;
    if (g_bc_soft) LOG("[metal] no BC texture support on this GPU: BC1-7 decoded on the CPU\n");
    // ASTC: Apple GPUs (Apple silicon Macs, iPhones / iPads from the A8) have it. Intel Macs' GPUs (Metal 2 only,
    // Common feature sets) may not (NVIDIA / AMD, and older Intel), and creating an ASTC texture there aborts inside
    // Metal ("Texture Descriptor Validation: invalid pixelFormat"): decode it on the CPU as the Vulkan path does.
    g_astc_soft = ![dev supportsFamily:MTLGPUFamilyApple2];
    if (getenv("RECOMP_ASTC_SOFT")) g_astc_soft = atoi(getenv("RECOMP_ASTC_SOFT")) != 0;
    if (g_astc_soft) LOG("[metal] no ASTC texture support on this GPU: ASTC decoded on the CPU\n");
    layer.device = dev;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = getenv("LARECOMP_WINDOW_SHOT") ? NO : YES;
    layer.maximumDrawableCount = 3;  // (frame generation: two presents per real frame)
}

SDL_Window *ui_window(void) { return window; }
void *ui_metal_layer(void) { return (__bridge void *)layer; }
void *ui_metal_device(void) { return (__bridge void *)dev; }

int ui_run(void) {
    // The emulated vsync/timer threads need precise sleeps; App Nap timer coalescing
    // otherwise stretches 16ms sleeps to ~100ms whenever the window isn't frontmost.
    static id activity;
    if (!activity)
        activity = [[NSProcessInfo processInfo]
            beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical
                              reason:@"Game emulation"];
    ui_window_init();
    atomic_store(&ui_ready, true);
    for (;;) {
        if (rt_reboot_requested()) return rt_reboot_requested();  // (runtime/main.c tears the game down)
        SDL_Event e;
        while (SDL_WaitEventTimeout(&e, 16)) {
            if (e.type == SDL_EVENT_QUIT) { LOG("window closed\n"); exit(0); }
            if (input_user_blocked(&e)) continue;  // scripted test runs: the real keyboard / mouse / pads do nothing
            if (menu_handle_event(&e)) continue;
            input_handle_event(&e);
        }
        menu_apply_window_requests(window);
        int w, h;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        layer.drawableSize = CGSizeMake(w, h);
        atomic_store(&g_display_hz, framegen_display_hz(window));  // (SDL's display state: read on this thread)
    }
}
