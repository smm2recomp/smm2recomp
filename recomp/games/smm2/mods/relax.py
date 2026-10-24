"""Relaxed checks: host patches that make two of the game's own checks always pass (merged into
games/smm2/patches.py). Each has its own setting and all are off by default, so the game is SMM2 unless the player
opts in (games/smm2/mods/relax.c).

Both work the way the console mod does it (_vendor/ocw-client-mod/source/program/main.cpp, its last two patches):
the whole function is made to return 1. Here the guard runs before the function's own first instruction and returns
early when the setting is on (`c->x[0] = 1; return;` is exactly `movz w0, #1; ret`, since the generator renders a
guest `ret` as a plain `return` - tools/gen.py); with the setting off the instruction runs and the function behaves
as before.

Offsets are from the start of `main` (the recomp loads it at 0x7000000000; the generator rebases them when the guest
window moves). Each is checked against the instruction word it expects, so another build of the game fails loudly
instead of being hooked in the wrong place. Both words are stack prologues, which is also what confirms each address
is the entry of its function: the early return is only the same as the console mod's `movz w0, #1; ret` if the hook
sits at the top of the generated function.
"""

MAIN = 0x7000000000

# offset in main -> (expected first instruction word, which setting, what the function is)
FORCE_OK = {
    # sub sp, sp, #0x1a0
    0x17B8020: (0xD10683FF, 'no_word_filter', "the text check (nn::ngc's word filter)"),
    # stp x28, x27, [sp, #-0x60]!
    0x17BB9C0: (0xA9BA6FFC, 'allow_broken_courses', 'the corrupt-course check'),
}


def _force_ok(word, key, what):
    def patch(gen, addr):
        have = gen.words[(addr - gen.lo) // 4]
        if have != word:
            raise SystemExit(f'mods/relax.py: {addr:#x} ({what}) is {have:#010x}, not {word:#010x} (not v3.0.3?)')
        ins = gen.lift(addr)
        if ins.kind == 'bl':  # (a call: through the dispatcher, so the guard still comes first)
            orig = f'c->x[30] = {addr + 4:#x}ull; c->pc = {ins.target:#x}ull; rt_dispatch(c);'
        else:
            orig = ' '.join(gen.render_single(ins, addr, None, set()))
        return (f'{{ extern _Bool smm2_relax_ok(uint64_t); if (smm2_relax_ok({addr:#x}ull)) '
                f'{{ c->x[0] = 1; return; }} }} {orig}')
    return patch


PATCHES = {MAIN + off: _force_ok(word, key, what) for off, (word, key, what) in FORCE_OK.items()}
