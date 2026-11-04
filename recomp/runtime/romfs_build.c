// RomFS image builder (romfs_build.h). Layout as the game's own image: a 0x50-byte header, file data
// from 0x200 (each file 16-byte aligned), then the directory hash table, directory entries, file hash table and file
// entries. Entries are found by hash (the game's RomFS code walks the tables itself), so the order inside them only
// has to be consistent: directories and files are sorted by name, children before siblings' children.
#include "romfs_build.h"

#include "platform.h"  // (Windows: POSIX mkdir)

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/clonefile.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif

#define EMPTY 0xFFFFFFFFu
#define DATA_START 0x200u

typedef struct Dir Dir;
typedef struct {
    char *name, *path;
    uint64_t size, data_off;
    uint32_t off;  // entry offset in the file table
    Dir *parent;
} File;
struct Dir {
    char *name, *path;
    Dir *parent;
    Dir **dirs;
    File **files;
    int ndirs, nfiles;
    uint32_t off;  // entry offset in the directory table
};

static int by_name_dir(const void *a, const void *b) { return strcmp((*(Dir *const *)a)->name, (*(Dir *const *)b)->name); }
static int by_name_file(const void *a, const void *b) { return strcmp((*(File *const *)a)->name, (*(File *const *)b)->name); }

// A file's size. On Windows the C runtime's stat() reports a symbolic link's own size, 0, not its target's (a RomFS
// set up from folders is a tree of symlinks where Developer Mode allows them: every file came out empty, issue #66),
// so a 0 there is asked of the file itself, through the link.
static uint64_t file_size(const char *p, const struct stat *st) {
#ifdef _WIN32
    if (st->st_size == 0) {
        HANDLE h = CreateFileA(p, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        LARGE_INTEGER n;
        uint64_t size = 0;
        if (h != INVALID_HANDLE_VALUE) {
            if (GetFileSizeEx(h, &n)) size = (uint64_t)n.QuadPart;
            CloseHandle(h);
        }
        return size;
    }
#else
    (void)p;
#endif
    return (uint64_t)st->st_size;
}

static Dir *scan(const char *path, const char *name, Dir *parent, int depth) {
    if (depth > 32) return NULL;
    DIR *d = opendir(path);
    if (!d) return NULL;
    Dir *dir = calloc(1, sizeof *dir);
    dir->name = strdup(name), dir->path = strdup(path), dir->parent = parent;
    int cd = 0, cf = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;  // (".", "..", and hidden files such as .DS_Store aren't the game's)
        size_t n = strlen(path) + strlen(e->d_name) + 2;
        char *p = malloc(n);
        snprintf(p, n, "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) { free(p); continue; }
        if (S_ISDIR(st.st_mode)) {
            Dir *c = scan(p, e->d_name, dir, depth + 1);
            free(p);
            if (!c) continue;
            if (dir->ndirs == cd) dir->dirs = realloc(dir->dirs, sizeof *dir->dirs * (cd = cd ? cd * 2 : 8));
            dir->dirs[dir->ndirs++] = c;
        } else if (S_ISREG(st.st_mode)) {
            File *f = calloc(1, sizeof *f);
            f->name = strdup(e->d_name), f->path = p, f->size = file_size(p, &st), f->parent = dir;
            if (dir->nfiles == cf) dir->files = realloc(dir->files, sizeof *dir->files * (cf = cf ? cf * 2 : 16));
            dir->files[dir->nfiles++] = f;
        } else {
            free(p);
        }
    }
    closedir(d);
    if (dir->ndirs) qsort(dir->dirs, dir->ndirs, sizeof *dir->dirs, by_name_dir);
    if (dir->nfiles) qsort(dir->files, dir->nfiles, sizeof *dir->files, by_name_file);
    return dir;
}

static void free_tree(Dir *d) {
    if (!d) return;
    for (int i = 0; i < d->ndirs; i++) free_tree(d->dirs[i]);
    for (int i = 0; i < d->nfiles; i++) free(d->files[i]->name), free(d->files[i]->path), free(d->files[i]);
    free(d->dirs), free(d->files), free(d->name), free(d->path), free(d);
}

// flat lists, in table order
typedef struct { Dir **dirs; File **files; int nd, nf, cd, cf; } Lists;
static void flatten(Dir *d, Lists *l) {
    for (int i = 0; i < d->nfiles; i++) {
        if (l->nf == l->cf) l->files = realloc(l->files, sizeof *l->files * (l->cf = l->cf ? l->cf * 2 : 256));
        l->files[l->nf++] = d->files[i];
    }
    for (int i = 0; i < d->ndirs; i++) {
        if (l->nd == l->cd) l->dirs = realloc(l->dirs, sizeof *l->dirs * (l->cd = l->cd ? l->cd * 2 : 64));
        l->dirs[l->nd++] = d->dirs[i];
    }
    for (int i = 0; i < d->ndirs; i++) flatten(d->dirs[i], l);
}

// the RomFS name hash and bucket count (as the game's RomFS code computes them)
static uint32_t name_hash(uint32_t parent, const char *name) {
    uint32_t h = parent ^ 123456789u;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) h = ((h >> 5) | (h << 27)) ^ *p;
    return h;
}
static uint32_t bucket_count(uint32_t n) {
    if (n < 3) return 3;
    if (n < 19) return n | 1;
    uint32_t c = n;
    while (c % 2 == 0 || c % 3 == 0 || c % 5 == 0 || c % 7 == 0 || c % 11 == 0 || c % 13 == 0 || c % 17 == 0) c++;
    return c;
}

