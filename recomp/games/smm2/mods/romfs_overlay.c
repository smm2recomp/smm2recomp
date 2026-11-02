// The mods' shared RomFS overlay (romfs_overlay.h): generators, the generated folder, Yaz0 / SARC / MSBT /
// BFLYT editing.
// Formats: Yaz0 (decoded by lyt_yaz0; written uncompressed, every flag bit a literal), SARC (little endian), MSBT
// LBL1 / TXT2, BFLYT sections. Generators register from constructors; the generation runs at the first
// game_romfs_overlay() call (the RomFS opening), into a fresh folder renamed into place when all have run.
#include "romfs_overlay.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mod.h"
#include "../../../runtime/platform.h"
#include "../../../runtime/usermods.h"  // (the player's mods: what generators read first, and what's layered below)  // (mkdir with a mode and plat_remove_tree on every host)
#include "smm1/lyt.h"  // lyt_yaz0

typedef RovBuf Buf;

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t u64(const uint8_t *p) { return (uint64_t)u32(p) | (uint64_t)u32(p + 4) << 32; }
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void w16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> 8 * i); }

static void put(Buf *b, const void *d, size_t n) {
    b->d = realloc(b->d, b->n + n + 1);
    if (d) memcpy(b->d + b->n, d, n);
    else memset(b->d + b->n, 0, n);
    b->n += n;
}
static void put32(Buf *b, uint32_t v) { uint8_t t[4]; w32(t, v); put(b, t, 4); }
static void pad_to(Buf *b, size_t align, uint8_t fill) {
    while (b->n % align) put(b, &fill, 1);
}

