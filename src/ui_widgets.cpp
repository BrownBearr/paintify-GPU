#include "ui_widgets.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace ui {
namespace {

// Text up to "##", which ImGui treats as ID-only.
const char* labelEnd(const char* label) {
    const char* e = std::strstr(label, "##");
    return e ? e : label + std::strlen(label);
}

ImVec2 textSize(ImFont* f, const char* b, const char* e = nullptr) {
    return f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.f, b, e);
}

// Text vertically centred in [y0, y0 + h).
void drawText(ImDrawList* dl, ImFont* f, float x, float y0, float h, ImU32 c,
              const char* b, const char* e = nullptr) {
    dl->AddText(f, f->FontSize, ImVec2(std::floor(x), std::floor(y0 + (h - f->FontSize) * 0.5f)),
                c, b, e);
}

struct DisabledFrame { bool disabled; const char* reason; };
std::vector<DisabledFrame> g_disabled;

// Popups opened by these widgets share one compact look.
void pushPopupStyle() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(4.f), px(4.f)));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, px(M.radiusLg));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, C.surface2);
    ImGui::PushStyleColor(ImGuiCol_Border, C.borderDefault);
}
void popPopupStyle() {
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

// Scrub state. Only one field can be dragged or typed into at a time, so a
// single slot is enough.
struct ScrubState {
    ImGuiID dragId = 0;
    bool moved = false;
    float startVal = 0.f;
    float accum = 0.f;
    float pressX = 0.f;
    ImGuiID editId = 0;
    int editFrames = 0;
    float editVal = 0.f;
} g_scrub;

bool scrubCore(const char* label, float* v, float lo, float hi, bool isInt,
               const ScrubOpts& o) {
    ImGuiIO& io = ImGui::GetIO();
    const ImGuiID id = ImGui::GetID(label);
    const float h = px(M.controlH);
    const float w = o.width > 0.f ? o.width : ImGui::GetContentRegionAvail().x;
    const float pad = px(M.fieldPadX);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + w, p0.y + h);
    bool changed = false;
    bool released = false;

    // ── typed entry, in place ─────────────────────────────────────────
    if (g_scrub.editId == id) {
        // The label stays put; the number is typed where the value was.
        const char* le = labelEnd(label);
        const float lw = le != label ? pad + textSize(F.body, label, le).x + px(8.f) : 0.f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float r = px(M.radiusMd);
        dl->AddRectFilled(p0, p1, col(C.surface3), r);
        dl->AddRect(p0, p1, col(C.accent), r, 0, 1.f);
        if (lw > 0.f) drawText(dl, F.body, p0.x + pad, p0.y, h, col(C.textSecondary), label, le);
        ImGui::PushID(int(id));
        ImGui::SetCursorScreenPos(ImVec2(p0.x + lw, p0.y));
        ImGui::SetNextItemWidth(w - lw);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(pad, (h - F.mono->FontSize) * 0.5f));
        ImGui::PushFont(F.mono);
        if (g_scrub.editFrames++ == 0) ImGui::SetKeyboardFocusHere();
        if (isInt) {
            int iv = int(std::lround(g_scrub.editVal));
            ImGui::InputInt("##type", &iv, 0, 0, ImGuiInputTextFlags_AutoSelectAll);
            g_scrub.editVal = float(iv);
        } else {
            ImGui::InputFloat("##type", &g_scrub.editVal, 0.f, 0.f, o.format,
                              ImGuiInputTextFlags_AutoSelectAll);
        }
        ImGui::PopFont();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            const float nv = std::clamp(g_scrub.editVal, lo, hi);
            if (nv != *v) { *v = nv; changed = true; }
            released = true;
        }
        if (ImGui::IsItemDeactivated() ||
            (g_scrub.editFrames > 2 && !ImGui::IsItemActive()))
            g_scrub.editId = 0;
        ImGui::PopID();
        if (o.released) *o.released = released;
        return changed;
    }

    // ── drag to scrub ─────────────────────────────────────────────────
    ImGui::InvisibleButton(label, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    if (ImGui::IsItemActivated()) {
        g_scrub.dragId = id;
        g_scrub.moved = false;
        g_scrub.startVal = *v;
        g_scrub.accum = 0.f;
        g_scrub.pressX = io.MousePos.x;
        if (io.KeyCtrl) {                     // Ctrl+click types, like ImGui
            g_scrub.editId = id;
            g_scrub.editFrames = 0;
            g_scrub.editVal = *v;
        }
    }
    const bool dragging = active && g_scrub.dragId == id;
    if (dragging && g_scrub.editId != id) {
        if (!g_scrub.moved && std::fabs(io.MousePos.x - g_scrub.pressX) > px(3.f))
            g_scrub.moved = true;
        if (g_scrub.moved) {
            const float perPx = (hi - lo) / std::max(w, 1.f) * (io.KeyShift ? 0.1f : 1.f);
            g_scrub.accum += io.MouseDelta.x * perPx;
            float nv = std::clamp(g_scrub.startVal + g_scrub.accum, lo, hi);
            g_scrub.accum = nv - g_scrub.startVal;        // no dead zone past the ends
            if (isInt) nv = std::round(nv);
            if (nv != *v) {
                *v = nv;
                changed = true;
            }
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
    }
    if (ImGui::IsItemDeactivated() && g_scrub.dragId == id) {
        if (g_scrub.moved) {
            released = true;
        } else if (hovered && g_scrub.editId != id) {
            // A click without a drag types a value (Blender's behaviour).
            g_scrub.editId = id;
            g_scrub.editFrames = 0;
            g_scrub.editVal = *v;
        }
        g_scrub.dragId = 0;
    }
    const bool scrubbing = dragging && g_scrub.moved;

    // Right-click: reset.
    if (o.def) {
        ImGui::PushID(int(id));
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##reset");
        pushPopupStyle();
        if (ImGui::BeginPopup("##reset")) {
            char buf[64];
            char item[96];
            snprintf(buf, sizeof(buf), o.format, isInt ? double(std::lround(*o.def)) : double(*o.def));
            snprintf(item, sizeof(item), "Reset to default (%s%s)", buf, o.unit ? o.unit : "");
            if (MenuItem(item)) {
                if (*v != *o.def) { *v = *o.def; changed = true; released = true; }
            }
            ImGui::EndPopup();
        }
        popPopupStyle();
        ImGui::PopID();
    }

    // ── draw ──────────────────────────────────────────────────────────
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = px(M.radiusMd);
    dl->AddRectFilled(p0, p1, col((hovered || scrubbing) ? C.surface2 : C.surface1), r);

    const float span = hi - lo;
    const float frac = span > 0.f ? std::clamp((*v - lo) / span, 0.f, 1.f) : 0.f;
    const float zero = (lo < 0.f && hi > 0.f) ? (-lo / span) : 0.f;
    const float a = std::min(frac, zero), b = std::max(frac, zero);
    if (b > a) {
        const float xa = p0.x + w * a, xb = p0.x + w * b;
        ImDrawFlags corners = ImDrawFlags_None;
        if (a <= 0.f) corners |= ImDrawFlags_RoundCornersLeft;
        if (b >= 1.f) corners |= ImDrawFlags_RoundCornersRight;
        dl->AddRectFilled(ImVec2(xa, p0.y), ImVec2(xb, p1.y),
                          col(scrubbing ? C.accentTintStrong : C.accentTint),
                          corners ? r : 0.f, corners);
    }
    // The level itself: a 2 px accent rule along the bottom of the fill.
    // (A vertical tick at the fill edge, as first drawn, cut through the
    // label at low values.)
    if (b > a) {
        const float xa = p0.x + w * a, xb = p0.x + w * b;
        const float lw = std::max(1.f, px(2.f));
        dl->PushClipRect(ImVec2(xa, p1.y - lw), ImVec2(xb, p1.y), true);
        dl->AddRectFilled(ImVec2(p0.x, p1.y - r * 2.f), p1, col(C.accent), r,
                          ImDrawFlags_RoundCornersBottom);
        dl->PopClipRect();
    }
    if (hovered && !scrubbing)
        dl->AddRect(p0, p1, col(C.borderStrong), r, 0, 1.f);

    // Value, right-aligned in mono, then the unit in tertiary.
    char vbuf[64];
    const char* vtext = o.valueText;
    if (!vtext) {
        snprintf(vbuf, sizeof(vbuf), o.format, isInt ? double(std::lround(*v)) : double(*v));
        vtext = vbuf;
    }
    const float unitW = o.unit ? textSize(F.caption, o.unit).x + px(2.f) : 0.f;
    const float valW = textSize(F.mono, vtext).x;
    float x = p1.x - pad - unitW;
    drawText(dl, F.mono, x - valW, p0.y, h, col(scrubbing ? C.accent : C.textPrimary), vtext);
    if (o.unit) drawText(dl, F.caption, x + px(2.f), p0.y, h, col(C.textTertiary), o.unit);

    // Label, clipped so it never runs under the value.
    const char* le = labelEnd(label);
    const float labelMax = x - valW - px(8.f);
    dl->PushClipRect(p0, ImVec2(std::max(p0.x, labelMax), p1.y), true);
    drawText(dl, F.body, p0.x + pad, p0.y, h,
             col(hovered || scrubbing ? C.textPrimary : C.textSecondary), label, le);
    dl->PopClipRect();
    // A changed value gets a quiet dot in the left padding.
    if (o.def && std::fabs(*v - *o.def) > 1e-5f * std::max(1.f, std::fabs(*o.def)))
        dl->AddCircleFilled(ImVec2(p0.x + pad * 0.45f, p0.y + h * 0.5f), px(2.f),
                            col(C.textTertiary));

    if (!active) {
        if (o.help)
            Tooltip(o.help, "Drag to adjust, Shift for fine. Click to type. Right-click to reset.");
        else if (o.def)
            Tooltip(nullptr, "Drag to adjust, Shift for fine. Click to type. Right-click to reset.");
    }
    if (o.released) *o.released = released;
    return changed;
}

} // namespace

