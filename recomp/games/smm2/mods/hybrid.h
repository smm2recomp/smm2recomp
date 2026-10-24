// Which parts of Course World the SMM2 mod answers, for each online backend (games/smm2/mods/mod.h,
// runtime/net/netcfg.h). Plain C11, no guest headers (tools/test_hybrid.c tests it on its own).
//
// - ours: the mod answers everything with hundred=1 (Course World, the Endless Challenge, 100 Mario and the login:
//   the enl session logs in without NEX), nothing without it.
// - nextendo: hybrid ("Nextendo for multiplayer only", DECISIONS.md 2026-10-20): the game's own NEX client logs in to
//   Nextendo, Network Play is theirs; Course World, the Endless Challenge and 100 Mario stay the mod's (TGRCode or
//   our server), whatever hundred= says. course_world=backend leaves all of Course World to Nextendo.
// - local-nex: the game's own NEX client against a local test server; course_world=mod makes it hybrid (tests).
// - ocw: OpenCourse World answers all of it (mods/ocw.c), so the mod answers nothing - as course_world=backend does
//   for nextendo. Course World, the Endless Challenge and 100 Mario are OCW's own.
#pragma once
#include <stdbool.h>

typedef struct {
    bool world;   // the mod answers Course World's requests: courses, makers, the Endless Challenge, 100 Mario
    bool login;   // ... and the login: the enl session and Course World's login sequence (no NEX connection)
    bool hybrid;  // the game's own NEX client is logged in to the backend while the mod answers Course World
} HyMode;

// backend: the NetBackend in effect (0 ours, 1 local-nex, 2 nextendo, 3 ocw); hundred: hundred=1;
// course_world: "" (unset), "mod" or "backend" (any other value counts as unset)
HyMode hy_mode(int backend, bool hundred, const char *course_world);
