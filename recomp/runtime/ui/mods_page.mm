// The overlay menu's Mods tab (menu.mm tab_mods): a page choice at the top, then
//   Built-in    the game's own mods (game_mod_toggles, by section) and "Try it now";
//   Installed   the player's mods (runtime/usermods.h), by category, with a search; a mod's row opens its page (on /
//               off, order, its GameBanana page, remove);
//   GameBanana  the game's mods there (runtime/gamebanana.h), most downloaded first or a search; a row opens its page
//               (details, which file, install);
//   Online      the online backend, the game's own settings for it (game_rows) and the app's own rows.
// Rows are the menu's option rows, so a controller, touch and the mouse all work it; B (Esc) leaves a mod's page.
#include <SDL3/SDL.h>
#ifdef __APPLE__
#import <Metal/Metal.h>
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../game.h"  // GAME_TITLE
#include "../game_rows.h"
#include "imgui.h"
#include "option_rows.h"

extern "C" {
#include "../gamebanana.h"
#include "../usermods.h"
}

typedef struct { const char *label, *desc; bool *value; bool def; } GameModToggle;
extern "C" int game_mod_toggles(const GameModToggle **out);
extern "C" void game_mod_toggles_changed(void);
typedef struct {
    const char *label, *desc;
    bool (*available)(void);
    void (*trigger)(int choice);
    const char *choice_label;
    const char *const *choices;
    int nchoices;
    const char *(*why)(void);
} GameModTrigger;
extern "C" int game_mod_triggers(const GameModTrigger **out);
extern "C" bool launcher_settings_rows(void);
extern "C" bool online_rows_available(void);
extern "C" void online_rows(void);
extern "C" void rt_request_reboot(int kind);
void menu_close(void);  // (menu.mm)

// A game's settings that are not switches (runtime/game_rows.h): the Online page draws them under the backend
// choice, since that is what they configure (OpenCourse World's domain and token, games/smm2/mods/ocw.c).
// (no menu_keyboard_run here: it blocks re-presenting frames from the game thread, and these rows are drawn during
// a present - opt_text has its own field, with the on-screen keyboard for a controller)
#define MAX_SHOWN_ROWS 16
static bool g_row_shown[MAX_SHOWN_ROWS];  // a secret row was activated: its field is drawn until the menu closes
// What each text row held when the game last stored it. opt_text reports a change only when Enter confirmed one
// (ImGuiInputTextFlags_EnterReturnsTrue), while ImGui edits the buffer as it is typed: so a field left by clicking
// away, tabbing on, or closing the keyboard showed the new text and kept it in memory, but the game was never told
// to store it - and it was gone at the next start (owner's report on 0.5.5: "credentials would not save"). The
// comparison below notices that the moment the row stops being edited, whichever way it was left.
static char g_row_was[MAX_SHOWN_ROWS][256];
static bool g_row_known[MAX_SHOWN_ROWS];
extern "C" void game_mod_rows_hide(void) {  // (menu.mm, when the menu closes)
    for (int i = 0; i < MAX_SHOWN_ROWS; i++) g_row_shown[i] = false;
}