// ── Text and spacing ───────────────────────────────────────────────────────

void Gap(float px100) {
    const float g = px(px100);
    if (g > 0.f) ImGui::Dummy(ImVec2(0.f, std::max(0.f, g - ImGui::GetStyle().ItemSpacing.y)));
}

void Text(ImFont* font, const ImVec4& color, const char* text) {
    ImGui::PushFont(font);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void TextWrapped(ImFont* font, const ImVec4& color, const char* text) {
    ImGui::PushFont(font);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void Caption(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    TextWrapped(F.caption, C.textTertiary, buf);
}

void Hairline(bool fullWidth) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    float x0 = p.x, x1 = p.x + ImGui::GetContentRegionAvail().x;
    if (fullWidth) {
        x0 = ImGui::GetWindowPos().x;
        x1 = x0 + ImGui::GetWindowWidth();
    }
    dl->AddRectFilled(ImVec2(x0, p.y), ImVec2(x1, p.y + 1.f), col(C.borderSubtle));
    ImGui::Dummy(ImVec2(0.f, 1.f));
}

static void tooltipBody(const char* text, const char* hint) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(10.f), px(8.f)));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, px(M.radiusLg));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(4.f), px(4.f)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, C.surface2);
    ImGui::PushStyleColor(ImGuiCol_Border, C.borderDefault);
    if (ImGui::BeginTooltip()) {
        ImGui::PushTextWrapPos(px(300.f));
        if (text) TextWrapped(F.body, C.textPrimary, text);
        if (hint) TextWrapped(F.caption, C.textTertiary, hint);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

void Tooltip(const char* text, const char* hint) {
    if ((text || hint) && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        tooltipBody(text, hint);
}

void TooltipWhenDisabled(const char* text) {
    if (text && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled |
                                     ImGuiHoveredFlags_DelayShort |
                                     ImGuiHoveredFlags_NoSharedDelay))
        tooltipBody(text, nullptr);
}

// ── Structure ─────────────────────────────────────────────────────────────

bool Section(const char* title, bool defaultOpen, bool first) {
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID id = ImGui::GetID(title);
    bool open = storage->GetBool(id, defaultOpen);

    if (!first) {
        Gap(M.sectionGap);
        Hairline();
        Gap(M.sectionTitleGap);
    }

    const float h = F.section->FontSize + px(8.f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    if (ImGui::InvisibleButton(title, ImVec2(w, h))) {
        open = !open;
        storage->SetBool(id, open);
    }
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    drawText(dl, F.section, p0.x, p0.y, h, col(C.textPrimary), title, labelEnd(title));
    DrawChevron(dl, ImVec2(p0.x + w - px(6.f), p0.y + h * 0.5f), px(8.f), open,
                col(hovered ? C.textSecondary : C.textTertiary));
    if (open) Gap(M.sectionBodyGap);
    return open;
}

bool Disclosure(const char* label, bool defaultOpen) {
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID id = ImGui::GetID(label);
    bool open = storage->GetBool(id, defaultOpen);
    Gap(4.f);
    const char* le = labelEnd(label);
    const float tw = textSize(F.body, label, le).x;
    const float h = px(24.f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton(label, ImVec2(tw + px(20.f), h))) {
        open = !open;
        storage->SetBool(id, open);
    }
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 c = col(hovered ? C.textPrimary : C.textSecondary);
    drawText(dl, F.body, p0.x, p0.y, h, c, label, le);
    DrawChevron(dl, ImVec2(p0.x + tw + px(10.f), p0.y + h * 0.5f + px(1.f)), px(7.f), open, c);
    return open;
}

void GroupLabel(const char* text) {
    Gap(M.groupGap - 2.f);
    Text(F.caption, C.textSecondary, text);
    Gap(2.f);
}

bool TextTabs(const char* id, const char* const* items, int count, int* index) {
    ImGui::PushID(id);
    const float h = px(M.tabsH);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool changed = false;
    float x = start.x;
    for (int i = 0; i < count; ++i) {
        const float tw = textSize(F.bodyStrong, items[i], labelEnd(items[i])).x;
        ImGui::SetCursorScreenPos(ImVec2(x, start.y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##tab", ImVec2(tw, h)) && *index != i) {
            *index = i;
            changed = true;
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool on = *index == i;
        const ImVec4& c = on ? C.textPrimary : hovered ? C.textSecondary : C.textTertiary;
        drawText(dl, F.bodyStrong, x, start.y, h, col(c), items[i], labelEnd(items[i]));
        if (on)
            dl->AddRectFilled(ImVec2(x, start.y + h - px(2.f)), ImVec2(x + tw, start.y + h),
                              col(C.textPrimary));
        x += tw + px(M.s5);
    }
    // The underline row: one hairline across the whole panel.
    const float x0 = ImGui::GetWindowPos().x;
    dl->AddRectFilled(ImVec2(x0, start.y + h), ImVec2(x0 + ImGui::GetWindowWidth(), start.y + h + 1.f),
                      col(C.borderSubtle));
    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y));
    ImGui::Dummy(ImVec2(0.f, h + 1.f));
    ImGui::PopID();
    return changed;
}

void BeginDisabledWhy(bool disabled, const char* reason) {
    g_disabled.push_back({disabled, reason});
    ImGui::BeginDisabled(disabled);
    ImGui::BeginGroup();
}

void EndDisabledWhy() {
    ImGui::EndGroup();
    ImGui::EndDisabled();
    const DisabledFrame f = g_disabled.back();
    g_disabled.pop_back();
    if (f.disabled) TooltipWhenDisabled(f.reason);
}

// ── Controls ──────────────────────────────────────────────────────────────

bool ScrubFloat(const char* label, float* v, float lo, float hi, const ScrubOpts& o) {
    return scrubCore(label, v, lo, hi, false, o);
}

bool ScrubInt(const char* label, int* v, int lo, int hi, const ScrubOpts& o) {
    float f = float(*v);
    float def = o.def ? *o.def : 0.f;
    ScrubOpts io = o;
    if (!o.format || std::strcmp(o.format, "%.2f") == 0) io.format = "%.0f";
    if (o.def) io.def = &def;
    const bool changed = scrubCore(label, &f, float(lo), float(hi), true, io);
    if (changed) *v = int(std::lround(f));
    return changed;
}

bool Toggle(const char* label, bool* v, const char* help) {
    const float h = px(M.controlH);
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    bool changed = false;
    if (ImGui::InvisibleButton(label, ImVec2(w, h))) {
        *v = !*v;
        changed = true;
    }
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    drawText(dl, F.body, p0.x, p0.y, h, col(hovered ? C.textPrimary : C.textSecondary),
             label, labelEnd(label));

    const float tw = px(M.toggleW), th = px(M.toggleH);
    const ImVec2 t0(p0.x + w - tw, p0.y + (h - th) * 0.5f);
    const ImVec2 t1(t0.x + tw, t0.y + th);
    const ImVec4& track = *v ? (hovered ? C.accentHover : C.accent)
                             : (hovered ? C.borderStrong : C.surface3);
    dl->AddRectFilled(t0, t1, col(track), th * 0.5f);
    const float kr = th * 0.5f - px(2.f);
    const float kx = *v ? t1.x - th * 0.5f : t0.x + th * 0.5f;
    dl->AddCircleFilled(ImVec2(kx, t0.y + th * 0.5f), kr,
                        col(*v ? C.onAccent : C.textSecondary), 20);
    if (help) Tooltip(help);
    return changed;
}

bool Segmented(const char* id, const char* const* items, int count, int* index,
               float width, float height, const char* const* tooltips) {
    ImGui::PushID(id);
    const float h = height > 0.f ? height : px(M.controlH);
    const float inset = px(M.segmentInset);
    const float padX = px(10.f);
    std::vector<float> widths;
    widths.resize(size_t(count));
    float natural = 0.f;
    for (int i = 0; i < count; ++i) {
        widths[size_t(i)] = textSize(F.body, items[i], labelEnd(items[i])).x + 2.f * padX;
        natural += widths[size_t(i)];
    }
    if (width < 0.f) width = ImGui::GetContentRegionAvail().x;
    if (width > 0.f) {
        const float each = (width - 2.f * inset) / float(count);
        for (float& wi : widths) wi = each;
    }
    const float total = (width > 0.f) ? width : natural + 2.f * inset;

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + total, p0.y + h), col(C.surface1), px(M.radiusMd));

    bool changed = false;
    float x = p0.x + inset;
    for (int i = 0; i < count; ++i) {
        const float wi = widths[size_t(i)];
        ImGui::SetCursorScreenPos(ImVec2(x, p0.y + inset));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##seg", ImVec2(wi, h - 2.f * inset)) && *index != i) {
            *index = i;
            changed = true;
        }
        const bool hovered = ImGui::IsItemHovered();
        if (tooltips && tooltips[i]) Tooltip(tooltips[i]);
        ImGui::PopID();
        const bool on = *index == i;
        if (on)
            dl->AddRectFilled(ImVec2(x, p0.y + inset), ImVec2(x + wi, p0.y + h - inset),
                              col(C.surface3), px(M.radiusMd) - inset);
        ImFont* f = on ? F.bodyStrong : F.body;
        const float tw = textSize(f, items[i], labelEnd(items[i])).x;
        drawText(dl, f, x + (wi - tw) * 0.5f, p0.y, h,
                 col(on || hovered ? C.textPrimary : C.textSecondary), items[i],
                 labelEnd(items[i]));
        x += wi;
    }
    ImGui::SetCursorScreenPos(p0);
    ImGui::Dummy(ImVec2(total, h));
    ImGui::PopID();
    return changed;
}

