#pragma once

#include <array>
#include <utility>
#include <vector>
#include <juce_core/juce_core.h>
#include "Config.h"

namespace labx0
{
    struct Preset
    {
        juce::String name;
        std::vector<std::pair<juce::String, float>> values;      // parameter id, real value; others use defaults
        std::array<juce::String, numPads + numBreaks> samples;   // SampleRef strings, "KIT:<name>" for catalogue entries
        std::vector<std::pair<int, juce::String>> patterns;      // pattern index, text form (see Pattern.h)
    };

    const std::vector<Preset>& factoryPresets();
}
