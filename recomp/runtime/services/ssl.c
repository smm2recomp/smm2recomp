// ssl (the system's TLS service) on Mbed TLS (third_party/mbedtls, Apache-2.0): TLS 1.2 client connections over the
// game's own host sockets (bsd_host.c, backends local-nex / nextendo). Interfaces as switchbrew documents them: ISslService
// (0 CreateContext, 5 SetInterfaceVersion), ISslContext (options, PKI imports, 2 CreateConnection) and
// ISslConnection (0 SetSocketDescriptor, 1 SetHostName, 2 SetVerifyOption, 3 SetIoMode, 8 DoHandshake,
// 9 DoHandshakeGetServerCert, 10 Read, 11 Write, 12 Pending, 13 Peek, 14 Poll, 22 SetOption, 26 SetNextAlpnProto ...).
//
// Trust is the runtime's, not the game's (runtime/net/netcfg.h): the CAs and client certificates the game imports or
// names are not used.
//   tls_trust=local-ca  the server's certificate must chain to the local test CA (net/test-ca.pem, made by
//                       tools/nex_test_server), and match the host name
//   tls_trust=any       any certificate, towards a local address (the test server) or, under the ocw backend, the
//                       one host ocw-config.json names (net_tls_any_allowed; the default there)
//   backend nextendo    pinned: Nextendo's game server presents a self-signed certificate, so the chain is not
//                       checked (verify optional); after the handshake the certificate's public key
//                       (SubjectPublicKeyInfo, DER) must hash (SHA-256) to the pin (net_tls_pin), or the connection is
//                       refused (R_VERIFY_FAILED) before the game sends anything. Never a fallback to "any".
// Host name: when the game names an address (it connects by IP), the name last resolved to that address is used for
// SNI and the check instead ("rebuilt"). ALPN: only http/1.1 is offered (the game's WebSocket upgrade is HTTP/1.1).
//
// Blocking I/O waits in the socket layer with the calling thread's core free; a connection's lock is never held
// while waiting, so one thread can read while another writes.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#include "../hle.h"
#include "../ipc.h"
#include "../net/netcfg.h"
#include "bsd.h"

#define SSL_RESULT(d) MAKE_RESULT(123, d)
#define R_WOULD_BLOCK SSL_RESULT(204)
#define R_TIMEOUT SSL_RESULT(205)
#define R_NO_SOCKET SSL_RESULT(103)
#define R_INVALID_SOCKET SSL_RESULT(106)
#define R_VERIFY_FAILED SSL_RESULT(301)  // [?] the console's number for a certificate it does not trust
#define R_INTERNAL SSL_RESULT(999)

enum { IO_BLOCKING = 1, IO_NONBLOCKING = 2 };
enum { OPT_DO_NOT_CLOSE_SOCKET = 0, OPT_GET_SERVER_CERT_CHAIN = 1, OPT_SKIP_DEFAULT_VERIFY = 2, OPT_ENABLE_ALPN = 3 };

typedef struct { int version; int conns; } SslCtx;

typedef struct {
    pthread_mutex_t mu;
    SslCtx *ctx;
    int fd;
    BsdSock *sock;
    char host[256];        // as the game set it
    char sni[256];         // as used (rebuilt from the address when the game named one)
    uint32_t verify_opt, io_mode;
    bool opt[4];
    bool alpn_asked;
    bool ready, done, closed;
    uint32_t verify_result;  // the last handshake's (0: trusted)
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca;
    uint8_t *peek;          // Peek: read ahead, served first by Read
    size_t npeek;
    uint64_t sent, recvd;   // plaintext bytes (logs)
} SslConn;

// ---- one random generator for every connection (Mbed TLS: not thread-safe on its own)
static pthread_mutex_t rng_lock = PTHREAD_MUTEX_INITIALIZER;
static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context drbg;
static bool rng_ok;

