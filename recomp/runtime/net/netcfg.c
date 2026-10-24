// The game network stack's configuration (runtime/net/netcfg.h): backend, redirect table, destination policy, TLS
// trust. Read once, the first time anything asks.
//
// Nextendo's addresses and their game server's key [obs, 2026-10-20]: the game server 51.178.29.194:443 presents a
// self-signed certificate (CN=nintendo-private-server) over TLS 1.2; the pin is the SHA-256 of its
// SubjectPublicKeyInfo (`openssl x509 -pubkey | openssl pkey -pubin -outform der | sha256`). Their NAT check answers
// on 164.132.111.120 for nncs2-* (nncs1-* is the game server).
#include "netcfg.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../hle.h"
#include "hostsock.h"
#include "nextendo.h"
#ifndef _WIN32
#include <netdb.h>
#endif

// a game's own settings store (runtime/game_hooks.c, weak: none): SMM2 keeps these in smm2mods.ini
bool game_net_config(const char *key, char *out, size_t cap);
void game_net_config_set(const char *key, const char *val);

#define MAX_RULES 32
typedef struct { char pattern[128]; uint32_t ip; } Rule;  // "name" or "*.domain"

#define NEXTENDO_SERVER "51.178.29.194"
#define NEXTENDO_NAT "164.132.111.120"
#define NEXTENDO_NAT_HOST "nncs2-lp1.n.n.srv.nintendo.net"
#define NEXTENDO_PIN "d2e7388e48ee30cfbecfbeb522874f2cc6b85f7aa1105c57669f3c2aa85fb30e"

static pthread_once_t once = PTHREAD_ONCE_INIT;
static NetBackend backend_set = NET_BACKEND_OURS, backend_eff = NET_BACKEND_OURS;
static char fallback_why[160];  // why this session runs as ours though another backend is set (net_backend_fall_back)
static NetTrust trust = NET_TRUST_LOCAL_CA;
static bool log_on, log_data_on;
static uint32_t test_ip = 0x7f000001;
static uint16_t test_port = 44300;
static Rule rules[MAX_RULES];
static int nrules;
static char ca_path[512] = "net/test-ca.pem", token_file[512], nex_token_file[512];
static uint32_t nx_server, nx_nat;  // nextendo: the game server and the NAT check (host order)
static uint8_t pin[32];

static pthread_mutex_t rev_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { uint32_t ip; char name[128]; } rev[16];
static int nrev;

static const char *const backend_keys[NET_BACKEND_COUNT] = {"ours", "local-nex", "nextendo", "ocw"};

// ---- the ocw backend's one host (net_ocw_set_target, from games/smm2/mods/ocw.c)
#define OCW_MAX_IPS 8
#define OCW_TTL_S 300    // re-resolve the name this often: OCW's address is in AWS and will move
#define OCW_RETRY_S 15   // ... and after a lookup that found nothing, no sooner than this (the game retries hard)
static pthread_mutex_t ocw_lock = PTHREAD_MUTEX_INITIALIZER;
static char ocw_name[128];                 // lower case, as net_resolve compares
static char ocw_fixed[64];                 // the config's "ip": the name points here, the resolver is never asked
static uint32_t ocw_ips[OCW_MAX_IPS];      // every address the name has answered with (merged, never dropped: a
static int ocw_nips;                       // session already connected to one must keep sending to it)
static time_t ocw_when;
static bool trust_set;  // tls_trust was given explicitly (the ocw backend defaults to "any" without it)

static bool cfg(const char *env, const char *key, char *out, size_t cap) {
    const char *e = getenv(env);
    if (e) { snprintf(out, cap, "%s", e); return true; }
    return game_net_config(key, out, cap);
}

void ip_to_str(uint32_t ip, char out[16]) {
    snprintf(out, 16, "%u.%u.%u.%u", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255);
}

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static bool parse_ip(const char *s, uint32_t *ip) {
    unsigned a, b, c, d;
    char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return false;
    *ip = a << 24 | b << 16 | c << 8 | d;
    return true;
}

bool net_ip_is_local(uint32_t ip) {
    return (ip >> 24) == 127 || (ip >> 24) == 10 || (ip >> 20) == (172 << 4 | 1) || (ip >> 16) == (192 << 8 | 168) ||
           (ip >> 16) == (169 << 8 | 254) || (ip >> 22) == (100u << 2 | 1);  // (100.64.0.0/10: CGNAT, e.g. Tailscale)
}

