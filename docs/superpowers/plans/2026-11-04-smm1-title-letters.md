# SMM1 Title Letters Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** On SMM2's title screen, tapping (or clicking) each letter of the logo does what that letter did on SMM1's
title screen, as a mod that is off by default.

**Architecture:** A new mod, `recomp/games/smm2/mods/title_letters/`, wakes only while SMM2's title scene runs and
the switch is on. It hit-tests taps against SMM2's own logo panes (`P_Super_00`, `P_LogoM_00` ...), plays the
letter's SMM1 button layout (`Play_Title<X>Btn_0n`, from the player's SMM1 dump) over the logo through the existing
SMM1 layout player and overlay, plays SMM1's sounds through `smm1_audio`, and runs the letter's effect: screen
effects drawn by us, and course effects through SMM2's own actor-creation function on the title course.

**Tech Stack:** C (runtime and mods), the SMM1 layout player (`mods/smm1/lyt_*.c`, `lay.c`), `smm1_audio.c`,
address hooks (`hle_override`, `MAIN_ADDR`), the Metal presenter on the Mac for the sepia pass.

**Spec:** the owner's request (2026-11-04): "the interactions you could do with the Mario Maker 1 title text ... tap
the letters of the text Super Mario Maker and each letter would do something different", "only as a mod and off by
default", "work on it only on the mac and github not ipad", "you do all the testing", "see if you can get all the
way there". Per-letter behaviour: research spec from Task 1 (`research/specs/smm1-title-letters.md`, private), with
the SMM1 summary below as its starting point.

| Letter (SMM1 button layout) | SMM1 behaviour (to be confirmed by Task 1) |
|---|---|
| SUPER (`Play_TitleSuperBtn_00`) | five Super Stars appear; a voice (random) calls "Super Mario Maker" |
| M (`Play_TitleMBtn_00`) | M flips to W ("Super Wario Maker"); a Mystery Mushroom with Wario's costume (SMB1 style only); Wario taunts |
| A (`Play_TitleABtn_00`) | multi-coloured stars fall behind; every enemy and shell on screen is defeated; train whistle |
| R (`Play_TitleRBtn_00`) | the R's legs extend and shoot a shower of coins; cash register + "Ta-da" |
| I (`Play_TitleIBtn_00`) | the parakeet sits on the I, records sounds and repeats them altered, then leaves |
| O (`Play_TitleOBtn_00`) | a Koopa Clown Car (Junior Clown Car in NSMBU style); cymbal |
| M (`Play_TitleMBtn_01`) | an empty Buzzy Shell or Spiny Shell; explosion sound |
| A (`Play_TitleABtn_01`) | Reset Rocket: countdown, it launches, clears every item and enemy on screen, comes back |
| K (`Play_TitleKBtn_00`) | sepia picture and 8-bit title music; tapping K again turns it back |
| E (`Play_TitleEBtn_00`) | a Trampoline; drum |
| R (`Play_TitleRBtn_01`) | extends, turns, and gives an empty Goomba's Shoe or a Yoshi's Egg (by style) |
| elsewhere | a tap spawns a Goomba, a Koopa, or rarely Bowser; a drag leaves a trail of magic dust |

## Global Constraints

- The switch is `smm1_title_letters` in smm2mods.ini, label "SMM1 title letters", group "Look and sound", **default
  off**. With it off, no hook changes anything and no file of the SMM1 dump is opened.
- Mac and GitHub only: no iPad builds or installs for this work. (It must still compile for every target:
  `scripts/check.sh` and the Linux / Windows release builds.)
- Nothing from Nintendo is committed: no code copied from either game, no assets, no decompiled output. SMM1's
  layouts and sounds are read from the player's own dump at run time (`smm1_data.c`), as the 100 Mario screens are.
- Research notes with addresses and behaviour go in `research/specs/` (gitignored); code comments describe behaviour
  in our own words, with addresses as the other mods do.
- Without an SMM1 dump the switch's description says it needs one, and the letters do nothing.
- Test runs: muted and hidden (`LARECOMP_NO_AUDIO=1 LARECOMP_HIDDEN=1`), one game instance at a time, never stealing
  focus. Gate commits on `scripts/check.sh`'s exit code with `&&`.
- Code style: the repo's: terse parenthetical comments, lines ≤ 120 columns, `MLOG`/`LOG` as the other mods.

## Review Focus

- **Off means off:** with the switch off, title-screen taps, the logo and the title music behave exactly as before.
  Task 2's test runs the title with the switch off and checks no `[title]` log line and no hidden pane.
