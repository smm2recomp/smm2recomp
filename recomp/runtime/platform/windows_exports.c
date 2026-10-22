// Windows: what the game code DLL (<id>game.dll) calls in the executable - the runtime functions cpu.h declares for the
// generated code, and the hooks a game's generator patches call (smm2_player_hook: games/smm2/patches.py). Exported from the .exe by linker directives (zig's lld-link has no --export-all-symbols), and listed
// again in windows_exports.def, from which the Makefile makes the import library the DLL links against
// (`zig dlltool`; runtime/gamebuild.c links the player's build against it too). Keep the two lists the same.
#ifdef _WIN32
__asm__(".section .drectve\n"
        ".ascii \" -export:rt_dispatch -export:rt_svc -export:rt_brk -export:rt_udf -export:rt_ticks\"\n"
        ".ascii \" -export:fnprof_hit -export:g_fnprof,data -export:smm2_player_hook -export:smm2_mod_after\"\n"
        ".ascii \" -export:smm1_extras_after -export:smm2_relax_ok -export:smm2_ocw_header\"\n"
        ".ascii \" -export:guest_call_orig\"\n"
        ".text\n");
#endif
