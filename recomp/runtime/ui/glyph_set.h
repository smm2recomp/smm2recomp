// The name entry's Chinese, Japanese and Korean text (#99): the characters the open dialog has shown that the overlay's
// fonts (Latin-1 only) have no glyph for, kept sorted, and every character no console font could add. Header-only:
// runtime/ui/menu.mm (which builds the font atlas again with them, from the console's fonts) and
// tools/test_glyph_set.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define GLYPHS_MAX 256  // (a name is 32 characters at most, a comment 75; the set starts again with each dialog)
#define CJK_FONTS 5     // (SharedFontType 0 - 4: Standard with the Japanese, the three Chinese ones, Korean)
// cp: the dialog's characters; failed: a bit per code point the atlas couldn't get, kept for the whole session
typedef struct { uint16_t cp[GLYPHS_MAX]; int n; uint8_t failed[0x10000 / 8]; } GlyphSet;

static inline bool glyphs_failed(const GlyphSet *s, uint32_t c) { return s->failed[c >> 3] >> (c & 7) & 1; }

// One text's code points from U+0100 to U+FFFF (ImGui's ImWchar is 16 bits) into the set. false: it is full.
// Not U+FFFD: ImGui's text field stores it for a character beyond the BMP (an emoji), which would read the fonts and
// build the atlas again for a text without any of these scripts, and a font with that glyph would draw it in place
// of the ? such characters had before. Not a failed one either: it would only build the same atlas again
static inline bool glyphs_add(GlyphSet *s, const char *text, bool *added) {
    for (const unsigned char *p = (const unsigned char *)text; *p;) {
        uint32_t c = *p++;
        int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        if (more) c &= 0x3f >> more;
        for (; more && (*p & 0xc0) == 0x80; more--) c = c << 6 | (*p++ & 0x3f);
        if (more || c < 0x100 || c > 0xffff || c == 0xfffd) continue;  // (cut short, Latin-1 or beyond the BMP)
        if (glyphs_failed(s, c)) continue;
        int i = 0;
        while (i < s->n && s->cp[i] < c) i++;
        if (i < s->n && s->cp[i] == c) continue;
        if (s->n == GLYPHS_MAX) return false;
        memmove(s->cp + i + 1, s->cp + i, (size_t)(s->n - i) * sizeof *s->cp);
        s->cp[i] = (uint16_t)c;
        s->n++;
        *added = true;
    }
    return true;
}

// The dialog's two texts into the set; a full set starts again with just these. true: there are new characters
static inline bool glyphs_scan(GlyphSet *s, const char *text, const char *title) {
    bool added = false;
    if (glyphs_add(s, text, &added) && glyphs_add(s, title, &added)) return added;
    s->n = 0;
    glyphs_add(s, text, &added);
    glyphs_add(s, title, &added);
    return true;
}

// A new dialog: none of the characters, but the failed ones stay failed
static inline void glyphs_clear(GlyphSet *s) { s->n = 0; }

// A character the atlas can't get: out of the set, and never into it again
static inline void glyphs_fail(GlyphSet *s, uint16_t c) {
    s->failed[c >> 3] |= (uint8_t)(1 << (c & 7));
    for (int i = 0; i < s->n; i++)
        if (s->cp[i] == c) {
            memmove(s->cp + i, s->cp + i + 1, (size_t)(s->n - i - 1) * sizeof *s->cp);
            s->n--;
            break;
        }
}

// Hangul (syllables, Jamo, compatibility Jamo): the Korean font is only loaded for these
static inline bool glyphs_hangul(uint16_t c) {
    return (c >= 0xac00 && c <= 0xd7a3) || (c >= 0x1100 && c <= 0x11ff) || (c >= 0x3130 && c <= 0x318f);
}

// The set as ImFontConfig::GlyphRanges: first / last pairs, 0-terminated (out: 2 * GLYPHS_MAX + 1)
static inline void glyphs_ranges(const GlyphSet *s, uint16_t *out) {
    for (int i = 0; i < s->n; i++) out[2 * i] = out[2 * i + 1] = s->cp[i];
    out[2 * s->n] = 0;
}

// The order the console's fonts are merged in for the game's language (settings_language()): the first font with a
// character draws it, so Chinese text gets the Chinese shapes of the Han characters it shares with Japanese
static inline void glyphs_font_order(const char *lang, int order[CJK_FONTS]) {
    static const int zh_tw[CJK_FONTS] = {3, 0, 1, 2, 4}, zh_cn[CJK_FONTS] = {1, 2, 0, 3, 4};
    static const int ko[CJK_FONTS] = {4, 0, 1, 2, 3}, other[CJK_FONTS] = {0, 1, 2, 3, 4};
    const int *o = !strcmp(lang, "zh-TW") ? zh_tw : !strcmp(lang, "zh-CN") ? zh_cn : !strcmp(lang, "ko") ? ko : other;
    memcpy(order, o, sizeof other);
}

// The next font to read for characters the atlas lacks, in that order (the Korean one first for Hangul, and only for
// it). -1: every font they could come from was tried
static inline int glyphs_next_font(const int order[CJK_FONTS], const bool tried[CJK_FONTS], bool hangul) {
    if (hangul && !tried[4]) return 4;
    for (int i = 0; i < CJK_FONTS; i++)
        if (order[i] != 4 && !tried[order[i]]) return order[i];
    return -1;
}

static inline bool glyphs_same_order(const int a[CJK_FONTS], const int b[CJK_FONTS]) {
    return !memcmp(a, b, CJK_FONTS * sizeof *a);
}

// The language changed: the font its order merges first, if it was never tried (the Korean one only for Hangul), so
// that it draws the characters it shares with the fonts already read. -1: none to read
static inline int glyphs_reorder_font(const int order[CJK_FONTS], const bool tried[CJK_FONTS], bool hangul) {
    for (int i = 0; i < CJK_FONTS; i++)
        if (order[i] != 4 || hangul) return tried[order[i]] ? -1 : order[i];
    return -1;
}

// Every font a character could come from was tried: one the atlas still lacks can't be added
static inline bool glyphs_all_tried(const bool tried[CJK_FONTS], uint16_t c) {
    for (int i = 0; i < CJK_FONTS; i++)
        if (!tried[i] && (i != 4 || glyphs_hangul(c))) return false;
    return true;
}
