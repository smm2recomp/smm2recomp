//! Client of the open-source course server (`mmk-server`, Phase 10): Course World browsing,
//! downloads, uploads and statistics, a [`CourseSource`] so 100 Mario runs can draw their
//! courses from a server, and the server-held Endless Challenge runs (`Client::endless_*`).
//! Never talks to Nintendo's network.

use mmk_modes::Rng;
use mmk_modes::endless::{Difficulty, Finished, HistoryEntry, Report, Ruleset, RunStatus};
use mmk_modes::pool::{CourseRef, CourseSource};
use serde_json::Value;

/// A course listing as the server describes it.
#[derive(Clone, Debug, PartialEq)]
pub struct Listing {
    pub code: String,
    pub name: String,
    pub maker: String,
    pub style: String,
    pub theme: u8,
    pub plays: u64,
    pub clears: u64,
    pub likes: u64,
    pub boos: u64,
    pub difficulty: u8,
    /// The uploader's maker ID (empty for anonymous uploads), the record (ms, 0 none) and its
    /// holder, the first clearer.
    pub maker_id: String,
    pub record_ms: u64,
    pub record_holder: String,
    /// The record was proven by a replay the server re-simulated.
    pub record_verified: bool,
    pub first_clear: String,
    /// The course's description, upload time (seconds since 1970) and attempts.
    pub description: String,
    pub uploaded: u64,
    pub attempts: u64,
}

impl Listing {
    fn from_json(v: &Value) -> Option<Self> {
        let s = |k: &str| v[k].as_str().unwrap_or_default().to_string();
        let n = |k: &str| v[k].as_u64().unwrap_or(0);
        Some(Self {
            code: v["code"].as_str()?.to_string(),
            name: s("name"),
            maker: s("maker"),
            style: s("style"),
            theme: n("theme") as u8,
            plays: n("plays"),
            clears: n("clears"),
            likes: n("likes"),
            boos: n("boos"),
            difficulty: n("difficulty") as u8,
            maker_id: s("maker_id"),
            record_ms: n("record_ms"),
            record_holder: s("record_holder"),
            record_verified: v["record_verified"].as_bool().unwrap_or(false),
            first_clear: s("first_clear"),
            description: s("description"),
            uploaded: n("uploaded"),
            attempts: n("attempts"),
        })
    }
}

/// A server account (the token is the credential; keep it private).
#[derive(Clone, Debug, PartialEq)]
pub struct Account {
    pub id: u32,
    pub name: String,
    pub maker_id: String,
}

impl Account {
    fn from_json(v: &Value) -> Option<Self> {
        Some(Self {
            id: v["id"].as_u64()? as u32,
            name: v["name"].as_str()?.to_string(),
            maker_id: v["maker_id"].as_str().unwrap_or_default().to_string(),
        })
    }
}

/// A course comment.
#[derive(Clone, Debug, PartialEq)]
pub struct Comment {
    pub maker_id: String,
    pub name: String,
    pub text: String,
    pub posted: u64,
}

/// The signed-in player's Endless Challenge data (`GET /api/endless`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct EndlessStatus {
    /// The run per difficulty (index = `Difficulty::index`), None when there is none.
    pub runs: [Option<RunStatus>; 4],
    /// Best clears per difficulty.
    pub best: [u64; 4],
    /// Lifetime skips (0–3, for the skip hint).
    pub skips: u32,
    pub last: Option<Difficulty>,
}

impl EndlessStatus {
    pub fn run(&self, d: Difficulty) -> Option<&RunStatus> {
        self.runs[d.index()].as_ref()
    }
}

/// The run's next courses, to play in order.
#[derive(Clone, Debug, PartialEq)]
pub struct EndlessBatch {
    pub courses: Vec<Listing>,
    /// The server had to leave the difficulty's band to find them.
    pub widened: bool,
    pub run: RunStatus,
}

