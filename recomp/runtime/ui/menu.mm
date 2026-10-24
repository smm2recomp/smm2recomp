// In-game overlay menu built with Dear ImGui (Metal renderer on macOS, Vulkan elsewhere; input fed from SDL events).
//
// Threads: SDL events arrive on the main thread and are queued; the menu is laid out and drawn on the
// render thread inside backend_present, after the game image, into the same drawable.
#ifdef __APPLE__
#import <Metal/Metal.h>
#endif
#include <SDL3/SDL.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#include "../game.h"
#include <math.h>
#include <pthread.h>
#include <atomic>
#include <vector>

#include "imgui.h"
#include "option_rows.h"
#include "imgui_internal.h"  // (NavCursorVisible)
#include "osk.h"
#ifdef __APPLE__
#include "imgui_impl_metal.h"
#else
#include "imgui_impl_vulkan.h"
extern "C" bool vk_imgui_backend_init(void);  // runtime/nvn/vulkan/imgui_glue.cpp
#endif

extern "C" {
#include "../game_rows.h"
#include "../hle.h"
#include "../settings.h"
#include "menu.h"
void input_reset(void);
void controls_bindings_draw(float scale);  // controls_ui.mm
#include "../input_bind.h"
#include "../paths.h"
#include "../platform.h"
#include "../nvn/framegen.h"
#include "system_font.h"
}

// ------------------------------------------------------------------ state shared with the main thread
static std::atomic<bool> g_open{false};
static std::atomic<bool> g_text_busy{false};  // a text field / the on-screen keyboard has the keys (render thread: ui_text_busy)
static std::atomic<bool> g_want_fullscreen_apply{false};

struct QEvent {
    enum { KEY, ANALOG, MOUSE_POS, MOUSE_BUTTON, WHEEL, TEXT } kind;
    ImGuiKey key;
    bool down;
    float value, x, y;
    int button;
    bool touch;  // (mouse events SDL made up from a finger: SDL_TOUCH_MOUSEID)
    char text[32];
};
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static std::vector<QEvent> q_events;
static float g_pixel_density = 1;

static void push(const QEvent &e) {
    pthread_mutex_lock(&q_lock);
    q_events.push_back(e);
    pthread_mutex_unlock(&q_lock);
}

bool menu_is_open(void) { return g_open.load(); }
static void set_open(bool open);
extern "C" void menu_test_open(void) {
    set_open(true);
    if (getenv("LARECOMP_MENU_TAB")) { extern int g_tab_request_ext; g_tab_request_ext = atoi(getenv("LARECOMP_MENU_TAB")); }
}

void menu_tabs_focus(bool first_tab);
static void set_open(bool open) {
    if (open == g_open.load()) return;
    g_open.store(open);
    input_reset();  // nothing held while the menu had focus leaks into the game
    bind_capture_cancel();
    if (open) {
        static bool opened;  // the controller's cursor: the first tab (Controls) the first time this session, later the
        menu_tabs_focus(!opened);  // selected one
        opened = true;
    }
    // (game_mod_rows_hide: a game's secret settings rows go back to "set" / "not set")
    else { settings_save(), osk_cancel(), game_mod_rows_hide(); g_text_busy = false; }
}

static ImGuiKey key_from_scancode(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
    case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
    case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
    case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
    case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
    case SDL_SCANCODE_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
    case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
    case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
    case SDL_SCANCODE_DELETE: return ImGuiKey_Delete;
    case SDL_SCANCODE_HOME: return ImGuiKey_Home;
    case SDL_SCANCODE_END: return ImGuiKey_End;
    case SDL_SCANCODE_PAGEUP: return ImGuiKey_PageUp;
    case SDL_SCANCODE_PAGEDOWN: return ImGuiKey_PageDown;
    case SDL_SCANCODE_LSHIFT: return ImGuiKey_LeftShift;
    case SDL_SCANCODE_RSHIFT: return ImGuiKey_RightShift;
    case SDL_SCANCODE_LCTRL: return ImGuiKey_LeftCtrl;
    case SDL_SCANCODE_RCTRL: return ImGuiKey_RightCtrl;
    case SDL_SCANCODE_LGUI: return ImGuiKey_LeftSuper;
    case SDL_SCANCODE_RGUI: return ImGuiKey_RightSuper;
    case SDL_SCANCODE_A: return ImGuiKey_A;
    case SDL_SCANCODE_C: return ImGuiKey_C;
    case SDL_SCANCODE_V: return ImGuiKey_V;
    case SDL_SCANCODE_X: return ImGuiKey_X;
    case SDL_SCANCODE_Z: return ImGuiKey_Z;
    case SDL_SCANCODE_Q: return ImGuiKey_Q;  // previous tab
    case SDL_SCANCODE_E: return ImGuiKey_E;  // next tab
    default: return ImGuiKey_None;
    }
}

