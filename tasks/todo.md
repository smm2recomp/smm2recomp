# Port the OCW client mod (`_vendor/ocw-client-mod`) to the recomp

Goal: Super Mario Maker 2 in the recomp talking to OpenCourse World's servers, driven by the
player's own `ocw-config.json` (`domain`, `token`) — the same thing the Atmosphère / Ryujinx mod
does on a Switch, done the recomp's way.

## Why the mod cannot simply be loaded

`mod-v0.2.zip` is `atmosphere/contents/01009B90006DC000/exefs/subsdk9` + `main.npdm`: an exlaunch
NSO that rewrites live AArch64 instructions (`exl::patch::CodePatcher`) and uses
`nn::fs::MountSdCardForDebug` / `svcOutputDebugString`.

- `recomp/runtime/loader.c:177` loads **only** `main` and `sdk`. No subsdk slot, no rtld for mods.
- The game runs as recompiled C — there are no guest instructions left to patch at run time.
- The recomp's own "mods" are RomFS overlays plus C hooks (`runtime/mods.h:2`: "no game code").

So: a port. The good news is that most of what the mod does by patching, the recomp already does
host-side.

## What the mod actually does (`_vendor/ocw-client-mod/source/program/main.cpp`)

| # | Patch | Address (3.0.3) | Needed in the recomp? |
|---|---|---|---|
| 1 | write `domain` over a string | `main+0x22cc307` | **yes** — plain write to guest memory at boot |
| 2 | replace the `bl` that builds the WebSocket handshake, injecting the token header | `main+0xAF560` | **yes** — a gen-time patch |
| 3 | stub `GetNetworkServiceAccountId` -> 0xCAFE | `main+0x1CB7620` | no — `services/acc.c:99` answers `GetAccountId` already |
| 4 | stub `IsNetworkServiceAccountAvailable` -> 0 | `main+0x1CBA0A0` | no — same path |
| 5 | stub `LoadNetworkServiceAccountIdTokenCache` -> 0 | `main+0x1CB75F0` | no — `acc.c:64` `LoadIdTokenCache` returns the local JWT |
| 6 | `EnsureNetworkServiceAccountIdTokenCacheAsync` -> 0 | `main+0x01CB75A0` | no — `acc.c:60` returns an IAsyncContext |
| 7 | skip the AsyncContext wait + its destructor | `main+0x2F43C`, `+0x2F538` | no — `acc.c:45` hands back an already-signalled event |
| 8 | stub `EnsureNetworkServiceAccountAvailable` -> 0 | `main+0x60E794` | no — same path |
| 9 | disable the profanity filter | `main+0x17B8020` | **separate mod** (owner, 2026-10-22) — see below |
| 10 | disable the corrupt-course check | `main+0x17BB9C0` | **separate mod** (owner, 2026-10-22) — see below |

Items 3-8 exist because on a Switch those calls reach real Nintendo services. In the recomp they
reach `acc.c`, which already pretends success and hands out a local account ID plus a local JWT
whenever `net_real_sockets()` is true. **Two patches, not ten.**

## Plan

### 1. Config: `ocw-config.json` in the data root
- [x] New `recomp/games/smm2/mods/ocw.c` + `ocw.h`. Read `ocw-config.json` from the data root
      (CWD after `rt_paths_init()`, so plain `fopen` — same as `mods/settings.c:51`).
      `recomp/private/smm2/ocw-config.json` is the right place; it stands in for the mod's
      `sd:/ocw-config.json`.
- [x] Parse with the existing reader, `runtime/net/json.h` (`json_parse` / `json_get` / `json_str`).
- [x] Validate as the mod does: `domain` <= 29 bytes (the in-binary string slot), `token` exactly
      22 bytes. On anything missing or wrong: log and stay **inert** (the mod calls `EXL_ABORT`;
      we must not kill the game).
- [x] Env overrides for testing: `RECOMP_OCW_CONFIG=<path>`.
- [x] **Config shape** (owner, 2026-10-22). `domain` may be a host name **or a dotted IP**, and an
      optional third field points the name at an address:
      ```json
      { "domain": "server.opencourse.world", "token": "<22 chars>", "ip": "10.0.0.5" }
      ```
      - `domain` is what gets written into the binary, so the game uses it for its DNS lookup, its
        `Host:` header and (through `ssl.c:138`) its SNI.
      - `ip` present: `net_resolve(domain)` answers with it and the host resolver is never asked.
        This is what makes a **self-hosted / local OCW instance** work, and it is the clean way to
        get IP-only operation while keeping a real name for SNI and `Host:`.
      - `ip` absent and `domain` a name: the host resolver, every returned address allowed.
      - `ip` absent and `domain` an IP: works, but note SNI will be empty — `ssl.c:173` only sets
        the host name when it is not an IP, and `net_name_for_ip()` has nothing to rebuild from. A
        name-based vhost at the far end may then not route. Prefer `domain` + `ip`.

