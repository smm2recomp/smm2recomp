// Vulkan backend for the NVN layer (Windows; macOS uses runtime/nvn/metal.m + renderer.m).
// Files: device.c (instance, window, swapchain, present, screenshots, ImGui hooks), textures.c (images, uploads,
// views), exec.c (command execution: passes, draws, clears, copies, compute), shaders.c (GLSL -> SPIR-V, programs,
// pipelines). Mirrors the Metal backend's behaviour; differences are noted where they happen.
//
// Synchronisation model: every image lives in VK_IMAGE_LAYOUT_GENERAL, and a full memory barrier separates
// render passes, copies and dispatches (Metal tracks these hazards itself). Submissions are numbered; a timeline
// semaphore signals each number, which is what NVN fences (renderer_fence_seq / renderer_wait_seq) wait for.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#if __has_include(<vma/vk_mem_alloc.h>)
#include <vma/vk_mem_alloc.h>
#else
#include <vk_mem_alloc.h>  // (Debian / Ubuntu libvulkan-memory-allocator-dev)
#endif

// The Vulkan 1.2 / 1.3 commands, fetched from the device when it is made (device.c) instead of imported: a Vulkan
// loader older than that (an old vulkan-1.dll) lacks them, and Windows then wouldn't start the program at all.
#define VK_LATE_FUNCS(X) X(CmdBeginRendering) X(CmdEndRendering) X(CmdPipelineBarrier2) X(CmdSetCullMode) \
    X(CmdSetDepthBiasEnable) X(CmdSetDepthCompareOp) X(CmdSetDepthTestEnable) X(CmdSetDepthWriteEnable) \
    X(CmdSetFrontFace) X(CmdSetPrimitiveRestartEnable) X(CmdSetPrimitiveTopology) X(CmdSetStencilOp) \
    X(CmdSetStencilTestEnable) X(CmdWriteTimestamp2) X(QueueSubmit2) X(GetSemaphoreCounterValue) X(WaitSemaphores) \
    X(GetBufferDeviceAddress)
#define X(n) extern PFN_vk##n vkl_##n;
VK_LATE_FUNCS(X)
#undef X
#define vkCmdBeginRendering vkl_CmdBeginRendering
#define vkCmdEndRendering vkl_CmdEndRendering
#define vkCmdPipelineBarrier2 vkl_CmdPipelineBarrier2
#define vkCmdSetCullMode vkl_CmdSetCullMode
#define vkCmdSetDepthBiasEnable vkl_CmdSetDepthBiasEnable
#define vkCmdSetDepthCompareOp vkl_CmdSetDepthCompareOp
#define vkCmdSetDepthTestEnable vkl_CmdSetDepthTestEnable
#define vkCmdSetDepthWriteEnable vkl_CmdSetDepthWriteEnable
#define vkCmdSetFrontFace vkl_CmdSetFrontFace
#define vkCmdSetPrimitiveRestartEnable vkl_CmdSetPrimitiveRestartEnable
#define vkCmdSetPrimitiveTopology vkl_CmdSetPrimitiveTopology
#define vkCmdSetStencilOp vkl_CmdSetStencilOp
#define vkCmdSetStencilTestEnable vkl_CmdSetStencilTestEnable
#define vkCmdWriteTimestamp2 vkl_CmdWriteTimestamp2
#define vkQueueSubmit2 vkl_QueueSubmit2
#define vkGetSemaphoreCounterValue vkl_GetSemaphoreCounterValue
#define vkWaitSemaphores vkl_WaitSemaphores
#define vkGetBufferDeviceAddress vkl_GetBufferDeviceAddress

#include "../nvn.h"
#include "../maxwell/shader.h"

void vk_device_fault_report(void);
#define VK_CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { if (r_ == VK_ERROR_DEVICE_LOST) vk_device_fault_report(); \
    FATAL("%s failed: %d (%s:%d)\n", #x, r_, __FILE__, __LINE__); } } while (0)

typedef struct {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t qfam;
    VmaAllocator vma;
    VkPhysicalDeviceProperties props;
    VkPipelineCache pcache;
    VkSemaphore timeline;      // value = last completed submission number
    VkCommandPool pool;        // render thread only
    bool custom_border, divisor, host_import, geometry;
    bool bda;  // bufferDeviceAddress: shader global memory through the heap's device address (exec.c gmem_desc)
    // window
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat sc_format;
    VkExtent2D sc_extent;
    uint32_t sc_count;
    VkImage sc_images[8];
    VkImageView sc_views[8];
} VkG;
extern VkG g_vk;

// ---- submissions (exec.c)
// Work is recorded into the current command buffer; uploads go into a separate one that runs first.
VkCommandBuffer vk_cmd(void);         // current command buffer (outside any rendering unless exec.c began one)
VkCommandBuffer vk_upload_cmd(void);  // upload command buffer of the pending submission
uint64_t vk_pending_seq(void);        // number the pending submission will get
uint64_t vk_completed_seq(void);
void vk_wait_seq(uint64_t seq);
void vk_full_barrier(VkCommandBuffer cb);
void vk_end_pass(void);
void vk_submit_extra(VkSemaphore wait, VkSemaphore signal, VkCommandBuffer extra);  // submit + one more cb (present)

// Transient GPU-visible memory that lives until the submission it was allocated for completes.
typedef struct { VkBuffer buf; uint64_t off; void *ptr; } VkSpan;
VkSpan vk_transient(uint64_t size, uint64_t align);
VkSpan vk_transient_copy(const void *data, uint64_t size, uint64_t align);
VkSpan vk_readback(uint64_t size);  // host-cached GPU -> CPU staging (read right after the copy completes)
VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout);

