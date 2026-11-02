// The player's own mods (usermods.h).
#include "usermods.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>  // strcasecmp
#include <sys/stat.h>
#include <unistd.h>

#include "platform.h"     // (Windows: POSIX mkdir; plat_remove_tree)
#include "romfs_build.h"  // romfs_copy_tree
#include "rt.h"

#define MAX_MODS 128
#define INI USERMODS_DIR "/mods.ini"

typedef struct {
    UserMod m;
    char **files;  // romfs/ paths, sorted (for conflicts; read when first needed)
    int nf;
    bool files_read;
} Entry;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static Entry g_e[MAX_MODS];
static int g_n;
static bool g_scanned;
static char g_started[4096];  // the identity the running game started with ("" before)
static bool g_started_set;

static bool is_dir(const char *p) {
    struct stat st;
    return !stat(p, &st) && S_ISDIR(st.st_mode);
}

static void append_name(char *buf, size_t cap, const char *name);
static void save_ini(void);

// ---------------------------------------------------------------- mod.info (key=value; "\n" in values as \n)
static void unescape(char *s) {
    char *o = s;
    for (char *p = s; *p; p++) {
        if (p[0] == '\\' && p[1] == 'n') *o++ = '\n', p++;
        else *o++ = *p;
    }
    *o = 0;
}
static void read_info(const char *id, UserMod *m) {
    char path[512], line[2048];
    snprintf(path, sizeof path, USERMODS_DIR "/%s/mod.info", id);
    snprintf(m->name, sizeof m->name, "%s", id);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        unescape(v);
        if (!strcmp(k, "name") && *v) snprintf(m->name, sizeof m->name, "%s", v);
        else if (!strcmp(k, "author")) snprintf(m->author, sizeof m->author, "%s", v);
        else if (!strcmp(k, "category")) snprintf(m->category, sizeof m->category, "%s", v);
        else if (!strcmp(k, "license")) {  // (tags out: GameBanana's can be a link)
            size_t o = 0;
            for (const char *p = v; *p && o + 1 < sizeof m->license; p++) {
                if (*p == '<') { const char *e = strchr(p, '>'); if (!e) break; p = e; continue; }
                m->license[o++] = *p;
            }
            m->license[o] = 0;
        }
        else if (!strcmp(k, "version")) snprintf(m->version, sizeof m->version, "%s", v);
        else if (!strcmp(k, "url")) snprintf(m->url, sizeof m->url, "%s", v);
        else if (!strcmp(k, "description")) snprintf(m->description, sizeof m->description, "%s", v);
        else if (!strcmp(k, "gamebanana")) m->gb_id = atoi(v);
    }
    fclose(f);
}
static void put_kv(FILE *f, const char *k, const char *v) {
    if (!v || !*v) return;
    fprintf(f, "%s=", k);
    for (const char *p = v; *p; p++) {
        if (*p == '\n') fputs("\\n", f);
        else if (*p != '\r') fputc(*p, f);
    }
    fputc('\n', f);
}

// ---------------------------------------------------------------- the files of a mod's romfs/
static void walk_files(const char *root, const char *rel, char ***out, int *n, int *cap, uint64_t *bytes) {
    char path[1024];
    snprintf(path, sizeof path, "%s%s%s", root, *rel ? "/" : "", rel);
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char sub[1024], full[1100];
        snprintf(sub, sizeof sub, "%s%s%s", rel, *rel ? "/" : "", de->d_name);
        snprintf(full, sizeof full, "%s/%s", root, sub);
        struct stat st;
        if (stat(full, &st)) continue;
        if (S_ISDIR(st.st_mode)) { walk_files(root, sub, out, n, cap, bytes); continue; }
        if (out) {
            if (*n == *cap) *out = realloc(*out, sizeof **out * (size_t)(*cap = *cap ? *cap * 2 : 64));
            (*out)[*n] = strdup(sub);
        }
        (*n)++;
        if (bytes) *bytes += (uint64_t)st.st_size;
    }
    closedir(d);
}
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void read_files(Entry *e) {
    if (e->files_read) return;
    e->files_read = true;
    char root[512];
    snprintf(root, sizeof root, USERMODS_DIR "/%s/romfs", e->m.id);
    int cap = 0;
    e->nf = 0;
    walk_files(root, "", &e->files, &e->nf, &cap, NULL);
    if (e->nf) qsort(e->files, (size_t)e->nf, sizeof *e->files, cmp_str);
}
static bool share_a_file(Entry *a, Entry *b) {  // (both sorted)
    read_files(a), read_files(b);
    for (int i = 0, j = 0; i < a->nf && j < b->nf;) {
        int c = strcmp(a->files[i], b->files[j]);
        if (!c) return true;
        if (c < 0) i++;
        else j++;
    }
    return false;
}

