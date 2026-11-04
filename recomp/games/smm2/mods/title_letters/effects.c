// SMM1 title letters: what a letter does besides its animation (title_letters.h).
// - Its SMM1 sounds, played from the player's SMM1 dump as SMM1's sound link set them up: each letter's button has its
//   own bank (UI_Play_Title_00_L_<X>Btn_0n), the title page's own has the drag's, the touch sounds are the menus'.
// - The screen effects, drawn by us into the overlay: A's falling stars and the drag's magic dust (SMM1 draws them with
//   its effect system, whose effects aren't read here: these are our own sprites, made in code).
// - K's sepia picture (the renderers' colour filter, with SMM1's sepia filter values) and SMM2's own 8-bit title music
//   (BGM_Title_FC, played by the voices that play the title music, at their position: runtime/services/audio.c).
// - The parakeet's stay on the I; SUPER's five Super Stars (actors on the title course: spawn.c).
//
// Research: research/specs/smm1-title-letters.md (private): the sounds, volumes, pitches and delays per letter, the
// title page's drag, K's filter, the parakeet. SMM1's parakeet recorded the GamePad's microphone and played it back;
// no microphone is used here: it lands, chirps, and leaves on a second tap or when SMM1's recording time is over.
//
// Threads: tl_effects_load runs on the thread that opens the SMM1 dump; tl_effect_* and tl_effects_frame on the game
// thread (the title's hooks); tl_effects_draw and tl_effects_lost on the renderers'. State under g_lock.
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../mod.h"
#include "../romfs_overlay.h"
#include "../smm1/smm1_audio.h"
#include "color_filter.h"
#include "title_letters.h"

// runtime/services/audio.c: a stream played as another
bool audren_swap_add(const int16_t coefs[16], const uint8_t *adpcm, size_t size, const int16_t *pcm, int stride,
                     uint32_t frames);
void audren_swap(bool on);
int64_t audren_swap_position(void);

#define PARAKEET_STAY 180  // frames on the I after landing (SMM1: until its recorder is done [inf: ~3 s]; spec §2)
#define SEPIA_FADE 30      // frames the picture takes to turn sepia and back (SMM1 cross-fades its K change)
#define MAX_FX 256         // sprites at once (the oldest go first)

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_loaded, g_music;  // the banks; the 8-bit music registered with the mixer

static uint64_t g_rng;
static float frand(void) {  // (xorshift; 0 .. 1)
    if (!g_rng) g_rng = (uint64_t)time(NULL) * 0x2545f4914f6cdd1dull | 1;
    g_rng ^= g_rng << 13, g_rng ^= g_rng >> 7, g_rng ^= g_rng << 17;
    return (float)((g_rng >> 11) % 1000000) / 1000000.f;
}
static float frange(float a, float b) { return a + (b - a) * frand(); }

// ---- sounds
typedef struct {
    const char *name;  // NULL: silence (a random group's "mute")
    float vol, p0, p1;  // volume; pitch, random between p0 and p1 (p1 0: p0)
    short delay, stop;  // 60 Hz frames (0: none)
    float w;            // weight in a random group
} Snd;

static int play(const Snd *s, int extra_delay, bool loop) {
    if (!s->name) return 0;
    float pitch = s->p1 > 0 ? frange(s->p0, s->p1) : s->p0 > 0 ? s->p0 : 1;
    int d = s->delay + extra_delay, h = smm1_se_ex(s->name, s->vol, pitch, d, s->stop, loop);
    const char *res = h > 0 ? "played" : h < 0 ? "no voice free" : "not found";
    if (d) LOG("[title] sound %s (in %d frames): %s\n", s->name, d, res);
    else LOG("[title] sound %s: %s\n", s->name, res);
    return h;
}
static void play_all(const Snd *s, int n) {
    for (int i = 0; i < n; i++) play(&s[i], 0, false);
}
// one of a random group, by weight; *last: SMM1's "random2" (not the one played last time)
static const Snd *pick(const Snd *g, int n, int *last) {
    float total = 0;
    for (int i = 0; i < n; i++) total += last && *last == i && n > 1 ? 0 : g[i].w;
    float r = frand() * total;
    int k = n - 1;
    for (int i = 0; i < n; i++) {
        float w = last && *last == i && n > 1 ? 0 : g[i].w;
        if (r < w) { k = i; break; }
        r -= w;
    }
    if (last) *last = k;
    return &g[k];
}
#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))