- **Taps on SMM2's own buttons:** a tap on Play / Make / the "Press L+R" bar still does what SMM2 does, and only
  taps on a letter's box are the mod's. Task 2's hit-test unit test has points just outside each letter box.
- **Leaving the title mid-effect:** pressing L+R or choosing Make while a letter animates, the sepia is on, the
  parakeet is recording or the rocket is up leaves nothing behind (overlay, hidden pane, sepia, music, spawned
  actors cleaned up by the scene change). Task 2 adds `title_letters_reset()`; Task 4 and Task 6 each test leaving
  mid-effect.
- **Every game style and the logo's black / white variant:** effects that depend on style (Clown Car / Junior Clown
  Car, Shoe / Egg, Wario costume in SMB1 only) pick the right one or do nothing in styles that lack it (3D World
  title courses). Task 6's test cycles the title courses.
- **Repeated and rapid taps:** tapping the same letter while its animation plays, many letters at once, or 50 taps
  in the background must not stack unbounded actors or overlays. Task 3 caps one animation per letter; Task 5 caps
  live spawned actors with SMM1's own limits (Task 1 spec).

**Task 1 outcome (2026-11-04):** specs written (`research/specs/smm1-title-letters.md`, `smm2-title-scene.md`).
Letters are mapped by letter, not index (SMM2's logo has an extra "2"). SMM2 has no Reset Rocket and no Mystery
Mushroom actor: A2 clears the screen itself after its animation, M spawns the Mystery Mushroom costumes mod's item
when that mod is on. The general create (`main+0xe50830`) made a Goomba actor on the 3D World title course but it
wasn't drawn: Task 5 starts there.

---

### Task 1: Research: SMM1's letters and SMM2's title scene

**Files:**
- Create: `research/specs/smm1-title-letters.md` (private)
- Create: `research/specs/smm2-title-scene.md` (private)

**Interfaces:**
- Produces, for later tasks, written in the specs:
  - per letter: the button layout's animation names and lengths, the order they play in, the sound names (bank and
    entry) and when they play, what spawns (actor kind, count, position relative to the letter or Mario, style
    variants), cooldowns or limits, and what a second tap does;
  - the background tap (actor odds) and the drag trail (spacing, lifetime);
  - SMM2 v3.0.3: the title scene's update / enter / leave functions (addresses), how to read a logo pane's screen
    rectangle and set its visibility (reuse `edit.c` `find_pane`), the black / white logo variant, whether the
    title course's Mario takes input, the actor-creation function (start from `lanky.c`'s interception at
    `0x71013547c0`) with its argument layout and a worked example, screen → course coordinates on the title camera,
    the function(s) that defeat enemies / clear the screen (POW, Reset Rocket or course-clear sweep), and the title
    music's 8-bit variant (an SMB1-style track, or none).

- [ ] **Step 1: SMM1 per-letter behaviour from Block.rpx v208**: start from the converted ELF (scratchpad
  `smm1elf/Block.elf`) and the code near `0x02507064` (letter wiring) and `0x023bb694` (sepia); read each
  `Play_Title<X>Btn` layout in `~/smm-work/cache/smm1-v208/ui/layouts/`. Only one headless Ghidra job at a time.
- [ ] **Step 2: SMM2 title scene** in the v3.0.3 Ghidra project (match v1.0.0 addresses in
  `research/specs/smm2-frontend.md` by strings), plus the actor-creation call.
- [ ] **Step 3: Verify the actor call in the game**: a throwaway hook (not committed) that creates one Goomba on the
  title course 10 s after the title appears, run muted and hidden, screenshot with `mod_shot`. Expected: a Goomba
  on screen.
- [ ] **Step 4: Write both specs**, each claim marked confirmed (binary / layout / run) or inferred.

### Task 2: The switch, title detection and letter hit-testing

**Files:**
- Create: `recomp/games/smm2/mods/title_letters/title_letters.h`, `title_letters.c` (hooks, state),
  `hit.c` (pure hit-testing)
- Modify: `recomp/games/smm2/mods/smm1_extras.c` (the switch, its group), `recomp/Makefile` (sources, test target),
  `scripts/check.sh` (run the test)
- Test: `recomp/tools/test_title_letters.c`

