// Mod 2, Mystery Mushroom costumes (research/specs/recomp-mods.md §3): SMM1's costumes worn in SMB1-style courses.
//
// The player wearing a costume stays form 0 (small Mario) for the game's logic: size class, physics, hit boxes and the
// block and power-up rules are untouched, which is SMM1's costume behaviour. What changes:
//   - Look (overrides shared with lanky.c: player_model.c): the model builder (0x710155fcd0, once per player per
//     course) loads, for the SMB1 style, a costume into the spare form slot 12 (Balloon: unused in SMB1, size class 1
//     like small Mario) by pointing that slot's model name at "Player_Chara_<res>" for the call; slot 12 is shown where
//     the game asks for slot 0 while the costume is worn (the visual slot setter 0x710155fcc0, and each update). The
//     models are generated from the player's dumps (smm1/costume_gen.c) and laid over the romfs.
//   - Another costume mid-course: every costume model is the same template (SMM2's small SMB1 Mario: skeleton,
//     material, animations, the 24 x 24 quad) with other texels in its 25 textures, so the costume a model shows is its
//     texel data alone. The textures the builder makes for slot 12 are noted (runtime game_texture_init: 25 of 24 x 24,
//     in the BNTX's order, made in place over the loaded file), and another costume is put on by writing its texels over
//     them (costume_look: the generated model's own bytes, so nothing but the pixels differs); the renderers see the
//     change as for any texture the game rewrites. Before each write the texels are checked to be the look noted last
//     (a texture since dropped, or memory used for something else, is never written). The game keeps a loaded model
//     for later courses, so one that is used again is put back to its own costume when the builder asks for it.
//   - Pickup: SMM1's Mystery Mushroom is the SMB1 skin of the style power-up (the Big Mushroom): taking it as small,
//     Super or Fire Mario puts a costume on instead (the change becomes one to small: from small the costume is on at
//     once, from Super / Fire after the game's shrink), drawn at each pickup as SMM1 does: uniform over the owned
//     costumes minus the one worn (costume_item=super: the Super Mushroom instead, as SMCraft did). Another power-up
//     replaces the costume. The item looks like SMM1's "?" mushroom (its model in the generated pack,
//     smm1/costume_gen.c mystery_item).
//   - Damage: a hit takes the costume off with the Super -> small power-down (the form is read as 1 for the game's
//     damage function, hook point 0x15a50a4), as a costume absorbs a hit in SMM1; its freeze and invulnerability blink
//     are SMM2's, its look SMM1's: small, costume, small, costume, small, 6 updates each (LOSS_FRAMES), then small
//     Mario blinking.
//   - Facing: slot 12 is Balloon Mario's, which the holder's placement never turns; for a costume it does (holder_place).
//   - Taunt: Up held 8 updates standing still, in costume: the costume's appeal pose for 120 updates (its appeal.0 frame
//     written over the model's textures; moving, jumping or a hit ends it) and its appeal sound.
//   - Sound: the costume's own power-up, jump, landing, taunt, goal and death sounds (SMM1's Sound/CharaMario banks,
//     host mixer). SMM2's goal and death fanfares give way to them (their choosers, 0x71017a5020 / 0x71017a4bc0, see the
//     state 6 / 7 request) and so does SMM2's jump sound: the player's jump sound (0x71015c78b0) reads its request as
//     silent (hook point 0x15c78c4) and one state's own "jump" (0x71015cfe70) gets a sound user without an instance
//     (hook point 0x15cfe90); the costume's jump plays there instead. An event the costume has no sound for keeps
//     SMM2's (SMM1 falls back to the normal sound). Nothing changes without a costume worn.
//   - Sub-areas: the player object (and the costume model) stays through a pipe to the sub-area and back, so the
//     costume stays on (seen on the test course, tools/costume_course.py --variant subarea).
//   - Reward: one random costume not owned yet at the end of a 100 Mario run, from SMM1's pools by difficulty
//     (smm1-100mario-code.md §7); owned costumes are kept in costumes-owned.txt in the data folder.
//   - "Mystery Mushroom costume now" in the overlay menu's Try it now (triggers.c): player 1 takes a Mystery Mushroom
//     with the costume chosen in its row (any costume: it is not added to the owned ones), through the game's own
//     form change request as a pickup. Greyed out outside an SMB1 course being played, in Endless / 100 Mario runs (they
//     keep records) and with a power-up the Mystery Mushroom doesn't take (anything but small, Super and Fire).
//   - The SMM1 title letters' M (title_letters/letters.c): on the title, its Mystery Mushroom gives Wario's costume
//     (costume_pickup_gives), as SMM1's title M did.
// Settings: costumes=1 in smm2mods.ini (or RECOMP_MOD_COSTUMES=1) turns it on (models generated at the next start);
// costume_item=big|super. Tests: RECOMP_COSTUME=<res> wears that costume from each course's start and gives it at each
// pickup; RECOMP_COSTUME_TRY=<frame>:<res> presses "Mystery Mushroom costume now" with that costume at that presented
// frame; RECOMP_COSTUME_DEBUG=1 logs forms, slots, textures and sounds.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "mod.h"

int costume_count(void);
const char *costume_res(int i);
int costume_rare(const char *res);
void costume_list(void);
void costume_name(const char *res, char *name, size_t cap, char *title, size_t tcap);
int costume_look(const char *res, bool appeal, uint8_t **out);
bool smm1_data_open(void);
void smm1_se_bank(const char *bank);
bool smm1_se(const char *name);
bool smm1_se_has(const char *name);
extern uint64_t renderer_present_count(void);