static bool read_host(const char *path, Buf *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out->d = malloc((size_t)n + 1), out->n = (size_t)n;
    bool ok = n >= 0 && fread(out->d, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    return ok;
}

// ---- a file of the game's RomFS: the extracted folder, or the romfs.bin image (level-3 RomFS)
static bool romfs_bin_read(const char *image, const char *path, Buf *out) {
    FILE *f = fopen(image, "rb");
    if (!f) return false;
    uint8_t h[0x50];
    bool ok = false;
    if (fread(h, 1, sizeof h, f) != sizeof h) goto done;
    uint64_t doff = u64(h + 0x18), dsz = u64(h + 0x20), foff = u64(h + 0x38), fsz = u64(h + 0x40), data = u64(h + 0x48);
    if (dsz > (64u << 20) || fsz > (64u << 20)) goto done;
    uint8_t *dm = malloc(dsz + 1), *fm = malloc(fsz + 1);
    if (fseeko(f, (off_t)doff, SEEK_SET) || fread(dm, 1, dsz, f) != dsz || fseeko(f, (off_t)foff, SEEK_SET) ||
        fread(fm, 1, fsz, f) != fsz) {
        free(dm), free(fm);
        goto done;
    }
    for (uint64_t o = 0; o + 0x20 <= fsz && !ok;) {
        uint32_t parent = u32(fm + o), nl = u32(fm + o + 0x1c);
        uint64_t at = u64(fm + o + 8), size = u64(fm + o + 0x10);
        // the path: the parents' names, then the file's
        char full[512] = "", tmp[512];
        for (uint32_t d = parent; d && d + 0x18 <= dsz;) {
            uint32_t dnl = u32(dm + d + 0x14);
            if (d + 0x18 + dnl > dsz || dnl > 200) break;
            snprintf(tmp, sizeof tmp, "%.*s/%s", (int)dnl, (const char *)dm + d + 0x18, full);
            snprintf(full, sizeof full, "%s", tmp);
            d = u32(dm + d);
        }
        if (o + 0x20 + nl <= fsz && nl < 200) {
            snprintf(tmp, sizeof tmp, "%s%.*s", full, (int)nl, (const char *)fm + o + 0x20);
            if (!strcmp(tmp, path)) {
                out->d = malloc(size + 1), out->n = size;
                ok = !fseeko(f, (off_t)(data + at), SEEK_SET) && fread(out->d, 1, size, f) == size;
            }
        }
        o += 0x20 + ((nl + 3) & ~3u);
    }
    free(dm), free(fm);
done:
    fclose(f);
    return ok;
}

static bool romfs_read(const char *path, Buf *out) {
    char p[1200];
    // the player's mods first (runtime/usermods.h): a generator edits the file the game would otherwise get
    if (usermods_find(path, p, sizeof p) && read_host(p, out)) return true;
    snprintf(p, sizeof p, "%s/%s", g_romfs_dir ? g_romfs_dir : "", path);
    if (read_host(p, out)) return true;
    snprintf(p, sizeof p, "%s.bin", g_romfs_dir ? g_romfs_dir : "");
    return romfs_bin_read(p, path, out);
}

// ---- Yaz0 (decoded by lyt_yaz0; written as literals: every 8 bytes behind a 0xff flag byte)
static Buf yaz0_store(const Buf *d, uint32_t align) {
    Buf o = {0};
    uint8_t h[16] = {'Y', 'a', 'z', '0'};
    h[4] = (uint8_t)(d->n >> 24), h[5] = (uint8_t)(d->n >> 16), h[6] = (uint8_t)(d->n >> 8), h[7] = (uint8_t)d->n;
    h[8] = (uint8_t)(align >> 24), h[9] = (uint8_t)(align >> 16), h[10] = (uint8_t)(align >> 8), h[11] = (uint8_t)align;
    put(&o, h, 16);
    for (size_t i = 0; i < d->n; i += 8) {
        uint8_t flag = 0xff;
        put(&o, &flag, 1);
        put(&o, d->d + i, d->n - i < 8 ? d->n - i : 8);
    }
    return o;
}

// ---- SARC (little endian): read into entries, write back with each file at its old alignment
typedef struct { char name[128]; Buf data; uint32_t hash, attr, align; } SEntry;
typedef struct { SEntry *e; int n; uint32_t mul, data_align; } Sarc;

static bool sarc_read(const uint8_t *d, size_t n, Sarc *s) {
    memset(s, 0, sizeof *s);
    if (n < 0x14 || memcmp(d, "SARC", 4)) return false;
    uint32_t hl = u16(d + 4), data_off = u32(d + 0xc);
    if (hl + 12 > n || memcmp(d + hl, "SFAT", 4)) return false;
    int cnt = u16(d + hl + 6);
    s->mul = u32(d + hl + 8);
    size_t names = hl + 12 + 16 * (size_t)cnt + 8;
    if (names > n) return false;
    s->e = calloc((size_t)cnt, sizeof *s->e), s->n = cnt;
    s->data_align = 1;
    while (s->data_align < 0x2000 && data_off % (s->data_align * 2) == 0) s->data_align *= 2;
    for (int i = 0; i < cnt; i++) {
        const uint8_t *nd = d + hl + 12 + 16 * i;
        SEntry *e = &s->e[i];
        e->hash = u32(nd), e->attr = u32(nd + 4);
        uint32_t st = u32(nd + 8), en = u32(nd + 12);
        size_t no = names + (e->attr & 0xffff) * 4;
        if (no >= n || data_off + (size_t)en > n || en < st) return false;
        snprintf(e->name, sizeof e->name, "%s", (const char *)d + no);
        e->data.n = en - st, e->data.d = malloc(e->data.n + 1);
        memcpy(e->data.d, d + data_off + st, e->data.n);
        e->align = 1;
        while (e->align < 0x2000 && st % (e->align * 2) == 0) e->align *= 2;
        if (e->align < 4) e->align = 4;
    }
    return true;
}

static int by_hash(const void *a, const void *b) {
    uint32_t x = ((const SEntry *)a)->hash, y = ((const SEntry *)b)->hash;
    return x < y ? -1 : x > y;
}

static Buf sarc_write(Sarc *s) {
    qsort(s->e, (size_t)s->n, sizeof *s->e, by_hash);
    Buf names = {0};
    uint32_t *no = calloc((size_t)s->n + 1, 4);
    for (int i = 0; i < s->n; i++) {
        no[i] = (uint32_t)names.n;
        put(&names, s->e[i].name, strlen(s->e[i].name) + 1);
        pad_to(&names, 4, 0);
    }
    size_t hdr = 0x14 + 12 + 16 * (size_t)s->n + 8 + names.n;
    size_t data_off = (hdr + s->data_align - 1) / s->data_align * s->data_align;
    Buf data = {0}, nodes = {0};
    for (int i = 0; i < s->n; i++) {
        while ((data_off + data.n) % s->e[i].align) put(&data, NULL, 1);
        uint32_t st = (uint32_t)data.n;
        put(&data, s->e[i].data.d, s->e[i].data.n);
        put32(&nodes, s->e[i].hash), put32(&nodes, (s->e[i].attr & 0xffff0000u) | no[i] / 4);
        put32(&nodes, st), put32(&nodes, (uint32_t)data.n);
    }
    Buf o = {0};
    uint8_t h[0x14] = {'S', 'A', 'R', 'C'};
    w16(h + 4, 0x14), w16(h + 6, 0xfeff), w32(h + 0xc, (uint32_t)data_off), w16(h + 0x10, 0x100);
    put(&o, h, sizeof h);
    uint8_t sf[12] = {'S', 'F', 'A', 'T'};
    w16(sf + 4, 12), w16(sf + 6, (uint32_t)s->n), w32(sf + 8, s->mul);
    put(&o, sf, 12);
    put(&o, nodes.d, nodes.n);
    uint8_t sn[8] = {'S', 'F', 'N', 'T'};
    w16(sn + 4, 8);
    put(&o, sn, 8);
    put(&o, names.d, names.n);
    put(&o, NULL, data_off - o.n);
    put(&o, data.d, data.n);
    w32(o.d + 8, (uint32_t)o.n);
    free(names.d), free(nodes.d), free(data.d), free(no);
    return o;
}

static void sarc_free(Sarc *s) {
    for (int i = 0; i < s->n; i++) free(s->e[i].data.d);
    free(s->e);
    memset(s, 0, sizeof *s);
}

static SEntry *sarc_find(Sarc *s, const char *name) {
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->e[i].name, name)) return &s->e[i];
    return NULL;
}


