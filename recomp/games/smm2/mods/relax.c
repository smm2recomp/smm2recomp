// Relaxed checks: two of the game's own checks made to always pass, each behind its own setting, both off by
// default. They came from the OpenCourse World console mod (_vendor/ocw-client-mod), which applies them
// unconditionally; here they are a mod of their own, so they are useful with any course server and OCW does not
// quietly change the game's behaviour. One switch each in the overlay menu's Mods tab, kept in smm2mods.ini; an
// environment variable wins over the ini:
//
//   RECOMP_NO_WORD_FILTER=1        no_word_filter=1        the text check always passes
//   RECOMP_ALLOW_BROKEN_COURSES=1  allow_broken_courses=1  the corrupt-course check always passes
//
// no_word_filter only changes what this copy of the game filters locally; a course server does its own moderation
// (docs/self-hosting.md). allow_broken_courses makes the game play a course it would otherwise refuse and replace
// with a blank one - useful for courses from an archive, and it can mean a course that genuinely misbehaves.
//
// The hook points are in mods/relax.py.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mod.h"

typedef struct { const char *label, *desc; bool *value; bool def; } GameModToggle;  // (runtime/game_hooks.c)

static bool no_word_filter, allow_broken;

static const struct Setting {
    const char *key, *env;
    bool *value;
    uint64_t addr;  // the function this setting forces (offset in main, as in relax.py)
} settings[] = {
    {"no_word_filter", "RECOMP_NO_WORD_FILTER", &no_word_filter, 0x17B8020},
    {"allow_broken_courses", "RECOMP_ALLOW_BROKEN_COURSES", &allow_broken, 0x17BB9C0},
};
#define NSETTINGS (sizeof settings / sizeof *settings)

static const char *ini_path(void) { return getenv("RECOMP_MOD_INI") ? getenv("RECOMP_MOD_INI") : "smm2mods.ini"; }

static void load_once(void) {
    FILE *f = fopen(ini_path(), "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            char *eq = strchr(line, '=');
            if (line[0] == '#' || !eq) continue;
            *eq = 0;
            for (size_t i = 0; i < NSETTINGS; i++)
                if (!strcmp(line, settings[i].key)) *settings[i].value = atoi(eq + 1) != 0;
        }
        fclose(f);
    }
    for (size_t i = 0; i < NSETTINGS; i++) {
        if (getenv(settings[i].env)) *settings[i].value = atoi(getenv(settings[i].env)) != 0;
        if (*settings[i].value) LOG("[mod] relaxed checks: %s on\n", settings[i].key);
    }
}

static void load(void) {  // (the game's threads and the menu: once)
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, load_once);
}

void mod_ini_set(const char *key, const char *val);  // (settings.c: replace or add one key=value line)

// ---- the hooks (relax.py): true = the function returns 1 without running
bool smm2_relax_ok(uint64_t addr) {
    load();
    for (size_t i = 0; i < NSETTINGS; i++)
        if (addr == MAIN_ADDR(settings[i].addr)) return *settings[i].value;
    return false;
}

// ---- the Mods tab (settings.c appends these to its own)
static bool shown[NSETTINGS];  // what the tab last showed, to see which one changed

int relax_toggles(GameModToggle *out, int room) {
    static const GameModToggle toggles[] = {
        {"No word filter",
         "The game's own text check always passes, so course and maker names it would star out are shown as they "
         "are. Only this copy of the game: a course server does its own moderation. Immediately.",
         &shown[0], false},
        {"Play courses the game calls broken",
         "The game's corrupt-course check always passes, so a course it would refuse (and replace with a blank one) "
         "is played. Useful for courses out of an archive; a course that really is broken may misbehave. From the "
         "next course start.",
         &shown[1], false},
    };
    load();
    int n = 0;
    for (size_t i = 0; i < NSETTINGS && n < room; i++) {
        shown[i] = *settings[i].value;
        out[n++] = toggles[i];
    }
    return n;
}

void relax_toggles_changed(void) {
    for (size_t i = 0; i < NSETTINGS; i++) {
        if (shown[i] == *settings[i].value) continue;
        *settings[i].value = shown[i];
        mod_ini_set(settings[i].key, shown[i] ? "1" : "0");
        LOG("[mod] relaxed checks: %s %s\n", settings[i].key, shown[i] ? "on" : "off");
    }
}