#define NAME_TABLE MAIN_ADDR(0x2c17638)   // model base names: SafeString {vtable, char *} [style * 16 + form]
#define STYLE_PTR MAIN_ADDR(0x2c57d58)    // *(*(this + 0x30) + 0x1c): the course's style (0 SMB1)
#define PLAYERS MAIN_ADDR(0x2c19290)      // ptr: the players at +0x28 + 8 * i (i < 4)
#define GOAL_FANFARE MAIN_ADDR(0x17a5020) // course music state changes; state 6 = the goal fanfare
#define DOWN_FANFARE MAIN_ADDR(0x17a4bc0) // ... state 7 = the death fanfare
#define FORM_REQUEST MAIN_ADDR(0x15a94a0) // (player, int *form): 0 accepted, 1 the same change pending, 2 refused
#define DAMAGE_FORM_LOAD 0x15a50a4        // hook point: ldr w8, [x19, #0x4a8] at the damage function's start
#define TRANSFORM_REQ 0x15a94a0           // hook point: the form change request's first instruction (player, int *form)
#define JUMP_SOUND_SILENT 0x15c78c4       // hook point: the jump sound (0x71015c78b0, player) loads the request's silent
                                          // byte (+0x24a4) into w8; set, it returns before any sound
#define STATE_JUMP_KEY 0x15cfe90          // hook point: a state's enter (0x71015cfe70) has made "jump" in x1, the
                                          // player's sound user in x0, for 0x7100666af0
#define COSTUME_SLOT 12
#define P_FORM 0x4a8      // the player's form (0 small, 1 Super, 2 Fire, ...)
#define P_PENDING 0x4b0   // the form a change asked for
#define P_JUMP_KIND 0x249c  // the jump request's kind: 1 "jump", 2 "jump_high" (not in SMB1), 3 none, 4 "voice_jump_high"
                            // [obs 0x71015c78b0]
#define P_HOLDER 0x2f90   // the player's model holders (the builder's and the setter's x0) [obs at run time, lanky.c]
#define P_HOLDER2 0x2f98
#define H_STYLE 0x470     // the holder's style (0 SMB1)
#define H_SLOT 0x478      // the holder's visual slot
#define TEX_W 24          // a costume model's textures: 24 x 24 RGBA8 sRGB (NVN format 0x38), 4096 bytes each
#define TEX_FMT 0x38
#define TEX_BYTES 4096
#define TAUNT_FRAMES 120

// ---------------------------------------------------------------- settings (a block of its own in smm2mods.ini)
static int g_on = -1, g_item = 6;
static char g_forced[48], g_course[48], g_model_name[96] = "Player_Chara_Mario";

static const char *ini_path(void) { return getenv("RECOMP_MOD_INI") ? getenv("RECOMP_MOD_INI") : "smm2mods.ini"; }
static int ini_int(const char *key, int def) {
    FILE *f = fopen(ini_path(), "r");
    if (!f) return def;
    char line[256];
    size_t kl = strlen(key);
    int v = def;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, key, kl) && line[kl] == '=') v = atoi(line + kl + 1);
    fclose(f);
    return v;
}
static bool ini_is(const char *key, const char *val) {
    FILE *f = fopen(ini_path(), "r");
    if (!f) return false;
    char line[256], want[128];
    snprintf(want, sizeof want, "%s=%s", key, val);
    bool r = false;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, want, strlen(want))) r = true;
    fclose(f);
    return r;
}
static void ini_put(const char *key, const char *val) {  // replace or append key=val, the rest kept
    char *text = NULL;
    size_t n = 0;
    FILE *f = fopen(ini_path(), "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        n = (size_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        text = calloc(1, n + 1);
        if (fread(text, 1, n, f) != n) n = 0;
        fclose(f);
    }
    f = fopen(ini_path(), "w");
    if (!f) { free(text); return; }
    size_t kl = strlen(key);
    bool done = false;
    for (char *s = text, *e; s && *s; s = e ? e + 1 : NULL) {
        e = strchr(s, '\n');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        if (!strncmp(s, key, kl) && s[kl] == '=') {
            if (!done) fprintf(f, "%s=%s\n", key, val), done = true;
        } else
            fprintf(f, "%.*s\n", (int)l, s);
    }
    if (!done) fprintf(f, "%s=%s\n", key, val);
    fclose(f);
    free(text);
}
static int g_debug = -1;
bool costumes_on(void) {
    if (g_on < 0) {
        const char *e = getenv("RECOMP_MOD_COSTUMES");
        g_on = e ? atoi(e) != 0 : ini_int("costumes", 0) != 0;
        const char *w = getenv("RECOMP_COSTUME"), *it = getenv("RECOMP_COSTUME_ITEM");
        if (w && *w) snprintf(g_forced, sizeof g_forced, "%s", w);
        g_item = (it ? !strcmp(it, "super") : ini_is("costume_item", "super")) ? 1 : 6;
        g_debug = getenv("RECOMP_COSTUME_DEBUG") && atoi(getenv("RECOMP_COSTUME_DEBUG"));
        if (g_on)
            LOG("[costume] Mystery Mushroom costumes on (the %s Mushroom)%s%s\n", g_item == 1 ? "Super" : "Big",
                g_forced[0] ? ", wearing " : "", g_forced);
    }
    return g_on;
}
// the form the Mystery Mushroom item asks for: 6 the Big Mushroom, 1 the Super Mushroom (costume_item=super); its model
// gets SMM1's look (smm1/costume_gen.c)
int costume_item_form(void) {
    costumes_on();
    return g_item;
}
// the Mods tab's switch (the models are generated at the next start)
static int g_setting = -1;  // the switch as last set (the mod itself follows at the next start)
bool costumes_setting(void) { return g_setting >= 0 ? g_setting != 0 : costumes_on(); }
void costumes_set_setting(bool on) {
    if (on == costumes_setting()) return;
    g_setting = on;
    ini_put("costumes", on ? "1" : "0");
    LOG("[costume] Mystery Mushroom costumes %s (at the next start)\n", on ? "on" : "off");
}

// (the models: the shared RomFS overlay's "costumes" generator, smm1/costume_gen.c)

