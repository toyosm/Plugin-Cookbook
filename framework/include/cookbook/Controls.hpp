// Plugin Cookbook — the generic Dear ImGui controls, shared by the plugin editor and the standalone app.
// SPDX-License-Identifier: MIT
//
// Float/int parameters become knobs or sliders, bools toggles or buttons, choices drop-downs or button rows.
// Controls are laid out in rows by group ("flow"), or each at its own position over a background image
// ("free"); knobs can be drawn by code or from an image (rotating, or a filmstrip of frames).
//   drag           change value (hold Shift for fine control)
//   mouse wheel    nudge value
//   double-click   reset to default
//   hover          show the parameter description
//
// Header-only. In the plugin editor include it after DistrhoUI.hpp (which includes DearImGui.hpp with
// the UI friend declarations it needs).

#pragma once

#include "DearImGui.hpp"
#include "OpenGL-include.hpp"

#include "cookbook/ParamInfo.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

// PNG decoding: DPF's dgl library already contains stb_image (from NanoVG), with these names.
extern "C" unsigned char* dpf_stbi_load_from_memory(const unsigned char* buffer, int len, int* x, int* y,
                                                    int* channels, int desiredChannels);
extern "C" void dpf_stbi_image_free(void* data);

namespace cookbook {
namespace ui {

// Editor geometry in unscaled pixels. tools/recipe.py (window size) and the Plugin Studio preview
// (tools/studio/static/index.html) use the same numbers and layout rules: keep all three in sync.
constexpr float kPad = 18.f;
constexpr float kHeaderH = 58.f;
constexpr float kSubtitleH = 16.f;
constexpr float kTitleH = 28.f;
constexpr float kGroupGap = 6.f;
constexpr float kCellW = 118.f;
constexpr float kCellH = 132.f;
constexpr float kKnobSize = 58.f;

inline float headerHeight(const char* subtitle)
{
    return kHeaderH + (subtitle && subtitle[0] ? kSubtitleH : 0.f);
}

inline float knobDiameter(Control c)
{
    return c == Control::SmallKnob ? 40.f : c == Control::LargeKnob ? 88.f : kKnobSize;
}

inline uint32_t utf8Length(const char* s)
{
    uint32_t n = 0;
    for (; *s; ++s)
        n += ((unsigned char)*s & 0xC0) != 0x80;
    return n;
}

// Unscaled cell size of a parameter's control.
inline ImVec2 cellSize(const ParamInfo& p)
{
    switch (p.control)
    {
    case Control::SmallKnob: return ImVec2(92.f, 110.f);
    case Control::LargeKnob: return ImVec2(150.f, 168.f);
    case Control::Slider: return ImVec2(236.f, 84.f);
    case Control::Buttons: {
        uint32_t longest = 0;
        for (uint32_t c = 0; c < p.numChoices; ++c)
            longest = std::max(longest, utf8Length(p.choices[c]));
        const float w = 16.f + p.numChoices * (longest * 7.f + 16.f);
        return ImVec2(std::min(472.f, std::max(118.f, w)), 84.f);
    }
    default: return ImVec2(kCellW, kCellH);
    }
}

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

inline ImVec4 lighten(const ImVec4& c, float t) { return mix(c, ImVec4(1, 1, 1, c.w), t); }
inline ImVec4 darken(const ImVec4& c, float t) { return mix(c, ImVec4(0, 0, 0, c.w), t); }

// A knob drawn with ImDrawList on a normalized 0..1 value; the value arc grows from `origin`, so bipolar
// ranges grow from their zero point. bodyColor is 0xRRGGBB or -1 for the style's own; ticks adds 11 scale
// marks. The Plugin Studio preview draws the same shapes: keep them in sync.
//   Modern   track and value arcs around a round body with a line pointer
//   Vintage  cream body with a chicken-head pointer and scale marks
//   Flat     a thick arc with a dot at the value, no body
//   Minimal  an outlined body, thin arcs and an accent pointer
//   Metal    brushed-metal body with a knurled edge and a thin value ring
inline bool knob(const char* id, float* t, float size, float origin, const ImVec4& accent,
                 KnobStyle style = KnobStyle::Modern, int64_t bodyColor = -1, bool ticks = false)
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
    const ImVec2 dir(std::cos(aVal), std::sin(aVal));
    const ImVec2 perp(-dir.y, dir.x);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiStyle& st = ImGui::GetStyle();
    const ImU32 track = ImGui::GetColorU32(st.Colors[ImGuiCol_FrameBgActive]);
    const ImU32 acc = ImGui::GetColorU32(accent);
    const ImU32 text = ImGui::GetColorU32(st.Colors[ImGuiCol_Text]);
    const float hot = hovered || active ? 0.08f : 0.f;
    auto at = [&](float radius, float angle) { return ImVec2(c.x + std::cos(angle) * radius, c.y + std::sin(angle) * radius); };
    auto arc = [&](float radius, float a0, float a1, ImU32 col, float thickness) {
        if (std::fabs(a1 - a0) < 0.01f)
            return;
        dl->PathArcTo(c, radius, std::min(a0, a1), std::max(a0, a1), 48);
        dl->PathStroke(col, 0, thickness);
    };
    auto drawTicks = [&](float r0, float r1) {
        const ImU32 col = ImGui::GetColorU32(st.Colors[ImGuiCol_TextDisabled]);
        for (int k = 0; k <= 10; ++k)
        {
            const float a = kMin + (kMax - kMin) * k / 10.f;
            dl->AddLine(at(r0, a), at(r1, a), col, std::max(1.f, r * 0.045f));
        }
    };

