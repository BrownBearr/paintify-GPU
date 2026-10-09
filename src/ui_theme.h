#pragma once
#include <imgui.h>

#include <string>

// Brushkit's design tokens: one place for every colour, size and font the
// interface uses. main.cpp and ui_widgets.cpp read these; nothing else should
// hard-code a colour or a pixel size.
//
// Visual direction "Darkroom / Gallery Wall": near-neutral chrome so the
// painting is the only colourful thing on screen, one accent that marks
// *active state* only (primary action, drag, on, focus -- never a resting
// fill), and hierarchy carried by type and space rather than boxes.
//
// One palette, "Paper": cerulean blue for action, pale tints of it (highlight*)
// for selected or filled state. Every text and control-boundary pair is held to WCAG AA
// (text 4.5:1, boundaries 3:1).
//
// All metrics are authored at 100% scale and multiplied by the content scale
// (glfwGetWindowContentScale) through ui::px(). Fonts are rasterised once per
// scale at startup or on a monitor change -- ImGui 1.91 cannot resize a font
// on the fly, and FontGlobalScale would only blur the bitmaps.
namespace ui {

// ── Colour roles (sRGB; near-neutral, never more than a few units of tint) ──
struct Palette {
    ImVec4 bgApp;            // chrome: top bar, inspector, status bar
    ImVec4 bgCanvasDark;     // canvas surround, default
    ImVec4 bgCanvasGray;     // 18% reflectance mid grey
    ImVec4 bgCanvasWhite;    // paper preview, not pure white
    ImVec4 surface1;         // field fill at rest
    ImVec4 surface2;         // hover fill, popups
    ImVec4 surface3;         // pressed / selected fill
    ImVec4 borderSubtle;     // hairlines and dividers (decorative, no contrast target)
    ImVec4 borderDefault;    // control outlines at rest, toggle tracks, popups: >= 3:1
    ImVec4 borderStrong;     // hovered field outline: >= 3:1, stronger than default
    ImVec4 textPrimary;      // values, titles
    ImVec4 textSecondary;    // control labels
    ImVec4 textTertiary;     // hints, units, status
    ImVec4 textDisabled;
    ImVec4 accent;           // active state only: primary, on, drag, focus
    ImVec4 accentHover;
    ImVec4 accentPressed;
    ImVec4 accentTint;       // quiet accent wash (selection)
    ImVec4 accentTintStrong; // slider fill while dragging
    ImVec4 onAccent;         // text on an accent fill
    ImVec4 highlight;        // selected / filled state: slider value, selected segment, tab
    ImVec4 highlightHover;   // hovered row in menus and popups
    ImVec4 highlightStrong;  // slider fill while dragging, pressed row
    ImVec4 highlightEdge;    // cyan rule / marker that conveys the state (>= 3:1)
    ImVec4 danger;
    ImVec4 success;
    ImVec4 scrollGrab;
    ImVec4 scrollGrabHover;
    ImVec4 modalDim;
    ImVec4 imageEdge;        // 1 px edge around the painting on the white surround
    ImVec4 pillBg;           // canvas labels ("Source", "Painted")
    ImVec4 wipeLine;         // split-view divider
};

// ── Metrics, in px at 100% ─────────────────────────────────────────────────
struct Metrics {
    // spacing scale (4 px base)
    float s1 = 4.f, s2 = 8.f, s3 = 12.f, s4 = 16.f, s5 = 24.f, s6 = 32.f, s7 = 48.f;
    // radii
    float radiusSm = 4.f;    // chips, checkbox, scrollbar grab
    float radiusMd = 6.f;    // fields, buttons
    float radiusLg = 8.f;    // popups, cards
    // controls
    float controlH = 28.f;   // every field, button and row
    float primaryH = 32.f;   // the one primary button per region
    float segmentInset = 2.f;
    float toggleW = 28.f, toggleH = 16.f;
    float fieldPadX = 10.f;  // label/value inset inside a field
    // regions
    float topBarH = 44.f;
    float statusBarH = 28.f;
    float stripH = 44.f;     // video transport / batch queue strip
    float inspectorW = 360.f;
    float inspectorMinW = 300.f;
    float inspectorMaxW = 520.f;
    float inspectorPadX = 24.f;
    float inspectorPadY = 16.f;
    float tabsH = 44.f;
    float splitterHit = 6.f;
    // section rhythm
    float sectionGap = 22.f;      // above a section's hairline
    float sectionTitleGap = 14.f; // hairline to title
    float sectionBodyGap = 12.f;  // title to first control
    float groupGap = 16.f;        // between sub-groups in a section
    float canvasMargin = 24.f;    // surround around the painting at fit
    // component sizes
    float barPadX = 16.f;         // top bar and strip inset
    float statusPadX = 12.f;      // status bar inset
    float menuPadX = 10.f;        // text inset inside menus and popovers
    float compactH = 22.f;        // status-bar segmented control, canvas pills
    float dotR = 3.f;             // status and notice dots
    float progressH = 2.f;        // export progress line under the top bar
    float iconStroke = 1.5f;      // drawn icons (chevrons, handle arrows)
    float handleR = 14.f;         // split-view grab handle
    float tooltipMaxW = 300.f;    // wrap width cap, about 38 characters of body text
    float tooltipPadX = 10.f, tooltipPadY = 8.f;
    float popoverGap = 6.f;       // popover offset above the status bar
    float popoverPadX = 16.f, popoverPadY = 14.f;
    float cardPadX = 20.f, cardPadY = 18.f;
    float dialogPadX = 24.f, dialogPadY = 20.f;
    float progressCardW = 380.f;  // export progress card
    float demoCardW = 440.f;      // demo-image card
    float dialogW = 400.f;        // confirmation dialog
    float diagnosticsW = 380.f;   // status-bar popovers
    float shortcutsW = 340.f;
    float keyColumnW = 150.f;     // label column inside those popovers
    float pickerMaxH = 460.f;     // brush picker list
    float playButtonW = 72.f;
    float scrubMinW = 120.f;      // smallest useful timeline
};

// ── Type scale, in px at 100% ─────────────────────────────────────────────
// Schibsted Grotesk (sans) for words, Geist Mono for numbers only: FreeType in
// ImGui 1.91 does no OpenType shaping, so tabular figures are unreachable in a
// proportional face and a value readout would jitter while dragged.
struct TypeScale {
    float title = 22.f;      // SemiBold: wordmark, page titles
    float section = 15.f;    // SemiBold: section headers
    float body = 14.f;        // Regular: labels, buttons (default font)
    float bodyStrong = 14.f; // Medium: tabs, primary buttons
    float caption = 13.f;     // Regular: hints, status bar
    float mono = 13.f;        // Geist Mono: values, timings
};

struct Fonts {
    ImFont* title = nullptr;
    ImFont* section = nullptr;
    ImFont* body = nullptr;
    ImFont* bodyStrong = nullptr;
    ImFont* caption = nullptr;
    ImFont* mono = nullptr;
    std::string source;      // where the faces came from, for Diagnostics
};

extern const Palette C;
extern const Metrics M;
extern const TypeScale T;
extern Fonts F;

// Current content scale (1 = 96 dpi).
float scale();
// A metric at the current scale, rounded to whole pixels so 1 px hairlines
// and text baselines land on the pixel grid.
float px(float v);

// Rebuilds the font atlas and the style for `contentScale`. Call once after
// ImGui::CreateContext(), and again when the window moves to a monitor with a
// different scale (then recreate the backend's font texture).
void applyTheme(float contentScale);

// Colour helpers. ImGui::GetColorU32(ImVec4) applies the current style alpha,
// which is what makes BeginDisabled() fade custom-drawn widgets too.
ImU32 col(const ImVec4& c);
ImU32 col(const ImVec4& c, float alpha);
ImVec4 withAlpha(const ImVec4& c, float a);

// Canvas surround choices (dark / 18% grey / white).
enum class Backdrop { Dark, Gray, White };
ImVec4 backdropColor(Backdrop b);

} // namespace ui
