// Two-pane settings pages (see option_rows.h): rows on the left, the selected / hovered row's description on the right.
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>

#include "imgui.h"
#include "imgui_internal.h"  // (SetKeyOwner, NavCursorVisible)

#include "option_rows.h"
#include <string.h>
extern "C" const char *menu_clipboard(void);  // (menu.mm: the system clipboard's text)
#include "osk.h"
#include "theme_draw.h"

static float g_scale = 1;
static std::string g_desc_hover, g_desc_focus, g_desc_shown;  // this frame's hovered / selected row, and the last shown
struct DescImage { uint64_t tex = 0; int w = 0, h = 0; };
static DescImage g_img_hover, g_img_focus, g_img_shown;
static std::string g_desc_first;  // the page's first row (shown until a row is selected or hovered)

// the menu theme's accent / caption colours (runtime/ui/menu.mm)
extern "C" uint32_t menu_accent(int a);
extern "C" uint32_t menu_muted(void);
extern "C" float menu_row_round(void);   // the theme's row corner radius / outline width (px at 1080 p)
extern "C" float menu_row_border(void);
extern "C" float menu_row_notch(void);   // > 0: the NES theme (stepped pixel corners, pointer, pixel arrows) instead of rounded rows
#define kGold menu_accent(255)
static bool g_next_disabled;
void opt_disable_next(void) { g_next_disabled = true; }
static int g_indent;
static bool g_focus_req, g_take_first;  // (opt_focus_first: requested / this page's first row takes it)
void opt_focus_first(bool on) { g_focus_req = on; }
static bool g_held;  // the last row's left / right has been held a while (steppers go faster)
static bool g_touch;
void opt_touch(bool on) { g_touch = on; }
void opt_indent(int step) { g_indent = g_indent + step < 0 ? 0 : g_indent + step; }

void opt_begin(const char *id, float scale) {
    static std::string page;  // another page: its own first row's description, not the last page's
    if (page != id) page = id, g_desc_shown.clear();
    g_scale = scale;
    g_take_first = g_focus_req;
    g_focus_req = false;
    if (g_take_first) ImGui::NavMoveRequestCancel();  // (this frame's Down goes to the first row instead)
    g_indent = 0;
    g_desc_hover.clear(), g_desc_focus.clear();
    g_img_hover = g_img_focus = DescImage();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::PushID(id);
    ImGui::BeginChild("##rows", ImVec2(avail.x * 0.48f, avail.y), ImGuiChildFlags_None, ImGuiWindowFlags_NavFlattened);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, (g_touch ? 8 : 4) * scale));  // (rows close)
    g_desc_first.clear();
}

void opt_section(const char *title) {
    std::string t = title;
    for (char &c : t) c = (char)toupper((unsigned char)c);
    if (ImGui::GetCursorPosY() > ImGui::GetStyle().WindowPadding.y + 1) ImGui::Dummy(ImVec2(0, 10 * g_scale));
    ImGui::PushStyleColor(ImGuiCol_Text, menu_muted());
    ImGui::TextUnformatted(t.c_str());
    ImGui::PopStyleColor();
}

