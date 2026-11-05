// Builds the game code library (runtime/gamelib.h) on the player's machine, from their own game files: the generator
// (tools/gen.py; shipped frozen as lasgen/) turns exefs/main and exefs/sdk into C, the bundled compiler (zig/) builds
// that into <game_dir>/lasgame.*. The C and the objects are deleted afterwards.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // now and then, from the calling thread: how far the whole build is (0..1) and what it's doing
    void (*progress)(double fraction, const char *step);
    int jobs;  // compiler processes at once (>= 1)
} GameBuildOpts;

// Builds it (a few minutes). false: why says what went wrong (details in the log).
bool gamebuild_run(const char *game_dir, const GameBuildOpts *opts, char *why, size_t why_size);
// Its first half: generates the C from <game_dir>/exefs/{main,sdk} into work/ (an existing folder) and compiles each
// file as it is written, plus gamelib_entry.c. cc / cflags (NULL-terminated argument lists): another compiler and all
// its flags (-I and -DGAMELIB_ID included; the iPhone build, runtime/iosbuild.c), NULL: the game code library's. The C
// is deleted as it compiles; the objects stay in work/, listed in *objs (gamebuild_free_objs).
bool gamebuild_compile(const char *game_dir, const char *work, const char *const *cc, const char *const *cflags,
                       const GameBuildOpts *opts, char ***objs, char *why, size_t why_size);
void gamebuild_free_objs(char **objs);
// Runs one program to the end (its output to log); the cancel ends it too. Its exit code, -1: cancelled / didn't start.
int gamebuild_exec(const char *const *argv, const char *log);
// Cancel: the running gamebuild_compile / gamebuild_exec ends its programs and returns false ("cancelled"). Only sets
// a flag (a signal handler may call it); stays set until gamebuild_cancel_clear (gamebuild_run clears it itself).
void gamebuild_cancel(void);
void gamebuild_cancel_clear(void);
bool gamebuild_cancelled(void);
// The generator and the compiler are there (shipped builds; a source checkout with tools/venv and clang).
bool gamebuild_available(void);
// The Python the generator and the setup scripts (runtime/ui/launcher.mm) run with: the one that comes with the
// package (python/), else tools/venv (a source checkout, or a package whose player made it). Also sets up the
// environment the children get (no bytecode written into the app, no user site-packages, UTF-8). false: none.
bool gamebuild_python(char *out, size_t n);

#ifdef __cplusplus
}
#endif