    switch (style)
    {
    case KnobStyle::Modern: {
        if (ticks)
            drawTicks(r * 0.94f, r * 1.0f);
        arc(r * 0.84f, kMin, kMax, track, r * 0.14f);
        arc(r * 0.84f, aOrg, aVal, acc, r * 0.14f);
        const ImVec4 body = bodyColor >= 0 ? lighten(rgb((uint32_t)bodyColor), hot)
                                           : (hot > 0.f ? st.Colors[ImGuiCol_ButtonHovered] : st.Colors[ImGuiCol_Button]);
        dl->AddCircleFilled(c, r * 0.62f, ImGui::GetColorU32(body), 40);
        dl->AddLine(at(r * 0.18f, aVal), at(r * 0.56f, aVal), text, std::max(1.5f, r * 0.08f));
        break;
    }
    case KnobStyle::Vintage: {
        drawTicks(r * 0.84f, r * 0.98f);
        const ImVec4 body = lighten(bodyColor >= 0 ? rgb((uint32_t)bodyColor) : rgb(0xe8dcc0), hot);
        dl->AddCircleFilled(c, r * 0.74f, ImGui::GetColorU32(darken(body, 0.55f)), 40);
        dl->AddCircleFilled(c, r * 0.68f, ImGui::GetColorU32(body), 40);
        // chicken-head pointer: a tapered bar through the center
        const ImU32 head = ImGui::GetColorU32(rgb(0x2a2420));
        const ImVec2 tip(c.x + dir.x * r * 0.78f, c.y + dir.y * r * 0.78f);
        const ImVec2 tail(c.x - dir.x * r * 0.34f, c.y - dir.y * r * 0.34f);
        const ImVec2 left(c.x + perp.x * r * 0.17f, c.y + perp.y * r * 0.17f);
        const ImVec2 right(c.x - perp.x * r * 0.17f, c.y - perp.y * r * 0.17f);
        dl->AddQuadFilled(tip, left, tail, right, head);
        dl->AddCircleFilled(c, r * 0.17f, head, 24);
        dl->AddLine(at(r * 0.2f, aVal), at(r * 0.7f, aVal), ImGui::GetColorU32(body), std::max(1.f, r * 0.05f));
        break;
    }
    case KnobStyle::Flat: {
        if (ticks)
            drawTicks(r * 0.94f, r * 1.0f);
        arc(r * 0.76f, kMin, kMax, track, r * 0.24f);
        arc(r * 0.76f, aOrg, aVal, acc, r * 0.24f);
        dl->AddCircleFilled(at(r * 0.76f, aVal), r * 0.15f, hot > 0.f ? ImGui::GetColorU32(lighten(accent, 0.3f)) : text, 20);
        break;
    }
    case KnobStyle::Minimal: {
        if (ticks)
            drawTicks(r * 0.94f, r * 1.0f);
        arc(r * 0.86f, kMin, kMax, track, std::max(1.f, r * 0.06f));
        arc(r * 0.86f, aOrg, aVal, acc, std::max(1.5f, r * 0.08f));
        const ImVec4 ring = bodyColor >= 0 ? rgb((uint32_t)bodyColor) : st.Colors[ImGuiCol_TextDisabled];
        dl->AddCircle(c, r * 0.66f, ImGui::GetColorU32(lighten(ring, hot)), 40, std::max(1.f, r * 0.05f));
        dl->AddLine(c, at(r * 0.6f, aVal), acc, std::max(1.5f, r * 0.08f));
        dl->AddCircleFilled(c, r * 0.08f, acc, 12);
        break;
    }
    case KnobStyle::Metal: {
        if (ticks)
            drawTicks(r * 0.94f, r * 1.0f);
        arc(r * 0.9f, kMin, kMax, track, std::max(1.f, r * 0.06f));
        arc(r * 0.9f, aOrg, aVal, acc, std::max(1.5f, r * 0.08f));
        const ImVec4 base = bodyColor >= 0 ? rgb((uint32_t)bodyColor) : rgb(0xb4b8bf);
        dl->AddCircleFilled(c, r * 0.8f, ImGui::GetColorU32(darken(base, 0.62f)), 40);
        // knurled edge
        const ImU32 knurl = ImGui::GetColorU32(darken(base, 0.4f));
        for (int k = 0; k < 40; ++k)
        {
            const float a = 6.2831853f * k / 40.f;
            dl->AddLine(at(r * 0.7f, a), at(r * 0.79f, a), knurl, std::max(1.f, r * 0.04f));
        }
        // a light from the top left: concentric discs drifting towards it
        constexpr int kSteps = 10;
        for (int k = 0; k <= kSteps; ++k)
        {
            const float f = k / (float)kSteps;
            const float rr = r * (0.68f - 0.4f * f);
            const ImVec2 cc(c.x - r * 0.12f * f, c.y - r * 0.12f * f);
            dl->AddCircleFilled(cc, rr, ImGui::GetColorU32(lighten(darken(base, 0.25f - 0.25f * f), 0.45f * f + hot)), 36);
        }
        dl->AddLine(at(r * 0.16f, aVal), at(r * 0.64f, aVal), ImGui::GetColorU32(rgb(0x1e1f23)), std::max(1.5f, r * 0.07f));
        break;
    }
    }
    return changed;
}

// background: 0xRRGGBB, or -1 for the theme's own.
inline void applyStyle(uint32_t accentRgb, bool light = false, int64_t background = -1)
{
    const ImVec4 accent = rgb(accentRgb);
    const ImVec4 bg = background >= 0 ? rgb((uint32_t)background) : rgb(light ? 0xf2f1ee : 0x17181b);
    const ImVec4 panel = rgb(light ? 0xffffff : 0x24262b);
    const ImVec4 text = rgb(light ? 0x1e1f23 : 0xe8e6e1);

    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 0.f;
    st.FrameRounding = 4.f;
    st.PopupRounding = 4.f;
    st.WindowBorderSize = 0.f;
    st.FrameBorderSize = 0.f;

    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = panel;
    c[ImGuiCol_MenuBarBg] = mix(bg, text, 0.035f); // standalone app's bottom bar
    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = rgb(light ? 0x6b6e75 : 0x8a8d93);
    c[ImGuiCol_Border] = rgb(light ? 0xd6d4cf : 0x34373d);
    c[ImGuiCol_FrameBg] = light ? rgb(0xe4e2dd) : panel;
    c[ImGuiCol_FrameBgHovered] = rgb(light ? 0xdad8d3 : 0x2d3036);
    c[ImGuiCol_FrameBgActive] = rgb(light ? 0xd6d4cf : 0x2e3036);
    // knob body = Button / ButtonHovered, knob/slider track = FrameBgActive
    c[ImGuiCol_Button] = rgb(light ? 0xe4e2dd : 0x34373e);
    c[ImGuiCol_ButtonHovered] = rgb(light ? 0xd9d7d2 : 0x40434b);
    c[ImGuiCol_ButtonActive] = accent;
    c[ImGuiCol_Header] = mix(panel, accent, 0.35f);
    c[ImGuiCol_HeaderHovered] = mix(panel, accent, 0.55f);
    c[ImGuiCol_HeaderActive] = accent;
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = mix(accent, text, 0.3f);
    c[ImGuiCol_Separator] = mix(bg, accent, 0.6f);
}

// Text on top of the accent color: dark or light, whichever reads better.
inline ImU32 onAccent(uint32_t accentRgb)
{
    const float lum = 0.2126f * ((accentRgb >> 16) & 0xff) + 0.7152f * ((accentRgb >> 8) & 0xff) + 0.0722f * (accentRgb & 0xff);
    return ImGui::GetColorU32(lum > 140.f ? rgb(0x17181b) : rgb(0xffffff));
}

// Plugin name on the left, brand on the right, optional subtitle under the name, accent rule underneath.
inline void drawHeader(float s, float width, const char* name, const char* brand, const char* subtitle,
                       uint32_t accentRgb)
{
    const ImVec4 accent = rgb(accentRgb);
    const ImVec4 muted = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];

