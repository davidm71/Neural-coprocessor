#pragma once
// ---------------------------------------------------------------------------
// panel_anim.hpp - PA1: game-menu style motion for the MGPU panel.
//
//   - the panel fades in and slides in from the right when it opens
//   - a section's contents fade in when it is opened
//   - a teal ripple around a control when it is clicked
//   - a title banner with a pulsing status dot (green running, amber starting
//     or waiting, red failed / off) and a slow teal sheen
//
// mgpu.ini PanelAnimations=1 (default) / 0 off. Everything here runs only
// while the panel is drawn, on the thread drawing it, and costs a few draw
// commands. Include after <imgui.h>, <reshade.hpp> and panel_theme.hpp.
// ---------------------------------------------------------------------------
#if defined(MGPU_HAVE_IMGUI)

#include <cmath>

namespace mgpu
{
namespace anim
{
    struct state
    {
        int    on = -1;            // -1: not read yet
        double last_draw = -1.0;   // ImGui time of the last drawn panel frame
        double open_t = -1.0;      // when this opening began
        float  alpha = 1.0f;       // the panel's alpha this frame (open fade)
        double sec_t[16] = {};     // when each section was last opened
        bool   sec_pushed = false; // a section fade's style var is pushed
        ImVec2 rip_min, rip_max;   // the last clicked control
        double rip_t = -1.0;
    };
    inline state &st() { static state s; return s; }

    inline float ease_out(float k) { k = k < 0.0f ? 0.0f : (k > 1.0f ? 1.0f : k); const float u = 1.0f - k; return 1.0f - u * u * u; }

    // At the top of the panel body. Returns the slide offset (pixels) for this
    // frame; pushes the fade alpha (pop with frame_end).
    inline float frame_begin(bool on)
    {
        state &s = st();
        const double now = ImGui::GetTime();
        if (s.last_draw < 0.0 || now - s.last_draw > 0.25) s.open_t = now;   // closed since: a new opening
        s.last_draw = now;
        if (!on) { s.alpha = 1.0f; return 0.0f; }
        const float e = ease_out((float)((now - s.open_t) / 0.30));
        s.alpha = 0.15f + 0.85f * e;
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, s.alpha);
        return (1.0f - e) * 48.0f;
    }

    inline void ripple_draw(bool on)
    {
        state &s = st();
        if (!on || s.rip_t < 0.0) return;
        const float k = (float)((ImGui::GetTime() - s.rip_t) / 0.35);
        if (k >= 1.0f) { s.rip_t = -1.0; return; }
        const float g = 2.0f + 8.0f * ease_out(k);
        const int a = (int)(170.0f * (1.0f - k));
        ImGui::GetWindowDrawList()->AddRect(ImVec2(s.rip_min.x - g, s.rip_min.y - g),
                                            ImVec2(s.rip_max.x + g, s.rip_max.y + g),
                                            IM_COL32(92, 232, 196, a), 3.0f + g * 0.5f, 0, 2.0f);
    }

    inline void frame_end(bool on)
    {
        state &s = st();
        if (s.sec_pushed) { ImGui::PopStyleVar(); s.sec_pushed = false; }
        ripple_draw(on);
        if (on) ImGui::PopStyleVar();
    }

    // After a control: a click (or a keyboard change) starts the ripple.
    inline void note_item(bool on, bool changed)
    {
        if (!on) return;
        if (ImGui::IsItemActivated() || (changed && !ImGui::IsItemActive()))
        {
            state &s = st();
            s.rip_min = ImGui::GetItemRectMin(); s.rip_max = ImGui::GetItemRectMax(); s.rip_t = ImGui::GetTime();
        }
    }

    // A section header was drawn: opened now -> its contents fade in.
    inline void section(bool on, int id, bool open, bool was)
    {
        state &s = st();
        if (s.sec_pushed) { ImGui::PopStyleVar(); s.sec_pushed = false; }
        if (!on || id < 0 || id >= 16) return;
        const double now = ImGui::GetTime();
        if (ImGui::IsItemClicked())
        { s.rip_min = ImGui::GetItemRectMin(); s.rip_max = ImGui::GetItemRectMax(); s.rip_t = now; }
        if (open && !was) s.sec_t[id] = now;
        if (!open) return;
        const float k = (float)((now - s.sec_t[id]) / 0.28);
        if (k >= 1.0f) return;
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, s.alpha * (0.1f + 0.9f * ease_out(k)));
        s.sec_pushed = true;
    }
    // A section's body ended (the next header, or the panel's end).
    inline void section_end()
    {
        state &s = st();
        if (s.sec_pushed) { ImGui::PopStyleVar(); s.sec_pushed = false; }
    }

    // The title banner: pulsing status dot, name, state word and build tag, a
    // slow sheen. bstate: 0 waiting, 1 starting, 2 running, 3 failed / off.
    // PO1: reserve > 0 keeps that much of the bar's right end free (the
    // "ReShade menu" button sits there); returns the bar's top-right corner.
    inline ImVec2 banner(bool on, int bstate, const char *tag, float reserve = 0.0f)
    {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const float lh = ImGui::GetTextLineHeight();
        const float h = lh + 10.0f;
        const ImVec2 q(p.x + w, p.y + h);
        dl->AddRectFilled(p, q, IM_COL32(18, 84, 84, 255), 3.0f);
        const double t = ImGui::GetTime();
        if (on)
        {
            // sheen: sweeps across in 0.9 s, every 4.5 s
            const float ph = (float)std::fmod(t, 4.5) / 0.9f;
            if (ph < 1.0f)
            {
                const float x0 = p.x - w * 0.3f + (w * 1.6f) * ph, sw = w * 0.18f;
                dl->PushClipRect(p, q, true);
                dl->AddQuadFilled(ImVec2(x0 + h * 0.4f, p.y), ImVec2(x0 + sw + h * 0.4f, p.y),
                                  ImVec2(x0 + sw, q.y), ImVec2(x0, q.y), IM_COL32(120, 240, 210, 34));
                dl->PopClipRect();
            }
        }
        ImU32 c = IM_COL32(46, 207, 122, 255);
        const char *word = "RUNNING";
        if (bstate == 0) { c = IM_COL32(232, 176, 60, 255); word = "WAITING"; }
        else if (bstate == 1) { c = IM_COL32(232, 176, 60, 255); word = "STARTING"; }
        else if (bstate == 3) { c = IM_COL32(226, 75, 74, 255); word = "OFF / FAILED"; }
        const ImVec2 dc(p.x + 12.0f, p.y + h * 0.5f);
        if (on)
        {
            const float k = (float)std::fmod(t, 1.8) / 1.8f;   // ring grows and fades
            dl->AddCircle(dc, 4.5f + 6.0f * k, (c & 0x00FFFFFFu) | ((ImU32)(140.0f * (1.0f - k)) << 24), 20, 1.5f);
        }
        dl->AddCircleFilled(dc, 4.5f, c, 16);
        dl->AddText(ImVec2(p.x + 24.0f, p.y + 5.0f), IM_COL32(222, 230, 228, 255), "Atomic MGPU Bridge");
        char r[48];
        snprintf(r, sizeof r, "%s  %s", word, tag != nullptr ? tag : "");
        const float rw = ImGui::CalcTextSize(r).x;
        dl->AddText(ImVec2(q.x - reserve - rw - 8.0f, p.y + 5.0f), IM_COL32(150, 200, 190, 255), r);
        ImGui::Dummy(ImVec2(w, h + 4.0f));
        return ImVec2(q.x, p.y);
    }
}
}
#endif