static uint32_t align4(uint32_t x) { return (x + 3) & ~3u; }
static uint64_t align16(uint64_t x) { return (x + 15) & ~15ull; }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

uint64_t romfs_dir_bytes(const char *dir) {
    Dir *t = scan(dir, "", NULL, 0);
    if (!t) return 0;
    Lists l = {0};
    flatten(t, &l);
    uint64_t n = 0;
    for (int i = 0; i < l.nf; i++) n += l.files[i]->size;
    free(l.dirs), free(l.files);
    free_tree(t);
    return n;
}

static bool copy_data(FILE *out, const char *path, uint64_t size, uint8_t *buf, size_t bufsize, uint64_t *done,
                      uint64_t total, void (*progress)(uint64_t, uint64_t)) {
    FILE *in = fopen(path, "rb");
    if (!in) return false;
    uint64_t left = size;
    while (left) {
        size_t want = left < bufsize ? (size_t)left : bufsize;
        size_t got = fread(buf, 1, want, in);
        if (got != want || fwrite(buf, 1, got, out) != got) { fclose(in); return false; }
        left -= got;
        *done += got;
        if (progress) progress(*done, total);
    }
    fclose(in);
    return true;
}

// Files of `over` replace (same path) or join (new path) those of `base`; `over` is consumed.
static void reparent(Dir *d, Dir *parent) {
    d->parent = parent;
    for (int i = 0; i < d->nfiles; i++) d->files[i]->parent = d;
}
static void merge(Dir *base, Dir *over) {
    for (int i = 0; i < over->nfiles; i++) {
        File *f = over->files[i];
        int k = 0;
        while (k < base->nfiles && strcmp(base->files[k]->name, f->name)) k++;
        if (k < base->nfiles) {
            File *old = base->files[k];
            free(old->name), free(old->path), free(old);
            base->files[k] = f;
        } else {
            base->files = realloc(base->files, sizeof *base->files * (base->nfiles + 1));
            base->files[base->nfiles++] = f;
        }
        f->parent = base;
    }
    for (int i = 0; i < over->ndirs; i++) {
        Dir *d = over->dirs[i];
        int k = 0;
        while (k < base->ndirs && strcmp(base->dirs[k]->name, d->name)) k++;
        if (k < base->ndirs) {
            merge(base->dirs[k], d);
        } else {
            base->dirs = realloc(base->dirs, sizeof *base->dirs * (base->ndirs + 1));
            base->dirs[base->ndirs++] = d;
            reparent(d, base);
        }
    }
    if (base->ndirs) qsort(base->dirs, base->ndirs, sizeof *base->dirs, by_name_dir);
    if (base->nfiles) qsort(base->files, base->nfiles, sizeof *base->files, by_name_file);
    free(over->files), free(over->dirs), free(over->name), free(over->path), free(over);
}

