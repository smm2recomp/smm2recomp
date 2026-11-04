// The colour filter over the game's image (color_filter.h): set by a mod's thread, read by the present pass.
#include <pthread.h>
#include <string.h>

#include "color_filter.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static float g_m[12];
static bool g_on;

void renderer_set_color_filter(const float m[12]) {
    pthread_mutex_lock(&g_lock);
    g_on = m != NULL;
    if (m) memcpy(g_m, m, sizeof g_m);
    pthread_mutex_unlock(&g_lock);
}

bool renderer_color_filter(float m[12]) {
    pthread_mutex_lock(&g_lock);
    bool on = g_on;
    if (on) memcpy(m, g_m, sizeof g_m);
    pthread_mutex_unlock(&g_lock);
    return on;
}