// One row: draws it, handles A / click and left / right while selected. Returns 2 (A / click), -1 / +1 (left / right) or 0.
// arrows: ◀ value ▶ (a choice of several, or a stepper): left / right step it, and the arrows click too
static int row(const char *label, const std::string &value, bool changed, const char *desc, bool upper = true,
               bool arrows = false) {
    ImGui::PushID(label);
    if (g_indent) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + g_indent * 28 * g_scale);  // (an option of the row above)
    ImVec2 p = ImGui::GetCursorScreenPos();
    const bool nes = menu_row_notch() > 0;  // (the NES theme: pixel-art buttons, pointer and arrows)
    float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetFontSize() * (g_touch || nes ? 2.5f : 1.9f);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));  // (the row's own border shows the selection)
    bool clicked = ImGui::InvisibleButton("##row", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    if (g_take_first) {  // (opt_focus_first)
        g_take_first = false;
        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
        ImGui::SetNavCursorVisible(true);
        ImGui::SetScrollHereY(0);
    }
    bool off = g_next_disabled;  // (greyed out: still selectable, so its description shows, but it can't change)
    g_next_disabled = false;
    bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible;
    int step = clicked && !off ? 2 : 0;
    g_held = false;
    if (foc && !off) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft) ||
            ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
            step = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight) ||
            ImGui::IsKeyPressed(ImGuiKey_RightArrow))
            step = 1;
        for (ImGuiKey k : {ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickLeft,
                           ImGuiKey_GamepadLStickRight, ImGuiKey_LeftArrow, ImGuiKey_RightArrow})
            if (ImGui::GetKeyData(k)->DownDuration > 1.0f) g_held = true;
        // left / right belong to the row (navigation would otherwise try to move sideways)
        for (ImGuiKey k : {ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_LeftArrow, ImGuiKey_RightArrow})
            ImGui::SetKeyOwner(k, ImGui::GetItemID());
    }
    if (foc) g_desc_focus = desc ? desc : "";
    if (hov) g_desc_hover = desc ? desc : "";
    if (g_desc_first.empty() && desc) g_desc_first = desc;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    float r = menu_row_round() * g_scale, pad = 16 * g_scale, fs = ImGui::GetFontSize();
    ImU32 fill = hov || foc ? IM_COL32(255, 255, 255, 22) : IM_COL32(0, 0, 0, 40);
    ImU32 edge = hov || foc ? (off ? menu_accent(120) : kGold) : IM_COL32(255, 255, 255, off ? 18 : 38);
    float bw = (hov || foc ? 2.0f * menu_row_border() : menu_row_border()) * (menu_row_border() > 1 ? g_scale : 1);
    const bool lit = (hov || foc) && !off;
    const float ap = fmaxf(2.0f, floorf(fs * 0.15f));  // (a pixel of the pointer / arrows)
    if (nes) {  // NES.css's buttons: stepped corners, an inset shadow along the bottom and right, blue when pointed at
        ImU32 nfill = off ? NES_RGB(27, 30, 34) : lit ? NES_BLUE : NES_RGB(47, 52, 58);
        ImU32 nshadow = off ? NES_RGB(20, 22, 25) : lit ? NES_BLUE_D : NES_RGB(26, 29, 32);
        ImU32 nedge = off ? NES_RGB(58, 63, 70) : lit ? NES_WHITE : NES_RGB(92, 97, 107);
        nes_box(dl, p, ImVec2(p.x + w, p.y + h), nfill, nedge, 3 * g_scale, nshadow);
    } else {
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), fill, r);
        dl->AddRect(p, ImVec2(p.x + w, p.y + h), edge, r, 0, bw);
    }
    std::string t = label;
    if (upper)
        for (char &c : t) c = (char)toupper((unsigned char)c);
    float ty = p.y + (h - fs) * 0.5f;
    ImU32 label_col = off ? IM_COL32(110, 110, 102, 255) : nes ? NES_WHITE : IM_COL32(240, 236, 224, 255);  // (greyed out: clearly dim)
    float lead = nes ? 8 * ap : 0;  // (the NES pointer's room before the label)
    if (nes && lit) {  // the pointer: NES.css's selection arrow
        const ImU32 pc[] = {NES_WHITE};
        nes_icon(dl, ImVec2(p.x + pad, p.y + (h - 9 * ap) * 0.5f), kNesArrow, ap, pc);
    }
    {  // a label that would run into the value is cut short with "..." (long names: mods, courses)
        float vw0 = value.empty() ? 0 : ImGui::CalcTextSize(value.c_str()).x;
        float vspace = value.empty() ? 0 : (vw0 < w * 0.6f ? vw0 : w * 0.6f) + fs * (arrows ? 2.4f : 0.9f);
        float room = w - 2 * pad - vspace - lead;
        if (room > fs && ImGui::CalcTextSize(t.c_str()).x > room) {
            while (!t.empty() && ImGui::CalcTextSize((t + "...").c_str()).x > room) t.pop_back();
            while (!t.empty() && (t.back() & 0xC0) == 0x80) t.pop_back();  // (never half a UTF-8 character)
            if (!t.empty() && (unsigned char)t.back() >= 0xC0) t.pop_back();
            t += "...";
        }
    }
    dl->AddText(ImVec2(p.x + pad + lead, ty), label_col, t.c_str());
    float right = p.x + w - pad;  // (the value's right edge)
    if (arrows && !value.empty()) {  // ◀ value ▶, drawn (the fonts have no arrow glyphs); a click on one steps
        float aw = nes ? 5 * ap : fs * 0.42f, gap = fs * 0.55f, cy = ty + fs * 0.52f;
        float rx = right - aw, lx = rx - gap - ImGui::CalcTextSize(value.c_str()).x - gap - aw;
        ImU32 ac = off ? IM_COL32(110, 110, 100, 255) : nes ? (lit ? NES_WHITE : NES_GREY) : hov || foc ? kGold : IM_COL32(170, 170, 160, 255);
        if (nes) {  // stepped pixel arrows
            const ImU32 pc[] = {ac};
            nes_icon(dl, ImVec2(lx, p.y + (h - 9 * ap) * 0.5f), kNesArrow, ap, pc, true);
            nes_icon(dl, ImVec2(rx, p.y + (h - 9 * ap) * 0.5f), kNesArrow, ap, pc);
        } else {
            dl->AddTriangleFilled(ImVec2(lx, cy), ImVec2(lx + aw, cy - aw * 0.75f), ImVec2(lx + aw, cy + aw * 0.75f), ac);
            dl->AddTriangleFilled(ImVec2(rx + aw, cy), ImVec2(rx, cy - aw * 0.75f), ImVec2(rx, cy + aw * 0.75f), ac);
        }
        if (clicked && !off) {
            float mx = ImGui::GetIO().MousePos.x, hit = g_touch ? gap * 2.2f : gap;  // (a fingertip: wider)
            if (mx >= lx - hit && mx <= lx + aw + hit) step = -1;
            else if (mx >= rx - hit && mx <= rx + aw + hit) step = 1;
        }
        right = rx - gap;
    }
    std::string shown = value;  // (a value wider than most of the row is cut short too)
    if (!shown.empty() && ImGui::CalcTextSize(shown.c_str()).x > w * 0.6f) {
        while (!shown.empty() && ImGui::CalcTextSize((shown + "...").c_str()).x > w * 0.6f) shown.pop_back();
        while (!shown.empty() && (shown.back() & 0xC0) == 0x80) shown.pop_back();
        if (!shown.empty() && (unsigned char)shown.back() >= 0xC0) shown.pop_back();
        shown += "...";
    }
    if (!shown.empty()) {
        const std::string &value = shown;
        float vw = ImGui::CalcTextSize(value.c_str()).x, vx = right - vw;
        dl->AddText(ImVec2(vx, ty), off ? IM_COL32(110, 110, 102, 255) : nes ? (lit || changed ? NES_WHITE : NES_GREY)
                                    : changed ? IM_COL32(255, 255, 255, 255) : IM_COL32(205, 205, 195, 255), value.c_str());
    }
    ImGui::PopID();
    return step;
}

