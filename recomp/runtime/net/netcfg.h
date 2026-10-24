// The game's own network stack (runtime/services/bsd.c sockets, sfdnsres.c names, nsd.c, ssl.c TLS, acc.c token):
// which backend it talks to, which names resolve where, which addresses a game socket may reach, and how TLS
// servers are trusted. Deny by default: nothing here ever asks the host's DNS for a name the game gives, and a game
// socket reaches only the addresses the redirect table names (and, with nextendo, PIA peers over UDP).
//
// Backends (setting "backend"):
//   ours       (default) the current mod path: the mods answer Course World from our course server over HTTP
//              (games/smm2/mods); the game's own sockets stay off (only the virtual LAN, runtime/net/vlan.h)
//   local-nex  the game's own NEX client runs (the mods' online overrides switch off) against a local NEX test
//              server (tools/nex_test_server): real TCP / UDP sockets, the game's hosts resolve to that server
//   nextendo   Nextendo Network (runtime/net/nextendo.h): the game's own NEX client against Nextendo's SMM2 server.
//              Needs a signed-in Nextendo account; without one it is logged and the backend is "ours". The game's
//              hosts resolve to Nextendo's game server (nncs2-lp1.n.n.srv.nintendo.net: their NAT check), TCP
//              reaches those two only (not net_hosts= targets), UDP any address (PIA's peers), and the game server's TLS certificate is
//              pinned (its public key's SHA-256). The account's PID, user name and an ID token carrying the
//              account's game token come from acc (services/acc.c, runtime/net/token.c)
//   ocw        OpenCourse World: the game's own online client against the player's own OCW account
//              (games/smm2/mods/ocw.c reads ocw-config.json in the data folder and calls net_ocw_set_target).
//              The only backend that reaches a public host, and it reaches exactly the one name that config
//              names - every other name still fails and every other address is still refused. Certificates are
//              not verified there (see net_tls_any_allowed), so a self-hosted OCW with a self-signed
//              certificate works too; the player opts in, and it is never the default.
//
// Settings: environment variable, else the game's settings (game_net_config: smm2mods.ini for SMM2):
//   RECOMP_NET_BACKEND        backend=           ours | local-nex | nextendo
//   RECOMP_NET_TEST_SERVER    nex_test_server=   the local NEX test server, ip[:port] (default 127.0.0.1:44300); must
//                                                be a local address (loopback, private, link-local, CGNAT 100.64/10).
//                                                The game's TCP connections to it on port 443 go to that port (443
//                                                needs privileges on most hosts).
//   RECOMP_NET_HOSTS          net_hosts=         extra redirect entries "name=ip;*.domain=ip" (local addresses only;
//                                                they come before the backend's own)
//   RECOMP_NET_TLS_TRUST      tls_trust=         local-nex: local-ca (default): server certificates must chain to the
//                                                local test CA; any: accept any certificate (only towards local
//                                                addresses). (nextendo: always the pin)
//   RECOMP_NET_TLS_CA         tls_ca=            the local test CA (PEM), default net/test-ca.pem in the data folder
//   RECOMP_NEXTENDO_SERVER    nextendo_server=   nextendo: the game server's address (default 51.178.29.194)
//   RECOMP_NEXTENDO_NAT       nextendo_nat=      nextendo: the NAT check's address (default 164.132.111.120)
//   RECOMP_NEXTENDO_TLS_PIN   nextendo_tls_pin=  nextendo: the game server's SPKI SHA-256, 64 hex digits
//   RECOMP_NET_ID_TOKEN_FILE  id_token_file=     a backend-issued account id token to send instead of the local one
//   RECOMP_NET_NEX_TOKEN_FILE nex_token_file=    local-nex: a backend's NEX login token, carried in the local ID
//                                                token's "nnex" claim (runtime/net/token.c)
//   RECOMP_NET_LOG            net_log=1          log every socket, name, TLS and token call
//   RECOMP_NET_LOG_DATA       net_log_data=1     ... and the TLS plaintext itself (the first 160 bytes of each read
//                                                and write): noisy, and off even with net_log=1. Registered secrets
//                                                are hidden from it (net_log_secret)
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    NET_BACKEND_OURS = 0,
    NET_BACKEND_LOCAL_NEX = 1,
    NET_BACKEND_NEXTENDO = 2,
    NET_BACKEND_OCW = 3,
    NET_BACKEND_COUNT
} NetBackend;
typedef enum { NET_TRUST_LOCAL_CA = 0, NET_TRUST_ANY = 1, NET_TRUST_PINNED = 2 } NetTrust;

