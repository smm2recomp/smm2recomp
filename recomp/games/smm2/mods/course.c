// SMM2 course codes <-> data ids (the same arithmetic as core/crates/formats course_id.rs and smmfmt/course_id.py).
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "mod.h"
#include "../../../runtime/net/netcfg.h"

static const char ALPHABET[] = "0123456789BCDFGHJKLMNPQRSTVWXY";
#define XOR_KEY 0x1680E07Cull

// OpenCourse World's ids (backend=ocw) are a counter from 1,000,000,000, not Nintendo's scrambled ones; its codes
// spread the id over the same nine base-30 digits: id bits 0-5 stay at 0-5, bits 6-25 go to 14-33, bits 26-31 to
// 34-39; the remaining bits are constant (0xA00 at bits 6-13, 3 at bit 40 up). Checked on DJF-3BW-HD5 = 1000017224.
#define OCW_FIXED ((3ull << 40) | 0xA00ull)
static bool ocw_ids(void) { return net_backend() == NET_BACKEND_OCW; }

bool code_to_data_id(const char *code, uint32_t *id) {
    char s[9];
    int n = 0;
    for (const char *p = code; *p && n <= 9; p++)
        if (*p != '-') {
            if (n == 9) return false;
            s[n++] = (char)toupper((unsigned char)*p);
        }
    if (n != 9) return false;
    uint64_t v = 0;
    for (int i = 8; i >= 0; i--) {
        const char *d = strchr(ALPHABET, s[i]);
        if (!d || !*d) return false;
        v = v * 30 + (uint64_t)(d - ALPHABET);
    }
    if (ocw_ids()) {
        if (v >> 42 || (v & ~((0x3Full << 34) | (0xFFFFFull << 14) | 0x3Full)) != OCW_FIXED) return false;
        *id = (uint32_t)((v >> 34 & 0x3F) << 26 | (v >> 14 & 0xFFFFF) << 6 | (v & 0x3F));
        return true;
    }
    uint64_t low = v & 0xFFF, one = (v >> 12) & 1, mid = (v >> 14) & 0xFFFFF, check = (v >> 34) & 0x3F,
             eight = (v >> 40) & 0xF;
    uint64_t data = ((low << 20) | mid) ^ XOR_KEY;
    if (one != 1 || eight != 8 || check != (uint64_t)(((int64_t)data - 31) % 64 + 64) % 64) return false;
    *id = (uint32_t)data;
    return true;
}

void data_id_to_code(uint32_t id, char out[12]) {
    uint64_t x = id ^ XOR_KEY;
    uint64_t check = (uint64_t)((((int64_t)id - 31) % 64 + 64) % 64);
    uint64_t n = (8ull << 40) | (check << 34) | ((x & 0xFFFFF) << 14) | (1ull << 12) | (x >> 20);
    if (ocw_ids()) n = OCW_FIXED | ((uint64_t)(id >> 26) << 34) | ((uint64_t)(id >> 6 & 0xFFFFF) << 14) | (id & 0x3F);
    char c[9];
    for (int i = 0; i < 9; i++) c[i] = ALPHABET[n % 30], n /= 30;
    snprintf(out, 12, "%.3s-%.3s-%.3s", c, c + 3, c + 6);
}