bool opt_toggle(const char *label, bool *v, bool def, const char *desc) {
    if (!row(label, *v ? "On" : "Off", *v != def, desc)) return false;  // (any step flips it)
    *v = !*v;
    return true;
}

bool opt_choice(const char *label, int *v, const char *const *names, int n, int def, const char *desc, bool wrap) {
    int cur = *v >= 0 && *v < n ? *v : 0;
    int step = row(label, names[cur], cur != def, desc, true, true);
    if (!step) return false;
    int nv = cur + (step == 2 ? 1 : step);
    nv = wrap ? (nv + n) % n : nv < 0 ? 0 : nv >= n ? n - 1 : nv;
    if (nv == cur) return false;
    *v = nv;
    return true;
}

bool opt_slider(const char *label, float *v, float lo, float hi, float step, float def, const char *fmt, const char *desc) {
    char buf[32];
    snprintf(buf, sizeof buf, fmt, *v);
    int s = row(label, buf, fabsf(*v - def) > step * 0.01f, desc, true, true);
    if (!s) return false;
    float eps = step * 0.01f, n = roundf(*v / step) * step + (s == 2 ? step : s * step);
    if (s == 2 && n > hi + eps) n = lo;  // (A / click past the top wraps to the bottom; left / right stop at the ends)
    *v = n < lo ? lo : n > hi ? hi : n;
    return true;
}