static const Snd TOUCH = {"SE_BongoCursol", 1, 3}, CANCEL = {"SE_Tyoi", 0.5f, 4};
static const Snd TOUCH_R[] = {{"SE_LEGI", 1}};
static const Snd TOUCH_O[] = {{"SE_timpaniRoll", 1, 0, 0, 0, 30}, {"SE_timpaniOneshot", 0.2f}};
static const Snd TOUCH_R2[] = {{"SE_Iyo", 1, 2}};

static const Snd SUPER_CLICK[] = {{"yr_Sys_Cursor_OFF_01", 1, 1.5f},
                                  {"yr_v_TitleCALL_LOGO_NOA_03_Delay_pxsps_l_ear", 1, .delay = 90}};
static const Snd SUPER_VOICE[] = {{"yr_v_TitleCALL_LOGO_NOA_01", 1, .w = 1}, {"yr_v_TitleCALL_LOGO_NOA_02", 1, .w = 1},
                                  {"yr_v_TitleCALL_LOGO_NOA_03", 1, .w = 1},
                                  {"yr_v_TitleCALL_Wisp_NCL_01", 1, .w = 0.1f},
                                  {"yr_v_TitleCALL_Wisp_NCL_02", 1, .w = 0.1f}};
static const Snd WARIO_A[] = {{"yr_v_WARIO_A_01_Turbo_WA_GV_05_01_80CD", 0.5f, .stop = 80, .w = 1},
                              {"yr_v_WARIO_A_02_Turbo_WA_RD_CMBW_01_08", 0.5f, .stop = 80, .w = 1},
                              {"yr_v_WARIO_A_03_Turbo_wr_Action_Attack-04_1DTS0", 0.6f, .stop = 80, .w = 1}};
static const Snd WARIO_B[] = {{"yr_v_WARIO_B_01_Turbo_WA_RA_GLD_01_00", 0.5f, .stop = 80, .w = 1},
                              {"yr_v_WARIO_B_02_wr_Action_Slipstream-02_NoDown", 0.6f, .stop = 80, .w = 1},
                              {"yr_v_WARIO_B_03_wr_Item_Success-03_cut", 0.6f, .stop = 80, .w = 1}};
static const Snd WARIO_WA[] = {{"yr_v_WARIO_A_Water_01_wr_Action_StartFail-03", 0.6f, .stop = 80, .w = 1},
                               {"yr_v_WARIO_A_Water_02_wr_org_Fall_Long-02", 0.5f, .stop = 80, .w = 1},
                               {"yr_v_WARIO_A_Water_03_wr_org_Minigame_Lose-08", 0.5f, .stop = 80, .w = 1}};
static const Snd WARIO_WB[] = {{"yr_v_WARIO_B_Water_01_wr_Comeback_Water-02", 0.5f, .stop = 80, .w = 1},
                               {"yr_v_WARIO_B_Water_02_wr_org_Damage_Fire-02", 0.6f, .stop = 80, .w = 1},
                               {"yr_v_WARIO_B_Water_03_wr_org_Fall_Water-02", 0.6f, .stop = 80, .w = 1}};
static const Snd WATER_H[] = {{"yr_Water_ING_H_01", 0.4f, .stop = 80, .w = 1},
                              {"yr_Water_ING_H_02", 0.4f, .stop = 80, .w = 1},
                              {"yr_Water_ING_H_03", 0.4f, .stop = 80, .w = 1}};
static const Snd WATER_M[] = {{"yr_Water_ING_M_01", 0.2f, .stop = 80, .w = 1},
                              {"yr_Water_ING_M_02", 0.2f, .stop = 80, .w = 1},
                              {"yr_Water_ING_M_03", 0.2f, .stop = 80, .w = 1}};
static const Snd A_DECIDE[] = {{"SE_POPPO", 1}};  // (its sparkle plays only with SMM1's 3-D sound on)
static const Snd R_DECIDE[] = {{"SE_jajan", 1, .stop = 100}};
static const Snd O_DECIDE[] = {{"SE_TimpaniHit", 0.7f}};
static const Snd M2_DECIDE[] = {{"yr_Metal_01", 1}, {"SE_Don", 1}};
static const Snd M2_METAL[] = {{"yr_Metal_MET_01_pxsps_l_ear", 1, .delay = 10, .w = 1},
                               {"yr_Metal_MET_02_pxsps_l_ear", 1, .delay = 10, .w = 1}};
static const Snd A2_DECIDE[] = {{"yr_Sys_Btn_Rocket_01", 1}, {"yr_ROCKET_TakeOFF_1", 1, .delay = 210},
                                {"turn", 1, .delay = 230}, {"yr_ROCKET_TakeOFF_2", 1, .delay = 320},
                                {"bl_rocket_center", 1, .delay = 320}, {"yr_Sys_Cursor_ON_01", 1, .delay = 370}};
