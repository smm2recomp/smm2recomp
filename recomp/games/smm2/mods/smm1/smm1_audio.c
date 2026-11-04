// SMM1's sound for the 100 Mario screens (smm1_audio.h).
//
// BFSTM (Cafe stream, big-endian): header with section references (0x4000 INFO, 0x4001 SEEK, 0x4002 DATA); INFO
// holds the stream info (codec, loop, channels, rate, loop start, sample count, block count / size / samples, the last
// block's sizes, a reference to the sample data in DATA) and a channel table whose entries point to each channel's
// DSP-ADPCM coefficients. Samples are stored block by block, each block one piece per channel.
// BFWAV (Cafe wave): INFO (encoding, loop, rate, loop start / end, channel table: sample reference into DATA and the
// ADPCM coefficients per channel). Both written from public descriptions of the NW4F formats; the DSP-ADPCM decoder
// follows SMCraft's mmk-formats audio/dsp.rs.
#include <math.h>
#include <stddef.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "smm1_audio.h"
#include "smm1_data.h"
#include "lyt.h"

static uint32_t be32(const uint8_t *d, size_t n, size_t o) {
    return o + 4 <= n ? (uint32_t)d[o] << 24 | (uint32_t)d[o + 1] << 16 | (uint32_t)d[o + 2] << 8 | d[o + 3] : 0;
}
static uint16_t be16(const uint8_t *d, size_t n, size_t o) { return o + 2 <= n ? (uint16_t)(d[o] << 8 | d[o + 1]) : 0; }

typedef struct {
    int16_t coefs[16], h1, h2;
} Dsp;
static int16_t clamp16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : (int16_t)v; }
// decode `samples` DSP-ADPCM samples from d (frames of 8 bytes: header, 14 nibbles) into out with the given stride
static void dsp_decode(const uint8_t *d, size_t n, int samples, Dsp *st, int16_t *out, int stride) {
    for (int i = 0; i < samples; i++) {
        size_t f = (size_t)(i / 14) * 8;
        if (f >= n) break;
        int k = i % 14;
        uint8_t hdr = d[f];
        if (f + 1 + (size_t)k / 2 >= n) break;
        uint8_t byte = d[f + 1 + k / 2];
        int nib = k % 2 == 0 ? byte >> 4 : byte & 15;
        if (nib >= 8) nib -= 16;
        int pair = hdr >> 4 & 7, scale = hdr & 15;
        int p = ((nib << scale) << 11) + st->coefs[pair * 2] * st->h1 + st->coefs[pair * 2 + 1] * st->h2;
        int16_t s = clamp16((p + 1024) >> 11);
        st->h2 = st->h1, st->h1 = s;
        out[(size_t)i * (size_t)stride] = s;
    }
}