**Interfaces:**
- Produces:
  - `enum TitleLetter { TL_NONE = -1, TL_SUPER, TL_M1, TL_A1, TL_R1, TL_I, TL_O, TL_M2, TL_A2, TL_K, TL_E, TL_R2,
    TL_COUNT };`
  - `typedef struct { float x, y, w, h; bool visible; } TlBox;` (game pixels, 1280 × 720, top-left origin)
  - `int tl_hit(const TlBox boxes[TL_COUNT], int x, int y);` → letter or `TL_NONE`
  - `bool title_letters_active(void);` (switch on and the title scene running)
  - `bool title_letters_box(int letter, TlBox *out);` (the SMM2 pane's box this frame)
  - `void title_letters_hide(int letter, bool hidden);`
  - `void title_letters_reset(void);` (called on leaving the title scene)
  - `extern bool g_title_letters;` (the switch)

- [x] **Step 1: Write the failing test** `test_title_letters.c`: `tl_hit` returns the letter for each box's centre,
  `TL_NONE` 1 px outside every edge, `TL_NONE` for a hidden box, and the later box for overlapping boxes.
- [x] **Step 2: Run** `make -C recomp test-title-letters`. Expected: FAIL (no `tl_hit`).
- [x] **Step 3: Implement `tl_hit`** in `hit.c`.
- [x] **Step 4: Run** the test. Expected: PASS.
- [x] **Step 5: The switch** in `smm1_extras.c` (key `smm1_title_letters`, default false, "Look and sound" group;
  description says it needs the SMM1 dump and works on the title screen by tapping or clicking a letter).
- [x] **Step 6: Title hooks** from Task 1's spec: set active on the title scene's enter / update, `title_letters_reset`
  on leave; read the panes' boxes each frame; on a tap (`touch_first`, lifted inside a box, as `flow.c` does) log
  `[title] letter <NAME>`.
- [x] **Step 7: In-game test**: switch on via `RECOMP_MOD_INI`, `LARECOMP_HOST_INPUT` taps at every letter's centre
  and one at an empty spot; expected: eleven `[title] letter` lines in order and none for the empty spot. Switch
  off: no `[title]` lines.
- [x] **Step 8: Gate and commit**: `scripts/check.sh && git commit`.

**Task 2 outcome (2026-11-04):** as planned, plus: `tl_trim` (hit.c, tested) cuts neighbouring letter boxes where
they overlap (SMM2's letter panes are 76.8 px wide but stand 46-70 px apart: R's covered half of I); the sources are
added in `games/smm2/game.mk` (the Makefile takes `GAME_SRC` from there); the hooks are Wait / Top / PlayMenu (taps)
and ToTop / ToPlayMenu (boxes only), with ToEdit and ToCourseRobot / ToNetwork / ToQuest resetting; a state not
followed (ToWait) is covered by re-finding the panes after 30 frames without a call and showing ours again if they
are still the page's. SMM2's title takes no taps on the logo (a tap there in Wait does nothing), so the mod only
watches the touch, it takes nothing from the game. In game (seed 4, `/Volumes/SSD/smm-work/tl-dev/run-1.log`): eleven
`[title] letter` lines in order, none for the empty spot; switch off (`run-2.log`): no `[title]` line.

### Task 3: The letters' SMM1 animations

**Files:**
- Create: `recomp/games/smm2/mods/title_letters/anim.c`
- Modify: `recomp/games/smm2/mods/smm1/flow.c` (chain `title_letters_image()` where `smw_goal_image` is chained),
  `recomp/tools/test_smm1ui.c` (a `--title-letter` render)

**Interfaces:**
- Consumes: `title_letters_box`, `title_letters_hide` (Task 2); `lay_open`, `lay_play`, `lay_draw`, `lay_rect`
  (`smm1/lay.h`).
- Produces:
  - `void tl_anim_start(int letter, int tick);` (one animation per letter; a tap while it plays is ignored unless
    Task 1 says SMM1 restarts it)
  - `bool tl_anim_draw(int tick, LQuads *out);` (false: nothing to draw)
  - `const uint8_t *title_letters_image(int *w, int *h, int *full, uint64_t *seq);` (as `smw_goal_image`)

- [x] **Step 1: Offline render**: `test_smm1ui --title-letter <LETTER> <out_%04d.png> <first> <last>` renders the
  letter's button layout through its animations at the SMM2 letter's box. Expected: PNGs showing SMM1's letter
  animating (compare against the layout's own frames).
- [x] **Step 2: Implement** `anim.c`: the SMM1 letter's layout scaled and placed so its idle frame covers the SMM2
  pane's box (scale from the two rectangles' heights), the SMM2 pane hidden while it plays, shown again after.
- [x] **Step 3: In-game test**: tap each letter, `mod_shot` at 20 / 60 / 120 frames after each tap. Expected: the
  SMM1 animation over the logo, the SMM2 letter back afterwards; tapping during the animation does nothing extra.
- [x] **Step 4: Gate and commit**.

**Task 3 outcome (2026-11-04):** as planned, with these changes. The layout is placed by its `B_Btn_00` (every
button layout's touch box: R's pieces and M2's offset included; the glyph pane alone put R's bowl 100 px off), at
the largest scale that fits SMM2's pane (min of the width and height ratios). anim.c's interface grew:
`tl_anim_start` returns whether it plays (logged), and `tl_anim_busy` (SMM2's pane hidden), `tl_anim_box` (where the
pane is each frame) and `tl_anim_reset` let the game thread and the renderers' thread share it under a lock; ticks
are a 60 Hz clock, not presented frames. Layouts load on the renderers' thread only (the layout player isn't shared
between threads); the SMM1 dump is opened on its own thread when the title first shows (switch on only). The
overlay's sequence numbers have their own range (the renderer re-uploads on a change). I and K hold DecideA's end
until the second tap plays DecideB. Offline: `test_smm1ui --title-letter` sheets in
`/Volumes/SSD/smm-work/tl-dev/off3/` (M flips to W; R grows tall; A2 launches off the top and comes back from below,
black, at frame 380; the parakeet lands on the I and leaves). In game (`run-3.log`, grid
`/Volumes/SSD/smm-work/tl-dev/s3/grid.png`): every letter's SMM1 animation over the logo, SMM2's letter back after;
a second M tap 30 frames in logs "still playing: no new one". Top state (`run-4.log`) works the same; tapping Make
mid-A2 (`run-5.log`, `tl5.png`) logs "letters off: left the title" and the editor shows nothing left behind. Switch
off (`run-6.log`, `tl6.png`): no `[title]` line, no dump opened, logo unchanged. (A tap anywhere puts SMM2's title in
touch mode, its own behaviour: a pad A right after doesn't press Make.)

### Task 4: Sounds, screen effects and the sepia pass

**Files:**
- Create: `recomp/games/smm2/mods/title_letters/effects.c` (stars, falling stars, magic dust, parakeet)
- Modify: `recomp/runtime/nvn/` Metal presenter (a colour-matrix pass with an on / off flag; Vulkan path too, so
  other platforms build and it works there), `recomp/games/smm2/mods/smm1/smm1_audio.c` (a short capture of the
  game's mixed output for the parakeet, if Task 1 confirms it records game sound)

**Interfaces:**
- Consumes: `smm1_se_bank`, `smm1_se`, `smm1_bgm` (`smm1_audio.h`); `tl_anim_start` (Task 3).
- Produces:
  - `void tl_effect_start(int letter, int tick);`
  - `void tl_effects_draw(int tick, LQuads *out);`
  - `void renderer_set_sepia(bool on);` (runtime; off at the scene change)

- [ ] **Step 1: Sounds**: each letter's SMM1 sounds at the times Task 1 gives; in-game test logs each `smm1_se`
  call's name and result. Expected: every name found (`true`).
- [ ] **Step 2: SUPER's five stars, A's falling stars, the drag trail**: drawn as layout quads (SMM1 effect textures
  if Task 1 finds them, else simple sprites of our own). In-game screenshots after each.
- [ ] **Step 3: K: sepia and 8-bit music**: `renderer_set_sepia` (a colour matrix on the final image, before the
  overlay), SMM2's own `BGM_Title_FC` swapped in at the title music's position (same loop points), SMM1's Ef_Sepia
  values; K again restores both. Screenshot on / off; test leaving the
  title with sepia on (it must turn off).
- [ ] **Step 4: I: the parakeet** lands on the I (DecideA), chirps and leaves (DecideB) on the second tap or after
  SMM1's recorder time. (SMM1 recorded the GamePad's microphone; no microphone is used here.)
- [ ] **Step 5: Gate and commit**.

### Task 5: Spawning on the title course

**Files:**
- Create: `recomp/games/smm2/mods/title_letters/spawn.c`

**Interfaces:**
- Consumes: Task 1's actor-creation call and coordinate conversion.
- Produces:
  - `enum TlActor { TA_GOOMBA, TA_KOOPA, TA_BOWSER, TA_COIN, TA_TRAMPOLINE, TA_CLOWN_CAR, TA_BUZZY_SHELL,
    TA_SPINY_SHELL, TA_SHOE, TA_YOSHI_EGG, TA_MYSTERY_MUSHROOM, TA_STAR };`
  - `bool tl_spawn(int actor, float screen_x, float screen_y, float vx, float vy);` (false when the style lacks it
    or the cap is reached)
  - `void tl_defeat_enemies(void);` and `void tl_clear_screen(void);`
  - `int tl_game_style(void);`

- [x] **Step 1: `tl_spawn` for a Goomba** at a screen point; in-game test: background tap → a Goomba where tapped
  (screenshot).
- [x] **Step 2: The other kinds**, each checked by screenshot in an SMB1, NSMBU and SMW title course.
- [x] **Step 3: `tl_defeat_enemies` and `tl_clear_screen`** with screenshots before / after.
- [x] **Step 4: Caps** as SMM1's (Task 1 spec: under 100 items / enemies / coins; Bowser 1 in 50, at most 3 alive,
  at most one per 30 taps, forced on the 30th; tap cooldown 20 frames): 50 background taps respect them.
