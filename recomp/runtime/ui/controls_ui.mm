// Control remapping editor (runtime/input_bind.h) of the in-game menu's Controls tab (menu.mm): a table of Switch
// inputs with the controller button and the key bound to each, for the player picked above it (#100: each player's
// controller has its own bindings, players 2-4 the same as player 1's until changed; the keyboard is player 1's).
// Select a binding, then press the new button / key (Esc or 5 s without a press cancels; a button already in use
// swaps over). Drawn in whichever ImGui context is current. Its Controller order page (controls_order_draw): which
// controller is which player, with the controllers drawn and what they hold shown on them.
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>

#include "imgui.h"

extern "C" {
#include "../input_bind.h"
#include "../settings.h"
#include "menu.h"
}

static void cell(int dev, int player, int slot, int fam, int cap_dev, int cap_player, int cap_slot, int ms_left) {
    char lbl[96];
    const int16_t *t = dev == BIND_DEV_PAD ? bind_pad_table(player) : g_bind_key;
    bool waiting = cap_dev == dev && cap_slot == slot && (dev != BIND_DEV_PAD || cap_player == player);
    if (waiting) snprintf(lbl, sizeof lbl, "Press %s...  %d##%d_%d", dev == BIND_DEV_PAD ? "a button" : "a key",
                          (ms_left + 999) / 1000, dev, slot);
    else snprintf(lbl, sizeof lbl, "%s##%d_%d", dev == BIND_DEV_PAD ? bind_pad_label(t[slot], fam) :
                                                 bind_key_label(t[slot]), dev, slot);
    // changed from the default: gold text; the same button / key on two inputs (a hand-edited larecomp.ini): red
    int n = dev == BIND_DEV_PAD ? BIND_PAD_COUNT : BIND_KEY_COUNT, pushed = 0;
    bool dup = false;
    for (int i = 0; i < n; i++) dup |= i != slot && t[i] == t[slot] && t[slot] > (dev == BIND_DEV_PAD ? -1 : 0);
    if (waiting) { ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.42f, 0.12f, 1)); pushed++; }
    else if (dup) { ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.16f, 0.12f, 1)); pushed++; }
    if (!bind_is_default_slot(dev, player, slot)) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.82f, 0.38f, 1));
        pushed++;
    }
    if (ImGui::Button(lbl, ImVec2(-FLT_MIN, 0))) {
        if (waiting) bind_capture_cancel();
        else bind_capture_begin(dev, player, slot);
    }
    ImGui::PopStyleColor(pushed);
    if (waiting && !ImGui::IsItemVisible()) ImGui::SetScrollHereY(0.5f);  // (a capture started by a test hook)
}

static int player = 1;  // whose bindings are shown
extern "C" int controls_player(void) { return player; }  // (menu.mm: the stick preview shows that player's controller)