/// The server's answer to a course result.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct EndlessAnswer {
    pub lives: u32,
    pub clears: u32,
    pub passed: u32,
    pub game_over: bool,
    /// The run reached its goal (a completed 100 Mario run).
    pub complete: bool,
    /// Best clears on the difficulty (a clear raises it); 100 Mario: completed runs.
    pub best: u64,
}

#[derive(Clone, Debug)]
pub struct Client {
    /// e.g. "http://localhost:8640"
    pub base: String,
    /// Account token for signed-in calls (uploads under the account, votes, comments).
    pub token: Option<String>,
}

pub type Result<T> = std::result::Result<T, String>;

impl Client {
    pub fn new(base: impl Into<String>) -> Self {
        Self {
            base: base.into().trim_end_matches('/').to_string(),
            token: None,
        }
    }

    pub fn with_token(mut self, token: Option<String>) -> Self {
        self.token = token;
        self
    }

    fn auth<B>(&self, req: ureq::RequestBuilder<B>) -> ureq::RequestBuilder<B> {
        match &self.token {
            Some(t) => req.header("Authorization", &format!("Bearer {t}")),
            None => req,
        }
    }

    /// POST a JSON body (signed in when a token is set); the JSON answer.
    fn post_json(&self, path: &str, body: &Value) -> Result<Value> {
        let mut r = self
            .auth(ureq::post(&format!("{}{path}", self.base)))
            .header("Content-Type", "application/json")
            .send(serde_json::to_vec(body).unwrap().as_slice())
            .map_err(|e| e.to_string())?;
        let s = r.body_mut().read_to_string().map_err(|e| e.to_string())?;
        serde_json::from_str(&s).map_err(|e| e.to_string())
    }

    /// Create an account named `name`: the account and its token (sign in with `with_token`).
    pub fn register(&self, name: &str) -> Result<(Account, String)> {
        let v = self.post_json("/api/accounts", &serde_json::json!({ "name": name }))?;
        let a = Account::from_json(&v["account"]).ok_or("registration refused")?;
        let t = v["token"].as_str().ok_or("no token")?.to_string();
        Ok((a, t))
    }

    /// The signed-in account.
    pub fn me(&self) -> Result<Account> {
        let mut r = self
            .auth(ureq::get(&format!("{}/api/me", self.base)))
            .call()
            .map_err(|e| e.to_string())?;
        let s = r.body_mut().read_to_string().map_err(|e| e.to_string())?;
        let v: Value = serde_json::from_str(&s).map_err(|e| e.to_string())?;
        Account::from_json(&v).ok_or_else(|| "not signed in".into())
    }

    /// Like (true) or boo (false) a course as the signed-in account.
    pub fn vote(&self, code: &str, like: bool) -> Result<Listing> {
        let what = if like { "like" } else { "boo" };
        let v = self.post_json(&format!("/api/courses/{code}/{what}"), &Value::Null)?;
        Listing::from_json(&v).ok_or_else(|| format!("vote refused: {v}"))
    }

    /// Report a clear in `time_ms` (records count when signed in).
    pub fn clear(&self, code: &str, time_ms: u64) -> Result<Listing> {
        let v = self.post_json(
            &format!("/api/courses/{code}/clear"),
            &serde_json::json!({ "time_ms": time_ms }),
        )?;
        Listing::from_json(&v).ok_or_else(|| format!("clear refused: {v}"))
    }

    pub fn comments(&self, code: &str) -> Result<Vec<Comment>> {
        let v = self.get_json(&format!("/api/courses/{code}/comments"))?;
        Ok(v["comments"]
            .as_array()
            .map(|a| {
                a.iter()
                    .filter_map(|c| {
                        Some(Comment {
                            maker_id: c["maker_id"].as_str()?.to_string(),
                            name: c["name"].as_str().unwrap_or_default().to_string(),
                            text: c["text"].as_str()?.to_string(),
                            posted: c["posted"].as_u64().unwrap_or(0),
                        })
                    })
                    .collect()
            })
            .unwrap_or_default())
    }

