// bsd:u / bsd:s on the host's own network stack: the game's TCP and UDP sockets as real host sockets, for the
// game's own online code against a local NEX test server or Nextendo Network (runtime/net/netcfg.h, backends
// local-nex / nextendo). Only then; otherwise bsd.c keeps the virtual LAN or reports no network.
//
// Deny by default: a socket may connect / send only to the destinations netcfg allows (TCP: net_dest_allowed, the
// redirect table's targets; UDP: net_dest_allowed_udp, the same with local-nex and any address with nextendo, for
// PIA's peers; anything else: ENETUNREACH, logged), binds to INADDR_ANY go to 127.0.0.1 when the local test server is
// on this machine, and datagrams / connections from other addresses are dropped.
//
// Every host socket is non-blocking; the game's blocking calls wait here in short polls with the calling thread's
// core given up (kernel_host_wait_begin), so the rest of the game runs. Errno numbers are FreeBSD's (hostsock.h).
// UDP ports: a port the game binds that is taken on the host (two game instances on one machine) is bound to another
// host port and the game keeps seeing its own; the same pair is used again when the game binds that port again.
// A UDP socket the game bound to a port and closes is parked for PARK_SECONDS rather than closed: PIA probes the NAT
// check, closes and binds the same port again ~100 ms later, and peer-to-peer needs the NAT mapping of that host
// port to survive [inf: research/specs/recomp-nextendo.md §2.1 item 8].
//
// Message layouts as in bsd.c: results {i32 ret, u32 errno} (+ a length); addresses are FreeBSD's sockaddr_in
// {u8 len, u8 family 2, u16 port BE, u32 addr BE, 8 zero}; pollfd {i32 fd, i16 events, i16 revents}.
//
// A plain-HTTP request the game sends may be answered by the game's mods instead (game_http_answer: SMM2 serves a
// course file it fetched ahead, games/smm2/mods/course_preload.h): the answer then stands in for the connection's
// stream ("canned": reads come from it, then end of stream; the request isn't sent; the socket polls ready).
// net_log=1: each socket's bytes and times when the game closes it, and a plain-HTTP socket's request line.
#include "bsd.h"

#include <pthread.h>
#ifndef _WIN32
#include <sys/ioctl.h>
#endif
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../ipc.h"
#include "../net/hostsock.h"
#include "../net/netcfg.h"
#include "../platform.h"

#define MAX_FDS 128
#define WAIT_SLICE_MS 50
#define PARK_SECONDS 10
#define MAX_PARKED 16

struct BsdSock {
    hsock_t h;
    int type;              // 1 stream, 2 datagram
    bool nonblock, closed, listening, bound;
    int refs;              // the fd table + calls in progress + ssl connections (under lock)
    uint32_t bound_ip;     // as the game sees it (host order)
    uint16_t guest_port;   // the port the game sees as its own (host order), 0 none yet
    uint32_t peer_ip;
    uint16_t peer_port;
    int64_t rcvtimeo_ms, sndtimeo_ms;  // SO_RCVTIMEO / SO_SNDTIMEO (0: none)
    uint8_t *canned;                   // the answer that stands in for the stream (NULL: the real one)
    size_t canned_len, canned_off;
    // (net_log: the summary at close)
    uint64_t t_open, t_conn, t_first_in;  // ns
    uint64_t bytes_out, bytes_in;
    uint32_t recvs, polls;
    char first_line[100];  // a plain-HTTP socket's request line (redacted)
};

// The game's mods may answer a plain-HTTP request (runtime/game_hooks.c: none by default); *resp is malloc'd.
bool game_http_answer(uint32_t ip, uint16_t port, const void *req, size_t len, uint8_t **resp, size_t *resp_len);

static BsdSock *table[MAX_FDS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct { uint16_t guest, host; } port_map[64];
static int nport_map;
static struct { uint16_t guest; hsock_t h; uint64_t until; } parked[MAX_PARKED];  // (under lock)
static int nparked;

// under lock: park a closed UDP socket's host socket (false: not parked, close it)
static bool park(BsdSock *s) {
    if (s->type != 2 || !s->bound || !s->guest_port) return false;
    uint64_t now = plat_now_ns();
    for (int i = 0; i < nparked;)  // (expired ones go)
        if (parked[i].until <= now) hsock_close(parked[i].h), parked[i] = parked[--nparked];
        else i++;
    if (nparked == MAX_PARKED) hsock_close(parked[0].h), parked[0] = parked[--nparked];
    parked[nparked].guest = s->guest_port, parked[nparked].h = s->h;
    parked[nparked++].until = now + PARK_SECONDS * 1000000000ull;
    return true;
}

// under lock: the parked host socket of a guest UDP port (taken out), HSOCK_BAD none
static hsock_t unpark(uint16_t guest) {
    uint64_t now = plat_now_ns();
    for (int i = 0; i < nparked; i++)
        if (parked[i].guest == guest && parked[i].until > now) {
            hsock_t h = parked[i].h;
            parked[i] = parked[--nparked];
            return h;
        }
    return HSOCK_BAD;
}

static void result(IpcResp *resp, int32_t ret, uint32_t err) {
    OUT(int32_t, 0) = ret, OUT(uint32_t, 4) = ret < 0 ? err : 0;
    resp_size(resp, 8);
}

// RECOMP_BSD_TRACE=1: every socket call with its result (protocol study)
static bool bsd_trace(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("RECOMP_BSD_TRACE"); on = e && *e && *e != '0'; }
    return on;
}

static void result_len(IpcResp *resp, int32_t ret, uint32_t err, uint32_t len) {
    result(resp, ret, err);
    OUT(uint32_t, 8) = len;
    resp_size(resp, 12);
}