static const Snd A2_COUNT[] = {{"yr_v_NOA_CountDown_Rocket_title_1", 1, .w = 1},
                               {"yr_v_NOA_CountDown_Rocket_title_2", 1, .w = 0.5f},
                               {"yr_v_NOA_CountDown_Rocket_title_3", 1, .w = 0.5f}};
static const Snd E_DECIDE[] = {{"SE_Tsutsumi", 1, 1.05f}};
static const Snd R2_BOING[] = {{"yr_EraseMode_Foot_B_1", 1, .w = 1}, {"yr_EraseMode_Foot_B_2", 1, .w = 1},
                               {"yr_EraseMode_Foot_B_4", 1, .w = 1}};
static const Snd R2_CORK[] = {{"SE_SenNuki", 1, .delay = 2}};
static const Snd R2_WATER[] = {{"SE_SenNuki", 1}, {"SE_SenNuki", 1, .delay = 10}, {"SE_SenNuki", 1, .delay = 21}};
static const Snd R2_WATER_H[] = {{"yr_Water_ING_H_01", 1, 1.1f, 1.3f, .w = 1},
                                 {"yr_Water_ING_H_02", 1, 1.1f, 1.3f, .w = 1},
                                 {"yr_Water_ING_H_03", 1, 1.1f, 1.3f, .w = 1}};
// the parakeet: its wings, and a chirp chosen by the I's new state (0 leaving, 1 landing)
static const Snd I_WING[] = {{"MW_BatFly", 1}}, I_WING_WATER[] = {{"MW_BatFly", 0.7f, 0.6f}};
static const Snd I_CHIRP0[] = {{"hz_inco_8", 0.7f, .w = 1}, {"hz_inco_5", 0.42f, .w = 1}, {NULL, .w = 0.9f}};
static const Snd I_CHIRP1[] = {{"hz_inco_3", 0.5f, .w = 1}, {"hz_inco_4", 0.5f, .w = 0.2f},
                               {"hz_inco_1", 0.5f, .w = 0.5f}, {NULL, .w = 0.8f}};
static const Snd I_DRIP[] = {{"yr_Water_Drip_M_01", 0.2f, 1.1f, 1.3f, 10, .w = 1},
                             {"yr_Water_Drip_M_02", 0.2f, 1.1f, 1.3f, 10, .w = 1},
                             {"yr_Water_Drip_M_03", 0.2f, 1.1f, 1.3f, 10, .w = 1}};
static const Snd I_BUBBLE[] = {{"yr_Water_ING_H_01", 0.1f, 1.1f, 1.3f, .w = 1},
                               {"yr_Water_ING_H_02", 0.1f, 1.1f, 1.3f, .w = 1},
                               {"yr_Water_ING_H_03", 0.1f, 1.1f, 1.3f, .w = 1}};
// while it sits: SMM1 played back what it recorded, changed; here one of its chirps at a random pitch instead
static const Snd I_SING[] = {{"hz_inco_1", 0.5f, 0.8f, 1.3f, .w = 1}, {"hz_inco_3", 0.5f, 0.8f, 1.3f, .w = 1},
                             {"hz_inco_4", 0.5f, 0.8f, 1.3f, .w = 1}, {"hz_inco_5", 0.42f, 0.8f, 1.3f, .w = 1},
                             {"hz_inco_8", 0.7f, 0.8f, 1.3f, .w = 1}};
// K: the switch, and the old film's noise while the picture is sepia (clicks between silences, a long hiss)
static const Snd K_ON[] = {{"yr_TV_Channel_Change_1", 1}}, K_OFF[] = {{"yr_Sys_NG_01", 1}};
static const Snd K_CLICK[] = {{"yr_NOISE_Click_01", 1, .w = 1}, {"yr_NOISE_Click_02", 1, .w = 1},
                              {"yr_NOISE_Click_03", 1, .w = 1}, {"yr_NOISE_Click_04", 1, .w = 1},
                              {"yr_NOISE_Click_05", 1, .w = 1}, {NULL, .w = 1}, {NULL, .w = 1}};
static const Snd K_HISS[] = {{"yr_NOISE_Long_05", 1, 0.9f, 1.1f, 10}, {"yr_NOISE_Long_06", 1, 0.9f, 1.1f, 10}};
// the drag on the course behind the title: Kamek's magic and a sparkle (both cut at 30 frames), and the magic's end
static const Snd DRAG[] = {{"SE_BOSS_KAMECK_MAGIC_FLY", 0.5f, .stop = 30}, {"kirakira", 0.5f, .stop = 30},
                           {"WU_SE_BOSS_CMN_MAGIC_DISAPP", 1}};

