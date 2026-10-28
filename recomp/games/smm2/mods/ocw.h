// OpenCourse World (mods/ocw.c): the game's own online client against the player's own OCW account.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct Cpu Cpu;

// backend=ocw was chosen and ocw-config.json was read: the domain is written into the game and the handshake carries
// the token. False: nothing of this mod does anything.
bool ocw_active(void);

// After the game's own GetUsers wrapper ran (online.c, world.c): fill the first user record's Play History (courses
// played / cleared, attempts, lives lost) from OCW's REST API. Does nothing with OCW off or a failed request.
void ocw_fill_play_stats(Cpu *c, uint64_t no, uint64_t ret);

// The WebSocket handshake, in place of the game's own snprintf call (mods/patches.py). False: OCW is off, and the
// generated code makes the game's own call instead.
bool smm2_ocw_header(Cpu *c);