// ---------------------------------------------------------------- owned costumes
static char (*g_owned)[48];
static int g_nowned = -1;
static void load_owned(void) {
    if (g_nowned >= 0) return;
    g_nowned = 0;
    costume_list();
    int n = costume_count();
    g_owned = calloc((size_t)n + 8, sizeof *g_owned);
    for (int i = 0; i < n; i++)  // category 0: always owned (Goomba, ? Block, Trampoline)
        if (costume_rare(costume_res(i)) == 0) snprintf(g_owned[g_nowned++], 48, "%s", costume_res(i));
    FILE *f = fopen("costumes-owned.txt", "r");
    char line[96];
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        bool have = !line[0];
        for (int i = 0; i < g_nowned && !have; i++) have = !strcmp(g_owned[i], line);
        if (!have && g_nowned < n + 8) snprintf(g_owned[g_nowned++], 48, "%s", line);
    }
    if (f) fclose(f);
}
static bool owned(const char *res) {
    load_owned();
    for (int i = 0; i < g_nowned; i++)
        if (!strcmp(g_owned[i], res)) return true;
    return false;
}
static unsigned rnd(void) {
    static unsigned s;
    if (!s) s = (unsigned)time(NULL) * 2654435761u | 1;
    s ^= s << 13, s ^= s >> 17, s ^= s << 5;
    return s;
}

// SMM1's draw at a pickup (smm1-extras-costume.md §5): uniform over the owned costumes minus the one worn; NULL if none
static const char *draw(const char *worn) {
    load_owned();
    if (g_forced[0]) return g_forced;
    int cand[256], nc = 0;
    for (int i = 0; i < g_nowned && nc < 256; i++)
        if (!worn || strcmp(g_owned[i], worn)) cand[nc++] = i;
    return nc ? g_owned[cand[rnd() % (unsigned)nc]] : worn;
}

// the costume reward at the end of a won 100 Mario run (ending.c calls it after the credits): one costume not owned
// yet, uniformly, from the pools up to the difficulty's (Easy 1; Normal 1, 2; Expert 1-3; Super Expert 1-3, 6), saved;
// NULL when none is left
const char *costume_reward_pick(int difficulty) {
    if (!costumes_on()) return NULL;
    load_owned();
    static const int ceiling[4] = {1, 2, 3, 6};
    int d = difficulty < 0 ? 0 : difficulty > 3 ? 3 : difficulty;
    int n = costume_count(), cand[256], nc = 0;
    for (int i = 0; i < n && nc < 256; i++) {
        int r = costume_rare(costume_res(i));
        if (r == 0 || r == 4 || r == 5 || r == 7 || r < 0 || r > ceiling[d]) continue;
        if (!owned(costume_res(i))) cand[nc++] = i;
    }
    if (!nc) { LOG("[costume] reward: none left at this difficulty\n"); return NULL; }
    const char *res = costume_res(cand[rnd() % (unsigned)nc]);
    snprintf(g_owned[g_nowned++], 48, "%s", res);
    FILE *f = fopen("costumes-owned.txt", "a");
    if (f) fprintf(f, "%s\n", res), fclose(f);
    LOG("[costume] reward: %s (%d left)\n", res, nc - 1);
    return res;
}
// ending.c's weak hook: the run is won (the reward itself is drawn by the ending, which asks costume_reward_pick)
void hundred_costume_reward(int difficulty) { (void)difficulty; }
// The costume reward after a won 100 Mario run, off by default (owner, 2026-10-24: any costume can be chosen in Try it
// now, so the reward unlocks nothing worth a window): costume_reward=1 in smm2mods.ini, or RECOMP_COSTUME_REWARD=1
bool costume_reward_on(void) {
    const char *e = getenv("RECOMP_COSTUME_REWARD");
    return costumes_on() && (e ? atoi(e) != 0 : ini_int("costume_reward", 0) != 0);
}

bool smb1_course(void) {
    uint64_t g = g64(STYLE_PTR);
    return g && g64(g + 0x30) && g32(g64(g + 0x30) + 0x1c) == 0;
}

// ---------------------------------------------------------------- the costume models' textures
// A group: the 25 textures of one loaded costume model (the builder's slot 12), their texels one after another as the
// BNTX lays them out. file: the costume it was loaded as (the model's file name); look: the costume it shows now.
typedef struct {
    uint64_t addr;
    int count;
    uint32_t character;
    char file[48], look[48];
    bool appeal;  // ... in the taunt pose
} Group;
#define MAX_GROUPS 16
static Group g_groups[MAX_GROUPS];
static int g_ngroups;
static volatile uint64_t g_building;   // the holder being built (player_model.c), 0 outside the builder
static uint32_t g_building_char;
static Group *g_new;                   // the group the builder's textures are going into
static struct { uint64_t holder; uint32_t character; char file[48]; } g_holders[16];  // what each holder loaded
static int g_nholders;

// looks read back from the generated models (costume_look), the last few kept
static struct { char res[48]; bool appeal; int count; uint8_t *data; } g_looks[6];
static int g_nlooks;
static const uint8_t *look_data(const char *res, bool appeal, int *count) {
    for (int i = 0; i < g_nlooks; i++)
        if (!strcmp(g_looks[i].res, res) && g_looks[i].appeal == appeal) { *count = g_looks[i].count; return g_looks[i].data; }
    uint8_t *d;
    int n = costume_look(res, appeal, &d);
    if (!n) { *count = 0; return NULL; }
    int slot = g_nlooks < 6 ? g_nlooks++ : (int)(rnd() % 6u);
    free(g_looks[slot].data);
    snprintf(g_looks[slot].res, 48, "%s", res);
    g_looks[slot].appeal = appeal, g_looks[slot].count = n, g_looks[slot].data = d;
    *count = n;
    return d;
}