// The image (out_path) or its index (ix: header, tables and file placement, nothing written).
static bool build(const char *dir, const char *overlay, const char *out_path, RomfsIndex *ix,
                  void (*progress)(uint64_t, uint64_t), char *why, size_t why_size) {
    #define FAIL(...) do { if (why) snprintf(why, why_size, __VA_ARGS__); goto fail; } while (0)
    bool ok = false;
    FILE *out = NULL;
    uint8_t *dtab = NULL, *ftab = NULL, *buf = NULL;
    uint32_t *dbuckets = NULL, *fbuckets = NULL;
    Lists l = {0};
    Dir *root = scan(dir, "", NULL, 0);
    if (!root) FAIL("can't read %s", dir);
    // overlay: folders separated by newlines, merged in order (a later one's file wins: mods, then generated files)
    for (const char *p = overlay; p && *p;) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char *one = malloc(n + 1);
        memcpy(one, p, n), one[n] = 0;
        Dir *o = n ? scan(one, "", NULL, 0) : NULL;
        if (o) merge(root, o);
        free(one);
        p = e ? e + 1 : p + n;
    }
    // table order: root, then each directory's subdirectories as flatten() lists them; files likewise
    if (l.nd == l.cd) l.dirs = realloc(l.dirs, sizeof *l.dirs * (l.cd = 64));
    l.dirs[l.nd++] = root;
    flatten(root, &l);
    if (!l.nf) FAIL("no files in %s", dir);

    // entry offsets
    uint32_t dsize = 0, fsize = 0;
    for (int i = 0; i < l.nd; i++) l.dirs[i]->off = dsize, dsize += 0x18 + align4((uint32_t)strlen(l.dirs[i]->name));
    for (int i = 0; i < l.nf; i++) l.files[i]->off = fsize, fsize += 0x20 + align4((uint32_t)strlen(l.files[i]->name));
    // file data offsets (relative to the data start), each 16-byte aligned
    uint64_t data = 0, total = 0;
    for (int i = 0; i < l.nf; i++) {
        data = align16(data);
        l.files[i]->data_off = data;
        data += l.files[i]->size;
        total += l.files[i]->size;
    }

    uint32_t nbd = bucket_count((uint32_t)l.nd), nbf = bucket_count((uint32_t)l.nf);
    dbuckets = malloc(4 * nbd), fbuckets = malloc(4 * nbf);
    for (uint32_t i = 0; i < nbd; i++) dbuckets[i] = EMPTY;
    for (uint32_t i = 0; i < nbf; i++) fbuckets[i] = EMPTY;
    dtab = calloc(1, dsize), ftab = calloc(1, fsize);
    for (int i = 0; i < l.nd; i++) {
        Dir *d = l.dirs[i];
        uint8_t *e = dtab + d->off;
        uint32_t parent = d->parent ? d->parent->off : 0, nl = (uint32_t)strlen(d->name);
        uint32_t sib = EMPTY;
        if (d->parent)
            for (int k = 0; k + 1 < d->parent->ndirs; k++)
                if (d->parent->dirs[k] == d) sib = d->parent->dirs[k + 1]->off;
        uint32_t h = name_hash(parent, d->name) % nbd;
        put32(e, parent);
        put32(e + 4, sib);
        put32(e + 8, d->ndirs ? d->dirs[0]->off : EMPTY);
        put32(e + 12, d->nfiles ? d->files[0]->off : EMPTY);
        put32(e + 16, dbuckets[h]);
        put32(e + 20, nl);
        memcpy(e + 0x18, d->name, nl);
        dbuckets[h] = d->off;
    }
    for (int i = 0; i < l.nf; i++) {
        File *f = l.files[i];
        uint8_t *e = ftab + f->off;
        uint32_t nl = (uint32_t)strlen(f->name), sib = EMPTY;
        for (int k = 0; k + 1 < f->parent->nfiles; k++)
            if (f->parent->files[k] == f) sib = f->parent->files[k + 1]->off;
        uint32_t h = name_hash(f->parent->off, f->name) % nbf;
        put32(e, f->parent->off);
        put32(e + 4, sib);
        put64(e + 8, f->data_off);
        put64(e + 16, f->size);
        put32(e + 24, fbuckets[h]);
        put32(e + 28, nl);
        memcpy(e + 0x20, f->name, nl);
        fbuckets[h] = f->off;
    }

    // header: tables after the file data (4-byte aligned)
    uint64_t dhash_off = (DATA_START + data + 3) & ~3ull;
    uint64_t dmeta_off = dhash_off + 4ull * nbd, fhash_off = dmeta_off + dsize, fmeta_off = fhash_off + 4ull * nbf;
    uint8_t hdr[DATA_START] = {0};
    uint64_t hv[10] = {0x50, dhash_off, 4ull * nbd, dmeta_off, dsize, fhash_off, 4ull * nbf, fmeta_off, fsize, DATA_START};
    memcpy(hdr, hv, sizeof hv);

    if (ix) {  // the index: tables in memory, each file's data left in its host file
        memcpy(ix->header, hdr, sizeof ix->header);
        ix->tables_off = dhash_off;
        ix->tables_size = 4ull * nbd + dsize + 4ull * nbf + fsize;
        uint8_t *t = ix->tables = malloc(ix->tables_size);
        memcpy(t, dbuckets, 4ull * nbd), t += 4ull * nbd;
        memcpy(t, dtab, dsize), t += dsize;
        memcpy(t, fbuckets, 4ull * nbf), t += 4ull * nbf;
        memcpy(t, ftab, fsize);
        ix->size = ix->tables_off + ix->tables_size;
        ix->nfiles = l.nf;
        ix->files = calloc((size_t)l.nf, sizeof *ix->files);
        for (int i = 0; i < l.nf; i++) {  // (flatten order is data order: sorted by offset)
            File *f = l.files[i];
            char rp[1024] = "", tmp[1024];
            snprintf(rp, sizeof rp, "%s", f->name);
            for (Dir *d = f->parent; d && d->parent; d = d->parent) {  // (the root's name is empty)
                snprintf(tmp, sizeof tmp, "%s/%s", d->name, rp);
                memcpy(rp, tmp, sizeof rp);
            }
            ix->files[i] = (RomfsIndexFile){DATA_START + f->data_off, f->size, strdup(f->path), strdup(rp), -1};
        }
        ok = true;
        goto fail;
    }

    out = fopen(out_path, "wb");
    if (!out) FAIL("can't write %s", out_path);
    if (fwrite(hdr, 1, DATA_START, out) != DATA_START) FAIL("write failed (disk full?)");
    size_t bufsize = 4u << 20;
    buf = malloc(bufsize);
    uint64_t pos = 0, done = 0;
    static const uint8_t zero[16];
    for (int i = 0; i < l.nf; i++) {
        File *f = l.files[i];
        if (f->data_off > pos && fwrite(zero, 1, (size_t)(f->data_off - pos), out) != f->data_off - pos)
            FAIL("write failed (disk full?)");
        if (!copy_data(out, f->path, f->size, buf, bufsize, &done, total, progress)) FAIL("can't copy %s", f->path);
        pos = f->data_off + f->size;
    }
    uint64_t tail = dhash_off - DATA_START - pos;
    if (tail && fwrite(zero, 1, (size_t)tail, out) != tail) FAIL("write failed (disk full?)");
    if (fwrite(dbuckets, 4, nbd, out) != nbd || fwrite(dtab, 1, dsize, out) != dsize ||
        fwrite(fbuckets, 4, nbf, out) != nbf || fwrite(ftab, 1, fsize, out) != fsize)
        FAIL("write failed (disk full?)");
    if (fclose(out) != 0) { out = NULL; FAIL("write failed (disk full?)"); }
    out = NULL;
    ok = true;
