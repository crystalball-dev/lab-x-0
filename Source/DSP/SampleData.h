#pragma once

#include <cstdint>
#include <vector>

namespace labx0::dsp
{
    // One decoded file, stereo, at its own sample rate. Guard zeros on both ends let the Hermite reader run
    // without bounds checks. Immutable once published to the audio thread.
    struct SampleData
    {
        static constexpr int guard = 4;

        std::vector<float> left, right;   // length + 2 * guard each
        int length = 0;
        double sampleRate = 44100.0;
        uint64_t id = 0;                  // unique per load: a reused address is never mistaken for the same file
        int generation = 0;               // the slot request this load answered
        float normaliseGain = 1.0f;       // applied at load (peak to -1 dBFS)

        // Break analysis (break and preview slots only)
        double bpm = 0.0;                 // loop tempo; 0 = unknown
        double beats = 0.0;               // length in beats at bpm
        int tempoSource = 0;              // 0 none, 1 ACID chunk, 2 file name, 3 folder name, 4 estimate
        std::vector<int> onsetPos;        // slicing candidates (sample positions)
        std::vector<float> onsetStrength;

        const float* l() const noexcept { return left.data() + guard; }
        const float* r() const noexcept { return right.data() + guard; }
    };

    inline const char* tempoSourceName (int s) noexcept
    {
        switch (s)
        {
            case 1:  return "ACID";
            case 2:  return "NAME";
            case 3:  return "FOLDER";
            case 4:  return "ESTIMATE";
            default: return "";
        }
    }
}