static void drop_group(Group *g) {
    if (g_new == g) g_new = NULL;
    *g = g_groups[--g_ngroups];
    if (g_new == &g_groups[g_ngroups]) g_new = g;
}
// the group's textures to show res (appeal: its taunt pose); false if they no longer hold what was written last
static bool set_look(Group *g, const char *res, bool appeal) {
    if (!strcmp(g->look, res) && g->appeal == appeal) return true;
    int n0, n1;
    const uint8_t *now = look_data(g->look, g->appeal, &n0), *want = look_data(res, appeal, &n1);
    if (!now || !want || n0 != g->count || n1 != g->count) {
        LOG("[costume] %s%s: no such model (%d / %d textures)\n", res, appeal ? " (taunt)" : "", n1, g->count);
        return false;
    }
    void *mem = (void *)(uintptr_t)g->addr;
    if (memcmp(mem, now, (size_t)g->count * TEX_BYTES)) {
        LOG("[costume] the textures of %s at %#llx changed under us: left alone\n", g->file, (unsigned long long)g->addr);
        drop_group(g);
        return false;
    }
    memcpy(mem, want, (size_t)g->count * TEX_BYTES);
    if (g_debug > 0) LOG("[costume] model %s (character %u) now shows %s%s\n", g->file, g->character, res, appeal ? " (taunt)" : "");
    snprintf(g->look, sizeof g->look, "%s", res);
    g->appeal = appeal;
    return true;
}
static Group *group_of(uint64_t holder) {
    for (int i = 0; i < g_nholders; i++)
        if (holder && g_holders[i].holder == holder)
            for (int k = 0; k < g_ngroups; k++)
                if (g_groups[k].character == g_holders[i].character && !strcmp(g_groups[k].file, g_holders[i].file))
                    return &g_groups[k];
    return NULL;
}

// runtime/nvn: the builder's 24 x 24 textures for slot 12 (the costume model is the only one of that size it loads)
void game_texture_init(uint64_t addr, uint32_t width, uint32_t height, uint32_t format) {
    if (!g_building || width != TEX_W || height != TEX_W || format != TEX_FMT || !g_course[0]) return;
    if (g_new && addr == g_new->addr + (uint64_t)g_new->count * TEX_BYTES) { g_new->count++; return; }
    if (g_ngroups == MAX_GROUPS) drop_group(&g_groups[0]);
    g_new = &g_groups[g_ngroups++];
    *g_new = (Group){addr, 1, g_building_char, "", "", false};
    snprintf(g_new->file, sizeof g_new->file, "%s", g_course);
    snprintf(g_new->look, sizeof g_new->look, "%s", g_course);
}
void game_texture_final(uint64_t addr) {
    for (int i = 0; i < g_ngroups; i++)
        if (addr >= g_groups[i].addr && addr < g_groups[i].addr + (uint64_t)g_groups[i].count * TEX_BYTES) {
            if (g_debug > 0) LOG("[costume] model %s (character %u) dropped\n", g_groups[i].file, g_groups[i].character);
            drop_group(&g_groups[i]);
            return;
        }
}

// ---------------------------------------------------------------- the players
typedef struct {
    uint64_t player;
    char wear[48];          // the costume worn ("" none)
    int up, taunt;          // updates Up was held standing still; taunt pose updates left
    bool was_ground;
    int losing;             // the costume loss (SMM1's): updates left of its model changes (LOSS_FRAMES down to 0)
    int lost;               // updates left in which the game's big slot is small Mario (the power-down it plays)
    volatile bool asked;    // "Mystery Mushroom costume now" (the menu's thread): the costume in want
    char want[48];
} Wearer;
static Wearer g_p[4];
static const char *g_giving;  // a costume the next form request gives (the trigger's), NULL: a pickup's draw
static char g_pickup[48];     // the costume every pickup gives instead of a draw (the SMM1 title's M: Wario), "" none
void costume_pickup_gives(const char *res) { snprintf(g_pickup, sizeof g_pickup, "%s", res ? res : ""); }

static int player_index(uint64_t player) {
    uint64_t pm = g64(PLAYERS);
    for (int i = 0; pm && i < 4; i++)
        if (g64(pm + 0x28 + 8 * (uint64_t)i) == player) return i;
    return -1;
}
static Wearer *wearer_of_holder(uint64_t holder) {
    uint64_t pm = g64(PLAYERS);
    for (int i = 0; pm && holder && i < 4; i++) {
        uint64_t p = g64(pm + 0x28 + 8 * (uint64_t)i);
        if (p && p == g_p[i].player && (g64(p + P_HOLDER) == holder || g64(p + P_HOLDER2) == holder)) return &g_p[i];
    }
    return NULL;
}

// the costume's sound for event from its bank, or its Static bank (SMM1 keeps powerup there: smm1-extras-costume.md §7);
// false: this costume has no such sound
static bool sound(const char *res, const char *event) {
    if (!res || !res[0] || !smm1_data_open()) return false;
    bool ok = false;
    for (int k = 0; k < 2 && !ok; k++) {
        char bank[96], name[128];
        snprintf(bank, sizeof bank, "CharaMario_%s%s", res, k ? "Static" : "");
        smm1_se_bank(bank);
        snprintf(name, sizeof name, "%s/%s", bank, event);
        ok = smm1_se(name);
    }
    bool often = !strncmp(event, "jump", 4) || !strcmp(event, "ground");  // (every jump: only when debugging)
    if (!often || g_debug > 0) LOG("[costume] %s sound (%s)%s\n", event, res, ok ? "" : ": none");
    return ok;
}
// whether the costume has its own sound for event (SMM1: an event the costume lacks plays the normal sound,
// smm1-extras-costume.md §7)
static bool has_sound(const char *res, const char *event) {
    if (!res || !res[0] || !smm1_data_open()) return false;
    for (int k = 0; k < 2; k++) {
        char bank[96], name[128];
        snprintf(bank, sizeof bank, "CharaMario_%s%s", res, k ? "Static" : "");
        smm1_se_bank(bank);
        snprintf(name, sizeof name, "%s/%s", bank, event);
        if (smm1_se_has(name)) return true;
    }
    return false;
}
static void taunt_end(Wearer *w) {
    w->taunt = 0;
    Group *g = w->player ? group_of(g64(w->player + P_HOLDER)) : NULL;
    if (g && g->appeal) set_look(g, g->look, false);
}
// the player puts res on (NULL / "": takes the costume off); its model is made to show it
static void wear(Wearer *w, const char *res, const char *why) {
    if (w->taunt) taunt_end(w);
    snprintf(w->wear, sizeof w->wear, "%s", res ? res : "");
    if (!w->wear[0]) return;
    Group *g = w->player ? group_of(g64(w->player + P_HOLDER)) : NULL;
    bool ok = g && set_look(g, w->wear, false);
    LOG("[costume] player %d wears %s (%s)%s\n", (int)(w - g_p), w->wear, why, ok ? "" : ": its model isn't known, so it looks like another");
}
const char *costume_worn(void) { return g_p[0].wear; }