BsdSock *bsd_sock_ref(int fd) {
    pthread_mutex_lock(&lock);
    BsdSock *s = fd >= 0 && fd < MAX_FDS ? table[fd] : NULL;
    if (s) s->refs++;
    pthread_mutex_unlock(&lock);
    return s;
}

void bsd_sock_unref(BsdSock *s) {
    if (!s) return;
    pthread_mutex_lock(&lock);
    bool last = --s->refs == 0, kept = last && park(s);
    pthread_mutex_unlock(&lock);
    if (last) {
        if (kept) NLOG("[bsd] UDP port %u parked (%d s)\n", s->guest_port, PARK_SECONDS);
        else hsock_close(s->h);
        free(s->canned);
        free(s);
    }
}

uint32_t bsd_sock_peer_ip(BsdSock *s) { return s ? s->peer_ip : 0; }

static int fd_new(BsdSock *s) {
    pthread_mutex_lock(&lock);
    int fd = 0;
    while (fd < MAX_FDS && table[fd]) fd++;
    if (fd < MAX_FDS) table[fd] = s, s->refs = 1;
    pthread_mutex_unlock(&lock);
    return fd < MAX_FDS ? fd : -1;
}

static void host_addr(struct sockaddr_in *a, uint32_t ip, uint16_t port) {
    memset(a, 0, sizeof *a);
    a->sin_family = AF_INET;
    a->sin_port = htons(port);
    a->sin_addr.s_addr = htonl(ip);
}

static bool get_addr(IpcBuf *b, uint32_t *ip, uint16_t *port) {
    if (!b || b->size < 8) return false;
    const uint8_t *a = (const uint8_t *)(uintptr_t)b->addr;
    *port = (uint16_t)(a[2] << 8 | a[3]);
    *ip = (uint32_t)a[4] << 24 | (uint32_t)a[5] << 16 | (uint32_t)a[6] << 8 | a[7];
    return true;
}

static uint32_t put_addr(IpcBuf *b, uint32_t ip, uint16_t port) {
    uint8_t a[16] = {16, 2, (uint8_t)(port >> 8), (uint8_t)port, (uint8_t)(ip >> 24), (uint8_t)(ip >> 16),
                     (uint8_t)(ip >> 8), (uint8_t)ip};
    if (b && b->size) memcpy((void *)(uintptr_t)b->addr, a, b->size < 16 ? b->size : 16);
    return 16;
}

static uint16_t host_port_of(hsock_t h) {
    struct sockaddr_in a;
    hsocklen_t n = sizeof a;
    return getsockname(h, (struct sockaddr *)&a, &n) == 0 ? ntohs(a.sin_port) : 0;
}

// Wait for events (POLLIN / POLLOUT) on s, at most ms (< 0: no limit), in slices with the core free. 1 ready,
// 0 timed out, -1 closed meanwhile / failed.
static int wait_events(BsdSock *s, short events, int64_t ms, bool guest) {
    uint64_t end = ms < 0 ? UINT64_MAX : plat_now_ns() + (uint64_t)ms * 1000000;
    kernel_host_wait_begin();
    int r = 0;
    for (;;) {
        if ((guest && s->closed) || g_rt_unwind) { r = -1; break; }  // (the game closed it: its own waits end)
        uint64_t now = plat_now_ns();
        if (now >= end) break;
        int slice = WAIT_SLICE_MS;
        if (end != UINT64_MAX && (end - now) / 1000000 < (uint64_t)slice) slice = (int)((end - now) / 1000000) + 1;
        hpollfd p = {.fd = s->h, .events = events};
        int n = hsock_poll(&p, 1, slice);
        if (n > 0) { r = 1; break; }
        if (n < 0 && hsock_errno() != EINTR) { r = -1; break; }
    }
    kernel_host_wait_end();
    return r;
}

int bsd_sock_wait(BsdSock *s, bool write, int timeout_ms) { return wait_events(s, write ? POLLOUT : POLLIN, timeout_ms, false); }

int bsd_sock_send(BsdSock *s, const void *data, size_t len) {
    long n = send(s->h, (const char *)data, HSOCK_IOLEN(len), MSG_NOSIGNAL);
    if (n >= 0) s->bytes_out += (uint64_t)n;
    if (n >= 0 && !s->t_conn) s->t_conn = plat_now_ns();
    if (n >= 0) return (int)n;
    return hsock_would_block(hsock_errno()) ? -1 : -2;
}

int bsd_sock_recv(BsdSock *s, void *data, size_t len) {
    long n = recv(s->h, (char *)data, HSOCK_IOLEN(len), 0);
    s->recvs++;
    if (n > 0 && !s->t_first_in) s->t_first_in = plat_now_ns();
    if (n >= 0) s->bytes_in += (uint64_t)n;
    if (n >= 0) return (int)n;
    return hsock_would_block(hsock_errno()) ? -1 : -2;
}

static void log_refused(const char *what, uint32_t ip, uint16_t port) {
    char a[16], host[256];
    ip_to_str(ip, a);
    // (name the backend's own rule: "not a local test address" is only true of local-nex, and sent anyone reading
    // an ocw log looking in the wrong place)
    if (net_backend() == NET_BACKEND_OCW && net_ocw_host(host, sizeof host))
        LOG("[bsd] %s %s:%u refused: not an address %s resolves to (runtime/net/netcfg.h). Looking the name up "
            "again\n", what, a, port, host);
    else
        LOG("[bsd] %s %s:%u refused: not a local test address (runtime/net/netcfg.h)\n", what, a, port);
}

