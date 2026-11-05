# iPhone / iPad Builder in the Mac App Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The macOS player app gets a "Build for iPhone / iPad" action that turns the player's own dump into an unsigned `.ipa`, which the player then signs and installs with a tool of their choice (issue #69).

**Architecture:** The release CI builds an **iOS kit** with no game code (the runtime as a static library, SDL3 and the course server for iOS, link flags, Info.plist, icons, entitlements) and ships it inside the Mac app. On the player's Mac, the app runs the generator it already ships, compiles the generated game code with Xcode's iOS compiler, links it with the kit, lays out `Payload/SMM2Recomp.app` (optionally with the game files), signs it ad hoc with the two memory entitlements and zips it into an `.ipa`. The same engine runs from the setup screen and from a command-line flag.

**Tech Stack:** C / Objective-C++ (runtime, Dear ImGui setup screen), Make, shell, Xcode command-line tools (`xcrun --sdk iphoneos clang`, `codesign`, `ditto`), GitHub Actions (macos-15).

**Spec:** this plan's own "Spec" section below (the decisions agreed with the owner on 2026-11-05; issue #69; research in the conversation: nxnative's approach, the pipeline map).

## Spec

- nxnative style: the Mac app makes an **unsigned (ad-hoc signed) `.ipa`**; the player chooses how to sign and install it (AltStore, SideStore, Sideloadly, LiveContainer, Xcode). We never handle their Apple account or device.
- **Xcode is required** on the player's Mac (the iOS SDK can't be shipped). Apple silicon Macs only in this version.
- Game code is generated and compiled **only on the player's Mac**, from their dump. Nothing with game code is ever shipped or uploaded.
- The app carries the entitlements `com.apple.developer.kernel.extended-virtual-addressing` and `com.apple.developer.kernel.increased-memory-limit`. Whether the player's signing keeps them is up to their account and tool; the docs say so plainly.
- Game files: included in the app by default (about 3 GB, the app links them into Documents at each launch), or left out (the player copies them into the app's Documents folder with Finder / the Files app).
- Mac + GitHub only; the owner's private icon is never used for this (the kit uses `docs/branding/ios`).

## Global Constraints

- iOS minimum: `17.0`; target triple `arm64-apple-ios17.0`; bundle id `org.smcraft.smm2recomp`; executable and app name `SMM2Recomp`.
- Kit folder in the Mac app: `Contents/Resources/ios-kit/`. Kit build output in a checkout: `recomp/build/ios-kit/`.
- The kit must contain no game code: `nm` of `libsmm2rt.a` has no `_f_7` symbol (as `tools/bundle_macos.sh:35` checks the Mac executable), and `scripts/check_package.py` passes on the Mac zip.
- The game code compiled on the player's Mac must have the kit's `GAMELIB_ID`; a mismatch refuses to build.
- Work folder on the player's Mac: `<data root>/ios-build/` (removed at the end, on failure and on quit). Default output: `~/Downloads/SMMaker2-<app version>.ipa` (replaced if it exists).
- Never commit or upload an `.ipa`, generated code or game files. Test output goes to `/Volumes/SSD`.
- Commits end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`; `scripts/check.sh` exit code gates each commit (`&&`, never piped).

## Review Focus

1. Xcode missing, or installed without the iOS platform: the button is disabled with "Install Xcode from the App Store, open it once, and add iOS under Xcode > Settings > Components" (Task 4 test `iosbuild_available` with `DEVELOPER_DIR` pointing at an empty folder).
2. Not enough disk space (game files ~3 GB in the app, plus the build): checked before starting; needs 8 GB free on the data volume, or 3 GB with game files left out (Task 4 test with `IOSBUILD_TEST_FREE_GB=1`).
3. Quitting or cancelling mid-build: compilers are killed and `ios-build/` is removed; no half-written `.ipa` is left (Task 4 test: `--build-ios` killed after the generate step, then the folder and output checked).
4. A kit from another build (`GAMELIB_ID` differs from the app's): refused with "This app's iPhone kit doesn't match it: reinstall the app" (Task 4 test with a doctored `kit.json`).
5. The game not set up yet (no `extracted/exefs/main`): the button is disabled with "Set up the game first" (Task 5 check).

---

## File Structure

| File | Responsibility |
|---|---|
| `recomp/tools/build_deps_ios.sh` (new) | Builds static SDL3 3.4.18 for iOS device into `build/deps-ios/` |
| `recomp/Makefile` (modify) | `ios-kit` target: runtime archive + kit layout; `IMGUI_OBJ` added to `runtime-objs` |
| `recomp/tools/ios_bundle_lib.sh` (new) | Info.plist and icon steps shared by `make_ios_app.sh` and `make_ios_kit.sh` |
| `recomp/tools/make_ios_kit.sh` (new) | Lays out `build/ios-kit/` (libs, `link.txt`, `game_cflags.txt`, Info.plist, icons, entitlements, `kit.json`) |
| `recomp/tools/test_ios_kit.sh` (new) | Checks a kit: no game code, iOS platform, links with test objects |
| `recomp/tools/bundle_macos.sh` (modify) | Copies `build/ios-kit/` into `Contents/Resources/ios-kit/` when present |
| `.github/workflows/release.yml` (modify) | macOS job builds the iOS deps, the iOS course server and the kit before `app-macos` |
| `recomp/runtime/gamebuild.c`, `gamebuild.h` (modify) | Generate + compile split out as `gamebuild_compile()` so a second compiler can use it |
| `recomp/runtime/iosbuild.c`, `iosbuild.h` (new, macOS only) | The iPhone build engine: checks, compile, link, bundle, sign, zip |
| `recomp/tools/test_ios_ipa.sh` (new) | Checks an `.ipa`: layout, platform, entitlements, game files present or not |
| `recomp/runtime/main.c` (modify) | `--build-ios [--no-game-files] [out.ipa]` |
| `recomp/runtime/ui/launcher.mm` (modify) | The setup screen's "iPhone / iPad" section |
| `README.md`, `recomp/README.md`, `docs/ios.md` (new) | Player instructions |

---

### Task 1: Static SDL3 for iOS

**Files:**
- Create: `recomp/tools/build_deps_ios.sh`

**Interfaces:**
- Produces: `recomp/build/deps-ios/lib/libSDL3.a`, `recomp/build/deps-ios/include/SDL3/*.h` (same SDL version and checksum as `tools/build_deps_macos.sh:26-27`). `make TARGET=ios-arm64 DEPS=build/deps-ios` uses it (the Makefile's `SDL := $(or $(DEPS),...)`).

- [ ] **Step 1:** Write `build_deps_ios.sh` modelled on `build_deps_macos.sh`: same `fetch` of `SDL3-3.4.18.tar.gz` with its sha256, cmake with `-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF`, install prefix `build/deps-ios`; skipped when `lib/libSDL3.a` exists.
- [ ] **Step 2:** Run `tools/build_deps_ios.sh`. Expected: `lipo -info build/deps-ios/lib/libSDL3.a` says `arm64`, and `otool -l` of one extracted member shows `platform 2` (iOS) in `LC_BUILD_VERSION`.
- [ ] **Step 3:** Build the iOS app with it to prove it's a drop-in: `make TARGET=ios-arm64 GAME=smm2 DEPS=build/deps-ios` (the local generated iOS game code exists). Expected: links with no undefined symbols.
- [ ] **Step 4:** `scripts/check.sh && git commit` ("iOS: build static SDL3 for iOS (build_deps_ios.sh)").

### Task 2: The iOS kit

**Files:**
- Modify: `recomp/Makefile` (`runtime-objs` gains `$(IMGUI_OBJ)`; new `ios-kit` target next to `app` at ~line 434)
- Create: `recomp/tools/make_ios_kit.sh`, `recomp/tools/test_ios_kit.sh`, `recomp/tools/ios_bundle_lib.sh`
- Modify: `recomp/tools/make_ios_app.sh` (its Info.plist and icon parts move into `ios_bundle_lib.sh`)

**Interfaces:**
- Consumes: Task 1's `build/deps-ios`.
- Produces: `make TARGET=ios-arm64 GAME=smm2 DEPS=build/deps-ios ios-kit` → `recomp/build/ios-kit/` containing:
  - `lib/libsmm2rt.a`: `RT_OBJ MBEDTLS_OBJ BACKEND_OBJ UI_OBJ IMGUI_OBJ` for iOS (includes `rt/gamelib.o`, built with `GAMELIB_STATIC`)
  - `lib/libSDL3.a`; `lib/libmmk_embed.a` when `../core/target/aarch64-apple-ios/release/libmmk_embed.a` exists
  - `link.txt`: one argument per line, the iOS `LDFLAGS` the Makefile links the app with (frameworks, `-lc++`, ...), without the SDL/kit library paths
  - `game_cflags.txt`: one argument per line, the flags the Makefile compiles generated game code and `gamelib_entry.c` with for iOS (`GENFLAGS`, `GAMEDEFS`, the iOS `OSFLAGS`), without `-I` paths and without `-target`
  - `Info.plist`: as `make_ios_app.sh` writes it, with `CFBundleShortVersionString` = the app version (`git describe --tags`, without `v`) and `CFBundleVersion` = `git rev-list --count HEAD`
  - `icons/`: `Assets.car` and the `AppIcon*.png` files, from `actool` over `docs/branding/ios` (as `make_ios_app.sh:68-93`), plus the Info.plist keys they need already merged into `Info.plist`
  - `entitlements.plist`: both kernel entitlements set to `true`
  - `kit.json`: `{"gamelib_id": "<GAMELIB_ID>", "ios_min": "17.0", "version": "<app version>"}`
- `test_ios_kit.sh <kit dir> [<dir of iOS game objects>]`: exit 0 when the kit is complete, has no `_f_7` symbol, every member of `libsmm2rt.a` is platform iOS, and (with the second argument) `xcrun --sdk iphoneos clang++ -target arm64-apple-ios17.0` links those objects + the kit + `@link.txt` into an executable.

- [ ] **Step 1:** Write `test_ios_kit.sh` with the checks above.
- [ ] **Step 2:** Run it on a missing kit: `tools/test_ios_kit.sh build/ios-kit`. Expected: FAIL ("no build/ios-kit/lib/libsmm2rt.a").
- [ ] **Step 3:** Add `$(IMGUI_OBJ)` to `runtime-objs`; add `ios-kit` (errors unless `TARGET=ios-arm64`): depends on those objects, archives them with `xcrun ar rcs`, then runs `tools/make_ios_kit.sh` with the variables it needs (`GAMELIB_ID`, `LDFLAGS`, the game C flags, `SDL`, `EMBED_LIB`). `make_ios_kit.sh` writes the files listed above; it shares the Info.plist and icon code with `make_ios_app.sh` by moving those two parts into functions in a new `tools/ios_bundle_lib.sh` that both source.
- [ ] **Step 4:** Run `make TARGET=ios-arm64 GAME=smm2 DEPS=build/deps-ios ios-kit && tools/test_ios_kit.sh build/ios-kit build/smm2-ios-arm64/obj`. Expected: PASS, and an executable linked.
- [ ] **Step 5:** `make TARGET=ios-arm64 GAME=smm2 DEPS=build/deps-ios app` still builds the same app as before (make_ios_app.sh refactor): `codesign` unchanged path, Info.plist keys identical (`plutil -p` diff against the previous build's).
- [ ] **Step 6:** `scripts/check.sh && git commit` ("iOS kit: the runtime for iPhone / iPad without game code (make ios-kit)").

### Task 3: The kit in the Mac app and the release

**Files:**
- Modify: `recomp/tools/bundle_macos.sh`, `.github/workflows/release.yml` (macOS job, ~lines 74-115)

**Interfaces:**
- Consumes: Task 2's `build/ios-kit/`.
- Produces: `Super Mario Maker 2.app/Contents/Resources/ios-kit/` (absent when the checkout has no kit: the app then has no iPhone section, Task 5).

- [ ] **Step 1:** `bundle_macos.sh`: copy `build/ios-kit/` to `Contents/Resources/ios-kit/` when it exists; run `tools/test_ios_kit.sh` on the copy and fail the bundle if it fails.
- [ ] **Step 2:** `release.yml` macOS job, before `app-macos`: `sudo xcode-select -s /Applications/Xcode.app` (whichever Xcode the image defaults to), `xcrun --sdk iphoneos --show-sdk-path` (fails the job clearly if missing), `tools/build_deps_ios.sh`, `make TARGET=ios-arm64 GAME=smm2 embed-server`, `make TARGET=ios-arm64 GAME=smm2 DEPS=build/deps-ios ios-kit`.
- [ ] **Step 3:** Locally: `make GAME=smm2 app-macos`, then `ls "<app>/Contents/Resources/ios-kit"` lists the kit and `python3 scripts/check_package.py <the zip>` passes.
- [ ] **Step 4:** `scripts/check.sh && git commit` ("Mac app: carry the iOS kit; release CI builds it"). The workflow change is first proven by the next release's tag (as every release.yml change is).

### Task 4: The build engine and `--build-ios`

**Files:**
- Modify: `recomp/runtime/gamebuild.c`, `recomp/runtime/gamebuild.h`, `recomp/runtime/main.c`, `recomp/Makefile` (iosbuild.c in the macOS runtime sources only; `ios-build/` sits in the data root, so `paths.c` `GAME_FILES` is unchanged)
- Create: `recomp/runtime/iosbuild.c`, `recomp/runtime/iosbuild.h`, `recomp/tools/test_ios_ipa.sh`

**Interfaces:**
- Consumes: the kit (Task 2 layout), `rt_resource()` (`runtime/paths.c:161`) to find `ios-kit`, `gamelib-src`, the generator.
- Produces:
  ```c
  // gamebuild.h: generate main + sdk and compile every chunk with cc[] (NULL-terminated argv prefix) + cflags[]
  // into work/; objects kept, listed in *objs (malloc'd, NULL-terminated). The progress, cancel and child-process
  // handling of gamebuild_run. gamebuild_run itself becomes gamebuild_compile + its link.
  bool gamebuild_compile(const char *game_dir, const char *work, const char *const *cc, const char *const *cflags,
                         GameBuildOpts opts, char ***objs, char *why, size_t n);
  void gamebuild_cancel(void);  // kills the children of a running gamebuild_compile / iosbuild_run

  // iosbuild.h (macOS only)
  typedef struct {
      bool game_files;     // copy extracted/{exefs,romfs}, fonts, sysdata into the app's data/
      const char *out;     // the .ipa to write (replaced)
      void (*progress)(const char *step, float frac);  // steps: "Checking", "Building the game code",
                                                      // "Linking", "Copying the game files", "Signing", "Packing"
  } IosBuildOpts;
  bool iosbuild_available(char *why, size_t n);  // false + the reason a player can act on (Review Focus 1, 4; Intel)
  bool iosbuild_run(const char *data_root, const IosBuildOpts *o, char *why, size_t n);
  ```
- CLI: `smm2recomp --build-ios [--no-game-files] [<out.ipa>]` prints each step, exit 0 on success, 1 with the reason on stderr.
- `test_ios_ipa.sh <ipa> with-files|no-files`: exit 0 when the zip has `Payload/SMM2Recomp.app/SMM2Recomp`, `Info.plist` with bundle id `org.smcraft.smm2recomp`, `otool -l` platform 2, `codesign -d --entitlements -` lists both entitlements, and `Payload/SMM2Recomp.app/data/extracted/romfs` exists (with-files) or no `data/` (no-files).

- [ ] **Step 1:** Write `test_ios_ipa.sh` with the checks above.
- [ ] **Step 2:** Run `build/smm2/smm2recomp --build-ios /Volumes/SSD/smm-work/ios-ipa/t1.ipa` in `/Volumes/SSD/smm-work/runtest` (LARECOMP_DATA_ROOT). Expected: FAIL (unknown flag / no output), then `test_ios_ipa.sh` FAIL.
- [ ] **Step 3:** Split `gamebuild_compile` out of `gamebuild_run` (same behaviour for the desktop build: generator bases `0x7000000000` / `0x7004000000`, `--hle` / `--patches`; iOS builds run only on Apple silicon, where the generator's default SIMD mode is already `asm`). The C files are still deleted as each compiles; objects are kept by `gamebuild_compile` and deleted by its caller.
- [ ] **Step 4:** Implement `iosbuild_available`: Apple silicon (`sysctl hw.optional.arm64`); kit present and `kit.json` `gamelib_id` equals `GAMELIB_ID`; `xcrun --sdk iphoneos --show-sdk-path` succeeds (runs with `DEVELOPER_DIR` as the environment has it). Implement `iosbuild_run`: free space check (Review Focus 2, `IOSBUILD_TEST_FREE_GB` overrides the measured value for tests); `gamebuild_compile` with `cc = {"xcrun","--sdk","iphoneos","clang","-target","arm64-apple-ios17.0",NULL}` and `cflags` = `game_cflags.txt` + `-I<gamelib-src>` + `-DGAMELIB_ID=...`; `gamelib_entry.c` compiled the same way; link with `clang++` the objects + `lib/*.a` + `@link.txt` into `ios-build/Payload/SMM2Recomp.app/SMM2Recomp`; copy `Info.plist` and `icons/*` into the app; with `game_files`, clone (`clonefile`, falling back to copy) `extracted/exefs`, `extracted/romfs`, `fonts`, `sysdata` into `data/extracted/...`, `data/fonts`, `data/sysdata` (the names `paths.c:86-96` links); `codesign --force --sign - --entitlements entitlements.plist --timestamp=none`; `ditto -c -k --keepParent ios-build/Payload <out>.part` then rename to `<out>`; remove `ios-build/` in every exit path (Review Focus 3).
- [ ] **Step 5:** `--build-ios` in `main.c` (before the setup screen; macOS only; default out per Global Constraints).
- [ ] **Step 6:** Run with and without game files:
  `build/smm2/smm2recomp --build-ios /Volumes/SSD/smm-work/ios-ipa/t1.ipa && tools/test_ios_ipa.sh /Volumes/SSD/smm-work/ios-ipa/t1.ipa with-files`
  `build/smm2/smm2recomp --build-ios --no-game-files /Volumes/SSD/smm-work/ios-ipa/t2.ipa && tools/test_ios_ipa.sh /Volumes/SSD/smm-work/ios-ipa/t2.ipa no-files`. Expected: both PASS.
- [ ] **Step 7:** Review Focus tests: `DEVELOPER_DIR=/Volumes/SSD/smm-work/ios-ipa/empty` → exit 1 with the Xcode message; `IOSBUILD_TEST_FREE_GB=1` → exit 1 with the space message; a `kit.json` with another id → exit 1 with the mismatch message; `--build-ios` killed (SIGTERM) during "Building the game code" → no `ios-build/` and no `.ipa`/`.part` left.
- [ ] **Step 8:** The desktop first-start build still works: delete `extracted/smm2game.dylib` in the runtest folder, start once (muted, hidden, one instance), expect the dylib rebuilt and the game reaching the title.
- [ ] **Step 9:** `scripts/check.sh && git commit` ("Mac app: build an iPhone / iPad app from your dump (--build-ios)").

### Task 5: The setup screen's iPhone / iPad section

**Files:**
- Modify: `recomp/runtime/ui/launcher.mm`

**Interfaces:**
- Consumes: `iosbuild_available`, `iosbuild_run`, `gamebuild_cancel` (Task 4).
- Produces: UI only.

- [ ] **Step 1:** On the setup screen (macOS, kit present), below the game's rows: a heading "iPhone and iPad", one line "Make an app for your iPhone or iPad from your game. You sign and install it yourself (AltStore, Sideloadly or Xcode).", a checkbox "Include the game files (about 3 GB)" (on), and a button "Build for iPhone / iPad". Disabled with the reason as its tooltip and a grey line under it when `iosbuild_available` is false or the game isn't set up (Review Focus 5).
- [ ] **Step 2:** The build runs on its own thread with its own state (not `g_steps` / `g_job`, which belong to setup): step text, progress bar, Cancel (calls `gamebuild_cancel`). Done: "Saved <file name> in Downloads." with "Show in Finder" (`NSWorkspace activateFileViewerSelectingURLs`) and the line "It contains your game: don't share it." Failed: the reason and "Show log" as the setup's own errors do.
- [ ] **Step 3:** Check by hand on this Mac (the app from `make GAME=smm2 app-macos`, data in the runtest folder): button disabled with a reason when `DEVELOPER_DIR` points at an empty folder; enabled otherwise; a build completes and Show in Finder selects the file; Cancel leaves no `ios-build/` behind. Screenshots to `/Volumes/SSD/smm-work/ios-ipa/`.
- [ ] **Step 4:** `scripts/check.sh && git commit` ("Setup screen: Build for iPhone / iPad").

### Task 6: Player docs

**Files:**
- Create: `docs/ios.md`
- Modify: `README.md` (Play it: the line on Android and iOS), `recomp/README.md` (iOS section links `docs/ios.md`)

- [ ] **Step 1:** `docs/ios.md`: what you need (Apple silicon Mac, Xcode with the iOS platform, the game set up in the Mac app); the button or `--build-ios`; installing with AltStore / SideStore / Sideloadly / LiveContainer / Xcode; the two entitlements and what happens without them (the app stops at start: "guest memory not reserved", or is closed for memory); a free Apple account re-signs every 7 days and may not keep the entitlements; game files in the app or copied into Documents; never share the `.ipa`.
- [ ] **Step 2:** README: replace "Android and iOS / iPadOS have no download" with: iPhone / iPad apps are built by the Mac app (link `docs/ios.md`); Android is built from source.
- [ ] **Step 3:** `scripts/check.sh && git commit` ("Docs: iPhone / iPad from the Mac app").

### Task 7: End-to-end check on a device (owner's go-ahead)

- [ ] **Step 1:** Ask the owner before installing on the iPad (standing rule: no iPad installs without them).
- [ ] **Step 2:** With their go-ahead: re-sign the `--build-ios` `.ipa` the way a paid-account player would (unzip, `codesign` with the owner's identity, embedded profile for `com.adriansmith.smm2recomp` as `install-release.sh` does, bundle id changed to match), install with `ios-install-tailscale`, launch with `ios-app launch`, `shot` the title screen. Expected: the game reaches its title on the iPad.
- [ ] **Step 3:** Reply on #69 with how to use it (after the release that carries it; releases only when the owner asks).
