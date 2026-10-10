#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include "../Pattern.h"

namespace labx0::dsp
{
    struct SeqEvent
    {
        int offset = 0;              // sample inside the block
        int lane = 0;
        float velocity = 1.0f;       // 0..1
        int pitch = 0;               // semitones
        int slice = -1;              // break lanes: -1 natural
        double stepQuarter = 0.0;    // unswung time of the step, in quarters from the pattern start
        int gateSamples = -1;        // the step's length, for Gate mode pads
        bool repeat = false;         // a ratchet hit after the first
    };

    // A run of identical slots on a TIME or VOLUME lane, located around one moment.
    struct Region
    {
        int slot = 0;
        double startQ = 0.0, endQ = 0.0;   // quarters, absolute
        int64_t id = -1;                   // unique per occurrence (its first step), -1 = none
    };

    // Turns the current pattern into sample-accurate events. Every event is a pure function of the step's
    // position, so a block that covers [q0, q1) fires exactly the events whose time falls inside it: host loops
    // and jumps need no special handling, and nothing fires twice at a block boundary.
    class StepSequencer
    {
    public:
        static constexpr int maxEvents = 1024;

        void reset() noexcept
        {
            patternStartQ = 0.0;
            hasExpected = false;
            wasRunning = false;
        }

        double getPatternStartQ() const noexcept { return patternStartQ; }

        // Odd steps arrive late by half the swing amount of a step (at swing 1, half a step).
        static double swingOffset (int64_t step, double stepQ, float swing) noexcept
        {
            return (step & 1) ? 0.5 * (double) std::clamp (swing, 0.0f, 1.0f) * stepQ : 0.0;
        }

        static float chance (int64_t step, int lane, int ratchet) noexcept
        {
            uint64_t h = (uint64_t) step * 0x9E3779B97F4A7C15ull ^ ((uint64_t) (lane + 1) * 0xC2B2AE3D27D4EB4Full) ^ ((uint64_t) (ratchet + 1) << 56);
            h ^= h >> 33; h *= 0xff51afd7ed558ccdull; h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ull; h ^= h >> 33;
            return (float) (h >> 40) * (1.0f / 16777216.0f);
        }

        // Continuity: a host position that continues the previous block within a fraction of a sample is
        // replaced by the exact continuation, so rounding never drops or doubles an event at a boundary.
        double continuePosition (double q, double qps) noexcept
        {
            if (hasExpected && std::abs (q - expectedQ) < 0.25 * qps + 1.0e-9)
                return expectedQ;
            return q;
        }

        // Collects the events of [q0, q0 + n * qps). `current` and `queued` are the pattern indices: a queued
        // pattern starts at the first boundary of the current one at or after the block start, and not before
        // queuedFromQ (the moment a pattern note asked for it, in this block only); at once when not running.
        int collect (const PatternBank& bank, int& current, int& queued, double queuedFromQ, bool running, double q0,
                     double qps, int n, float swing, SeqEvent* out, int maxOut) noexcept
        {
            int count = 0;
            if (! running)
            {
                if (queued >= 0)
                {
                    current = queued;
                    queued = -1;
                }
                wasRunning = false;
                hasExpected = false;
                return 0;
            }

            if (! wasRunning)
            {
                // A fresh start aligns pattern step 0 with the song's beat 0.
                patternStartQ = 0.0;
                wasRunning = true;
            }

            // Block positions accumulate rounding. A block edge within a fifth of a sample of a boundary of the current
            // pattern is moved onto it, so a bar line on a block edge belongs to exactly one block, the one it starts,
            // whichever way the positions rounded (at 48 kHz and 64-sample blocks that happens at 120, 150, 160 BPM...).
            // A fifth stays inside continuePosition's quarter, so the next block still continues from the moved edge.
            const auto& startPattern = bank.patterns[(size_t) std::clamp (current, 0, numPatterns - 1)];
            const double startPatternQ = startPattern.stepQuarters() * startPattern.length;
            q0 = snapToBoundary (q0, startPatternQ, 0.2 * qps);
            const double q1 = snapToBoundary (q0 + qps * n, startPatternQ, 0.2 * qps);
            expectedQ = q1;
            hasExpected = true;

            double from = q0;
            while (from < q1)
            {
                const auto& pat = bank.patterns[(size_t) std::clamp (current, 0, numPatterns - 1)];
                const double patternQ = pat.stepQuarters() * pat.length;
                double to = q1;
                bool switching = false;

                if (queued >= 0 && patternQ > 0.0)
                {
                    // The first boundary at or after both this range's start and the moment the switch was asked
                    // for, compared exactly, as events are: it falls in exactly one block.
                    const double earliest = std::max (from, queuedFromQ);
                    double k = std::ceil ((earliest - patternStartQ) / patternQ);
                    if (patternStartQ + k * patternQ < earliest)
                        k += 1.0;
                    else if (patternStartQ + (k - 1.0) * patternQ >= earliest)
                        k -= 1.0;
                    const double boundary = patternStartQ + k * patternQ;
                    if (boundary < q1)
                    {
                        to = boundary;
                        switching = true;
                    }
                }

                count = collectRange (pat, from, to, q0, qps, n, swing, out, count, maxOut);

                if (switching)
                {
                    current = queued;
                    queued = -1;
                    patternStartQ = to;
                }
                from = to;
            }

            std::sort (out, out + count, [] (const SeqEvent& a, const SeqEvent& b) { return a.offset < b.offset; });
            return count;
        }