namespace { int g_selectDepth = 0; }

bool BeginSelect(const char* label, const char* preview, float width, float maxPopupH) {
    ImGui::PushID(label);
    const float h = px(M.controlH);
    const float w = width > 0.f ? width : ImGui::GetContentRegionAvail().x;
    const float pad = px(M.fieldPadX);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + w, p0.y + h);
    if (ImGui::InvisibleButton("##field", ImVec2(w, h))) ImGui::OpenPopup("##popup");
    const bool hovered = ImGui::IsItemHovered();
    const bool open = ImGui::IsPopupOpen("##popup");

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, col(hovered || open ? C.surface2 : C.surface1), px(M.radiusMd));
    if (hovered || open) dl->AddRect(p0, p1, col(C.borderStrong), px(M.radiusMd), 0, 1.f);
    const char* le = labelEnd(label);
    const bool hasLabel = le != label;
    const float chevX = p1.x - pad - px(4.f);
    DrawChevron(dl, ImVec2(chevX, p0.y + h * 0.5f), px(7.f), true, col(C.textTertiary));
    const float valueRight = chevX - px(12.f);
    if (hasLabel) {
        const float lw = textSize(F.body, label, le).x;
        drawText(dl, F.body, p0.x + pad, p0.y, h, col(hovered ? C.textPrimary : C.textSecondary),
                 label, le);
        const float vx0 = p0.x + pad + lw + px(12.f);
        const float vw = textSize(F.body, preview).x;
        dl->PushClipRect(ImVec2(vx0, p0.y), ImVec2(valueRight, p1.y), true);
        drawText(dl, F.body, std::max(vx0, valueRight - vw), p0.y, h, col(C.textPrimary), preview);
        dl->PopClipRect();
    } else {
        dl->PushClipRect(p0, ImVec2(valueRight, p1.y), true);
        drawText(dl, F.body, p0.x + pad, p0.y, h, col(C.textPrimary), preview);
        dl->PopClipRect();
    }

    ImGui::SetNextWindowPos(ImVec2(p0.x, p1.y + px(4.f)));
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0.f),
                                        ImVec2(w, maxPopupH > 0.f ? maxPopupH : px(400.f)));
    pushPopupStyle();
    const bool shown = ImGui::BeginPopup("##popup", ImGuiWindowFlags_NoMove);
    popPopupStyle();
    if (!shown) {
        ImGui::PopID();
        return false;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, 0.f));
    ++g_selectDepth;
    return true;
}

