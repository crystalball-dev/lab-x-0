#pragma once

#include <vector>
#include <juce_core/juce_core.h>

namespace labx0::analysis
{
    // Spectral flux of log magnitudes (bins above 80 Hz), the onset strength used for slicing and tempo.
    struct OnsetEnvelope
    {
        std::vector<float> flux;
        double frameRate = 0.0;   // frames per second
        int hop = 256;
        int fftSize = 1024;
    };

    OnsetEnvelope computeEnvelope (const float* mono, int length, double sampleRate, double maxSeconds = 120.0);

    struct Onset
    {
        int position;      // sample index where the attack begins
        float strength;    // 0..1, relative to the strongest onset
    };

    // Slicing candidates: peaks of the flux above a local median, refined to the start of each attack.
    std::vector<Onset> detectOnsets (const OnsetEnvelope& env, const float* mono, int length);

    // Tempo up to an octave: the strongest beat period between 70 and 190 BPM, snapped to a whole power-of-two
    // number of beats when the file looks like a trimmed loop (20 s or less). 0 when nothing periodic is found.
    double estimateTempo (const OnsetEnvelope& env, double durationSeconds);

    // The octave of `bpm` closest to the host tempo (as FL's slicer does), kept within 55..230 BPM.
    double nearestOctave (double bpm, double hostBpm);

    // Tempo written in a name: "[174bpm]", "_172BPM_", "BPM 120". A bare 2-3 digit number ("Break_090",
    // "cw_amen01_175", "100 Ogasawara Break") counts only when the file then holds a whole number of beats.
    double tempoFromName (const juce::String& name, double durationSeconds, bool allowBare);

    // Equal slices: n = beats x divisions, or onset slices above a strength threshold with a minimum spacing.
    // Slice 0 always starts at sample 0. Returns the number of slices written (<= maxOut).
    int gridSlices (int length, double beats, int divisionsPerBeat, int* out, int maxOut);
    // Allocation free (audio thread): `scratch` must hold `count` ints.
    int onsetSlices (const int* positions, const float* strengths, int count, float sensitivity,
                     int minSpacing, int length, int* out, int maxOut, int* scratch) noexcept;
    float sensitivityThreshold (float sensitivity) noexcept;
}