static void game_rows(void) {
    const GameModRow *r;
    int n = game_mod_rows(&r);
    bool changed = false;
    for (int i = 0; i < n; i++) {
        if (!r[i].value && !r[i].text) { opt_section(r[i].label); continue; }
        if (r[i].value) { changed |= opt_toggle(r[i].label, r[i].value, r[i].def, r[i].desc); continue; }
        // a secret: "set" / "not set" until the row is activated, then the field itself (never masked in place -
        // that would mean swapping the buffer under a live ImGui field)
        if (r[i].secret && i < MAX_SHOWN_ROWS && !g_row_shown[i]) {
            if (opt_action(r[i].label, r[i].text[0] ? "set" : "not set", r[i].desc)) g_row_shown[i] = true;
            continue;
        }
        // max_len is in characters, as the game's own keyboard limit is: cut at the boundary after the last one,
        // never inside one (a domain is ASCII, but nothing here promises that)
        if (r[i].max_len > 0 && !opt_text_editing(r[i].label)) {
            int chars = 0;
            for (char *q = r[i].text; *q; q++) {
                if (((unsigned char)*q & 0xc0) == 0x80) continue;
                if (chars++ == r[i].max_len) { *q = 0; break; }
            }
        }
        if (i < MAX_SHOWN_ROWS && !g_row_known[i]) {  // (first sight: what it already held is not a change)
            snprintf(g_row_was[i], sizeof g_row_was[0], "%s", r[i].text);
            g_row_known[i] = true;
        }
        bool entered = opt_text(r[i].label, r[i].text, (size_t)r[i].cap, r[i].hint, r[i].desc);
        changed |= entered;
        if (entered && i < MAX_SHOWN_ROWS)  // (stored now: not again on the next frame)
            snprintf(g_row_was[i], sizeof g_row_was[0], "%s", r[i].text);
        // the row is no longer being edited and holds something else than when it was last stored
        else if (i < MAX_SHOWN_ROWS && !opt_text_editing(r[i].label) && strcmp(r[i].text, g_row_was[i])) {
            snprintf(g_row_was[i], sizeof g_row_was[0], "%s", r[i].text);
            changed = true;
        }
    }
    if (changed) game_mod_rows_changed();
}

static bool g_back_used;  // B / Esc went to a page here this frame (the menu doesn't close on it)
bool mods_page_used_back(void) { bool u = g_back_used; g_back_used = false; return u; }
static bool back_pressed(void) {
    return ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
}

static int g_page;             // 0 built-in, 1 installed, 2 GameBanana
static int g_mod = -1;         // the installed mod whose page is open
static int g_gb = 0;           // the GameBanana mod whose page is open (its id)
static int g_gb_file;
static bool g_ask_remove;
static char g_note[300];       // the last action's outcome (turned off another mod, installed...)
static char g_search[96], g_gb_search[96];
static int g_gb_sort, g_gb_cat;  // the GameBanana list's order, and its category (index in the choice: 0 all)
static bool g_gb_loaded;

static void fmt_size(uint64_t b, char *out, size_t cap) {
    if (b >= 1 << 30) snprintf(out, cap, "%.1f GB", b / 1073741824.0);
    else if (b >= 1 << 20) snprintf(out, cap, "%.0f MB", b / 1048576.0);
    else snprintf(out, cap, "%.0f KB", b / 1024.0);
}
// What the right-hand pane shows for a mod: its likes, downloads, submitter and developers first, then the rest
static void gb_info(const GbMod &m, char *out, size_t cap) {
    size_t o = (size_t)snprintf(out, cap, "%s\n\nSubmitter: %s\n\nLikes: %d     Downloads: %d\n", m.name, m.author, m.likes, m.downloads);
    auto add = [&](const char *fmt, const char *a) {
        if (o < cap && *a) o += (size_t)snprintf(out + o, cap - o, fmt, a);
    };
    add("\n%s\n", m.credits);
    if (m.nshots && o < cap) o += (size_t)snprintf(out + o, cap - o, "\nScreenshots: %d\n", m.nshots);
    add("\nCategory: %s\n", m.category);
    add("Licence: %s\n", m.license);
    add("\n%s", m.text);
}
#ifndef __APPLE__
extern "C" uint64_t vk_imgui_texture_rgba(const void *rgba, int w, int h);  // runtime/nvn/vulkan/imgui_glue.cpp
#endif
// A mod's screenshot as a texture (made once its pixels have arrived), for the description pane of the row just drawn
static void gb_show_image(int id, int idx) {
    static struct { int id, idx, w, h; uint64_t tid; } tex[32];
#ifdef __APPLE__
    static id<MTLTexture> keep[32];  // (the textures live as long as the menu)
#endif
    static int ntex;
    int w, h, i = 0;
    for (; i < ntex; i++)
        if (tex[i].id == id && tex[i].idx == idx) break;
    if (i == ntex) {
        const uint8_t *px = gb_image(id, idx, &w, &h);
        if (!px || ntex >= 32) {
            opt_desc_image(0, w < 0 ? -1 : 1, 1);  // (a place for it while it loads)
            return;
        }
        tex[i].id = id, tex[i].idx = idx, tex[i].w = w, tex[i].h = h;
#ifdef __APPLE__
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                     width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead;
        keep[i] = [MTLCreateSystemDefaultDevice() newTextureWithDescriptor:d];
        [keep[i] replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h) mipmapLevel:0 withBytes:px
                   bytesPerRow:(NSUInteger)w * 4];
        tex[i].tid = (uint64_t)(uintptr_t)(__bridge void *)keep[i];
#else
        tex[i].tid = vk_imgui_texture_rgba(px, w, h);
#endif
        ntex++;
    }
    if (tex[i].tid) opt_desc_image(tex[i].tid, tex[i].w, tex[i].h);
}
static bool contains(const char *hay, const char *needle) {
    if (!*needle) return true;
    for (const char *p = hay; *p; p++)
        if (!strncasecmp(p, needle, strlen(needle))) return true;
    return false;
}

