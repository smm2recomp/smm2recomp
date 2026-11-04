// A colour filter over the game's image (runtime/nvn/color_filter.c), applied by each backend's present pass (metal.m,
// vulkan/exec.c) as it draws the game's image into the window, so before the overlays and menus: for each colour
// channel, out = m[4i] r + m[4i + 1] g + m[4i + 2] b + m[4i + 3] (the image's stored values, 0..1).
#pragma once
#include <stdbool.h>

void renderer_set_color_filter(const float m[12]);  // NULL: none (a mod's: the SMM1 title letters' sepia)
bool renderer_color_filter(float m[12]);            // false: none this frame
