//! SMM2 Endless Challenge rules, written from the private behaviour spec
//! `research/specs/endless-challenge.md` (section numbers below refer to it). This is SMM2's
//! mode; the SMM1 100 Mario Challenge (a fixed run length, client-held lives) is [`crate::hundred`].
//!
//! In SMM2 the server holds the run: lives, clears and the course order come from it, the client
//! keeps the in-course counters and reports each course result (§3.3). This module has both
//! halves as plain data so that our own server and an offline library apply the same rules:
//!
//! - [`Run`]: the client's in-course state — current counters, the course-start snapshot, the
//!   pending 1-Ups (max 3 per course), the attempt counter and the skip hint (§2.3–§2.5, §4.1).
//! - [`RunRecord`]: the authoritative per-difficulty record (§5.1, §8 "Persistence"), with the
//!   48-entry history ring (§5.2), the run's played ids (no repeats) and its course queue. It
//!   applies a [`Report`], the message the client sends after each course.
//! - [`EndlessSave`]: the four records, best clears per difficulty, the last difficulty and the
//!   lifetime skip count (JSON).
//! - [`pick`]: the local course-pick rule (§3.4): clear-rate bands, no repeats, never the
//!   player's own courses, batches of 10, widening when a band runs dry.
//! - [`Session`]: one offline run over a local library, driven by the screens one course result
//!   at a time.
//!
//! Tags as in the spec: [obs] seen in the binary, [inf] inferred, [ext] outside knowledge.

use std::collections::{HashSet, VecDeque};

use serde_json::{Map, Value, json};

use crate::Rng;

/// Lives never exceed 99 (§2.2) [obs].
pub const LIVES_CAP: u32 = 99;
/// Pending 1-Ups per course (§2.3) [obs].
pub const PENDING_CAP: u32 = 3;
/// Score cap (§2.6) [obs].
pub const SCORE_CAP: u32 = 999_999_990;
/// Clears and courses passed saturate at 2³¹ − 1 (§2.4) [obs].
pub const COUNT_CAP: u32 = i32::MAX as u32;
/// Goal time bonus per whole second left (§2.6) [obs].
pub const BONUS_PER_SECOND: u32 = 50;
/// Courses requested at a time (§3.1 SearchCoursesEndlessMode count) [obs].
pub const BATCH: usize = 10;
/// History ring length per difficulty (§5.2) [obs].
pub const HISTORY: usize = 48;
/// Courses a player was dealt lately, across runs and difficulties ([`EndlessSave::recent`]): a new
/// draw avoids them while fresh ones are left (ours, not the original's).
pub const RECENT: usize = 200;
/// World map nodes and courses per map page (§4.4) [obs].
pub const MAP_NODES: u32 = 32;
pub const MAP_PAGE: u32 = 16;
/// The attempt counter stops at 3 (§2.5) [obs].
pub const ATTEMPT_CAP: u32 = 3;
/// Lifetime skip counter cap, and the most skips for which the skip hint is still shown (§2.5)
/// [obs].
pub const SKIP_COUNT_CAP: u32 = 3;
pub const SKIP_HINT_MAX_SKIPS: u32 = 2;
/// Like/boo is offered on skip or suspend after more than this many frames (§2.8) [obs].
pub const RATING_MIN_FRAMES: u32 = 1800;
/// The global rank is shown on the map when it is at most this (§4.4) [obs].
pub const RANK_SHOWN: u32 = 100_000;
/// Starting lives of a New Game, every difficulty. The binary has no client default (the server
/// sends it, §2.1); 5 is community knowledge [ext/inf], kept per difficulty in
/// [`Difficulty::start_lives`] so it can follow a verified value.
pub const START_LIVES: u32 = 5;
/// A course's clear rate rates it only after this many attempts (§3.4) [inf, our proposal].
pub const MIN_RATED_ATTEMPTS: u64 = 20;
/// Band thresholds on the clear rate, in basis points (1/100 %): Easy ≥ 34 %, Normal 12–34 %,
/// Expert 3–12 %, Super Expert < 3 % (§3.4) [inf, our proposal; Nintendo's are unknown].
pub const EASY_MIN_RATE: u64 = 3400;
pub const NORMAL_MIN_RATE: u64 = 1200;
pub const EXPERT_MIN_RATE: u64 = 300;

/// The rules a run follows: SMM2's own Endless Challenge, or SMM1's 100 Mario Challenge played
/// on the Endless screens (the recomp's mod and SMCraft's option): 100 lives (cap 100), a run of
/// 8 / 16 / 16 / 6 clears that then completes, SMM1's fail-rate bands, skips free even after a
/// death (`smm1-100mario-code.md`; recomp-mods §2.6).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum Ruleset {
    #[default]
    Smm2,
    Hundred,
}

impl Ruleset {
    pub fn name(self) -> &'static str {
        match self {
            Ruleset::Smm2 => "smm2",
            Ruleset::Hundred => "hundred",
        }
    }

    pub fn from_name(s: &str) -> Option<Self> {
        match s {
            "smm2" | "endless" => Some(Ruleset::Smm2),
            "hundred" | "100mario" => Some(Ruleset::Hundred),
            _ => None,
        }
    }

    /// Lives of a New Game.
    pub fn start_lives(self, d: Difficulty) -> u32 {
        match self {
            Ruleset::Smm2 => d.start_lives(),
            Ruleset::Hundred => 100,
        }
    }

    /// The most lives a run can hold.
    pub fn lives_cap(self) -> u32 {
        match self {
            Ruleset::Smm2 => LIVES_CAP,
            Ruleset::Hundred => 100,
        }
    }

    /// Clears that complete a run (0: endless).
    pub fn goal(self, d: Difficulty) -> u32 {
        match self {
            Ruleset::Smm2 => 0,
            Ruleset::Hundred => crate::hundred::Difficulty::ALL[d.index()].run_length() as u32,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Difficulty {
    Easy,
    Normal,
    Expert,
    SuperExpert,
}

impl Difficulty {
    pub const ALL: [Difficulty; 4] = [
        Difficulty::Easy,
        Difficulty::Normal,
        Difficulty::Expert,
        Difficulty::SuperExpert,
    ];

    /// 0–3, the index the binary uses (§1.1).
    pub fn index(self) -> usize {
        self as usize
    }

    pub fn from_index(i: usize) -> Option<Self> {
        Self::ALL.get(i).copied()
    }

    /// Wire name (`mmk_server::ENDLESS`).
    pub fn name(self) -> &'static str {
        match self {
            Difficulty::Easy => "easy",
            Difficulty::Normal => "normal",
            Difficulty::Expert => "expert",
            Difficulty::SuperExpert => "super_expert",
        }
    }

    pub fn from_name(s: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|d| d.name() == s)
    }

    /// Lives of a New Game ([`START_LIVES`], [ext/inf]).
    pub fn start_lives(self) -> u32 {
        START_LIVES
    }

    /// The band of a clear rate in basis points.
    pub fn from_rate(bp: u64) -> Self {
        if bp >= EASY_MIN_RATE {
            Difficulty::Easy
        } else if bp >= NORMAL_MIN_RATE {
            Difficulty::Normal
        } else if bp >= EXPERT_MIN_RATE {
            Difficulty::Expert
        } else {
            Difficulty::SuperExpert
        }
    }

    /// The band of a course from its statistics, or None while it has fewer than
    /// [`MIN_RATED_ATTEMPTS`] attempts.
    pub fn from_stats(clears: u64, attempts: u64) -> Option<Self> {
        if attempts < MIN_RATED_ATTEMPTS {
            return None;
        }
        Some(Self::from_rate(clears.min(attempts) * 10_000 / attempts))
    }

    /// The band of a difficulty value (fail rate 0–100, as `pool::CourseRef::difficulty` and
    /// the server's `difficulty` field use it): clear rate = 100 − value.
    pub fn from_difficulty_value(v: u8) -> Self {
        Self::from_rate((100 - v.min(100) as u64) * 100)
    }

    /// The bands `distance` steps away (both sides), easier first.
    pub fn neighbours(self, distance: usize) -> Vec<Difficulty> {
        let i = self.index() as isize;
        let d = distance as isize;
        [i - d, i + d]
            .into_iter()
            .filter(|&j| j >= 0 && (j != i || d == 0))
            .filter_map(|j| Self::from_index(j as usize))
            .fold(Vec::new(), |mut v, x| {
                if !v.contains(&x) {
                    v.push(x);
                }
                v
            })
    }
}

/// Goal time bonus: 50 points per whole second left. The binary computes remaining frames
/// ÷ 60 × 1000 = ms, then ⌊ms / 1000⌋ × 50 (§2.6), which is ⌊frames / 60⌋ × 50.
pub fn time_bonus(frames_left: u32) -> u32 {
    let b = (frames_left / 60) as u64 * BONUS_PER_SECOND as u64;
    b.min(SCORE_CAP as u64) as u32
}

/// Like/boo offer after leaving a course by skip or suspend (§2.8): a network course not made by
/// the player, played for more than [`RATING_MIN_FRAMES`]. (The binary also offers it on a
/// second play-info condition that is not decoded.)
pub fn offer_rating(frames_played: u32, network_course: bool, own_course: bool) -> bool {
    network_course && !own_course && frames_played > RATING_MIN_FRAMES
}

/// Where a history entry sits on the world map (§4.4): `seq` is the entry's 1-based sequence
/// number. Node (s − 1) mod 32; page 1 for the first 16 courses, then 2 and 3 alternating per 16.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MapPos {
    pub node: u32,
    pub page: u32,
}

pub fn map_position(seq: u32) -> MapPos {
    let s = seq.max(1) - 1;
    let page = if s < MAP_PAGE {
        1
    } else if s & MAP_PAGE != 0 {
        2
    } else {
        3
    };
    MapPos {
        node: s % MAP_NODES,
        page,
    }
}

/// The run's counters, kept as "current" and as the course-start snapshot (§1.3, §4.1).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Counters {
    /// Courses passed: clears + skips.
    pub passed: u32,
    pub clears: u32,
    pub lives: u32,
    /// Coins collected in the run (total); the HUD shows [`Counters::hud_coins`].
    pub coins: u32,
    pub score: u32,
}

impl Counters {
    /// The within-100 coin counter (§1.4, §2.3).
    pub fn hud_coins(&self) -> u32 {
        self.coins % 100
    }
}