    ImGui::SetCursorPos(ImVec2(kPad * s, 14.f * s));
    ImGui::SetWindowFontScale(1.45f);
    ImGui::TextUnformatted(name);
    ImGui::SetWindowFontScale(1.f);

    const float bw = ImGui::CalcTextSize(brand).x;
    ImGui::SetCursorPos(ImVec2(width - kPad * s - bw, 20.f * s));
    ImGui::TextColored(muted, "%s", brand);

    if (subtitle && subtitle[0])
    {
        ImGui::SetCursorPos(ImVec2(kPad * s, 38.f * s));
        ImGui::TextColored(muted, "%s", subtitle);
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const float y = wp.y + (headerHeight(subtitle) - 8.f) * s;
    dl->AddLine(ImVec2(wp.x + kPad * s, y), ImVec2(wp.x + width - kPad * s, y),
                ImGui::GetColorU32(ImVec4(accent.x, accent.y, accent.z, 0.55f)), 1.f * s);
}

// A horizontal fader on a normalized 0..1 value. Click or drag to set; hold Shift for fine control.
inline bool slider(const char* id, float* t, float w, float h, float origin, const ImVec4& accent)
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool active = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();

    bool changed = false;
    if (active)
    {
        const ImGuiIO& io = ImGui::GetIO();
        float nt = io.KeyShift ? *t + io.MouseDelta.x / (w * 10.f) : (io.MousePos.x - pos.x) / w;
        nt = std::min(1.f, std::max(0.f, nt));
        changed = nt != *t;
        *t = nt;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiStyle& st = ImGui::GetStyle();
    const float cy = pos.y + h * 0.5f;
    const float th = std::max(3.f, h * 0.22f);
    dl->AddRectFilled(ImVec2(pos.x, cy - th * 0.5f), ImVec2(pos.x + w, cy + th * 0.5f),
                      ImGui::GetColorU32(st.Colors[ImGuiCol_FrameBgActive]), th * 0.5f);
    const float xv = pos.x + w * *t, xo = pos.x + w * origin;
    if (std::fabs(xv - xo) > 0.5f)
        dl->AddRectFilled(ImVec2(std::min(xv, xo), cy - th * 0.5f), ImVec2(std::max(xv, xo), cy + th * 0.5f),
                          ImGui::GetColorU32(accent), th * 0.5f);
    const ImVec4 body = hovered || active ? st.Colors[ImGuiCol_ButtonHovered] : st.Colors[ImGuiCol_Button];
    dl->AddCircleFilled(ImVec2(xv, cy), h * 0.42f, ImGui::GetColorU32(body), 24);
    dl->AddCircle(ImVec2(xv, cy), h * 0.42f, ImGui::GetColorU32(accent), 24, std::max(1.f, h * 0.07f));
    return changed;
}

// Where parameter edits go: the plugin host (with gesture bracketing) or the standalone audio engine.
struct ControlsHost
{
    virtual ~ControlsHost() = default;
    virtual void beginEdit(uint32_t index) = 0;
    virtual void setValue(uint32_t index, float value) = 0;
    virtual void endEdit(uint32_t index) = 0;
};

// The recipe's images as OpenGL textures, loaded the first time they are drawn (a GL context is current
// then). Call release() while the context is still current, before the editor goes away.
class ImageSet
{
public:
    struct Image
    {
        GLuint texture = 0;
        int width = 0, height = 0;
        bool tried = false;
    };