SPcm *smm1_bfstm_decode(const uint8_t *d, size_t n) {
    if (n < 0x40 || memcmp(d, "FSTM", 4)) return NULL;
    int nsec = be16(d, n, 0x10);
    size_t info = 0, data = 0;
    for (int i = 0; i < nsec; i++) {
        uint16_t id = be16(d, n, 0x14 + (size_t)i * 12);
        size_t off = be32(d, n, 0x14 + (size_t)i * 12 + 4);
        if (id == 0x4000) info = off;
        if (id == 0x4002) data = off;
    }
    if (!info || !data) return NULL;
    size_t ib = info + 8, s = ib + be32(d, n, ib + 4), ct = ib + be32(d, n, ib + 0x14);
    int codec = d[s], loop = d[s + 1], ch = d[s + 2];
    SPcm *p = calloc(1, sizeof *p);
    p->rate = (int)be32(d, n, s + 4), p->loop = loop, p->loop_start = (int)be32(d, n, s + 8);
    p->frames = (int)be32(d, n, s + 0xC), p->channels = ch;
    uint32_t blocks = be32(d, n, s + 0x10), bsize = be32(d, n, s + 0x14), bsamples = be32(d, n, s + 0x18);
    uint32_t lsamples = be32(d, n, s + 0x20), lpadded = be32(d, n, s + 0x24);
    size_t sample0 = data + 8 + be32(d, n, s + 0x34);
    if (ch < 1 || ch > 16 || (codec != 1 && codec != 2) || !p->frames) { free(p); return NULL; }
    Dsp st[16];  // (multi-track streams: the mixer plays the first track, channels 0-1)
    memset(st, 0, sizeof st);
    if (codec == 2)
        for (int c = 0; c < ch; c++) {
            size_t ci = ct + be32(d, n, ct + 4 + (size_t)c * 8 + 4);
            size_t ai = ci + be32(d, n, ci + 4);
            for (int k = 0; k < 16; k++) st[c].coefs[k] = (int16_t)be16(d, n, ai + (size_t)k * 2);
        }
    p->pcm = calloc((size_t)p->frames * (size_t)ch, sizeof(int16_t));
    size_t at = sample0;
    for (uint32_t b = 0; b < blocks; b++) {
        bool last = b + 1 == blocks;
        size_t sz = last ? lpadded : bsize;
        int ns = (int)(last ? lsamples : bsamples);
        int16_t *o = p->pcm + (size_t)b * bsamples * (size_t)ch;
        if ((size_t)b * bsamples + (size_t)ns > (size_t)p->frames) ns = p->frames - (int)(b * bsamples);
        for (int c = 0; c < ch; c++, at += sz) {
            if (at >= n) break;
            size_t avail = n - at < sz ? n - at : sz;
            if (codec == 2) dsp_decode(d + at, avail, ns, &st[c], o + c, ch);
            else
                for (int i = 0; i < ns && (size_t)i * 2 + 1 < avail; i++) o[(size_t)i * (size_t)ch + (size_t)c] = (int16_t)be16(d, n, at + (size_t)i * 2);
        }
    }
    return p;
}

SPcm *smm1_bfwav_decode(const uint8_t *d, size_t n) {
    if (n < 0x40 || memcmp(d, "FWAV", 4)) return NULL;
    int nsec = be16(d, n, 0x10);
    size_t info = 0, data = 0;
    for (int i = 0; i < nsec; i++) {
        uint16_t id = be16(d, n, 0x14 + (size_t)i * 12);
        size_t off = be32(d, n, 0x14 + (size_t)i * 12 + 4);
        if (id == 0x7000) info = off;
        if (id == 0x7001) data = off;
    }
    if (!info || !data) return NULL;
    int enc = d[info + 8];
    SPcm *p = calloc(1, sizeof *p);
    p->loop = d[info + 9], p->rate = (int)be32(d, n, info + 0xC), p->loop_start = (int)be32(d, n, info + 0x10);
    p->frames = (int)be32(d, n, info + 0x14);
    size_t tab = info + 0x1C;
    int ch = (int)be32(d, n, tab);
    if (ch < 1 || ch > 8 || (enc != 1 && enc != 2) || !p->frames) { free(p); return NULL; }
    p->channels = ch;
    p->pcm = calloc((size_t)p->frames * (size_t)ch, sizeof(int16_t));
    for (int c = 0; c < ch; c++) {
        size_t ci = tab + be32(d, n, tab + 4 + (size_t)c * 8 + 4);
        size_t samples = data + 8 + be32(d, n, ci + 4);
        if (samples >= n) continue;
        if (enc == 2) {
            size_t ai = ci + be32(d, n, ci + 8 + 4);
            Dsp st;
            memset(&st, 0, sizeof st);
            for (int k = 0; k < 16; k++) st.coefs[k] = (int16_t)be16(d, n, ai + (size_t)k * 2);
            dsp_decode(d + samples, n - samples, p->frames, &st, p->pcm + c, ch);
        } else
            for (int i = 0; i < p->frames; i++) p->pcm[(size_t)i * (size_t)ch + (size_t)c] = (int16_t)be16(d, n, samples + (size_t)i * 2);
    }
    return p;
}

