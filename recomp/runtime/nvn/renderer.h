// Interface between the NVN executor (renderer.m) and the Metal plumbing (metal.m).
#pragma once
#ifdef __OBJC__
#import <Metal/Metal.h>
void renderer_init(id<MTLDevice> dev);
id<MTLCommandQueue> renderer_queue(void);
id<MTLTexture> renderer_texture(NvnTexture *t, id<MTLBlitCommandEncoder> blit, bool for_render);
MTLPixelFormat renderer_pixel_format(uint32_t nvn_format);
MTLPixelFormat renderer_texture_pixel_format(NvnTexture *t);  // (no texture made: what it has, or will be made with)
bool renderer_format_alpha_one(uint32_t nvn_format);
extern id<MTLBuffer> g_guest_buffer;
extern uint64_t g_guest_buffer_base;
// The heap buffer holding a guest address (the heap is wrapped in <= 2 GiB chunks) and its offset, or nil.
id<MTLBuffer> guest_buffer_chunk(uint64_t addr, uint64_t size, uint64_t *offset);
#endif
#include <SDL3/SDL_events.h>

bool renderer_is_gpu_owned(NvnTexture *t);
void renderer_flush(void);  // finish all recorded GPU work
bool renderer_origin_lower_left(void);
const char *renderer_stats(void);
bool renderer_format_is_depth(uint32_t f);
bool renderer_format_has_stencil(uint32_t f);
bool renderer_format_is_soft16(uint32_t f);  // packed 16-bit colour texture backed by RGBA8 (decoded on the CPU)
void renderer_soft16_decode(uint32_t f, const uint8_t *src, uint64_t n, uint8_t *dst);
void renderer_mark_gpu_owned(NvnTexture *t);
void input_handle_event(const SDL_Event *e);
bool input_user_blocked(const SDL_Event *e);
