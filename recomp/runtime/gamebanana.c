// GameBanana in the Mods tab (gamebanana.h).
#include "gamebanana.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>  // strncasecmp
#include <sys/stat.h>

#include "archive.h"
#include "net/http.h"
#include "net/json.h"
#include "platform.h"  // plat_remove_tree; (Windows: POSIX mkdir)
#include "rt.h"
#include "usermods.h"
#include "../third_party/stb/stb_image.h"

#define API "https://gamebanana.com/apiv11"
#define MAX_LIST 100
#define TMP USERMODS_DIR "/.download"  // (dot folders: not mods)

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wake = PTHREAD_COND_INITIALIZER;
static bool g_started;

static GbMod g_list[MAX_LIST];
static int g_nlist;
static GbState g_list_st;
static char g_list_msg[160];
static char g_search[128];
static bool g_want_list;

static const char *const sort_alias[GB_NSORTS] = {"Generic_MostDownloaded", "Generic_MostLiked", "Generic_MostViewed",
                                                   "Generic_Newest", "Generic_LatestModified"};
const char *const gb_sort_names[GB_NSORTS] = {"Most downloaded", "Most liked", "Most viewed", "Newest", "Recently updated"};
static int g_sort, g_cat;
static GbCategory g_cats[32];
static int g_ncats;
static bool g_want_cats, g_cats_asked;

static GbMod g_cache[64];  // details fetched
static int g_ncache;
static int g_want_details[16], g_nwant;

static struct { int id, fi; bool enable; } g_inst_req;
static bool g_want_install;
static GbState g_inst_st;
static int g_inst_id;
static float g_inst_progress = -1;
static char g_inst_msg[200];
static volatile int g_cancel;

#define MAX_IMG 24
static struct { int id, idx; char url[128]; uint8_t *px; int w, h; int st; } g_img[MAX_IMG];  // st: 1 queued, 2 done, 3 failed
static int g_nimg;

bool gb_available(void) { return game_gamebanana_id() > 0; }

// ---------------------------------------------------------------- helpers
static Json *get_json(const char *url, char *why, size_t cap) {
    HttpResponse r;
    int st = http_request("GET", url, "Accept: application/json\r\n", NULL, 0, 20000, &r);
    if (st != 200 || !r.body) {
        if (st) snprintf(why, cap, "GameBanana answered %d", st);
        else snprintf(why, cap, "couldn't reach GameBanana (%s)", r.error);
        http_free(&r);
        return NULL;
    }
    Json *j = json_parse(r.body, r.len);
    http_free(&r);
    if (!j) snprintf(why, cap, "GameBanana's answer couldn't be read");
    return j;
}
static void url_escape(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && o + 4 < cap; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("-_.~", *p))
            out[o++] = (char)*p;
        else o += (size_t)snprintf(out + o, cap - o, "%%%02X", *p);
    }
    out[o] = 0;
}
// HTML description -> plain text (tags out, a few entities, blank runs folded; <br> / </p> / <li> as new lines)
static void html_text(const char *h, char *out, size_t cap) {
    size_t o = 0;
    bool space = true;
    for (const char *p = h; p && *p && o + 2 < cap;) {
        if (*p == '<') {
            bool nl = !strncasecmp(p, "<br", 3) || !strncasecmp(p, "</p", 3) || !strncasecmp(p, "<li", 3) ||
                      !strncasecmp(p, "</h", 3) || !strncasecmp(p, "</div", 5);
            const char *e = strchr(p, '>');
            p = e ? e + 1 : p + strlen(p);
            if (nl && o && out[o - 1] != '\n') out[o++] = '\n', space = true;
            continue;
        }
        if (*p == '&') {
            static const struct { const char *e, *c; } ents[] = {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"},
                {"&quot;", "\""}, {"&#39;", "'"}, {"&nbsp;", " "}, {"&apos;", "'"}};
            bool hit = false;
            for (size_t i = 0; i < sizeof ents / sizeof *ents; i++)
                if (!strncmp(p, ents[i].e, strlen(ents[i].e))) {
                    out[o++] = ents[i].c[0], p += strlen(ents[i].e), hit = true, space = false;
                    break;
                }
            if (hit) continue;
        }
        if (*p == '\r' || *p == '\n' || *p == '\t' || *p == ' ') {
            if (!space) out[o++] = ' ', space = true;
            p++;
            continue;
        }
        out[o++] = *p++, space = false;
    }
    while (o && (out[o - 1] == ' ' || out[o - 1] == '\n')) o--;
    out[o] = 0;
}
static void copy_str(char *dst, size_t cap, const Json *j) { snprintf(dst, cap, "%s", json_str(j, "")); }