static const char *const BANKS[] = {"UI_Nwk_Root",  // (SE_BongoCursol, SE_Tyoi: SMM1's menus share them)
                                    "UI_Play_Title_00", "UI_Play_Title_00_L_SuperBtn_00", "UI_Play_Title_00_L_MBtn_00",
                                    "UI_Play_Title_00_L_ABtn_00", "UI_Play_Title_00_L_RBtn_00",
                                    "UI_Play_Title_00_L_IBtn_00", "UI_Play_Title_00_L_OBtn_00",
                                    "UI_Play_Title_00_L_MBtn_01", "UI_Play_Title_00_L_ABtn_01",
                                    "UI_Play_Title_00_L_KBtn_00", "UI_Play_Title_00_L_EBtn_00",
                                    "UI_Play_Title_00_L_RBtn_01"};

// ---- the 8-bit title music: SMM2's BGM_Title_FC, the same length and loop as BGM_Title (spec §7 of
// smm2-title-scene.md), each of BGM_Title's channels (two stereo tracks: the title plays the first) played as its side
static uint32_t le32(const uint8_t *d, size_t n, size_t o) {
    if (o + 4 > n) return 0;
    return (uint32_t)d[o] | (uint32_t)d[o + 1] << 8 | (uint32_t)d[o + 2] << 16 | (uint32_t)d[o + 3] << 24;
}
static bool load_music(void) {
    RovBuf src, fc;
    if (!rov_read_game("Sound/Resource/Stream/BGM_Title.bwav", &src)) return false;
    if (!rov_read_game("Sound/Resource/Stream/BGM_Title_FC.bwav", &fc)) { rov_free(&src); return false; }
    SPcm *p = smm1_bwav_decode(fc.d, fc.n);
    rov_free(&fc);
    int ch = src.n >= 0x10 ? src.d[0xe] | src.d[0xf] << 8 : 0, added = 0;
    uint32_t frames = src.n >= 0x10 + 0x4c ? le32(src.d, src.n, 0x18) : 0;
    if (!p || p->channels != 2 || ch < 1 || ch > 8 || (uint32_t)p->frames != frames) {
        LOG("[title] 8-bit title music: BGM_Title / BGM_Title_FC not as expected (%d channels, %u / %d samples)\n", ch,
            frames, p ? p->frames : 0);
        rov_free(&src);
        return false;
    }
    for (int c = 0; c < ch; c++) {  // (a channel's info: +0x10 its 16 coefficients, +0x34 its data's offset)
        size_t ci = 0x10 + (size_t)c * 0x4c, at = le32(src.d, src.n, ci + 0x34);
        size_t len = ((size_t)frames + 13) / 14 * 8;
        if (src.d[ci] != 1 || at >= src.n) continue;  // (DSP-ADPCM only)
        if (at + len > src.n) len = src.n - at;
        int16_t k[16];
        for (int i = 0; i < 16; i++) k[i] = (int16_t)(src.d[ci + 0x10 + 2 * i] | src.d[ci + 0x11 + 2 * i] << 8);
        added += audren_swap_add(k, src.d + at, len, p->pcm + (c & 1), 2, frames);
    }
    LOG("[title] 8-bit title music ready (%d of BGM_Title's %d channels)\n", added, ch);
    return added > 0;  // (src and p stay: the mixer reads them)
}

void tl_effects_load(void) {
    for (int i = 0; i < N(BANKS); i++) smm1_se_bank(BANKS[i]);
    bool music = load_music();
    pthread_mutex_lock(&g_lock);
    g_loaded = true, g_music = music;
    pthread_mutex_unlock(&g_lock);
}

// ---- the sprites (game pixels, 1280 x 720, top-left origin; the renderers' thread draws them where `tick` puts them)
enum { FX_FALL, FX_DUST };
typedef struct {
    int kind, born, life;
    float x, y, vx, vy, g, size, spin, phase;
    uint8_t rgb[3];
} Fx;
static Fx g_fx[MAX_FX];
static int g_nfx_next;

static void add_fx(const Fx *f) {  // (the lock held)
    g_fx[g_nfx_next++ % MAX_FX] = *f;
}

static const uint8_t RAINBOW[6][3] = {{255, 70, 70}, {255, 170, 40}, {255, 235, 60}, {90, 230, 90}, {80, 160, 255},
                                      {200, 110, 255}};
static const uint8_t DUST[4][3] = {{255, 255, 255}, {255, 240, 120}, {255, 150, 230}, {140, 230, 255}};

