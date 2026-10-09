#include "ui_theme.h"

#ifdef IMGUI_ENABLE_FREETYPE
#include <imgui_freetype.h>
#endif

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace ui {
namespace {

constexpr ImVec4 hex(unsigned rgb, float a = 1.f) {
    return ImVec4(float((rgb >> 16) & 0xFF) / 255.f, float((rgb >> 8) & 0xFF) / 255.f,
                  float(rgb & 0xFF) / 255.f, a);
}

float g_scale = 1.f;

// Fonts ship beside the executable (portable bundle and build tree alike, via
// the POST_BUILD copy), with the source tree as a fallback -- the same order
// the shader loader uses.
std::string fontDir() {
    namespace fs = std::filesystem;
#ifdef _WIN32
    char exe[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, exe, MAX_PATH)) {
        const fs::path beside = fs::path(exe).parent_path() / "fonts";
        if (fs::exists(beside / "Geist-Regular.ttf")) return beside.string();
    }
#endif
#ifdef SBR_ROOT_DIR
    const fs::path source = fs::path(SBR_ROOT_DIR) / "assets" / "fonts";
    if (fs::exists(source / "Geist-Regular.ttf")) return source.string();
#endif
    return {};
}

// Basic Latin + Latin-1 (covers ° × ·) plus the handful of typographic marks
// the copy uses. Anything wider would only bloat the atlas.
const ImWchar* glyphRanges() {
    static ImVector<ImWchar> ranges;
    if (ranges.empty()) {
        ImFontGlyphRangesBuilder b;
        b.AddRanges(ImGui::GetIO().Fonts->GetGlyphRangesDefault());
        for (ImWchar c : {ImWchar(0x2013), ImWchar(0x2014), ImWchar(0x2022), ImWchar(0x2026),
                          ImWchar(0x2190), ImWchar(0x2191), ImWchar(0x2192), ImWchar(0x2193),
                          ImWchar(0x2194), ImWchar(0x2212)})
            b.AddChar(c);
        b.BuildRanges(&ranges);
    }
    return ranges.Data;
}

ImFont* addFont(const std::string& path, float sizePx) {
    if (path.empty() || !std::filesystem::exists(path)) return nullptr;
    ImFontConfig cfg;
    cfg.OversampleH = 2;      // ignored by FreeType, used by stb_truetype
    cfg.OversampleV = 1;
    cfg.PixelSnapH = true;
#ifdef IMGUI_ENABLE_FREETYPE
    // Vertical-only hinting: crisp baselines and x-heights at 12-14 px while
    // keeping the face's own horizontal proportions.
    cfg.FontBuilderFlags = ImGuiFreeTypeBuilderFlags_LightHinting;
#endif
    return ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), sizePx, &cfg,
                                                    glyphRanges());
}

void loadFonts(float s) {
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    atlas->Clear();
    F = Fonts{};
    auto size = [&](float v) { return std::floor(v * s + 0.5f); };

    const std::string dir = fontDir();
    if (!dir.empty()) {
        const std::string regular = dir + "/Geist-Regular.ttf";
        const std::string medium = dir + "/Geist-Medium.ttf";
        const std::string semibold = dir + "/Geist-SemiBold.ttf";
        const std::string mono = dir + "/GeistMono-Regular.ttf";
        // The first font added is ImGui's default, so body goes first.
        F.body = addFont(regular, size(T.body));
        F.bodyStrong = addFont(medium, size(T.bodyStrong));
        F.caption = addFont(regular, size(T.caption));
        F.section = addFont(semibold, size(T.section));
        F.title = addFont(semibold, size(T.title));
        F.mono = addFont(mono, size(T.mono));
        if (F.body) F.source = "Geist, Geist Mono (" + dir + ")";
    }
    if (!F.body) {
        // Graceful fallback: the system UI face, then ImGui's built-in one.
        atlas->Clear();
        F = Fonts{};
        const std::string win = "C:/Windows/Fonts/";
        F.body = addFont(win + "segoeui.ttf", size(T.body));
        F.bodyStrong = addFont(win + "seguisb.ttf", size(T.bodyStrong));
        F.caption = addFont(win + "segoeui.ttf", size(T.caption));
        F.section = addFont(win + "seguisb.ttf", size(T.section));
        F.title = addFont(win + "seguisb.ttf", size(T.title));
        F.mono = addFont(win + "consola.ttf", size(T.mono));
        F.source = F.body ? "Segoe UI (Geist not found)" : "ImGui default (no fonts found)";
        if (!F.body) {
            ImFontConfig cfg;
            cfg.SizePixels = size(13.f);
            F.body = atlas->AddFontDefault(&cfg);
        }
        fprintf(stderr, "fonts: %s\n", F.source.c_str());
    }
    // Any face that failed to load falls back to body, so callers never
    // have to null-check.
    for (ImFont** f : {&F.bodyStrong, &F.caption, &F.section, &F.title, &F.mono})
        if (!*f) *f = F.body;
    atlas->Build();
}