static int rng(void *p, unsigned char *out, size_t n) {
    (void)p;
    pthread_mutex_lock(&rng_lock);
    int r = mbedtls_ctr_drbg_random(&drbg, out, n);
    pthread_mutex_unlock(&rng_lock);
    return r;
}

static void rng_init(void) {
    pthread_mutex_lock(&rng_lock);
    if (!rng_ok) {
        psa_crypto_init();
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
        static const char pers[] = "smm2recomp ssl";
        rng_ok = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)pers, sizeof pers) == 0;
        if (!rng_ok) LOG("[ssl] random generator failed to start\n");
    }
    pthread_mutex_unlock(&rng_lock);
}

static const char *errstr(int e) {
    static __thread char b[128];
    mbedtls_strerror(e, b, sizeof b);
    return b;
}

// A few bytes of plaintext for the log (the WebSocket upgrade, then binary frames). The plaintext has its own
// setting, off even with net_log=1: it is noisy, and the upgrade carries whatever the backend authenticates with.
// net_log_redact takes the player id and any registered secret out of the line first.
// RECOMP_NET_HEX=<file>: every TLS payload in full, as hex, appended to that file (protocol study: NEX's RMC). It holds
// the session's tokens and the player's id: a private file, never shared.
static void hex_dump(const char *dir, const SslConn *c, const uint8_t *p, size_t n) {
    static FILE *f;
    static int init;
    static pthread_mutex_t hl = PTHREAD_MUTEX_INITIALIZER;
    if (!init) { init = 1; const char *e = getenv("RECOMP_NET_HEX"); if (e && *e) f = fopen(e, "a"); }
    if (!f) return;
    pthread_mutex_lock(&hl);
    fprintf(f, "%.3f %s %s %zu ", plat_now_ns() / 1e9, c->sni[0] ? c->sni : "?", dir, n);
    for (size_t i = 0; i < n; i++) fprintf(f, "%02x", p[i]);
    fputc('\n', f);
    fflush(f);
    pthread_mutex_unlock(&hl);
}

static void log_data(const char *dir, const SslConn *c, const uint8_t *p, size_t n) {
    if (n) hex_dump(dir, c, p, n);
    if (!net_log_data() || !n) return;
    char line[200];
    size_t o = 0, show = n < 160 ? n : 160;
    for (size_t i = 0; i < show && o + 4 < sizeof line; i++) {
        uint8_t ch = p[i];
        if (ch == '\r') continue;
        line[o++] = ch == '\n' ? '|' : (ch >= 0x20 && ch < 0x7f) ? (char)ch : '.';
    }
    line[o] = 0;
    char shown[sizeof line + 16];
    net_log_redact(line, shown, sizeof shown);  // (the player id, and any registered token)
    LOG("[ssl] %s %s %zu bytes: %s%s\n", c->sni[0] ? c->sni : "?", dir, n, shown, n > show ? " ..." : "");
}

// ---- the BIO: the game's socket, non-blocking (waits happen in io_wait, outside the connection's lock)
static int bio_send(void *p, const unsigned char *b, size_t n) {
    int r = bsd_sock_send(((SslConn *)p)->sock, b, n);
    return r >= 0 ? r : r == -1 ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static int bio_recv(void *p, unsigned char *b, size_t n) {
    int r = bsd_sock_recv(((SslConn *)p)->sock, b, n);
    return r >= 0 ? r : r == -1 ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static bool is_ip(const char *s) {
    unsigned a, b, c, d;
    char t;
    return sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &t) == 4;
}

bool ssl_pin_check(const mbedtls_x509_crt *peer, const uint8_t pin[32]) {
    if (!peer || !peer->version) return false;
    unsigned char der[2048];  // (mbedtls writes the DER at the end of the buffer)
    int n = mbedtls_pk_write_pubkey_der(&peer->pk, der, sizeof der);
    if (n <= 0) return false;
    uint8_t h[32];
    if (mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), der + sizeof der - n, (size_t)n, h)) return false;
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= h[i] ^ pin[i];
    return diff == 0;
}