// SMM2's own sounds (little-endian, Switch): a BARS archive (u32 count at 0xC, then count name hashes, then per sound
// an (AMTA offset, BWAV offset) pair; the AMTA's STRG section holds the sound's name) of BWAVs. A BWAV: 0x10 header
// (u16 channel count at 0xE), then per channel 0x4C bytes of info: codec (0 PCM16, 1 DSP-ADPCM), rate, sample count,
// the ADPCM coefficients at +0x10, the channel's data offset (from the BWAV's start) at +0x34. Written from the
// project's own format notes (core/crates/formats/src/audio/bwav.rs, bars.rs).
static uint32_t le32(const uint8_t *d, size_t n, size_t o) {
    return o + 4 <= n ? (uint32_t)d[o] | (uint32_t)d[o + 1] << 8 | (uint32_t)d[o + 2] << 16 | (uint32_t)d[o + 3] << 24 : 0;
}
static uint16_t le16(const uint8_t *d, size_t n, size_t o) { return o + 2 <= n ? (uint16_t)(d[o] | d[o + 1] << 8) : 0; }

SPcm *smm1_bwav_decode(const uint8_t *d, size_t n) {
    if (n < 0x10 + 0x4C || memcmp(d, "BWAV", 4)) return NULL;
    int ch = le16(d, n, 0xE);
    if (ch < 1 || ch > 8 || 0x10 + (size_t)ch * 0x4C > n) return NULL;
    SPcm *p = calloc(1, sizeof *p);
    p->channels = ch, p->rate = (int)le32(d, n, 0x14), p->frames = (int)le32(d, n, 0x18);
    if (!p->rate || !p->frames) { free(p); return NULL; }
    p->pcm = calloc((size_t)p->frames * (size_t)ch, sizeof(int16_t));
    for (int c = 0; c < ch; c++) {
        size_t ci = 0x10 + (size_t)c * 0x4C, at = le32(d, n, ci + 0x34);
        int codec = le16(d, n, ci);
        if (at >= n) continue;
        if (codec == 1) {
            Dsp st;
            memset(&st, 0, sizeof st);
            for (int k = 0; k < 16; k++) st.coefs[k] = (int16_t)le16(d, n, ci + 0x10 + (size_t)k * 2);
            dsp_decode(d + at, n - at, p->frames, &st, p->pcm + c, ch);
        } else
            for (int i = 0; i < p->frames; i++) p->pcm[(size_t)i * (size_t)ch + (size_t)c] = (int16_t)le16(d, n, at + (size_t)i * 2);
    }
    return p;
}

SPcm *smm1_bars_sound(const uint8_t *d, size_t n, const char *name) {
    if (n < 0x10 || memcmp(d, "BARS", 4)) return NULL;
    uint32_t count = le32(d, n, 0xC);
    size_t pairs = 0x10 + (size_t)count * 4, len = strlen(name);
    for (uint32_t i = 0; i < count && pairs + (size_t)i * 8 + 8 <= n; i++) {
        size_t amta = le32(d, n, pairs + (size_t)i * 8), bwav = le32(d, n, pairs + (size_t)i * 8 + 4);
        if (amta + 0x20 > n || bwav >= n || memcmp(d + amta, "AMTA", 4)) continue;
        size_t end = amta + le32(d, n, amta + 8);  // (the AMTA's size)
        for (size_t s = amta; s + 8 + len < n && s + 8 + len <= end; s++)
            if (!memcmp(d + s, "STRG", 4)) {
                if (!memcmp(d + s + 8, name, len) && d[s + 8 + len] == 0) return smm1_bwav_decode(d + bwav, n - bwav);
                break;
            }
    }
    return NULL;
}

// ---------------------------------------------------------------- the mixer
typedef struct {
    SPcm *p;
    double pos;
    float gain, fade;  // fade: gain change per output sample (< 0 fading out)
    bool loop, on;
    int ch0;  // the first channel played (a multi-track stream's track: channels 2t, 2t + 1)
    float vol;  // the sound's own volume (its .bameta; a stream's also its track's) times its mix factor below
    float pitch;      // (smm1_se_ex) a factor on the rate (0: 1)
    int wait, left;   // (smm1_se_ex) output samples before it starts; after which it stops (0: none)
    int id;           // (smm1_se_ex) its handle
} Voice;