// ---------------------------------------------------------------- built-in
static void page_builtin(void) {
    const GameModToggle *t;
    int n = game_mod_toggles(&t);
    if (n > 0 && t[0].value) opt_section("Built-in mods");
    bool changed = false;
    for (int i = 0; i < n; i++) {
        if (!t[i].value) { opt_section(t[i].label); continue; }
        changed |= opt_toggle(t[i].label, t[i].value, t[i].def, t[i].desc);
    }
    if (changed) game_mod_toggles_changed();
    const GameModTrigger *tr;
    int nt = game_mod_triggers(&tr);
    static int choice[16];
    for (int i = 0; i < nt && i < 16; i++) {
        if (i == 0) opt_section("Try it now");
        bool on = !tr[i].available || tr[i].available();
        if (tr[i].nchoices > 0) {
            if (!on) opt_disable_next();
            opt_choice(tr[i].choice_label ? tr[i].choice_label : tr[i].label, &choice[i], tr[i].choices,
                       tr[i].nchoices, 0, tr[i].desc);
        }
        const char *why = !on && tr[i].why ? tr[i].why() : NULL;
        if (!on) opt_disable_next();
        if (why && *why) opt_action(tr[i].label, why, tr[i].desc, false);
        else if (opt_button(tr[i].label, tr[i].desc) && on && tr[i].trigger) tr[i].trigger(choice[i]);
    }
}

