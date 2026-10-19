// Plugin Cookbook — the generic Dear ImGui controls, shared by the plugin editor and the standalone app.
// SPDX-License-Identifier: MIT
//
// Float/int parameters become knobs, bools become toggles, choices become drop-downs.
//   drag           change value (hold Shift for fine control)
//   mouse wheel    nudge value
//   double-click   reset to default
//   hover          show the parameter description
//
// Header-only. In the plugin editor include it after DistrhoUI.hpp (which includes DearImGui.hpp with
// the UI friend declarations it needs).

#pragma once

#include "DearImGui.hpp"

#include "cookbook/ParamInfo.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace cookbook {
namespace ui {

constexpr float kPad = 18.f;
constexpr float kHeaderH = 58.f;
constexpr float kCellW = 118.f;
constexpr float kCellH = 132.f;
constexpr float kKnobSize = 58.f;

inline ImVec4 rgb(uint32_t c, float a = 1.f)
{
    return ImVec4(((c >> 16) & 0xff) / 255.f, ((c >> 8) & 0xff) / 255.f, (c & 0xff) / 255.f, a);
}

inline ImVec4 mix(const ImVec4& a, const ImVec4& b, float t)
{
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

inline float toNormalized(const ParamInfo& p, float v)
{
    if (p.max <= p.min)
        return 0.f;
    if (p.scale == Scale::Log && p.min > 0.f)
        return std::log(v / p.min) / std::log(p.max / p.min);
    return (v - p.min) / (p.max - p.min);
}

inline float fromNormalized(const ParamInfo& p, float t)
{
    t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
    if (p.scale == Scale::Log && p.min > 0.f)
        return p.min * std::pow(p.max / p.min, t);
    return p.min + t * (p.max - p.min);
}

inline void formatValue(const ParamInfo& p, float v, char* buf, size_t size)
{
    switch (p.type)
    {
    case ParamType::Bool:
        std::snprintf(buf, size, "%s", v > 0.5f ? "On" : "Off");
        return;
    case ParamType::Choice: {
        const int i = (int)std::lround(v);
        std::snprintf(buf, size, "%s", (i >= 0 && (uint32_t)i < p.numChoices) ? p.choices[i] : "?");
        return;
    }
    case ParamType::Int:
        std::snprintf(buf, size, p.unit[0] ? "%d %s" : "%d", (int)std::lround(v), p.unit);
        return;
    case ParamType::Float:
        break;
    }

    if (std::strcmp(p.unit, "Hz") == 0 && std::fabs(v) >= 1000.f)
        std::snprintf(buf, size, "%.2f kHz", v / 1000.f);
    else if (std::strcmp(p.unit, "ms") == 0 && std::fabs(v) >= 1000.f)
        std::snprintf(buf, size, "%.2f s", v / 1000.f);
    else if (std::strcmp(p.unit, "%") == 0)
        std::snprintf(buf, size, "%.0f%%", v);
    else
    {
        const float range = std::fabs(p.max - p.min);
        const float mag = std::fabs(v);
        const int decimals = (range >= 100.f || mag >= 100.f) ? 0 : (range >= 10.f || mag >= 10.f) ? 1 : 2;
        if (p.unit[0])
            std::snprintf(buf, size, "%.*f %s", decimals, v, p.unit);
        else
            std::snprintf(buf, size, "%.*f", decimals, v);
    }
}

inline void centeredText(const char* text, float cellX, float cellW, const ImVec4& color)
{
    const float w = ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPosX(cellX + std::max(0.f, (cellW - w) * 0.5f));
    ImGui::TextColored(color, "%s", text);
}

// A knob drawn with ImDrawList: track arc, value arc (from the origin, so bipolar ranges grow from
// their zero point), body and pointer. Operates on a normalized 0..1 value.
inline bool knob(const char* id, float* t, float size, float origin, const ImVec4& accent)
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool active = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();

    bool changed = false;
    if (active)
    {
        const ImGuiIO& io = ImGui::GetIO();
        const float delta = (io.MouseDelta.x - io.MouseDelta.y) / (io.KeyShift ? 2000.f : 200.f);
        if (delta != 0.f)
        {
            const float nt = std::min(1.f, std::max(0.f, *t + delta));
            changed = nt != *t;
            *t = nt;
        }
    }

    constexpr float kMin = 3.14159265f * 0.75f, kMax = 3.14159265f * 2.25f;
    const float r = size * 0.5f;
    const ImVec2 c(pos.x + r, pos.y + r);
    const float aVal = kMin + (kMax - kMin) * *t;
    const float aOrg = kMin + (kMax - kMin) * origin;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiStyle& st = ImGui::GetStyle();

    dl->PathArcTo(c, r * 0.84f, kMin, kMax, 48);
    dl->PathStroke(ImGui::GetColorU32(st.Colors[ImGuiCol_FrameBgActive]), 0, r * 0.14f);
    if (std::fabs(aVal - aOrg) > 0.01f)
    {
        dl->PathArcTo(c, r * 0.84f, std::min(aOrg, aVal), std::max(aOrg, aVal), 48);
        dl->PathStroke(ImGui::GetColorU32(accent), 0, r * 0.14f);
    }

    const ImVec4 body = hovered || active ? st.Colors[ImGuiCol_ButtonHovered] : st.Colors[ImGuiCol_Button];
    dl->AddCircleFilled(c, r * 0.62f, ImGui::GetColorU32(body), 40);
    const ImVec2 dir(std::cos(aVal), std::sin(aVal));
    dl->AddLine(ImVec2(c.x + dir.x * r * 0.18f, c.y + dir.y * r * 0.18f),
                ImVec2(c.x + dir.x * r * 0.56f, c.y + dir.y * r * 0.56f),
                ImGui::GetColorU32(st.Colors[ImGuiCol_Text]), std::max(1.5f, r * 0.08f));
    return changed;
}

inline void applyStyle(uint32_t accentRgb)
{
    const ImVec4 accent = rgb(accentRgb);
    const ImVec4 bg = rgb(0x17181b);
    const ImVec4 panel = rgb(0x24262b);
    const ImVec4 text = rgb(0xe8e6e1);

    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 0.f;
    st.FrameRounding = 4.f;
    st.PopupRounding = 4.f;
    st.WindowBorderSize = 0.f;
    st.FrameBorderSize = 0.f;

    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_PopupBg] = panel;
    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = rgb(0x8a8d93);
    c[ImGuiCol_Border] = rgb(0x34373d);
    c[ImGuiCol_FrameBg] = panel;
    c[ImGuiCol_FrameBgHovered] = rgb(0x2d3036);
    c[ImGuiCol_FrameBgActive] = rgb(0x2e3036);
    // knob body = Button / ButtonHovered, knob track = FrameBgActive
    c[ImGuiCol_Button] = rgb(0x34373e);
    c[ImGuiCol_ButtonHovered] = rgb(0x40434b);
    c[ImGuiCol_ButtonActive] = accent;
    c[ImGuiCol_Header] = mix(panel, accent, 0.35f);
    c[ImGuiCol_HeaderHovered] = mix(panel, accent, 0.55f);
    c[ImGuiCol_HeaderActive] = accent;
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = mix(accent, text, 0.3f);
    c[ImGuiCol_Separator] = mix(bg, accent, 0.6f);
}