void applyStyle(float s) {
    ImGuiStyle st;                      // fresh, so ScaleAllSizes is exact
    ImGui::StyleColorsDark(&st);

    st.WindowRounding = 0.f;
    st.ChildRounding = 0.f;
    st.FrameRounding = M.radiusMd;
    st.PopupRounding = M.radiusLg;
    st.GrabRounding = M.radiusSm;
    st.ScrollbarRounding = M.radiusSm;
    st.TabRounding = M.radiusMd;

    st.WindowBorderSize = 0.f;
    st.ChildBorderSize = 0.f;
    st.FrameBorderSize = 0.f;
    st.PopupBorderSize = 1.f;
    st.TabBorderSize = 0.f;
    st.SeparatorTextBorderSize = 1.f;

    st.WindowPadding = ImVec2(M.inspectorPadX, M.inspectorPadY);
    // 14 px body text + 2 x 7 = a 28 px frame, the same height as every
    // custom control.
    st.FramePadding = ImVec2(M.fieldPadX, (M.controlH - T.body) * 0.5f);
    st.ItemSpacing = ImVec2(M.s2, M.s2);
    st.ItemInnerSpacing = ImVec2(M.s2, M.s1);
    st.CellPadding = ImVec2(M.s2, M.s1);
    st.IndentSpacing = M.s3;
    st.ScrollbarSize = 10.f;
    st.GrabMinSize = 10.f;
    st.WindowMinSize = ImVec2(8.f, 8.f);
    st.PopupBorderSize = 1.f;
    st.DisabledAlpha = 0.38f;
    st.WindowTitleAlign = ImVec2(0.f, 0.5f);
    st.SelectableTextAlign = ImVec2(0.f, 0.5f);

    ImVec4* c = st.Colors;
    const ImVec4 clear(0.f, 0.f, 0.f, 0.f);
    // Every slot is assigned: any one left at ImGui's default blue makes the
    // whole interface read as a debug overlay.
    c[ImGuiCol_Text] = C.textPrimary;
    c[ImGuiCol_TextDisabled] = C.textTertiary;
    c[ImGuiCol_WindowBg] = C.bgApp;
    c[ImGuiCol_ChildBg] = clear;
    c[ImGuiCol_PopupBg] = C.surface2;
    c[ImGuiCol_Border] = C.borderDefault;
    c[ImGuiCol_BorderShadow] = clear;
    c[ImGuiCol_FrameBg] = C.surface1;
    c[ImGuiCol_FrameBgHovered] = C.surface2;
    c[ImGuiCol_FrameBgActive] = C.surface3;
    c[ImGuiCol_TitleBg] = C.bgApp;
    c[ImGuiCol_TitleBgActive] = C.bgApp;
    c[ImGuiCol_TitleBgCollapsed] = C.bgApp;
    c[ImGuiCol_MenuBarBg] = C.bgApp;
    c[ImGuiCol_ScrollbarBg] = clear;
    c[ImGuiCol_ScrollbarGrab] = C.scrollGrab;
    c[ImGuiCol_ScrollbarGrabHovered] = C.scrollGrabHover;
    c[ImGuiCol_ScrollbarGrabActive] = C.textTertiary;
    c[ImGuiCol_CheckMark] = C.accent;
    c[ImGuiCol_SliderGrab] = C.accent;
    c[ImGuiCol_SliderGrabActive] = C.accentPressed;
    c[ImGuiCol_Button] = C.surface1;
    c[ImGuiCol_ButtonHovered] = C.surface2;
    c[ImGuiCol_ButtonActive] = C.surface3;
    c[ImGuiCol_Header] = clear;
    c[ImGuiCol_HeaderHovered] = C.surface3;
    c[ImGuiCol_HeaderActive] = C.surface3;
    c[ImGuiCol_Separator] = C.borderSubtle;
    c[ImGuiCol_SeparatorHovered] = C.borderStrong;
    c[ImGuiCol_SeparatorActive] = C.accent;
    c[ImGuiCol_ResizeGrip] = clear;
    c[ImGuiCol_ResizeGripHovered] = clear;
    c[ImGuiCol_ResizeGripActive] = clear;
    c[ImGuiCol_TabHovered] = C.surface2;
    c[ImGuiCol_Tab] = clear;
    c[ImGuiCol_TabSelected] = C.surface3;
    c[ImGuiCol_TabSelectedOverline] = C.textPrimary;
    c[ImGuiCol_TabDimmed] = clear;
    c[ImGuiCol_TabDimmedSelected] = C.surface2;
    c[ImGuiCol_TabDimmedSelectedOverline] = clear;
    c[ImGuiCol_PlotLines] = C.textSecondary;
    c[ImGuiCol_PlotLinesHovered] = C.accent;
    c[ImGuiCol_PlotHistogram] = C.accent;
    c[ImGuiCol_PlotHistogramHovered] = C.accentHover;
    c[ImGuiCol_TableHeaderBg] = C.surface1;
    c[ImGuiCol_TableBorderStrong] = C.borderDefault;
    c[ImGuiCol_TableBorderLight] = C.borderSubtle;
    c[ImGuiCol_TableRowBg] = clear;
    c[ImGuiCol_TableRowBgAlt] = withAlpha(C.surface1, 0.5f);
    c[ImGuiCol_TextLink] = C.accent;
    c[ImGuiCol_TextSelectedBg] = withAlpha(C.accent, 0.35f);
    c[ImGuiCol_DragDropTarget] = C.accent;
    c[ImGuiCol_NavCursor] = C.accent;
    c[ImGuiCol_NavWindowingHighlight] = withAlpha(C.textPrimary, 0.7f);
    c[ImGuiCol_NavWindowingDimBg] = C.modalDim;
    c[ImGuiCol_ModalWindowDimBg] = C.modalDim;

    st.ScaleAllSizes(s);
    ImGui::GetStyle() = st;
}

} // namespace

