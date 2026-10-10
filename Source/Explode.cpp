#include <algorithm>
#include <array>
#include <cmath>
#include <juce_dsp/juce_dsp.h>
#include "Explode.h"

namespace labx0
{
    namespace
    {
        constexpr int numBands = 8;
        constexpr float bandEdges[numBands + 1] = { 0.0f, 80.0f, 160.0f, 400.0f, 1000.0f, 2500.0f, 5000.0f, 10000.0f, 1.0e9f };

        struct Shape
        {
            std::array<float, numBands> db {};    // band energy relative to the slice's total, dB
            std::array<float, numBands> share {}; // the same as fractions
        };

        // The spectral shape of a hit's first 120 ms. The window stays open over the attack and closes over the last
        // quarter, so the onset weighs fully.
        Shape shapeOf (const float* x, int n, double sampleRate, juce::dsp::FFT& fft, std::vector<float>& work)
        {
            Shape s;
            const int size = fft.getSize();
            std::fill (work.begin(), work.end(), 0.0f);
            const int m = std::min ({ n, size, (int) (0.12 * sampleRate) });
            const int fadeIn = std::max (1, (int) (0.0005 * sampleRate)), fadeOutStart = (m * 3) / 4;
            for (int i = 0; i < m; ++i)
            {
                float w = 1.0f;
                if (i < fadeIn)
                    w = (float) i / (float) fadeIn;
                else if (i >= fadeOutStart && m > fadeOutStart)
                    w = 0.5f + 0.5f * std::cos (3.14159265f * (float) (i - fadeOutStart) / (float) (m - fadeOutStart));
                work[(size_t) i] = x[i] * w;
            }
            fft.performFrequencyOnlyForwardTransform (work.data(), true);

            std::array<double, numBands> energy {};
            double total = 1.0e-20;
            const int bins = size / 2 + 1;
            for (int k = 1; k < bins; ++k)
            {
                const double hz = (double) k * sampleRate / size;
                int band = 0;
                while (band + 1 < numBands && hz >= bandEdges[band + 1])
                    ++band;
                const double e = (double) work[(size_t) k] * work[(size_t) k];
                energy[(size_t) band] += e;
                total += e;
            }
            for (int b = 0; b < numBands; ++b)
            {
                s.share[(size_t) b] = (float) (energy[(size_t) b] / total);
                s.db[(size_t) b] = 10.0f * std::log10 (s.share[(size_t) b] + 1.0e-6f);
            }
            return s;
        }

        // Kick: most of the energy under 160 Hz. Hat: mostly above 2.5 kHz with little low end. Snare: a body between
        // 160 Hz and 2.5 kHz with noise above. The rest is "other" (toms, rides, voices).
        ExplodeKind kindOf (const Shape& s)
        {
            const float sub = s.share[0] + s.share[1];
            const float body = s.share[2] + s.share[3] + s.share[4];
            const float highs = s.share[5] + s.share[6] + s.share[7];
            if (sub > 0.4f && highs < 0.3f)
                return ExplodeKind::kick;
            if (highs > 0.6f && sub < 0.1f)
                return ExplodeKind::hat;
            if (body > 0.4f && highs > 0.12f)
                return ExplodeKind::snare;
            if (highs > 0.4f)
                return ExplodeKind::hat;
            return ExplodeKind::other;
        }

        float shapeDistance (const Shape& a, const Shape& b)
        {
            double sum = 0.0;
            for (int k = 0; k < numBands; ++k)
            {
                // Bands far below the slice's main energy say little: clamp at -40 dB.
                const double d = std::max (a.db[(size_t) k], -40.0f) - std::max (b.db[(size_t) k], -40.0f);
                sum += d * d;
            }
            return (float) std::sqrt (sum / numBands);
        }
    }

    ExplodePlan planExplode (const float* mono, int length, double sampleRate, const std::vector<int>& slices, double beats,
                             int maxComponents, float mergeDb)
    {
        ExplodePlan plan;
        if (mono == nullptr || length < 64 || slices.empty() || beats <= 0.0 || maxComponents < 1)
        {
            plan.message = "NOTHING TO EXPLODE";
            return plan;
        }

        // Hits: one per slice, up to the next slice.
        for (size_t k = 0; k < slices.size(); ++k)
        {
            const int start = std::clamp (slices[k], 0, length - 1);
            const int end = k + 1 < slices.size() ? std::clamp (slices[k + 1], start + 1, length) : length;
            ExplodeHit h;
            h.start = start;
            h.length = std::min (end - start, (int) (1.5 * sampleRate));
            for (int i = start; i < start + h.length; ++i)
                h.peak = std::max (h.peak, std::abs (mono[i]));
            plan.hits.push_back (h);
        }

        const int n = (int) plan.hits.size();
        juce::dsp::FFT fft (13);   // 8192 points: 120 ms at up to 68 kHz
        std::vector<float> work ((size_t) fft.getSize() * 2, 0.0f);
        std::vector<Shape> shapes;
        shapes.reserve ((size_t) n);
        for (const auto& h : plan.hits)
            shapes.push_back (shapeOf (mono + h.start, h.length, sampleRate, fft, work));

        // Average-linkage clustering on the shapes (Lance-Williams update), merging while close or while there are
        // more components than pads. Silent slices (below -60 dB, a lead-in for instance) stay out of it.
        std::vector<std::vector<int>> clusters ((size_t) n);
        std::vector<bool> alive ((size_t) n, true);
        int count = 0;
        for (int i = 0; i < n; ++i)
        {
            if (plan.hits[(size_t) i].peak < 0.001f)
            {
                alive[(size_t) i] = false;
                continue;
            }
            clusters[(size_t) i] = { i };
            ++count;
        }
        std::vector<std::vector<float>> d ((size_t) n, std::vector<float> ((size_t) n, 0.0f));
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j)
                d[(size_t) i][(size_t) j] = d[(size_t) j][(size_t) i] = shapeDistance (shapes[(size_t) i], shapes[(size_t) j]);
        while (count > 1)
        {
            int a = -1, b = -1;
            float best = 1.0e30f;
            for (int i = 0; i < n; ++i)
                if (alive[(size_t) i])
                    for (int j = i + 1; j < n; ++j)
                        if (alive[(size_t) j] && d[(size_t) i][(size_t) j] < best)
                        {
                            best = d[(size_t) i][(size_t) j];
                            a = i;
                            b = j;
                        }
            if (a < 0 || (best > mergeDb && count <= maxComponents))
                break;
            const float na = (float) clusters[(size_t) a].size(), nb = (float) clusters[(size_t) b].size();
            for (int k = 0; k < n; ++k)
                if (alive[(size_t) k] && k != a && k != b)
                    d[(size_t) a][(size_t) k] = d[(size_t) k][(size_t) a] = (na * d[(size_t) a][(size_t) k] + nb * d[(size_t) b][(size_t) k]) / (na + nb);
            clusters[(size_t) a].insert (clusters[(size_t) a].end(), clusters[(size_t) b].begin(), clusters[(size_t) b].end());
            clusters[(size_t) b].clear();
            alive[(size_t) b] = false;
            --count;
        }

