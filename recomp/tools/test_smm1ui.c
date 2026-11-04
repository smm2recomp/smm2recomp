// Development check of the SMM1 layout player (games/smm2/mods/smm1): render layouts from the player's own SMM1 dump
// to PNG files without the game.
//
//   test_smm1ui <out.png> <Layout> [Anim@frame ...] [text:Pane=UTF-8 ...] [hide:Pane] [show:Pane]
//   test_smm1ui --flow <out_%04d.png> <screen> <first> <last> [step]   (the 100 Mario screens, see flow_test in flow.c)
//   test_smm1ui --title-letter <LETTER> <out_%04d.png> <first> <last> [step] [second tap]   (an SMM1 title letter's
//       animation at SMM2's letter, games/smm2/mods/title_letters/anim.c; LETTER: SUPER, M, A, R, I, O, M2, A2, K, E,
//       R2; tapped at frame 1, the toggles I and K again at <second tap>)
//
// Run from a data folder (the cache is generated into ./mods-generated); built by tools/test_smm1ui.sh.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../games/smm2/mods/smm1/lyt.h"
#include "../games/smm2/mods/smm1/smm1_data.h"
#include "../games/smm2/mods/smm1/bfres.h"
#include "../games/smm2/mods/smm1/smm1_audio.h"
#include "../games/smm2/mods/title_letters/title_letters.h"
#include <math.h>

// ---- a minimal PNG writer (stored deflate blocks)
static uint32_t crc_table[256];
static uint32_t crc(uint32_t c, const uint8_t *b, size_t n) {
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    c = ~c;
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ b[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v; }
static void chunk(FILE *f, const char *type, const uint8_t *d, size_t n) {
    uint8_t h[8];
    be32(h, (uint32_t)n);
    memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    fwrite(d, 1, n, f);
    uint32_t c = crc(crc(0, (const uint8_t *)type, 4), d, n);
    be32(h, c);
    fwrite(h, 1, 4, f);
}
int write_png(const char *path, const uint8_t *rgba, int w, int h) {
    size_t raw_n = (size_t)h * (1 + (size_t)w * 4);
    uint8_t *raw = malloc(raw_n);
    for (int y = 0; y < h; y++) {
        raw[(size_t)y * (1 + w * 4)] = 0;
        memcpy(raw + (size_t)y * (1 + w * 4) + 1, rgba + (size_t)y * w * 4, (size_t)w * 4);
    }
    size_t blocks = (raw_n + 65534) / 65535, zn = 2 + raw_n + blocks * 5 + 4;
    uint8_t *z = malloc(zn), *p = z;
    *p++ = 0x78, *p++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t o = 0; o < raw_n; o += 65535) {
        size_t len = raw_n - o < 65535 ? raw_n - o : 65535;
        *p++ = o + len >= raw_n;
        *p++ = len & 0xFF, *p++ = len >> 8, *p++ = ~len & 0xFF, *p++ = (~len >> 8) & 0xFF;
        memcpy(p, raw + o, len);
        p += len;
    }
    for (size_t i = 0; i < raw_n; i++) a = (a + raw[i]) % 65521, b = (b + a) % 65521;
    be32(p, b << 16 | a);
    p += 4;
    FILE *f = fopen(path, "wb");
    if (!f) { free(raw), free(z); return 1; }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ih[13];
    be32(ih, (uint32_t)w), be32(ih + 4, (uint32_t)h);
    ih[8] = 8, ih[9] = 6, ih[10] = ih[11] = ih[12] = 0;
    chunk(f, "IHDR", ih, 13);
    chunk(f, "IDAT", z, (size_t)(p - z));
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw), free(z);
    return 0;
}

