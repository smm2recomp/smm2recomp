"""Mod hook points for the generator: host code after single instructions of SMM2 v3.0.3 (merged into
games/smm2/patches.py).

Each entry keeps the game's own instruction and then calls smm2_mod_after(c, address) (games/smm2/mods/hooks.c). The
host side decides, per address and per active mod, whether to change anything (a register, the flags, a store); with
every mod off the instruction behaves exactly as before. So the behaviour of a hook point can change without
recompiling; only adding or removing a point needs the generator again.

Each point is checked against the instruction word it expects, so another build of the game fails loudly instead of
being hooked in the wrong place. Offsets are from the start of `main` (the recomp loads it at 0x7000000000).
"""

MAIN = 0x7000000000

# offset in main -> (expected instruction word, what the host does there when the mod is on)
HOOK_POINTS = {
    # The game's worker request queue (main+0x4d2d80, after its prologue: x0 is the queue): a request whose object was
    # destroyed is made to fail instead of calling into it ("Pure virtual function called!"; worker_guard.c) ...
    0x4d2d90: (0x9100C3FD, 'worker queue: a destroyed object\'s request fails; the queue step takes the queue lock'),
    # ... and the queue's other changes, under one host lock (worker_guard.c): the queue has none of its own, and a
    # request added from the game thread while the worker steps the queue could be lost or a finished one come back
    # (each point: where the lock is taken - the count loaded again under it - or released)
    0x4d2db4: (0xB94002EB, 'worker queue step: the head request read (lock released unless it is a kind 3)'),
    0x4d2dec: (0xAA1303E0, 'worker queue step: kind 3 popped (lock released)'),
    0x4d2f0c: (0xB9407A68, 'worker queue step: count for the pop (lock)'),
    0x4d2f8c: (0xB9407A68, 'worker queue step: count for the pop (lock)'),
    0x4d2f2c: (0xA9437BFD, 'worker queue step: exit (lock released)'),
    0x4d32f8: (0xB9407B69, 'worker queue: a request added (lock)'),
    0x4d3330: (0x910083E0, 'worker queue: a request added (lock released)'),
    0x4d3720: (0xB9407809, 'worker queue: an attach request added (lock)'),
    0x4d3760: (0xA9000509, 'worker queue: an attach request added (lock released)'),
    0x4d3768: (0x2A1F03E0, 'worker queue: queue full (lock released)'),
    0x4d3fe4: (0xB9407A68, 'worker queue: kind 3 requeues (lock)'),
    0x4d4064: (0xA9457BFD, 'worker queue: kind 3 requeues (lock released)'),
    0x4d4134: (0xB9407A69, 'worker queue: an attach request added (lock)'),
    0x4d4170: (0xA9000109, 'worker queue: an attach request added (lock released)'),
    0x4d4184: (0x2A1F03E0, 'worker queue: queue full (lock released)'),
    0x4d41a0: (0xB9407809, 'worker queue: a detach request added (lock)'),
    0x4d41e0: (0xA9000509, 'worker queue: a detach request added (lock released)'),
    0x4d41e8: (0x2A1F03E0, 'worker queue: queue full (lock released)'),
    # 100 Mario: the in-course lives clamp is 99 (per-frame score / coin / lives update): one point loads the cap
    # the clamps fall back to, the others compare a new lives count with it. With the mod: 100.
    0x16ce764: (0x52800C6E, 'lives cap: cap value'),
    0x16ce810: (0x71018CBF, 'lives cap: compare'),
    0x16ce92c: (0x71018C9F, 'lives cap: compare'),
    0x16ce9ac: (0x71018C9F, 'lives cap: compare'),
    0x16cea5c: (0x71018CBF, 'lives cap: compare'),
    0x16ceaa0: (0x71018D3F, 'lives cap: compare'),
    # 100 Mario: the per-player info the HUD and menus show (lives as a byte, clamped to 99), four players: each
    # clamp compares with 99 and loads 99 as the fallback
    0x16848e8: (0x71018D7F, 'lives shown: compare'),
    0x16848ec: (0x52800C6C, 'lives shown: cap value'),
    0x168494c: (0x71018D7F, 'lives shown: compare'),
    0x1684950: (0x52800C6C, 'lives shown: cap value'),
    0x16849b0: (0x71018D7F, 'lives shown: compare'),
    0x16849b4: (0x52800C6C, 'lives shown: cap value'),
    0x1684a14: (0x71018D5F, 'lives shown: compare'),
    0x1684a18: (0x52800C6B, 'lives shown: cap value'),
    0x1684a8c: (0x52800C6E, 'lives shown: cap value'),
    0x1684abc: (0x71018C1F, 'lives shown: compare'),
    # 100 Mario: the skip page marks "skipped during the miss demo" (Endless run flag +0x48 = 1), which costs a life.
    # SMM1's skip is always free: the host stores 0 again.
    0x15877b0: (0x39012109, 'skip during the miss demo: the run flag store'),
    # 100 Mario: course set-up, Endless retry branch, after clears and lives were reloaded from the run record. SMM1
    # throws away the coins and score of a failed attempt: the host copies them back from the course-start snapshot.
    0x16dfb40: (0x29012509, 'retry: the last lives store'),
    # 100 Mario: Endless banks extra lives as pending 1-Ups (max 3 per course), adding n for an award worth n (a 3-Up
    # Moon, a big coin haul). SMM1 counts every award as one: the host notes the pending count when it is loaded and
    # caps the stored one at that + 1. Two paths (the coin roll-over and the 1-Up items), load and store each.
    0x16ce788: (0xB9403A04, 'pending 1-Ups: load (coins)'),
    0x16ce7f4: (0xB9003A04, 'pending 1-Ups: store (coins)'),
    0x16ce9e0: (0xB9403A04, 'pending 1-Ups: load (items)'),
    0x16cea48: (0xB9003A04, 'pending 1-Ups: store (items)'),
    # Costumes (costume.c): the damage function's form load (ldr w8, [x19, #0x4a8]); in costume form 0 reads as 1,
    # so a hit takes the costume off with the Super -> small power-down instead of killing small Mario
    0x15a50a4: (0xB944AA68, 'costume: damage form load'),
    # Costumes: the form change request (0x71015a94a0, player, int *form; called directly by the item code) after
    # its first instruction (sub sp, sp, #0x80): from small Mario the Mystery item's form becomes 0 and the costume
    # goes on
    0x15a94a0: (0xD10203FF, 'costume: form change request'),
    # Costumes: the player's jump sound (0x71015c78b0, player; called from the jump states) loads the jump request's
    # "silent" byte (ldrb w8, [x0, x8], x8 = 0x24a4) and returns at once when it is set. In costume it reads 1, so
    # SMM2's jump sound ("jump", "voice_jump_high") is not requested and the costume's own plays instead (SMM1)
    0x15c78c4: (0x38686808, 'costume: jump sound, the silent flag load'),
    # Costumes: a player state's enter (0x71015cfe70, in the player state table at 0x7102954ff0) plays "jump" by name
    # itself (0x7100666af0 on the player's sound user, x0); after the key's address is made (add x1, x1, #0x1e4) a player
    # in costume gets a user with no sound instance (+0x10 = 0), which that call answers with an empty handle
    0x15cfe90: (0x91079021, 'costume: a state\'s own jump sound, the key load'),
    # Lanky Mario (lanky.c): the Jump state's begin (0x71015c8a60), the first instruction after the jump launch
    # (player vtable +0x1180) returns (ldr w8, [x19, #0x2738]); a lanky player's computed jump (the jump request's vy
    # at +0x248c is NaN) gets +0.5 on vy ([x19 + 0x240]), and the fuwa cue's timer starts
    0x15c8d54: (0xB9673A68, 'lanky: jump launch returned'),
    # Lanky Mario: the item pickup (0x710134ffd0: x19 the item, x20 the player, w9 the form the item gives) loads the
    # player's form (ldr w8, [x20, #0x4a8]); a Weird Mushroom makes a small or Super player lanky
    0x13502c4: (0xB944AA88, 'lanky: item pickup, the player\'s form load'),
    # Co-op (coop.c): the course scene's play mode ([*0x2a6bcd0 + 0xe0], stored by the player spawn 0x7100e581d0): 0 one player
    # (Endless, the title course, ...), 1 local multiplayer (Play Together: the set-up's player count, each player on
    # their own controller), 2 online
    0xe58328: (0xB900E268, 'co-op: the play mode store'),
    # Co-op (coop.c): the controllers' poll (sead's Npad device, 0x7100263d90) reads Npads 0 .. [x0 + 0x28] (and the handheld one);
    # Endless leaves it at player 1's
    0x263db0: (0xB9402808, 'co-op: the controller poll\'s count load'),
}