bool opt_int(const char *label, int *v, int lo, int hi, int step, int def, const char *fmt, const char *desc,
             const char *lo_name) {
    char buf[48];
    if (lo_name && *v == lo) snprintf(buf, sizeof buf, "%s", lo_name);
    else snprintf(buf, sizeof buf, fmt, *v);
    int s = row(label, buf, *v != def, desc, true, true);
    if (!s) return false;
    int d = s == 2 ? step : s * step * (g_held && hi - lo > 40 * step ? 10 : 1);
    int n = *v + d;
    if (s == 2 && n > hi) n = lo;  // (A / click past the top wraps; left / right stop at the ends)
    n = n < lo ? lo : n > hi ? hi : n;
    if (n == *v) return false;
    *v = n;
    return true;
}

bool opt_button(const char *label, const char *desc) { return row(label, "", false, desc) == 2; }

bool opt_radio(const char *label, bool selected, const char *desc) {
    bool off = g_next_disabled;
    bool chosen = row(label, "", false, desc) == 2;
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    float r = ImGui::GetFontSize() * 0.32f, cx = b.x - 16 * g_scale - r, cy = (a.y + b.y) * 0.5f;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImU32 col = off ? IM_COL32(150, 150, 140, 255) : selected ? kGold : IM_COL32(205, 205, 195, 255);
    if (menu_row_notch() > 0) {  // NES.css's radio: a stepped box, with a pixel star in it when chosen
        float u = 2 * g_scale, sz = r * 2.6f;
        nes_box(dl, ImVec2(cx - sz * 0.5f, cy - sz * 0.5f), ImVec2(cx + sz * 0.5f, cy + sz * 0.5f), NES_RGB(21, 24, 27),
                off ? NES_RGB(110, 110, 102) : NES_WHITE, u);
        if (selected) {  // a filled square inside
            float m = floorf(sz * 0.28f);
            dl->AddRectFilled(ImVec2(floorf(cx - sz * 0.5f + m), floorf(cy - sz * 0.5f + m)),
                              ImVec2(floorf(cx + sz * 0.5f - m), floorf(cy + sz * 0.5f - m)), off ? NES_RGB(110, 110, 102) : NES_GREEN);
        }
        return chosen && !selected;
    }
    dl->AddCircle(ImVec2(cx, cy), r, col, 0, 2.0f);
    if (selected) dl->AddCircleFilled(ImVec2(cx, cy), r * 0.55f, col);
    return chosen && !selected;
}

void opt_desc_image(uint64_t tex, int w, int h) {
    if (ImGui::IsItemHovered()) g_img_hover = {tex, w, h};
    if (ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible) g_img_focus = {tex, w, h};
}
bool opt_action(const char *label, const char *value, const char *desc, bool upper) {
    return row(label, value ? value : "", false, desc, upper) == 2;
}

static ImGuiID g_text_editing;  // the text row turned into a field (one at a time)
bool opt_text_editing(const char *label) { return g_text_editing && g_text_editing == ImGui::GetID(label); }

