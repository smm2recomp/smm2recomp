// SMM1's sound for the host-drawn 100 Mario screens (smm1_audio.c): its music streams (BFSTM) and sound effects
// (BFWAV in BARS archives) from the player's SMM1 dump, mixed into the game's audio output (runtime/services/audio.c,
// game_audio_mix). While an SMM1 screen covers the game, the game's own sound is faded out.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int rate, channels, frames;  // frames: samples per channel
    int16_t *pcm;                // interleaved
    bool loop;
    int loop_start;
} SPcm;
SPcm *smm1_bfstm_decode(const uint8_t *d, size_t n);
SPcm *smm1_bfwav_decode(const uint8_t *d, size_t n);
SPcm *smm1_bwav_decode(const uint8_t *d, size_t n);                    // an SMM2 BWAV
SPcm *smm1_bars_sound(const uint8_t *d, size_t n, const char *name);  // the named BWAV of an SMM2 BARS (NULL: none)

void smm1_bgm(const char *name, bool loop);  // a stream of Sound/Resource/Stream (cached), from its start; NULL stops
void smm1_bgm_fade(int frames);  // fade the music out over this many 60 Hz frames
void smm1_bgm_track(int track);  // a multi-track stream's track (0-based: Course World's map values 2 / 3 / 6 -> 1 / 2 / 5)
double smm1_bgm_pos(void);       // the music's position in its own samples (-1: none)
const char *smm1_bgm_name(void);
void smm1_se_bank(const char *bank);          // load a sound bank ("NetWorldMapScene", "UI_Play_CourseIn100Mario_00")
bool smm1_se(const char *name);              // a sound effect of a loaded bank, by name (false: none)
bool smm1_se_has(const char *name);          // ... whether a loaded bank has it (nothing played)
void smm1_play(SPcm *p);                     // a decoded sound, once, as a sound effect (kept by the caller)
void smm1_play_vol(SPcm *p, float vol);      // ... at this volume (1: as decoded)
// a sound effect of a loaded bank with SMM1's sound-link settings: volume and pitch factors, a delay and a stop time
// (60 Hz frames: it starts `delay` frames from now and is cut `stop` frames after; 0: none), looped if `loop` and its
// wave loops. A handle for smm1_se_stop; 0: no such sound, -1: no voice free
int smm1_se_ex(const char *name, float vol, float pitch, int delay, int stop, bool loop);
bool smm1_se_stop(int handle);              // false: it was over (or no such sound)
int smm1_se_frames(const char *name);  // a loaded sound's length in 60 Hz frames (0: none)
void smm1_game_sound(bool on);               // the game's own sound under the screens (faded)