bool SelectItem(const char* text, bool selected, const char* secondary) {
    const float h = secondary ? px(46.f) : px(M.controlH);
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::PushID(text);
    const bool clicked = ImGui::InvisibleButton("##row", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    if (selected && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY(0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hovered)
        dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), col(C.surface3), px(M.radiusSm));
    const float tx = p0.x + px(24.f);
    if (selected)
        dl->AddCircleFilled(ImVec2(p0.x + px(11.f), p0.y + (secondary ? px(15.f) : h * 0.5f)),
                            px(3.f), col(C.accent));
    if (secondary) {
        drawText(dl, F.body, tx, p0.y + px(4.f), px(22.f), col(C.textPrimary), text, labelEnd(text));
        drawText(dl, F.caption, tx, p0.y + px(22.f), px(18.f), col(C.textTertiary), secondary);
    } else {
        drawText(dl, F.body, tx, p0.y, h, col(C.textPrimary), text, labelEnd(text));
    }
    if (clicked) ImGui::CloseCurrentPopup();
    return clicked;
}

void EndSelect() {
    if (g_selectDepth > 0) {
        --g_selectDepth;
        ImGui::PopStyleVar();
    }
    ImGui::EndPopup();
    ImGui::PopID();
}

float ButtonWidth(const char* label, ButtonKind kind) {
    ImFont* f = kind == ButtonKind::Primary ? F.bodyStrong : F.body;
    return textSize(f, label, labelEnd(label)).x + 2.f * px(kind == ButtonKind::Primary ? 16.f : 12.f);
}