// ---------------------------------------------------------------- the jobs
static void do_list(void) {
    char search[128], url[512], why[200] = "";
    pthread_mutex_lock(&g_lock);
    snprintf(search, sizeof search, "%s", g_search);
    pthread_mutex_unlock(&g_lock);
    if (search[0]) {
        char q[400];
        url_escape(search, q, sizeof q);
        snprintf(url, sizeof url, API "/Util/Search/Results?_sModelName=Mod&_sOrder=best_match&_idGameRow=%d&_sSearchString=%s"
                 "&_nPerpage=50", game_gamebanana_id(), q);
    } else {
        int sort, cat;
        pthread_mutex_lock(&g_lock);
        sort = g_sort, cat = g_cat;
        pthread_mutex_unlock(&g_lock);
        snprintf(url, sizeof url, API "/Mod/Index?_nPerpage=50&_aFilters%%5BGeneric_Game%%5D=%d&_sSort=%s",
                 game_gamebanana_id(), sort_alias[sort >= 0 && sort < GB_NSORTS ? sort : 0]);
        if (cat > 0)
            snprintf(url + strlen(url), sizeof url - strlen(url), "&_aFilters%%5BGeneric_Category%%5D=%d", cat);
    }
    Json *j = get_json(url, why, sizeof why);
    GbMod tmp[MAX_LIST];
    int n = 0;
    for (size_t i = 0; j && i < json_len(json_get(j, "_aRecords")) && n < MAX_LIST; i++) {
        Json *r = json_at(json_get(j, "_aRecords"), i);
        if (strcmp(json_str(json_get(r, "_sModelName"), "Mod"), "Mod")) continue;
        GbMod *m = &tmp[n++];
        memset(m, 0, sizeof *m);
        m->id = (int)json_num(json_get(r, "_idRow"), 0);
        copy_str(m->name, sizeof m->name, json_get(r, "_sName"));
        copy_str(m->author, sizeof m->author, json_get(json_get(r, "_aSubmitter"), "_sName"));
        copy_str(m->category, sizeof m->category, json_get(json_get(r, "_aRootCategory"), "_sName"));
    }
    json_free(j);
    pthread_mutex_lock(&g_lock);
    memcpy(g_list, tmp, sizeof *tmp * (size_t)n);
    g_nlist = n;
    g_list_st = j || n ? GB_IDLE : GB_ERROR;
    snprintf(g_list_msg, sizeof g_list_msg, "%s", n ? "" : *why ? why : "nothing found");
    if (n) g_list_st = GB_IDLE;
    pthread_mutex_unlock(&g_lock);
    LOG("[gamebanana] list%s%s: %d mods%s%s\n", search[0] ? " for " : "", search, n, *why ? ": " : "", why);
}

static void do_categories(void) {
    char why[200];
    char url[200];
    snprintf(url, sizeof url, API "/Mod/Categories?_idGameRow=%d&_sSort=a_to_z&_nPerpage=50", game_gamebanana_id());
    Json *j = get_json(url, why, sizeof why);
    pthread_mutex_lock(&g_lock);
    g_ncats = 0;
    for (size_t i = 0; j && i < json_len(j) && g_ncats < 32; i++) {
        Json *r = json_at(j, i);
        if (json_bool(json_get(r, "_bIsObsolete"), false) || !json_num(json_get(r, "_nItemCount"), 0)) continue;
        GbCategory *k = &g_cats[g_ncats++];
        k->id = (int)json_num(json_get(r, "_idRow"), 0);
        k->count = (int)json_num(json_get(r, "_nItemCount"), 0);
        copy_str(k->name, sizeof k->name, json_get(r, "_sName"));
    }
    if (!j) g_cats_asked = false;  // (try again at the next refresh)
    pthread_mutex_unlock(&g_lock);
    json_free(j);
}

