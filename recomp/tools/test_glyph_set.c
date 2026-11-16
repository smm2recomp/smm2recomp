// Unit tests for the name entry's character set (runtime/ui/glyph_set.h, #99): which characters of the dialog's texts
// go into it, its order, a full set, a new dialog's, the failed ones, the Korean ones, the ranges ImGui gets and the
// order the console's fonts are read and merged in, and again when the language changes.
// usage: make test-glyph-set
#include <stdio.h>
#include <string.h>

#include "../runtime/ui/glyph_set.h"

static int failures;
#define CHECK(name, cond) \
    do { if (!(cond)) { printf("FAIL %s: %s (line %d)\n", name, #cond, __LINE__); failures++; return; } } while (0)

static GlyphSet s;  // (emptied by each test)

static void test_scan(void) {
    s.n = 0;
    CHECK("ASCII and Latin-1: nothing", !glyphs_scan(&s, "Mario 1-1 \xc3\xa9", "Enter text") && s.n == 0);
    // "Mario 關卡" then the title "テスト": U+95DC U+5361, U+30C6 U+30B9 U+30C8
    CHECK("new characters", glyphs_scan(&s, "Mario \xe9\x97\x9c\xe5\x8d\xa1", "\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88"));
    CHECK("sorted", s.n == 5 && s.cp[0] == 0x30b9 && s.cp[1] == 0x30c6 && s.cp[2] == 0x30c8 && s.cp[3] == 0x5361 &&
                        s.cp[4] == 0x95dc);
    CHECK("the same again: nothing new", !glyphs_scan(&s, "\xe5\x8d\xa1\xe9\x97\x9c", "") && s.n == 5);
    CHECK("the ends of the range",
          glyphs_scan(&s, "\xc4\x80\xef\xbf\xbf", "") && s.cp[0] == 0x100 && s.cp[6] == 0xffff);
}

static void test_utf8(void) {
    s.n = 0;
    CHECK("beyond the BMP (U+1F600): not kept", !glyphs_scan(&s, "\xf0\x9f\x98\x80", "") && s.n == 0);
    CHECK("U+FFFD (what the text field keeps of an emoji): not kept",
          !glyphs_scan(&s, "\xef\xbf\xbd", "a\xef\xbf\xbd") && s.n == 0);
    CHECK("its neighbours U+FFFC and U+FFFE are",
          glyphs_scan(&s, "\xef\xbf\xbc\xef\xbf\xbd\xef\xbf\xbe", "") && s.n == 2);
    CHECK("(U+FFFC, U+FFFE)", s.cp[0] == 0xfffc && s.cp[1] == 0xfffe);
    s.n = 0;
    CHECK("cut inside a character: not kept", !glyphs_scan(&s, "\xe9\x97", "\xe9") && s.n == 0);
    CHECK("a stray continuation byte, then a character", glyphs_scan(&s, "\x9c\xe9\x97\x9c", "") && s.n == 1);
    CHECK("(U+95DC)", s.cp[0] == 0x95dc);
    CHECK("a lead byte before ASCII, then a character", glyphs_scan(&s, "\xe9" "a\xe5\x8d\xa1", "") && s.n == 2);
    CHECK("(U+5361)", s.cp[0] == 0x5361);
}

static void test_full(void) {
    s.n = 0;
    char t[4] = {0};
    for (int c = 0x4e00; c < 0x4e00 + GLYPHS_MAX; c++) {  // (U+4E00 on, one at a time)
        t[0] = (char)(0xe0 | c >> 12), t[1] = (char)(0x80 | (c >> 6 & 0x3f)), t[2] = (char)(0x80 | (c & 0x3f));
        CHECK("filling", glyphs_scan(&s, t, ""));
    }
    CHECK("full", s.n == GLYPHS_MAX && s.cp[0] == 0x4e00 && s.cp[GLYPHS_MAX - 1] == 0x4e00 + GLYPHS_MAX - 1);
    CHECK("one already there: still full", !glyphs_scan(&s, t, "") && s.n == GLYPHS_MAX);
    // one more: only the dialog's own are left (U+95DC from the text, U+30C6 from the title)
    CHECK("one more starts again", glyphs_scan(&s, "\xe9\x97\x9c", "\xe3\x83\x86"));
    CHECK("with the two texts' characters", s.n == 2 && s.cp[0] == 0x30c6 && s.cp[1] == 0x95dc);
}

static void test_clear(void) {
    memset(&s, 0, sizeof s);
    glyphs_scan(&s, "\xe9\x97\x9c\xe5\x8d\xa1", "");  // U+95DC U+5361
    glyphs_clear(&s);
    CHECK("a new dialog: empty", s.n == 0);
    CHECK("the same characters again are new", glyphs_scan(&s, "\xe9\x97\x9c", "") && s.n == 1 && s.cp[0] == 0x95dc);
    glyphs_clear(&s);
    CHECK("a dialog without any: nothing new", !glyphs_scan(&s, "Mario", "Enter text") && s.n == 0);
}

static void test_failed(void) {
    memset(&s, 0, sizeof s);
    glyphs_scan(&s, "\xe9\x97\x9c\xe5\x8d\xa1\xe3\x83\x86", "");  // U+95DC U+5361 U+30C6
    glyphs_fail(&s, 0x5361);
    CHECK("out of the set", s.n == 2 && s.cp[0] == 0x30c6 && s.cp[1] == 0x95dc && glyphs_failed(&s, 0x5361));
    CHECK("its neighbours aren't failed", !glyphs_failed(&s, 0x5360) && !glyphs_failed(&s, 0x5362));
    CHECK("skipped: nothing new", !glyphs_scan(&s, "\xe5\x8d\xa1", "\xe5\x8d\xa1") && s.n == 2);
    CHECK("with a new one: just that", glyphs_scan(&s, "\xe5\x8d\xa1\xe3\x83\x88", "") && s.n == 3);  // + U+30C8
    CHECK("(U+30C8, not U+5361)", s.cp[0] == 0x30c6 && s.cp[1] == 0x30c8 && s.cp[2] == 0x95dc);
    glyphs_clear(&s);
    CHECK("a new dialog keeps it failed", glyphs_failed(&s, 0x5361));
    CHECK("(still skipped)", !glyphs_scan(&s, "\xe5\x8d\xa1", "") && s.n == 0);
    glyphs_fail(&s, 0x4e00);  // (not in the set)
    CHECK("one not in the set", glyphs_failed(&s, 0x4e00) && s.n == 0);
    glyphs_fail(&s, 0x100);
    glyphs_fail(&s, 0xffff);
    CHECK("the ends", glyphs_failed(&s, 0x100) && glyphs_failed(&s, 0xffff));
    CHECK("(skipped)", !glyphs_scan(&s, "\xc4\x80\xef\xbf\xbf", "") && s.n == 0);
    // a full set starting again leaves out the failed ones too
    memset(&s, 0, sizeof s);
    glyphs_fail(&s, 0x30c6);
    char t[4] = {0};
    for (int c = 0x4e00; c < 0x4e00 + GLYPHS_MAX; c++) {
        t[0] = (char)(0xe0 | c >> 12), t[1] = (char)(0x80 | (c >> 6 & 0x3f)), t[2] = (char)(0x80 | (c & 0x3f));
        glyphs_scan(&s, t, "");
    }
    CHECK("full, starting again", glyphs_scan(&s, "\xe9\x97\x9c", "\xe3\x83\x86") && s.n == 1 && s.cp[0] == 0x95dc);
    memset(&s, 0, sizeof s);
}

static void test_korean(void) {
    CHECK("Chinese and Japanese: no", !glyphs_hangul(0x95dc) && !glyphs_hangul(0x30c6));
    CHECK("a Hangul syllable", glyphs_hangul(0xd14c) && glyphs_hangul(0xac00) && glyphs_hangul(0xd7a3));
    CHECK("a compatibility Jamo", glyphs_hangul(0x3131));
    CHECK("a Jamo", glyphs_hangul(0x1100) && glyphs_hangul(0x11ff));
    CHECK("just outside", !glyphs_hangul(0xabff) && !glyphs_hangul(0xd7a4) && !glyphs_hangul(0x3190));
}

static void test_ranges(void) {
    static uint16_t r[2 * GLYPHS_MAX + 1];
    s.n = 0;
    glyphs_ranges(&s, r);
    CHECK("empty: just the end", r[0] == 0);
    glyphs_scan(&s, "\xe9\x97\x9c\xe5\x8d\xa1", "");
    glyphs_ranges(&s, r);
    CHECK("one pair each, then 0", r[0] == 0x5361 && r[1] == 0x5361 && r[2] == 0x95dc && r[3] == 0x95dc && r[4] == 0);
}

static void test_fonts(void) {
    int o[CJK_FONTS];
    glyphs_font_order("zh-TW", o);
    CHECK("zh-TW: Traditional first", o[0] == 3 && o[1] == 0 && o[2] == 1 && o[3] == 2 && o[4] == 4);
    glyphs_font_order("zh-CN", o);
    CHECK("zh-CN: the Simplified ones first", o[0] == 1 && o[1] == 2 && o[2] == 0 && o[3] == 3 && o[4] == 4);
    glyphs_font_order("ko", o);
    CHECK("ko: Korean first", o[0] == 4 && o[1] == 0 && o[2] == 1 && o[3] == 2 && o[4] == 3);
    static const char *const others[] = {"ja", "en-US", "es", "fr", "de", "it", "nl", "ru", "zh", ""};
    for (int i = 0; i < (int)(sizeof others / sizeof *others); i++) {
        glyphs_font_order(others[i], o);
        CHECK(others[i], o[0] == 0 && o[1] == 1 && o[2] == 2 && o[3] == 3 && o[4] == 4);
    }

    bool tried[CJK_FONTS] = {false};
    glyphs_font_order("zh-TW", o);
    CHECK("the order's first", glyphs_next_font(o, tried, false) == 3);
    tried[3] = true;
    CHECK("then the next", glyphs_next_font(o, tried, false) == 0);
    tried[0] = tried[1] = tried[2] = true;
    CHECK("without Hangul: never Korean", glyphs_next_font(o, tried, false) == -1);
    CHECK("for Hangul: Korean", glyphs_next_font(o, tried, true) == 4);
    bool none[CJK_FONTS] = {false};
    CHECK("for Hangul, Korean before the order's first", glyphs_next_font(o, none, true) == 4);
    tried[4] = true;
    CHECK("all tried", glyphs_next_font(o, tried, true) == -1 && glyphs_next_font(o, tried, false) == -1);
    glyphs_font_order("ko", o);
    CHECK("ko without Hangul: Standard first", glyphs_next_font(o, none, false) == 0);

    // the language changed (glyphs_same_order, glyphs_reorder_font)
    int en[CJK_FONTS], tw[CJK_FONTS], cn[CJK_FONTS], kr[CJK_FONTS];
    glyphs_font_order("en-US", en);
    glyphs_font_order("zh-TW", tw);
    glyphs_font_order("zh-CN", cn);
    glyphs_font_order("ko", kr);
    glyphs_font_order("ja", o);
    CHECK("the same order", glyphs_same_order(en, o) && glyphs_same_order(tw, tw));
    CHECK("another order", !glyphs_same_order(en, tw) && !glyphs_same_order(tw, cn) && !glyphs_same_order(en, kr));
    bool standard[CJK_FONTS] = {true, false, false, false, false};
    CHECK("to zh-TW: Traditional, not read yet", glyphs_reorder_font(tw, standard, false) == 3);
    CHECK("to zh-CN: Simplified", glyphs_reorder_font(cn, standard, false) == 1);
    CHECK("back to en-US: Standard was read", glyphs_reorder_font(en, standard, false) == -1);
    CHECK("to ko without Hangul: Standard was read", glyphs_reorder_font(kr, standard, false) == -1);
    CHECK("to ko with Hangul: Korean", glyphs_reorder_font(kr, standard, true) == 4);
    bool tw_read[CJK_FONTS] = {true, false, false, true, false};
    CHECK("zh-TW's first already tried", glyphs_reorder_font(tw, tw_read, false) == -1);
    CHECK("to zh-CN after it: just the first, not the next", glyphs_reorder_font(cn, tw_read, false) == 1);

    bool four[CJK_FONTS] = {true, true, true, true, false};
    CHECK("a Han character: every font tried but Korean is enough", glyphs_all_tried(four, 0x95dc));
    CHECK("Hangul: not until Korean", !glyphs_all_tried(four, 0xd14c));
    bool three[CJK_FONTS] = {true, true, false, true, true};
    CHECK("a font not read yet: not failed", !glyphs_all_tried(three, 0x95dc) && !glyphs_all_tried(three, 0xd14c));
    four[4] = true;
    CHECK("Hangul: all five", glyphs_all_tried(four, 0xd14c));
}

int main(void) {
    test_scan();
    test_utf8();
    test_full();
    test_clear();
    test_failed();
    test_korean();
    test_ranges();
    test_fonts();
    if (failures) { printf("%d FAILED\n", failures); return 1; }
    printf("glyph set: all passed\n");
    return 0;
}