static bool parse_pin(const char *s, uint8_t out[32]) {
    if (strlen(s) != 64) return false;
    for (int i = 0; i < 32; i++) {
        int h = hexval(s[2 * i]), l = hexval(s[2 * i + 1]);
        if (h < 0 || l < 0) return false;
        out[i] = (uint8_t)(h << 4 | l);
    }
    return true;
}

// allow_public: the backend's own targets (Nextendo's servers); the user's net_hosts= entries stay local-only
static void add_rule(const char *pattern, uint32_t ip, const char *why, bool allow_public) {
    char a[16];
    ip_to_str(ip, a);
    if (!allow_public && !net_ip_is_local(ip)) {
        LOG("[net] redirect %s -> %s refused: not a local address (only loopback / private test servers)\n", pattern, a);
        return;
    }
    if (nrules == MAX_RULES) return;
    snprintf(rules[nrules].pattern, sizeof rules[nrules].pattern, "%s", pattern);
    for (char *p = rules[nrules].pattern; *p; p++) *p = (char)tolower((unsigned char)*p);
    rules[nrules++].ip = ip;
    if (log_on) LOG("[net] redirect %s -> %s (%s)\n", pattern, a, why);  // (inside load: not NLOG)
}

static void load(void) {
    char v[1024];
    if (cfg("RECOMP_NET_LOG", "net_log", v, sizeof v)) log_on = atoi(v) != 0;
    if (cfg("RECOMP_NET_LOG_DATA", "net_log_data", v, sizeof v)) log_data_on = atoi(v) != 0;
    if (cfg("RECOMP_NET_BACKEND", "backend", v, sizeof v)) {
        for (int i = 0; i < NET_BACKEND_COUNT; i++)
            if (!strcmp(v, backend_keys[i])) backend_set = (NetBackend)i;
        if (!strcmp(v, "localnex") || !strcmp(v, "local_nex")) backend_set = NET_BACKEND_LOCAL_NEX;
    }
    backend_eff = backend_set;
    if (backend_set == NET_BACKEND_NEXTENDO && !nextendo_signed_in()) {  // (the store only: no network)
        LOG("[net] backend nextendo: not signed in; using our server\n");
        backend_eff = NET_BACKEND_OURS;
    }
    // (signed in: an account Nextendo refuses online - playing on another device, e-mail not verified... - would fail
    // the game's login at Course World; this session uses our server instead, net_backend_fall_back)
    bool allow = true;
    char reason[64] = "";
    if (backend_eff == NET_BACKEND_NEXTENDO && !getenv("RECOMP_NET_NO_PROBE") &&
        nextendo_online_status(3000, &allow, reason, sizeof reason) && !allow) {
        snprintf(fallback_why, sizeof fallback_why, "Nextendo Network refuses this account online (%s)", reason);
        LOG("[net] backend nextendo: %s; this session uses our server (Network Play off until the next start)\n", fallback_why);
        backend_eff = NET_BACKEND_OURS;
    }
    if (cfg("RECOMP_NET_TEST_SERVER", "nex_test_server", v, sizeof v)) {
        uint32_t ip;
        char *colon = strchr(v, ':');
        if (colon) *colon = 0, test_port = (uint16_t)atoi(colon + 1);
        if (parse_ip(v, &ip) && net_ip_is_local(ip)) test_ip = ip;
        else LOG("[net] nex_test_server=%s: a local IPv4 address wanted; using 127.0.0.1\n", v);
        if (!test_port) test_port = 443;
    }
    if (cfg("RECOMP_NET_TLS_TRUST", "tls_trust", v, sizeof v))
        trust = !strcmp(v, "any") ? NET_TRUST_ANY : NET_TRUST_LOCAL_CA, trust_set = true;
    if (cfg("RECOMP_NET_TLS_CA", "tls_ca", v, sizeof v) && v[0]) snprintf(ca_path, sizeof ca_path, "%s", v);
    if (cfg("RECOMP_NET_ID_TOKEN_FILE", "id_token_file", v, sizeof v)) snprintf(token_file, sizeof token_file, "%s", v);
    if (cfg("RECOMP_NET_NEX_TOKEN_FILE", "nex_token_file", v, sizeof v)) snprintf(nex_token_file, sizeof nex_token_file, "%s", v);
    if (backend_eff == NET_BACKEND_OCW) {
        // No redirect table: the one name the config gives resolves (net_ocw_set_target), nothing else. Certificates
        // are not checked towards it unless tls_trust says otherwise.
        if (!trust_set) trust = NET_TRUST_ANY;
        LOG("[net] backend ocw: the game's own online client against OpenCourse World, certificates %s\n",
            trust == NET_TRUST_ANY ? "not checked" : "checked against tls_ca");
        return;
    }
    if (backend_eff == NET_BACKEND_OURS) return;  // (no name resolves, no game socket reaches the host)
    // explicit entries first (first match wins), then the game's hosts -> the test server / Nextendo
    if (cfg("RECOMP_NET_HOSTS", "net_hosts", v, sizeof v)) {
        for (char *tok = strtok(v, ";, "); tok; tok = strtok(NULL, ";, ")) {
            char *eq = strchr(tok, '=');
            uint32_t ip;
            if (!eq || !parse_ip(eq + 1, &ip)) { LOG("[net] net_hosts entry \"%s\": name=ip wanted\n", tok); continue; }
            *eq = 0;
            add_rule(tok, ip, "net_hosts", false);
        }
    }
    static const char *const game_domains[] = {"*.nintendo.net", "*.nintendo.com", "*.nintendowifi.net",
                                               "*.nintendo.co.jp", "*.nintendoswitch.cn"};
    char a[16];
    if (backend_eff == NET_BACKEND_NEXTENDO) {
        parse_ip(NEXTENDO_SERVER, &nx_server), parse_ip(NEXTENDO_NAT, &nx_nat), parse_pin(NEXTENDO_PIN, pin);
        uint32_t ip;
        if (cfg("RECOMP_NEXTENDO_SERVER", "nextendo_server", v, sizeof v)) {
            if (parse_ip(v, &ip) && ip) nx_server = ip;
            else LOG("[net] nextendo_server=%s: an IPv4 address wanted; using " NEXTENDO_SERVER "\n", v);
        }
        if (cfg("RECOMP_NEXTENDO_NAT", "nextendo_nat", v, sizeof v)) {
            if (parse_ip(v, &ip) && ip) nx_nat = ip;
            else LOG("[net] nextendo_nat=%s: an IPv4 address wanted; using " NEXTENDO_NAT "\n", v);
        }
        if (cfg("RECOMP_NEXTENDO_TLS_PIN", "nextendo_tls_pin", v, sizeof v) && !parse_pin(v, pin))
            LOG("[net] nextendo_tls_pin: 64 hex digits wanted (the SPKI SHA-256); using the built-in pin\n");
        add_rule(NEXTENDO_NAT_HOST, nx_nat, "Nextendo NAT check", true);
        for (size_t i = 0; i < sizeof game_domains / sizeof *game_domains; i++)
            add_rule(game_domains[i], nx_server, "Nextendo game server", true);
        trust = NET_TRUST_PINNED;
        ip_to_str(nx_server, a);
        LOG("[net] backend nextendo: the game's own NEX client against Nextendo Network (game server %s, TLS: pinned "
            "certificate)\n", a);
        return;
    }
    for (size_t i = 0; i < sizeof game_domains / sizeof *game_domains; i++) add_rule(game_domains[i], test_ip, "test server", false);
    ip_to_str(test_ip, a);
    LOG("[net] backend local-nex: the game's own NEX client against the local test server %s:%u (TLS trust: %s)\n", a, test_port,
        trust == NET_TRUST_ANY ? "any certificate" : "local test CA");
}

