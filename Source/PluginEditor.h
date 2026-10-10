#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "UI/CrtLookAndFeel.h"
#include "UI/Widgets.h"
#include "UI/EditorContext.h"
#include "UI/PatternGrid.h"
#include "UI/Browser.h"
#include "UI/LanePanel.h"
#include "UI/SideViews.h"

class LabX0AudioProcessorEditor final : public juce::AudioProcessorEditor,
                                        public juce::DragAndDropContainer,
                                        private juce::Timer
{
public:
    explicit LabX0AudioProcessorEditor (LabX0AudioProcessor&);
    ~LabX0AudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    // Pulls meters, playheads and status from the processor. Public so the render harness can call it.
    void refreshFromProcessor();

    static constexpr int editorWidth = 1400;
    static constexpr int editorHeight = 920;

private:
    void timerCallback() override { refreshFromProcessor(); }

    struct PanelItem
    {
        juce::Component* component;
        float units;
    };

    struct Panel
    {
        juce::String title;
        std::vector<PanelItem> items;
        juce::Rectangle<int> bounds;
        bool accent = false;
    };

    labx0::ui::LabeledKnob* knob (const char* id, const char* caption, bool accent = false);
    labx0::ui::ChoiceStack* choices (const char* idA, const char* captionA, const char* idB = "", const char* captionB = "");
    void layoutRow (std::vector<Panel>& row, juce::Rectangle<int> area, float unitWidth);
    void stepPreset (int delta);
    void chooseSampleFor (int lane);
    void chooseFolderFor (int root);

    LabX0AudioProcessor& audioProcessor;
    labx0::ui::CrtLookAndFeel lookAndFeel;
    labx0::ui::EditorContext context;

    labx0::ui::BrowserPanel browser;
    labx0::ui::PatternPanel patternPanel;
    labx0::ui::LanePanel lanePanel;

    std::vector<std::unique_ptr<labx0::ui::LabeledKnob>> knobs;
    std::vector<std::unique_ptr<labx0::ui::ChoiceStack>> stacks;
    std::vector<std::vector<Panel>> rows;
    std::vector<Panel> sidePanels;
    juce::Rectangle<int> browserFrame, patternFrame;

    juce::ComboBox presetBox;
    juce::TextButton prevButton { "<" }, nextButton { ">" };
    labx0::ui::EmissionGauge gauge;
    labx0::ui::Dosimeter dosimeter;
    labx0::ui::ScopeView scopeView;
    labx0::ui::LabeledKnob* emissionKnob = nullptr;
    juce::Label systemLabel;
    labx0::ui::ScanlineOverlay overlay;
    juce::TooltipWindow tooltips { this, 600 };

    std::unique_ptr<juce::FileChooser> chooser;
    std::vector<float> scopeBuffer;
    int lastProgram = -1;
    juce::String lastLaneTitle;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LabX0AudioProcessorEditor)
};