    pub fn comment(&self, code: &str, text: &str) -> Result<()> {
        let v = self.post_json(
            &format!("/api/courses/{code}/comments"),
            &serde_json::json!({ "text": text }),
        )?;
        if v.get("error").is_some() {
            return Err(v["error"].as_str().unwrap_or("refused").to_string());
        }
        Ok(())
    }

    /// A maker's profile and courses.
    pub fn maker(&self, maker_id: &str) -> Result<(Account, Vec<Listing>)> {
        let v = self.get_json(&format!("/api/makers/{maker_id}"))?;
        let a = Account::from_json(&v["maker"]).ok_or("no such maker")?;
        Ok((a, Self::list(&v)))
    }

    /// Report an Endless Challenge run (`easy`, `normal`, `expert`, `super_expert`) that cleared
    /// `cleared` courses; the account's best on that difficulty.
    pub fn endless(&self, difficulty: &str, cleared: u64) -> Result<u64> {
        let v = self.post_json(
            &format!("/api/endless/{difficulty}"),
            &serde_json::json!({ "cleared": cleared }),
        )?;
        v["best"].as_u64().ok_or_else(|| format!("refused: {v}"))
    }

    /// A 100 Mario run on `difficulty` completed (held by this client); the account's completed
    /// runs on that difficulty.
    pub fn hundred_complete(&self, difficulty: &str) -> Result<u64> {
        let v = self.post_json(
            &format!("/api/endless/{difficulty}"),
            &serde_json::json!({ "ruleset": "hundred" }),
        )?;
        v["best"].as_u64().ok_or_else(|| format!("refused: {v}"))
    }

    /// Endless Challenge: the player's four runs, best clears, lifetime skips, last difficulty.
    pub fn endless_status(&self) -> Result<EndlessStatus> {
        let v = self.get_json("/api/endless")?;
        let mut s = EndlessStatus {
            skips: v["skips"].as_u64().unwrap_or(0) as u32,
            last: v["last"].as_str().and_then(Difficulty::from_name),
            ..EndlessStatus::default()
        };
        for d in Difficulty::ALL {
            let r = &v["runs"][d.name()];
            if r.is_object() {
                s.runs[d.index()] = Some(RunStatus::from_json(r));
            }
            s.best[d.index()] = v["best"][d.name()].as_u64().unwrap_or(0);
        }
        Ok(s)
    }

    /// Endless New Game on `d` (wipes that difficulty's run): the new run.
    pub fn endless_start(&self, d: Difficulty) -> Result<RunStatus> {
        self.endless_start_with(d, Ruleset::Smm2)
    }

    /// New Game under `ruleset`: SMM2's Endless rules or SMM1's 100 Mario Challenge.
    pub fn endless_start_with(&self, d: Difficulty, ruleset: Ruleset) -> Result<RunStatus> {
        let body = serde_json::json!({ "ruleset": ruleset.name() });
        let v = self.post_json(&format!("/api/endless/{}/start", d.name()), &body)?;
        Self::run_of(&v)
    }

    /// The run's next courses (up to `count`, at most 10). The first is the one to play; it stays
    /// first until its clear, skip or broken report.
    pub fn endless_next(&self, d: Difficulty, count: usize) -> Result<EndlessBatch> {
        let v = self.get_json(&format!("/api/endless/{}/next?count={count}", d.name()))?;
        Ok(EndlessBatch {
            courses: Self::list(&v),
            widened: v["widened"].as_bool().unwrap_or(false),
            run: Self::run_of(&v)?,
        })
    }