// ---------------------------------------------------------------- the model holder (player_model.c)
// the builder: for the SMB1 style, slot 12's file is a costume (one draw per course: every holder of the course loads
// the same one, and the game keeps it loaded)
int costume_model_swaps(uint64_t holder, uint32_t style, uint32_t character, PmSwap *out) {
    (void)holder, (void)character;
    if (!costumes_on() || style != 0) return 0;
    static uint64_t drawn_at;
    uint64_t now = renderer_present_count();
    if (!g_course[0] || now - drawn_at > 30) {  // (a new course: its holders are built within a frame or two)
        extern const char *game_romfs_overlay(void);
        game_romfs_overlay();  // (the models: generated by now, unless RECOMP_MOD_DIR took the romfs's place: then now)
        const char *res = draw(NULL);
        uint8_t *probe = NULL;  // (only a costume whose model was generated: a missing file stops the game)
        int n = 0;
        for (int tries = 0; res && tries < 8 && !(n = costume_look(res, false, &probe)); tries++) res = draw(res);
        // (none owned yet - costumes are 100 Mario's rewards: still a costume model in the slot, so "Mystery Mushroom
        // costume now" can put any costume on; a pickup still needs an owned one)
        for (int i = 0; !n && i < costume_count(); i++)
            if ((n = costume_look(costume_res(i), false, &probe))) res = costume_res(i);
        free(probe);
        snprintf(g_course, sizeof g_course, "%s", n ? res : "");
        if (!n) LOG("[costume] no costume model to load (an owned costume without its generated model?)\n");
    }
    drawn_at = now;
    if (!g_course[0]) return 0;
    snprintf(g_model_name, sizeof g_model_name, "Player_Chara_%s", g_course);
    out[0] = (PmSwap){NAME_TABLE + (uint64_t)(style * 16 + COSTUME_SLOT) * 16 + 8, (uint64_t)(uintptr_t)g_model_name, 0};
    g_building_char = character;
    if (g_debug > 0) LOG("[costume] holder %#llx (character %u): %s in slot %d\n", (unsigned long long)holder, character, g_course, COSTUME_SLOT);
    return 1;
}
// around the builder's call: the textures it makes are the costume model's, and a model loaded before (the game keeps
// them) is put back to its own costume
void costume_model_building(uint64_t holder, uint32_t style) {
    if (!costumes_on() || style != 0 || !g_course[0]) return;
    if (holder) {
        g_building = holder, g_new = NULL;
        int i = 0;
        while (i < g_nholders && g_holders[i].holder != holder) i++;
        if (i == g_nholders) i = g_nholders < 16 ? g_nholders++ : (int)(rnd() % 16u);
        g_holders[i].holder = holder, g_holders[i].character = g_building_char;
        snprintf(g_holders[i].file, sizeof g_holders[i].file, "%s", g_course);
        return;
    }
    if (g_new && g_debug > 0)
        LOG("[costume] model %s (character %u): %d textures at %#llx\n", g_new->file, g_new->character, g_new->count, (unsigned long long)g_new->addr);
    Group *g = group_of(g_building);
    if (g) set_look(g, g->file, false);
    g_building = 0, g_new = NULL;
}

// The costume loss, as SMM1 draws it (Block.rpx v208: the damage takes form 7 to 0, a change of the same size class,
// transform type 3 from the matrix at 0x100ca480; the transform's Active state, enter 0x0268151c, runs 30 updates, and
// its update 0x02681530 shows the new form while (6 - timer / 6) is even and the old one while it is odd, both at
// scale 1 for type 3): small, costume, small, costume, small, 6 updates each, then small Mario with the game's
// invulnerability blink. In SMM2 the hit is the Super -> small power-down (the form read as 1), whose freeze and blink
// are kept; what the holders show during it is this sequence, and the big slot it asks for is small Mario after it.
#define LOSS_FRAMES 30
#define LOSS_STEP 6
#define LOST_FRAMES 180  // (longer than the power-down and its blink: only while the player is still form 0)
static int32_t loss_slot(const Wearer *w) {
    int k = LOSS_FRAMES - w->losing;
    return (k / LOSS_STEP) & 1 ? COSTUME_SLOT : 0;
}
// the slot a costume wearer's holder shows for the one the game asks (slot); -1: the game's
static int32_t wearer_slot(const Wearer *w, int32_t slot) {
    if (w->losing > 0 && (slot == 0 || slot == 1)) return loss_slot(w);
    if (slot == 0 && w->wear[0]) return COSTUME_SLOT;
    if (slot == 1 && w->lost > 0 && !w->wear[0] && g32(w->player + P_FORM) == 0 && g32(w->player + P_PENDING) == 0)
        return 0;  // (the power-down's big frames: the player is small now, as in SMM1)
    return -1;
}
// the visual slot setter: a player in costume shows slot 12 for small, and the loss's sequence while it comes off
int32_t costume_visual_slot(uint64_t holder, int32_t slot) {
    if (!costumes_on() || g32(holder + H_STYLE) != 0) return slot;
    Wearer *w = wearer_of_holder(holder);
    int32_t s = w ? wearer_slot(w, slot) : -1;
    if (g_debug > 0 && w && (w->losing > 0 || w->lost > 0))
        LOG("[costume] player %d: the game asks slot %d, shown %d (loss %d)\n", (int)(w - g_p), slot, s < 0 ? slot : s,
            LOSS_FRAMES - w->losing);
    return s < 0 ? slot : s;
}
// The holder's placement each frame (0x7101561bb0, holder vtable +0xb8: holder, the matrix, the scale, the facing):
// for slot 12 the facing is taken as 0 (Balloon Mario never turns: cmp w8, #0xc; csel w26, wzr, w3, eq), unless the
// holder's facing override (+0x4b0, -1 none) is set. A costume turns as small Mario does, so for an SMB1 holder showing
// slot 12 the override is the facing passed, for the call (and +0x4ac, the facing it keeps, is what slot 0's would be).
#define HOLDER_PLACE MAIN_ADDR(0x1561bb0)
#define H_FACING_SET 0x4b0  // the holder's facing override (-1 none)
#define H_FACING 0x4ac      // the facing of the last placement (the override aside)
static void holder_place(Cpu *c) {
    uint64_t h = c->x[0];
    uint32_t facing = (uint32_t)c->x[3];
    bool turn = costumes_on() && g32(h + H_SLOT) == COSTUME_SLOT && g32(h + H_STYLE) == 0 && g32(h + H_FACING_SET) == ~0u;
    if (turn) s32(h + H_FACING_SET, facing);
    guest_call_orig(c, HOLDER_PLACE);
    if (turn) s32(h + H_FACING_SET, ~0u), s32(h + H_FACING, facing);
}