/// How a course attempt ended (§2.4). Wire names in [`Outcome::name`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Outcome {
    /// Goal (result 3).
    Clear,
    /// A miss (result 2).
    Death,
    /// "TIME'S UP!": a miss like a death [inf, §7 item 5].
    TimeUp,
    /// Start Over from the pause menu (result 5): costs a life.
    StartOver,
    /// Skip (result 1): free unless pressed during a miss demo.
    Skip,
    /// The course could not be downloaded or failed its check: moved past at no cost (§3.1, §8).
    Broken,
}

impl Outcome {
    pub const ALL: [Outcome; 6] = [
        Outcome::Clear,
        Outcome::Death,
        Outcome::TimeUp,
        Outcome::StartOver,
        Outcome::Skip,
        Outcome::Broken,
    ];

    pub fn name(self) -> &'static str {
        match self {
            Outcome::Clear => "clear",
            Outcome::Death => "death",
            Outcome::TimeUp => "time_up",
            Outcome::StartOver => "start_over",
            Outcome::Skip => "skip",
            Outcome::Broken => "broken",
        }
    }

    pub fn from_name(s: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|o| o.name() == s)
    }

    /// Death, time-up and Start Over cost a life and retry the course.
    pub fn is_miss(self) -> bool {
        matches!(self, Outcome::Death | Outcome::TimeUp | Outcome::StartOver)
    }
}

/// What the run does after a result.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Next {
    /// Go on to the next course.
    NextCourse,
    /// Play the same course again.
    Retry,
    /// Out of lives.
    GameOver,
}

/// A course result as the client reports it (to our server, or to the local record): the clear
/// report of §3.1 (job 0x36) generalised to every outcome. `coins` and `score` are what the run
/// gained since the course started, *before* the time bonus, which the record adds from
/// `frames_left` itself.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Report {
    /// Course id (the server's course code).
    pub code: String,
    pub outcome: Option<Outcome>,
    /// Pending 1-Ups at the goal (0–3).
    pub pending: u32,
    pub coins: u32,
    pub score: u32,
    /// Timer frames left at the goal (60 per second).
    pub frames_left: u32,
    /// A skip pressed while the miss demo was running (costs the life, §2.4).
    pub during_miss: bool,
}

impl Report {
    pub fn to_json(&self) -> Value {
        json!({
            "code": self.code,
            "result": self.outcome.map(Outcome::name),
            "pending": self.pending, "coins": self.coins, "score": self.score,
            "frames_left": self.frames_left, "during_miss": self.during_miss,
        })
    }

    pub fn from_json(v: &Value) -> Self {
        let n = |k: &str| v[k].as_u64().unwrap_or(0).min(u32::MAX as u64) as u32;
        Self {
            code: v["code"].as_str().unwrap_or_default().to_string(),
            outcome: v["result"].as_str().and_then(Outcome::from_name),
            pending: n("pending"),
            coins: n("coins"),
            score: n("score"),
            frames_left: n("frames_left"),
            during_miss: v["during_miss"].as_bool().unwrap_or(false),
        }
    }
}

/// The client's run: in-course counters and rules (§2.3–§2.5, §4.1).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Run {
    pub difficulty: Difficulty,
    pub current: Counters,
    /// The counters when the current course started (skip and suspend roll back to it).
    pub snapshot: Counters,
    /// 1-Ups earned in this course, paid into lives only at the goal (≤ 3, kept across retries).
    pub pending: u32,
    /// Retries of the current course (0 on its first attempt, capped at 3).
    pub attempts: u32,
    /// A skip now would interrupt a miss demo and cost the life (§2.4).
    pub during_miss: bool,
    /// The skip hint was shown since the run was entered from the menu.
    pub hint_shown: bool,
    /// Frames played in the current attempt (rating offer, §2.8).
    pub frames: u32,
}

impl Run {
    /// A New Game with `lives`.
    pub fn new(difficulty: Difficulty, lives: u32) -> Self {
        let c = Counters {
            lives: lives.min(LIVES_CAP),
            ..Counters::default()
        };
        Self {
            difficulty,
            current: c,
            snapshot: c,
            pending: 0,
            attempts: 0,
            during_miss: false,
            hint_shown: false,
            frames: 0,
        }
    }

    /// Continue from a record (§1.4): clears, coins, score and lives as the record holds them.
    pub fn resume(difficulty: Difficulty, s: &RunStatus) -> Self {
        let mut r = Self::new(difficulty, s.lives);
        r.current = Counters {
            passed: s.passed,
            clears: s.clears,
            lives: s.lives.min(LIVES_CAP),
            coins: s.coins,
            score: s.score.min(SCORE_CAP),
        };
        r.snapshot = r.current;
        r
    }

    /// A new course starts: snapshot the counters, reset the attempt counter (§4.1).
    pub fn start_course(&mut self) {
        self.snapshot = self.current;
        self.attempts = 0;
        self.pending = 0;
        self.during_miss = false;
        self.frames = 0;
    }

    /// The same course starts again after a miss: the attempt counter goes up (capped at 3);
    /// the snapshot and the pending 1-Ups are kept (§2.3, §4.1).
    pub fn retry_course(&mut self) {
        self.attempts = (self.attempts + 1).min(ATTEMPT_CAP);
        self.during_miss = false;
        self.frames = 0;
    }

    /// Coins collected: every 100 on the within-100 counter is a pending 1-Up (§2.3). Returns the
    /// 1-Ups that fitted under the cap.
    pub fn add_coins(&mut self, n: u32) -> u32 {
        let due = (self.current.hud_coins() as u64 + n as u64) / 100;
        self.current.coins = self.current.coins.saturating_add(n);
        self.one_up(due.min(u32::MAX as u64) as u32)
    }

    /// 1-Up items (a 3-Up Moon is 3, [inf]): pending = min(pending + n, 3); the rest is lost.
    /// Returns how many fitted.
    pub fn one_up(&mut self, n: u32) -> u32 {
        let new = self.pending.saturating_add(n).min(PENDING_CAP);
        let got = new - self.pending;
        self.pending = new;
        got
    }

    pub fn add_score(&mut self, points: u32) {
        self.current.score = self.current.score.saturating_add(points).min(SCORE_CAP);
    }

    /// The miss demo started: a skip from now on costs the life.
    pub fn mark_miss_demo(&mut self) {
        self.during_miss = true;
    }

    /// Count played frames (for [`Run::offer_rating`]).
    pub fn tick(&mut self, frames: u32) {
        self.frames = self.frames.saturating_add(frames);
    }

    pub fn offer_rating(&self, network_course: bool, own_course: bool) -> bool {
        offer_rating(self.frames, network_course, own_course)
    }

    /// Show the skip hint now? On the 4th attempt of a course (attempt counter 3), once per menu
    /// entry, while the player has skipped at most twice in total (§2.5). Marks it shown.
    pub fn take_skip_hint(&mut self, lifetime_skips: u32) -> bool {
        let show = !self.hint_shown
            && self.attempts == ATTEMPT_CAP
            && lifetime_skips <= SKIP_HINT_MAX_SKIPS;
        if show {
            self.hint_shown = true;
        }
        show
    }

    /// Coins and score gained since the course started (what a [`Report`] carries).
    pub fn gained(&self) -> (u32, u32) {
        (
            self.current.coins.saturating_sub(self.snapshot.coins),
            self.current.score.saturating_sub(self.snapshot.score),
        )
    }

    /// Apply a course result (§2.4) and build its report for course `code`.
    /// - Clear: score += [`time_bonus`]; pending → lives (cap 99); clears and passed +1; pending 0.
    /// - Death / time-up / Start Over: lives −1 (floor 0); coins, score and pending are kept.
    /// - Skip: counters back to the snapshot (lives as now, −1 during a miss demo); passed +1;
    ///   pending 0.
    /// - Broken: back to the snapshot, nothing counted.
    pub fn finish(&mut self, code: &str, outcome: Outcome, frames_left: u32) -> (Next, Report) {
        let (coins, score) = self.gained();
        let report = Report {
            code: code.to_string(),
            outcome: Some(outcome),
            pending: self.pending,
            coins,
            score,
            frames_left: if outcome == Outcome::Clear {
                frames_left
            } else {
                0
            },
            during_miss: outcome == Outcome::Skip && self.during_miss,
        };
        let c = &mut self.current;
        let next = match outcome {
            Outcome::Clear => {
                c.score = c
                    .score
                    .saturating_add(time_bonus(frames_left))
                    .min(SCORE_CAP);
                c.lives = (c.lives + self.pending).min(LIVES_CAP);
                c.clears = (c.clears + 1).min(COUNT_CAP);
                c.passed = (c.passed + 1).min(COUNT_CAP);
                self.pending = 0;
                Next::NextCourse
            }
            o if o.is_miss() => {
                c.lives = c.lives.saturating_sub(1);
                if c.lives == 0 {
                    Next::GameOver
                } else {
                    Next::Retry
                }
            }
            Outcome::Skip => {
                let lives = c.lives.saturating_sub(self.during_miss as u32);
                *c = self.snapshot;
                c.lives = lives;
                c.passed = (c.passed + 1).min(COUNT_CAP);
                self.pending = 0;
                if lives == 0 {
                    Next::GameOver
                } else {
                    Next::NextCourse
                }
            }
            _ => {
                let lives = c.lives;
                *c = self.snapshot;
                c.lives = lives;
                self.pending = 0;
                Next::NextCourse
            }
        };
        self.during_miss = false;
        (next, report)
    }

    /// Quit from the pause menu or the map (§2.4 result 0): the counters go back to the
    /// course-start snapshot, lives included; pending 0. (The saved record keeps the lives the
    /// misses already cost, see [`RunRecord`].)
    pub fn suspend(&mut self) {
        self.current = self.snapshot;
        self.pending = 0;
        self.attempts = 0;
        self.during_miss = false;
        self.hint_shown = false;
    }
}

/// How a course left the run (§5.2; the binary's own result values are not decoded, §7 item 9).
/// The ring holds clears only: the game pushes a local entry on a clear (sequence = last + 1) and
/// none on a skip, and its map draws every entry alike, so a skip in the ring would move the map
/// Mario ahead of the clear count after the next reload (#83). `Skip` is still read from old saves.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HistoryResult {
    Clear,
    Skip,
}

impl HistoryResult {
    pub fn name(self) -> &'static str {
        match self {
            HistoryResult::Clear => "clear",
            HistoryResult::Skip => "skip",
        }
    }
}