// ---- socket options: the game's (FreeBSD) level / name -> the host's
enum { OPT_INT, OPT_TIMEO, OPT_LINGER, OPT_IGNORE };
static bool map_opt(int32_t level, int32_t name, int *hl, int *hn, int *kind) {
    *kind = OPT_INT;
    if (level == 0xffff) {  // SOL_SOCKET
        *hl = SOL_SOCKET;
        switch (name) {
        case 0x4: *hn = SO_REUSEADDR; return true;
        case 0x8: *hn = SO_KEEPALIVE; return true;
        case 0x20: *hn = SO_BROADCAST; return true;
        case 0x80: *hn = SO_LINGER; *kind = OPT_LINGER; return true;
        case 0x100: *hn = SO_OOBINLINE; return true;
#ifdef SO_REUSEPORT
        case 0x200: *hn = SO_REUSEPORT; return true;
#else
        case 0x200: *hn = SO_REUSEADDR; return true;
#endif
        case 0x1001: *hn = SO_SNDBUF; return true;
        case 0x1002: *hn = SO_RCVBUF; return true;
        case 0x1005: *hn = SO_SNDTIMEO; *kind = OPT_TIMEO; return true;
        case 0x1006: *hn = SO_RCVTIMEO; *kind = OPT_TIMEO; return true;
        case 0x1007: *hn = SO_ERROR; return true;
        case 0x1008: *hn = SO_TYPE; return true;
        default: *kind = OPT_IGNORE; return true;  // (Horizon's own: shutdown-exempt, linger ...: accepted)
        }
    }
    if (level == 6) {  // IPPROTO_TCP
        *hl = IPPROTO_TCP;
        switch (name) {
        case 1: *hn = TCP_NODELAY; return true;
        default: *kind = OPT_IGNORE; return true;
        }
    }
    if (level == 0) {  // IPPROTO_IP
        *hl = IPPROTO_IP;
        switch (name) {
        case 3: *hn = IP_TOS; return true;
        case 4: *hn = IP_TTL; return true;
        default: *kind = OPT_IGNORE; return true;  // (multicast: not on the test network)
        }
    }
    return false;
}

static void do_setsockopt(BsdSock *s, IpcReq *req, IpcResp *resp) {
    int32_t level = IN(int32_t, 4), name = IN(int32_t, 8);
    IpcBuf *b = in_buf(req, 0);
    const uint8_t *v = b && b->size ? (const uint8_t *)(uintptr_t)b->addr : NULL;
    int hl, hn, kind;
    if (!map_opt(level, name, &hl, &hn, &kind)) { result(resp, -1, BSD_ENOPROTOOPT); return; }
    NLOG("[bsd] setsockopt level %#x opt %#x (%llu bytes)\n", level, name, (unsigned long long)(b ? b->size : 0));
    if (kind == OPT_IGNORE || !v) { result(resp, 0, 0); return; }
    if (kind == OPT_TIMEO) {  // {i64 sec, i64 usec}: kept here (the host socket is non-blocking anyway)
        int64_t ms = b->size >= 16 ? *(const int64_t *)v * 1000 + *(const int64_t *)(v + 8) / 1000 : 0;
        if (hn == SO_RCVTIMEO) s->rcvtimeo_ms = ms;
        else s->sndtimeo_ms = ms;
        result(resp, 0, 0);
        return;
    }
    int r;
    if (kind == OPT_LINGER) {
        struct linger l = {(int)(b->size >= 4 ? *(const int32_t *)v : 0), (int)(b->size >= 8 ? *(const int32_t *)(v + 4) : 0)};
        r = setsockopt(s->h, hl, hn, (const char *)&l, sizeof l);
    } else {
        int x = b->size >= 4 ? *(const int32_t *)v : v[0];
        r = setsockopt(s->h, hl, hn, (const char *)&x, sizeof x);
    }
    result(resp, r == 0 ? 0 : -1, r == 0 ? 0 : hsock_bsd_errno(hsock_errno()));
}

static void do_getsockopt(BsdSock *s, IpcReq *req, IpcResp *resp) {
    int32_t level = IN(int32_t, 4), name = IN(int32_t, 8);
    IpcBuf *b = out_buf(req, 0);
    uint8_t *o = b && b->size ? (uint8_t *)(uintptr_t)b->addr : NULL;
    int hl, hn, kind;
    if (!map_opt(level, name, &hl, &hn, &kind)) { result_len(resp, -1, BSD_ENOPROTOOPT, 0); return; }
    if (o) memset(o, 0, b->size);
    if (kind == OPT_IGNORE || !o) { result_len(resp, 0, 0, b ? (uint32_t)b->size : 0); return; }
    if (kind == OPT_TIMEO) {
        int64_t ms = hn == SO_RCVTIMEO ? s->rcvtimeo_ms : s->sndtimeo_ms;
        if (b->size >= 16) *(int64_t *)o = ms / 1000, *(int64_t *)(o + 8) = ms % 1000 * 1000;
        result_len(resp, 0, 0, 16);
        return;
    }
    int x = 0;
    hsocklen_t n = sizeof x;
    int r = getsockopt(s->h, hl, hn, (char *)&x, &n);
    if (r == 0 && hn == SO_ERROR && x) x = (int)hsock_bsd_errno(x);
    if (r == 0 && hn == SO_TYPE) x = s->type;
    if (b->size >= 4) *(int32_t *)o = x;
    result_len(resp, r == 0 ? 0 : -1, r == 0 ? 0 : hsock_bsd_errno(hsock_errno()), 4);
}

// ---- poll / select
static short ev_to_host(int16_t ev) {
    short h = 0;
    if (ev & (0x1 | 0x40 | 0x80)) h |= POLLIN;  // POLLIN, POLLRDNORM, POLLRDBAND
#ifndef _WIN32
    if (ev & 0x2) h |= POLLPRI;
#endif  // (WSAPoll fails with WSAEINVAL when POLLPRI is asked for, and poll_fds then reported "nothing ready" forever)
    if (ev & (0x4 | 0x100)) h |= POLLOUT;       // POLLOUT (= POLLWRNORM), POLLWRBAND
    return h;
}