extern "C" void controls_bindings_draw(float scale) {
    (void)scale;
    int cd, cp, cs, ms;
    bind_capture_active(&cd, &cp, &cs, &ms);
    if (cd == BIND_DEV_PAD && cp != player) player = cp;  // (a capture started by a test hook)
    // (the in-game menu's alone: on the start screen no controller is a player's yet, and nothing would take L + R)
    if (menu_is_open() && ImGui::Button("Controller order")) {
        bind_capture_cancel();
        input_pad_order_request(PAD_ORDER_OPEN);
    }
    // one button per player, its controller's name under it (a row of buttons: tabs in tabs would fight over L / R),
    // each with a radio mark in front: filled on the player shown, hollow on the others. The four share the row's
    // width evenly; a name wider than its button goes on over further lines, and all four buttons are as tall as the
    // one with the most.
    ImVec2 fp = ImGui::GetStyle().FramePadding;
    float radio = ImGui::GetFontSize() * 0.4f, text_x = 2 * radio + ImGui::GetStyle().ItemInnerSpacing.x;
    float bw = (ImGui::GetContentRegionAvail().x - (BIND_PLAYERS - 1) * ImGui::GetStyle().ItemSpacing.x) / BIND_PLAYERS;
    float tw = bw - 2 * fp.x - text_x;
    char pads[BIND_PLAYERS][256];
    int lines = 1;
    for (int p = 1; p <= BIND_PLAYERS; p++) {
        const char *name = input_player_pad_name(p);
        if (!name)
            name = input_player_pad(p) ? "Connected" : p == 1 && g_settings.pads_from_p2 ? "Keyboard" : "No controller";
        char *o = pads[p - 1], *oe = o + sizeof pads[0] - 1;
        int n = 1;
        for (const char *s = name, *e = s + strlen(s); s < e;) {
            float fs = ImGui::GetFontSize() / ImGui::GetFont()->FontSize;
            const char *brk = ImGui::GetFont()->CalcWordWrapPositionA(fs, s, e, tw);
            // (too narrow for one character: it alone)
            if (brk == s) do brk++; while (brk < e && (*brk & 0xC0) == 0x80);
            while (s < brk && o < oe) *o++ = *s++;
            s = brk;
            while (s < e && *s == ' ') s++;
            if (s < e && o < oe) *o++ = '\n', n++;
        }
        *o = 0;
        if (n > lines) lines = n;
    }
    float bh = (1 + lines) * ImGui::GetFontSize() + 2 * fp.y;
    for (int p = 1; p <= BIND_PLAYERS; p++) {
        char id[16], lbl[300];
        snprintf(id, sizeof id, "###player%d", p);
        snprintf(lbl, sizeof lbl, "Player %d\n%s", p, pads[p - 1]);
        if (p > 1) ImGui::SameLine();
        if (ImGui::Button(id, ImVec2(bw, bh)) && p != player) { bind_capture_cancel(); player = p; }
        // (the whole button is the item; the mark and the two lines are drawn on it)
        ImVec2 a = ImGui::GetItemRectMin(), c(a.x + fp.x + radio, a.y + fp.y + ImGui::GetFontSize() * 0.5f);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddCircle(c, radio, ImGui::GetColorU32(ImGuiCol_Text), 0, ImGui::GetFontSize() / 17);
        if (p == player) dl->AddCircleFilled(c, radio * 0.55f, ImGui::GetColorU32(ImGuiCol_CheckMark));
        dl->AddText(ImVec2(a.x + fp.x + text_x, a.y + fp.y), ImGui::GetColorU32(ImGuiCol_Text), lbl);
    }
    int fam = input_player_family(player);
    static const char *const fam_name[] = {"Nintendo", "Xbox", "PlayStation"};
    bool connected = input_player_pad(player) != 0;
    const char *pad_name = input_player_pad_name(player);
    char line[200];
    if (connected) snprintf(line, sizeof line, "Controller: %s  (%s button names)", pad_name ? pad_name : "connected",
                            fam_name[fam % 3]);
    else snprintf(line, sizeof line, "No controller connected  (%s button names shown)", fam_name[fam % 3]);
    ImGui::TextUnformatted(line);
    if (player > 1) ImGui::Text("Bindings: %s", bind_pad_custom(player) ? "Custom" : "Same as player 1");
    ImGui::PushTextWrapPos(0);
    if (player == 1 && g_settings.pads_from_p2) ImGui::TextDisabled("Used by players set to Same as player 1.");
    ImGui::TextDisabled("Select a binding, then press the new button or key. Esc (or 5 seconds) cancels. A button "
                        "that's already in use swaps with the old one.");
    ImGui::PopTextWrapPos();

    // a finished capture: say what happened, keep larecomp.ini current
    static int seen_serial = -1;
    static char msg[200];
    int rdev, rplayer, rslot, rother, serial;
    int kind = bind_last_result(&rdev, &rplayer, &rslot, &rother, &serial);
    if (seen_serial < 0) seen_serial = serial;
    if (serial != seen_serial) {
        seen_serial = serial;
        const int16_t *rt = bind_pad_table(rplayer);
        int rfam = input_player_family(rplayer);
        char who[16] = "";  // (players 2-4's controllers: whose)
        if (rdev == BIND_DEV_PAD && rplayer > 1) snprintf(who, sizeof who, "Player %d: ", rplayer);
        const char *now = rslot < 0 ? "" : rdev == BIND_DEV_PAD ? bind_pad_label(rt[rslot], rfam) :
                          bind_key_label(g_bind_key[rslot]);
        if (kind == 5) snprintf(msg, sizeof msg, "%s%s bindings reset to the defaults.", who,
                                rdev == BIND_DEV_PAD ? "Controller" : "Keyboard");
        else if (kind == 6) snprintf(msg, sizeof msg, "%sSame as player 1.", who);
        else if (kind == 1) snprintf(msg, sizeof msg, "%s%s is now %s.", who, bind_input_name(rslot), now);
        else if (kind == 2)
            snprintf(msg, sizeof msg, "%s%s is now %s; %s moved to %s (swapped).", who, bind_input_name(rslot), now,
                     bind_input_name(rother),
                     rdev == BIND_DEV_PAD ? bind_pad_label(rt[rother], rfam) : bind_key_label(g_bind_key[rother]));
        else if (kind == 3) snprintf(msg, sizeof msg, "Cancelled: %s unchanged.", bind_input_name(rslot));
        else if (kind == 4) snprintf(msg, sizeof msg, "Nothing pressed: %s unchanged.", bind_input_name(rslot));
        if (kind == 1 || kind == 2 || kind == 5 || kind == 6) settings_save();
    }
    if (msg[0]) ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "%s", msg);

    // players 2-4: the controller alone (the keyboard is player 1's), and nothing to select without a controller
    ImGui::BeginDisabled(player > 1 && !connected);
    if (ImGui::BeginTable(player == 1 ? "##binds" : "##binds_pad", player == 1 ? 3 : 2,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("Switch", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Controller", ImGuiTableColumnFlags_WidthStretch, 1.3f);
        if (player == 1) ImGui::TableSetupColumn("Keyboard", ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableHeadersRow();
        // in the order a player thinks of them: face buttons, shoulders, Plus / Minus, d-pad, sticks
        static const int order[] = {BIND_A, BIND_B, BIND_X, BIND_Y, BIND_L, BIND_R, BIND_ZL, BIND_ZR, BIND_PLUS,
                                    BIND_MINUS, BIND_UP, BIND_DOWN, BIND_LEFT, BIND_RIGHT, BIND_LSTICK, BIND_RSTICK,
                                    BIND_LS_UP, BIND_LS_DOWN, BIND_LS_LEFT, BIND_LS_RIGHT, BIND_RS_UP, BIND_RS_DOWN,
                                    BIND_RS_LEFT, BIND_RS_RIGHT};
        for (int slot : order) {
            if (player > 1 && slot >= BIND_PAD_COUNT) continue;  // (the stick directions are the keyboard's)
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(bind_input_name(slot));
            ImGui::TableNextColumn();
            if (slot < BIND_PAD_COUNT) cell(BIND_DEV_PAD, player, slot, fam, cd, cp, cs, ms);
            else ImGui::TextDisabled(slot < BIND_RS_UP ? "Left stick" : "Right stick");
            if (player > 1) continue;
            ImGui::TableNextColumn();
            cell(BIND_DEV_KEY, player, slot, fam, cd, cp, cs, ms);
        }
        ImGui::EndTable();
    }
    ImGui::EndDisabled();
    ImGui::Spacing();
    if (player > 1) {
        ImGui::BeginDisabled(!bind_pad_custom(player));
        if (ImGui::Button("Same as player 1")) { bind_capture_cancel(); bind_pad_follow(player); }
        ImGui::EndDisabled();
        ImGui::SameLine();
    }
    // (a player following player 1 can always be reset: the defaults become its own bindings)
    ImGui::BeginDisabled(bind_pad_custom(player) && bind_is_default(BIND_DEV_PAD, player));
    if (ImGui::Button(player == 1 ? "Reset controller to defaults" : "Reset to defaults")) {
        bind_capture_cancel();
        bind_reset(BIND_DEV_PAD, player);
    }
    ImGui::EndDisabled();
    if (player == 1) {
        ImGui::SameLine();
        ImGui::BeginDisabled(bind_is_default(BIND_DEV_KEY, 1));
        if (ImGui::Button("Reset keyboard to defaults")) { bind_capture_cancel(); bind_reset(BIND_DEV_KEY, 1); }
        ImGui::EndDisabled();
    }
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Bindings apply to the game only. Menus keep the fixed layout (right face button / Enter "
                        "select, bottom face button / Esc back; A and B swapped in these bindings or by \"A and B "
                        "swapped in menus\" trade the two), and the in-game menu always opens with F1 or Esc, or "
                        "the controller's Back + Start held together, or its Home / Guide button - so this page can't be "
                        "locked out.");
    ImGui::PopTextWrapPos();
}

// The Controller order page's own colours: the Switch screen's light boxes with a dark controller in them, whatever
// the menu's theme (what is outside the boxes keeps the menu's text colours). An empty box is a faint veil over the
// menu, so that it stays behind the boxes that have a player.
static const ImU32 k_order_box = IM_COL32(250, 250, 250, 255), k_order_box_empty = IM_COL32(245, 245, 245, 36);
static const ImU32 k_order_dark = IM_COL32(48, 48, 48, 255), k_order_grey = IM_COL32(180, 180, 180, 255);
static const ImU32 k_order_number = IM_COL32(180, 180, 180, 255), k_order_light_on = IM_COL32(150, 205, 28, 255);
static const ImU32 k_order_key = IM_COL32(72, 102, 220, 255);

// The Controller order page's controller, an icon in unit coordinates: its body 1 wide and k_pad_height high, x to
// the right and y down. Each shape is a closed outline of cubic Beziers, its start and then control 1, control 2, end
// per segment, clockwise; only the right half is listed, the left one is its mirror image (x -> 1 - x). k_pad_body
// runs from the top centre round the shoulder, down the outer side, which leans out, round the grip's end and up its
// slanted inner side to the flat arch between the grips. k_pad_r (L / R) is the cap over the body's upper corner and
// k_pad_zr (ZL / ZR) a tag of its own beside it, its outer top corner cut, which reaches k_pad_side past the body.
// The rest are centres and sizes of what is on the body, and k_pad_line the width of the thin lines.
static const ImVec2 k_pad_body[] = {
    {0.500f, 0.025f},  // the start
    {0.600f, 0.023f}, {0.700f, 0.024f}, {0.790f, 0.034f},
    {0.840f, 0.040f}, {0.880f, 0.062f}, {0.906f, 0.105f},
    {0.922f, 0.135f}, {0.934f, 0.180f}, {0.940f, 0.235f},
    {0.950f, 0.320f}, {0.985f, 0.440f}, {0.996f, 0.555f},
    {1.003f, 0.620f}, {0.975f, 0.695f}, {0.905f, 0.695f},
    {0.868f, 0.695f}, {0.850f, 0.672f}, {0.832f, 0.635f},
    {0.808f, 0.590f}, {0.790f, 0.555f}, {0.770f, 0.520f},
    {0.757f, 0.498f}, {0.745f, 0.494f}, {0.715f, 0.494f},
    {0.640f, 0.494f}, {0.570f, 0.494f}, {0.500f, 0.494f},
};
static const ImVec2 k_pad_r[] = {
    {0.649f, 0.022f},  // the start
    {0.680f, 0.010f}, {0.705f, 0.002f}, {0.740f, 0.002f},
    {0.800f, 0.002f}, {0.850f, 0.035f}, {0.890f, 0.088f},
    {0.895f, 0.096f}, {0.888f, 0.101f}, {0.880f, 0.095f},
    {0.850f, 0.076f}, {0.815f, 0.056f}, {0.770f, 0.048f},
    {0.730f, 0.041f}, {0.690f, 0.036f}, {0.649f, 0.022f},
};
static const ImVec2 k_pad_zr[] = {
    {0.982f, 0.005f},  // the start
    {1.001f, 0.005f}, {1.021f, 0.005f}, {1.040f, 0.005f},
    {1.046f, 0.005f}, {1.053f, 0.008f}, {1.057f, 0.012f},
    {1.062f, 0.017f}, {1.068f, 0.023f}, {1.073f, 0.028f},
    {1.077f, 0.032f}, {1.080f, 0.040f}, {1.080f, 0.045f},
    {1.080f, 0.056f}, {1.080f, 0.067f}, {1.080f, 0.078f},
    {1.080f, 0.083f}, {1.076f, 0.088f}, {1.070f, 0.088f},
    {1.041f, 0.088f}, {1.011f, 0.088f}, {0.982f, 0.088f},
    {0.976f, 0.088f}, {0.972f, 0.083f}, {0.972f, 0.078f},
    {0.972f, 0.057f}, {0.972f, 0.036f}, {0.972f, 0.015f},
    {0.972f, 0.009f}, {0.976f, 0.005f}, {0.982f, 0.005f},
};
static const float k_pad_height = 0.700f, k_pad_side = 0.080f, k_pad_line = 0.005f;
static const ImVec2 k_pad_tag_text = {1.024f, 0.049f};
static const float k_pad_tag_text_h = 0.042f;
static const ImVec2 k_pad_lstick = {0.222f, 0.209f}, k_pad_rstick = {0.628f, 0.347f};
static const float k_pad_ring = 0.080f, k_pad_head = 0.057f, k_pad_cap = 0.042f, k_pad_travel = 0.020f;
static const ImVec2 k_pad_dpad = {0.348f, 0.347f};
static const float k_pad_arm = 0.076f, k_pad_arm_half = 0.026f, k_pad_arm_round = 0.006f, k_pad_arm_mark = 0.011f;
static const ImVec2 k_pad_face = {0.762f, 0.208f};
static const float k_pad_face_step = 0.077f, k_pad_face_r = 0.038f;
static const ImVec2 k_pad_minus = {0.375f, 0.130f}, k_pad_plus = {0.625f, 0.130f};
static const ImVec2 k_pad_capture = {0.428f, 0.208f}, k_pad_home = {0.572f, 0.208f};
static const float k_pad_small_r = 0.023f, k_pad_sign = 0.012f, k_pad_sign_thick = 0.007f;
static const float k_pad_capture_half = 0.020f, k_pad_dot = 0.010f, k_pad_home_ring = 0.013f;
// Its colours: a light grey body with a thin dark outline, dark buttons with light marks on them, dark caps and tags.
static const ImU32 k_pad_col_body = IM_COL32(214, 214, 214, 255), k_pad_col_edge = k_order_dark;
static const ImU32 k_pad_col_key = k_order_dark, k_pad_col_mark = k_order_box, k_pad_col_cap = k_order_dark;
static const ImU32 k_pad_col_soft = IM_COL32(120, 120, 120, 255);  // (the sticks' thin rings)

// The outline `t` (n points: a start and three per segment) scaled to `w` at `at`, added to the draw list's path.
// Mirrored (`m`) it is walked from its end back to its start, so that it stays clockwise, as ImDrawList's fills want
// their shapes.
static void order_pad_path(ImDrawList *dl, ImVec2 at, float w, const ImVec2 *t, int n, bool m) {
    auto pt = [&](ImVec2 p) { return ImVec2(at.x + (m ? 1 - p.x : p.x) * w, at.y + p.y * w); };
    dl->PathLineToMergeDuplicate(pt(t[m ? n - 1 : 0]));
    if (m) for (int i = n - 3; i >= 1; i -= 3) dl->PathBezierCubicCurveTo(pt(t[i + 1]), pt(t[i]), pt(t[i - 1]));
    else for (int i = 1; i + 2 < n; i += 3) dl->PathBezierCubicCurveTo(pt(t[i]), pt(t[i + 1]), pt(t[i + 2]));
}

// A controller of the Controller order page, drawn (no image) with its body `w` wide and k_pad_height of that high,
// the body's top left corner at `at`: a Pro Controller seen from the front (the tables above), with the sticks, the
// d-pad, the four face buttons, Minus, Plus, Capture and Home on it, a cap over each upper corner for L / R and a tag
// beside each for ZL / ZR, which carries the trigger's name in the words of the controller's family (`fam`). What the
// controller holds (input_pad_order_state) is red, each arm of the d-pad too, and a stick's head goes where the stick
// is tilted: the page's button test. The body's outline is concave (the arch between the grips), so it is filled as
// such. The face buttons carry no letters (they differ by the controller's make).
static void order_pad_draw(ImDrawList *dl, ImVec2 at, float w, uint32_t held, const int32_t stick[4], int fam) {
    const ImU32 key = k_pad_col_key, mark = k_pad_col_mark, red = ImGui::GetColorU32(ImVec4(0.75f, 0.16f, 0.12f, 1));
    const float line = w * k_pad_line < 1 ? 1 : w * k_pad_line;
    auto pt = [&](float x, float y) { return ImVec2(at.x + x * w, at.y + y * w); };
    auto on = [&](int host, ImU32 idle) { return held >> host & 1 ? red : idle; };
    auto bar = [&](float x0, float y0, float x1, float y1, ImU32 col) {
        dl->AddRectFilled(pt(x0, y0), pt(x1, y1), col, k_pad_arm_round * w);
    };
    auto shape = [&](const ImVec2 *t, int n, bool m) {
        order_pad_path(dl, at, w, t, n, m);
        dl->_Path.pop_back();  // (it ends where it began)
    };
    // the triggers' tags (left: the mirror image), each with its name, shrunk if it is too long for the tag
    ImFont *font = ImGui::GetFont();
    for (int i = 0; i < 2; i++) {
        int host = i ? PADB_LTRIGGER : PADB_RTRIGGER;
        shape(k_pad_zr, IM_ARRAYSIZE(k_pad_zr), i);
        dl->PathFillConcave(on(host, k_pad_col_cap));
        const char *name = bind_pad_label(host, fam);
        float size = k_pad_tag_text_h * w / 0.72f, room = (k_pad_side + 0.02f) * w;  // (capitals: 0.72 of the size)
        ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, name);
        if (ts.x > room) { size *= room / ts.x; ts = font->CalcTextSizeA(size, FLT_MAX, 0, name); }
        ImVec2 c = pt(i ? 1 - k_pad_tag_text.x : k_pad_tag_text.x, k_pad_tag_text.y);
        dl->AddText(font, size, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), mark, name);
    }
    // the body: its right half down to the bottom centre, then the left half back up to the start; filled, then its
    // outline
    for (int i = 0; i < 2; i++) {
        order_pad_path(dl, at, w, k_pad_body, IM_ARRAYSIZE(k_pad_body), false);
        shape(k_pad_body, IM_ARRAYSIZE(k_pad_body), true);
        if (i) dl->PathStroke(k_pad_col_edge, ImDrawFlags_Closed, line);
        else dl->PathFillConcave(k_pad_col_body);
    }
    // L / R: the caps over the body's upper corners
    shape(k_pad_r, IM_ARRAYSIZE(k_pad_r), false);
    dl->PathFillConcave(on(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, k_pad_col_cap));
    shape(k_pad_r, IM_ARRAYSIZE(k_pad_r), true);
    dl->PathFillConcave(on(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, k_pad_col_cap));
    // the sticks (left: upper left, right: lower middle): a thin ring, and the head where the stick is
    for (int i = 0; i < 2; i++) {
        ImVec2 c = i ? k_pad_rstick : k_pad_lstick;
        float dx = stick[2 * i] / 32767.0f * k_pad_travel, dy = -stick[2 * i + 1] / 32767.0f * k_pad_travel;
        dl->AddCircle(pt(c.x, c.y), k_pad_ring * w, k_pad_col_soft, 0, line);
        dl->AddCircleFilled(pt(c.x + dx, c.y + dy), k_pad_head * w,
                            on(i ? SDL_GAMEPAD_BUTTON_RIGHT_STICK : SDL_GAMEPAD_BUTTON_LEFT_STICK, key));
        dl->AddCircle(pt(c.x + dx, c.y + dy), k_pad_cap * w, k_pad_col_soft, 0, line);
    }
    // the d-pad (lower left): one cross, red over the arm of a direction that is held, and a small arrow on each arm
    static const struct { int host; float x, y; } dirs[] = {
        {SDL_GAMEPAD_BUTTON_DPAD_UP, 0, -1}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0, 1},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, -1, 0}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 1, 0}};
    const float cx = k_pad_dpad.x, cy = k_pad_dpad.y, arm = k_pad_arm, half = k_pad_arm_half;
    bar(cx - half, cy - arm, cx + half, cy + arm, key);
    bar(cx - arm, cy - half, cx + arm, cy + half, key);
    for (const auto &d : dirs) {
        float mid = (arm + half) * 0.5f, len = (arm - half) * 0.5f;  // (from the middle square's edge to the tip)
        float mx = cx + d.x * mid, my = cy + d.y * mid, hx = d.x ? len : half, hy = d.y ? len : half;
        if (held >> d.host & 1) bar(mx - hx, my - hy, mx + hx, my + hy, red);
        float k = k_pad_arm_mark, bx = mx - d.x * k * 0.6f, by = my - d.y * k * 0.6f;  // (the arrow: tip, then base)
        dl->AddTriangleFilled(pt(mx + d.x * k, my + d.y * k), pt(bx - d.y * k, by + d.x * k),
                              pt(bx + d.y * k, by - d.x * k), mark);
    }
    // the face buttons (upper right), by position
    static const struct { int host; float x, y; } face[] = {
        {SDL_GAMEPAD_BUTTON_NORTH, 0, -1}, {SDL_GAMEPAD_BUTTON_SOUTH, 0, 1},
        {SDL_GAMEPAD_BUTTON_WEST, -1, 0}, {SDL_GAMEPAD_BUTTON_EAST, 1, 0}};
    for (const auto &f : face)
        dl->AddCircleFilled(pt(k_pad_face.x + f.x * k_pad_face_step, k_pad_face.y + f.y * k_pad_face_step),
                            k_pad_face_r * w, on(f.host, key));
    // Minus and Plus (Back and Start), with their signs
    const ImVec2 mi = k_pad_minus, pl = k_pad_plus, ca = k_pad_capture, ho = k_pad_home;
    float sign = k_pad_sign, thick = w * k_pad_sign_thick < 1 ? 1 : w * k_pad_sign_thick;
    dl->AddCircleFilled(pt(mi.x, mi.y), k_pad_small_r * w, on(SDL_GAMEPAD_BUTTON_BACK, key));
    dl->AddCircleFilled(pt(pl.x, pl.y), k_pad_small_r * w, on(SDL_GAMEPAD_BUTTON_START, key));
    dl->AddLine(pt(mi.x - sign, mi.y), pt(mi.x + sign, mi.y), mark, thick);
    dl->AddLine(pt(pl.x - sign, pl.y), pt(pl.x + sign, pl.y), mark, thick);
    dl->AddLine(pt(pl.x, pl.y - sign), pt(pl.x, pl.y + sign), mark, thick);
    // Capture (under Minus: a square with a dot) and Home (under Plus: a circle with a ring)
    float q = k_pad_capture_half;
    bar(ca.x - q, ca.y - q, ca.x + q, ca.y + q, on(SDL_GAMEPAD_BUTTON_MISC1, key));
    dl->AddCircleFilled(pt(ca.x, ca.y), k_pad_dot * w, mark);
    dl->AddCircleFilled(pt(ho.x, ho.y), k_pad_small_r * w, on(SDL_GAMEPAD_BUTTON_GUIDE, key));
    dl->AddCircle(pt(ho.x, ho.y), k_pad_home_ring * w, mark, 0, line);
}

