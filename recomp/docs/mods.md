# SMM2 recomp mods

Mods change the recompiled game at run time. Their code is ours (`games/smm2/mods/`, `runtime/net/`); they ship no
game code or data. Anything they need from a game is made on the player's machine from the player's own dumps, into
`private/`, which git ignores.

## Online against our server, and the 100 Mario Challenge

One mod does two things:

- **Course World against our own server** (`mmk server`, core/crates/server) or a **local library** run the same way,
  never Nintendo's servers. SMM2's own **Endless Challenge** works as SMM2 made it, under the server's `smm2` rules
  (5 lives, endless).
- **SMM1's 100 Mario Challenge**, SMM1's rules: **100 lives**, lives cap at **100**, a run is won after
  **8 / 16 / 16 / 6** clears (Easy / Normal / Expert / Super Expert), **skips are free** (also during the miss
  animation), a miss throws away the coins and score of the failed attempt, an award worth several lives banks one
  extra life (at most 3 per course, paid on the clear), Expert opens after a won Normal run and Super Expert after a
  won Expert run. SMM1's screens, map and ending are drawn host-side from the player's SMM1 dump (another part of the
  mod: `games/smm2/mods/smm1/`, the SMM1 screens; needs the SMM1 dump: `RECOMP_SMM1_DUMP`); the courses play in SMM2's own course play. The run side is
  described below ("The bridge").

### Turning it on

Environment variables, or `smm2mods.ini` (`key=value` lines) in the data folder (`private/smm2/` in a source build):