static int16_t ev_from_host(short rev, int16_t ev) {
    int16_t g = 0;
    if (rev & POLLIN) g |= ev & (0x1 | 0x40 | 0x80) ? ev & (0x1 | 0x40 | 0x80) : 0x1;
    if (rev & POLLPRI) g |= 0x2;
    if (rev & POLLOUT) g |= ev & (0x4 | 0x100) ? ev & (0x4 | 0x100) : 0x4;
    if (rev & POLLERR) g |= 0x8;
    if (rev & POLLHUP) g |= 0x10;
    if (rev & POLLNVAL) g |= 0x20;
    return g;
}

// poll over guest fds; fills revents; returns the ready count (waits in slices with the core free)
static int poll_fds(int32_t *fds, int16_t *events, int16_t *revents, uint32_t n, int64_t timeout_ms) {
    hpollfd *p = calloc(n ? n : 1, sizeof *p);
    BsdSock **ss = calloc(n ? n : 1, sizeof *ss);
    int ready = 0;
    for (uint32_t i = 0; i < n; i++) {
        revents[i] = 0;
        ss[i] = fds[i] >= 0 ? bsd_sock_ref(fds[i]) : NULL;
        p[i].fd = ss[i] ? ss[i]->h : HSOCK_BAD;
        p[i].events = ss[i] ? ev_to_host(events[i]) : 0;
        if (fds[i] >= 0 && !ss[i]) revents[i] = 0x20, ready++;  // POLLNVAL
        if (ss[i]) ss[i]->polls++;
        if (ss[i] && ss[i]->canned) {  // (an answer standing in for the stream: readable and writable now)
            revents[i] = ev_from_host(p[i].events & (POLLIN | POLLOUT), events[i]);
            ready += revents[i] != 0;
            p[i].fd = HSOCK_BAD, p[i].events = 0;
        }
    }
    uint64_t end = timeout_ms < 0 ? UINT64_MAX : plat_now_ns() + (uint64_t)timeout_ms * 1000000;
    bool released = false;
    while (!ready) {
        uint64_t now = plat_now_ns();
        int slice = timeout_ms == 0 ? 0 : WAIT_SLICE_MS;
        if (end != UINT64_MAX && now < end && (end - now) / 1000000 < (uint64_t)slice) slice = (int)((end - now) / 1000000) + 1;
        if (slice && !released) kernel_host_wait_begin(), released = true;
#ifdef _WIN32
        // (WSAPoll wants at least one valid socket)
        bool any = false;
        for (uint32_t i = 0; i < n; i++) any |= p[i].fd != HSOCK_BAD;
        int r = any ? hsock_poll(p, n, slice) : (Sleep(slice), 0);
#else
        int r = hsock_poll(p, n, slice);
#endif
        if (r > 0) {
            for (uint32_t i = 0; i < n; i++)
                if (ss[i] && p[i].revents) revents[i] = ev_from_host(p[i].revents, events[i]), ready += revents[i] != 0;
            break;
        }
        if (r < 0 && hsock_errno() != EINTR) break;
        if (g_rt_unwind || timeout_ms == 0 || plat_now_ns() >= end) break;
    }
    if (released) kernel_host_wait_end();
    for (uint32_t i = 0; i < n; i++) bsd_sock_unref(ss[i]);
    free(p), free(ss);
    return ready;
}

static void do_poll(IpcReq *req, IpcResp *resp) {
    uint32_t nfds = IN(uint32_t, 0);
    int32_t timeout = IN(int32_t, 4);
    IpcBuf *in = in_buf(req, 0), *out = out_buf(req, 0);
    if (nfds > 4096 || !in || in->size < nfds * 8ull) { result(resp, -1, BSD_EINVAL); return; }
    uint8_t *f = malloc(nfds * 8u + 8);
    memcpy(f, (void *)(uintptr_t)in->addr, nfds * 8u);
    int32_t *fds = malloc((nfds + 1) * 4);
    int16_t *ev = malloc((nfds + 1) * 2), *rev = malloc((nfds + 1) * 2);
    for (uint32_t i = 0; i < nfds; i++) fds[i] = *(int32_t *)(f + 8 * i), ev[i] = *(int16_t *)(f + 8 * i + 4);
    int ready = poll_fds(fds, ev, rev, nfds, timeout);
    if (bsd_trace()) {
        char line[200];
        int o = snprintf(line, sizeof line, "[bsd] poll %u fds, timeout %d ms -> %d ready:", nfds, timeout, ready);
        for (uint32_t i = 0; i < nfds && o < (int)sizeof line - 24; i++) o += snprintf(line + o, sizeof line - o, " fd %d %#x->%#x", fds[i], ev[i], rev[i]);
        LOG("%s\n", line);
    }
    for (uint32_t i = 0; i < nfds; i++) *(int16_t *)(f + 8 * i + 6) = rev[i];
    if (out && out->size >= nfds * 8ull) memcpy((void *)(uintptr_t)out->addr, f, nfds * 8u);
    free(f), free(fds), free(ev), free(rev);
    result(resp, ready, 0);
}