// A line of the Controller order page that can be clicked, without a button's frame (as the Switch screen's prompts):
// `text` in the menu's text colour (greyed and dead when not `enabled`), underlined under the mouse, centred in the
// room left (align 0.5) or at its right edge (1). True when clicked.
static bool order_prompt(const char *id, const char *text, float align, bool enabled) {
    ImVec2 size = ImGui::CalcTextSize(text);
    float room = ImGui::GetContentRegionAvail().x;
    if (room > size.x) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (room - size.x) * align);
    ImGui::BeginDisabled(!enabled);
    bool clicked = ImGui::InvisibleButton(id, size);
    ImGui::EndDisabled();
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImU32 col = ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddText(a, col, text);
    float thin = ImGui::GetFontSize() * 0.04f;
    if (enabled && (ImGui::IsItemHovered() || ImGui::IsItemFocused()))
        dl->AddLine(ImVec2(a.x, b.y), b, col, thin < 1 ? 1 : thin);
    return clicked;
}

// Controls > Controller order (runtime/hle/input.c input_pad_order_*), in place of the Controls tab while it is up,
// laid out and drawn as the Switch's "Change Grip/Order" screen: a centred line that says what to do with L and R
// set off as keys, a row of square boxes, one per player, and under it the lines to leave a box, to confirm and (at
// the right) to go back. A controller that holds L + R joins in the lowest empty box: the box turns light with a dark
// frame and shows its player's lights in green and the controller, with what it holds in red (which also tests it).
// An empty box is faint, with its number small in the middle. Nothing changes until the draft is confirmed, and the
// controllers' buttons don't reach the menu meanwhile, but Guide and Back + Start, which close it. The order isn't
// saved: after a restart the controllers are in the order they are found again. Sizes go by the font's and the
// boxes', so the page scales with the menu. big: the font of the numbers.
extern "C" void controls_order_draw(ImFont *big) {
    bool from_p2 = g_settings.pads_from_p2, any = false;
    for (int p = 1; p <= BIND_PLAYERS; p++) any |= input_player_pad(p) != 0;
    // the names of the buttons: the controller's that has the say; before one has joined, the connected controller's
    // (as the bindings table's); with none connected, the keyboard's keys
    int lead = input_pad_order_lead(), fam = bind_label_family();
    {
        uint32_t held = 0;
        int32_t stick[4] = {0, 0, 0, 0};
        if (lead) input_pad_order_state(input_pad_order_box(lead, NULL), &held, stick, &fam);
    }
    const char *l = bind_pad_label(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, fam);
    const char *r = bind_pad_label(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, fam);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
    const float fs = ImGui::GetFontSize(), line = ImGui::GetTextLineHeight(), room = ImGui::GetContentRegionAvail().x;

    // "Press L + R on the controller.", centred, L and R each on a key
    const float kx = fs * 0.28f, ky = fs * 0.14f;
    ImGui::Dummy(ImVec2(room, line + 2 * ky));
    ImVec2 top = ImGui::GetItemRectMin();
    if (any) {
        const char *part[] = {"Press ", l, " + ", r, " on the controller."};
        float total = 0;
        for (int i = 0; i < 5; i++) total += ImGui::CalcTextSize(part[i]).x + (i & 1 ? 2 * kx : 0);
        float x = top.x + (room > total ? (room - total) * 0.5f : 0), y = top.y + ky;
        for (int i = 0; i < 5; i++) {
            float tw = ImGui::CalcTextSize(part[i]).x;
            if (i & 1) {
                dl->AddRectFilled(ImVec2(x, y - ky), ImVec2(x + tw + 2 * kx, y + line + ky), k_order_key, fs * 0.14f);
                dl->AddText(ImVec2(x + kx, y), IM_COL32_WHITE, part[i]);
                x += tw + 2 * kx;
            } else {
                dl->AddText(ImVec2(x, y), text, part[i]);
                x += tw;
            }
        }
    } else {
        const char *none = "No controller connected.";
        float tw = ImGui::CalcTextSize(none).x;
        dl->AddText(ImVec2(top.x + (room > tw ? (room - tw) * 0.5f : 0), top.y + ky), text, none);
    }
    ImGui::Dummy(ImVec2(0, fs * 0.25f));

    // the boxes: near square, across the page's whole width with narrow gaps (as the Switch's), the row centred. Only
    // when the lines under them wouldn't fit in the page's height that way are they smaller (the page shouldn't have
    // to scroll), though no smaller than 4 times the font's size
    const float part = 0.04f, sp = ImGui::GetStyle().ItemSpacing.y;  // (a gap, of a box's width)
    float under = 2 * fs * 0.25f + 3 * line + 5 * sp;  // (two gaps, three lines and the spacing between the items)
    float bw = floorf(room / (BIND_PLAYERS + 3 * part)), tall = (ImGui::GetContentRegionAvail().y - under) / 0.98f;
    if (tall < fs * 4) tall = fs * 4;
    if (bw > tall) bw = floorf(tall);
    float gap = floorf(bw * part), bh = floorf(bw * 0.98f), rw = bw * BIND_PLAYERS + 3 * gap;
    float thin = bw * 0.006f < 1 ? 1 : bw * 0.006f;
    if (room > rw) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (room - rw) * 0.5f);
    ImGui::Dummy(ImVec2(rw, bh));
    ImVec2 row = ImGui::GetItemRectMin();
    for (int p = 1; p <= BIND_PLAYERS; p++) {
        ImVec2 a(row.x + (p - 1) * (bw + gap), row.y), b(a.x + bw, a.y + bh);
        float leaving = 0;
        uint32_t id = input_pad_order_box(p, &leaving), held = 0;
        int32_t stick[4] = {0, 0, 0, 0};
        int pfam = 0;
        bool keyboard = p == 1 && from_p2;
        bool joined = !keyboard && id && input_pad_order_state(id, &held, stick, &pfam);
        if (!joined && !keyboard) {  // empty: faint, a thin grey frame, its number
            char num[2] = {(char)('0' + p), 0};
            float size = bh * 0.18f;
            ImVec2 ts = big->CalcTextSizeA(size, FLT_MAX, 0, num);
            dl->AddRectFilled(a, b, k_order_box_empty);
            dl->AddRect(a, b, k_order_grey, 0, 0, thin);
            dl->AddText(big, size, ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), k_order_number, num);
            continue;
        }
        dl->AddRectFilled(a, b, k_order_box);
        dl->AddRect(a, b, k_order_dark, 0, 0, 2 * thin);
        // the player's lights, as on a controller: p of the four lit
        float light = bw * 0.055f, lx = (a.x + b.x) * 0.5f - light * (4 + 3 * 0.42f) * 0.5f, ly = a.y + bh * 0.105f;
        for (int i = 0; i < 4; i++, lx += light * 1.42f)
            dl->AddRectFilled(ImVec2(lx, ly), ImVec2(lx + light, ly + light), i < p ? k_order_light_on : k_order_grey);
        if (keyboard) {
            ImFont *font = ImGui::GetFont();  // (the menu's font at the box's size, as the number of an empty box)
            float size = bh * 0.1f;
            ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, "Keyboard");
            dl->AddText(font, size, ImVec2((a.x + b.x - ts.x) * 0.5f, a.y + bh * 0.55f - ts.y * 0.5f), k_order_dark,
                        "Keyboard");
            continue;
        }
        // (0.9 of the box's width with the triggers' tags; its middle a little below the box's, between the lights
        // and the bar of leaving)
        float pw = bw * 0.9f / (1 + 2 * k_pad_side);
        ImVec2 at(a.x + (bw - pw) * 0.5f, a.y + bh * 0.55f - pw * k_pad_height * 0.5f);
        order_pad_draw(dl, at, pw, held, stick, pfam);
        if (leaving > 0) {  // holding L + R to leave: how far along, a thin bar along the bottom
            float bar = bh * 0.025f, m = bw * 0.1f;
            ImVec2 b0(a.x + m, b.y - bh * 0.075f), b1(b.x - m, b0.y + bar);
            dl->AddRectFilled(b0, b1, k_order_grey, bar * 0.5f);
            dl->AddRectFilled(b0, ImVec2(b0.x + (b1.x - b0.x) * leaving, b1.y), k_order_dark, bar * 0.5f);
        }
    }
    ImGui::Dummy(ImVec2(0, fs * 0.25f));

    char lbl[96];
    snprintf(lbl, sizeof lbl, "Hold %s + %s to leave.", l, r);  // (how long: the bar in the box)
    if (lead) {
        float tw = ImGui::CalcTextSize(lbl).x, left = ImGui::GetContentRegionAvail().x;
        if (left > tw) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (left - tw) * 0.5f);
        ImGui::TextDisabled("%s", lbl);
    } else {
        ImGui::TextDisabled(" ");
    }
    ImGui::Dummy(ImVec2(0, fs * 0.25f));
    // (with "Controllers start at player 2" and nobody in a box, no controller has the say: Enter; no player 1 yet:
    // greyed, and a click does nothing)
    snprintf(lbl, sizeof lbl, "Press %s when you're ready.",
             lead || (any && !from_p2) ? bind_pad_label(input_pad_order_button(true), fam) : "Enter");
    if (order_prompt("##order_ready", lbl, 0.5f, input_pad_order_ready())) input_pad_order_request(PAD_ORDER_CONFIRM);
    snprintf(lbl, sizeof lbl, "%s Back", lead || any ? bind_pad_label(input_pad_order_button(false), fam) : "Esc");
    if (order_prompt("##order_back", lbl, 1, true)) input_pad_order_request(PAD_ORDER_CANCEL);
}
