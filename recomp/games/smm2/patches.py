"""Super Mario Maker 2 v3.0.3: per-instruction patches for the generator (tools/patches.py).

Each patch checks the instruction word it replaces, so another build of the game fails loudly.
"""

MAIN = 0x7000000000

# Entry of the player's per-frame update (main+0x158a5c4, after the prologue): the instruction that copies the player
# object (x0) into x19. The host hook (games/smm2/trace.c: script clock, state trace) runs first, then the copy. The
# Ryujinx trace build replaces the same instruction with an SVC, so both runtimes sample at the same point.
PLAYER_UPDATE = MAIN + 0x158a5c4
MOV_X19_X0 = 0xAA0003F3


def player_update_hook(gen, addr):
    word = gen.words[(addr - gen.lo) // 4]
    if word != MOV_X19_X0:
        raise SystemExit(f'patches.py: {addr:#x} is {word:#010x}, not the expected instruction (not v3.0.3?)')
    return '{ extern void smm2_player_hook(Cpu *); smm2_player_hook(c); } c->x[19] = c->x[0];'


# The player's state frame (main+0x15d3cc0, v3.0.3), after its first instruction (sub sp, sp, #0x60; x0 the player):
# the function smm2-hooks samples its "player" rows at in Eden (smm2-decomp src-sim/test/fixtures/*_eden.json:
# "hook player 0x71015D3CC0"). games/smm2/trace.c writes its records here instead with RECOMP_TRACE_POINT=eden, so a
# trace compares with those recordings field for field.
EDEN_PLAYER = MAIN + 0x15d3cc0
SUB_SP_0x60 = 0xD10183FF


def eden_player_hook(gen, addr):
    word = gen.words[(addr - gen.lo) // 4]
    if word != SUB_SP_0x60:
        raise SystemExit(f'patches.py: {addr:#x} is {word:#010x}, not the expected instruction (not v3.0.3?)')
    orig = ' '.join(gen.render_single(gen.lift(addr), addr, None, set()))
    return orig + ' { extern void smm2_eden_point(Cpu *); smm2_eden_point(c); }'


# Its only return (main+0x15d466c, ret; no tail calls): smm2-hooks reads a hook's fields after the call returns
# (smm2-hooks docs/probe.md), so the record is written here, of the player the entry saw.
EDEN_PLAYER_RET = MAIN + 0x15d466c
RET = 0xD65F03C0


def eden_player_ret_hook(gen, addr):
    word = gen.words[(addr - gen.lo) // 4]
    if word != RET:
        raise SystemExit(f'patches.py: {addr:#x} is {word:#010x}, not the expected instruction (not v3.0.3?)')
    orig = ' '.join(gen.render_single(gen.lift(addr), addr, None, set()))
    return '{ extern void smm2_eden_return(Cpu *); smm2_eden_return(c); } ' + orig


# The sideways spring's execute (main+0x13da5d0, after its first instruction, stp x24, x23, [sp, #-0x40]!; its only
# return main+0x13daadc): games/smm2/trace.c's side-spring probe (RECOMP_SIDE_PROBE_OUT) logs its animation machine
# and its model animation's frame after each execute.
SIDE_EXEC = MAIN + 0x13da5d0
SIDE_EXEC_RET = MAIN + 0x13daadc


def side_exec_hook(gen, addr):
    word = gen.words[(addr - gen.lo) // 4]
    if word != 0xA9BC5FF8:
        raise SystemExit(f'patches.py: {addr:#x} is {word:#010x}, not the expected instruction (not v3.0.3?)')
    orig = ' '.join(gen.render_single(gen.lift(addr), addr, None, set()))
    return orig + ' { extern void smm2_side_probe_enter(Cpu *); smm2_side_probe_enter(c); }'


def side_exec_ret_hook(gen, addr):
    word = gen.words[(addr - gen.lo) // 4]
    if word != RET:
        raise SystemExit(f'patches.py: {addr:#x} is {word:#010x}, not the expected instruction (not v3.0.3?)')
    orig = ' '.join(gen.render_single(gen.lift(addr), addr, None, set()))
    return '{ extern void smm2_side_probe_return(Cpu *); smm2_side_probe_return(c); } ' + orig


PATCHES = {PLAYER_UPDATE: player_update_hook, EDEN_PLAYER: eden_player_hook, EDEN_PLAYER_RET: eden_player_ret_hook,
           SIDE_EXEC: side_exec_hook, SIDE_EXEC_RET: side_exec_ret_hook}

# Mods (games/smm2/mods/patches.py): host hooks after single instructions; they change nothing unless a mod is on.
import importlib.util as _ilu, os as _os
for _name in ('patches', 'smm1_extras', 'relax'):  # (smm1_extras: the SMM1 feature settings; relax: relaxed checks)
    _spec = _ilu.spec_from_file_location('smm2_mod_' + _name, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)),
                                                                            'mods', _name + '.py'))
    _mods = _ilu.module_from_spec(_spec)
    _spec.loader.exec_module(_mods)
    PATCHES.update(_mods.PATCHES)