static void do_select(IpcReq *req, IpcResp *resp) {
    int32_t nfds = IN(int32_t, 0);
    uint64_t sec = IN(uint64_t, 8), usec = IN(uint64_t, 16);
    bool forever = req->data_size >= 25 && IN(uint8_t, 24);
    int64_t ms = forever ? -1 : (int64_t)(sec * 1000 + usec / 1000);
    if (nfds > MAX_FDS) nfds = MAX_FDS;
    uint8_t set[3][128] = {{0}};
    for (int k = 0; k < 3; k++) {
        IpcBuf *b = in_buf(req, k);
        if (b && b->size) memcpy(set[k], (void *)(uintptr_t)b->addr, b->size < 128 ? b->size : 128);
    }
    int32_t fds[MAX_FDS];
    int16_t ev[MAX_FDS], rev[MAX_FDS];
    uint32_t n = 0;
    for (int f = 0; f < nfds; f++) {
        int16_t e = (set[0][f >> 3] >> (f & 7) & 1 ? 0x1 : 0) | (set[1][f >> 3] >> (f & 7) & 1 ? 0x4 : 0) |
                    (set[2][f >> 3] >> (f & 7) & 1 ? 0x2 : 0);
        if (e) fds[n] = f, ev[n++] = e;
    }
    poll_fds(fds, ev, rev, n, ms);
    if (bsd_trace()) {
        char line[200];
        int k = snprintf(line, sizeof line, "[bsd] select %d fds, %lld ms:", nfds, (long long)ms);
        for (uint32_t i = 0; i < n && k < (int)sizeof line - 24; i++) k += snprintf(line + k, sizeof line - k, " fd %d %#x->%#x", fds[i], ev[i], rev[i]);
        LOG("%s\n", line);
    }
    uint8_t o[3][128] = {{0}};
    int ready = 0;
    for (uint32_t i = 0; i < n; i++) {
        int f = fds[i];
        if (rev[i] & (0x1 | 0x10 | 0x8)) o[0][f >> 3] |= (uint8_t)(1 << (f & 7)), ready += (ev[i] & 0x1) != 0;
        if (rev[i] & 0x4) o[1][f >> 3] |= (uint8_t)(1 << (f & 7)), ready++;
        if (rev[i] & 0x2) o[2][f >> 3] |= (uint8_t)(1 << (f & 7)), ready++;
    }
    for (int k = 0; k < 3; k++) {
        for (int f = 0; f < nfds; f++)  // (only the asked-for bits)
            if (!(set[k][f >> 3] >> (f & 7) & 1)) o[k][f >> 3] &= (uint8_t)~(1 << (f & 7));
        IpcBuf *b = out_buf(req, k);
        if (b && b->size) memcpy((void *)(uintptr_t)b->addr, o[k], b->size < 128 ? b->size : 128);
    }
    result(resp, ready, 0);
}

// ---- data
static bool dest_ok(const BsdSock *s, uint32_t ip) { return s->type == 2 ? net_dest_allowed_udp(ip) : net_dest_allowed(ip); }

static void do_send(BsdSock *s, int fd, IpcReq *req, IpcResp *resp) {
    uint32_t flags = req->cmd == 24 ? 0 : IN(uint32_t, 4);
    IpcBuf *d = in_buf(req, 0);
    const char *data = d && d->size ? (const char *)(uintptr_t)d->addr : "";
    size_t len = d ? (size_t)d->size : 0;
    uint32_t ip = s->peer_ip;
    uint16_t port = s->peer_port;
    struct sockaddr_in to;
    bool has_to = false;
    if (req->cmd == 11 && get_addr(in_buf(req, 1), &ip, &port)) has_to = true;
    if ((has_to || s->peer_ip) && !dest_ok(s, ip)) { log_refused("send to", ip, port); result(resp, -1, BSD_ENETUNREACH); return; }
    if (has_to) host_addr(&to, ip, net_dest_port(ip, port));
    if (s->type == 1 && !s->first_line[0] && len >= 4 && port == 80) {  // (net_log: what a plain-HTTP socket asks)
        char line[sizeof s->first_line];
        size_t k = 0;
        while (k < len && k < sizeof line - 1 && data[k] != '\r' && data[k] != '\n') line[k] = data[k], k++;
        line[k] = 0;
        net_log_redact(line, s->first_line, sizeof s->first_line);
    }
    if (s->type == 1 && !s->canned && !has_to && len) {
        uint8_t *ans = NULL;
        size_t alen = 0;
        if (game_http_answer(ip, port, data, len, &ans, &alen) && ans) {
            s->canned = ans, s->canned_len = alen, s->canned_off = 0;
            NLOG("[bsd] send %d: %zu bytes answered by the game's mods (%zu bytes back)\n", fd, len, alen);
        }
    }
    if (s->canned) {  // (the request goes nowhere: the answer is already there)
        s->bytes_out += len;
        result(resp, (int32_t)len, 0);
        return;
    }
    int hflags = MSG_NOSIGNAL | (flags & 1 ? MSG_OOB : 0);
    bool nb = s->nonblock || (flags & 0x80);
    for (;;) {
        long n = has_to ? sendto(s->h, data, HSOCK_IOLEN(len), hflags, (struct sockaddr *)&to, sizeof to)
                        : send(s->h, data, HSOCK_IOLEN(len), hflags);
        if (n >= 0) {
            NLOG("[bsd] send %d: %ld bytes\n", fd, n);
            s->bytes_out += (uint64_t)n;
            if (!s->t_conn) s->t_conn = plat_now_ns();
            result(resp, (int32_t)n, 0);
            return;
        }
        int e = hsock_errno();
        if (!hsock_would_block(e) || nb) { result(resp, -1, hsock_bsd_errno(e)); return; }
        int w = wait_events(s, POLLOUT, s->sndtimeo_ms ? s->sndtimeo_ms : -1, true);
        if (w <= 0) { result(resp, -1, w < 0 ? BSD_EBADF : BSD_EAGAIN); return; }
    }
}