static void init(void) { pthread_once(&once, load); }

NetBackend net_backend(void) { init(); return __atomic_load_n(&backend_eff, __ATOMIC_ACQUIRE); }
// The backend failed its login this session (a game's mod says when: games/smm2/mods/online.c): the rest of the session
// runs as ours - Course World, the Endless Challenge and 100 Mario from the mod, Network Play off - and the setting
// stays for the next start (owner, 2026-10-23: Course World never depends on Nextendo).
bool net_backend_fall_back(const char *why) {
    init();
    if (__atomic_load_n(&backend_eff, __ATOMIC_ACQUIRE) == NET_BACKEND_OURS) return false;
    snprintf(fallback_why, sizeof fallback_why, "%s", why);
    LOG("[net] backend %s: %s; this session uses our server (Network Play off until the next start)\n",
        backend_keys[backend_eff], why);
    __atomic_store_n(&backend_eff, NET_BACKEND_OURS, __ATOMIC_RELEASE);
    return true;
}
const char *net_backend_fallback_why(void) { init(); return fallback_why; }
NetBackend net_backend_setting(void) { init(); return backend_set; }
const char *net_backend_key(NetBackend b) { return b < NET_BACKEND_COUNT ? backend_keys[b] : "?"; }
bool net_real_sockets(void) {
    NetBackend b = net_backend();
    return b == NET_BACKEND_LOCAL_NEX || b == NET_BACKEND_NEXTENDO || b == NET_BACKEND_OCW;
}
bool net_log_on(void) { init(); return log_on; }
bool net_log_data(void) { init(); return log_data_on; }