// A: multicoloured stars fall over the whole screen once its animation is over (SMM1: an effect at its end)
static void falling_stars(int tick) {  // (the lock held)
    for (int i = 0; i < 28; i++) {
        Fx f = {FX_FALL, tick + 50 + i * 3, 220};
        f.x = frange(20, 1260), f.y = frange(-60, -20), f.vx = frange(-0.6f, 0.6f), f.vy = frange(2.5f, 4.5f);
        f.g = 0.01f, f.size = frange(26, 44), f.spin = frange(-0.08f, 0.08f), f.phase = frange(0, 6.28f);
        memcpy(f.rgb, RAINBOW[i % 6], 3);
        add_fx(&f);
    }
}

static void dust(float x, float y, int tick, int n, float speed) {  // (the lock held)
    for (int i = 0; i < n; i++) {
        Fx f = {FX_DUST, tick, (int)frange(30, 46)};
        float a = frange(0, 6.28f), v = speed * frange(0.3f, 1);
        f.x = x + frange(-8, 8), f.y = y + frange(-8, 8), f.vx = cosf(a) * v, f.vy = sinf(a) * v + 0.4f;
        f.g = 0.02f, f.size = frange(24, 40), f.spin = frange(-0.15f, 0.15f), f.phase = frange(0, 6.28f);
        memcpy(f.rgb, DUST[(int)(frand() * 4) & 3], 3);
        add_fx(&f);
    }
}

// ---- the state of K, I and the drag (game thread, under the lock: the renderers' thread may turn them off)
static struct {
    bool k_on, i_on, i_sang;
    int k_at, i_at;          // ticks K / I were turned on
    int k_hiss[2], k_next;   // the hiss's sounds; the next click
    float sepia, k_from;     // 0 .. 1 as set last; where K's fade back started
    bool drag;
    int drag_last;
    int last_super, last_count, last_metal, last_boing, last_wa, last_h, last_click;  // ("random2" groups)
    bool said;               // the course's theme logged
} S;

static bool water_course(void) {  // (SMM1's sound "scene set" water: an underwater area, or a forest's at night)
    bool night = false;
    int t = tl_course_theme(&night);
    return t == 4 || (t == 9 && night);
}

static void sepia_matrix(float k, float m[12]) {
    // SMM1's Ef_Sepia (Env/ColorCorrection, spec §2 K): saturation 0.138, brightness 0.83, black and white mapped to
    // (0.08, 0.058, 0) and (0.88, 0.77, 0.4588); k blends from no change
    static const float lo[3] = {0.08f, 0.058f, 0}, hi[3] = {0.88f, 0.77f, 0.4588f}, L[3] = {0.299f, 0.587f, 0.114f};
    const float sat = 0.138f, bright = 0.83f;
    for (int i = 0; i < 3; i++) {
        float span = (hi[i] - lo[i]) * bright;
        for (int j = 0; j < 3; j++) {
            float s = span * ((1 - sat) * L[j] + (i == j ? sat : 0)), id = i == j ? 1.f : 0.f;
            m[4 * i + j] = id + (s - id) * k;
        }
        m[4 * i + 3] = lo[i] * k;
    }
}
static void set_sepia(float k) {  // (the lock held)
    if (k == S.sepia) return;
    S.sepia = k;
    float m[12];
    sepia_matrix(k, m);
    renderer_set_color_filter(k > 0 ? m : NULL);
}

static void k_noise(bool on, int tick) {  // (the lock held)
    for (int i = 0; i < 2; i++) smm1_se_stop(S.k_hiss[i]), S.k_hiss[i] = 0;
    if (on)
        for (int i = 0; i < 2; i++) S.k_hiss[i] = play(&K_HISS[i], 0, true);
    S.k_next = tick + 10;
}

static void k_off(bool fade, int tick) {  // (the lock held) the music back, the picture back now or over SEPIA_FADE
    S.k_on = false;
    if (fade) S.k_from = S.sepia, S.k_at = tick;
    else set_sepia(0);
    audren_swap(false);
    k_noise(false, 0);
}

// ---- the game thread
void tl_effect_touch(int letter) {
    pthread_mutex_lock(&g_lock);
    if (g_loaded) {
        if (letter == TL_R1) play_all(TOUCH_R, N(TOUCH_R));
        else if (letter == TL_O) play_all(TOUCH_O, N(TOUCH_O));
        else if (letter == TL_R2) play_all(TOUCH_R2, N(TOUCH_R2));
        else play(&TOUCH, 0, false);
    }
    pthread_mutex_unlock(&g_lock);
}