### 2. The domain (mod patch 1) — run time, no recompile
- [x] `loader.c:62` commits each module's whole mapping **read/write** (`plat_commit`), so `main`'s
      rodata is writable. The domain is a plain `memcpy` to `MAIN_BASE + 0x22cc307`.
- [x] Do it from `mods_boot()` (`runtime/mods.h:14`, "each boot, after the modules load").
- [x] No pre-known expected bytes needed (the mod writes them blind too): `setup.py` has already
      verified the ExeFS against the v3.0.3 build IDs **and** hashes in `versions.py`, which is a
      stronger guarantee than the mod's own runtime version check. Log the string that was there
      before overwriting it, so the first run records what was replaced.

### 3. The handshake header (mod patch 2) — gen-time patch
- [x] Add a patch at `main+0xAF560`. This one **replaces** rather than appends, so follow the
      `player_update_hook` idiom in `games/smm2/patches.py:17` (return C that stands in for the
      instruction) rather than the `_hook(word)` append helper — that helper's `bl` case dispatches
      through the original call and runs the host hook *after* it returns
      (`mods/smm1_extras.py:38-41`), which is not what we want here.
- [x] The expected-instruction guard needs **no value from the dump up front**: the generator reads
      the word itself (`gen.words[(addr - gen.lo) // 4]`, `mods/patches.py:66`). Write the patch so
      the first `make gen` *prints* the word found at that address; put it in the constant
      afterwards. It should be the original `bl snprintf` the mod branch-links over.
- [x] Host side in `ocw.c`: port `HeaderSnprintf` from the mod — the same `GET / HTTP/1.1`,
      `Upgrade: websocket`, `Sec-WebSocket-Key`, `<protocol>: <token>`, `Mod-Version`,
      `Game-Version` lines. Read args from the `Cpu *` (x0..x7 at the call) and write through the
      guest buffer pointer. Version strings: mod `0.2`, game `3.0.3`.
- [x] Inert when no config loaded: do exactly what the replaced `snprintf` did.

### 4. A new network backend `ocw`
This is the real work: the runtime's network policy is deny-by-default *by design*
(`DECISIONS.md:1705`, `net/netcfg.h:1`), and OCW is a public host.

- [x] Add `NET_BACKEND_OCW` to `netcfg.h` / `netcfg.c` (key `"ocw"`, listed in
      `net_backend_options()` so the Mods tab can pick it).
- [x] `net_real_sockets()` must be true for it — that is what turns the mod's Course World
      overrides off (`mods/backend.c:14`), makes `nifm` report online (`net_online()`,
      `net/online.c:14`) and makes `acc.c` hand out the local JWT.
- [x] **Name resolution:** `net_resolve()` currently answers only from the redirect table and
      `add_rule()` refuses non-local addresses (`netcfg.c:62`). Add a narrow path for the `ocw`
      backend: *the configured domain and nothing else* resolves — from the config's `ip` if given,
      else through the host resolver (`getaddrinfo`, the first time it is asked, cached). Register
      the result with `remember()` so `net_name_for_ip()` still gives `ssl.c:142` its SNI name.
      Every other name keeps failing.
- [x] **Destination policy:** `net_dest_allowed()` returns false outright unless `local-nex`
      (`netcfg.c:205`). Allow the OCW address(es) under the `ocw` backend. Keep everything else
      refused and logged — no blanket opening.
- [x] **Re-resolve, don't pin an address.** `server.opencourse.world` answered as 44.251.176.89
      (AWS), so the address is liable to change and there may be several A records. Allow *every*
      address the name resolves to, and re-resolve rather than hard-coding one, or a working setup
      breaks silently later.