// ---------------------------------------------------------------- mods/ and mods.ini
static void free_entry(Entry *e) {
    for (int i = 0; i < e->nf; i++) free(e->files[i]);
    free(e->files);
    memset(e, 0, sizeof *e);
}
static int find_id(const char *id) {
    for (int i = 0; i < g_n; i++)
        if (!strcmp(g_e[i].m.id, id)) return i;
    return -1;
}
static void add_entry(const char *id, bool enabled) {
    if (g_n >= MAX_MODS || find_id(id) >= 0) return;
    char p[512];
    snprintf(p, sizeof p, USERMODS_DIR "/%s", id);
    if (!is_dir(p)) return;
    Entry *e = &g_e[g_n++];
    memset(e, 0, sizeof *e);
    snprintf(e->m.id, sizeof e->m.id, "%s", id);
    e->m.enabled = enabled;
    read_info(id, &e->m);
    snprintf(p, sizeof p, USERMODS_DIR "/%s/romfs", id);
    e->m.has_romfs = is_dir(p);
    if (e->m.has_romfs) {
        int cap = 0;
        walk_files(p, "", NULL, &e->m.nfiles, &cap, &e->m.bytes);
    }
    snprintf(p, sizeof p, USERMODS_DIR "/%s/exefs", id);
    e->m.has_exefs = is_dir(p);
}
static void save_ini(void) {
    mkdir(USERMODS_DIR, 0755);
    FILE *f = fopen(INI ".tmp", "w");
    if (!f) return;
    fputs("# The installed mods, in order (a later one's files win); 1 = on. The Mods tab writes this.\n", f);
    for (int i = 0; i < g_n; i++) fprintf(f, "%s=%d\n", g_e[i].m.id, g_e[i].m.enabled);
    fclose(f);
    remove(INI);
    rename(INI ".tmp", INI);
}
static void scan_locked(void) {
    for (int i = 0; i < g_n; i++) free_entry(&g_e[i]);
    g_n = 0;
    FILE *f = fopen(INI, "r");
    char line[512];
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        add_entry(line, atoi(eq + 1) != 0);
    }
    if (f) fclose(f);
    // folders the list doesn't have yet (copied in by hand): at the end, off
    DIR *d = opendir(USERMODS_DIR);
    char *names[MAX_MODS];
    int nn = 0;
    struct dirent *de;
    while (d && (de = readdir(d)) && nn < MAX_MODS) {
        if (de->d_name[0] == '.' || find_id(de->d_name) >= 0) continue;
        char p[512];
        snprintf(p, sizeof p, USERMODS_DIR "/%s", de->d_name);
        if (is_dir(p)) names[nn++] = strdup(de->d_name);
    }
    if (d) closedir(d);
    qsort(names, (size_t)nn, sizeof *names, cmp_str);
    for (int i = 0; i < nn; i++) add_entry(names[i], false), free(names[i]);
    g_scanned = true;
    if (nn) save_ini();
}
// A run that stopped on a fatal error with mods on (usermods_note_crash, from the crash path) left mods/.crashed with
// their ids: at the next start they're turned off, so a mod that breaks the game can't keep it from starting; the
// Mods tab says so (usermods_crash_note), and the player can turn them back on.
static char g_crash_note[600];
static void crash_recovery_locked(void) {
    FILE *f = fopen(USERMODS_DIR "/.crashed", "r");
    if (!f) return;
    char line[256];
    bool any = false;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        int i = find_id(line);
        if (i < 0 || !g_e[i].m.enabled) continue;
        g_e[i].m.enabled = false, any = true;
        append_name(g_crash_note, sizeof g_crash_note, g_e[i].m.name);
    }
    fclose(f);
    remove(USERMODS_DIR "/.crashed");
    if (any) save_ini(), LOG("[usermods] the last run stopped with an error while mods were on: turned off: %s\n", g_crash_note);
}
static void ensure_scanned(void) {
    if (!g_scanned) scan_locked(), crash_recovery_locked();
}
const char *usermods_crash_note(void) { return g_crash_note; }
void usermods_note_crash(void) {
    if (!g_started_set || !g_started[0] || pthread_mutex_trylock(&g_lock)) return;  // (no mods in this run, or busy)
    FILE *f = fopen(USERMODS_DIR "/.crashed", "w");
    for (int i = 0; f && i < g_n; i++)
        if (g_e[i].m.enabled && g_e[i].m.has_romfs) fprintf(f, "%s\n", g_e[i].m.id);
    if (f) fclose(f);
    pthread_mutex_unlock(&g_lock);
}