// SMM2's letter panes at rest on its title (game pixels; from a run: the logo's chain of translations and scales)
static TlBox smm2_pane(int letter) {
    static const float tx[TL_COUNT] = {-224, -301, -231, -167, -121, -68, 28, 98, 162, 222, 284};
    float cx = 590 + tx[letter], cy = letter == TL_SUPER ? 166 : 242, w = letter == TL_SUPER ? 223.2f : 76.8f;
    float h = letter == TL_SUPER ? 62 : 76.8f;
    return (TlBox){cx - w / 2, cy - h / 2, w, h, true};
}
static int title_letter_main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "--title-letter <LETTER> <out_%%04d.png> <first> <last> [step] [second tap]\n");
        return 2;
    }
    int letter = TL_NONE;
    for (int i = 0; i < TL_COUNT; i++)
        if (!strcmp(argv[1], tl_letter_name[i])) letter = i;
    if (letter == TL_NONE) { fprintf(stderr, "no letter %s\n", argv[1]); return 2; }
    long first = atol(argv[3]), last = atol(argv[4]), step = argc > 5 ? atol(argv[5]) : 1;
    long tap2 = argc > 6 ? atol(argv[6]) : -1;
    if (step < 1) step = 1;
    for (int i = 0; i < TL_COUNT; i++) { TlBox b = smm2_pane(i); tl_anim_box(i, &b); }
    uint8_t *px = malloc(1280 * 720 * 4);
    LQuads q = {0};
    for (long t = 1; t <= last; t++) {
        if (t == 1 || t == tap2)
            fprintf(stderr, "frame %ld: tap %s\n", t, tl_anim_start(letter, (int)t) ? "plays" : "ignored");
        bool busy = tl_anim_busy(letter, (int)t);
        q.n = 0;
        bool any = tl_anim_draw((int)t, &q);  // (also on frames not written: the layout is loaded at the first)
        if (t < first || (t - first) % step) continue;
        const uint8_t board[4] = {216, 216, 208, 255};  // (SMM2's logo board, roughly)
        lyt_clear(px, 1280, 720, board);
        TlBox b = smm2_pane(letter);  // (SMM2's pane: a red frame)
        for (int x = (int)b.x; x <= (int)(b.x + b.w); x++)
            for (int k = 0; k < 2; k++) {
                uint8_t *p = px + ((size_t)(k ? (int)(b.y + b.h) : (int)b.y) * 1280 + (size_t)x) * 4;
                p[0] = 255, p[1] = p[2] = 0;
            }
        for (int y = (int)b.y; y <= (int)(b.y + b.h); y++)
            for (int k = 0; k < 2; k++) {
                uint8_t *p = px + ((size_t)y * 1280 + (size_t)(k ? (int)(b.x + b.w) : (int)b.x)) * 4;
                p[0] = 255, p[1] = p[2] = 0;
            }
        if (any) lyt_raster(&q, px, 1280, 720, 1280, 720);
        char path[512];
        snprintf(path, sizeof path, argv[2], (int)t);
        write_png(path, px, 1280, 720);
        fprintf(stderr, "frame %ld: %s, %d quads -> %s\n", t, busy ? "SMM1's letter" : "SMM2's letter", q.n, path);
    }
    free(px);
    return 0;
}