// SMM1's sounds against SMM2's mix: SMM1 played them through its own engine at its own output level, which SMM2's
// mix doesn't share. The .bameta volumes keep SMM1's balance between its sounds; these factors set their level next
// to the SMM2 sounds they stand in for. Measured with LARECOMP_AUDIO_WAV (macOS; levels are the loudest 50 ms RMS):
// - SMM1_MIX, the music (LARECOMP_SMM1_GAIN overrides it, for measuring). 2026-10-21: at 1 the select's music was
//   -27 dB RMS against -38 dB for SMM2's Endless menu and Course World music; 0.3 is -10.5 dB.
// - SMM1_SE_MIX, the screens' sound effects (select, map, card, pause, clear, ending; LARECOMP_SMM1_SE_GAIN). They
//   stand in for SMM2's menu sounds, which SMM2 plays quieter than its course sounds. 2026-10-23: at 0.3 the map's
//   loud ones (the Go's jump, M3_MoveMap, M3_Wipe, M3_StarsGather) were -18 to -19 dB, the same SMB1 jump wave in an
//   SMM2 course -28.3 to -28.9 dB, SMM2's loudest Course World menu sound -28.7 dB; 0.1 is -9.5 dB.
// - SMM1_COSTUME_MIX, the costumes' sounds (LARECOMP_SMM1_COSTUME_GAIN). They stand in for SMM2's course sounds, at
//   whose level they already are: 2026-10-23, at 0.3 the median costume jump / powerup / goal is -27.7 / -26.3 / -25.7
//   dB against SMM2's SMB1 jump -28.3 to -28.9, power-ups -26.1 to -27.1, goal fanfare -26.2.
// - SMM1_SE_CAP: no effect plays louder than SMM2's loudest course sounds (-24.5 to -26 dB: power-up, power-down,
//   Big Mushroom, goal fanfare). A quarter of the costumes' sounds are over it at their factor, the loudest by 9 dB
//   (a few jumps, goals and misses are shouts mastered near full scale); each is played at the cap instead.
#define SMM1_MIX 0.3f
#define SMM1_SE_MIX 0.1f
#define SMM1_COSTUME_MIX 0.3f
#define SMM1_SE_CAP 0.0562f  // (-25 dBFS)
static float env_gain(const char *name, float def) {
    const char *e = getenv(name);
    float g = e && *e ? (float)atof(e) : def;
    return g < 0 ? 0 : g;
}
static float smm1_mix(void) {
    static float g = -1;
    if (g < 0) g = env_gain("LARECOMP_SMM1_GAIN", SMM1_MIX);
    return g;
}
static float se_mix(bool costume) {
    static float g[2] = {-1, -1};
    if (g[costume] < 0)
        g[costume] = costume ? env_gain("LARECOMP_SMM1_COSTUME_GAIN", SMM1_COSTUME_MIX) : env_gain("LARECOMP_SMM1_SE_GAIN", SMM1_SE_MIX);
    return g[costume];
}
// a sound's loudest 50 ms RMS (0..1, all channels: a window sliding one frame at a time), the level SMM1_SE_CAP is in
static float pcm_level(const SPcm *p) {
    size_t ch = (size_t)p->channels, w = (size_t)(p->rate / 20 > 0 ? p->rate / 20 : 1), frames = (size_t)p->frames;
    if (w > frames) w = frames;
    double sum = 0, best = 0;
    for (size_t f = 0; f < frames; f++) {
        for (size_t c = 0; c < ch; c++) {
            double a = p->pcm[f * ch + c];
            sum += a * a;
            if (f >= w) {
                double b = p->pcm[(f - w) * ch + c];
                sum -= b * b;
            }
        }
        if (f + 1 >= w && sum > best) best = sum;
    }
    return w ? (float)(sqrt(best / (double)(w * ch)) / 32768) : 0;
}

// A .bameta (AMTA, big-endian): +0xC its DATA section's offset. DATA (after its 8-byte header): +0xC the volume
// (f32), +0xA the track count, +0x20 a table of 8 tracks (u32 channel count, f32 volume). out: the sound's volume and
// each track's (1 where there is none).
static bool amta_volumes(const uint8_t *d, size_t n, float *vol, float track[8]) {
    for (int i = 0; i < 8; i++) track[i] = 1;
    *vol = 1;
    if (n < 0x18 || memcmp(d, "AMTA", 4)) return false;
    size_t data = be32(d, n, 0xC);
    if (data + 8 + 0x60 > n || memcmp(d + data, "DATA", 4)) return false;
    const uint8_t *p = d + data + 8;
    uint32_t v = be32(p, 0x60, 0xC);
    memcpy(vol, &v, 4);
    if (!(*vol > 0 && *vol <= 4)) *vol = 1;
    int tracks = p[0xA] > 8 ? 8 : p[0xA];
    for (int i = 0; i < tracks; i++) {
        uint32_t t = be32(p, 0x60, 0x20 + (size_t)i * 8 + 4);
        memcpy(&track[i], &t, 4);
        if (!(track[i] > 0 && track[i] <= 4)) track[i] = 1;
    }
    return true;
}
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
#define SE_VOICES 16
static Voice g_bgm, g_se[SE_VOICES];
static char g_bgm_name[64];
static float g_game_gain = 1, g_game_target = 1;