/// One history ring entry (§5.2): course id, result, 1-based sequence number, style and theme.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct HistoryEntry {
    pub code: String,
    pub result: HistoryResult,
    pub seq: u32,
    /// Game style code ("M1", "M3", "MW", "WU", "3W").
    pub style: String,
    pub theme: u8,
}

impl HistoryEntry {
    pub fn map(&self) -> MapPos {
        map_position(self.seq)
    }

    pub fn to_json(&self) -> Value {
        json!({ "code": self.code, "result": self.result.name(), "seq": self.seq,
                "style": self.style, "theme": self.theme })
    }

    pub fn from_json(v: &Value) -> Option<Self> {
        Some(Self {
            code: v["code"].as_str()?.to_string(),
            result: match v["result"].as_str()? {
                "clear" => HistoryResult::Clear,
                "skip" => HistoryResult::Skip,
                _ => return None,
            },
            seq: v["seq"].as_u64().unwrap_or(0) as u32,
            style: v["style"].as_str().unwrap_or_default().to_string(),
            theme: v["theme"].as_u64().unwrap_or(0) as u8,
        })
    }
}

/// A run's summary (the status record of §5.1): what the menu shows and Continue restores.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RunStatus {
    /// The run exists and is not over.
    pub active: bool,
    pub lives: u32,
    pub clears: u32,
    pub passed: u32,
    /// Total coins (HUD: mod 100).
    pub coins: u32,
    pub score: u32,
    pub start_lives: u32,
    /// Seconds since the epoch (set by the caller; the core has no clock).
    pub started_at: u64,
    pub updated_at: u64,
    /// Best clears on this difficulty when the run started ("New High Score!" when beaten).
    pub best_before: u32,
    /// The pick rule had to leave the difficulty's band at least once in this run.
    pub widened: bool,
    /// The rules of this run, and the clears that complete it (0: endless).
    pub ruleset: Ruleset,
    pub goal: u32,
    /// The run reached its goal (a completed 100 Mario run).
    pub complete: bool,
}

impl RunStatus {
    /// Continue is offered while the run has lives and isn't complete (§1.2).
    pub fn can_continue(&self) -> bool {
        self.active && self.lives > 0 && !self.complete
    }

    pub fn new_high_score(&self) -> bool {
        self.clears > self.best_before
    }

    pub fn to_json(&self) -> Value {
        json!({
            "active": self.active, "lives": self.lives, "clears": self.clears,
            "passed": self.passed, "coins": self.coins, "score": self.score,
            "start_lives": self.start_lives, "started_at": self.started_at,
            "updated_at": self.updated_at, "best_before": self.best_before,
            "widened": self.widened, "ruleset": self.ruleset.name(), "goal": self.goal,
            "complete": self.complete,
        })
    }

    pub fn from_json(v: &Value) -> Self {
        let n = |k: &str| v[k].as_u64().unwrap_or(0).min(u32::MAX as u64) as u32;
        let ruleset = v["ruleset"]
            .as_str()
            .and_then(Ruleset::from_name)
            .unwrap_or_default();
        Self {
            ruleset,
            goal: n("goal"),
            complete: v["complete"].as_bool().unwrap_or(false),
            active: v["active"].as_bool().unwrap_or(false),
            lives: n("lives").min(ruleset.lives_cap()),
            clears: n("clears"),
            passed: n("passed"),
            coins: n("coins"),
            score: n("score").min(SCORE_CAP),
            start_lives: n("start_lives"),
            started_at: v["started_at"].as_u64().unwrap_or(0),
            updated_at: v["updated_at"].as_u64().unwrap_or(0),
            best_before: n("best_before"),
            widened: v["widened"].as_bool().unwrap_or(false),
        }
    }
}

/// What a report did to the record (the server's {lives, clears} answer, §3.1).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Applied {
    pub lives: u32,
    pub clears: u32,
    pub passed: u32,
    pub game_over: bool,
    /// This clear completed the run (it reached its goal).
    pub complete: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RunError {
    /// No run on this difficulty.
    NoRun,
    /// The run is over (no lives).
    NotActive,
    /// The report names a course that is not queued in this run.
    NotInRun,
    /// No course can be picked.
    NoCourses,
}

/// The authoritative record of one difficulty's run (§5.1 + §8 "Persistence"): what our server
/// keeps per (player, difficulty), and what the offline [`Session`] saves.
///
/// Lives follow the misses as they are reported: a quit restores the client's counters to the
/// course-start snapshot, but the record keeps the lives already lost on that course. (The
/// binary's retry reloads lives from the server record, so the server counts each miss, §4.1.)
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct RunRecord {
    pub status: RunStatus,
    /// The last [`HISTORY`] courses passed, oldest first.
    pub history: VecDeque<HistoryEntry>,
    /// Every course passed in this run (no repeats), in order.
    pub played: Vec<String>,
    /// Courses drawn for the run and not passed yet; the first is the one being played.
    pub queue: Vec<String>,
    /// Reports received for the queue's first course.
    pub course_reports: u32,
}

impl RunRecord {
    /// A New Game under `ruleset` on `d`, with the difficulty's best so far and the time.
    pub fn with_rules(ruleset: Ruleset, d: Difficulty, best_before: u32, now: u64) -> Self {
        let mut r = Self::new(ruleset.start_lives(d), best_before, now);
        r.status.ruleset = ruleset;
        r.status.goal = ruleset.goal(d);
        r.status.lives = ruleset.start_lives(d).min(ruleset.lives_cap());
        r.status.start_lives = r.status.lives;
        r
    }

    /// A New Game with `lives`, the difficulty's best so far and the time.
    pub fn new(lives: u32, best_before: u32, now: u64) -> Self {
        Self {
            status: RunStatus {
                active: true,
                lives: lives.min(LIVES_CAP),
                start_lives: lives.min(LIVES_CAP),
                started_at: now,
                updated_at: now,
                best_before,
                ..RunStatus::default()
            },
            ..Self::default()
        }
    }

    /// The course being played (the head of the queue).
    pub fn current(&self) -> Option<&str> {
        self.queue.first().map(String::as_str)
    }

    /// Ids the pick rule must not draw again in this run.
    pub fn excluded(&self) -> HashSet<&str> {
        self.played
            .iter()
            .chain(&self.queue)
            .map(String::as_str)
            .collect()
    }

    /// Add a drawn batch to the queue (ids already in the run are dropped).
    pub fn enqueue(&mut self, ids: impl IntoIterator<Item = String>, widened: bool) {
        for id in ids {
            if !self.played.contains(&id) && !self.queue.contains(&id) {
                self.queue.push(id);
            }
        }
        self.status.widened |= widened;
    }

    /// The queue's first course is done (passed or broken).
    fn pass(&mut self, code: &str) {
        self.queue.retain(|c| c != code);
        if !self.played.iter().any(|c| c == code) {
            self.played.push(code.to_string());
        }
        self.course_reports = 0;
    }

    fn push_history(&mut self, code: &str, result: HistoryResult, style: &str, theme: u8) {
        if self.history.len() >= HISTORY {
            self.history.pop_front();
        }
        self.history.push_back(HistoryEntry {
            code: code.to_string(),
            result,
            seq: self.status.clears,
            style: style.to_string(),
            theme,
        });
    }

    /// Apply a course result on course `r.code` (style and theme for the history entry). The
    /// course must be queued in this run. Same rules as [`Run::finish`]; the pending count is
    /// capped at 3 whatever the report says.
    pub fn report(
        &mut self,
        r: &Report,
        style: &str,
        theme: u8,
        now: u64,
    ) -> Result<Applied, RunError> {
        let outcome = r.outcome.ok_or(RunError::NotInRun)?;
        if !self.status.can_continue() {
            return Err(RunError::NotActive);
        }
        if !self.queue.contains(&r.code) {
            return Err(RunError::NotInRun);
        }
        if self.current() != Some(r.code.as_str()) {
            // reported out of order: make it the current course
            self.queue.retain(|c| *c != r.code);
            self.queue.insert(0, r.code.clone());
            self.course_reports = 0;
        }
        self.course_reports += 1;
        let s = &mut self.status;
        let cap = s.ruleset.lives_cap();
        match outcome {
            Outcome::Clear => {
                s.lives = (s.lives + r.pending.min(PENDING_CAP)).min(cap);
                s.clears = (s.clears + 1).min(COUNT_CAP);
                s.passed = (s.passed + 1).min(COUNT_CAP);
                s.coins = s.coins.saturating_add(r.coins);
                s.score = s
                    .score
                    .saturating_add(r.score)
                    .saturating_add(time_bonus(r.frames_left))
                    .min(SCORE_CAP);
                self.push_history(&r.code, HistoryResult::Clear, style, theme);
                self.pass(&r.code);
            }
            o if o.is_miss() => s.lives = s.lives.saturating_sub(1),
            Outcome::Skip => {
                // SMM2 charges a skip during the miss demo; SMM1's skips are always free
                if s.ruleset == Ruleset::Smm2 {
                    s.lives = s.lives.saturating_sub(r.during_miss as u32);
                }
                s.passed = (s.passed + 1).min(COUNT_CAP);
                self.pass(&r.code);
            }
            _ => self.pass(&r.code),
        }
        let s = &mut self.status;
        s.updated_at = now;
        let complete = outcome == Outcome::Clear && s.goal > 0 && s.clears >= s.goal;
        if complete {
            s.complete = true;
            s.active = false;
        }
        if s.lives == 0 {
            s.active = false;
        }
        Ok(Applied {
            lives: s.lives,
            clears: s.clears,
            passed: s.passed,
            game_over: s.lives == 0,
            complete,
        })
    }

    pub fn to_json(&self) -> Value {
        let mut v = self.status.to_json();
        v["history"] = Value::Array(self.history.iter().map(HistoryEntry::to_json).collect());
        v["played"] = json!(self.played);
        v["queue"] = json!(self.queue);
        v["course_reports"] = json!(self.course_reports);
        v
    }

