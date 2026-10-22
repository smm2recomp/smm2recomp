# Progress

## Roadmap
✅ done · 🔄 in progress · ⬜ not started · ⏳ waiting on owner

### Phase 1 — Environment and extraction ✅
- ✅ Dumps gitignored; provenance checked; toolchain pinned; CI + pre-commit guard
- ✅ SMM1 decrypted (base v16 + update v208); SMM2 v1.0.0 extracted; inventories; 550-course sample
- ✅ SMM2 update v3.0.3 extracted (patched RomFS + ExeFS); both executables converted to ELF

### Phase 2 — Course formats ✅ (awaiting go-ahead)
- ✅ Community format docs gathered and cross-checked (13 sources; disagreements recorded)
- ✅ Confirmed: SMM2 `.bcd` = 0x10 header + AES-CBC body + IV / random state / CMAC; SMM1 `.cdt` = 0x15000 BE, CRC32 @0x08 over [0x10:]
- ✅ Format docs: [docs/formats/smm2-course.md](docs/formats/smm2-course.md), [docs/formats/smm1-course.md](docs/formats/smm1-course.md)
- ✅ `smmfmt` reference package: SMM2 course parser/writer, SMM1 `.cdt` parser/writer (CRC), course codes, `python -m smmfmt` summary CLI
- ✅ SMM2 `.bcd` decrypt/encrypt; key table found at runtime in the owner's executable (`smmfmt.smm2.keyfind`, ~2 s; same table in v1.0.0 and v3.0.3)
- ✅ Byte-exact round trips: 550 dataset courses, 180 built-in SMM2 courses × 2 versions (decrypt → parse → write → re-encrypt), 156 built-in SMM1 `.cdt` — 30 tests, CI runs the 24 that need no game data
- ✅ Finding: path arrays (snake, clear pipe, creeper, ! block, track block) are slot-indexed and can be sparse
- ⏳ Owner's own saved courses (optional; not needed to proceed)

### Phase 3 — Binary analysis ✅ (awaiting go-ahead)
Specs are private (`research/specs/`, gitignored): behaviour in prose/tables, no decompiled code.
- ✅ Ghidra headless analysis of SMM2 v1.0.0 and v3.0.3 (≈7–8 min each), SMM1 `Block.rpx` v16 + v208
  (own RPX→ELF converter, nothing installed); query scripts in `tools/ghidra/`
- ✅ Data-file survey: physics is in code; EnemyDB attack matrix, EditDB parts, per-style SceneDB,
  263 player states are data; 108 lesson input replays found
- ✅ Course-loading pipeline: decrypt → validate → placements → actors; part→actor tables per style;
  flag bits resolved (big = 0x4000 size field, 0x400 = on track, 0x1 from pipe, …)
- ✅ Player physics: one shared model for all styles (style = feature switches); SMB1 constants
  complete (walk 1.5 / run 3.0, jump 3.628 + speed bonus, held-jump gravity bands, fall cap −4.0);
  simulated jump heights 4.2 / 5.4 tiles match the game; unchanged in v3.0.3
- ✅ Themes: night = area flag bit 1; per-theme day/night effects, wind, liquids, autoscroll, timer,
  clear conditions (CRC32 of internal names)
- ✅ Objects: 82 object specs + shared mechanics (actor base, 18 enemy contexts, generic walker,
  attack/reaction matrix, blocks); all top-25 objects in depth
- ✅ SMM1 100 Mario Challenge code: 11/14 open questions answered (requests 50 courses per band,
  lives cap 100, ≤3 banked lives per course, free unlimited skip, unlock rules, reward selection)
- ⬜ Left for later phases: enemy stomp bounce speed, activation range, 44 style power-up and 42 clown
  car specs, remaining 51 rarer/2.0+ object ids (Phase 7)

### SMCraft (the clean-room engine, Phases 4–11) — archived 2026-10-19
At the owner's call the recomp is now the only version. SMCraft (Rust engine, Swift app, web
build, SMM2 front end, Course Maker, its own 100 Mario) is kept whole on the `smcraft-archive`
branch (tag `smcraft-final`), where it builds and its tests pass as they did; its roadmap and
evidence are in that branch's PROGRESS.md. Main keeps the parts the recomp uses: the course
server and the crates it needs (`core/`: formats, sim, editor, modes, netplay, server, online,
and `mmk`, now just the server tool).

### Course server (core/) ✅ in use by the recomp's mods
- ✅ `mmk server`: accounts (name + secret token), course upload / search / download, likes,
  comments, leaderboards, world records (replays verified by SMCraft's engine), Endless runs held
  on the server, online-play relay (UDP + WebSocket). Self-hosting: docs/self-hosting.md
- ✅ 100 Mario ruleset for Endless runs (`{"ruleset":"hundred"}` on `/start`: 100 lives capped at
  100, done after 8 / 16 / 16 / 6 clears with `run_complete`, SMM1 fail-rate bands, free skips),
  `hundred_<d>` leaderboards, the caller's own rank row (`me`)
- ⬜ Replay verification for recomp runs: the server re-simulates replays with SMCraft's engine,
  which isn't the game; recomp records need their own proof [?]

### Recomp — native SMM2 v3.0.3 by static recompilation 🔄 (owner request 2026-10-18; the only version from 2026-10-19)
In `recomp/`; approach in DECISIONS.md (base: LAS Recomp, MIT; Wonder
framework ideas; Ryujinx as the reference). Nothing counts as done without the evidence below.
- ✅ Approach chosen and recorded; proprietary-content guard extended for recomp output
- ✅ Toolchain: LAS Recomp forked into `recomp/` (MIT notice, upstream commits in `recomp/UPSTREAM.md`), retargeted
  to SMM2 v3.0.3 (`main` + `sdk`), shared runtime + `games/smm2/`; one command from the player's dump:
  `make -C recomp smm2` (setup, recompile, build), `make -C recomp run-smm2`
- ✅ R1 (macOS): boots to the title screen and renders it (2026-10-19). What it took: `recomp/docs/smm2-bringup.md`
  (per-core scheduling, nifm/friend/mii/fonts/system data, NVN conditional rendering, 3D render targets, blocking
  first-use compiles, narrow vertex strides, in-order uniform updates, translator TLD4/TMML/LDC.64 fix)
- ✅ Shader prefetch (2026-10-20, Metal + Vulkan): a submission's first-use builds start side by side, draws wait for
  theirs and are never skipped; cold worst frame 19.6 -> 2.8 s (Mac), 6.9 -> 2.6 s (iPad), warm unchanged
  (`recomp/docs/smm2-bringup.md`)
- 🔄 Title screen frames vs Ryujinx, five courses in four styles (3D World, SMB1 x2, SMB3, NSMBU), same course
  and nearest frame, both windows grabbed through the window server: 2.50-4.80 % of pixels differ by more than 24,
  0.66-2.98 % by more than 48, 0.13-1.15 % by more than 96 (table in `recomp/docs/smm2-bringup.md`). Fixed in
  this round: skinned meshes (Mario, Koopa Car: signed LDC offsets), a culled stand-in mesh, geometry shaders,
  2D tile sheets (rendered textures keep their memory). Left: texel-edge and text differences, animation phase
  (not exactly alignable: Ryujinx grabs are wall-clock timed). Ryujinx against itself: 0.3-1.8 %.
  Evidence: `recomp/private/smm2/evidence/r1/final_*` (`_side_by_side.png`, `_diff.png`, `_recomp.png`; private,
  not in git)
- 🔄 In-course frames vs Ryujinx, lockstep (2026-10-19): 90 reference courses (5 styles x 10 themes, night for the four
  2D styles), the same game state in both runtimes (both freeze at the same player update, every `sead::Random`
  seeded alike) and the presented frame captured raw at 1080p. **71 of 90 bit-identical** at update 900 (70 at 1200):
  every SMB1, SMB3 and SMW course, day and night; NSMBU and 3D World within 20 pixels of +-1-4 levels, except NSMBU
  underwater (3,706 px, <= 17 levels; 0.00 % > 24) and the 3D World liquid surfaces (castle lava 2.28 % > 24 /
  0.03 % > 48; forest poison 0.20 % / 0.05 %; deterministic in both runtimes, so a real difference: the lava surface
  draw, not yet found). Table and how: `recomp/docs/smm2-bringup.md` ("In-course frames"); evidence
  `recomp/private/smm2/evidence/frames-r3/` (side-by-sides, diff maps, tables; private)
- ✅ Audio vs Ryujinx (rough check, 2026-10-19): each runtime's mixed output dumped and aligned. Boot / logo audio
  matched; in a course the recomp was ~5 dB quieter (10 dB above 2 kHz) because voices sent through splitters were
  dropped. Fixed (splitters, the AuxiliaryBuffer and BiquadFilter effects): the course's level now -27.5 dB against
  Ryujinx's -27.6, every octave band within 0.6 dB, 70 sound onsets against 69 (not sample-aligned: audio runs on host
  time)
- ✅ Windows / Linux ports (2026-10-19): `make -C recomp TARGET=windows-x86_64 | linux-x86_64 | linux-aarch64 GAME=smm2
  DEPS=<deps> all package`, cross-built from macOS with zig; the SMM2 fixes ported to the Vulkan backend and the GLSL
  translator (conditional rendering, report values, buffer clears, cull-both skip, 3D target slices, first-render
  uploads, storage aliases, cube-array views, in-order uniform updates, compute result flush, blocking compiles, real
  geometry shaders, TLD4 / TMML); player packages recompile the dump on first launch. Evidence in the table;
  `recomp/private/smm2/evidence/linux/` (private). Mario's model on the 3D World title (2026-10-19): his composite drew nothing (vertex data 2.6 GB into the heap, past lavapipe's 2 GB allocation limit: now copied there) and his depth mask was not written on lavapipe (a lavapipe-only workaround splits such draws into submissions); he is in the scene now, but a few NaN texels at his eyes bloom into a white blur in the final frame (open). Shadow samplers on a colour texture now go through a depth copy (validation clean apart from the 3.2 GB global-memory binding, see `recomp/docs/smm2-bringup.md`). Left: that NaN; Windows and x64 untested