static struct { char name[64]; SPcm *p; float vol, track[8]; } g_pcm[48];
static int g_npcm;
// a stream's .bameta is in its scene's sound archive (cached as sound/<scene>.bars)
static void stream_volumes(const char *name, float *vol, float track[8]) {
    static const char *scenes[] = {"NetWorldMapScene", "NetworkMainScene2", "NetEndingScene", "NetEndingDemoScene",
                                   "CourseListScene", "GameOverScene", NULL};
    char meta[96];
    snprintf(meta, sizeof meta, "%s.bameta", name);
    for (int i = 0; scenes[i]; i++) {
        size_t n, mn;
        uint8_t *d = smm1_sound_file(scenes[i], &n);
        LSarc s;
        if (!d || !lyt_sarc_open(&s, d, n)) { free(d); continue; }
        const uint8_t *m = lyt_sarc_get(&s, meta, &mn);
        bool ok = m && amta_volumes(m, mn, vol, track);
        lyt_sarc_close(&s);
        free(d);
        if (ok) return;
    }
    amta_volumes(NULL, 0, vol, track);
}
static int get_stream(const char *name) {  // its g_pcm index, -1: none
    for (int i = 0; i < g_npcm; i++)
        if (!strcmp(g_pcm[i].name, name)) return g_pcm[i].p ? i : -1;
    size_t n;
    uint8_t *d = smm1_sound_file(name, &n);
    SPcm *p = d ? smm1_bfstm_decode(d, n) : NULL;
    free(d);
    if (!p) fprintf(stderr, "[smm1ui] sound %s: missing\n", name);
    if (g_npcm >= 48) return -1;
    int i = g_npcm++;
    snprintf(g_pcm[i].name, sizeof g_pcm[0].name, "%s", name), g_pcm[i].p = p;
    if (p) stream_volumes(name, &g_pcm[i].vol, g_pcm[i].track);
    return p ? i : -1;
}
static int g_bgm_pcm = -1;  // g_bgm's g_pcm index

void smm1_bgm(const char *name, bool loop) {
    int k = name ? get_stream(name) : -1;
    SPcm *p = k >= 0 ? g_pcm[k].p : NULL;
    pthread_mutex_lock(&g_mx);
    g_bgm_pcm = k;
    g_bgm = (Voice){p, 0, 1, 0, loop && p && p->loop, p != NULL, 0, k >= 0 ? g_pcm[k].vol * g_pcm[k].track[0] * smm1_mix() : 1};
    if (p && loop && !p->loop) g_bgm.loop = true, p->loop_start = 0;
    snprintf(g_bgm_name, sizeof g_bgm_name, "%s", name ? name : "");
    pthread_mutex_unlock(&g_mx);
}
void smm1_bgm_track(int track) {
    pthread_mutex_lock(&g_mx);
    if (g_bgm.p) {
        int c = 2 * track;
        g_bgm.ch0 = c + 1 < g_bgm.p->channels ? c : 0;
        if (g_bgm_pcm >= 0) {
            int t = g_bgm.ch0 / 2 < 8 ? g_bgm.ch0 / 2 : 0;
            g_bgm.vol = g_pcm[g_bgm_pcm].vol * g_pcm[g_bgm_pcm].track[t] * smm1_mix();
        }
    }
    pthread_mutex_unlock(&g_mx);
}
double smm1_bgm_pos(void) {
    pthread_mutex_lock(&g_mx);
    double p = g_bgm.on && g_bgm.p ? g_bgm.pos : -1;
    pthread_mutex_unlock(&g_mx);
    return p;
}
const char *smm1_bgm_name(void) { return g_bgm.on ? g_bgm_name : ""; }
void smm1_bgm_fade(int frames) {
    pthread_mutex_lock(&g_mx);
    if (g_bgm.on) g_bgm.fade = -1.0f / (float)(frames > 0 ? frames * 800 : 1);  // (48000 / 60 samples a frame)
    pthread_mutex_unlock(&g_mx);
}
void smm1_game_sound(bool on) { g_game_target = on ? 1 : 0; }