    /// Report a course result (build it with `mmk_modes::endless::Run::finish`).
    pub fn endless_report(&self, d: Difficulty, r: &Report) -> Result<EndlessAnswer> {
        let v = self.post_json(&format!("/api/endless/{}/result", d.name()), &r.to_json())?;
        if v.get("error").is_some() || !v["lives"].is_u64() {
            return Err(format!("refused: {v}"));
        }
        let n = |k: &str| v[k].as_u64().unwrap_or(0) as u32;
        Ok(EndlessAnswer {
            lives: n("lives"),
            clears: n("clears"),
            passed: n("passed"),
            game_over: v["game_over"].as_bool().unwrap_or(false),
            complete: v["run_complete"].as_bool().unwrap_or(false),
            best: v["best"].as_u64().unwrap_or(0),
        })
    }

    /// Quit to the menu: the run is kept for Continue.
    pub fn endless_suspend(&self, d: Difficulty) -> Result<RunStatus> {
        let v = self.post_json(&format!("/api/endless/{}/suspend", d.name()), &Value::Null)?;
        Self::run_of(&v)
    }

    /// Finish the run after the game over: final clears, best, new high score.
    pub fn endless_finish(&self, d: Difficulty) -> Result<Finished> {
        let v = self.post_json(&format!("/api/endless/{}/finish", d.name()), &Value::Null)?;
        if !v["best"].is_u64() {
            return Err(format!("refused: {v}"));
        }
        Ok(Finished::from_json(&v))
    }

    /// The run's last 48 courses, oldest first (the world map).
    pub fn endless_history(&self, d: Difficulty) -> Result<Vec<HistoryEntry>> {
        let v = self.get_json(&format!("/api/endless/{}/history", d.name()))?;
        Ok(v["history"]
            .as_array()
            .map(|a| a.iter().filter_map(HistoryEntry::from_json).collect())
            .unwrap_or_default())
    }

    fn run_of(v: &Value) -> Result<RunStatus> {
        if v["run"].is_object() {
            Ok(RunStatus::from_json(&v["run"]))
        } else {
            Err(format!("refused: {v}"))
        }
    }

    /// Leaderboard `kind` (maker_points, clears, first_clears, records, endless_<difficulty>):
    /// (rank, score, maker ID, name), best first.
    pub fn rankings(&self, kind: &str, limit: usize) -> Result<Vec<(u32, i64, String, String)>> {
        let v = self.get_json(&format!("/api/rankings/{kind}?limit={limit}"))?;
        Ok(v["rankings"]
            .as_array()
            .map(|a| {
                a.iter()
                    .filter_map(|r| {
                        Some((
                            r["rank"].as_u64()? as u32,
                            r["score"].as_i64()?,
                            r["maker_id"].as_str()?.to_string(),
                            r["name"].as_str().unwrap_or_default().to_string(),
                        ))
                    })
                    .collect()
            })
            .unwrap_or_default())
    }

    /// Prove a clear with a replay (`mmk_modes::replay`): the server re-simulates it and records
    /// its time (a verified record when it is the best).
    pub fn submit_replay(&self, code: &str, replay: &[u8]) -> Result<Listing> {
        let mut r = self
            .auth(ureq::post(&format!(
                "{}/api/courses/{code}/replay",
                self.base
            )))
            .header("Content-Type", "application/octet-stream")
            .send(replay)
            .map_err(|e| e.to_string())?;
        let s = r.body_mut().read_to_string().map_err(|e| e.to_string())?;
        let v: Value = serde_json::from_str(&s).map_err(|e| e.to_string())?;
        Listing::from_json(&v).ok_or_else(|| format!("refused: {v}"))
    }

    /// The verified record's replay (to race as a ghost).
    pub fn record_replay(&self, code: &str) -> Result<Vec<u8>> {
        let mut r = ureq::get(&format!("{}/api/courses/{code}/replay", self.base))
            .call()
            .map_err(|e| e.to_string())?;
        r.body_mut()
            .with_config()
            .limit(1 << 20)
            .read_to_vec()
            .map_err(|e| e.to_string())
    }

