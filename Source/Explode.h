#pragma once

#include <vector>
#include <juce_core/juce_core.h>
#include "Pattern.h"

namespace labx0
{
    // EXPLODE (0.2.0): takes a break apart into its components and writes it back as a pattern of pads.
    //
    // Each slice is described by the spectral shape of its first 120 ms (energy in eight bands, relative to its own
    // total, in dB). Slices of the same shape - every snare hit, ghost notes included - join one component (average
    // linkage, merged while their shapes stay within mergeDistanceDb, and always down to the number of pads
    // available). A component becomes one pad holding its loudest instance as a region of the break file; the
    // pattern hits that pad wherever an instance occurred, on the nearest step with a nudge for the exact time and a
    // velocity for its level, so the pattern plays the break back while every component has a lane of its own.
    struct ExplodeHit
    {
        int start = 0, length = 0;       // samples in the decoded break
        float peak = 0.0f;               // linear, of the decoded (normalised) break
        int component = -1;
    };

    enum class ExplodeKind : int { kick = 0, snare, hat, other };
    inline constexpr const char* explodeKindNames[] = { "KICK", "SNARE", "HAT", "OTHER" };

    struct ExplodeComponent
    {
        int representative = 0;          // index into hits: the loudest instance
        ExplodeKind kind = ExplodeKind::other;
        int count = 0;                   // instances in the break
        float levelDb = 0.0f;            // pad level that keeps the break's balance after the pad's own normalising
    };

    struct ExplodePlan
    {
        bool ok = false;
        juce::String message;
        std::vector<ExplodeHit> hits;
        std::vector<ExplodeComponent> components;   // ordered kick, snare, hat, other; then by first appearance
        Pattern pattern;                            // lanes 0.. = components (shifted to the first pad when applied)
        int steps = 0, resolution = 1;              // pattern length and resolution index (1/16, or 1/8 for long breaks)
        double coveredBeats = 0.0;                  // beats of the break the pattern holds (at most 16 at 1/16, 32 at 1/8)
    };

    inline constexpr float mergeDistanceDb = 3.0f;   // the amen: kick, snare, ghost snare, ride hits sit 3.6-8 dB apart

    // mono: the decoded break (as the break slot plays it); slices: slice starts in samples, ascending, the first 0;
    // beats: the break's length in beats; maxComponents: pads available (1..16); mergeDb: how alike two hits must be
    // to share a pad (0 = every slice its own pad, merged only when there are more slices than pads).
    ExplodePlan planExplode (const float* mono, int length, double sampleRate, const std::vector<int>& slices, double beats,
                             int maxComponents, float mergeDb = mergeDistanceDb);
}