// (each update: the slots the holders show, as the setter would, and the model showing the costume worn: a model built
// again mid-course starts as its own costume)
static void show(Wearer *w) {
    uint64_t h[2] = {g64(w->player + P_HOLDER), g64(w->player + P_HOLDER2)};
    Group *g = w->wear[0] ? group_of(h[0]) : NULL;
    if (g && (strcmp(g->look, w->wear) || g->appeal != (w->taunt > 0))) set_look(g, w->wear, w->taunt > 0);
    for (int k = 0; k < 2; k++) {
        if (!h[k] || g32(h[k] + H_STYLE) != 0) continue;
        int32_t shown = (int32_t)g32(h[k] + H_SLOT), s = wearer_slot(w, shown);
        if (w->losing > 0 && shown == COSTUME_SLOT) s = loss_slot(w);  // (the sequence's own last change)
        if (s < 0 && !w->wear[0] && !w->losing && shown == COSTUME_SLOT) s = 0;  // (the costume is off)
        if (s >= 0 && s != shown) s32(h[k] + H_SLOT, (uint32_t)s);
    }
}

// ---------------------------------------------------------------- each update of each player (games/smm2/trace.c)
// SMM1's costume sounds from the player's motion (SMCraft's events): the landing (ground) and the taunt (Up held 8
// updates standing still: the appeal pose and sound). The jump's comes from the game's own jump sound request (above).
#define P_GROUND 0x2308  // bit 0: on the ground
#define P_VEL_Y 0x240    // float, up positive [inf: the sign from the jump test]
#define P_SPEED 0x274    // float, forward speed
#define P_INPUT 0x550    // pointer to the player's input block: +0x10 Up held, +0x11 Down held [obs: Link's arrow aim
                         // 0x71015cd8e0 and the door test 0x71015a9c60 read +0x10 as up, the crouch test 0x71015d6270
                         // +0x11 as down]
static bool g_goal;      // the goal was reached: no taunt until the next course
static void motion(Wearer *w, uint64_t player) {
    bool ground = g32(player + P_GROUND) & 1;
    float vy, sp;
    uint32_t a = g32(player + P_VEL_Y), b = g32(player + P_SPEED);
    memcpy(&vy, &a, 4), memcpy(&sp, &b, 4);
    if (w->wear[0]) {
        if (g_debug > 0 && w->was_ground && !ground && vy > 0.5f) LOG("[costume] player %d takes off (vy %.2f)\n", (int)(w - g_p), vy);
        if (!w->was_ground && ground) sound(w->wear, "ground");
        uint64_t in = g64(player + P_INPUT);
        bool still = ground && sp > -0.01f && sp < 0.01f;
        if (still && in && g8(in + 0x10) && !g_goal) {
            if (++w->up == 8) {
                Group *g = group_of(g64(player + P_HOLDER));
                w->taunt = TAUNT_FRAMES;
                if (g) set_look(g, w->wear, true);
                sound(w->wear, "appeal");
                if (g_debug > 0) LOG("[costume] player %d taunts (%s)\n", (int)(w - g_p), w->wear);
            }
        } else
            w->up = 0;
        if (w->taunt && (!still || --w->taunt == 0)) taunt_end(w);
    }
    w->was_ground = ground;
}

static void give_now(Cpu *c, Wearer *w, uint64_t player);
void costume_test_frame(void);
void costume_player_frame(Cpu *c, uint64_t player) {
    extern volatile uint64_t g_costume_last_update;
    g_costume_last_update = renderer_present_count();
    if (!costumes_on()) return;
    costume_test_frame();
    if (!g_course[0] || !smb1_course()) return;
    int i = player_index(player);
    if (i < 0) return;
    Wearer *w = &g_p[i];
    if (w->player != player) {  // (a new course, or a restart: the player starts small, or in RECOMP_COSTUME's)
        bool asked = w->asked;
        char want[48];
        memcpy(want, w->want, sizeof want);
        memset(w, 0, sizeof *w);
        w->player = player, w->asked = asked;
        memcpy(w->want, want, sizeof want);
        if (i == 0) g_goal = false;
        if (g_forced[0]) wear(w, g_forced, "RECOMP_COSTUME");
    }
    uint32_t form = g32(player + P_FORM), pending = g32(player + P_PENDING);
    static uint32_t last_form[4] = {~0u, ~0u, ~0u, ~0u}, last_pending[4];
    if (g_debug > 0 && (form != last_form[i] || pending != last_pending[i]))
        LOG("[costume] player %d form %u pending %u%s%s\n", i, form, pending, w->wear[0] ? " in " : "", w->wear);
    last_form[i] = form, last_pending[i] = pending;
    if (w->wear[0] && form != 0 && pending != 0) wear(w, NULL, "another power-up");  // (Super -> costume: form 1
                                                                                       // until the change is done)
    if (w->losing && --w->losing == 0 && g_debug > 0) LOG("[costume] player %d: the costume loss ends (small Mario)\n", i);
    if (w->lost > 0) w->lost--;
    if (w->asked) w->asked = false, give_now(c, w, player);
    show(w);
    motion(w, player);
}

