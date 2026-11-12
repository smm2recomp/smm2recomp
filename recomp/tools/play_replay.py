"""Replay a recorded course attempt (RECOMP_PLAY_RECORD, games/smm2/playrec.c), and check it replays alike.

    python3 tools/play_replay.py <attempt folder> [--build build/smm2] [--data private/smm2] [--hidden]
                                 [--speed game|fast] [--no-audio] [--trace out.trc] [--check] [--keep-dir DIR]
                                 [--timeout 900]

A replay plays in the window with its sound, as the recording did, so it can be watched. --hidden makes the window
without showing it (LARECOMP_HIDDEN), so the replay takes no focus, and --no-audio opens no audio device
(LARECOMP_NO_AUDIO); both are what to pass for a run nobody is watching. A headless build (no window, no GPU) replays
the same way with --build pointing at it, and ignores them.

--speed game (the default) paces the presents as the console's display does, so the course replays at its own speed.
--speed fast leaves them unpaced (RECOMP_NO_DISPLAY_PACE) and the course runs as quickly as the machine draws it: on
a 120 Hz display twice the game's speed, on a headless build as fast as the CPU goes - worth it for a long --check
run, where nobody is watching. The replay itself is the same either way: the clock the inputs play on is the player's
own update count, not time - but the sound is paced by the presents, so --speed fast garbles it.

The run starts the recorded course with the direct boot (RECOMP_BOOT_COURSE: course.dat is the header and decrypted
body it takes), plays the recorded inputs on the player-update clock (RECOMP_PAD_SCRIPT, made from inputs.csv), with the recording's seeds
(SMM2_SEED, SMM2_SEED_ALL) and its two random generators' state put back at the first player update
(RECOMP_PLAY_RANDOM), the pad held from before the course as it was (RECOMP_PAD_HELD), and ends with the script.
--trace writes the player trace (RECOMP_TRACE_OUT, games/smm2/trace.c).

The build has to boot a course directly (RECOMP_BOOT_COURSE). Without it the game comes up on its title screen, the
script plays into the menus and the replay records nothing of the course; the tool says so.

--check records the replay itself (RECOMP_PLAY_RECORD into a scratch folder) and compares it with the recording: the
same inputs and the player at the same place in the same state at the start of every recorded update (player.csv;
the replay runs one update more: its script ends on the update after the last). Exit 0 when alike, 1 when not.

Restarts. A death can take a few updates more or fewer in another build (the player takes no input then), so the
replay is re-synced at each restart (RECOMP_PAD_SYNC: the recording's restarts.csv, or for a recording without one the
restarts read from player.csv: the first update in state 16 after a down state), and --check compares life by life:
each segment from one restart to the next, aligned at its start. A segment whose replay only stays down longer or
shorter at its end counts as alike (its death took another number of updates). --whole compares the updates as they
come instead, without the re-sync.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def game_lib(build: Path) -> Path | None:
    """The game code library of that build, whatever the host calls it (none: the runtime looks beside itself)."""
    return next((p for p in (build / f"smm2game.{s}" for s in ("dylib", "so", "dll")) if p.exists()), None)


def env_for(folder: Path, meta: dict, build: Path, hidden: bool, speed: str, audio: bool) -> dict:
    env = {
        "RECOMP_BOOT_COURSE": str(folder / "course.dat"),
        "RECOMP_PAD_EXIT": "1",
    }
    # An ordinary build makes a window, because the renderer needs one, and shows it unless asked not to. A headless
    # build has no window to hide and no sound to make, and ignores both of these.
    if hidden:
        env["LARECOMP_HIDDEN"] = "1"
    if not audio:
        env["LARECOMP_NO_AUDIO"] = "1"
    if speed == "fast":  # the presents unpaced: the course runs as quickly as the machine draws it
        env["RECOMP_NO_DISPLAY_PACE"] = "1"
    lib = game_lib(build)
    if lib:
        env["LARECOMP_GAME_LIB"] = str(lib)
    if meta.get("seed") is not None:
        env["SMM2_SEED"] = str(meta["seed"])
    if meta.get("seed_all"):
        env["SMM2_SEED_ALL"] = "1"
    if meta.get("play_random"):
        env["RECOMP_PLAY_RANDOM"] = meta["play_random"]
    if meta.get("pad_held"):
        env["RECOMP_PAD_HELD"] = meta["pad_held"]
    # Quick level return changes the timing of every miss (it shortens SMM2's own): the replay takes the recording's
    # setting, and it matches only where this build's mod does what the recording build's did.
    mods = meta.get("mods") or {}
    env["RECOMP_QUICK_RETRY"] = "1" if mods.get("quick_retry") else "0"
    if mods.get("quick_retry"):
        print(f"note: recorded with Quick level return on ({meta.get('recomp')}): misses replay the same only with this "
              "build's mod doing the same; record with it off for the game's own timing", file=sys.stderr)
    return env


BUTTONS = ["A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R", "ZL", "ZR", "PLUS", "MINUS", "LEFT", "UP", "RIGHT", "DOWN"]


def pad_from_csv(folder: Path, out: Path) -> int:
    """inputs.csv (one line per change) as a RECOMP_PAD_SCRIPT, as long as player.csv: the script, whatever became of
    inputs.pad (complete only once the attempt closed). Returns the frames."""
    changes = [ln.split(",") for ln in rows(folder / "inputs.csv")]
    frames = len(rows(folder / "player.csv"))
    lines = ["# made by tools/play_replay.py from inputs.csv"]
    for i, c in enumerate(changes):
        start = int(c[0])
        end = int(changes[i + 1][0]) if i + 1 < len(changes) else frames
        if end <= start:
            continue
        b = int(c[1], 0) & 0xFFFF  # (the stick directions, bits 16-23, come back from the sticks)
        names = [n for k, n in enumerate(BUTTONS) if b >> k & 1] or ["-"]
        sticks = [f"{ax}={v}" for ax, v in zip(("LX", "LY", "RX", "RY"), c[2:6]) if int(v)]
        lines.append(" ".join([str(end - start)] + names + sticks))
    out.write_text("\n".join(lines) + "\n")
    return frames


def rows(csv: Path) -> list[str]:
    return [ln.strip() for ln in csv.read_text().splitlines()[1:] if ln.strip()]


DOWN = range(113, 120)  # cState_DownPose .. cState_RestartWait (games/smm2/playrec.c)


def restarts_of(folder: Path) -> list[int]:
    """The updates each new life starts at: restarts.csv, else read from player.csv."""
    rst = folder / "restarts.csv"
    if rst.exists():
        return [int(r.split(",")[0]) for r in rows(rst)]
    out, down = [], False
    for r in rows(folder / "player.csv"):
        u, _, _, st = r.split(",")
        if int(st) in DOWN:
            down = True
        elif down and int(st) == 16:
            out.append(int(u))
            down = False
    return out


def segments(players: list[str], starts: list[int]) -> list[list[tuple]]:
    """player.csv rows (update, x, y, state) cut at each restart: one list of (x, y, state) per life."""
    cuts = [0] + starts + [len(players)]
    rowsx = [tuple(r.split(",")[1:]) for r in players]
    return [rowsx[cuts[i]:cuts[i + 1]] for i in range(len(cuts) - 1)]


def inputs_per_update(folder: Path, n: int) -> list[tuple]:
    """inputs.csv (one line per change) as one (buttons, sticks...) per update, n updates."""
    out, cur = [], None
    changes = [r.split(",") for r in rows(folder / "inputs.csv")]
    k = 0
    for u in range(n):
        while k < len(changes) and int(changes[k][0]) <= u:
            cur = tuple(changes[k][1:])
            k += 1
        out.append(cur)
    return out


def same_inputs_by_life(w: list[tuple], h: list[tuple], ws: list[int], hs: list[int]) -> tuple[bool, str]:
    """The inputs life by life over each life's common length (a longer death in the replay reads on into the next
    life's frames until its restart re-syncs it: the player takes no input then)."""
    wc, hc = [0] + ws + [len(w)], [0] + hs + [len(h)]
    for i in range(len(wc) - 1):
        if i + 1 >= len(hc):
            return False, f"life {i} missing"
        a, b = w[wc[i]:wc[i + 1]], h[hc[i]:hc[i + 1]]
        n = min(len(a), len(b))
        # a life's last updates are its death: input there is not read
        k = next((j for j in range(n) if a[j] != b[j]), None)
        if k is not None:
            return False, f"life {i} at +{k}"
    return True, ""