// Nintendo layout: the right face button (A) confirms, the bottom one (B) goes back.
static ImGuiKey key_from_pad(Uint8 b) {
    switch (b) {
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceDown;   // activate
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceRight; // cancel / back
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

// ---- software keyboard (runtime/hle/swkbd.c: the game's name entry etc.) --------------------------------------
// The game thread waits in menu_keyboard_run (re-presenting the last frame); this dialog takes the text: typed
// directly when the player last used a keyboard, an on-screen keyboard (d-pad + A, B deletes, Start / + OK) when
// they last used a controller.
static std::atomic<int> g_kbd{0};  // 1 open, 2 confirmed, 3 cancelled
static std::atomic<bool> g_last_pad{false};
static char g_kbd_text[400];  // (a comment: 75 characters of up to 4 bytes)
static char g_kbd_title[200];  // the game's header or guide text, if it gives one
static int g_kbd_max = 8;
static bool capturing() { return g_open.load() || g_kbd.load() == 1; }
// The on-screen controller's menu button (runtime/ui/touchpad.mm)
extern "C" void menu_toggle(void) { set_open(!g_open.load()); }

// A notice over the game for a few seconds (bottom centre), from any thread: menu_notice(text, seconds)
static pthread_mutex_t notice_lock = PTHREAD_MUTEX_INITIALIZER;
static char notice_text[512];
static double notice_until;  // SDL_GetTicks seconds
extern "C" void menu_notice(const char *text, double seconds) {
    pthread_mutex_lock(&notice_lock);
    snprintf(notice_text, sizeof notice_text, "%s", text);
    notice_until = SDL_GetTicks() / 1000.0 + seconds;
    pthread_mutex_unlock(&notice_lock);
}
static bool notice_active(void) { return SDL_GetTicks() / 1000.0 < notice_until; }
extern "C" bool touchpad_visible(void);
extern "C" void touchpad_draw(float w, float h);
// The game gets no input while this is true (runtime/hle/input.c): menu or name entry up.
extern "C" bool menu_input_captured(void) { return capturing(); }

bool menu_handle_event(const SDL_Event *e) {
    static bool back_down, start_down;
    if (g_open.load() && bind_capture_event(e)) return true;  // Controls tab waiting for a new binding: it takes the press
    if (e->type == SDL_EVENT_KEY_DOWN) g_last_pad = false;
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) g_last_pad = true;
    if (g_kbd.load() == 1 && e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {  // Enter confirms, Esc cancels
        if (e->key.scancode == SDL_SCANCODE_RETURN || e->key.scancode == SDL_SCANCODE_KP_ENTER) { g_kbd = 2; return true; }
        if (e->key.scancode == SDL_SCANCODE_ESCAPE) { g_kbd = 3; return true; }
    }
    if (g_kbd.load() == 1 && e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e->gbutton.button == SDL_GAMEPAD_BUTTON_START) {
        g_kbd = 2;
        return true;
    }
    // typing (a text field, the on-screen keyboard, the name entry): keys are text, not menu shortcuts
    bool typing = g_kbd.load() == 1 || (g_open.load() && g_text_busy.load());
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        if (!e->key.repeat && (e->key.scancode == SDL_SCANCODE_F1 ||
                               ((e->key.scancode == SDL_SCANCODE_ESCAPE || e->key.scancode == SDL_SCANCODE_AC_BACK) &&
                                !g_open.load()))) {  // (AC_BACK: Android's Back)
            if (!typing) set_open(!g_open.load());  // (Esc with the menu open goes to ImGui below: it cancels a field)
            return true;
        }
        if (!e->key.repeat && (e->key.scancode == SDL_SCANCODE_F11 ||
                               (e->key.scancode == SDL_SCANCODE_RETURN && (e->key.mod & (SDL_KMOD_ALT | SDL_KMOD_GUI))) ||
                               (e->key.scancode == SDL_SCANCODE_F && (e->key.mod & SDL_KMOD_GUI) && (e->key.mod & SDL_KMOD_CTRL)))) {
            g_settings.fullscreen = !g_settings.fullscreen;
            g_want_fullscreen_apply.store(true);
            settings_save();
            return true;
        }
        /* fallthrough */
    case SDL_EVENT_KEY_UP: {
        if (!capturing()) return false;
        bool down = e->type == SDL_EVENT_KEY_DOWN;
        if (down && (e->key.scancode == SDL_SCANCODE_ESCAPE || e->key.scancode == SDL_SCANCODE_AC_BACK)) {  // Esc: back out of popups, else close
            QEvent q = {};
            q.kind = QEvent::KEY; q.key = ImGuiKey_Escape; q.down = true; push(q);
            q.down = false; push(q);
            return true;
        }
        ImGuiKey k = key_from_scancode(e->key.scancode);
        if (k != ImGuiKey_None) { QEvent q = {}; q.kind = QEvent::KEY; q.key = k; q.down = down; push(q); }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::TEXT; strlcpy(q.text, e->text.text, sizeof q.text); push(q); }
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        bool down = e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_BACK) back_down = down;
        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_START) start_down = down;
        if (down && !typing && ((back_down && start_down) || e->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE)) {
            set_open(!g_open.load());
            return true;
        }
        if (!capturing()) return false;
        ImGuiKey k = key_from_pad(e->gbutton.button);
        if (k != ImGuiKey_None) { QEvent q = {}; q.kind = QEvent::KEY; q.key = k; q.down = down; push(q); }
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (!capturing()) return false;
        float v = e->gaxis.value / 32767.0f;
        QEvent q = {};
        q.kind = QEvent::ANALOG;
        switch (e->gaxis.axis) {
        case SDL_GAMEPAD_AXIS_LEFTX:
            q.key = ImGuiKey_GamepadLStickLeft; q.value = v < -0.3f ? -v : 0; push(q);
            q.key = ImGuiKey_GamepadLStickRight; q.value = v > 0.3f ? v : 0; push(q);
            break;
        case SDL_GAMEPAD_AXIS_LEFTY:
            q.key = ImGuiKey_GamepadLStickUp; q.value = v < -0.3f ? -v : 0; push(q);
            q.key = ImGuiKey_GamepadLStickDown; q.value = v > 0.3f ? v : 0; push(q);
            break;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: q.key = ImGuiKey_GamepadL2; q.value = v; push(q); break;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: q.key = ImGuiKey_GamepadR2; q.value = v; push(q); break;
        default: break;
        }
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if (!capturing()) return false;
        {
            QEvent q = {};
            q.kind = QEvent::MOUSE_POS; q.x = e->motion.x * g_pixel_density; q.y = e->motion.y * g_pixel_density;
            q.touch = e->motion.which == SDL_TOUCH_MOUSEID;
            push(q);
        }
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        if (!capturing()) return false;
        {
            QEvent q = {};
            q.kind = QEvent::MOUSE_BUTTON;
            q.button = e->button.button == SDL_BUTTON_LEFT ? 0 : e->button.button == SDL_BUTTON_RIGHT ? 1 : 2;
            q.down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            q.touch = e->button.which == SDL_TOUCH_MOUSEID;
            push(q);
        }
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::WHEEL; q.x = e->wheel.x; q.y = e->wheel.y; q.touch = e->wheel.which == SDL_TOUCH_MOUSEID; push(q); }
        return true;
    default:
        return false;
    }
}

void menu_apply_window_requests(void *win) {
    SDL_Window *w = (SDL_Window *)win;
    g_pixel_density = SDL_GetWindowPixelDensity(w);
    static bool text_input;
#if (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE) || defined(__ANDROID__)
    // (iOS, Android: text input brings up the system keyboard - only for a text field or the name entry, not the whole
    // menu)
    bool want_text = (g_open.load() && g_text_busy.load()) || (g_kbd.load() == 1 && !g_last_pad.load());
#else
    bool want_text = capturing();
#endif
    if (want_text != text_input) {
        if (want_text) SDL_StartTextInput(w);
        else SDL_StopTextInput(w);
        text_input = want_text;
    }
    static bool first = true;
    if (first || g_want_fullscreen_apply.exchange(false)) {
        first = false;
#if !(defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE) && !defined(__ANDROID__)  // (iOS, Android: always the whole screen)
        SDL_SetWindowFullscreen(w, g_settings.fullscreen);
#endif
    }
}

// ------------------------------------------------------------------ render thread
static bool g_inited;
static ImGuiContext *g_ctx;  // ours: the start screen (back to it after a reboot) has its own context
static ImFont *g_font, *g_font_big;
static ImFont *g_menu_font, *g_menu_font_big;  // the menu window (Baloo 2); everything else keeps g_font
static int g_tab, g_tab_request = -2;  // (-2: no request; -1 is the Prelaunch tab)
static int g_focus_tab = -99;  // put the nav cursor on this tab next frame (-100: the selected one; -99: none)
static int g_focus_frames;
// The fonts are rasterised at the output's size (up to twice the 1080 p sizes), so text on an iPad or a 4K screen is
// as sharp as at 1080 p rather than a scaled-up 1080 p atlas
static float g_atlas_scale = 1;

// The start screen's Settings opening: the controller's cursor starts on the first tab the first time in a session,
// later on the tab that's selected
void menu_tabs_focus(bool first_tab) { g_focus_tab = first_tab ? -1 : -100, g_focus_frames = 0; }
int g_tab_request_ext = -1;