bool opt_text(const char *label, char *buf, size_t cap, const char *hint, const char *desc) {
    ImGuiID &editing = g_text_editing;
    static int focus;        // frames left to give it the keyboard focus
    ImGuiID id = ImGui::GetID(label);
    if (editing != id) {
        if (row(label, buf[0] ? buf : (hint ? hint : ""), false, desc) == 2) editing = id, focus = 2;
        return false;
    }
    ImGui::PushID(label);
    // a Paste button beside the field: the clipboard's text in place of the field's (touch screens have no Cmd+V;
    // owner, 2026-10-30: an OpenCourse World token couldn't be pasted)
    const char *paste_label = "Paste";
    float pw = ImGui::CalcTextSize(paste_label).x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetNextItemWidth(-(pw + ImGui::GetStyle().ItemSpacing.x));
    if (focus > 0) ImGui::SetKeyboardFocusHere(), focus--;
    bool done = ui_input_text("##text", buf, cap, ImGuiInputTextFlags_EnterReturnsTrue, hint, label);
    bool active = ImGui::IsItemActive();
    if (ImGui::IsItemFocused()) g_desc_focus = desc ? desc : "";
    ImGui::SameLine();
    bool paste = ImGui::Button(paste_label);
    bool paste_hot = ImGui::IsItemHovered() || ImGui::IsItemActive();  // (pressing it takes the focus off the field)
    if (paste) {
        const char *t = menu_clipboard();
        while (*t == ' ' || *t == '\t' || *t == '\n' || *t == '\r') t++;
        size_t n = strlen(t);
        while (n && (t[n - 1] == ' ' || t[n - 1] == '\t' || t[n - 1] == '\n' || t[n - 1] == '\r')) n--;
        if (n >= cap) n = cap - 1;
        memcpy(buf, t, n);
        buf[n] = 0;
        done = true;
    }
    if (done || ui_input_text_cancelled() || (focus == 0 && !active && !paste_hot && !osk_is_open())) editing = 0;
    ImGui::PopID();
    return done;
}

void opt_end(void) {
    ImGui::PopStyleVar();
    if (const char *t = getenv("LARECOMP_OPT_SCROLL")) ImGui::SetScrollY(ImGui::GetScrollMaxY() * (float)atof(t));  // (test)
    ImGui::EndChild();
    // the description: the hovered row (mouse) wins, else the selected one (controller), else the last shown
    if (!g_desc_hover.empty()) g_desc_shown = g_desc_hover, g_img_shown = g_img_hover;
    else if (!g_desc_focus.empty()) g_desc_shown = g_desc_focus, g_img_shown = g_img_focus;
    else if (g_desc_shown.empty()) g_desc_shown = g_desc_first, g_img_shown = DescImage();
    ImGui::SameLine(0, 0);
    ImVec2 p = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
    float gap = 24 * g_scale;
    const bool nes = menu_row_notch() > 0;
    if (nes) {  // the description in its own NES.css container (the child's own background is left clear)
        nes_box(ImGui::GetWindowDrawList(), ImVec2(p.x + gap, p.y), ImVec2(p.x + avail.x, p.y + avail.y),
                ImGui::GetColorU32(ImGuiCol_ChildBg), NES_GREY, 3 * g_scale);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    } else {
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + gap * 0.5f, p.y), ImVec2(p.x + gap * 0.5f, p.y + avail.y),
                                            menu_accent(110), 1.0f);
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x + gap, p.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, nes ? ImVec2(26 * g_scale, 20 * g_scale) : ImVec2(14 * g_scale, 6 * g_scale));
    ImGui::BeginChild("##desc", ImVec2(0, avail.y), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar();
    if (nes) ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(nes ? 1.0f : 1.3f);  // (the description reads at a glance)
    if (g_img_shown.w != 0) {  // a picture, or its place while it loads (w < 0: it won't come): a fixed 16:9 box
        ImVec2 room = ImGui::GetContentRegionAvail();
        float bw = fminf(room.x, room.y * 0.5f * 16 / 9), bh = bw * 9 / 16;
        ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + bw, p0.y + bh);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 110));
        if (g_img_shown.tex && g_img_shown.w > 0) {  // (fitted inside the box)
            float s = fminf(bw / g_img_shown.w, bh / g_img_shown.h), iw = g_img_shown.w * s, ih = g_img_shown.h * s;
            ImVec2 q(p0.x + (bw - iw) / 2, p0.y + (bh - ih) / 2);
            dl->AddImage((ImTextureID)g_img_shown.tex, q, ImVec2(q.x + iw, q.y + ih));
        } else {
            const char *t = g_img_shown.w < 0 ? "No screenshot" : "Loading screenshot...";
            ImVec2 ts = ImGui::CalcTextSize(t);
            dl->AddText(ImVec2(p0.x + (bw - ts.x) / 2, p0.y + (bh - ts.y) / 2), menu_muted(), t);
        }
        ImGui::Dummy(ImVec2(bw, bh));
        ImGui::Spacing();
    }
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(g_desc_shown.c_str());
    ImGui::PopTextWrapPos();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::EndChild();
    ImGui::PopID();
}