static GbMod *cache_find(int id) {
    for (int i = 0; i < g_ncache; i++)
        if (g_cache[i].id == id) return &g_cache[i];
    return NULL;
}
static bool fetch_details(int id, GbMod *m, char *why, size_t cap) {
    char url[300];
    snprintf(url, sizeof url, API "/Mod/%d/ProfilePage", id);
    Json *j = get_json(url, why, cap);
    if (!j) return false;
    memset(m, 0, sizeof *m);
    m->id = id;
    m->details = true;
    copy_str(m->name, sizeof m->name, json_get(j, "_sName"));
    copy_str(m->author, sizeof m->author, json_get(json_get(j, "_aSubmitter"), "_sName"));
    copy_str(m->category, sizeof m->category, json_get(json_get(j, "_aCategory"), "_sName"));
    if (!m->category[0]) copy_str(m->category, sizeof m->category, json_get(json_get(j, "_aRootCategory"), "_sName"));
    html_text(json_str(json_get(j, "_sLicense"), ""), m->license, sizeof m->license);  // (it can be HTML: a link)
    copy_str(m->version, sizeof m->version, json_get(j, "_sVersion"));
    m->downloads = (int)json_num(json_get(j, "_nDownloadCount"), 0);
    m->likes = (int)json_num(json_get(j, "_nLikeCount"), 0);
    m->views = (int)json_num(json_get(j, "_nViewCount"), 0);
    m->thanks = (int)json_num(json_get(j, "_nThanksCount"), 0);
    m->posts = (int)json_num(json_get(j, "_nPostCount"), 0);
    m->subscribers = (int)json_num(json_get(j, "_nSubscriberCount"), 0);
    m->added = (int64_t)json_num(json_get(j, "_tsDateAdded"), 0);
    m->modified = (int64_t)json_num(json_get(j, "_tsDateModified"), 0);
    m->obsolete = json_bool(json_get(j, "_bIsObsolete"), false);
    m->ported = json_bool(json_get(j, "_bIsPorted"), false);
    const char *raw = json_str(json_get(j, "_sText"), "");
    const char *yt = strstr(raw, "youtube.com/embed/");  // (the description's video, if it has one)
    if (!yt) yt = strstr(raw, "youtube-nocookie.com/embed/");
    if (yt) {
        yt = strstr(yt, "/embed/") + 7;
        size_t n = strcspn(yt, "?\"'& ");
        if (n && n < 40) snprintf(m->video, sizeof m->video, "https://youtu.be/%.*s", (int)n, yt);
    }
    html_text(raw, m->text, sizeof m->text);
    Json *tags = json_get(j, "_aTags");
    for (size_t i = 0, o = 0; i < json_len(tags) && o + 40 < sizeof m->tags; i++) {
        Json *t = json_at(tags, i);
        const char *s = json_str(json_get(t, "_sValue"), json_str(t, ""));
        o += (size_t)snprintf(m->tags + o, sizeof m->tags - o, "%s%s", o ? ", " : "", s);
    }
    Json *cr = json_get(j, "_aCredits");  // "Developers: Name, Name" (a line per group, names only)
    for (size_t g = 0, o = 0; g < json_len(cr) && o + 60 < sizeof m->credits; g++) {
        Json *grp = json_at(cr, g), *au = json_get(grp, "_aAuthors");
        o += (size_t)snprintf(m->credits + o, sizeof m->credits - o, "%s%s: ", o ? "\n" : "",
                              json_str(json_get(grp, "_sGroupName"), "Credits"));
        for (size_t i = 0; i < json_len(au) && o + 60 < sizeof m->credits; i++)
            o += (size_t)snprintf(m->credits + o, sizeof m->credits - o, "%s%s", i ? ", " : "",
                                  json_str(json_get(json_at(au, i), "_sName"), "?"));
    }
    Json *pm = json_get(json_get(j, "_aPreviewMedia"), "_aImages");
    for (size_t i = 0; i < json_len(pm) && m->nshots < GB_MAX_SHOTS; i++) {
        Json *im = json_at(pm, i);
        const char *base = json_str(json_get(im, "_sBaseUrl"), ""), *f = json_str(json_get(im, "_sFile530"), "");
        if (!*base || !*f) f = json_str(json_get(im, "_sFile"), "");
        if (*base && *f) snprintf(m->shot[m->nshots++], sizeof m->shot[0], "%s/%s", base, f);
    }
    Json *lc = json_get(j, "_aLicenseChecklist");  // what the licence allows, asks about, forbids
    static const struct { const char *key, *head; } kinds[] = {{"yes", "Allowed"}, {"ask", "Ask the author first"},
                                                                  {"no", "Not allowed"}};
    for (int k = 0, o = 0; k < 3; k++) {
        Json *l = json_get(lc, kinds[k].key);
        if (!json_len(l)) continue;
        o += snprintf(m->rights + o, sizeof m->rights - (size_t)o, "%s%s: ", o ? "\n" : "", kinds[k].head);
        for (size_t i = 0; i < json_len(l) && o + 60 < (int)sizeof m->rights; i++)
            o += snprintf(m->rights + o, sizeof m->rights - (size_t)o, "%s%s", i ? "; " : "", json_str(json_at(l, i), ""));
    }
    Json *files = json_get(j, "_aFiles");
    for (size_t i = 0; i < json_len(files) && m->nfiles < GB_MAX_FILES; i++) {
        Json *f = json_at(files, i);
        int k = m->nfiles++;
        m->file_id[k] = (int)json_num(json_get(f, "_idRow"), 0);
        copy_str(m->file_name[k], sizeof m->file_name[k], json_get(f, "_sFile"));
        copy_str(m->file_desc[k], sizeof m->file_desc[k], json_get(f, "_sDescription"));
        m->file_size[k] = (uint64_t)json_num(json_get(f, "_nFilesize"), 0);
        m->file_downloads[k] = (int)json_num(json_get(f, "_nDownloadCount"), 0);
        m->file_date[k] = (int64_t)json_num(json_get(f, "_tsDateAdded"), 0);
    }
    json_free(j);
    return true;
}
static void do_details(int id) {
    GbMod m;
    char why[200];
    bool ok = fetch_details(id, &m, why, sizeof why);
    pthread_mutex_lock(&g_lock);
    if (ok) {
        GbMod *c = cache_find(id);
        if (!c) c = &g_cache[g_ncache < 64 ? g_ncache++ : 63];
        *c = m;
    }
    pthread_mutex_unlock(&g_lock);
}