bool Button(const char* label, ButtonKind kind, float width) {
    ImFont* f = kind == ButtonKind::Primary ? F.bodyStrong : F.body;
    const float h = px(kind == ButtonKind::Primary ? M.primaryH : M.controlH);
    float w = width;
    if (w < 0.f) w = ImGui::GetContentRegionAvail().x;
    if (w == 0.f) w = ButtonWidth(label, kind);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(label, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 bg(0, 0, 0, 0), fg = C.textPrimary;
    switch (kind) {
        case ButtonKind::Primary:
            bg = held ? C.accentPressed : hovered ? C.accentHover : C.accent;
            fg = C.onAccent;
            break;
        case ButtonKind::Secondary:
            bg = held ? C.surface3 : hovered ? C.surface2 : C.surface1;
            break;
        case ButtonKind::Ghost:
            bg = held ? C.surface3 : hovered ? C.surface2 : ImVec4(0, 0, 0, 0);
            fg = hovered ? C.textPrimary : C.textSecondary;
            break;
    }
    if (bg.w > 0.f) dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), col(bg), px(M.radiusMd));
    const char* le = labelEnd(label);
    const float tw = textSize(f, label, le).x;
    drawText(dl, f, p0.x + (w - tw) * 0.5f, p0.y, h, col(fg), label, le);
    return pressed;
}