static const char *trust_name(NetTrust t) {
    return t == NET_TRUST_ANY ? "any certificate (local test server)" : t == NET_TRUST_PINNED ? "Nextendo's certificate (pinned)" : "local test CA";
}

// Set Mbed TLS up for this connection under the trust policy; false = refused (logged)
static bool conn_setup(SslConn *c) {
    if (c->ready) return true;
    rng_init();
    uint32_t peer = bsd_sock_peer_ip(c->sock);
    char pa[16];
    ip_to_str(peer, pa);
    // the host name: the game's, or (an address, or none) the name last resolved to the peer
    snprintf(c->sni, sizeof c->sni, "%s", c->host);
    if (!c->sni[0] || is_ip(c->sni)) {
        char n[256];
        if (net_name_for_ip(peer, n, sizeof n)) {
            LOG("[ssl] host name %s -> %s (rebuilt from the address)\n", c->sni[0] ? c->sni : "(none)", n);
            snprintf(c->sni, sizeof c->sni, "%s", n);
        }
    }
    NetTrust trust = net_tls_trust();
    if (trust == NET_TRUST_ANY && !net_tls_any_allowed(peer)) {
        LOG("[ssl] %s (%s): \"accept any certificate\" is for local test servers and the ocw host only: refused\n",
            c->sni, pa);
        return false;
    }
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_x509_crt_init(&c->ca);
    if (mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)) return false;
    mbedtls_ssl_conf_rng(&c->conf, rng, NULL);
    mbedtls_ssl_conf_min_tls_version(&c->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&c->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    if (trust == NET_TRUST_ANY) {
        mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_NONE);
    } else if (trust == NET_TRUST_PINNED) {
        uint8_t pin[32];
        if (!net_tls_pin(pin)) { LOG("[ssl] %s: no certificate pin: refused\n", c->sni); return false; }
        mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);  // (self-signed: the pin is the check)
    } else {
        int r = mbedtls_x509_crt_parse_file(&c->ca, net_tls_ca_path());
        if (r) {
            LOG("[ssl] no local test CA at %s (%s): run tools/nex_test_server once to make it\n", net_tls_ca_path(), errstr(r));
            return false;
        }
        mbedtls_ssl_conf_ca_chain(&c->conf, &c->ca, NULL);
        mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    }
    static const char *alpn[] = {"http/1.1", NULL};  // (HTTP/1.1 only: the WebSocket upgrade)
    if (c->alpn_asked || c->opt[OPT_ENABLE_ALPN]) mbedtls_ssl_conf_alpn_protocols(&c->conf, alpn);
    if (mbedtls_ssl_setup(&c->ssl, &c->conf)) return false;
    mbedtls_ssl_set_hostname(&c->ssl, c->sni[0] && !is_ip(c->sni) ? c->sni : NULL);
    mbedtls_ssl_set_bio(&c->ssl, c, bio_send, bio_recv, NULL);
    c->ready = true;
    LOG("[ssl] connection to %s (%s): TLS 1.2, trust: %s, verify option %#x from the game (not used)\n",
        c->sni[0] ? c->sni : "?", pa, trust_name(trust), c->verify_opt);
    return true;
}

// Run op (handshake / read / write) until it is done, waiting on the socket as the I/O mode says.
// Returns the op's value (>= 0) or a result code in *res.
typedef int (*SslOp)(SslConn *c, void *buf, size_t n);
static int op_handshake(SslConn *c, void *b, size_t n) { (void)b, (void)n; return mbedtls_ssl_handshake(&c->ssl); }
static int op_read(SslConn *c, void *b, size_t n) { return mbedtls_ssl_read(&c->ssl, b, n); }
static int op_write(SslConn *c, void *b, size_t n) { return mbedtls_ssl_write(&c->ssl, b, n); }