    pub fn from_json(v: &Value) -> Self {
        let ids = |k: &str| -> Vec<String> {
            v[k].as_array()
                .map(|a| {
                    a.iter()
                        .filter_map(|x| Some(x.as_str()?.to_string()))
                        .collect()
                })
                .unwrap_or_default()
        };
        let status = RunStatus::from_json(v);
        let mut history: VecDeque<HistoryEntry> = v["history"]
            .as_array()
            .map(|a| a.iter().filter_map(HistoryEntry::from_json).collect())
            .unwrap_or_default();
        if history.iter().any(|e| e.result == HistoryResult::Skip) {
            // a save from before v0.7.12 (skips in the ring, numbered by courses passed): keep
            // the clears and number them up to the run's clear count
            history.retain(|e| e.result == HistoryResult::Clear);
            let n = history.len() as u32;
            for (i, e) in history.iter_mut().enumerate() {
                e.seq = (status.clears + 1 + i as u32).saturating_sub(n).max(1);
            }
        }
        while history.len() > HISTORY {
            history.pop_front();
        }
        Self {
            status,
            history,
            played: ids("played"),
            queue: ids("queue"),
            course_reports: v["course_reports"].as_u64().unwrap_or(0) as u32,
        }
    }
}

/// Game over or a finished run (§2.7).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Finished {
    pub clears: u32,
    pub best: u32,
    pub new_high_score: bool,
}

impl Finished {
    pub fn to_json(&self) -> Value {
        json!({ "clears": self.clears, "best": self.best, "new_high_score": self.new_high_score })
    }

    pub fn from_json(v: &Value) -> Self {
        Self {
            clears: v["clears"].as_u64().unwrap_or(0) as u32,
            best: v["best"].as_u64().unwrap_or(0) as u32,
            new_high_score: v["new_high_score"].as_bool().unwrap_or(false),
        }
    }
}

/// Endless data of one player (§8 "Persistence"): the four run records, best clears per
/// difficulty, the last difficulty chosen and the lifetime skip count (0–3, §2.5).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct EndlessSave {
    /// SMM2's Endless runs (ruleset smm2).
    pub runs: [Option<RunRecord>; 4],
    /// The 100 Mario runs (ruleset hundred), kept apart: a New Game in one mode never ends the other's run.
    pub hundred: [Option<RunRecord>; 4],
    pub best: [u32; 4],
    pub last: Option<Difficulty>,
    pub skips: u32,
    /// The last [`RECENT`] courses dealt to this player in any run, oldest first.
    pub recent: VecDeque<String>,
}

impl EndlessSave {
    /// Remember courses just dealt (the oldest are forgotten past [`RECENT`]).
    pub fn note_recent<'a>(&mut self, ids: impl IntoIterator<Item = &'a str>) {
        for id in ids {
            self.recent.retain(|c| c != id);
            self.recent.push_back(id.to_string());
        }
        while self.recent.len() > RECENT {
            self.recent.pop_front();
        }
    }

    pub fn run(&self, d: Difficulty) -> Option<&RunRecord> {
        self.runs[d.index()].as_ref()
    }

    pub fn run_mut(&mut self, d: Difficulty) -> Option<&mut RunRecord> {
        self.runs[d.index()].as_mut()
    }

    pub fn can_continue(&self, d: Difficulty) -> bool {
        self.run(d).is_some_and(|r| r.status.can_continue())
    }

    /// New Game: wipe that difficulty's run and start one with its starting lives.
    pub fn new_game(&mut self, d: Difficulty, now: u64) -> &mut RunRecord {
        self.new_game_with(d, Ruleset::Smm2, now)
    }

    /// New Game under `ruleset` (100 Mario: 100 lives and a goal), in that ruleset's slot.
    pub fn new_game_with(&mut self, d: Difficulty, ruleset: Ruleset, now: u64) -> &mut RunRecord {
        self.last = Some(d);
        let best = self.best[d.index()];
        self.slots_mut(ruleset)[d.index()].insert(RunRecord::with_rules(ruleset, d, best, now))
    }

    fn slots_mut(&mut self, ruleset: Ruleset) -> &mut [Option<RunRecord>; 4] {
        match ruleset {
            Ruleset::Smm2 => &mut self.runs,
            Ruleset::Hundred => &mut self.hundred,
        }
    }

    /// The run of `ruleset` on `d`.
    pub fn run_of(&self, ruleset: Ruleset, d: Difficulty) -> Option<&RunRecord> {
        match ruleset {
            Ruleset::Smm2 => self.runs[d.index()].as_ref(),
            Ruleset::Hundred => self.hundred[d.index()].as_ref(),
        }
    }

    pub fn run_of_mut(&mut self, ruleset: Ruleset, d: Difficulty) -> Option<&mut RunRecord> {
        self.slots_mut(ruleset)[d.index()].as_mut()
    }

    /// Which slot a request on `d` means: `want` when given, else the one holding a run that can
    /// go on, else the one holding a run at all (SMM2's when both or neither do).
    pub fn slot(&self, d: Difficulty, want: Option<Ruleset>) -> Ruleset {
        let live = |r: Ruleset| self.run_of(r, d).is_some_and(|x| x.status.can_continue());
        let held = |r: Ruleset| self.run_of(r, d).is_some();
        let hundred = (live(Ruleset::Hundred) && !live(Ruleset::Smm2))
            || (held(Ruleset::Hundred) && !held(Ruleset::Smm2));
        want.unwrap_or(if hundred {
            Ruleset::Hundred
        } else {
            Ruleset::Smm2
        })
    }

    /// Finish the run of `ruleset` on `d` (see [`Self::finish`]).
    pub fn finish_of(&mut self, ruleset: Ruleset, d: Difficulty) -> Option<Finished> {
        let r = self.slots_mut(ruleset)[d.index()].take()?;
        // a 100 Mario run isn't an Endless score
        let best = if r.status.ruleset == Ruleset::Smm2 {
            self.update_best(d, r.status.clears)
        } else {
            self.best[d.index()]
        };
        Some(Finished {
            clears: r.status.clears,
            best,
            new_high_score: r.status.new_high_score(),
        })
    }

    /// Keep the best clears (called after every clear, so a suspended run counts too).
    pub fn update_best(&mut self, d: Difficulty, clears: u32) -> u32 {
        let b = &mut self.best[d.index()];
        *b = (*b).max(clears);
        *b
    }

    /// One more skip on the lifetime counter (capped at 3).
    pub fn note_skip(&mut self) {
        self.skips = (self.skips + 1).min(SKIP_COUNT_CAP);
    }

    /// Finish the run (game over): update the best and clear the run.
    pub fn finish(&mut self, d: Difficulty) -> Option<Finished> {
        self.finish_of(Ruleset::Smm2, d)
    }

    pub fn to_json(&self) -> Value {
        let per = |f: &dyn Fn(Difficulty) -> Value| -> Map<String, Value> {
            Difficulty::ALL
                .iter()
                .map(|&d| (d.name().to_string(), f(d)))
                .collect()
        };
        json!({
            "version": 1,
            "runs": per(&|d| self.run(d).map_or(Value::Null, RunRecord::to_json)),
            "hundred_runs": per(&|d| {
                self.run_of(Ruleset::Hundred, d)
                    .map_or(Value::Null, RunRecord::to_json)
            }),
            "best": per(&|d| json!(self.best[d.index()])),
            "last": self.last.map(Difficulty::name),
            "skips": self.skips,
            "recent": self.recent,
        })
    }

    pub fn from_json(v: &Value) -> Self {
        let mut s = Self::default();
        for d in Difficulty::ALL {
            // (files from before the separate slots kept 100 Mario runs under "runs": by their ruleset)
            for key in ["runs", "hundred_runs"] {
                let r = &v[key][d.name()];
                if r.is_object() {
                    let rec = RunRecord::from_json(r);
                    let ruleset = rec.status.ruleset;
                    s.slots_mut(ruleset)[d.index()] = Some(rec);
                }
            }
            s.best[d.index()] = v["best"][d.name()].as_u64().unwrap_or(0) as u32;
        }
        s.last = v["last"].as_str().and_then(Difficulty::from_name);
        s.skips = (v["skips"].as_u64().unwrap_or(0) as u32).min(SKIP_COUNT_CAP);
        let recent = v["recent"].as_array().into_iter().flatten();
        s.note_recent(recent.filter_map(Value::as_str));
        s
    }
}

/// A course the pick rule can draw.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Candidate {
    pub id: String,
    pub name: String,
    pub maker: String,
    /// The maker's id ("" unknown): never drawn for that player.
    pub owner: String,
    pub style: String,
    pub theme: u8,
    pub attempts: u64,
    pub clears: u64,
    /// The band to use while the course has too few attempts to be rated: the course's own
    /// difficulty value or an author-set band (offline libraries). None: unrated.
    pub hint: Option<Difficulty>,
    /// Broken or banned: never drawn.
    pub broken: bool,
}

impl Candidate {
    /// The course's band: from its clear rate once it has [`MIN_RATED_ATTEMPTS`] attempts, else
    /// its hint, else None (unrated).
    pub fn band(&self) -> Option<Difficulty> {
        Difficulty::from_stats(self.clears, self.attempts).or(self.hint)
    }
}

/// The result of a draw.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Picked {
    /// Indices into the candidate list, in play order.
    pub indices: Vec<usize>,
    /// The draw left the difficulty's band (neighbouring bands or unrated courses).
    pub widened: bool,
}

/// The local pick rule (§3.4): up to `count` (≤ [`BATCH`] is the original's request) courses for
/// difficulty `d`, drawn at random from the courses in its band, never one in `excluded` (the
/// run's played and queued ids), never one owned by `player` (unless `player` is empty), never a
/// broken one. When the band has none left, the draw widens to the bands one step away (both
/// sides), then two, then three, then to unrated courses, and says so in [`Picked::widened`].
/// A band with fewer than `count` left gives just those (the next draw widens).
pub fn pick(
    candidates: &[Candidate],
    d: Difficulty,
    excluded: &HashSet<&str>,
    player: &str,
    count: usize,
    rng: &mut Rng,
) -> Picked {
    let eligible: Vec<usize> = candidates
        .iter()
        .enumerate()
        .filter(|(_, c)| !c.broken && !excluded.contains(c.id.as_str()))
        .filter(|(_, c)| player.is_empty() || c.owner != player)
        .map(|(i, _)| i)
        .collect();
    let tiers: Vec<Vec<Option<Difficulty>>> = (0..4)
        .map(|k| d.neighbours(k).into_iter().map(Some).collect())
        .chain(std::iter::once(vec![None]))
        .collect();
    for (k, tier) in tiers.iter().enumerate() {
        let mut pool: Vec<usize> = eligible
            .iter()
            .copied()
            .filter(|&i| tier.contains(&candidates[i].band()))
            .collect();
        if pool.is_empty() {
            continue;
        }
        let n = pool.len();
        for i in 0..n.min(count) {
            let j = i + rng.below((n - i) as u32) as usize;
            pool.swap(i, j);
        }
        pool.truncate(count);
        return Picked {
            indices: pool,
            widened: k > 0,
        };
    }
    Picked::default()
}