static void style_init(void) {
    ImGuiStyle &s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowRounding = 14; s.FrameRounding = 8; s.GrabRounding = 8; s.TabRounding = 8; s.ChildRounding = 10;
    s.WindowPadding = ImVec2(28, 24); s.FramePadding = ImVec2(14, 8); s.ItemSpacing = ImVec2(14, 12);
    s.WindowBorderSize = 0; s.ScrollbarSize = 18;
    ImVec4 *c = s.Colors;
    ImVec4 gold(0.93f, 0.76f, 0.33f, 1), gold_dim(0.62f, 0.49f, 0.20f, 1), green(0.15f, 0.36f, 0.25f, 1);
    c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.09f, 0.08f, 0.94f);
    c[ImGuiCol_ChildBg] = ImVec4(0.09f, 0.13f, 0.11f, 0.85f);
    c[ImGuiCol_PopupBg] = ImVec4(0.07f, 0.10f, 0.09f, 0.98f);
    c[ImGuiCol_Text] = ImVec4(0.96f, 0.94f, 0.88f, 1);
    c[ImGuiCol_TextDisabled] = ImVec4(0.62f, 0.64f, 0.58f, 1);
    c[ImGuiCol_FrameBg] = ImVec4(0.13f, 0.19f, 0.16f, 1);
    c[ImGuiCol_FrameBgHovered] = green;
    c[ImGuiCol_FrameBgActive] = ImVec4(0.19f, 0.45f, 0.31f, 1);
    c[ImGuiCol_Button] = ImVec4(0.14f, 0.22f, 0.18f, 1);
    c[ImGuiCol_ButtonHovered] = green;
    c[ImGuiCol_ButtonActive] = ImVec4(0.21f, 0.50f, 0.34f, 1);
    c[ImGuiCol_Header] = green;
    c[ImGuiCol_HeaderHovered] = ImVec4(0.19f, 0.45f, 0.31f, 1);
    c[ImGuiCol_HeaderActive] = ImVec4(0.21f, 0.50f, 0.34f, 1);
    c[ImGuiCol_Tab] = ImVec4(0.12f, 0.18f, 0.15f, 1);
    c[ImGuiCol_TabHovered] = green;
    c[ImGuiCol_TabSelected] = ImVec4(0.21f, 0.44f, 0.31f, 1);
    c[ImGuiCol_TabSelectedOverline] = gold;
    c[ImGuiCol_CheckMark] = gold;
    c[ImGuiCol_SliderGrab] = gold_dim;
    c[ImGuiCol_SliderGrabActive] = gold;
    c[ImGuiCol_NavCursor] = gold;
    c[ImGuiCol_Separator] = ImVec4(0.30f, 0.36f, 0.30f, 1);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_WindowBg];
}


#ifdef __APPLE__
static void init(id<MTLDevice> dev) {
#else
static void init(void) {
#endif
    IMGUI_CHECKVERSION();
    g_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.IniFilename = NULL;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    char sys[1024];
    int face = 0;
    if (ui_system_font(sys, sizeof sys, &face)) {  // (system_font.mm: an existing file; a missing one asserts in ImGui)
        cfg.FontNo = face;
        g_font = io.Fonts->AddFontFromFileTTF(sys, 34 * g_atlas_scale, &cfg);
        if (g_font) g_font_big = io.Fonts->AddFontFromFileTTF(sys, 56 * g_atlas_scale, &cfg);
        LOG("[menu] font %s\n", sys);
    }
    // the menu window itself: Baloo 2 SemiBold, as on the start screen (shipped, OFL), at its Settings window's sizes; the
    // FPS counter, overlays and on-screen keyboard keep the fonts above
    char baloo[1024];
    if (const char *f = getenv("LARECOMP_MENU_FONT")) strlcpy(baloo, f, sizeof baloo);  // (test) another weight / face
    if (getenv("LARECOMP_MENU_FONT") || rt_resource("Baloo2-SemiBold.ttf", "third_party/fonts/Baloo2/Baloo2-SemiBold.ttf", baloo, sizeof baloo)) {
        g_menu_font = io.Fonts->AddFontFromFileTTF(baloo, 39 * g_atlas_scale, &cfg);
        g_menu_font_big = io.Fonts->AddFontFromFileTTF(baloo, 72 * g_atlas_scale, &cfg);
    }
    if (!g_font) {  // (no font file: ImGui's own, at the menu's size rather than its 13 px)
        LOG("[menu] no system font: the built-in one\n");
        ImFontConfig d;
        d.SizePixels = 34 * g_atlas_scale;
        g_font = io.Fonts->AddFontDefault(&d);
    }
    if (!g_font_big) g_font_big = g_font;
    if (!g_menu_font || !g_menu_font_big) g_menu_font = g_font, g_menu_font_big = g_font_big;
    style_init();
#ifdef __APPLE__
    ImGui_ImplMetal_Init(dev);
#else
    vk_imgui_backend_init();
#endif
    g_inited = true;
}

// Touch: the last pointer input was a finger (SDL's mice made up from touches; the iPhone / iPad from the start). The
// menu then has bigger rows, tabs and scrollbars, and a drag scrolls the page.
static bool g_touch_ui =
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
    true;
#else
    false;
#endif

static void drain_events(ImGuiIO &io) {
    pthread_mutex_lock(&q_lock);
    std::vector<QEvent> ev;
    ev.swap(q_events);
    pthread_mutex_unlock(&q_lock);
    for (const QEvent &q : ev) {
        switch (q.kind) {
        case QEvent::KEY: io.AddKeyEvent(q.key, q.down); break;
        case QEvent::ANALOG: io.AddKeyAnalogEvent(q.key, q.value > 0.1f, q.value); break;
        // A finger is a touch screen to ImGui: its press comes a frame after the move (there is no hover before a
        // tap), so a single tap selects a tab or a row (as a mouse's press, it came in the same frame as the jump to
        // the new spot, and the first tap often only moved there: tabs needed a double tap on the iPad)
        case QEvent::MOUSE_POS:
            io.AddMouseSourceEvent(q.touch ? ImGuiMouseSource_TouchScreen : ImGuiMouseSource_Mouse);
            io.AddMousePosEvent(q.x, q.y);
            g_touch_ui = q.touch;
            break;
        case QEvent::MOUSE_BUTTON:
            io.AddMouseSourceEvent(q.touch ? ImGuiMouseSource_TouchScreen : ImGuiMouseSource_Mouse);
            io.AddMouseButtonEvent(q.button, q.down);
            g_touch_ui = q.touch;
            break;
        case QEvent::WHEEL:
            io.AddMouseSourceEvent(q.touch ? ImGuiMouseSource_TouchScreen : ImGuiMouseSource_Mouse);
            io.AddMouseWheelEvent(q.x, q.y);
            break;
        case QEvent::TEXT: io.AddInputCharactersUTF8(q.text); break;
        }
    }
}

// Section header in the big font.
static void header(const char *t) {
    ImGui::PushFont(g_font_big);
    ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "%s", t);
    ImGui::PopFont();
    ImGui::Separator();
    ImGui::Spacing();
}

static void hint(const char *t) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", t);
    ImGui::PopTextWrapPos();
}

// A row with a label on the left and a choice of options on the right (dpad left/right cycles).
// reserve: room kept free right of the combo (a button on the same line)
static bool choice(const char *label, int *value, const char *const *names, int n, float reserve = 0) {
    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.45f);
    bool changed = false;
    ImGui::SetNextItemWidth(reserve > 0 ? -reserve : -1);
    if (ImGui::BeginCombo("##c", names[*value])) {
        for (int i = 0; i < n; i++) {
            if (ImGui::Selectable(names[i], i == *value)) { *value = i; changed = true; }
            if (i == *value) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemFocused() && !ImGui::IsPopupOpen("##c")) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
            *value = (*value + 1) % n; changed = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
            *value = (*value + n - 1) % n; changed = true;
        }
    }
    ImGui::PopID();
    return changed;
}