static int run(SslConn *c, SslOp op, void *buf, size_t n, uint32_t *res) {
    *res = 0;
    for (;;) {
        pthread_mutex_lock(&c->mu);
        int r = c->ready ? op(c, buf, n) : MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
        pthread_mutex_unlock(&c->mu);
        if (r >= 0) return r;
        if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == MBEDTLS_ERR_SSL_CONN_EOF) return 0;
        if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
                char why[256];
                mbedtls_x509_crt_verify_info(why, sizeof why, "", mbedtls_ssl_get_verify_result(&c->ssl));
                for (char *p = why; *p; p++) if (*p == '\n') *p = ' ';
                LOG("[ssl] %s: the server's certificate is not trusted: %s\n", c->sni, why);
                c->verify_result = R_VERIFY_FAILED;
                *res = R_VERIFY_FAILED;
            } else {
                LOG("[ssl] %s: %s\n", c->sni, errstr(r));
                *res = R_INTERNAL;
            }
            return -1;
        }
        if (c->io_mode == IO_NONBLOCKING) { *res = R_WOULD_BLOCK; return -1; }
        int w = bsd_sock_wait(c->sock, r == MBEDTLS_ERR_SSL_WANT_WRITE, -1);
        if (w < 0) { *res = R_INTERNAL; return -1; }
    }
}

static void conn_free(SslConn *c) {
    if (c->ready) {
        pthread_mutex_lock(&c->mu);
        if (c->done) mbedtls_ssl_close_notify(&c->ssl);
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->conf);
        mbedtls_x509_crt_free(&c->ca);
        pthread_mutex_unlock(&c->mu);
    }
    if (c->sock) bsd_sock_unref(c->sock);
    if (c->ctx) c->ctx->conns--;
    free(c->peek);
    pthread_mutex_destroy(&c->mu);
    NLOG("[ssl] connection to %s closed (%llu bytes out, %llu in)\n", c->sni[0] ? c->sni : "?",
         (unsigned long long)c->sent, (unsigned long long)c->recvd);
    free(c);
}

static void conn_on_close(Service *s) { conn_free(s->state); s->state = NULL; }

static void handshake(SslConn *c, IpcReq *req, IpcResp *resp) {
    if (!c->sock) { resp->result = R_NO_SOCKET; return; }
    if (!conn_setup(c)) { resp->result = R_VERIFY_FAILED; return; }
    uint32_t res;
    int r = run(c, op_handshake, NULL, 0, &res);
    if (r < 0) { resp->result = res; return; }
    uint8_t pin[32];
    if (net_tls_trust() == NET_TRUST_PINNED && (!net_tls_pin(pin) || !ssl_pin_check(mbedtls_ssl_get_peer_cert(&c->ssl), pin))) {
        LOG("[ssl] %s: certificate pin mismatch (expected Nextendo's game server); refused\n", c->sni);
        c->verify_result = R_VERIFY_FAILED;  // (not done: no read or write goes through)
        resp->result = R_VERIFY_FAILED;
        return;
    }
    c->done = true;
    const char *alpn = mbedtls_ssl_get_alpn_protocol(&c->ssl);
    LOG("[ssl] %s: handshake done (%s, %s%s%s)\n", c->sni, mbedtls_ssl_get_version(&c->ssl),
        mbedtls_ssl_get_ciphersuite(&c->ssl), alpn ? ", ALPN " : "", alpn ? alpn : "");
    if (req->cmd == 9) {  // DoHandshakeGetServerCert -> buffer, u32 size, u32 count
        const mbedtls_x509_crt *crt = mbedtls_ssl_get_peer_cert(&c->ssl);
        IpcBuf *b = out_buf(req, 0);
        uint32_t size = 0, count = 0;
        if (crt && b && b->size >= crt->raw.len) memcpy((void *)(uintptr_t)b->addr, crt->raw.p, crt->raw.len), size = (uint32_t)crt->raw.len, count = 1;
        OUT(uint32_t, 0) = size, OUT(uint32_t, 4) = count;
        resp_size(resp, 8);
    }
}