void tl_effect_untouch(int letter) {
    (void)letter;
    pthread_mutex_lock(&g_lock);
    if (g_loaded) play(&CANCEL, 0, false);
    pthread_mutex_unlock(&g_lock);
}

static void parakeet(bool landing, bool water) {  // (the lock held) the wings and a chirp
    if (water) {
        play_all(I_WING_WATER, 1);
        play(landing ? pick(I_BUBBLE, N(I_BUBBLE), NULL) : pick(I_DRIP, N(I_DRIP), NULL), 0, false);
    } else {
        play_all(I_WING, 1);
        play(landing ? pick(I_CHIRP1, N(I_CHIRP1), NULL) : pick(I_CHIRP0, N(I_CHIRP0), NULL), 0, false);
    }
}

void tl_effect_decide(int letter, bool on, const TlBox *box, int tick) {
    pthread_mutex_lock(&g_lock);
    if (!g_loaded) { pthread_mutex_unlock(&g_lock); return; }
    bool water = water_course(), night = false;
    if (!S.said) {
        int theme = tl_course_theme(&night);
        LOG("[title] the title course: theme %d%s: %s\n", theme, night ? " at night" : "",
            water ? "underwater, the letters' water sounds" : "not underwater");
    }
    S.said = true;
    switch (letter) {
    case TL_SUPER: {
        play(&SUPER_CLICK[0], 0, false);
        play(pick(SUPER_VOICE, N(SUPER_VOICE), &S.last_super), 0, false);
        play(&SUPER_CLICK[1], 0, false);
        // five Super Stars from the letter (SMM1 makes five Star actors there, each with its own number)
        static const float vx[5] = {-2, -1, 0, 1, 2};
        int made = 0;
        for (int i = 0; i < 5; i++) made += tl_spawn(TA_STAR, box->x + box->w / 2, box->y + box->h / 2, vx[i], 1.5f);
        LOG("[title] SUPER: %d of 5 Super Stars\n", made);
        break;
    }
    case TL_M1:
        if (water) {
            const Snd *a = pick(WARIO_WA, N(WARIO_WA), &S.last_wa), *h = pick(WATER_H, N(WATER_H), &S.last_h);
            play(a, 0, false), play(h, 0, false);
            int la = smm1_se_frames(a->name), lh = smm1_se_frames(h->name);
            play(pick(WARIO_WB, N(WARIO_WB), NULL), la > 0 && la < 80 ? la : 80, false);
            play(pick(WATER_M, N(WATER_M), NULL), lh > 0 && lh < 80 ? lh : 80, false);
        } else {  // (Wario's A voice, then his B voice when it ends)
            const Snd *a = pick(WARIO_A, N(WARIO_A), &S.last_wa);
            play(a, 0, false);
            int la = smm1_se_frames(a->name);
            play(pick(WARIO_B, N(WARIO_B), NULL), la > 0 && la < 80 ? la : 80, false);
        }
        break;
    case TL_A1:
        play_all(A_DECIDE, N(A_DECIDE));
        falling_stars(tick);
        break;
    case TL_R1: play_all(R_DECIDE, N(R_DECIDE)); break;
    case TL_O: play_all(O_DECIDE, N(O_DECIDE)); break;
    case TL_M2:
        play_all(M2_DECIDE, N(M2_DECIDE));
        play(pick(M2_METAL, N(M2_METAL), &S.last_metal), 0, false);
        break;
    case TL_A2:
        play_all(A2_DECIDE, N(A2_DECIDE));
        play(pick(A2_COUNT, N(A2_COUNT), &S.last_count), 0, false);
        break;
    case TL_E: play_all(E_DECIDE, N(E_DECIDE)); break;
    case TL_R2:
        if (water) {
            play_all(R2_WATER, N(R2_WATER));
            play(pick(R2_WATER_H, N(R2_WATER_H), NULL), 0, false);
        } else {
            play(pick(R2_BOING, N(R2_BOING), &S.last_boing), 0, false);
            play_all(R2_CORK, N(R2_CORK));
        }
        break;
    case TL_I:
        S.i_on = on, S.i_at = tick, S.i_sang = false;
        parakeet(on, water);
        LOG("[title] the parakeet %s\n", on ? "lands on the I" : "leaves");
        break;
    case TL_K:
        if (on) {
            play_all(K_ON, N(K_ON));
            S.k_on = true, S.k_at = tick;
            if (g_music) audren_swap(true);
            k_noise(true, tick);
            LOG("[title] K: sepia and the 8-bit title music%s\n",
                g_music ? "" : " (no 8-bit music: BGM_Title_FC missing)");
        } else {
            play_all(K_OFF, N(K_OFF));
            k_off(true, tick);
            LOG("[title] K: the picture and the title music back\n");
        }
        break;
    }
    pthread_mutex_unlock(&g_lock);
}