def _hook(word):
    def patch(gen, addr):
        have = gen.words[(addr - gen.lo) // 4]
        if have != word:
            raise SystemExit(f'mods/patches.py: {addr:#x} is {have:#010x}, not {word:#010x} (not v3.0.3?)')
        orig = ' '.join(gen.render_single(gen.lift(addr), addr, None, set()))
        return f'{orig} {{ extern void smm2_mod_after(Cpu *, uint64_t); smm2_mod_after(c, {addr:#x}ull); }}'
    return patch


PATCHES = {MAIN + off: _hook(word) for off, (word, _) in HOOK_POINTS.items()}


# OpenCourse World (mods/ocw.c): the call that builds the WebSocket handshake's HTTP headers. The host builds them
# instead, so the player's OCW token rides along in one of them; this is the console mod's one code patch
# (_vendor/ocw-client-mod/source/program/main.cpp branch-links over the same instruction).
#
# Unlike the hook points above this one *replaces* the instruction rather than running after it, because the point is
# to do the call's job differently. With OCW off the host hook returns false and the game's own call is made exactly
# as before, through the recompiled function the instruction called (so no assumption is made about the format string
# or its arguments).
OCW_HEADER = 0xAF560
OCW_HEADER_WORD = 0x947020B4  # bl main+0x1cb7830 (the string formatting the header is built with)


def ocw_header(gen, addr):
    have = gen.words[(addr - gen.lo) // 4]
    if have != OCW_HEADER_WORD:
        raise SystemExit(f'mods/patches.py: {addr:#x} is {have:#010x}, not {OCW_HEADER_WORD:#010x} (not v3.0.3?)')
    ins = gen.lift(addr)
    if ins.kind != 'bl':
        raise SystemExit(f'mods/patches.py: {addr:#x} is {have:#010x}, not a call (not v3.0.3?)')
    return ('{ extern _Bool smm2_ocw_header(Cpu *); extern void guest_call_orig(Cpu *, uint64_t); '
            f'if (!smm2_ocw_header(c)) guest_call_orig(c, {ins.target:#x}ull); }}')


PATCHES[MAIN + OCW_HEADER] = ocw_header
