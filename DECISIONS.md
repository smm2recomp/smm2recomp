# Decisions

Newest first. Each entry: what was decided, why, and what would change it.

## 2026-10-18 — The Swift app runs the Rust front end (`mmk_front_*`); its screen classes are gone
**Decision:** with the SMM2 UI import, the Apple app's screens are `mmk-front`'s, through a C API
(`core/crates/ffi/src/front.rs`, section "front end" of `mmk.h`):
- **Create:** `mmk_front_new(options, engine, assets, audio, host)` — the app makes the engine
  (with its SMM1-extra options and character) and its audio, the front end loads, steps and
  restarts courses on that engine. The `Host` is implemented in Rust over it (status from the
  world as `mmk-play`'s host does; title courses from the cache / import; server listing,
  downloads, reports and the online calls on threads with `mmk-online`; replays recorded for
  server courses). The app only provides a callback table (`MmkFrontHost`): persistent values
  (`saves` / `hundred` stay the app's `saves.json` / `hundred.json`, the openings-seen flags and
  the character stay in UserDefaults, other keys go to UserDefaults `front.<key>`) and the pause
  volume. The library is pushed as JSON (`mmk_front_set_library`) whenever the Swift scan
  changes it, so the SwiftUI library and Course World list the same courses.
- **Per tick:** `mmk_front_tick` (menu buttons, engine buttons, pause, skip, a click in layout
  units); then `mmk_front_frame` (layout quad lists + a course-view item), `mmk_front_toast`,
  `mmk_front_sounds` (WAV names, volume, delay), `mmk_front_status` (course shown / playing / a
  direct play finished), textures through `mmk_front_texture(_count)`.
- **Swift keeps** its renderer (the course where the frame has the course item, then the layouts
  through `UILayer`, which now takes a texture-list closure), audio, input (keyboard, pads, touch
  controls; clicks / taps via a SwiftUI tap on the game view), the SwiftUI library, Settings and
  importer. `GameSession` is either the front end or plain play (no SMM2 UI import: drawn HUD,
  SwiftUI pause / clear / game over).
- **Removed:** `SMM2Boot/Title/CourseWorld/Course/Endless/Menus`, `Challenge`,
  `ChallengeScreens`, `SMM1UI`, `SceneFade` (~4 500 lines). `UISound` only plays the WAVs the
  front end names.
- **Captures:** `MMK_UI_SCREENSHOT` + `MMK_UI_SCREEN` now name `mmk-play --front-screen`'s screens;
  `MMK_UI_TICKS=n` runs n front-end ticks at once and holds (course screens: until the screen has
  been up n ticks), as `mmk-play` does. Before / after on the same 30 screens: 27 identical, the
  rest within an animation phase (numbers in PROGRESS.md Phase 8).
- **Departures:** without the SMM2 UI import there is no 100 Mario Challenge any more (it was the
  SwiftUI stand-in or the SMM1 layouts driven by Swift); the library's Story Mode filter is no
  longer opened by the title (Story Mode lists the built-in courses in the course list, as in
  `mmk-play`); `MMK_LIVES=on` no longer applies to SMM2-screen play; the demo strip plays plain.
**Why:** one implementation of every screen for every platform (owner: "same screens
everywhere"); the Swift copies had started to drift from the Rust port.
**Would change:** a ghost in the Apple app needs the engine snapshot to draw a second world (the
`ghost` online action answers "not shown in this app yet"); Settings screens in layouts.

## 2026-10-18 — Course World online in the Rust front end: SMM2's own screens over our server
**Decision:** `mmk-front` gets the Course World online features against the open-source server,
each on the game's own layout where it has one:
- **Account:** the hub's player card (`L_PlayerBody_00`) opens `Nwk_PlayerInfo_00` over the hub
  (its InPlayerInfo / In). The game's face / name / outfit buttons become ours: "Create
  account" (type a name, `register`), "Sign in with a token" (type an existing account's token,
  checked with `me`), "Sign out" [departure: no Miis or Nintendo accounts]. The token and the
  name / maker ID are kept with `Host::store_value` (`online.token`, `online.account`); the card
  shows the name [inf].
- **Course page:** "More info" on an open server course row opens `Nwk_CourseListWindow_00` in
  its course mode (Type 2, contents `Nwk_CourseDetail_00`) over the list: likes, plays, the first
  clearer and the record holder with the time (names fetched with `maker`), refreshed with
  `info`. The maker's Mii button opens their profile (the window's maker mode, Type 0, with their
  uploaded courses as course rows); the first clearer's button opens theirs; the record holder's
  races the record's ghost (`ghost`, then the course) [inf: the game opens the holder's profile;
  ghosts are ours]. The "Cleared by / Liked by" column shows the layout's "no one yet"
  [departure: the server keeps no player lists].
- **Likes / boos** where SMM2 has them: the judge buttons (`Cmn_GoodBtn_00` /
  `Cmn_NegativeBtn_00`) on a server course's clear screen and in its pause menu (Type 2, the
  judge row between the close button and Start Over; ←/→ between Boo and Like [inf]); the voted
  one keeps the end of its Decide on both screens [inf].
- **Comments:** "View comments" opens `Nwk_CommentViewer_00` (text comments, left and right in
  turn, name and date; absolute dates because the host gives no clock for "n hours ago" [inf]);
  confirm opens `Nwk_CommentPosting_00` in its text form to type one (signed in) [inf: the game
  posts from the pause / clear screens].
- **Leaderboards:** the hub's Leaderboards button opens `Nwk_CourseListMaker_00`: Course Makers =
  `maker_points`; Players = clears, first clears, world records, Endless Easy / Normal / Expert /
  Super Expert (the sort button's TypeClearCourse categories, cycled by it [inf: the game opens a
  sort window]); ←/→ step through all eight; rows `Nwk_MakerData_00` (rank, name, score under its
  label; own row TypeOwn); confirm opens the maker's profile. Versus, followed, friends and
  official makers are hidden [departure].
- **Clears:** signed in, a server course's clear is sent as its replay (`submit_replay`: the host
  records every server course from its start and hands over the cleared run), the server
  re-simulating it; refused, a plain `clear` with the time follows; with play options that change
  play (`Host::play_options_affect_play`) the clear goes without a time and no replay [inf].
  Signed out: the anonymous clear report as before.
**Done:** each row is SMM2's own course card, `Play_CourseIn_00` read from the game's romfs at run
time (the layout player now reads Switch BFLYT 8+/9 and BNTX textures with the runtime's Tegra
deswizzle, BC and ASTC decoders), with the course name and maker set and the ID, flag, Mii and the
card's other parts hidden. Each card sits where SMM1's row would be on that frame, scaled 0.6, so the
roll keeps SMM1's timing, scroll and music. Its text uses SMM1's bitmap MessageFont: SMM2's message
font is a scalable OpenType font from the console's system fonts, which the recomp does not ship. 100 Mario in SMM2's presentation reports the
  run's courses cleared after each clear (`endless`; the server keeps the best) [inf: as the run
  grows, so a run that ends any way counts].
- **Input:** `FrontInput::text` (one typed key per tick: a character, '\u{8}' erase, '\n' done,
  '\u{1b}' cancel) with `Front::typing()` telling the host when a field is open; `mmk-play`
  then routes the keyboard there (`FrontApp::key_text`). Clicks select-and-decide on every new
  screen, the course list, the Endless menu / map and the SMM1 100 Mario screens.
- Messages shown by these screens come from the layouts' message tables at run time; our own
  lines (button labels on the player info, toasts, "No comments yet.") are ours. Empty-list texts
  go in the loading part's waiting line: its no-data text uses an outlined font style the layer
  doesn't draw yet.
**Why:** the owner's Phase 10 (accounts, likes, comments, leaderboards, records and ghosts)
reachable from SMM2's own Course World on every platform, the server staying ours.
**Open / would change it:** the pause / clear screens' comment buttons; the server timing a
replay to the end of the clear (`Replay::run` stops at `World::cleared`) while the clear screen
shows the goal time, so the record shown differs from the clear screen's time; a Course Maker in
the front end (upload); thumbnails, Miis and per-player lists if the server gains them; the
game's sort window, its relative comment times with a host clock.

## 2026-10-18 — The web build runs the SMM2 front end; the UI cache reads through a file source
**Decision:** the browser shows the same SMM2 menus as `mmk-play` (`mmk-front`), and the UI
cache no longer reads files itself:
- **`mmk_ui::UiFiles`** (`read(rel) → Fetch::{Ready, Later, Missing}`, `list`, `is_dir`):
  `DirFiles` on disk (what `UiCache::open` uses, unchanged behaviour), `MemFiles` in memory.
  A `Later` texture gets its id and size at once (sizes come from the layout JSON / font
  metrics) with an empty stand-in (`TextureEntry::ready == false`); the host takes the requests
  (`take_requests`), reads the files and hands them back (`provide`), as `mmk_assets::Cache`
  does for the game's textures. Layouts, font metrics and message tables must be `Ready`.
- **On the web**: the SMM2 UI import lives in OPFS `mmk-ui/` with a file list (`files.txt`;
  walking ~500 folders of OPFS takes seconds). `open_front` reads all its JSON (782 files, 13 MB;
  ~0.4 s) and the UI WAVs (19 MB, kept as bytes, decoded on first play) into memory, lists the
  textures (170 MB) as `Later`; each frame `pump_ui` starts the reads, and the front end holds
  (at most 30 frames) while they're on their way — screens appear whole; the layer skips quads
  whose textures are still missing. The host (`web_front`): library = the game data's built-in
  courses (decoded once in the background, kept zlib-compressed) plus files the page opens;
  saves / settings in `localStorage`; no course server listing (no HTTP client in the web
  build); a course's music is read when the front end loads it (until then the bank's music).
  The import arrives as an `mmk import-smm2-ui` folder, or the in-page dump import makes it
  (`smm2_ui` now reads through the importer's `Vfs`).
**Why:** a pluggable source keeps one cache for every host; preloading everything (≈200 MB)
would cost memory and start-up time, and a "not loaded yet, retry" path through every screen
would spread async through the front end. Textures are the only bulk and the only thing a
screen can show late.
**Would change:** if a screen ever needs a layout the page could not read up front (it reads
them all), or memory limits on mobile browsers call for evicting textures.

## 2026-10-18 — SMM2 2.0/3.0 parts: own modules, behaviour from v3.0.3, test courses from the editor
**Decision:** the 2.0/3.0 enemies, blocks and gizmos (Spike, Pokey, P Block, Dash Block,
Mechakoopa, ON/OFF Trampoline, the Koopalings) live in `actors/v3.rs`, `actors/koopalings.rs`,
`world/v3.rs`, `world/koopalings.rs` and `level/v3.rs`; the shared files only carry hook lines
(Kind variants, dispatch arms, the contact / fire / shell / carry / tail / dash hooks, one
`v3: V3State` actor field, one `v3: V3World` world field, one `v3: V3Tiles` level field). Numbers
come from the v3.0.3 executable (specs in the private `research/specs/objects/`); as the sample
corpus predates 2.0, every part has an editor-built test course and sim tests.
**Choices the binary did not settle** (all marked in the code):
- rolling spike balls / snowballs and a thrown knocked-down Mechakoopa knock out enemies they
  run into [ext]; a carried snowball or robot hitting an enemy breaks like a carried shell [inf];
- grabbing a Snow Pokey with Run puts a snowball in the player's hands [inf: the grab hits the
  head and the player ends up holding it];
- the missile's blast also knocks out enemies in its box [inf];
- Koopalings: a Koopaling in a Clown Car is reported unsupported (its Clown context is not
  modelled); Roy only comes up from the floor (the ≤ 33 % ceiling exits are not modelled);
  Morton's winged feints, wing flight and parachute behaviour are not modelled; the ledge-jump
  probe is one column one tile ahead;
- the Mechakoopa laser and the spike-ball spin are effects the importer does not export: the beam
  is drawn under its own name (`M1_Effect_laser`), which the cache lacks;