// ---------------------------------------------------------------- installed
static void credit(const UserMod &m, char *out, size_t cap) {
    snprintf(out, cap, "%s%s%s%s%s%s%s%s%s", m.author[0] ? "By " : "", m.author, m.author[0] ? ". " : "",
             m.category[0] ? m.category : "", m.category[0] ? ". " : "", m.license[0] ? "Licence: " : "", m.license,
             m.license[0] ? ". " : "", m.url);
}
static void apply_row(void) {
    if (!usermods_changed_since_start()) return;
    opt_section("Not applied yet");
    if (opt_button("Restart the game to apply",
                   "The changed mods load when the game starts again. This goes back to the start screen: progress "
                   "since your last save is lost."))
        rt_request_reboot(2), menu_close();
}
static void page_mod(int i) {
    UserMod m;
    if (!usermods_get(i, &m)) { g_mod = -1; return; }
    if (back_pressed()) { g_mod = -1, g_ask_remove = false, g_back_used = true; return; }
    char cred[700], desc[1800], conf[400], sz[32];
    credit(m, cred, sizeof cred);
    snprintf(desc, sizeof desc, "%s%s%s", m.description, m.description[0] ? "\n\n" : "", cred);
    opt_section(m.name);
    if (opt_action("< Back to installed mods", "", "Back to the list (B).", false)) { g_mod = -1, g_ask_remove = false; return; }
    bool on = m.enabled;
    int nconf = usermods_conflicts(i, conf, sizeof conf);
    char tdesc[1200];
    snprintf(tdesc, sizeof tdesc, "%s%s%s%s", m.has_romfs ? "Loads this mod's game files at the next start." :
             "This mod has no game files to load.", nconf && !on ? "\n\nIt replaces some of the same files as: " : "",
             nconf && !on ? conf : "", nconf && !on ? ". Turning it on turns those off." : "");
    if (!m.has_romfs) opt_disable_next();
    if (opt_toggle("On", &on, false, tdesc)) {
        char off[400];
        usermods_set_enabled(i, on, off, sizeof off);
        snprintf(g_note, sizeof g_note, "%s%s%s", off[0] ? "Turned off (same files): " : "", off, off[0] ? "." : "");
    }
    if (g_note[0]) opt_action(g_note, "", "", false);
    opt_action("By", m.author[0] ? m.author : "unknown", desc, false);
    if (m.category[0]) opt_action("Category", m.category, desc, false);
    if (m.license[0]) opt_action("Licence", m.license, "The author's licence. Mods are never part of " GAME_TITLE ": "
                                 "each player downloads them.", false);
    fmt_size(m.bytes, sz, sizeof sz);
    char files[64];
    snprintf(files, sizeof files, "%d (%s)", m.nfiles, sz);
    opt_action("Game files", files, "What it replaces or adds in the game's RomFS.", false);
    if (m.has_exefs) opt_action("Code patches", "not loaded yet", "Patches to the game's code (exefs / .ips) aren't supported yet.", false);
    opt_section("Order");
    if (opt_button("Load earlier", "Mods later in the list win where two replace the same file.")) usermods_move(i, -1), g_mod = i > 0 ? i - 1 : i;
    if (opt_button("Load later", "Mods later in the list win where two replace the same file.")) usermods_move(i, 1), g_mod = i + 1 < usermods_count() ? i + 1 : i;
    opt_section("More");
    if (m.url[0] && opt_button("Open its page", m.url)) SDL_OpenURL(m.url);
    if (!g_ask_remove) {
        if (opt_button("Remove", "Deletes the mod from this device.")) g_ask_remove = true;
    } else if (opt_button("Yes, remove it", "Deletes the mod's files from this device.")) {
        usermods_remove(i);
        g_mod = -1, g_ask_remove = false;
    }
}
static void page_installed(void) {
    if (g_mod >= 0) { page_mod(g_mod); return; }
    int n = usermods_count();
    if (usermods_crash_note()[0]) {
        opt_section("Turned off after an error");
        char d[800];
        snprintf(d, sizeof d, "The game stopped with an error while these mods were on, so they're off now: %s. If "
                 "the error wasn't theirs, turn them back on below.", usermods_crash_note());
        opt_action(usermods_crash_note(), "", d, false);
    }
    apply_row();
    if (!n) {
        opt_section("Installed mods");
        opt_action("None yet", "", "Get mods from GameBanana (the next page), or copy a mod's folder into the data "
                   "folder's mods/ (its files in mods/<name>/romfs).", false);
        return;
    }
    opt_section("Installed mods");
    opt_text("Search installed", g_search, sizeof g_search, "name, author, category", "Shows only the mods that match.");
    // by category: each one once, in the order first seen
    char cats[32][64];
    int nc = 0;
    for (int i = 0; i < n; i++) {
        UserMod m;
        if (!usermods_get(i, &m)) continue;
        const char *c = m.category[0] ? m.category : "Other";
        bool seen = false;
        for (int k = 0; k < nc && !seen; k++) seen = !strcmp(cats[k], c);
        if (!seen && nc < 32) snprintf(cats[nc++], sizeof cats[0], "%s", c);
    }
    for (int k = 0; k < nc; k++) {
        bool header = false;
        for (int i = 0; i < n; i++) {
            UserMod m;
            if (!usermods_get(i, &m) || strcmp(m.category[0] ? m.category : "Other", cats[k])) continue;
            if (!contains(m.name, g_search) && !contains(m.author, g_search) && !contains(cats[k], g_search)) continue;
            if (!header) opt_section(cats[k]), header = true;
            char label[260], cred[700], desc[2000], conf[300];
            snprintf(label, sizeof label, "%s%s%s", m.name, m.author[0] ? "  -  " : "", m.author);
            credit(m, cred, sizeof cred);
            int nconf = m.enabled ? 0 : usermods_conflicts(i, conf, sizeof conf);
            snprintf(desc, sizeof desc, "%s%s%s%s%s%s", m.description, m.description[0] ? "\n\n" : "", cred,
                     nconf ? "\n\nShares files with: " : "", nconf ? conf : "", m.has_exefs ? "\n\nIts code patches aren't loaded." : "");
            ImGui::PushID(i);
            if (opt_action(label, m.enabled ? "On" : nconf ? "Off (conflict)" : "Off", desc, false)) g_mod = i, g_note[0] = 0;
            ImGui::PopID();
        }
    }
}

