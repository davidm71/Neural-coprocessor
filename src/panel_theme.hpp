#pragma once
// TH1: the panel's own colours. Until IN54 the panel took ReShade's ImGui
// colours, so it looked like every other ImGui add-on (OptiScaler included).
// This is a graphite-and-teal palette of its own, pushed only around the
// mod's panel - ReShade's own windows keep their colours.
//
// mgpu.ini: PanelTheme=2 (default since IN65) puts these colours on the mod's
// panel and on all of ReShade's windows (TH2), PanelTheme=1 on the mod's panel
// only, PanelTheme=0 keeps ReShade's. Include after <imgui.h> and <reshade.hpp>.

#if defined(MGPU_HAVE_IMGUI)

namespace mgpu
{
namespace theme
{
    inline ImVec4 rgb(int r, int g, int b, float a = 1.0f)
    {
        return ImVec4((float)r / 255.0f, (float)g / 255.0f, (float)b / 255.0f, a);
    }

    struct entry { ImGuiCol c; ImVec4 v; };

    // The window's own frame: background, border, title bar.
    inline const entry *window_palette(int *n)
    {
        static const entry e[] = {
            { ImGuiCol_WindowBg,         rgb(18, 21, 24, 0.97f) },
            { ImGuiCol_Border,           rgb(40, 110, 110, 0.60f) },
            { ImGuiCol_TitleBg,          rgb(16, 38, 40) },
            { ImGuiCol_TitleBgActive,    rgb(18, 84, 84) },
            { ImGuiCol_TitleBgCollapsed, rgb(16, 38, 40, 0.80f) },
        };
        *n = (int)(sizeof e / sizeof e[0]);
        return e;
    }

    // Everything drawn inside a window.
    inline const entry *content_palette(int *n)
    {
        static const entry e[] = {
            { ImGuiCol_Text,                 rgb(222, 230, 228) },
            { ImGuiCol_TextDisabled,         rgb(122, 140, 138) },
            { ImGuiCol_ChildBg,              rgb(18, 21, 24, 0.97f) },
            { ImGuiCol_PopupBg,              rgb(20, 26, 28, 0.98f) },
            { ImGuiCol_Border,               rgb(40, 110, 110, 0.45f) },
            { ImGuiCol_FrameBg,              rgb(30, 42, 44) },
            { ImGuiCol_FrameBgHovered,       rgb(34, 72, 72) },
            { ImGuiCol_FrameBgActive,        rgb(30, 100, 98) },
            { ImGuiCol_Header,               rgb(22, 78, 78) },
            { ImGuiCol_HeaderHovered,        rgb(28, 110, 108) },
            { ImGuiCol_HeaderActive,         rgb(34, 136, 132) },
            { ImGuiCol_Button,               rgb(26, 86, 86) },
            { ImGuiCol_ButtonHovered,        rgb(34, 122, 118) },
            { ImGuiCol_ButtonActive,         rgb(40, 150, 144) },
            { ImGuiCol_CheckMark,            rgb(92, 232, 196) },
            { ImGuiCol_SliderGrab,           rgb(70, 196, 170) },
            { ImGuiCol_SliderGrabActive,     rgb(110, 240, 206) },
            { ImGuiCol_Separator,            rgb(40, 110, 110, 0.55f) },
            { ImGuiCol_SeparatorHovered,     rgb(60, 170, 160) },
            { ImGuiCol_SeparatorActive,      rgb(92, 232, 196) },
            { ImGuiCol_ScrollbarBg,          rgb(14, 17, 19, 0.60f) },
            { ImGuiCol_ScrollbarGrab,        rgb(38, 70, 70) },
            { ImGuiCol_ScrollbarGrabHovered, rgb(46, 100, 98) },
            { ImGuiCol_ScrollbarGrabActive,  rgb(56, 130, 124) },
            { ImGuiCol_ResizeGrip,           rgb(40, 110, 110, 0.30f) },
            { ImGuiCol_ResizeGripHovered,    rgb(60, 170, 160, 0.70f) },
            { ImGuiCol_ResizeGripActive,     rgb(92, 232, 196, 0.90f) },
            { ImGuiCol_TextSelectedBg,       rgb(40, 150, 144, 0.45f) },
            { ImGuiCol_PlotLines,            rgb(92, 232, 196) },
            { ImGuiCol_PlotHistogram,        rgb(70, 196, 170) },
        };
        *n = (int)(sizeof e / sizeof e[0]);
        return e;
    }

    // Extra colours only the whole-ReShade theme (PanelTheme=2) needs: tabs,
    // docking, menus and table lines that ReShade's own windows use.
    inline const entry *global_extra_palette(int *n)
    {
        static const entry e[] = {
            { ImGuiCol_MenuBarBg,            rgb(16, 30, 32) },
            { ImGuiCol_Tab,                  rgb(20, 56, 56) },
            { ImGuiCol_TabHovered,           rgb(28, 110, 108) },
            { ImGuiCol_TabSelected,          rgb(24, 92, 90) },
            { ImGuiCol_TabDimmed,            rgb(18, 36, 38) },
            { ImGuiCol_TabDimmedSelected,    rgb(20, 60, 60) },
            { ImGuiCol_DockingPreview,       rgb(40, 150, 144, 0.60f) },
            { ImGuiCol_DockingEmptyBg,       rgb(14, 17, 19) },
            { ImGuiCol_TableHeaderBg,        rgb(20, 48, 50) },
            { ImGuiCol_TableBorderStrong,    rgb(40, 110, 110, 0.60f) },
            { ImGuiCol_TableBorderLight,     rgb(40, 110, 110, 0.30f) },
            { ImGuiCol_NavCursor,            rgb(92, 232, 196) },
        };
        *n = (int)(sizeof e / sizeof e[0]);
        return e;
    }

    inline int push(const entry *e, int n)
    {
        for (int i = 0; i < n; ++i) ImGui::PushStyleColor(e[i].c, e[i].v);
        return n;
    }

    // Pushed before Begin() of the panel's own window; returns how many to pop.
    inline int push_window() { int n = 0; const entry *e = window_palette(&n); return push(e, n); }

    // Everything drawn inside the panel. Returns how many to pop.
    inline int push_content() { int n = 0; const entry *e = content_palette(&n); return push(e, n); }

    // TH2 (PanelTheme=2): the same colours written into the style of the ImGui
    // context that is current - that runtime's ReShade overlay - so ReShade's
    // own windows (Home, Add-ons, Settings...) and every other add-on match.
    // Written each time the panel or the overlay is drawn; ReShade's own style
    // is not saved over unless its style editor saves it.
    inline void apply_global()
    {
        ImGuiStyle &st = ImGui::GetStyle();
        int n = 0;
        const entry *e = window_palette(&n);
        for (int i = 0; i < n; ++i) st.Colors[e[i].c] = e[i].v;
        e = content_palette(&n);
        for (int i = 0; i < n; ++i) if (e[i].c != ImGuiCol_Border) st.Colors[e[i].c] = e[i].v;
        e = global_extra_palette(&n);
        for (int i = 0; i < n; ++i) st.Colors[e[i].c] = e[i].v;
    }
}
}

#endif