// ---------------------------------------------------------------- hook points (games/smm2/mods/hooks.c)
// true when handled
bool costume_after(Cpu *c, uint64_t addr) {
    if (addr - MAIN_ADDR(0) == TRANSFORM_REQ) {  // a power-up asks for a form: the Mystery item dresses small, Super
        uint64_t player = c->x[0], target = c->x[1];  // or Fire Mario
        uint32_t form = g32(player + P_FORM), want = g32(target);
        if (g_debug > 0) LOG("[costume] change request: player %#llx form %u -> %u\n", (unsigned long long)player, form, want);
        int i = player_index(player);
        if (!costumes_on() || !g_course[0] || !smb1_course() || i < 0 || form > 2 || want != (uint32_t)g_item) {
            g_giving = NULL;
            return true;
        }
        Wearer *w = &g_p[i];
        if (w->player != player) return true;
        const char *res = g_giving ? g_giving : g_pickup[0] ? g_pickup : draw(w->wear[0] ? w->wear : NULL);
        g_giving = NULL;
        if (!res) return true;  // (no costume owned yet: the item stays the game's)
        s32(target, 0);  // (a change to small: from small nothing to see, from Super / Fire the shrink)
        wear(w, res, form ? "a Mystery Mushroom as Super / Fire Mario" : "a Mystery Mushroom");
        sound(w->wear, "powerup");
        return true;
    }
    if (addr - MAIN_ADDR(0) == JUMP_SOUND_SILENT || addr - MAIN_ADDR(0) == STATE_JUMP_KEY) {  // a jump sound
        uint64_t player = addr - MAIN_ADDR(0) == JUMP_SOUND_SILENT ? c->x[0] : c->x[19];
        int i = player_index(player);
        Wearer *w = i >= 0 ? &g_p[i] : NULL;
        // (a costume without a jump sound keeps SMM2's: SMM1 falls back to the normal sound)
        if (!costumes_on() || !w || w->player != player || !w->wear[0] || !smb1_course() || !has_sound(w->wear, "jump"))
            return true;
        bool state = addr - MAIN_ADDR(0) == STATE_JUMP_KEY;
        uint32_t kind = g32(player + P_JUMP_KIND);
        // (only where SMM2 plays a sound in the SMB1 style: a request not silent, "jump" or "voice_jump_high"; the
        // others are left to the game, jump_high is not SMB1's)
        if (!state && ((uint8_t)c->x[8] || (kind != 1 && kind != 4))) return true;
        if (g_debug > 0) LOG("[costume] player %d: SMM2's jump sound muted (%s, kind %u)\n", i, state ? "state 0x71015cfe70" : "jump", kind);
        if (state) {
            static const uint64_t no_sound[4];  // (a sound user without its instance: the play call returns an empty handle)
            c->x[0] = (uint64_t)(uintptr_t)no_sound;
        } else
            c->x[8] = 1;  // (silent: no sound, no sound properties)
        sound(w->wear, (rnd() & 1) && has_sound(w->wear, "jump2") ? "jump2" : "jump");  // (jump2 half the time, SMM1)
        return true;
    }
    if (addr - MAIN_ADDR(0) != DAMAGE_FORM_LOAD) return false;
    uint64_t player = c->x[19];
    int i = player_index(player);
    Wearer *w = i >= 0 ? &g_p[i] : NULL;
    if (costumes_on() && w && w->player == player && w->wear[0] && (uint32_t)c->x[8] == 0) {  // a hit in costume:
        c->x[8] = 1;                                                                            // Super -> small
        LOG("[costume] hit: player %d's %s comes off\n", i, w->wear);
        wear(w, NULL, "a hit");
        w->losing = LOSS_FRAMES, w->lost = LOST_FRAMES;  // (SMM1's costume -> small, from this update: the holders are
        show(w);                                         // set now, as the power-down never calls the slot setter)
    }
    return true;
}

// (a costume without its own goal / down sound leaves SMM2's fanfare: SMM1 falls back to the normal one)
static void goal_fanfare(Cpu *c) {
    const char *res = g_p[0].wear;
    bool mine = costumes_on() && res[0] && (uint32_t)c->x[2] == 6 && has_sound(res, "goal");
    if ((uint32_t)c->x[2] == 6) g_goal = true;
    if (mine) c->x[2] = 0;  // (the course music only fades out)
    guest_call_orig(c, GOAL_FANFARE);
    if (mine) sound(res, "goal");
}
static void down_fanfare(Cpu *c) {
    extern void quick_retry_miss(void);  // (mods/quick_retry.c: the miss is fast-forwarded when it's on)
    extern void endless_miss_demo(void);  // (mods/endless.c: a skip from now on costs the life)
    if (getenv("RECOMP_MUSIC_LOG"))  // (tests: every course music state change, with the presented frame)
        LOG("[music] state %u at frame %llu\n", (uint32_t)c->x[2], (unsigned long long)renderer_present_count());
    if ((uint32_t)c->x[2] == 7) quick_retry_miss(), endless_miss_demo();
    char res[48];
    snprintf(res, sizeof res, "%s", g_p[0].wear);
    bool worn = costumes_on() && res[0] && (uint32_t)c->x[2] == 7, mine = worn && has_sound(res, "down");
    extern bool smm1_long_down_fanfare(void);  // (smm1_extras.c: the whole fanfare through the host)
    if (mine || ((uint32_t)c->x[2] == 7 && smm1_long_down_fanfare())) c->x[2] = 0;
    guest_call_orig(c, DOWN_FANFARE);
    if (mine) sound(res, "down");
    if (worn)
        for (int i = 0; i < 4; i++) g_p[i].wear[0] = 0, g_p[i].taunt = 0;
}

