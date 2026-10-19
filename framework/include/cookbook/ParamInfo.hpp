// Plugin Cookbook — parameter metadata shared by the DSP wrapper, the generic UI and the test harness.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace cookbook {

enum class ParamType { Float, Int, Bool, Choice };
enum class Scale { Linear, Log };

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
};

} // namespace cookbook