        // Pattern-relative step index (unbounded) at quarter position q.
        int64_t stepIndexAt (const Pattern& pat, double q) const noexcept
        {
            return (int64_t) std::floor ((q - patternStartQ) / pat.stepQuarters() + 1.0e-9);
        }

        static int laneStepFor (int64_t stepIndex, int laneLength) noexcept
        {
            const int64_t m = stepIndex % laneLength;
            return (int) (m < 0 ? m + laneLength : m);
        }

        // The TIME or VOLUME region at quarter position q (slot 0 = none). Regions follow the straight grid.
        Region regionAt (const Pattern& pat, int lane, double q) const noexcept
        {
            Region r;
            const auto& l = pat.lanes[(size_t) lane];
            if (l.mute)
                return r;
            const double stepQ = pat.stepQuarters();
            const int len = pat.laneLength (lane);
            const int64_t s = stepIndexAt (pat, q);
            const int ls = laneStepFor (s, len);
            const int slot = l.steps[(size_t) ls].slot;
            if (slot == 0)
                return r;

            int first = ls, last = ls;
            while (first > 0 && l.steps[(size_t) first - 1].slot == slot)
                --first;
            while (last + 1 < len && l.steps[(size_t) last + 1].slot == slot)
                ++last;

            const int64_t startStep = s - (ls - first);
            r.slot = slot;
            r.startQ = patternStartQ + (double) startStep * stepQ;
            r.endQ = patternStartQ + (double) (s + (last - ls) + 1) * stepQ;
            r.id = startStep;
            return r;
        }

    private:
        // The nearest boundary of the current pattern (same arithmetic as the switch, so the values are identical)
        // when q lies within `tolerance` of it; q otherwise.
        double snapToBoundary (double q, double patternQ, double tolerance) const noexcept
        {
            if (patternQ <= 0.0)
                return q;
            const double b = patternStartQ + std::round ((q - patternStartQ) / patternQ) * patternQ;
            return std::abs (q - b) <= tolerance ? b : q;
        }

        int collectRange (const Pattern& pat, double from, double to, double blockQ0, double qps, int n, float swing,
                          SeqEvent* out, int count, int maxOut) const noexcept
        {
            const double stepQ = pat.stepQuarters();
            const int64_t firstStep = stepIndexAt (pat, from) - 1;
            const int64_t lastStep = stepIndexAt (pat, to) + 1;
            const int gate = std::max (1, (int) std::lround (stepQ / qps));

            for (int lane = 0; lane < laneTime; ++lane)
            {
                const auto& l = pat.lanes[(size_t) lane];
                if (l.mute)
                    continue;
                const int len = pat.laneLength (lane);

                for (int64_t s = firstStep; s <= lastStep; ++s)
                {
                    const auto& st = l.steps[(size_t) laneStepFor (s, len)];
                    if (! st.on)
                        continue;

                    const double base = patternStartQ + (double) s * stepQ + swingOffset (s, stepQ, swing) + (double) st.nudge * 0.01 * stepQ;
                    const int ratchets = std::clamp ((int) st.ratchet, 1, 4);
                    for (int k = 0; k < ratchets; ++k)
                    {
                        const double t = base + (double) k * stepQ / ratchets;
                        if (t < from || t >= to)
                            continue;
                        if (st.probability < 100 && chance (s, lane, k) * 100.0f >= (float) st.probability)
                            continue;
                        if (count >= maxOut)
                            return count;
                        auto& e = out[count++];
                        e.offset = std::clamp ((int) std::floor ((t - blockQ0) / qps + 1.0e-6), 0, std::max (0, n - 1));
                        e.lane = lane;
                        e.velocity = (float) st.velocity / 127.0f;
                        e.pitch = st.pitch;
                        e.slice = st.slice;
                        e.stepQuarter = (double) s * stepQ;
                        e.gateSamples = std::max (1, gate / ratchets);
                        e.repeat = k > 0;
                    }
                }
            }
            return count;
        }

        double patternStartQ = 0.0, expectedQ = 0.0;
        bool hasExpected = false, wasRunning = false;
    };
}