/// The 100 Mario pick rule: like [`pick`], but by SMM1's fail-rate bands (`hundred::Difficulty::
/// band`: Easy 0–34, Normal 0–74, Expert 75–95, Super Expert 96–100) on the courses rated by
/// their attempts; when the band has none left it widens to any rated course, then unrated ones.
pub fn pick_hundred(
    candidates: &[Candidate],
    d: Difficulty,
    excluded: &HashSet<&str>,
    player: &str,
    count: usize,
    rng: &mut Rng,
) -> Picked {
    let (lo, hi) = crate::hundred::Difficulty::ALL[d.index()].band();
    let fail = |c: &Candidate| -> Option<u64> {
        (c.attempts >= MIN_RATED_ATTEMPTS)
            .then(|| 100 - (c.clears * 100 / c.attempts.max(1)).min(100))
    };
    let eligible: Vec<usize> = candidates
        .iter()
        .enumerate()
        .filter(|(_, c)| !c.broken && !excluded.contains(c.id.as_str()))
        .filter(|(_, c)| player.is_empty() || c.owner != player)
        .map(|(i, _)| i)
        .collect();
    type Tier<'a> = Box<dyn Fn(&Candidate) -> bool + 'a>;
    let tiers: [Tier; 3] = [
        Box::new(|c| fail(c).is_some_and(|f| f >= lo as u64 && f <= hi as u64)),
        Box::new(|c| fail(c).is_some()),
        Box::new(|c| fail(c).is_none()),
    ];
    for (k, tier) in tiers.iter().enumerate() {
        let mut pool: Vec<usize> = eligible
            .iter()
            .copied()
            .filter(|&i| tier(&candidates[i]))
            .collect();
        if pool.is_empty() {
            continue;
        }
        let n = pool.len();
        for i in 0..n.min(count) {
            let j = i + rng.below((n - i) as u32) as usize;
            pool.swap(i, j);
        }
        pool.truncate(count);
        return Picked {
            indices: pool,
            widened: k > 0,
        };
    }
    Picked::default()
}

/// A pick rule ([`pick`] or [`pick_hundred`]).
pub type PickRule = fn(&[Candidate], Difficulty, &HashSet<&str>, &str, usize, &mut Rng) -> Picked;

/// `rule`, avoiding the player's `recent` courses (oldest first) as well as the run's `excluded`
/// ones: fresh courses first; when fewer than `count` are fresh, the oldest recent ones are let
/// back in, a growing share at a time, so a repeat is the course seen longest ago.
#[allow(clippy::too_many_arguments)] // the pick rule's own arguments, plus the rule and the recent list
pub fn pick_fresh(
    rule: PickRule,
    candidates: &[Candidate],
    d: Difficulty,
    excluded: &HashSet<&str>,
    recent: &VecDeque<String>,
    player: &str,
    count: usize,
    rng: &mut Rng,
) -> Picked {
    let mut ex: HashSet<&str> = excluded.clone();
    ex.extend(recent.iter().map(String::as_str));
    let mut out = rule(candidates, d, &ex, player, count, rng);
    // short of fresh ones: the recent courses this run hasn't had, oldest first, a growing share at a time
    let back: Vec<&str> = recent
        .iter()
        .map(String::as_str)
        .filter(|c| !excluded.contains(c))
        .collect();
    let mut released = 0;
    while out.indices.len() < count && released < back.len() {
        released = (released.max(count) * 2).min(back.len());
        let mut ex: HashSet<&str> = excluded.clone();
        ex.extend(&back[released..]);
        ex.extend(out.indices.iter().map(|&i| candidates[i].id.as_str()));
        let p = rule(candidates, d, &ex, player, count - out.indices.len(), rng);
        // (only the courses let back in: a wider band's fresh ones were the first draw's to take)
        let again: Vec<usize> = p
            .indices
            .into_iter()
            .filter(|&i| back[..released].contains(&candidates[i].id.as_str()))
            .collect();
        out.indices.extend(again);
    }
    out
}

/// What the screen shows next after [`Session::report`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Step {
    /// A new course is current (course-in card, then play).
    Play,
    /// Retry the same course.
    Retry,
    /// Out of lives: Game Over, then the run was finished.
    GameOver(Finished),
    /// The library has no course left for this run: back to the menu (the run is kept).
    OutOfCourses,
}

/// HUD / menu counters of the session's run.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Status {
    pub lives: u32,
    pub clears: u32,
    pub passed: u32,
    /// Within-100 coins (HUD) and the run's total.
    pub coins: u32,
    pub coins_total: u32,
    pub score: u32,
    pub pending: u32,
    pub attempts: u32,
    pub best: u32,
}

/// One offline Endless session over a local library: the save, the library and the run being
/// played, like [`crate::challenge::Challenge`] for 100 Mario. The record is updated exactly as
/// our server would update it (the same [`RunRecord::report`]), so the save mirrors the server.
///
/// Flow: `new_game(d)` or `continue_run(d)` → `current()` is the course to play → the screen
/// feeds `add_coins` / `one_up` / `add_score` / `mark_miss_demo` / `tick` during play, asks
/// `take_skip_hint()` at the course-in → `report(outcome, frames_left)` gives the next step →
/// `suspend()` on Quit. Write `save_json()` after each call that changes the save.
pub struct Session {
    pub save: EndlessSave,
    pub library: Vec<Candidate>,
    /// The player's maker id (their own courses are never drawn; "" none).
    pub player: String,
    /// Seconds since the epoch for the record's timestamps (the core has no clock).
    pub now: u64,
    rng: Rng,
    difficulty: Option<Difficulty>,
    run: Option<Run>,
    widened: bool,
}

impl Session {
    pub fn new(save: EndlessSave, seed: u64) -> Self {
        Self {
            save,
            library: Vec::new(),
            player: String::new(),
            now: 0,
            rng: Rng::new(seed),
            difficulty: None,
            run: None,
            widened: false,
        }
    }

    pub fn from_json(save: Option<&str>, seed: u64) -> Self {
        let data = save
            .and_then(|s| serde_json::from_str::<Value>(s).ok())
            .map(|v| EndlessSave::from_json(&v))
            .unwrap_or_default();
        Self::new(data, seed)
    }

    pub fn save_json(&self) -> String {
        self.save.to_json().to_string()
    }

    pub fn add_course(&mut self, c: Candidate) {
        self.library.push(c);
    }

    /// Library courses in the difficulty's own band (without widening).
    pub fn available(&self, d: Difficulty) -> usize {
        self.library
            .iter()
            .filter(|c| !c.broken && (self.player.is_empty() || c.owner != self.player))
            .filter(|c| c.band() == Some(d))
            .count()
    }

    pub fn difficulty(&self) -> Option<Difficulty> {
        self.difficulty
    }

    pub fn run(&self) -> Option<&Run> {
        self.run.as_ref()
    }

    pub fn run_mut(&mut self) -> Option<&mut Run> {
        self.run.as_mut()
    }

    pub fn record(&self) -> Option<&RunRecord> {
        self.save.run(self.difficulty?)
    }

    /// The last draw had to leave the band.
    pub fn widened(&self) -> bool {
        self.widened
    }

    /// The course to play now.
    pub fn current(&self) -> Option<&Candidate> {
        let id = self.record()?.current()?;
        self.library.iter().find(|c| c.id == id)
    }

    /// New Game on `d` (wipes its run). Picks the first course.
    pub fn new_game(&mut self, d: Difficulty) -> Result<(), RunError> {
        let now = self.now;
        let r = self.save.new_game(d, now);
        let run = Run::new(d, r.status.lives);
        self.enter(d, run)
    }

    /// Continue the run on `d` (offered while it has lives).
    pub fn continue_run(&mut self, d: Difficulty) -> Result<(), RunError> {
        let r = self.save.run(d).ok_or(RunError::NoRun)?;
        if !r.status.can_continue() {
            return Err(RunError::NotActive);
        }
        let run = Run::resume(d, &r.status);
        self.save.last = Some(d);
        self.enter(d, run)
    }

    fn enter(&mut self, d: Difficulty, mut run: Run) -> Result<(), RunError> {
        run.start_course();
        self.difficulty = Some(d);
        self.run = Some(run);
        self.fill()
    }

    /// Make sure the queue's head is a library course, drawing a batch when it is empty.
    fn fill(&mut self) -> Result<(), RunError> {
        let d = self.difficulty.ok_or(RunError::NoRun)?;
        let lib = &self.library;
        let rec = self.save.runs[d.index()].as_mut().ok_or(RunError::NoRun)?;
        rec.queue.retain(|id| lib.iter().any(|c| c.id == *id));
        if rec.queue.is_empty() {
            let p = pick(lib, d, &rec.excluded(), &self.player, BATCH, &mut self.rng);
            self.widened = p.widened;
            rec.enqueue(p.indices.iter().map(|&i| lib[i].id.clone()), p.widened);
        }
        if rec.queue.is_empty() {
            Err(RunError::NoCourses)
        } else {
            Ok(())
        }
    }

    pub fn add_coins(&mut self, n: u32) -> u32 {
        self.run.as_mut().map_or(0, |r| r.add_coins(n))
    }

    pub fn one_up(&mut self, n: u32) -> u32 {
        self.run.as_mut().map_or(0, |r| r.one_up(n))
    }

    pub fn add_score(&mut self, points: u32) {
        if let Some(r) = self.run.as_mut() {
            r.add_score(points);
        }
    }

    pub fn mark_miss_demo(&mut self) {
        if let Some(r) = self.run.as_mut() {
            r.mark_miss_demo();
        }
    }

    pub fn tick(&mut self, frames: u32) {
        if let Some(r) = self.run.as_mut() {
            r.tick(frames);
        }
    }

    /// Show the skip hint at this course-in? (Once per entry, on the 4th attempt, §2.5.)
    pub fn take_skip_hint(&mut self) -> bool {
        let skips = self.save.skips;
        self.run.as_mut().is_some_and(|r| r.take_skip_hint(skips))
    }