- 🔄 iOS / iPadOS (2026-10-19): `make -C recomp TARGET=ios-arm64 | iossim-arm64 GAME=smm2 app` (AOT: the game code is compiled on the Mac and linked into the app; no JIT, no game code library); Documents holds the game files and the log; touches are the console touch screen (macOS: the left mouse button), controllers through SDL; BC textures decoded on the CPU where the GPU lacks them. Evidence: the Simulator boots to the title; an iPad Pro 13 M5 (iOS 27.0, installed over Tailscale, App ID with extended virtual addressing + increased memory limit: `recomp/tools/ios_profile.sh`) boots through the intro to the title screen and its menu at a steady 60 fps (MetalFX to 2752x2064, the 3.25 GB heap buffer taken whole), no faults (`recomp/private/smm2/evidence/ios/`, private: screenshots, boot log, the address-space log below). The guest window stays at 0x7000000000 on iOS: with the entitlement iOS 27 gives an app up to 0x8000000000 and keeps 0x2ee000000-0x7000000000 itself (measured; `GUEST_ORIGIN` moves it where that differs). On-screen controller for touch-only devices (stick, ABXY, L/R/ZL/ZR, +/-, menu, hide), other touches stay the touch screen. Left: play a course on the device; the on-screen controller is drawn but not yet tried by hand
- 🔄 Android (2026-10-19, emulator only: the owner has no Android device): `make -C recomp TARGET=android-arm64 GAME=smm2 apk install push-data` (AOT as on iOS: the game code is compiled on the Mac and shipped in the APK as `libsmm2game.so`; SDL's Java activity, no Gradle; deps from source: `tools/android_deps.sh`); game files in the app's external files folder (adb), log there and in logcat, `recomp.env` for environment variables; Vulkan backend with an RGBA swapchain fallback, compositor rotation, surface recreation after the home screen, CPU BC decoding; on-screen controller and touch screen, Back = overlay menu. Guest window at 0x800000000 (DECISIONS.md "Android"): [obs] the emulator's kernel has a 39-bit address space, and on one of its boots 742 mappings (6.4 GB) sat in the desktop window 0x7000000000-0x7320000000. Fixed on the way for every Vulkan target: the translator's GLSL `fmin` / `fmax` helpers (renamed: they broke MoltenVK's SPIR-V -> MSL), a settings-load crash on a saved `smm2.ini`. Evidence (`recomp/private/smm2/evidence/android/`, private: screenshots `emulator_title_3.png`, `emulator_stick_walk.png`, `emulator_a_jump.png`, `emulator_course_walk2.png`, `emulator_back_menu.png`, `emulator_resumed.png`, boot logs): intro, full title screen, the title course played by touch. Left: a physical device (install path untested); a course from the menus; fps on real hardware
- 🔄 Frame generation, 120 fps output (owner request 2026-10-20; DECISIONS.md 2026-10-20): Video tab "Frame
  generation: Off / 120 fps" adds one motion-estimated in-between frame per real one on displays of 100 Hz and more;
  the game itself stays at 60 (scripted-run player traces identical off and on). Spec and plan:
  `docs/superpowers/specs/2026-10-20-frame-generation-design.md`, `docs/superpowers/plans/2026-10-20-frame-generation.md`.
  - ✅ The passes: one source for Metal and Vulkan (`runtime/nvn/framegen_src.c`); `make -C recomp test-framegen`
    (scroll lands halfway, still image and HUD exact, a cut shows the previous frame, a sprite moves halfway).
  - ✅ macOS (Metal), with the 120 Hz path forced (`LARECOMP_FORCE_DISPLAY_HZ`: the Mac's displays are 60 Hz): the game
    holds 60 with an in-between frame for about every real one, Mario halfway in the generated frame, the menu sharp;
    on a 60 Hz display nothing is generated
  - ✅ Vulkan (Linux arm64 on Mesa lavapipe in a container): generated frames, a window resize, the validation layer
    clean apart from its two earlier messages; Windows and Android build
  - ⏳ iPad: the app is built and signed; checking real 120 Hz on the M5 waits for the iPad to be online
  - ⬜ A real 120 Hz display on macOS, Windows, Linux and Android (none here)
- ✅ R2 (macOS): plays courses by input injection, and input replays stay in sync with Ryujinx frame for frame
  (2026-10-19). The game now gets input (`nn::hid::GetNpadStates` answered by the runtime; before, it saw none,
  keyboard included); `RECOMP_PAD_SCRIPT` plays the Ryujinx trace tooling's scripts on the game's own clock (one frame
  per player update); `RECOMP_TRACE_OUT` records the player at the same instruction as the Ryujinx trace build;
  `tools/replay_sync.py` runs both and compares. What it took and how: `recomp/docs/smm2-bringup.md` ("Input replays")
  - Replays: 31 sessions, 456 tests (flat, wall, slope, water, items and power-up forms in all five styles, plus
    two runs on the game's own title course with enemies): 221,189 of 221,189 player updates identical in position,
    speed, facing and state, bit for bit; the whole 12 KB player object identical apart from one word holding a heap
    address. No divergence, so no recomp bug to fix
  - Plays: the game's own title course from the start to the goal (Koopa Car, enemies, coins; 1,911 updates, identical
    in both runtimes); Course Maker test play reached through the menus (L + R, Make, −) on the presented-frame clock
  Evidence: `recomp/private/smm2/evidence/r2/` (per-course tables `results/*.md`, traces of both runtimes, sessions,
  screenshots; private, not in git)
- 🔄 Mods (owner's plan, 2026-10-19: a perfect SMM2 v3.0.3 recomp; SMM1's 100 Mario exactly as SMM1;
  each SMM1 feature behind its own setting; never Nintendo's servers)
  - 🔄 **100 Mario Challenge exactly as in SMM1** (not SMM2's Endless screens reskinned): SMM1's own
    screens from the owner's SMM1 dump, drawn by the recomp host: difficulty tabs, the world map with
    the Goombas carrying Peach and the castle at the end, the dot walk, the start card with 100 little
    Marios, pause, clear, game over, credits (`Nwk_StaffCredit_00/01/03`) and `T_ThankYou_10/20/30`;
    the rescue / ending scene recovered from the Wii U binary (code, not a layout), emulator footage
    of the owner's SMM1 dump as the reference. Courses play in SMM2's own course play.
    - ✅ Server: `hundred` ruleset (100 lives capped at 100, runs of Easy 8 / Normal 16 / Expert 16 /
      Super Expert 6 then `run_complete`, SMM1 fail-rate bands, free skips), `hundred_<d>` boards
    - ✅ Game side over our server (`recomp/docs/mods.md`): pretend-online (nifm, acc NSA, mii, enl
      session, Course World's login answered locally), Course World opens against our server; the
      Endless requests answered from `/api/endless`; courses download (the game's own encryption, its
      CRC / validity checks pass) and play in SMM2's own play, ×100 on the course card and HUD; clear /
      miss / skip / suspend / finish reported. Evidence [obs]: a full **Super Expert run won against a
      local server** - 6 clears, 3 misses (100 -> 97), skips, the win -> ending -> run finished; the
      server's `hundred_super_expert` board shows the completion (`recomp/private/smm2/evidence/mods/
      se_run/`: run log, server JSON, screenshots; `course_in_card_100.png`, `course_hud_100.png`)
    - ✅ The bridge to the SMM1 screens (`games/smm2/mods/hundred.h`, `bridge.c`): a host call starts a
      server course in SMM2's play (the Endless menu stays hidden and is driven), the outcome comes back
      as `hundred_course_result`; the win plays the ending and finishes the run (not a game over).
      ✅ Continue (the menu offers it; a saved Easy run continued at 100 lives / 1 clear and cleared on),
      ✅ SMM2's pause held closed and routed to the flow (resume, Start Over = a life, Save & Quit), ✅ the
      next course named before the card, `form` at the goal, the difficulty picked with L / R (New Game
      on Super Expert with a saved Easy run started)
    - ✅ SMM1 client rules (`games/smm2/mods/hooks.c`, 22 hook points, only in a 100 Mario run): lives
      cap 100 (HUD and in-course clamps), free skip during the miss demo, a miss rolls back the
      attempt's coins and score, a multi-life award banks one 1-Up; unlocks Expert after a won Normal /
      Super Expert after a won Expert (server boards). Seen in runs: free skips, 100 -> 99 per miss,
      1-Ups banked 0 -> 1 -> 2 -> 3 on the HUD (`evidence/mods/pending_1ups_*`); the coin / score rollback
      runs on every retry (logged) but no recorded attempt collected coins before dying yet; no SMM2
      award worth several lives found in a test course (the cap is untested in play)
    - 🔄 SMM1 screens and the ending, host-drawn (`games/smm2/mods/smm1/`: SMCraft's SMM1 layout player
      ported to C, a CPU rasterizer drawn through the overlay on Metal and Vulkan; everything from the
      player's SMM1 dump, generated into `mods-generated/smm1-ui-v6/`):
      - ✅ difficulty select (tabs by unlock, Continue only with a record, OutR/OutL slides), world map
        demos as Block.rpx plays them (MapIn, Telop, the Goombas carrying Peach, AnimB, Mario's jump /
        walk / Go, the castle at the end, lives and n/total counters, A skips to Go), the start card (one
        little Mario per life, AnimA/AnimB by case, held until the course plays, Out over it; "Loading
        courses..." while the server gathers them); between courses SMM2's own screens (course-in, pause,
        skip, retry, clear and its Next / Like / Boo result), then SMM1's map and card after each clear
        (`HUNDRED_NEXT` from the run side)
      - ✅ the ending from the binary: the rescue scene in the cleared course's style (SMB1, SMB3, SMW
        sprites; NSMBU's 3-D Mario posed with its walk), Peach (Toad on Easy), the Thank-you line by
        difficulty, the credits roll on SMM1's timing (`Nwk_StaffCredit_01/00/03`) with each cleared course
        as SMM2's own course card (`Play_CourseIn_00` from the game's romfs: name banner, maker underneath,
        no Mii), TO BE CONTINUED... / END / THE END; nothing skips (as SMM1); a hook for the Mystery
        Mushroom reward
      - ✅ checked against SMM1 itself in Cemu (owner's dump, private captures in
        `evidence/cemu/`): colours composed in linear light into an sRGB frame as SMM1 does, font border
        and shadow parameters, the balloon's place and dot pattern, the styles' skies, NSMBU Mario's yaw,
        skinning, depth and lighting; NSMBU's 3-D sky is an approximation (fixed picture) [inf]
      - ✅ SMM1's music and sounds (Course World stream per difficulty with the beat bounce, the map
        streams, the ending and staff-roll music incl. SMW's Toad tune, the screens' sound banks), the game's sound faded under
      - ✅ a whole Super Expert run against the local server, unattended: select → map → card → course →
        clear … (skips, misses) → the ending with the run's courses and makers in the credits → back on
        the select (`RECOMP_HUNDRED_AUTO`, `RECOMP_HUNDRED_SHOTS`; evidence in
        `recomp/private/smm2/evidence/e2e5_sx/`, private); pause, Save & Quit and Continue through the run side
      - ✅ Challenge Modes: with the mod's modes on, the select's header reads "ZL < 100 Mario Challenge
        > ZR" and ZL / ZR or a tap on it switch to SMM2's Endless (`hundred_mode_switch`); the run side
        asks `hundred_flow_ready_to_start` before starting a course (true once the card is up); touches
        don't reach the game under a full host screen
      - ⬜ per-frame sound triggers (SLink database not decoded: sounds are placed by name [inf]), the
        NSMBU ending's perspective camera; 🔄 Cemu side-by-sides (Cemu runs the owner's SMM1 here, frames
        captured; reaching the ending scene in progress)
  - 🔄 **Settings, one toggle per SMM1 feature** (overlay menu's Mods tab, kept in `smm2mods.ini`):
    ✅ "100 Mario Challenge (SMM1)" (on screen in the Mods tab: `evidence/mods/mods_tab.png`); ✅ "Mystery Mushroom costumes" (`mods/costume.c`, `recomp/docs/mods.md` "Mystery Mushroom
    costumes"; finished 2026-10-22, checked in game on a test course, `tools/costume_course.py`): models generated
    from both dumps, worn as small Mario in SMB1 courses; the Mystery Mushroom (the SMB1 Big Mushroom) taken as small,
    Super or Fire Mario; ✅ **more than one costume per course**: a new costume drawn at each pickup (SMM1's draw:
    owned minus the one worn), put on by writing its texels over the loaded model's textures (every costume model is
    the same template; the textures found through the runtime's new `game_texture_init` hook) - seen: Peach, then
    Pikachu / Yoshi at the second pickup; ✅ **a hit removes the costume**: a Koopa's hit (test course `--variant
    hit`) takes it off through the Super → small power-down (the costume flickers, then small Mario blinking); ✅ **the
    taunt**: Up held 8 updates standing still → the costume's appeal frame for 120 updates (Peach's parasol, Samus
    facing front) and its appeal sound; not after the goal; ✅ **the reward window**: its lower-right frame was cut off
    by the layout player's four-frame window texcoords (a flipped frame mirrored about its middle); now anchored at
    the frame's corner as nw::lyt does (`lyt_player.c` `frame4_uv`); the other 100 Mario screens render identically
    (63 frames compared); ✅ **"Mystery Mushroom costume now"** in Try it now with a costume choice row (every costume,
    DECISIONS 2026-10-22): MarioGold given mid-course; the power-up sound now comes from the costume's Static bank.
    ✅ **SMM2's jump sound muted under the costume's** (2026-10-22): the game's own request (the player's jump sound
    0x71015c78b0 reads its request as silent, hook point main+0x15c78c4; one state's own "jump", 0x71015cfe70, gets a
    sound user without an instance, main+0x15cfe90) and the costume's jump plays there instead of on take-off; a
    costume without a jump sound keeps SMM2's (SMM1's fallback; goal and death fanfares likewise now). Seen with the
    audio log: 5 jumps play SMB1's jump wave 5 times with costumes off, 0 times in Peach, 5 times in Goomba (no jump
    sound); ✅ **sub-areas**: the player object and model stay through a pipe, so the costume stays on, main → sub →
    main → sub (test course `--variant subarea`, Peach given by "costume now"); ✅ **the Mystery Mushroom's own look**:
    the item's model (`M1_Item_special` / `special2`, or `M1_Item_kinoko` with `costume_item=super`) in the generated
    pack gets SMM1's "?" mushroom (`Add_kinoko` of `M1_Item_CharaKinoko`, the update's `CharaKinoko2`) at one texel a
    pixel (generator "costumes" v4). Evidence (private): `recomp/private/smm2/evidence/costume-leftovers/`.
    ⬜ the Mystery Mushroom keeps the Big Mushroom's 2 x 2 size (only its look is SMM1's); the editor palette's icon is
    still the Big Mushroom's; the data folders' SMM1 subset (made before `tools/smm1_subset.sh` took `Static.pack`
    and `Sound/CharaMario`) has no costume table, so a draw finds no owned costume there: run the script again.
    Note: the data folders' SMM1 subset link has no `Sound/CharaMario`, so costume sounds log "none" there (the full
    disc dump has them). **Crash ("Pure virtual function called", WorkerMgr, main+0x4d2f50: a
    queued object destroyed while its request was pending)** seen twice with costumes on (entering Course World;
    a course start). The costume pack was 38 MB against the game's 5 MB (stored zstd blocks); it is now
    compressed (8.4 MB, generator "costumes" v2). Since then 5 of 5 runs with costumes on entered Course World
    and started a course without the crash (before: 2 crashes in about 6 such runs; logs `crash1-3.log`,
    `cos8.log`, `cos9.log`, private). Likely fixed by the smaller pack [inf: consistent, not proven; the SMM1
    audio mix is host-only and touches no guest object]; not seen in the 2026-10-22 runs (about 15 with costumes on;
    the one crash there was a test set-up's: a mod folder without the generated pack, and a missing costume model
    stops the builder; the builder now only loads a costume whose model it finds in the pack);
    the SMM1 extras (`mods/smm1_extras.c`, all off by default, `recomp/docs/mods.md` "SMM1 extras"):
    ✅ "Overlapping pipes (SMM1)" (one hook in the game's placement check, main+0xfe827c: SMM2 rejects a
    course whose pipes overlap and plays a blank one, and its Course Maker snaps a pipe dragged onto another back;
    with the setting on, both test courses (ours and the pipes of SMM1's sample course 95, read from the SMM1
    dump) load and play, and a pipe dragged onto another stays; with it off, a replay against Ryujinx is
    identical in 606 / 606 updates and the whole player object; both switches on screen in the Mods tab: `evidence/smm1-extras/`); ✅ "Mid-air shell
    jumps (SMM1)" (SMM1's flat falling stomp window on shells, main+0x1267ce8, and the 5-frame instead of
    16-frame no-hit window after a mid-air kick off a wall, main+0x1202c88; ✅ test course `tools/shell_jump_course.py`
    with a scripted input, 2026-10-22: Mario kicks a shell off a wall in mid-air; on, he lands on it as it comes
    back and bounces onto the wall top; off, it passes under him (16-frame window) and he can't climb; traces equal
    to update 259, reruns byte-identical; `research/evidence/smm1-shell-jump/`; ⬜ an off-case Ryujinx replay with
    shell contacts); ⬜ more from research/specs/smm1-extras*
  - ✅ **Edit any course** (`games/smm2/mods/edit.c`, `recomp/docs/mods.md` "Edit any course"):
    ✅ Course World's Download into the Coursebot against our server (stand-in thumbnail); ✅ SMM2's Make in the
    Coursebot window of a downloaded course (SMM1's Edit placement), the Course Maker opens with the course, an edit
    saved to My Courses and played (screens private); ✅ typed names intact on Save As (re-tested 2026-10-21);
    ✅ d-pad cursor onto Make (2026-10-22: the cursor controllers' neighbour links, Make at the top of the column as
    in My Courses; My Courses' window unchanged),
    ✅ the copy is the player's own (2026-10-21: reset after the Course Maker's reload; saved copy has upload id 0;
    the yellow arrow on cards isn't an upload mark), ✅ the "Based on" record follows Save As (new creation id; checked
    in game 2026-10-21), ✅ upload credit in game (2026-10-22: download → Make → edit → Save As → clear check →
    Upload → Course World shows "Based on 9TC-5PV-7CG by <maker>"; fixed CanPostCourse's answer and the maker name),
    ✅ Edit from Course World's card (2026-10-22: Download on a course already in the Coursebot opens the Course
    Maker instead of SMM2's "You cannot save the same course."), ✅ the card's button text (2026-10-22: a course in
    the Coursebot reads the Coursebot window's own "Make", looked up in the player's message archive at run time;
    other cards keep Download; reused cards follow their course), ✅ Endless / 100 Mario "save for later" (2026-10-22,
    DECISIONS "Edit any course, the leftovers": the run's courses on the player's Played Courses → Download keeps one
    in the Coursebot → Make edits it; checked with a 100 Mario run), ✅ an uploaded course's picture (2026-10-22: `PUT
    /api/courses/<code>/thumbnail` after the upload, the uploader only; New Courses shows it); screens private
    (`evidence/mods/edit-leftovers/`); ⬜ an own upload's whole-course map (the game uploads none)
  - 🔄 **Our open-source server everywhere the game would reach Nintendo** (`recomp/docs/mods.md`,
    `games/smm2/mods/world.c`): ✅ Course World's course lists (Hot, Popular, New, detailed search, a
    maker's courses) from `/api/courses`, the course page (likes, plays, clear rate, world record, maker
    name and Mii), download and play in SMM2's own play, the play / clear posted with its time
    (`evidence/mods/world/`); ✅ leaderboards list the server's makers with their maker points
    (`leaderboard_makers.png`); ✅ SMM2's own Endless under the `smm2` ruleset (5 lives, endless, its
    own screens and HUD; an Easy run against the server: `evidence/mods/smm2_endless/`); 🔄 built, not
    yet seen working: likes / boos (the clear screen's hearts took no test input), comments (text),
    the ID search, maker pages; ✅ uploads from the Course Maker (2026-10-22: the course, its credit and its picture;
    New Courses lists it with the picture), ✅ a player's Played Courses (the run's courses too); ✅ Super Worlds
    (list, map, courses, title, progress and Continue, world clear; below); ✅ thumbnails (item 6 below); ⬜ Ninji;
    ✅ courses=tgrcode setting (built-in server, awaiting core/crates/embed);
    ⬜ online multiplayer: plan in `DECISIONS.md` (local play over our relay first, Network Play at
    the enl layer second)
  - 🔄 **The game's own network stack** (groundwork for Nextendo Network and a local NEX server, offline:
    `recomp/docs/mods.md` "The game's own network stack"): ✅ real TCP / UDP sockets (`bsd:u`), nsd + sfdnsres
    from a deny-by-default redirect table, `ssl` on Mbed TLS with the runtime's trust policy, a local account and
    JWT ID token, `backend=` setting (ours / local-nex / nextendo greyed out), `tools/nex_test_server`;
    ✅ **Proof 1**: with `backend=local-nex` the game resolves its NEX host to the test server, opens TLS, upgrades
    the WebSocket and sends its NEX login (`ValidateAndRequestTicketWithParam`, decoded, token checked; evidence
    in `/Volumes/SSD/smm-work/recomp-net/evidence/proof1/`, private); ✅ default backend unchanged (no sockets,
    no lookups); ✅ **Proof 2**: `tools/nex_test_server` answers the ticket, the secure connection, Register,
    GetIntegerSettings and SMM2's DataStore login calls (GetUsers, GetUserNameNgType, SyncUserProfile,
    GetMiiClothes, GetNewNotification), and Course World's top screen opens through the game's **own** NEX client,
    no mod override involved (`evidence/proof2/`, private); ✅ UDP port parking for PIA; ⬜ course lists, downloads
    and the rest of DataStore on the local NEX server; ⬜ Nextendo (owner's go-ahead, app registration, sign-in)

- 🔄 **Releases (melee-pc style, owner-approved)**: per-platform launcher + recompiler, no game code
  (`recomp/README.md` "Player packages and the setup screen", DECISIONS.md "How it's built")
  - ✅ **Setup screen** in the runtime (`runtime/ui/launcher.mm`, SDL3 + ImGui, Metal / Vulkan) in place of the game
    until it's set up: the dump (NSP / XCI or extracted folders), prod.keys + firmware (suggested from Ryujinx /
    yuzu), optional SMM1 dump; then the v3.0.3 check + layout (`games/smm2/setup.py --source`), fonts and system
    data (`sysdata.py`), the first-launch recompile with progress (`runtime/gamebuild.c`, bundled Python + zig),
    and the game in the same window. Errors per step with what to do; keys read in place, values never shown or
    logged (hactool's output masked). Back to it: Quit to Start Screen, Mods > Game files, `--setup`
  - ✅ **Known-good v3.0.3 table** (`games/smm2/versions.py`): build ids, SHA-256 of the 4 ExeFS files, title
    versions (3.0.3 = update v458752), program NCA ids - hashes of the owner's files only
  - ✅ **Update check**: GitHub's latest release vs the build's tag, notice + link only (setup screen and the
    overlay menu's Mods tab, "Check for updates" switch); `RECOMP_UPDATE_TAGS=v0.2.0` shows it offline
  - ✅ **Packages** (`make app-macos`, `make TARGET=... package`; each checked by `scripts/check_package.py`,
    which fails on anything from a dump - names, magics, the known ExeFS hashes, key lines, recompiled code):
    macOS `.app` 320 MB → zip 96 MB (macOS 15+, ad hoc signed); Windows zip 113 MB (425 MB);
    Linux x86-64 tar.gz 113 MB (373 MB); Linux arm64 tar.gz 104 MB (341 MB). Runtime exes: macOS 2.6 MB, Windows 2.7 MB,
    Linux 13.7 / 14.3 MB; the bulk is zig (241-308 MB unpacked) and Python (73-115 MB)
  - ✅ **Release workflow** `.github/workflows/release.yml`: only on `v*` tags; Windows + Linux cross-built in a
    Debian 13 container, macOS on a macOS runner, every package re-checked, attached to the release (a draft if
    none) - ⬜ not run yet (no tag)
  - ✅ **macOS end to end** (2026-10-19, the .app from `make app-macos`, a fresh data folder): the setup screen
    filled in with the owner's extracted dump folders (keys and firmware found in Ryujinx's folder by the screen
    itself, SMM1 dump given) → layout + v3.0.3 check (6 s; build ids and hashes match, RomFS 3,805 files) → fonts
    and system data from the firmware with the bundled hactool (9 s) → recompile with the bundled Python + zig
    (41 min, 8 jobs, the machine loaded by other agents) → the game in the same window → title screen
    (seed 4, frame 2400, "Ver. 3.0.3"). No 32-hex value in any log. Captures (private):
    `recomp/private/evidence/releases/` (setup screen, steps, progress, title). The first start of a freshly signed
    .app waited ~5 min in dyld while macOS checked the bundle (once per build)
  - 🔄 **Where this stands (2026-10-19, paused at the owner's call):** launcher flow ~90 % (NSP / XCI extraction
    through the screen untested end to end: the owner's NSPs' metadata, tickets and versions read correctly with
    the bundled hactool, the extraction itself not run - ~6 GB); packages ~90 % (all four build and pass the check;
    none run on Windows / Linux); release workflow ~80 % (written, YAML checked, never run: needs a tag); docs ~95 %
  - ⬜ Windows / Linux packages not run on their systems yet; Linux AppImage (bundled SDL3); notarized macOS app
    (needs a Developer ID); Android / iOS stay build-it-yourself (iOS: a desktop build-sign-install helper)

- ✅ **Bug (owner, 2026-10-19; closed 2026-10-22: the owner found the timing fine in play, Next features item 11):** sound effects when opening a course and during course loading are
  slightly off (timing). Probably tied to the SLink sound trigger table not being decoded (sounds are
  placed by name, not frame-exact). 🔄 2026-10-21: SMM1's SLink2DB (XLNK v0x15) partly decoded: users, keys,
  actions, frame triggers and the parameter names (notes in `research/specs/smm1-slink2db.md`, private). The 100
  Mario card has no frame triggers. Its walk / jump sounds are keys its code calls, and the walk key picks
  SE_walk80 / 60 / 40 / 20 / 10 / 0, apparently by how many Marios walk. The mod plays one footstep sound for all.
  Left: the value table (each key's Delay / Duration), the selection rule, and the card code's call times.
  Owner (2026-10-21): it's the SMM1 map. Selecting a level makes the sound, then the animation happens. Found: the
  wipe's sound (M3_Wipe) played at the start of Mario's Go pose, about two seconds before the iris wipe it belongs
  to. Moved to the wipe's start (`mods/smm1/screens.c` P_FADE). Not yet heard in game.
- ⬜ **100 Mario polish (owner's test, 2026-10-20: "overall it's not bad"):**
  - ✅ The SMM1 screens (select, map, card) were louder than SMM2's (owner, 2026-10-20). Fixed 2026-10-21. Each
    SMM1 sound plays at its own `.bameta` volume (0.787; the select's tracks 0.55-0.91 on top). The level is set
    against SMM2's mix: `SMM1_MIX` 0.3 in `mods/smm1/smm1_audio.c`. Measured with `LARECOMP_AUDIO_WAV` on macOS:
    the select's music went from -27 dB RMS to -37.7 dB, against -38.0 dB for SMM2's Endless menu and Course World.
  - ✅ Leaving Challenge Modes still flashes the other mode's screen for a moment. For example, closing it from
    100 Mario shows Endless for about a second. The ZL / ZR switch itself is much better now. Cause: on B the
    select faded out in 32 frames, but the B press on SMM2's Endless menu underneath only comes after the
    player's B is released (`DRV_BACK`), and that menu then slides away. Done (2026-10-21, not yet run): the
    select stays until the menu's Disappear has run and stopped (`hundred_menu_left`; 4 s at most), then fades
    over Course World. Checked in game 2026-10-21 (macOS, frames every 6 around B): the select stays 45 frames
    while the menu leaves, then cross-fades into Course World; the Endless menu is never seen. Left: leaving from
    SMM2's Endless side (owner to say if that one flashes too).
  - 🔄 The transition from 100 Mario's map / card into the course isn't exact yet, and lags slightly. Measured
    2026-10-21 (macOS, two New Game runs): the lag is shaders compiling the first time a course of a new style /
    theme is seen. As the card gives way to the course, 36-49 programs compile (one took 426 ms, 49 together
    9.3 s of compiler time). The first course frame waits up to 0.4-0.7 s, then one 1.6 s gap with a black
    frame. Once seen, a shader is in Metal's cache and the precompiled set, so it happens once per machine per
    style / theme. A smaller 0.57 s stall falls on the black between the iris and the card (the scene switch),
    where it isn't seen. Measured how much earlier they could compile (`LARECOMP_PROG_STATS=1`; 35 new
    programs at one course start): 34 use the default key, so a guess would be right. But the game sets their
    shaders only shortly before the first draw: 6 under 0.1 s, 27 in 0.1-1 s, 2 over 1 s. Compiling at that
    point would shorten the hitch by about half, not remove it. Left as is (once per machine per style /
    theme). A full fix would compile the game's shaders well ahead (at setup or at the title), which is a
    bigger project. The rest of the animation still needs the
    owner's eye against SMM1 / SMM2.
- 🔄 **Mac app won't start the game; release zip lacks `smm1_extras.py` (owner, 2026-10-20 / 21):** one cause.
  Every v0.2.0 package left out `gamelib-src/mods/smm1_extras.py` (`tools/package_lib.sh` copied only
  `mods/patches.py`), so the first-launch recompile stopped at once. The owner's Mac app log shows it: steps 1-2
  passed with their NSPs, keys and firmware, then `generator (main) failed: FileNotFoundError: ...
  gamelib-src/mods/smm1_extras.py`. Fixed (2026-10-21): the packages copy every `mods/*.py`, and
  `scripts/check_package.py` now fails a package that lacks a file the recompile reads (it flags the installed
  v0.2.0 app). Also, that app's data folder is a link to the external SSD, so with the SSD unplugged it can't
  start: the error now says the link points to a folder that isn't there. ⏳ Needs a new release (v0.2.1) to reach
  the owner's Mac.
- 🔄 **Windows setup: reading the NSP fails with "no output" (owner, 2026-10-21, v0.2.0):** step 1 stopped at
  "Reading Super Mario Maker 2 [01009B90006DC000][v0].nsp failed: no output", and blamed `prod.keys`. The bundled
  hactool printed nothing and failed. Its exe is normal: standard UCRT DLLs and a 16 MB stack; the largest frame
  is `main` at 257 KB. So it most likely crashed. The cause is not known yet (no Windows machine here). Done
  (2026-10-21): a silent hactool failure now says "hactool stopped without a message", with the exit code
  decoded (access violation, missing DLL and so on), and no longer blames the keys; the exit code goes to the
  setup log. ⏳ Needs the owner to run hactool once by hand on Windows, or the new message from v0.2.1.
- 🔄 **3D World rendering on the iPad (owner's screenshot, 2026-10-20; 3D World ground course, Metal):**
  - 🔄 Mario's model has a grainy, speckled look across his face and overalls, like noise or broken dithering.
    **Found and fixed 2026-10-22 (Mac; iPad still to see):** reproduced on the Mac in windows whose render scale
    leaves the game's targets at fractional sizes: spots at 1.075, 1.43 and 1.45, none at 1.2, 1.25, 1.4, 1.5,
    1.6, 1.8, 2 (the iPad renders at 1.43). The earlier "smooth at 1.43 on the Mac" check had the image scaled down
    to a small window, which hid them. The automatic render scale now goes down to a multiple of 0.2 on each axis
    (`runtime/nvn/output_scale.c`), so 1920x1080, 1600x900 and their halves and quarters all land on whole pixels;
    the same windows are then clean. Explicit test scales (`LARECOMP_SCALE`) stay as given.
    **The iPad's own speckle is a different bug and is still there** (checked on the iPad 2026-10-22: it now renders
    at 1.4, the heavy cyan speckle remains). Narrowed down with per-draw dumps (`LARECOMP_DEBUG_EACH_DRAW=1` plus the
    new `LARECOMP_DEBUG_DRAW_FS=<fs,...>` filter, iPad vs Mac, `SMM2_SEED=4`, title frame 2386, 2752x2064): Mario's
    colour layer is drawn the same on both (only a pixel or two of pose difference), and the scene has a Mario-shaped
    hole until the composite draw with fragment program 72711e4500 (draw ~529 of the frame) lays the character layer
    over it; the speckle is in the scene right after that. Next: dump the iPad's image after that draw (the run was
    paused at that point), then the translated shader of 72711e4500 and what it reads (the RG16F depth layer).
    2026-10-22, later: the composite (72711e4500, `LARECOMP_SAVE_MSL=1` saves it) samples the character depth layer
    and discards where it reads exactly 1.0 (nothing drawn), then compares the scene's linearised depth (through a
    MUFU reciprocal) with Mario's own (`r9 < -r4`). Behind Mario the two are nearly equal, so the compare sits on a
    knife edge: a precision difference on the M5 would flip it pixel by pixel, as the speckle does. Ruled out on the
    iPad: depth textures sampled without filtering (an experiment, no change; removed). In progress: precise
    reciprocal / rsqrt for MUFU (`LARECOMP_PRECISE_MUFU`, stashed as `git stash` "iPad 3D World: ..."; it compiles on
    the Mac; the iPad run reached no screenshot in its 150 s, likely recompiling every shader, so not yet seen).
    (The iPad has the v0.4.4 release since 2026-10-22, its `recomp-test.env` cleared; this needs the iPad again.)
    It's not how he looks on the console. Compare the same frame with Ryujinx; suspects are his shading or AO
    pass and texture or sampler state on the iPad's GPU. The Vulkan NaN blur at his eyes (line 98) may be related.
    Checked 2026-10-21: on the Mac, Mario on the 3D World title is smooth at render scale 1.0 and at 1.43 (the
    iPad's). The iPad uses the GPU's own BC textures (no software decode in its log). MetalFX ran there only on
    frame 0 (1920x1080 -> 2752x1548), before the 1.43 render scale applied. So the grain is not those, and it is
    something about the iPad. Reproduced 2026-10-21 on the iPad itself (v0.3.0, `recomp-test.env`: `SMM2_SEED=4`,
    `LARECOMP_HOST_INPUT=2386:shot:...`; the app's Documents needs a `build/` folder): Mario on the 3D World title
    is covered in cyan / blue speckle, and the same frame on the Mac is clean. The scenery and the Koopa match. The
    speckle pattern changes from run to run. Ruled out on the iPad: BC decoding (`RECOMP_BC_SOFT=1`: same
    speckle) and Maxwell's FMZ multiply (now translated as the hardware does it, 0 x anything = 0; same speckle).
    Also ruled out on the iPad (2026-10-21, `recomp-test.env` switches): zero-cleared new textures
    (`LARECOMP_ZERO_TEXTURES=1`, the frame stays byte-identical), IEEE maths in the shaders (`LARECOMP_SAFE_MATH=1`),
    mip levels (`LARECOMP_LOD_MAX=0`: the scenery changes, Mario's speckle doesn't). Positions are already invariant.
    The frame is deterministic. The speckle shows what's behind Mario (sky, water), so his pixels are dropped. Lead:
    3D World Mario is drawn into a character layer (RG11B10F + RG16F with his view depth, cleared to 1.0), then a
    composite discards where that depth says "nothing" or loses to the scene depth (bring-up notes "Linux arm64").
    Lavapipe shows NaNs in the same composite (his eyes). Next: an Xcode GPU capture on the iPad (needs it local,
    not over Tailscale) of the character layer's RG16F target and the composite's discard.
  - ⬜ The water in the background looks a bit too shiny. The 3D World liquid surfaces already differ most from
    Ryujinx (castle lava 2.28 % px > 24), so check specular and reflection on liquids.

- 🔄 **Vulkan fixes found by the SM35 port (2026-10-21; (1) ported on branch `b5-dev`, compiles, Metal output
  unchanged, not yet run on a Vulkan device):** (1) shader global memory is one storage
  buffer over the 3.2 GB heap, but a GPU only reaches `maxStorageBufferRange` of it (128 MB on lavapipe and Adreno,
  1-2 GB on some desktop GPUs; reads past that give 0): SM35 uses bufferDeviceAddress where available
  (sm35recomp e292dce). (2) without host memory import (Android emulator, many phones) global memory reads give 0:
  SM35 copies each draw's bound storage buffers into per-draw memory (3fce300). (3) Android 11+ can't read files
  adb pushed into Android/data/<pkg>: SM35 falls back to the internal folder (a5e5395). Likely behind SMM2's
  Vulkan-only differences (3D World Mario on lavapipe); port when Windows / Linux / Android get attention.

### GitHub issues (triaged 2026-10-22; owner: no replies needed, fix what they reveal)
Release times (UTC, 2026-10-21): v0.3.0 06:51, v0.3.1 09:23, v0.3.2 12:58. #9 (prod.keys legal): the owner's, no action.
Setup failures first, since they block new players:
- ✅ (v0.4.1) **"hactool stopped without a message (exit code 1)" on the cnmt NCA** (#10, #17, a comment on #7;
  "no output" in v0.2.1: #4, #6). hactool did say why, but its message mentions keys ("Invalid NCA header! Are keys
  correct?", "Key (...) must be 32 hex digits!"), and `masked()` drops every line with "key" in it, so setup called
  it a crash. Now: a header key that doesn't fit says so (`KEYS`, also in sysdata.py), and our hactool build skips a
  key file line it can't read (a known key whose value is the wrong length, a title key listed twice) instead of
  exiting on it, without printing the value (`tools/build_hactool.sh`; checked with a broken key file on macOS and
  the Windows build under Wine). Emulators' key files often have such a line, which is why "it works in Ryujinx".
- ✅ (v0.4.1) **Setup copied and linked its own last result onto itself** when the chosen folder held the data
  folder (on Windows the program's folder is the data folder): the scan found `extracted/exefs` and
  `extracted/romfs` as sources. It now skips setup's own output. Likely #11 ("keep the game files out of the
  program's folder" fixed it for two players); its crash after setup still needs a log.
- ✅ (v0.4.1) **Windows: "vkCmdBeginRendering not found in vulkan-1.dll" before anything shows** (#15): the exe
  imported 17 Vulkan 1.2 / 1.3 commands (and VMA two more), which an older Vulkan loader doesn't have, so Windows
  refused to start it. They're now fetched from the device (`vk.h` `VK_LATE_FUNCS`, VMA's dynamic functions); the
  cross-built exe imports no 1.2 / 1.3 command. A driver without Vulkan 1.3 still can't run it, but now says so.
- 🔄 **"The game code couldn't be prepared" with disk space free** (#5, #12): the generator failed. Its main module
  peaks at ~3.2 GB of memory with the sdk one beside it, and the launcher started up to (RAM − 2) compilers of
  ~1 GB each at the same time. Now (RAM − 6), a MemoryError is named, and the hint says disk space *and* memory.
  Needs the reporter's `smm2recomp.log` (the generator's last line is in it) to confirm.
- 🔄 **Crash after setup** (#11, v0.3.2): see the self-copy fix above; needs `smm2recomp.log` if it still happens.
- ⬜ **Firmware 20.0.0+ "shared fonts not found" / "No system titles could be read"** (#16; 18.0.0 works with the
  same keys): not a hactool limit, since the owner's firmware 21.0.0 sets up fine on v0.4.0. Needs the setup log and
  where the firmware came from (Ryujinx install, Eden, a firmware zip).
- ✅ **Course World bounces to the title, "create a Mii"** (#14, #13's second half): fixed in v0.4.0.
- ✅ **Applying the update crashed (access violation)** (#7, v0.3.1): fixed in v0.3.2.
- ⬜ **Steam Deck runs slowly** (#13, v0.3.2): needs the frame rate and render scale; try a lower render scale.
- ✅ **PR #8 merged** (2026-10-22, x86-64-v2 baseline, gamebuild.c builds v3 where the CPU has AVX2; in v0.4.1).
  Follow-up, minor: the game code's id comes from the v2 flags, so a data folder moved from an AVX2 machine to an
  older CPU keeps its v3 build (illegal instruction). Put the chosen level in the id when it matters.
- ✅ **100 Mario run start: the Peach-carried-away demo** (owner, 2026-10-22: "on Normal the animation seems to be
  missing; Easy showed it"): the SMM1 layout player kept at most 48 animations per layout and dropped any more
  without a word. A world map on the select screen holds its dots, Mario, the castles and the beat bounce on Course
  World's music, so on the 16-dot maps (Normal, Expert) the run's Telop and MoveKuribo never started: no banner,
  no Goombas, Peach already in Bowser's castle. Now 160, with a log line if it ever fills. Checked in game for all
  four difficulties (select -> New Game, music bounce on): banner, Goombas carrying Peach, Peach on the castle.
  Offline it had always played (no music there, so no bounce). Super Expert's banner text ran past the window
  (680 wide in a 600 box): SMM1 draws message text in the system font (`FontInfo.byml`: MessageFont is CafeStd),
  narrower than our bitmap stand-in, so a line wider than its box is now narrowed to fit (`lyt_player.c`). Not
  compared in Cemu this round.
- ✅ **100 Mario ending, each difficulty** (owner, 2026-10-22: "Normal didn't show the Peach animation; Easy did"):
  Peach's and Toad's looping wait pattern stopped on its last key after one loop (the rescue draws it on the
  scene's clock), so NSMBU / 3D World Peach froze after 39 frames, SMW's blink after its first, while Easy's Toad
  (80 frames) still moved for longer. `bfres_texpat_at` now wraps looping patterns. Checked offline
  (`test_smm1ui --flow ending:<d>:0:<style>`) for all four difficulties x five styles: Toad on Easy, Peach on the
  rest, the Thank-you line by difficulty (Easy row 1, Normal 2, Expert / Super Expert 3), the credits roll; in game
  (Normal, NSMBU) Peach and her line at 60 fps. Music is chosen by difficulty and style as in the spec (not
  listened to). Not compared in Cemu this round.

### Owner's v0.4.2 reports (2026-10-22: "100 Mario is working great" apart from these)
- ✅ **A rare 100 Mario hang after a death** (once; closed 2026-10-22 with the watchdogs below): after the lives card the fade back into the course never came;
  the game ran on at 60 fps and the menu opened. Not reproduced (a lava course, misses with quick level return on
  and off: each came back). Two ways it could stick are now caught, said and undone: the bridge holding the course
  for the flow (`hundred_flow_ready_to_start`) longer than 10 s, and the flow's black over a course longer than
  10 s (`[hundred] black over the course`); both log the flow's whole state (`hundred_flow_describe`), and each
  miss logs the lives and the flow's state. If it comes back, the log says which.
- ✅ **Course World "Hot" is always the same courses, from 2020**, **course lists freeze at each new page**,
  **thumbnails missing in some cases**: fixed together, see Next features item 6 (live lists, prefetch, busy retry).
- ✅ **Quick level return crashed after a checkpoint with pink / red coins** (owner, 2026-10-22): closed as not
  reproduced (owner, 2026-10-22). Tried: misses at the start and after a checkpoint, a time-up after the checkpoint,
  red coins collected, in Course World and 100 Mario, audio on and off: each came back. The previous session's log
  is now kept (`smm2recomp.previous.log`), so if it recurs the crash's log survives the next start.

- ✅ **The software keyboard could abort the game** (found 2026-10-23 in a scripted upload: ImGui's "Forgot to call
  Render()" from `menu_keyboard_run`): the game thread waiting for the text re-presented the last frame while the
  game's render thread went on presenting, so two threads ran the backend's present and the overlay's ImGui frame at
  once (the window presented 97-105 times a second while the keyboard was up). Now one present at a time, and the
  dialog re-presents only while the game hasn't for 100 ms (`runtime/nvn/objects.c` nvn_represent): 60 a second,
  the text comes back. Test: `LARECOMP_HOST_INPUT=900:kbd:<initial>:0` opens the keyboard from another thread.

### Next features (owner's order, 2026-10-22)
1. ✅ Co-op in Endless / 100 Mario (see the entry under Future): Mods-tab switch; spawn, own controllers, shared lives
   and a clear by either player (then Next and the map) checked in game with a scripted player 2. The ending stays
   Mario's (SMM1's scene has only Mario's models). Not yet tried with a real second controller (owner)
2. ✅ Lanky Mario (SMM1's Weird Mushroom) as an SMM1 mod switch, off by default (2026-10-22, checked in game, `mods/lanky.c`; docs/mods.md,
   DECISIONS 2026-10-22): Slim Mario in the SMB1 row's slot 8 (models made from the SMM1 dump, Luigi recoloured,
   Toad / Toadette in Mario's), +0.5 on computed ground jumps, SMM1's three sounds by SLink key, a hit gives small,
   SMM1's random Weird Mushroom (1 in 20 out of blocks, at most / at least one per 100). Open: the Weird Mushroom's
   own look (`M1_Item_kinokofunny`)
   - with it: **mod triggers** (owner, 2026-10-22): the overlay menu's "Try it now" rows from a small registry
     (`mods/triggers.c`); "Lanky Mario now" registered; greyed out without the switch, outside an SMB1 course and in
     100 Mario / Endless runs. The costume trigger came with item 4
3. ✅ Mid-air shell jumps (finished 2026-10-22: the test course shows the SMM1 jump on, SMM2 off)
4. ✅ Mystery Mushroom costumes (2026-10-22, checked in game, `mods/costume.c`; docs/mods.md, DECISIONS 2026-10-22):
   a Koopa's hit takes the costume off (the Super → small power-down); the taunt (Up standing still: the costume's
   appeal pose 120 updates and its sound, not after the goal); the reward window's lower-right frame fixed (the layout
   player's flipped four-frame window); a new costume at each pickup, put on mid-course by rewriting the loaded
   model's textures (every costume model is one template with other texels); the Mystery Mushroom taken as Super /
   Fire too (SMM1). With it: "Mystery Mushroom costume now" in Try it now, a choice row of every costume. Then
   (2026-10-22): SMM2's jump sound muted under the costume's (the game's own jump sound request; a costume without one
   keeps SMM2's, as do goal / death), the costume stays on through a sub-area pipe and back (checked), and the
   Mystery Mushroom looks like SMM1's ("?" mushroom). The SMM1 sound loader holds every costume's banks (it kept 32
   banks / 512 sounds: costumes past about 16 in a session went silent). Open: its 2 x 2 size and palette icon (the Big Mushroom's)
5. ✅ Edit any course: ✅ the d-pad onto Make, ✅ the upload credit, ✅ Edit on the course card (Download again);
   ✅ the card's text (Make, the game's own word, 2026-10-22), ✅ Endless / 100 Mario "save for later" (Played
   Courses → Download → Make), ✅ an uploaded course's picture
6. ✅ Course World thumbnails (2026-10-22): they were blank when TGRCode was busy (it allows about one request per
   5-7 s): the server fetched in reverse order, slept through waiting clients and dropped a busy picture for good.
   Now the first pictures of Hot / New / Popular are fetched ahead, the page's and the next page's while browsing,
   and a busy one is asked for again; Hot = Popular's latest uploads first; live lists from TGRCode (`lists.rs`)
7. ✅ Super Worlds (2026-10-22; `recomp/docs/mods.md` "Super Worlds",
   `games/smm2/mods/superworld.c`, server `superworlds.rs`; evidence `private/smm2/evidence/mods/superworlds/`)
   - ✅ server: a pool from TGRCode's `/get_super_worlds` (grows ten random worlds an answer, kept on disk, refreshed at
     most every 10 minutes while a client browses), a world's details and map fetched once and kept, its courses
     listed (fetched from TGRCode when played); progress per player; uploads of our players' own worlds, delete; tests
   - ✅ in game: config value 37 on; Course World > Super Worlds lists TGRCode's worlds (name, worlds, courses, planet);
     Play: the world's map, decrypted by the game's own converter (lives, roads, castles), Mario walks it; a course
     point downloads its course and plays it (an SMB3 and a 3D World course seen); the pause menu shows the course's
     name, world number and maker
   - ✅ fixed on the way: GetCourses skipped the first id of its list (a world's course start asks for one: the game
     then called the course corrupt)
   - ✅ the title names the owner, "SUPER <maker> WORLD" with their Mii (2026-10-23): the game takes another maker's
     name from its own copy of the world's owner block, but only when the save's player name (the Mii the player
     picks for Course World) isn't the default " "; our Mii database is empty, so it always was and every world read
     "SUPER Player WORLD". The mod holds the account's nickname there for the length of the title's set-up
   - ✅ seen in game (2026-10-23, local test server, a 1-world 2-course world of a second test maker): a course
     cleared saves the progress (record status 2, the course's data id, the clear bits); the castle course clears
     the world ("WORLD CLEAR!", the ending, status 1 with `clear` 2); Played Worlds in My Info lists the world, and
     its entry opens the maker's page with their world (Play works); a player's own uploaded world shows in My Info
     and the Worldbot ("Super World uploaded.")
   - ✅ fixed: **Continue** never came up. The game resumes a world from the online record only when the record's
     own status is 2 (main+0x148eae0); the mod sent the save's kind (+0x1b17e8: 1 a course, 2 the world) as the
     status. It now sends both (`status`, `clear`; the server keeps `clear` at its highest). Checked: Play again
     after a course gives New Game / Continue, and Continue puts Mario back on the cleared course point
   - ⬜ upload and delete in game not tried: both start from a Worldbot slot (World Maker), which needs courses in
     Coursebot uploaded through the game's clear check; the test save has none (a world uploaded through the API
     shows as uploaded, but the game offers no delete for a world that isn't in a slot)
   - ⬜ seen once: the Worldbot stopped on a heap check (sdk trap from a map object's destructor, main+0x1465f40 /
     +0x1465810, called from main+0x171a360) after its first pigeon message, with the API-made world uploaded; not
     seen again in three more visits (with and without it)
   - ⬜ thumbnails stay the game's placeholder planets (TGRCode's are on Nintendo's CDN; our own uploads' blocks are
     kept by the server but no in-game upload has made one yet)
8. ✅ Disable the buttons that don't work yet (2026-10-22, checked in game): every Course World entry works or answers
   the way SMM2 does when a service is off. Ninji Speedruns showed "A communication error occurred": its two
   requests (event count main+0x14f3980, event list +0x14f3770) are now answered empty, so it shows SMM2's own "No
   active speedruns / No courses found". Super Worlds and Network Play's modes already say SMM2's "This service is
   not active right now" (the application config's switches); Leaderboards opens (empty until our server ranks).
9. ✅ Online multiplayer via Nextendo (2026-10-22: branch `nextendo` merged into main; new, not yet tested on
   hardware): main merged into it, SM35's gen.py jump-table fix ported (no change to SMM2's code: function count
   unchanged), regenerated, boots, plays a 100 Mario course with default settings (the backend stays ours unless the
   Online section picks Nextendo), `nextendo_ios.mm` compiles for iOS. Owner to test: a live sign-in (verified
   e-mail, linked Discord) and Network Play with a second player on another connection
10. ✅ iPhone layout (2026-10-22, iPhone 17 Simulator with the Dynamic Island, landscape): the on-screen controller
    keeps clear of the safe area (ZL / L right of the island, the right column inset to match), 60 fps. Not yet on the
    owner's iPhone 12 mini. The Simulator build needed two Simulator-only guards (no timed presents / presented
    handlers, no binary-archive save); `LARECOMP_TOUCHPAD_SHOW=1` shows the controller with a gamepad connected

11. ✅ Exact sound timing: the owner found it fine in play (2026-10-22); no change made, nothing concrete to fix

### Future (owner's backlog, not scheduled)
- 🔄 **120 fps output by frame generation** (owner, 2026-10-19): built 2026-10-20 (Video tab, `frame_gen`,
  off by default; our own interpolation for Metal and Vulkan, not MetalFX's; game logic stays 60 Hz). Owner on
  the iPad: "doesn't look amazing at this stage". Left: picture quality, the iPad's shown-frames count, a real
  120 Hz display. Spec: `docs/superpowers/specs/2026-10-20-frame-generation-design.md`
- ✅ **Quick level return** (owner, 2026-10-21; built 2026-10-21): a Mods-tab switch, off by default. After a miss the
  game's own miss, lives card and restart are fast-forwarded unseen, so lives still count and nothing changes when
  it's off. Coursebot 1.6 s instead of ~4.6, 100 Mario 2.3 s instead of 9.7, test play's return to the editor about
  0.5 s. Owner: skip the lives card, and apply it in test play. `recomp/docs/mods.md`, DECISIONS 2026-10-21
- ✅ **100 Mario New Game on SMM2's menu** (2026-10-21): the bridge's presses could back out to Course World (then
  the map's Play presses opened the Mii outfit editor) when the run had no clears yet or the difficulty needed no
  change. It now reads whether SMM2's menu shows Continue (the menu's record: lives and a clear). Checked both ways
- ✅ **Setup from NSPs on every OS** (testers' Windows reports, 2026-10-21): applying the 3.0.3 update was broken
  everywhere. The base RomFS was handed over in the wrong coordinates, and a memcpy overlap in hactool crashed it on
  Windows. Fixed; the result is byte-identical to a known-good 3.0.3 extraction (macOS, and the Windows hactool
  under Wine). An open report: one tester's firmware step reads no NCA at all ("117 NCAs unreadable"). The owner's
  firmware works under Wine, so that's their keys or a newer firmware
- 🔄 **Local multiplayer input, players 1-4** (owner, 2026-10-20; built 2026-10-21): every further controller is
  the next player, Npad No. 2..4, through the same bindings (`runtime/hle/input.c`). Player 1 keeps the keyboard,
  the on-screen controller and the first controller. `GetNpadState(s)` and `GetNpadStyleSet` report players 2-4
  while their controller is there. The style set update events fire when a controller comes or goes. The controller
  support applet (Play Together's "how many players") is answered at once with the players there are.
  `LARECOMP_INPUT2` scripts a player 2 for tests. Checked in game (macOS, scripted player 2): a downloaded course's
  Play Together shows Mario and Luigi on the card, and in the course Luigi runs on player 2's input while Mario
  stands. Left: real second / third / fourth controllers (none here), and controllers connected mid-game.
  2026-10-22: a Controls-tab switch "Controllers start at player 2" (`pads_from_p2` in `smm2.ini`, off by default):
  the keyboard and the on-screen controls are player 1 and every controller is the next player, so a keyboard and
  one controller (the owner's PS5 pad) play two-player. Changing it lets go of every controller and takes them again
  under the new rule. Seen in the menu; with real controllers: not yet tried
- 🔄 **Endless / 100 Mario in local co-op** (owner, 2026-10-20; rules decided 2026-10-21: as SMM2's local
  multiplayer does, lives shared, a life lost when all players are down, cleared when anyone reaches the goal).
  Groundwork 2026-10-21:
  - Play Together is the Coursebot's course start main+0x16e3d60 (scene change reason 6). It reads the player count
    from `*(main+0x2a66498) + 0x494` (2 in a real Play Together course, 0 on the title course) and a per-player
    character list (+0x330).
  - The course set-up (main+0x16dfe..) takes that count only when a mode field (`[x24 + 0x880] -> +0x28 -> +0x18`)
    is 1; retries keep the last course's count; anything else, Endless included, stores one player (main+0x16dffb4).
  - A hook point there (`patches.py`, `hooks.c`; `RECOMP_COOP_TEST=<n>`) writes n players. In a 100 Mario course
    with a scripted player 2 the count reads 2 in the course, but only Mario spawns: the second player needs more
    than the count. 2026-10-22: the set-up's two per-player records (0x100 bytes each, at +0x18) are byte for byte
    the same as a real Play Together course's, so the data isn't what's missing: Endless's play scene doesn't create
    a second player actor. Next: find where the play scene creates players (Ghidra project copy:
    /Volumes/SSD/smm-work/ghidra/agents/edit-smm2, script research/decomp/smw-goal/scripts/Multi.java; JAVA_HOME is
    Homebrew's openjdk@21). Not in v0.4.0.
  - 2026-10-22, the spawn found (function profile `LARECOMP_FNPROF` of a Play Together course start against a 100
    Mario one, then `LARECOMP_FNPROF_BT` callers): players are created by the play scene's set-up (main+0x16c2c70,
    called from the scene manager main+0x178ed80) in a loop at main+0x16cd600 over a list (`[x25 + 0x28]` entries
    at `[x25 + 0x30]`, each made through main+0xe443c0 -> ... -> the player constructor main+0x1587d10): one entry
    in 100 Mario, two in Play Together. Right after, main+0x16cd670 compares a count (`[main+0x2c01cf0] + 0x98`)
    with a 4-bit "players present" mask (`+0x54`): likely the multiplayer session. Next: where that list is filled
    (and whether the session's mask must hold the second player), then fill it for co-op.
  - 2026-10-22, correction and the real spot: the list at `[*(main+0x2a6bcd0)] + 0x28 / +0x30` is the course's two
    areas (main and sub-area: always two entries, 0x188 bytes each, made in the play scene set-up main+0x16aac20),
    not players. Players are actors (profile "Player", main+0x2a56388) created by main+0xe581d0, which loops over a
    player count (w22, player index in w23) chosen by the play mode `[*(main+0x2a6bcd0)] + 0xe0`: 1 takes the course
    set-up's count (`[*(main+0x2a66fb8)] + 0x40`; 2 in one mode), 2 the online session's (`[*(main+0x2c01cf0)] +
    0x98`), anything else exactly one player. A hook point on that mode load (main+0xe59614) forces 1 under
    `RECOMP_COOP_TEST` to test spawning player 2 in Endless.
  - 2026-10-22, **Luigi in a 100 Mario course, on player 2's controller** (test only, `RECOMP_COOP_TEST=2` with
    `LARECOMP_INPUT2`): two hook points. (1) The course's play mode, stored by the player spawn at main+0xe58328
    (`[*(main+0x2a6bcd0)] + 0xe0`: 0 one player, as in Endless and the title course; 1 local multiplayer, as in Play
    Together; 2 online), is set to 1 with the set-up count 2: Luigi spawns next to Mario and each player is on their
    own controller (checked both ways: player 1 moves only Mario, player 2 only Luigi).
    (2) The controllers' poll (sead's Npad device, main+0x263d90) reads Npads 0 .. `[x0 + 0x28]`; Endless leaves it
    at 1, now 2 (hook at +0x263db0), so the game reads player 2's controller and Luigi walks and jumps on it.
    SMM2's own lives card already shows Mario and Luigi side by side.
  - 2026-10-22, **the Mods-tab switch** "Co-op in Endless and 100 Mario" (`coop`, `RECOMP_COOP`; off by default;
    `mods/coop.c`, DECISIONS): with it on and two or more players' controllers connected, an Endless / 100 Mario
    course is started in local multiplayer (3D World courses stay single player: SMM2 has no multiplayer there).
    **Shared lives checked in game** (a one-course test server with a lava course, both players scripted): one
    player down (state 3 at Endless's miss request) costs nothing while the other stands; both down costs one life
    (100 -> 99 -> 98) and SMM2's retry brings both back. Left: a clear by either player, the ending with all
    players, a real second controller.
  - Owner, 2026-10-22: the 100 Mario ending shows all the players in the rescue scene if that's reasonable, else
    whoever cleared the last course; the lives card shows each player ("give that a shot").
- ✅ **User report (2026-10-22, platform not stated, maybe Windows): Course World says "make an ID" and goes back;
  only Story mode is unlocked.** Fixed 2026-10-22: reproduced with an empty data folder on the Mac. Without an
  `smm2mods.ini` the desktop builds left the online mod off (only iOS turned it on), so Course World ran SMM2's own
  sign-in ("let's create a Mii and a name"), waited and fell back to the title. Every platform now defaults to the
  online mod with TGRCode courses through the built-in server (`games/smm2/mods/settings.c`); the same empty folder
  then enters Course World's first-visit pages with the default Mii "Player". A new save offers Story Mode, Course
  World and Coursebot from the start, so "only Story" was this bounce. Report text: The owner doesn't see it on the iPad. Likely a fresh-save / first-run case rather
  than an OS one: a new player meets SMM2's first-time setup (maker profile / Mii / user creation, Course World's
  ID step), and the am / mii / acc answers may end it at once. Check by reproducing with an empty save / data folder
  first; also check what a new save unlocks. Not started (paused; the owner sets the order).
  A second report, likely the same bug (Windows or Mac): "click Course World, it goes in and immediately back to the
  main screen". When reproducing with an empty save, also check: which version they're on (v0.3.0 had no built-in
  server, though that showed "communication error"); the `courses=` / server settings a fresh data folder gets; and
  whether Course World's sign-in (our offline answer) fails without a saved account. If it isn't fixed by v0.4.0,
  the release notes should ask for the version and `smm2recomp.log`.
- ✅ **OpenCourse World as a course source** (owner, 2026-10-22; was moved to "possible features" the same day,
  then built and working the same day): https://opencourse.world/, a replacement online service for SMM2 3.0.2 /
  3.0.3 on Switch (Atmosphere) and Ryujinx. The player signs in, accepts its EULA and is given a domain and a
  personal token; the game's own online code then talks to OCW's server. Its web API
  (server.opencourse.world/api/...) carries course metadata but no course download, so the way in was the game's own
  network stack, not a TGRCode-style HTTP source.
  **Working (owner, macOS, 2026-10-22): browsing, playing, likes, comments and uploads.** This is the first time the
  game's **own** online code has reached a real service through the recomp — `local-nex` had only ever run against
  `tools/nex_test_server` — so it is evidence for the Nextendo route as well.
  Built from the owner's own console mod (`_vendor/ocw-client-mod`): two of its ten patches were needed, the domain
  string at `main+0x22cc307` (written at boot) and the WebSocket handshake at `main+0xAF560` (a generator patch, so
  the host builds the header with the player's token). The other eight stub `nn::account` calls that
  `services/acc.c` already answers. Backend `ocw` in `runtime/net/netcfg.c`, mod in `games/smm2/mods/ocw.c`,
  settings in the Mods tab (domain, address, token) written to `ocw-config.json`. Needs both `backend=ocw` and a
  usable config before anything changes; deny-by-default stands (one name resolves, only its addresses are
  reachable, re-resolved every 5 min) and OCW's certificate is not checked (owner's call, which also makes a
  self-hosted OCW with a self-signed certificate work). TLS stays a 1.2 client: OCW accepts 1.2 (Let's Encrypt ISRG
  Root X2). Two things had to be fixed on the way:
  - **The Mii.** `services/mii.c` held an empty database (`GetCount` 0), so Course World asked for a Mii the player
    had no way to make. It now holds the one default Mii the runtime already had, through `Get` / `Get1`; `Get2` /
    `Get3` (StoreData, the packed record with its checksum) still answer "none" and say so in the log. **Not gated
    on the backend: this is the one change here that every player gets.** Whether it moves the Ryujinx frame
    comparison (71 / 90 courses bit-identical) has not been re-measured — Ryujinx with a user profile has a Mii
    too, so it should move towards the reference, but that is reasoning, not a measurement.
  - **The account.** The profile would not stick and the server did not find it again. `net/account.txt` persists
    with a stable id, so it was not identity churn: it was the local **ID token**, a JWT whose `sub` is the local
    account id, signed with a key OCW cannot check, where the console mod sends none at all. Under `backend=ocw`
    the account now matches the mod exactly (`net_account_shared()`): id `0xCAFE` — the same for every OCW player
    on a console, which is itself the proof that OCW identifies players by the handshake token — and an empty ID
    token.
  Decision and trade-offs in `DECISIONS.md` (2026-10-22); plan, diagnosis and what is still open in
  `tasks/todo.md`. Still open: the `backend=ours` no-change check since this landed, and every platform but macOS
  (the same runtime compiles for them, but none has been built with these changes).
- ✅ **Relaxed checks as their own mod** (owner, 2026-10-22: "maybe profanity filter and corrupt course can be their
  own recomp mod"): `games/smm2/mods/relax.c` + `relax.py`, two settings **off by default** — `no_word_filter` (the
  `nn::ngc` text check always passes) and `allow_broken_courses` (the corrupt-course check always passes, so a
  course the game would replace with a blank one is played). Both come from the OCW console mod, which applies them
  unconditionally; here each has its own switch in the Mods tab, so they work with any course server. Each forces
  its whole function to return 1, as the mod does, with the guard before the function's own first instruction; the
  prologues found there (`sub sp, sp, #0x1a0`, `stp x28, x27, [sp, #-0x60]!`) confirm both addresses are function
  entries, so the early return is equivalent. Not yet exercised by hand.
- 🔄 **iPhone: on-screen L and ZL sit behind the notch** (owner, 2026-10-20, iPhone 12 mini; built 2026-10-21: the
  controls are offset by the window's left / right safe-area insets, `SDL_GetWindowSafeArea`, updated on safe-area
  and resize events and on touches; not yet seen on the iPhone): the touch
  controls (`recomp/runtime/ui/touchpad.mm`) don't keep clear of the safe area, so the L / ZL buttons are partly
  under the notch in landscape. Inset them by the window's safe area. The phone reached the title screen and ran
  well, then was killed for memory (jetsam `vm-pageshortage` at ~1.85 GB); owner isn't chasing that for now.
- ✅ **"Lanky Mario" as an SMM1 mod** (owner, 2026-10-21; built 2026-10-22, Next features item 2): SMM1's tall, stretched, glitchy-looking Mario. This is
  most likely the Weird Mushroom ("Weird Mario"), SMB1 style only. Found 2026-10-21: SMM1's name for it is
  `KinokoFunny` (editor sounds in every style, edit DB entries); there is no separate model, so the stretch and
  its jump are likely code (Block.rpx). Notes in `research/specs/smm1-extras-recomp-notes.md` (private). Confirm from the SMM1 files what it is: its
  model and animations, whether it changes the jump or physics, and how it turns up in courses. Then add it behind
  its own switch, next to the Mystery Mushroom costumes (`mods/costume.c`). Not started.
- ✅ **Rename the app** (owner, 2026-10-20; decided 2026-10-21): "Super Mario Maker 2" on desktop (window title,
  setup screen, macOS app "Super Mario Maker 2.app", Linux menu entry), "SMMaker 2" on iOS and Android home screens
  (game.mk `GAME_TITLE` / `GAME_MOBILE_TITLE`). Bundle IDs, data folders, exe names and package file names stay, so
  saves and scripts carry over. From the release after v0.3.1.

### Possible features (not planned)
- **Platform extras** (owner, 2026-10-22: backlog, don't start): a Linux AppImage, a notarised macOS app, a web
  build.

| Platform | Builds | Boots to title | Plays courses | Frames match Ryujinx | Input replays in sync |
|---|---|---|---|---|---|
| macOS (Apple Silicon, Metal) | ✅ | ✅ title rendered, 60 fps | ✅ title course to the goal, Course Maker test play | 🔄 in course: 71 / 90 courses bit-identical (all SMB1/SMB3/SMW), the rest within 20 px except 3D World lava 2.28 % px >24 (title: 2.5-4.8 %) | ✅ 221,189 / 221,189 updates identical (31 sessions) |
| Windows (x64, Vulkan) | ✅ cross-built with zig: exe 2.4 MB + game DLL 186 MB, needs SDL3.dll, libiconv-2.dll, libshaderc_shared.dll, vulkan-1.dll (driver); player zip 85 MB | ⬜ not run (no Windows machine) | ⬜ | ⬜ | ⬜ |
| Linux arm64 (Vulkan) | ✅ cross-built with zig: exe 11.9 MB + game .so 688 MB, needs libSDL3.so.0, libvulkan.so.1, libshaderc.so.1, glibc 2.36; player zip 71 MB | ✅ title rendered (Debian 13 VM, Mesa lavapipe, ~7 fps); package first launch recompiled the game code from the dump (29 min) and booted | ⬜ | 🔄 seed 4 frame 2386 vs macOS same frame: 0.6 % px >24, 0.17 % >96 (before: Mario model missing on Vulkan; now in the scene, with a bloom blur at his eyes); colour-managed vs Ryujinx 2.99 % >24, 0.24 % >96 (macOS 2.35 / 0.11) | ⬜ |
| Linux x64 (Vulkan) | ✅ cross-built with zig: exe 11.3 MB + game .so 856 MB (generated/smm2-c, x86-64-v3), same libraries; player zip 74 MB | ⬜ not run | ⬜ | ⬜ | ⬜ |
| iOS / iPadOS (arm64, Metal, AOT) | ✅ game code built on the Mac from the dump and linked into the app (exe 141 MB; SDL3 static); device app signed with an App ID carrying extended virtual addressing + increased memory limit (`tools/ios_profile.sh`) | ✅ device: iPad Pro 13 M5 (iOS 27.0) through the intro to the title screen and menu, steady 60 fps, no faults; Simulator (iPad Pro 13 M4): title at 60 fps, lighting wrong there (the Simulator's Metal limits: 32-byte colour targets, no cube arrays) | ⬜ on-screen controller drawn, not yet tried by hand | ⬜ | ⬜ |
| Android (arm64, Vulkan, AOT) | ✅ cross-built on macOS with the NDK (r29) for the emulator's arm64 image: game code built from the dump into `libsmm2game.so` (143 MB) + runtime `libmain.so` (14 MB stripped), APK 160 MB without Gradle (`make TARGET=android-arm64 apk`); guest window at 0x800000000 | ✅ Android Emulator only (API 35 arm64, gfxstream over MoltenVK): intro at 13-60 fps, full title screen with its course at 5-9 fps (Mac shared with other jobs); survives home screen and back; device install untested (no device) | 🔄 the title course played with the on-screen controller (stick walks Mario into the Koopa Car, A jumps), 5-9 fps; no menu course yet | ⬜ | ⬜ |
| Web (wasm + WebGPU) | ⬜ feasibility decided after the native ports | | | | |

---

## Phase 1 detail

### Done
- Repo initialised. Public home since 2026-10-19: https://github.com/smm2recomp/smm2recomp (published as
  snapshots; the full history, SMCraft included, is at https://github.com/aksmith-git/MarioMaker). Earlier:
  https://github.com/smcraft-git/smcraft (snapshots;
  2026-10-18). `.gitignore` excludes dumps, keys, extracted data and `research/`.
- Proprietary-content guard (`scripts/check_no_proprietary.py`) + tests + CI workflow + pre-commit hook.
- Toolchain installed and pinned outside the repo — see [docs/TOOLCHAIN.md](docs/TOOLCHAIN.md).
- `tools/inventory.py`: identifies extracted files by magic (through Yaz0/zstd), looks inside SARCs.
- **SMM1** (WUX disc image) decrypted with console common key + disc key:
  - `~/smm-work/extracted/smm1-disc/` — base game v16 (`000500001018DC00`)
  - `~/smm-work/extracted/smm1-update-v208/` — update v208 bundled on the disc
  - Detailed inventories: `research/inventory/` (private)

### SMM1 layout summary (base v16 + update v208)
| Area | Where | Format |
|---|---|---|
| Main executable | `code/Block.rpx` (v208: 9.0 MiB) | Wii U RPX (PPC, big-endian) |
| Course data | `content/Course/*.szs` (152: sample, tutorial, title, new) | Yaz0 → SARC → `.cdt` (SMM1 course) |
| Course thumbnails | `content/CourseThumbnail/*.szs` | Yaz0 → SARC → `.tnl` |
| Tilesets / scenery | `content/Model/M1_Field_*`, `M1_DV_*` etc. per style | Yaz0 → SARC → BFRES |
| Sprites / characters | `content/Model/*_Player_Chara_*`, `Pack/*.pack` | BFRES, SARC packs of Yaz0 SARCs |
| Per-style environments | `content/Pack/{M1,M3,MW,WU}_FieldEnv_{Normal,Dark,Water,Yogan}.pack` | SARC |
| Physics / object params | `Pack/*.pack` → `.params`, `.byaml` | text params, BYML (to be confirmed in Phase 3) |
| Text | `content/Message/*_USen.szs` | Yaz0 → SARC → MSBT |
| Layout / UI | `content/Layout/*.szs` | Yaz0 → SARC → SARC (BFLYT/BFLIM) |
| Music | `content/Sound/**/*.bfstm` (147) | BFSTM |
| SFX | `content/Sound/*.bars` (193), `Sound.pack` | BARS → BFWAV |

Styles: `M1` SMB1, `M3` SMB3, `MW` SMW, `WU` NSMBU. Themes: plain, underground, castle, airship,
water, haunted house.

- **SMM2** from `MarioMaker2.NSP` (owner's dump; v1.0.0, title `01009B90006DC000`, SDK 7.4.0, no update):
  - `~/smm-work/extracted/smm2-nsp/exefs/` — `main` (NSO, 17 MiB), `sdk`, `rtld`, `main.npdm`
  - `~/smm-work/extracted/smm2-romfs/` → symlink to `/Volumes/SSD/smm-work/extracted/smm2-romfs` (2.7 GiB; internal disk is full)
  - `SMM2.xci` is NOT used (it is the `[romstorage.com]` file renamed).
- **Course sample**: `tools/fetch_courses.py` pulled 550 courses into `~/smm-work/courses/mm2_level/`
  from the public `TheGreatRambler/mm2_level` dataset (CC BY-NC-SA 4.0): 25 per style x difficulty
  plus 50 extra SMB1 ground courses. All from June–July 2019 uploads, so they only use v1.0.0 content.
  All 550 decompress to 0x5BFC0 bytes and their headers (timer, style, name) match the metadata.

### SMM2 layout summary (v1.0.0)
| Area | Where | Format |
|---|---|---|
| Main executable | `exefs/main` | NSO (AArch64); Ghidra Switch Loader |
| Built-in courses | `Course/*.szs` (120 story `quest`, 50 `lesson`, 10 `title`) | Yaz0 → SARC → `.bcd` (to confirm in Phase 2 whether encrypted) |
| Online/save courses | (from dataset) | 0x5BFC0-byte decrypted course; on-console `.bcd` is encrypted |
| Tilesets / scenery | `Model/{M1,M3,MW,WU,3W}_Field_<theme>`, `_DV_` (background), `_S_DV_` | zstd → SARC → BFRES (+ BFSHA) |
| Theme animation | `Pack/<style>_FieldAnime_{Normal,Dark,Ice,Snow,Water,Yogan}.pack` | SARC |
| Sprites / objects | `Pack/<style>_Model.pack`, `Static*.pack`, `Model/` | SARC of zstd SARCs → BFRES |
| Params / physics candidates | `Pack/*.pack` → 423 BYML; `Bake/*.bkres` | BYML; SARC (to be mapped in Phase 3) |
| Text | `Message/*.szs` | Yaz0 → SARC → MSBT (3038) + MSBP |
| UI | `Layout/*.szs` | Yaz0 → SARC → SARC (BFLYT etc.) |
| Music & SFX | `Sound/Resource/**` (471 BWAV, 2.1 GiB), `Pack/<style>_bars.pack` | BWAV, BARS |
| Sound delay/env per theme | `Sound/Effect/delay_<style><theme>.byml` | BYML |

Styles: `M1` SMB1, `M3` SMB3, `MW` SMW, `WU` NSMBU, `3W` SM3DW. Themes: plain, underground, castle,
airship, water, hauntedhouse, snow, desert, athletic (sky), woods.

- **SMM2 update v3.0.3** (`Mario Maker2 Updatev3.0.3.nsp`, patch title `01009B90006DC800`, NACP display
  version 3.0.3) applied over the v1.0.0 base:
  - `~/smm-work/extracted/smm2-v303-romfs/` (3801 files, 2.8 GiB; adds `MyWorld/`, +22 models, +3 packs,
    +344 sound files, +123 layouts) and `~/smm-work/extracted/smm2-v303-exefs/` (`main` 20 MiB NSO)
  - ELFs for analysis: `/Volumes/SSD/smm-work/extracted/smm2-v100-main.elf`, `smm2-v303-main.elf`

### Open
- Owner's own saved courses: not yet exported from the Switch; the dataset sample covers Phase 2 for now.