// ================================================================ the overlay folder and the generators
#define ROV_ROOT "mods-generated"
static struct { char name[48]; int version; RovGenerator fn; } gens[16];
static int ngens;
static char gen_dir[256];   // the folder being generated (writes go there), or the finished one
static bool generating;

void rov_register(const char *name, int version, RovGenerator fn) {
    if (ngens >= 16) return;
    snprintf(gens[ngens].name, sizeof gens[ngens].name, "%s", name);
    gens[ngens].version = version, gens[ngens].fn = fn;
    ngens++;
}

static bool mkdirs(const char *path) {
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            if (mkdir(p, 0755) && errno != EEXIST) return false;
            *s = '/';
        }
    return !mkdir(p, 0755) || errno == EEXIST;
}

static bool write_path(const char *path, const void *d, size_t n) {
    char dir[1100], tmp[1100];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0, mkdirs(dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(d, 1, n, f) == n;
    ok = !fclose(f) && ok;
    return ok && !rename(tmp, path);
}

bool rov_write(const char *rel, const void *d, size_t n) {
    if (!gen_dir[0]) return false;
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", gen_dir, rel);
    return write_path(p, d, n);
}

bool rov_read_game(const char *rel, RovBuf *out) { return romfs_read(rel, out); }

bool rov_read(const char *rel, RovBuf *out) {
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", gen_dir, rel);
    return (gen_dir[0] && read_host(p, out)) || romfs_read(rel, out);
}

void rov_free(RovBuf *b) {
    free(b->d);
    b->d = NULL, b->n = 0;
}

// ---- archives
struct RovSarc { Sarc s; bool yaz0; uint32_t align; };

RovSarc *rov_sarc_parse(const RovBuf *b) {
    RovSarc *o = calloc(1, sizeof *o);
    size_t dn = 0;
    uint8_t *dec = b->n >= 16 && !memcmp(b->d, "Yaz0", 4) ? lyt_yaz0(b->d, b->n, &dn) : NULL;
    if (dec) o->yaz0 = true, o->align = be32(b->d + 8);
    bool ok = dec ? sarc_read(dec, dn, &o->s) : sarc_read(b->d, b->n, &o->s);
    free(dec);
    if (!ok) sarc_free(&o->s), free(o), o = NULL;
    return o;
}

RovSarc *rov_sarc_load(const char *rel) {
    RovBuf b = {0};
    if (!rov_read(rel, &b)) return NULL;
    RovSarc *o = rov_sarc_parse(&b);
    rov_free(&b);
    return o;
}

RovBuf *rov_sarc_member(RovSarc *s, const char *name) {
    SEntry *e = s ? sarc_find(&s->s, name) : NULL;
    return e ? &e->data : NULL;
}

const char *rov_sarc_name(RovSarc *s, int i) { return s && i >= 0 && i < s->s.n ? s->s.e[i].name : NULL; }

RovBuf rov_sarc_build(RovSarc *s) {
    Buf w = sarc_write(&s->s);
    if (!s->yaz0) return w;
    Buf y = yaz0_store(&w, s->align ? s->align : 0x80);
    free(w.d);
    return y;
}

bool rov_sarc_store(RovSarc *s, const char *rel) {
    if (!s) return false;
    bool ok = true;
    if (rel) {
        RovBuf b = rov_sarc_build(s);
        ok = rov_write(rel, b.d, b.n);
        rov_free(&b);
    }
    sarc_free(&s->s);
    free(s);
    return ok;
}

// ---- MSBT: the text of one label replaced
static void put_utf16(Buf *b, const char *s) {  // UTF-8 -> UTF-16LE (BMP), NUL-terminated
    const uint8_t *p = (const uint8_t *)s;
    while (*p) {
        uint32_t c = *p++;
        if (c >= 0xe0 && p[0] && p[1]) c = (c & 0xf) << 12 | (p[0] & 0x3fu) << 6 | (p[1] & 0x3fu), p += 2;
        else if (c >= 0xc0 && p[0]) c = (c & 0x1f) << 6 | (p[0] & 0x3fu), p++;
        uint8_t u[2] = {(uint8_t)c, (uint8_t)(c >> 8)};
        put(b, u, 2);
    }
    put(b, NULL, 2);
}

// The label's message: its TXT2 section (body offset, size) and index; false: not an MSBT / no such label / damaged
static bool msbt_find(const RovBuf *m, const char *label, size_t *txt_at, size_t *txt_size, uint32_t *index) {
    if (!m || m->n < 0x20 || memcmp(m->d, "MsgStdBn", 8) || u16(m->d + 8) != 0xfeff) return false;
    int nsec = u16(m->d + 0xe);
    size_t o = 0x20, lbl = 0, lbl_n = 0, txt = 0, txt_n = 0;
    for (int i = 0; i < nsec; i++) {
        if (o + 16 > m->n) return false;
        uint32_t size = u32(m->d + o + 4);
        if (o + 16 + size > m->n) return false;
        if (!memcmp(m->d + o, "LBL1", 4)) lbl = o + 16, lbl_n = size;
        if (!memcmp(m->d + o, "TXT2", 4)) txt = o + 16, txt_n = size;
        o = (o + 16 + size + 15) & ~(size_t)15;
    }
    if (!lbl || !txt || lbl_n < 4 || txt_n < 4) return false;
    uint32_t slots = u32(m->d + lbl), idx = UINT32_MAX;  // the label's message index (LBL1: hash slots of labels)
    size_t want = strlen(label);
    for (uint32_t s = 0; s < slots && 12 + 8 * (size_t)s <= lbl_n && idx == UINT32_MAX; s++) {
        uint32_t cnt = u32(m->d + lbl + 4 + 8 * s);
        size_t p = u32(m->d + lbl + 8 + 8 * s);
        for (uint32_t k = 0; k < cnt && p < lbl_n; k++) {
            size_t ln = m->d[lbl + p];
            if (p + 1 + ln + 4 > lbl_n) return false;
            if (ln == want && !memcmp(m->d + lbl + p + 1, label, ln)) idx = u32(m->d + lbl + p + 1 + ln);
            p += 1 + ln + 4;
        }
    }
    uint32_t n = u32(m->d + txt);
    if (idx >= n || 4 + 4 * (size_t)n > txt_n) return false;
    *txt_at = txt, *txt_size = txt_n, *index = idx;
    return true;
}

int rov_msbt_get(const RovBuf *m, const char *label, uint16_t *out, int cap) {
    size_t txt, txt_n;
    uint32_t idx;
    if (cap < 1 || !msbt_find(m, label, &txt, &txt_n, &idx)) return -1;
    uint32_t n = u32(m->d + txt), a = u32(m->d + txt + 4 + 4 * idx);
    uint32_t e = idx + 1 < n ? u32(m->d + txt + 8 + 4 * idx) : (uint32_t)txt_n;
    if (a > e || e > txt_n) return -1;
    int len = 0;
    for (size_t p = txt + a; p + 2 <= txt + e && len < cap - 1;) {
        uint16_t ch = u16(m->d + p);
        if (!ch) break;
        if (ch == 0x0e && p + 8 <= txt + e) { p += 8 + u16(m->d + p + 6); continue; }  // (a tag: left out)
        if (ch == 0x0f && p + 6 <= txt + e) { p += 6; continue; }                      // (a tag's end)
        out[len++] = ch, p += 2;
    }
    out[len] = 0;
    return len;
}

bool rov_msbt_set(RovBuf *m, const char *label, const char *text) {
    size_t txt, txt_n;
    uint32_t idx;
    if (!msbt_find(m, label, &txt, &txt_n, &idx)) return false;
    uint32_t n = u32(m->d + txt);
    Buf t = {0}, body = {0};  // TXT2 again with that string replaced
    put32(&t, n);
    size_t base = 4 + 4 * (size_t)n;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t a = u32(m->d + txt + 4 + 4 * i), e = i + 1 < n ? u32(m->d + txt + 8 + 4 * i) : (uint32_t)txt_n;
        if (a > e || e > txt_n) {
            free(t.d), free(body.d);
            return false;
        }
        put32(&t, (uint32_t)(base + body.n));
        if (i == idx) put_utf16(&body, text);
        else put(&body, m->d + txt + a, e - a);
    }
    put(&t, body.d, body.n);
    free(body.d);
    Buf out = {0};  // the file again: the sections before TXT2, TXT2 new, the rest
    put(&out, m->d, txt - 16);
    put(&out, m->d + txt - 16, 4), put32(&out, (uint32_t)t.n), put(&out, m->d + txt - 8, 8);
    put(&out, t.d, t.n);
    pad_to(&out, 16, 0xab);
    size_t after = (txt + txt_n + 15) & ~(size_t)15;
    if (after < m->n) put(&out, m->d + after, m->n - after);
    w32(out.d + 0x12, (uint32_t)out.n);
    free(t.d), free(m->d);
    *m = out;
    return true;
}