static void do_recv(BsdSock *s, int fd, IpcReq *req, IpcResp *resp) {
    uint32_t flags = req->cmd == 25 ? 0 : IN(uint32_t, 4);
    IpcBuf *d = out_buf(req, 0);
    char *data = d && d->size ? (char *)(uintptr_t)d->addr : NULL;
    size_t cap = d ? (size_t)d->size : 0;
    int hflags = (flags & 1 ? MSG_OOB : 0) | (flags & 2 ? MSG_PEEK : 0);
    bool nb = s->nonblock || (flags & 0x80), all = (flags & 0x40) && s->type == 1;
    size_t got = 0;
    s->recvs++;
    if (s->canned) {  // (the answer, then end of stream)
        size_t n = s->canned_len - s->canned_off;
        if (n > cap) n = cap;
        if (data && n) memcpy(data, s->canned + s->canned_off, n);
        if (!(flags & 2)) s->canned_off += n;  // (MSG_PEEK leaves it)
        if (n && !s->t_first_in) s->t_first_in = plat_now_ns();
        s->bytes_in += n;
        result_len(resp, (int32_t)n, 0, req->cmd == 9 ? put_addr(out_buf(req, 1), s->peer_ip, s->peer_port) : 0);
        return;
    }
    for (;;) {
        struct sockaddr_in from;
        hsocklen_t fl = sizeof from;
        memset(&from, 0, sizeof from);
        long n = recvfrom(s->h, data ? data + got : (char *)&from, HSOCK_IOLEN(data ? cap - got : 0), hflags,
                          (struct sockaddr *)&from, &fl);
        if (n >= 0) {
            uint32_t sip = ntohl(from.sin_addr.s_addr);
            if (s->type == 2 && !net_dest_allowed_udp(sip)) continue;  // (a datagram from outside: dropped)
            got += (size_t)n;
            s->bytes_in += (uint64_t)n;
            if (n > 0 && !s->t_first_in) s->t_first_in = plat_now_ns();
            if (all && n > 0 && got < cap) continue;
            uint32_t alen = 0;
            if (req->cmd == 9) alen = put_addr(out_buf(req, 1), s->type == 2 ? sip : s->peer_ip, s->type == 2 ? ntohs(from.sin_port) : s->peer_port);
            NLOG("[bsd] recv %d: %zu bytes\n", fd, got);
            result_len(resp, (int32_t)got, 0, alen);
            return;
        }
        int e = hsock_errno();
        if (!hsock_would_block(e) || nb) {
            if (got) { result_len(resp, (int32_t)got, 0, 0); return; }
            result_len(resp, -1, hsock_bsd_errno(e), 0);
            return;
        }
        int w = wait_events(s, POLLIN, s->rcvtimeo_ms ? s->rcvtimeo_ms : -1, true);
        if (w <= 0) { result_len(resp, got ? (int32_t)got : -1, w < 0 ? BSD_EBADF : BSD_EAGAIN, 0); return; }
    }
}

// ---- the rest
static void do_socket(IpcReq *req, IpcResp *resp) {
    int32_t domain = IN(int32_t, 0), type = IN(int32_t, 4), proto = IN(int32_t, 8);
    bool nonblock = (type & 0x20000000) != 0;
    type &= 0x0fffffff;
    if (domain != 2) { result(resp, -1, BSD_EAFNOSUPPORT); return; }
    if (type != 1 && type != 2) { LOG("[bsd] socket type %d refused\n", type); result(resp, -1, BSD_EPROTONOSUPPORT); return; }
    hsock_startup();
    hsock_t h = socket(AF_INET, type == 1 ? SOCK_STREAM : SOCK_DGRAM, proto == 6 ? IPPROTO_TCP : proto == 17 ? IPPROTO_UDP : 0);
    if (h == HSOCK_BAD) { result(resp, -1, hsock_bsd_errno(hsock_errno())); return; }
    hsock_nonblock(h);
    BsdSock *s = calloc(1, sizeof *s);
    s->h = h, s->type = type, s->nonblock = nonblock, s->t_open = plat_now_ns();
    int fd = fd_new(s);
    if (fd < 0) { hsock_close(h); free(s); result(resp, -1, BSD_EMFILE); return; }
    NLOG("[bsd] socket %d: %s%s\n", fd, type == 1 ? "TCP" : "UDP", nonblock ? " (non-blocking)" : "");
    result(resp, fd, 0);
}

static uint32_t bind_ip_for(uint32_t ip) {
    if (ip || net_backend() != NET_BACKEND_LOCAL_NEX) return ip;  // (nextendo: INADDR_ANY, the internet is the peer)
    return (net_test_server_ip() >> 24) == 127 ? 0x7f000001 : 0;  // (INADDR_ANY: loopback when the test server is here)
}

static void do_bind(BsdSock *s, int fd, IpcReq *req, IpcResp *resp) {
    uint32_t ip;
    uint16_t port;
    if (!get_addr(in_buf(req, 0), &ip, &port)) { result(resp, -1, BSD_EINVAL); return; }
    if (ip && !net_ip_is_local(ip)) { result(resp, -1, BSD_EADDRNOTAVAIL); return; }
    if (s->type == 2 && port) {  // the same port again soon after a close: its parked host socket (NAT mapping kept)
        pthread_mutex_lock(&lock);
        hsock_t h = unpark(port);
        pthread_mutex_unlock(&lock);
        if (h != HSOCK_BAD) {
            hsock_close(s->h);
            s->h = h, s->bound = true, s->bound_ip = ip, s->guest_port = port;
            NLOG("[bsd] bind %d: UDP port %u again: the parked host socket (port %u) is reused\n", fd, port, host_port_of(h));
            result(resp, 0, 0);
            return;
        }
    }
    uint16_t hport = port;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < nport_map; i++)
        if (port && port_map[i].guest == port) hport = port_map[i].host;
    pthread_mutex_unlock(&lock);
    struct sockaddr_in a;
    host_addr(&a, bind_ip_for(ip), hport);
    int r = bind(s->h, (struct sockaddr *)&a, sizeof a);
    if (r != 0 && s->type == 2 && port && hsock_bsd_errno(hsock_errno()) == BSD_EADDRINUSE) {
        host_addr(&a, bind_ip_for(ip), 0);  // (taken on this machine: another host port, the game keeps its own)
        r = bind(s->h, (struct sockaddr *)&a, sizeof a);
        if (r == 0) {
            pthread_mutex_lock(&lock);
            int i = 0;
            while (i < nport_map && port_map[i].guest != port) i++;
            if (i < 64) {
                port_map[i].guest = port, port_map[i].host = host_port_of(s->h);
                if (i == nport_map) nport_map++;
            }
            pthread_mutex_unlock(&lock);
            LOG("[bsd] UDP port %u is taken here: host port %u stands in for it\n", port, host_port_of(s->h));
        }
    }
    if (r != 0) { result(resp, -1, hsock_bsd_errno(hsock_errno())); return; }
    s->bound_ip = ip, s->bound = true;
    s->guest_port = port ? port : host_port_of(s->h);
    char t[16];
    ip_to_str(ip, t);
    NLOG("[bsd] bind %d: %s:%u (host port %u)\n", fd, t, s->guest_port, host_port_of(s->h));
    result(resp, 0, 0);
}

