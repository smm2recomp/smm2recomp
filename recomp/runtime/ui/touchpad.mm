// On-screen controller for touch-only devices (iPad, iPhone): a left stick, A/B/X/Y, L/R/ZL/ZR, Plus/Minus and a
// button that opens the overlay menu, drawn over the game with Dear ImGui (runtime/ui/menu.mm calls touchpad_draw).
// A finger that lands on one of them belongs to the controller until it lifts (it may slide between buttons); every
// other finger goes on to the console's touch screen (runtime/hle/touch.c), which the Course Maker is made for. The
// small button in the top left corner hides / shows the controller (it stays hidden while a gamepad is connected).
// Plain C++ (no Objective-C): the Vulkan builds compile it too; it shows there once a touch screen is touched.
#include <SDL3/SDL.h>
#include <math.h>
#include <atomic>

#include "imgui.h"

extern "C" void menu_toggle(void);                // runtime/ui/menu.mm
extern "C" bool menu_input_captured(void);        // runtime/ui/menu.mm: menu or name entry up (it takes the touches)
extern "C" bool input_gamepad_connected(void);    // runtime/hle/input.c

enum { B_A = 0, B_B, B_X, B_Y, B_L = 6, B_R, B_ZL, B_ZR, B_PLUS, B_MINUS };  // nn::hid button bits (input.c BTN_*)
enum { C_STICK = 1, C_BUTTON, C_MENU, C_TOGGLE };

struct Control { int kind, bit; float x, y, r; const char *label; };  // x, y, r in units of the screen's height
// (x < 0: from the right edge). Landscape, laid out for 4:3 (iPad) to 19.5:9 (iPhone).
static const Control CONTROLS[] = {
    {C_STICK, -1, 0.20f, 0.74f, 0.13f, ""},
    {C_BUTTON, B_A, -0.10f, 0.72f, 0.055f, "A"},
    {C_BUTTON, B_B, -0.20f, 0.84f, 0.055f, "B"},
    {C_BUTTON, B_X, -0.20f, 0.60f, 0.055f, "X"},
    {C_BUTTON, B_Y, -0.30f, 0.72f, 0.055f, "Y"},
    {C_BUTTON, B_L, 0.08f, 0.40f, 0.05f, "L"},
    {C_BUTTON, B_ZL, 0.08f, 0.26f, 0.05f, "ZL"},
    {C_BUTTON, B_R, -0.08f, 0.40f, 0.05f, "R"},
    {C_BUTTON, B_ZR, -0.08f, 0.26f, 0.05f, "ZR"},
    {C_BUTTON, B_MINUS, 0.30f, 0.06f, 0.035f, "-"},
    {C_BUTTON, B_PLUS, -0.30f, 0.06f, 0.035f, "+"},
    {C_MENU, -1, -0.06f, 0.06f, 0.035f, "="},
    {C_TOGGLE, -1, 0.06f, 0.06f, 0.035f, ""},
};
static const int NCONTROLS = sizeof CONTROLS / sizeof *CONTROLS;

// The window's safe area (iPhone: the notch / Dynamic Island in landscape): left and right insets in pixels, kept by
// the UI thread (safe-area events, touches) for both threads. The controls keep clear of them.
static std::atomic<float> g_inset_l{0}, g_inset_r{0};
static void update_insets(SDL_Window *win) {
    SDL_Rect safe;
    int ww = 0, wh = 0, pw = 0, ph = 0;
    if (!win || !SDL_GetWindowSafeArea(win, &safe) || !SDL_GetWindowSize(win, &ww, &wh) ||
        !SDL_GetWindowSizeInPixels(win, &pw, &ph) || ww <= 0)
        return;
    float k = (float)pw / (float)ww;
    g_inset_l = (float)safe.x * k, g_inset_r = (float)(ww - safe.x - safe.w) * k;
}

static float cx(const Control &c, float w, float h) {
    return c.x < 0 ? w - g_inset_r.load() + c.x * h : g_inset_l.load() + c.x * h;
}

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE) || defined(__ANDROID__)
static std::atomic<bool> g_touch_device{true};  // (iOS, Android: touch is the input there is)
#else
static std::atomic<bool> g_touch_device{false};  // set by the first finger
#endif
static std::atomic<bool> g_hidden{false};
static std::atomic<uint64_t> g_buttons{0};
static std::atomic<int32_t> g_lx{0}, g_ly{0};

// (LARECOMP_TOUCHPAD_SHOW=1, tests: shown with a gamepad connected too, e.g. the Simulator's)
static bool force_shown(void) { static int f = -1; if (f < 0) f = getenv("LARECOMP_TOUCHPAD_SHOW") != NULL; return f; }
static bool shown(void) {
    return g_touch_device.load() && (force_shown() || !input_gamepad_connected()) && !menu_input_captured();
}
extern "C" bool touchpad_visible(void) { return g_touch_device.load() && (force_shown() || !input_gamepad_connected()); }