// Plugin name on the left, brand on the right, accent rule underneath.
inline void drawHeader(float s, float width, const char* name, const char* brand, uint32_t accentRgb)
{
    const ImVec4 accent = rgb(accentRgb);

    ImGui::SetCursorPos(ImVec2(kPad * s, 14.f * s));
    ImGui::SetWindowFontScale(1.45f);
    ImGui::TextUnformatted(name);
    ImGui::SetWindowFontScale(1.f);

    const float bw = ImGui::CalcTextSize(brand).x;
    ImGui::SetCursorPos(ImVec2(width - kPad * s - bw, 20.f * s));
    ImGui::TextColored(ImGui::GetStyle().Colors[ImGuiCol_TextDisabled], "%s", brand);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const float y = wp.y + (kHeaderH - 8.f) * s;
    dl->AddLine(ImVec2(wp.x + kPad * s, y), ImVec2(wp.x + width - kPad * s, y),
                ImGui::GetColorU32(ImVec4(accent.x, accent.y, accent.z, 0.55f)), 1.f * s);
}

// Where parameter edits go: the plugin host (with gesture bracketing) or the standalone audio engine.
struct ControlsHost
{
    virtual ~ControlsHost() = default;
    virtual void beginEdit(uint32_t index) = 0;
    virtual void setValue(uint32_t index, float value) = 0;
    virtual void endEdit(uint32_t index) = 0;
};

// The parameter grid. `values` mirrors the current parameter values; keep it updated when they change
// from outside (host automation, loaded settings).
template <class Meta>
class ControlsView
{
public:
    static constexpr uint32_t kSize = Meta::kNumParams > 0 ? Meta::kNumParams : 1;

    float values[kSize];

    ControlsView()
    {
        for (uint32_t i = 0; i < Meta::kNumParams; ++i)
            values[i] = Meta::kParams[i].def;
    }

