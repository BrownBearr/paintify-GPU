#pragma once
#include "ui_theme.h"

#include <imgui.h>

// Brushkit's component library, built on the tokens in ui_theme.h. Every
// control is one 28 px row so the inspector keeps a single vertical rhythm;
// labels sit inside fields (secondary colour) and values are set in Geist Mono
// (primary colour), right-aligned so digits never move while dragging.
//
// Labels follow ImGui's convention: text after "##" is part of the ID but is
// not drawn, so two controls can both read "Hue" without colliding.
namespace ui {

enum class Tone { Neutral, Accent, Danger, Success };
enum class ButtonKind { Primary, Secondary, Ghost };

// ── Text and spacing ───────────────────────────────────────────────────────
void Gap(float px100);                         // vertical space at 100% scale
void Text(ImFont* font, const ImVec4& color, const char* text);
void TextWrapped(ImFont* font, const ImVec4& color, const char* text);
void Caption(const char* fmt, ...) IM_FMTARGS(1);   // wrapped, tertiary, 12 px
void Hairline(bool fullWidth = false);         // 1 px border.subtle
// A tooltip for the last item, after the usual hover delay. `hint` is a
// second, tertiary line (e.g. how to operate the control).
void Tooltip(const char* text, const char* hint = nullptr);
// Same, but also while the item is disabled -- for "why is this greyed out".
void TooltipWhenDisabled(const char* text);

// ── Structure ─────────────────────────────────────────────────────────────
// Collapsible section: hairline, 15 px SemiBold title, chevron at the right.
// The open state lives in ImGui's storage, keyed by the title.
bool Section(const char* title, bool defaultOpen = true, bool first = false);
// "Advanced" disclosure inside a section; closed by default.
bool Disclosure(const char* label, bool defaultOpen = false);
// Small sub-heading that names a cluster of 2-6 controls.
void GroupLabel(const char* text);
// Text tabs: Medium 14, active = primary text with a 2 px underline.
bool TextTabs(const char* id, const char* const* items, int count, int* index);

// Everything between Begin/EndDisabledWhy is greyed out and, when disabled,
// hovering it explains why. Use instead of hiding rows: controls that never
// move build muscle memory.
void BeginDisabledWhy(bool disabled, const char* reason);
void EndDisabledWhy();

// ── Controls ──────────────────────────────────────────────────────────────
struct ScrubOpts {
    const char* format = "%.2f";     // number only; units go in `unit`
    const char* unit = nullptr;      // tertiary, after the value
    const float* def = nullptr;      // enables Reset and the "modified" dot
    const char* help = nullptr;      // tooltip
    float width = 0.f;               // 0 = all available width
    const char* valueText = nullptr; // replaces the formatted value
    bool* released = nullptr;        // set true the frame an edit finishes
};
// Blender-style scrub field: drag anywhere to adjust (Shift = fine), click to
// type, right-click to reset. Fill runs from the minimum (or from zero for a
// range that spans it) to the value.
bool ScrubFloat(const char* label, float* v, float lo, float hi, const ScrubOpts& o = {});
bool ScrubInt(const char* label, int* v, int lo, int hi, const ScrubOpts& o = {});

// On/off switch, label left, 28x16 pill right-aligned.
bool Toggle(const char* label, bool* v, const char* help = nullptr);

// Segmented control. width 0 sizes each segment to its text; height 0 = 28.
bool Segmented(const char* id, const char* const* items, int count, int* index,
               float width = 0.f, float height = 0.f, const char* const* tooltips = nullptr);

// Select (combo): a field with the label on the left and the current value on
// the right. Between BeginSelect/EndSelect add SelectItem rows.
bool BeginSelect(const char* label, const char* preview, float width = 0.f,
                 float maxPopupH = 0.f);
bool SelectItem(const char* text, bool selected, const char* secondary = nullptr);
void EndSelect();

bool Button(const char* label, ButtonKind kind = ButtonKind::Secondary, float width = 0.f);
float ButtonWidth(const char* label, ButtonKind kind = ButtonKind::Secondary);

// A button that opens a menu; add MenuItem rows between Begin/End.
bool BeginMenuButton(const char* label, ButtonKind kind = ButtonKind::Ghost,
                     const char* valueText = nullptr);
bool MenuItem(const char* label, const char* shortcut = nullptr, bool enabled = true);
void MenuSeparator();
void EndMenuButton();

// Text field with its label stacked above (paths and lists need the width).
// width 0 = the whole row; the label row is skipped when label is "##id".
bool InputField(const char* label, char* buf, size_t size, ImGuiInputTextFlags flags = 0,
                const char* hint = nullptr, float width = 0.f);

// One-line message with a tone dot. Danger text is red; the rest secondary.
void Notice(Tone tone, const char* fmt, ...) IM_FMTARGS(2);

// ── Bars ──────────────────────────────────────────────────────────────────
// Text that occupies one control-height slot, vertically centred, so it lines
// up with buttons on the same SameLine() row.
void BarText(ImFont* font, const ImVec4& color, const char* text);
bool BarLink(ImFont* font, const ImVec4& color, const char* id, const char* text);
float TextWidth(ImFont* font, const char* text);

// ── Drawing primitives ────────────────────────────────────────────────────
void DrawChevron(ImDrawList* dl, ImVec2 center, float size, bool down, ImU32 color);
void DrawProgress(ImDrawList* dl, ImVec2 min, ImVec2 max, float frac, bool indeterminate);
void DrawPill(ImDrawList* dl, ImVec2 pos, ImFont* font, const char* text, ImU32 bg, ImU32 fg);

} // namespace ui
