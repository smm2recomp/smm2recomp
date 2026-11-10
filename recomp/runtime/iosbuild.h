// The Mac app's "Build for iPhone / iPad" (issue #69; macOS only): an iPhone / iPad app from the player's own game
// files, as an .ipa they sign and install with a tool of their choice. The game code is generated and compiled here
// with Xcode's iOS compiler (runtime/gamebuild.h), then linked with the iOS kit the app carries
// (Contents/Resources/ios-kit: tools/make_ios_kit.sh), signed ad hoc with the two memory entitlements and zipped.
// Work folder: <data root>/ios-build, removed at the end, on failure, cancel and quit.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool game_files;  // copy extracted/{exefs,romfs}, fonts, sysdata (and the SMM1 dump if one was chosen) into the app's data/ (about 3 GB, 4.2 with SMM1)
    const char *out;  // the .ipa to write (replaced)
    // from the calling thread: the step ("Checking", "Building the game code", "Linking", "Copying the game files",
    // "Signing", "Packing") and how far the whole build is (0..1)
    void (*progress)(const char *step, float frac);
} IosBuildOpts;

// This Mac can build it: Apple silicon, the app's iOS kit made for this app's game code (GAMELIB_ID), Xcode with
// the iOS platform. false: why is what the player can do about it.
bool iosbuild_available(char *why, size_t n);
// Builds it (as long as the first start's game code build, plus a minute or two). false: why says what went wrong,
// "cancelled" after gamebuild_cancel() (details in the log).
bool iosbuild_run(const char *data_root, const IosBuildOpts *o, char *why, size_t n);
// ~/Downloads/SMMaker2-<version>.ipa
void iosbuild_default_out(char *out, size_t n);

#ifdef __cplusplus
}
#endif