bool BeginMenuButton(const char* label, ButtonKind kind, const char* valueText) {
    ImGui::PushID(label);
    const char* le = labelEnd(label);
    const float h = px(M.controlH);
    const float pad = px(10.f);
    const float lw = textSize(F.body, label, le).x;
    const float vw = valueText ? textSize(F.body, valueText).x + px(8.f) : 0.f;
    const float w = pad + lw + vw + px(18.f) + pad * 0.5f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##menubtn", ImVec2(w, h))) ImGui::OpenPopup("##menu");
    const bool hovered = ImGui::IsItemHovered();
    const bool open = ImGui::IsPopupOpen("##menu");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 bg = kind == ButtonKind::Secondary ? C.surface1 : ImVec4(0, 0, 0, 0);
    if (hovered || open) bg = C.surface2;
    if (bg.w > 0.f) dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), col(bg), px(M.radiusMd));
    const ImU32 fg = col(hovered || open || kind == ButtonKind::Secondary ? C.textPrimary
                                                                         : C.textSecondary);
    drawText(dl, F.body, p0.x + pad, p0.y, h, fg, label, le);
    if (valueText)
        drawText(dl, F.body, p0.x + pad + lw + px(8.f), p0.y, h, col(C.textPrimary), valueText);
    DrawChevron(dl, ImVec2(p0.x + w - pad * 0.5f - px(8.f), p0.y + h * 0.5f + px(1.f)), px(7.f),
                true, col(C.textTertiary));

    ImGui::SetNextWindowPos(ImVec2(p0.x, p0.y + h + px(4.f)));
    ImGui::SetNextWindowSizeConstraints(ImVec2(px(220.f), 0.f), ImVec2(px(420.f), FLT_MAX));
    pushPopupStyle();
    const bool shown = ImGui::BeginPopup("##menu", ImGuiWindowFlags_NoMove);
    popPopupStyle();
    if (!shown) {
        ImGui::PopID();
        return false;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, 0.f));
    return true;
}