fail:
    if (out) fclose(out);
    if (!ok && out_path) remove(out_path);
    free(buf), free(dtab), free(ftab), free(dbuckets), free(fbuckets), free(l.dirs), free(l.files);
    free_tree(root);
    return ok;
    #undef FAIL
}

bool romfs_build(const char *dir, const char *out_path, void (*progress)(uint64_t, uint64_t), char *why, size_t why_size) {
    return build(dir, NULL, out_path, NULL, progress, why, why_size);
}

bool romfs_index(const char *dir, const char *overlay, RomfsIndex *ix, char *why, size_t why_size) {
    memset(ix, 0, sizeof *ix);
    return build(dir, overlay, NULL, ix, NULL, why, why_size);
}

// ------------------------------------------------------------------ copying the player's folder
static bool copy_file(const char *src, const char *dst, uint64_t size, uint64_t *done, uint64_t total,
                      void (*progress)(uint64_t, uint64_t)) {
    remove(dst);
#ifdef __APPLE__
    if (clonefile(src, dst, 0) == 0) goto linked;  // (APFS: a copy-on-write clone, instant)
#endif
#ifdef _WIN32
    if (CreateHardLinkA(dst, src, NULL)) goto linked;  // (same NTFS volume: a hard link)
#else
    if (link(src, dst) == 0) goto linked;  // (same volume: a hard link)
#endif
    {
        FILE *in = fopen(src, "rb"), *out = in ? fopen(dst, "wb") : NULL;
        bool ok = in && out;
        static uint8_t buf[1 << 20];
        while (ok) {
            size_t n = fread(buf, 1, sizeof buf, in);
            if (!n) break;
            if (fwrite(buf, 1, n, out) != n) ok = false;
            *done += n;
            if (progress) progress(*done, total);
        }
        if (in) fclose(in);
        if (out && fclose(out) != 0) ok = false;
        return ok;
    }
linked:
    *done += size;
    if (progress) progress(*done, total);
    return true;
}

