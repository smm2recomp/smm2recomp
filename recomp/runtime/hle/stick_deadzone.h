// Controls > Sticks (#98): a controller stick's dead zone, for a worn stick that drifts at rest. Radial (the stick's
// distance from its centre, not each axis alone, so a small diagonal push isn't pulled onto an axis) and rescaled (the
// game still sees every tilt from 0 to full, starting at the dead zone's edge). Header-only: runtime/hle/input.c
// (fill_state), runtime/ui/menu.mm (the Controls tab's preview) and tools/test_stick_deadzone.c.
#pragma once
#include <math.h>
#include <stdint.h>

// x, y: a stick as SDL reports it (-32768..32767 each); pct: the dead zone, percent of a full tilt. 0 (off) leaves the
// stick untouched. Otherwise a stick inside the dead zone reads (0, 0), and outside it keeps its direction with its
// length rescaled from dead zone..32767 to 0..32767 (capped at 32767 before each axis is rounded, so neither axis
// leaves that range: a corner pushed past the circle, e.g. 32767, 32767, comes back onto it).
static inline void stick_deadzone(int32_t *x, int32_t *y, int pct) {
    if (pct <= 0) return;
    if (pct > 99) pct = 99;  // (the setting stops at 30: only so that 100 can't divide by 0 below)
    double r = hypot(*x, *y), dz = pct / 100.0 * 32767;
    if (r <= dz) { *x = *y = 0; return; }
    double len = (r - dz) / (32767 - dz);
    double k = (len < 1 ? len : 1) * 32767 / r;
    *x = (int32_t)lround(*x * k), *y = (int32_t)lround(*y * k);
}