// the sound effects: the screens' sound banks (Pack/Sound.pack -> Sound/Resource/<bank>.bars: a SARC of
// <name>.bfwav + <name>.bameta), loaded on first use (smm1_se_bank). Room for every costume's two banks (100
// costumes) besides the screens' own; a bank past that is not loaded (once said in the log), never loaded twice
#define SFX_BANKS 256
#define SFX_SOUNDS 4096
static struct { char name[80]; SPcm *p; float vol; } g_sfx[SFX_SOUNDS];  // vol: as played (factor and cap in)
static int g_nsfx;
static char g_banks[SFX_BANKS][64];
static int g_nbanks;
void smm1_se_bank(const char *bank) {
    for (int i = 0; i < g_nbanks; i++)
        if (!strcmp(g_banks[i], bank)) return;
    if (g_nbanks == SFX_BANKS || g_nsfx == SFX_SOUNDS) {
        static bool said;
        if (!said) fprintf(stderr, "[smm1ui] sound banks full: %s not loaded\n", bank);
        said = true;
        return;
    }
    snprintf(g_banks[g_nbanks++], 64, "%s", bank);
    size_t n;
    uint8_t *d = smm1_sound_file(bank, &n);
    LSarc s;
    if (!d || !lyt_sarc_open(&s, d, n)) { free(d); return; }
    for (int i = 0; i < s.count && g_nsfx < SFX_SOUNDS; i++) {
        const char *nm = s.e[i].name, *dot = strstr(nm, ".bfwav");
        if (!dot) continue;
        SPcm *p = smm1_bfwav_decode(s.data + s.e[i].start, s.e[i].end - s.e[i].start);
        if (!p) continue;
        char meta[96];
        size_t mn;
        float track[8];
        snprintf(meta, sizeof meta, "%.*s.bameta", (int)(dot - nm), nm);
        const uint8_t *m = lyt_sarc_get(&s, meta, &mn);
        float vol = 1, level = pcm_level(p);
        if (m) amta_volumes(m, mn, &vol, track);
        // (a costume's bank: its waves are named "<bank>/<wave>", every costume has a "jump", a "goal"...)
        bool costume = !strncmp(bank, "CharaMario_", 11);
        if (costume) snprintf(g_sfx[g_nsfx].name, sizeof g_sfx[0].name, "%s/%.*s", bank, (int)(dot - nm), nm);
        else snprintf(g_sfx[g_nsfx].name, sizeof g_sfx[0].name, "%.*s", (int)(dot - nm), nm);
        vol *= se_mix(costume);
        if (level * vol > SMM1_SE_CAP) vol = SMM1_SE_CAP / level;
        g_sfx[g_nsfx].vol = vol;
        g_sfx[g_nsfx++].p = p;
    }
    lyt_sarc_close(&s);
    free(d);
}
bool smm1_se_has(const char *name) {
    for (int i = 0; i < g_nsfx; i++)
        if (!strcmp(g_sfx[i].name, name)) return true;
    return false;
}
bool smm1_se(const char *name) {
    for (int i = 0; i < g_nsfx; i++)
        if (!strcmp(g_sfx[i].name, name)) {
            pthread_mutex_lock(&g_mx);
            for (int k = 0; k < SE_VOICES; k++)
                if (!g_se[k].on) { g_se[k] = (Voice){g_sfx[i].p, 0, 1, 0, false, true, 0, g_sfx[i].vol}; break; }
            pthread_mutex_unlock(&g_mx);
            return true;
        }
    return false;
}
void smm1_play_vol(SPcm *p, float vol) {
    if (!p) return;
    pthread_mutex_lock(&g_mx);
    for (int k = 0; k < SE_VOICES; k++)
        if (!g_se[k].on) { g_se[k] = (Voice){p, 0, 1, 0, false, true, 0, vol}; break; }
    pthread_mutex_unlock(&g_mx);
}
void smm1_play(SPcm *p) { smm1_play_vol(p, 1); }
int smm1_se_ex(const char *name, float vol, float pitch, int delay, int stop, bool loop) {
    static int next;
    for (int i = 0; i < g_nsfx; i++)
        if (!strcmp(g_sfx[i].name, name)) {
            const SPcm *p = g_sfx[i].p;
            int id = 0, per_frame = 48000 / 60;  // (the mixer's rate: audren's 48 kHz)
            pthread_mutex_lock(&g_mx);
            for (int k = 0; k < SE_VOICES; k++)
                if (!g_se[k].on) {
                    id = ++next > 0 ? next : (next = 1);
                    g_se[k] = (Voice){g_sfx[i].p, 0, 1, 0, loop && p->loop, true, 0, g_sfx[i].vol * vol};
                    g_se[k].pitch = pitch > 0 ? pitch : 1, g_se[k].id = id;
                    g_se[k].wait = delay > 0 ? delay * per_frame : 0, g_se[k].left = stop > 0 ? stop * per_frame : 0;
                    break;
                }
            pthread_mutex_unlock(&g_mx);
            return id ? id : -1;  // (-1: found, but no voice free)
        }
    return 0;
}
int smm1_se_frames(const char *name) {
    for (int i = 0; i < g_nsfx; i++)
        if (!strcmp(g_sfx[i].name, name) && g_sfx[i].p->rate > 0)
            return (int)((int64_t)g_sfx[i].p->frames * 60 / g_sfx[i].p->rate);
    return 0;
}
void smm1_se_stop(int id) {
    if (id <= 0) return;
    pthread_mutex_lock(&g_mx);
    for (int k = 0; k < SE_VOICES; k++)
        if (g_se[k].on && g_se[k].id == id) g_se[k].on = false;
    pthread_mutex_unlock(&g_mx);
}