static bool copy_dir(Dir *d, const char *dst, uint64_t *done, uint64_t total, void (*progress)(uint64_t, uint64_t)) {
    mkdir(dst, 0755);
    for (int i = 0; i < d->nfiles; i++) {
        size_t n = strlen(dst) + strlen(d->files[i]->name) + 2;
        char *p = malloc(n);
        snprintf(p, n, "%s/%s", dst, d->files[i]->name);
        bool ok = copy_file(d->files[i]->path, p, d->files[i]->size, done, total, progress);
        free(p);
        if (!ok) return false;
    }
    for (int i = 0; i < d->ndirs; i++) {
        size_t n = strlen(dst) + strlen(d->dirs[i]->name) + 2;
        char *p = malloc(n);
        snprintf(p, n, "%s/%s", dst, d->dirs[i]->name);
        bool ok = copy_dir(d->dirs[i], p, done, total, progress);
        free(p);
        if (!ok) return false;
    }
    return true;
}

bool romfs_copy_tree(const char *src, const char *dst, void (*progress)(uint64_t, uint64_t)) {
    Dir *t = scan(src, "", NULL, 0);
    if (!t) return false;
    Lists l = {0};
    flatten(t, &l);
    uint64_t total = 0, done = 0;
    for (int i = 0; i < l.nf; i++) total += l.files[i]->size;
    bool ok = copy_dir(t, dst, &done, total, progress);
    free(l.dirs), free(l.files);
    free_tree(t);
    return ok;
}