// ---------------------------------------------------------------- GameBanana
static bool installed_gb(int id, bool *on) {
    int n = usermods_count();
    for (int i = 0; i < n; i++) {
        UserMod m;
        if (usermods_get(i, &m) && m.gb_id == id) { if (on) *on = m.enabled; return true; }
    }
    return false;
}
static void install_status_row(void) {
    int id;
    float p;
    char msg[200];
    GbState st = gb_install_state(&id, &p, msg, sizeof msg);
    if (!msg[0] || !strcmp(msg, "Starting")) return;
    char val[32] = "";
    if (st == GB_BUSY && p >= 0) snprintf(val, sizeof val, "%.0f%%", p * 100);
    opt_action(msg, val, st == GB_BUSY ? "Downloading and unpacking happen on this device. Select to cancel." : "", false);
    if (st == GB_BUSY && ImGui::IsItemClicked()) gb_install_cancel();
}
static void page_gb_mod(int id) {
    if (back_pressed()) { g_gb = 0, g_back_used = true; return; }
    GbMod m;
    bool have = gb_get(id, &m);
    if (!have) gb_details(id);
    opt_section(have ? m.name : "Loading...");
    if (opt_action("< Back to GameBanana", "", "Back to the list (B).", false)) { g_gb = 0; return; }
    if (!have) return;
    static char desc[9000];
    char v[64];
    gb_info(m, desc, sizeof desc);
    static int shot;
    static int shot_mod;
    if (shot_mod != id) shot = 0, shot_mod = id;
    if (m.nshots > 1) {
        static const char *sn[GB_MAX_SHOTS] = {"1", "2", "3", "4", "5", "6"};
        if (shot >= m.nshots) shot = 0;
        opt_choice("Screenshot", &shot, sn, m.nshots, 0, desc);
        gb_show_image(id, shot);
    }
    opt_action("By", m.author, desc, false);
    gb_show_image(id, shot);
    if (m.category[0]) opt_action("Category", m.category, desc, false);
    snprintf(v, sizeof v, "%d", m.downloads);
    opt_action("Downloads", v, desc, false);
    gb_show_image(id, shot);
    snprintf(v, sizeof v, "%d", m.likes);
    opt_action("Likes", v, desc, false);
    gb_show_image(id, shot);
    gb_show_image(id, shot);
    if (m.video[0] && opt_button("Watch the video", m.video)) SDL_OpenURL(m.video);
    if (m.license[0]) opt_action("Licence", m.license, "The author's licence. The mod is downloaded onto this device "
                                 "only; it is never part of " GAME_TITLE ".", false);
    opt_section("Install");
    if (m.nfiles > 1) {
        static const char *names[GB_MAX_FILES];
        static char labels[GB_MAX_FILES][200];
        for (int i = 0; i < m.nfiles; i++) {
            char sz[32];
            fmt_size(m.file_size[i], sz, sizeof sz);
            snprintf(labels[i], sizeof labels[i], "%s (%s)", m.file_name[i], sz);
            names[i] = labels[i];
        }
        if (g_gb_file >= m.nfiles) g_gb_file = 0;
        opt_choice("File", &g_gb_file, names, m.nfiles, 0, m.file_desc[g_gb_file][0] ? m.file_desc[g_gb_file] :
                   "The mod has several downloads (versions or parts): which one to install.");
    } else if (m.nfiles == 1) {
        char sz[32];
        fmt_size(m.file_size[0], sz, sizeof sz);
        opt_action("File", sz, m.file_name[0], false);
        g_gb_file = 0;
    }
    bool on = false, inst = installed_gb(id, &on);
    int busy_id;
    bool busy = gb_install_state(&busy_id, NULL, NULL, 0) == GB_BUSY;
    if (busy || !m.nfiles) opt_disable_next();
    if (opt_button(inst ? "Install again" : "Install and turn on",
                   "Downloads the mod from GameBanana onto this device, unpacks it and turns it on (another mod that "
                   "replaces the same files goes off). It loads at the next start."))
        gb_install(id, g_gb_file, true);
    install_status_row();
    char url[80];
    snprintf(url, sizeof url, "https://gamebanana.com/mods/%d", id);
    if (opt_button("Open on GameBanana", url)) SDL_OpenURL(url);
}
static void page_gb(void) {
    if (!gb_available()) { opt_section("GameBanana"); opt_action("Not for this game", "", "", false); return; }
    if (g_gb) { page_gb_mod(g_gb); return; }
    if (!g_gb_loaded) gb_refresh(NULL), g_gb_loaded = true;
    opt_section("GameBanana");
    if (opt_text("Search GameBanana", g_gb_search, sizeof g_gb_search, "name", "Searches GameBanana's mods for this game."))
        gb_refresh(g_gb_search);
    {   // order and category (the browse list; a search is ordered by how well it matches)
        GbCategory cats[32];
        int nc = gb_categories(cats, 32);
        static const char *cn[33];
        static char cl[33][64];
        snprintf(cl[0], sizeof cl[0], "All categories");
        cn[0] = cl[0];
        for (int i = 0; i < nc; i++) snprintf(cl[i + 1], sizeof cl[0], "%s (%d)", cats[i].name, cats[i].count), cn[i + 1] = cl[i + 1];
        if (g_gb_cat > nc) g_gb_cat = 0;
        int sort = g_gb_sort, cat = g_gb_cat;
        const char *why = "Applies to browsing; a search lists what matches best first.";
        opt_choice("Sort by", &sort, gb_sort_names, GB_NSORTS, 0, why);
        opt_choice("Category", &cat, cn, nc + 1, 0, why);
        if (sort != g_gb_sort || cat != g_gb_cat) {
            g_gb_sort = sort, g_gb_cat = cat;
            gb_set_filter(sort, cat > 0 ? cats[cat - 1].id : 0);
            gb_refresh(g_gb_search);
        }
    }
    if (g_gb_search[0] && opt_button("Most downloaded", "Back to the most downloaded mods."))
        g_gb_search[0] = 0, gb_refresh(NULL);
    install_status_row();
    char msg[160];
    GbState st = gb_list_state(msg, sizeof msg);
    static GbMod list[100];
    int n = st == GB_BUSY ? 0 : gb_list(list, 100);
    if (st != GB_IDLE || !n) {
        opt_action(msg[0] ? msg : "Nothing found", "", "Mods are listed from GameBanana's public site. Downloads go onto "
                   "this device only; each mod's author and licence are shown on its page.", false);
        if (st == GB_ERROR && opt_button("Try again", "")) gb_refresh(g_gb_search);
    }
    opt_section(g_gb_search[0] ? "Results" : gb_sort_names[g_gb_sort]);
    for (int i = 0; i < n; i++) {
        bool on = false, inst = installed_gb(list[i].id, &on);
        char label[260];
        static char desc[9000];
        snprintf(label, sizeof label, "%s  -  %s", list[i].name, list[i].author);
        if (list[i].details)
            gb_info(list[i], desc, sizeof desc);
        else snprintf(desc, sizeof desc, "%s\n\nSubmitter: %s", list[i].name, list[i].author);  // (the details fill in below it)
        ImGui::PushID(list[i].id);
        if (opt_action(label, inst ? (on ? "On" : "Installed") : "", desc, false)) g_gb = list[i].id, g_gb_file = 0;
        if (ImGui::IsItemFocused() || ImGui::IsItemHovered()) {  // (the pane fills in)
            if (!list[i].details) gb_details(list[i].id);
            gb_show_image(list[i].id, 0);
        }
        ImGui::PopID();
    }
}