void usermods_scan(void) {
    pthread_mutex_lock(&g_lock);
    scan_locked();
    pthread_mutex_unlock(&g_lock);
}
int usermods_count(void) {
    pthread_mutex_lock(&g_lock);
    ensure_scanned();
    int n = g_n;
    pthread_mutex_unlock(&g_lock);
    return n;
}
bool usermods_get(int i, UserMod *out) {
    pthread_mutex_lock(&g_lock);
    ensure_scanned();
    bool ok = i >= 0 && i < g_n;
    if (ok) *out = g_e[i].m;
    pthread_mutex_unlock(&g_lock);
    return ok;
}
static void append_name(char *buf, size_t cap, const char *name) {
    if (!buf || !cap) return;
    size_t l = strlen(buf);
    snprintf(buf + l, cap - l, "%s%s", l ? ", " : "", name);
}
void usermods_set_enabled(int i, bool on, char *turned_off, size_t cap) {
    if (turned_off && cap) turned_off[0] = 0;
    pthread_mutex_lock(&g_lock);
    ensure_scanned();
    if (i >= 0 && i < g_n && g_e[i].m.enabled != on) {
        g_e[i].m.enabled = on;
        if (on)
            for (int k = 0; k < g_n; k++)
                if (k != i && g_e[k].m.enabled && share_a_file(&g_e[i], &g_e[k]))
                    g_e[k].m.enabled = false, append_name(turned_off, cap, g_e[k].m.name);
        save_ini();
        LOG("[usermods] %s %s%s%s\n", g_e[i].m.id, on ? "on" : "off", turned_off && *turned_off ? "; off: " : "",
            turned_off && *turned_off ? turned_off : "");
    }
    pthread_mutex_unlock(&g_lock);
}
void usermods_move(int i, int delta) {
    pthread_mutex_lock(&g_lock);
    ensure_scanned();
    int j = i + delta;
    if (i >= 0 && i < g_n && j >= 0 && j < g_n) {
        Entry t = g_e[i];
        g_e[i] = g_e[j], g_e[j] = t;
        save_ini();
    }
    pthread_mutex_unlock(&g_lock);
}
bool usermods_remove(int i) {
    pthread_mutex_lock(&g_lock);
    ensure_scanned();
    bool ok = i >= 0 && i < g_n;
    if (ok) {
        char p[512];
        snprintf(p, sizeof p, USERMODS_DIR "/%s", g_e[i].m.id);
        LOG("[usermods] %s removed\n", g_e[i].m.id);
        ok = plat_remove_tree(p);
        free_entry(&g_e[i]);
        memmove(&g_e[i], &g_e[i + 1], sizeof *g_e * (size_t)(g_n - i - 1));
        g_n--;
        save_ini();
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}
int usermods_conflicts(int i, char *names, size_t cap) {
    if (names && cap) names[0] = 0;
    pthread_mutex_lock(&g_lock);
    ensure_scanned();
    int n = 0;
    for (int k = 0; i >= 0 && i < g_n && k < g_n; k++)
        if (k != i && g_e[k].m.enabled && share_a_file(&g_e[i], &g_e[k])) n++, append_name(names, cap, g_e[k].m.name);
    pthread_mutex_unlock(&g_lock);
    return n;
}

// ---------------------------------------------------------------- the game's side
static void identity_locked(char *buf, size_t cap) {
    buf[0] = 0;
    for (int i = 0; i < g_n; i++) {
        if (!g_e[i].m.enabled || !g_e[i].m.has_romfs) continue;
        size_t l = strlen(buf);
        snprintf(buf + l, cap - l, "%s:%d:%llu\n", g_e[i].m.id, g_e[i].m.nfiles, (unsigned long long)g_e[i].m.bytes);
    }
}
void usermods_identity(char *buf, size_t cap) {
    pthread_mutex_lock(&g_lock);
    ensure_scanned();
    identity_locked(buf, cap);
    pthread_mutex_unlock(&g_lock);
}
static char g_layer[128];
void usermods_set_layer(const char *dir) {
    pthread_mutex_lock(&g_lock);
    snprintf(g_layer, sizeof g_layer, "%s", dir ? dir : "");
    pthread_mutex_unlock(&g_lock);
}
size_t usermods_overlay(char *buf, size_t cap) {
    pthread_mutex_lock(&g_lock);
    ensure_scanned();
    size_t l = 0;
    buf[0] = 0;
    for (int i = 0; i < g_n; i++) {
        if (!g_e[i].m.enabled || !g_e[i].m.has_romfs) continue;
        int w = snprintf(buf + l, cap - l, USERMODS_DIR "/%s/romfs\n", g_e[i].m.id);
        if (w < 0 || (size_t)w >= cap - l) break;
        l += (size_t)w;
        LOG("[usermods] on: %s (%s, %d files)\n", g_e[i].m.name, g_e[i].m.id, g_e[i].m.nfiles);
    }
    if (l && g_layer[0]) {
        int w = snprintf(buf + l, cap - l, "%s\n", g_layer);
        if (w > 0 && (size_t)w < cap - l) l += (size_t)w;
    }
    if (!g_started_set) identity_locked(g_started, sizeof g_started), g_started_set = true;
    pthread_mutex_unlock(&g_lock);
    return l;
}
void usermods_each_file(const char *const *suffixes, void (*cb)(const char *id, const char *rel, void *ud), void *ud) {
    char **ids = NULL, **rels = NULL;
    int n = 0, cap = 0;
    pthread_mutex_lock(&g_lock);
    ensure_scanned();
    for (int i = 0; i < g_n; i++) {
        if (!g_e[i].m.enabled || !g_e[i].m.has_romfs) continue;
        read_files(&g_e[i]);
        for (int k = 0; k < g_e[i].nf; k++) {
            size_t l = strlen(g_e[i].files[k]);
            bool match = false;
            for (int s = 0; suffixes[s] && !match; s++) {
                size_t sl = strlen(suffixes[s]);
                match = l >= sl && !strcasecmp(g_e[i].files[k] + l - sl, suffixes[s]);
            }
            if (!match) continue;
            if (n == cap) {
                cap = cap ? cap * 2 : 16;
                ids = realloc(ids, sizeof *ids * (size_t)cap), rels = realloc(rels, sizeof *rels * (size_t)cap);
            }
            ids[n] = strdup(g_e[i].m.id), rels[n] = strdup(g_e[i].files[k]), n++;
        }
    }
    pthread_mutex_unlock(&g_lock);
    for (int i = 0; i < n; i++) {
        cb(ids[i], rels[i], ud);
        free(ids[i]), free(rels[i]);
    }
    free(ids), free(rels);
}
bool usermods_changed_since_start(void) {
    char now[sizeof g_started];
    pthread_mutex_lock(&g_lock);
    ensure_scanned();
    identity_locked(now, sizeof now);
    bool changed = g_started_set && strcmp(now, g_started);
    pthread_mutex_unlock(&g_lock);
    return changed;
}
bool usermods_find(const char *rel, char *out, size_t cap) {
    pthread_mutex_lock(&g_lock);
    ensure_scanned();
    bool found = false;
    for (int i = g_n - 1; i >= 0 && !found; i--) {
        if (!g_e[i].m.enabled || !g_e[i].m.has_romfs) continue;
        struct stat st;
        found = (size_t)snprintf(out, cap, USERMODS_DIR "/%s/romfs/%s", g_e[i].m.id, rel) < cap && !stat(out, &st) &&
                S_ISREG(st.st_mode);
    }
    pthread_mutex_unlock(&g_lock);
    return found;
}

// ---------------------------------------------------------------- installing from an unpacked download
// the game's own top-level RomFS folders (a mod packed without its romfs/ folder)
static const char *const k_romfs_tops[] = {"Model", "Pack", "Sound", "Layout", "Effect", "Env", "Message", "Font",
                                           "Course", "Stage", "Movie", "Mii", "Shader", "UI", NULL};
static bool has_romfs_top(const char *dir) {
    char p[1100];
    for (int i = 0; k_romfs_tops[i]; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, k_romfs_tops[i]);
        if (is_dir(p)) return true;
    }
    return false;
}
static void find_roots(const char *dir, int depth, char roots[][1024], int *n, int max, char exefs[][1024], int *ne) {
    if (depth > 7 || *n >= max) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    bool top_here = has_romfs_top(dir);
    char subs[64][256];
    int ns = 0;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.' || !strcmp(de->d_name, "__MACOSX")) continue;
        char p[1100];
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        if (!is_dir(p)) continue;
        if (!strcasecmp(de->d_name, "romfs")) {
            if (*n < max) snprintf(roots[(*n)++], 1024, "%s", p);
            continue;
        }
        if (!strcasecmp(de->d_name, "exefs") || !strcasecmp(de->d_name, "exefs_patches")) {
            if (*ne < 8) snprintf(exefs[(*ne)++], 1024, "%s", p);
            continue;
        }
        if (ns < 64) snprintf(subs[ns++], sizeof subs[0], "%s", de->d_name);
    }
    closedir(d);
    if (top_here) {  // (this folder is a romfs root itself: its subfolders are the game's)
        if (*n < max) snprintf(roots[(*n)++], 1024, "%s", dir);
        return;
    }
    for (int i = 0; i < ns; i++) {
        char p[1100];
        snprintf(p, sizeof p, "%s/%s", dir, subs[i]);
        find_roots(p, depth + 1, roots, n, max, exefs, ne);
    }
}
static void slug(const char *s, char *out, size_t cap) {
    size_t o = 0;
    bool dash = false;
    for (const unsigned char *p = (const unsigned char *)s; *p && o + 1 < cap; p++) {
        if (isalnum(*p)) out[o++] = (char)tolower(*p), dash = false;
        else if (!dash && o) out[o++] = '-', dash = true;
    }
    while (o && out[o - 1] == '-') o--;
    out[o] = 0;
}
static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}
static bool move_tree(const char *src, const char *dst) {
    if (!rename(src, dst)) return true;
    return romfs_copy_tree(src, dst, NULL);  // (another volume: copied; the download folder goes afterwards)
}