bool MenuItem(const char* label, const char* shortcut, bool enabled) {
    const float h = px(M.controlH);
    const char* le = labelEnd(label);
    const float lw = textSize(F.body, label, le).x;
    const float sw = shortcut ? textSize(F.caption, shortcut).x + px(24.f) : 0.f;
    const float w = std::max(ImGui::GetContentRegionAvail().x, lw + sw + px(24.f));
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    ImGui::PushID(label);
    const bool clicked = ImGui::InvisibleButton("##item", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hovered) dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), col(C.surface3), px(M.radiusSm));
    drawText(dl, F.body, p0.x + px(10.f), p0.y, h, col(C.textPrimary), label, le);
    if (shortcut) {
        const float sx = p0.x + w - px(10.f) - textSize(F.caption, shortcut).x;
        drawText(dl, F.caption, sx, p0.y, h, col(C.textTertiary), shortcut);
    }
    ImGui::EndDisabled();
    if (clicked) ImGui::CloseCurrentPopup();
    return clicked;
}

void MenuSeparator() {
    Gap(4.f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2(p.x - px(4.f), p.y), ImVec2(p.x + ImGui::GetContentRegionAvail().x + px(4.f), p.y + 1.f),
        col(C.borderDefault));
    ImGui::Dummy(ImVec2(0.f, 1.f));
    Gap(4.f);
}

void EndMenuButton() {
    ImGui::PopStyleVar();
    ImGui::EndPopup();
    ImGui::PopID();
}

bool InputField(const char* label, char* buf, size_t size, ImGuiInputTextFlags flags,
                const char* hint, float width) {
    const char* le = labelEnd(label);
    if (le != label) {
        ImGui::PushFont(F.body);
        ImGui::PushStyleColor(ImGuiCol_Text, C.textSecondary);
        ImGui::TextUnformatted(label, le);
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y + px(4.f));
    }
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(width > 0.f ? width : -FLT_MIN);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(px(M.fieldPadX), (px(M.controlH) - F.body->FontSize) * 0.5f));
    const bool r = hint ? ImGui::InputTextWithHint("##in", hint, buf, size, flags)
                        : ImGui::InputText("##in", buf, size, flags);
    if (ImGui::IsItemHovered() && !ImGui::IsItemActive()) {
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRect(a, b, col(C.borderStrong), px(M.radiusMd), 0, 1.f);
    }
    if (ImGui::IsItemActive()) {
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRect(a, b, col(C.accent), px(M.radiusMd), 0, 1.f);
    }
    ImGui::PopStyleVar();
    ImGui::PopID();
    return r;
}