- [x] **Step 5: Gate and commit**.

**Task 5 outcome (2026-11-04):** the Task 1 "Goomba" was not one: v3.0.3 renumbered the actor profiles (Treeki's ids
are v1.0.0's; v3.0.3's 87 is the Superball). With v3.0.3's ids (dumped from the profiles' own names at run time) the
general create of Task 1 works unchanged in player 1's area, and every TlActor kind is drawn in SMB1, SMB3, SMW,
NSMBU and 3D World title courses (`/Volumes/SSD/smm-work/tl-dev/t5/shots/k_*_b.png`). Interface as planned, plus
`tl_actor_name`, `tl_background_tap` (the tap rule, with SMM1's odds and 20-frame cooldown), and
`tl_spawn_frame` / `tl_spawn_done` / `tl_spawn_reset`: **tl_spawn works only inside the title's state hooks**
(title_frame gives it the guest CPU; letter taps already run there). Kinds: Goomba (Galoomba in SMW), Koopa, Bowser
(the style's; 3D World's is Meowser), coin (a free Coin actor pops, takes vx / vy, bounces, collectable), Trampoline,
Clown Car (Junior's in NSMBU), Buzzy / Spiny Shell (the alternate form), Shoe (empty; SMB1 / SMB3), Yoshi's Egg
(SMW / NSMBU), Mystery Mushroom (the costumes mod's Big Mushroom, SMB1, only with that mod on: `seed1b`, shot
`mm_10.png`), Star. 3D World has no Clown Car, Buzzy Shell, Shoe or Egg: tl_spawn returns false. Defeat = SMM2's
star attack on each enemy-base enemy on screen (they flip and fall); clear = SMM2's death with the vanish reaction
for every enemy-base enemy, item and object on screen (puffs). Background taps miss the logo (and its gaps), the "2"
and SMM2's buttons of the state. 65 taps (`caps2.log`): a cycle without a lucky Bowser got one on its 30th tap, a
lucky one at tap 4 got none after; taps closer than 20 frames log "too soon". Switch off (`off.log`): no `[title]`
line, nothing spawned. Tests: `RECOMP_TITLE_LETTERS_SPAWN=<kind>[:vx:vy],...`, `RECOMP_TITLE_LETTERS_DO=defeat@n,
clear@n`; any title course is forced with a mod folder (spec §4b). For Task 6: spawned enemies can kill the title
course's Mario (the course restarts, spawns gone); an actor made inside terrain (the logo stands over terrain in some
courses) is destroyed at once, so letter spawns may want a point below the logo; the costumes mod together with a
mod folder crashes at start-up, before the title (not this mod: the costume pack isn't laid with RECOMP_MOD_DIR), so
test the Mystery Mushroom with `SMM2_SEED=1` (an SMB1 title course).

### Task 6: Each letter's full behaviour

**Files:**
- Create: `recomp/games/smm2/mods/title_letters/letters.c` (the per-letter table: animation, sounds, effect,
  spawns, timings from Task 1)

**Interfaces:**
- Consumes: Tasks 3–5.

- [ ] **Step 1: Wire every letter** as the table at the top of this plan, corrected by Task 1's spec. Wario's
  Mystery Mushroom uses the Mystery Mushroom costumes mod's item when that mod is on, else the M's animation and
  taunt only.
- [ ] **Step 2: Full in-game pass**: every letter, the background tap and the drag, in each title course style,
  with screenshots at the moments Task 1 names; leave the title mid-effect for A2 (rocket), K, I. Expected: matches
  the spec; nothing left behind after leaving.
- [ ] **Step 3: Gate and commit**.

### Task 7: Documentation and shipping

**Files:**
- Modify: `recomp/docs/mods.md` (the mod's section), `PROGRESS.md`

- [ ] **Step 1: Document** what each letter does, that it needs the SMM1 dump, that it is off by default, and what
  differs from SMM1 (the parakeet replays game sound, no microphone).
- [ ] **Step 2: Final checks**: a fresh run with the switch off (title unchanged), `scripts/check.sh`, and a Linux
  or Windows cross-compile of the new files.
- [ ] **Step 3: Commit and push** to main (no release, no iPad install unless the owner asks).