static void set_inst(GbState st, float progress, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_inst(GbState st, float progress, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&g_lock);
    g_inst_st = st, g_inst_progress = progress;
    vsnprintf(g_inst_msg, sizeof g_inst_msg, fmt, ap);
    pthread_mutex_unlock(&g_lock);
    va_end(ap);
}
static void on_progress(unsigned long long done, unsigned long long total, void *u) {
    (void)u;
    pthread_mutex_lock(&g_lock);
    g_inst_progress = total ? (float)((double)done / (double)total) : -1;
    snprintf(g_inst_msg, sizeof g_inst_msg, "Downloading: %.0f of %.0f MB", done / 1048576.0, total / 1048576.0);
    pthread_mutex_unlock(&g_lock);
}
static void on_unpack(int files, void *u) {
    (void)u;
    pthread_mutex_lock(&g_lock);
    snprintf(g_inst_msg, sizeof g_inst_msg, "Unpacking: %d files", files);
    pthread_mutex_unlock(&g_lock);
}
static void do_install(int id, int fi, bool enable) {
    GbMod m;
    char why[300] = "";
    g_cancel = 0;
    set_inst(GB_BUSY, -1, "Getting the mod's details");
    if (!fetch_details(id, &m, why, sizeof why)) { set_inst(GB_ERROR, -1, "Not installed: %s", why); return; }
    if (fi < 0 || fi >= m.nfiles) { set_inst(GB_ERROR, -1, "Not installed: it has no files to download"); return; }
    mkdir(USERMODS_DIR, 0755);
    plat_remove_tree(TMP);
    mkdir(TMP, 0755);
    char url[200], arc[600], dir[300];
    snprintf(url, sizeof url, "https://gamebanana.com/dl/%d", m.file_id[fi]);
    const char *ext = strrchr(m.file_name[fi], '.');
    snprintf(arc, sizeof arc, TMP "/download%s", ext && strlen(ext) < 8 ? ext : "");
    snprintf(dir, sizeof dir, TMP "/unpacked");
    LOG("[gamebanana] installing %s (%d) file %s (%llu bytes)\n", m.name, id, m.file_name[fi], (unsigned long long)m.file_size[fi]);
    HttpResponse r;
    int st = http_download(url, arc, NULL, 30000, on_progress, NULL, &g_cancel, &r);
    if (st != 200) {
        set_inst(GB_ERROR, -1, "Not installed: %s", st ? "GameBanana didn't send the file" : r.error);
        plat_remove_tree(TMP);
        return;
    }
    set_inst(GB_BUSY, -1, "Unpacking");
    if (!archive_extract(arc, dir, on_unpack, NULL, why, sizeof why)) {
        set_inst(GB_ERROR, -1, "Not installed: %s", why);
        plat_remove_tree(TMP);
        return;
    }
    remove(arc);
    UserModInfo info = {0};
    snprintf(info.name, sizeof info.name, "%s", m.name);
    snprintf(info.author, sizeof info.author, "%s", m.author);
    snprintf(info.category, sizeof info.category, "%s", m.category);
    snprintf(info.license, sizeof info.license, "%s", m.license);
    snprintf(info.version, sizeof info.version, "%s", m.version);
    snprintf(info.url, sizeof info.url, "https://gamebanana.com/mods/%d", id);
    snprintf(info.description, sizeof info.description, "%s", m.text);
    info.gb_id = id;
    set_inst(GB_BUSY, -1, "Installing");
    int n = usermods_install_dir(dir, &info, enable, why, sizeof why);
    plat_remove_tree(TMP);
    if (!n) { set_inst(GB_ERROR, -1, "Not installed: %s", why); return; }
    if (n > 1) set_inst(GB_IDLE, 1, "Installed: %d versions (the first is on; switch under Installed)", n);
    else set_inst(GB_IDLE, 1, "Installed%s", enable ? " and on: it shows from the next start" : "");
}

