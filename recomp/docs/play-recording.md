# Recorded play

`RECOMP_PLAY_RECORD=<dir>` records every course attempt played (by hand, or by a script) so it can be played again,
frame for frame: to look at what the game did, to profile the calls a real play makes, or as a long
regression check against another implementation of the game.

## Recording

```sh
SMM2_SEED=4 SMM2_SEED_ALL=1 RECOMP_PLAY_RECORD=~/smm-work/plays ./smm2recomp     # or set them in recomp.env
```

Record with **Quick level return off** (the Mods tab, or `RECOMP_QUICK_RETRY=0`). It shortens SMM2's own miss, so with it
on every death has another timing than the game's, and a replay stays in step only in a build whose mod does exactly the
same. `meta.json` keeps the setting (`"mods"`), and `tools/play_replay.py` replays with it and says so.

`SMM2_SEED` and `SMM2_SEED_ALL` are not required (the recording keeps the state the random generators had), but with
them every generator the course makes starts from a known seed. Play as usual; each course played is a folder (the next
course starts a new one; a miss and its restart stay in the same folder, and the replay restarts where the game did):

| File | What it holds |
|---|---|
| `course.dat` | the course as the loader took it (the .bcd's 0x10-byte header and the decrypted 0x5bfc0-byte body, from the save buffer's prepared slot: Coursebot's Play and a downloaded course both play from there); what `RECOMP_BOOT_COURSE` boots |
| `inputs.csv` | `frame,buttons,stick_lx,stick_ly,stick_rx,stick_ry`, one line per change; `frame` counts player updates from the attempt's first; `buttons` are nn::hid bits as the game read them (the stick directions, 16-23, included) |
| `inputs.pad` | the same as a `RECOMP_PAD_SCRIPT` (complete once the attempt has closed) |
| `player.csv` | `update,x,y,state`: the player at the start of every update (its place +0x230 / +0x234, its state +0x3f8) |
| `restarts.csv` | `restart,death,x,y,state,gap`: each restart after a miss (the update the new life starts at, the update the miss began at, the player there, the presented frames without an update before it) |
| `meta.json` | the course (style, name, game version, CRC), the seeds, the global and area generators' state at the first update (`play_random`), the pad held from before the course (`held_before`, `pad_held`), the start and the end (why, place, state, updates, `cleared`) |

The clock is the one pad scripts play on: script frame k is what the game reads between player update k and k + 1,
so frame k here is the last pad state the game read in that interval (games/smm2/playrec.c). `inputs.csv` and
`player.csv` are flushed every update and `meta.json` rewritten every 60, so a session that ends without a clean
quit keeps them.

What is left out, and why:
- Plus and Minus (the pause): the player is not updated while paused, so a script's clock cannot move through the
  pause menu; replayed, Plus would pause the replay with its clock stopped. The game does not move while paused.
- The title screen's course (it has a player of its own).
- Players 2-4.
- A course test-played from the Course Maker is recorded as the prepared slot holds it, which may not be the edit
  being played (meta.json says what was taken; play a saved course from Coursebot to be sure).

## Replaying

```sh
python3 tools/play_replay.py ~/smm-work/plays/<folder>            # watch it play again, at the game's own speed
python3 tools/play_replay.py ~/smm-work/plays/<folder> --check    # and check it plays the same, update for update
python3 tools/play_replay.py ~/smm-work/plays/<folder> --check --hidden --no-audio --speed fast   # ... out of the way
python3 tools/play_replay.py ~/smm-work/plays/<folder> --trace out.trc   # with the player trace (RECOMP_TRACE_OUT)
```

The ordinary build (`build/smm2`) replays, in its window and with its sound, at the game's own speed: a replay is
something to watch. `--hidden` makes the window without showing it (`LARECOMP_HIDDEN`), so the replay takes no focus,
`--no-audio` opens no audio device (`LARECOMP_NO_AUDIO`), and `--speed fast` leaves the presents unpaced
(`RECOMP_NO_DISPLAY_PACE`) so the course runs as quickly as the machine draws it - on a 120 Hz display twice the
game's speed. Those three are for a long `--check` run that nobody is watching. `--build` points at another build, a
headless one included.

The replay is the same at either speed: the clock the inputs play on is the player's own update count, not time. The
sound is paced by the presents, so `--speed fast` garbles it.

The replay boots the course directly (`RECOMP_BOOT_COURSE`, a build with the direct boot), plays the recorded inputs
as a pad script on the player-update clock, with the recording's seeds, its random state put back at the first
update (`RECOMP_PLAY_RANDOM`) and its held buttons held into the start (`RECOMP_PAD_HELD`). `--check` records the
replay too and compares: the same inputs, and the player at the same place in the same state at the start of every
recorded update.

Restarts. A death can take a few updates more or fewer in another build or on another host (a first recording's
restart at a checkpoint: 222 updates from the miss on macOS, 228 headless), while the player takes no input. So the
replay is re-synced at each restart (`RECOMP_PAD_SYNC=<restarts.csv>`: at each of its own restarts the script goes on
at the recorded restart's frame), and `--check` compares life by life, each life aligned at its start; a life whose
replay only stays down a few updates longer or shorter counts as alike, and the summary says by how much. A recording
without `restarts.csv` (made before it) has its restarts read from `player.csv`. `--whole` replays and compares
update for update without the re-sync.

A folder holds the whole course played, misses and restarts included, so a direct boot starts it at the course's
start, as the recording did; a restart at a checkpoint later in the folder replays as the game restarts there.