    /// Remove a course (yours, or any with the admin token).
    pub fn delete(&self, code: &str) -> Result<()> {
        self.auth(ureq::delete(&format!("{}/api/courses/{code}", self.base)))
            .call()
            .map(|_| ())
            .map_err(|e| e.to_string())
    }

    fn get_json(&self, path: &str) -> Result<Value> {
        let mut r = self
            .auth(ureq::get(&format!("{}{path}", self.base)))
            .call()
            .map_err(|e| e.to_string())?;
        let s = r.body_mut().read_to_string().map_err(|e| e.to_string())?;
        serde_json::from_str(&s).map_err(|e| e.to_string())
    }

    fn list(v: &Value) -> Vec<Listing> {
        v["courses"]
            .as_array()
            .map(|a| a.iter().filter_map(Listing::from_json).collect())
            .unwrap_or_default()
    }

    /// Search courses: `q` text, optional style, sort "new" / "popular" / "hard".
    pub fn search(
        &self,
        q: &str,
        style: Option<&str>,
        sort: &str,
        offset: usize,
        limit: usize,
    ) -> Result<Vec<Listing>> {
        let mut path = format!(
            "/api/courses?q={}&sort={sort}&offset={offset}&limit={limit}",
            encode(q)
        );
        if let Some(s) = style {
            path += &format!("&style={s}");
        }
        Ok(Self::list(&self.get_json(&path)?))
    }

    pub fn info(&self, code: &str) -> Result<Listing> {
        Listing::from_json(&self.get_json(&format!("/api/courses/{code}"))?)
            .ok_or_else(|| "bad listing".into())
    }

    /// The course data (raw).
    pub fn download(&self, code: &str) -> Result<Vec<u8>> {
        let mut r = ureq::get(&format!("{}/api/courses/{code}/data", self.base))
            .call()
            .map_err(|e| e.to_string())?;
        r.body_mut()
            .with_config()
            .limit(0x80000)
            .read_to_vec()
            .map_err(|e| e.to_string())
    }

    /// Upload raw course data as `maker`; returns the new listing (with its code).
    pub fn upload(&self, data: &[u8], maker: &str) -> Result<Listing> {
        let mut r = self
            .auth(ureq::post(&format!("{}/api/courses", self.base)))
            .header("X-Maker", maker)
            .header("Content-Type", "application/octet-stream")
            .send(data)
            .map_err(|e| e.to_string())?;
        let s = r.body_mut().read_to_string().map_err(|e| e.to_string())?;
        let v: Value = serde_json::from_str(&s).map_err(|e| e.to_string())?;
        Listing::from_json(&v).ok_or_else(|| format!("upload refused: {s}"))
    }

    /// Report a play ("play") or an anonymous clear ("clear"); likes need an account (`vote`).
    pub fn report(&self, code: &str, what: &str) -> Result<()> {
        ureq::post(&format!("{}/api/courses/{code}/{what}", self.base))
            .send_empty()
            .map(|_| ())
            .map_err(|e| e.to_string())
    }

    /// Random courses with a difficulty value in `band` (100 Mario).
    pub fn pool(&self, band: (u8, u8), count: usize) -> Result<Vec<Listing>> {
        Ok(Self::list(&self.get_json(&format!(
            "/api/pool?min={}&max={}&count={count}",
            band.0, band.1
        ))?))
    }
}

/// A listing as JSON (the FFI's and the front ends' shape).
pub fn listing_json(l: &Listing) -> Value {
    serde_json::json!({
        "code": l.code, "name": l.name, "maker": l.maker, "style": l.style, "theme": l.theme,
        "plays": l.plays, "clears": l.clears, "likes": l.likes, "difficulty": l.difficulty,
        "boos": l.boos, "maker_id": l.maker_id, "record_ms": l.record_ms,
        "record_holder": l.record_holder, "record_verified": l.record_verified,
        "first_clear": l.first_clear, "description": l.description, "uploaded": l.uploaded,
        "attempts": l.attempts,
    })
}