void tl_effect_drag(float x, float y, int tick, bool first) {
    pthread_mutex_lock(&g_lock);
    if (g_loaded) {
        if (first) play_all(DRAG, N(DRAG)), S.drag_last = tick - 2;
        if (tick - S.drag_last >= 1) dust(x, y, tick, 2, 1.2f), S.drag_last = tick;
        S.drag = true;
    }
    pthread_mutex_unlock(&g_lock);
}

void tl_effect_release(float x, float y, int tick) {
    pthread_mutex_lock(&g_lock);
    if (g_loaded && S.drag) dust(x, y, tick, 10, 3.5f);
    S.drag = false;
    pthread_mutex_unlock(&g_lock);
}

void tl_effects_frame(int tick) {
    pthread_mutex_lock(&g_lock);
    if (!g_loaded) { pthread_mutex_unlock(&g_lock); return; }
    // K: the picture fades to sepia and back; the film's clicks while it is sepia
    float k = fminf(1, (float)(tick - S.k_at) / SEPIA_FADE);
    set_sepia(S.k_on ? k : S.sepia > 0 ? fmaxf(0, S.k_from * (1 - k)) : 0);
    if (S.k_on && tick >= S.k_next) {
        play(pick(K_CLICK, N(K_CLICK), &S.last_click), 0, false);
        S.k_next = tick + (int)frange(50, 100);
    }
    if (S.i_on && !S.i_sang && tick - S.i_at >= 40 + PARAKEET_STAY / 2) {  // (DecideA's 40 frames, half the stay)
        S.i_sang = true;
        play(pick(I_SING, N(I_SING), NULL), 0, false);
    }
    bool leave = S.i_on && tick - S.i_at >= 40 + PARAKEET_STAY;
    pthread_mutex_unlock(&g_lock);
    if (leave && tl_anim_on(TL_I, tick) && tl_anim_start(TL_I, tick)) {
        LOG("[title] the parakeet's time is over\n");
        TlBox b = {0};
        tl_effect_decide(TL_I, false, &b, tick);
    } else if (leave) {
        pthread_mutex_lock(&g_lock);
        S.i_on = false;
        pthread_mutex_unlock(&g_lock);
    }
}

static void reset_locked(void) {
    if (S.k_on || S.sepia > 0) LOG("[title] K off: the picture and the title music back\n");
    k_off(false, 0);
    for (int i = 0; i < MAX_FX; i++) g_fx[i].life = 0;
    memset(&S, 0, sizeof S);
}

void tl_effects_reset(void) {
    pthread_mutex_lock(&g_lock);
    reset_locked();
    pthread_mutex_unlock(&g_lock);
}

void tl_effects_lost(void) {  // (the renderers' thread: the title went without a word)
    pthread_mutex_lock(&g_lock);
    if (S.k_on || S.sepia > 0 || S.i_on) reset_locked();
    pthread_mutex_unlock(&g_lock);
}

// ---- drawing (the renderers' thread)
static void lin(uint8_t out[4], const uint8_t rgb[3], float a) {  // (sRGB colour -> the raster's linear values)
    for (int k = 0; k < 3; k++) {
        float f = rgb[k] / 255.0f;
        f = f <= 0.04045f ? f / 12.92f : powf((f + 0.055f) / 1.055f, 2.4f);
        out[k] = (uint8_t)(f * 255 + 0.5f);
    }
    out[3] = (uint8_t)(fminf(fmaxf(a, 0), 1) * 255 + 0.5f);
}

