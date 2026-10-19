// Plugin Cookbook — generic DPF plugin wrapper.
// SPDX-License-Identifier: MIT
//
// This file is the same for every recipe. Everything plugin-specific comes from the generated
// DistrhoPluginInfo.h, RecipeMeta.hpp and RecipeKernel.hpp (see tools/recipe.py).

#include "DistrhoPlugin.hpp"

#include "RecipeKernel.hpp"
#include "cookbook/Engine.hpp"

START_NAMESPACE_DISTRHO

class CookbookPlugin : public Plugin
{
public:
    using Meta = recipe::Meta;

    CookbookPlugin()
        : Plugin(Meta::kNumParams, 0, 0)
    {
    }

protected:
    // -----------------------------------------------------------------------------------------------------------------
    // Information

    const char* getLabel() const override { return COOKBOOK_LABEL; }
    const char* getDescription() const override { return COOKBOOK_DESCRIPTION; }
    const char* getMaker() const override { return COOKBOOK_MAKER; }
    const char* getHomePage() const override { return COOKBOOK_HOMEPAGE; }
    const char* getLicense() const override { return COOKBOOK_LICENSE; }
    uint32_t getVersion() const override
    {
        return d_version(COOKBOOK_VERSION_MAJOR, COOKBOOK_VERSION_MINOR, COOKBOOK_VERSION_MICRO);
    }
    int64_t getUniqueId() const override { return d_cconst(COOKBOOK_UNIQUE_ID_CHARS); }

    // -----------------------------------------------------------------------------------------------------------------
    // Init

    void initAudioPort(const bool input, const uint32_t index, AudioPort& port) override
    {
        const uint32_t count = input ? Meta::kNumInputs : Meta::kNumOutputs;
        port.groupId = count == 2 ? kPortGroupStereo : kPortGroupMono;

        if (count == 2)
        {
            port.name = input ? (index == 0 ? "Input Left" : "Input Right") : (index == 0 ? "Output Left" : "Output Right");
            port.symbol = input ? (index == 0 ? "in_l" : "in_r") : (index == 0 ? "out_l" : "out_r");
        }
        else
        {
            port.name = input ? "Input" : "Output";
            port.symbol = input ? "in" : "out";
        }
    }

    void initParameter(const uint32_t index, Parameter& parameter) override
    {
        if (index >= Meta::kNumParams)
            return;

        const cookbook::ParamInfo& p = Meta::kParams[index];

        parameter.hints = kParameterIsAutomatable;
        parameter.name = p.name;
        parameter.shortName = p.shortName;
        parameter.symbol = p.symbol;
        parameter.unit = p.unit;
        parameter.description = p.description;
        parameter.ranges.min = p.min;
        parameter.ranges.max = p.max;
        parameter.ranges.def = p.def;

        switch (p.type)
        {
        case cookbook::ParamType::Float:
            if (p.scale == cookbook::Scale::Log)
                parameter.hints |= kParameterIsLogarithmic;
            break;
        case cookbook::ParamType::Int:
            parameter.hints |= kParameterIsInteger;
            break;
        case cookbook::ParamType::Bool:
            parameter.hints |= kParameterIsInteger | kParameterIsBoolean;
            break;
        case cookbook::ParamType::Choice:
            parameter.hints |= kParameterIsInteger;
            if (p.numChoices > 0)
            {
                ParameterEnumerationValue* const values = new ParameterEnumerationValue[p.numChoices];
                for (uint32_t i = 0; i < p.numChoices; ++i)
                {
                    values[i].label = p.choices[i];
                    values[i].value = static_cast<float>(i);
                }
                parameter.enumValues.count = p.numChoices;
                parameter.enumValues.restrictedMode = true;
                parameter.enumValues.values = values; // DPF takes ownership
            }
            break;
        }
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Parameters

    float getParameterValue(const uint32_t index) const override { return fEngine.getParameter(index); }
    void setParameterValue(const uint32_t index, const float value) override { fEngine.setParameter(index, value); }

    // -----------------------------------------------------------------------------------------------------------------
    // Processing

    // DPF deactivates/re-activates around sample-rate and buffer-size changes, so this is the one place
    // where memory is (re)allocated. It never runs concurrently with run().
    void activate() override
    {
        fEngine.prepare(getSampleRate(), getBufferSize());
#if DISTRHO_PLUGIN_WANT_LATENCY
        setLatency(fEngine.latency());
#endif
    }

    void run(const float** inputs, float** outputs, uint32_t frames) override
    {
        fEngine.process(inputs, outputs, frames);
    }

private:
    cookbook::Engine<recipe::Kernel, Meta> fEngine;

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CookbookPlugin)
};

Plugin* createPlugin()
{
    return new CookbookPlugin();
}

END_NAMESPACE_DISTRHO