const Palette C = {
    /*bgApp*/            hex(0x161616),
    /*bgCanvasDark*/     hex(0x1F1F1F),
    /*bgCanvasGray*/     hex(0x767676),
    /*bgCanvasWhite*/    hex(0xF2F2F2),
    /*surface1*/         hex(0x1E1E1E),
    /*surface2*/         hex(0x262626),
    /*surface3*/         hex(0x2E2E2E),
    /*borderSubtle*/     hex(0x262626),
    /*borderDefault*/    hex(0x333333),
    /*borderStrong*/     hex(0x4A4A4A),
    /*textPrimary*/      hex(0xEDEDED),
    /*textSecondary*/    hex(0xA3A3A3),
    /*textTertiary*/     hex(0x6E6E6E),
    /*textDisabled*/     hex(0x4D4D4D),
    /*accent*/           hex(0xE0A458),
    /*accentHover*/      hex(0xEBB673),
    /*accentPressed*/    hex(0xC88D42),
    /*accentTint*/       hex(0x3D3327),
    /*accentTintStrong*/ hex(0x54442E),
    /*onAccent*/         hex(0x161616),
    /*danger*/           hex(0xE5484D),
    /*success*/          hex(0x46A758),
    /*scrollGrab*/       hex(0x333333),
    /*scrollGrabHover*/  hex(0x4A4A4A),
    /*modalDim*/         hex(0x000000, 0.55f),
    /*imageEdge*/        hex(0x000000, 0.25f),
    /*pillBg*/           hex(0x262626, 0.92f),
    /*wipeLine*/         hex(0xEDEDED, 0.75f),
};
const Metrics M{};
const TypeScale T{};
Fonts F;

float scale() { return g_scale; }
float px(float v) { return std::floor(v * g_scale + 0.5f); }

void applyTheme(float contentScale) {
    g_scale = contentScale > 0.5f ? contentScale : 1.f;
    loadFonts(g_scale);
    applyStyle(g_scale);
}

ImU32 col(const ImVec4& c) { return ImGui::GetColorU32(c); }
ImU32 col(const ImVec4& c, float alpha) { return ImGui::GetColorU32(withAlpha(c, c.w * alpha)); }
ImVec4 withAlpha(const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, a); }

ImVec4 backdropColor(Backdrop b) {
    switch (b) {
        case Backdrop::Gray: return C.bgCanvasGray;
        case Backdrop::White: return C.bgCanvasWhite;
        default: return C.bgCanvasDark;
    }
}

} // namespace ui