- [x] **TLS: trust any certificate** (owner, 2026-10-22 — "ignore tls certs and trust any cert,
      even self signed; that's ok for our use case"). This needs no new trust mode and no CA
      bundle: `tls_trust=any` already exists and maps to `MBEDTLS_SSL_VERIFY_NONE`
      (`ssl.c:158-160`). The only change is the one gate at `ssl.c:147-151`, which today refuses
      "any certificate" towards a non-local address — let it through for the `ocw` backend's
      destination. The ISRG Root X2 / CA-bundle work in the earlier draft is **dropped**; it also
      means a self-hosted OCW with a self-signed certificate just works.
      Worth stating once: with verification off, anyone on the path between the Mac and OCW can
      read or alter the session, including the 22-character auth token in the handshake header.
      That is an informed trade for a fan project talking to a server the player controls or
      trusts; it should stay opt-in and never become the default.
- [x] Record the decision in `DECISIONS.md` and the item in `PROGRESS.md` (it is already tracked as
      "OpenCourse World as a course source", `PROGRESS.md:423`) — including that this is the first
      backend that reaches a public host, and that it is opt-in and off by default.

### 5. Settings + docs
- [x] `backend=ocw` in `smm2mods.ini` via the existing `game_net_config` key list
      (`mods/backend.c:28` — add nothing new if `backend` already carries it).
- [x] A Mods-tab entry, like `t_local_nex` in `mods/settings.c:133`.
- [x] Document in `recomp/docs/mods.md`: where `ocw-config.json` goes, how to get a token
      (sign in at opencourse.world, accept their EULA, take `domain` + `token` from the package
      they give you), and that the token is personal and must not be committed.

### 6. Test
- [x] `make gen` (2026-10-22, on the owner's Mac): all three patches applied, 105,374 functions in
      17 s, no guard tripped. The three words are in `OCW_HEADER_WORD` and `FORCE_OK` now, so a
      different build fails loudly.
- [x] `net_log=1` + `log=1` in `smm2mods.ini` and watch: domain written, name resolved, socket
      allowed, TLS handshake, the handshake header carrying the token. Done 2026-10-22: it connects.
- [x] Course World opens and lists courses; then download and play one. Done 2026-10-22, after the
      Mii and account fixes below: Course World opens, plays, and the profile is found on the server.
- [ ] **Still to do.** With `backend=ours` (default) and with no `ocw-config.json`: nothing changes,
      nothing goes out. This is the regression that matters most — it protects the default path that
      every other player is on, and nothing since the OCW work landed has exercised it.

### 7. Separate mod: the profanity filter and the corrupt-course check (owner, 2026-10-22)
Not part of the OCW port — each gets its own switch, off by default, following the SMM1 extras
pattern exactly (`mods/smm1_extras.c:1`, `mods/smm1_extras.py:1`).

- [x] New `mods/relax.c` + `mods/relax.py` (name TBD), two settings in `smm2mods.ini` with env
      overrides, two Mods-tab entries, both default off:
      - `no_word_filter` / `RECOMP_NO_WORD_FILTER` — the profanity filter always passes
        (`main+0x17B8020`)
      - `allow_broken_courses` / `RECOMP_ALLOW_BROKEN_COURSES` — the corrupt-course check always
        passes (`main+0x17BB9C0`)
- [x] Mechanism: the mod forces each whole function to `mov w0, 1; ret`. The recomp's hook idiom
      appends after an instruction, so either (a) a replacement patch at the function's first
      instruction that sets `x0` and returns early (the `player_update_hook` idiom,
      `games/smm2/patches.py:17`), or (b) hook the *consumer* of the result and flip the flags, as
      `smm1_extras.py` does for the pipe-overlap rule. Decide once the two functions are
      disassembled; (b) is more in keeping with the existing code.
- [x] Note in `docs/mods.md` that the word filter one only affects what the game shows locally —
      a course server does its own moderation (`docs/self-hosting.md`).

## Open questions / risks

1. ~~**TLS 1.2 only.**~~ **Resolved, 2026-10-22.** `openssl s_client -tls1_2 -connect
   server.opencourse.world:443` completes: the host accepts TLS 1.2, chain Let's Encrypt
   **ISRG Root X2** (ECDSA P-384). The vendored mbedtls supports it as built — `MBEDTLS_ECDSA_C`,
   `MBEDTLS_ECP_DP_SECP384R1_ENABLED`, `MBEDTLS_GCM_C`, `MBEDTLS_SHA384_C`,
   `MBEDTLS_X509_CRT_PARSE_C`, `MBEDTLS_PK_PARSE_C` and `MBEDTLS_FS_IO` (for `tls_ca=`) are all on
   in `third_party/mbedtls/include/mbedtls/mbedtls_config.h`, and
   `runtime/net/mbedtls_user_config.h` undefines only TLS 1.3, DTLS, the server side, tickets,
   net/timing and PSA storage. No mbedtls reconfiguration needed.
   Still the load-bearing part of that measurement even with verification off: `ssl.c:155-156`
   pins both min and max version to TLS 1.2, so a server that *required* 1.3 would fail the
   handshake no matter what the trust setting says. OCW accepts 1.2, so this is fine — but it is
   the thing to re-check if OCW ever tightens its TLS configuration.
2. **How far does the game's own online stack get in the recomp?** `local-nex` has only ever been
   run against `tools/nex_test_server`. Course World against a real service exercises much more of
   `bsd`/`ssl`/`nsd`/`friend`. Now the biggest unknown; find out early with step 6's log.
3. **The policy relaxation is a real change in posture.** Today the guest can reach nothing but
   loopback and local test servers, and that is a stated design decision. The `ocw` backend must
   stay opt-in, name exactly one host, and log what it allows.
4. **`_vendor/` is not gitignored.** `_vendor/ocw-client-mod` would be committed if added; the
   `*.zip` rule hides the mod packages but not the source. Decide whether to ignore it.

## The other route (not this plan, recorded for comparison)

A host-side OCW client in Rust as a course *upstream* next to TGRCode
(`core/crates/server/src/upstream.rs`, already `ureq` + rustls with a `Source::with_base`
abstraction): the guest network stays off entirely and no policy changes. Costs reimplementing
OCW's protocol host-side instead of letting the game speak it. Better long-term shape; more work,
and it does not reuse the mod.

## Review

**Written 2026-10-22; builds here only as far as this container allows (no clang, no dump).**

### Changed / added

| File | What |
|---|---|
| `recomp/games/smm2/mods/ocw.c`, `ocw.h` | new: the config, the domain write, the handshake builder |
| `recomp/games/smm2/mods/patches.py` | new patch at `main+0xAF560` (replacement, falls back to the game's own call) |
| `recomp/games/smm2/mods/relax.c`, `relax.py` | new: the two relaxed checks, one switch each, off by default |
| `recomp/games/smm2/patches.py` | loads `relax.py` as well |
| `recomp/games/smm2/mods/settings.c` | "Online: OpenCourse World" switch, the relaxed checks appended, one backend at a time, `all[16]` -> `all[24]` |
| `recomp/runtime/net/netcfg.h`, `netcfg.c` | `NET_BACKEND_OCW`, `net_ocw_set_target()`, `net_ocw_host()`, `net_tls_any_allowed()`, the one-name resolver |
| `recomp/runtime/services/ssl.c` | the "any certificate" gate is now `net_tls_any_allowed()` |
| `recomp/runtime/game_hooks.c`, `mods.c` | new weak hook `game_modules_loaded()`, called from `mods_boot()` |
| `recomp/runtime/platform/windows_exports.def`, `.c` | the three new patch hooks exported for the game DLL |
| `scripts/test_check_package.py` | the export guard now catches hooks whose first argument is not `Cpu *` |
| `recomp/docs/mods.md`, `DECISIONS.md`, `PROGRESS.md`, `.gitignore` | docs, the decision, status, `/_vendor/` ignored |

### Decisions taken while writing it

- **The inert fallback for the handshake patch calls the game's own function** (`guest_call_orig`,
  the `bl`'s own target) instead of re-running `snprintf` host-side. That makes "OCW off" exactly
  the original behaviour with no assumption about the format string or its arguments.
- **`game_modules_loaded()` is a new weak per-game hook** rather than a constructor: constructors
  run before `loader_load()`, when there is no loaded image to write into.
- **One backend at a time in the Mods tab.** The switch just moved wins; turning one off goes back
  to `ours`.
- **`smm2_relax_ok()` takes the hooked address**, so the two checks share one hook, as
  `smm1_extras_after` does.

### The account, and why the profile did not stick (2026-10-22)

Course World opened and played, but the player's profile did not persist and the
server did not find it again. Diagnosis, with the owner's evidence:

- `private/smm2/net/account.txt` **does** persist (`nsa_id 34f68a4ae0f374ae`), so the
  local account ID was stable across runs. "Doesn't stick" was therefore *not* a
  changing identity, and `token.c:71`'s `mkdir`/write is working.
- That leaves the **ID token**. `services/acc.c` was answering
  `LoadIdTokenCache` with the local JWT (`token.c:137`): `iss` `recomp-local`,
  `sub` the account ID as 16 hex digits, signed with a local key. The console mod
  stubs that call entirely, so a console sends **no ID token at all**. Most likely
  OCW read `sub` as the player identity — making us a different player from the one
  the 22-character token belongs to — or failed to validate the token and did not
  bind the profile.
- The account ID also differed: the mod writes a flat `0xCAFE` for *every* OCW
  player, which is itself the proof that OCW identifies players by the handshake
  token, not by this ID.

Fixed by matching the console mod on both (`net_account_shared()`): id `0xCAFE`,
empty ID token, under `backend=ocw` only. Given the stable id above, the empty
token is the change expected to matter; the id is fidelity. Which one it actually
was is still unconfirmed — a one-line experiment if it ever matters.

### Found while working (2026-10-22, second round)

- **The OCW token was reaching the log.** `services/ssl.c` `log_data()` dumps TLS plaintext under
  `net_log=1`, and the WebSocket upgrade our own hook builds carries the token — while the README
  tells players to attach `smm2recomp.log` to a bug report. Two fixes: the plaintext dump now has
  its own setting (`net_log_data=1`, off even with `net_log=1`, so `net_log` stays readable), and
  `net_log_secret()` / `net_log_redact()` (`net/netcfg.c`) overwrite any registered secret before a
  line is logged. `ocw.c` registers the token at boot. `net_log_data` also had to be added to
  `mods/backend.c`'s key list, or the ini would have ignored it.

### Found while working

- `scripts/test_check_package.py` caught `guest_call_orig` missing from `windows_exports.def` —
  the exact failure mode that broke the v0.3.0 first-launch build. Its regex only matched hooks
  taking `Cpu *` first, so `smm2_relax_ok` slipped through; widened, and all six hooks are
  guarded now.
- A failed OCW lookup would have been retried on every `net_resolve()` call. `ocw_when` is now the
  last *attempt*, with a 15 s floor on retries and the 5 min TTL on successes.

### Verified here

- Both patch modules import; the merged table is 32 patches and contains all three new addresses.
- Every changed C file passes `-fsyntax-only -Wall -Wextra` (gcc; only the pre-existing
  `__builtin_bitreverse*` warnings from `cpu.h`, which clang provides).
- `scripts/check_no_proprietary.py`, `test_check_no_proprietary.py`, `test_check_package.py` and
  `make -C recomp test-buildsync` all pass.

### Not verified — needs the Mac and the dump

- `make -C recomp GAME=smm2 runtime-objs` and the real link: **clang is not installed here**, so
  only syntax was checked.
- Everything in step 6: the log walk-through, Course World itself, and the no-change check.

### Closed 2026-10-22 by the owner's `make gen`

`main` generated in 17 s (104,463 boundaries, 105,374 functions) with all three patches applied and
no guard tripped. The words it reported are now the guards:

| Address | Word | Instruction |
|---|---|---|
| `main+0xAF560` | `0x947020B4` | `bl main+0x1cb7830` — the call the header is built with, as expected |
| `main+0x17B8020` | `0xD10683FF` | `sub sp, sp, #0x1a0` |
| `main+0x17BB9C0` | `0xA9BA6FFC` | `stp x28, x27, [sp, #-0x60]!` |

The two prologues close the open question about `relax.py`: each address is its function's entry, so
the early return is equivalent to the console mod's whole-function `movz w0, #1; ret`.

## Outcome (2026-10-22)

**It works.** The game connects to OpenCourse World, Course World opens and plays, and the player's
profile is found on the server. The open question the plan was built around — how far SMM2's own
Course World code gets against a real service — is answered: far enough. That is the first time the
game's own online code has reached a real service through the recomp, so it is also evidence for the
Nextendo route, which had only ever run against `tools/nex_test_server`.

Two things needed fixing beyond the port itself, neither of them in the plan:

1. **The Mii** (`services/mii.c`) — an empty database, so Course World asked for a Mii that could
   not be made. It now holds the one default Mii the runtime already had.
2. **The account** (`services/acc.c`, `net_account_shared()`) — the local ID token's `sub` claim was
   presenting an identity other than the one the handshake token belongs to. Now matched to the
   console mod: id `0xCAFE`, no ID token.

Also fixed on the way, found rather than planned: the TLS plaintext dump was putting the player's
token in `smm2recomp.log`, which the README asks players to attach to bug reports
(`net_log_data`, `net_log_secret`).

### What is still open

- The `backend=ours` no-change check (above) — the one thing left from step 6.
- Likes, comments and uploads against OCW: untried.
- Which of the two account divergences actually broke the profile. Both were matched to the
  known-good client at once; the stable `nsa_id` points at the ID token.
- The Rust-upstream route (step "The other route") is not needed for OCW now, but remains the
  better shape if the guest network path ever becomes a burden.