// The fingers on the controller (UI thread only)
struct Finger { SDL_FingerID id; int control; };  // id 0: free
static Finger fingers[10];

static int hit(float px, float py, float w, float h, bool hidden) {
    for (int i = 0; i < NCONTROLS; i++) {
        const Control &c = CONTROLS[i];
        if (hidden && c.kind != C_TOGGLE) continue;
        float dx = px - cx(c, w, h), dy = py - c.y * h, r = c.r * h * (c.kind == C_STICK ? 1.3f : 1.25f);
        if (dx * dx + dy * dy <= r * r) return i;
    }
    return -1;
}

static void recompute(float w, float h, const float (*pos)[2]) {
    uint64_t b = 0;
    int32_t lx = 0, ly = 0;
    for (int i = 0; i < 10; i++) {
        if (!fingers[i].id || fingers[i].control < 0) continue;
        const Control &c = CONTROLS[fingers[i].control];
        if (c.kind == C_BUTTON) b |= 1ull << c.bit;
        if (c.kind == C_STICK) {
            float dx = (pos[i][0] - cx(c, w, h)) / (c.r * h), dy = (pos[i][1] - c.y * h) / (c.r * h);
            float m = sqrtf(dx * dx + dy * dy);
            if (m > 1) dx /= m, dy /= m;
            lx = (int32_t)(dx * 32767), ly = (int32_t)(-dy * 32767);
        }
    }
    g_buttons = b, g_lx = lx, g_ly = ly;
}

// UI thread: true when the event was the controller's (the touch screen and the game's other input don't see it)
extern "C" bool touchpad_handle_event(const SDL_Event *e) {
    if (e->type == SDL_EVENT_WINDOW_SAFE_AREA_CHANGED || e->type == SDL_EVENT_WINDOW_RESIZED) {
        update_insets(SDL_GetWindowFromID(e->window.windowID));
        return false;
    }
    if (e->type != SDL_EVENT_FINGER_DOWN && e->type != SDL_EVENT_FINGER_MOTION && e->type != SDL_EVENT_FINGER_UP &&
        e->type != SDL_EVENT_FINGER_CANCELED)
        return false;
    g_touch_device = true;
    static float pos[10][2];
    int w = 1, h = 1;
    SDL_Window *win = SDL_GetWindowFromID(e->tfinger.windowID);
    if (win) SDL_GetWindowSizeInPixels(win, &w, &h);
    if (e->type == SDL_EVENT_FINGER_DOWN) update_insets(win);
    float px = e->tfinger.x * w, py = e->tfinger.y * h;
    int slot = -1;
    for (int i = 0; i < 10; i++)
        if (fingers[i].id == e->tfinger.fingerID + 1) slot = i;
    if (e->type == SDL_EVENT_FINGER_DOWN) {
        if (slot >= 0 || !shown()) return false;
        int c = hit(px, py, (float)w, (float)h, g_hidden.load());
        if (c < 0) return false;  // (the touch screen's)
        for (int i = 0; i < 10 && slot < 0; i++)
            if (!fingers[i].id) slot = i;
        if (slot < 0) return true;
        if (CONTROLS[c].kind == C_MENU) { menu_toggle(); return true; }
        if (CONTROLS[c].kind == C_TOGGLE) { g_hidden = !g_hidden.load(); return true; }
        fingers[slot] = {e->tfinger.fingerID + 1, c};
    } else {
        if (slot < 0) return false;
        int ctl = fingers[slot].control;
        if (e->type == SDL_EVENT_FINGER_MOTION && !(ctl >= 0 && CONTROLS[ctl].kind == C_STICK)) {
            int c = hit(px, py, (float)w, (float)h, false);  // (slides between buttons; off them: released)
            fingers[slot].control = c >= 0 && CONTROLS[c].kind == C_BUTTON ? c : -1;
        }
        if (e->type != SDL_EVENT_FINGER_MOTION) fingers[slot] = {0, -1};
    }
    if (slot >= 0) pos[slot][0] = px, pos[slot][1] = py;
    recompute((float)w, (float)h, pos);
    return true;
}

// The game's input (runtime/hle/input.c): what the controller holds now
extern "C" void touchpad_state(uint64_t *buttons, int32_t *lx, int32_t *ly) {
    if (!shown()) { *buttons = 0, *lx = *ly = 0; return; }
    *buttons = g_buttons.load(), *lx = g_lx.load(), *ly = g_ly.load();
}