| Variable | ini key | Meaning |
|---|---|---|
| `RECOMP_MOD_100MARIO=1` | `hundred=1` | the mod is on (Course World online against our server) |
| `RECOMP_MOD_SERVER=<url>` | `server=` | the course server, plain `http://` (default `http://127.0.0.1:8080`) |
| `RECOMP_MOD_TOKEN=<token>` | `token=` | your account's token on that server (see below); keep it private |
| `RECOMP_MOD_RULESET=hundred\|smm2` | `ruleset=` | `hundred` (default): 100 Mario is offered: Course World's Endless Challenge button opens SMM1's 100 Mario Challenge, and B on its difficulty select leads on to SMM2's own Endless Challenge (both in one build; SMM2's rules there until the player leaves that menu); `smm2`: the button opens SMM2's own Endless Challenge only |
| `RECOMP_MOD_COURSES=server\|tgrcode` | `courses=` | `server` (default): the server above; `tgrcode` (iOS default, with the mod on; hybrid's default, below): a course server built into the app on a loopback port, with a local account, fed from TGRCode's archive of SMM2 courses (needs the course key table: the mod writes it once from your game into `mods-generated/course_table.bin`, private); falls back to `server=` when the build has no built-in server. Also in the Mods tab ("Courses: TGRCode"), from the next start |
| `RECOMP_MOD_COURSE_WORLD=mod\|backend` | `course_world=` | only with another online backend than `ours` (below): `mod` (nextendo's default): **hybrid**, the mod answers Course World, the Endless Challenge and 100 Mario while the game's own NEX client logs in to the backend; `backend`: all of Course World from the backend, the mod's online overrides off. With `local-nex` only `mod` turns hybrid on |
| `RECOMP_MOD_TIMEOUT_MS=<n>` | `timeout_ms=` | per request (default 8000) |
| `RECOMP_MOD_LOG=1` | `log=1` | log every request and answer |

    RECOMP_MOD_100MARIO=1 RECOMP_MOD_SERVER=http://127.0.0.1:8640 RECOMP_MOD_TOKEN=... make -C recomp run-smm2

Then: Play -> Course World -> Endless Challenge.

### Server

Any `mmk server` works (yours, a friend's, a community one):

    cd core && cargo build --release -p mmk-cli
    target/release/mmk server --dir my-courses --addr 127.0.0.1:8640
    target/release/mmk register MyName --server http://127.0.0.1:8640      # prints your maker ID and token
    target/release/mmk upload --server http://127.0.0.1:8640 courses/*.zlib # raw, zlib, or .bcd with --keys

**Local library** (no network at all): the same server on 127.0.0.1 with a folder of your own courses (your saved
courses, the decrypted course data, or exports). Courses without play statistics fall into the difficulty bands by
the server's rules (unplayed = 50 % fail rate, the band widens when one runs dry).

The server holds the run: lives, clears, the course queue, the history, completed runs (`hundred_<difficulty>`
leaderboards) and SMM2-rule high scores.

### How it works

| Piece | Where | What |
|---|---|---|
| HTTP client | `runtime/net/http.c` | blocking HTTP/1.1, timeouts, sockets (Winsock on Windows); one connection per request |
| JSON | `runtime/net/json.c` | a small reader for the server's answers |
| Pretend online | `runtime/net/online.h`, `services/nifm.c`, `acc.c`, `mii.c` | with the mod on (backend `ours`; hybrid has a real connection): nifm reports a connection, acc a network service account (a local placeholder token), mii a default Mii; nothing talks to Nintendo |
| Login | `games/smm2/mods/online.c` | the game's network session ("enl") logs in without NEX; Course World's login sequence gets neutral answers and the player's record (name from the server, a default Mii, registered) |
| Endless | `games/smm2/mods/endless.c` | the Endless Challenge's request functions answered from the server (below) |
| Bridge | `games/smm2/mods/bridge.c`, `hundred.h` | 100 Mario: SMM1's screens on top, SMM2's play underneath |
| Rules | `games/smm2/mods/hooks.c` + `patches.py` | host code after single instructions (22 points): lives clamps 99 -> 100, free skip, coin / score rollback on a miss, one banked life per award; only in a 100 Mario run |
| Settings | `games/smm2/mods/settings.c` | the variables above |

The game's Course World requests are each one function (a "wrapper") that the network worker calls through a
pointer; the mod replaces those it needs with `hle_override` and runs the game's own otherwise. Every other Course
World screen runs the game's own code, which finds no connection and shows "A communication error occurred."
The mod's requests to the server give the calling thread's core up while they wait (`server.c`, as the game's own
network waits do): the game's worker threads share cores with the thread that draws, and a picture the server waited
4 s for stopped the frame for 4 s (Course World's lists froze at each card, owner's v0.4.6 report).

| Request (v3.0.3) | Answered from |
|---|---|
| GetEndlessModeStatus | `GET /api/endless`: lives, clears, coins, score per difficulty; start lives 100 (5 under `smm2`) |
| InitEndlessMode (New Game) | `POST /api/endless/<d>/start {"ruleset"}` |
| GetEndlessModePlayInfo | `GET /api/endless/<d>/history` |
| GetEndlessModeRank | `GET /api/rankings/hundred_<d>` (or `endless_<d>`), the `me` row |
| SearchCoursesEndlessMode | `GET /api/endless/<d>/next?count=10` |
| GetObject (course file) | `GET /api/courses/<code>/data`, given the game's header and its own course encryption, so its CRC and validity checks run unchanged |
| StartEndlessModeCourse | first post for a course: the run as it is; a second post for the same course (sent during a miss): `result: death` |
| DominateEndlessModeCourse (clear) | `POST /api/endless/<d>/result {"result":"clear", pending, coins, score}` |
| PassEndlessModeCourse (skip) | `result: skip` |
| Suspend / FinishEndlessMode | `POST /api/endless/<d>/suspend`, `/finish` |
| GetUsers (own) | `GET /api/me` (name, maker ID), best per difficulty (completed 100 Mario runs under `hundred`), the courses uploaded and `upload_limit` of `/api/makers/<id>` (My Info's "Courses uploaded", record +0x190 / +0x194) |
| GetCourses (by id) | `GET /api/courses/<code>` |
| course list info, an id check | answered empty / passes |
| ReportFromBugDetection (the game found a downloaded course invalid: "The course data is corrupt.") | in a run: `result: broken` (the course leaves the queue); the bridge answers the window, the flow hears `HUNDRED_SKIP`, the next course comes |
| GetApplicationConfig | Endless switched on for the four difficulties |

Data ids and course codes map 1:1 (`games/smm2/mods/course.c`, the same arithmetic as `mmk_formats::course_id`).
A maker's player id is a hash of their maker ID on our server (the player's own too), so the game's "my course"
and "me" checks work.

### Course World against our server (`games/smm2/mods/world.c`)

The rest of Course World goes to our server too. Each request's name is the DataStore (SMM2) method its function
calls (read from the client vtable slot it uses):

| Request (method id) | Function (main+) | Answered from |
|---|---|---|
| course lists: PickUp (84, "Hot"), PointRanking (71, "Popular", by difficulty), Latest (73, "New"), Advanced (72, detailed search: difficulty, style, sort), PostedBy (74, a maker's courses), BestTime / FirstClear (81 / 80) | 0x14d8550 (search type +0x746e8) | `GET /api/courses?sort=popular\|new&style=&offset=&limit=100`, filtered into the game's difficulty bands (a whole page of 100 sets the search's next-page flag +0x737ac: Detailed Search's page arrows at the top right go by it, before 2026-10-23 they were always grey; Hot, Popular and New keep theirs grey, open); a maker's courses from `GET /api/makers/<id>`; a player's Played Courses / Liked Courses (PlayedBy 76, PositiveRatedBy 75) from `GET /api/makers/<id>/played` / `liked`; followed makers and versus: empty |
| GetUserOrCourse (131): the ID search | 0x14dc0d0 | `GET /api/courses/<code>`, else `GET /api/makers/<code>` |
| GetUsers (48): the makers of listed courses, a maker's page | 0x14d02d0, 0x14d7780, 0x14d2f00 | `GET /api/makers/<id>`: name, maker code, likes received and maker points from their courses, the courses uploaded and the server's `upload_limit` (record +0x190 / +0x194: a maker page's "Courses uploaded" header); comments on (+0x98); the default Mii |
| SearchUsers* (50, 51, 57, 58): leaderboards | 0x14d1820 (kind +0x73a14) | `GET /api/rankings/maker_points` (makers, also the period's board), `endless_<d>` (Endless), `clears` |
| CanPostRatingAndComment (61) | 0x14ebc60 | allowed (below) |
| PostRatingInfo (104): like / boo | 0x14e6b90 | `POST /api/courses/<code>/like` or `/boo`; a rating taken back (0): `DELETE /api/courses/<code>/vote` |
| PostPlayResult (96) | 0x14e30e0 | `POST /api/courses/<code>/play`, and `/clear {time_ms}` on a clear (not for Endless courses: those are counted by the Endless reports) |
| SearchComments (94 / 95); the text, stamp and drawn comment; deleting one's own | 0x14e8ec0; 0x14e96b0, 0x14eaa20, 0x14e9c10; 0x14eadb0 | `GET` / `POST /api/courses/<code>/comments` (SMM2's whole comment, below), a drawing's picture `PUT` / `GET /api/comments/<id>/picture`, `DELETE /api/comments/<id>` |
| a drawn comment's picture (the relation object, as for a course's picture) | 0x14eaf30 | `GET /api/comments/<id>/picture` |
| CanPostCourse (60), the course upload (66 + upload + 67), DeleteObject | 0x14e0d40, 0x14de3f0, 0x14e0c50 | allowed; the game's own course (header "SCDL", not yet encrypted) to `POST /api/courses` as the signed-in maker (`?based_on=` for an edited copy, below), its code back as the upload's data id, then its picture to `PUT /api/courses/<code>/thumbnail`; `DELETE /api/courses/<code>` |
| GetReqGetInfoHeadersInfo (134) + HTTP: a course's picture ("relation objects", 5 at a time) | 0x14e0e70 (`edit.c`) | `GET /api/courses/<code>/thumbnail?wait_ms=` (below) |
| the same for a course's whole-course map (More info) | 0x14e2cf0 (`edit.c`) | `GET /api/courses/<code>/entire_thumbnail?wait_ms=` |

Course World courses download and play exactly like the Endless ones (the same GetObject). Super Worlds: below. Not
served: Ninji speedruns, notifications, follows, Network Play.

**Likes and boos** (checked in game 2026-10-23, two accounts: one player's course liked / booed by the other). SMM2
offers them on the clear screen (Boo! / I like it!) and nowhere else in Course World: a course's card has none. Two
things decide whether the hearts take input (main+0x1be73b0): the player's own relation to the course, which the
game keeps in the course record (+0x188 of a list entry, the course global main+0x2c4b710 +0x1b8: 1 not played, 2
played, 3 liked, 4 booed [inf: PostRatingInfo's answer, main+0x151fd00, sets 3 / 4 / 2 for like / boo / take back;
after a play the game moves 1 to 2 itself, main+0x151a520]), and the CanPostRatingAndComment answer (rating +0xe8580: u8 +8 and u32 state +0xc). The hearts are live at
relation 1 or 2 with state other than 2 [obs]; state 2 greys them [obs], relation 0 leaves them drawn but dead [obs:
before this, the relation was always 0: "took no test input"], 3 / 4 show the choice made. The
game asks for the course record again after PostPlayResult (GetCourses), so it learns it from the server: every
course object carries the signed-in player's `you` = `{played, cleared, vote}` (`Store::annotate`), which
`mod_fill_course` turns into +0x188; CanPostRatingAndComment answers state 0. A course already liked shows "I like
it!" chosen on its next clear (and Boo! greyed), as on a console. The like counts on the course card, Liked Courses on
the player's page and the maker page's Likes / Maker Points come from the server's votes. Evidence (private):
`evidence/mods/world-social/` (clear_screen_*, course_card_*, my_info_liked_courses, maker_page_after_likes).

**Comments** (checked in game 2026-10-23: text, stamp and drawn comments posted, listed with the poster's name, Mii
and time, shown under the clear screen, a drawing fetched in a new session, one's own deleted; `comments_*`,
`post_comment_*`). The comment button wants the maker's
"comments on" (user record +0x98, the course's +0x830 / course global +0x860: `fill_user` sets it) and the comment
answer's two allowances (+0xe8530 +0x10 / +0x11: 10) above the player's comment count on the course. Post a Comment's
three tabs post through three requests (main+0x17f88e0; NetAppTask 0x20 / 0x21 / 0x22) with one request record:
course +0x8e688, posted from the clear screen +0x8e690, type +0x8e694 (0 drawing, 1 text, 2 stamp), where the player was
+0x8e698 / +0x8e69c, in the sub area +0x8e6a0, the face picked for the Mii +0x8e6a1, "Require course clear to view"
+0x8e6a2, then the text (UTF-16, 75 characters) at *(+0x8e6b0), the stamp's number +0x8e758, or the drawing at
*(+0x8f400) (its size +0x8e75c; the game's own compressed data [obs: 944 bytes], the picture it keeps; a second
0x1c200-byte buffer at *(+0x8f408) is not kept). The server keeps all of it (`Comment`: `type`, `text`, `stamp`, `x`,
`y`, `sub_area`, `face`, `clear_required`, `cleared`, and the drawing as `comments/<id>.bin`) and gives each comment
an id, which goes back into the request's answer (*(+0x8e768)). A listed comment is the 0x390-byte record the game's
converter fills (main+0x14b7720; the fields in `world.c`), with +0x388 set (unset, the list showed empty bubbles)
and, for a drawing, the comment's id as its picture's key (+0x148, +0x268): the game asks main+0x14eaf30 for the
pictures not in its cache (main+0x14b8930: type 0 only; before this every comment counted as a drawing, and the
unanswered request gave "A communication error occurred."), and the mod fills its download buffers from the server.
Delete (one's own comments: the list shows Delete instead of Report) is main+0x14eadb0 with the comment's id.
"X minute(s) ago" counts from the game's server time (the NEX core object *(main+0x2a17f08) +0x110 / +0x118, set
each NEX tick, main+0x21710, from the connection): with no connection it stays unknown and every comment showed its
layout's placeholder, "5分前"; the mod's tick sets it from this machine's clock (`nex_tick`). The software keyboard
now shows the game's own text as its title (ShowKeyboardArg header / sub / guide text: "Please input the Course ID
or Maker ID you want to search for."), and takes up to the game's length (a comment's 75; the dialog stopped at 8).

**Comments in the course, "Require course clear to view", the maker's view** (checked in game 2026-10-23 with three
accounts: the poster, the course's maker and a player who had not cleared it; `evidence/mods/world-social/
comments-in-course/`). The course start asks for the comments (SearchComments mode 1) and the game shows the ones
posted from the pause menu (record +0x58 0) as bubbles at the place they were posted (x / y in pixels, 16 to a block):
text, stamps and drawings, with the poster's Mii and face, the maker's framed; they open one at a time and the ones
near the player stay closed (the game's own display, main+0x17f39e0). The ones posted from the clear screen (+0x58 1)
show there instead. The pause menu's Comment posts from where the player stands, and its Display Comments switches
them. "Require course clear to view" is the game's own: it hides such a comment, in the course and in the list ("You
can view comments for this course after clearing it."), from a player whose play state for the course (course record
+0x184) isn't 3, unless they posted it or made the course. That state was always 0, so nobody but the poster and the
maker saw them, even after a clear: `fill_course` now gives it from the server's `you` (1 not played, 2 played, 3
cleared), and the rating +0x188 1 for a course not played (0 drew a heart on its card; the red cleared flag shows at
3). The server sends every comment, as Nintendo's did (DECISIONS 2026-10-23). The maker sees all of them, with Delete
on their own and Report on the others': SMM2 offers the maker nothing more on their course's comments (the server
would take the uploader's DELETE). Drawings: the drawing canvas is a pitch-linear texture (NVN flag
LINEAR_RENDER_TARGET, 0x40) that the runtime read as block-linear, with a 4096-byte row (device integer 57, the linear
row alignment, now 32): a drawn line showed as dashes or a column of blocks, thin ones not at all; the scripted drags
were fine. Drawings posted before the fix (test data only) show the game's broken-picture sign.

**Thumbnails** (a course's picture on its card and course page, its whole-course map under More info): the server
keeps them as JPEGs (`thumbnails/<code>.jpg`, `<code>-entire.jpg` in its folder). For a course from TGRCode's archive
the server fetches both from TGRCode (`level_thumbnail` is the game's thumbnail block, `level_entire_thumbnail` a
plain JPEG), one request at a time with the course imports, one request starting every 6 s at most (TGRCode answers
429 to more than about ten or eleven a minute; a picture takes about 1 s). The order: the pictures the game just
asked for (the cards it shows, the latest first; one not asked for again within 10 s drops back), then the stock ahead
of the player (`lists.rs`: the cards around the last one whose picture was asked for, 3 before and 40 after, then the
rest of the page and the next; before any page, the first cards of Hot, New and Popular), then, while it has nothing
else to do, those of every course it holds. The start's Endless searches wait for the first of those pictures (all of
them while a list is being looked at) unless a run asks for courses.
How the game asks [obs, 2026-10-23]: the list's request builder (main+0x14b5990) asks for up to five cache slots
that are wanted and neither valid nor failed, as each card comes into view, once each; the answer's completion
(main+0x1520800) puts every block through main+0x14b55b0 (a direct call: no override reaches it), which marks a slot
without a picture failed for good. A card on screen keeps the placeholder even when its slot is made valid later;
a card that comes into view again (scrolled out and back, a page turn) shows what its slot holds. So `relation_get`
answers at once with what the server holds (`soon_ms=1500`: and one TGRCode is answering for that moment), the
others show the placeholder, a host thread looks once a second (`?peek=1`, nothing asked) for up to 2 minutes, and
a card's update (main+0x1a2cec0, the game's thread) signs the ones that came into their slots through the same
function (valid): the card shows its picture the next time it comes into view. The server answers 202 for a picture
it is fetching and 404 for one that won't come. A map waits up to 6 s. The mod signs each JPEG into the block the
game checks (the
JPEG, then in the last 0x64 bytes its size, HMAC-SHA256 under a key from four seed words, the seeds; main+0x1698a80)
with the game's own functions; the map's 0x19000-byte block takes the same tail [obs: the map shows]. Our own
uploads: the picture the Course Maker made goes up after the course (`world.c` post_pictures: the thumbnail block
that follows the course in the upload buffer, +0x5c000, its JPEG's size at +0x1bf9c) as `PUT
/api/courses/<code>/thumbnail` (signed in, the course's uploader only; the server keeps the JPEG). The upload buffer
holds no whole-course map [obs: one JPEG from +0x5c000 to +0xb0000], so an own upload's More info has no map.
Checked 2026-10-22: an upload's card in New Courses shows its picture (earlier uploads the placeholder).

**Download** (the course page's Download, into the Coursebot's Downloaded Courses) works: SMM2 waits for the
course's thumbnail before it hands the download to the Coursebot (cDownload, main+0x17661f0, needs a valid entry in
the thumbnail cache, slots of 0x1c1b8 bytes: +0x1c000 data id, +0x1c1b2 failed, +0x1c1b4 valid) and keeps a course
only with a thumbnail it can check (otherwise "Corrupt data was found so the course has been deleted."). A course the
server has no picture for gets a stand-in: a 640 x 360 one-colour JPEG encoded by the mod (grey-blue [inf]), signed
the same way. A course whose picture didn't come with the list stopped the download at "A communication error
occurred." [obs]: the download asks for the picture again and the cache takes it, but keeps the list's failed mark,
which cDownload checks first; the mod clears that mark (and fills a slot still without a picture) while cDownload
waits. [obs] downloads saved (`course_data_120.bcd` + `course_thumb_120.btl`, the real picture; `_121` after a
withheld picture, `RECOMP_MOD_TEST_NO_THUMB=<code>`, the stand-in).

**Played Courses** (a maker page's tab): the server keeps each player's played courses, newest first (300): a play
posted from Course World, and every course of a challenge run (Endless, 100 Mario) on its first report; a store from
before this starts from the courses its runs passed. **Liked Courses**: the courses the player liked (checked
2026-10-23).

**The ID search and maker pages** (checked 2026-10-23). The Course World ID button's search finds a course (its card:
name, description, likes, plays, clear rate, world record, maker) or a maker (their page) by ID, and an unknown ID
gives SMM2's "Course or Maker not found."; a maker page (from a card's Mii, or the search) shows the maker's Likes and
Maker Points from their courses' votes, their Super World, Uploaded Courses and Liked Courses
(`evidence/mods/world-social/id_search_*`, `maker_page_*`).

### Super Worlds (`games/smm2/mods/superworld.c`)

Course World > Super Worlds lists, opens and plays other makers' Super Worlds from our server, which takes them from
TGRCode (spec: `research/specs/super-worlds-v303.md`). Application config value 37 = 1 switches the button on
(`online.c`; anything else: "This service is not active right now").

| Request (method id) | Function (main+) | Answered from |
|---|---|---|
| SearchWorldMap (161): Course World's list (flag +0x1b2728 = 1) | 0x14f79a0 | `GET /api/super_worlds?limit=50&wait_ms=20000`; the owners' names then come through GetUsers (world.c, up to 64 ids) |
| SearchWorldMap (160): My Info's list (flag 0) | 0x14f79a0 | `GET /api/makers/<own id>/super_worlds` (the worlds the player has progress in) |
| GetWorldMap (159): the selected world (id at +0x1b17a8) into the detail record +0x1b0d00 | 0x14f6600 | `GET /api/super_worlds/<id>?wait_ms=20000` and `/map` |
| GetWorldMap (159) into the list ring | 0x14f6100 | `GET /api/super_worlds/<id>` |
| play info get / reset / save (162 / 164 / 165), id at +0x1b1748 | 0x14f7d10, 0x14f8260, 0x14f84d0 | `GET` / `POST /api/super_worlds/<id>/progress` (the record's `status`: 2 progress to continue, 1 none; `clear`: the save's kind, 1 a course, 2 the world), `POST …/progress/reset` (always ok: the game keeps its own copy) |
| world thumbnails (87 + HTTP): the requests' ring +0x1b1c38 (0x1e0-byte records, the world id first), each into the 0xc804-byte buffer *(+0x1b25b8)[i] | 0x14f8850 | `GET /api/super_worlds/<id>/thumbnail` (our uploads' blocks); the rest (TGRCode's): the game's blank placeholder (below) |
| RegisterWorldMap (158): upload | 0x14f5940 | the map encrypted with the game's own function and the world key table, then `POST /api/super_worlds?data_ids=&worlds=&planet_type=&version=` (map + thumbnail block; `planet_type` the slot's byte +0xcd25 [inf]); the server takes only the player's own courses on it |
| DeleteWorldMap (163) | 0x14f7c30 | `DELETE /api/super_worlds/mine` |
| the player's courses taken down (128), asked by My Info's Uploaded Courses | 0x14f0090 (world.c) | none, ok |
| a maker's page / own user record with a world (user record +0x1b0) | 0x14d2f00, 0x14d5cd0 | `super_world_id` of `/api/makers/<id>`, `/api/me` (our players' uploads), then the detail as above |

**How the answers are written:** through the game's own converters, as a NEX answer would be. The mod fills a guest
WorldMapInfo (the NEX record, 0x140 bytes: id, owner, the map blob, counts, planet type, NEX DateTime, the courses'
data ids, an empty ninji map) and calls the list-entry converter main+0x14d54d0 or the detail converter main+0x14d48a0,
which resets the record, fills the entry, **decrypts the map** (the 0x500-byte blob TGRCode calls `unk1`, kept and
served as it is) with the game's own key table and the data id ring; the mod then adds the ninjis (when the source has
one per course) and the owner's name and player id at +0x948 / +0x970 [inf]. The world's courses
are ordinary courses: the course start asks GetCourses (main+0x14dbae0, endless.c, which read its id list from the
second id on and so answered nothing for a world's one-id request: fixed) and downloads with GetObject; the server
registers a fetched world's courses as listed, so it fetches and imports each from TGRCode when its data is asked for.

**Server** (`core/crates/server/src/superworlds.rs`): the pool is every world a TGRCode `/get_super_worlds` answer
named (ten random recent ones an answer), newest first, at most 300 plus our own uploads first; asked for when a client
browses and the pool is older than 10 minutes (3 answers while it holds fewer than 30; the time of the last answer is
kept across restarts), always through the one fetcher with its pacing and 429 back-off. A world's details and map are
fetched the first time a client asks (ahead of everything else) and kept for good (`superworlds/<id>.json`, `.map`).
Progress is kept per player and world (`superworlds/progress.json`); a player's upload (one each, replacing the last)
is kept here only, with its thumbnail block. TGRCode's world thumbnails are on Nintendo's CDN: never fetched.

**The world's title** ("SUPER <name> WORLD", main+0x199fb60): for another maker's world the game takes the owner's
name and Mii from the owner block of its profile copy of the world (profile +0x226010 name, +0x22606c Mii, +0x226108
set), which the game fills itself from the Course World list's entry of that world (main+0x14ba520; the detail
record's own +0x940 block is reset there). But it first checks the save's player name (save.dat +0x10, from the Mii the
player picks for Course World) against the default " " (main+0x168d300), and while it is the default it shows the
account's nickname for every world. Our Mii database is empty (runtime/services/mii.c), so the name is never set:
the mod's wrapper of the title's set-up puts the account's nickname there for the length of the call when the world
is another maker's and its owner is known, then the default again.

**Continue:** the 162 answer is copied to the profile (+0x226110, main+0x14bb710), and the world's loader
(main+0x148eae0) puts the score, lives, world, Mario's map place and the clear bits back on the map only when the
record's status is 2; the title then offers Continue beside New Game. A course end sets the record's status to 2
while there is progress and to 1 when the world is cleared, and the save's kind (+0x1b17e8) separately to 1 / 2
(main+0x1493950); the mod sends both, the server keeps the record as sent and `clear` at its highest. New Game resets
the record (status 1). +0x1b1790 is the data id of the course just cleared, +0x1b1798 the clear bits.

**Upload and delete** (the game's own flow): a world is uploaded from its Worldbot slot (World Maker's Save As puts it
there); every course on it must be one of the player's courses uploaded through the Coursebot after its clear check.
Before the upload the game checks the player's posted courses (SearchCourses type 6 with the console's account id,
answered as the player's own maker) and then sends RegisterWorldMap; afterwards it re-reads the own user record, whose
`super_world_id` (`/api/me`) names the new world. A second upload asks "will be overwritten" and the server replaces
the first. **Delete** is My Info's (the own maker page's Super World panel): the game shows it only when the user
record's player id is the account id it holds itself (GetNetworkServiceAccountId; main+0x1aa46a0 sets the panel's
"own" flag), so the mod gives the player's own maker that id (endless.c `pid_of_maker_id`); then DeleteWorldMap. The
Worldbot slot's trash deletes the local slot only.

**Thumbnails** (a world's picture: My Info's and a maker page's world panel, My Info's Played Worlds; Course World's
Super Worlds screen shows the planet type, never a thumbnail [obs: it asks for none]). The game keeps them in the
profile's world thumbnail cache (+0x226180, 50 slots of 0xca30 bytes: the block, +0xc808 the world id, +0xca28 /
+0xca29 wanted, +0xca2a failed, +0xca2b loaded; filled with the list's ids by main+0x152dfb0) and asks for up to five
wanted slots at a time (main+0x1527ed0, a second recent-thumbnail cache at profile +0x5b4930 first). The block is the
Worldbot slot's thumbnail as uploaded (+0x500): the JPEG, its size at +0xc800, no signature [obs]. The callback
(main+0x1525090) copies each buffer into the slot of its id; a failed request marks every slot not loaded as failed,
which shows a blank placeholder. The mod asks our server for each id and answers with the blocks it has; when only
some came (a TGRCode world beside our own: TGRCode's thumbnails are on Nintendo's CDN, never fetched), the others'
ids are emptied, which the callback skips, and their slots marked failed as the failure path would. [obs 2026-10-23:
the own world's picture in My Info and Played Worlds, the other world beside it blank.]

**The map objects' heap check** (fixed in the runtime): the World Maker's / Worldbot's map object (destructor
main+0x1465f40) owns an nn::ngc::ProfanityFilter that the game allocates with its own operator new; the SDK's
destructor deletes it with operator delete. On the console both modules' references to operator new / delete bind
to main's (main defines them strong, the SDK weak); our loader bound the SDK's to its own copy, whose CentralHeap
then trapped on a pointer it didn't own (World Maker's Save As, the Worldbot's exit). runtime/loader.c now binds
default-visibility definitions to the first module's, as rtld does (DECISIONS 2026-10-23).

**Seen in game** (`evidence/mods/superworlds/`): 2026-10-22 the list with TGRCode's worlds, a world's map, its
first course playing; 2026-10-23 (a local test server with a second test maker's 1-world 2-course world) the title
with the owner's name and Mii, a course cleared and its progress saved, Continue after Play again, the castle course
clearing the world (WORLD CLEAR, the ending, the list's planets flagged), My Info's Played Worlds (the entry opens the
owner's page with their world), a player's own uploaded world in My Info and the Worldbot; 2026-10-23
(`2026-10-23-upload-delete/`) two courses made, clear-checked and uploaded, a world built in World Maker with them,
uploaded from the Worldbot, listed in Course World and played there and in the Worldbot, overwritten by a second
upload, deleted from My Info; World Maker's Save As twice and ten Worldbot visits after the loader fix without a stop.
2026-10-23 (`sw3`): the own upload's thumbnail in My Info and Played Worlds, a world without one blank beside it.

### Edit any course (`games/smm2/mods/edit.c`)

Any downloaded course opens in SMM2's own Course Maker (DECISIONS.md "Edit any course"). The placement is SMM1's:
SMM1's Course World offers **Download** (`Nwk_CourseListFrame_01/02` "Download"), and its Coursebot window of a
downloaded or sample course has **Edit** above **Play** (`Lcl_CourseInfoWindow_00` `N_SampleDataWin_00`:
`L_EditBtn_01`, `L_PlayBtn_01`; the own-course window adds Upload, Change Name, Delete) [obs SMM1 v208 layouts]; the
pause menu of a Coursebot course has "Edit Course". SMM1's 100 Mario and course clear offer no edit [obs layouts].
SMM2's window of a downloaded course has only Play, Play Together and Delete, so the mod gives it SMM2's own
**Make** button:

- **The button.** The window is one layout for both lists (`TypeMode`: My Courses' `N_MyDataBtn_00` or the
  downloaded course's `N_DLData_00`), and main+0x1891880 switches off every button but the shown list's. In a
  downloaded course's window the mod shows My Courses' Make (`L_Edit_00`) alone, at the top of the button column where
  My Courses has it (a little smaller: it fits between the header and the stats [inf]), and switches it on
  (main+0x761190) after the info window machine's Appear / Disp (main+0x188c980 / +0x188cb10) each frame; for a My
  Courses slot everything goes back. Touch and the d-pad reach it [obs].
- **The d-pad.** Each window button has a cursor controller (button +0x218) in the scene's cursor manager
  (controller +0x18). A controller's +0x1460 holds its neighbour per direction (0 up, 1 down, 2 left, 3 right, then
  the diagonals; table main+0x23b2030); +0x14a0 / +0x14a4 its place, +0x14a8 / +0x14ac its size, +0x14b0 the cursors
  allowed on it (the enable call's bit). A move (main+0x74db40) takes the neighbour when it has one in the same
  manager, and stays put when that neighbour is switched off; only a button with none is matched by place
  (main+0x74e490). The window sets the shown list's neighbours and empties the others' [obs dump]: My Courses' Make
  has up = the list tab, down = Upload; the downloaded course's Delete has up = the tab, down = Play. So the
  downloaded window's Make, with no neighbours and Delete's up taken, was never reached. The mod puts Make at the
  top of that column, as in My Courses: Make up = the tab (Delete's up before), down = Delete, left / right itself;
  Delete up = Make, each frame after the window's update. The window restores My Courses' own neighbours itself
  [obs]. Checked: Play → Delete → Make and back down; My Courses' window unchanged (Play → Name → Upload); A on Make
  opens the Course Maker [obs].
- **The press** (UI event 0x130, read in the Downloaded list's cCourseInfoWindow, main+0x16e2db0) sends the list
  to its cToPlay (main+0x16e33c0), which the mod turns into My Courses' Make: the slot is prepared as for Play
  (main+0x16e44e0) and the scene change goes to the Course Maker (reason 3, cRoboToEdit, with the slot's course
  number), as main+0x16e5c10 does for My Courses. The Course Maker loads any Coursebot slot the same way.
- **The copy** is the player's own course: upload id 0, management flags 1 and 4 cleared, no clear check yet, a
  fresh creation id (so it can be uploaded as a new course). Name and description stay. The editor's Save is "Save
  As" into My Courses (the download stays as it was).
- **The original** (code and maker name from the downloaded-course record: save buffer +0xc010 + slot · 0x2a8, +0x08
  data id, +0x16c maker) is kept in `smm2edit.txt` in the data folder under the copy's creation id. When the copy is
  uploaded, `world.c` posts it as `POST /api/courses?based_on=<code>&based_on_maker=<name>`; the server credits a
  course it holds to its own maker (otherwise the given name), and Course World shows "Based on <code> by <maker>"
  under the description (`edit_listing_description`). The original's record on the server is not touched. The
  maker name is the record's 10 UTF-16 characters (+0x16c, no terminator when the name fills them: the Mii data
  follows) as UTF-8, percent-encoded.
- **From Course World's card.** The card (`Nwk_CourseData_00`) has Download, More info, View comments, Report,
  Play Together and Play; SMM1 had Download in Course World and Edit in the Coursebot, which the Make above now
  gives. For a course already in the Coursebot, SMM2's Download fetches the course and then says "You cannot save
  the same course." (the card's download machine, registered in main+0x17774a0: cDownload, cDownloaded, cDisp,
  cLaterPlay (main+0x1766830: to the Coursebot, reason 0x0D, or 0x0E into My Courses for the player's own course),
  cNextScene, cSavedAlready (enter main+0x1766bb0, exec main+0x1766c40), cAppletError, cError). The mod makes that
  dead end the Edit: in cSavedAlready, when the course is in a Downloaded slot (save.dat +0xB930 slot table: byte 0
  file number, byte 1 bit 0 a filled slot; later.dat record = file number − 0x78, +0x08 data id), it puts the course
  just fetched (NetObj main+0x2c4b710 +0x1c868), its picture (the thumbnail cache, or the stand-in) and an empty
  replay block where the Course Maker's loader takes a slot's data (save buffer +0x6C000 / +0xC8000 / +0xE4000;
  main+0x100e3ba90 copies them for any slot that isn't the current course), makes it the player's own (as above)
  and requests the Course Maker with that slot (reason 3). Without that preparation the Course Maker opened a blank
  course [obs]. So: Download, then Download again = Edit.
- **The card's button text.** For a course already in the Coursebot the card's Download reads SMM2's own word for
  opening a stored course in the Course Maker: the Coursebot window's **Make** (`Lcl_CourseInfoWindow_00.msbt`,
  `L_Edit_00-T_Btn_00`), looked up once at run time in the player's own `Message/LayoutMsg_<language>.szs` (the
  language as the game picks it: main+0x8a16b0 formats that path from the language index main+0x281120 and the code
  table main+0xd8b5b0; `rov_msbt_get`), so every language gets the game's own word and none is in the repository.
  The SMM2 messages have no "Edit" for a course; the pause menu's "Edit Course" (`Cmn_PauseMenu_00`,
  `L_MakeBtn_00`) is the same action from inside a course, and Make is the word of the button the card's Edit
  mirrors. The cards (`L_CourseData_00` .. `04` of Course World's lists, the profile's lists too) are card objects
  whose update (main+0x1a2cec0, each frame) sets their 12 text panes (+0x540 count, +0x548 text parts; part +0x48
  → +0x08 the ui2d text box; part 11 is `T_CourseID_00`). A ui2d text box keeps its string at +0xd8 (UTF-16) [obs: the
  card's code; the layout's `WWW-WWW-WWW` before a course is in it]; main+0x718fd0 (text box, string) sets it (the
  length, then the text box's SetString, vtable +0x110). After the card's own update the mod reads the code the card
  shows, looks for it in the Downloaded slots (as cSavedAlready's Edit does) and sets the button's text box
  (`L_DownloadPlayBtn_00/N_Cursor_00/N_Load_00/T_Btn_00` under the card's pane, looked up each frame) to Make, or back
  to its own text; the cards are reused as the list scrolls, so this follows the course each card shows. Checked
  2026-10-22: Hot Courses' card of a course in the Coursebot reads Make, the next card Download; a course downloaded
  from the card reads Make as soon as the list is back; Make opens the Course Maker.
- **Endless / 100 Mario: save for later** (built 2026-10-22, DECISIONS.md "Save for later"): SMM1 offers no edit
  there, and SMM2's result screen has no room for one. A course met in a run is kept on the player's **Played
  Courses** (Course World → the player's own page → Played Courses: SMM2's own list), which our server keeps for
  every course a run starts (`note_played`, newest first, the last 300; the Endless and 100 Mario runs alike, any
  ruleset). Its card there is an ordinary course card: **Download** keeps it in the Coursebot (playable offline,
  and there for good), and on a course in the Coursebot the same button reads **Make** and opens it in the Course
  Maker (the card's Edit above). No scene change from inside the run, so the 100 Mario flow is untouched. Checked
  2026-10-22: an unattended 100 Mario run (Easy) → Played Courses lists its courses newest first → Download into
  the Coursebot → the card reads Make → the run's course open in the Course Maker. The design's other route (a "Save
  to Coursebot" from the result screen via cLaterPlay, main+0x1766830, reason 0x0D, and back into the run) is not
  built: it needs a button SMM2's result screen doesn't have and a round trip out of the run scene.
- `RECOMP_MOD_EDIT=0` turns it off (the Make button, the card's Edit).

**Where this stands (history, latest last):**
- [obs] Download from Course World into the Coursebot; the downloaded course's window shows Make; a tap on it opens
  the Course Maker with that course; an edit (a row of ? blocks) saved with Save As into My Courses
  (`course_data_001.bcd`), the download untouched; the saved copy opens in My Courses and plays with the edit.
  Screens in `private/smm2/build/` of the test data folder (`e1_*`, `e2_*`, `s19_*`; private).
- Not yet: the d-pad cursor doesn't stop on Make (touch does): the window's cursor targets seem fixed before the
  mod switches Make on [?]. The saved copy still showed the "uploaded" mark and a greyed Upload in My Courses in the
  first test; the reset now also covers the Coursebot's "current course" copy (save manager +0x58), not yet checked
  in game, and the current course was 182 (the title course) in that run, so where the Course Maker takes the
  upload id from is still [?]. The upload with `based_on` hasn't been run in game (the server side is tested:
  `edited_copies_credit_the_original`). Typed course names arrive garbled: runtime/hle/swkbd.c answers in UTF-8,
  SMM2 reads UTF-16 (not this mod's file).
- 2026-10-21 re-test (main build, `RECOMP_EDIT_CTL`): Download → Make (tap) → Course Maker → an edit → Save As into an
  empty My Courses slot works, and the typed name now arrives intact ("Edited Copy"; the swkbd UTF-16 fix). The
  d-pad cursor still goes Play → Delete → the list tabs, skipping Make. The game's enable call (main+0x761190) only
  sets the button's enabled bytes (+0x3a / +0xbc in its two lists) and a bit in a pane controller's mask (+0x218 →
  +0x14b0), so the cursor's targets come from somewhere else (not found yet). 2026-10-21: the controller's
  +0x18 is a list of cursors (one per input index), each pointing at the button it's on (+0x10); disabling a focused
  button only detaches the cursor (main+0x74dad0), and enabling attaches nothing. So the d-pad's neighbours are kept
  elsewhere still (likely per window, built when the list's buttons are set up) [?]. On that run SMM2's current course was
  slot 2, not the download, so the Course Maker took the download from storage, not the reset copy. The copy and the
  editor's Save / Load card show the yellow arrow mark. Upload is greyed until a clear check, which is SMM2's normal
  rule.
- Fixed 2026-10-21: the Course Maker reloads the course into the current-course buffer (save manager +0x58) during
  the scene change, after the mod's reset of the prepared slot. Both buffers held creation id 1 and upload id
  0x100001 once it was up. A helper thread now resets that buffer's header whenever the original's comes back (20 s
  after the request). Checked in game: the Save / Load menu then offers Save As and Load only, as for a new course;
  after Save As the saved copy's header has flags 0 and upload id 0. The yellow arrow mark on its card stays, so it
  isn't an "uploaded" mark: own, uncleared courses have it too.
- Found 2026-10-21: Save As gives the copy a new creation id (e.g. e099c3d7 -> cf2b3d34), so `smm2edit.txt`, which is
  keyed by creation id, doesn't find it at upload. Fixed the same day: Save As asks for a name and Load doesn't, so
  while the copy is in the Course Maker, a new creation id in its buffer right after a name entry
  (`g_swkbd_entries`, runtime/hle/swkbd.c) carries the record to the new id. A change without a name entry (another
  course loaded) ends the watch. Checked in game: `655ff576 -> 7f8d0ecf`, both in `smm2edit.txt`. The upload itself
  needs a clear check first and hasn't been run in game.
- 2026-10-22, all three open items, checked in game (built-in server with `RECOMP_EMBED_SERVER=local`, a store
  holding the instant-goal courses 9TC-5PV-7CG and LRH-R12-R7G; screens private):
  - The d-pad reaches Make (above); d-pad + A on Make opened the Course Maker with 9TC-5PV-7CG.
  - The upload credit end to end: download 9TC-5PV-7CG → Make → a coin → Save As ("EditCopy") → Upload → clear
    check (passed) → the upload notices → `POST /api/courses?based_on=9TC-5PV-7CG&based_on_maker=…` → 201,
    X62-J6M-J8G; Course World lists it by Player and its card reads "EditCopy / Based on 9TC-5PV-7CG by <its
    maker>" (the server credits the maker of the course it holds). Fixed on the way: CanPostCourse answered 100 at
    +0x8e610, which the game reads as "can't upload" ("You can't upload any more courses right now. Please wait a
    while and try again later."); 0 is "allowed" (the wrapper stores the call's u32 only when its flag is clear,
    else 0). Maker names went as '?' and ran into the next field; now UTF-8, 10 characters at most.
  - The card's Edit: Download on 9TC-5PV-7CG (already in the Coursebot) opened the Course Maker with it, the
    Save / Load menu offering Save As / Load (a new course), Save As into My Courses carried the origin
    (`f9a44964 -> cfa34afd`).
- 2026-10-22, the leftovers, checked in game (screens private, `evidence/mods/edit-leftovers/`): the card's text
  (Make for a course in the Coursebot, above), save for later from Endless / 100 Mario (Played Courses, above), an
  uploaded course's picture (the Course World section above: `PUT /api/courses/<code>/thumbnail` after the upload).
- Left: an own upload's whole-course map (the game's upload buffer has none; the server could draw one from the
  course [?]).

Tests: `RECOMP_EDIT_CTL=<file>` (`edit_ctl.c`) reads commands appended to the file while the game runs (`tap x y
[frames]`, `drag x0 y0 x1 y1 [frames]`, `btn A+B [frames]`, `stick x y [frames]` (the left stick, -32767..32767),
`kbd <text>` (answers the open keyboard dialog), `shot <name>`, `wait <frames>`, `peek <addr> [len]`, `poke <addr>
<byte>`, `find <addr> <name> [depth]`); test input must be on (`LARECOMP_INPUT=999999:A:1`). In the Course Maker the
stick scrolls and drags of the goal's handle shorten the course (a one-screen course for
a quick clear check). `LARECOMP_KBD_AUTO=<name>` answers the
course-name keyboard of Save As. `RECOMP_EDIT_TRACE=1` logs the window's UI events.

### The bridge (100 Mario)

`hundred.h` is the interface between the run side (here) and the flow (SMM1's screens). With `ruleset=hundred`,
when Challenge Modes is chosen on Course World (its Endless sub-sequence's Activate, main+0x1a22730, before the menu
comes in) the run side calls `hundred_flow_open()`; the flow then shows SMM1's screens
and calls `hundred_run_start()`, `hundred_play_next()`, `hundred_play_skip()`, `hundred_run_finish()` and reads
`hundred_run_status()`; the run side reports `hundred_flow_course_started()` and `hundred_course_result()`.

How a server course starts in SMM2's play without SMM2's Endless screens: they stay underneath, hidden by the flow,
and the run side drives them. SMM2's Endless machinery is what makes a server course play in SMM2's own play (the
run manager, the course list and download jobs, the checks, the course-in card, the HUD with the lives and three
banked 1-Ups, the skip, the retry after a miss, the goal), and its requests already go to our server. The run side
picks the difficulty with L / R until the menu's difficulty (main+0x2c3f618) is the one wanted, then Down to the
buttons (after L / R the focus is on the tab bar), and presses the menu's own buttons (New Game or Continue, the
confirm window's Yes, the map's Play, skip, back) through the test-input hook while the player's input goes to the
flow. The course's outcome comes back through the game's own requests (course start,
miss, clear, skip). After a skip SMM2's Endless starts the next course at once; after a miss it retries by itself.

**The clear is SMM2's own:** goal, fanfare and the result screen (Boo / I like it! / Quit / Next) belong to the
player; the run side presses nothing there. `HUNDRED_CLEAR` comes at the goal (the flow shows nothing for it);
`HUNDRED_NEXT` when the player leaves the result screen with Next, `HUNDRED_GIVEUP_SAVE` with Quit (the run is kept,
SMM2 goes back to its menu). The course-end machine (registered at main+0x1bed050) tells them apart: its
cPostRatingCheckCan exec (main+0x1bef440) reads the screen's choice (+0x2a0: 5 Next, 6 / 7 the ratings, 8 Quit,
9 comment, 10 tags) and goes to cNextScene (0x4b, enter main+0x1bf6430) for Next and, in the Endless Challenge, to
cEndlessMode_GetStatus (0x4d, enter main+0x1bf6630) for Quit; the run side hooks the two enters. A miss, the skip
and SMM2's pause are SMM2's own too (the flow draws nothing for them); at 0 lives the flow calls
`hundred_run_finish()`, SMM2's game over shows, and its return to the Endless menu opens the flow's select again.

The win: the Nth clear's answer has `run_complete`; on its Next the flow plays SMM1's ending (`hundred_ending_show`)
and calls `hundred_run_finish()`, which finishes the run on the server (the completion is on its `hundred_<d>` board
already).

The flow is `games/smm2/mods/smm1/` (strong `hundred_flow_*`, `hundred_course_result`, the ending); it keeps weak
stand-ins of the run side's functions, which `bridge.c` / `endless.c` replace. `hundred_run_status()` returns a
snapshot under the run's lock (it is called on the render thread); `style` is the current course's game style.

`RECOMP_MOD_FLOW_TEST=<difficulty>` plays a whole run unattended in place of the flow (the flow's screens are left
out): the run starts on entering the Endless menu, Play follows every clear, the win plays the ending and finishes
the run. `RECOMP_MOD_AUTOPLAY=1` (or `=jump`) plays the courses (right + run [+ jump], skip after
`RECOMP_MOD_AUTOPLAY_SKIP` seconds, default 20). Both need `LARECOMP_INPUT` set (the test-input hook), which also
scripts the way to the Endless menu. `RECOMP_MOD_UNLOCK_ALL=1` opens every difficulty; `RECOMP_MOD_TRACE=1` logs
Course World's states and every request the mod doesn't answer.

**Challenge Modes** (owner, 2026-10-19). With 100 Mario offered, Course World's Endless Challenge button reads
"Challenge Modes" and opens the mode used last (`challenge_mode=` in smm2mods.ini): SMM1's 100 Mario Challenge or
SMM2's own Endless Challenge, which runs under SMM2's rules (5 lives: the start lives are switched in the game's run
records) and SMM2's screens. ZL / ZR or a tap on the header switch: on SMM2's menu its header reads
"ZL < Endless Challenge > ZR" and the run side opens the flow; on the flow's select the flow draws
"ZL ◀ 100 Mario Challenge ▶ ZR" and calls `hundred_mode_switch()`. L / R stay the difficulty, B stays back
(`hundred_flow_exit()`: the run side presses B on SMM2's menu underneath, back to Course World). **After a win**
the last result screen's Next is turned into its Quit (cPostRatingCheckCan exec, the screen's choice 5 -> 8) while the
flow plays the ending, so SMM2 goes back to its Endless menu for the next run; `hundred_run_start()` waits until it
is. **Course start** (owner, 2026-10-20: SMM2's own course-in card, the lives / loading card it shows after a miss,
also at a course's start): the map Mario's Play is pressed on SMM2's map underneath, and the menu's CourseIn state
(index 0xb, exec main+0x1a258f0: it posts 0x80, to the course, once the Decide animation is over) is held until
`hundred_flow_ready_to_start()`, which turns true when the flow's iris has closed on Mario; SMM2 then changes scene
under the flow's black, and the course scene's first state (cCourseListDownLoad enter main+0x1bbb820 or
cCourseDataDownLoad enter main+0x1bbb9a0, else the course start request) goes to the flow as
`hundred_flow_course_in()`, which lets the black go: SMM2's card comes up out of it. With the "SMM1 course card"
setting (`hundred_smm1_card=1`) the flow shows SMM1's card instead, and a course SMM2 loads underneath the map waits
at its first play frame until `hundred_flow_ready_to_start()` (the card is up). **Transitions:** after the result screen's
Next SMM2's own block wipe (`Cmn_Fade_00` OutBlock / InBlock from the game's romfs, drawn by the flow) covers it and
uncovers the map (or the ending); the ending goes to the select the same way; the
select fades in over Course World when it opens (SMM2's Activate is held until the select is in, so SMM2's Endless
menu comes in under it unseen; it used to open on that menu's first Disp, which showed SMM2's menu for half a second
first) and out over SMM2's Endless menu when Challenge Modes switches or B leaves (B starts SMM2's way back to Course
World at once, under the map's Out; a B pressed while that menu is still coming in counts once it is in,
`hundred_menu_in()`). After a Save & Quit (the pause's, or Quit on the result screen) SMM2's way back skips that
Activate and shows its menu as it fades in from black; the flow's black comes up on SMM2's black (the scene fade's
waiting state, enter main+0x192f580: `hundred_flow_cover()`) and the select comes out of it. A won run goes back under
the ending; a game over goes to Course World through SMM2's GAME OVER scene. **Challenge Modes input:** the buttons and the
touch held when the flow opens count as held, not pressed, until they are let go: SMM2's menu acts on a ZL / ZR in
the very frame it comes, before the flow has seen it held, so the select used to take the same press and switch
straight back to Endless (half the switches back to 100 Mario bounced, in a test with the player's own key path); on SMM2's menu a
press is caught on the run side's driver thread as well as on the menu's Disp and is good for a second (the Disp
doesn't run while the menu changes difficulty or redraws its map, as right after a switch, and a press then was
lost). `RECOMP_MOD_TEST_SWITCH=<letters>` makes switches as a player does (z ZL, Z ZR, t a tap on the header, . a
pause), `RECOMP_MOD_SHOT_BURST=<frames>` takes a window shot every few frames after Next and after SMM2 leaves its
map.

**The RomFS overlay** (`romfs_overlay.{h,c}`): the mods' generated game files (the Challenge Modes texts,
`relabel.c`; costumes; layout edits) come from the player's own dump at the first launch that needs them, into
`<data>/mods-generated/romfs/` (private), and replace the game's files (`game_romfs_overlay()`). A feature registers
`ROV_GENERATOR(name, version, fn)`; the folder is generated again when the set of names and versions changes.
Helpers: files (`rov_read`, `rov_write`), SARC with or without Yaz0 (`rov_sarc_*`), MSBT texts (`rov_msbt_set`),
BFLYT sections (`rov_flyt_*`).
**Loading:** a course list may take long (TGRCode upstream: about a minute for a difficulty's first batch); the
request waits up to 3 minutes and `hundred_run_status().loading` is set meanwhile.

**Pause.** SMM2's own pause opens in a run course (Start Over costs a life, posted as `start_over`; its Save & Quit
suspends the run and the menu's return sends `HUNDRED_GIVEUP_SAVE`). `RECOMP_MOD_SMM1_PAUSE=1` brings back the
earlier interception for the flow's own pause: SMM2's pause state (`cPausePlay`, state 3 of the Endless play scene's
machine; its exec is the scene's virtual at vtable+0x390, main+0x1bba050) is held without opening its menu and the
flow gets `hundred_flow_pause()`; `hundred_play_resume()` leaves it as the menu's close does (state 1),
`hundred_play_retry()` as Start Over (state 0x13), `hundred_run_save_quit()` as the confirm window's Yes (state
0x12, the suspend). **Next course data:** when a course starts, the one after it is downloaded in the background
(`/next?count=2`, its `/data`), so the next course-in doesn't wait on the server. **Continue:** the run record's state byte (+4 = 1) makes the menu
offer Continue (and show the run on its map); `hundred_run_start(d, true)` presses it. **Course on screen:**
`hundred_flow_course_started()` comes on the first frame of the play scene's play state (state 1, vtable+0x350,
main+0x1bb9440) after a course start or a retry. **Next course:** the head of the server's queue is read after New
Game, each clear / skip and on Continue, so `hundred_run_status().current` names the next course before the game
asks. `form` is player 0's form at the goal (+0x4a8, small / big from the size class table main+0x23d5e0c).

Flow test extras: `RECOMP_MOD_FLOW_TEST_CONTINUE=1` (Continue instead of New Game), `RECOMP_MOD_FLOW_TEST_PAUSE=<letters>`
(pause 8 s into a course and answer r resume, o Start Over, q Save & Quit; with `RECOMP_MOD_SMM1_PAUSE=1`),
`RECOMP_MOD_FLOW_TEST_QUIT_AFTER_CLEAR=<n>` (Quit, tapped, on the first n result screens),
`RECOMP_MOD_FLOW_TEST_RESULT_KEYS=<letters>` (keys, stick moves or a tap on the result screen first; see
`result_keys` in `bridge.c`), `RECOMP_MOD_SHOTS=<name>` (window shots at the run's moments into
`build/<name>_<n>_<what>.png`). On the result screen the D-pad and the stick don't move the focus off Next in the
port yet [obs]; touch reaches every button.

**A won run on demand** (with the real flow on top; used for the owner's v0.4.8 crash after a win). Use a copy of
the test server's folder (below), and edit its `endless.json` so the player has a 100 Mario run one clear short of
its goal. For example `hundred_runs.expert` = `{"active": true, "lives": 100, "clears": 15, "passed": 15, "goal": 16,
"ruleset": "hundred", "queue": [<short test courses>], "history": [15 entries {code, result "clear", seq 1..15,
style, theme}]}`. Without a history SMM2's menu shows no Continue and the run side's Continue becomes a New Game.
Lock or unlock tabs with `social.json` `endless`: `{"hundred_normal": {"<maker id>": 1}}` opens Expert and leaves
Super Expert locked. Then drive the flow's select with
`RECOMP_HUNDRED_SELECT_KEYS=<n>:<tick>:<BUTTON>,...` (presses on the n-th select screen of the session, at that
select tick). For example `1:90:R,1:130:R,1:220:A,2:60:R,2:240:A` continues Expert, wins it with
`RECOMP_MOD_AUTOPLAY=jump`, then on the select after the ending presses R onto the newly unlocked Super Expert and
starts it. Leave out `LARECOMP_INPUT`'s `5200:DOWN,5300:A` with this server: they land on SMM2's menu under the
select and press its New Game. The test course 5JM-KJ2-Q9G is reported corrupt by the game; drop it from a run.

### Test recipe (end to end, unattended)

The whole chain (difficulty select -> map -> card -> course -> clear -> ... -> ending) against a local server, the
window hidden. Paths are the ones on the dev machine; any folder works.

**1. The server.** The test server is `/Volumes/SSD/smm-work/recomp-mods/server-test` on port **8650**, 12 short
test courses (flat / items / power-up courses in each style; running right and jumping clears most), one account "Runner" whose token is in
`/Volumes/SSD/smm-work/recomp-mods/account-test.txt` (line `token <hex>`; keep it out of logs and commits). The
corpus server is `server-data` on port 8640 (550 real courses, account in `account.txt`). Start one (if
`curl http://127.0.0.1:8650/api/health` doesn't answer):

    cd /Volumes/SSD/smm-work/recomp-mods
    ./cargo-target/release/mmk server --dir server-test --addr 127.0.0.1:8650 \
        --writes-per-minute 100000 --accounts-per-hour 1000 > server-test.log 2>&1 &

A new one, seeded (any raw / zlib SMM2 course files; the 30 short ones in `testcourses/` were made for this):

    cd core && cargo build --release -p mmk-cli          # (target/release/mmk)
    mmk server --dir my-test --addr 127.0.0.1:8651 --writes-per-minute 100000 --accounts-per-hour 1000 &
    mmk register Runner --server http://127.0.0.1:8651 > my-account.txt   # maker ID + token
    mmk upload --server http://127.0.0.1:8651 --maker testlib /Volumes/SSD/smm-work/recomp-mods/testcourses/*.course

A run needs at least `total` courses in the difficulty's band (or the band widens): with only short test courses,
every difficulty draws from all of them.

**2. The game.** Build (`cd recomp && make GAME=smm2 -j10 all`), then from the data folder (`recomp/private/smm2`):

    cd recomp/private/smm2
    export LARECOMP_HIDDEN=1 SMM2_SEED=4 RECOMP_LOG_TIME=1
    export RECOMP_MOD_100MARIO=1 RECOMP_MOD_RULESET=hundred RECOMP_MOD_LOG=1 RECOMP_MOD_UNLOCK_ALL=1
    export RECOMP_MOD_SERVER=http://127.0.0.1:8650
    export RECOMP_MOD_TOKEN="$(sed -n 's/^token //p' /Volumes/SSD/smm-work/recomp-mods/account-test.txt)"
    export RECOMP_MOD_AUTOPLAY=jump RECOMP_MOD_AUTOPLAY_SKIP=25
    export LARECOMP_INPUT="2500:L:20,2500:R:20,2800:RIGHT:8,2900:A:8,3300:DOWN:8,3400:A:8,4600:DOWN:8,4700:RIGHT:8,4800:A:8"
    ../../build/smm2/smm2recomp extracted > run.log 2>&1

- `LARECOMP_INPUT` (`frame:BUTTON:frames`, presented frames from boot) goes from the title to the main menu, to
  Course World, then to Endless Challenge (frame 4800: DOWN, RIGHT, A).
  It is timed for `SMM2_SEED=4` and the save in `/Volumes/SSD/smm-work/recomp-mods/private/smm2/save` (Course
  World seen before, no first-visit intro): copy that `save/` over yours if the script lands elsewhere.
  `LARECOMP_HOST_INPUT=<frame>:shot:<name>,...` saves window shots into `private/smm2/build/`.
- Choosing Challenge Modes under `ruleset=hundred` calls `hundred_flow_open()`: the flow takes over from there
  (it calls `hundred_run_start()`, `hundred_play_next()` ...). `RECOMP_MOD_FLOW_TEST=<0..3>` instead plays the run
  without the flow (the stand-in in `bridge.c`).
- `RECOMP_MOD_AUTOPLAY=jump` plays each course (right + run + jump) and skips it with the test input after
  `RECOMP_MOD_AUTOPLAY_SKIP` seconds; under `RECOMP_MOD_FLOW_TEST` the test presses the result screen's Next
  after a clear (a real player's run side presses nothing there).
  `RECOMP_MOD_UNLOCK_ALL=1` opens Expert and Super Expert without winning the runs before.
- A Super Expert run (6 clears) takes about 6-12 minutes of game time; Easy (8) about 10. Expected log lines
  (`grep '\[mod\]' run.log`): `New Game: super_expert, hundred rules, 100 lives`, `course start <code>`,
  `clear <code> ... clears n`, `... -- 100 Mario Challenge complete!`, `bridge: run finished`.
- After the run: `curl -s http://127.0.0.1:8650/api/rankings/hundred_super_expert` lists the completion.

### Where this stands (2026-10-19)

- Done [obs on macOS]:
  - Challenge Modes: the relabel, ZL / ZR / the tap, the remembered mode, B back to Course World, and 5 lives on Endless right after a switch.
  - Separate run slots per ruleset on the server.
  - The 100 Mario run side: Next / Quit on SMM2's result screen, a won run back to SMM2's menu, no course twice across a player's runs.
  - Broken courses: a course the game finds corrupt counts as a skip. The run comes back after the window (Course World → a tap on Challenge Modes → Continue → the next course; `RECOMP_MOD_TEST_BREAK=<n>:<code>` reproduces it).
  - The result screen's comment and tag buttons are enabled (comment allowances 10/10, the course's comments-on flag course global +0x860).
- Half done:
  - Likes and boos: the hearts are enabled. A tap on "I like it!" didn't post a rating before the test was stopped, so whether PostRatingInfo (world.c `post_rating`) reaches the server from the result screen is unverified. Course World's own clear screen is untested too.
  - Posting a comment is untested.
  - Test-injected D-pad presses don't move the result screen's focus. Real keys and pads do.
- Next:
  - Verify a like / boo from the result screen (a tap, then Next: `rating <code>: 1 (like)` in the log, then the course's likes on the server).
  - Comment posting.
  - The 0-lives game-over round trip with the flow.
  - Local play over the vlan relay (ldn emulation).

### Limits

- Not online yet: Network Play (plan: `DECISIONS.md` "Online multiplayer"), Ninji speedruns, follows,
  notifications; they answer "not connected" or empty.
- The Maker avatar is the console's default Mii with your account's name (creating one needs the Mii editor applet,
  which the runtime doesn't have). `online.c` puts it in the save's user settings (+0x60) when there is none, so the
  avatar icon at the top left of Course World's menus shows it from the start (it was a white box until a maker page
  brought the record's Mii).
- No SMM2 Mii-costume rewards (the server sends no notifications).

## SMM1 extras: one setting per SMM1 feature

Each setting turns on one SMM1 behaviour in the otherwise exact SMM2 game. All are **off by default**: with every one
off, the game is SMM2's (an input replay against Ryujinx stays identical, below). They are switches in the overlay
menu's Mods tab (always shown), kept in `smm2mods.ini`; an environment variable wins over the ini.

| Setting (Mods tab) | ini key | Variable | When |
|---|---|---|---|
| Overlapping pipes (SMM1) | `smm1_overlap_pipes=1` | `RECOMP_SMM1_PIPES=1` | from the next course load / placement |
| Mid-air shell jumps (SMM1) | `smm1_shell_jumps=1` | `RECOMP_SMM1_SHELL_JUMPS=1` | immediately |
| SMM1 course card (100 Mario) | `hundred_smm1_card=1` | `RECOMP_HUNDRED_SMM1_CARD=1` | from the next course |

`RECOMP_SMM1_LOG=1` logs every decision the hooks see (`[smm1] ...`), on or off.

How: host code after single instructions of the game (`games/smm2/mods/smm1_extras.py` lists them with the
instruction word each expects; `smm1_extras.c` acts on them). With a setting off its hooks change nothing.

**Overlapping pipes.** SMM1 let a pipe overlap another pipe: Nintendo's own SMM1 sample courses have six such
overlaps (elbows, T joints, a pipe inside a pipe) [obs, the course files]; none of the 550 sample SMM2 courses has one.
SMM2 forbids it in one place, its placement check (the course analyzer the Course Maker and the course loader both
run): for each part with an overlap rule it collects the parts in the cells it covers and asks, per pair, whether the
other part is "solid" (main+0xfe8490: pipes count); a solid part whose shape overlaps is a conflict [obs].
- Course Maker: a pipe dragged onto another turns red and goes back where it was [obs].
- Course load: a course with overlapping pipes counts as broken and a blank course plays instead [obs].
- With the setting on, the hook after that call (main+0xfe827c) answers "not solid" for a pipe against a pipe, so
  both accept it; pipes against anything else are checked as before. In play, overlapping pipes are two pipes:
  each draws and collides as itself [obs: in the test course Mario walks into one, stands on the short one and
  jumps up onto the tall one it overlaps].
- A course saved with overlapping pipes needs the setting to load (SMM2, and the setting off, treat it as broken).

**Mid-air shell jumps.** SMM1's shell code is SMM2's (the kicks, the speeds, and the mid-air kick off a wall: the
"wall assist", below) except two rules [obs, Block.rpx v208 against SMM2]; on a shell the setting applies SMM1's:
- the kicker's no-hit time after a mid-air kick off a wall: SMM1 5 frames (the contact's own minimum), SMM2 16
  (main+0x1202c88: the 16 becomes 0, and the contact's epilogue, main+0x1203868, turns 0 into 5). The shell coming
  back off the wall can be landed on 11 frames sooner. This is the rule that makes the shell jump work [obs, below].
- the stomp window when the player is falling onto an ordinary-size target: SMM1 a flat 10 px; SMM2 3 px less plus
  the player's fall this frame, at most 3 (`fmax(dy, -3)`), so 7-10 px, never wider than SMM1's (main+0x1267ce8: the
  "falling?" compare before that step reads "no").

SMM1 applied the flat window to every small enemy; the setting limits both rules to shells.

The wall assist (SMM2 main+0x105a2d0, the shell's kick handler; SMM1 the same [obs]). A still shell kicked by a player
in the air and falling counts as a kick off a wall when: nothing solid is within 16 px under the player's feet just
ahead, nor within 16 px under the shell's centre one tile behind it (the shell is at the edge of its ledge), and a wall
faces the shell 10-16 px from its centre in the kick's direction (twice that for a big shell). Then the shell is
kicked into the wall and records a stop point one tile short of it, the player gets a small hop (3.51855, a stomp's
0.9), and no no-hit window is written. Off the wall the shell brakes (0.5 a frame) and parks at the stop point while
the player is in the air, ready to be landed on.

**The test course** (`tools/shell_jump_course.py`, our own course; the script writes it as the ten title courses of
a mod folder, encrypted with the key table from the player's own executable, and the input): SMB1 style, a plateau
with a low ceiling, a 2-tile pit, a 1-tile ledge against a wall 8 tiles high that leads to the goal, and a Buzzy Shell
on the ledge 14 px from the wall (centre 354 px; where the wall assist itself parks a shell, between tiles, so not a
Course Maker position). The only way up the wall is a shell jump. The input, one line per player update
(`RECOMP_PAD_SCRIPT`): 60 nothing, 199 RIGHT, 25 RIGHT + A, 120 RIGHT. Mario walks off the plateau, falls past the
ledge and his side touches the shell at update 255: the wall assist (the hook logs it), a hop; A from update 259.

| update | switch off (SMM2) | switch on (SMM1) |
|---|---|---|
| 255 | the mid-air kick off the wall; no-hit window 16 | the same; no-hit window 5 |
| 256-260 | the window counts 15 ... 11 | 4 ... 0 |
| 260 | the shell, back off the wall (vx -2.75), passes under Mario: no contact | the contact: Mario bounces off the shell (vy 3.9095, the stomp bounce; A held) |
| peak | y 65 (feet; the shell's top is at 64) | y 144 |
| after | back on the ledge at update 283, under the wall (top at 128) | on top of the wall at update 303, and on to the goal |

[obs] The two traces (the whole 0x2800-byte player object per update) are identical through update 259; each run
repeats itself exactly (trace files byte for byte). Evidence (private, from the dump):
`research/evidence/smm1-shell-jump/` (the side-by-side frames, both traces and logs, the input). How it was run:

    python -I tools/shell_jump_course.py --romfs recomp/private/smm2/extracted/romfs --elf <main.elf> --out <dir>
    cd recomp/private/smm2 && LARECOMP_NO_AUDIO=1 LARECOMP_HIDDEN=1 RECOMP_MOD_DIR=<dir> SMM2_SEED=4 \
        RECOMP_SMM1_SHELL_JUMPS=0|1 RECOMP_SMM1_LOG=1 RECOMP_PAD_SCRIPT=<dir>/shell_jump.pad \
        RECOMP_TRACE_OUT=trace.bin RECOMP_TRACE_EXIT=1 RECOMP_TRACE_SHOT=250,255,259,261,266,285,310 \
        ../../tools/run_for.sh 120 ../../build/smm2/smm2recomp extracted

`RECOMP_SMM1_LOG=1` logs each stomp test on a shell (both windows, the player and the shell), each mid-air kick off a
wall, and the kicker's no-hit window for the 20 updates after it. Notes:
- The course plays as the title course: the title screen's own course play (SMM2's player, enemies and contact code).
- In the SMB1 style SMM2's Mario does not pick a shell up (in the title course he kicks it; in SMW style he carries
  it [obs]), so the SMB1 shell jump is this kick off a wall, not a thrown shell.
- The stomp-window rule makes no difference in this run (the contacts there fall at 4 px a frame, or rise): not shown
  separately in play; the log shows both windows at every test.

**Other test courses.** Made at run time from the player's own data, never in the repository: our pipe courses are
built from SMCraft's flat test course, the SMM1 one copies the pipes, ground and blocks of SMM1's sample course 95 from
the player's SMM1 dump into an SMM2 SMB1-style course. Installed as the title course (`RECOMP_MOD_DIR`) they play
straight away; the Course Maker is reached with `LARECOMP_HOST_INPUT` (L + R, A, then taps;
`frame:drag:x0/y0/x1/y1:frames` moves a finger across the touch screen).

**Where this stands (2026-10-22).**
- Overlapping pipes: done. It covers the Course Maker drag, the course load and play, and the off-case replay.
- Mid-air shell jumps: done. The test course above: on, the shell jump works; off, SMM2's.
  - Left: an off-case replay against Ryujinx with shell contacts (the replay so far has pipes but no enemy contact).
- More SMM1 behaviours from research/specs/smm1-extras* are not started: pipe spawn rates per enemy, spawn caps,
  Clown Car weights, attack reactions. They are EnemyDB data changes.

**Off-case check.** The same input session on a course with side-by-side pipes, played in the recomp (settings off)
and in the Ryujinx trace build (`tools/replay_sync.py`'s comparison): 606 of 606 player updates identical, the whole
12 KB player object and the input block too.

## The game's own network stack (backends local-nex and nextendo): a local NEX server and Nextendo Network
## SMW goal wipe (`games/smm2/mods/smw_goal.c`)

In Super Mario World style courses, a goal-tape clear in real play ends the way SMW's levels do: once the fanfare is
over, a black circle closes on Mario with SMW's own goal-wipe sound. Then:

- where SMM2 shows its clear panel (Course World, Story, Endless / 100 Mario...), the black fades into the panel's own
  wipe and COURSE CLEAR! card; Next then leaves the course as SMM2 does;
- where it shows none (My Courses), the screen stays black while SMM2 changes scene under it, and the black fades away
  when the next screen is up.

The castle's axe, online versus and the Course Maker's test play are left as they were. On by default: the Mods tab's
"SMW goal wipe" switch, `smw_goal_wipe=0/1` in `smm2mods.ini`, or `RECOMP_SMW_GOAL_WIPE=0/1` (wins over the ini).

- **The sound** is SMW's original (`SuperMarioWorld_43_GoalWipe`, 0.8 s at 32 kHz), which SMM2 keeps in its story
  mode's sound bank `Sound/Resource/Quest_UndoDog.bars`. The mod reads it from the player's RomFS (`rov_read_game`) and
  decodes it (`smm1_bars_sound`, `smm1_bwav_decode`: SMM2's BARS of BWAVs, DSP-ADPCM, in `smm1/smm1_audio.c`); it
  plays through the mods' mixer (`smm1_play`). Nothing is shipped.
- **The circle** is drawn host-side (`smw_iris`, an antialiased circle over 1280 x 720) through
  `game_overlay_image`, which the 100 Mario flow owns: it hands its frames to `smw_goal_image` while it has nothing
  of its own to show, and calls `smw_goal_poll` every frame. The centre is the body of the player who broke the tape
  (actor x / y at +0x230 / +0x234, mapped through the camera's view rect); the screen centre when that can't be read.
  The circle takes the sound's 0.8 s, then 0.25 s of black.
- **The timing** comes from MariowGoal's own Fade-state thunks (enter main+0x12d2890, execute main+0x12d28a0: every
  goal type has its own, so the axe never comes here). The enter override arms the wipe once per goal (the demo
  controller sits at the same address course after course, and the Fade can start after the fanfare's counter has run
  out: the Fade comes on a 480-frame timer, the fanfare plays in real time, so slow frames leave no counter to see).
  SMM2's Fade waits while a fade is under way or while the demo's counter (fanfare, then 35 frames) runs; then it
  opens the clear panel, or asks for the scene change. The execute override starts the armed circle on the first
  frame the Fade would go on and skips the Fade until the circle is shut (a skipped frame changes nothing: the Fade
  only waits). Then the Fade runs: a scene change asked for (the scene manager's byte at +0x344)
  keeps the black until the new scene exists; otherwise the black fades 54 frames later, when the panel's staircase
  wipe covers the course.
- Research (private): `research/specs/smw-goal-wipe.md`.
- Test: `RECOMP_SMW_GOAL_SHOTS=<every>` saves a window shot every `<every>` frames for 4 s from the wipe's start
  (`build/smwgoal_<frame>.png`). The 100 Mario test recipe above with a server holding only `MW_*` test courses
  clears SMW courses unattended.

## Muncher hit box (SMM1) (`games/smm2/mods/muncher_hitbox.c`)

Munchers hurt the player the way SMM1's did. In SMM2 a one-tile gap between two Muncher columns can only be fallen or
jumped through dead centre; with the mod Mario has SMM1's room of about 2 px either side. Off by default: the Mods
tab's "Muncher hit box (SMM1)" switch, `muncher_smm1_hitbox=0/1` in `smm2mods.ini`, or `RECOMP_MUNCHER_SMM1_HITBOX=0/1`
(wins over the ini). It applies from the next frame.

- **SMM2** gives a Muncher one body, a 16 x 16 box that is also a spike: any side the player touches hurts. The
  player's wall sensors reach 7.9 px either side of him, so in a 16 px gap more than 0.1 px off centre is a touch
  [obs: in a test course Mario 2.2 px off centre is pushed to x = gap centre - 0.1 and dies].
- **SMM1** (Block.rpx v208) builds the Muncher from two bodies: the same 16 x 16 box, harmless, for everything else,
  and a spike box 2 px smaller on every side (12 x 12) for the player. Enemies' sensors skip the spike box. The player
  meets the spike 2 px later from any side, so for him the gap is 20 px wide and he can be 2.1 px off centre.
- **The mod** gives the player SMM1's box. While a player's per-frame update runs (main+0x158a590, called through the
  player's vtable), every live Muncher's body is shrunk to the inner box: its edges and its world bounds (the player's
  queries go by both) are scaled by 12/16 about the box's centre, so a Big Muncher's loses 4 px a side. They are put
  back byte for byte when the update returns. Enemies, items and everything else still meet the 16 x 16 box. Munchers
  are found when they are built (main+0x107d340, the build function both Muncher profiles point at) and dropped when
  they are destroyed (main+0x107ed30 / 0x107ed80).
- **Tested** on macOS with a title-screen test course (two Muncher columns, a one-tile gap, small Mario riding donut
  blocks straight down at offsets -2.2, -0.7, +0.8, +2.3, +3.8 px from the gap's centre): off, he dies at every offset;
  on, he reaches the floor at -0.7 and +0.8 and dies at the others, pushed out 2 px later than without the mod. Big
  Munchers and enemies on Munchers weren't run.
- Research (private): `research/specs/objects/57-muncher.md` (section "Hit box").

## Quick level return (`games/smm2/mods/quick_retry.c`)

After a miss the course starts again (at its start or the checkpoint) much sooner. Off by default: the Mods tab's
"Quick level return" switch, `quick_retry=0/1` in `smm2mods.ini`, or `RECOMP_QUICK_RETRY=0/1` (wins over the ini).
It applies immediately.

- **How:** the game plays its own miss (the lost life, the lives card, the course set up again). The frames from
  the death fanfare until the restarted course run as fast as the machine can and aren't shown
  (`runtime/nvn/objects.c` `g_present_hidden`): no display pacing and no window image, but each frame's GPU work,
  the renderers' per-frame work and the 100 Mario flow's tick still run. Nothing in the game's logic is changed.
  Lives count as before (100 Mario and Endless: a miss still costs one), and so do clears, checkpoints and skips.
  Off, the hooks only return.
- **Start:** the death fanfare (music state 7: `costume.c` `down_fanfare`, main+0x17a4bc0).
- **End:** in Endless and 100 Mario, the Endless scene's play state running again (`bridge.c` `play_exec`,
  main+0x1bb9440), up to 900 frames. Elsewhere, the first player update after frames without one (`trace.c`), each
  update extending the fast-forward by 60 frames, up to 360. Where no player comes (test play's miss goes back to
  the Course Maker) it lapses by itself.
- **Measured** (M-series Mac, muted test runs), miss to playing again:

  | | SMM2 | Quick level return |
  |---|---|---|
  | Coursebot course | ~4.6 s | 1.6 s |
  | 100 Mario (lives card included) | 9.7 s | 2.3 s |

  Test play's miss went back to the Course Maker about 0.5 s after the miss. About 0.7 s of each restart is the
  course's reload, which the fast-forward can't shorten. With the switch off, the same 100 Mario run missed at the
  same point, took SMM2's 9.7 s, and never ran above 60 fps.
- **Test:** a one-course server holding `testcourses/3W_water.course`, the 100 Mario recipe above with
  `courses=server` (a copy of `smm2mods.ini` through `RECOMP_MOD_INI`), `RECOMP_HUNDRED_INPUT="5950:A:8,6150:A:8,6350:A:8"`
  for the flow's map, and right held from frame 6700: Mario misses at frame ~9780. `LARECOMP_FPS_INTERVAL=0.25`
  shows the fast-forward's rate. Don't save frames during it: the PNG writes slow every frame down.

## Co-op in Endless and 100 Mario (`games/smm2/mods/coop.c`)

A Mods-tab switch, off by default (`coop=0/1` in `smm2mods.ini`, `RECOMP_COOP=0/1`). With it on and two or more
players' controllers connected (Settings > Controls: every further controller is the next player; "Controllers
start at player 2" makes the keyboard / on-screen controls player 1), each Endless Challenge and 100 Mario course
is played the way Play Together plays: Mario, Luigi, Toad and Toadette, each on their own controller.

- **Rules** (owner, 2026-10-21): SMM2's local multiplayer ones. A player who is down comes back in a bubble while
  another stands; a life is lost only when every player is down; anyone reaching the goal clears the course. The
  lives card shows every player.
- **3D World courses** are played alone (SMM2 has no multiplayer in that style).
- **How**: the course's play mode (`[*0x2a6bcd0 + 0xe0]`) is made SMM2's local multiplayer one where the player spawn
  stores it, with the set-up's player count; the controllers' poll reads as many controllers as there are players;
  Endless's miss request, sent for any player's miss, costs a life only when no player stands. Hook points in
  `patches.py`, handlers in `hooks.c`; DECISIONS 2026-10-22.
- **Tests**: `RECOMP_COOP_TEST=<n>` plays as if n players were connected; `LARECOMP_INPUT2` scripts player 2.
  The log says `co-op: <n> players in this course`, and at each miss request every player's state.

## Mystery Mushroom costumes (`games/smm2/mods/costume.c`)

A Mods-tab switch, "Mystery Mushroom costumes", off by default (`costumes=0/1` in `smm2mods.ini`,
`RECOMP_MOD_COSTUMES`). It takes effect at the next start: the costume models are made then from the SMM1 dump
(research/specs/recomp-mods.md §3, smm1-extras-costume.md for SMM1's rules).

- **The form:** a player in costume is small Mario (form 0) for the game: size, physics, hit boxes, block and power-up
  rules (SMM1's costume Mario is small too).
- **Models:** for each SMM1 costume, SMM2's small SMB1 Mario model with the costume's 24 x 24 frames in its 25 textures
  and SMM1's 24 x 24 quad (`smm1/costume_gen.c`, overlay generator `costumes` v3: `Pack/M1_Model.pack` gets
  `Model/M1_Player_Chara_<res>{,_2,_3,_4}`; `Mods/CostumeAppeal.bin` holds each costume's appeal frame). The builder
  (`player_model.c`) loads one into the SMB1 row's spare slot 12 (Balloon: size class 1, unused in SMB1), drawn once
  per course from the owned costumes; slot 12 is shown where the game asks for slot 0 while a costume is worn.
- **Another costume mid-course** (SMM1 draws one at each pickup): all the models are the same template, so a costume is
  only texels. The builder's slot-12 textures are noted as the game makes them (runtime hook `game_texture_init`,
  `runtime/nvn/objects.c`: 25 textures of 24 x 24, their texels in place in the loaded file, 4096 bytes apart), and
  another costume is put on by writing its texels over them (`costume_look`: the generated model's own bytes); the
  renderers re-upload a texture whose texels change, as for any the game rewrites. Before a write the texels are
  compared with what was written last (a texture since dropped, `game_texture_final`, or reused memory is never
  written). The game keeps a loaded model for later courses, so the builder puts a reused one back to its own costume.
- **Pickup:** SMM1's Mystery Mushroom is the SMB1 skin of the style power-up, the Big Mushroom (`costume_item=super`:
  the Super Mushroom, as SMCraft did). Taken as small, Super or Fire Mario (SMM1 takes it from any form; SMM2's SMB1
  has no other form that would), the form change it asks for (hook point main+0x15a94a0) becomes one to small: from
  small the costume is on at once, from Super / Fire the game's shrink plays (about 30 updates of big Mario's frames,
  seen) and the costume follows. The
  costume is drawn at each pickup as SMM1 does: uniform over the owned costumes minus the one worn. Its power-up sound
  plays (the costume's Static bank). Another power-up takes the costume off.
- **The item's look:** the Mystery Mushroom item's model in the generated pack (`Model/M1_Item_special` and
  `M1_Item_special2`, the Big Mushroom's; `M1_Item_kinoko` with `costume_item=super`) has its texture redrawn with
  SMM1's "?" mushroom (`Add_kinoko` of `Model/M1_Item_CharaKinoko`, `Pack/StaticSkin.pack` of the SMM1 dump; the
  update's `M1_Item_CharaKinoko2`), bottom middle at one texel a pixel, so it is SMM1's size; quad, material and
  animation stay SMM2's (`mystery_item`, generator `costumes` v4; `costume item:super` in the overlay's identity
  regenerates it when the item changes). The item is still the Big Mushroom's 2 x 2 for the game (its hit box), and
  the Course Maker's palette icon is still the Big Mushroom's.
- **Hit:** the damage function's form load (hook point main+0x15a50a4) reads 1 for a player in costume, so a hit
  takes the costume off with SMM2's Super -> small power-down: its freeze (about 30 updates) and invulnerability blink
  are SMM2's. What the player looks like is SMM1's costume -> small (Block.rpx v208: a change within one size class,
  transform type 3; its Active state runs 30 updates, enter 0x0268151c, and shows the new form while (6 - timer / 6)
  is even, the old one while odd, both at scale 1, update 0x02681530): small, costume, small, costume, small, 6 updates
  each, from the hit's own update, then small Mario blinking. SMM2's power-down never calls the slot setter, so the
  holders' slot is set each update (`show`, and at the hit); a request for the big slot in the 180 updates after,
  while the player is still form 0 with nothing pending, shows small Mario. Seen with the hit test course: per-update
  frames 6 / 6 / 6 / 6 / 6, then the blink as small Mario (before: the costume stood still through the freeze, then
  blinked as the costume for 60 more updates).
- **Facing:** the holder's placement each frame (0x7101561bb0, holder vtable +0xb8: holder, matrix, scale, facing)
  takes the facing as 0 for slot 12 (Balloon Mario never turns: `cmp w8, #0xc; csel w26, wzr, w3, eq`) unless the
  holder's facing override (+0x4b0, -1 none) is set. For an SMB1 holder showing slot 12 the mod sets that override to
  the facing passed for the call (hle_override, `holder_place`), so the costume turns as small Mario does (and so does
  its shadow); seen walking left and standing after it (Link).
- **Taunt:** Up held 8 updates standing still, in costume (Up: the input block at player +0x550, +0x10): the costume's
  appeal frame on every texture of its model for 120 updates (moving, jumping, a hit or a new costume end it) and its
  `appeal` sound. Not after the goal (the goal fanfare's request ends taunts until the next course).
- **Sounds:** jump / jump2 (half and half), landing, power-up, taunt, goal and death from SMM1's
  `Sound/CharaMario/CharaMario_<res>{,Static}.bars` (host mixer). SMM2's goal and death fanfares and its jump sound
  give way to the costume's, as in SMM1, where an event the costume has no sound for plays the normal one: a costume
  without `goal`, `down` or `jump` keeps SMM2's (Goomba, Block, the Pokémon). The jump: the player's jump sound
  (main+0x15c78b0, called from the jump states) returns at once when its request's silent byte (+0x24a4) is set; hook
  point main+0x15c78c4 reads it as set for a player in costume where SMM2 would play "jump" or "voice_jump_high"
  (request kinds 1 and 4; 2 is `jump_high`, not SMB1's), and the costume's jump plays there; one player state's enter
  (main+0x15cfe70) plays "jump" itself, and hook point main+0x15cfe90 hands it a sound user without an instance (the
  play call returns an empty handle). Nothing changes without a costume worn. A dump without `Sound/CharaMario` (the
  subset some data folders link) has no costume sounds; the log says `<event> sound (<res>): none`. Level: SMM1's
  sound effects play at their `.bameta` volume times a factor per group (`smm1/smm1_audio.c`): the costumes' 0.3, at
  SMM2's course sounds' level as they are; the 100 Mario screens' 0.1, at SMM2's menu sounds' level; and none louder
  than SMM2's loudest course sounds (-25 dB, loudest 50 ms RMS), which takes the loud quarter of the costumes' sounds
  down (by up to 9 dB). `LARECOMP_SMM1_COSTUME_GAIN` / `LARECOMP_SMM1_SE_GAIN` / `LARECOMP_SMM1_GAIN` (music) set
  the factors, for measuring with `LARECOMP_AUDIO_WAV` (with `LARECOMP_NO_AUDIO=1` the WAV is written and nothing
  plays).
- **Sub-areas:** the player object and its model stay through a pipe to the sub-area and back, so the costume stays
  on (checked: main → sub → main → sub on the test course's `--variant subarea`).
- **Reward (off by default since 2026-10-24, owner: any costume can be chosen in Try it now; `costume_reward=1` in
  smm2mods.ini or `RECOMP_COSTUME_REWARD=1` turns it back on):** after a won 100 Mario run, SMM1's costume window (`Cmn_CMListWindow_01`, `smm1/ending.c`) shows one
  costume not owned yet, from SMM1's pools by difficulty; owned costumes are kept in `costumes-owned.txt`. The
  window's frame is a four-frame window whose bottom frame is flipped; the layout player now anchors each frame's
  texture at its corner as nw::lyt does (`lyt_player.c` `frame4_uv`), which drew the lower-right frame cut off
  before. `RECOMP_HUNDRED_TEST_REWARD=<res>` with `test_smm1ui --flow <out> ending:0:2:0 2440 2440` renders it.
- **Try it now:** "Mystery Mushroom costume now" with a "Costume" row (every SMM1 costume by its English name, owned
  or not; it isn't added to the owned ones): player 1 takes a Mystery Mushroom with that costume, through the game's
  own form change request. Greyed out without the switch, outside an SMB1 course being played, in Endless / 100 Mario
  runs, and with a power-up the Mystery Mushroom doesn't take.
- **Co-op:** each player wears their own costume (each character's model is its own file, so its own textures).
- **Tests:** `RECOMP_COSTUME=<res>` wears that costume from each course's start (and every pickup gives it);
  `RECOMP_COSTUME_TRY=<frame>:<res>` presses "Mystery Mushroom costume now" at that presented frame;
  `RECOMP_COSTUME_DEBUG=1` logs forms, model textures, looks and every sound. `tools/costume_course.py` makes a test
  course as the title courses (`RECOMP_MOD_DIR`, `SMM2_SEED=1`; link the mod folder's `romfs/Pack/M1_Model.pack` and
  `romfs/Mods/CostumeAppeal.bin` to the ones in `mods-generated/romfs/`): pens with a Super Mushroom, two Mystery
  Mushrooms and a Koopa, and its pad script (`--variant hit`: a Koopa walking to a player in costume who taunts first;
  `--variant subarea`: a warp pipe in each area, walked into, out of and into again). With `RECOMP_MOD_DIR` the
  romfs index is made at start, before the overlay is generated (lazily, at the first costume model), so after a
  change to the generated pack the links show the previous start's: start once more.
  Checked (2026-10-22, later): SMM2's jump sound muted under Peach's (`LARECOMP_AUDIO_LOG=1`: SMB1's jump wave, 0x341a
  bytes, 5 times in 5 jumps with costumes off, 0 in Peach, 5 in Goomba), the costume through the sub-area pipe and
  back, the "?" item with the Big and the Super Mushroom as the Mystery Mushroom.
  Checked (2026-10-22): Super Mario takes a Mystery Mushroom (Peach), taunts (her parasol pose), takes the second
  (Pikachu, then Yoshi: a new draw each), "costume now" gives Mario (Gold) mid-course, a Koopa's hit takes it off
  (the costume flickers, then small Mario).

## Lanky Mario (SMM1's Weird Mushroom) (`games/smm2/mods/lanky.c`)

A Mods-tab switch, "Lanky Mario (Weird Mushroom)", off by default (`lanky=0/1` in `smm2mods.ini`, `RECOMP_MOD_LANKY`).
It takes effect at the next start: the Slim Mario models are made then from the SMM1 dump.

- **The form:** a lanky player is Super Mario (form 1) for the game: body, speeds, gravity, power-ups (a Super
  Mushroom is refused, a Fire Flower gives Fire) and damage (a hit gives small Mario, as for Lanky in SMM1).
- **Look:** Slim Mario in the spare SMB1 form slot 8 (Builder: size class 2, Super's animations, unused in SMB1),
  shown where the game asks for slot 1 while the player is lanky (`player_model.c`: the holder's builder and visual
  slot setter, shared with the costumes). The models are SMM2's Super SMB1 Mario with SMM1's Slim frames in its 21
  textures (`smm1/costume_gen.c`, overlay generator `lanky`: `Pack/M1_Model.pack` gets
  `Model/M1_Player_SlimMarioMdl{,_2,_3,_4}`, read and written through the overlay so the costumes' models stay).
  Luigi's is recoloured with his SMB1 Super palette (each Super Mario colour mapped to Super Luigi's at the same
  pixel); Toad and Toadette, drawn differently, wear Mario's. `test_smm1ui --lanky <sheet.png>` makes them alone and
  draws every texture.
- **Jump:** +0.5 px/frame on computed ground jumps (hook point main+0x15c8d54, the Jump state's begin after the launch:
  the request's vy at +0x248c is NaN). Stomp bounces (preset 3.9095), springs, crouch, spin and shoe jumps are as
  Super Mario's.
- **Sounds:** SMM1's `power_up_Funny`, `jump_3_Funny` and `h_fuyu` (the "fuwa" cue 19 updates into a held jump, never
  on a tap), which SMM2 still has (SLink user `M1_Player`), played by name with the game's own call (0x7100666af0).
- **Weird Mushrooms:** SMM1's easter egg. Each Super Mushroom out of a block or a container in an SMB1 course counts
  (the Super Mushroom's create, 0x71013547c0, overridden: its spawn kind is 1 when placed in the course, 4 out of a
  ? block): while fewer than 100 were counted and none was weird, each is weird
  with a chance of 1 in 20; the 100th is weird if none was. Small or Super Mario taking one becomes lanky (hook point
  main+0x13502c4, the item pickup's form load). It looks like a Super Mushroom (SMM1's `M1_Item_kinokofunny`: open).
- **Try it now:** "Lanky Mario now" (the overlay menu, `triggers.c`): player 1 becomes lanky, from small through the
  game's own power-up request (the grow demo plays), from Super at once. Greyed out without the switch, outside an
  SMB1 course being played, with another power-up (as the Weird Mushroom), and in Endless / 100 Mario runs (they keep
  records).
- **Co-op:** any player can be lanky.
- **Co-op detail:** each player has two model holders (+0x2f90, +0x2f98); both show Slim Mario.
- **Tests:** `RECOMP_LANKY=1` makes player 1 lanky 90 updates into each SMB1 course, `RECOMP_LANKY_WEIRD=1` makes
  every eligible Super Mushroom weird, `RECOMP_LANKY_TRY=<frame>` presses "Lanky Mario now" at that frame,
  `RECOMP_LANKY_DEBUG=1` logs the jumps (vy and peak height), mushrooms, forms and sounds. An SMB1 title course
  with ? blocks (`RECOMP_MOD_DIR` with `romfs/Course/title_course_data_0N.szs`, `SMM2_SEED=1`) is playable on the
  title screen; with `RECOMP_MOD_DIR` the generated overlay isn't served, so link `romfs/Pack/M1_Model.pack` to the
  one in `mods-generated/romfs/`. Measured (2026-10-22): a standing lanky jump leaves at vy 4.128 (3.628 + 0.5);
  held 45 frames it peaks at 96.8 px (Super Mario: 64.0 held 40 frames), a 2-frame tap at 25.7 px (19.9); h_fuyu
  plays 18 frames into a held jump and not on a tap; a Koopa's hit makes a lanky Mario small.

## Try it now: mod effects on demand (`games/smm2/mods/triggers.c`)

The overlay menu's Mods tab has a "Try it now" section under the switches: one button per mod effect that can be
asked for at once (runtime `game_mod_triggers`, drawn by `runtime/ui/menu.mm` `tab_mods`). A mod adds its rows with
`mod_trigger_add` from a constructor (label, description, `available()`, `trigger(choice)`, optional choices). The
rule each row follows (DECISIONS 2026-10-22): a try-it-out helper, greyed out outside a course and in Endless /
100 Mario runs; `trigger()` only records the request and the mod applies it on the game's thread. Rows: "Mystery
Mushroom costume now" with its "Costume" choice row (`costume.c`), "Lanky Mario now" (`lanky.c`). A row's choices
may be filled in after start-up through the row `mod_trigger_add` returns (the costume names are read from the SMM1
dump at the first course); the menu reads them at each draw.

## The game's own network stack (backend local-nex): groundwork for Nextendo and a local NEX server

The mod above *fakes* being online: nifm / acc / friends / mii answer locally, the enl session logs in without NEX,
and the game's request functions are answered from our HTTP server. The runtime also has a **real network stack**
for the game's own NEX client, so that SMM2's NEX code talks to a NEX server: a local test server (`local-nex`), or
the community servers of Nextendo Network (`nextendo`, with a signed-in Nextendo account). Nothing ever reaches
Nintendo's servers: only the redirect table's names resolve, every other name is "no such name".

### Backends

| `backend=` (smm2mods.ini) / `RECOMP_NET_BACKEND` | What runs |
|---|---|
| `ours` (default) | the mod path above; the game's sockets stay off (only the virtual LAN), every name lookup fails |
| `local-nex` | the mod's online overrides switch **off** (`mod_hundred()` is false), so the game's own enl / NEX code runs on real host sockets against a local NEX test server; with `course_world=mod`: hybrid (below), the offline test of the nextendo setup |
| `ocw` | the mod's online overrides switch **off**, and the game's own online code talks to **OpenCourse World** with the player's own account ([below](#opencourse-world-backendocw)). The only backend that reaches a public host, and it reaches that one host only |
| `nextendo` | with a signed-in Nextendo account (`runtime/net/nextendo.h`): **hybrid** (below): the game's own NEX client logs in to Nextendo's SMM2 server, Network Play is theirs, and the mod keeps answering Course World, the Endless Challenge and 100 Mario (courses from TGRCode unless `courses=` says otherwise; `hundred=` doesn't matter). `course_world=backend`: all of Course World from Nextendo, the mod's online overrides off. Not signed in: logged ("not signed in; using our server") and the backend is `ours` |

In the Mods tab's Online section: the "Online backend" choice (the next start) and the Nextendo account (Sign in / Sign out). For the app-level settings page,
`net_backend_options()` / `net_backend_select()` (`runtime/net/netcfg.h`) list them all; Nextendo can be chosen
while an account is signed in. OpenCourse World's own domain and token are the rows just under that section
(`games/smm2/mods/ocw.c`, through `runtime/game_rows.h`).

### Hybrid: Network Play from the backend, Course World from the mod (`games/smm2/mods/hybrid.h`)

The owner's choice for Nextendo (DECISIONS.md 2026-10-20 "Nextendo for multiplayer only"). With `backend=nextendo`
(or `local-nex` with `course_world=mod`, for offline tests) the mod answers Course World but not the login:

| What | Answered by |
|---|---|
| nifm / acc / mii, the enl session's login, Course World's login sequence (GetUserNameNgType, SyncUserProfile (a blank name filled, below), GetMiiClothes, LoginCheck, UpdateLastLoginInfo, PostActivityQuest, GetNewNotification, GetEventCourseStatus) | the game's own NEX client, against the backend (`mod_answers_login()` is false) |
| GetApplicationConfig | the backend's (Utility.GetIntegerSettings(0): Network Play's switches), then the mod switches the Endless Challenge's four keys (+0x737b0, 0..3) on: `GetApplicationConfig: the backend's, Endless on` |
| GetUsers (own) | the backend's record of the player (name, Mii, maker code, registration, player id), then the mod writes our server's Endless bests into it (entry +0x114) and sets the 100 Mario unlocks; its player id (entry +0x58) becomes the player's own id in the mod's answers (`mod_set_backend_pid`: "my course", "me", the player's courses on our server; never logged). No Maker avatar name is written; a blank record name becomes the Nextendo user name (below): `GetUsers (own): the backend's record, our Endless bests` |
| Course World's lists, makers, downloads, uploads, comments, likes; the Endless Challenge; 100 Mario | the mod (`mod_hundred()` is true whatever `hundred=` says): TGRCode by default (`courses=` unset: `hybrid: courses from TGRCode (courses= not set)`; the built-in server's local account is named after the Nextendo user), or `courses=server` |
| Network Play (matchmaking, rooms, PIA) | the game's own NEX client (Task 7 of the Nextendo plan routes its course search and download) |

[obs, 2026-10-20] Offline (`local-nex`, `course_world=mod`, the test server's Endless keys 0..3 off): the login
(ValidateAndRequestTicketWithParam), GetIntegerSettings, GetUsers, GetUsernameNgType, SyncUserProfile, GetMiiClothes,
GetNewNotification, GetEventCourseStatus and, on opening Challenge Modes, UpdateLastLoginTime (59) reach the test
server; no other DataStore request does. With RECOMP_MOD_TRACE=1 no wrapper outside the mod's answers ran ("not
answered by the mod": none, as with backend `ours`), so no other Course World request is answered by the backend.
New Game, course start and a clear came from our server; Network Play's Co-op reached
MatchmakeExtension.AutoMatchmakeWithParam_Postpone with no `enl command` line. Live on Nextendo: the NEX login,
TGRCode's Popular list and a course played, 100 Mario (New Game, a course with 100 lives); the game's TCP went only
to 51.178.29.194. The NAT check's names come to sfdnsres as `nncs1-%` / `nncs2-%`: `net_resolve` reads `%` as nsd's
environment (`lp1`), so `nncs2-%` reaches the NAT check (164.132.111.120, [obs] live) and `nncs1-%` the game server.
**The Co-op search freeze (fixed).** [obs] The Co-op search screen stopped presenting frames for good about 10 s
after AutoMatchmake, offline (also with the mod off) and live alike. It was the runtime's kernel, not the network
code: matchmaking's two worker threads poll a condition variable with 5 ms timed waits, and a timed-out
svcWaitProcessWideKeyAtomic took the mutex back. Horizon returns 0xEA01 without it, and the game's SDK (nn::os
TimedWait, sdk+0x32f240) then locks it itself, so each worker waited on itself (ArbitrateLock with itself as owner) at
its first timeout; when matchmaking failed, the main thread blocked behind them in the error teardown. Now a
timed-out wait returns without the mutex (`runtime/kernel.c`; tests: `tools/test_kernel_condvar.sh`), and a lock on a
mutex the thread already owns is logged (`[kernel] guest locks a mutex it already owns`). [obs] offline the search
now ends in the game's own error path (MatchMaking.2 is not implemented by the test server: enl `cSilentError`, a
NEX disconnect) and the game is back on the Network Play menu at 60 fps. [obs] Live (Nextendo) the kernel deadlock is
gone too, but Co-op still stops: the NAT check gets no UDP reply, the game tries again twice (enl `cSilentError`,
`cLogIn`, `cAutoMatchReady`, about 17 s each), then goes to netplay `cError`, and from there it draws nothing new
(the "Finding players" screen stays, the window stays responsive and NEX keeps pinging; the main thread waits in
WaitForAddress, called from main+0x75f5b0). Not looked into yet.

**When the backend's login fails** (2026-10-23): an error the game shows before the backend's login has ever succeeded
switches the session to `ours` (`net_backend_fall_back`, `[net] backend nextendo: its login failed; this session uses
our server`): the mod answers the next login and all of Course World, Network Play is off until the next start, and a
notice says so. At start, an account Nextendo refuses online (online-status) runs the session as `ours` straight away
(`RECOMP_NET_NO_PROBE=1` skips that check). [obs] offline (local-nex, `course_world=mod`, no server): error 2306-0502
on the first try, then Courses opened with TGRCode's list.

**The player's name.** SyncUserProfile (main+0x14cef40) sends the name in the NetObj string at +0x738e0 (buffer
+0x738e8, capacity +0x738f0; 10 characters), the save's Maker name; a save whose avatar was made without the
console's Mii editor has a blank one (empty or spaces), and the backend would then show the player without a name.
In hybrid a blank name is replaced, for that request only (the save isn't written by the mod), by the backend's
name for the player: nextendo's user name, or (local-nex) the name in the backend's record of the player
(`SyncUserProfile: the save's Maker name is blank; sending the backend's name`). A name the player set is sent as
it is. GetUsers (own) likewise fills a blank name in the record (entry +0x30) with the Nextendo user name, so the
card shows it before the backend has the synced one. [obs] offline the profile shows the test server's `--name`,
live the Nextendo user name.

The net log's TLS plaintext dump masks the player id in NEX station URLs (`PID=<hidden>`, `net_log_redact`).

### Settings (all in smm2mods.ini, or the environment)

| ini key | Variable | Meaning |
|---|---|---|
| `backend=` | `RECOMP_NET_BACKEND` | above |
| `course_world=` | `RECOMP_MOD_COURSE_WORLD` | `mod` / `backend`: who answers Course World with these backends (hybrid, above) |
| `nex_test_server=` | `RECOMP_NET_TEST_SERVER` | `ip[:port]` of the test server (default `127.0.0.1:44300`); local addresses only (loopback, private, link-local, 100.64/10). The game's TCP connections to it on 443 go to that port |
| `net_hosts=` | `RECOMP_NET_HOSTS` | extra redirect entries, `name=ip;*.domain=ip` (local addresses only) |
| `tls_trust=` | `RECOMP_NET_TLS_TRUST` | local-nex: `local-ca` (default): the server must present a certificate from the local test CA (`net/test-ca.pem`); `any`: any certificate, towards local addresses or the `ocw` host (where it is the default). (nextendo: always the pin) |
| `tls_ca=` | `RECOMP_NET_TLS_CA` | another CA file |
| `nextendo_server=` | `RECOMP_NEXTENDO_SERVER` | nextendo: the game server's address (default `51.178.29.194`) |
| `nextendo_nat=` | `RECOMP_NEXTENDO_NAT` | nextendo: the NAT check's address (default `164.132.111.120`) |
| `nextendo_tls_pin=` | `RECOMP_NEXTENDO_TLS_PIN` | nextendo: the game server certificate's SPKI SHA-256 (64 hex digits) |
| `id_token_file=` | `RECOMP_NET_ID_TOKEN_FILE` | a backend-issued ID token to send instead of the local one |
| `nex_token_file=` | `RECOMP_NET_NEX_TOKEN_FILE` | local-nex: a backend's NEX login token, carried in the local ID token's `nnex` claim; a secret: never logged (nextendo: the signed-in account's own) |
| `net_log=1` | `RECOMP_NET_LOG=1` | log every socket, name, TLS and token call |
| `net_log_data=1` | `RECOMP_NET_LOG_DATA=1` | ... and the TLS plaintext itself, the first 160 bytes of each read and write. Noisy, so it is off even with `net_log=1`. The player id and any registered secret (`net_log_secret`: the OCW token) are taken out before the line is logged, since a log file is what a player attaches to a bug report |

### Pieces

| Piece | Where | What |
|---|---|---|
| Configuration | `runtime/net/netcfg.c` | backend, **deny-by-default redirect table** (`net_hosts=` first; with `local-nex`: `*.nintendo.net`, `*.nintendo.com`, `*.nintendowifi.net`, `*.nintendo.co.jp`, `*.nintendoswitch.cn` → the test server; with `nextendo`: `nncs2-lp1.n.n.srv.nintendo.net` → their NAT check, the same five domains → their game server; every other name: no such name), destination policy (local-nex: loopback and the table's targets; nextendo: TCP to their game server and NAT check only, UDP anywhere for PIA's peers), TLS trust |
| Sockets | `runtime/services/bsd_host.c` (bsd.c dispatches) | `bsd:u` / `bsd:s` on host sockets (POSIX and Winsock, `runtime/net/hostsock.h`): TCP and UDP, non-blocking I/O, poll / select, socket options, accept / listen, FreeBSD errno numbers. Blocking calls wait in 50 ms slices with the guest thread's core released (`kernel_host_wait_begin`). A UDP port the game binds that is taken on the host gets another host port, and the game keeps seeing its own, the same pair again on a rebind (two instances on one machine). A UDP socket the game bound and closes is **parked** for 10 s and handed back when the game binds that port again (PIA closes and rebinds after its NAT check; the NAT mapping must survive). Connects / sends elsewhere: `ENETUNREACH`, logged (UDP: `net_dest_allowed_udp`). With nextendo a bind to INADDR_ANY stays INADDR_ANY. The virtual LAN keeps its own path |
| Names | `runtime/services/names.c` | `nsd:u` / `nsd:a` (`%` → `lp1`: the game asks for `g22306d00-%.s.n.srv.nintendo.net`) and `sfdnsres` (GetAddrInfo(WithOptions), GetHostByName(WithOptions)) from the redirect table only; the host's DNS is never asked |
| TLS | `runtime/services/ssl.c` + `third_party/mbedtls` (3.6.7 LTS, Apache-2.0) | `ssl`: contexts, connections over the game's socket, handshake (TLS 1.2), read / write / peek / pending / poll, ALPN (http/1.1 only), server certificate out. The **runtime's** trust policy decides (the game's own CAs and client certificates are ignored); when the game names an address as the host, the name last resolved to it is used for SNI and the check ("rebuilt"). With nextendo the chain is not checked (their certificate is self-signed) but the certificate's public key must match the pin (`ssl_pin_check`), else the connection is refused before the game sends anything |
| Account | `runtime/net/token.c`, `services/acc.c` | a local network service account ID (`GetAccountId`; the game logs in with it as its NEX user name) and an ID token: a JWT (HS256, `kid` `recomp-local`, claims shaped like a network service account's plus `tv` = the game version and, with `nex_token_file=`, `nnex`) signed with a local key, both kept in `net/account.txt` in the data folder (private). `id_token_file=` plugs a whole backend-issued token in instead. With nextendo: the account's PID, its user name as the nickname, and an RS256 ID token (below) |
| Test server | `tools/nex_test_server/` (Python, standard library + the `openssl` command) | a local NEX server: TLS on a local address, the HTTP/1.1 WebSocket upgrade, PRUDP Lite (SYN, CONNECT, acks, pings, fragments, resends), RMC, the Kerberos-style ticket (RC4 + HMAC-MD5), the secure server on PRUDP port 2, and enough of SMM2's DataStore for Course World's login (table below). Every request is logged and decoded where known; the token's signature is checked with `net/account.txt` and masked in the log. Makes the local test CA and its certificate in `recomp/private/smm2/net/` on first run (git-ignored; the proprietary guard blocks keys and certificates) |

### What the game does [obs, v3.0.3, backend local-nex]

When the enl session logs in, the game: loads the ID token (acc), asks nsd to resolve
`g22306d00-%.s.n.srv.nintendo.net` (→ `-lp1`), resolves it with getaddrinfo, then gethostbyname, opens a TCP
socket (SO_NOSIGPIPE), connects to port 443, makes an ssl context (version 1) and a connection (option
DoNotCloseSocket on, verify option 0x3, host name set), does the handshake, sends
`GET / HTTP/1.1` with `Upgrade: websocket`, `Sec-WebSocket-Key: NEX`, `Sec-WebSocket-Protocol: NEX`, and then
PRUDP Lite in binary frames: SYN (stream RVSecure, port 31 → 1, supported functions 6), CONNECT (with a lite
connection signature), DATA: RMC `Authentication.ValidateAndRequestTicketWithParam` {platform 3, user name = the
account ID in decimal, `AuthenticationInfo` {the ID token, NGS version 4, token type 2, server version 0}, NEX
version 40605, client version 60}. Given a ticket and the station URL `prudps:/address=0.0.0.1;port=1;...;sid=2`
("the same server"), it opens PRUDP port 30 → 2 **on the same WebSocket**, sends the ticket in its CONNECT, then
calls, in this order and all at boot (about frame 280, long before Course World opens):

| Request | The test server's answer |
|---|---|
| SecureConnection.Register (its station URL `prudp:/address=127.0.0.1;port=1;Pl=3;Tpt=2;...;sid=30`) | success, connection id, the URL back |
| Utility.GetIntegerSettings(0) | an empty map |
| DataStore.GetUsers (48) {own PID, option 0xe284} | one UserInfo (revision 3): the PID, a maker code, the name, a default Mii (CharInfo, the same as `services/mii.c`'s), country US. An empty list sends Course World to "Create Maker Avatar" (which needs the console's Mii editor) |
| DataStore.GetUserNameNgType (65) | 0 (the name is fine) |
| DataStore.SyncUserProfile (49) {name, Mii (the console's CharInfo), region, country ...} | the profile back |
| DataStore.GetMiiClothes (63) | none |
| DataStore.GetNewNotification (125) | a structure of three bools, all false [obs: its response extractor, main+0x5ef890, reads the structure header and three bools] (an error answer here crashed the game's network worker later: "Pure virtual function called") |

[obs] The Mii in a UserInfo is the 0x58-byte `nn::mii::CharInfo`: the GetUsers converter (main+0x14d0a60) copies
the buffer into the user entry as it is.

**Proof 2** (2026-10-19): with these answers Course World's top screen opens through the game's **own** NEX client
(Courses, Leaderboards, Network Play, Endless Challenge, Ninji Speedruns, the Maker card with the Mii); no mod
override ran (no `[mod]` line). Evidence (private): `/Volumes/SSD/smm-work/recomp-net/evidence/proof2/`. Course
lists, course downloads and the rest of DataStore are not answered yet (they log and get Core::NotImplemented).

### Try it

    python3 -m tools.nex_test_server --log /tmp/nex.log        # from the repository root; listens on 127.0.0.1:44300
    cd recomp/private/smm2 && RECOMP_NET_BACKEND=local-nex RECOMP_NET_LOG=1 ../../build/smm2/smm2recomp extracted

The server logs the game's TLS connection, the upgrade, the PRUDP handshakes, the decoded login (the token's
signature masked) and every request; then go to Course World: its top screen opens.

### The Nextendo backend (research/specs/recomp-nextendo.md)

The app is registered with Nextendo (a public OAuth client). The player signs in once in the browser
(`runtime/net/nextendo.c`: OAuth 2.0 + PKCE; the refresh token in the Keychain or a private file); `backend=nextendo`
then takes effect at the next start.

- **Names and addresses** (`netcfg.c`): `nncs2-lp1.n.n.srv.nintendo.net` → their NAT check `164.132.111.120`; every
  other name under the five Nintendo domains → their game server `51.178.29.194` (the game's NEX host is
  `g22306d00-lp1.s.n.srv.nintendo.net`). Only these built-in targets (and `nextendo_server=` / `nextendo_nat=`) may be
  public; `net_hosts=` stays local-only. TCP reaches those two addresses only (not `net_hosts=` targets); UDP any address (PIA peer to peer).
- **TLS** (`ssl.c`): their game server's certificate is self-signed (`CN=nintendo-private-server`), so it is pinned:
  after the handshake the SHA-256 of its SubjectPublicKeyInfo must be
  `d2e7388e48ee30cfbecfbeb522874f2cc6b85f7aa1105c57669f3c2aa85fb30e` (`nextendo_tls_pin=` replaces it). A mismatch
  (a new certificate, or someone in between) logs "certificate pin mismatch (expected Nextendo's game server);
  refused" and fails the handshake; there is no fallback to "any".
- **Account** (`acc.c`): `GetAccountId` is the Nextendo PID (the game logs in with it as its NEX user name), the
  profile's nickname is the Nextendo user name (cut to 32 bytes on a character boundary). `LoadIdTokenCache` first
  makes sure a game token is ready (`nextendo_prepare_game_login`, at most 10 s with the guest core free; cached
  for 24 h); without one the game gets no ID token (size 0) and shows its own login error.
- **ID token** (`token.c`): an RS256 JWT shaped like a network service account's (`kid` `nextendo-baas-key-1`, the
  BaaS issuer and audience, `sub` = the PID, a kept device id in `di` / `bs:did`, `tv` = the game version) whose
  `nnex` claim carries the game token; that is what their server checks. The RSA-2048 key is made once and kept in
  `net/nextendo-jwt-key.pem` (private). No game token (offline, the sign-in ended): no ID token, and the game shows
  its own login error. The PID, the game token and the ID token are never logged (lengths only).
- the rest is the game's own code: NAT check (`nncs1/2`), MatchmakeExtension, PIA peer-to-peer; their gates (a
  verified e-mail, one device at a time) show as login errors.

Tests: `tools/test_net.sh` (`tools/test_netcfg.c`: routing, destinations, the ID token's signature, the pin;
`NET_TEST_LIVE=1` adds a real handshake with their game server through the pin check).

### Proof 3 (two recomp instances in one Network Play lobby): where this stands (WIP, paused 2026-10-19)

- **Server side built** (`tools/nex_test_server`): the NAT check server (UDP 10025 / 10125, replies from 10026 /
  10027), server-sent requests (notifications, NAT traversal's InitiateProbe), MatchmakeExtension auto-matchmake
  (3 / 15 / 40), create / join (6 / 7 / 38 / 39), MatchMaking.GetSessionURLs, NAT traversal (1 / 3 / 4 / 5 / 6 / 7),
  SecureConnection UpdateURLs / ReplaceURL, MatchmakeExtension's no-answer methods, EndParticipation; sessions
  matched by game mode (`matchmaking.py`); accounts of several data folders (`--accounts`).
- **[obs] the application config is Utility.GetIntegerSettings(0)** (keys 0..37 -> the game's config array; the
  request function main+0x14ce2f0). With an empty map Co-op shows "This service is not active right now"; with every
  key 1 (now the default, `--config k=v` overrides) Co-op goes on: Network Play menu -> `cInetMultiClear` ->
  `cGetMyUserInfo` -> `cAutoMatchReadyEx` -> NetAppTask `cStartAutoMatch` -> enl `cAutoMatchReady` ->
  **MatchmakeExtension.AutoMatchmakeWithParam_Postpone** (game mode 11 for co-op, 6 attributes) arrives at the server.
- **Where it stops:** the game refuses the server's answer (the new session it creates): enl `cSilentError`,
  then MatchMaking method 2 and the Network Play error window. Next: check the MatchmakeSession encoding against the
  game's extractor (structure versions, the MatchmakeParam and later fields), as was done for GetNewNotification.
- **Still to do:** the NAT check and PIA's UDP (not reached yet); two instances on one Mac probably need a
  per-instance virtual address for PIA's UDP ports (127.0.0.2 can't be bound without root); the co-op course download
  (DataStore PrepareGetObject + an HTTPS course file); then both instances in one lobby, a course started, inputs.
- **Tools** (private, `/Volumes/SSD/smm-work/recomp-net/`): `run2.sh` (server + two instances with their own data
  folders / accounts), `cycle.sh` (one instance), `RECOMP_NET_TRACE_STATES=<states.txt>` (the enl / Network Play /
  NetAppTask state names, made from the player's own game by `stategen.py`; never committed) logs each state entered.
- Machine load matters: with ~28 other game instances on this Mac one instance reaches Network Play in 4-12 min.

## OpenCourse World (`backend=ocw`)

`games/smm2/mods/ocw.c`. **OpenCourse World** (<https://opencourse.world/>) is a replacement online service for
SMM2 v3.0.2 / 3.0.3. On a console you install its mod (Atmosphère / Ryujinx) and the game's own online code talks
to OCW instead of Nintendo. The recomp does the same thing, with the player's own OCW account.

The console mod is `_vendor/ocw-client-mod` (exlaunch, a `subsdk9`). It cannot be loaded here — the recomp loads
only `main` and `sdk` (`runtime/loader.c`) and runs recompiled C, so there are no instructions to patch at run
time — so the two patches that matter were ported. Its other eight stub `nn::account` calls that reach real
Nintendo services on a console; here those reach `runtime/services/acc.c`, which already reports a network service
account and hands out a local ID token whenever the game's own sockets are on.

| What | Where |
|---|---|
| the domain, written over the game's own host string at `main+0x22cc307` | `ocw.c` `game_modules_loaded()`, once the modules are loaded (the whole mapping is read/write) |
| the WebSocket handshake, built by the host so the token rides in a header | `ocw.c` `smm2_ocw_header()`, from the one patch at `main+0xAF560` (`mods/patches.py`) |

### Turning it on

Sign in at opencourse.world, accept its terms, and take the **domain** and your **personal token** from the package
it gives you (on a console those go on the SD card as `ocw-config.json`).

**In the game:** the overlay menu (F1) → **Mods**. Pick **OpenCourse World** under "Online backend" in the Online
section, and fill in the **OpenCourse World** rows just below it: `Domain`, `Address` and `Token`. Each opens a
field when activated — typed directly with a keyboard, or through the on-screen keyboard with a controller. The
rows are always shown, even with every mod off, because they have to be set *before* anything can work. Changing
any of them writes `ocw-config.json`. It all takes effect at the next start: the domain is written into the game's
code while the modules load.

`Token` says only "set" or "not set" until you activate it; the field appears then, and is hidden again when the
menu closes. The token never reaches a log either — `net_log_secret` registers it, and `net_log_redact` takes it out
of a logged line along with the player id.

**Or by hand**, which is the same thing — the file is where the settings live either way:

1. (as above: domain and token from opencourse.world)
2. `ocw-config.json` next to `smm2mods.ini` in the data folder (macOS source build:
   `recomp/private/smm2/ocw-config.json`):

   ```json
   { "domain": "server.opencourse.world", "token": "<22 characters>" }
   ```

   | key | meaning |
   |---|---|
   | `domain` | the host written into the game, so the game looks it up, sends it as `Host:` and names it for SNI. At most **29 characters**: the size of the string slot in the game's own binary. May itself be an address |
   | `token` | your account's token, exactly **22 characters**. A secret: keep it out of git |
   | `ip` | optional. The address `domain` resolves to, instead of asking the host resolver — this is how a self-hosted or local OCW instance is reached, and how an IP-only set-up keeps a real name for SNI and `Host:` |

3. `backend=ocw` in `smm2mods.ini` (what the Online section's backend choice writes). Both are needed: without the
   config the backend logs what is missing and the game stays offline, and without the backend the config is
   ignored and the game is left exactly as it is.

With `net_log=1` and `log=1` the log shows the domain being written, the name resolving, the socket being allowed
and the TLS handshake. Add `net_log_data=1` to see the plaintext of the WebSocket upgrade itself; the token is
overwritten there, so the log stays safe to attach to a bug report.

### What it is allowed to reach

The runtime's network policy is deny-by-default (`runtime/net/netcfg.h`, `../../DECISIONS.md`) and the `ocw`
backend is the one narrow opening in it:

- **exactly one name resolves** — the one `domain` names, from `ip` if given, else the host resolver. Every other
  name still fails; the game's names never reach the host resolver.
- **only its addresses are reachable** — `net_dest_allowed()` allows loopback and the addresses that name has
  answered with, nothing else. The name is re-resolved every five minutes (OCW's address is in AWS and moves), and
  addresses are merged rather than replaced so a session already connected to one keeps working.
- **its certificate is not checked.** `tls_trust` defaults to `any` under this backend, which is also what makes a
  self-hosted OCW with a self-signed certificate work. The trade is that anyone on the path can read or alter the
  session, including the token in the handshake; it is opt-in and never the default. `tls_trust=local-ca` with
  `tls_ca=` pointing at a real bundle turns verification back on.

### What the mod answers under `ocw`

Nothing: `hy_mode` (`games/smm2/mods/hybrid.h`) gives the `ocw` backend `{world 0, login 0, hybrid 0}`, so Course
World, the Endless Challenge and 100 Mario are all OCW's own, as `course_world=backend` does for Nextendo. Because
it is not hybrid, the mid-session fall-back to our server on a failed login (`net_backend_fall_back`, which Nextendo
uses) does not apply here.

### The account

OCW identifies the player by the **token in the handshake**, not by the console's account. The console mod makes
that plain: it stubs `GetNetworkServiceAccountId` to a flat `0xCAFE`, the same for every OCW player, and stubs
`LoadNetworkServiceAccountIdTokenCache` so no ID token is sent at all.

Under `backend=ocw` the recomp matches that exactly (`net_account_shared()`, `services/acc.c`): account ID `0xCAFE`
and an empty ID token. It does *not* use the local account (`runtime/net/token.h`), whose ID is random per install
and whose JWT is signed with a local key OCW could not check — a changing identity and an unverifiable token are
both good ways for a server not to find the player's profile again.

### Limits

TLS is a **1.2 client only** (`runtime/net/mbedtls_user_config.h`); `ssl.c` pins both the minimum and the maximum,
so a server that required 1.3 would fail the handshake whatever `tls_trust` says. OCW accepts 1.2 (checked
2026-10-22, Let's Encrypt ISRG Root X2) — re-check here if its TLS configuration is ever tightened.

How far the game's own Course World code gets against a real service is **not yet known**: `local-nex` has only
ever been run against `tools/nex_test_server`.

## Relaxed checks (`games/smm2/mods/relax.c`)

Two of the game's own checks made to always pass, each behind its own setting, **both off by default**. They came
from the OpenCourse World console mod, which applies them unconditionally; here they are a mod of their own, so
they work with any course server and OCW does not quietly change the game's behaviour.

| ini key | Variable | Mods tab | What |
|---|---|---|---|
| `no_word_filter=1` | `RECOMP_NO_WORD_FILTER=1` | "No word filter" | the text check (`nn::ngc`'s word filter) always passes, so names it would star out are shown as they are. Only this copy of the game: a course server does its own moderation (`../../docs/self-hosting.md`). Immediately |
| `allow_broken_courses=1` | `RECOMP_ALLOW_BROKEN_COURSES=1` | "Play courses the game calls broken" | the corrupt-course check always passes, so a course the game would refuse (and replace with a blank one) is played. Useful for courses out of an archive; one that really is broken may misbehave. From the next course start |

Each works as the console mod does it — the whole function returns 1 — but behind the setting: the hook
(`mods/relax.py`) runs before the function's own first instruction and returns early only when the setting is on
(`c->x[0] = 1; return;` is exactly `movz w0, #1; ret`, since the generator renders a guest `ret` as a plain
`return`). With the setting off the function behaves as before.