fn account_json(a: &Account) -> Value {
    serde_json::json!({ "id": a.id, "name": a.name, "maker_id": a.maker_id })
}

/// One server call by name, JSON in and out (the C API's `mmk_online_call` and the front ends'
/// `Host::online`):
///
/// | action | args | result |
/// |---|---|---|
/// | `register` | `{name}` | `{account, token}` (keep the token; it is the credential) |
/// | `me` | — | account `{id, name, maker_id}` |
/// | `info` | `{code}` | listing |
/// | `search` | `{q, sort, offset, limit}` | `[listing]` |
/// | `vote` | `{code, like: bool}` | listing |
/// | `clear` | `{code, time_ms}` | listing |
/// | `comments` | `{code}` | `[{maker_id, name, text, posted}]` |
/// | `comment` | `{code, text}` | `{}` |
/// | `maker` | `{maker_id}` | `{maker, courses}` |
/// | `rankings` | `{kind, limit}` | `[{rank, score, maker_id, name}]` |
/// | `endless` | `{difficulty, cleared}` | `{best}` |
/// | `hundred` | `{difficulty}` (a completed 100 Mario run) | `{best}` (completed runs) |
/// | `delete` | `{code}` | `{}` |
///
/// Replays (bytes) go through [`Client::submit_replay`] / [`Client::record_replay`].
pub fn call(server: &str, token: Option<&str>, action: &str, args: &Value) -> Result<Value> {
    use serde_json::json;
    let c = Client::new(server).with_token(token.filter(|t| !t.is_empty()).map(str::to_string));
    let s = |k: &str| args[k].as_str().unwrap_or_default();
    let n = |k: &str| args[k].as_u64().unwrap_or(0);
    Ok(match action {
        "register" => {
            let (a, t) = c.register(s("name"))?;
            json!({ "account": account_json(&a), "token": t })
        }
        "me" => account_json(&c.me()?),
        "info" => listing_json(&c.info(s("code"))?),
        "search" => Value::Array(
            c.search(
                s("q"),
                args["style"].as_str(),
                args["sort"].as_str().unwrap_or("popular"),
                n("offset") as usize,
                args["limit"].as_u64().unwrap_or(100).clamp(1, 500) as usize,
            )?
            .iter()
            .map(listing_json)
            .collect(),
        ),
        "vote" => listing_json(&c.vote(s("code"), args["like"].as_bool().unwrap_or(true))?),
        "clear" => listing_json(&c.clear(s("code"), n("time_ms"))?),
        "comments" => Value::Array(
            c.comments(s("code"))?
                .iter()
                .map(|m| {
                    json!({ "maker_id": m.maker_id, "name": m.name, "text": m.text,
                            "posted": m.posted })
                })
                .collect(),
        ),
        "comment" => {
            c.comment(s("code"), s("text"))?;
            json!({})
        }
        "maker" => {
            let (a, list) = c.maker(s("maker_id"))?;
            json!({ "maker": account_json(&a),
                    "courses": list.iter().map(listing_json).collect::<Vec<_>>() })
        }
        "rankings" => Value::Array(
            c.rankings(s("kind"), n("limit").clamp(1, 500) as usize)?
                .into_iter()
                .map(|(rank, score, id, name)| {
                    json!({ "rank": rank, "score": score, "maker_id": id, "name": name })
                })
                .collect(),
        ),
        "endless" => json!({ "best": c.endless(s("difficulty"), n("cleared"))? }),
        "hundred" => json!({ "best": c.hundred_complete(s("difficulty"))? }),
        "delete" => {
            c.delete(s("code"))?;
            json!({})
        }
        _ => return Err(format!("unknown action {action}")),
    })
}

fn encode(s: &str) -> String {
    s.bytes()
        .map(|b| match b {
            b'a'..=b'z' | b'A'..=b'Z' | b'0'..=b'9' | b'-' | b'_' | b'.' => (b as char).to_string(),
            b' ' => "+".into(),
            _ => format!("%{b:02X}"),
        })
        .collect()
}

