// Control remapping editor (runtime/input_bind.h) of the in-game menu's Controls tab (menu.mm): a table of Switch
// inputs with the controller button and the key bound to each, for the player picked above it (#100: each player's
// controller has its own bindings, players 2-4 the same as player 1's until changed; the keyboard is player 1's).
// Select a binding, then press the new button / key (Esc or 5 s without a press cancels; a button already in use
// swaps over). Drawn in whichever ImGui context is current.
#include <SDL3/SDL.h>
#include <stdio.h>

#include "imgui.h"

extern "C" {
#include "../input_bind.h"
#include "../settings.h"
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

extern "C" void controls_bindings_draw(float scale) {
    (void)scale;
    static int player = 1;  // whose bindings are shown
    int cd, cp, cs, ms;
    bind_capture_active(&cd, &cp, &cs, &ms);
    if (cd == BIND_DEV_PAD && cp != player) player = cp;  // (a capture started by a test hook)
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
                        "select, bottom face button / Esc back), and the in-game menu always opens with F1 or Esc, or "
                        "the controller's Back + Start held together, or its Home / Guide button - so this page can't be "
                        "locked out.");
    ImGui::PopTextWrapPos();
}