    /// Apply the current course's result. `frames_left`: the timer at the goal (clears only).
    pub fn report(&mut self, outcome: Outcome, frames_left: u32) -> Step {
        let (Some(d), Some(course)) = (self.difficulty, self.current().cloned()) else {
            return Step::OutOfCourses;
        };
        let Some(run) = self.run.as_mut() else {
            return Step::OutOfCourses;
        };
        let (next, report) = run.finish(&course.id, outcome, frames_left);
        let now = self.now;
        let Some(rec) = self.save.run_mut(d) else {
            return Step::OutOfCourses;
        };
        let Ok(applied) = rec.report(&report, &course.style, course.theme, now) else {
            return Step::OutOfCourses;
        };
        // the record is authoritative, like the server's answer
        run.current.lives = applied.lives;
        run.current.clears = applied.clears;
        run.current.passed = applied.passed;
        if outcome == Outcome::Skip {
            self.save.note_skip();
        }
        if outcome == Outcome::Clear {
            self.save.update_best(d, applied.clears);
        }
        if applied.game_over || next == Next::GameOver {
            let f = self.save.finish(d).unwrap_or_default();
            self.run = None;
            return Step::GameOver(f);
        }
        match next {
            Next::Retry => {
                if let Some(r) = self.run.as_mut() {
                    r.retry_course();
                }
                Step::Retry
            }
            _ => {
                if let Some(r) = self.run.as_mut() {
                    r.start_course();
                }
                match self.fill() {
                    Ok(()) => Step::Play,
                    Err(_) => Step::OutOfCourses,
                }
            }
        }
    }

    /// Quit (pause menu or map): the run is kept for Continue, with the lives the misses cost.
    pub fn suspend(&mut self) {
        if let Some(r) = self.run.as_mut() {
            r.suspend();
        }
        if let Some(rec) = self.difficulty.and_then(|d| self.save.run_mut(d)) {
            rec.status.updated_at = self.now;
        }
        self.run = None;
    }

    /// Leave the session (after Game Over or a suspend).
    pub fn end(&mut self) {
        self.run = None;
        self.difficulty = None;
    }

    pub fn status(&self) -> Status {
        let best = self.difficulty.map_or(0, |d| self.save.best[d.index()]);
        let Some(r) = &self.run else {
            return Status {
                best,
                ..Status::default()
            };
        };
        Status {
            lives: r.current.lives,
            clears: r.current.clears,
            passed: r.current.passed,
            coins: r.current.hud_coins(),
            coins_total: r.current.coins,
            score: r.current.score,
            pending: r.pending,
            attempts: r.attempts,
            best,
        }
    }