// ---- what must never be read out of a log. The PID is the player's account id, which the TLS plaintext dump would
// show in every NEX station URL; a registered secret is an account token (net_log_secret, e.g. OCW's). A log file is
// what a player attaches to a bug report.
#define MAX_SECRETS 4
#define MIN_SECRET 8
static pthread_mutex_t secret_lock = PTHREAD_MUTEX_INITIALIZER;
static char secrets[MAX_SECRETS][160];
static int nsecrets;

void net_log_secret(const char *s) {
    if (!s || strlen(s) < MIN_SECRET) return;  // (too short to find without hitting ordinary text)
    pthread_mutex_lock(&secret_lock);
    bool have = false;
    for (int i = 0; i < nsecrets; i++) have |= !strcmp(secrets[i], s);
    if (!have && nsecrets < MAX_SECRETS) snprintf(secrets[nsecrets++], sizeof secrets[0], "%s", s);
    pthread_mutex_unlock(&secret_lock);
}

// Every registered secret in s, overwritten in place (s is the redacted copy, never the caller's own text).
static void hide_secrets(char *s) {
    if (!nsecrets) return;
    pthread_mutex_lock(&secret_lock);
    for (int i = 0; i < nsecrets; i++) {
        size_t n = strlen(secrets[i]);
        for (char *p = strstr(s, secrets[i]); p; p = strstr(p + n, secrets[i])) memset(p, 'x', n);
    }
    pthread_mutex_unlock(&secret_lock);
}

void net_log_redact(const char *in, char *out, size_t cap) {
    static const char mask[] = "<hidden>";
    size_t o = 0;
    if (!cap) return;
    while (*in && o + 1 < cap) {
        if (tolower((unsigned char)in[0]) == 'p' && tolower((unsigned char)in[1]) == 'i' &&
            tolower((unsigned char)in[2]) == 'd' && in[3] == '=' && isdigit((unsigned char)in[4])) {
            for (int i = 0; i < 4 && o + 1 < cap; i++) out[o++] = *in++;
            while (isdigit((unsigned char)*in)) in++;
            for (const char *m = mask; *m && o + 1 < cap; m++) out[o++] = *m;
            continue;
        }
        out[o++] = *in++;
    }
    out[o] = 0;
    hide_secrets(out);
}
uint32_t net_test_server_ip(void) { init(); return test_ip; }
uint16_t net_dest_port(uint32_t ip, uint16_t port) {
    init();
    return backend_eff == NET_BACKEND_LOCAL_NEX && ip == test_ip && port == 443 ? test_port : port;
}
NetTrust net_tls_trust(void) { init(); return trust; }
const char *net_tls_ca_path(void) { init(); return ca_path; }
const char *net_id_token_file(void) { init(); return token_file[0] ? token_file : NULL; }
const char *net_nex_token_file(void) { init(); return nex_token_file[0] ? nex_token_file : NULL; }
const char *net_nsd_environment(void) { return "lp1"; }

bool net_tls_pin(uint8_t out[32]) {
    init();
    if (trust != NET_TRUST_PINNED) return false;
    memcpy(out, pin, 32);
    return true;
}

int net_backend_options(const NetBackendOption **out) {
#define OPTS(nextendo_available)                                                                                      \
    {{NET_BACKEND_OURS, "ours", "Our server",                                                                         \
      "Course World against your course server (the mod's server= setting), answered by the mod. Nothing else "      \
      "goes online.", true},                                                                                          \
     {NET_BACKEND_LOCAL_NEX, "local-nex", "Local test server",                                       \
      "The game's own online code talks to a NEX test server on this computer or your local network "                \
      "(tools/nex_test_server). For development; Course World needs our server.", true},                             \
     {NET_BACKEND_NEXTENDO, "nextendo", "Nextendo Network",                                                           \
      "Network Play (online co-op and versus) on Nextendo's community servers. Course World, the Endless "          \
      "Challenge and 100 Mario stay on your course source (TGRCode or your server). Needs a Nextendo account "       \
      "(sign in below).",                                                                                             \
      nextendo_available},                                                                                            \
     {NET_BACKEND_OCW, "ocw", "OpenCourse World",                                                                     \
      "Course World against your own OpenCourse World account: the game's own online code talks to OCW's server. "  \
      "Needs its domain and your personal token (below). The only backend that reaches a public host, and it "       \
      "reaches that one host only; its certificate is not checked.", true}}
    static const NetBackendOption signed_out[] = OPTS(false), signed_in[] = OPTS(true);
#undef OPTS
    *out = nextendo_signed_in() ? signed_in : signed_out;
    return (int)(sizeof signed_in / sizeof *signed_in);
}