int usermods_install_dir(const char *src, const UserModInfo *info, bool enable, char *why, size_t why_cap) {
    static char roots[16][1024], exefs[8][1024];
    int n = 0, ne = 0;
    find_roots(src, 0, roots, &n, 16, exefs, &ne);
    if (!n) {
        snprintf(why, why_cap, ne ? "it only patches the game's code (exefs), which isn't supported yet"
                                  : "no game files found in it (not a romfs mod: a save file or level pack?)");
        return 0;
    }
    mkdir(USERMODS_DIR, 0755);
    int done = 0;
    for (int k = 0; k < n; k++) {
        // the variant's name: the folder holding its romfs/ (or the folder itself), when there are several
        char variant[256] = "", parent[1024];
        if (n > 1) {
            snprintf(parent, sizeof parent, "%s", roots[k]);
            char *s = strrchr(parent, '/');
            if (s && !strcasecmp(s + 1, "romfs")) *s = 0;
            snprintf(variant, sizeof variant, "%s", strcmp(parent, src) ? base_name(parent) : "");
        }
        char s1[96], s2[96], id[96];
        slug(info->name, s1, sizeof s1);
        slug(variant, s2, sizeof s2);
        if (info->gb_id) snprintf(id, sizeof id, "gb%d-%.40s%s%.30s", info->gb_id, s1, *s2 ? "-" : "", s2);
        else snprintf(id, sizeof id, "%.50s%s%.30s", *s1 ? s1 : "mod", *s2 ? "-" : "", s2);
        char dir[512], dst[600];
        snprintf(dir, sizeof dir, USERMODS_DIR "/%s", id);
        plat_remove_tree(dir);  // (installed before: replaced)
        mkdir(dir, 0755);
        snprintf(dst, sizeof dst, "%s/romfs", dir);
        if (!move_tree(roots[k], dst)) {
            snprintf(why, why_cap, "couldn't move its files into mods/ (disk full?)");
            plat_remove_tree(dir);
            continue;
        }
        for (int x = 0; x < ne && k == 0; x++) {  // (code patches: kept for later; not loaded)
            snprintf(dst, sizeof dst, "%s/exefs", dir);
            move_tree(exefs[x], dst);
        }
        snprintf(dst, sizeof dst, "%s/mod.info", dir);
        FILE *f = fopen(dst, "w");
        if (f) {
            char name[300];
            snprintf(name, sizeof name, "%s%s%s%s", info->name, *variant ? " (" : "", variant, *variant ? ")" : "");
            put_kv(f, "name", name), put_kv(f, "author", info->author), put_kv(f, "category", info->category);
            put_kv(f, "license", info->license), put_kv(f, "version", info->version), put_kv(f, "url", info->url);
            put_kv(f, "description", info->description);
            if (info->gb_id) fprintf(f, "gamebanana=%d\n", info->gb_id);
            fclose(f);
        }
        LOG("[usermods] installed %s from %s\n", id, roots[k]);
        pthread_mutex_lock(&g_lock);
        scan_locked();
        int i = find_id(id);
        pthread_mutex_unlock(&g_lock);
        // (one of several variants: only the first goes on)
        if (enable && i >= 0 && done == 0) usermods_set_enabled(i, true, NULL, 0);
        done++;
    }
    return done;
}
