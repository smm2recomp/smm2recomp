// OpenCourse World (mods/ocw.c): the game's own online client against the player's own OCW account.
#pragma once
#include <stdbool.h>

typedef struct Cpu Cpu;

// backend=ocw was chosen and ocw-config.json was read: the domain is written into the game and the handshake carries
// the token. False: nothing of this mod does anything.
bool ocw_active(void);

// The WebSocket handshake, in place of the game's own snprintf call (mods/patches.py). False: OCW is off, and the
// generated code makes the game's own call instead.
bool smm2_ocw_header(Cpu *c);
