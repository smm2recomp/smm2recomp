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


# The title scene's change into its phase 2, the opening cutscene (main+0x17e432c: orr w1, wzr, #2, then a tail call to
# Lp::Utl::StateMachine::changeState on the machine at +0x238). With a direct boot (games/smm2/mods/boot.c,
# boot_skip_intro) the phase is 4 instead, playing, as smm2-hooks' skip_intro patch for emulators makes it
# (comex/domthewiz/possamato's): the title is up, without the cutscene, and takes a play request sooner.
TITLE_PHASE = MAIN + 0x17e432c
ORR_W1_2 = 0x321F03E1


def title_phase_hook(gen, addr):
    word = gen.words[(addr - gen.lo) // 4]
    if word != ORR_W1_2:
        raise SystemExit(f'patches.py: {addr:#x} is {word:#010x}, not the expected instruction (not v3.0.3?)')
    orig = ' '.join(gen.render_single(gen.lift(addr), addr, None, set()))
    return orig + ' { extern _Bool boot_skip_intro(void); if (boot_skip_intro()) c->x[1] = 4; }'


PATCHES = {PLAYER_UPDATE: player_update_hook, TITLE_PHASE: title_phase_hook}

# Mods (games/smm2/mods/patches.py): host hooks after single instructions; they change nothing unless a mod is on.
import importlib.util as _ilu, os as _os
for _name in ('patches', 'smm1_extras', 'relax'):  # (smm1_extras: the SMM1 feature settings; relax: relaxed checks)
    _spec = _ilu.spec_from_file_location('smm2_mod_' + _name, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)),
                                                                            'mods', _name + '.py'))
    _mods = _ilu.module_from_spec(_spec)
    _spec.loader.exec_module(_mods)
    PATCHES.update(_mods.PATCHES)
