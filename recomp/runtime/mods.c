// Mods (mods.h): a folder chosen with RECOMP_MOD_DIR. Nothing is active by default.
#include "mods.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "rt.h"

const char *mods_dir(void) {
    const char *d = getenv("RECOMP_MOD_DIR");
    return d && *d ? d : NULL;
}

// The overlay folders, lowest first, one per line: the active mod's romfs/, then the game's (runtime/game_hooks.c: the
// player's mods - none with a mod folder, which takes their place - and the files a game's mods generate, which must
// be served with it: SMM2's costume models are only there)
static char g_overlay[16384 + 1100];
static void overlay_once(void) {
    extern const char *game_romfs_overlay(void);  // (a game's own: none by default)
    const char *d = mods_dir(), *g = game_romfs_overlay();
    snprintf(g_overlay, sizeof g_overlay, "%s%s%s%s", d ? d : "", d ? "/romfs\n" : "", g ? g : "",
             g && *g && g[strlen(g) - 1] != '\n' ? "\n" : "");
}
const char *mods_overlay(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, overlay_once);
    return g_overlay[0] ? g_overlay : NULL;
}

bool mods_romfs_override(const char *path, char *out, size_t cap) {
    const char *g = mods_overlay();
    if (!g) return false;
    struct stat st;
    // (folders, one per line, the last one winning - look from the end)
    const char *end = g + strlen(g);
    while (end > g) {
        const char *start = end;
        while (start > g && start[-1] != '\n') start--;
        if (end > start && (size_t)snprintf(out, cap, "%.*s/%s", (int)(end - start), start, path) < cap &&
            stat(out, &st) == 0 && S_ISREG(st.st_mode))
            return true;
        end = start > g ? start - 1 : g;
    }
    return false;
}

void mods_boot(void) {
    if (mods_dir()) LOG("[mods] %s\n", mods_dir());
    extern void game_modules_loaded(void);  // (runtime/game_hooks.c: nothing)
    game_modules_loaded();
}