        // Components: kind by the loudest instance; ordered kick, snare, hat, other, then by first appearance.
        for (int i = 0; i < n; ++i)
        {
            if (! alive[(size_t) i] || clusters[(size_t) i].empty())
                continue;
            ExplodeComponent c;
            c.representative = clusters[(size_t) i].front();
            for (const int h : clusters[(size_t) i])
                if (plan.hits[(size_t) h].peak > plan.hits[(size_t) c.representative].peak)
                    c.representative = h;
            c.kind = kindOf (shapes[(size_t) c.representative]);
            c.count = (int) clusters[(size_t) i].size();
            c.levelDb = juce::Decibels::gainToDecibels (plan.hits[(size_t) c.representative].peak / 0.891f, -48.0f);
            plan.components.push_back (c);
        }
        auto firstOf = [&plan, &clusters, &alive, n] (const ExplodeComponent& c)
        {
            for (int i = 0; i < n; ++i)
                if (alive[(size_t) i] && std::find (clusters[(size_t) i].begin(), clusters[(size_t) i].end(), c.representative) != clusters[(size_t) i].end())
                    return *std::min_element (clusters[(size_t) i].begin(), clusters[(size_t) i].end());
            return c.representative;
        };
        std::stable_sort (plan.components.begin(), plan.components.end(), [&firstOf] (const ExplodeComponent& x, const ExplodeComponent& y)
        {
            if (x.kind != y.kind)
                return (int) x.kind < (int) y.kind;
            return firstOf (x) < firstOf (y);
        });
        for (int c = 0; c < (int) plan.components.size(); ++c)
            for (int i = 0; i < n; ++i)
                if (alive[(size_t) i] && std::find (clusters[(size_t) i].begin(), clusters[(size_t) i].end(),
                                                    plan.components[(size_t) c].representative) != clusters[(size_t) i].end())
                    for (const int h : clusters[(size_t) i])
                        plan.hits[(size_t) h].component = c;

        // The pattern: 1/16 steps for up to 16 beats, 1/8 for up to 32.
        const bool eighths = beats > 16.0 + 1.0e-6;
        const double stepsPerBeat = eighths ? 2.0 : 4.0;
        plan.resolution = eighths ? 0 : 1;   // resolutionNames: 1/8, 1/16
        plan.coveredBeats = std::min (beats, eighths ? 32.0 : 16.0);
        plan.steps = std::clamp ((int) std::lround (plan.coveredBeats * stepsPerBeat), 1, maxSteps);
        plan.pattern.clear();
        plan.pattern.length = (uint8_t) plan.steps;
        plan.pattern.resolution = (uint8_t) plan.resolution;
        for (const auto& h : plan.hits)
        {
            if (h.component < 0)
                continue;
            const double atBeats = (double) h.start / (double) length * beats;
            if (atBeats >= plan.coveredBeats - 1.0e-9)
                continue;
            const double exact = atBeats * stepsPerBeat;
            int step = (int) std::lround (exact);
            double nudge = (exact - step) * 100.0;
            if (step >= plan.steps)
            {
                step -= plan.steps;   // just before the loop point: the start of the next pass
                nudge = (exact - plan.steps) * 100.0;
            }
            const auto& comp = plan.components[(size_t) h.component];
            const float repPeak = std::max (1.0e-6f, plan.hits[(size_t) comp.representative].peak);
            const int velocity = std::clamp ((int) std::lround (127.0 * h.peak / repPeak), 1, 127);
            auto& st = plan.pattern.lanes[(size_t) h.component].steps[(size_t) step];
            if (st.on && st.velocity >= velocity)
                continue;   // two hits of one component on one step: keep the louder
            st.on = 1;
            st.velocity = (uint8_t) velocity;
            st.nudge = (int8_t) std::clamp ((int) std::lround (nudge), -50, 50);
            st.probability = 100;
            st.ratchet = 1;
            st.pitch = 0;
            st.slice = -1;
        }

        plan.ok = ! plan.components.empty();
        plan.message = juce::String (n) + " HITS -> " + juce::String ((int) plan.components.size()) + " PADS";
        return plan;
    }
}