    /// The run's history ring on `d` (oldest first), for the world map.
    pub fn history(&self, d: Difficulty) -> Vec<HistoryEntry> {
        self.save
            .run(d)
            .map(|r| r.history.iter().cloned().collect())
            .unwrap_or_default()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn difficulty_names_and_bands() {
        for d in Difficulty::ALL {
            assert_eq!(Difficulty::from_name(d.name()), Some(d));
            assert_eq!(Difficulty::from_index(d.index()), Some(d));
            assert_eq!(d.start_lives(), 5);
        }
        assert_eq!(
            Difficulty::ALL.map(Difficulty::name),
            ["easy", "normal", "expert", "super_expert"]
        );
        // clear rate bands, only with ≥ 20 attempts
        assert_eq!(Difficulty::from_stats(19, 19), None);
        assert_eq!(Difficulty::from_stats(7, 20), Some(Difficulty::Easy)); // 35 %
        assert_eq!(Difficulty::from_stats(68, 200), Some(Difficulty::Easy)); // 34 %
        assert_eq!(Difficulty::from_stats(67, 200), Some(Difficulty::Normal));
        assert_eq!(Difficulty::from_stats(12, 100), Some(Difficulty::Normal));
        assert_eq!(Difficulty::from_stats(11, 100), Some(Difficulty::Expert));
        assert_eq!(Difficulty::from_stats(3, 100), Some(Difficulty::Expert));
        assert_eq!(
            Difficulty::from_stats(2, 100),
            Some(Difficulty::SuperExpert)
        );
        assert_eq!(Difficulty::from_stats(0, 50), Some(Difficulty::SuperExpert));
        assert_eq!(Difficulty::from_difficulty_value(50), Difficulty::Easy);
        assert_eq!(Difficulty::from_difficulty_value(80), Difficulty::Normal);
        assert_eq!(Difficulty::from_difficulty_value(95), Difficulty::Expert);
        assert_eq!(
            Difficulty::from_difficulty_value(99),
            Difficulty::SuperExpert
        );
        assert_eq!(
            Difficulty::Normal.neighbours(1),
            vec![Difficulty::Easy, Difficulty::Expert]
        );
        assert_eq!(Difficulty::Easy.neighbours(0), vec![Difficulty::Easy]);
        assert_eq!(
            Difficulty::Easy.neighbours(3),
            vec![Difficulty::SuperExpert]
        );
        assert!(Difficulty::Normal.neighbours(3).is_empty());
    }

    #[test]
    fn time_bonus_is_50_per_whole_second() {
        assert_eq!(time_bonus(0), 0);
        assert_eq!(time_bonus(59), 0, "nothing under one second");
        assert_eq!(time_bonus(60), 50);
        assert_eq!(time_bonus(119), 50);
        assert_eq!(time_bonus(300 * 60), 15_000);
        // the binary's float formula gives the same
        for f in (0..100_000u32).step_by(7) {
            let ms = f as f32 / 60.0 * 1000.0;
            assert_eq!(time_bonus(f), (ms / 1000.0).floor() as u32 * 50, "{f}");
        }
    }

    #[test]
    fn pending_one_ups_cap_at_three_and_coins_roll_over() {
        let mut r = Run::new(Difficulty::Normal, 5);
        r.start_course();
        assert_eq!(r.add_coins(99), 0);
        assert_eq!(r.current.hud_coins(), 99);
        assert_eq!(r.add_coins(1), 1, "the 100th coin");
        assert_eq!((r.current.hud_coins(), r.current.coins), (0, 100));
        assert_eq!(r.add_coins(250), 2);
        assert_eq!(r.pending, 3);
        assert_eq!(r.one_up(1), 0, "a 1-Up that does not fit is lost");
        assert_eq!(r.pending, 3);
        assert_eq!(r.current.lives, 5, "pending is not lives");
        let mut r = Run::new(Difficulty::Normal, 5);
        assert_eq!(r.one_up(3), 3);
        let mut r = Run::new(Difficulty::Normal, 5);
        r.one_up(2);
        assert_eq!(r.one_up(3), 1, "3-Up Moon over 2 pending");
    }

    #[test]
    fn goal_pays_pending_and_the_time_bonus() {
        let mut r = Run::new(Difficulty::Easy, 97);
        r.start_course();
        r.add_score(100);
        r.add_coins(30);
        r.one_up(3);
        let (next, rep) = r.finish("A", Outcome::Clear, 61 * 60 + 30);
        assert_eq!(next, Next::NextCourse);
        assert_eq!(r.current.lives, 99, "capped at 99");
        assert_eq!(r.current.score, 100 + 61 * 50);
        assert_eq!((r.current.clears, r.current.passed), (1, 1));
        assert_eq!(r.pending, 0);
        assert_eq!(
            (rep.pending, rep.coins, rep.score, rep.frames_left),
            (3, 30, 100, 61 * 60 + 30)
        );
        let mut r = Run::new(Difficulty::Easy, 5);
        r.current.score = SCORE_CAP - 10;
        r.finish("A", Outcome::Clear, 6000);
        assert_eq!(r.current.score, SCORE_CAP);
        let mut r = Run::new(Difficulty::Easy, 5);
        r.current.clears = COUNT_CAP;
        r.current.passed = COUNT_CAP;
        r.finish("A", Outcome::Clear, 0);
        assert_eq!((r.current.clears, r.current.passed), (COUNT_CAP, COUNT_CAP));
    }

    #[test]
    fn misses_cost_a_life_and_keep_pending_coins_and_score() {
        for o in [Outcome::Death, Outcome::TimeUp, Outcome::StartOver] {
            let mut r = Run::new(Difficulty::Expert, 2);
            r.start_course();
            r.add_coins(12);
            r.add_score(400);
            r.one_up(2);
            assert_eq!(r.finish("A", o, 0).0, Next::Retry);
            r.retry_course();
            assert_eq!(r.current.lives, 1);
            assert_eq!(r.pending, 2, "the 3-per-course cap holds across retries");
            assert_eq!((r.current.coins, r.current.score), (12, 400));
            r.one_up(5);
            assert_eq!(r.pending, 3);
            assert_eq!(r.finish("A", o, 0).0, Next::GameOver);
            assert_eq!(r.current.lives, 0);
            assert_eq!(r.finish("A", o, 0).0, Next::GameOver, "floor 0");
            assert_eq!(r.current.lives, 0);
        }
    }

    #[test]
    fn skip_is_free_and_rolls_back_to_the_snapshot() {
        let mut r = Run::new(Difficulty::Normal, 5);
        r.current.coins = 40;
        r.current.score = 1000;
        r.current.clears = 2;
        r.current.passed = 3;
        r.start_course();
        r.add_coins(70);
        r.add_score(500);
        r.finish("A", Outcome::Death, 0);
        r.retry_course();
        let (next, rep) = r.finish("A", Outcome::Skip, 0);
        assert_eq!(next, Next::NextCourse);
        assert!(!rep.during_miss);
        assert_eq!(
            r.current.lives, 4,
            "the earlier miss stays, the skip is free"
        );
        assert_eq!((r.current.coins, r.current.score), (40, 1000));
        assert_eq!((r.current.clears, r.current.passed), (2, 4));
        assert_eq!(r.pending, 0);
        // a skip during the miss demo costs the life
        let mut r = Run::new(Difficulty::Normal, 1);
        r.start_course();
        r.mark_miss_demo();
        let (next, rep) = r.finish("A", Outcome::Skip, 0);
        assert!(rep.during_miss);
        assert_eq!((next, r.current.lives), (Next::GameOver, 0));
        // unlimited
        let mut r = Run::new(Difficulty::Normal, 5);
        for i in 0..50 {
            r.start_course();
            assert_eq!(
                r.finish(&i.to_string(), Outcome::Skip, 0).0,
                Next::NextCourse
            );
        }
        assert_eq!((r.current.lives, r.current.passed), (5, 50));
    }

    #[test]
    fn suspend_restores_the_snapshot_with_lives() {
        let mut r = Run::new(Difficulty::Normal, 5);
        r.start_course();
        r.add_coins(150);
        r.finish("A", Outcome::Death, 0);
        r.retry_course();
        r.suspend();
        assert_eq!(r.current, r.snapshot);
        assert_eq!((r.current.lives, r.current.coins, r.pending), (5, 0, 0));
    }

    #[test]
    fn attempts_and_the_skip_hint() {
        let mut r = Run::new(Difficulty::Normal, 10);
        r.start_course();
        for k in 0..5 {
            assert!(!r.take_skip_hint(0) || k == 3);
            r.finish("A", Outcome::Death, 0);
            r.retry_course();
            assert_eq!(r.attempts, (k + 1).min(3));
        }
        // shown once per entry
        let mut r = Run::new(Difficulty::Normal, 10);
        r.attempts = 3;
        assert!(!r.clone().take_skip_hint(3), "not after 3 lifetime skips");
        assert!(r.take_skip_hint(2));
        assert!(!r.take_skip_hint(2), "once");
        r.start_course();
        assert_eq!(r.attempts, 0);
        assert!(offer_rating(1801, true, false));
        assert!(!offer_rating(1800, true, false));
        assert!(!offer_rating(5000, true, true));
        assert!(!offer_rating(5000, false, false));
    }

    #[test]
    fn map_positions() {
        let p = |s| {
            let m = map_position(s);
            (m.node, m.page)
        };
        assert_eq!(p(1), (0, 1));
        assert_eq!(p(16), (15, 1));
        assert_eq!(p(17), (16, 2));
        assert_eq!(p(32), (31, 2));
        assert_eq!(p(33), (0, 3));
        assert_eq!(p(49), (16, 2));
        assert_eq!(p(65), (0, 3));
    }

    fn rep(code: &str, outcome: Outcome) -> Report {
        Report {
            code: code.into(),
            outcome: Some(outcome),
            ..Report::default()
        }
    }

    #[test]
    fn record_applies_reports_like_the_run() {
        let mut rec = RunRecord::new(5, 7, 100);
        rec.enqueue(["a", "b", "c", "a"].map(String::from), false);
        assert_eq!(rec.queue, ["a", "b", "c"]);
        assert_eq!(
            rec.report(&rep("zz", Outcome::Death), "M1", 0, 101),
            Err(RunError::NotInRun)
        );
        let a = rec.report(&rep("a", Outcome::Death), "M1", 0, 101).unwrap();
        assert_eq!((a.lives, a.clears, a.game_over), (4, 0, false));
        assert_eq!(rec.current(), Some("a"), "a death keeps the course");
        let clear = Report {
            pending: 9,
            coins: 120,
            score: 300,
            frames_left: 600,
            ..rep("a", Outcome::Clear)
        };
        let a = rec.report(&clear, "M3", 2, 102).unwrap();
        assert_eq!(
            (a.lives, a.clears, a.passed),
            (7, 1, 1),
            "pending capped at 3"
        );
        assert_eq!((rec.status.coins, rec.status.score), (120, 300 + 500));
        assert_eq!(rec.current(), Some("b"));
        let a = rec.report(&rep("b", Outcome::Skip), "M1", 0, 103).unwrap();
        assert_eq!((a.lives, a.clears, a.passed), (7, 1, 2));
        let b = Report {
            during_miss: true,
            ..rep("c", Outcome::Skip)
        };
        assert_eq!(rec.report(&b, "M1", 0, 104).unwrap().lives, 6);
        assert_eq!(rec.played, ["a", "b", "c"]);
        assert!(rec.queue.is_empty());
        // skips stay off the map (#83)
        let h: Vec<_> = rec
            .history
            .iter()
            .map(|e| (e.code.as_str(), e.result, e.seq))
            .collect();
        assert_eq!(h, [("a", HistoryResult::Clear, 1)]);
        assert_eq!(rec.history[0].style, "M3");
        assert_eq!(rec.status.updated_at, 104);
        // broken: passed over, nothing counted
        rec.enqueue(["d".to_string()], false);
        let a = rec
            .report(&rep("d", Outcome::Broken), "M1", 0, 105)
            .unwrap();
        assert_eq!((a.lives, a.passed, rec.history.len()), (6, 3, 1));
        // game over
        rec.enqueue(["e".to_string()], true);
        assert!(rec.status.widened);
        for _ in 0..5 {
            rec.report(&rep("e", Outcome::Death), "M1", 0, 106).unwrap();
        }
        let a = rec.report(&rep("e", Outcome::Death), "M1", 0, 107).unwrap();
        assert!(a.game_over);
        assert!(!rec.status.active);
        assert_eq!(
            rec.report(&rep("e", Outcome::Death), "M1", 0, 108),
            Err(RunError::NotActive)
        );
    }

    #[test]
    fn old_save_drops_skips_from_the_map() {
        // before v0.7.12 skips were in the ring, numbered by courses passed (#83)
        let mut rec = RunRecord::new(5, 0, 0);
        for (id, o) in [
            ("a", Outcome::Clear),
            ("b", Outcome::Clear),
            ("c", Outcome::Skip),
            ("d", Outcome::Skip),
            ("e", Outcome::Clear),
        ] {
            rec.enqueue([id.to_string()], false);
            rec.report(&rep(id, o), "M1", 0, 0).unwrap();
        }
        let mut v = rec.to_json();
        v["history"] = json!([
            {"code": "a", "result": "clear", "seq": 1, "style": "M1", "theme": 0},
            {"code": "b", "result": "clear", "seq": 2, "style": "M1", "theme": 0},
            {"code": "c", "result": "skip", "seq": 3, "style": "M1", "theme": 0},
            {"code": "d", "result": "skip", "seq": 4, "style": "M1", "theme": 0},
            {"code": "e", "result": "clear", "seq": 5, "style": "M1", "theme": 0},
        ]);
        let back = RunRecord::from_json(&v);
        let h: Vec<_> = back
            .history
            .iter()
            .map(|e| (e.code.as_str(), e.seq))
            .collect();
        assert_eq!(h, [("a", 1), ("b", 2), ("e", 3)]);
        assert_eq!((back.status.clears, back.status.passed), (3, 5));
        assert_eq!(RunRecord::from_json(&rec.to_json()).history, rec.history);
    }

    #[test]
    fn history_ring_keeps_48() {
        let mut rec = RunRecord::new(5, 0, 0);
        for i in 0..60 {
            let id = format!("c{i}");
            rec.enqueue([id.clone()], false);
            rec.report(&rep(&id, Outcome::Clear), "M1", 0, 0).unwrap();
        }
        assert_eq!(rec.history.len(), HISTORY);
        assert_eq!(rec.history[0].seq, 13);
        assert_eq!(rec.history[47].seq, 60);
        assert_eq!(rec.played.len(), 60);
    }

    #[test]
    fn save_round_trips_and_finishes() {
        let mut s = EndlessSave::default();
        let r = s.new_game(Difficulty::Expert, 50);
        r.enqueue(["x".to_string(), "y".to_string()], false);
        r.report(&rep("x", Outcome::Clear), "WU", 3, 60).unwrap();
        s.update_best(Difficulty::Expert, 1);
        s.note_skip();
        s.note_skip();
        s.note_skip();
        s.note_skip();
        assert_eq!(s.skips, 3, "capped");
        assert_eq!(s.last, Some(Difficulty::Expert));
        assert!(s.can_continue(Difficulty::Expert));
        assert!(!s.can_continue(Difficulty::Easy));
        let back = EndlessSave::from_json(&s.to_json());
        assert_eq!(back, s);
        let f = s.finish(Difficulty::Expert).unwrap();
        assert_eq!(
            f,
            Finished {
                clears: 1,
                best: 1,
                new_high_score: true
            }
        );
        assert!(s.run(Difficulty::Expert).is_none());
        let r = s.new_game(Difficulty::Expert, 70);
        assert_eq!((r.status.best_before, r.status.lives), (1, 5));
        assert_eq!(Report::from_json(&clear_report().to_json()), clear_report());
    }

    fn clear_report() -> Report {
        Report {
            code: "ABC-DEF-GHJ".into(),
            outcome: Some(Outcome::Clear),
            pending: 2,
            coins: 7,
            score: 1200,
            frames_left: 3600,
            during_miss: false,
        }
    }

    fn cand(id: &str, clears: u64, attempts: u64) -> Candidate {
        Candidate {
            id: id.into(),
            owner: "maker".into(),
            style: "M1".into(),
            clears,
            attempts,
            ..Candidate::default()
        }
    }

    #[test]
    fn pick_draws_from_the_band_without_repeats_or_own_courses() {
        let mut cands = Vec::new();
        for i in 0..30 {
            cands.push(cand(&format!("easy{i}"), 50, 100));
        }
        for i in 0..5 {
            cands.push(cand(&format!("norm{i}"), 20, 100));
        }
        cands.push(cand("fresh", 0, 3)); // unrated
        let mut mine = cand("mine", 1, 100);
        mine.owner = "me".into();
        cands.push(mine);
        let mut bad = cand("bad", 1, 100);
        bad.broken = true;
        cands.push(bad);
        let mut rng = Rng::new(5);
        let p = pick(
            &cands,
            Difficulty::Easy,
            &HashSet::new(),
            "me",
            BATCH,
            &mut rng,
        );
        assert_eq!(p.indices.len(), 10);
        assert!(!p.widened);
        assert!(p.indices.iter().all(|&i| cands[i].id.starts_with("easy")));
        let uniq: HashSet<_> = p.indices.iter().collect();
        assert_eq!(uniq.len(), 10);
        // the run's played ids are not drawn again
        let excluded: HashSet<&str> = p.indices.iter().map(|&i| cands[i].id.as_str()).collect();
        let q = pick(&cands, Difficulty::Easy, &excluded, "me", BATCH, &mut rng);
        assert!(q.indices.iter().all(|i| !p.indices.contains(i)));
        // Expert has nothing of its own except "mine" (own) and "bad" (broken): widen to Normal
        let e = pick(
            &cands,
            Difficulty::Expert,
            &HashSet::new(),
            "me",
            BATCH,
            &mut rng,
        );
        assert!(e.widened);
        let ids: HashSet<_> = e.indices.iter().map(|&i| cands[i].id.as_str()).collect();
        assert_eq!(ids.len(), 5);
        assert!(ids.iter().all(|id| id.starts_with("norm")), "{ids:?}");
        // Normal has 5: a short batch, not widened
        let n = pick(
            &cands,
            Difficulty::Normal,
            &HashSet::new(),
            "me",
            BATCH,
            &mut rng,
        );
        assert_eq!((n.indices.len(), n.widened), (5, false));
        // with every rated course used, unrated ones are last
        let all: HashSet<&str> = cands
            .iter()
            .filter(|c| c.id != "fresh")
            .map(|c| c.id.as_str())
            .collect();
        let u = pick(&cands, Difficulty::SuperExpert, &all, "me", BATCH, &mut rng);
        assert_eq!(u.indices, vec![cands.len() - 3]);
        assert!(u.widened);
        // an empty player id excludes nothing by owner
        let mut only_own = vec![cand("x", 1, 100)];
        only_own[0].owner = String::new();
        let o = pick(
            &only_own,
            Difficulty::SuperExpert,
            &HashSet::new(),
            "",
            10,
            &mut rng,
        );
        assert_eq!(o.indices, vec![0]);
    }

    #[test]
    fn pick_falls_back_to_the_hint_offline() {
        let mut c = cand("local", 0, 0);
        c.hint = Some(Difficulty::from_difficulty_value(90)); // Expert
        let cands = vec![c, cand("local2", 0, 0)];
        let p = pick(
            &cands,
            Difficulty::Expert,
            &HashSet::new(),
            "",
            10,
            &mut Rng::new(1),
        );
        assert_eq!((p.indices, p.widened), (vec![0], false));
        // statistics win over the hint once rated
        let mut c = cand("rated", 50, 100);
        c.hint = Some(Difficulty::SuperExpert);
        assert_eq!(c.band(), Some(Difficulty::Easy));
    }

    fn library(n: usize, clears: u64) -> Session {
        let mut s = Session::new(EndlessSave::default(), 3);
        for i in 0..n {
            s.add_course(cand(&format!("c{i}"), clears, 100));
        }
        s
    }

    #[test]
    fn offline_session_plays_a_run() {
        let mut s = library(25, 50);
        s.now = 1000;
        assert_eq!(s.available(Difficulty::Easy), 25);
        assert!(!s.save.can_continue(Difficulty::Easy));
        s.new_game(Difficulty::Easy).unwrap();
        assert_eq!(s.status().lives, 5);
        assert_eq!(s.record().unwrap().queue.len(), BATCH);
        let first = s.current().unwrap().id.clone();
        // a miss: same course, one life less
        s.one_up(2);
        assert_eq!(s.report(Outcome::Death, 0), Step::Retry);
        assert_eq!(s.current().unwrap().id, first);
        assert_eq!(
            (s.status().lives, s.status().pending, s.status().attempts),
            (4, 2, 1)
        );
        // clear: 2 pending → 6 lives
        s.add_coins(20);
        assert_eq!(s.report(Outcome::Clear, 600), Step::Play);
        assert_ne!(s.current().unwrap().id, first);
        let st = s.status();
        assert_eq!(
            (st.lives, st.clears, st.passed, st.coins, st.score),
            (6, 1, 1, 20, 500)
        );
        assert_eq!(st.best, 1);
        // skip
        s.add_coins(5);
        assert_eq!(s.report(Outcome::Skip, 0), Step::Play);
        assert_eq!(s.save.skips, 1);
        assert_eq!((s.status().coins, s.status().passed), (20, 2));
        let h = s.history(Difficulty::Easy);
        assert_eq!(
            h.iter()
                .map(|e| (e.code.clone(), e.result))
                .collect::<Vec<_>>(),
            vec![(first.clone(), HistoryResult::Clear)]
        );
        // die on the third course, then quit: lives lost stay lost in the record
        let third = s.current().unwrap().id.clone();
        s.report(Outcome::Death, 0);
        s.suspend();
        let json = s.save_json();
        let mut back = Session::from_json(Some(&json), 9);
        back.library = s.library.clone();
        assert!(back.save.can_continue(Difficulty::Easy));
        back.continue_run(Difficulty::Easy).unwrap();
        assert_eq!(
            back.current().unwrap().id,
            third,
            "Continue replays the quit course"
        );
        let st = back.status();
        assert_eq!((st.lives, st.clears, st.coins, st.score), (5, 1, 20, 500));
        // play to the end of the library: no repeats, then out of courses
        let mut seen: HashSet<String> = [first].into();
        let mut step = Step::Play;
        while step == Step::Play {
            assert!(
                seen.insert(back.current().unwrap().id.clone()),
                "no repeats"
            );
            step = back.report(Outcome::Clear, 0);
        }
        assert_eq!(step, Step::OutOfCourses);
        assert_eq!(back.record().unwrap().played.len(), 25);
    }

    #[test]
    fn offline_game_over_finishes_the_run() {
        let mut s = library(12, 1); // 1 % clear rate: Super Expert
        s.new_game(Difficulty::Easy).unwrap();
        assert!(s.widened(), "Easy has nothing: widened");
        assert!(s.record().unwrap().status.widened);
        s.report(Outcome::Clear, 0);
        for _ in 0..4 {
            assert_eq!(s.report(Outcome::StartOver, 0), Step::Retry);
        }
        assert!(s.take_skip_hint(), "4th attempt");
        assert!(!s.take_skip_hint());
        let step = s.report(Outcome::TimeUp, 0);
        assert_eq!(
            step,
            Step::GameOver(Finished {
                clears: 1,
                best: 1,
                new_high_score: true
            })
        );
        assert!(s.save.run(Difficulty::Easy).is_none());
        assert_eq!(s.save.best[0], 1);
        assert_eq!(s.continue_run(Difficulty::Easy), Err(RunError::NoRun));
        // an empty library
        let mut e = Session::new(EndlessSave::default(), 1);
        assert_eq!(e.new_game(Difficulty::Normal), Err(RunError::NoCourses));
    }

    #[test]
    fn own_courses_are_never_drawn_offline() {
        let mut s = library(3, 50);
        s.player = "maker".into(); // owns all three
        s.add_course(Candidate {
            owner: "someone".into(),
            ..cand("theirs", 50, 100)
        });
        s.new_game(Difficulty::Easy).unwrap();
        assert_eq!(s.record().unwrap().queue, ["theirs"]);
    }

    #[test]
    fn the_two_rulesets_keep_their_own_slots_and_old_saves_are_sorted_in() {
        let mut save = EndlessSave::default();
        save.new_game_with(Difficulty::Easy, Ruleset::Smm2, 1);
        save.new_game_with(Difficulty::Easy, Ruleset::Hundred, 2);
        assert_eq!(save.run(Difficulty::Easy).unwrap().status.lives, 5);
        assert_eq!(
            save.run_of(Ruleset::Hundred, Difficulty::Easy)
                .unwrap()
                .status
                .lives,
            100
        );
        let back = EndlessSave::from_json(&save.to_json());
        assert_eq!(back, save);
        // a file from before the slots: a 100 Mario run under "runs"
        let mut old = save.to_json();
        old["runs"]["easy"] = old["hundred_runs"]["easy"].take();
        let back = EndlessSave::from_json(&old);
        assert!(back.run(Difficulty::Easy).is_none());
        assert_eq!(back.slot(Difficulty::Easy, None), Ruleset::Hundred);
        assert_eq!(
            back.run_of(Ruleset::Hundred, Difficulty::Easy)
                .unwrap()
                .status
                .lives,
            100
        );
    }

    #[test]
    fn a_hundred_mario_run_has_100_lives_free_skips_and_an_end() {
        let mut save = EndlessSave::default();
        let rec = save.new_game_with(Difficulty::Easy, Ruleset::Hundred, 10);
        assert_eq!(
            (rec.status.lives, rec.status.goal, rec.status.ruleset),
            (100, 8, Ruleset::Hundred)
        );
        let ids: Vec<String> = (0..12).map(|i| format!("c{i}")).collect();
        rec.enqueue(ids.iter().cloned(), false);
        rec.report(&rep("c0", Outcome::Death), "M1", 0, 11).unwrap();
        let skip = Report {
            during_miss: true,
            ..rep("c0", Outcome::Skip)
        };
        let a = rec.report(&skip, "M1", 0, 12).unwrap();
        assert_eq!((a.lives, a.passed), (99, 1), "SMM1's skips are free");
        for (k, id) in ids[1..9].iter().enumerate() {
            let c = Report {
                pending: 3,
                ..rep(id, Outcome::Clear)
            };
            let a = rec.report(&c, "M1", 0, 13).unwrap();
            assert_eq!(a.complete, k == 7);
            assert!(a.lives <= 100, "capped at 100");
        }
        assert!(rec.status.complete && !rec.status.can_continue());
        let back = RunStatus::from_json(&rec.status.to_json());
        assert_eq!(back, rec.status);
        // not an Endless score
        let f = save.finish_of(Ruleset::Hundred, Difficulty::Easy).unwrap();
        assert_eq!((f.clears, save.best[0]), (8, 0));
        // SMM2's rules are unchanged
        assert_eq!(Ruleset::Smm2.goal(Difficulty::SuperExpert), 0);
        assert_eq!(Ruleset::Hundred.goal(Difficulty::SuperExpert), 6);
    }

    #[test]
    fn hundred_draws_by_smm1_fail_rate_bands() {
        // fail rates: 10 (easy band), 50 (normal), 85 (expert), unrated
        let cands = vec![
            cand("easy", 90, 100),
            cand("normal", 50, 100),
            cand("expert", 15, 100),
            cand("new", 0, 3),
        ];
        let draw = |d| {
            let p = pick_hundred(&cands, d, &HashSet::new(), "", 10, &mut Rng::new(1));
            let mut v: Vec<&str> = p.indices.iter().map(|&i| cands[i].id.as_str()).collect();
            v.sort();
            (v, p.widened)
        };
        assert_eq!(draw(Difficulty::Easy), (vec!["easy"], false));
        assert_eq!(draw(Difficulty::Normal), (vec!["easy", "normal"], false));
        assert_eq!(draw(Difficulty::Expert), (vec!["expert"], false));
        // nothing at 96–100 %: any rated course
        assert_eq!(
            draw(Difficulty::SuperExpert),
            (vec!["easy", "expert", "normal"], true)
        );
    }
}