// ---- BFLYT
int rov_flyt_sections(const RovBuf *b, RovSection *out, int cap) {
    if (!b || b->n < 0x14 || memcmp(b->d, "FLYT", 4)) return -1;
    static char magics[256][5];  // (the magic strings, for out[].magic)
    uint32_t hl = u16(b->d + 6), nsec = u16(b->d + 0x10);
    size_t off = hl;
    int n = 0;
    for (uint32_t i = 0; i < nsec; i++) {
        if (off + 8 > b->n) return -1;
        uint32_t size = u32(b->d + off + 4);
        if (size < 8 || off + size > b->n) return -1;
        if (n < cap && n < 256) {
            memcpy(magics[n], b->d + off, 4), magics[n][4] = 0;
            out[n] = (RovSection){magics[n], off, size};
        }
        n++;
        off += size;
    }
    return n;
}

bool rov_flyt_insert(RovBuf *b, size_t at, const void *sec, size_t n) {
    if (!b || b->n < 0x14 || memcmp(b->d, "FLYT", 4) || at > b->n || n < 8) return false;
    b->d = realloc(b->d, b->n + n);
    memmove(b->d + at + n, b->d + at, b->n - at);
    memcpy(b->d + at, sec, n);
    b->n += n;
    w16(b->d + 0x10, u16(b->d + 0x10) + 1u);
    w32(b->d + 0xc, (uint32_t)b->n);
    return true;
}

