//! Endless Challenge runs, held by the server as SMM2's server holds them (spec
//! `endless-challenge.md` §3.3, §3.4): lives, clears, the course order and the history live here;
//! the client keeps only the in-course counters (`mmk_modes::endless::Run`) and reports each
//! result. The rules are `mmk_modes::endless` ([`RunRecord::report`] and [`pick`]), shared with
//! the offline session.
//!
//! - Runs are per (account maker ID, difficulty), kept in `endless.json`
//!   (`{"players": {maker_id: EndlessSave}}`). Best clears stay in the leaderboard map of
//!   `social.json` that `POST /api/endless/<d>` and `GET /api/rankings/endless_<d>` already use;
//!   every clear raises it, so a suspended run counts too.
//! - Courses are drawn by the clear-rate bands of [`pick`] (only courses with ≥ 20 attempts are
//!   rated; unrated ones come last), never the player's own (`Meta::maker_id`), never one of a
//!   banned account, never twice in a run. A batch of up to 10 is queued in the run record, so
//!   `next` is idempotent and Continue replays the course that was being played.
//! - Endless plays count in the course's normal statistics: the first report of a course in a
//!   run counts a play, each miss or clear an attempt, a clear a clear (first clear and the
//!   account's cleared list too; no record time). A skip counts an attempt only when it ended a
//!   miss; a broken course counts nothing.

use std::collections::{HashMap, HashSet, VecDeque};
use std::fs;
use std::path::Path;

use mmk_modes::endless::{
    Applied, BATCH, Candidate, Difficulty, EndlessSave, Finished, HistoryEntry, Outcome, Report,
    Ruleset, RunError, RunStatus, pick, pick_fresh, pick_hundred,
};
use serde_json::{Map, Value, json};

use crate::{Inner, Meta, Store, StoreError, now};

/// Runs per maker ID, from `endless.json`.
pub(crate) fn load(dir: &Path) -> HashMap<String, EndlessSave> {
    fs::read(dir.join("endless.json"))
        .ok()
        .and_then(|b| serde_json::from_slice::<Value>(&b).ok())
        .and_then(|v| v["players"].as_object().cloned())
        .map(|o| {
            o.iter()
                .map(|(who, s)| (who.clone(), EndlessSave::from_json(s)))
                .collect()
        })
        .unwrap_or_default()
}

/// The courses a player passed in their challenge runs, newest first (a played list for a store
/// kept before played lists were: seeded from the runs it holds).
pub(crate) fn played_in_runs(save: &EndlessSave) -> Vec<String> {
    let mut runs: Vec<&mmk_modes::endless::RunRecord> =
        save.runs.iter().chain(&save.hundred).flatten().collect();
    runs.sort_by_key(|r| r.status.updated_at);
    let mut out: Vec<String> = Vec::new();
    for code in runs.iter().rev().flat_map(|r| r.played.iter().rev()) {
        if !out.contains(code) && out.len() < crate::PLAYED_KEPT {
            out.push(code.clone());
        }
    }
    out
}

fn run_err(e: RunError) -> StoreError {
    match e {
        RunError::NoRun => StoreError::NotFound,
        RunError::NotActive => StoreError::Bad("the run is over".into()),
        RunError::NotInRun => StoreError::Bad("that course is not in this run".into()),
        RunError::NoCourses => StoreError::NotFound,
    }
}

/// A report's answer: the record's {lives, clears} (§3.1) and the best on that difficulty.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Answer {
    pub applied: Applied,
    pub best: u64,
}

/// A drawn batch: the courses to play in order, whether the draw left the band, the run.
#[derive(Clone, Debug, PartialEq)]
pub struct Batch {
    pub courses: Vec<Meta>,
    pub widened: bool,
    pub run: RunStatus,
}

/// The leaderboard of a ruleset on `d`: `easy` … (Endless best clears) or `hundred_easy` …
/// (completed 100 Mario runs).
pub(crate) fn board(ruleset: Ruleset, d: Difficulty) -> String {
    match ruleset {
        Ruleset::Smm2 => d.name().to_string(),
        Ruleset::Hundred => format!("hundred_{}", d.name()),
    }
}

impl Inner {
    fn best(&self, d: Difficulty, who: &str) -> u64 {
        self.board_score(&board(Ruleset::Smm2, d), who)
    }

    fn board_score(&self, board: &str, who: &str) -> u64 {
        self.endless
            .get(board)
            .and_then(|m| m.get(who))
            .copied()
            .unwrap_or(0)
    }

    /// One more completed 100 Mario run on `d`; the new count.
    fn add_completion(&mut self, d: Difficulty, who: &str) -> u64 {
        let b = self
            .endless
            .entry(board(Ruleset::Hundred, d))
            .or_default()
            .entry(who.to_string())
            .or_insert(0);
        *b += 1;
        *b
    }

    fn raise_best(&mut self, d: Difficulty, who: &str, clears: u64) -> u64 {
        let b = self
            .endless
            .entry(d.name().to_string())
            .or_default()
            .entry(who.to_string())
            .or_insert(0);
        *b = (*b).max(clears);
        *b
    }

    fn next_seed(&mut self) -> u64 {
        self.rng ^= self.rng << 13;
        self.rng ^= self.rng >> 7;
        self.rng ^= self.rng << 17;
        self.rng
    }
}

impl Store {
    fn save_endless(&self, g: &Inner) -> Result<(), StoreError> {
        let players: Map<String, Value> = g
            .endless_runs
            .iter()
            .map(|(who, s)| (who.clone(), s.to_json()))
            .collect();
        self.write_json("endless.json", &json!({ "players": players }))
    }

    /// New Game on `d` for `who`: wipes that difficulty's run and starts one under `ruleset`
    /// with its starting lives (SMM2: 5 [ext/inf]; 100 Mario: 100).
    pub fn endless_start(
        &self,
        who: &str,
        d: Difficulty,
        ruleset: Ruleset,
    ) -> Result<RunStatus, StoreError> {
        let mut g = self.inner.lock().unwrap();
        let best = g.board_score(&board(ruleset, d), who).min(u32::MAX as u64) as u32;
        let save = g.endless_runs.entry(who.to_string()).or_default();
        let rec = save.new_game_with(d, ruleset, now());
        rec.status.best_before = best;
        let out = rec.status;
        self.save_endless(&g)?;
        drop(g);
        self.short_of(d); // (an upstream source tops the difficulty up while the run starts)
        Ok(out)
    }

