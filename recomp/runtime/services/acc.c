// acc:u0 (one local user) and the preselected-user launch parameter. With backend nextendo (runtime/net/netcfg.h)
// the user is the signed-in Nextendo account: its user name is the nickname, its PID the network service account ID.
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../ipc.h"
#include "../net/netcfg.h"
#include "../net/nextendo.h"
#include "../net/online.h"
#include "../net/token.h"

const uint8_t g_user_id[16] = {0x4c, 0x41, 0x52, 0x65, 0x63, 0x6f, 0x6d, 0x70, 0x01, 0, 0, 0, 0, 0, 0, 0x01};

static void write_uid_list(IpcReq *req) {
    IpcBuf *c = out_buf(req, 0);
    if (!c) return;
    memset((void *)(uintptr_t)c->addr, 0, c->size);
    if (c->size >= 16) memcpy((void *)(uintptr_t)c->addr, g_user_id, 16);
}

// the game token over the network (at most 10 s): the calling thread's core is free meanwhile
static bool prepare_nextendo(void) {
    kernel_host_wait_begin();
    bool ok = nextendo_prepare_game_login(10000);
    kernel_host_wait_end();
    return ok;
}

// the nickname field holds 32 bytes of UTF-8 and a NUL: cut before a character that does not fit whole
static void put_nickname(char *dst, const char *name) {
    size_t n = strlen(name);
    if (n > 32) {
        n = 32;
        while (n && ((unsigned char)name[n] & 0xc0) == 0x80) n--;
    }
    memcpy(dst, name, n);
    dst[n] = 0;
}

static void profile(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: case 1: {  // Get / GetBase -> ProfileBase {uid, u64 timestamp, nickname[0x21]}
        memset(resp->data, 0, 0x38);
        memcpy(resp->data, g_user_id, 16);
        *(uint64_t *)(resp->data + 16) = 1600000000;
        char nx[64];
        if (net_backend() == NET_BACKEND_NEXTENDO && nextendo_username(nx, sizeof nx)) put_nickname((char *)resp->data + 24, nx);
        else strcpy((char *)resp->data + 24, getenv("RECOMP_USER_NAME") ? getenv("RECOMP_USER_NAME") : "Player");
        resp_size(resp, 0x38);
        if (req->cmd == 0 && out_buf(req, 0)) memset((void *)(uintptr_t)out_buf(req, 0)->addr, 0, out_buf(req, 0)->size);
        break;
    }
    case 10: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;  // GetImageSize
    case 11: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;  // LoadImage
    default: LOG("[acc] IProfile cmd %u (ok)\n", req->cmd);
    }
}

// Pretend online (runtime/net/online.h): the user has a network service account whose ID token is ready. The token
// is a local placeholder (no Nintendo service is asked; mods answer the game's online calls themselves). With the
// game's own network stack on (backend local-nex) the account ID and the token are the local account's
// (runtime/net/token.h): a JWT the local NEX test server can check. With backend nextendo they are the Nextendo
// account's PID and an ID token carrying its game token; loading the token first makes sure the game token is fresh
// (nextendo_prepare_game_login: the network, at most 10 s, with the core free; the game asks once per login). With
// nextendo and no ID token (offline, the sign-in has ended) the game gets none (size 0) and shows its own login
// error.
static void async_context(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: {  // GetSystemEvent: already signalled (done)
        uint32_t w, r;
        KEvent *e = kernel_create_event_pair(&w, &r);
        e->signaled = true;
        resp->copy_handles[resp->ncopy++] = r;
        break;
    }
    case 2: OUT(uint8_t, 0) = 1; resp_size(resp, 1); break;  // HasDone
    default: break;                                          // Cancel, GetResult: success
    }
}