extern "C" void input_pads_reassign(void);  // runtime/hle/input.c

static bool toggle(const char *label, bool *v) {
    const char *names[] = {"Off", "On"};
    int i = *v;
    bool ch = choice(label, &i, names, 2);
    *v = i;
    return ch;
}

extern "C" void renderer_get_scale(float *sx, float *sy);
extern "C" int input_pad_family(void);  // runtime/hle/input.c
extern "C" double output_scale_dynamic_factor(void);

static void tab_display(void) {
    float scale = ImGui::GetFontSize() / 34;
    opt_begin("##video", scale);
    opt_section("Display");
    const char *aspects[] = {"Unlocked", "Original 16:9"};
    opt_choice("Aspect ratio", &g_settings.aspect, aspects, 2, ASPECT_UNLOCKED,
               "Unlocked: the view fills the window, so wider (or taller) windows show more of the island - up to "
               "ultrawide; houses and dungeons keep their framing. Original 16:9: the game's own framing, with bars.");
    bool fs = g_settings.fullscreen;
    if (opt_toggle("Fullscreen", &fs, false, "Fills the screen. Also: F11, Alt+Enter or Cmd+Ctrl+F.")) {
        g_settings.fullscreen = fs;
        g_want_fullscreen_apply.store(true);
    }
    opt_toggle("Show FPS", &g_settings.show_fps, false, "Shows the frame rate in the top right corner.");
    opt_toggle("Show memory use", &g_settings.show_memory, false,
               "Shows how much memory the app uses in the top right corner, once a second (on iPad and iPhone also how "
               "much iOS still allows before it closes the app).");

    opt_section("Rendering");
    // display order -> RES_* (the ini / enum order predates the menu order)
    // (up to 4x: 7680 x 4320, 8K - more only ran out of GPU memory, and sat next to 75% in the list)
    // (Match display in the middle: left from it 75% / 50% / 25%, right Dynamic then 1x-4x; user 2026-10-11)
    static const char *res[] = {"25%", "50%", "75%", "Match display", "Dynamic", "1x", "2x", "3x", "4x"};
    static const int res_mode[] = {RES_Q25, RES_Q50, RES_Q75, RES_MATCH, RES_DYNAMIC, 1, 2, 3, 4};
    enum { RES_MATCH_AT = 3 };
    enum { NRES = sizeof res_mode / sizeof res_mode[0] };
    {
        int ri = RES_MATCH_AT;
        for (int i = 0; i < NRES; i++) if (res_mode[i] == g_settings.res_mode) ri = i;
        float sx, sy;
        renderer_get_scale(&sx, &sy);
        char d[400];
        int n = snprintf(d, sizeof d, "How sharp the 3D image is. Match display: your screen's own resolution. Dynamic: "
                                      "as sharp as your GPU keeps at full speed. 1x-4x: the Switch's 1080p times that. "
                                      "75%% / 50%% / 25%%: a share of the display, for slower machines.\n\nRendering at "
                                      "%.0f x %.0f.", 1920.0 * sx, 1080.0 * sy);
        if (g_settings.res_mode == RES_DYNAMIC)
            snprintf(d + n, sizeof d - n, " Dynamic: %.0f%% of the display right now.", output_scale_dynamic_factor() * 100);
        if (opt_choice("Internal resolution", &ri, res, NRES, RES_MATCH_AT, d, false)) g_settings.res_mode = res_mode[ri];
    }
    {   // each OS's own upscaler next to bilinear (UPSCALE_SHARP: MetalFX on macOS, AMD FSR 1 on Windows)
#ifdef __APPLE__
        static const char *ups[] = {"Bilinear", "MetalFX"};
        const char *ud = "How the image is scaled up to the window when it renders at a lower resolution. MetalFX: Apple's "
                         "upscaler, sharper. Bilinear: plain and soft.";
#else
        static const char *ups[] = {"Bilinear", "AMD FSR 1"};
        const char *ud = "How the image is scaled up to the window when it renders at a lower resolution. AMD FSR 1: "
                         "edge-aware upscaling and sharpening, sharper. Bilinear: plain and soft.";
#endif
        int ui = g_settings.upscaler == UPSCALE_BILINEAR ? 0 : 1;
        char udm[512];
        if (g_settings.res_mode == RES_MATCH) {  // (renders at the window's size: nothing to scale up)
            snprintf(udm, sizeof udm, "%s\n\nNot used at Match display: the game already renders at your display's resolution.", ud);
            ud = udm;
            opt_disable_next();
        }
        if (opt_choice("Upscaler", &ui, ups, 2, 1, ud)) g_settings.upscaler = ui ? UPSCALE_SHARP : UPSCALE_BILINEAR;
    }
    static const char *aa[] = {"Off", "SMAA"};
    opt_choice("Anti-aliasing", &g_settings.antialias, aa, 2, AA_OFF,
               "Smooths jagged edges of the game image before it is scaled to the window. SMAA: finds the edges and "
               "blends only along them, so textures stay sharp. Most useful at a lower internal resolution.");
    static const char *fg[] = {"Off", "120 fps"};
    opt_choice("Frame generation", &g_settings.frame_gen, fg, 2, FRAMEGEN_OFF,
               "Adds a generated frame between each pair of real frames for 120 fps on 120 Hz displays (ProMotion, "
               "high-refresh monitors). The game itself still runs at 60. Adds a little input lag; fast motion may show "
               "small artifacts. Does nothing on a 60 Hz display.");

    opt_section("Game");
    opt_toggle("Remove blur", &g_settings.blur_removal, false,
               "Turns off the tilt-shift blur at the top and bottom of the screen.");
    opt_toggle("Hide Hero Mode icon", &g_settings.hide_hero_icon, false,
               "Hides the Hero Mode emblem in the corner of the screen while playing a Hero Mode file.");
    opt_end();
}

static void tab_audio(void) {
    opt_begin("##audio", ImGui::GetFontSize() / 34);
    opt_section("Sound");
    float pct = roundf(g_settings.volume * 100);
    if (opt_slider("Volume", &pct, 0, 200, 5, 100, "%g%%",
                   "The game's volume. 100% is the game's own level; above it, louder."))
        g_settings.volume = pct / 100.0f;
    opt_toggle("Reverb", &g_settings.reverb, true, "The game's reverb effect.");
    opt_toggle("Movie audio", &g_settings.movie_audio, true,
               "Sound in the game's movies. Applies the next time a movie starts.");
    opt_end();
}

// Mods: the game's settings page - online backend, course source, each SMM1 feature (runtime/game_hooks.c
// game_mod_toggles; the tab shows only when a game has some). An entry without a value starts a section, titled by its
// label (desc unused): {"Online", NULL, NULL, false}. Entries before the first section go under "Mods".
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
    const char *(*why)(void);  // greyed out: a few words on why (shown on the row; NULL or "": none)
} GameModTrigger;  // (runtime/game_hooks.c)
extern "C" int game_mod_triggers(const GameModTrigger **out);
extern "C" bool launcher_has_settings(void);   // runtime/ui/launcher.mm: the Mods tab's app section (desktop)
extern "C" bool launcher_settings_rows(void);
extern "C" bool online_rows_available(void);   // runtime/ui/online_rows.mm: the Online section (backend, Nextendo account)
extern "C" void online_rows(void);