static int img_next(void) {  // (g_lock held) a queued screenshot
    for (int i = 0; i < g_nimg; i++)
        if (g_img[i].st == 1) return i;
    return -1;
}
static void do_image(int i) {
    char url[128];
    pthread_mutex_lock(&g_lock);
    snprintf(url, sizeof url, "%s", g_img[i].url);
    pthread_mutex_unlock(&g_lock);
    HttpResponse r;
    int st = http_request("GET", url, NULL, NULL, 0, 20000, &r);
    int w = 0, h = 0, n;
    uint8_t *px = st == 200 && r.body ? stbi_load_from_memory((const unsigned char *)r.body, (int)r.len, &w, &h, &n, 4) : NULL;
    http_free(&r);
    pthread_mutex_lock(&g_lock);
    g_img[i].px = px, g_img[i].w = w, g_img[i].h = h, g_img[i].st = px ? 2 : 3;
    pthread_mutex_unlock(&g_lock);
    LOG("[gamebanana] screenshot %s: %s\n", url, px ? "ok" : "failed");
}

static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_lock);
        while (!g_want_list && !g_nwant && !g_want_install && !g_want_cats && img_next() < 0) pthread_cond_wait(&g_wake, &g_lock);
        bool list = g_want_list, inst = g_want_install;
        int det = g_nwant ? g_want_details[--g_nwant] : 0;
        int iid = g_inst_req.id, ifi = g_inst_req.fi;
        bool ien = g_inst_req.enable;
        int img = img_next();
        bool cats = g_want_cats;
        g_want_cats = false;
        g_want_list = false, g_want_install = false;
        pthread_mutex_unlock(&g_lock);
        if (cats) do_categories();
        if (img >= 0) do_image(img);
        if (inst) do_install(iid, ifi, ien);
        if (list) do_list();
        if (det) do_details(det);
    }
    return NULL;
}
static void start(void) {  // (g_lock held)
    if (g_started) return;
    g_started = true;
    pthread_t t;
    pthread_create(&t, NULL, worker, NULL);
    pthread_detach(t);
}