    /// The four runs, the best clears per difficulty, the lifetime skip count (0–3) and the last
    /// difficulty, as JSON.
    pub fn endless_status(&self, who: &str) -> Value {
        let g = self.inner.lock().unwrap();
        let save = g.endless_runs.get(who).cloned().unwrap_or_default();
        let slots = |ruleset: Ruleset| -> Map<String, Value> {
            Difficulty::ALL
                .iter()
                .map(|&d| {
                    let v = save
                        .run_of(ruleset, d)
                        .map_or(Value::Null, |r| r.status.to_json());
                    (d.name().to_string(), v)
                })
                .collect()
        };
        let runs = slots(Ruleset::Smm2);
        let hundred_runs = slots(Ruleset::Hundred);
        let best: Map<String, Value> = Difficulty::ALL
            .iter()
            .map(|&d| (d.name().to_string(), json!(g.best(d, who))))
            .collect();
        json!({ "runs": runs, "hundred_runs": hundred_runs, "best": best, "skips": save.skips,
                "last": save.last.map(Difficulty::name) })
    }

    /// The next courses of the run (up to `count`, at most 10): the run's queue, refilled by the
    /// pick rule when it is empty.
    pub fn endless_next(
        &self,
        who: &str,
        d: Difficulty,
        want: Option<Ruleset>,
        count: usize,
    ) -> Result<Batch, StoreError> {
        let mut g = self.inner.lock().unwrap();
        let seed = g.next_seed();
        let Inner {
            courses,
            by_code,
            accounts,
            endless_runs,
            cleared,
            ..
        } = &mut *g;
        let save = endless_runs.get_mut(who).ok_or(StoreError::NotFound)?;
        save.last = Some(d);
        // courses to avoid, the first let back in first: the ones the player cleared anywhere (Course World, Played
        // Levels, an earlier run) that weren't dealt lately, then the ones dealt in earlier runs, oldest first (#83)
        let mine: &[String] = cleared.get(who).map(Vec::as_slice).unwrap_or_default();
        let mut recent: VecDeque<String> = mine
            .iter()
            .filter(|c| !save.recent.contains(c))
            .cloned()
            .collect();
        recent.extend(save.recent.iter().cloned());
        let slot = save.slot(d, want);
        let rec = save.run_of_mut(slot, d).ok_or(StoreError::NotFound)?;
        if !rec.status.can_continue() {
            return Err(run_err(RunError::NotActive));
        }
        rec.queue.retain(|c| by_code.contains_key(c));
        let mut widened = false;
        let mut dealt: Vec<String> = Vec::new();
        let refilled = rec.queue.is_empty();
        if refilled {
            let banned: HashSet<u32> = accounts
                .list
                .iter()
                .filter(|a| a.banned)
                .map(|a| a.id)
                .collect();
            let cands: Vec<Candidate> = courses
                .iter()
                .map(|m| Candidate {
                    id: m.code.clone(),
                    name: m.name.clone(),
                    maker: m.maker.clone(),
                    owner: m.maker_id.clone(),
                    style: m.style.clone(),
                    theme: m.theme,
                    attempts: m.attempts,
                    clears: m.clears,
                    hint: None,
                    broken: m.owner != 0 && banned.contains(&m.owner),
                })
                .collect();
            let rule: mmk_modes::endless::PickRule = match rec.status.ruleset {
                Ruleset::Smm2 => pick,
                Ruleset::Hundred => pick_hundred,
            };
            // (courses the player cleared or was dealt in earlier runs come back only when no fresh one is left)
            let p = pick_fresh(
                rule,
                &cands,
                d,
                &rec.excluded(),
                &recent,
                who,
                BATCH,
                &mut mmk_modes::Rng::new(seed),
            );
            widened = p.widened;
            dealt = p.indices.iter().map(|&i| cands[i].id.clone()).collect();
            rec.enqueue(dealt.iter().cloned(), p.widened);
        }
        // 100 Mario ends at the castle, as SMM1's does (owner, 2026-10-25): the courses that can end the run - from the
        // one the run's last clear would come on if every course before it were cleared, to the end of the batch, so
        // skips still end on one - are castle-theme ones where the pool has them (the queue's first course is the
        // one being played). Only as the queue is refilled, before any of its courses has been handed out: the game
        // keeps the list it was given (up to 10), so a course already handed out never changes under it (v0.5.4
        // changed one, and the game's next result came back "that course is not in this run").
        if refilled && rec.status.ruleset == Ruleset::Hundred {
            const CASTLE: u8 = 2; // (SMM2's theme numbers: 0 ground, 1 underground, 2 castle, ...)
            let last = mmk_modes::hundred::Difficulty::ALL[d.index()].run_length();
            let from = last.saturating_sub(1 + rec.status.clears as usize);
            let castle = |code: &str| {
                by_code
                    .get(code)
                    .is_some_and(|&i| courses[i].theme == CASTLE)
            };
            let mut taken: HashSet<String> = rec.excluded().iter().map(|s| s.to_string()).collect();
            let banned: HashSet<u32> = accounts
                .list
                .iter()
                .filter(|a| a.banned)
                .map(|a| a.id)
                .collect();
            let mut rng = mmk_modes::Rng::new(seed ^ 0xca57);
            for pos in from..rec.queue.len() {
                if castle(&rec.queue[pos]) {
                    continue;
                }
                let castles: Vec<Candidate> = courses
                    .iter()
                    .filter(|m| m.theme == CASTLE && !taken.contains(&m.code))
                    .map(|m| Candidate {
                        id: m.code.clone(),
                        name: m.name.clone(),
                        maker: m.maker.clone(),
                        owner: m.maker_id.clone(),
                        style: m.style.clone(),
                        theme: m.theme,
                        attempts: m.attempts,
                        clears: m.clears,
                        hint: None,
                        broken: m.owner != 0 && banned.contains(&m.owner),
                    })
                    .collect();
                // (one the player hasn't cleared, if there is one)
                let not_mine: HashSet<&str> = mine.iter().map(String::as_str).collect();
                let mut p = pick_hundred(&castles, d, &not_mine, who, 1, &mut rng);
                if p.indices.is_empty() {
                    p = pick_hundred(&castles, d, &HashSet::new(), who, 1, &mut rng);
                }
                if let Some(&i) = p.indices.first() {
                    taken.insert(castles[i].id.clone());
                    rec.queue[pos] = castles[i].id.clone();
                } else if let Some(j) = (0..from.min(pos)).rev().find(|&j| castle(&rec.queue[j])) {
                    // (no fresh one: a castle course drawn for earlier in this batch moves to the end - nothing of
                    // the batch has been handed out yet)
                    rec.queue.swap(j, pos);
                }
            }
        }
        let list: Vec<Meta> = rec
            .queue
            .iter()
            .take(count.clamp(1, BATCH))
            .filter_map(|c| by_code.get(c).map(|&i| courses[i].clone()))
            .collect();
        let run = rec.status;
        save.note_recent(dealt.iter().map(String::as_str));
        self.save_endless(&g)?;
        drop(g);
        if widened {
            self.short_of(d); // (the bands ran dry: an upstream source fetches more)
        }
        Ok(Batch {
            courses: list,
            widened,
            run,
        })
    }

