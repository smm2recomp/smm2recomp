# SMM2 Recomp

A native port of **Super Mario Maker 2** (v3.0.3) for macOS, iOS / iPadOS, Windows, Linux and
Android, made by **static recompilation**. The game's AArch64 code is translated to C ahead of time
and compiled for your platform. A native runtime stands in for the Switch's system services, and
the game's graphics run on Metal or Vulkan. Nothing is emulated at run time.

On top of the original game it adds:

- **The 100 Mario Challenge from Super Mario Maker 1**, with SMM1's difficulty select, world map,
  start card and ending, next to SMM2's own Endless Challenge
- **Course World without Nintendo's servers**, through a self-hostable open-source course server,
  courses from [TGRCode](https://tgrcode.com/mm2)'s public archive, or your own
  [OpenCourse World](https://opencourse.world/) account
- **SMM1 features as mods**, each with its own switch: Mystery Mushroom costumes, Lanky Mario (the Weird
  Mushroom), mid-air shell jumps, overlapping pipes, and "Try it now" buttons to try them in a course
- **Co-op in Endless and 100 Mario**, **Super Worlds** in Course World, and **editing any course** you
  downloaded

> **You need your own copy of the game.** This repository and its downloads contain no code,
> assets, text, keys or recompiled output from Super Mario Maker 2 or Super Mario Maker. You point
> the program at your own dump, and everything made from it is generated on your machine.

## Play it

A download is a **launcher plus the recompiler**, not the game: on first start it takes your dump,
checks it, recompiles the game code on your computer, and then runs it.

1. **Download** the package for your system from
   [Releases](https://github.com/smm2recomp/smm2recomp/releases):

   | System | Package | Also needs |
   |---|---|---|
   | Windows 10 / 11, x64 | `SMM2Recomp-windows-x86_64.zip` | a CPU with x86-64-v2 (AVX2 used when available), a Vulkan 1.3 graphics driver ([docs/windows.md](docs/windows.md)) |
   | Linux, x64 or arm64 | `SMM2Recomp-linux-x86_64.tar.gz`, `-linux-aarch64.tar.gz` | SDL3 3.2+ and a Vulkan 1.3 driver from your distribution (Debian 13, Ubuntu 25.04, Fedora 41, Arch or newer); x64: x86-64-v2 (AVX2 used when available) |
   | macOS 15+, Apple silicon | `SMM2Recomp-macos-arm64.zip` (`Super Mario Maker 2.app`) | nothing; the app isn't notarized: right-click > Open the first time (macOS then checks the whole app once, which can take a few minutes) |
| macOS 15+, Intel (x86-64) | no package yet: build it from source (see [Building](#building)) | a CPU with AVX2 (Intel Haswell / 2013 or newer); Homebrew `sdl3` |

2. **Unpack it and start it** (`smm2recomp.exe`, `./smm2recomp` or `Super Mario Maker 2.app`). Windows
   keeps everything in the unpacked folder, so put it somewhere writable.
3. **Point it at your dump.** The setup screen asks for:
   - **Super Mario Maker 2 with the 3.0.3 update:** the game's NSP or XCI and the update's NSP,
     or folders with their extracted ExeFS and RomFS. The version is checked against the known
     3.0.3 files (build ids and hashes of your own files).
   - **Your console's `prod.keys` and firmware** (the folder of its system `.nca` files): the
     game's fonts come from there. If an emulator already has them, they're filled in. They're
     read where they are, never copied, and key values are never shown or logged.
   - Optionally **Super Mario Maker (Wii U)**, for the SMM1 mods.
4. **Set up and play.** The first time it extracts and lays out your files, copies the fonts and
   recompiles the game code with the bundled compiler: a few minutes to half an hour. Then the
   game starts, and later starts go straight to it.

**If something goes wrong,** please open an [issue](https://github.com/smm2recomp/smm2recomp/issues)
and attach the log files from the program's data folder: `smm2recomp.log` (the program) and
`logs/setup.log` (the setup's steps). Neither contains key values. After a crash, the next start keeps the crashed
session's log as `smm2recomp.previous.log`: attach that one. The data folder is:

| System | Data folder |
|---|---|
| Windows | the unpacked folder, next to `smm2recomp.exe` |
| Linux | `~/.local/share/smm2recomp` (or `$XDG_DATA_HOME/smm2recomp`) |
| macOS | `~/Library/Application Support/org.smcraft.smm2recomp` |

The setup screen tells you when a newer release is out and links to it; nothing is downloaded by
itself. Android and iOS / iPadOS have no download: they're built yourself from source
([recomp/README.md](recomp/README.md)); iOS needs a Mac to build, sign and install the app.

## Screenshots

| Title screen | Playing the title course | Make / Play |
|---|---|---|
| ![Title screen](docs/screenshots/title.jpg) | ![Mario running the title course](docs/screenshots/title-course.jpg) | ![Make and Play menu](docs/screenshots/menu.jpg) |

**100 Mario Challenge**

| Difficulty select | World map | Course-in card |
|---|---|---|
| ![100 Mario Challenge difficulty select](docs/screenshots/100mario-select.jpg) | ![Super Expert world map with Bowser's castle](docs/screenshots/100mario-map.jpg) | ![SMM2's course-in card with 100 lives](docs/screenshots/100mario-card.jpg) |

| Ending | Credits |
|---|---|
| ![Peach thanks Mario](docs/screenshots/100mario-ending.jpg) | ![Credits: each cleared course as an SMM2 course card with its course ID and maker](docs/screenshots/100mario-credits.jpg) |

## Status

**Early development.** Release packages for Windows, Linux and macOS are on the
[Releases](https://github.com/smm2recomp/smm2recomp/releases) page; iOS / iPadOS and Android are built from
source.

| Platform | Graphics | Status |
|---|---|---|
| **macOS** (Apple Silicon) | Metal | Plays courses. Input replays stay frame-for-frame in sync with the game running in Ryujinx, and in-course frames are pixel-identical in 71 of 90 style × theme combinations. The reference platform. The player app goes from an empty data folder through its setup screen and the first-launch recompile to the title screen. |
| **macOS** (Intel, x86-64) | Metal | Builds and runs natively on an Intel Mac: the game code uses the portable-C lowering (as on Windows / Linux x64) and the SIMD differential tests pass. Booted to the title at 60 fps and into Course World on a Metal 2-only GPU (NVIDIA GT 755M), which needs the driver limits in [PROGRESS.md](PROGRESS.md) worked around (no ASTC textures, no packed 16-bit colour textures, no swizzle views, no Metal binary archives). MetalFX (the upscaler) needs Apple silicon or a supported GPU: the app falls back to bilinear scaling. |
| **iOS / iPadOS** (arm64) | Metal | Plays at 60 fps on iPad Pro, with an on-screen controller and touch. Built on a Mac and installed on your own device. |
| **Windows** (x64) | Vulkan | Testers play the release package; their reports are in [Issues](https://github.com/smm2recomp/smm2recomp/issues). See [docs/windows.md](docs/windows.md). |
| **Linux** (x64, arm64) | Vulkan | Testers play the release packages (a Steam Deck runs it, slowly so far); arm64 boots to the title screen in a VM. |
| **Android** (arm64, Android 13+) | Vulkan | Builds an APK on your computer from your dump; boots and plays the title course in the Android emulator. Not yet tested on a real device. |

| Feature | Status |
|---|---|
| Title, menus, course play, Course Maker test play | Working; fidelity work ongoing |
| 100 Mario Challenge (SMM1) | Playable start to finish, ending included; co-op with more controllers |
| Endless Challenge (SMM2) | Playable against the course server; co-op with more controllers |
| Course World (lists, search, download, play, leaderboards, uploads, Super Worlds) | Working against the course server; likes and comments in progress |
| Course World against your own OpenCourse World account | Working on macOS: browsing, playing, likes, comments and uploads ([recomp/docs/mods.md](recomp/docs/mods.md)) |
| Edit any course (as in SMM1) | Working |
| SMM1 feature switches | Costumes, Lanky Mario, shell jumps, overlapping pipes |
| Online multiplayer | Through [Nextendo](https://nextendo.network) (sign-in); new, not yet tested on hardware |

[PROGRESS.md](PROGRESS.md) has the detailed checklist and the evidence behind each item, and
[DECISIONS.md](DECISIONS.md) records the design decisions and why they were made.

## How it works

1. **Recompile.** The recompiler (`recomp/tools/`, Python) reads the game's executable from your
   dump and translates every function to C.
2. **Build.** That C is compiled together with the runtime (`recomp/runtime/`, C and
   Objective-C), which implements the Switch's kernel, system services, audio, input and the NVN
   graphics API on Metal or Vulkan.
3. **Play.** The result is an ordinary native app that loads the game's assets straight from your
   dump.

Mods hook into the recompiled game at chosen functions. The course server (`core/`, Rust) answers
the requests the game would normally send to Nintendo.

The recompiler is a fork of [LAS Recomp](https://github.com/peiikai/LASRecomp) (MIT).

## Building

### What you need

- A dump of **Super Mario Maker 2**: the base game RomFS plus the **v3.0.3 update** ExeFS and
  RomFS, extracted
- For the 100 Mario Challenge, a dump of **Super Mario Maker** (Wii U)
- macOS on Apple silicon or Intel (x86-64, with AVX2) with the Xcode command-line tools, Homebrew
  `sdl3` and `ffmpeg`, and Python 3.12 or newer
- About 2 GB of free disk space

### macOS

```sh
make -C recomp venv        # once: the recompiler's Python environment
make -C recomp smm2        # verify the dump, recompile it to C, build (10–20 min the first time)
make -C recomp run-smm2    # play
```

**Controls:** a controller works out of the box. On the keyboard:

| Keys | Switch button |
|---|---|
| WASD | left stick |
| arrow keys | D-pad |
| K / J / L / I | A / B / X / Y |
| Q / E | L / R |
| 1 / 3 | ZL / ZR |
| Enter / Backspace | + / − |
| F1 or Esc | settings menu |
| F11 | full screen |

### Other platforms

- **Windows:** cross-compiled from macOS into a package that recompiles the game from your dump on
  first launch. See [docs/windows.md](docs/windows.md).
- **iOS / iPadOS, Linux, Android:** see [recomp/README.md](recomp/README.md).

### The player packages

```sh
make -C recomp GAME=smm2 app-macos                                  # dist/SMM2Recomp-macos-arm64.zip
recomp/tools/fetch_zig.sh && recomp/tools/fetch_deps.sh windows-x86_64
make -C recomp TARGET=windows-x86_64 GAME=smm2 DEPS="$PWD/recomp/build/deps-windows-x86_64" package
```

Each package is checked for anything from a game dump before it is archived
(`scripts/check_package.py`). `.github/workflows/release.yml` builds all of them when a `v*` tag
is pushed and attaches them to the release; details in [recomp/README.md](recomp/README.md).

## The course server

Course World, the Endless Challenge and the 100 Mario Challenge talk to a self-hostable
open-source server instead of Nintendo's. The server holds accounts, courses, leaderboards and
challenge runs, and can also pull courses from TGRCode's archive. On iOS a copy runs inside the
app, so no setup is needed.

To run your own:

```sh
cd core
cargo run --release -p mmk-cli -- server --dir ./server-data --addr 0.0.0.0:8640
cargo run --release -p mmk-cli -- register Ada --server http://localhost:8640   # prints a token
cargo run --release -p mmk-cli -- upload --token <token> courses/*.bcd --keys <course key table>
```

[docs/self-hosting.md](docs/self-hosting.md) covers ports, HTTPS, accounts and moderation. The API
is documented at the top of `core/crates/server/src/lib.rs`.

## Repository layout

| Path | Contents |
|---|---|
| `recomp/` | the recompiler, the runtime and SMM2's game-specific code and mods ([recomp/README.md](recomp/README.md)) |
| `core/` | the course server and shared course logic (Rust) |
| `smmfmt/`, `tests/` | a Python reference implementation of the course file formats |
| `docs/` | course file formats, toolchain, self-hosting, Windows |

## Contributing

```sh
scripts/install-hooks.sh     # pre-commit check that blocks proprietary files
scripts/check.sh             # everything CI runs: server fmt / clippy / tests, recomp compile check, proprietary-file check
```

Never commit game files or anything generated from them. Generated output stays in the gitignored
`recomp/private/`, `recomp/generated/` and `recomp/build/`. `scripts/check_no_proprietary.py` runs
in CI and in the pre-commit hook to enforce this.

## Legal

This is an unofficial fan project, not affiliated with or endorsed by Nintendo. Super Mario Maker,
Super Mario Maker 2 and related names are trademarks of Nintendo. No Nintendo code or assets are
distributed here. You need a legally obtained copy of the game, and you build everything from it
yourself.

## License

SMM2 Recomp is licensed under the [GNU General Public License v3.0 or later](LICENSE).

It includes code under other licenses, which keep their own terms:

- the recompiler started as a fork of [LAS Recomp](https://github.com/peiikai/LASRecomp) (MIT, [recomp/LICENSE-LASRecomp](recomp/LICENSE-LASRecomp))
- the third-party libraries in `recomp/third_party/` each have their own license file (Dear ImGui, SMAA, bcdec, Mbed TLS and others)

[recomp/UPSTREAM.md](recomp/UPSTREAM.md) lists each upstream project and its license.

The license covers this project's own code only. It grants no rights to Nintendo's games, code or
assets, or to anything generated from your dump.
