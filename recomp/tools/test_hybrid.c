// Tests for games/smm2/mods/hybrid.c: which parts of Course World the mod answers for each online backend. Run by
// tools/test_hybrid.sh.
// Usage: test_hybrid [case...]   (no case: all of them)
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../games/smm2/mods/hybrid.h"

static int failures;
#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        if (!(cond)) { printf("  %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); failures++; }     \
    } while (0)

// (backend, hundred, course_world) -> {world, login, hybrid}
static void mode_table(void) {
    static const struct { int backend; bool hundred; const char *cw; HyMode want; } rows[] = {
        {0, false, "", {0, 0, 0}},
        {0, true, "", {1, 1, 0}},
        {0, true, "backend", {1, 1, 0}},
        {1, true, "", {0, 0, 0}},
        {1, false, "mod", {1, 0, 1}},
        {2, false, "", {1, 0, 1}},
        {2, true, "mod", {1, 0, 1}},
        {2, true, "backend", {0, 0, 0}},
        {2, true, "bogus", {1, 0, 1}},
        {3, false, "", {0, 0, 0}},        // ocw: OCW answers all of Course World, the mod none
        {3, true, "", {0, 0, 0}},         // ... whatever hundred= says
        {3, true, "mod", {0, 0, 0}},      // ... and course_world= cannot ask the mod back in
    };
    for (size_t i = 0; i < sizeof rows / sizeof *rows; i++) {
        HyMode m = hy_mode(rows[i].backend, rows[i].hundred, rows[i].cw);
        CHECK(m.world == rows[i].want.world && m.login == rows[i].want.login && m.hybrid == rows[i].want.hybrid,
              "hy_mode(%d, %d, \"%s\") = {%d,%d,%d}, want {%d,%d,%d}", rows[i].backend, rows[i].hundred, rows[i].cw,
              m.world, m.login, m.hybrid, rows[i].want.world, rows[i].want.login, rows[i].want.hybrid);
    }
}

// a NULL course_world counts as unset; an unknown backend number answers nothing
static void mode_edges(void) {
    HyMode m = hy_mode(2, false, NULL);
    CHECK(m.world && !m.login && m.hybrid, "hy_mode(2, 0, NULL) = {%d,%d,%d}, want {1,0,1}", m.world, m.login,
          m.hybrid);
    m = hy_mode(0, true, NULL);
    CHECK(m.world && m.login && !m.hybrid, "hy_mode(0, 1, NULL) = {%d,%d,%d}, want {1,1,0}", m.world, m.login,
          m.hybrid);
    m = hy_mode(1, true, "MOD");
    CHECK(!m.world && !m.login && !m.hybrid, "hy_mode(1, 1, \"MOD\") = {%d,%d,%d}, want {0,0,0}", m.world, m.login,
          m.hybrid);
    m = hy_mode(7, true, "mod");
    CHECK(!m.world && !m.login && !m.hybrid, "hy_mode(7, 1, \"mod\") = {%d,%d,%d}, want {0,0,0}", m.world, m.login,
          m.hybrid);
}

static const struct { const char *name; void (*fn)(void); } cases[] = {
    {"mode_table", mode_table},
    {"mode_edges", mode_edges},
};

int main(int argc, char **argv) {
    int bad = 0;
    for (size_t k = 0; k < sizeof cases / sizeof *cases; k++) {
        bool wanted = argc < 2;
        for (int i = 1; i < argc; i++) wanted |= !strcmp(argv[i], cases[k].name);
        if (!wanted) continue;
        int before = failures;
        cases[k].fn();
        printf("%s %s\n", failures == before ? "ok  " : "FAIL", cases[k].name);
        bad += failures != before;
    }
    if (bad) { printf("FAILURES\n"); return 1; }
    printf("ALL PASS\n");
    return 0;
}
