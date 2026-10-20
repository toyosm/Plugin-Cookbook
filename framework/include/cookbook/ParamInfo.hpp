// Plugin Cookbook — parameter metadata shared by the DSP wrapper, the generic UI and the test harness.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace cookbook {

enum class ParamType { Float, Int, Bool, Choice };
enum class Scale { Linear, Log };

// How a parameter is drawn. Float/int: Knob, SmallKnob, LargeKnob, Slider, Vslider, Number (int also
// Selector). Bool: Toggle, Button, Led, Footswitch, Lever, Checkbox. Choice: Dropdown, Buttons, Radio, Selector,
// and, with exactly two options, the bool controls (on = the second option).
enum class Control
{
    Knob, SmallKnob, LargeKnob, Slider, Vslider, Number, Selector,
    Toggle, Button, Led, Footswitch, Lever, Checkbox,
    Dropdown, Buttons, Radio
};

// How a knob looks (knob controls only).
enum class KnobStyle { Modern, Vintage, Flat, Minimal, Metal, Fluted };

struct ParamInfo
{
    const char* symbol;
    const char* name;
    const char* shortName;
    const char* unit;
    const char* description;
    ParamType type;
    Scale scale;
    float min, max, def;
    float smoothingMs;
    const char* const* choices; // Choice only
    uint32_t numChoices;
    Control control;
    KnobStyle knobStyle;
};

// Editor-only details of a parameter.
struct UiParam
{
    float x, y, w, h;   // free layout: the control's box, in unscaled pixels
    bool label, value;  // show the name above / the value below
    int32_t image;      // knob image: index into recipe::kAssets, -1 = drawn
    uint32_t frames;    // 1 = the image rotates; N = vertical (or horizontal) filmstrip of N frames
    int32_t labelColor; // 0xRRGGBB for the name, -1 = the theme's
    int32_t valueColor; // 0xRRGGBB for the value text, -1 = the theme's
    int32_t labelFont;  // font slot (Meta::kFonts) for the name, -1 = the default font
    int32_t valueFont;  // font slot for the value text
};

// A font the editor bakes: an embedded TTF/OTF (index into recipe::kAssets, -1 = DejaVu Sans) at a size.
struct FontSlot
{
    int32_t asset;
    float size; // px, before the display scale
};

// An image embedded in the plugin (PNG bytes).
struct Asset
{
    const char* name;
    const unsigned char* data;
    uint32_t size;
};

// A decoration drawn behind the controls. kind: 0 line (a, b = end point), 1 rect (a, b = size),
// 2 circle (a = radius), 3 text (a = size). Coordinates in unscaled px from the window's top-left (pedal
// and free layouts: from the design's top-left).
struct UiShape
{
    int32_t kind;
    float x, y, a, b;
    int32_t stroke, fill; // 0xRRGGBB, -1 = none (text: stroke = its color, -1 = the theme's label color)
    float width, radius;
    const char* text;
    int32_t font;         // text: font slot (Meta::kFonts)
    float rotation;       // degrees, clockwise, around the shape's center
    int32_t image;        // image: index into recipe::kAssets, -1 = none
};

// A section of the editor: an optional title and the parameters it shows, in order.
struct UiGroup
{
    const char* title;     // "" = no title
    uint32_t columns;      // max controls per row
    const uint32_t* params;
    uint32_t numParams;
};

} // namespace cookbook