bool net_backend_select(NetBackend b) {
    const NetBackendOption *o;
    int n = net_backend_options(&o);
    if ((int)b >= n || !o[b].available) return false;
    init();  // (before backend_set is written: a later load() would read the ini back over it)
    game_net_config_set("backend", backend_keys[b]);
    // The setting as chosen, now - a settings page re-reads it every frame to draw its switch, and without this the
    // switch sprang back to the old backend the moment it was moved. What is *in effect* (backend_eff) stays as it
    // was at boot: the network stack is set up around it.
    backend_set = b;
    LOG("[net] backend \"%s\" from the next start\n", backend_keys[b]);
    return true;
}

static bool match(const char *pattern, const char *name) {
    if (pattern[0] == '*' && pattern[1] == '.') {
        size_t pl = strlen(pattern + 1), nl = strlen(name);  // ".domain": the domain itself or any name under it
        return (nl > pl && !strcmp(name + nl - pl, pattern + 1)) || !strcmp(name, pattern + 2);
    }
    return !strcmp(pattern, name);
}

static void remember(uint32_t ip, const char *name) {
    pthread_mutex_lock(&rev_lock);
    int i = 0;
    while (i < nrev && rev[i].ip != ip) i++;
    if (i == nrev) i = nrev < 16 ? nrev++ : 15;
    rev[i].ip = ip;
    snprintf(rev[i].name, sizeof rev[i].name, "%s", name);
    pthread_mutex_unlock(&rev_lock);
}

void net_ocw_set_target(const char *host, const char *ip) {
    pthread_mutex_lock(&ocw_lock);
    size_t n = 0;
    for (; host && host[n] && n < sizeof ocw_name - 1; n++) ocw_name[n] = (char)tolower((unsigned char)host[n]);
    ocw_name[n] = 0;
    while (n && ocw_name[n - 1] == '.') ocw_name[--n] = 0;
    snprintf(ocw_fixed, sizeof ocw_fixed, "%s", ip ? ip : "");
    ocw_nips = 0;
    ocw_when = 0;
    pthread_mutex_unlock(&ocw_lock);
    LOG("[net] ocw host \"%s\"%s%s\n", ocw_name, ocw_fixed[0] ? " at " : "", ocw_fixed);
}

bool net_ocw_host(char *out, size_t cap) {
    pthread_mutex_lock(&ocw_lock);
    snprintf(out, cap, "%s", ocw_name);
    bool any = ocw_name[0] != 0;
    pthread_mutex_unlock(&ocw_lock);
    return any;
}

// Note an address of the ocw host. Addresses are merged, never replaced: a session already connected to one has to
// go on sending to it even after the name has moved (bsd_host.c checks every send).
static void ocw_add(uint32_t ip) {
    for (int i = 0; i < ocw_nips; i++)
        if (ocw_ips[i] == ip) return;
    if (ocw_nips == OCW_MAX_IPS) return;
    ocw_ips[ocw_nips++] = ip;
    char a[16];
    ip_to_str(ip, a);
    LOG("[net] ocw %s -> %s\n", ocw_name, a);
}

// The ocw host's addresses, resolved on first use and re-resolved every OCW_TTL_S. Call with ocw_lock held.
static void ocw_lookup_locked(void) {
    if (!ocw_name[0]) return;
    time_t now = time(NULL);
    // (ocw_when is the last attempt: a lookup that found nothing must not be retried on every call)
    if (ocw_when && now - ocw_when < (ocw_nips ? OCW_TTL_S : OCW_RETRY_S)) return;
    ocw_when = now;
    uint32_t ip;
    if (ocw_fixed[0]) {  // the config points the name at an address: the resolver is never asked
        if (parse_ip(ocw_fixed, &ip)) ocw_add(ip);
        else LOG("[net] ocw ip=\"%s\": not an IPv4 address\n", ocw_fixed);
        return;
    }
    if (parse_ip(ocw_name, &ip)) { ocw_add(ip); return; }  // the config's domain is itself an address
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hsock_startup();
    int e = getaddrinfo(ocw_name, NULL, &hints, &res);
    if (e || !res) {
        LOG("[net] ocw: cannot resolve %s (%d)\n", ocw_name, e);
        return;
    }
    for (struct addrinfo *a = res; a; a = a->ai_next)
        if (a->ai_family == AF_INET && a->ai_addr)
            ocw_add(ntohl(((struct sockaddr_in *)(void *)a->ai_addr)->sin_addr.s_addr));
    freeaddrinfo(res);
}