// our own sprites: a five-pointed star (white, a grey rim) and a four-pointed twinkle (white, soft)
static int star_tex(void) {
    static int tex = -2;
    if (tex != -2) return tex;
    enum { S_ = 64, SS = 4 };
    uint8_t *px = malloc(S_ * S_ * 4);
    if (!px) return tex = -1;
    float vx[10], vy[10];
    for (int i = 0; i < 10; i++) {
        float a = -1.5707963f + (float)i * 0.62831853f, r = i & 1 ? 0.42f : 1.0f;
        vx[i] = cosf(a) * r * 30, vy[i] = sinf(a) * r * 30 + 2;
    }
    for (int y = 0; y < S_; y++)
        for (int x = 0; x < S_; x++) {
            int in = 0, rim = 0;
            for (int sy = 0; sy < SS; sy++)
                for (int sx = 0; sx < SS; sx++) {
                    float px_ = x + (sx + 0.5f) / SS - 32, py = y + (sy + 0.5f) / SS - 32;
                    for (int pass = 0; pass < 2; pass++) {  // (the star, then the star shrunk: what's left is rim)
                        float k = pass ? 0.8f : 1;
                        bool c = false;
                        for (int i = 0, j = 9; i < 10; j = i++) {
                            float xi = vx[i] * k, yi = vy[i] * k, xj = vx[j] * k, yj = vy[j] * k;
                            if ((yi > py) != (yj > py) && px_ < (xj - xi) * (py - yi) / (yj - yi) + xi) c = !c;
                        }
                        if (!pass) in += c;
                        else if (!c && in) rim++;
                    }
                }
            float a = (float)in / (SS * SS), r = in ? (float)rim / in : 0;
            uint8_t *p = px + ((size_t)y * S_ + x) * 4, g = (uint8_t)(255 - r * 150);
            p[0] = p[1] = p[2] = g, p[3] = (uint8_t)(a * 255 + 0.5f);
        }
    return tex = lyt_tex_add(S_, S_, px);
}

static int twinkle_tex(void) {
    static int tex = -2;
    if (tex != -2) return tex;
    enum { S_ = 32 };
    uint8_t *px = malloc(S_ * S_ * 4);
    if (!px) return tex = -1;
    for (int y = 0; y < S_; y++)
        for (int x = 0; x < S_; x++) {
            float dx = fabsf((x + 0.5f) / S_ * 2 - 1), dy = fabsf((y + 0.5f) / S_ * 2 - 1);
            float core = expf(-(dx * dx + dy * dy) * 18), arms = expf(-dx * 14) * (1 - dy) + expf(-dy * 14) * (1 - dx);
            float a = fminf(1, core + arms * 0.9f);
            uint8_t *p = px + ((size_t)y * S_ + x) * 4;
            p[0] = p[1] = p[2] = 255, p[3] = (uint8_t)(a * 255 + 0.5f);
        }
    return tex = lyt_tex_add(S_, S_, px);
}

static void sprite(LQuads *out, int tex, float x, float y, float size, float angle, const uint8_t col[4]) {
    LQuad q;
    memset(&q, 0, sizeof q);
    float c = cosf(angle) * size / 2, s = sinf(angle) * size / 2, X = x - 640, Y = 360 - y;  // (layout space, y up)
    // the corners TL, TR, BL, BR of the square turned by angle
    float P[4][2] = {{X - c - s, Y - s + c}, {X + c - s, Y + s + c}, {X - c + s, Y - s - c}, {X + c + s, Y + s - c}};
    float U[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    memcpy(q.pos, P, sizeof P), memcpy(q.uv0, U, sizeof U);
    for (int v = 0; v < 4; v++) memcpy(q.col[v], col, 4);
    memset(q.white, 255, 4);
    q.tex0 = tex, q.tex1 = -1;
    q.flags = LQ_LINEAR | LQ_BLEND_NORMAL | LQ_COMB_NONE << LQ_COMB_C | LQ_COMB_NONE << LQ_COMB_A;
    if (out->n == out->cap) {
        int cap = out->cap ? out->cap * 2 : 256;
        LQuad *nq = realloc(out->q, sizeof(LQuad) * (size_t)cap);
        if (!nq) return;
        out->q = nq, out->cap = cap;
    }
    out->q[out->n++] = q;
}

bool tl_effects_draw(int tick, LQuads *out, bool front) {
    int from = out->n;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_FX; i++) {
        const Fx *f = &g_fx[i];
        int t = tick - f->born;
        if (f->life <= 0 || t < 0 || t >= f->life || (f->kind == FX_DUST) != front) continue;
        float x = f->x + f->vx * t + (f->kind == FX_FALL ? sinf(f->phase + t * 0.05f) * 12 : 0);
        float y = f->y + f->vy * t + 0.5f * f->g * t * t, life = (float)t / f->life;
        if (y > 760) continue;
        uint8_t col[4];
        if (f->kind == FX_FALL) {
            lin(col, f->rgb, life > 0.8f ? (1 - life) * 5 : 1);
            sprite(out, star_tex(), x, y, f->size, f->phase + f->spin * t, col);
        } else {
            float tw = 0.75f + 0.25f * sinf(f->phase + t * 0.9f);  // (a twinkle)
            lin(col, f->rgb, 1 - life * life);
            sprite(out, twinkle_tex(), x, y, f->size * (1 - 0.6f * life) * tw, f->phase + f->spin * t, col);
        }
    }
    pthread_mutex_unlock(&g_lock);
    return out->n > from;
}