NetBackend net_backend(void);          // in effect (nextendo without a signed-in account -> ours)
NetBackend net_backend_setting(void);  // as set
bool net_backend_fall_back(const char *why);  // this session runs as ours from now on (false: it already does)
const char *net_backend_fallback_why(void);  // why it does though another backend is set ("": it doesn't)
const char *net_backend_key(NetBackend b);  // "ours", "local-nex", "nextendo"
bool net_real_sockets(void);           // the game's sockets reach the host's network stack (local-nex, nextendo)
bool net_log_on(void);
bool net_log_data(void);  // ... and the TLS plaintext (net_log_data=1; off even with net_log=1)
// A log line with what must not be read from it taken out: every "PID=<digits>" (any case; NEX station URLs carry
// it) becomes "PID=<hidden>", and so does any string registered with net_log_secret (an account token). Always
// NUL-terminated within cap; a cut digit run is dropped, never printed.
void net_log_redact(const char *in, char *out, size_t cap);
// A secret that must never appear in a log, registered before the game runs (a log file is what a player attaches
// to a bug report). Shorter than 8 characters: ignored, too short to find in ordinary text safely.
void net_log_secret(const char *s);
#define NLOG(...) do { if (net_log_on()) LOG(__VA_ARGS__); } while (0)

// For a settings page: the backends, which can be chosen now, and choosing one (saved; takes effect at the next
// start). Nextendo is available while a Nextendo account is signed in.
typedef struct { NetBackend id; const char *key, *label, *desc; bool available; } NetBackendOption;
int net_backend_options(const NetBackendOption **out);
bool net_backend_select(NetBackend b);  // false: not available

// Names. Returns true and the IPv4 address (host order) the redirect table gives `name`, false = no such name
// (every name not in the table). Literal dotted addresses resolve to themselves.
bool net_resolve(const char *name, uint32_t *ip);
// The last name resolved to ip (TLS: the game connects by address and names the address as the host); false = none
bool net_name_for_ip(uint32_t ip, char *out, size_t cap);
// May a game socket connect / send to ip (host order)? TCP (and anything not UDP): local-nex: loopback and the
// redirect table's targets; nextendo: Nextendo's game server and NAT check only. UDP: the same with local-nex; any
// address with nextendo (PIA's peers).
bool net_dest_allowed(uint32_t ip);
bool net_dest_allowed_udp(uint32_t ip);
bool net_ip_is_local(uint32_t ip);     // loopback, private, link-local, CGNAT
uint32_t net_test_server_ip(void);     // host order
// The port a game connection to ip:port really goes to (the test server's port stands in for 443)
uint16_t net_dest_port(uint32_t ip, uint16_t port);
const char *net_nsd_environment(void); // the nsd environment that replaces '%' in the game's host names ("lp1")

// The OCW backend's one host (games/smm2/mods/ocw.c, from ocw-config.json), set while the modules are loaded and
// before the game's first instruction. `host` is the name written into the game and the name it looks up; `ip`
// (NULL or empty: ask the host resolver) points that name at an address, which is how a self-hosted or local OCW
// instance is reached and how an IP-only setup works. Nothing else resolves and nothing else is reachable.
void net_ocw_set_target(const char *host, const char *ip);
bool net_ocw_host(char *out, size_t cap);  // the configured host ("" / false: none)

// What the backend wants as the network service account (services/acc.c). True: use *id for the account ID and send
// no ID token at all. The ocw backend does, and matches the console mod exactly there (_vendor/ocw-client-mod: it
// stubs GetNetworkServiceAccountId to one shared id and LoadNetworkServiceAccountIdTokenCache to nothing), because
// OCW identifies the player by the token in the WebSocket handshake, not by this id - the id is the same for every
// OCW player on a console. False: the local account (runtime/net/token.h), a random id and a local JWT.
bool net_account_shared(uint64_t *id);

// TLS
NetTrust net_tls_trust(void);         // nextendo: NET_TRUST_PINNED
// May a connection to ip go ahead with "accept any certificate" (services/ssl.c)? Local addresses always; under the
// ocw backend its own addresses too (the owner's choice: OCW's session is not certificate-checked).
bool net_tls_any_allowed(uint32_t ip);
const char *net_tls_ca_path(void);
bool net_tls_pin(uint8_t out[32]);     // NET_TRUST_PINNED: the server certificate's SPKI SHA-256 to require
// (runtime/services/ssl.c) true when the certificate's public key (SubjectPublicKeyInfo, DER) hashes to pin
struct mbedtls_x509_crt;
bool ssl_pin_check(const struct mbedtls_x509_crt *peer, const uint8_t pin[32]);
const char *net_id_token_file(void);   // NULL: none (the local token, runtime/net/token.c)
const char *net_nex_token_file(void);  // NULL: none (local-nex only)

void ip_to_str(uint32_t ip, char out[16]);