// ---------------------------------------------------------------- the tab
// (tests) LARECOMP_MODS_PAGE=<0..3>: the page shown first; RECOMP_GB_INSTALL=<mod id>[:<file>]: install that mod from
// GameBanana in the background at the first look at the tab (game_boot also calls mods_page_test)
void mods_page_test(void) {
    static bool done;
    if (done) return;
    done = true;
    if (const char *p = getenv("LARECOMP_MODS_PAGE")) g_page = atoi(p);
    if (const char *p = getenv("LARECOMP_MODS_MOD")) g_mod = atoi(p);   // (an installed mod's page open)
    if (const char *p = getenv("LARECOMP_MODS_GB")) g_gb = atoi(p);     // (a GameBanana mod's page open)
    if (getenv("RECOMP_GB_INSTALL")) {  // (a list: id[:file],...; one after another; RECOMP_GB_INSTALL_OFF=1: installed off)
        static pthread_t t;
        pthread_create(&t, NULL, [](void *) -> void * {
            const char *g = getenv("RECOMP_GB_INSTALL");
            bool on = !getenv("RECOMP_GB_INSTALL_OFF");
            for (const char *p = g; p && *p;) {
                int id = 0, fi = 0;
                if (sscanf(p, "%d:%d", &id, &fi) >= 1 && id > 0) {
                    gb_install(id, fi, on);
                    SDL_Delay(200);
                    char msg[200];
                    while (gb_install_state(NULL, NULL, msg, sizeof msg) == GB_BUSY) SDL_Delay(200);
                    fprintf(stderr, "[gamebanana] test install %d:%d: %s\n", id, fi, msg);
                }
                p = strchr(p, ',');
                if (p) p++;
            }
            fprintf(stderr, "[gamebanana] test installs done\n");
            return NULL;
        }, NULL);
        pthread_detach(t);
    }
}
void mods_page(void) {
    mods_page_test();
    char installed[40];
    snprintf(installed, sizeof installed, "Installed (%d)", usermods_count());
    const char *pages[] = {"Built-in", installed, "GameBanana"};
    int np = gb_available() ? 3 : 2;
    if (g_page >= np) g_page = 0;
    opt_section("Mods");
    int page = g_page;
    opt_choice("Show", &page, pages, np, 0, "Built-in: this program's own mods. Installed: mods you added (they "
               "load at the next start). GameBanana: get more.");
    if (page != g_page) g_mod = -1, g_gb = 0;  // (another page: no mod's page open)
    g_page = page;
    switch (g_page) {
    case 0: page_builtin(); break;
    case 1: page_installed(); break;
    default: page_gb(); break;
    }
}

// The Online tab (owner, 2026-10-30: the Mods tab's Online page was hard to find): the backend and its accounts, the
// game's own rows for it (OpenCourse World's domain, address and token), then the app's (updates, game files).
void online_page(void) {
    if (online_rows_available()) online_rows();
    game_rows();
    launcher_settings_rows();
}
