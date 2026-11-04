// Mods: files that replace or add to the game's RomFS, and their own save slots (runtime/mods.c). LAS Recomp's
// randomizer seeds used the same hooks; here a mod is a folder with a romfs/ tree (no game code).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The active mod's folder (RECOMP_MOD_DIR), or NULL for the unmodified game. Its romfs/ overrides and adds files,
// its save/ holds the mod's own saves.
const char *mods_dir(void);
// The folders laid over the RomFS, lowest first, one per line: the active mod's romfs/, then the game's own overlay
// (the player's mods, a game's generated files: runtime/game_hooks.c); NULL when none.
const char *mods_overlay(void);
// A replacement for the RomFS file `path` ("Folder/name"), from the overlay: true with the host file in out.
bool mods_romfs_override(const char *path, char *out, size_t cap);
// Each boot, after the modules load.
void mods_boot(void);