// A game's settings that are not switches (runtime/game_rows.h): sections, switches and text fields.
// (no menu_keyboard_run here: it blocks re-presenting frames from the game thread, and these rows are drawn during
// a present - opt_text has its own field, with the on-screen keyboard for a controller)
#define MAX_SHOWN_ROWS 16
static bool g_row_shown[MAX_SHOWN_ROWS];  // a secret row was activated: its field is drawn until the menu closes
extern "C" void game_mod_rows_hide(void) {
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
        changed |= opt_text(r[i].label, r[i].text, (size_t)r[i].cap, r[i].hint, r[i].desc);
    }
    if (changed) game_mod_rows_changed();
}

static void tab_mods(void) {
    const GameModToggle *t;
    int n = game_mod_toggles(&t);
    opt_begin("##mods", ImGui::GetFontSize() / 34);
    if (online_rows_available()) online_rows();  // (first: the backend decides what the rest talks to)
    game_rows();  // the game's own settings for that backend (OCW's domain and token), then its mods below
    if (n > 0 && t[0].value) opt_section("Mods");
    bool changed = false;
    for (int i = 0; i < n; i++) {
        if (!t[i].value) { opt_section(t[i].label); continue; }
        changed |= opt_toggle(t[i].label, t[i].value, t[i].def, t[i].desc);
    }
    if (changed) game_mod_toggles_changed();
    // effects to try now (game_mod_triggers): a choice row (e.g. which costume) above its button where it has choices
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
        if (why && *why) opt_action(tr[i].label, why, tr[i].desc, false);  // (greyed: the reason as its value)
        else if (opt_button(tr[i].label, tr[i].desc) && on && tr[i].trigger) tr[i].trigger(choice[i]);
    }
    launcher_settings_rows();  // the app's own: updates, game files
    opt_end();
}

// Quit: back to the start screen (in game: the game reboots to it) or out to the desktop. Each asks first (progress
// since the last save is lost); B / Esc or Cancel backs out.
extern "C" void rt_request_reboot(int kind);  // runtime/main.c (RT_REBOOT_TITLE = 2: the start screen)
static void tab_quit(float scale, bool in_game) {
    static int ask;  // 0: the two choices; 1: start screen?; 2: desktop?
    if (ask && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
        ask = 0;
    const char *lost = in_game ? "\n\nProgress since your last save is lost." : "";
    char d1[200], d2[200];
    snprintf(d1, sizeof d1, "Leaves the game and goes back to the start screen.%s", lost);
    snprintf(d2, sizeof d2, "Closes " GAME_TITLE ".%s", lost);
    opt_begin("##quit", scale);
    opt_section("Quit");
    if (!ask) {
        if (!in_game) opt_disable_next();  // (already on it)
        if (opt_action("Quit to Start Screen", "", in_game ? d1 : "You're on the start screen.")) ask = 1;
        if (opt_action("Quit to Desktop", "", d2)) ask = 2;
    } else {
        if (opt_action(ask == 1 ? "Yes, quit to Start Screen" : "Yes, quit to Desktop", "", ask == 1 ? d1 : d2)) {
            settings_save();
            if (ask == 1) {
                rt_request_reboot(2);
                set_open(false);
            } else {
                SDL_Event q = {};
                q.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&q);
            }
            ask = 0;
        }
        if (opt_action("Cancel", "", "Stays where you are.")) ask = 0;
    }
    opt_end();
}

static void tab_about(void) {
    header("Button prompts");
    {
        static const char *const fam[] = {"Nintendo", "Xbox", "PlayStation"};
        char autol[64];
        snprintf(autol, sizeof autol, "Auto (%s)", fam[input_pad_family() % 3]);
        const char *names[] = {autol, "Nintendo", "Xbox", "PlayStation"};
        choice("Button icons", &g_settings.prompts, names, 4);
    }
    ImGui::Spacing();
    header("System language");
    {
        // what the game is told the console's language is: its text follows this (runtime/settings.c)
        char autol[64];
        snprintf(autol, sizeof autol, "Auto (%s)", settings_language_name());
        const char *names[N_LANGUAGES];
        names[0] = autol;
        for (int i = 1; i < N_LANGUAGES; i++) names[i] = g_language_names[i];
        if (choice("Language", &g_settings.language, names, N_LANGUAGES)) settings_save();
    }
    hint("The game's text language, as its own system setting. Applies when the game restarts: Quit -> Title "
         "screen, then Play.");
    ImGui::Spacing();
    header("Controls");
    if (opt_toggle("Controllers start at player 2", &g_settings.pads_from_p2, false,
                   "The keyboard and the on-screen controls are player 1, and every controller is the next player "
                   "(player 2, 3, 4), for local multiplayer with a keyboard and one controller. Off: the first "
                   "controller is player 1's too.")) {
        settings_save();
        input_pads_reassign();
    }
    controls_bindings_draw(ImGui::GetFontSize() / 34);
    ImGui::Spacing();
    header("Menu");
    ImGui::TextUnformatted("Open / close this menu:  F1, Esc, the controller's Home / Xbox / PlayStation button, or Back + Start");
    ImGui::TextUnformatted("Switch tabs:  L / R  (Q / E)        Back:  B  (Esc)");
    ImGui::TextUnformatted("Fullscreen:  F11 / Alt+Enter");
}


// Reboot (runtime/main.c): the game is gone - menu and name dialog close; the ImGui context stays.
RT_ON_RESET(menu_reset) {
    set_open(false);
    g_kbd = 0;
    osk_cancel();
    pthread_mutex_lock(&q_lock);
    q_events.clear();
    pthread_mutex_unlock(&q_lock);
}

static void keyboard_draw(ImGuiIO &io, float scale) {
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 120));
    float w = fminf(io.DisplaySize.x * 0.9f, 1100 * scale);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
    ImGui::Begin("##kbd", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushFont(g_font_big);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.93f, 0.76f, 0.33f, 1));
    // (no text from the game: a name entry - Link's, a course's - or a longer text, a Course World comment)
    ImGui::TextWrapped("%s", g_kbd_title[0] ? g_kbd_title : g_kbd_max <= 32 ? "Enter your name" : "Enter text");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    static bool was_open, was_pad;
    bool pad = g_last_pad.load(), appearing = !was_open || pad != was_pad;  // (switching device: focus the new one)
    was_open = true, was_pad = pad;
    if (!pad) {  // keyboard: type straight into the field
        if (appearing) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1);
        ImGui::PushFont(g_font_big);
        ImGui::InputText("##name", g_kbd_text, (size_t)(g_kbd_max * 4 + 1 < (int)sizeof g_kbd_text ? g_kbd_max * 4 + 1 : sizeof g_kbd_text),
                         ImGuiInputTextFlags_CallbackCharFilter,
                         [](ImGuiInputTextCallbackData *d) -> int { return d->EventChar < 0x20 ? 1 : 0; });
        ImGui::PopFont();
        // (the game's limit is in characters: cut at the character boundary after the last one, never inside one)
        int chars = 0;
        for (char *p = g_kbd_text; *p; p++) {
            if (((unsigned char)*p & 0xc0) == 0x80) continue;
            if (chars++ == g_kbd_max) { *p = 0; chars--; break; }
        }
        ImGui::TextDisabled("Enter  confirm      Esc  cancel      (%d / %d)", chars, g_kbd_max);
    } else {  // controller: on-screen keyboard (osk.mm, shared with our menus' text fields)
        int r = osk_keys(g_kbd_text, sizeof g_kbd_text, g_kbd_max, appearing, scale, g_font_big);
        if (r) g_kbd = r > 0 ? 2 : 3;  // (Start / + confirms in menu_handle_event too)
    }
    ImGui::End();
    if (g_kbd.load() != 1) was_open = false;
}