static void do_connect(BsdSock *s, int fd, IpcReq *req, IpcResp *resp) {
    uint32_t ip;
    uint16_t port;
    if (!get_addr(in_buf(req, 0), &ip, &port)) { result(resp, -1, BSD_EINVAL); return; }
    char t[16];
    ip_to_str(ip, t);
    if (!dest_ok(s, ip)) { log_refused("connect to", ip, port); result(resp, -1, BSD_ENETUNREACH); return; }
    struct sockaddr_in a;
    uint16_t hport = net_dest_port(ip, port);  // (the test server's own port for 443)
    host_addr(&a, ip, hport);
    s->peer_ip = ip, s->peer_port = port;
    int r = connect(s->h, (struct sockaddr *)&a, sizeof a);
    uint32_t err = r == 0 ? 0 : hsock_bsd_errno(hsock_errno());
    if (r != 0 && (err == BSD_EINPROGRESS || err == BSD_EAGAIN) && !s->nonblock) {
        int w = wait_events(s, POLLOUT, s->sndtimeo_ms ? s->sndtimeo_ms : 30000, true);
        int e = 0;
        hsocklen_t n = sizeof e;
        if (w > 0 && getsockopt(s->h, SOL_SOCKET, SO_ERROR, (char *)&e, &n) == 0) err = e ? hsock_bsd_errno(e) : 0;
        else err = w == 0 ? BSD_ETIMEDOUT : BSD_EBADF;
        r = err ? -1 : 0;
    } else if (r != 0 && err == BSD_EAGAIN) {
        err = BSD_EINPROGRESS;
    }
    if (!s->guest_port) s->guest_port = host_port_of(s->h);
    if (r == 0 && !s->t_conn) s->t_conn = plat_now_ns();
    LOG("[bsd] connect %d (%s) to %s:%u%s: %s%s\n", fd, s->type == 1 ? "TCP" : "UDP", t, port, hport != port ? " (test server port)" : "",
        r == 0 ? "connected" : err == BSD_EINPROGRESS ? "in progress" : err == BSD_EISCONN ? "already connected" : "failed",
        s->nonblock ? " (non-blocking)" : "");
    if (r != 0 && bsd_trace()) LOG("[bsd]   connect error %u\n", err);
    result(resp, r == 0 ? 0 : -1, err);
}

static void do_accept(BsdSock *s, IpcReq *req, IpcResp *resp) {
    for (;;) {
        struct sockaddr_in a;
        hsocklen_t n = sizeof a;
        hsock_t h = accept(s->h, (struct sockaddr *)&a, &n);
        if (h != HSOCK_BAD) {
            uint32_t ip = ntohl(a.sin_addr.s_addr);
            if (!net_dest_allowed(ip)) { log_refused("connection from", ip, ntohs(a.sin_port)); hsock_close(h); continue; }
            hsock_nonblock(h);
            BsdSock *c = calloc(1, sizeof *c);
            c->h = h, c->type = 1, c->peer_ip = ip, c->peer_port = ntohs(a.sin_port), c->guest_port = s->guest_port;
            int fd = fd_new(c);
            if (fd < 0) { hsock_close(h); free(c); result_len(resp, -1, BSD_EMFILE, 0); return; }
            result_len(resp, fd, 0, put_addr(out_buf(req, 0), ip, c->peer_port));
            return;
        }
        int e = hsock_errno();
        if (!hsock_would_block(e) || s->nonblock) { result_len(resp, -1, hsock_bsd_errno(e), 0); return; }
        int w = wait_events(s, POLLIN, -1, true);
        if (w <= 0) { result_len(resp, -1, BSD_EBADF, 0); return; }
    }
}

static void bsd_host_client_1(IpcReq *req, IpcResp *resp);

void bsd_host_client(IpcReq *req, IpcResp *resp) {
    bsd_host_client_1(req, resp);
    if (bsd_trace() && req->cmd != 5 && req->cmd != 6 && req->cmd != 14)  // (poll, select and connect say it themselves)
        LOG("[bsd] cmd %u fd %d -> %d (errno %u)\n", req->cmd, req->data_size >= 4 ? IN(int32_t, 0) : -1,
            resp->data_size >= 8 ? OUT(int32_t, 0) : 0, resp->data_size >= 8 ? OUT(uint32_t, 4) : 0);
}