__attribute__((weak)) int flow_test_main(int argc, char **argv) {  // flow.c's, when linked
    (void)argc, (void)argv;
    fprintf(stderr, "built without the flow\n");
    return 2;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <out.png> <Layout> [Anim@frame ...] [text:Pane=...] [hide:Pane] [show:Pane]\n", argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "--bfres")) {  // --bfres <Output.bfres>: what bfres.c reads
        size_t n;
        uint8_t *d = smm1_read_file(argv[2], &n);
        Bfres *b = d ? bfres_load(d, n) : NULL;
        if (!b) return 1;
        bfres_dump(b);
        if (argc > 3)  // --bfres <Output.bfres> <out_dir>: its textures as <out_dir>/<name>.png too
            for (int i = 0; i < b->ntex; i++) {
                const LTex *t = lyt_tex(b->tex[i]);
                char path[512];
                snprintf(path, sizeof path, "%s/%s.png", argv[3], b->tex_name[i]);
                if (t && t->px) write_png(path, t->px, t->w, t->h);
            }
        return 0;
    }
    if (!smm1_data_open()) return 1;
    if (!strcmp(argv[1], "--flow")) return flow_test_main(argc - 1, argv + 1);
    if (!strcmp(argv[1], "--title-letter")) return title_letter_main(argc - 1, argv + 1);
    if (!strcmp(argv[1], "--lanky")) {  // --lanky <sheet.png>: the Slim Mario models (mods/smm1/costume_gen.c), shown
        extern bool lanky_generate(void), lanky_sheet(const char *path);
        return lanky_generate() && lanky_sheet(argv[2]) ? 0 : 1;
    }
    if (!strcmp(argv[1], "--costumes")) {  // generate the costume models (mods/smm1/costume_gen.c)
        extern bool costume_generate(void);
        return costume_generate() ? 0 : 1;
    }
    if (!strcmp(argv[1], "--sound") && argc > 3) {  // --sound <stream name> <out.wav>: decode one stream
        size_t n;
        uint8_t *d = smm1_sound_file(argv[2], &n);
        SPcm *p = d ? smm1_bfstm_decode(d, n) : NULL;
        if (!p) { fprintf(stderr, "no stream %s\n", argv[2]); return 1; }
        int peak = 0;
        double sum = 0;
        for (size_t i = 0; i < (size_t)p->frames * (size_t)p->channels; i++) {
            int v = abs(p->pcm[i]);
            if (v > peak) peak = v;
            sum += (double)v * v;
        }
        fprintf(stderr, "%s: %d Hz, %d ch, %d frames (%.2f s), loop %d from %d, peak %d, rms %.0f\n", argv[2], p->rate,
                p->channels, p->frames, (double)p->frames / p->rate, p->loop, p->loop_start, peak,
                sqrt(sum / ((double)p->frames * p->channels)));
        FILE *f = fopen(argv[3], "wb");
        uint32_t bytes = (uint32_t)p->frames * (uint32_t)p->channels * 2;
        uint32_t h[11] = {0x46464952, 36 + bytes, 0x45564157, 0x20746d66, 16, 0x00010001u | ((uint32_t)p->channels << 16) - 0x10000 + 0,
                          (uint32_t)p->rate, (uint32_t)p->rate * (uint32_t)p->channels * 2, 0, 0x61746164, bytes};
        h[5] = 1u | ((uint32_t)p->channels << 16);
        h[8] = (uint32_t)(p->channels * 2) | (16u << 16);
        fwrite(h, 4, 11, f);
        fwrite(p->pcm, 2, (size_t)p->frames * (size_t)p->channels, f);
        fclose(f);
        return 0;
    }
    LRes *r = lyt_load(argv[2]);
    if (!r) return 1;
    LInst *in = lyt_inst(r);
    for (int i = 3; i < argc; i++) {
        char *a = argv[i], *eq;
        if (!strncmp(a, "text:", 5) && (eq = strchr(a, '='))) {
            *eq = 0;
            lyt_set_text8(in, a + 5, eq + 1);
        } else if (!strncmp(a, "hide:", 5))
            lyt_set_visible(in, a + 5, 0);
        else if (!strncmp(a, "show:", 5))
            lyt_set_visible(in, a + 5, 1);
        else if ((eq = strchr(a, '@'))) {
            *eq = 0;
            if (!lyt_bind(in, a, (float)atof(eq + 1))) fprintf(stderr, "no animation %s\n", a);
        }
    }
    LQuads q = {0};
    lyt_draw(in, &q);
    int w = 1280, h = 720;
    uint8_t *px = malloc((size_t)w * h * 4);
    const uint8_t bg[4] = {40, 40, 40, 255};
    lyt_clear(px, w, h, bg);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    lyt_raster(&q, px, w, h, r->w > 0 ? r->w : 1280, r->h > 0 ? r->h : 720);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    fprintf(stderr, "%s: %d panes, %d anims, %d quads, raster %.1f ms\n", argv[2], r->npane, r->nanim, q.n,
            (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
    if (getenv("LIST")) {
        for (int i = 0; i < r->nanim; i++) fprintf(stderr, "  anim %s (%d)\n", r->anim[i].name, r->anim[i].frames);
        for (int i = 0; i < r->npane; i++)
            fprintf(stderr, "  pane %*s%s kind %d%s%s\n", 0, "", r->pane[i].name, r->pane[i].kind,
                    r->pane[i].kind == LP_PRT ? " -> " : "", r->pane[i].kind == LP_PRT ? r->pane[i].prt.layout : "");
        for (int i = 0; i < r->msg.count; i++) {
            fprintf(stderr, "  msg %s: ", r->msg.m[i].label);
            for (int k = 0; k < r->msg.m[i].n; k++) {
                uint32_t c = r->msg.m[i].text[k];
                if (c < 0x80) fputc((int)c, stderr);
                else fprintf(stderr, "<%x>", c);
            }
            fputc('\n', stderr);
        }
    }
    return write_png(argv[1], px, w, h);
}