// ---- generation: at the first call (the game opening its RomFS), while the mods are on

static bool same_identity(const char *path, const char *id) {
    RovBuf b = {0};
    if (!read_host(path, &b)) return false;
    bool same = b.n == strlen(id) && !memcmp(b.d, id, b.n);
    rov_free(&b);
    return same;
}

static void generate_all(void) {
    if (!ngens) return;
    char id[1024];
    extern bool costumes_on(void), lanky_on(void);  // (mods/costume.c, lanky.c: their models only with the mod on)
    snprintf(id, sizeof id, "hundred offered:%d\ncostumes on:%d\n", mod_hundred_offered(), costumes_on());  // (Challenge Modes: only then)
    if (lanky_on()) snprintf(id + strlen(id), sizeof id - strlen(id), "lanky on:1\n");  // (absent when off: no new id)
    extern int costume_item_form(void);  // (mods/costume.c: the Mystery Mushroom's model is the Super Mushroom's)
    if (costumes_on() && costume_item_form() == 1) snprintf(id + strlen(id), sizeof id - strlen(id), "costume item:super\n");
    for (int i = 0; i < ngens; i++) {
        size_t l = strlen(id);
        snprintf(id + l, sizeof id - l, "%s:%d\n", gens[i].name, gens[i].version);
    }
    {  // (the player's mods: generated files are edits of theirs, so another set of mods is another generation)
        char um[2048];
        usermods_identity(um, sizeof um);
        size_t l = strlen(id);
        if (*um) snprintf(id + l, sizeof id - l, "player mods:\n%s", um);
    }
    snprintf(gen_dir, sizeof gen_dir, ROV_ROOT "/romfs");
    if (same_identity(ROV_ROOT "/romfs.id", id)) return;  // (generated before, for the same generators)
    snprintf(gen_dir, sizeof gen_dir, ROV_ROOT "/romfs.new");
    plat_remove_tree(gen_dir);
    mkdirs(gen_dir);
    generating = true;
    int ok = 0;
    for (int i = 0; i < ngens; i++) {
        bool r = gens[i].fn();
        ok += r;
        LOG("[mod] overlay: %s v%d %s\n", gens[i].name, gens[i].version, r ? "generated" : "FAILED");
    }
    generating = false;
    plat_remove_tree(ROV_ROOT "/romfs");
    if (rename(ROV_ROOT "/romfs.new", ROV_ROOT "/romfs")) {
        LOG("[mod] overlay: could not move the generated files into place (%s)\n", strerror(errno));
        gen_dir[0] = 0;
        return;
    }
    snprintf(gen_dir, sizeof gen_dir, ROV_ROOT "/romfs");
    if (ok == ngens) write_path(ROV_ROOT "/romfs.id", id, strlen(id));  // (a failure is tried again next launch)
    else remove(ROV_ROOT "/romfs.id");
}

// The game's RomFS overlay (runtime/game_hooks.c, runtime/mods.c, runtime/services/fs.c): the player's enabled mods'
// folders (runtime/usermods.h), then the generated files on top (made from the mods' versions), one per line.
static void overlay_list(void);
static char g_overlay[16384];
const char *game_romfs_overlay(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, overlay_list);
    return g_overlay[0] ? g_overlay : NULL;
}
static void overlay_list(void) {
    extern bool costumes_on(void), lanky_on(void);
    size_t l = usermods_overlay(g_overlay, sizeof g_overlay);
    if (!mod_endless() && !costumes_on() && !lanky_on()) return;
    generate_all();
    if (gen_dir[0] && !generating) snprintf(g_overlay + l, sizeof g_overlay - l, "%s\n", gen_dir);
}