static void bsd_host_client_1(IpcReq *req, IpcResp *resp) {
    int32_t fd = req->data_size >= 4 ? IN(int32_t, 0) : -1;
    switch (req->cmd) {
    case 0: OUT(uint64_t, 0) = 0; resp_size(resp, 8); return;  // RegisterClient
    case 1: return;                                            // StartMonitoring
    case 2: case 3: do_socket(req, resp); return;              // Socket / SocketExempt
    case 5: do_select(req, resp); return;
    case 6: do_poll(req, resp); return;
    case 7: result(resp, 0, 0); return;                        // Sysctl
    case 23: result(resp, 0, 0); return;                       // ShutdownAllSockets
    default: break;
    }
    BsdSock *s = bsd_sock_ref(fd);
    if (!s) { result_len(resp, -1, BSD_EBADF, 0); return; }
    switch (req->cmd) {
    case 8: case 9: case 25: do_recv(s, fd, req, resp); break;   // Recv / RecvFrom / Read
    case 10: case 11: case 24: do_send(s, fd, req, resp); break; // Send / SendTo / Write
    case 12: do_accept(s, req, resp); break;
    case 13: do_bind(s, fd, req, resp); break;
    case 14: do_connect(s, fd, req, resp); break;
    case 15: case 16: {  // GetPeerName / GetSockName
        bool peer = req->cmd == 15;
        struct sockaddr_in pa;
        hsocklen_t pn = sizeof pa;
        // (not connected yet, or a non-blocking connect still in progress: the host socket knows)
        if (peer && (!s->peer_ip || (s->type == 1 && getpeername(s->h, (struct sockaddr *)&pa, &pn) != 0))) {
            result_len(resp, -1, BSD_ENOTCONN, 0);
            break;
        }
        uint32_t ip = peer ? s->peer_ip : s->bound_ip;
        uint16_t port = peer ? s->peer_port : s->guest_port ? s->guest_port : host_port_of(s->h);
        result_len(resp, 0, 0, put_addr(out_buf(req, 0), ip, port));
        break;
    }
    case 17: do_getsockopt(s, req, resp); break;
    case 18: {  // Listen(fd, backlog)
        int r = listen(s->h, IN(int32_t, 4));
        s->listening = r == 0;
        result(resp, r == 0 ? 0 : -1, r == 0 ? 0 : hsock_bsd_errno(hsock_errno()));
        break;
    }
    case 19: {  // Ioctl(fd, request, count) + buffers: FIONBIO, FIONREAD
        uint32_t rq = IN(uint32_t, 4);
        IpcBuf *b = in_buf(req, 0), *o = out_buf(req, 0);
        if (rq == 0x8004667e && b && b->size >= 4) s->nonblock = *(uint32_t *)(uintptr_t)b->addr != 0;
        if (rq == 0x4004667f && o && o->size >= 4) {
#ifdef _WIN32
            u_long avail = 0;
            ioctlsocket(s->h, FIONREAD, &avail);
#else
            int avail = 0;
            ioctl(s->h, FIONREAD, &avail);
#endif
            if (s->canned) avail = (int)(s->canned_len - s->canned_off);
            *(uint32_t *)(uintptr_t)o->addr = (uint32_t)avail;
        }
        result(resp, 0, 0);
        break;
    }
    case 20: {  // Fcntl(fd, cmd, flags): F_GETFL 3, F_SETFL 4. O_NONBLOCK is Linux's 0x800 (FreeBSD's 4 is taken too)
        int32_t cmd = IN(int32_t, 4), fl = IN(int32_t, 8);
        int32_t ret = 0;
        if (cmd == 3) ret = (s->nonblock ? 0x800 : 0) | 2;  // (O_RDWR)
        else if (cmd == 4) s->nonblock = (fl & (0x800 | 4)) != 0;
        if (bsd_trace()) LOG("[bsd] fcntl %d cmd %d flags %#x -> %#x (non-blocking %d)\n", fd, cmd, fl, ret, s->nonblock);
        result(resp, ret, 0);
        break;
    }
    case 21: do_setsockopt(s, req, resp); break;
    case 22: {  // Shutdown(fd, how)
        int r = shutdown(s->h, IN(int32_t, 4));
        result(resp, r == 0 ? 0 : -1, r == 0 ? 0 : hsock_bsd_errno(hsock_errno()));
        break;
    }
    case 26: {  // Close
        pthread_mutex_lock(&lock);
        if (table[fd] == s) table[fd] = NULL, s->refs--, s->closed = true;
        pthread_mutex_unlock(&lock);
        NLOG("[bsd] close %d\n", fd);
        if (net_log_on() && s->type == 1 && s->peer_ip) {  // (where a download's time went: connect, wait, transfer)
            char a[16];
            ip_to_str(s->peer_ip, a);
            uint64_t now = plat_now_ns(), t0 = s->t_open;
            long long conn = s->t_conn ? (long long)((s->t_conn - t0) / 1000000) : -1;
            long long first = s->t_first_in ? (long long)((s->t_first_in - t0) / 1000000) : -1;
            LOG("[bsd] fd %d %s:%u: out %llu / in %llu bytes, connect %lld ms, first byte %lld ms, total %llu ms, "
                "%u recvs, %u polls%s%s%s%s\n", fd, a, s->peer_port, (unsigned long long)s->bytes_out,
                (unsigned long long)s->bytes_in, conn, first, (unsigned long long)((now - t0) / 1000000), s->recvs,
                s->polls, s->canned ? ", answered by the game's mods" : "", s->first_line[0] ? " (\"" : "",
                s->first_line, s->first_line[0] ? "\")" : "");
        }
        result(resp, 0, 0);
        break;
    }
    case 27: {  // DuplicateSocket(fd, u64 owner)
        pthread_mutex_lock(&lock);
        int n = 0;
        while (n < MAX_FDS && table[n]) n++;
        if (n < MAX_FDS) table[n] = s, s->refs++;
        pthread_mutex_unlock(&lock);
        result(resp, n < MAX_FDS ? n : -1, BSD_EMFILE);
        break;
    }
    default:
        LOG("[bsd] cmd %u (fd %d): not supported\n", req->cmd, fd);
        result(resp, -1, BSD_EOPNOTSUPP);
    }
    bsd_sock_unref(s);
}