    ImageSet(const Asset* assets, uint32_t count) : assets_(assets), count_(std::min<uint32_t>(count, kMax)) {}

    const Image* get(int32_t index)
    {
        if (index < 0 || (uint32_t)index >= count_)
            return nullptr;
        Image& img = images_[index];
        if (!img.tried)
        {
            img.tried = true;
            load(assets_[index], img);
        }
        return img.texture ? &img : nullptr;
    }

    void release()
    {
        for (uint32_t i = 0; i < count_; ++i)
            if (images_[i].texture)
            {
                glDeleteTextures(1, &images_[i].texture);
                images_[i] = Image();
            }
    }

    static ImTextureID id(const Image& img) { return (ImTextureID)(intptr_t)img.texture; }

private:
    static constexpr uint32_t kMax = 64;

    static void load(const Asset& asset, Image& img)
    {
        int w = 0, h = 0, channels = 0;
        unsigned char* pixels = dpf_stbi_load_from_memory(asset.data, (int)asset.size, &w, &h, &channels, 4);
        if (!pixels)
            return;
#ifndef GL_CLAMP_TO_EDGE
# define GL_CLAMP_TO_EDGE 0x812F
#endif
        GLint previous = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
        glGenTextures(1, &img.texture);
        glBindTexture(GL_TEXTURE_2D, img.texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glBindTexture(GL_TEXTURE_2D, (GLuint)previous);
        dpf_stbi_image_free(pixels);
        img.width = w;
        img.height = h;
    }

    const Asset* assets_;
    uint32_t count_;
    Image images_[kMax];
};

// The drag behaviour every knob shares. Returns true when *t changed.
inline bool knobDrag(float* t)
{
    if (!ImGui::IsItemActive())
        return false;
    const ImGuiIO& io = ImGui::GetIO();
    const float delta = (io.MouseDelta.x - io.MouseDelta.y) / (io.KeyShift ? 2000.f : 200.f);
    if (delta == 0.f)
        return false;
    const float nt = std::min(1.f, std::max(0.f, *t + delta));
    const bool changed = nt != *t;
    *t = nt;
    return changed;
}

// A knob drawn from an image: with frames <= 1 the image rotates (pointing up = middle of the range,
// 135 degrees each way); otherwise it is a filmstrip of `frames` equal frames, stacked vertically (or
// side by side when the image is wider than tall), first frame = minimum.
inline bool imageKnob(const char* id, float* t, float size, const ImageSet::Image& img, uint32_t frames)
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool changed = knobDrag(t);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImTextureID tex = ImageSet::id(img);
    if (frames <= 1)
    {
        const float a = (*t - 0.5f) * 4.71238898f; // +-135 degrees
        const float cs = std::cos(a), sn = std::sin(a), h = size * 0.5f;
        const ImVec2 c(pos.x + h, pos.y + h);
        auto corner = [&](float x, float y) { return ImVec2(c.x + x * cs - y * sn, c.y + x * sn + y * cs); };
        dl->AddImageQuad(tex, corner(-h, -h), corner(h, -h), corner(h, h), corner(-h, h));
    }
    else
    {
        const float f = std::round(*t * (frames - 1));
        const bool vertical = img.height >= img.width;
        const ImVec2 uv0 = vertical ? ImVec2(0.f, f / frames) : ImVec2(f / frames, 0.f);
        const ImVec2 uv1 = vertical ? ImVec2(1.f, (f + 1.f) / frames) : ImVec2((f + 1.f) / frames, 1.f);
        dl->AddImage(tex, pos, ImVec2(pos.x + size, pos.y + size), uv0, uv1);
    }
    return changed;
}

// The parameter controls. `values` mirrors the current parameter values; keep it updated when they change
// from outside (host automation, loaded settings).
//
// Flow layout: groups stack top to bottom. A titled group starts with its title. Within a group, controls
// fill rows left to right; a row ends at the group's column limit or when the next control would not fit
// the window. Rows are left-aligned inside a block that is centered in the window.
//
// Free layout: each control sits at its own box (Meta::kUiParams) over the background image; the whole
// design scales uniformly to fit the window, centered.
template <class Meta>
class ControlsView
{
public:
    static constexpr uint32_t kSize = Meta::kNumParams > 0 ? Meta::kNumParams : 1;