static void conn_read(SslConn *c, IpcReq *req, IpcResp *resp, bool peek) {
    IpcBuf *b = out_buf(req, 0);
    uint8_t *dst = b && b->size ? (uint8_t *)(uintptr_t)b->addr : NULL;
    size_t cap = dst ? (size_t)b->size : 0;
    OUT(uint32_t, 0) = 0;
    resp_size(resp, 4);
    if (!c->done) { resp->result = R_INTERNAL; return; }
    pthread_mutex_lock(&c->mu);
    size_t have = c->npeek;
    pthread_mutex_unlock(&c->mu);
    if (!have && cap) {
        uint8_t *tmp = malloc(cap);
        uint32_t res;
        int r = run(c, op_read, tmp, cap, &res);
        if (r < 0) { free(tmp); resp->result = res; return; }
        pthread_mutex_lock(&c->mu);
        c->peek = realloc(c->peek, c->npeek + (size_t)r + 1);
        memcpy(c->peek + c->npeek, tmp, (size_t)r);
        c->npeek += (size_t)r;
        pthread_mutex_unlock(&c->mu);
        free(tmp);
        if (r) log_data("->game", c, c->peek + c->npeek - r, (size_t)r);
    }
    pthread_mutex_lock(&c->mu);
    size_t n = c->npeek < cap ? c->npeek : cap;
    if (n) memcpy(dst, c->peek, n);
    if (!peek) memmove(c->peek, c->peek + n, c->npeek - n), c->npeek -= n, c->recvd += n;
    pthread_mutex_unlock(&c->mu);
    OUT(uint32_t, 0) = (uint32_t)n;
}

static void conn_write(SslConn *c, IpcReq *req, IpcResp *resp) {
    IpcBuf *b = in_buf(req, 0);
    OUT(uint32_t, 0) = 0;
    resp_size(resp, 4);
    if (!c->done) { resp->result = R_INTERNAL; return; }
    if (!b || !b->size) return;
    log_data("game->", c, (const uint8_t *)(uintptr_t)b->addr, (size_t)b->size);
    uint32_t res;
    int r = run(c, op_write, (void *)(uintptr_t)b->addr, (size_t)b->size, &res);
    if (r < 0) { resp->result = res; return; }
    c->sent += (uint64_t)r;
    OUT(uint32_t, 0) = (uint32_t)r;
}