// Is `name` the ocw host? Then its address (the first known one).
static bool ocw_resolve(const char *name, uint32_t *ip) {
    bool hit = false;
    pthread_mutex_lock(&ocw_lock);
    if (ocw_name[0] && !strcmp(ocw_name, name)) {
        ocw_lookup_locked();
        if (ocw_nips) *ip = ocw_ips[0], hit = true;
    }
    pthread_mutex_unlock(&ocw_lock);
    if (hit) remember(*ip, name);  // (ssl.c rebuilds the host name from the address for SNI)
    return hit;
}

static bool ocw_owns(uint32_t ip) {
    bool mine = false;
    pthread_mutex_lock(&ocw_lock);
    for (int i = 0; i < ocw_nips && !mine; i++) mine = ocw_ips[i] == ip;
    pthread_mutex_unlock(&ocw_lock);
    return mine;
}

bool net_resolve(const char *name, uint32_t *ip) {
    init();
    char n[256];
    size_t len = 0;
    // (a '%' is nsd's environment, as nsd resolves it: the game asks sfdnsres for "nncs2-%.n.n.srv.nintendo.net")
    for (const char *p = name; *p && len < sizeof n - 1; p++)
        if (*p == '%')
            for (const char *e = net_nsd_environment(); *e && len < sizeof n - 1; e++) n[len++] = *e;
        else n[len++] = (char)tolower((unsigned char)*p);
    n[len] = 0;
    while (len && n[len - 1] == '.') n[--len] = 0;  // (an absolute name)
    if (parse_ip(n, ip)) return true;
    if (!strcmp(n, "localhost")) { *ip = 0x7f000001; return true; }
    if (backend_eff == NET_BACKEND_OCW && ocw_resolve(n, ip)) return true;
    for (int i = 0; i < nrules; i++)
        if (match(rules[i].pattern, n)) {
            *ip = rules[i].ip;
            remember(*ip, n);
            return true;
        }
    return false;
}

bool net_name_for_ip(uint32_t ip, char *out, size_t cap) {
    pthread_mutex_lock(&rev_lock);
    bool found = false;
    for (int i = 0; i < nrev && !found; i++)
        if (rev[i].ip == ip) snprintf(out, cap, "%s", rev[i].name), found = true;
    pthread_mutex_unlock(&rev_lock);
    return found;
}

// The account ID the console mod writes (_vendor/ocw-client-mod/source/program/main.cpp: movz x8, #0xCAFE), shared
// by every OCW player there. Matched exactly so OCW sees what it sees from a console.
#define OCW_NSA_ID 0xCAFEull

bool net_account_shared(uint64_t *id) {
    init();
    if (backend_eff != NET_BACKEND_OCW) return false;
    *id = OCW_NSA_ID;
    return true;
}

bool net_tls_any_allowed(uint32_t ip) {
    init();
    return net_ip_is_local(ip) || (backend_eff == NET_BACKEND_OCW && ocw_owns(ip));
}

// (nextendo: TCP to Nextendo's game server and their NAT check only, not to net_hosts= targets. ocw: the one host
// its config names, and loopback.)
bool net_dest_allowed(uint32_t ip) {
    init();
    if (backend_eff == NET_BACKEND_OURS) return false;
    if (backend_eff == NET_BACKEND_OCW) return (ip >> 24) == 127 || ocw_owns(ip);
    if (backend_eff == NET_BACKEND_NEXTENDO) return ip == nx_server || ip == nx_nat;
    if (backend_eff == NET_BACKEND_LOCAL_NEX && (ip >> 24) == 127) return true;
    for (int i = 0; i < nrules; i++)
        if (rules[i].ip == ip) return true;
    return false;
}

bool net_dest_allowed_udp(uint32_t ip) {
    init();
    if (backend_eff == NET_BACKEND_NEXTENDO) return true;  // (PIA: peer to peer, the addresses come from the server)
    return net_dest_allowed(ip);
}