// ---------------------------------------------------------------- the overlay menu's "Mystery Mushroom costume now"
volatile uint64_t g_costume_last_update;  // presented frame of the last player update
static const char *g_choice[256];          // the row's costumes: SMM1's English names, by name
static char g_choice_res[256][48];
static volatile int g_nchoices;
static GameModTrigger *g_row;
// (the game's thread, at a course's first update: SMM1's names read once)
static void fill_choices(void) {
    if (g_nchoices) return;
    costume_list();
    int n = costume_count() < 256 ? costume_count() : 256;
    static bool said;
    if (!n && !said) {
        said = true;
        LOG("[costume] the SMM1 files have no costume list (Pack/Static.pack): costumes need the SMM1 dump's Static, "
            "Model and Sound/CharaMario files (a copy made with an older tools/smm1_subset.sh lacks them: copy it again)\n");
    }
    static char names[256][64];
    int order[256];
    for (int i = 0; i < n; i++) {
        costume_name(costume_res(i), names[i], sizeof names[i], NULL, 0);
        order[i] = i;
    }
    for (int i = 1; i < n; i++)  // (by name)
        for (int k = i; k > 0 && strcasecmp(names[order[k - 1]], names[order[k]]) > 0; k--) {
            int t = order[k];
            order[k] = order[k - 1], order[k - 1] = t;
        }
    for (int i = 0; i < n; i++) {
        g_choice[i] = names[order[i]];
        snprintf(g_choice_res[i], 48, "%s", costume_res(order[i]));
    }
    if (g_row) g_row->choices = g_choice;
    __atomic_store_n(&g_nchoices, n, __ATOMIC_RELEASE);
    if (g_row) g_row->nchoices = n;
    if (g_debug > 0) LOG("[costume] %d costumes in Try it now's row\n", n);
}

bool costume_now_available(void) {
    if (!costumes_on() || !g_nchoices || !g_course[0] || !smb1_course() || bridge_endless_course()) return false;
    if (renderer_present_count() - g_costume_last_update > 30) return false;  // (no course being played)
    uint64_t pm = g64(PLAYERS), p = pm ? g64(pm + 0x28) : 0;
    return p && g_p[0].player == p && g32(p + P_FORM) <= 2;  // (small, Super or Fire: as the Mystery Mushroom)
}
// why the row is greyed out, in the order a player would fix it (a few words: it sits beside the label)
static const char *costume_now_why(void) {
    if (!costumes_on()) return "Turn on costumes";
    if (renderer_present_count() - g_costume_last_update > 30) return "Only in a course";
    if (bridge_endless_course()) return "Not in runs";
    if (!smb1_course()) return "SMB1 courses only";
    if (!g_nchoices) return "No costumes in SMM1 files";
    if (!g_course[0]) return "No costume model";
    uint64_t pm = g64(PLAYERS), p = pm ? g64(pm + 0x28) : 0;
    if (p && g32(p + P_FORM) > 2) return "Small/Super/Fire only";
    return "";
}
void costume_now_trigger(int choice) {
    if (!costume_now_available() || choice < 0 || choice >= g_nchoices) {
        LOG("[costume] Mystery Mushroom costume now: not available\n");
        return;
    }
    snprintf(g_p[0].want, sizeof g_p[0].want, "%s", g_choice_res[choice]);
    g_p[0].asked = true;
}
// (the game's thread, from the player's update: the game's own form change request, as the pickup makes it)
static void give_now(Cpu *c, Wearer *w, uint64_t player) {
    uint32_t form = g32(player + P_FORM);
    if (form > 2) {
        LOG("[costume] Mystery Mushroom costume now: player %d has another power-up (form %u)\n", (int)(w - g_p), form);
        return;
    }
    static char res[48];
    snprintf(res, sizeof res, "%s", w->want);
    uint64_t sp = c->sp;
    c->sp -= 0x40;
    uint64_t arg = c->sp + 0x20;
    s32(arg, (uint32_t)g_item);
    g_giving = res;
    int r = (int)(uint32_t)pm_call(c, FORM_REQUEST, player, arg, 0, 0);
    g_giving = NULL;
    c->sp = sp;
    if (strcmp(w->wear, res)) LOG("[costume] Mystery Mushroom costume now: player %d can't change now (%d)\n", (int)(w - g_p), r);
    else if (g_debug > 0) LOG("[costume] Mystery Mushroom costume now: %s (request %d)\n", res, r);
}

__attribute__((constructor)) static void costume_register(void) {
    hle_override(GOAL_FANFARE, goal_fanfare, "costume: goal fanfare");
    hle_override(DOWN_FANFARE, down_fanfare, "costume: death fanfare");
    hle_override(HOLDER_PLACE, holder_place, "costume: the holder's placement (a costume turns)");
    static const GameModTrigger t = {
        "Mystery Mushroom costume now",
        "Player 1 takes a Mystery Mushroom with the costume chosen above (as small, Super or Fire Mario) in the Super "
        "Mario Bros. style course being played; any costume, owned or not (it isn't added to yours). Needs the Mystery "
        "Mushroom costumes switch; not in Endless or 100 Mario runs.",
        costume_now_available, costume_now_trigger, "Costume", NULL, 0, costume_now_why};
    g_row = mod_trigger_add(&t);
}

// the trigger pressed by a test (RECOMP_COSTUME_TRY=<presented frame>:<res>), and the row's names, from the player's
// update (the game's thread)
void costume_test_frame(void) {
    if (!costumes_on()) return;
    fill_choices();
    static long at = -2;
    static char res[48];
    if (at == -2) {
        const char *e = getenv("RECOMP_COSTUME_TRY");
        at = e ? atol(e) : -1;
        const char *colon = e ? strchr(e, ':') : NULL;
        snprintf(res, sizeof res, "%s", colon ? colon + 1 : "");
    }
    if (at <= 0 || renderer_present_count() < (uint64_t)at) return;
    at = -1;
    int k = -1;
    for (int i = 0; i < g_nchoices; i++)
        if (!strcmp(g_choice_res[i], res)) k = i;
    LOG("[costume] RECOMP_COSTUME_TRY: Mystery Mushroom costume now (%s) is %s\n", res, costume_now_available() ? "available" : "greyed out");
    costume_now_trigger(k);
}