- the P Switch lasts 540 frames (the course switch state's timer, 0x71016c4a48) instead of the
  earlier [ext] 600.
**What would change it:** frame-stepping v3.0.3 in Ryujinx (see each spec's "How to verify").

## 2026-10-18 — The SMM2 front end lives in Rust (`mmk-front`); hosts only draw, play and input
**Decision:** the SMM2 presentation — opening, scene fades, title and Play menu, Course World,
100 Mario in SMM2's Endless screens or SMM1's, the in-course HUD / card / pause / settings /
clear / give up / time up / game over screens, the course-play rules around them (when the
engine steps, deaths, retries, the run's results) and the UI sound resolution — is the crate
`core/crates/front` (`mmk-front`), a port of the Swift app's screens, platform-neutral:
- **In:** per 60 Hz tick a `FrontInput` (menu buttons held, pause, skip, an optional click in
  layout units) and a `Host` (the engine: load / step / restart a course and read a
  `CourseStatus`; the library and the server listing; small key → string storage; a random
  number). The front end never touches files, sockets, the sim or a GPU.
- **Out:** `Front::frame()` — layout draw lists in order (`DrawItem::Layout`: quads, layout
  size, which import's textures) with `DrawItem::Course` where the engine's picture goes (the
  whole 16:9 view), plus a message (the Swift app's toasts); `Front::take_sounds()` — `SoundCue`s
  (WAV names of the SMM2 UI import's `ui/sound/`, volume, delay in frames).
- Scene changes are deferred `Go` values run when the fade has covered the screen, so screens
  stay plain state machines without callbacks; captures make fades instant, as the Swift app's
  frozen captures do (`MMK_FADES=1` keeps them).
- `mmk-play` uses it by default when no course is given and the SMM2 UI import is found
  (`--ui-cache`, `$MMK_UI_CACHE`, `~/smm-work/cache/smm2-ui-3.0.3`); its host is
  `shell/src/front_host.rs` (library = a course folder + the asset cache's built-in courses,
  scanned on a thread; server through `mmk-online` on a thread; saves and settings as files in
  `~/.mmk-play`), its drawing `front_app.rs` (one `UiLayer` per import, the course with the game
  renderer), its sound `ui_sound.rs` (UI clips mixed into the game's output stream).
- **Verification:** `mmk-play --front-screen NAME --ticks N --screenshot out.png` renders the
  same screens as the Swift app's `MMK_UI_SCREENSHOT` / `MMK_UI_SCREEN` / `MMK_UI_TICKS`; the
  deterministic ones match it to within rounding (max channel difference ≤ 5).
- Departures of the portable front end: Story Mode and Coursebot open the course list (over the
  built-in Story Mode courses / the library) instead of the Swift app's SwiftUI library; dates in
  the course list use the C library's local time zone (UTC on Windows for now); messages are drawn
  with the shell's bitmap font.
**Why:** one implementation for every platform (the owner's "same screens everywhere"), testable
headless, and the Swift app can switch to it through the FFI (next step: a `mmk_front_*` C API —
create with the UI import(s) and a host vtable of C callbacks, `tick(input)`, `frame()` returning
draw items whose quads the existing `UILayer` already draws, `sounds()` — replacing the Swift
screen classes). **Would change:** a web front end needs the UI cache read asynchronously (OPFS)
before `Ui` can open layouts on demand; until then the web build keeps its plain HUD. (Done the
same day: see "The web build runs the SMM2 front end".)

## 2026-10-18 — Name: SMCraft
**Decision:** the project, app and window titles are **SMCraft** (owner's choice). App bundle id
`dev.smcraft.app`; the macOS data folder becomes `Application Support/SMCraft` (an existing `MMK`
folder is moved over once). The old working name survives only internally: crate names (`mmk-*`),
the `mmk` CLI, `mmk-play`, the `MMK_*` environment variables and the C API prefix `mmk_`.
**Why:** a rename of every crate and symbol would churn the whole tree while parallel work is in
flight; the internal prefix is invisible to players. A full internal rename can follow later.

## 2026-10-18 — SMM2 front end driven by the game's scene code; what is inferred
The opening, the title, the scene fades, Course World and the Endless Challenge menus follow the
owner's v3.0.3 binary, read as prose in the private `research/specs/smm2-frontend.md` (states,
their order, the fade types, the opening's odds, which button the cursor starts on) [obs]; the
layouts, animations, texts and sounds are the game's own data from the owner's import.
- **Opening** (`Boot_BootScene_00`): the boot page's choice — the newest update opening not seen
  yet (Koopalings, Link, co-op; seen flags in UserDefaults `boot.openingsSeen`), else 66 % default,
  8 % characters, 8 % Goombas, 2 % co-op, 8 % Link, 4 % Koopalings, 4 % Bowser; InMain; then the
  title under the black circle fade. Not skippable, as in the game; Settings > Game turns it off.
- **Fades:** `Cmn_Fade_00` drawn over every screen by both Metal renderers (a black veil over
  SwiftUI screens). Scene changes without parameters use the black fade (30 + 30); the opening
  hands over with the black circle. How long the screen stays covered while the next one loads
  is 2 ticks here [inf].
- **Inferred [inf]:** how long a button's Decide plays before its action (the title's 24 ticks
  kept), the Course World cursor start (Courses), the course list's tab meanings over our sources,
  the death iris in Course World play (the circle closes on the player 60 frames before the
  restart), the Endless map's order (Telop with Mario out of the castle, then the walk), the dots
  hidden until reached, which container child a context picks for sounds.
- **Departures:** Make says "coming soon" (no editor in the app); the network-only Course World
  buttons say so; the player card, ID search, Miis, flags, badges, thumbnails, comments are
  hidden; no ending for an Endless-styled run (back to the menu with the result).
What would change it: reading the Course World / Endless sequences' Disp states and the
course-in / death sequence in full; footage of the game.

## 2026-10-18 — Course World without lives (owner's decision)
SMM2's Course World and Story Mode have no lives, so library / Course World / Story Mode play
has unlimited retries: HUD `Type` 0, course card `AnimDefault` at the start only, no game over;
after a death the black circle closes on the player and opens on the restart [inf: the iris].
100 Mario (either presentation) and the Endless-styled menus keep lives: HUD `Type` 1, card
`AnimEndlessMode` after each death, `Play_GameOver_00`. Supersedes "Lives stay" below.

## 2026-10-18 — 100 Mario Challenge presented with SMM2's Endless Challenge screens
Default presentation (Settings > Game `hundred.presentation` = smm2; smm1 keeps SMM1's own
layouts). Rules stay SMM1's (`core/crates/modes`). `Nwk_EndlessModeMenu_00` + `Nwk_WorldMap_00`
for the menu and the map between courses; SMM2's in-course screens with lives for the courses.
The menu's New Game / Continue buttons are drawn from a second instance of the layout over the
first: in the layout they sit under the map frame's opaque bottom band (drawn after them), where
the game shows them [inf]. Locked tiers (SMM1's unlock rule; SMM2 has none) are greyed.

## 2026-10-18 — Course World over the library and the open-source server
The course list shows library courses (statistics, tags and dates from the dataset's
`index.json` when present, else this player's own) and the courses of the server set in
Settings > Game (`online.server`; none by default). The FFI links `mmk-online` (`ureq`, rustls)
for `mmk_online_search / download / report`; Swift runs them off the main thread. Server
courses are downloaded into `courses/Course World/` before playing; plays and clears are
reported. Never Nintendo's network.

## 2026-10-18 — Menu sounds from the game's SLink2 data, as WAV in the UI import
Which sound a button or animation plays is the game's data: `Pack/Static.pack`
`Param/Layout/<Layout>.byml` (SLink user, button → sound type, animations with sounds) and
`SLink2/SLink2DB.szs` (XLNK: user → event key → sound, volume, delay; containers by condition or
in sequence). `mmk import-smm2-ui` v4 writes `ui/sound/sounds.json` and the referenced sounds as
16-bit WAV (`formats::xlink`, `import::smm2_ui_sound`); the app plays them with AVAudioPlayer
(`UISound`, master × effects volume). The conditions themselves are not evaluated: the
controller variant, the title's "back", the course style's pause jingle are chosen by context
[inf]. Missing: sounds not in `Sound/Resource/*.bars` or the bars packs (the pause jingles).

## 2026-10-18 — iOS packaging: one Swift package, cross-compiled, bundled by a script
The iOS / iPadOS app is the macOS Swift package built for iOS, not a second project.
- **Build:** `swift build -c release --triple arm64-apple-ios17.0[-simulator] --sdk <sdk>`. The
  Rust core is a plain static library per target (`cargo build -p mmk-ffi --target
  aarch64-apple-ios` / `aarch64-apple-ios-sim`), passed with `-Xlinker -L <dir>`. SwiftPM
  conditions can't tell the simulator from a device, so `Package.swift` keeps its link path
  for macOS only. `scripts/make-ios-app.sh [--device]` builds both halves and writes the
  `.app` (Info.plist, icon, ad-hoc signature for the Simulator) like `make-app.sh` does on
  macOS. `scripts/check.sh --ios` runs it for the Simulator.
- **Rejected:** an XCFramework + `.binaryTarget` (it would change the macOS build and needs the
  framework rebuilt before every `swift build`); an `.xcodeproj` (hand-kept project files,
  nothing to generate them without new tools); the `AppleProductTypes` `.iOSApplication`
  product (Xcode / Playgrounds only, breaks `swift build`).
- **Signing is the owner's:** device builds are signed only when `MMK_IOS_SIGN_IDENTITY` and
  `MMK_IOS_PROFILE` are set (the profile's entitlements are applied), otherwise left unsigned.
- **Data on iOS:** the app's Documents folder holds what Application Support/MMK holds on
  macOS, and the Files app shows it. The importer stays on the Mac; the owner copies the cache
  folder in, or picks it anywhere and the app copies it (copying is simpler and safer than
  keeping security-scoped bookmarks to files the core reads lazily). In the Simulator the
  development caches are found through the Mac's home (`SIMULATOR_HOST_HOME`).
- **BC textures:** iPhones and iPads before M1 can't sample BCn. There the core decodes to
  RGBA8 (`mmk_texture_decode_rgba`, shared with the portable shell, whose BC7 decoder moved to
  `formats`).

What would change it: needing App Store distribution or extensions (then an Xcode project), or
SwiftPM gaining a simulator/device condition (then the link path moves back into the manifest).

## 2026-10-18 — 3D styles: meshes in the cache, sim sprites become mesh draws
NSMBU (WU) and 3D World (3W) are drawn from their BFRES meshes. What the data showed first:
- **NSMBU** enemies, items and objects are pre-rendered frames on a quad (`walk_Alb.000` …,
  switched by texture-pattern animations), like the 2D styles but at a finer frame rate. Its
  players (body + head models), Yoshi and the bosses are skinned meshes. Terrain is the
  `WU_Field_*` atlas, drawn as tiles as before.
- **3D World** is meshes throughout. Its terrain is one model per field-atlas cell
  (`<theme>_model_<atlas index in hex>`, the same index the sim already computes for the 2D
  atlases), with neighbour variants (`_ff`, `_fs`, `_l_f`, …) for slopes and edges. Blocks,
  coins and pipes live in `Pack/StaticModel.pack`, which the importer now reads for WU_/3W_.

Decisions:
- **Cache format:** `meshes/<Model>.mmesh` holds every model of a game file: skeleton,
  materials (albedo texture, wrap, unlit flag), shapes, LOD 0 only. Vertices are baked into the
  bind pose in model space, so rigid and single-bone shapes skin like smooth ones. Each
  animation is its own `anims/<Model>/<anim>.mskel`, plus an `index.json` per folder. It merges
  the skeletal, bone visibility and texture-pattern tracks that share a base name (`walk` +
  `walk_ftp`). Curves are kept as decoded keys and evaluated at run time, not baked per frame.
  Both files are a JSON header plus a binary blob (`mmk_assets::mesh`). Importer version 5.
- **Who poses:** Rust (`MeshModel::pose`, through `mmk_mesh_pose`) computes the skinning matrices,
  bone visibility and the texture of each shape. Metal skins on the GPU. Only the 3D styles use
  this path. `mmk_engine_set_meshes(false)` / `MMK_NO_MESH=1` turns it off.
- **Mapping a sim sprite to a mesh** (`ffi/src/mesh.rs`):
  - Model: `M1_<rest>` → `<style>_<rest>`. A model of its own for the frame wins
    (`3W_Enemy_nokonokoA_shell` for `shell.*`). In a multi-model file, the model named after the
    frame stem wins, then the one named after the file, never a `*break*` debris model.
  - Players (table): NSMBU `WU_Player_MarioMdl`, `SMB_model` + `SMH_model` for small, `MB_model`
    + `MH_model` otherwise. "S" = small is inferred from the sizes [inf]. The head rides the
    body's `face_1` bone [inf]. 3D World uses `3W_Player_Mario` / `…MarioMini` with the Face,
    Eye, HandL and HandR files on the body's `Head` / `HandL` / `HandR` bones. Fire forms use the
    `.1` textures.
  - Blocks (3W): field-atlas index → `3W_Object_block_*` (brick, ?, used, hard, donut, cloud,
    ice). Other 3W tiles take the terrain model for their atlas index. The first variant is used
    for edge and slope cells [inf]. Cells without a model (pipes, note blocks, …) fall back to
    the NSMBU atlas tile.
  - Animation: the frame stem (`walk.1` → `walk`), matched case-insensitively, then a synonym
    table (`fall` → `jump2` / `FallLoop`, `shell` → `Move`, `dead` → `DieOver`, …), then
    `wait`, else the rest pose. Players also try their shared animation files
    (`WU_Player_CommonAnm`, `3W_Player_Animation_*`). Small NSMBU Mario tries `SMB_`-prefixed
    names first; the NSMBU head uses `PH_` texture animations.
  - Time = the world tick [inf: the sim keeps no per-actor animation clock], so all actors
    animate in phase and one-shot animations start mid-way.
  - Placement: origin = the sim's model origin (x + w/2, y). Scale = sim width ÷ the SMB1
    texture width. Enemies and players turn 70° from the camera toward their walking direction
    [inf: chosen so the face shows; not measured]. Pre-rendered NSMBU cards are mirrored like
    the 2D sprites; their art is taken to face left like SMB1 [inf].
  - Bones the animation doesn't mention keep their rest visibility, except alternative parts.
    For `<name>NN` siblings only `…00` shows (3D World faces, hands, eyelids) [inf].
- **Rendering:** orthographic side view sharing the sprites' world → NDC transform. A depth
  buffer is used per depth group (consecutive mesh draws of one layer), and every later group is
  in front of all earlier ones. Meshes keep real depth among themselves inside a layer, and
  painter's order with sprites between layers. Lighting is one directional light plus ambient
  (our choice). Cards are unlit and alpha-tested. 3D albedo alpha is ignored, since the render
  state isn't read [inf]. Backgrounds (`_DV_`) aren't meshed yet.
- **Debug:** `MMK_MESH_PREVIEW=<mesh>[#model][@anim],<style>:<sim model>:<frame>,…` model viewer
  with `MMK_SCREENSHOT` (mirrors `MMK_UI_LAYOUT`); `mmk bfres <file> [--member] [--curves]`.

What would change it: the game's per-actor animation state and blend rules, the camera angle
and actor turn from Ryujinx captures, material render info (translucency, emission), and the
3D World neighbour-variant rules.

## 2026-10-18 — 3D styles: the game's cameras, background scenes, environment and materials
Measured against reference frames of the owner's SMM2 in Ryujinx (pipeline outside git; findings
with addresses in research/specs/render-3d.md).
- **Cameras** (`mmk_host::scene3d`, from the layer-camera update FUN_7100aced20): NSMBU field
  orthographic; 3D World field perspective, fovy 12.5° in play, distance keeping z = 0 exactly
  216 px tall, eye 35 px above the view centre looking 5 px above it; the Course Maker view stays
  flat. Background scene: perspective from the style's SceneDB row (`DV_Play_Fovy`, near 100,
  far 80000, eye at (view centre x, view centre y + CamMoveY·view bottom, 1000), frustum offset
  ProjOffsetY + (ProjMoveY − CamMoveY)·view bottom / 1080). Matrices are column-major,
  world px → clip with depth 0…1; both renderers use the same ones.
- **Background**: the `_DV_` model (importer v6 meshes them) is drawn first in a pass with its own
  depth; the sim's 2D backdrop sprites (`*_DV_*`) are dropped when one is drawn. Its skeletal
  animation (clouds) runs on the world tick.
- **Lighting** (our model, [inf]): per pixel ambient + hemisphere + up to three directional
  lights (Lambert, back-side colour on faces turned away) from `env/<name>.json`, times an
  exposure (1.0); fogs as the materials switch them (`enable_fog`, `enable_fog1`,
  `enable_fog_y`). The game builds cube-map light maps from the same lights; a wrap-around
  (half-Lambert) variant measured worse.
- **Materials**: one shared decision per shape (`mmk_host::mesh::shape_look`, also behind
  `mmk_mesh_shape`): render mode, alpha test, faces, self-lit, fog switches, one extra texture
  (emission added / alpha mask / sky mask), world-space texture coordinates (span 64 px,
  measured), TEV-0 tint, texture SRT, vertex-colour ratio. Sky: a gradient between its constant
  colours 5 and 4 by the mask under clouds shaded between colours 1 and 0 [inf].
- **Cache**: importer v6 (materials keep textures by shader sampler, render state, selected
  parameters and options; vertex colours and UV1; `env/`). Older caches still load (lit, opaque,
  no backgrounds) — re-run `mmk import` for the new look.
- **C API**: `MmkMeshShape` (68 bytes) and `MmkMeshModel` (80) grew; `mmk_engine_scene3d`,
  `mmk_engine_background`.

What would change it: the game's light-map and fog shader math (the translated shaders can be
logged through MoltenVK, see the pipeline README), the remaining uber-shader features (normal,
specular and bake maps, env maps), bloom / colour correction.
## 2026-10-18 — SMM2 UI: same cache form as SMM1, scalable fonts rasterised at import
- **One cache form for both games.** `mmk import-smm2-ui` writes `<out>/manifest.json`
  (`"game": "SMM2"`) and `ui/{layouts,textures,fonts,messages}` exactly like the SMM1 UI import,
  so `UiCache`, the FFI and the app load either; messages sit flat in `messages/<archive>/`
  (SMM2 has no groups) and the reader takes both. Location: `<assets root>/smm2-ui-<build>/`
  (development copy `~/smm-work/cache/smm2-ui-3.0.3/`), `AppPaths.findSMM2UICache()`
  (`MMK_UI2_CACHE` first); the asset-cache search skips `smm2-ui-*`.
- **Text: TrueType/OpenType → bitmap fonts at import.** SMM2's text fonts are scalable
  (BFTTF/BFOTF behind BFCPX combinations). The importer rasterises, once, every character the
  chosen language's messages use (plus ASCII and Latin-1) at each combination's authored size
  into the BFFNT-like grid the player already draws, with `ab_glyph` 0.2.32 — already pinned in
  Cargo.lock (winit uses it on Linux), built offline, licence Apache-2.0 (ttf-parser MIT/Apache).
  The player stays a bitmap-font player; no font code at run time. Would change for user text
  in scripts the import doesn't cover (Japanese course names): bake more, or rasterise at run time.
- **Metrics we chose [inf]:** line height 1.5 × size, character width = size (SMM2's text panes are
  authored at 1.5–1.57 height:width, which then scales glyphs evenly); baseline centring the main
  Latin font's em box; later combination entries win (makes every SMM2 entry reachable); the
  console's own fonts are missing: UD Kakugo stands in for the system UD gothic, and the
  controller button icons (U+E0E0… A/B/X/Y/L/R/ZL/ZR/SL/SR/+/−) are drawn as shapes with
  letters (`smm2_font::button_icon`, community code-point table [inf]). No glyph borders yet.
- **The title courses go into the UI import too** (`courses/title_course_data_*.bcd`, encrypted
  as shipped), so the title has its demo even next to an asset cache without built-in courses.

## 2026-10-18 — SMM2 title screen: the game's layouts, inferred choreography, two departures
- **Layouts and their animations are the game's** (`Play_Title_00`, `Cmn_ClapperboardBtn_00`,
  `Cmn_MainMenuBtn_00`, `Cmn_Cursor_00`); states and transitions are the title layout's own
  animations (PressKey / DecideBackBtnTop, DecidePlayBtn / DecideBackBtn) [obs]. Inferred:
  the screen starts on the "Press L + R" prompt; any menu button brings Make / Play; the cursor
  is `Cmn_Cursor_00` sized to the selected button's `N_Carsor_00` / `B_Btn_00`; logo letters
  white over a dark sky (`Type` frame 1), black otherwise; 24 ticks of the button's Decide before
  the next screen. Details in the private `research/specs/smm2-layouts.md`.
- **Behind it**: the title courses the engine can play (NSMBU / 3D World ones need the mesh
  renderer), one after another, with a scripted demo (stand 1 s, walk right, jump every 2 s;
  next course on death, clear or after 40 s). In the game the player moves Mario there.
- **Departures:** a fourth Play-menu button "100 Mario Challenge" (a copy of the Coursebot part,
  castle icon) for the SMM1-style challenge, with the list drawn at 80% so four fit between the
  logo and the copyright line; Make (Course Maker) only claps and says "coming soon". Story Mode
  opens the library on its Built-in filter, Course World and Coursebot the library.
- **Player semantics added [inf]:** window flag bit 0 "one material for all frames" gives every
  frame the first frame's colours and keeps each frame's texture (SMM2 button shadows); a part
  property with material usage 1 takes only the enclosing material's colours (Course Maker icon
  tint). Would change with a frame-by-frame comparison against the game.

## 2026-10-18 — SMM2 in-course screens: the game's layouts, inferred flow, lives kept
Library courses (not 100 Mario runs, not the title demo) get SMM2's in-course layouts when the
SMM2 UI import is there (`SMM2Course.swift`, `CourseScreens`); otherwise the drawn HUD strip and
the SwiftUI menus as before (`MMK_UI_SMM2=off`). Layouts, animations, texts and icons are the
game's, read at run time; the flow between them is not in the data:
- **Lives stay** (superseded 2026-10-18 by "Course World without lives" for library play; still
  how 100 Mario runs show lives). SMM2's Course World and Story Mode have no lives; the engine has 5 and a game
  over. We keep the engine's rule and show it the way SMM2 shows lives elsewhere: HUD `Type`
  frame 1 (the Endless Challenge variant: lives counter with the character icon; its 1-Up
  progress hidden) and the course card `Play_CourseIn_00` `AnimEndlessMode` (× lives) at the
  start and after each death, `Play_GameOver_00` when they run out [?: owner may prefer
  infinite retries like Course World].
- **Course card** on a black `Cmn_Fade_00` (FadeOutB held, FadeInB after): course name, maker
  when known, the clear-condition sentence + icon (the icon placed just left of the centred
  sentence: the game aligns them, the layout leaves it at a fixed place [inf]). The 3D player
  figure in its middle is not drawn (no layout).
- **HUD** `Play_PlayScene_00`: `Skin` frame = style (M1, M3, MW, WU, 3W — the data's order),
  `TypeCharacter` = character; number widths from the value tags' kinds (9 → 2 digits, 10 → 3,
  16 → 9, matching the samples "99", "999", "123456789") [inf]; `TimeCaution` from 100 s left
  [inf]; clear condition: icon + "n/N" (`TypeMission` 0) or icon alone (1), `W_MissionClear_00`
  shown when met, `InMissionFailed` when broken [inf]; pink coins: one slot per coin in the
  course, rightmost slots, `Type<style>` frame 1 = collected, `Get` pop, the frame's left end
  moved to the first slot [inf]. Keys: `Play_KeyIcon_00` icons in a row under the score — a
  departure (the game shows held keys with the player). Pop-ups that need in-course positions
  (points, condition met balloon, Ninji panes) are hidden.
- **Clear-condition icons and sentences:** icons are the textures of
  `Model/<style>_Lyt_Mission_RGBA8` (StaticModel.pack), imported to `ui/textures/MissionIcon/`
  and put into `Cmn_MissionIcon`'s `P_DummyIcon_00` (the course's style, else another's);
  sentences are ReferenceMsg `GoalCondition` templates filled with the count and a name from
  `PartsIconName(_plural)`. Condition → (label, part, icon) is our table, matched by the
  internal names shared by the condition list, the name table and the icon textures; "at least
  N" (`_02`) is always used [inf]; no table entry → our own short wording.
- **Pause** `Cmn_PauseMenu_00`: `Type` 3 (Start Over + Exit Course; no Edit, likes, comments),
  `TypeCourseInfo` 1 (Course World block: mode, title, condition line, maker, best time; Mii,
  flag, badges, tags hidden); "Retry" instead of "Start Over" once a checkpoint was reached; the
  close button is the Continue item and the selection starts there (as in our SMM1 pause); a
  black dim at 82% stands in for the game's blurred capture behind the panel and the HUD fades
  out [inf]. Settings and "Mario's Moves" are hidden.
- **Exit Course** → "You gave up..." `Play_CourseRetire_00` with Exit / Keep playing [inf: how
  the game uses that screen in single play].
- **Clear:** `Play_CourseClear_00` `InNet`, `Type` 2 with the network parts hidden, the button
  pair (Start Over / Quit, labels "0000"/"0001" [inf]) centred because Comment / Tag are gone.
  The banner (`N_Cap_00`) and the give-up screen's purple card are authored off screen and shown
  through panes that distort a frame capture (`PF_*`, `P_Wave_*`: indirect textures over a
  `Dummy` placeholder the game replaces with the capture); we draw `N_Cap_00` in place,
  undistorted, and keep the "COURSE CLEAR!" glow lettering white because the letter-shaped
  holes would otherwise show only the band [inf]. `RegionLanguageType` 1 for English [inf].
- **Time up:** `Play_TimeUp_00` over the death when the timer runs out (engine: dying with
  0 frames left), then the usual death handling.
- **Player additions:** a caller texture for a picture's material (`set_picture_texture`,
  textures from `ui/textures/<group>/`), forced material colours (`set_material_color`), text
  width (`text_width`); shared message tables (`UiCache::table_message`); SMM2 number tags
  (group 250) become value placeholders in templates.
- **Fonts:** Switch BFFNT sheets are stored bottom row first like the Wii U ones (SMM2's number
  fonts came out upside down), and one-channel sheets are read as luminance as well as coverage
  so the SMB3 / SMW digits keep their shading [inf: matches how the font colours interpolate].

What would change it: footage of the game's course start / death / clear / give-up sequences,
the frame-capture effects, and the owner's choice on lives.
## 2026-10-18 — 3D World enemies (96, 65, 51, 50, 114, 117, 86, 107): own modules, one state struct
Ant Trooper, Stingby, Skipsqueak, Hop-Chops, Porcupuffer, Bully, Charvaargh and Piranha Creeper
live in `actors/w3.rs` and `world/w3.rs`, like the bosses: the shared files only gain variants,
match arms and one-line hooks. Their extra state is one `W3State` on the actor, which also holds
what the world senses for them each frame (the liquid, the player's height, wetness and jump).
Their guesses were burnt down against the binary (specs updated, citations there); what is still
open or not modelled:
- **Animation timing [inf ±1 frame]:** states that wait for a model animation last
  ceil(frames / rate), with the frame counts from the model files and the rates from
  `EnemyModelParams` (which of its two rate fields is the normal size is read off the 2D files).
  Model yaw turns take DirDeg·2 / 8 frames.
- **Ant Trooper:** the winged one flies as the binary's wing flyer (level at 0.5, 8-px bob, wall
  turn) and keeps its wings when stomped; the horned one's top hurts only while it walks on the
  floor (spin jumps bounce off, except off the big one's live Needle). The hover helper's
  "pressed" re-centring is not modelled (its trigger bit is unclear).
- **Skipsqueak:** the player flag it watches lasts one frame (cleared by the player's pre-update),
  read as "left the ground rising" or a stroke's kick; spring high jumps (two frames) are not
  told apart.
- **Porcupuffer:** the surface-jump window's vertical centre stays as before [?].
- **Bully:** the winged one lives on the ground and flies only as the binary does (up walls and
  ledges after the player, back onto a ledge it was knocked off); ObjJump is not modelled. The
  player's Pushed state is approximated by setting its speed (3.0, every 4th push 4.0 up); its
  control lock lives in the player code. The shell's own bounce is [inf].
- **Charvaargh:** the side leap is exact (heading curves from the model, body boxes along the
  head's path). The vertical leap rides the model's root bone (sampled every 10 frames), its body
  along the head's path (within ≈ 25 px of the posed bones) and touching only at the course's
  depth; the chained leap reads the actor's camera view (one view: the game checks every
  player's). Several Charvaarghs share the area's list (turn, spacing) through
  `World::w3_charvaarghs`, which needs a one-line call in `World::step` ahead of the actor loop;
  without it each one leaps on its own. One hook in `actors.rs`: it is exempt from the
  fell-into-a-pit kill, as it dives far below the lava.
- **Piranha Creeper:** PreStop, the trample rules (stomps, spins, slides, fireballs, shells; ground
  pound 4 pieces; 16-frame cooldown) and the head hurting while trampled follow the binary. Not
  modelled: the stalk as solid terrain (it is; whether its NeedleCube surface hurts is open) and
  tramples by blocks bumped from below — both need the world's terrain / block code.

## 2026-10-18 — Bosses (Bowser, Boom Boom / Pom Pom, Bowser Jr.): HP through one hit path
Every boss hit goes through `Actor::boss_hit(attack)`, which applies the spec's HP rules. Bowser:
fire 1, shell 3, 20 HP (big 40), and stomps are Invalid. Boom Boom and Bowser Jr.: 3 HP; a
fireball drains a 5-point counter (big 10) and costs 1 HP when it empties. Star, POW quake and lava
use the generic kill paths. The code is in `actors/bosses.rs` and `world/bosses.rs`, so the shared
files only gain variants, match arms and one-line hooks.

Update (same day, boss burn-down): the behaviour is now read from the decompiled game code (and the
animation lengths from the sprite data), state by state; the specs 62/77/98 cite each address.
Highlights: Bowser chases once the player gets past him (0.7 SMB1 / 0.9 px/f), waits in place while
off screen, stands still while breathing (35-frame SMB1 breath, flame at its end), fires into one
of three lanes 17 px apart, hops on a block bump; Boom Boom starts curled up, floats while stunned,
dashes and brakes into Defend, wall-jumps 3.75 / 3.75 / 5.0, jumps 5.5, and winged keeps its wings,
hovering near the view and diving at the player; Bowser Jr. hops by a jump solver, walks 1.25,
shift-jumps straight up, shoots 3 aimed fireballs (winged: 4 hammers), and attacks from the Clown
Car every 150 frames. What is still a guess, and why:
- A stomp on Bowser hurts the player, as on Spike Top (the other "Invalid" stamp).
- A Bob-omb blast is a Shell-strength hit [obs]: the exploding bomb's collider carries attack cc 6
  (shell) (0x7100732640). A shell that hits a boss is spent [inf].
- The held Key appears 8 px below the boss's centre, where the defeat is recorded [inf].
- Clown Car steering for riders [?] (the car's 3.8 KB Chase routine is not decoded) and riders
  only flash when hit [?]. Thrown Clown Car shells use the engine's shell launch [?].
- Bowser: the obstacle / gap jumps of the chase decision [?]; flames add his current forward speed
  (the game averages a short history) [inf]; the hip-drop quake's player stun and big Bowser's
  block bounce are not modelled (no engine hook); water timings (×1.5) not modelled.
- Bowser Jr.: the Shell context's slide length (8 KB state not decoded) [?], ReturnFromShell [?],
  the drop-down shift jump [?], non-SMB1 wind-up animations [?]; the hop arc check is sampled [inf].
- Pom Pom (3D World only): throw timing, the shuriken's flight and where she reappears [?];
  placeholder art.

What would change this: decoding the Clown Car Chase state, the boss Shell context, BulletPunpun /
Punpun's Throw and Lapse states, or Ryujinx measurements.

Pass 3 (bosses): those were decoded, and the NSMBU / 3D World animation lengths read from the
models with their EnemyModelParams play rates. Now settled: NSMBU breath 104 / turn rates; Bowser's
16-frame speed-history mean for the flame; Bowser Jr.'s wind-ups (SMB3 / SMW 4, NSMBU 25), view test
(his own box vs the view), arc test, the Shell context (200-frame slide at up to 1.6, brake-turns,
its own hit rules, ShellWakeup's hop), the Clown Car Chase steering (0.02 / 1.0 within ±320 px) and
ride heights, Bowser Jr.'s Clown DamageDown and the Koopa Troopas he throws; Pom Pom's whole throw /
hide / Lapse cycle and the homing shuriken (3W model art); a defeated boss's Key is a key event for
the player (counted at once). Still open: the Bob-omb explosion's attack type, the shell's own side
of hitting a boss, the obstacle / gap jumps (Bowser chase, the shell slide's step hop), Boom Boom
leaving the car when hit (needs a separate car actor), Pom Pom's decoys.

## 2026-10-18 — SMM1 UI: JSON + `.mtex` cache, layout player in its own crate
- **Cache form:** `mmk import-smm1` writes each layout as JSON (parsed BFLYT + its BFLAN
  animations + texture list), textures as untiled `.mtex` (same format as the SMM2 cache, so the
  Metal upload code is shared), fonts as JSON metrics + sheet `.mtex`, messages as JSON. JSON
  keeps the cache endian-neutral and inspectable, and the engine never needs the original files;
  `formats::lyt::json` round-trips every parsed field (tested). Written by hand on
  `serde_json::Value` — no serde-derive dependency added.
- **Location:** `<assets root>/smm1-v<update title_version>/` (development copy
  `~/smm-work/cache/smm1-v208/`), separate from the SMM2 cache because it is loaded lazily per
  layout (the SMM2 `Cache` loads every texture at open).
- **Everything, not a subset:** all 337 layout archives are imported (67 MB): the app menus will
  reuse them and the cost is small.
- **Fonts the game does not ship:** layouts that name the Wii U system fonts (`CafeStd_25/50`)
  get MessageFont; every font falls back per character to SpecialFont (7380 glyphs, kana/kanji),
  so user course names in Japanese render.
- **Crate split:** parsers in `formats`, import in `import`, evaluation in a new `ui` crate (no
  file I/O in the player itself; tests build layouts in memory), C ABI in `ffi/src/ui.rs`, Swift
  in `UILayer.swift` / `UIPreview.swift` — kept out of the files other work is changing.

## 2026-10-18 — Layout player: shared window colours win over texture-only materials [inf]
A window with "one material for all frames" gives its frames the first frame's colours even when
a frame's own material is texture-only (the Endless map frame's dark red corners), and a
texture-only content takes them too (SMM2's dark name balloons with white text). Layouts have no
scissor clipping yet: lists hide the rows outside their scissor instead.

## 2026-10-18 — Layout player semantics where the format leaves room (verify against the game)
Chosen from how the SMM1 data is authored and checked visually on the owner's dump:
- Pane-based texture projection maps the pane's origin point to the texture centre
  (uv = 0.5 + (x/w, −y/h)); the texture SRT translation is added. The world-map Goombas are only
  right this way.
- Parent anchor offset = (anchor factor − parent origin factor) × parent size (13 panes in the
  whole game use a non-centre anchor).
- Two-texture materials: TEV mode 0 takes tex1, 1 multiplies, 2 adds, 5 subtracts; no stage =
  multiply. Third textures are ignored.
- Colours: material and vertex colours are treated as authored sRGB values (linearised in the
  shader, sRGB target); BC4/BC5 masks are linear factors.
- Text ids resolve in the layout's own MSBT; part-property text ids resolve in the outermost
  (screen) layout's MSBT first — the difficulty buttons only get their labels this way.
- Window flag bit 4 = content not drawn (cursor frames are hollow).
Would change with a frame-by-frame comparison in an emulator.

## 2026-10-18 — App shell: working name "MMK", storage locations, shell-owned pause
- **Name:** the app is called "MMK" (bundle id `dev.mmk.engine`) until the owner picks a name. No
  Nintendo names or marks in the app name, icon or bundle id. Name, bundle id and version live
  only in `app/Sources/MMKApp/Identity.swift`; `scripts/make-app.sh` reads them from there and the
  Application Support folder is named after it, so renaming is a one-line change (existing data
  would then need moving from the old folder).
- **Storage:** `~/Library/Application Support/MMK/` holds `assets/<build>/` (importer output),
  `courses/` (the library; added files are copied in, so the library survives the originals
  moving) and `saves.json`. The older `…/mmk/assets` and `~/smm-work/cache` locations are still
  searched for an asset cache. Settings are UserDefaults keys (README "Settings").
- **Save key:** FNV-1a 64 of the decoded course data, so a course keeps its record whether it was
  added as `.bcd`, zlib or raw data, and wherever the file lives. Not a security hash; collisions
  only merge two records.
- **Pause is the shell's job:** the start/pause button no longer reaches the engine; the shell stops
  stepping it. Course clear = HUD flag bit 0 (set when the goal sequence ends); clear time runs
  from the attempt's start (course start, death restart or menu restart) to the Goal event.
  Game over at 0 lives is an app rule for now (SMM2 normal play has no lives); revisit for Phase 6.
- **Placeholder art:** without a dump the engine still runs; sprites whose texture isn't in the
  cache come out of the snapshot as `MMK_TEXTURE_NONE` and the renderer draws a box coloured by
  draw layer (only in placeholder mode or with `MMK_DEBUG_MISSING_ART=1`, so a real import
  with a missing texture looks as before).
- **`mmk_course_info` takes an assets handle** (may be NULL) in addition to the bytes: encrypted
  `.bcd` files need the key table from the asset cache.

## 2026-10-18 — Audio: game BWAVs in the cache, decoded at load, mixed in Rust (Phase 5 step 4)
- The importer copies each needed sound's BWAV unchanged into `<cache>/audio/` (outside the
  repo) with an `index.json`; the engine decodes DSP-ADPCM/PCM16 to PCM16 once when the audio
  handle opens (≈30 MB for the slice, mostly the two ground music streams). Simpler than decoding
  in the render path and loop points need no ADPCM history bookkeeping. Revisit (stream from
  ADPCM) when more themes' music is loaded at once.
- Engine sound ids → game wave names live in a code table (`import::audio::M1_SOUNDS`), with the
  game's per-sound volumes as gains (numbers, like the sim's physics constants). The mapping comes
  from the game's own sound-link data; evidence is in the private research notes.
- Mixer: 32 voices, 32.32 fixed-point positions, linear interpolation to 48 kHz, short
  anti-click fade on cuts, three exclusive groups modelled on the NES pulse 1 / pulse 2 / noise
  channels (a new sound in a group cuts the previous one), same sound retriggers instead of
  stacking, fixed ×2 output gain (the game's waves peak around 0.2 FS). The group behaviour and
  the kick sound's two takes alternating are inferred, not read from the game.
- Threads: the engine (game thread) only sends commands over a bounded `std::sync::mpsc` channel;
  the mixer is owned by the audio handle and locked only by `mmk_audio_render`. No new crates.
- The sim now emits `PowerUp` for every mushroom/flower pickup (the game plays the sound even
  when the form does not change).
- Only the M1 ground (day) music is imported; other themes play it too until their streams are
  added. `MMK_MUTE=1` disables audio in the app.

## 2026-10-18 — Camera choices where the spec is inferred (Phase 5 step 5)
The follow rules and constants come from the binary as-is (dead zone ±4 px; box −28/+40 px around
home, shrinking to ±16 while rising; 4 px vertical hysteresis; follower caps 5–8.5 px/frame
horizontally, 4.99 up / 7.99 down vertically; 216 px ceiling; 60-frame scroll-stop hold, 4 px/frame
release). Checked identical in v3.0.3. Inferred until the Ryujinx harness can measure them:
- Header boundary type ≠ 0 means "nothing built above the line" and gives the 216 px ceiling
  (strong corpus correlation with build height; blank courses have 1).
- The box-shrink curve is the identity (Bézier controls 0, 1/3, 2/3, 1); the multiplayer join
  weight is 1 for one player.
- The engine has no separate Fall state: leaving the ground with vy ≤ 0 counts as a walk-off (no
  hysteresis) until an upward impulse; jumps use the normal mode, as the game's Jump state does.
- Scroll-stop walls count any solid cell (ground and blocks); semisolids and slopes do not. Which
  block types the game counts is unverified.
- The descend-only start strategy is on at course start (the game enables it in a play mode that
  was not identified; it only matters in low-camera main areas).
- Death freezes the view (camera mode 3 → the empty cDemo state); the goal keeps following with
  the goal strategy (5 px/frame vertical cap).
- Player velocity for the followers is the player's vx/vy (the game adds a carrying platform's speed).

## 2026-10-18 — 2D sprites are placed by their BFRES quads; the sim passes model origins (Phase 5 step 3)
The importer reads each M1 model's BFRES (own minimal reader for version 0.9: skeleton rest pose,
materials and sampler wraps, shape vertices, texture-pattern animations) and writes
`sprites.json`: per texture the quad in model space (1 unit = 1 px, origin = actor origin) and
its texture coordinates; per animation the frame keys. Convention: for model sprites the sim's
(x + w/2, y) is the model origin and (w, h) the texture size times the scale; the FFI snapshot
draws the texture at origin + quad (mirrored around the origin for flip_x, flipped in place for
flip_y). The sim stays asset-free and no longer carries art offsets. Without quad data (old
caches) the texture fills the sim's rectangle as before.
Consequences taken from the data as-is: enemies sit 1 px into the ground (Goomba art offset and
Koopa bone offset agree; Mario does not), the goal flag is mirrored (mirror-repeat sampler),
the fireball is drawn 16×16 around its centre, the castle flag rests on top of the tower.
Texture-to-quad matching when several models share a material name (Koopa walk vs shell): the
quad of the texture's own size wins. Frame timings are exposed (`Cache::anim`); the sim uses them
only for the Goomba/Koopa walk (10 frames per texture at play rate 1.0 from EnemyModelParams).
What would change it: Ryujinx captures showing different placement, or code that offsets models
or scales animation rates (player walk-rate scaling is not known yet).

## 2026-10-18 — Core-object choices where the specs have no number (Phase 5 step 6)
Values read from the binary are used as-is (bump curve, coin pop, item emergence, walker speeds,
shell wake 511, kick 2.5 + 0.5·|v|, death 45-frame pose → vy 3.2 / g −0.16 / restart after 140,
damage Fire → Super → Small with 127 frames of invincibility, goal height score and slide speeds).
Where nothing was found, these stand-ins are used until the Ryujinx comparison harness can measure
them:
- Enemy stomp bounce: the player-vs-player pair 4.128 (jump held) / 3.628.
- Stomp test: player falling with the feet in the enemy's upper half.
- Grow/shrink freeze: 48 frames.
- Off-screen actors are removed when their activation box (32 px half-extents; sliding shells
  160 × 32) leaves the view + 32 px, and respawn when their home position re-enters the view;
  killed actors stay dead.
- Shell kick scores 400; stomp/shell chains use the score-type table 100 … 8000, then 1-UP.
- Goal pole stands in the goal ground's second column (X0 = left + 24): with the castle door at
  X0 + 112 the 80-px castle then ends flush with the goal ground's right edge.
- Pole touch uses body box B (a small player on the base block only touches the box with box A).
- Lifts placed off a track are static (the binary has an oscillating Move state, ±48 px, but
  which lift actor the course loader picks was not traced; course makers use plain lifts as
  fixed platforms). Verify in Ryujinx.
- Spring launch: 6.0 px/frame, or 4.43 with jump held (the held-jump gravity bands then give
  the high bounce) — the binary pairs the speeds with "jump profile" ids whose meaning is [inf].
- Side Thwomp (pass 3, from the binary): flag bits 0x18 not both set; looks one way ((flags >> 3)
  & 3 == 2 right, else left), charges at a player in its 136 px attack box on that side only.
- P-switch duration 600 frames ([ext] ~10 s; not found in the binary). Dotted-line block colour
  from the alternate flag [?]. Conveyor art uses a placeholder tile until the belt model is found.
- Simplified for now: Spike Top walks floors only (turns at ledges) instead of crawling around
  blocks; Hammer Bro alternates a 5.5 jump with a 2.5 hop (no drop-through); Bob-omb blast:
  a 2-tile diamond of terrain, and actors/players touching a half-18 box 8 px up (pass 3, [obs];
  one frame here, 16 in the game).

- Spike Top crawls round blocks (pass 3): placed side from the facing field, 30-frame corner
  turns; an outside corner swings on a quarter circle round the block corner (grid stand-in).
- Hammer / Sledge Bros jump up or down through terrain (pass 3, 0x71006205d0 / 0x71007a4b20).
- Simplified for now: Bob-omb blast radius
  1 tile (plus) for terrain and 32 px for actors/player [inf].
- Fire Bar rotation 2.8125°/frame (the shared counter's step was not located); Burner flame
  12 px wide × 48 px (texture length); one-way walls are drawn nowhere yet (no M1 texture found).
- Seesaw rider weight = 1 per px of lever arm; Grinder hurt radius 20 px; cannon diagonal
  facings (odd values of (flags >> 24) & 7) mapped as 1 down-left, 3 down-right, 5 up-left,
  7 up-right [inf]. Sprites don't rotate yet: seesaw drawn as stepped tiles, cannon barrels not
  drawn.
- Swinging claw pendulum: amplitude 60°, period 160 frames, rope 40 px (the rope simulation is
  outside the decompiled range) [inf]; Twister lift: vy toward 4.5 at +0.6/frame [inf].
- Sprite art offsets inside textures are measured from the textures (e.g. the Goomba sits in the
  top half of its 16×32 image) until BFRES quad placement is imported. (Superseded: BFRES quads,
  see the Phase 5 step 3 entry.)

## 2026-10-18 — 100 Mario Challenge uses SMM1's own presentation, like-for-like
Owner request: title/menu/world map and the before/after screens and animations as in SMM1.
Assets come from the owner's SMM1 dump at first launch (never committed). Rendered by a layout
player for Nintendo's BFLYT/BFLAN format so screens run from the original layout and animation
data. Courses themselves are SMM2 courses on the SMM2 engine. Roughly doubles Phase 6; the SMM1
importer and the layout player start in Phase 5b because the app menus reuse them.
Pending owner choice: SMM1 HUD during 100 Mario courses (default yes), languages (default EN-US).

## 2026-10-18 — For now the Swift render loop drives the 60 Hz tick
docs/architecture.md plans a Rust-owned sim thread. Until there is audio and heavier simulation,
the renderer steps the engine from an accumulator (fixed 1/60 s ticks, capped catch-up) on the
main thread. Simpler, still deterministic (ticks never depend on frame time). Revisit with audio.

## 2026-10-18 — 2D styles render as sprite quads, not a full BFRES renderer
SMB1 (and the other 2D styles) store each animation frame as its own small texture named
`<anim>.<frame>` inside the model, and terrain as one tile atlas per theme. The slice draws
textured quads; quad placement and frame timing will be read from the BFRES later for accuracy.
SM3DW will need the model renderer.

## 2026-10-18 — Engine architecture: Rust core + thin Swift shell (Option B)
Owner's choice from docs/architecture-options.md. Rust core (formats, importer, deterministic
60 Hz sim, audio mixer, modes) behind a C ABI; Swift app for window, Metal rendering,
GameController input, audio output and SwiftUI menus. Reason: native Mac app quality plus a
portable, independently testable core that leaves a Windows/Linux port open. Cost accepted: two
languages and an FFI boundary. Design: docs/architecture.md. Toolchain: Rust 1.99.0 (pinned),
Swift 6.4 / Xcode 27, macOS 14+ Apple Silicon.

## 2026-10-18 — Behavioural specs stay private; the engine is written from them
Phase 3 specs (`research/specs/`) describe behaviour in prose, tables and maths with evidence tags,
never decompiled code. They stay gitignored because they cite addresses and quote game data values
from the owner's dump. Public docs (`docs/formats/`) only carry format facts.

## 2026-10-18 — Ghidra headless directly, REA for spot checks
REA's MCP calls time out (330 s) before Ghidra finishes auto-analysing a 36 MB binary. Headless
analysis runs in ~7 min; small read-only query scripts (`tools/ghidra/`) do the rest. Parallel
agents each get a private copy of the analysed project.

## 2026-10-17 — Course key table is located at runtime, never shipped
`smmfmt.smm2.keyfind` scans the user's own executable for the 64-word table that decrypts one of
the user's own `.bcd` files (cheap one-block test, then full CRC32 + CMAC). The engine's
first-launch importer will do the same and cache the result in the user's app-data folder.
Community sources that print the table were not used for it.

## 2026-10-17 — Format code lives in a Python reference package (`smmfmt`) for now
Phase 4 chooses the engine language. Until then, `smmfmt` is the executable specification: the
engine's own parser will be tested against it (same inputs, same parsed values). Records keep
their raw bytes so unknown fields survive round trips.

## 2026-10-17 — Path arrays are slot-indexed
Snake blocks, clear pipes, piranha creepers, ! blocks and track blocks store entry *i* in slot *i*
and may be sparse; the count is the number of live slots. Found in 16 of 730 courses. Parsers that
read only the first *count* slots lose entries.

## 2026-10-17 — v3.0.3 is the behavioural target; v1.0.0 is the mapping aid
The engine targets SMM2 3.0.3 (final) behaviour and assets. Community RE notes (e.g. Treeki's
SMM2Reversing, July 2019) were written against v1.0.x, so Phase 3 maps functions in the v1.0.0
binary first and carries names across to 3.0.3 by function matching. The first-launch importer
will take the base dump plus an optional update dump.

## 2026-10-17 — Native app milestone (Phase 5b) before the 100 Mario Challenge
Owner wants a proper native macOS app before the 100 Mario Challenge work starts. Phase 5b adds the
app bundle, import screen, course library, pause menu, settings, save data and per-course
"playable" marking. Phase 6's course pool is limited to playable courses and grows with Phase 7
(alternative: do Phase 7 first; rejected for now because it delays the mode a lot).

## 2026-10-17 — Course sample from the public mm2_level dataset
TGRCode's API returned 502 and SMMDB served a self-signed certificate. TheGreatRambler's Hugging
Face dataset (26.6M courses, Feb 2022, CC BY-NC-SA 4.0) is read remotely with DuckDB range requests.
It also carries Nintendo's own difficulty bucket and clear rates, useful for Phase 6. Shard 0 is
used first: its uploads predate game v2.0, matching the v1.0.0 dump. Courses stay outside the repo.

## 2026-10-17 — SMM2 extracted with hactool rather than Ryujinx's Extract Data
Same output, but scriptable and repeatable. Keys are read in place from Ryujinx's folder.

## 2026-10-17 — SMM2 RomFS lives on the external SSD
Internal disk had ~1 GB free. `~/smm-work/extracted/smm2-romfs` is a symlink, so paths are unchanged.

## 2026-10-17 — CDecrypt patched to take the common key at runtime
Upstream CDecrypt embeds the Wii U common key. Rule: key material only comes from the owner's
console. The local build reads `$WIIU_COMMON_KEY_FILE` instead. Patch kept outside the repo.

## 2026-10-17 — SMM1 reference build is update v208, not the disc's v16
The disc ships base v16 plus update v208 (in the GI partition). Only v208 contains Super Expert
in the 100 Mario Challenge, so v208's `Block.rpx` and content are the reference for Phase 6.
Base content + v208 overlay = the effective game.

## 2026-10-17 — NSO loading via Ghidra Switch Loader built from source; nx2elf as fallback
Upstream Switch Loader release targets Ghidra 12.1.2; extensions must match Ghidra exactly, and
REA is configured against 12.1.4. Built 1.6.1 @ 5d80d195 against 12.1.4.

## 2026-10-17 — hactoolnet dropped
Its source repository is no longer on GitHub; hactool 1.4.0 + nstool v1.9.2 cover the need.

## 2026-10-17 — Proprietary-content guard checks content, not just extensions
Extension lists miss renamed files. The guard also checks magic bytes (Yaz0, SARC, zstd, BNTX,
BFRES, MSBT, BYML, NSO, XCI, WUX, RPX...), key-file shapes, and a 5 MiB size cap. Runs in CI and
as a pre-commit hook.

## Open
- Licence for the repository (owner's call).
- Engine language/framework: deferred to Phase 4 per plan.

## 2026-10-18 — 100 Mario over a local library: three deliberate departures from the server
SMM1 refuses to start when the first search returns fewer than 32 courses; a local library is
far smaller, so any non-empty band starts a run (`Pool::with_minimum`). Courses already cleared
in the run are not offered again while others remain (the server has no such memory, but with a
small library repeats would dominate). The castle search (last course) is read as "castle theme"
and falls back to any course. Difficulty value = 100 − clear rate (the same fail-rate scale the
SMM1 bands use); courses without metadata get 30, so they appear only in Easy and Normal runs.

## 2026-10-18 — Tracks as a geometric graph, not the game's per-cell rail grid
The game expands each track record into a 3×3 (5×5 for junctions) template of tile codes and walks
a cell grid. The engine builds the same paths as line and arc pieces joined at shared end points.
The geometry was fitted to the corpus: object reference points land on the pieces they link to.
Riders keep their offset from the track point and move 0.75 px/frame (1.5 winged). The pieces
are 32 px long; only free ends reverse. Still guessed: which way curves bulge, the start direction
field, and open-vs-closed ends (all ends reverse). Junctions aren't built. Tracks are drawn with
white texels from the field atlas, a placeholder for the real per-cell track tiles.
**Superseded (pass 3):** the engine now builds the game's own cell grid from the record templates
in the binary and walks it with the game's follower rules (track.rs header), junctions included;
the public `Tracks` / `Rail` API is unchanged and world.rs only passes the ON/OFF bit in.

## 2026-10-18 — Cross-platform shell (owner asked for Windows, macOS, Linux, web and iOS)
The Rust core has no platform code, so it ports as is. Two ways to reach the platforms the
SwiftUI/Metal app can't (Windows, Linux, web):
- **chosen:** a portable Rust shell (winit + wgpu + gilrs + cpal; wasm32 for the web) that runs
  the same core and draws everything itself, including the menus (layout player). The Swift app stays
  for macOS/iOS.
- **rejected:** a separate native app per platform. It would mean five UIs to keep in sync.

Menus move from SwiftUI to engine-drawn layouts in both shells. That's what the like-for-like goal
needs anyway, since SMM1 and SMM2 menus are layouts. Proposal; it can be revisited before Phase 8
starts.

## 2026-10-18 — Online: an open-source server, never Nintendo's
The owner wants multiplayer through an open-source server. The engine isn't the Switch game, so it
doesn't need Nintendo's NEX protocol.
- **Proposal for course sharing:** our own open-source server (Rust, MIT/Apache), self-hostable,
  with an HTTP API. It covers courses, search, likes, makers and Endless/100 Mario pools, and can
  import from SMMDB.
- **Proposal for real-time play:** rollback netcode on the deterministic sim over WebRTC
  (open-source matchbox signalling), so desktop and browser players can join the same game.

⏳ Open for the owner: whether they already have a specific open-source server in mind.

## 2026-10-18 — SMM1 UI cache location
The app looks for the SMM1 UI import (`mmk import-smm1`) in `MMK_UI_CACHE`, then
`<Application Support>/MMK/assets/smm1-<build>/` (next to the SMM2 import), then the development
cache `~/smm-work/cache/smm1-<build>/`; newest build name first (`AppPaths.findSMM1Cache`). No
import → the SwiftUI stand-in 100 Mario screens. The in-app importer doesn't run the SMM1 import yet.

## 2026-10-18 — 100 Mario screens: SMM1 layouts, inferred choreography
The layouts and their animations are the game's; how the game strings them together is not in
the data, so these are inferred and marked [inf] in `ChallengeScreens.swift`:
- **Difficulty select = tabs over the world map.** `Nwk_ListTitle_00` and the map both have a
  300-frame MapIn whose frame 0 shows the tab bar above an un-zoomed map with New Game / Continue
  [obs]. Confirm plays MapIn (bar leaves, map zooms, curtains close) [obs keys]; left/right swap
  maps with OutL/OutR + InR/InL.
- **Locked tiers are shown greyed** (button "Invalid"), per the owner's brief. The game adds tabs as
  they unlock (`OpenType` 0/1/2 = 2/3/4 tabs) [obs], so this is a deliberate departure.
- **Selected tab = the button's Decide animation held at its end** (dark button, pointer below).
- **New Game:** MapIn; at 200 the banner (Telop) while the Goombas carry Peach (MoveKuribo); then
  Peach on the castle (AnimB); Mario jumps out of the start castle and walks to dot 1 (MoveMario);
  Out; intro card. **Continue:** MapIn with Mario on his dot, Out, card. **After a clear:** In,
  the cleared dot's Clear pop, Mario walks one dot, Out, card. **After a skip:** straight to the
  card. Mario's stops are the MoveMario frames where his pane reaches each dot's position (the
  animation authors one walk along all dots); the walk plays at the authored speed.
- **Cleared dot = `Type` frame 1** (shows the clear disc).
- **Intro card:** one small Mario per life; the formation is 100 and AnimB sends #99 forward, so the
  hidden ones are #0…#(99 − lives). After AnimA + AnimB the card holds for the length of its Out
  animation (150; Out and OutMiss only key the fade to 0, so they act as timers). After a death:
  AnimB + OutMiss (60) over the paused game. Confirm skips (A skips since 1.20 [F]).
- **Pause:** `Cmn_PauseMenu_00` Type 6 (Start Over + exit, no network buttons); the exit button
  shows the table's "Save & Quit" text. Skip is not a pause item in the layout version: SMM1 skips
  with the held button. The selection starts on the close (continue) button.
- **Course clear:** `ModeType` 0 (Save & Quit + Next, no recommendations); on the run's last course
  `ClearType` 1 (Next alone). Star, comments, recommendations, Miiverse, Mii and flags are hidden:
  there is no data for them. "World Record" is the dataset's `world_record` when the index has it.
- **Skip:** hold for 3 × 32 frames (each CountDown step), then Pass (32) and the course ends.
  Default keys − (minus) and Tab, controller Options (−); bound like the other actions.
- **Ending:** the rescue scene isn't a layout, so the ending text balloon plays over the finished
  world map (Mario at the castle). Text: `T_ThankYou_10` (Easy), `_20` (Normal), `_30` (Expert,
  Super Expert); credits `Nwk_StaffCredit_01` (8 entries, Easy), `_00` (16, Normal and Expert),
  `_03` (6, Super Expert), chosen by their entry counts.
- **Layout player:** a child without parent-anchor bits sits on its parent's origin point (fits
  the castle flags and the balloon text; the earlier "parent centre" rule floated them), and an
  "influenced alpha" pane fades all its descendants, parts included (Mario's fade-in on the map).

## 2026-10-18 — Portable shell: shared `host` crate, wgpu, our own BC7 decoder
- **`host` crate:** sprite placement (`place`, the resolver), clear colour, course bytes decoding
  and the sound director moved out of `ffi` into `core/crates/host`, so the Swift app (through
  `ffi`) and the Rust shell run the same code. `ffi` re-exports `place`, and `MmkAudio` is now an
  alias of `host::audio::AudioOut`, so the C ABI is unchanged. New sim events get their sounds in
  `host/src/audio.rs` (formerly `ffi/src/audio.rs`).
- **Stack:** winit 0.30 (latest stable; 0.31 is still beta), wgpu 30, gilrs 0.11, cpal 0.18. One
  wgpu renderer covers Metal, Vulkan, DX12, GL and WebGPU/WebGL2.
- **BCn fallback:** GPUs without BC support (WebGL2, mobile) get CPU-decoded RGBA8:
  `formats::bcn` for BC1–5, and a small BC7 decoder of our own in the shell, written from the
  Khronos format specification, instead of a new dependency. `mmk-play --check-textures` checks
  it against the GPU (every BC7 texture of the owner's cache matches exactly). BC6H is not
  decoded; the caches have none.

## 2026-10-18 — CI runs Linux only on push
GitHub Actions minutes were close to the 2000-minute limit, and macOS minutes bill at 10×.
Pushes now run only the Linux jobs: Rust fmt, clippy and tests; the Python tests; the
proprietary guard. The macOS (Rust and the Swift app), Windows and wasm shell jobs run when started
by hand (`workflow_dispatch`). A newer push cancels the older run. `scripts/check.sh --app`
still runs every gate locally, macOS app included, before each commit.

## 2026-10-18 — Textures load lazily
With all five styles imported, the cache holds 12,540 textures (729 MB). Loading and uploading
all of them took about 2 GB of memory at start-up. Now `Cache::open` reads texture sizes from
`textures/index.json`, and a texture's pixels only when first asked for (`Cache::texture`). Both
renderers upload a texture when a sprite first uses it. Measured on the same course and frame:
- portable shell: 2.0 GB → 107 MB peak, start 1.1 s → 0.5 s;
- macOS app: about 275 MB peak.

`Cache::load` through a reader function, used by the web and by tests, still loads everything.

## 2026-10-18 — Web build: game data in OPFS, streamed; both import paths in the browser
- **Storage: the origin private file system (OPFS), not IndexedDB.** It holds the cache as
  files (about 1.3 GB, 15,944 files without the unused `meshes/` and `anims/`), a picked `File`
  is written without passing through JavaScript memory, and reads come back per file. Copying
  the owner's `mmk import` cache takes about 15 s in Edge. Nothing is uploaded; **Remove game
  data** deletes it. Rejected: keeping a `showDirectoryPicker` handle to the owner's folder
  instead of a copy (Chromium only, and every visit asks for permission again).
- **The whole cache never sits in memory.** OPFS reads are asynchronous and the game loop is
  not, so `Cache::load_deferred` reads only the index and the shell streams texture files on
  first use (`take_requests` / `provide`; the session holds up to 30 frames while reads are on
  their way, then plays on and the art pops in). Course music is read before the course starts.
  `Cache::load` (everything up front) stays for tests and small caches.
- **The dump import runs in the browser too, with the real importer.** Measured natively, it
  peaks at about 1.3 GB, which fits in wasm32's 4 GB. `mmk-import` builds for wasm32; its file
  access goes through `mmk_import::vfs` (a thread-local override, so the stage functions keep
  their signatures). The page runs it in a module worker, because only workers can read picked
  files synchronously (`FileReaderSync`). The worker posts each output file to the page, which
  stores it, because workers can't wait for storage either. In headless Edge it takes about
  35 s and the Edge process peaks near 1.5 GB. The output matches the desktop import (same file
  list; 169 sampled files byte-identical). Copying a cache made on the desktop is still the
  lighter way, for machines with little memory and for mobile browsers.
- **Sound: cpal's WebAudio backend** (the `wasm-bindgen` feature; `futures-channel` was already
  in the local cargo cache) rather than our own AudioWorklet. The native and web shells share
  `audio_out`. The stream opens on the first pointer or key event, since browsers block audio
  before a gesture. It schedules buffers from the main thread, so a long main-thread stall can
  click. An AudioWorklet needs wasm threads (cross-origin isolation) to share the mixer.


## 2026-10-18 — Online play: relay rooms on the course server; browsers over WebSocket
- **Rooms, not addresses.** Players join a named room (≤ 4) on the course server's relay instead
  of typing each other's IPs. The relay tells members each other's public UDP addresses; peers
  probe each other (hole punching) and talk directly once a probe gets through, relayed until then.
  Direct `--net-bind/--net-peer` stays for LAN play.
- **Browsers join the same rooms over a WebSocket** (`mmk server`, HTTP port + 1), not WebRTC.
  Native WebRTC would need a large stack in every native build for web↔native cross-play; a
  WebSocket bridge into the UDP rooms gives cross-play now and works through any NAT or firewall.
  Cost: TCP head-of-line delay for browser players (rollback absorbs some of it). WebRTC data
  channels stay a possible later upgrade for browser↔browser latency.
- **Start sync and resends.** A session never runs more than `MAX_LEAD` (10) frames past the
  inputs it has from everyone, so whoever starts first waits, and packets carry acks so inputs a
  late or lossy peer missed are resent (up to 64 per packet). Found by a two-browser test where
  the late joiner missed the first frames and both sides stalled.
- The relay holds no accounts or course data; rooms expire 30 s after their members go quiet.

## 2026-10-18 — Replays: verified records, and when to bump SIM_VERSION
- A clear can be proven by its replay (per-frame inputs, `mmk_modes::replay`). The server plays it
  on the stored course and takes the clear time from the simulation, so a record can't be typed
  in (`mmk server --verified-records` makes that the only way). The record holder's replay is
  served for ghosts.
- A replay only means something for the sim that recorded it. `replay::SIM_VERSION` is stamped into
  every replay and servers refuse other versions. While fidelity passes keep changing physics,
  bump it **once per release build**, not per commit; a server upgraded to a new release keeps
  its old records but needs new replays for new ones.
- Replays play one player, offline, from the course start (deaths and respawns are inside the
  deterministic run; a restart starts a new replay). Verification runs on the server's request
  thread (a 10-minute run simulates in well under a second in release builds).

## 2026-10-18 — The sim's float math is platform-independent
- Online peers on different systems step the same world, and the server (Linux) re-simulates
  replays recorded on any platform, so the sim must produce identical bits everywhere.
  Arithmetic and `sqrt` are exact IEEE operations; transcendental functions (`sin`, `cos`,
  `atan2`, `hypot`, …) come from each platform's math library and differ in the last bits.
- The sim calls them through `mmk_sim::fmath::DetMath` (`.dsin()`, `.dcos()`, …), backed by the
  pure-Rust `libm` crate (musl's algorithms), identical on every target including wasm.
  `core/crates/sim/clippy.toml` forbids the `f32`/`f64` platform versions in the sim, so CI and
  `scripts/check.sh` reject new uses. Rust never fuses multiply-adds on its own, so no FMA issue.
- Effect when switched: 1 of the 550 sample courses ended in a different state (its trig hit a
  last-bit difference between Apple's library and musl's) — the case this prevents across
  machines. Front ends (rendering, audio) may keep using the platform functions.


## 2026-10-18 — Course Maker writes what real course files contain
- **Ground truth is the file, then the binary.** Every encoding the editor writes (pipe anchors
  per direction, warp ids, door / warp box pairs, link ids, track records and their end codes,
  path nodes, big parts' doubled size, rider positions, management flags) was checked against the
  550 sample courses; the per-part sizes, forms and composition flags come from the game's own
  editor part table. Where the sample is irregular (track joint codes at diagonal bends) the editor
  writes the most common value per case; the engine reads tracks from geometry, so play is
  unaffected either way.
- **Rules are data where possible.** Overlap is decided by layers (semisolid-like parts behind,
  ground-embeddable parts such as Monty Moles and claws, everything else solid), chosen because
  that is what the sample shows; the start zone (columns 0–6 up to three rows above the start
  ground) and the goal ground are protected because no sample course puts anything there.
- **Limits per area, as the game counts them.** The editor's part counters (general, power-ups,
  plain blocks, terrain, and per-part ones) and their limits come from the v1.0.0 placement check;
  a block or coin with wings or a link moves to the general counter, as in the game. There are no
  per-course counters (the often-quoted "2 checkpoints" is 1 per area).
- **Edits are all-or-nothing.** Paste and move check every placement and restore the course on
  failure; the UI wraps each action in one undo checkpoint and drops it when nothing changed.
- **Editor-laid tracks are straight or 45°.** Bends and junctions in opened files are kept and
  their end codes left untouched; laying them waits for the game's bend geometry.
- **Paths are stored as steps.** The editor takes a heading and a list of steps and encodes the
  node codes (straight, corner, end) itself, so the UI and the file can't disagree.

## 2026-10-18 — SMM1 extras are engine options, by name
- Phase 11's SMM1 behaviours are fields of one `mmk_sim::Options` value, each off by default; with
  all of them off the simulation is the SMM2 one (every existing test passes unchanged). A world
  keeps its options across deaths and restarts; hosts hold one value and hand it to every world
  they make (`World::set_options`), so a course load or restart can't drop a setting.
- Hosts address options by name and integer (`Options::set` / `get` / `apply`): the FFI has one
  setter/getter pair (`mmk_engine_set_option(engine, "smm1_shell_kicks", 1)`), the command lines
  take `--option NAME=VALUE` plus a flag per option. A new option needs no new entry point and no
  C struct layout change (the app's header is owned by the Swift side).
- Options follow the binary, not memory: the request said "pipes drawn over the player", but
  SMM1 draws the player in front of pipes in play; the option is `smm1_layering`, SMM1's whole
  depth ladder. The shell option likewise carries everything SMM1's shell code does differently
  from the engine (window, no-bounce kick, kicker windows, re-kick, wall assist).
- SMM2 courses contain no Weird or Mystery Mushrooms, so `smb1_mushroom` turns every Super
  Mushroom item of an SMB1-style course into one (SMM1 made them by shaking the Super Mushroom,
  SMB1 only). That includes the Super Mushroom a small player gets from a Fire Flower block — the
  simple rule, kept on purpose.
- Costumes are numbered as SMM1 numbers them (0–99, amiibo 100 + n); the importer reads that order
  from the owner's executable, so no costume list is in the repository. The sim only knows ids
  and draws `SMM1_Costume_<id>`; random picks come from the costumes the host has art for
  (`Options::costume_pool`, set by the FFI engine and `mmk-play` from the merged cache).
- The SMM1 art and sounds are a separate asset cache (`<smm1 cache>/sprites`) merged into the
  SMM2 one at load time (`Cache::merge`); without it the extras still play, drawn with SMM2
  stand-ins (small Mario, Super Mario, the Super Mushroom).
- Options are not yet part of replays, rollback sessions or `World::checksum` input: a replay
  or an online session is only valid with all options off (⬜ carry them in both).

## 2026-10-18 — Story Mode and the Dojo run the game's own event flows
- **The flows are the rules.** SMM2 keeps almost all of Story Mode (which jobs are offered when,
  build order, givers, crises, ending, post-game) in `Pack/QuestEventFlow.pack`, and the Dojo's
  lessons in `EventFlow/lesson_*`. The engine reads them at import time from the owner's dump
  (`eventflow/*.bfevfl` in the asset cache, never in git) and runs them on a small interpreter
  (`mmk_story::flow`) instead of re-coding them; our code is the `QuestSystemActor` rules the flows
  call (story-mode.md §2–§6) and a request / answer loop for the screens. Hand-coding the story
  from the tables was the alternative; it would have meant transcribing 3 900 lines of flow.
- **Forks run sequentially.** SMM2 uses forks to overlap presentation (a fade with a camera move);
  running each branch to the join in order keeps the logic and lets a screen without the 3D hub
  skip presentation entirely.
- **Every UI and presentation action is a `Request`** (message, job list, build menu, reward
  window, enter course, staff roll, save, or `Present(call)` for camera / animation / fade / wait /
  demo). Pure state changes happen at once. A screen answers questions (`answer`), the lists
  (`order_job`, `order_build`) and calls `step` again; the 2D hub lists `hub_things()` (map
  objects whose gates hold) as the places to talk.
- **Story state is our JSON, with quest.dat's meaning** (per-job / per-build bits, days, coins,
  ordered job, one-time bits, gear, talk words, named flags) plus the two unlocks SMM2 keeps in
  the main save. Session-only values (selected build, pop-up queue, last result) stay in memory as
  in the game.
- **Move flags settle whenever the hub is rebuilt** (after a course, after a load), which is when
  SMM2 rebuilds the Story scene from its save buffer; that is how NPCs "move on" between visits.
- **Table columns by CRC32.** The Story / lesson tables have Japanese column names and enum values;
  the importer matches them by the CRC32 of the name and writes English JSON (`story/*.json`,
  `lessons/index.json`), so no game text is in the source.
- **Inferred, to verify in Ryujinx:** Try Again after TOO BAD restarts with 5 lives; the attempt
  counter behind the "struggling" play type counts misses (its writer was not found);
  `checkFirstTalked` / `checkFirstTalkedFromPowerOn` are per-NPC "talked this visit / since
  start"; `checkBuildEndTriggerForBuildChief` is the lowest finished build the Chief has not yet
  announced; `checkBGMChangeProgress` (manager +0x80) answers 0.

## 2026-10-18 — Endless Challenge on SMM2's rules, server-held runs
- **Separate from `hundred`, shaped like SMM2.** The run record is authoritative, as Nintendo's
  server is in SMM2; the offline `Session` applies the same `RunRecord::report` the server does,
  so a local save mirrors the server.
- **Starting lives 5** [ext]: the binary has no client default (the server sends it); kept as one
  constant per difficulty so it can follow a verified value.
- **Quit restores the client's counters to the snapshot, but the saved record keeps lives already
  lost.** The binary reloads lives from the server record on a retry, so the server counts each
  miss; quitting cannot refund lives.
- **Reports carry what the course gained**, not run totals (coins and score since the snapshot,
  before the bonus); the record adds the time bonus from `frames_left` and caps pending at 3.
- **Pick rule.** Bands by clears ÷ attempts with at least 20 attempts: Easy ≥ 34 %, Normal 12–34 %,
  Expert 3–12 %, Super Expert < 3 % [inf]; uniform within the band; only an empty band widens
  (±1, ±2, ±3, then unrated), and that is reported. Courses drawn into a run are never drawn again;
  never the player's own or banned courses; `next` is idempotent and Continue replays the course
  being played. Endless plays feed the courses' statistics (a play on first report, an attempt per
  miss or clear, a clear without a record time).

## 2026-10-18 — Coursebot slots and Dojo state
- **The game's slot indirection is kept**: a 180-entry slot table maps course number → file
  number, so a drag is a swap of table entries (My Courses only, as in v3.0.3) and files are never
  renamed. Coursebot metadata (name, description, style, clear check, upload code, Downloaded
  stats) lives in the Coursebot index keyed by file number; hosts write name / description into the
  course header when they hand a slot to the editor or the uploader. Files: `course_data_NNN.bcd`
  (the course bytes as the engine loads them), `course_thumb_NNN.png`, `course_replay_NNN.dat`
  (our replay format, dropped when the slot is overwritten). Overwriting needs an explicit
  confirmation from the screen.
- **Dojo completion** is 128 bits (stored as a hex string); only Maker Lessons set bits, Maker
  Basics sets its own flag (which also grows the title-course rotation from 8 to 10), power-up
  lessons set nothing; Nina's introduction is one flag for every lesson kind (save +0x100 bit 10).

## 2026-10-20 — Shader prefetch: a submission's first-use builds start side by side; draws wait, never skip
**Decision:** before a submission's lists run, both backends walk them on a copy of the executor state and start the
programs, pipelines and compute programs their draws need, on worker threads; a draw waits for its own build (on
Vulkan running it itself while it's still queued). Default; `LARECOMP_NO_SHADER_PREFETCH=1` keeps R1's inline builds,
`LARECOMP_ASYNC_SHADERS=1` LAS's skipping. Design: `docs/superpowers/specs/2026-10-20-shader-prefetch-design.md`;
numbers: `recomp/docs/smm2-bringup.md` ("Shader prefetch").
- **Why:** R1's blocking compiles kept the one-shot renders but stalled for the sum of a load's compiles (a 19.6 s
  frame cold on the Mac); prefetch keeps the rule (no skipped draw) and stalls for about the slowest instead: cold
  worst frame 19.6 -> 2.8 s (Mac), 6.9 -> 2.6 s (iPad); warm unchanged.
- **The scan is advisory:** a key it gets wrong only means that draw builds its own and waits, as before; the scan
  and the draws share the key functions (`apply_state`, `program_keys`, `pass_layout`, the pipeline key), and the pass
  formats it predicts are checked against the real pass on every pipeline miss.
- **Not done:** Metal's translation stays on the game thread (0.14 ms a shader, 22 ms on the worst load frame,
  measured); no record-time scan (more lead time, guessed state); no start-up build of the archive descriptions.
**What would change it:** a game whose one submission carries less than a frame (lead time too short: then scan at
`EndRecording`), or a scan cost over 0.5 ms a frame on a target (then memoise lists that found nothing).

## 2026-10-19 — Android: guest window at 0x800000000, game code built on the Mac, APK without Gradle
The recomp builds for Android arm64 (`make -C recomp TARGET=android-arm64 GAME=smm2 apk install push-data`; how-to in
`recomp/README.md`, "Building (Android)"). The choices:
- **The guest window moves to 0x800000000-0xB20000000 on Android** (`GUEST_ORIGIN`, the mechanism the iOS work added:
  the game code is generated for that base into `generated/smm2-0x800000000`). Android's arm64 kernels commonly give
  apps a 39-bit address space (user space ends at 0x8000000000) and map libraries top-down from its end, with up to
  ~64 GB of randomisation, so 0x7000000000-0x7320000000 falls where libraries and the zygote's mappings can already
  sit, the same for every app forked from it, and an app can't re-exec for a fresh layout. Nothing maps low by default
  (ART's heaps are under 4 GB, executables at ~0x55_5555_5555), and the window still fits the guest's 39-bit pointers.
  It is claimed with `MAP_FIXED_NOREPLACE` from a constructor in `libmain.so` (when the activity loads it, before
  SDL_main); if the claim fails, the app says so and logs what is mapped there. Evidence [obs]: the API 35 arm64
  emulator (kernel 6.6, 4K pages) has a 39-bit space (highest mapping ends at 0x7fd2c01000) and the low window is
  claimed. [obs] Whether the default window would have been free changes from boot to boot: one emulator boot had
  nothing mapped in 0x7000000000-0x7320000000, the next had 742 mappings (6.4 GB) there, so the desktop base would
  have failed on it while the low window was claimed both times.
- **Game code compiled on the Mac, shipped in the APK** (as on iOS: no compiler on the device). It is a separate
  `libsmm2game.so` (143 MB) linked against `libmain.so` (the runtime, SDL3's entry, static libc++ and shaderc), loaded
  by name so it can stay inside the APK (`extractNativeLibs=false`, 16 KB-aligned). The APK carries code made from the
  player's dump: their own devices only; `scripts/check_no_proprietary.py` blocks `.apk`, `.aab`, `.dex`, `.ipa` and
  keystores.
- **Android 13 (API 33) minimum:** the Vulkan backend needs 1.3 (dynamic rendering, synchronization2, timeline
  semaphores), and the NDK's `libvulkan.so` exports the 1.3 entry points from API 33.
- **No Gradle:** `tools/make_android_apk.sh` runs javac, d8, aapt2, zipalign and apksigner on SDL's own Java activity
  (`org.libsdl.app.SDLActivity`) and a generated manifest, signed with a local debug key. Fewer moving parts than an
  Android Studio project, and the same make-driven pipeline as the other targets.
- **shaderc built from Google's sources** (`tools/android_deps.sh`, v2026.1 with its pinned glslang / SPIRV-Tools): the
  NDK's bundled copy (2022) rejects `GL_EXT_texture_shadow_lod`, which the shader translator emits.
- **Game files in the app's external files folder** (`/sdcard/Android/data/org.smcraft.smm2recomp/files`), pushed with
  adb (`tools/android_push_data.sh`, links followed, then opened to the app: adb's files belong to the shell user).
  Not bundled in the APK: 3 GB, and the APK should hold as little of the game as possible.
  2026-10-23: the internal folder is used instead when the game files are in it (sm35recomp a5e5395): Android 11+
  may refuse the app what adb put in the external one. Put there through `run-as`, so only for debuggable builds;
  chosen over a SAF / file-picker import (needs Java code in the APK and copies 3 GB on the device).
- **Tested in the emulator only** (the owner has no Android device): the arm64 API 35 image with `-gpu host`, whose
  Vulkan is gfxstream over MoltenVK. Two things that only showed there were fixed for all Vulkan targets rather than
  worked around: GLSL helpers named like Metal built-ins (`fmin` / `fmax`, ambiguous once MoltenVK turns SPIR-V into
  MSL), and the emulator keyboard counted as a gamepad (it hid the on-screen controller; Android only). The
  SwiftShader mode is not an option (no 8- / 16-bit storage).
- **BC textures decoded on the CPU** where the GPU lacks them (most phone GPUs), in the Vulkan backend as on iOS
  (`runtime/nvn/bcsoft.c`); `RECOMP_BC_SOFT=1` forces it for testing.
Would change: a device whose low window is taken (move `GUEST_ORIGIN`, or probe several bases at run time, which
would need base-independent game code); a store build (then the game code would have to be built on the device or
by the player, as the desktop packages do).

## 2026-10-19 — Recomp R2: replays keyed to the player's update, compared record for record with Ryujinx
Input replays run in the recompiled game and in Ryujinx and stay identical (findings: `recomp/docs/smm2-bringup.md`,
"Input replays"). The choices:
- **The clock is the game's own player update, at the same instruction in both runtimes.** The Ryujinx trace build
  turns the first instruction of the player's per-frame update (after the prologue) into an SVC; the recomp patches
  the same instruction into a host call. Both record the player there and apply one script frame per update, so
  record k is the same game frame in both and nothing depends on wall time, frame pacing or host speed. Menus, where
  no player exists, use presented frames instead (`RECOMP_PAD_CLOCK=frame`).
- **`GetNpadStates` is answered by the runtime**, not by filling hid shared memory for the SDK to read: the HLE layer
  already answered `GetNpadState`, and one sample per call gives each game read exactly one new state (no host-timer
  sampling to race with).
- **One session per course, not one boot per test.** Every test of a course kind plays in one script with resets
  between tests (run into the course's left edge, then a fixed walk), sized from a recomp pre-run and then identical
  in both runtimes. Boots are the slow part (Ryujinx ~20 s); the edge makes each test's start state exact.
- **Randomness is pinned in both runtimes for the game's own courses** (`SMM2_SEED_ALL` / an IPS patch of the tick
  read in `sead::Random::init()` on Ryujinx). The test courses don't need it.
- **Comparison is bit for bit** on position, speed, facing, state and frames in state, plus the whole player object
  with pointers compared by target. A tolerance would hide exactly the lifter / FP bugs this is meant to catch.
Would change: a game path with no player update (e.g. comparing menus or the editor) needs another shared hook point.

## 2026-10-19 — Recomp R1: blocking shader compiles, repeatable boots, like-for-like frame captures
The recompiled game reaches and renders the title screen on macOS (findings: `recomp/docs/smm2-bringup.md`).
Three choices made on the way, for the ports that follow:
- **First-use shader compiles block** (LAS compiled in the background and skipped draws until ready). SMM2 renders
  some things exactly once (the colour-correction LUT, light probes); a skipped draw is never redone, and the scene
  stayed black. The warm-up cache keeps later runs fast; `LARECOMP_ASYNC_SHADERS=1` keeps LAS's way for comparison.
- **Repeatable boots through the game's own seed.** The title course (and other randomness) comes from a
  `sead::Random` seeded with the system tick. `SMM2_SEED=<n>` fixes that one seed in the game glue
  (`games/smm2/seed.c`) rather than making the clock fake: frames are then bit-identical from boot to boot, which
  frame comparisons and the input-replay milestone (R2) need.
- **Frames are compared the way they are captured.** Ryujinx's reference frames are window grabs, which the window
  server colour-manages into the display's space; our raw frames are sRGB bytes. Ours are grabbed the same way for
  comparisons (`LARECOMP_CG_SHOT`), at the same size (1280x720 game area). Raw-against-grab differs by a uniform
  shift that would hide real differences (26.6 % of pixels against 6.96 % like for like).

## 2026-10-18 — Native SMM2 port by static recompilation: base on LAS Recomp, borrow from the Wonder framework
**Goal (owner):** a native port of Super Mario Maker 2 v3.0.3 for macOS, Windows, Linux, iOS/iPadOS
and the web, built by static recompilation from the player's own dump, with SMM1's 100 Mario
Challenge and Mystery Mushroom costumes as mods. It lives in `recomp/`; SMCraft stays as it is.

**Survey** (r/decomps posts named by the owner, checked against the projects' own repos, Oct 2026):
| Project | Method | Reuses / provides | License | Platforms |
|---|---|---|---|---|
| SwitchRecomp (Super Mario Odyssey) | static ARM64→C, own Horizon layer, NVN→Vulkan/OpenGL | its own runtime | not released (no code) | PC; Android announced |
| SuperMarioWonderRecomp ("SwitchGamesRecomp") | static AArch64→C++; shared `runtime/` + per-game `games/<g>/` (Wonder, Hades) | kernel/IPC/`nn::*` HLE written for it; NVN replayed into Vulkan (MoltenVK on Mac); Maxwell shaders → SPIR-V offline with the **Ryujinx translator (MIT)**; SDK AudioRenderer in process | MIT | macOS arm64 (tested); Windows planned; Android in progress; no iOS/web |
| LAS Recomp (Link's Awakening, 2019) | static AArch64→C (Python lifter, Capstone; Unicorn for jump tables), **recompiled on the player's machine at first launch** | own kernel/services/NVN; **native Metal** (macOS) + Vulkan (Win/Linux); envytools-based Maxwell shader translator; SDL3 | MIT | macOS arm64, Windows x64, Linux x64 (aarch64 experimental); no iOS/web |
| SkyRecomp (Skyward Sword HD) | LAS Recomp's pipeline retargeted to another game | LAS runtime + NVN layer | MIT | Windows, Linux, macOS (Metal) |
| HD-2D A Link to the Past (WIP) | fan remake on the SNES decompilation (not a recomp) | — | — | — |

**Decision:** static recompilation, built on **LAS Recomp** (fork into `recomp/`, MIT, upstream commit
recorded in `recomp/UPSTREAM.md`), borrowing from SuperMarioWonderRecomp where it is stronger.
- Nothing public handles SMM2 out of the box; SwitchRecomp has no code to build on.
- LAS Recomp has already been retargeted to a second game (SkyRecomp), so its recompiler and runtime
  aren't welded to one title. Its SDK/NVN generation (2019) matches SMM2 v3.0.3, whose libraries are
  tagged SDK 7.3.2 / NEX 4.6.2 (2019) — Wonder targets a 2023 SDK, and NVN is resolved by name (SMM2
  imports 513 `nvn*` names), so the closer generation carries over more.
- Native Metal on macOS is the best route to iOS/iPadOS (Metal, AOT — static recompilation needs
  no JIT), with Vulkan for Windows/Linux.
- It recompiles on the player's machine from their dump, so the repo and any release hold none of
  the game's code — the same rule SMCraft follows.
- Its patch/mod layer (`runtime/hle/*`, `tools/patches.py`) suits the 100 Mario and costume mods.
- From the Wonder framework: the shared-runtime / per-game split as the layout goal, and its
  Ryujinx-translator-based shader path (the owner asked to reuse Ryujinx, MIT) if LAS's translator
  falls short on SMM2's shaders.
- **Reference runtime:** Ryujinx (MIT), already set up with the owner's dump, for frame captures and
  input-replay comparisons (the render-diff setup and the memory-reading Ryujinx built for SMCraft).

**Done means evidence, per platform** (tracked in PROGRESS.md "Recomp"): boots to the title and
plays courses; frame captures match Ryujinx on the same dump; recorded-input replays stay in sync
with Ryujinx frame for frame (positions and outcomes).

**Web:** last. A recompiled game is ~1–2 GB of generated C and uses 3–4 GB of guest memory; it needs
wasm64 (memory64), threads (SharedArrayBuffer) and an NVN→WebGPU backend. Whether that's practical is
decided after the native ports, with the reasons written down either way.

**Rules:** no Nintendo binaries, assets, keys, recompiled output or disassembly in git —
`scripts/check_no_proprietary.py` now also blocks native binaries, SPIR-V, AArch64 ELFs, anything
under `recomp/private|generated|build/`, and text that looks like disassembly or recompiled code.

## 2026-10-18 — Physics tuned against the original game, frame by frame
- Reference: the owner's own SMM2 v3.0.3 in a locally patched Ryujinx. The patch hooks the
  player's per-frame update (a one-instruction exefs patch turns an instruction at its entry
  into a supervisor call the emulator handles), dumps the player object every frame and feeds
  a scripted pad state per emulated frame, so input timing is exact. It runs from its own data
  root (keys read in place, nothing of the owner's changed). Test courses are built with the
  editor crate and played as the title-screen courses. The emulator patch and scripts stay in
  private research; the repo only gets the generic `mmk trace` / `mmk trace-courses` commands.
- A difference is fixed only when the binary explains it (state machine, timers, tables); the
  trace decides between readings. Where the code wasn't found, the measured rule is used and
  marked [obs: Ryujinx trace] (e.g. the 3-frame jump-press retry, CrouchEnd lengths per style).
- The engine keeps vy = 0 while grounded where the game holds −0.5 (positions are identical);
  comparisons treat the two as equal.
- Replays recorded before these fixes no longer verify (`SIM_VERSION` is bumped per release, as
  decided above).

## 2026-10-18 — The update power-ups (part 81, the Master Sword, the wearables)
- **Own modules, small hooks.** The player's new states live in `sim/src/player/powerups.rs`
  (`Player::pu`), the items, projectiles and EnemyDB reaction rows in `actors/powerups.rs`, the
  world side (collecting, attacks, lifting and throwing) in `world/powerups.rs`. `player.rs` only
  gains the form variants, the field and hook calls: one before the movement (it may take the
  whole frame — balloon, frog swim, squirrel flight / glide / cling, Link's dash, shield and
  down-thrust, SMB2 LiftUp, Propeller Box, Bullet Bill dash), one on a ground jump's launch speed,
  one after the normal physics, and hooks in `hurt` (a wearable takes the hit), `bounce` (the
  balloon only drifts) and `sensors` (the balloon's pose). The frame-matching work on the normal
  movement stays untouched.
- **Rules from the binary.** Forms, size classes (Link and the balloon have the small body), the
  per-style remap of part 81, the small-player block swap (only form 0, mask 0x7d7d2: the sword
  and the wearables come out as they are), every new form → Super on a hit, and the per-state
  numbers are the specs' [obs] values (81-style-powerup-2.md, 127-headdress-items.md).
- **EnemyDB reaction columns as a table per engine kind.** `Kind::pu_reaction` holds the v3
  rows for reaction_sword / sword_under / shield / arrow / boomerang (and `usa_carry` the SMB2
  carry flag and height); the engine kinds map to the rows by style and size. Reactions the
  engine has no motion for map to the nearest one: Damage → into the shell (Koopa, Buzzy), lit
  (Bob-omb), collapsed (Dry Bones), kicked (a shell), knocked down (Mechakoopa), else DieFall;
  ReactOnly on bosses = a shell-strength hit for the sword, a fireball for arrows and boomerangs
  [inf]; CapBound (the shield) turns the enemy away.
- **Where the binary was silent the choice is marked [inf] in the code:** the SMB2 Mushroom
  stands on what it can carry instead of stomping it (SMB2's rule [ext]; the "touched from above"
  flag was not traced); the bomb throw's vy 2.0; the down-thrust's bounce (the stomp bounce) and
  its landing as a small ground pound; the sword's tip breaking bricks like the tail; thrown SMB2
  enemies knocking out what they hit and landing back in their own state; the shield destroying
  blocked projectiles; enemies hidden by the Goomba Mask keeping to their own way.
- **Not modelled yet** (no engine counterpart): the balloon's tilt and the arrow's angle are not
  drawn (sprites don't rotate); SMB2 cloud pulling; the Frog skimming water at full P-speed while
  carrying; the Goomba Mask's crouch-walk synced to the Goombas (the engine has no crouch-walk);
  the balloon's push-stagger and the multiplayer balloon spin; the Red POW Box's keep-for-a-
  pending-POW rule. Sounds are Event variants only; the sound director maps them later.

## 2026-10-18 — Slopes: the game's surface, a solid wedge underneath
- A slope object is a band in the game (a sloped floor and a sloped ceiling 16 px apart between
  two 1×1 end caps). The engine keeps the binary's surface and caps exactly but fills the cells
  under the surface (and under the high cap) solid instead of modelling the hollow underside:
  walkers and the player meet the same top, and a slope's underside is rarely reachable. Revisit
  if a course needs to pass under a floating slope.

## 2026-10-18 — Pass 3: world rules from the binary
- Clown Car: Jump (not Down + Jump) climbs out; holding Down crouches in it. Escort Toad knocked
  out: back after 45 frames (his balloon's flight not modelled), no run failure. Burner flames hurt
  only at full length. Cape spin box alternates sides every 2 frames; hammer box on frames 11–17.
  Spiny helmet knocks out what is above it whatever the player's speed; Buzzy helmet only Spike Tops
  and Lakitu. Bill Blaster extras use one course-wide generator (fixed seed; the game's is per play).
  Spawn boxes per kind from the actor profiles.

## 2026-10-18 — Joint positions come from the model data, missing joints included
- Where the game places something at a model joint, the engine uses that joint's position from
  the style's model at the frame the game reads it (Spike's ball: `GeneratorPosition` at the
  throw's release frame). The 2D and flat NSMBU models are authored facing left (+x behind).
- When the model has no such joint the game's lookup fails and it uses the actor's position; the
  engine does the same rather than invent a joint. Mechakoopa's weapons ask for "shot", which no
  robot model has, so missiles and the laser start at its feet.

## 2026-10-18 — Gameplay sound from the game's own sound links
- What each event sounds like comes from the user's own data, not from code: the importer
  turns `SLink2/SLink2DB` (per object and style: event keys → trees of switch / random / blend /
  sequence containers over waves, with volumes, pitches, delays, groups, priorities) plus the
  group tree, distance sets, per-theme effects and ground materials into `audio/sound.json`,
  and copies every wave it names (`audio/se/`, `audio/music/` by the game's names). Engine
  code names only the database's identifiers (user bases like `Kuribo`, keys like
  `r_damage_stamp`); the game's internal Japanese property names are renamed by the importer
  to roles (`@style`, `@theme`, `@hour`, `@ground`) from their value domains.
- The director plays a key from a sim event or from a state change it observes (actor modes,
  the player's state), as the game's reaction code and action triggers do. This keeps sim
  edits out of the sound work: no sound-specific events were added; the existing unit events
  are matched to the actor whose state changed in the same tick (stomp, kick, defeat), and fall
  back to a generic enemy when none did. Doors are told from pipes by the door positions.
- Music streams stay DSP-ADPCM in memory and are decoded while they play (about a quarter of
  their PCM size; four-channel SMW/NSMBU tracks carry the Yoshi layer); effect waves are
  decoded to PCM per course. The course sound set only grows during a course, so a wave
  loaded on first play doesn't cut voices.
- Caches without `sound.json` keep the old fixed SMB1 set (`audio/index.json`). The importer
  no longer writes the `audio/music/<style>_<theme>` copies; re-run `mmk import` (importer
  version 7). SMM1 costume sounds (Phase 11) are added to the course sound set when a costume
  is put on, in both directors (the mixer has no music slots any more).
- Choices where the game's behaviour is not known are marked [inf] in code and in the private
  `research/specs/audio.md` §11: pan depth 0.5, the attenuation reference (the view's edge),
  effect sends at 1.0, limit types other than 2 needing a strictly higher priority, the Yoshi
  layer's 30-frame fade, footsteps every 9 px of walk cycle, the low-pass cutoff mapping.

## 2026-10-19 — The recomp is the only version; SMCraft archived

**Decision (owner):** make the static recompilation the project and archive SMCraft. Main keeps
`recomp/`, the course server (`core/`: formats, sim, editor, modes, netplay, server, online, and
`mmk` cut down to `server` / `register` / `upload`), the Python format reference and the docs.
SMCraft as a whole (Swift app, portable and web shells, front end, UI, audio, importer, Story
Mode) stays on the `smcraft-archive` branch, tag `smcraft-final`, where it builds and tests as
before.

- **Why the server stays:** the recomp's 100 Mario mod answers the game's Endless calls from it
  (accounts, course pools, runs under the `hundred` ruleset, leaderboards). It depends on `sim`
  and `editor` for course validation and replay checks, so those crates stay with it.
- **What changes:** `scripts/check.sh` gates the server's Rust, the recomp's runtime compile
  (macOS) and the guard; CI runs the server and Python jobs on Linux. The older goal's "SMCraft
  stays as it is and keeps working" is met by the archive branch, not main.
- **Open:** the server's verified records re-simulate replays with SMCraft's engine; recomp runs
  need another proof (or none) before records from the recomp count as verified.

## 2026-10-19 — The recomp's goals: perfect SMM2, SMM1's 100 Mario as SMM1, our server only

**Owner's plan** (to session mario-maker-27, relayed; also to this session):
1. **A perfect SMM2 v3.0.3 recomp** — frames and input replays checked against Ryujinx.
2. **100 Mario Challenge exactly as in SMM1**, replacing the earlier "SMM2's Endless screens with 100
   lives" presentation: SMM1's own screens from the player's SMM1 dump (difficulty tabs, world map
   with the Goombas carrying Peach to the castle at the end, dot walk, the start card with 100 little
   Marios, pause, clear, credits `Nwk_StaffCredit_00/01/03`, `T_ThankYou_10/20/30`), SMM1's rules, and
   the ending when you win. Run lengths Easy 8, Normal 16, Expert 16, Super Expert 6 (owner confirmed).
3. **A setting per SMM1 feature** (Mystery Mushroom costumes, mid-air shell jumps, overlapping pipes …).
4. **Never Nintendo's servers**: course search / download for 100 Mario, SMM2's Endless and Course
   World, and online multiplayer, all through our open-source server.

**How:**
- **Screens host-drawn, courses in the game.** The SMM1 screens and the ending are drawn by the
  recomp host over (or instead of) the game's frame, from SMM1 layouts generated on the player's
  machine; SMCraft's SMM1 layout player (our own Rust, `smcraft-archive`: `core/crates/ui`,
  `front/src/smm1.rs`, choreography notes in its DECISIONS "100 Mario screens: SMM1 layouts, inferred
  choreography") is reused rather than rewritten. Courses still play in SMM2's own course play, so
  play is exact; the mod's server plumbing (built first on SMM2's Endless path) carries the run.
- **The rescue scene** is code in SMM1, not a layout (SMCraft only showed the Thank-you balloon over the
  map, which isn't it): recovered from Block.rpx v208 (ending scene 0x0259e7d4 / 0x0259f40c) with
  emulator footage of the owner's SMM1 dump as the reference.
- **SMM2's own Endless Challenge** stays as SMM2 made it, against our server under the `smm2`
  ruleset; 100 Mario is a separate mode.

## 2026-10-19 — SMM1's 100 Mario screens in the recomp: SMCraft's layout player ported to C, drawn on the CPU

**What:** the host-drawn SMM1 screens (`recomp/games/smm2/mods/smm1/`): difficulty select, world map
and walk, start card, pause / clear / skip / miss overlays, game over, and the ending (rescue scene,
Thank-you line, staff credits), from the player's SMM1 dump (base v16 + update v208), generated at
first use into `<data>/mods-generated/smm1-ui-v6/` (layouts, fonts, texts, the ending's models,
the screens' music and sound banks). Nothing of SMM1 is in the repository.

**Port, not link (coordinator approved):** "reused rather than rewritten" above meant SMCraft's Rust
crates (`mmk-ui` player/text/font, `mmk-formats` lyt/bflim/bffnt/gx2/msbt, `front/src/smm1.rs`).
They are reused as a C port with the same rules and the same [inf] notes, not linked as a Rust
static library, because:
- the recomp builds with clang / zig only; a Rust static library would add cargo to every target's
  build (macOS, iOS, Linux arm64 / x86-64 cross-built with zig, Windows), and the Linux cross targets
  aren't installed for cargo here;
- the player crates pull the importer and asset crates, which stayed on the archive branch;
- the port is small (formats ~900 lines, player ~800, rasterizer ~250).

**CPU rasterizer, one image:** the layout player's quads are drawn into one 1280 × 720 RGBA image
(SMM1's native resolution) on a worker thread, and the renderers draw that image over (or instead
of) the game's frame through the overlay menu's ImGui on Metal and on Vulkan. One small hook per
renderer (`game_overlay_image`, `vk_imgui_texture_stream`), no shaders of our own on either backend,
and the result is the same on both (and testable without the game: `tools/test_smm1ui`). Composed
in gamma space with the authored 8-bit colours [inf]. Cost: 6-20 ms of CPU per frame on the busiest
screens (the map's MapIn zoom, the card's 100 Marios), off the present path.

**Runtime hooks (additive):** `game_overlay_image` / `game_overlay_captured` / `game_input_mask` /
`game_audio_mix` (weak in `runtime/game_hooks.c`); `input_player_buttons()` (keyboard and pads only).
While a screen has the controls the game gets no player input, but still test input: the 100 Mario
mod presses SMM2's hidden Endless buttons that way.

**From the binary over SMCraft's guesses** (research/specs/smm1-ending.md, private): the rescue
scene is a dedicated scene (`NetEndingDemoScene`), not the map: Mario walks in from x −32 at 1.5 a
frame to 384 next to Peach (Toad on Easy) at 416 on the cleared course's style's ground model, the
balloon line comes 30 frames after he stops (row by difficulty, variant by the costume he wore), and
the credits follow on black (`_01` Easy, `_00` Normal / Expert, `_03` Super Expert). **Nothing
skips**: neither ending scene reads the controller [obs], so A does not skip the ending (the brief
said it did). The staff-roll music is `easy` for Easy and Super Expert, `normal` for Normal and Expert.
SMM2's SM3DW style has no SMM1 scene; its courses end in NSMBU's, the 3-D one [inf].

**Between courses, from the binary** (research/specs/smm1-100mario-screens.md, private): the map's demo
states, the card's In by case (AnimA + AnimB on a new run, AnimB after Continue or a miss, none
otherwise) and its hold until the course is there, the select screen's tab and button rules, the
Course World music with one track per difficulty and the beat bounce timed from its markers.

**Where the recomp differs, and why:**
- **When the course is fetched.** SMM1 downloads the next course while its map plays and starts it
  after the card. SMM2 starts a course as soon as it has it, so the flow asks the run side for it only
  once the card is up; the card holds until SMM2 reports the start, then plays its Out over the course.
- **Between courses the screens are SMM2's** (owner, 2026-10-19, entry below): SMM2's course-in,
  pause, skip, miss / retry and Endless's clear with its Next / Like / Boo result screen. The flow's
  SMM1 pause, clear, miss card, hold-to-skip and game over are no longer shown in a run (the code
  stays in `screens.c`). After a clear, once the player leaves SMM2's result screen with Next
  (`HUNDRED_NEXT`), the flow shows SMM1's map (Mario to the next dot) and the card, as SMM1 does
  between courses, or the ending when the run is won. The card stays up until the course is playing,
  which hides the load and SMM2's course-in.
- **The fade to the card** is an iris on Mario as SMM1's fader type 7 does; its length is [inf].
- **Sounds** are SMM1's own, placed by name on the events the binary fires (the SLink trigger table
  that pins each to a frame is not decoded) [inf].

**Open:** a New Game straight after an ending (SMM2 is not back on its Endless menu after a win: run
side); the NSMBU ending's perspective
camera (orthographic here); the Mystery Mushroom reward after the credits is a hook
(`hundred_costume_reward`) for the costumes mod; no Cemu here for side-by-side captures.

## 2026-10-19 — Online multiplayer: local play over our relay first, Network Play at the enl layer second (plan, not built)

**Decision (plan):** SMM2's multiplayer reaches other players only through our server. SMM2 runs its
own netcode on Nintendo's PIA library, which has three back ends: `nex` (internet: NEX matchmaking,
NAT check and relay on Nintendo's servers), `lan` (UDP broadcast) and `local` (local wireless, the
`ldn` service). The recomp has no bsd / ldn services yet, so none of it reaches a network today.
1. **First: a virtual local-wireless network over our server's relay.** The runtime implements the
   `ldn:u` service and the UDP calls of `bsd:u` that PIA's `local` back end uses, with a relay room
   (`core/crates/netplay` relay, already in every `mmk server`) as "the air". SMM2's own local play
   (versus and co-op with nearby players) then works between recomp players anywhere, with no game
   code touched. Our server gets a room list (code, up to 4 players).
2. **Then: Network Play proper** (random matches, ratings, friends): enl's matchmaking commands
   (3..7, dropped by the mods today) answered from matchmaking on our server, PIA's `nex` session
   given the peers and a session key from the room, PIA's packets over the relay as in 1; the
   versus / co-op result requests (StartBattleMode, EndBattleMode, PostPlayResultBattleMode*,
   GetBattleModeRating, …) answered from new server endpoints.
3. Not chosen: speaking NEX / PIA's internet protocols on our server (our server would become a NEX
   server), or replacing the game's input exchange with our own rollback (most RE, changes the game).

**Why:** the same approach as Course World (answer the game's own requests, never Nintendo's
protocols); option 1 needs no reverse engineering of SMM2's netcode and gives two-player play soonest.
**Would change it:** SMM2 exposing PIA's `lan` back end (shorter than 1), or PIA's `nex` session
depending on NEX state too deeply to answer at the enl layer. Details, unknowns and the research list:
`research/specs/recomp-online.md`.

## 2026-10-19 — Endless / 100 Mario courses from TGRCode's API, opt-in; the server inside the app

**Decision (owner, 2026-10-19):** the Endless Challenge and the 100 Mario Challenge may draw their courses from
TGRCode's API (`https://tgrcode.com/mm2`), and a device needs no server of its own. This relaxes point 4 of the
entry above for course *content*: TGRCode is a third-party service that reads Nintendo's Course World, so the
courses come from Nintendo's network, but the game never talks to Nintendo or to TGRCode, only to our server.

**How:**
- **An upstream source in our server** (`core/crates/server/src/upstream.rs`), off unless asked for
  (`mmk server --upstream tgrcode --exe <your main>` / `--keys <table>`). A run that starts, or a pick whose
  clear-rate bands ran dry, tells it which difficulty is short; it searches TGRCode's Endless pool for that
  difficulty (`search_endless_mode`), downloads the courses it doesn't hold (`level_data`), decrypts them and
  imports them under their own codes with their attempts and clears, so `pick` / `pick_hundred` band them like
  any course. Requests are paced (3 s apart, a difficulty searched at most once a minute) and back off when
  TGRCode answers 429 (it rate-limits at ~1.5 s); when it fails, runs play the courses the server holds.
- **The course key table** comes from the player's own game: found with the first download in the loaded
  executable (`keyfind::find_in`, ~0.5 s), never written to disk or logged.
- **The server inside the app** (`core/crates/embed`, a static library; C interface
  `recomp/runtime/net/embed_server.h`), on 127.0.0.1 with one local account, so the mod's plumbing is unchanged.
  On iOS / iPadOS it starts by default with the TGRCode source and turns the 100 Mario mod on through the mod's
  own settings, unless a server is chosen (`RECOMP_MOD_SERVER` / `server=` in smm2mods.ini);
  `RECOMP_EMBED_SERVER=tgrcode|local|off` elsewhere. An in-game setting for the course source belongs to the
  mod (`games/smm2/mods/settings.c`), wired by its agent against this API.
- **Rust stays optional in the recomp build:** the library is linked only when it's been built
  (`make embed-server`) or `EMBED_SERVER=1`; Apple targets first, Linux / Windows / Android later. Every target
  still builds with clang or zig alone.

**Would change it:** TGRCode going away or asking us to stop (the server keeps what it imported; a self-hosted
server never uses it unless started with `--upstream`), or the owner wanting a pre-fetched pool only.

## 2026-10-19 — 100 Mario: SMM1 for the run's start and end, SMM2's screens in between

**Owner, after playing build 367 on the iPad** ("working a lot better, not perfect"): use SMM1's
screens only for the run's **start and finish** (difficulty select, world map with the castle end
point, start card, and the ending when you win), and **SMM2's own screens in between**: SMM2's
course-in, pause, skip, miss / retry, and SMM2 Endless's course-clear flow with its Next / Like /
Boo result screen. This narrows the earlier "SMM1's screens throughout" plan.

- **Course clear** must follow SMM2 Endless's own clear sequence; showing "Course Complete" straight
  away (as the build did) is wrong.
- **Course loading** between the card and the course is noticeably slow (the screen visibly changes
  before the course appears); hide or shorten that wait.

## 2026-10-19 — 100 Mario ending credits in SMM2's look (decided)

**Owner (relayed by session mario-maker-c4), tentative:** "we won't have the Miis", so the SMM1-style
credits (`Nwk_StaffCredit_*` rows with each maker's Mii face) will probably need to change to
something more in line with SMM2's look. Not final: the SMM1 rescue scene and thank-you stay; only
the credits roll is in question. Options put to the owner: keep SMM1's rows with another picture
in the Mii slot; switch to an SMM2-style course list for the credits; or keep SMM1's rows with real
Miis, if the course source supplies Mii data (TGRCode's course records carry the maker's Mii, which
SMM2's own Mii renderer could draw). **Decided (owner, option 2):** the credits roll uses SMM2's look: each cleared course as SMM2's
yellow course-name banner with the maker underneath, like the course card, and no Mii slot. The
SMM1 rescue scene and Peach's thank-you stay SMM1; the roll ends on THE END / TO BE CONTINUED as
before.

## 2026-10-19 — Mystery Mushroom costumes: worn as small Mario, the model through a spare slot

**What:** mod 2 (`recomp/games/smm2/mods/costume.c`, `mods/smm1/costume_gen.c`), per
research/specs/recomp-mods.md §3. At the first start with the switch on, the costume models are built
from the player's own dumps: SMM2's small SMB1 Mario model with each SMM1 costume's 24x24 frames
(`Model/M1_Player_Chara_<res>.szs` of the SMM1 dump), written into a rebuilt `Pack/M1_Model.pack`
(zstd level 3, about 8 MB for 100 costumes against the game's 5 MB; stored blocks made it 38 MB) by the mods' shared RomFS overlay (generator "costumes",
`<data>/mods-generated/romfs/`), which lays it over the game's.
Nothing of either game is in the repository.

**How it plays:**
- The player in costume stays form 0 (small Mario) for the game's logic, so size, physics, hit boxes
  and block rules are SMM2's own small Mario (SMM1's costume Mario is small too).
- The look: the model builder loads the course's costume into form slot 12 (Balloon, unused in SMB1,
  same size class as small Mario) by pointing that slot's model name at `Player_Chara_<res>` for the
  call; the visual slot setter shows slot 12 where the game asks for slot 0 while the costume is on.
  One costume per course (a model loads with the player; the pickup can't load one mid-course) [inf].
- The pickup: SMM1's Mystery Mushroom is the SMB1 skin of the style power-up, so the Big Mushroom from
  small Mario puts the costume on instead of growing (`costume_item=super`: the Super Mushroom, as
  SMCraft did). Another power-up replaces it.
- A hit takes the costume off through the Super → small power-down (hook point 0x15a50a4 reads the
  form as 1), as a costume absorbs one hit in SMM1.
- Sound: the costume's power-up, goal and death sounds from SMM1's `Sound/CharaMario` banks on the
  host mixer; SMM2's goal and death fanfares give way to them. Jump (`jump` / `jump2`, half and half),
  landing (`ground`) and the taunt (`appeal`: Up held 8 frames standing still) come from the player's
  motion each frame (on-ground bit, vertical speed); SMM2's own player sounds still play under them,
  since they are action triggers in the sound links [inf: muting them needs the links' play call]. The
  taunt has no pose: SMM2's small-Mario model has no slot for SMM1's `appeal` frame.
- Reward: after a won 100 Mario run the ending shows SMM1's costume window (`Cmn_CMListWindow_01`)
  with one random costume not owned yet, from SMM1's pools by difficulty (smm1-100mario-code.md §7);
  owned costumes are kept in `costumes-owned.txt` (category 0 always owned).
- The switch: "Mystery Mushroom costumes" in the Mods tab (`costumes=1` in `smm2mods.ini`).

## 2026-10-19 — 100 Mario gets its own Course World button; Endless stays exactly SMM2's

**Owner:** separate menu entries. The **100 Mario Challenge** gets its own button in Course World,
and SMM2's **Endless Challenge** button behaves exactly as in SMM2 (it no longer opens the 100 Mario
select, and nothing relies on pressing B there). Replaces the interim "B on the SMM1 select leads to
SMM2's Endless" route. The new button comes from a Course World layout overlay generated on the
player's machine from their own dump, plus a hook on its press; both screens are checked on the
iPad with the Mods tab.

**Android:** the owner has no Android device; Android is tested on the emulator.

## 2026-10-19 — Edit any course; Nextendo Network for multiplayer; one app-level settings page

**Owner (relayed by mario-maker-c4):**
1. **Edit any course, not just your own**, in SMM2's own Course Maker: Course World, Endless and 100
   Mario courses alike, as SMM1 allowed for downloaded courses. Edit buttons go where SMM1 put its
   edit option for a downloaded course, translated into SMM2's style (course detail page, after a
   clear, …). The work is the entry points plus loading the downloaded course into the editor (a
   Coursebot slot). **Re-uploading (owner chose):** an edited copy of someone else's course uploads as a new
   course under the uploader's name, credited "based on <code> by <maker>".
2. **Nextendo Network** (github.com/NextendoNetwork: community-run Switch online servers with a Go
   NEX/PRUDP server, account/dauth/BAAS servers) **for multiplayer**, and possibly for Course World,
   Endless and 100 Mario too. **TGRCode and our own server stay**: the online backend (Nextendo / our
   server) and the course source (Nextendo / our server / TGRCode) are player settings. This widens
   the earlier "our open-source server only" scope: community-run servers the player chooses are
   allowed; Nintendo's own servers still never are. Whether Nextendo supports SMM2 is being checked
   first.
3. **Settings live at app level**: one settings page in the recomp app's own UI (grown from the
   overlay menu's Mods tab), not a screen drawn in SMM2's style. It holds the online backend, the
   course source and every SMM1 feature switch.

## 2026-10-19 — 100 Mario's Course World button: try a real sixth button

**Owner (asked directly in session mario-maker-76):** of the options (take Ninji Speedruns' slot,
take the Super Worlds globe, or try a real sixth button), the owner chose **try a real sixth
button** built into Course World's top menu page (`Nwk_Title_00`), with its own panes, update, draw
and cursor navigation. If that proves unworkable, the fallback is Ninji Speedruns' slot, confirmed
with the owner first.

## 2026-10-19 — "Challenge Modes" replaces the sixth button; Nextendo groundwork offline

**Owner:**
- **Challenge Modes** (replaces both the sixth-button plan and "separate menu entries"): Course
  World's Endless Challenge button is relabelled **"Challenge Modes"**. Inside, **ZL / ZR** (or a tap
  on the "ZL ◀ mode ▶ ZR" header) switch between **100 Mario Challenge** (SMM1's select / map / card
  flow) and **Endless Challenge** (SMM2's own, exactly as SMM2); **L / R** still switch difficulty;
  **B** stays "back"; the last mode used is remembered.
- **Nextendo Network:** research done (research/specs/recomp-nextendo.md): SMM2 v3.0.3 is supported
  ("experimental"); a recomp client needs no console identity, but Nextendo must register the app
  (their developer portal is closed) and their SMM2 server is private; their code is PolyForm
  Shield, so we write our own client and copy none of it. **Start the offline groundwork now**
  (real sockets, DNS redirect table, TLS, account token, app sign-in) against a **local test
  server only**; nothing connects to Nextendo's servers until the owner has their permission and an
  app registration (the owner is contacting them on Discord). The same pieces give us our own
  local NEX server route later.

## 2026-10-19 — Releases: a launcher plus recompiler per platform, bring your own game

**Owner-approved plan (relayed by mario-maker-c4), modelled on melee-pc** (github.com/999sian/melee-pc):
per-platform releases that ship **no game code**. The generated C is Nintendo's code, so a release is
a **launcher plus the recompiler**: the launcher takes the player's dump, checks its hash against the
known v3.0.3 versions, uses the player's own prod.keys, extracts the dump, recompiles the game on
first launch with the bundled zig, then runs it; it also takes an optional SMM1 dump (100 Mario,
costumes) and checks for updates.

- **Windows, Linux and macOS first** (builds on the existing first-launch recompile of the Windows
  package). **Android and iOS later**, as build-it-yourself paths; iOS can't compile on the device,
  so it needs a desktop helper that builds, signs and installs the app.
- Release builds come from GitHub Actions, run only for releases (CI minutes budget).
- The repository still has no top-level licence for the project's own code; the owner hasn't chosen
  one (MIT like LAS Recomp, or GPL-3.0 like melee-pc). Until then none is added.

**How it's built (2026-10-19, the releases agent):**
- **The launcher is the runtime's own start screen** (`recomp/runtime/ui/launcher.mm`, on the overlay menu's SDL3 +
  ImGui stack, Metal on macOS and Vulkan elsewhere), shown in place of the game until it is set up; then the game boots
  in the same window. No separate launcher program: one download, one process, and the same window for the setup,
  the recompile's progress and the game.
- **The dump logic stays in Python** (`games/smm2/setup.py --source`, `sysdata.py`), driven by the screen through
  `@step / @progress / @error` lines: one implementation for `make` and the launcher, testable without a window. The
  identifiers of known-good v3.0.3 dumps live in `games/smm2/versions.py` (build ids, the ExeFS files' SHA-256, title
  versions, program NCA ids): hashes of the player's own files only.
- **A Python of its own in every package** (python-build-standalone 3.12, pinned by hash, plus capstone / unicorn /
  lz4 wheels pinned by hash) instead of LAS's PyInstaller-frozen generator: wheels and Python builds exist for every
  target, so one machine (a Mac, or the release workflow's Linux container) makes the Windows and Linux packages, and
  players install nothing. About 25 MB per package.
- **hactool is bundled** (ISC; its mbedtls Apache-2.0), built from the 1.4.0 release's source with zig for each target.
  hactoolnet (LibHac, BSD) would also be allowed, but needs .NET; hactool is one small binary and already what
  `sysdata.py` used. NSP / XCI extraction copies the program NCA out of the container, extracts the base RomFS raw and
  lets hactool apply the update's patch (`--baseromfs`): about 6 GB of free disk at the peak.
- **macOS 15+** for the app: zig 0.17 itself needs it. Signed ad hoc (no Developer ID on this machine); the release
  workflow signs with a Developer ID when the secrets `MACOS_CERT_P12` / `MACOS_CERT_PASSWORD` / `MACOS_IDENTITY` exist.
- **Linux packages use the distribution's SDL3 (3.2+) and Vulkan loader** and bundle only shaderc (Debian's
  self-contained `libshaderc.so.1`, glibc 2.38+). Debian's SDL3 links PipeWire, sndio, libdecor and more directly, so
  it can't ship; an AppImage with an SDL3 built for it is left for later.
- **The update check only notifies:** the latest release's tag from GitHub's API (a background `curl`), compared with
  the tag the build was made from (`RECOMP_VERSION`), shown with a link to the releases page; switchable in the overlay
  menu's Mods tab (the app settings page) and on the setup screen.
- **No game content in a package, checked twice:** `scripts/check_package.py` fails `tools/package.sh` /
  `tools/bundle_macos.sh` and the release workflow on any file of a set-up data folder, game file names, magics or
  known ExeFS hashes, key-shaped lines, recompiled code, or a runtime with game functions linked in.

## 2026-10-19 — The game's own network stack: Mbed TLS, the runtime's trust policy, deny-by-default names

**Decision** (the Nextendo groundwork above, offline):
- **TLS library: Mbed TLS 3.6.7 LTS**, vendored unmodified in `recomp/third_party/mbedtls` and taken under its
  **Apache-2.0** licence (it is dual Apache-2.0 / GPL-2.0-or-later; notice in `LICENSE` and `README.recomp`). It
  builds from C on every target (macOS, iOS, Windows with zig, Linux, Android) with no system TLS. Configured as a
  TLS 1.2 client only (`runtime/net/mbedtls_user_config.h`): the console's ssl service speaks TLS 1.2 to the game's
  servers, and it keeps PSA crypto / threading out of the picture.
- **Trust is the runtime's, never the game's.** The CAs and client certificates the game imports or names are
  ignored (no console certificate or PRODINFO is used anywhere). `tls_trust=local-ca` (default): the server must
  present a certificate from the local test CA (made by `tools/nex_test_server` into `recomp/private/`, never
  committed; the proprietary guard now blocks PEM keys / certificates and `.pem` / `.key` / `.crt` files).
  `tls_trust=any`: accept any certificate, and only towards local addresses. A pinned CA for a community server
  is a third mode, added with the registration.
- **Names and addresses are deny-by-default.** The game's names never reach the host's DNS: nsd and sfdnsres answer
  from a redirect table; with the default backend every name fails, with `backend=local-nex` the game's domains go
  to the local test server. Every redirect target must be a local address (loopback, private, link-local,
  100.64/10), and a game socket may reach only loopback and those targets; anything else is refused and logged.
- **Backend setting** in smm2mods.ini: `ours` (default, the mod path, unchanged), `local-nex` (the mod's online
  overrides off, the game's own NEX client against the test server), `nextendo` (listed, refused: awaiting
  registration).
- **Account:** a local network service account ID and a local JWT ID token (HS256 with a local key in
  `net/account.txt`); a backend's real token plugs in through `id_token_file=`.
**Why:** the owner's "start the groundwork offline, against a local test server only"; our own client code (none
of Nextendo's PolyForm-Shield or citron's GPL code), written from public protocol documentation and the game's
behaviour. The same pieces are the "local NEX server" route.
**Would change:** the owner's go-ahead with Nextendo's permission and registration (public redirect targets and a
pinned CA behind an explicit setting); a game server that needs TLS 1.3.

## 2026-10-20 — Nextendo for multiplayer only

**Owner** (after the first live login, which reached Course World's top screen on Nextendo with the mod off):
**Nextendo for multiplayer only.** Network Play (online co-op and versus: matchmaking, rooms, PIA) goes through
Nextendo Network; Course World (browsing, downloads, uploads), the Endless Challenge and 100 Mario stay answered by
the mod from TGRCode or our own server, and 100 Mario stays on.
**Decision** (the "hybrid" backend, `games/smm2/mods/hybrid.h`):
- `backend=nextendo` means hybrid: the game's own NEX client logs in to Nextendo (the enl session and Course
  World's login sequence are the game's own, against their server), while the mod's Course World, Endless and
  100 Mario overrides keep answering, whatever `hundred=` says. With `courses=` unset the courses come from TGRCode
  (the built-in server, its local account named after the Nextendo user), so no account on a server of ours is
  needed. `course_world=backend` leaves all of Course World to Nextendo (the earlier behaviour).
- Two answers are merged: GetApplicationConfig is Nextendo's config (Network Play's switches) with the Endless
  Challenge's four keys switched on; GetUsers (own) is Nextendo's record of the player (name, Mii, maker code,
  player id) with our Endless bests and the 100 Mario unlocks. The backend's player id becomes the player's own id
  in the mod's answers, so "my course" and "me" agree with what the game's NEX session knows.
- `backend=ours` is unchanged; `backend=local-nex` stays all-NEX unless `course_world=mod` (the offline test of
  hybrid against `tools/nex_test_server`).
**Why:** the owner's choice: Nextendo's SMM2 catalogue is not where the player's courses are, while Network Play
can only work through a real NEX server.
**Would change:** the owner wanting Nextendo's Course World too (`course_world=backend` already gives it), or
Nextendo refusing logins whose Course World traffic doesn't reach them.
## 2026-10-20 — Frame generation: our own interpolation, the game stays at 60

**Decision:** a Video-tab setting, "Frame generation: Off / 120 fps" (`frame_gen` in `smm2.ini`, off by default),
shows one generated frame between each pair of real frames on displays of 100 Hz and more.
- **The game logic stays at 60 Hz.** SMM2 moves one fixed step per present, and `display_pace` keeps the presents
  at 60. Unlocking that speeds the game up and breaks the input-replay match with Ryujinx. The generated frames
  exist only on the screen: the player traces of a scripted run are identical with the setting off and on.
- **Our own interpolation, not Apple's MTLFXFrameInterpolator.** That API wants the engine's depth and motion
  vectors, and the runtime only sees the finished colour image at present time. So motion is estimated from
  consecutive frames:
  - a luma pyramid, then block matching coarse to fine (1/16, 1/8, 1/4, then 1 px)
  - the median of the neighbouring vectors as a cost-checked candidate
  - a warp that tries each block's vector and its neighbours' for every pixel
  - fallbacks: the still pixel, one real image where something appears or disappears, and the previous frame on a
    scene cut
  One source serves both backends (`runtime/nvn/framegen_src.c`: MSL and GLSL), so Metal (macOS, iOS) and Vulkan
  (Windows, Linux, Android) behave the same.
- **Presentation:** the generated frame is presented at once and the real one 1/120 s later
  (`presentDrawable:afterMinimumDuration:` on Metal; a present thread on Vulkan, with every submit, present and
  device wait under one queue lock). This costs about 8 ms of input lag. The menu is never interpolated: it is built
  once per real frame and drawn on both images.
- **No generation below 100 Hz**, after a hitch (real frames not 12-22 ms apart: also the game's 30 fps mode), across
  a size, format or crop change, and while the game is held.
**Why:** the owner asked for an fps unlock "done with frame gen" (2026-10-20); true high-fps rendering (hooking
object and camera transforms) was judged far too invasive.
**Would change:** engine-side motion vectors (would allow MetalFX's interpolator or FSR 3), or rates above 120
(the passes already take a phase `t`).

## 2026-10-21 — Quick level return: fast-forward the game's own miss, nothing skipped in its logic

**Decision:** a Mods-tab switch, "Quick level return" (`quick_retry` in `smm2mods.ini`, `RECOMP_QUICK_RETRY`),
off by default. After a miss, the frames from the death fanfare to the restarted course run as fast as the
machine can and aren't shown (`runtime/nvn/objects.c` `g_present_hidden`): no display pacing, no window image,
but each frame's GPU work, the renderers' per-frame work and the 100 Mario flow's tick still run.
- **The game does everything itself**: the miss, the lost life, the lives card, the course set up again. Nothing in
  its logic is skipped or patched, so lives count as before (100 Mario and Endless: a miss still costs one), clears,
  checkpoints, Start Over and the skip rules don't change, and with the switch off not one instruction of the game
  runs differently (the hooks only return).
- **The end of the fast-forward**: in Endless and 100 Mario the Endless scene's play state running again (after
  the lives card, which the owner wanted skipped; up to 900 frames); elsewhere (Course World, Coursebot, test play)
  the first player update after the course is built again (a stretch of frames with no player), each player update
  extending it by 60 frames up to 360, so it lapses by itself where no player comes (test play's miss returns to the
  Course Maker). The screen never stays still for long.
- **Measured** (M-series Mac): a Coursebot course's miss to playing again 1.6 s (SMM2 ~4.6 s); 100 Mario 2.3 s
  (SMM2 9.7 s, the lives card included); the fast-forward runs at about 270 frames a second.
- **Applies in the Course Maker's test play too** (owner, 2026-10-21).
- The death fanfare is heard only for as long as the fast-forward lasts; the next course music cuts it.
**Why:** the owner asked for a quick retry mod (2026-10-21), off by default and exactly SMM2 when off. Patching
SMM2's miss sequence (states, timers, the card) would mean changing game logic in several places per mode; running
the same frames faster changes nothing the game can see except wall time.
**Would change:** a restart that has to be quicker than the machine can run the miss frames (the reload itself, about
0.7 s on an M-series Mac, isn't shortened), or the owner wanting the fanfare kept.

## 2026-10-22 — Co-op in Endless / 100 Mario: run the course in SMM2's own local multiplayer mode

**Decision:** a Mods-tab switch, "Co-op in Endless and 100 Mario" (`coop` in `smm2mods.ini`, `RECOMP_COOP`), off by
default. With it on and more than one player's controller connected, an Endless / 100 Mario course is started in
SMM2's local multiplayer play mode, the one Play Together uses, with as many players as there are controllers (up to
four). Three small changes do it (`mods/coop.c`, `hooks.c`, `patches.py`):
- the course's play mode (`[*0x2a6bcd0 + 0xe0]`: 0 one player, 1 local multiplayer, 2 online), stored by the player
  spawn, becomes 1 in an Endless course, with the set-up's player count (`[*0x2a66fb8 + 0x40]`) set: SMM2 spawns the
  players and gives each their own controller, as in Play Together;
- the controllers' poll reads as many Npads as there are players (Endless leaves it at one);
- Endless's miss request, which the game sends for any player's miss, costs a life only when no player is left
  standing (state 3 is down, bit 5 of +0x48c a bubble).
**Why:** the owner's rules (2026-10-21) are SMM2's local multiplayer ones: shared lives, a life lost when all players
are down, a clear when anyone reaches the goal. SMM2 already has all of that for Play Together; switching the mode it
runs the course in reuses it instead of writing multiplayer into Endless. Off, every hook returns at once.
**Would change:** SMM2's multiplayer rules diverging from the owner's in play (e.g. a bubble rule that isn't wanted),
or 3D World courses (SMM2 has no multiplayer in that style: those stay single player).

## 2026-10-22 — Mod triggers ("Try it now"): a try-it-out helper, never in record-keeping runs

**Decision:** the overlay menu's Mods tab has a "Try it now" section with one button per mod effect that can be asked
for on demand (first: "Lanky Mario now"; a chosen costume next). Mods register their rows in a small list
(`mods/triggers.c`, `mod_trigger_add`: label, description, `available()`, `trigger()`, optional choices). Every row
follows the same rule: it is a helper for trying a mod out, kept apart from the normal game: greyed out when its mod's
switch is off, outside a course being played, and in Endless / 100 Mario runs (SMM2's Endless and SMM1's 100 Mario
keep records). Pressing it only records the request; the mod applies it on the game's thread at the player's next
update, through the game's own paths where there is one (Lanky from small: the game's power-up request, so the grow
demo plays).
**Why:** the owner (2026-10-22) asked for a way to see a mod's effect without hunting for the item that gives it,
without letting it into runs whose results are kept.
**Would change:** a mod effect that should also be offered in runs (it would need the owner's word), or a trigger
that needs a choice the menu's single choice row can't express.

## 2026-10-22 — Lanky Mario: Super Mario with Slim's look and SMM1's jump bonus, SMM1's random Weird Mushroom

**Decision:** a Mods-tab switch "Lanky Mario (Weird Mushroom)" (`lanky`, `RECOMP_MOD_LANKY`), off by default, effective
at the next start (the models are made then). A lanky player is form 1 (Super) for SMM2, drawn with Slim Mario in the
SMB1 row's spare slot 8, +0.5 px/frame on computed ground jumps only (preset jumps, stomp bounces, crouch jumps keep
their vy), SMM1's three sounds through SMM2's own SLink keys, a hit gives small. Weird Mushrooms come from SMM1's
easter egg (1 in 20 of the Super Mushrooms out of blocks and containers, at most one and at least one per 100), in
every SMB1 course including 100 Mario / Endless runs (it is part of SMM1's game, not a helper); the trigger is not.
Luigi's Slim model is recoloured with his SMB1 Super palette; Toad and Toadette, whose SMB1 sprites are shaped
differently, wear Mario's Slim art (SMM1 has only Mario's). The builder and visual-slot overrides are shared with
the costumes (`mods/player_model.c`: one override per address).
**Why:** the owner's request (2026-10-22, "Lanky Mario as an SMM1 mod switch, off by default" with a trigger); the
v3.0.3 research (`lanky-smm2-v303.md`) found every piece in SMM2 itself: a spare model slot with Super's size and
animation class, the jump launch's computed/preset split, SMM1's lanky sounds still in SMM2's data.
**Would change:** the owner wanting the bonus on preset or crouch jumps too, the random mushroom kept out of runs, or
the Weird Mushroom's own look (SMM1's `M1_Item_kinokofunny`: not done, the mushroom looks like a Super Mushroom).

## 2026-10-22 — Super Worlds: TGRCode's worlds through the game's own converters, the map blob never decrypted by us

**Decision:** Course World's Super Worlds are served by our server from TGRCode (`/get_super_worlds`,
`/super_world/<id>`), and the mod answers the game's eight Super World requests (`mods/superworld.c`).
- **The answers go through the game's converters**, not hand-written records: the mod builds the NEX record the game
  would have received (a guest WorldMapInfo) and calls the list-entry converter (main+0x14d54d0) or the detail
  converter (main+0x14d48a0). The detail converter resets the 0xa40-byte record, decrypts the map with the game's own
  key table and fills the course ids, so neither the server nor the mod holds a key or decrypts anything: the server
  keeps TGRCode's `unk1` blob byte for byte. An upload is encrypted by the game's own function before it leaves.
- **The pool grows from random answers**: each `/get_super_worlds` gives ten random recent worlds, so the server merges
  every answer into one list (newest first, 300 kept, our players' uploads first), asks again only while a client
  browses and the last answer is 10 minutes old (three answers while it is small), and keeps it on disk with the time
  of the last answer. A world's details and map are fetched the first time a client opens it and kept for good; its
  courses join the listed courses, so their data comes through the existing on-demand import.
- **Thumbnails fail** to the game's placeholder planets: TGRCode's world thumbnails are Nintendo CDN addresses, which
  we never contact. Our own uploads keep theirs on the server (not shown yet).
- **Uploads stay on our server**, one world a player (a new one replaces the last), made only of that player's own
  courses on our server.
**Why:** the converters already do the record layout, the reset, the map decryption and the ring writes exactly as the
game expects; writing them by hand would copy that logic and need the world key table outside the game. TGRCode has no
"all worlds" list, only random picks, so a kept, growing pool is the only way to offer more than ten at a time without
hammering it.
**Would change:** a source with a full world list, or world thumbnails we may fetch or render ourselves (the server
could then serve a picture and the thumbnail request answer it).

## 2026-10-22 — Mystery Mushroom costumes: a new costume at each pickup by rewriting the model's texels

**Decision:** a pickup draws a new costume as SMM1 does (uniform over the owned costumes minus the one worn) and puts
it on mid-course by writing its texels over the loaded costume model's 25 textures, instead of loading another model
or preloading several. Every costume model is the same template (SMM2's small SMB1 Mario: skeleton, material,
animations, SMM1's 24 x 24 quad) with other texels, so a costume is its texel data alone. The runtime tells the game
code about each texture made and dropped (`game_texture_init` / `game_texture_final`, weak hooks: nothing for other
games); the costume mod notes the 24 x 24 textures the builder makes for slot 12 (in place in the loaded file, in the
BNTX's order) and checks them against what it wrote last before every write (never writing a dropped texture or
reused memory). The taunt pose uses the same path (the costume's appeal frame on every texture for 120 updates).
**Why:** the owner's item ("more than one per course"). Loading a model mid-course means building a model holder
outside the game's own course set-up (the builder allocates and binds resources through the game's heaps and the
renderer's pools [inf]); preloading needs one form slot per costume, and the SMB1 row has 9 spare slots, one of which
Lanky Mario uses, while the size-class table (read inline at about 100 sites) would have to be patched for each.
Texels need neither: the renderers already re-upload a texture whose data changes, as for any texture the game
rewrites.
**Would change:** a costume whose model differs from the template in more than texels (SMM1's Inkling squid
`_W` and Block `_L` sets, not used), or a renderer that stops noticing texel changes.

## 2026-10-22 — "Mystery Mushroom costume now": every costume in its row, a pickup through the game's request

**Decision:** the costume trigger's choice row lists every SMM1 costume of the player's dump (100), by its English
name (SMM1's `Cmn_CharaMarioName`), sorted by name, owned or not; choosing one doesn't add it to the owned costumes
(the random draws and 100 Mario's rewards still go by `costumes-owned.txt`). Pressing it gives player 1 a Mystery
Mushroom with that costume through the game's own form change request (as a pickup: from Super / Fire the shrink
plays), so it is greyed out with a power-up the Mystery Mushroom doesn't take (anything but small, Super and Fire),
besides the triggers' rule (no switch, no SMB1 course being played, Endless / 100 Mario runs). The Mystery Mushroom
item itself is taken as small, Super or Fire Mario (SMM1 takes it from any form).
**Why:** the trigger is a try-it-out helper (DECISIONS 2026-10-22, mod triggers): a list of only the owned costumes
would hide most of them until many 100 Mario runs are won, which is what trying a costume out is for; not counting
the choice as owned keeps SMM1's unlock game intact.
**Would change:** the owner wanting the row limited to the owned costumes (a one-line filter in `fill_choices`), or
the trigger counting as an unlock.

## 2026-10-22 — Costume sounds replace SMM2's at the game's own requests, with SMM1's fallback; the item keeps its size

**Decision:** (1) SMM2's jump sound is muted under a costume at the game's own jump sound request (the player's jump
sound function reads its request as silent; one state that plays "jump" itself gets a sound user without an
instance), and the costume's jump plays at that request instead of on a detected take-off. (2) As in SMM1, an event
the costume has no sound for keeps the normal sound: a costume without `jump`, `goal` or `down` leaves SMM2's jump
sound and fanfares alone (before, a costume without them made the goal and the miss silent). (3) The Mystery
Mushroom's look is SMM1's "?" mushroom drawn into the item model's own texture at one texel a pixel (bottom middle);
the item stays the Big Mushroom's 2 x 2 for the game.
**Why:** (1) the request is where SMM2 decides a jump makes a sound (a stomp bounce or a spring doesn't), so the
costume's sound comes exactly where SMM2's would, and muting there touches nothing else (the sound links stay the
game's); a global "mute this key" in the SLink play call would need a hook in a function every object calls. (2)
SMM1's rule (smm1-extras-costume.md §7). (3) Changing the item's size or hit box means another object (SMM2 has no
16 x 16 Mystery Mushroom); the texture alone needs no game code and shows SMM1's art at SMM1's size.
**Would change:** a 16 x 16 Mystery Mushroom object (its own actor and course id), or another place SMM2 plays the
player's jump sound found playing under a costume (the audio log shows SMB1's jump wave, 0x341a bytes).

## 2026-10-22 — Edit any course, the leftovers: the card says Make, "save for later" is Played Courses, uploads send their picture

**Decision (agent, from the owner's 2026-10-19 "Edit any course" and the design in recomp/docs/mods.md):**
1. **The card's text.** On a Course World card whose course is already in the Coursebot, the Download button (which
   then opens the Course Maker) reads the Coursebot window's own **Make**, taken at run time from the player's
   message archive in the game's language; other cards keep Download. SMM2's messages have no "Edit" for a course
   (the pause menu's "Edit Course" is the in-course action), and Make is the word of the button this one mirrors.
2. **Endless / 100 Mario "save for later"** (the design's open question: how to keep a course without leaving the
   run): the run's courses are kept on the player's **Played Courses** in their Course World page, which our server
   already fills from every run (newest first, the last 300). From there the card's Download keeps the course in the
   Coursebot for good, and the same card then reads Make and opens it in the Course Maker. Nothing is added to the
   result screen and the run scene is never left.
3. **An uploaded course's picture**: after the upload the mod sends the picture the Course Maker made (the thumbnail
   block that follows the course in the game's upload buffer) to `PUT /api/courses/<code>/thumbnail`; only the
   course's uploader (or the admin) may set it. The game's upload carries no whole-course map, so none is sent.
**Why:** each is the smallest change that stays inside SMM2's own screens. The text comes from the player's dump, so
it is right in every language and no game text is committed. For save for later, the design's "Save to Coursebot"
on the result screen would need a button SMM2's result screen doesn't have and a round trip from the run scene to
the Coursebot and back, which the 100 Mario flow would have to survive; Played Courses is SMM2's own list of exactly
these courses, and its card already has Download and (now) Make.
**Would change:** the owner wanting an explicit keep inside the run (the result screen route, or a pause item), or a
course to be kept past the 300 played (then Liked Courses, which the result screen's "I like it!" fills, or a
server-side keep list); a whole-course map for own uploads (the server could draw it from the course).

## 2026-10-23 — Super World title: the save's player name held for the length of the title's set-up

**Decision:** for another maker's world, the mod's wrapper of the world title's set-up (main+0x199fb60) writes the
account's nickname into the save's player name (save.dat +0x10) when it is still the game's default " ", calls the
game's set-up, and puts the default back. The game then takes its own path for another maker's world: the owner's
name and Mii from the owner block it built from the Course World list. The progress record keeps the game's two
status values apart: `status` is the record's own (2 progress to continue, 1 none), `clear` the save's kind.
**Why:** on a console the save's name is the nickname of the Mii the player picks for Course World, so the game's
"name still default" fallback (the account's nickname for every world) never applies; here the Mii database is empty
and the name stays default. Setting the save's name for good would also change what Network Play sends as the
player's name and Mii (save +0x10 / +0x60), which is outside this change; holding it only during the call changes
nothing else. Writing the owner's name into the world record instead does nothing: the game rebuilds that block.
**Would change:** a Mii database with the player's Mii (the save's name and Mii then set the way a console sets
them), after which the wrapper has nothing to do.

## 2026-10-23 — The loader binds a symbol both modules define to main's, as the console's loader does

**Decision:** `runtime/loader.c` resolves a relocation against a symbol the module defines itself to the first
global / weak, default-visibility definition in load order (main, then the SDK), not to the module's own copy. For
SMM2 that moves the SDK's references to operator new / new[] / delete / delete[] (strong in main, weak in the SDK)
and a handful of identical template copies (std `__tree` helpers, `complex` operators, `nn::os::Mutex::~Mutex`, a
crypto callback) to main's definitions; the loader logs each one (`[loader] sdk: <name> bound to an earlier
module's`).
**Why:** the World Maker's Save As and the Worldbot's exit stopped on the SDK heap's check (PROGRESS item 7): the map
object's nn::ngc::ProfanityFilter is allocated by the game's operator new (its sead heaps) and deleted by the SDK's
destructor through operator delete, which was bound to the SDK's own malloc-based copy. The console's rtld looks
every default-visibility symbol up in load order, so on hardware the SDK uses main's operators throughout; matching
that removes the whole class of cross-module new / delete mismatches rather than patching this one destructor.
**Would change:** a module whose own definition must win (protected / hidden visibility already does: those are left
alone), or another game whose SDK must keep its own allocator (none in this repo).

## 2026-10-23 — The player's own maker has the account id the game holds as its player id

**Decision:** the mod's player id for the signed-in account's own maker (endless.c `pid_of_maker_id`) is the network
service account id the runtime gives the game (`acc_account_id()`, the GetAccountId answer), not a hash of the maker
ID like every other maker's. Hybrid mode keeps the backend's id; without an `/api/me` answer the token hash stays.
**Why:** besides comparing course owners with the player's own record (which a hash satisfied), the game compares a
user record's player id with the account id directly in places: My Info's Super World panel shows Delete only then
(main+0x1aa46a0), so the player could not delete their uploaded world. On a console the two are the same number.
**Would change:** a backend whose player ids must be the server's own (Nextendo: its PID is the account id already).

## 2026-10-23 — Shader compile-ahead: the game's own shader files and the programs it sets, compiled in the background
**Decision:** both backends compile the game's shaders before their first use, from boot, in the background
(`runtime/nvn/shader_ahead.h`; Metal `renderer.m`, Vulkan `vulkan/ahead.c`; measurements in
`recomp/docs/smm2-bringup.md`, "Shader compile-ahead"). Of the two ideas weighed, (a) compiling from the game's own
shader archives in the player's RomFS was built; (b) recording per style / theme what a machine compiled and replaying
it already existed (`cache/shaders/` and its warm-up, the pipeline cache) and can't help with a style the machine has
never drawn, which is the case at hand. (a) is extended by the programs the game sets up (`nvnProgramSetShaders`),
which carry exact pairs for agl's and the effects' programs the files alone don't give.
- **Shaders, not archive formats:** the scan takes every NVN shader blob in the RomFS's archives (SARC / Yaz0 / zstd),
  whatever holds it (BNSH in models and layouts, `.sharcb`, `.ptcl`), and translates each alone; a fragment shader
  as if its vertex shader wrote every input it reads (true of almost all the game's pairs); BNSH variations are also
  taken whole. Map-mode runs (`LARECOMP_AHEAD=map`) found all but 1 of the sources an NSMBU course start compiles.
- **Order over completeness:** a pass is ~6,900 sources, 7-11 minutes while the game runs, so the order matters for a
  new player's first minutes: what the game set in the last 10 s, a model it's reading, the next course's style and
  themes (SMM2: from the run's next course and every download), then SMM2's ranks (`games/smm2/shader_rank.c`: the
  models every course draws and the course themes' first), the boot backlog of set programs, the rest; agl's
  thousands of variants last.
- **Never in the game's way:** no compile while the game builds or has built in the last 1.5 s, 4 at a time (2 on
  iOS) at utility QoS, none while the device is hot or in low power mode; the title kept its 59 fps. Bounded: the
  set programs kept are at most 4,096 copies, a file is read, its shaders passed on and freed; a stop flag at exit.
- **Where it lands:** Metal's own shader cache (we can't serialize a source-compiled library; the offline compiler
  isn't on players' machines), so the hit relies on Metal keeping it: the 6,900 sources stayed cached (a forced pass
  over them took 20 s). Vulkan: our `cache/spirv/`. A stamp (`cache/ahead.done`: executable size and time, key,
  salt) skips the files on later starts; a new version reads them again.
- **No list in the repository:** no shader hashes or "hot lists" are shipped; everything comes from the player's
  dump at run time.
**What would change it:** Metal evicting the compiled sources before use (then keep the libraries of the likeliest
in memory, or a pipeline-level warm-up); pipelines (Metal's back end, per state) becoming the bigger part of a course
start's hitch (then guess the common pipeline states from the programs the game sets); iOS measurements showing heat
or battery cost (then compile only while the player is in menus, or only ranks 0-1).

## 2026-10-23 — Course World likes and comments: the server says where each player stands, and keeps SMM2's whole comment

**Decision (agent):**
1. **The player's own relation to a course travels with the course.** Every course object our server sends to a
   signed-in player carries `you` = `{played, cleared, vote}` (`Store::annotate`), and the mod writes it into the
   course record the way SMM2's converter writes the NEX field (+0x188: 1 played, 2 cleared, 3 liked, 4 booed). SMM2
   reads it there to open the clear screen's hearts and to show a choice already made; it learns it from the course
   record it asks for again after posting the play. A rating taken back (the game's rating 0) is `DELETE
   /api/courses/<code>/vote`.
2. **A comment is kept as SMM2 posts it**: type (text, stamp, drawing), text, stamp number, where the player was (x, y,
   sub area), the Mii's face, "require course clear to view", posted after a clear; a drawing's picture is kept as the
   game's own data (`comments/<id>.bin`), not turned into an image format. Comment ids are the server's, unique across
   courses, so the game's picture cache and Delete need nothing else (`/api/comments/<id>`).
3. **The game's server time is this machine's clock** when the mod is the server: the NEX core's tick (main+0x21710)
   leaves it unknown without a connection, so the mod's override sets it after each tick.
**Why:** (1) is how SMM2's own server answers (the field is per player in each CourseInfo); a separate request per
course would cost a round trip per list. (2) Without the position, face and flags the comments would show in the
wrong place or with the wrong face, and converting drawings would need the game's picture format worked out with
nothing gained: only SMM2 shows them. (3) Every "X ago" label counts from it; a client with a wrong clock shows wrong
ages but nothing breaks, and the server's time could be sent later if that matters.
**Would change:** a web view of comments wanting drawings as images (then decode the game's format server side);
the time coming from the server (a field in the answers) if clients' clocks prove unreliable.

## 2026-10-23 — Vulkan shader global memory: the heap's device address; per-draw copies only without host import
**Decision:** shaders reach the guest heap (LDG / STG) through the import's device address (bufferDeviceAddress, a
`buffer_reference` in the GLSL; binding 17 holds the address and size) wherever the heap is imported, and through
per-draw copies of the bound storage buffers only where there is no host memory import (from sm35recomp e292dce and
3fce300).
- **Why the address:** a storage buffer binding reaches only maxStorageBufferRange of the 3.2 GB heap (128 MB on
  lavapipe and Adreno, 1-2 GB on some desktop GPUs), and reads past it went wrong: 3D World Mario was a white blob on
  lavapipe, and the validation layer flagged every bind. bufferDeviceAddress is required by Vulkan 1.3, which the
  backend already needs, so every device the backend runs on has it; it isn't a second path that most players skip.
- **Why copies only without the import:** a per-draw copy costs CPU time for every draw that reads global memory, and
  a draw's stores into the copy are lost; with the import the address reaches the real memory at no copy. Without it
  there is nothing to address, and the copies bring back what read 0 (the HUD text).
- **Kept as switches:** `LARECOMP_VK_NO_BDA=1` (the old binding), `LARECOMP_VK_NO_GMEM=1` (no import: the copies).
- **Not done:** SignedZeroInfNanPreserve in every SPIR-V module (so drivers can't fold the FMZ and isnan tests the
  translated code relies on) was tried for the remaining eye NaN and made it worse on lavapipe.
**What would change it:** a device whose bufferDeviceAddress reads are slower than a binding (then bind when the
reads fit in the range); a driver that can't take the address of host-pointer imported memory (then the copies there
too); compute without the import mattering in a game (then copy the dispatch's buffers in and back as gfast does).

## 2026-10-23 — Linux Vulkan tests on the Mac run in Docker, not a VM
**Decision:** Linux arm64 runs for Vulkan work go through `recomp/tools/linux_vk_docker.sh`: the zig cross-build (as
the release packages) run in a Debian 13 arm64 container on lavapipe under Xvfb, with the build and the data folder
mounted at their own paths (read-only where they can be). The Lima VM it replaces was gone, needed its own disk
image and a copy of the build; Docker is already installed, starts in a second, and the image holds only Debian
packages (no game data, never pushed).
**What would change it:** a real Linux machine with a GPU for the testers' cases (lavapipe is software and has quirks
of its own: the MRT split, the eye NaN), or an x86-64 need (the same script with an amd64 image under emulation).

## 2026-10-23 — "Require course clear to view": the server sends every comment, the game hides them
**Decision (agent):** our server gives every comment on a course to every player, as Nintendo's did, and leaves
"Require course clear to view" to the game: SMM2 itself hides such a comment, in the course and in the comment list
("You can view comments for this course after clearing it."), from a player whose play state for the course isn't
"cleared", unless they posted it or made the course. What the mod has to get right is that play state (course record
+0x184), which it now fills from the server's `you` (1 not played, 2 played, 3 cleared).
**Why:** the game needs the hidden comments in its list to show their "clear required" placeholder and to count them,
and it already knows the poster and the maker; filtering on the server would need the same three rules twice and
would drop the placeholder. The comments are not secret, only spoilers.
**What would change it:** another client of `/api/courses/<code>/comments` (a web view, SMM1 / SMCraft) that doesn't
apply the rule itself (then a `?for=<viewer>` filter on the server).

## 2026-10-23 — Nextendo's login failing never takes Course World down: the session falls back to our server

**Decision:** with a backend whose login the game itself runs (Nextendo; local-nex in hybrid), an error the game shows
before that login has ever succeeded switches the rest of the session to backend `ours`
(`runtime/net/netcfg.c` net_backend_fall_back; the mod decides in `games/smm2/mods/online.c` game_online_error). The
mod then answers the next login and every Course World request, as it does with `ours`, and a notice says
Network Play is off until the next start. At start, an account Nextendo answers "refused" for (online-status: on
another device, e-mail not verified, ...) runs the session on our server straight away. The setting itself is kept.
**Why:** the owner's decision (Nextendo is for multiplayer only; Course World, the Endless Challenge and 100 Mario
stay on TGRCode or our server whether or not the player is signed in), and the owner's report: after a restart with
Nextendo signed in, Course World stopped working until they signed out. An error after a successful login (a match
not found) changes nothing, so one failed match doesn't switch Network Play off.
**Would change:** a way to run the game's NEX login only when Network Play is opened (then Course World would never
wait on it at all).

## 2026-10-23 — Costume loss: SMM1's look over SMM2's power-down; a costume turns through the holder's facing override

**Decision (agent):** a hit in costume stays SMM2's Super -> small power-down for the game (its freeze, sound and
invulnerability), but what the holders show is SMM1's costume -> small sequence, driven by the mod each update:
small, costume, small, costume, small, 6 updates each (SMM1's transform type 3, 30 updates, Block.rpx v208
0x0268151c / 0x02681530), then small Mario. A costume faces left by setting the holder's facing override (+0x4b0)
to the facing passed to its placement (0x7101561bb0) for slot 12, which SMM2 otherwise never turns (Balloon Mario).
**Why:** the owner's v0.4.5 reports (the loss looked wrong; the costume kept facing right). SMM2's SMB1 power-down
never calls the slot setter, so mapping its "Super" frames to the costume (v0.4.5) left the costume on screen through
the freeze and its blink; a schedule of our own matches SMM1 frame for frame. SMM1's freeze length was an open item
(smm1-extras-costume.md §9): the transform's 30 updates, the same as SMM2's freeze here. The facing override is the
holder's own mechanism, so the placement keeps everything else (offsets, shadow) as for small Mario.
**What would change it:** a costume damage pose (SMM1 has none: no damage frame in the costume textures), or a slot
other than 12 for the costume (then the placement turns it by itself).

## 2026-10-23 — SMM1's sound effects: a level per group, set against the SMM2 sounds they stand in for, and a cap

**Decision (agent):** SMM1's sounds keep their own `.bameta` volumes, and each group gets its own factor against
SMM2's mix (`mods/smm1/smm1_audio.c`): music 0.3 (unchanged), the 100 Mario screens' effects 0.1, the costumes'
effects 0.3; and no effect plays louder than -25 dB (its loudest 50 ms RMS), SMM2's loudest course sounds.
**Why:** the owner's report (SMM1's effects louder than SMM2's). Measured with `LARECOMP_AUDIO_WAV`: SMM2 plays its
menu sounds (about -29 to -37 dB in Course World) well under its course sounds (-25 to -29), and SMM1's screen
sounds are mastered hot, so one factor can't fit both. The screens' factor is set on a sound both games have: the
map's Go jump is SMB1's jump wave, and at 0.1 it plays as loud as SMM2's own (-28.4 against -28.3 / -28.9 dB). The
costumes' sounds were already at SMM2's course level at the median; what was louder is a quarter of them (shouts
near full scale, up to 9 dB over), and a cap fixes those without changing the rest. SMM1's own engine levels (its
sound players' volumes) aren't known; the costume banks' `.bameta` volume field is 0, read as unset (1).
**What would change it:** SMM1's real output levels from its engine (a sound player or category volume per group),
or the owner hearing a group as off: each factor is one constant, and `LARECOMP_SMM1_SE_GAIN` /
`LARECOMP_SMM1_COSTUME_GAIN` try others without a build.

## 2026-10-23 — Course World pictures: never waited for on the game's frame; a stock ahead of the player at TGRCode's pace

**Decision (agent):** for the owner's v0.4.6 report ("New Courses scrolling is still laggy and slow"):
1. The mod's requests to the server give the calling guest thread's emulated core up while they wait
   (`games/smm2/mods/server.c`, `kernel_host_wait_begin`/`_end`, as the game's own socket waits do). The cause of the
   lag: the network worker that asks for a card's picture shares a core with the thread that draws, and the picture
   request waited up to 4 s, so the frame stopped for up to 4 s when a list opened and at each card that scrolled in
   (measured: longest frames of 4042, 2800 and 4064 ms in New Courses' first 30 s).
2. A card's picture request (relation_get) answers at once with what the server holds (and waits up to 1.5 s only
   for a picture TGRCode is answering for at that moment). The game asks once per card and never again for one that
   failed, and a card on screen keeps its placeholder even when its cache slot is made valid later, while a card that
   comes into view again shows what its slot holds [obs]. So pictures that come later are signed into their slots (a
   host thread looks once a second; a card's update puts them in on the game's thread): the card shows its picture
   the next time it comes into view.
3. The server paces TGRCode at one request every 6 s, start to start (measured: at 3.5 s a third were 429; at 5 s the
   twelfth of each minute was 429, 60 s apart), and a 429 pauses 10 s (doubling in a row). Before: 1.5 s after the
   previous answer, so most minutes had several 429s and pauses up to 32 s.
4. TGRCode's requests go in this order: the pictures the game just asked for (latest first; dropped back after 10 s
   without being asked again), the lists when due, the stock ahead of the player (the cards around the last one
   whose picture was asked for: 3 before, 40 after, then the page and the next), the start's Endless searches (they
   wait for the first round of pictures, or for the whole stock while a list is looked at, unless a run asks), then
   the pictures of every course held.
5. New: TGRCode's `search_new` gives only the last ten minutes or so of uploads (11-20 courses, the same for minutes;
   measured; a cold start listed 7 courses in New), so the server keeps New on disk (`lists-new.json`, the newest
   300) to grow it across sessions, and Popular's listed courses follow it with the ones held, by upload time (a cold
   start now lists a full 100).
6. The search's next-page flag (+0x737ac, the original's out-parameter to the NEX call) is set for a whole page of
   100: the page arrows were always grey and every list ended at its first 100. Detailed Search now turns pages;
   Hot, Popular and New keep their arrows grey with the flag set (something else limits them: not the application
   config's values 4-36, tried at 10; open).
**Why:** TGRCode allows about ten requests a minute and gives one picture a request (no bulk picture endpoint; the
picture links in its course records are Nintendo's CDN, never contacted), while the game shows three cards and asks
for each as it scrolls in. No pacing keeps up with scrolling, so the game must never wait for it, and each request
should be the picture the player is most likely to see next. A card that refreshes in place needs the card's picture
path (the card update main+0x1a2cec0, the image-job worker main+0x17b78f0), not traced yet.
**Would change:** the card's own picture refresh found (a late picture would show at once); TGRCode offering several
pictures a request or a higher limit; our own server holding the pictures (then all of this is local).

## 2026-10-22 — macOS x86-64 builds natively, with the portable-C lowering

**Decision:** macOS is a native-only target: `make` builds for the host architecture (`uname -m`), so an Intel
Mac builds an x86-64 executable and `.app` without a cross toolchain. The host architecture also selects the game
code's FP/SIMD lowering: the generated code's inline asm on Apple silicon (`generated/smm2`, `--simd asm`), the
portable-C lowering on x86-64 (`generated/smm2-c`, `--simd c`), the same one Windows / Linux x64 use, built with
`-march=x86-64-v3` (AVX2 + BMI2/FMA, checked at startup by `require_cpu()` in `runtime/platform/macos.c`; a CPU
without them gets a message instead of an illegal instruction). Homebrew paths are detected for both prefixes
(`/opt/homebrew` and `/usr/local`), and the tool scripts (`bundle_macos.sh`, `build_deps_macos.sh`,
`build_hactool.sh`, `fetch_python.sh`, `fetch_zig.sh`, `package_lib.sh`) and `requirements-gen.txt` take
`macos-x86_64` where they took `macos-arm64`.
**Why:** the recompiler emits host code and the Metal layer is host architecture-agnostic, so an Intel build is the
same pipeline with a different code generator; the x86-64 SIMD asm lowering doesn't exist yet, and the portable-C
one is proven on Windows / Linux x64 (the SIMD differential test passes for `x86_64-macos` with zig cc: 3000
encodings, 35988 cases).
**Would change:** an x86-64 inline-asm lowering (for speed and to match the other platforms' code shape), or a
universal (arm64 + x86-64) `.app`.

## 2026-10-22 — ASTC is decoded on the CPU where the Metal GPU has no ASTC textures

**Decision:** at backend init the Metal renderer sets `g_astc_soft` unless the device reports the Apple GPU family
(`supportsFamily:MTLGPUFamilyApple2`; `RECOMP_ASTC_SOFT=1|0` overrides it). In that mode `mtl_format` maps NVN's
ASTC LDR formats (`0x79`-`0x94`) to `RGBA8Unorm` / `RGBA8Unorm_sRGB` (the sRGB half of the range), and
`upload_texture` decodes each level's blocks with `runtime/nvn/astc.c` (the decoder the Vulkan path already uses)
before uploading, per depth slice.
**Why:** opening the Course World on a Metal 2-only Intel Mac (NVIDIA GT 755M, GPU family Common1/2) aborted in
`-[MTLTextureDescriptorInternal validateWithDevice:]` with "invalid pixelFormat (204)" (`ASTC_4x4_LDR`); Metal
validates descriptor formats at creation and there is no support query that would let the renderer fall back per
texture. The alternatives were to guess from the device name / `Mac2` (the GT 755M reports `Mac2 = 0`, but older
Intel GPUs are ambiguous) or to decode always. Checking Apple GPU families is exact for the GPUs that have ASTC and
falls back to a known-correct (just slower at upload) path elsewhere; Intel Macs with native ASTC merely lose a
one-time hardware decode.
**Would change:** a per-pixel-format support query arriving in Metal, or Intel Macs' upload cost mattering enough
to allow `Mac2` devices through after checking their vendor another way.

## 2026-10-22 — Packed 16-bit colour textures are decoded on the CPU where the Metal GPU rejects them

**Decision:** the same non-Apple-GPU test that turns on the ASTC fallback turns on `g_soft16`
(`RECOMP_SOFT16=1|0` overrides it). `mtl_format` then maps NVN's packed colour formats — RGBA4 `0x39`, RGB5 `0x3a`,
RGB5A1 `0x3b`, RGB565 `0x3c` (and the BGR5 variants `0x95`-`0x97`, which the Metal path already maps to `A1BGR5`) — to
`RGBA8Unorm`. `upload_texture` expands each 16-bit texel on the CPU with the same bit layouts `vulkan/textures.c`
uses (`R4G4B4A4`, `R5G6B5`, `R5G5B5A1`), and buffer→texture copies do the same expansion. Texture→buffer readbacks of
these textures are skipped like the other host-expanded formats (SMM2 does none).
**Why:** shooting a fireball in the first level made the game create an RGB565 texture, and the GT 755M's Metal 2
driver aborted in `MTLTextureDescriptor` validation with "invalid pixelFormat (40)" (`B5G6R5Unorm`). `A1BGR5Unorm`
(41, `0x3b`/`0x95`-`0x97`) and `ABGR4Unorm` (42, `0x39`) are rejected the same way (`RGBA8Unorm_sRGB` /
`RG8Unorm_sRGB` 11/31 too, but no NVN format maps to them). `BGR5A1Unorm` (43) is supported, but its bit layout
(`A` in the low bit) is not NVN's `0x3b`, so it can't substitute. There is no per-format support query, as with ASTC;
on a device where the formats exist the decoded path is still correct, just slower at upload. The first level
then completed on the GT 755M with fireballs and no abort (2026-10-22).
**Would change:** a per-pixel-format support query in Metal, or proof that some non-Apple families support the
formats (all of them are gated on `Apple2` today, so Intel Macs with native RGB565 pay a one-time CPU expansion).
## 2026-10-22 — OpenCourse World: the console mod ported, one narrow opening in the network policy

**Decision** (owner, 2026-10-22: "how can we make this ryujinx mod work with this recomp"): SMM2's own online code
talks to **OpenCourse World** with the player's own account, as the Atmosphère / Ryujinx mod does it on a console.
A new backend `ocw` (`runtime/net/netcfg.h`) next to `ours`, `local-nex` and `nextendo`, and
`games/smm2/mods/ocw.c` reading `ocw-config.json` (`domain`, `token`, optional `ip`) from the data folder.

- **Ported, not loaded.** The console mod (`_vendor/ocw-client-mod`, exlaunch) ships a `subsdk9` that rewrites live
  AArch64 instructions. The recomp loads only `main` and `sdk` and runs recompiled C: there is no instruction left
  to patch at run time, no subsdk slot, and a recomp "mod" is a RomFS overlay plus C hooks.
- **Two of its ten patches were needed.** The domain string (`main+0x22cc307`, written at boot: the module's whole
  mapping is read/write) and the WebSocket handshake (`main+0xAF560`, a generator patch so the host builds the
  header and the token rides in it). The other eight stub `nn::account` calls that reach real Nintendo services on
  a console; here they reach `runtime/services/acc.c`, which already reports a network service account and hands
  out a local ID token whenever the game's own sockets are on. The two remaining ones (word filter, corrupt-course
  check) became **their own mod** with their own switches (`mods/relax.c`, owner's call) rather than riding along
  with OCW: they are useful with any server, and OCW must not quietly change the game's behaviour.
- **Deny-by-default stands; this is one opening in it.** Exactly the one name `domain` gives resolves (from `ip` if
  present, else the host resolver; every other name still fails and the game's names still never reach the host
  resolver), only the addresses that name answers with are reachable, the name is re-resolved every five minutes
  (OCW is in AWS and will move) and addresses are merged rather than replaced so a live session keeps working.
  Nothing happens at all unless `backend=ocw` **and** a usable `ocw-config.json` are both there.
- **Certificates are not checked there** (owner, 2026-10-22: "ignore tls certs and trust any cert, even self
  signed; that's ok for our use case"). `tls_trust=any` already existed and already mapped to
  `MBEDTLS_SSL_VERIFY_NONE`; the only change is the gate in `services/ssl.c` that refused it towards a non-local
  address, now `net_tls_any_allowed()` (local addresses, or the ocw host). It is the default under this backend,
  which is also what makes a self-hosted OCW with a self-signed certificate work. The trade is explicit: anyone on
  the path can read or alter the session, including the 22-character token in the handshake. Opt-in, never the
  default, and `tls_trust=local-ca` with `tls_ca=` turns verification back on.
- **TLS stays a 1.2 client.** OCW accepts TLS 1.2 (checked 2026-10-22: Let's Encrypt ISRG Root X2, ECDSA P-384,
  which the vendored Mbed TLS supports as built), so the "TLS 1.2 client only" configuration is unchanged. `ssl.c`
  pins minimum and maximum, so this is the thing to re-check if OCW ever tightens its TLS.

**Why:** the owner's own OCW mod and account, and OCW is a live service the player already has a relationship with,
so the game's own online code against it is the shortest path to a working Course World. The alternative — an OCW
client host-side in Rust as a course upstream next to TGRCode (`core/crates/server/src/upstream.rs`) — keeps the
guest network off entirely and needs no policy change, but means reimplementing OCW's protocol instead of letting
the game speak it; recorded in `tasks/todo.md` for comparison. OCW's public web API has course metadata but no
course download, so a TGRCode-style HTTP source was never an option (`PROGRESS.md`).
**Would change:** OCW requiring TLS 1.3; wanting verification on by default (a pinned OCW CA is the third trust
mode `DECISIONS.md` already anticipated); or the game's own Course World code turning out not to reach far enough
in the recomp, which would make the Rust upstream the better route after all.