    float values[kSize];

    explicit ControlsView(const Asset* assets = nullptr, uint32_t numAssets = 0)
        : images_(assets, numAssets)
    {
        for (uint32_t i = 0; i < Meta::kNumParams; ++i)
            values[i] = Meta::kParams[i].def;
    }

    void releaseImages() { images_.release(); }

    // Free layout: the scale and top-left of the design inside a width x height area.
    static void freeTransform(float width, float height, float& scale, ImVec2& origin)
    {
        scale = std::min(width / Meta::kBaseWidth, height / Meta::kBaseHeight);
        origin = ImVec2((width - Meta::kBaseWidth * scale) * 0.5f, (height - Meta::kBaseHeight * scale) * 0.5f);
    }

    // Draws the background image, if any, under everything else. Flow layout: it covers the window
    // (cropped to keep its proportions). Free layout: it is the design itself.
    void drawBackground(const float width, const float height)
    {
        const ImageSet::Image* img = images_.get(Meta::kBackgroundImage);
        if (!img)
            return;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 wp = ImGui::GetWindowPos();
        if (Meta::kFreeLayout)
        {
            float sc;
            ImVec2 o;
            freeTransform(width, height, sc, o);
            dl->AddImage(ImageSet::id(*img), ImVec2(wp.x + o.x, wp.y + o.y),
                         ImVec2(wp.x + o.x + Meta::kBaseWidth * sc, wp.y + o.y + Meta::kBaseHeight * sc));
            return;
        }
        const float ia = (float)img->width / (float)img->height, wa = width / std::max(1.f, height);
        ImVec2 uv0(0, 0), uv1(1, 1);
        if (ia > wa)
        {
            const float k = wa / ia;
            uv0.x = (1.f - k) * 0.5f;
            uv1.x = 1.f - uv0.x;
        }
        else
        {
            const float k = ia / wa;
            uv0.y = (1.f - k) * 0.5f;
            uv1.y = 1.f - uv0.y;
        }
        dl->AddImage(ImageSet::id(*img), wp, ImVec2(wp.x + width, wp.y + height), uv0, uv1);
    }

