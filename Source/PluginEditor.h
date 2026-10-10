#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

// Temporary editor while the engine is brought up; replaced by the full CRT editor.
class LabX0AudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit LabX0AudioProcessorEditor (LabX0AudioProcessor& p) : AudioProcessorEditor (&p) { setSize (editorWidth, editorHeight); }
    void paint (juce::Graphics& g) override { g.fillAll (juce::Colours::black); }
    void refreshFromProcessor() {}
    static constexpr int editorWidth = 1400;
    static constexpr int editorHeight = 920;
};
