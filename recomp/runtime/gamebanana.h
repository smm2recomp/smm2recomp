// GameBanana in the Mods tab (gamebanana.c): the game's mods there, most downloaded first (or a search), each one's
// details and files, and installing one: downloaded onto the player's machine, unpacked (runtime/archive.h) and put
// in mods/ (runtime/usermods.h) with its author, licence and page. Uses GameBanana's public API
// (https://gamebanana.com/apiv11); all on a background thread, so the menu never waits. A game without a GameBanana
// id (game_gamebanana_id(): 0) has none of it.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define GB_MAX_FILES 8
#define GB_MAX_SHOTS 6

typedef struct {
    int id;
    char name[128], author[96], category[64];
    // details (gb_details): loaded once asked for
    bool details;
    int downloads, likes, views, thanks, posts, subscribers;
    int64_t added, modified;  // unix time
    bool obsolete, ported;
    char license[96], version[32], text[3000];
    char video[128], tags[200], credits[800], rights[400];
    int nshots;  // screenshots (gb_image)
    char shot[GB_MAX_SHOTS][128];
    int nfiles;
    int file_id[GB_MAX_FILES];
    char file_name[GB_MAX_FILES][128], file_desc[GB_MAX_FILES][128];
    uint64_t file_size[GB_MAX_FILES];
    int file_downloads[GB_MAX_FILES];
    int64_t file_date[GB_MAX_FILES];
} GbMod;

typedef struct { int id, count; char name[48]; } GbCategory;
#define GB_NSORTS 5
extern const char *const gb_sort_names[GB_NSORTS];  // "Most downloaded", "Most liked", ...

typedef enum { GB_IDLE, GB_BUSY, GB_ERROR } GbState;

int game_gamebanana_id(void);       // the game's id there (runtime/game_hooks.c: 0, none)
bool gb_available(void);
void gb_refresh(const char *search);  // the list again: the most downloaded (search NULL / ""), or a search
void gb_set_filter(int sort, int category);  // the list's order (0..GB_NSORTS-1) and a top-level category id (0: all);
                                              // they apply to the browse list, not to a search (the next gb_refresh)
int gb_categories(GbCategory *out, int max);  // the game's categories (fetched with the first refresh; 0 until then)
GbState gb_list_state(char *msg, int cap);
int gb_list(GbMod *out, int max);   // what the last refresh found
void gb_details(int id);            // fetch one's details (then gb_get shows them)
bool gb_get(int id, GbMod *out);
// Install file fi of mod id (on when enable). One at a time; gb_install_state says how it goes.
void gb_install(int id, int fi, bool enable);
GbState gb_install_state(int *id, float *progress, char *msg, int cap);  // progress 0..1 (-1: unknown)
void gb_install_cancel(void);
// Screenshot idx (0..nshots-1) of mod id as RGBA8 (*w x *h), fetched in the background: NULL until it is there
// (the first call for one starts it; *w is -1 when it failed). The pixels stay valid.
const uint8_t *gb_image(int id, int idx, int *w, int *h);