// ---------------------------------------------------------------- the menu's side
void gb_refresh(const char *search) {
    if (!gb_available()) return;
    pthread_mutex_lock(&g_lock);
    start();
    snprintf(g_search, sizeof g_search, "%s", search ? search : "");
    if (!g_cats_asked) g_cats_asked = g_want_cats = true;
    g_want_list = true;
    g_list_st = GB_BUSY;
    snprintf(g_list_msg, sizeof g_list_msg, "Loading from GameBanana...");
    pthread_cond_signal(&g_wake);
    pthread_mutex_unlock(&g_lock);
}
GbState gb_list_state(char *msg, int cap) {
    pthread_mutex_lock(&g_lock);
    GbState s = g_list_st;
    if (msg) snprintf(msg, (size_t)cap, "%s", g_list_msg);
    pthread_mutex_unlock(&g_lock);
    return s;
}
int gb_list(GbMod *out, int max) {
    pthread_mutex_lock(&g_lock);
    int n = g_nlist < max ? g_nlist : max;
    for (int i = 0; i < n; i++) {
        GbMod *c = cache_find(g_list[i].id);
        out[i] = c ? *c : g_list[i];
    }
    pthread_mutex_unlock(&g_lock);
    return n;
}
void gb_details(int id) {
    pthread_mutex_lock(&g_lock);
    start();
    bool have = cache_find(id) != NULL;
    for (int i = 0; i < g_nwant && !have; i++) have = g_want_details[i] == id;
    if (!have && g_nwant < 16) g_want_details[g_nwant++] = id, pthread_cond_signal(&g_wake);
    pthread_mutex_unlock(&g_lock);
}
bool gb_get(int id, GbMod *out) {
    pthread_mutex_lock(&g_lock);
    GbMod *c = cache_find(id);
    if (c) *out = *c;
    pthread_mutex_unlock(&g_lock);
    return c != NULL;
}
void gb_install(int id, int fi, bool enable) {
    pthread_mutex_lock(&g_lock);
    start();
    if (g_inst_st != GB_BUSY) {
        g_inst_req.id = id, g_inst_req.fi = fi, g_inst_req.enable = enable;
        g_want_install = true;
        g_inst_id = id, g_inst_st = GB_BUSY, g_inst_progress = -1;
        snprintf(g_inst_msg, sizeof g_inst_msg, "Starting");
        pthread_cond_signal(&g_wake);
    }
    pthread_mutex_unlock(&g_lock);
}
GbState gb_install_state(int *id, float *progress, char *msg, int cap) {
    pthread_mutex_lock(&g_lock);
    GbState s = g_inst_st;
    if (id) *id = g_inst_id;
    if (progress) *progress = g_inst_progress;
    if (msg) snprintf(msg, (size_t)cap, "%s", g_inst_msg);
    pthread_mutex_unlock(&g_lock);
    return s;
}
void gb_install_cancel(void) { g_cancel = 1; }

const uint8_t *gb_image(int id, int idx, int *w, int *h) {
    pthread_mutex_lock(&g_lock);
    const uint8_t *px = NULL;
    *w = 0;
    int i = 0;
    for (; i < g_nimg; i++)
        if (g_img[i].id == id && g_img[i].idx == idx) break;
    if (i < g_nimg) {
        if (g_img[i].st == 2) px = g_img[i].px, *w = g_img[i].w, *h = g_img[i].h;
        else if (g_img[i].st == 3) *w = -1;  // (failed)
    } else {
        GbMod *m = cache_find(id);
        if (m && idx >= 0 && idx < m->nshots && g_nimg < MAX_IMG) {
            g_img[g_nimg].id = id, g_img[g_nimg].idx = idx, g_img[g_nimg].st = 1;
            snprintf(g_img[g_nimg].url, sizeof g_img[0].url, "%s", m->shot[idx]);
            g_nimg++;
            start();
            pthread_cond_signal(&g_wake);
        }
    }
    pthread_mutex_unlock(&g_lock);
    return px;
}

void gb_set_filter(int sort, int category) {
    pthread_mutex_lock(&g_lock);
    g_sort = sort, g_cat = category;
    pthread_mutex_unlock(&g_lock);
}
int gb_categories(GbCategory *out, int max) {
    pthread_mutex_lock(&g_lock);
    int n = g_ncats < max ? g_ncats : max;
    memcpy(out, g_cats, sizeof *out * (size_t)n);
    pthread_mutex_unlock(&g_lock);
    return n;
}
