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
    ✅ "100 Mario Challenge (SMM1)" (on screen in the Mods tab: `evidence/mods/mods_tab.png`); 🔄 "Mystery Mushroom costumes" (`mods/costume.c`: models generated from both dumps, worn as small
    Mario in SMB1 courses; in game: the costume on from the start (Peach, Samus), the pickup (Super Mushroom
    → Block costume, its power-up sound), the goal and death sounds, the reward window after the ending
    (`evidence/reward/006_reward.png`, private), jump / landing / taunt sounds from the player's motion;
    ⬜ SMM2's own jump sound muted under the costume's, the taunt pose, more than one costume per course).
    **Where this stands (WIP, paused 2026-10-19):** in game (cos9, private log): costume worn, Super Mushroom
    pickup keeps it, jump sounds, goal and death sounds play. ⬜ A hit taking the costume off is still unseen:
    every test death so far was a pit (the hook at 0x15a50a4 is in place). ⬜ The taunt fired once right after a
    goal (the Up byte at player +0x560 is [inf]: check it). ⬜ The reward window's lower-right frame looks cut off
    (`evidence/reward/006_reward.png`). **Crash ("Pure virtual function called", WorkerMgr, main+0x4d2f50: a
    queued object destroyed while its request was pending)** seen twice with costumes on (entering Course World;
    a course start). The costume pack was 38 MB against the game's 5 MB (stored zstd blocks); it is now
    compressed (8.4 MB, generator "costumes" v2). Since then 5 of 5 runs with costumes on entered Course World
    and started a course without the crash (before: 2 crashes in about 6 such runs; logs `crash1-3.log`,
    `cos8.log`, `cos9.log`, private). Likely fixed by the smaller pack [inf: consistent, not proven; the SMM1
    audio mix is host-only and touches no guest object]. Paused by the owner: the hit test, the reward frame,
    the sounds; the SMM1 extras (`mods/smm1_extras.c`, all off by default, `recomp/docs/mods.md` "SMM1 extras"):
    ✅ "Overlapping pipes (SMM1)" (one hook in the game's placement check, main+0xfe827c: SMM2 rejects a
    course whose pipes overlap and plays a blank one, and its Course Maker snaps a pipe dragged onto another back;
    with the setting on, both test courses (ours and the pipes of SMM1's sample course 95, read from the SMM1
    dump) load and play, and a pipe dragged onto another stays; with it off, a replay against Ryujinx is
    identical in 606 / 606 updates and the whole player object; both switches on screen in the Mods tab: `evidence/smm1-extras/`); 🔄 "Mid-air shell
    jumps (SMM1)" (hooks in place: SMM1's flat falling stomp window on shells, main+0x1267ce8, and the 5-frame
    instead of 16-frame no-hit window after a mid-air kick off a wall, main+0x1202c88; ⬜ a test course where
    the SMM1 jump works with it on); ⬜ more from research/specs/smm1-extras*
  - 🔄 **Edit any course** (`games/smm2/mods/edit.c`, `recomp/docs/mods.md` "Edit any course"; WIP, paused):
    ✅ Course World's Download into the Coursebot against our server (stand-in thumbnail); ✅ SMM2's Make in the
    Coursebot window of a downloaded course (SMM1's Edit placement), the Course Maker opens with the course, an edit
    saved to My Courses and played (screens private); ✅ typed names intact on Save As (re-tested 2026-10-21);
    ⬜ d-pad cursor onto Make (touch works; the cursor's target list isn't the enable call's, see mods.md),
    ✅ the copy is the player's own (2026-10-21: reset after the Course Maker's reload; saved copy has upload id 0;
    the yellow arrow on cards isn't an upload mark), ✅ the "Based on" record follows Save As (new creation id; checked
    in game 2026-10-21), ⬜ upload credit in game (server `based_on` done and tested),
    ⬜ Edit on the course card / Endless (proposed)
  - 🔄 **Our open-source server everywhere the game would reach Nintendo** (`recomp/docs/mods.md`,
    `games/smm2/mods/world.c`): ✅ Course World's course lists (Hot, Popular, New, detailed search, a
    maker's courses) from `/api/courses`, the course page (likes, plays, clear rate, world record, maker
    name and Mii), download and play in SMM2's own play, the play / clear posted with its time
    (`evidence/mods/world/`); ✅ leaderboards list the server's makers with their maker points
    (`leaderboard_makers.png`); ✅ SMM2's own Endless under the `smm2` ruleset (5 lives, endless, its
    own screens and HUD; an Easy run against the server: `evidence/mods/smm2_endless/`); 🔄 built, not
    yet seen working: likes / boos (the clear screen's hearts took no test input), comments (text),
    uploads from the Course Maker, the ID search, maker pages; ⬜ thumbnails, Super Worlds, Ninji;
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

- ⬜ **Bug (owner, 2026-10-19):** sound effects when opening a course and during course loading are
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
    The iPad still has that test build and a `recomp-test.env` on it.
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
- ⬜ **OpenCourse World as a course source** (owner, 2026-10-22): https://opencourse.world/ selectable as one of the
  course server options (next to our own server and TGRCode, in the Mods tab's course source setting). Looked at
  2026-10-22: OCW is a replacement online service for SMM2 3.0.2 / 3.0.3 on Switch (Atmosphere) and Ryujinx. The
  player signs in (Discord etc.), accepts its EULA, and downloads a mod package plus a personal auth token for the SD
  card; the game's own online code then talks to OCW's server. Its web API (server.opencourse.world/api/...) is
  public for course metadata (latest, uncleared, makers, Ninji leaderboards, one-screen thumbnails) but has no web
  download of course data. So the way in is the game's own network stack (backend local-nex, the Nextendo groundwork)
  pointed at OCW with the player's token, not a TGRCode-style HTTP source. Needs the owner's OCW account to build and
  test; not for v0.4.0.
- 🔄 **iPhone: on-screen L and ZL sit behind the notch** (owner, 2026-10-20, iPhone 12 mini; built 2026-10-21: the
  controls are offset by the window's left / right safe-area insets, `SDL_GetWindowSafeArea`, updated on safe-area
  and resize events and on touches; not yet seen on the iPhone): the touch
  controls (`recomp/runtime/ui/touchpad.mm`) don't keep clear of the safe area, so the L / ZL buttons are partly
  under the notch in landscape. Inset them by the window's safe area. The phone reached the title screen and ran
  well, then was killed for memory (jetsam `vm-pageshortage` at ~1.85 GB); owner isn't chasing that for now.
- ⬜ **"Lanky Mario" as an SMM1 mod** (owner, 2026-10-21): SMM1's tall, stretched, glitchy-looking Mario. This is
  most likely the Weird Mushroom ("Weird Mario"), SMB1 style only. Found 2026-10-21: SMM1's name for it is
  `KinokoFunny` (editor sounds in every style, edit DB entries); there is no separate model, so the stretch and
  its jump are likely code (Block.rpx). Notes in `research/specs/smm1-extras-recomp-notes.md` (private). Confirm from the SMM1 files what it is: its
  model and animations, whether it changes the jump or physics, and how it turns up in courses. Then add it behind
  its own switch, next to the Mystery Mushroom costumes (`mods/costume.c`). Not started.
- ✅ **Rename the app** (owner, 2026-10-20; decided 2026-10-21): "Super Mario Maker 2" on desktop (window title,
  setup screen, macOS app "Super Mario Maker 2.app", Linux menu entry), "SMMaker 2" on iOS and Android home screens
  (game.mk `GAME_TITLE` / `GAME_MOBILE_TITLE`). Bundle IDs, data folders, exe names and package file names stay, so
  saves and scripts carry over. From the release after v0.3.1.

| Platform | Builds | Boots to title | Plays courses | Frames match Ryujinx | Input replays in sync |
|---|---|---|---|---|---|
| macOS (Apple Silicon, Metal) | ✅ | ✅ title rendered, 60 fps | ✅ title course to the goal, Course Maker test play | 🔄 in course: 71 / 90 courses bit-identical (all SMB1/SMB3/SMW), the rest within 20 px except 3D World lava 2.28 % px >24 (title: 2.5-4.8 %) | ✅ 221,189 / 221,189 updates identical (31 sessions) |
| macOS x86-64 (Intel, Metal) | ✅ native build on an Intel Mac (iMac14,2, Haswell): the runtime compiles, the player package builds (`.app` 334 MB, zip 113 MB) and starts (the setup screen opens); the game code uses the portable-C lowering (`generated/smm2-c`, `--simd c`, x86-64-v3), the same one as Windows / Linux x64; the SIMD differential test passes with zig cc for `x86_64-macos` (3000 encodings, 35988 cases) | ✅ another Intel Mac (MacBook Pro 11,3, NVIDIA GT 755M: Metal 2 only, GPU family Common1/2): intro and title at a steady 60 fps once five driver limits are worked around - its textures have no swizzle view nor descriptor swizzle (the translated shaders apply the channel swizzle from `TSWZ_BUFFER_INDEX`, `tex_swz` in `maxwell/translate.c`) and it can't serialize Metal binary archives (saving one aborts in `MTLLoader`: the pipeline cache is off there, the shaders still cached as MSL); a no-copy buffer over 2 GiB reads back as zeros on it (the guest heap is wrapped in 2 GiB chunks, `guest_buffer_chunk`, else every vertex/uniform read was zeroed and the window stayed black), `getBytes` on a texture returns zeros (saved frames blit into a shared buffer instead) and it has no ASTC textures (creating one aborts in Metal's `MTLTextureDescriptor` validation: ASTC is decoded on the CPU at upload, `RECOMP_ASTC_SOFT`, as the Vulkan path does; the Course World entry that crashed on 2026-10-22 now loads) | ⬜ | ⬜ | ⬜ |
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