// (tests: games/smm2/mods/edit_ctl.c "kbd <text>") answer the open keyboard dialog with text
extern "C" void menu_kbd_test_answer(const char *text) {
    if (g_kbd.load() != 1) return;
    snprintf(g_kbd_text, sizeof g_kbd_text, "%s", text);
    g_kbd = 2;
}

// Game thread (runtime/hle/swkbd.c): show the dialog, re-present the last frame until it's answered.
// Returns 1 confirmed (out = text, UTF-8), 0 cancelled.
// The dialog's title for the next menu_keyboard_run (UTF-8; NULL or "": a name entry's or a text's own)
extern "C" void menu_keyboard_title(const char *title) { snprintf(g_kbd_title, sizeof g_kbd_title, "%s", title ? title : ""); }

extern "C" int menu_keyboard_run(const char *initial, int max_len, char *out, int cap) {
    extern void nvn_represent(void);  // runtime/nvn/objects.c
    snprintf(g_kbd_text, sizeof g_kbd_text, "%s", initial ? initial : "");
    g_kbd_max = max_len > 0 && max_len < (int)sizeof g_kbd_text / 4 ? max_len : 8;
    input_reset();
    g_kbd = 1;
    // (test) LARECOMP_KBD_AUTO=<text>: answer at once
    if (getenv("LARECOMP_KBD_AUTO")) { snprintf(g_kbd_text, sizeof g_kbd_text, "%s", getenv("LARECOMP_KBD_AUTO")); g_kbd = 2; }
    while (g_kbd.load() == 1 && !g_rt_unwind) {  // ~60 Hz (a reboot cancels)
        uint64_t t0 = SDL_GetTicks();
        nvn_represent();
        uint64_t dt = SDL_GetTicks() - t0;
        if (dt < 16) SDL_Delay((uint32_t)(16 - dt));
    }
    bool ok = g_kbd.load() == 2 && g_kbd_text[0];
    if (ok) snprintf(out, cap, "%s", g_kbd_text);
    g_kbd = 0;
    g_kbd_title[0] = 0;
    input_reset();
    return ok;
}

// The tab bar and its pages, into the current window: the in-game menu, and the start screen's Settings (in_game
// false: nothing of the game is loaded yet - the tabs that need it are greyed out; prelaunch: the start screen's own
// first tab, game files and fonts, drawn by launcher.mm).
static void tabs_body(float scale, bool in_game, void (*prelaunch)(float)) {
    // (id: the page; -1 = Prelaunch)
    struct Tab { const char *name; int id; bool on; };
    Tab tabs[11];
    int ntabs = 0;
    if (prelaunch) tabs[ntabs++] = {"Prelaunch", -1, true};
    // (LAS Recomp's game tabs - Save Editor, Randomizer, Warp, Cheats, Gameplay, Cosmetics - are not part of the
    // shared runtime; their ids stay unused so the test hooks keep their numbers)
    static const char *names[] = {"Video", "Audio", "Mods", "", "", "", "", "", "Controls", "Quit"};
    static const int order[] = {8, 0, 1, 2, 9};  // Controls, Video, Audio, Mods, Quit
    const GameModToggle *mods;
    const GameModTrigger *triggers;
    const GameModRow *mrows;
    bool have_mods = game_mod_toggles(&mods) > 0 || game_mod_rows(&mrows) > 0 ||
                     game_mod_triggers(&triggers) > 0 || launcher_has_settings() || online_rows_available();
    for (int i : order)  // (Quit: in game only; Mods: when the game has some)
        if ((i != 9 || !prelaunch) && (i != 2 || have_mods)) tabs[ntabs++] = {names[i], i, true};
    int cur = 0;  // g_tab / g_tab_request hold ids
    for (int i = 0; i < ntabs; i++) if (tabs[i].id == g_tab) cur = i;
    if (!tabs[cur].on) cur = 0;
    // L/R (or Q/E) switch tabs, skipping greyed-out ones (not while typing: Q / E are letters then, and the on-screen
    // keyboard has the controller)
    if (g_tab_request_ext >= 0) { g_tab_request = g_tab_request_ext; g_tab_request_ext = -1; }
    bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId), typing = ui_text_busy();
    static int focused_tab = -99;  // the tab the controller's cursor is on (from the frame before; -99: none)
    if (!any_popup && !typing) {
        int step = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false) ? 1
                 : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false) ? -1 : 0;
        if (step) {
            int i = cur;
            do i = (i + step + ntabs) % ntabs; while (!tabs[i].on);
            g_tab_request = tabs[i].id;
            // the cursor on the tab bar goes along (left behind, A on it switched straight back; user 2026-10-11)
            if (focused_tab != -99) g_focus_tab = tabs[i].id, g_focus_frames = 0;
        }
    }
    // Down with the cursor on the tab bar: the page's first row (option rows; other pages keep ImGui's own pick)
    bool down_from_tab = focused_tab != -99 && ImGui::GetCurrentContext()->NavCursorVisible && !any_popup &&
                         (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickDown) ||
                          ImGui::IsKeyPressed(ImGuiKey_DownArrow));
    opt_focus_first(down_from_tab);
    // touch: taller, wider tabs, and the pages' buttons / combos too (a fingertip's size)
    bool touch = g_touch_ui;
    if (touch) ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x * 1.6f,
                                                                      ImGui::GetStyle().FramePadding.y * 2.2f));
    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_NoTabListScrollingButtons)) {
        for (int k = 0; k < ntabs; k++) {
            const Tab &t = tabs[k];
            ImGuiTabItemFlags f = g_tab_request == t.id && t.on ? ImGuiTabItemFlags_SetSelected : 0;
            char lbl[64];
            snprintf(lbl, sizeof lbl, prelaunch ? " %s " : "  %s  ", t.name);  // (ten tabs: tighter, so none is cut short)
            ImGui::BeginDisabled(!t.on);  // (out of the game: Save Editor, Warp, Cheats)
            // focus (controller / keyboard): the tab lights up a little with a gold bar along its bottom, instead of
            // ImGui's outline (it sits 4 px outside the tab and the tab bar's clipping could cut its top off on macOS)
            // (focused_tab: from the frame before - colours go in before the tab is drawn)
            bool lit = focused_tab == t.id && ImGui::GetCurrentContext()->NavCursorVisible;
            ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
            if (lit) {
                ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.19f, 0.27f, 0.22f, 1));
                ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(0.19f, 0.27f, 0.22f, 1));
            }
            if (g_focus_tab == -1 && !prelaunch) g_focus_tab = 8;  // (no Prelaunch in game: Controls, the first tab)
            bool focus_here = g_focus_tab == t.id || (g_focus_tab == -100 && t.id == g_tab);
            bool open = ImGui::BeginTabItem(lbl, NULL, f);
            ImGui::PopStyleColor(lit ? 3 : 1);
            if (focus_here) {  // the controller's cursor starts on this tab (SetKeyboardFocusHere skips tabs: no tab stop)
                ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                ImGui::SetNavCursorVisible(true);
                // a few frames: the page's child window appearing makes ImGui pick its first widget once it's laid out
                if (++g_focus_frames >= 4) g_focus_tab = -99, g_focus_frames = 0;
                else g_focus_tab = t.id;
            }
            if (lit) {
                ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                float h = 4 * scale > 2 ? 4 * scale : 2;
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(a.x + 2, b.y - h), ImVec2(b.x - 2, b.y),
                                                          IM_COL32(236, 194, 84, 255));  // (the menu's gold)
            }
            if (ImGui::IsItemFocused()) focused_tab = t.id;
            else if (focused_tab == t.id) focused_tab = -99;
            ImGui::EndDisabled();
            if (open) {
                g_tab = t.id;
                ImGui::Spacing();
                ImGui::BeginChild("##page", ImVec2(0, 0), ImGuiChildFlags_None,  // (the rest of the window: no hint line)
                                  ImGuiWindowFlags_NavFlattened);
                // Down from the tab bar, pages that aren't option rows (Prelaunch, Controls, Warp): their first control
                // (option-row pages: opt_focus_first)
                if (down_from_tab && (t.id == -1 || t.id == 8 || t.id == 4)) {
                    opt_focus_first(false);
                    ImGui::SetKeyboardFocusHere();
                    ImGui::SetNavCursorVisible(true);
                }
                // Down from the tab bar, pages that aren't option rows (Prelaunch, Controls, Warp): their first control
                // (option-row pages: opt_focus_first)
                if (down_from_tab && (t.id == -1 || t.id == 8 || t.id == 4)) {
                    opt_focus_first(false);
                    ImGui::SetKeyboardFocusHere();
                    ImGui::SetNavCursorVisible(true);
                }
                switch (t.id) {
                case -1: prelaunch(scale); break;
                case 0: tab_display(); break;
                case 1: tab_audio(); break;
                case 2: tab_mods(); break;
                case 8: tab_about(); break;
                case 9: tab_quit(scale, in_game); break;
                }
                static const char *scroll_test = getenv("LARECOMP_MENU_SCROLL");  // test hook: page scroll, 0..1
                if (scroll_test) ImGui::SetScrollY(ImGui::GetScrollMaxY() * (float)atof(scroll_test));
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        }
        g_tab_request = -2;
        opt_focus_first(false);  // (a page without option rows: nothing took it)
        ImGui::EndTabBar();
    }
    if (touch) ImGui::PopStyleVar();
}