/// 100 Mario courses from a server. `CourseRef::id` is the course code; the app downloads it
/// before playing. Network errors give an empty search (the run reports "not enough courses").
pub struct RemoteSource {
    pub client: Client,
    /// Only courses in styles the engine plays.
    pub styles: Vec<String>,
}

impl CourseSource for RemoteSource {
    fn search(
        &mut self,
        band: (u8, u8),
        max: usize,
        _castle: bool,
        _rng: &mut Rng,
    ) -> Vec<CourseRef> {
        self.client
            .pool(band, max)
            .unwrap_or_default()
            .into_iter()
            .filter(|l| self.styles.is_empty() || self.styles.contains(&l.style))
            .map(|l| CourseRef {
                id: l.code,
                name: l.name,
                maker: l.maker,
                difficulty: l.difficulty,
            })
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn talks_to_a_live_server() {
        let dir = std::env::temp_dir().join(format!("mmk-online-test-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let store = mmk_server::Store::open(&dir).unwrap();
        let port = 20000 + (std::process::id() % 20000) as u16;
        let addr = format!("127.0.0.1:{port}");
        let a2 = addr.clone();
        std::thread::spawn(move || mmk_server::serve(store, &a2));
        std::thread::sleep(std::time::Duration::from_millis(200));
        let c = Client::new(format!("http://{addr}"));
        let mut course = mmk_formats::smm2::Course::blank("M1");
        course.header.name.set("Online test");
        let data = course.to_bytes().unwrap();
        let up = c.upload(&data, "tester").unwrap();
        assert_eq!(up.name, "Online test");
        assert_eq!(c.download(&up.code).unwrap(), data);
        c.report(&up.code, "play").unwrap();
        c.report(&up.code, "clear").unwrap();
        assert_eq!(c.info(&up.code).unwrap().difficulty, 0);
        assert_eq!(c.search("online", None, "new", 0, 10).unwrap().len(), 1);
        let mut src = RemoteSource {
            client: c.clone(),
            styles: vec!["M1".into()],
        };
        let got = src.search((0, 34), 50, false, &mut Rng::new(1));
        assert_eq!(got.len(), 1);
        assert_eq!(got[0].id, up.code);
        // accounts
        let (acc, token) = c.register("Tess").unwrap();
        let me = c.clone().with_token(Some(token));
        assert_eq!(me.me().unwrap().name, "Tess");
        let mine = me.upload(&data, "ignored").unwrap();
        assert_eq!(
            (mine.maker.as_str(), mine.maker_id.as_str()),
            ("Tess", acc.maker_id.as_str())
        );
        assert!(c.vote(&mine.code, true).is_err(), "votes need an account");
        assert_eq!(me.vote(&mine.code, true).unwrap().likes, 1);
        assert_eq!(me.clear(&mine.code, 42_000).unwrap().record_ms, 42_000);
        me.comment(&mine.code, "hi").unwrap();
        assert_eq!(c.comments(&mine.code).unwrap()[0].text, "hi");
        assert_eq!(c.maker(&acc.maker_id).unwrap().1.len(), 1);
        assert_eq!(c.rankings("records", 10).unwrap()[0].2, acc.maker_id);
        assert_eq!(me.endless("normal", 12).unwrap(), 12);
        assert_eq!(me.endless("normal", 3).unwrap(), 12);
        assert_eq!(c.rankings("endless_normal", 5).unwrap()[0].1, 12);
        me.delete(&mine.code).unwrap();
        assert!(c.info(&mine.code).is_err());
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn endless_runs_on_a_live_server() {
        use mmk_modes::endless::{HistoryResult, Outcome, Run};
        let dir = std::env::temp_dir().join(format!("mmk-online-endless-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let store = mmk_server::Store::open(&dir).unwrap();
        let port = 40000 + (std::process::id() % 20000) as u16;
        let addr = format!("127.0.0.1:{port}");
        let a2 = addr.clone();
        std::thread::spawn(move || mmk_server::serve(store, &a2));
        std::thread::sleep(std::time::Duration::from_millis(200));
        let anon = Client::new(format!("http://{addr}"));
        let (_, maker_token) = anon.register("Mo").unwrap();
        let maker = anon.clone().with_token(Some(maker_token));
        let data = mmk_formats::smm2::Course::blank("M1").to_bytes().unwrap();
        for _ in 0..3 {
            maker.upload(&data, "").unwrap();
        }
        let (_, token) = anon.register("Pia").unwrap();
        let me = anon.clone().with_token(Some(token));
        assert!(anon.endless_status().is_err(), "needs an account");
        assert_eq!(me.endless_status().unwrap(), EndlessStatus::default());
        assert!(me.endless_next(Difficulty::Easy, 10).is_err(), "no run yet");

        let run = me.endless_start(Difficulty::Easy).unwrap();
        assert_eq!((run.lives, run.active), (5, true));
        let b = me.endless_next(Difficulty::Easy, 10).unwrap();
        assert_eq!(b.courses.len(), 3);
        assert!(b.widened, "fresh courses are unrated");
        // the client keeps the in-course counters and reports with Run::finish
        let mut r = Run::resume(Difficulty::Easy, &b.run);
        r.start_course();
        let first = b.courses[0].code.clone();
        let (_, rep) = r.finish(&first, Outcome::Death, 0);
        let a = me.endless_report(Difficulty::Easy, &rep).unwrap();
        assert_eq!((a.lives, a.game_over), (4, false));
        assert_eq!(a.lives, r.current.lives);
        r.retry_course();
        r.one_up(1);
        r.add_coins(10);
        let (_, rep) = r.finish(&first, Outcome::Clear, 120);
        let a = me.endless_report(Difficulty::Easy, &rep).unwrap();
        assert_eq!((a.lives, a.clears, a.best), (5, 1, 1));
        assert_eq!((a.lives, a.clears), (r.current.lives, r.current.clears));
        let b = me.endless_next(Difficulty::Easy, 10).unwrap();
        assert_eq!(b.courses.len(), 2);
        assert_eq!((b.run.coins, b.run.score), (10, 100));
        r.start_course();
        let (_, rep) = r.finish(&b.courses[0].code, Outcome::Skip, 0);
        assert_eq!(me.endless_report(Difficulty::Easy, &rep).unwrap().passed, 2);
        let h = me.endless_history(Difficulty::Easy).unwrap();
        assert_eq!(
            h.iter().map(|e| e.result).collect::<Vec<_>>(),
            [HistoryResult::Clear]
        );
        let s = me.endless_suspend(Difficulty::Easy).unwrap();
        assert!(s.can_continue());
        let st = me.endless_status().unwrap();
        assert_eq!(st.run(Difficulty::Easy).unwrap().clears, 1);
        assert_eq!(
            (st.skips, st.last, st.best[0]),
            (1, Some(Difficulty::Easy), 1)
        );
        // game over on the last course
        let last = me.endless_next(Difficulty::Easy, 10).unwrap().courses[0]
            .code
            .clone();
        let mut a = EndlessAnswer::default();
        while !a.game_over {
            let rep = Report {
                code: last.clone(),
                outcome: Some(Outcome::TimeUp),
                ..Report::default()
            };
            a = me.endless_report(Difficulty::Easy, &rep).unwrap();
        }
        let f = me.endless_finish(Difficulty::Easy).unwrap();
        assert_eq!((f.clears, f.best, f.new_high_score), (1, 1, true));
        assert!(me.endless_finish(Difficulty::Easy).is_err());
        assert_eq!(anon.rankings("endless_easy", 5).unwrap()[0].1, 1);
        let _ = std::fs::remove_dir_all(&dir);
    }
}