// ---- textures (textures.c)
typedef struct TexBackend {
    VkImage image;
    VmaAllocation mem;
    VkFormat format;
    VkImageViewType base_type;
    uint32_t width, height, depth, layers, levels, samples;
    VkImageAspectFlags aspect;
    bool gpu_owned, uploaded, dynamic;
    uint64_t data_hash, sample_hash, checked_frame, full_frame;
    uint32_t wgen;  // page write tracking generation at the last upload check (textures.c)
    bool scalable, presented;
    uint64_t touched_frame, frame_cleared;
    uint32_t level_gen;
    float sx, sy;
    void *pending_rgba;  // host-produced pixels (video) waiting to be uploaded on the render thread
    uint32_t pending_w, pending_h, pending_stride;
    uint8_t astc_bw, astc_bh;  // ASTC without GPU support: decoded to RGBA8 on upload (runtime/nvn/astc.c)
    bool astc_srgb;
    VkImageUsageFlags usage;  // the image's usage (a pass's crash note, #60)
    uint8_t bc_soft;  // BC1-7 without GPU support (mobile GPUs): BCSOFT_* kind, decoded on upload (runtime/nvn/bcsoft.c)
    int tp;              // runtime/nvn/texpatch.c: 0 not checked yet, -1 none, else known UI texture + 1
    uint32_t tp_gen;     // texpatch_generation() it was last uploaded with
    int reg;  // slot in textures.c's live list + 1 (a reboot destroys them all)
    int refs;  // texture objects sharing it (aliases of the same storage: textures.c tex_backend)
} TexBackend;

TexBackend *vk_tex(NvnTexture *t, bool for_render);  // backend image (uploads CPU contents unless for_render)
VkFormat vk_format(uint32_t nvn_format);
bool vk_format_alpha_one(uint32_t nvn_format);
// View of t (optionally through an NVN texture view) for rendering or sampling.
VkImageView vk_view(NvnTexture *t, const NvnTextureView *v, bool has_view, bool render, int want_textype);
VkImageView vk_image_view(TexBackend *b, VkImageViewType type, VkFormat fmt, uint32_t lv0, uint32_t nlv, uint32_t ly0,
                          uint32_t nly, VkComponentMapping sw, VkImageAspectFlags aspect);
VkImageView vk_dummy_view(int textype);
VkImageView vk_shadow_view(NvnTexture *t);  // a colour texture sampled with depth comparison: a depth copy, or NULL
VkSampler vk_sampler(NvnSamplerDesc *d);
VkSampler vk_default_sampler(bool linear);
void vk_image_init_layout(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect);
// Destruction once every submission that may use the object has completed.
void vk_defer_image(VkImage img, VmaAllocation mem);
void vk_defer_view(VkImageView v);
void vk_views_forget(TexBackend *b);  // drop (deferred) the cached views of an image being replaced
uint64_t vk_frame_counter(void);

// ---- shaders / pipelines (shaders.c)
VkShaderModule vk_compile_glsl(const char *src, int stage, const char *name);  // NULL on failure (logged)
VkShaderModule vk_builtin_shader(const char *name);  // internal shaders (clear, copy, present)
VkShaderModule vk_builtin_shader_try(const char *name);  // ... VK_NULL_HANDLE if it fails to compile

// ---- device / window (device.c)
void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h);
// Frame generation (runtime/nvn/framegen.h): the real frame's present runs on a present thread, 1/120 s after the
// generated one; every submit, present and device wait takes the queue lock.
void vk_queue_lock(void);
void vk_queue_unlock(void);
void vk_present_flush(void);                                           // wait until a deferred present was issued
void vk_frame_end_at(const char *shot_path, uint64_t present_at_ns);   // vk_frame_end, presented at that time
VkFormat vk_frame_format(void);                                        // the format being rendered into
void vk_offscreen_begin(TexBackend *img, const float clear[4]);        // render into img (GENERAL) like a window image
void vk_offscreen_end(void);
// exec.c
TexBackend *vk_framegen_image(int i, uint32_t w, uint32_t h);          // game images 0 / 1: RGBA8, the window's size
void vk_framegen_encode(VkCommandBuffer cb, TexBackend *prev, TexBackend *cur, float t, bool reset);  // into the window image
void vk_present_copy(VkCommandBuffer cb, TexBackend *src);             // src over the whole window image