void menu_tabs_draw(float scale, ImFont *font, ImFont *font_big, bool in_game, void (*prelaunch)(float)) {
    ImFont *f0 = g_font, *f1 = g_font_big;
    g_font = font, g_font_big = font_big;
    ImGuiStyle saved = ImGui::GetStyle();  // the menu's look (the start screen has its own style)
    style_init();
    ImGui::GetStyle().ScaleAllSizes(scale);
    tabs_body(scale, in_game, prelaunch);
    ImGui::GetStyle() = saved;
    g_font = f0, g_font_big = f1;
}

// ---- a game's host-drawn screen (game_overlay_image, runtime/game_hooks.c): one RGBA8 image, drawn first (under the
// menu) into the 16:9 rectangle of the window; when it covers the game the rest of the window goes black.
extern "C" const uint8_t *game_overlay_image(int *w, int *h, int *full, uint64_t *seq);
#ifndef __APPLE__
extern "C" uint64_t vk_imgui_texture_stream(const void *rgba, int w, int h);  // runtime/nvn/vulkan/imgui_glue.cpp
#endif
static void game_overlay_draw(const uint8_t *img, int w, int h, bool full, uint64_t seq, ImVec2 disp) {
    static uint64_t shown = ~0ull;
    ImTextureID tid = 0;
#ifdef __APPLE__
    static id<MTLTexture> tex;
    if (!tex || (int)tex.width != w || (int)tex.height != h) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                     width:w height:h mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead;
        tex = [MTLCreateSystemDefaultDevice() newTextureWithDescriptor:d];
        shown = ~0ull;
    }
    if (seq != shown) [tex replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:img bytesPerRow:(NSUInteger)w * 4];
    tid = (ImTextureID)(intptr_t)(__bridge void *)tex;
#else
    static uint64_t vk_id;
    if (seq != shown || !vk_id) vk_id = vk_imgui_texture_stream(img, w, h);
    tid = (ImTextureID)vk_id;
#endif
    shown = seq;
    float s = fminf(disp.x / w, disp.y / h), dw = w * s, dh = h * s, x = (disp.x - dw) / 2, y = (disp.y - dh) / 2;
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    if (full) dl->AddRectFilled(ImVec2(0, 0), disp, IM_COL32(0, 0, 0, 255));
    if (tid) dl->AddImage(tid, ImVec2(x, y), ImVec2(x + dw, y + dh));
}

// Touch: a finger dragged up / down scrolls the page under it (ImGui scrolls with a wheel or the scrollbar only). Once
// it has moved 14 px (at 1080 p) it's a drag, not a tap: the row it started on lets go without being pressed.
static void touch_drag_scroll(ImGuiIO &io, float scale) {
    static bool down, drag;
    static ImVec2 start, last;
    static ImGuiID win_id;
    ImGuiContext &g = *ImGui::GetCurrentContext();
    if (io.MouseSource != ImGuiMouseSource_TouchScreen || !io.MouseDown[0]) { down = drag = false; return; }
    if (!down) {
        down = true, drag = false, start = last = io.MousePos, win_id = 0;
        for (ImGuiWindow *w = g.HoveredWindow; w; w = w->ParentWindow)  // (the nearest window that scrolls)
            if (w->ScrollMax.y > 0) { win_id = w->ID; break; }
        return;
    }
    ImGuiWindow *w = win_id ? ImGui::FindWindowByID(win_id) : NULL;
    if (!w) return;
    if (!drag && fabsf(io.MousePos.y - start.y) > 14 * scale) {
        drag = true;
        ImGui::ClearActiveID();  // (the press doesn't become a tap on release)
    }
    if (drag) ImGui::SetScrollY(w, w->Scroll.y - (io.MousePos.y - last.y));
    last = io.MousePos;
}

static bool g_drew;  // the last menu_render drew (menu_redraw draws it again)

#ifdef __APPLE__
void menu_redraw(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> target) {
    (void)dev;
    if (!g_drew) return;
    ImGui::SetCurrentContext(g_ctx);
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cb, enc);
    [enc endEncoding];
}
#else
void menu_redraw(void *cmd, uint32_t target_w, uint32_t target_h) {
    (void)target_w, (void)target_h;
    if (!g_drew) return;
    ImGui::SetCurrentContext(g_ctx);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)cmd);  // inside the window image's rendering
}
#endif