    /// Apply a course result reported by `who` (see [`Report`]); the course must be queued in
    /// the run. Updates the course's statistics and the best.
    pub fn endless_report(
        &self,
        who: &str,
        d: Difficulty,
        want: Option<Ruleset>,
        r: &Report,
    ) -> Result<Answer, StoreError> {
        let outcome = r.outcome.ok_or_else(|| {
            StoreError::Bad("result: clear, death, time_up, start_over, skip or broken".into())
        })?;
        let mut g = self.inner.lock().unwrap();
        let ix = g.by_code.get(&r.code).copied();
        let (style, theme) = ix.map_or((String::new(), 0), |i| {
            (g.courses[i].style.clone(), g.courses[i].theme)
        });
        let save = g.endless_runs.get_mut(who).ok_or(StoreError::NotFound)?;
        let slot = save.slot(d, want);
        let rec = save.run_of_mut(slot, d).ok_or(StoreError::NotFound)?;
        let first = rec.current() != Some(r.code.as_str()) || rec.course_reports == 0;
        let ruleset = rec.status.ruleset;
        let applied = rec.report(r, &style, theme, now()).map_err(run_err)?;
        if outcome == Outcome::Skip {
            save.note_skip();
        }
        let best = match ruleset {
            Ruleset::Smm2 if outcome == Outcome::Clear || applied.game_over => {
                g.raise_best(d, who, applied.clears as u64)
            }
            Ruleset::Smm2 => g.best(d, who),
            Ruleset::Hundred if applied.complete => g.add_completion(d, who),
            Ruleset::Hundred => g.board_score(&board(ruleset, d), who),
        };
        let played = first && outcome != Outcome::Broken && ix.is_some();
        if played {
            g.note_played(who, &r.code);
        }
        if let Some(i) = ix {
            let m = &mut g.courses[i];
            if first && outcome != Outcome::Broken {
                m.plays += 1;
            }
            let attempt = outcome.is_miss()
                || outcome == Outcome::Clear
                || (outcome == Outcome::Skip && r.during_miss);
            if attempt {
                m.attempts += 1;
            }
            if outcome == Outcome::Clear {
                m.clears += 1;
                if m.first_clear.is_empty() {
                    m.first_clear = who.to_string();
                }
                let mine = g.cleared.entry(who.to_string()).or_default();
                if !mine.contains(&r.code) {
                    mine.push(r.code.clone());
                }
            }
            self.save_index(&g)?;
        }
        if played || outcome == Outcome::Clear || applied.game_over || applied.complete {
            self.save_social(&g)?;
        }
        self.save_endless(&g)?;
        Ok(Answer { applied, best })
    }

    /// Quit to the menu: the run is kept for Continue (the lives already lost stay lost).
    pub fn endless_suspend(
        &self,
        who: &str,
        d: Difficulty,
        want: Option<Ruleset>,
    ) -> Result<RunStatus, StoreError> {
        let mut g = self.inner.lock().unwrap();
        let rec = g
            .endless_runs
            .get_mut(who)
            .and_then(|s| s.run_of_mut(s.slot(d, want), d))
            .ok_or(StoreError::NotFound)?;
        rec.status.updated_at = now();
        let out = rec.status;
        self.save_endless(&g)?;
        Ok(out)
    }

    /// Finish the run (after the game over): the best is updated and the run cleared.
    pub fn endless_finish(
        &self,
        who: &str,
        d: Difficulty,
        want: Option<Ruleset>,
    ) -> Result<Finished, StoreError> {
        let mut g = self.inner.lock().unwrap();
        let save = g.endless_runs.get_mut(who).ok_or(StoreError::NotFound)?;
        let ruleset = save.slot(d, want);
        let f = save.finish_of(ruleset, d).ok_or(StoreError::NotFound)?;
        let best = match ruleset {
            Ruleset::Smm2 => g.raise_best(d, who, f.clears as u64),
            Ruleset::Hundred => g.board_score(&board(ruleset, d), who),
        };
        self.save_social(&g)?;
        self.save_endless(&g)?;
        Ok(Finished {
            best: best.min(u32::MAX as u64) as u32,
            ..f
        })
    }

    /// The run's last 48 courses (oldest first).
    pub fn endless_history(
        &self,
        who: &str,
        d: Difficulty,
        want: Option<Ruleset>,
    ) -> Result<Vec<HistoryEntry>, StoreError> {
        let g = self.inner.lock().unwrap();
        let rec = g
            .endless_runs
            .get(who)
            .and_then(|s| s.run_of(s.slot(d, want), d))
            .ok_or(StoreError::NotFound)?;
        Ok(rec.history.iter().cloned().collect())
    }
}

