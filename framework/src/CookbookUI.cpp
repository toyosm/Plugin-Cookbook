// Plugin Cookbook — generic Dear ImGui editor, built from the recipe's parameter table.
// SPDX-License-Identifier: MIT
//
// The controls themselves live in cookbook/Controls.hpp (shared with the standalone app); this file
// only connects them to the plugin host.

#include "DistrhoUI.hpp"
#include "DistrhoStandaloneUtils.hpp"

#include "RecipeAssets.hpp"
#include "RecipeMeta.hpp"
#include "cookbook/Controls.hpp"

START_NAMESPACE_DISTRHO

namespace cui = cookbook::ui;

class CookbookUI : public UI,
                   private cui::ControlsHost
{
public:
    using Meta = recipe::Meta;

    CookbookUI()
        : UI(DISTRHO_UI_DEFAULT_WIDTH, DISTRHO_UI_DEFAULT_HEIGHT),
          fControls(recipe::kAssets, recipe::kNumAssets)
    {
        const double scale = getScaleFactor();
        if (Meta::kFreeLayout) // a fixed design: resize it, but keep its proportions
            setGeometryConstraints(Meta::kBaseWidth * 0.5 * scale, Meta::kBaseHeight * 0.5 * scale, true);
        else
            setGeometryConstraints(260 * scale, 180 * scale);
        cui::applyStyle(COOKBOOK_UI_ACCENT, COOKBOOK_UI_LIGHT, COOKBOOK_UI_BACKGROUND);
    }

    ~CookbookUI() override
    {
        fControls.releaseImages(); // DPF keeps the GL context current while the UI is destroyed
    }

protected:
    void parameterChanged(const uint32_t index, const float value) override
    {
        if (index < Meta::kNumParams)
            fControls.values[index] = value;
    }

    void onImGuiDisplay() override
    {
        const float s = getScaleFactor();
        const float width = getWidth();
        const float height = getHeight();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(width, height));

        // DPF's own standalone (the "jack" format) on native audio opens only the output until input
        // is requested. Ask once on the first frame; on macOS this triggers the microphone prompt.
        if (!fInputRequested)
        {
            fInputRequested = true;
            if (Meta::kNumInputs > 0 && isUsingNativeAudio() && supportsAudioInput() && !isAudioInputEnabled())
                requestAudioInput();
        }

        if (ImGui::Begin("##cookbook", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus))
        {
            fControls.drawBackground(width, height);
            if (!Meta::kFreeLayout) // a free layout's artwork carries its own title
                cui::drawHeader(s, width, DISTRHO_PLUGIN_NAME, DISTRHO_PLUGIN_BRAND, COOKBOOK_UI_SUBTITLE,
                                COOKBOOK_UI_ACCENT);
            fControls.draw(s, width, (cui::headerHeight(COOKBOOK_UI_SUBTITLE) + 8.f) * s, COOKBOOK_UI_ACCENT, *this,
                           height);
            drawStandaloneStatus(s, height);
        }
        ImGui::End();
    }

private:
    void beginEdit(const uint32_t index) override { editParameter(index, true); }
    void setValue(const uint32_t index, const float value) override { setParameterValue(index, value); }
    void endEdit(const uint32_t index) override { editParameter(index, false); }

    // Only shown in DPF's standalone when it runs on native audio and the input is still off
    // (e.g. permission denied): lets the user retry.
    void drawStandaloneStatus(const float s, const float height)
    {
        if (Meta::kNumInputs == 0 || !isUsingNativeAudio() || !supportsAudioInput() || isAudioInputEnabled())
            return;

        ImGui::SetCursorPos(ImVec2(cui::kPad * s, height - 26.f * s));
        if (ImGui::SmallButton("Audio input is off - click to enable"))
            requestAudioInput();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", "Uses the system's default input device. On macOS, allow microphone access "
                                    "in System Settings > Privacy & Security > Microphone.");
    }

    cui::ControlsView<Meta> fControls;
    bool fInputRequested = false;

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CookbookUI)
};

UI* createUI()
{
    return new CookbookUI();
}

END_NAMESPACE_DISTRHO