static bool online_manager(IpcReq *req, IpcResp *resp) {
    static const char token[] = "recomp-local-placeholder-token";
    switch (req->cmd) {
    case 2: case 141:  // EnsureIdTokenCacheAsync, EnsureNetworkServiceAccountIdTokenCacheAsync...
        ipc_return_object(resp, service_new("IAsyncContext", async_context, NULL));
        return true;
    case 3: case 142: {  // LoadIdTokenCache -> buffer, u32 size
        IpcBuf *b = out_buf(req, 0);
        static char jwt[2048];
        const char *t = token;
        uint32_t n = sizeof token - 1;
        uint64_t shared;
        if (net_account_shared(&shared)) {  // (ocw: none at all, as the console mod leaves the buffer)
            if (b && b->size) memset((void *)(uintptr_t)b->addr, 0, b->size);
            OUT(uint32_t, 0) = 0;
            resp_size(resp, 4);
            return true;
        }
        bool nx = net_backend() == NET_BACKEND_NEXTENDO;
        if (nx && !prepare_nextendo()) LOG("[acc] Nextendo: no game token (offline, or the sign-in has ended)\n");
        if (net_real_sockets() && (n = (uint32_t)net_id_token(jwt, sizeof jwt))) t = jwt;
        else n = nx ? 0 : sizeof token - 1;  // (nextendo: no placeholder; the game reports its login error)
        if (b && b->size) {
            if (n > b->size) LOG("[acc] the ID token (%u bytes) does not fit the game's buffer (%llu)\n", n, (unsigned long long)b->size), n = (uint32_t)b->size;
            memset((void *)(uintptr_t)b->addr, 0, b->size);
            memcpy((void *)(uintptr_t)b->addr, t, n);
        }
        OUT(uint32_t, 0) = n;
        resp_size(resp, 4);
        return true;
    }
    case 130:  // GetNintendoAccountUserResourceCacheForApplication: u64 id, buffers left empty
        for (int i = 0; i < 2; i++) {
            IpcBuf *b = out_buf(req, i);
            if (b && b->size) memset((void *)(uintptr_t)b->addr, 0, b->size);
        }
        OUT(uint64_t, 0) = 0x1234567890abcdefull;
        resp_size(resp, 8);
        return true;
    case 150:  // CreateAuthorizationRequest
        ipc_return_object(resp, service_new("IAsyncContext", async_context, NULL));
        return true;
    default: return false;
    }
}

// The network service account id GetAccountId answers (the game's own player id in Course World: it compares a user
// record's player id with it for "this is me", e.g. My Info's Super World Delete). Nextendo: the PID once known, else 0.
uint64_t acc_account_id(void) {
    uint64_t shared;
    // (ocw: the one id the console mod gives every OCW player - OCW knows the player by the handshake token)
    if (net_account_shared(&shared)) return shared;
    if (net_backend() == NET_BACKEND_NEXTENDO) return nextendo_pid();
    return net_real_sockets() ? net_nsa_id() : 0x1234567890abcdefull;
}

static void manager(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (net_online() && online_manager(req, resp)) return;
    switch (req->cmd) {
    case 0: break;  // CheckAvailability
    case 1: {  // GetAccountId (the network service account; nextendo: the PID, known once a game token is)
        uint64_t id = acc_account_id();
        if (net_backend() == NET_BACKEND_NEXTENDO) {
            static bool asked;  // (asked before the ID token: one fetch, not one per call when offline)
            if (!id && !asked) asked = true, prepare_nextendo(), id = nextendo_pid();
        }
        OUT(uint64_t, 0) = id;
        resp_size(resp, 8);
        break;
    }
    default: LOG("[acc] IManagerForApplication cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}

SERVICE("acc:u0", acc_u0) {
    (void)s;
    switch (req->cmd) {
    case 0: OUT(int32_t, 0) = 1; resp_size(resp, 4); break;  // GetUserCount
    case 1:  // GetUserExistence
        OUT(uint8_t, 0) = !memcmp(req->data, g_user_id, 16);
        if (!OUT(uint8_t, 0)) LOG("[acc] GetUserExistence: %016llx%016llx is not the user\n",
                                  (unsigned long long)((uint64_t *)req->data)[1], (unsigned long long)((uint64_t *)req->data)[0]);
        resp_size(resp, 1);
        break;
    case 2: case 3: case 141: write_uid_list(req); break;  // ListAllUsers / ListOpenUsers / ListQualifiedUsers
    case 130:  // LoadOpenContext(uid): the user the application had open before (ours), as an IManagerForApplication
        ipc_return_object(resp, service_new("IManagerForApplication", manager, NULL));
        break;
    case 131: write_uid_list(req); break;  // ListOpenContextStoredUsers
    case 4: case 51: memcpy(resp->data, g_user_id, 16); resp_size(resp, 16); break;  // GetLastOpenedUser
    case 5: ipc_return_object(resp, service_new("IProfile", profile, NULL)); break;
    case 50: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;
    case 100: case 140: break;  // InitializeApplicationInfo
    case 101: ipc_return_object(resp, service_new("IManagerForApplication", manager, NULL)); break;
    case 150: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;  // IsUserAccountSwitchLocked
    default: LOG("[acc] acc:u0 cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}