def compare_lives(want: list[list[tuple]], have: list[list[tuple]]) -> tuple[bool, list[str]]:
    """Each life of the recording against the replay's, aligned at its start."""
    lines, alike = [], True
    for i, w in enumerate(want):
        if i >= len(have):
            lines.append(f"life {i}: missing in the replay")
            alike = False
            continue
        h = have[i]
        n = min(len(w), len(h))
        first = next((k for k in range(n) if w[k] != h[k]), None)
        last_life = i == len(want) - 1
        tail = (w if len(w) > len(h) else h)[n:]
        tail_down = all(int(r[2]) in DOWN for r in tail)
        if first is not None:
            lines.append(f"life {i} ({len(w)} updates): differs at +{first}: recorded {w[first]}, replayed {h[first]}")
            alike = False
        elif len(w) == len(h) or (last_life and len(h) >= len(w)):
            lines.append(f"life {i} ({len(w)} updates): the same")
        elif tail_down:
            lines.append(f"life {i} ({len(w)} updates): the same; its death took {len(h) - len(w):+d} updates in the replay")
        else:
            lines.append(f"life {i} ({len(w)} updates): the same for {n}, then {len(h)} against {len(w)} updates")
            alike = False
    return alike, lines


def main() -> int:
    here = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("folder", type=Path)
    ap.add_argument("--build", type=Path, default=here / "build" / "smm2",
                    help="the build to replay with (default build/smm2; a headless build works too)")
    ap.add_argument("--data", type=Path, default=here / "private" / "smm2")
    ap.add_argument("--hidden", action="store_true", help="make the window without showing it (default: shown, so "
                                                          "the replay can be watched)")
    ap.add_argument("--no-audio", action="store_true", help="open no audio device (default: the course plays with "
                                                            "its sound)")
    ap.add_argument("--speed", choices=("game", "fast"), default="game",
                    help="game (the default): the console's own pace, the course at its real speed; fast: unpaced, as "
                         "quickly as the machine draws it")
    ap.add_argument("--trace", type=Path)
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--whole", action="store_true", help="no re-sync at restarts; --check compares the updates as they come")
    ap.add_argument("--keep-dir", type=Path, help="with --check: record the replay here (default a scratch folder)")
    ap.add_argument("--timeout", type=int, default=900)
    a = ap.parse_args()

    folder = a.folder.resolve()
    meta = json.loads((folder / "meta.json").read_text())
    if not meta.get("course_found") or not (folder / "course.dat").exists():
        print(f"{folder}: no course.dat (meta.json course_found false): nothing to boot", file=sys.stderr)
        return 2
    build = a.build.resolve()
    exe = build / "smm2recomp"
    if not exe.exists():
        print(f"{exe}: no runtime there (--build)", file=sys.stderr)
        return 2
    env = env_for(folder, meta, build, a.hidden, a.speed, not a.no_audio)
    if a.trace:
        env.update({"RECOMP_TRACE_OUT": str(a.trace.resolve()), "RECOMP_TRACE_EXIT": "1"})
    scratch = None
    if a.check:
        scratch = a.keep_dir.resolve() if a.keep_dir else Path(tempfile.mkdtemp(prefix="playrec-"))
        scratch.mkdir(parents=True, exist_ok=True)
        env["RECOMP_PLAY_RECORD"] = str(scratch)
    log = (scratch or folder) / "replay.log"
    pad = (scratch or folder) / "replay.pad"
    frames = pad_from_csv(folder, pad)
    env["RECOMP_PAD_SCRIPT"] = str(pad)
    starts = restarts_of(folder)
    if starts and not a.whole:
        sync = (scratch or folder) / "replay.sync"
        sync.write_text("restart\n" + "".join(f"{u}\n" for u in starts))
        env["RECOMP_PAD_SYNC"] = str(sync)
    print(f"{folder.name}: {frames} updates, {len(starts)} restarts"
          + (" (re-synced at each)" if starts and not a.whole else "")
          + (" (unpaced)" if a.speed == "fast" else ""))
    with open(log, "w") as out:
        r = subprocess.run([str(exe), "extracted"], cwd=a.data.resolve(), env={**os.environ, **env}, stdout=out,
                           stderr=subprocess.STDOUT, timeout=a.timeout)
    print(f"replay exited {r.returncode} ({log})")
    if not a.check:
        return 0 if r.returncode == 0 else 1
    got = sorted(p for p in scratch.iterdir() if p.is_dir())
    if not got:
        print(f"the replay recorded no attempt: this build may not boot a course directly (RECOMP_BOOT_COURSE), so the "
              f"script played into the title screen instead. {log} says what it did.", file=sys.stderr)
        return 1
    again = got[0]
    m2 = json.loads((again / "meta.json").read_text())
    n = len(rows(folder / "player.csv"))
    # Frame k is the last pad state read between update k and k + 1: the recording's last frame (n - 1) is read after
    # its last update, by none, and the replay's script has ended by then. Compared: the frames an update read.
    read = n - 1
    want = [r for r in rows(folder / "inputs.csv") if int(r.split(",")[0]) < read]
    have = [r for r in rows(again / "inputs.csv") if int(r.split(",")[0]) < read]
    same_inputs = want == have
    p1, p2 = rows(folder / "player.csv"), rows(again / "player.csv")
    if starts and not a.whole:
        hs = restarts_of(again)
        same_play, lines = compare_lives(segments(p1, starts), segments(p2, hs))
        same_inputs, where = same_inputs_by_life(inputs_per_update(folder, read), inputs_per_update(again, min(len(p2), read)),
                                                 starts, hs)
        print(f"inputs: {'the same in every life' if same_inputs else 'differ (' + where + ')'}")
        for ln in lines:
            print("  " + ln)
        print(f"player: {'the same in all ' + str(len(starts) + 1) + ' lives' if same_play else 'differs'}")
        if not a.keep_dir and same_inputs and same_play:
            shutil.rmtree(scratch, ignore_errors=True)
        return 0 if same_inputs and same_play else 1
    first = next((i for i, (u, v) in enumerate(zip(p1, p2)) if u != v), None)
    same_play = first is None and len(p2) >= len(p1)
    print(f"inputs: {'the same' if same_inputs else 'differ'} ({len(want)} and {len(have)} changes); "
          f"player: {'the same at all ' + str(len(p1)) + ' updates' if same_play else 'differs'}"
          + (f" (first at update {first}: recorded {p1[first]}, replayed {p2[first]})" if first is not None else "")
          + (f" (the replay has {len(p2)} of {len(p1)} updates)" if first is None and len(p2) < len(p1) else ""))
    if not a.keep_dir and same_inputs and same_play:
        shutil.rmtree(scratch, ignore_errors=True)
    return 0 if same_inputs and same_play else 1


if __name__ == "__main__":
    sys.exit(main())