    void draw(const float s, const float width, const float top, const uint32_t accentRgb, const uint32_t maxCols,
              ControlsHost& host)
    {
        if (Meta::kNumParams == 0)
            return;

        const float cellW = kCellW * s;
        const float cellH = kCellH * s;
        const float usable = width - 2.f * kPad * s;
        uint32_t cols = (uint32_t)std::max(1.f, std::floor(usable / cellW));
        cols = std::min<uint32_t>(cols, maxCols);
        cols = std::min<uint32_t>(cols, Meta::kNumParams);

        const float gridW = cols * cellW;
        const float x0 = std::max(kPad * s, (width - gridW) * 0.5f);
        const float y0 = top;

        const ImVec4 accent = rgb(accentRgb);
        const ImVec4 muted = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
        const ImVec4 bright = ImGui::GetStyle().Colors[ImGuiCol_Text];

        for (uint32_t i = 0; i < Meta::kNumParams; ++i)
        {
            const ParamInfo& p = Meta::kParams[i];
            const float cx = x0 + (i % cols) * cellW;
            const float cy = y0 + (i / cols) * cellH;

            ImGui::PushID((int)i);

            ImGui::SetCursorPosY(cy);
            centeredText(p.shortName[0] ? p.shortName : p.name, cx, cellW, muted);

            const float controlTop = cy + 24.f * s;
            ImGui::SetCursorPos(ImVec2(cx + (cellW - kKnobSize * s) * 0.5f, controlTop));

            bool changed = false;
            float newValue = values[i];

            switch (p.type)
            {
            case ParamType::Float:
            case ParamType::Int: {
                // knobs work on a normalized value; while dragging we keep the unrounded position
                if (!editing_[i])
                    dragT_[i] = toNormalized(p, values[i]);
                float t = dragT_[i];
                const float origin = (p.min < 0.f && p.max > 0.f && p.scale == Scale::Linear) ? toNormalized(p, 0.f) : 0.f;
                if (knob("##k", &t, kKnobSize * s, origin, accent))
                {
                    dragT_[i] = t;
                    newValue = fromNormalized(p, t);
                    if (p.type == ParamType::Int)
                        newValue = std::round(newValue);
                    changed = newValue != values[i];
                }
                break;
            }
            case ParamType::Bool: {
                bool v = values[i] > 0.5f;
                const ImVec2 size(46.f * s, 24.f * s);
                ImGui::SetCursorPos(ImVec2(cx + (cellW - size.x) * 0.5f, controlTop + 16.f * s));
                ImGuiTogglePalette onPalette = {};
                onPalette.Frame = accent;
                onPalette.FrameHover = mix(accent, ImGui::GetStyle().Colors[ImGuiCol_Text], 0.15f);
                ImGuiToggleConfig cfg;
                cfg.Flags = ImGuiToggleFlags_Animated;
                cfg.Size = size;
                cfg.On.Palette = &onPalette;
                if (ImGui::Toggle("##t", &v, cfg))
                {
                    newValue = v ? 1.f : 0.f;
                    changed = true;
                }
                break;
            }
            case ParamType::Choice: {
                const int cur = (int)std::lround(values[i]);
                const float comboW = cellW - 14.f * s;
                ImGui::SetCursorPos(ImVec2(cx + (cellW - comboW) * 0.5f, controlTop + 16.f * s));
                ImGui::SetNextItemWidth(comboW);
                const char* preview = (cur >= 0 && (uint32_t)cur < p.numChoices) ? p.choices[cur] : "";
                if (ImGui::BeginCombo("##c", preview))
                {
                    for (uint32_t c = 0; c < p.numChoices; ++c)
                    {
                        const bool selected = (int)c == cur;
                        if (ImGui::Selectable(p.choices[c], selected))
                        {
                            newValue = (float)c;
                            changed = true;
                        }
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                break;
            }
            }

            const bool hovered = ImGui::IsItemHovered();
            const bool active = ImGui::IsItemActive();
            const bool isKnob = p.type == ParamType::Float || p.type == ParamType::Int;

            // double-click resets to default
            if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && p.type != ParamType::Choice)
            {
                newValue = p.def;
                changed = true;
            }

            // mouse wheel nudges knobs
            if (hovered && isKnob && ImGui::GetIO().MouseWheel != 0.f)
            {
                const float step = ImGui::GetIO().MouseWheel * (ImGui::GetIO().KeyShift ? 0.002f : 0.02f);
                if (p.type == ParamType::Int)
                    newValue = values[i] + (ImGui::GetIO().MouseWheel > 0.f ? 1.f : -1.f);
                else
                    newValue = fromNormalized(p, toNormalized(p, values[i]) + step);
                newValue = std::min(std::max(newValue, p.min), p.max);
                changed = true;
            }

            if (hovered && p.description[0] && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                ImGui::SetTooltip("%s", p.description);

            // gesture bracketing: drags are one gesture, clicks/wheel are instant edits
            if (active && !editing_[i])
            {
                editing_[i] = true;
                host.beginEdit(i);
            }
            if (changed)
            {
                const bool instant = !editing_[i];
                if (instant)
                    host.beginEdit(i);
                values[i] = newValue;
                host.setValue(i, newValue);
                if (instant)
                    host.endEdit(i);
            }
            if (!active && editing_[i])
            {
                editing_[i] = false;
                host.endEdit(i);
            }

            if (p.type != ParamType::Choice) // the combo already shows its value
            {
                char buf[64];
                formatValue(p, values[i], buf, sizeof(buf));
                ImGui::SetCursorPosY(controlTop + kKnobSize * s + 8.f * s);
                centeredText(buf, cx, cellW, bright);
            }

            ImGui::PopID();
        }
    }

private:
    bool editing_[kSize] = {};
    float dragT_[kSize] = {};
};

} // namespace ui
} // namespace cookbook