static void connection(Service *s, IpcReq *req, IpcResp *resp) {
    SslConn *c = s->state;
    switch (req->cmd) {
    case 0: {  // SetSocketDescriptor(fd) -> fd (-1: the connection owns it now)
        int32_t fd = IN(int32_t, 0);
        BsdSock *sock = bsd_sock_ref(fd);
        OUT(int32_t, 0) = -1;
        resp_size(resp, 4);
        if (!sock) { LOG("[ssl] SetSocketDescriptor %d: not a host socket\n", fd); resp->result = R_INVALID_SOCKET; return; }
        if (c->sock) bsd_sock_unref(c->sock);
        c->sock = sock, c->fd = fd;
        OUT(int32_t, 0) = c->opt[OPT_DO_NOT_CLOSE_SOCKET] ? fd : -1;
        NLOG("[ssl] socket %d\n", fd);
        return;
    }
    case 1: {  // SetHostName(buffer)
        IpcBuf *b = in_buf(req, 0);
        size_t n = b && b->size ? (size_t)b->size : 0;
        if (n >= sizeof c->host) n = sizeof c->host - 1;
        if (n) memcpy(c->host, (const void *)(uintptr_t)b->addr, n);
        c->host[n] = 0;
        NLOG("[ssl] host name \"%s\"\n", c->host);
        return;
    }
    case 2: c->verify_opt = IN(uint32_t, 0); return;  // SetVerifyOption (the runtime's trust policy decides)
    case 3: c->io_mode = IN(uint32_t, 0); return;     // SetIoMode
    case 4: OUT(int32_t, 0) = c->sock ? c->fd : -1; resp_size(resp, 4); return;  // GetSocketDescriptor
    case 5: {  // GetHostName -> buffer, u32 size
        IpcBuf *b = out_buf(req, 0);
        size_t n = strlen(c->host);
        if (b && b->size) memset((void *)(uintptr_t)b->addr, 0, b->size), memcpy((void *)(uintptr_t)b->addr, c->host, n < b->size ? n : (size_t)b->size - 1);
        OUT(uint32_t, 0) = (uint32_t)n;
        resp_size(resp, 4);
        return;
    }
    case 6: OUT(uint32_t, 0) = c->verify_opt; resp_size(resp, 4); return;
    case 7: OUT(uint32_t, 0) = c->io_mode; resp_size(resp, 4); return;
    case 8: case 9: handshake(c, req, resp); return;
    case 10: conn_read(c, req, resp, false); return;
    case 11: conn_write(c, req, resp); return;
    case 12: {  // Pending -> s32
        pthread_mutex_lock(&c->mu);
        size_t n = c->npeek + (c->done ? mbedtls_ssl_get_bytes_avail(&c->ssl) : 0);
        pthread_mutex_unlock(&c->mu);
        OUT(int32_t, 0) = (int32_t)n;
        resp_size(resp, 4);
        return;
    }
    case 13: conn_read(c, req, resp, true); return;  // Peek
    case 14: {  // Poll(u32 events: 1 read, 2 write, 4 except; u32 timeout ms) -> u32 events
        uint32_t ev = IN(uint32_t, 0), tmo = IN(uint32_t, 4), out = 0;
        pthread_mutex_lock(&c->mu);
        bool buffered = c->npeek || (c->done && mbedtls_ssl_get_bytes_avail(&c->ssl));
        pthread_mutex_unlock(&c->mu);
        if ((ev & 1) && buffered) out |= 1;
        if (ev & 2) out |= 2;
        if (!out && (ev & 1) && c->sock && bsd_sock_wait(c->sock, false, (int)tmo) > 0) out |= 1;
        OUT(uint32_t, 0) = out;
        resp_size(resp, 4);
        if (!out) resp->result = R_TIMEOUT;
        return;
    }
    case 15: resp->result = c->verify_result; return;  // GetVerifyCertError
    case 16: {  // GetNeededServerCertBufferSize -> u32
        pthread_mutex_lock(&c->mu);
        const mbedtls_x509_crt *crt = c->done ? mbedtls_ssl_get_peer_cert(&c->ssl) : NULL;
        OUT(uint32_t, 0) = crt ? (uint32_t)crt->raw.len : 0;
        pthread_mutex_unlock(&c->mu);
        resp_size(resp, 4);
        return;
    }
    case 17: case 19: case 20: return;  // SetSessionCacheMode, FlushSessionCache, SetRenegotiationMode
    case 18: case 21: OUT(uint32_t, 0) = 0; resp_size(resp, 4); return;  // Get{SessionCache,Renegotiation}Mode
    case 22: {  // SetOption(u8 value, u32 option)
        uint32_t o = IN(uint32_t, 4);
        if (o < 4) c->opt[o] = IN(uint8_t, 0) != 0;
        NLOG("[ssl] option %u = %u\n", o, IN(uint8_t, 0));
        return;
    }
    case 23: { uint32_t o = IN(uint32_t, 0); OUT(uint8_t, 0) = o < 4 && c->opt[o]; resp_size(resp, 1); return; }
    case 24: OUT(uint32_t, 0) = 0, OUT(uint32_t, 4) = 0; resp_size(resp, 8); return;  // GetVerifyCertErrors
    case 25: {  // GetCipherInfo(u32 version) -> {char name[64], char version[8]}
        IpcBuf *b = out_buf(req, 0);
        if (b && b->size >= 72) {
            char *o = (char *)(uintptr_t)b->addr;
            memset(o, 0, 72);
            pthread_mutex_lock(&c->mu);
            if (c->done) snprintf(o, 64, "%s", mbedtls_ssl_get_ciphersuite(&c->ssl)), snprintf(o + 64, 8, "TLSv1.2");
            pthread_mutex_unlock(&c->mu);
        }
        return;
    }
    case 26: {  // SetNextAlpnProto(buffer: length-prefixed names): only http/1.1 is ever offered
        IpcBuf *b = in_buf(req, 0);
        char names[128] = "";
        if (b && b->size) {
            const uint8_t *p = (const uint8_t *)(uintptr_t)b->addr;
            for (size_t i = 0; i < b->size && p[i] && i + 1 + p[i] <= b->size;) {
                size_t l = strlen(names);
                snprintf(names + l, sizeof names - l, "%s%.*s", l ? "," : "", p[i], (const char *)p + i + 1);
                i += 1 + p[i];
            }
        }
        c->alpn_asked = true;
        LOG("[ssl] the game asks ALPN {%s}: offering http/1.1 only\n", names);
        return;
    }
    case 27: {  // GetNextAlpnProto -> u32 state, u32 size, buffer
        pthread_mutex_lock(&c->mu);
        const char *p = c->done ? mbedtls_ssl_get_alpn_protocol(&c->ssl) : NULL;
        pthread_mutex_unlock(&c->mu);
        IpcBuf *b = out_buf(req, 0);
        uint32_t n = p ? (uint32_t)strlen(p) : 0;
        if (p && b && b->size >= n) memcpy((void *)(uintptr_t)b->addr, p, n);
        OUT(uint32_t, 0) = p ? 1 : 0, OUT(uint32_t, 4) = n;  // [?] state 1: negotiated
        resp_size(resp, 8);
        return;
    }
    default: LOG("[ssl] ISslConnection cmd %u (ok)\n", req->cmd);
    }
}