#ifdef __APPLE__
void menu_render(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> target) {
    const double tw = target.width, th = target.height;
#else
void menu_render(void *cmd, uint32_t target_w, uint32_t target_h) {
    const double tw = target_w, th = target_h;
#endif
    bool open = g_open.load();
    bool kbd = g_kbd.load() == 1;
    int ov_w = 0, ov_h = 0, ov_full = 0;
    uint64_t ov_seq = 0;
    const uint8_t *ov_img = game_overlay_image(&ov_w, &ov_h, &ov_full, &ov_seq);
    g_drew = false;
    if (!open && !kbd && !g_settings.show_fps && !g_settings.show_memory && !ov_img && !touchpad_visible() && !notice_active()) {
        if (g_inited) { pthread_mutex_lock(&q_lock); q_events.clear(); pthread_mutex_unlock(&q_lock); }
        return;
    }
    if (!g_inited) g_atlas_scale = th / 1080.0 < 1 ? 1.0f : th / 1080.0 > 2 ? 2.0f : (float)(th / 1080.0);
#ifdef __APPLE__
    if (!g_inited) init(dev);
#else
    if (!g_inited) init();
#endif
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)tw, (float)th);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    static double last;
    double now = SDL_GetTicks() / 1000.0;
    io.DeltaTime = last > 0 && now > last ? (float)(now - last) : 1.0f / 60;
    last = now;
    drain_events(io);
    // fonts are built at 34/56 px for a ~1080 px tall output; scale to the window
    float scale = (float)(th / 1080.0);
    if (scale < 0.45f) scale = 0.45f;
    io.FontGlobalScale = scale / g_atlas_scale;

#ifdef __APPLE__
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
#endif
    // spacing/rounding follow the output size too (style tuned for 1080 p)
    static ImGuiStyle base;
    static float style_scale = -1;
    if (style_scale < 0) base = ImGui::GetStyle();
    if (fabsf(style_scale - scale) > 0.01f) {
        ImGui::GetStyle() = base;
        ImGui::GetStyle().ScaleAllSizes(scale);
        style_scale = scale;
    }
#ifdef __APPLE__
    ImGui_ImplMetal_NewFrame(rp);
#else
    ImGui_ImplVulkan_NewFrame();
#endif
    ImGui::NewFrame();
    if (ov_img) game_overlay_draw(ov_img, ov_w, ov_h, ov_full, ov_seq, io.DisplaySize);

    if (g_settings.show_fps || g_settings.show_memory) {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 16 * scale, 16 * scale), 0, ImVec2(1, 0));
        ImGui::SetNextWindowBgAlpha(0.5f);
        ImGui::Begin("##fps", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
        static uint64_t real0, gen0;  // frame generation: the share of generated frames, over about half a second
        static double t0, out_ratio;
        if (now - t0 > 0.5) {
            uint64_t dr = g_fg_real - real0, dg = g_fg_generated - gen0;
            out_ratio = dr ? 1.0 + (double)dg / dr : 1.0;
            real0 = g_fg_real, gen0 = g_fg_generated, t0 = now;
        }
        if (g_settings.show_fps) {
            if (out_ratio > 1.05) ImGui::Text("%.0f fps (%.0f with frame generation)", io.Framerate, io.Framerate * out_ratio);
            else ImGui::Text("%.0f fps", io.Framerate);
        }
        if (g_settings.show_memory) {  // (sampled once a second: the call isn't free on every frame)
            static double mem_t;
            static uint64_t used, left;
            if (now - mem_t >= 1.0 || mem_t == 0) used = plat_memory_used(&left), mem_t = now;
            if (left) ImGui::Text("%llu MB (%llu MB left)", (unsigned long long)(used >> 20), (unsigned long long)(left >> 20));
            else ImGui::Text("%llu MB", (unsigned long long)(used >> 20));
        }
        ImGui::End();
    }

    if (!open && !kbd) touchpad_draw((float)tw, (float)th);
    if (!open && notice_active()) {
        char t[512];
        pthread_mutex_lock(&notice_lock);
        snprintf(t, sizeof t, "%s", notice_text);
        pthread_mutex_unlock(&notice_lock);
        float w = fminf(io.DisplaySize.x * 0.8f, 1100 * scale);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 40 * scale), 0, ImVec2(0.5f, 1));
        ImGui::SetNextWindowSize(ImVec2(w, 0));
        ImGui::SetNextWindowBgAlpha(0.85f);
        ImGui::Begin("##notice", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                                           ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(t);
        ImGui::PopTextWrapPos();
        ImGui::End();
    }

    if (open) touch_drag_scroll(io, scale);
    opt_touch(g_touch_ui);
    if (open) {
        // dim the game behind the menu
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 110));
        float w = fminf(io.DisplaySize.x * 0.9f, 1500 * scale), h = io.DisplaySize.y - 2 * 18 * scale;  // (all the height: a slim margin)
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
        if (g_touch_ui) ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 34 * scale);  // (a thumb's width)
        ImGui::Begin("##menu", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImFont *keep_big = g_font_big;  // (the window in the menu's font; header() uses g_font_big)
        g_font_big = g_menu_font_big;
        ImGui::PushFont(g_menu_font);
        // header as on the start screen's Settings: the title, and a close X in the corner (B / Esc too). ("Title
        // screen" - rt_request_reboot(RT_REBOOT_TITLE) - is to find a new home; user 2026-10-11.)
        ImGui::PushFont(g_font_big);
        ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "Settings");
        ImGui::PopFont();
        {
            float bx = (g_touch_ui ? 64 : 44) * scale;
            ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - bx);
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
            bool close = ImGui::InvisibleButton("##close", ImVec2(bx, bx), ImGuiButtonFlags_EnableNav);
            ImGui::PopStyleColor();
            bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
            ImU32 col = hot ? IM_COL32(236, 194, 84, 255) : IM_COL32(220, 216, 205, 255);
            float m = bx * 0.3f, t = (hot ? 3.0f : 2.4f) * scale * 1.5f;
            ImDrawList *dl = ImGui::GetWindowDrawList();
            if (hot) dl->AddRectFilled(p, ImVec2(p.x + bx, p.y + bx), IM_COL32(255, 255, 255, 20), 8 * scale);
            dl->AddLine(ImVec2(p.x + m, p.y + m), ImVec2(p.x + bx - m, p.y + bx - m), col, t);
            dl->AddLine(ImVec2(p.x + bx - m, p.y + m), ImVec2(p.x + m, p.y + bx - m), col, t);
            if (close) set_open(false);
        }

        bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId), typing = ui_text_busy();  // (before the
        tabs_body(scale, true, NULL);                                                                          //  pages run)
        // B with nothing focused-deeper closes the menu (not the Esc / B that ends typing: typing is from the frame
        // before, so it's still true the frame a field lets go)
        if (!any_popup && !typing && !ui_text_busy() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
            set_open(false);
        ImGui::PopFont();
        g_font_big = keep_big;
        ImGui::End();
        if (g_touch_ui) ImGui::PopStyleVar();
        osk_draw(g_font_big, scale);  // (a text field activated with the controller)
    }

    if (kbd) keyboard_draw(io, scale);
    ImGui::Render();
    g_drew = true;
    g_text_busy = open && ui_text_busy();
#ifdef __APPLE__
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cb, enc);
    [enc endEncoding];
#else
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)cmd);  // inside the window image's rendering
#endif
}