static void mix_voice(Voice *v, float *out, int n, int rate) {
    if (!v->on || !v->p) return;
    const SPcm *p = v->p;
    double step = (double)p->rate / rate * (v->pitch > 0 ? v->pitch : 1);
    for (int i = 0; i < n; i++) {
        if (v->wait > 0) { v->wait--; continue; }
        if (v->left > 0 && --v->left == 0) { v->on = false; return; }
        int k = (int)v->pos;
        if (k >= p->frames - 1) {
            if (v->loop && p->loop_start < p->frames - 1) v->pos = p->loop_start + (v->pos - k), k = (int)v->pos;
            else { v->on = false; return; }
        }
        float f = (float)(v->pos - k), g = v->gain * v->vol * (1.0f / 32768);
        const int16_t *a = p->pcm + (size_t)k * (size_t)p->channels + v->ch0, *b = a + p->channels;
        float l = (a[0] + (b[0] - a[0]) * f) * g;
        float r = p->channels > 1 + v->ch0 ? (a[1] + (b[1] - a[1]) * f) * g : l;
        out[2 * i] += l, out[2 * i + 1] += r;
        v->pos += step;
        if (v->fade < 0 && (v->gain += v->fade) <= 0) { v->on = false; return; }
    }
}

// runtime/services/audio.c: the game's mixed output (interleaved stereo float, rate Hz), just before it is played
void game_audio_mix(float *stereo, int n, int rate) {
    pthread_mutex_lock(&g_mx);
    float step = 1.0f / (rate / 4.0f);  // the game's sound fades in / out over a quarter of a second
    if (g_game_gain != 1 || g_game_target != 1)
        for (int i = 0; i < n; i++) {
            if (g_game_gain < g_game_target) g_game_gain = g_game_gain + step > g_game_target ? g_game_target : g_game_gain + step;
            else if (g_game_gain > g_game_target) g_game_gain = g_game_gain - step < g_game_target ? g_game_target : g_game_gain - step;
            stereo[2 * i] *= g_game_gain, stereo[2 * i + 1] *= g_game_gain;
        }
    mix_voice(&g_bgm, stereo, n, rate);
    for (int k = 0; k < SE_VOICES; k++) mix_voice(&g_se[k], stereo, n, rate);
    pthread_mutex_unlock(&g_mx);
}