static void context(Service *s, IpcReq *req, IpcResp *resp) {
    SslCtx *x = s->state;
    static uint64_t next_id = 1;
    switch (req->cmd) {
    case 0: return;  // SetOption
    case 1: OUT(int32_t, 0) = 0; resp_size(resp, 4); return;  // GetOption
    case 2: {  // CreateConnection
        SslConn *c = calloc(1, sizeof *c);
        pthread_mutex_init(&c->mu, NULL);
        c->ctx = x, c->fd = -1, c->io_mode = IO_BLOCKING;
        x->conns++;
        Service *cs = service_new("ssl:ISslConnection", connection, c);
        cs->on_close = conn_on_close;
        ipc_return_object(resp, cs);
        return;
    }
    case 3: OUT(uint32_t, 0) = (uint32_t)x->conns; resp_size(resp, 4); return;
    case 4: case 5: case 8: case 10: case 12: {  // Import{Server,Client}Pki, RegisterInternalPki, ImportCrl, ...: an id
        NLOG("[ssl] context cmd %u: the game's own certificates are not used (runtime trust policy)\n", req->cmd);
        OUT(uint64_t, 0) = next_id++;
        resp_size(resp, 8);
        return;
    }
    default: return;  // Remove*, AddPolicyOid, ...
    }
}

static void ctx_on_close(Service *s) { free(s->state); s->state = NULL; }

SERVICE("ssl", ssl) {
    (void)s;
    switch (req->cmd) {
    case 0: {  // CreateContext(u32 ssl version, u64 pid)
        SslCtx *x = calloc(1, sizeof *x);
        x->version = IN(int32_t, 0);
        Service *cs = service_new("ssl:ISslContext", context, x);
        cs->on_close = ctx_on_close;
        ipc_return_object(resp, cs);
        NLOG("[ssl] context (version %#x)\n", x->version);
        return;
    }
    case 1: OUT(uint32_t, 0) = 0; resp_size(resp, 4); return;  // GetContextCount
    case 2: case 3: OUT(uint32_t, 0) = 0; resp_size(resp, 4); return;  // GetCertificates / GetCertificateBufSize
    case 5: return;  // SetInterfaceVersion
    default: LOG("[ssl] ISslService cmd %u (ok)\n", req->cmd);
    }
}