void Notice(Tone tone, const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    const ImVec4& dot = tone == Tone::Danger ? C.danger : tone == Tone::Accent ? C.accent
                      : tone == Tone::Success ? C.success : C.textTertiary;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float indent = px(14.f);
    ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(p.x + px(3.f), p.y + F.caption->FontSize * 0.55f + px(1.f)), px(3.f), col(dot));
    ImGui::SetCursorScreenPos(ImVec2(p.x + indent, p.y));
    ImGui::BeginGroup();
    TextWrapped(F.caption, tone == Tone::Danger ? C.danger : C.textSecondary, buf);
    ImGui::EndGroup();
}

// ── Bars ──────────────────────────────────────────────────────────────────

float TextWidth(ImFont* font, const char* text) { return textSize(font, text).x; }

void BarText(ImFont* font, const ImVec4& color, const char* text) {
    const float h = px(M.controlH);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(textSize(font, text).x, h));
    drawText(ImGui::GetWindowDrawList(), font, p.x, p.y, h, col(color), text);
}

bool BarLink(ImFont* font, const ImVec4& color, const char* id, const char* text) {
    const float h = px(M.controlH);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = textSize(font, text).x + px(12.f);
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hovered || ImGui::IsItemActive())
        dl->AddRectFilled(ImVec2(p.x, p.y + px(3.f)), ImVec2(p.x + w, p.y + h - px(3.f)),
                          col(C.surface2), px(M.radiusSm));
    drawText(dl, font, p.x + px(6.f), p.y, h, col(hovered ? C.textPrimary : color), text);
    return pressed;
}

// ── Drawing primitives ────────────────────────────────────────────────────

void DrawChevron(ImDrawList* dl, ImVec2 c, float size, bool down, ImU32 color) {
    const float s = size * 0.5f;
    const float t = std::max(1.f, px(1.5f));
    if (down) {
        const ImVec2 pts[3] = {ImVec2(c.x - s, c.y - s * 0.5f), ImVec2(c.x, c.y + s * 0.5f),
                               ImVec2(c.x + s, c.y - s * 0.5f)};
        dl->AddPolyline(pts, 3, color, ImDrawFlags_None, t);
    } else {
        const ImVec2 pts[3] = {ImVec2(c.x - s * 0.5f, c.y - s), ImVec2(c.x + s * 0.5f, c.y),
                               ImVec2(c.x - s * 0.5f, c.y + s)};
        dl->AddPolyline(pts, 3, color, ImDrawFlags_None, t);
    }
}

void DrawProgress(ImDrawList* dl, ImVec2 a, ImVec2 b, float frac, bool indeterminate) {
    const float r = (b.y - a.y) * 0.5f;
    dl->AddRectFilled(a, b, col(C.surface3), r);
    const float w = b.x - a.x;
    if (indeterminate) {
        // A 30% segment sweeping across: work is happening, length unknown.
        const float t = float(std::fmod(ImGui::GetTime() * 0.6, 1.0));
        const float x0 = a.x + (t * 1.3f - 0.3f) * w;
        const float x1 = x0 + 0.3f * w;
        dl->AddRectFilled(ImVec2(std::max(a.x, x0), a.y), ImVec2(std::min(b.x, x1), b.y),
                          col(C.accent), r);
    } else if (frac > 0.f) {
        dl->AddRectFilled(a, ImVec2(a.x + w * std::clamp(frac, 0.f, 1.f), b.y), col(C.accent), r);
    }
}

void DrawPill(ImDrawList* dl, ImVec2 pos, ImFont* font, const char* text, ImU32 bg, ImU32 fg) {
    const ImVec2 ts = textSize(font, text);
    const float padX = px(8.f), h = px(22.f);
    dl->AddRectFilled(pos, ImVec2(pos.x + ts.x + 2.f * padX, pos.y + h), bg, px(M.radiusSm));
    dl->AddText(font, font->FontSize,
                ImVec2(pos.x + padX, std::floor(pos.y + (h - font->FontSize) * 0.5f)), fg, text);
}

} // namespace ui