    // Flow layout: controls from `top` down. Free layout: `top` is ignored; `height` is the area's height.
    // `s` is the display scale factor (HiDPI).
    void draw(const float s, const float width, const float top, const uint32_t accentRgb, ControlsHost& host,
              const float height = 0.f)
    {
        if (Meta::kFreeLayout)
            drawFree(s, width, height, accentRgb, host);
        else
            drawFlow(s, width, top, accentRgb, host);
    }

private:
    void drawFree(const float s, const float width, const float height, const uint32_t accentRgb,
                  ControlsHost& host)
    {
        float sc;
        ImVec2 o;
        freeTransform(width, height, sc, o);
        ImGui::SetWindowFontScale(sc / s);
        const ImVec4 muted = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
        const ImVec4 bright = ImGui::GetStyle().Colors[ImGuiCol_Text];
        for (uint32_t i = 0; i < Meta::kNumParams; ++i)
        {
            const ParamInfo& p = Meta::kParams[i];
            const UiParam& u = Meta::kUiParams[i];
            const float bx = o.x + u.x * sc, by = o.y + u.y * sc, bw = u.w * sc, bh = u.h * sc;
            if (u.label)
            {
                ImGui::SetCursorPosY(by - 20.f * sc);
                centeredText(p.shortName[0] ? p.shortName : p.name, bx, bw, muted);
            }
            const bool hasValue = drawControl(i, bx, by, bw, bh, sc, accentRgb, host);
            if (u.value && hasValue)
            {
                char buf[64];
                formatValue(p, values[i], buf, sizeof(buf));
                ImGui::SetCursorPosY(by + bh + 6.f * sc);
                centeredText(buf, bx, bw, bright);
            }
        }
        ImGui::SetWindowFontScale(1.f);
    }

    void drawFlow(const float s, const float width, const float top, const uint32_t accentRgb, ControlsHost& host)
    {
        const float avail = width - 2.f * kPad * s;
        const ImVec4 muted = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
        float y = top;

        for (uint32_t g = 0; g < Meta::kNumGroups; ++g)
        {
            const UiGroup& group = Meta::kGroups[g];
            if (group.numParams == 0)
                continue;

            if (group.title[0])
            {
                ImGui::SetCursorPos(ImVec2(kPad * s, y + 6.f * s));
                ImGui::TextColored(muted, "%s", group.title);
                const float tw = ImGui::CalcTextSize(group.title).x;
                const ImVec2 wp = ImGui::GetWindowPos();
                const float ly = wp.y + y + 6.f * s + ImGui::GetTextLineHeight() * 0.5f;
                ImGui::GetWindowDrawList()->AddLine(ImVec2(wp.x + kPad * s + tw + 8.f * s, ly),
                                                    ImVec2(wp.x + width - kPad * s, ly),
                                                    ImGui::GetColorU32(ImGui::GetStyle().Colors[ImGuiCol_Border]),
                                                    1.f * s);
                y += kTitleH * s;
            }

            // first pass: row breaks and the widest row
            uint32_t rowStart[kSize + 1];
            uint32_t numRows = 0, inRow = 0;
            float blockW = 0.f, rowW = 0.f;
            for (uint32_t k = 0; k < group.numParams; ++k)
            {
                const float w = cellSize(Meta::kParams[group.params[k]]).x * s;
                if (inRow > 0 && (inRow >= group.columns || rowW + w > avail))
                {
                    blockW = std::max(blockW, rowW);
                    rowW = 0.f;
                    inRow = 0;
                }
                if (inRow == 0)
                    rowStart[numRows++] = k;
                rowW += w;
                ++inRow;
            }
            blockW = std::max(blockW, rowW);
            rowStart[numRows] = group.numParams;

            const float x0 = std::max(kPad * s, (width - blockW) * 0.5f);
            for (uint32_t r = 0; r < numRows; ++r)
            {
                float x = x0, rowH = 0.f;
                for (uint32_t k = rowStart[r]; k < rowStart[r + 1]; ++k)
                {
                    const uint32_t i = group.params[k];
                    const ImVec2 cell = cellSize(Meta::kParams[i]);
                    drawCell(i, x, y, cell.x * s, s, accentRgb, host);
                    x += cell.x * s;
                    rowH = std::max(rowH, cell.y * s);
                }
                y += rowH;
            }
            y += kGroupGap * s;
        }
    }

