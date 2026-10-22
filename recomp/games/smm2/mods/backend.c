// The online backend for SMM2 (runtime/net/netcfg.h): its settings live in smm2mods.ini with the mod's, under these
// keys (environment variables win, as for the mod's own settings):
//
//   backend=ours | local-nex | nextendo | ocw   RECOMP_NET_BACKEND
//   nex_test_server=<ip>                    RECOMP_NET_TEST_SERVER
//   net_hosts=<name=ip;...>                 RECOMP_NET_HOSTS
//   tls_trust=local-ca | any                RECOMP_NET_TLS_TRUST
//   tls_ca=<path>                           RECOMP_NET_TLS_CA
//   id_token_file=<path>                    RECOMP_NET_ID_TOKEN_FILE
//   nex_token_file=<path>                   RECOMP_NET_NEX_TOKEN_FILE
//   net_log=1                               RECOMP_NET_LOG
//   net_log_data=1                          RECOMP_NET_LOG_DATA
//
//   course_world=mod | backend              RECOMP_MOD_COURSE_WORLD (settings.c)
//
// backend=ours (default) is the mod's path: the overrides answer Course World and the login from our course server
// (with hundred=1). Any other backend runs the game's own NEX client on the runtime's network stack, and the mod
// answers less (hybrid.h, settings.c):
// - nextendo: hybrid ("Nextendo for multiplayer only"): the game's NEX client logs in to Nextendo (Network Play is
//   theirs) while the mod's overrides keep answering Course World, the Endless Challenge and 100 Mario from
//   TGRCode (the default here) or our server; course_world=backend leaves all of Course World to Nextendo.
// - local-nex: the mod's online overrides are off (mod_hundred() is false); course_world=mod makes it hybrid, as
//   with nextendo (offline tests against tools/nex_test_server).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mod.h"

void mod_ini_set(const char *key, const char *val);  // settings.c

static const char *ini_path(void) { return getenv("RECOMP_MOD_INI") ? getenv("RECOMP_MOD_INI") : "smm2mods.ini"; }

bool game_net_config(const char *key, char *out, size_t cap) {
    static const char *const keys[] = {"backend", "nex_test_server", "net_hosts", "tls_trust", "tls_ca",
                                       "id_token_file", "nex_token_file", "net_log", "net_log_data",
                                       "nextendo_server", "nextendo_nat", "nextendo_tls_pin"};
    bool known = false;
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) known |= !strcmp(keys[i], key);
    FILE *f = known ? fopen(ini_path(), "r") : NULL;
    if (!f) return false;
    char line[1024];
    bool found = false;
    size_t kl = strlen(key);
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || strncmp(line, key, kl) || line[kl] != '=') continue;
        char *v = line + kl + 1, *e = v + strlen(v);
        while (e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        snprintf(out, cap, "%s", v);
        found = true;
    }
    fclose(f);
    return found;
}

void game_net_config_set(const char *key, const char *val) { mod_ini_set(key, val); }

// RECOMP_NET_TRACE_STATES=<file>: log the states the game's state machines enter (the enl session, Network Play's
// menus ...) while its own NEX client runs. The file (made privately from the player's own game, never shipped) has
// one line per state: "<main offset hex> <0 exec | 1 enter> <label>". Enter functions are logged when called; exec
// functions (called every frame) when a machine's state changes. They are reached through pointers, so overrides
// see them.
#define MAX_TRACED 512
static struct { uint64_t addr; char label[64]; bool enter; } traced[MAX_TRACED];
static int ntraced;
static struct { uint64_t self, pc; } last_exec[64];

static void trace_state(Cpu *c) {
    uint64_t pc = c->pc, self = c->x[0];
    for (int i = 0; i < ntraced; i++) {
        if (traced[i].addr != pc) continue;
        if (traced[i].enter) LOG("[state] %s\n", traced[i].label);
        else {
            int j = 0;
            while (j < 64 && last_exec[j].self && last_exec[j].self != self) j++;
            if (j == 64) j = (int)(self >> 4) & 63;
            if (last_exec[j].self != self || last_exec[j].pc != pc) LOG("[state] %s\n", traced[i].label);
            last_exec[j].self = self, last_exec[j].pc = pc;
        }
        break;
    }
    guest_call_orig(c, pc);
}

__attribute__((constructor)) static void backend_trace_start(void) {
    const char *path = getenv("RECOMP_NET_TRACE_STATES");
    FILE *f = path ? fopen(path, "r") : NULL;
    if (!f) return;
    char line[160];
    while (ntraced < MAX_TRACED && fgets(line, sizeof line, f)) {
        unsigned long long off;
        int enter, n = 0;
        if (sscanf(line, "%llx %d %n", &off, &enter, &n) < 2 || !n) continue;
        char *e = line + strlen(line);
        while (e > line && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
        traced[ntraced].addr = MAIN_ADDR(off), traced[ntraced].enter = enter != 0;
        snprintf(traced[ntraced].label, sizeof traced[ntraced].label, "%s", line + n);
        hle_override(traced[ntraced].addr, trace_state, "net: state trace");
        ntraced++;
    }
    fclose(f);
}
