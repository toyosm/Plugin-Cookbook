// Plugin Cookbook — parameter metadata shared by the DSP wrapper, the generic UI and the test harness.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace cookbook {

enum class ParamType { Float, Int, Bool, Choice };
enum class Scale { Linear, Log };

// How a parameter is drawn. Float/int: Knob, SmallKnob, LargeKnob, Slider. Bool: Toggle, Button.
// Choice: Dropdown, Buttons.
enum class Control { Knob, SmallKnob, LargeKnob, Slider, Toggle, Button, Dropdown, Buttons };

// How a knob looks (knob controls only).
enum class KnobStyle { Modern, Vintage, Flat, Minimal, Metal };

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
};

// An image embedded in the plugin (PNG bytes).
struct Asset
{
    const char* name;
    const unsigned char* data;
    uint32_t size;
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