    // Flow layout: a cell is the label, the control's box and the value underneath.
    void drawCell(const uint32_t i, const float cx, const float cy, const float cellW, const float s,
                  const uint32_t accentRgb, ControlsHost& host)
    {
        const ParamInfo& p = Meta::kParams[i];
        const ImGuiStyle& st = ImGui::GetStyle();

        ImGui::SetCursorPosY(cy);
        centeredText(p.shortName[0] ? p.shortName : p.name, cx, cellW, st.Colors[ImGuiCol_TextDisabled]);

        const float top = cy + 24.f * s;
        float bx, by, bw, bh, valueY;
        switch (p.control)
        {
        case Control::Slider:
            bx = cx + 12.f * s, by = top + 2.f * s, bw = cellW - 24.f * s, bh = 20.f * s, valueY = top + 30.f * s;
            break;
        case Control::Toggle:
            bw = 46.f * s, bh = 24.f * s, bx = cx + (cellW - bw) * 0.5f, by = top + 16.f * s;
            valueY = top + kKnobSize * s + 8.f * s;
            break;
        case Control::Button:
            bw = 84.f * s, bh = 30.f * s, bx = cx + (cellW - bw) * 0.5f, by = top + 14.f * s, valueY = -1.f;
            break;
        case Control::Dropdown:
            bx = cx + 7.f * s, by = top + 16.f * s, bw = cellW - 14.f * s, bh = ImGui::GetFrameHeight(), valueY = -1.f;
            break;
        case Control::Buttons:
            bx = cx + 8.f * s, by = top + 4.f * s, bw = cellW - 16.f * s, bh = 28.f * s, valueY = -1.f;
            break;
        default: { // knobs
            const float d = knobDiameter(p.control) * s;
            bx = cx + (cellW - d) * 0.5f, by = top, bw = bh = d, valueY = top + d + 8.f * s;
            break;
        }
        }

        drawControl(i, bx, by, bw, bh, s, accentRgb, host);

        if (valueY >= 0.f)
        {
            char buf[64];
            formatValue(p, values[i], buf, sizeof(buf));
            ImGui::SetCursorPosY(valueY);
            centeredText(buf, cx, cellW, st.Colors[ImGuiCol_Text]);
        }
    }