type Response = (u16, &'static str, Vec<u8>);

fn json_resp(status: u16, v: Value) -> Response {
    (status, "application/json", serde_json::to_vec(&v).unwrap())
}

fn store_err(e: StoreError) -> Response {
    let (st, msg) = match e {
        StoreError::NotFound => (404, "no such run".to_string()),
        StoreError::Forbidden => (403, "forbidden".to_string()),
        StoreError::Bad(m) | StoreError::NotACourse(m) => (400, m),
        StoreError::TooLarge => (413, "too large".to_string()),
        StoreError::Io(m) => (500, m),
    };
    json_resp(st, json!({ "error": msg }))
}

/// The Endless routes (see the table in the crate docs); None for any other request (the older
/// `POST /api/endless/<d>` is handled by the main router). `who`: the signed-in, unbanned
/// account's maker ID.
pub(crate) fn route(
    store: &Store,
    method: &str,
    parts: &[&str],
    q: &HashMap<String, String>,
    who: Option<&str>,
    body: &[u8],
) -> Option<Response> {
    let action = match parts {
        ["api", "endless"] if method == "GET" => None,
        ["api", "endless", _, a] => Some(*a),
        _ => return None,
    };
    let known = matches!(
        (method, action),
        ("GET", None | Some("next" | "history"))
            | ("POST", Some("start" | "result" | "suspend" | "finish"))
    );
    if !known {
        return None;
    }
    let Some(who) = who else {
        return Some(json_resp(401, json!({ "error": "sign in" })));
    };
    let Some(action) = action else {
        return Some(json_resp(200, store.endless_status(who)));
    };
    let Some(d) = Difficulty::from_name(parts[2]) else {
        return Some(json_resp(404, json!({ "error": "no such difficulty" })));
    };
    let name = d.name();
    // the run slot: ?ruleset= (smm2 or hundred), else the one with a run that can go on
    let want = match q.get("ruleset").map(|n| Ruleset::from_name(n)) {
        None => None,
        Some(Some(r)) => Some(r),
        Some(None) => {
            return Some(json_resp(
                400,
                json!({ "error": "ruleset: smm2 or hundred" }),
            ));
        }
    };
    let out = match action {
        "start" => {
            let v = serde_json::from_slice::<Value>(body).unwrap_or(Value::Null);
            let ruleset = match v["ruleset"].as_str() {
                None => Ruleset::Smm2,
                Some(n) => match Ruleset::from_name(n) {
                    Some(r) => r,
                    None => {
                        return Some(json_resp(
                            400,
                            json!({ "error": "ruleset: smm2 or hundred" }),
                        ));
                    }
                },
            };
            store
                .endless_start(who, d, ruleset)
                .map(|s| json_resp(201, json!({ "difficulty": name, "run": s.to_json() })))
        }
        "next" => {
            let count = q.get("count").and_then(|c| c.parse().ok()).unwrap_or(BATCH);
            store.endless_next(who, d, want, count).map(|b| {
                json_resp(
                    200,
                    json!({
                        "difficulty": name,
                        "courses": b.courses.iter().map(Meta::to_json).collect::<Vec<_>>(),
                        "widened": b.widened,
                        "run": b.run.to_json(),
                    }),
                )
            })
        }
        "result" => {
            let v = serde_json::from_slice::<Value>(body).unwrap_or(Value::Null);
            store.endless_report(who, d, want, &Report::from_json(&v)).map(|a| {
                json_resp(
                    200,
                    json!({
                        "difficulty": name, "lives": a.applied.lives, "clears": a.applied.clears,
                        "passed": a.applied.passed, "game_over": a.applied.game_over,
                        "run_complete": a.applied.complete, "best": a.best,
                    }),
                )
            })
        }
        "suspend" => store
            .endless_suspend(who, d, want)
            .map(|s| json_resp(200, json!({ "difficulty": name, "run": s.to_json() }))),
        "finish" => store.endless_finish(who, d, want).map(|f| {
            let mut v = f.to_json();
            v["difficulty"] = json!(name);
            json_resp(200, v)
        }),
        _ => store.endless_history(who, d, want).map(|h| {
            // each course's name and maker too, where the server holds it (a continued 100 Mario run's credits)
            let history: Vec<Value> = h
                .iter()
                .map(|e| {
                    let mut v = e.to_json();
                    if let Some(m) = store.get(&e.code) {
                        v["name"] = json!(m.name);
                        v["maker"] = json!(m.maker);
                    }
                    v
                })
                .collect();
            json_resp(200, json!({ "difficulty": name, "history": history }))
        }),
    };
    Some(out.unwrap_or_else(store_err))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::handle;
    use mmk_formats::smm2::Course;

    #[test]
    fn a_played_list_seeded_from_the_runs_is_newest_first() {
        let run = |at: u64, codes: &[&str]| {
            let mut r = mmk_modes::endless::RunRecord::default();
            r.status.updated_at = at;
            r.played = codes.iter().map(|c| c.to_string()).collect();
            Some(r)
        };
        let mut save = EndlessSave::default();
        save.runs[0] = run(10, &["AAA-AAA-AAA", "BBB-BBB-BBB"]);
        save.hundred[1] = run(20, &["CCC-CCC-CCC", "AAA-AAA-AAA"]);
        assert_eq!(
            played_in_runs(&save),
            ["AAA-AAA-AAA", "CCC-CCC-CCC", "BBB-BBB-BBB"]
        );
    }

    fn tmp(name: &str) -> std::path::PathBuf {
        let d = std::env::temp_dir().join(format!(
            "mmk-server-endless-{name}-{}-{}",
            std::process::id(),
            now()
        ));
        let _ = fs::remove_dir_all(&d);
        d
    }

    fn call(
        store: &Store,
        method: &str,
        url: &str,
        token: Option<&str>,
        body: &str,
    ) -> (u16, Value) {
        let headers: Vec<(String, String)> = token
            .map(|t| vec![("Authorization".into(), format!("Bearer {t}"))])
            .unwrap_or_default();
        let (st, _, b) = handle(store, method, url, &headers, body.as_bytes());
        (st, serde_json::from_slice(&b).unwrap_or(Value::Null))
    }

    fn account(store: &Store, name: &str) -> (String, String) {
        let (st, v) = call(
            store,
            "POST",
            "/api/accounts",
            None,
            &format!(r#"{{"name":"{name}"}}"#),
        );
        assert_eq!(st, 201);
        (
            v["token"].as_str().unwrap().to_string(),
            v["account"]["maker_id"].as_str().unwrap().to_string(),
        )
    }

    /// Upload a blank course as `token` with these statistics.
    fn course(store: &Store, token: &str, clears: u64, attempts: u64) -> String {
        let a = store.account(token).unwrap();
        let data = Course::blank("M1").to_bytes().unwrap();
        let m = store.upload_as(&data, "", Some(&a)).unwrap();
        let mut g = store.inner.lock().unwrap();
        let i = g.by_code[&m.code];
        g.courses[i].clears = clears;
        g.courses[i].attempts = attempts;
        m.code
    }

    fn codes(v: &Value) -> Vec<String> {
        v["courses"]
            .as_array()
            .unwrap()
            .iter()
            .map(|c| c["code"].as_str().unwrap().to_string())
            .collect()
    }

    fn report(code: &str, result: &str, extra: &str) -> String {
        format!(r#"{{"code":"{code}","result":"{result}"{extra}}}"#)
    }

    #[test]
    fn both_modes_keep_their_own_run() {
        let dir = tmp("slots");
        let store = Store::open(&dir).unwrap();
        let (me, _) = account(&store, "Pia");
        let (maker, _) = account(&store, "Mo");
        for _ in 0..12 {
            course(&store, &maker, 80, 100);
        }
        let me = Some(me.as_str());
        let start = |body: &str| call(&store, "POST", "/api/endless/easy/start", me, body);
        assert_eq!(start(r#"{"ruleset":"smm2"}"#).0, 201);
        assert_eq!(
            start(r#"{"ruleset":"hundred"}"#).0,
            201,
            "doesn't end the SMM2 run"
        );
        let (_, s) = call(&store, "GET", "/api/endless", me, "");
        assert_eq!(s["runs"]["easy"]["lives"].as_u64(), Some(5));
        assert_eq!(s["hundred_runs"]["easy"]["lives"].as_u64(), Some(100));
        // each slot plays its own queue, by ?ruleset=
        let (st, b) = call(&store, "GET", "/api/endless/easy/next?ruleset=smm2", me, "");
        assert_eq!((st, b["run"]["ruleset"].as_str()), (200, Some("smm2")));
        let first = codes(&b)[0].clone();
        let (_, a) = call(
            &store,
            "POST",
            "/api/endless/easy/result?ruleset=smm2",
            me,
            &report(&first, "death", ""),
        );
        assert_eq!(a["lives"].as_u64(), Some(4));
        let (_, b) = call(
            &store,
            "GET",
            "/api/endless/easy/next?ruleset=hundred",
            me,
            "",
        );
        assert_eq!(
            b["run"]["lives"].as_u64(),
            Some(100),
            "the 100 Mario run kept its lives"
        );
        assert_eq!(
            call(&store, "GET", "/api/endless/easy/next?ruleset=odd", me, "").0,
            400
        );
        // finishing the 100 Mario run leaves the SMM2 one, and the file keeps both (reopened)
        call(
            &store,
            "POST",
            "/api/endless/easy/finish?ruleset=hundred",
            me,
            "",
        );
        drop(store);
        let store = Store::open(&dir).unwrap();
        let (_, s) = call(&store, "GET", "/api/endless", me, "");
        assert_eq!(s["runs"]["easy"]["lives"].as_u64(), Some(4));
        assert!(s["hundred_runs"]["easy"].is_null());
        // without ?ruleset= the slot with a run that can go on
        let (st, b) = call(&store, "GET", "/api/endless/easy/next", me, "");
        assert_eq!((st, b["run"]["ruleset"].as_str()), (200, Some("smm2")));
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn endless_avoids_courses_cleared_elsewhere() {
        let dir = tmp("cleared");
        let store = Store::open(&dir).unwrap();
        let (me, who) = account(&store, "Pia");
        let (maker, _) = account(&store, "Mo");
        let all: Vec<String> = (0..30).map(|_| course(&store, &maker, 80, 100)).collect();
        // 25 of the 30 cleared in Course World: a new run gets the other 5 first
        for c in &all[..25] {
            store.clear(c, Some(&who), 0).unwrap();
        }
        let me = Some(me.as_str());
        let (st, _) = call(&store, "POST", "/api/endless/easy/start", me, "");
        assert_eq!(st, 201);
        let (_, b) = call(&store, "GET", "/api/endless/easy/next?count=10", me, "");
        let batch = codes(&b);
        assert!(batch.len() >= 5, "{batch:?}");
        assert!(
            batch[..5].iter().all(|c| all[25..].contains(c)),
            "{batch:?}"
        );
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn two_runs_on_a_30_course_pool_share_no_courses() {
        let dir = tmp("fresh");
        let store = Store::open(&dir).unwrap();
        let (me, _) = account(&store, "Pia");
        let (maker, _) = account(&store, "Mo");
        for _ in 0..30 {
            course(&store, &maker, 80, 100);
        }
        let me = Some(me.as_str());
        let mut dealt: Vec<Vec<String>> = Vec::new();
        for _ in 0..2 {
            let (st, _) = call(
                &store,
                "POST",
                "/api/endless/easy/start",
                me,
                r#"{"ruleset":"hundred"}"#,
            );
            assert_eq!(st, 201);
            let mut run = Vec::new();
            while run.len() < 8 {
                let (_, b) = call(&store, "GET", "/api/endless/easy/next?count=10", me, "");
                let batch = codes(&b);
                run.extend(
                    batch
                        .iter()
                        .filter(|c| !run.contains(*c))
                        .cloned()
                        .collect::<Vec<_>>(),
                );
                let (st, _) = call(
                    &store,
                    "POST",
                    "/api/endless/easy/result",
                    me,
                    &report(&batch[0], "clear", ""),
                );
                assert_eq!(st, 200);
            }
            call(&store, "POST", "/api/endless/easy/finish", me, "");
            dealt.push(run);
        }
        assert!(dealt[1].iter().all(|c| !dealt[0].contains(c)), "{dealt:?}");
        // the recent list is the player's, kept in endless.json
        drop(store);
        let store = Store::open(&dir).unwrap();
        let g = store.inner.lock().unwrap();
        let recent = &g.endless_runs.values().next().unwrap().recent;
        assert!(dealt[0].iter().chain(&dealt[1]).all(|c| recent.contains(c)));
        drop(g);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_run_on_the_server() {
        let dir = tmp("run");
        let store = Store::open(&dir).unwrap();
        let (me, my_id) = account(&store, "Pia");
        let (maker, _) = account(&store, "Mo");
        let easy: Vec<String> = (0..12).map(|_| course(&store, &maker, 50, 100)).collect();
        let expert: Vec<String> = (0..3).map(|_| course(&store, &maker, 5, 100)).collect();
        let unrated: Vec<String> = (0..2).map(|_| course(&store, &maker, 0, 10)).collect();
        let own = course(&store, &me, 60, 100);
        let me = Some(me.as_str());

        assert_eq!(call(&store, "GET", "/api/endless", None, "").0, 401);
        let (st, s) = call(&store, "GET", "/api/endless", me, "");
        assert_eq!(st, 200);
        assert!(s["runs"]["easy"].is_null());
        assert_eq!(
            (s["best"]["easy"].as_u64(), s["skips"].as_u64()),
            (Some(0), Some(0))
        );
        assert_eq!(call(&store, "GET", "/api/endless/easy/next", me, "").0, 404);
        assert_eq!(
            call(&store, "POST", "/api/endless/silly/start", me, "").0,
            404
        );

        // New Game
        let (st, v) = call(&store, "POST", "/api/endless/easy/start", me, "");
        assert_eq!(st, 201);
        assert_eq!(
            (v["run"]["lives"].as_u64(), v["run"]["active"].as_bool()),
            (Some(5), Some(true))
        );
        let (st, b) = call(&store, "GET", "/api/endless/easy/next", me, "");
        assert_eq!(st, 200);
        let batch = codes(&b);
        assert_eq!(batch.len(), 10);
        assert!(
            batch.iter().all(|c| easy.contains(c)),
            "Easy band only, never my own"
        );
        assert!(!batch.contains(&own));
        assert_eq!(b["widened"], false);
        let (_, again) = call(&store, "GET", "/api/endless/easy/next?count=3", me, "");
        assert_eq!(codes(&again), batch[..3], "the queue is kept");

        // a miss, then a clear with 2 pending 1-Ups
        let c0 = &batch[0];
        let (st, a) = call(
            &store,
            "POST",
            "/api/endless/easy/result",
            me,
            &report(c0, "death", ""),
        );
        assert_eq!(st, 200);
        assert_eq!(
            (a["lives"].as_u64(), a["game_over"].as_bool()),
            (Some(4), Some(false))
        );
        let clear = report(
            c0,
            "clear",
            r#","pending":2,"coins":30,"score":100,"frames_left":600"#,
        );
        let (_, a) = call(&store, "POST", "/api/endless/easy/result", me, &clear);
        assert_eq!(
            (
                a["lives"].as_u64(),
                a["clears"].as_u64(),
                a["best"].as_u64()
            ),
            (Some(6), Some(1), Some(1))
        );
        let m = store.get(c0).unwrap();
        assert_eq!(
            (m.plays, m.attempts, m.clears),
            (1, 102, 51),
            "Endless plays count"
        );
        assert_eq!(m.first_clear, my_id);
        let (_, r) = call(&store, "GET", "/api/rankings/endless_easy", None, "");
        assert_eq!(r["rankings"][0]["score"], 1);
        // a course that is not in the run, or already passed; no result
        assert_eq!(
            call(&store, "POST", "/api/endless/easy/result", me, &clear).0,
            400
        );
        assert_eq!(
            call(
                &store,
                "POST",
                "/api/endless/easy/result",
                me,
                &report(&own, "clear", "")
            )
            .0,
            400
        );
        assert_eq!(
            call(
                &store,
                "POST",
                "/api/endless/easy/result",
                me,
                r#"{"code":"x"}"#
            )
            .0,
            400
        );
        // a free skip
        let (_, a) = call(
            &store,
            "POST",
            "/api/endless/easy/result",
            me,
            &report(&batch[1], "skip", ""),
        );
        assert_eq!(
            (a["lives"].as_u64(), a["passed"].as_u64()),
            (Some(6), Some(2))
        );
        // the map's history: clears only, a skip stays off it (#83)
        let (_, h) = call(&store, "GET", "/api/endless/easy/history", me, "");
        let h = h["history"].as_array().unwrap();
        assert_eq!(h.len(), 1);
        assert_eq!(
            (
                h[0]["result"].as_str(),
                h[0]["seq"].as_u64(),
                h[0]["code"].as_str()
            ),
            (Some("clear"), Some(1), Some(batch[0].as_str()))
        );
        // with each course's name and maker (a continued run's credits)
        let m = store.get(&batch[0]).unwrap();
        assert_eq!(
            (h[0]["name"].as_str(), h[0]["maker"].as_str()),
            (Some(m.name.as_str()), Some(m.maker.as_str()))
        );
        // suspend and status
        let (st, v) = call(&store, "POST", "/api/endless/easy/suspend", me, "");
        assert_eq!((st, v["run"]["active"].as_bool()), (200, Some(true)));
        let (_, s) = call(&store, "GET", "/api/endless", me, "");
        let run = RunStatus::from_json(&s["runs"]["easy"]);
        assert_eq!(
            (run.lives, run.clears, run.passed, run.coins, run.score),
            (6, 1, 2, 30, 600)
        );
        assert_eq!(
            (s["skips"].as_u64(), s["last"].as_str()),
            (Some(1), Some("easy"))
        );
        // persisted: the queue continues with the third course
        let store = Store::open(&dir).unwrap();
        let (_, b) = call(&store, "GET", "/api/endless/easy/next", me, "");
        assert_eq!(codes(&b)[0], batch[2]);
        assert_eq!(codes(&b).len(), 8);
        // clear the rest of the batch, then the band runs dry and the draw widens
        for c in &batch[2..] {
            call(
                &store,
                "POST",
                "/api/endless/easy/result",
                me,
                &report(c, "clear", ""),
            );
        }
        let (_, b) = call(&store, "GET", "/api/endless/easy/next", me, "");
        assert_eq!(
            (codes(&b).len(), b["widened"].as_bool()),
            (2, Some(false)),
            "the last 2 Easy"
        );
        for c in codes(&b) {
            call(
                &store,
                "POST",
                "/api/endless/easy/result",
                me,
                &report(&c, "clear", ""),
            );
        }
        let (_, b) = call(&store, "GET", "/api/endless/easy/next", me, "");
        let mut got = codes(&b);
        got.sort();
        let mut want = expert.clone();
        want.sort();
        assert_eq!(
            (got, b["widened"].as_bool()),
            (want, Some(true)),
            "Normal is empty: Expert"
        );
        assert_eq!(b["run"]["widened"], true);
        for c in codes(&b) {
            call(
                &store,
                "POST",
                "/api/endless/easy/result",
                me,
                &report(&c, "skip", ""),
            );
        }
        let (_, b) = call(&store, "GET", "/api/endless/easy/next", me, "");
        let mut got = codes(&b);
        got.sort();
        let mut want = unrated.clone();
        want.sort();
        assert_eq!(got, want, "unrated courses come last");
        // game over
        let last = &codes(&b)[0];
        let mut a = Value::Null;
        for _ in 0..6 {
            a = call(
                &store,
                "POST",
                "/api/endless/easy/result",
                me,
                &report(last, "start_over", ""),
            )
            .1;
        }
        assert_eq!(
            (a["lives"].as_u64(), a["game_over"].as_bool()),
            (Some(0), Some(true))
        );
        assert_eq!(call(&store, "GET", "/api/endless/easy/next", me, "").0, 400);
        let (st, f) = call(&store, "POST", "/api/endless/easy/finish", me, "");
        assert_eq!(st, 200);
        assert_eq!(
            (
                f["clears"].as_u64(),
                f["best"].as_u64(),
                f["new_high_score"].as_bool()
            ),
            (Some(11), Some(11), Some(true))
        );
        assert_eq!(
            call(&store, "GET", "/api/endless/easy/history", me, "").0,
            404
        );
        assert_eq!(
            call(&store, "POST", "/api/endless/easy/finish", me, "").0,
            404
        );
        // the older best-only route feeds the same best
        let (_, v) = call(&store, "POST", "/api/endless/easy", me, r#"{"cleared":20}"#);
        assert_eq!(v["best"], 20);
        let (_, s) = call(&store, "GET", "/api/endless", me, "");
        assert_eq!(s["best"]["easy"], 20);
        let (_, v) = call(&store, "POST", "/api/endless/easy/start", me, "");
        assert_eq!(v["run"]["best_before"], 20);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_hundred_mario_run_on_the_server() {
        let dir = tmp("hundred");
        let store = Store::open(&dir).unwrap();
        let (me, my_id) = account(&store, "Pia");
        let (maker, _) = account(&store, "Mo");
        let easy: Vec<String> = (0..10).map(|_| course(&store, &maker, 80, 100)).collect();
        for _ in 0..3 {
            course(&store, &maker, 40, 100);
        }
        let me = Some(me.as_str());
        let start = |body: &str| call(&store, "POST", "/api/endless/easy/start", me, body);
        assert_eq!(start(r#"{"ruleset":"silly"}"#).0, 400);
        let (st, v) = start(r#"{"ruleset":"hundred"}"#);
        assert_eq!(st, 201);
        assert_eq!(
            (v["run"]["lives"].as_u64(), v["run"]["goal"].as_u64()),
            (Some(100), Some(8))
        );
        assert_eq!(v["run"]["ruleset"], "hundred");
        let (_, b) = call(&store, "GET", "/api/endless/easy/next", me, "");
        let batch = codes(&b);
        assert!(batch.iter().all(|c| easy.contains(c)), "fail rate 0–34 %");
        let post = |code: &str, result: &str, extra: &str| {
            call(
                &store,
                "POST",
                "/api/endless/easy/result",
                me,
                &report(code, result, extra),
            )
        };
        post(&batch[0], "death", "");
        let (_, a) = post(&batch[0], "skip", r#","during_miss":true"#);
        assert_eq!(a["lives"].as_u64(), Some(99), "free skip");
        for (k, c) in batch[1..9].iter().enumerate() {
            let (st, a) = post(c, "clear", "");
            assert_eq!(st, 200);
            assert_eq!(a["run_complete"].as_bool(), Some(k == 7));
        }
        let (_, a) = call(&store, "GET", "/api/endless", me, "");
        assert_eq!(a["hundred_runs"]["easy"]["complete"], true);
        assert_eq!(a["best"]["easy"].as_u64(), Some(0), "not an Endless score");
        assert_eq!(call(&store, "GET", "/api/endless/easy/next", me, "").0, 400);
        let (_, r) = call(&store, "GET", "/api/rankings/hundred_easy", me, "");
        assert_eq!(r["rankings"][0]["score"], 1);
        assert_eq!(r["me"]["maker_id"], my_id.as_str());
        assert_eq!(r["me"]["rank"], 1);
        let (_, r) = call(&store, "GET", "/api/rankings/hundred_easy", None, "");
        assert!(r["me"].is_null());
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_hundred_mario_run_ends_at_the_castle() {
        let dir = tmp("castle");
        let store = Store::open(&dir).unwrap();
        let (me, _) = account(&store, "Pia");
        let (maker, _) = account(&store, "Mo");
        for _ in 0..12 {
            course(&store, &maker, 80, 100);
        }
        let castles: Vec<String> = (0..3).map(|_| course(&store, &maker, 80, 100)).collect();
        {
            let mut g = store.inner.lock().unwrap();
            for c in &castles {
                let i = g.by_code[c];
                g.courses[i].theme = 2;
            }
        }
        let me = Some(me.as_str());
        call(
            &store,
            "POST",
            "/api/endless/easy/start",
            me,
            r#"{"ruleset":"hundred"}"#,
        );
        // the 8th course of an Easy run (the last) is the castle one, the first seven aren't
        let mut seen = Vec::new();
        for _ in 0..8 {
            let (_, b) = call(
                &store,
                "GET",
                "/api/endless/easy/next?ruleset=hundred",
                me,
                "",
            );
            let c = codes(&b)[0].clone();
            call(
                &store,
                "POST",
                "/api/endless/easy/result?ruleset=hundred",
                me,
                &report(&c, "clear", ""),
            );
            seen.push(c);
        }
        assert!(
            castles.contains(&seen[7]),
            "the run's last course is a castle-theme one"
        );
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_hundred_mario_run_with_skips_still_ends_at_the_castle() {
        let dir = tmp("castle_skips");
        let store = Store::open(&dir).unwrap();
        let (me, _) = account(&store, "Pia");
        let (maker, _) = account(&store, "Mo");
        let all: Vec<String> = (0..16).map(|_| course(&store, &maker, 80, 100)).collect();
        let castles: Vec<String> = all[..4].to_vec();
        {
            let mut g = store.inner.lock().unwrap();
            for c in &castles {
                let i = g.by_code[c];
                g.courses[i].theme = 2;
            }
        }
        let me = Some(me.as_str());
        call(
            &store,
            "POST",
            "/api/endless/easy/start",
            me,
            r#"{"ruleset":"hundred"}"#,
        );
        // two skips on the way: the course whose clear completes the run is still a castle one
        for k in 0..10 {
            let (_, b) = call(
                &store,
                "GET",
                "/api/endless/easy/next?count=2&ruleset=hundred",
                me,
                "",
            );
            let c = codes(&b)[0].clone();
            let result = if k == 2 || k == 5 { "skip" } else { "clear" };
            let (_, a) = call(
                &store,
                "POST",
                "/api/endless/easy/result?ruleset=hundred",
                me,
                &report(&c, result, ""),
            );
            if a["run_complete"].as_bool() == Some(true) {
                assert!(
                    castles.contains(&c),
                    "the run's last course is a castle-theme one"
                );
                let _ = fs::remove_dir_all(&dir);
                return;
            }
        }
        panic!("the run didn't complete");
    }

    #[test]
    fn a_course_handed_out_never_changes() {
        // (the client fetches the next course ahead: the one after the current must still be the next one after a
        // clear, a skip or deaths - v0.5.4's castle swap broke that)
        let dir = tmp("handed");
        let store = Store::open(&dir).unwrap();
        let (me, _) = account(&store, "Pia");
        let (maker, _) = account(&store, "Mo");
        let all: Vec<String> = (0..40).map(|_| course(&store, &maker, 80, 100)).collect();
        let me = Some(me.as_str());
        call(
            &store,
            "POST",
            "/api/endless/normal/start",
            me,
            r#"{"ruleset":"hundred"}"#,
        );
        let mut ahead: Option<String> = None;
        let mut seen: HashSet<String> = HashSet::new();
        for k in 0..16 {
            if k == 15 {
                // (castle courses turn up late - an upstream source brought them - just before the last course is
                // the one handed out ahead, and so the one about to be played)
                let mut g = store.inner.lock().unwrap();
                for c in all.iter().filter(|c| !seen.contains(*c)) {
                    let i = g.by_code[c];
                    g.courses[i].theme = 2;
                }
            }
            let (_, b) = call(
                &store,
                "GET",
                "/api/endless/normal/next?count=2&ruleset=hundred",
                me,
                "",
            );
            let got = codes(&b);
            if let Some(a) = &ahead {
                assert_eq!(
                    &got[0], a,
                    "course {k}: the one handed out ahead is the next one"
                );
            }
            ahead = got.get(1).cloned();
            seen.extend(got.iter().cloned());
            let result = if k % 3 == 1 { "skip" } else { "clear" };
            if k % 4 == 2 {
                call(
                    &store,
                    "POST",
                    "/api/endless/normal/result?ruleset=hundred",
                    me,
                    &report(&got[0], "death", ""),
                );
            }
            let (st, _) = call(
                &store,
                "POST",
                "/api/endless/normal/result?ruleset=hundred",
                me,
                &report(&got[0], result, ""),
            );
            assert_eq!(st, 200, "course {k}: its result is taken");
        }
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn banned_makers_and_own_courses_are_never_drawn() {
        let dir = tmp("banned");
        let store = Store::open(&dir).unwrap().with_admin(Some("root".into()));
        let (me, _) = account(&store, "Pia");
        let (bad, _) = account(&store, "Bad");
        let (good, _) = account(&store, "Good");
        for _ in 0..3 {
            course(&store, &bad, 1, 100);
        }
        course(&store, &me, 1, 100);
        let fine = course(&store, &good, 1, 100);
        let id = store.account(&bad).unwrap().id;
        assert_eq!(
            call(
                &store,
                "POST",
                &format!("/api/admin/ban/{id}"),
                Some("root"),
                ""
            )
            .0,
            200
        );
        let me = Some(me.as_str());
        call(&store, "POST", "/api/endless/super_expert/start", me, "");
        let (_, b) = call(&store, "GET", "/api/endless/super_expert/next", me, "");
        assert_eq!(codes(&b), vec![fine]);
        // a banned player can't play either
        assert_eq!(
            call(&store, "POST", "/api/endless/easy/start", Some(&bad), "").0,
            401
        );
        let _ = fs::remove_dir_all(&dir);
    }
}