// Render thread, inside the overlay's ImGui frame: the controller over the game (w, h: the drawable in pixels)
extern "C" void touchpad_draw(float w, float h) {
    if (!shown()) return;
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    ImFont *font = ImGui::GetFont();
    const ImU32 ring = IM_COL32(255, 255, 255, 120), fill = IM_COL32(20, 20, 20, 70), held = IM_COL32(255, 255, 255, 110),
                text = IM_COL32(255, 255, 255, 200);
    uint64_t b = g_buttons.load();
    bool hidden = g_hidden.load();
    for (int i = 0; i < NCONTROLS; i++) {
        const Control &c = CONTROLS[i];
        if (hidden && c.kind != C_TOGGLE) continue;
        ImVec2 p(cx(c, w, h), c.y * h);
        float r = c.r * h, t = fmaxf(2.0f, h * 0.003f);
        if (c.kind == C_STICK) {
            dl->AddCircleFilled(p, r, fill, 48);
            dl->AddCircle(p, r, ring, 48, t);
            ImVec2 k(p.x + g_lx.load() / 32767.0f * r * 0.6f, p.y - g_ly.load() / 32767.0f * r * 0.6f);
            dl->AddCircleFilled(k, r * 0.45f, g_lx.load() || g_ly.load() ? held : IM_COL32(255, 255, 255, 60), 32);
            continue;
        }
        bool on = c.kind == C_BUTTON && (b >> c.bit & 1);
        dl->AddCircleFilled(p, r, on ? held : fill, 32);
        dl->AddCircle(p, r, ring, 32, t);
        if (c.kind == C_TOGGLE) {  // (a little controller outline: show / hide)
            float s = r * 0.5f;
            dl->AddRect(ImVec2(p.x - s, p.y - s * 0.55f), ImVec2(p.x + s, p.y + s * 0.55f), text, s * 0.4f, 0, t);
            if (hidden) dl->AddLine(ImVec2(p.x - s, p.y + s), ImVec2(p.x + s, p.y - s), text, t);
            continue;
        }
        if (c.kind == C_MENU) {  // (three lines)
            for (int k = -1; k <= 1; k++)
                dl->AddLine(ImVec2(p.x - r * 0.45f, p.y + k * r * 0.3f), ImVec2(p.x + r * 0.45f, p.y + k * r * 0.3f), text, t);
            continue;
        }
        float size = r * (c.label[1] ? 0.8f : 1.0f);
        ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, c.label);
        dl->AddText(font, size, ImVec2(p.x - ts.x * 0.5f, p.y - ts.y * 0.5f), text, c.label);
    }
}

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#import <objc/message.h>
#import <objc/runtime.h>
// iPadOS's own Command shortcuts, Command-W (close the window) above all: SDL's view reads the keyboard through
// GameController and passes key presses on to UIKit only while typing, so the system never saw them. Presses made with
// Command held now go on up the responder chain as well (the game still gets the keys), and a closed window ends the
// app, as closing does other iPadOS apps.
static NSMutableSet<UIPress *> *g_cmd_presses;  // passed on at their start: their end goes too (UI thread)

static void pass_cmd_presses(id view, SEL sel, NSSet<UIPress *> *presses, UIPressesEvent *event, bool began) {
    NSMutableSet<UIPress *> *pass = [NSMutableSet set];
    for (UIPress *p in presses) {
        if (began) {
            if (p.key && (p.key.modifierFlags & UIKeyModifierCommand)) { [g_cmd_presses addObject:p]; [pass addObject:p]; }
        } else if ([g_cmd_presses containsObject:p]) {
            [g_cmd_presses removeObject:p];
            [pass addObject:p];
        }
    }
    if (!pass.count || SDL_TextInputActive(SDL_GetKeyboardFocus())) return;  // (typing: SDL passes them on itself)
    struct objc_super up = {view, [UIView class]};
    ((void (*)(struct objc_super *, SEL, id, id))objc_msgSendSuper)(&up, sel, pass, event);
}

__attribute__((constructor)) static void ios_system_shortcuts(void) {
    Class c = NSClassFromString(@"SDL_uikitview");
    if (!c) return;
    g_cmd_presses = [NSMutableSet set];
    const SEL sels[] = {@selector(pressesBegan:withEvent:), @selector(pressesEnded:withEvent:),
                        @selector(pressesCancelled:withEvent:)};
    for (int i = 0; i < 3; i++) {
        Method m = class_getInstanceMethod(c, sels[i]);
        if (!m) continue;
        SEL sel = sels[i];
        bool began = i == 0;
        IMP orig = method_getImplementation(m);
        method_setImplementation(m, imp_implementationWithBlock(^(id view, NSSet<UIPress *> *presses, UIPressesEvent *event) {
            ((void (*)(id, SEL, id, id))orig)(view, sel, presses, event);
            pass_cmd_presses(view, sel, presses, event, began);
        }));
    }
    [NSNotificationCenter.defaultCenter addObserverForName:UISceneDidDisconnectNotification object:nil queue:nil
                                                usingBlock:^(NSNotification *n) {
        if (![n.object isKindOfClass:UIWindowScene.class]) return;
        fprintf(stderr, "[ios] the window was closed: quitting\n");
        exit(0);
    }];
}
#endif