    // The control itself inside its box (window-relative pixels), with all interaction. Returns whether
    // this kind of control shows its value as text underneath.
    bool drawControl(const uint32_t i, const float bx, const float by, const float bw, const float bh, const float s,
                     const uint32_t accentRgb, ControlsHost& host)
    {
        const ParamInfo& p = Meta::kParams[i];
        const UiParam& u = Meta::kUiParams[i];
        const ImVec4 accent = rgb(accentRgb);
        const ImGuiStyle& st = ImGui::GetStyle();
        const ImVec4 bright = st.Colors[ImGuiCol_Text];

        ImGui::PushID((int)i);
        ImGui::SetCursorPos(ImVec2(bx, by));

        bool changed = false, hovered = false, active = false, showsValue = false;
        float newValue = values[i];

        switch (p.control)
        {
        case Control::Knob:
        case Control::SmallKnob:
        case Control::LargeKnob:
        case Control::Slider: {
            // continuous controls work on a normalized value; while dragging we keep the unrounded position
            if (!editing_[i])
                dragT_[i] = toNormalized(p, values[i]);
            float t = dragT_[i];
            const float origin = (p.min < 0.f && p.max > 0.f && p.scale == Scale::Linear) ? toNormalized(p, 0.f) : 0.f;
            bool moved;
            if (p.control == Control::Slider)
                moved = slider("##s", &t, bw, bh, origin, accent);
            else if (const ImageSet::Image* img = images_.get(u.image))
                moved = imageKnob("##k", &t, std::min(bw, bh), *img, u.frames);
            else
                moved = knob("##k", &t, std::min(bw, bh), origin, accent, p.knobStyle, Meta::kKnobColor, Meta::kKnobTicks);
            hovered = ImGui::IsItemHovered();
            active = ImGui::IsItemActive();
            showsValue = true;
            if (moved)
            {
                dragT_[i] = t;
                newValue = fromNormalized(p, t);
                if (p.type == ParamType::Int)
                    newValue = std::round(newValue);
                changed = newValue != values[i];
            }
            break;
        }
        case Control::Toggle: {
            bool v = values[i] > 0.5f;
            ImGuiTogglePalette onPalette = {};
            onPalette.Frame = accent;
            onPalette.FrameHover = mix(accent, bright, 0.15f);
            ImGuiToggleConfig cfg;
            cfg.Flags = ImGuiToggleFlags_Animated;
            cfg.Size = ImVec2(bw, bh);
            cfg.On.Palette = &onPalette;
            if (ImGui::Toggle("##t", &v, cfg))
            {
                newValue = v ? 1.f : 0.f;
                changed = true;
            }
            hovered = ImGui::IsItemHovered();
            showsValue = true;
            break;
        }
        case Control::Button: {
            const bool on = values[i] > 0.5f;
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##b", ImVec2(bw, bh)))
            {
                newValue = on ? 0.f : 1.f;
                changed = true;
            }
            hovered = ImGui::IsItemHovered();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec4 fill = on ? (hovered ? mix(accent, bright, 0.15f) : accent)
                                   : (hovered ? st.Colors[ImGuiCol_ButtonHovered] : st.Colors[ImGuiCol_Button]);
            dl->AddRectFilled(pos, ImVec2(pos.x + bw, pos.y + bh), ImGui::GetColorU32(fill), 5.f * s);
            const char* label = on ? "On" : "Off";
            const ImVec2 ts = ImGui::CalcTextSize(label);
            dl->AddText(ImVec2(pos.x + (bw - ts.x) * 0.5f, pos.y + (bh - ts.y) * 0.5f),
                        on ? onAccent(accentRgb) : ImGui::GetColorU32(bright), label);
            break;
        }
        case Control::Dropdown: {
            const int cur = (int)std::lround(values[i]);
            ImGui::SetNextItemWidth(bw);
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
            hovered = ImGui::IsItemHovered();
            break;
        }
        case Control::Buttons: {
            const int cur = (int)std::lround(values[i]);
            const float segW = bw / (float)std::max<uint32_t>(1, p.numChoices);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            for (uint32_t c = 0; c < p.numChoices; ++c)
            {
                ImGui::PushID((int)c);
                ImGui::SetCursorPos(ImVec2(bx + c * segW, by));
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                if (ImGui::InvisibleButton("##seg", ImVec2(segW, bh)) && (int)c != cur)
                {
                    newValue = (float)c;
                    changed = true;
                }
                const bool segHover = ImGui::IsItemHovered();
                hovered = hovered || segHover;
                const bool sel = (int)c == cur;
                const ImVec4 fill = sel ? accent : (segHover ? st.Colors[ImGuiCol_ButtonHovered] : st.Colors[ImGuiCol_Button]);
                const ImDrawFlags corners = p.numChoices == 1 ? ImDrawFlags_RoundCornersAll
                                          : c == 0 ? ImDrawFlags_RoundCornersLeft
                                          : c + 1 == p.numChoices ? ImDrawFlags_RoundCornersRight
                                          : ImDrawFlags_RoundCornersNone;
                dl->AddRectFilled(pos, ImVec2(pos.x + segW - (c + 1 < p.numChoices ? 1.f * s : 0.f), pos.y + bh),
                                  ImGui::GetColorU32(fill), 5.f * s, corners);
                const ImVec2 ts = ImGui::CalcTextSize(p.choices[c]);
                dl->PushClipRect(pos, ImVec2(pos.x + segW, pos.y + bh), true);
                dl->AddText(ImVec2(pos.x + std::max(4.f * s, (segW - ts.x) * 0.5f), pos.y + (bh - ts.y) * 0.5f),
                            sel ? onAccent(accentRgb) : ImGui::GetColorU32(bright), p.choices[c]);
                dl->PopClipRect();
                ImGui::PopID();
            }
            break;
        }
        }

        const bool continuous = p.type == ParamType::Float || p.type == ParamType::Int;

        // double-click resets to default
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && p.type != ParamType::Choice &&
            p.control != Control::Button)
        {
            newValue = p.def;
            changed = true;
        }

        // mouse wheel nudges knobs and sliders
        if (hovered && continuous && ImGui::GetIO().MouseWheel != 0.f)
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

        ImGui::PopID();
        return showsValue;
    }

    ImageSet images_;
    bool editing_[kSize] = {};
    float dragT_[kSize] = {};
};

} // namespace ui
} // namespace cookbook
