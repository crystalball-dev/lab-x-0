#include <algorithm>
#include <cmath>
#include <numeric>
#include <juce_dsp/juce_dsp.h>
#include "AudioAnalysis.h"

namespace labx0::analysis
{
    OnsetEnvelope computeEnvelope (const float* mono, int length, double sampleRate, double maxSeconds)
    {
        OnsetEnvelope env;
        env.hop = sampleRate <= 50000.0 ? 256 : 512;
        env.fftSize = 1024;
        env.frameRate = sampleRate / env.hop;
        if (mono == nullptr || length <= 0 || sampleRate <= 0.0)
            return env;

        length = std::min (length, (int) (maxSeconds * sampleRate));
        const int n = env.fftSize;
        const int frames = 1 + std::max (0, (length - n) / env.hop);
        env.flux.assign ((size_t) frames, 0.0f);

        juce::dsp::FFT fft (10);
        std::vector<float> window ((size_t) n), work ((size_t) n * 2), previous ((size_t) n / 2 + 1, 0.0f);
        for (int i = 0; i < n; ++i)
            window[(size_t) i] = 0.5f - 0.5f * std::cos (6.28318530718f * (float) i / (float) (n - 1));

        const int lo = std::max (1, (int) std::ceil (80.0 * n / sampleRate));
        const int bins = n / 2 + 1;

        for (int f = 0; f < frames; ++f)
        {
            std::fill (work.begin(), work.end(), 0.0f);
            const int start = f * env.hop;
            for (int i = 0; i < n && start + i < length; ++i)
                work[(size_t) i] = mono[start + i] * window[(size_t) i];
            fft.performFrequencyOnlyForwardTransform (work.data(), true);

            float sum = 0.0f;
            for (int k = 0; k < bins; ++k)
            {
                const float m = std::log1p (100.0f * work[(size_t) k]);
                if (f > 0 && k >= lo)
                    sum += std::max (0.0f, m - previous[(size_t) k]);
                previous[(size_t) k] = m;
            }
            env.flux[(size_t) f] = f > 0 ? sum : 0.0f;
        }
        return env;
    }

    //==============================================================================
    std::vector<Onset> detectOnsets (const OnsetEnvelope& env, const float* mono, int length)
    {
        std::vector<Onset> out;
        const auto& flux = env.flux;
        const int frames = (int) flux.size();
        if (frames < 3 || mono == nullptr)
            return out;

        // Novelty above a moving median of +-8 frames (about +-45 ms).
        std::vector<float> novelty ((size_t) frames, 0.0f), window;
        for (int i = 0; i < frames; ++i)
        {
            window.clear();
            for (int k = std::max (0, i - 8); k <= std::min (frames - 1, i + 8); ++k)
                window.push_back (flux[(size_t) k]);
            std::nth_element (window.begin(), window.begin() + (long) window.size() / 2, window.end());
            novelty[(size_t) i] = std::max (0.0f, flux[(size_t) i] - window[window.size() / 2]);
        }

        const float top = *std::max_element (novelty.begin(), novelty.end());
        if (top <= 1.0e-6f)
            return out;

        const int lastPosition = -1;
        juce::ignoreUnused (lastPosition);
        for (int i = 1; i < frames - 1; ++i)
        {
            const float v = novelty[(size_t) i];
            if (v <= 0.02f * top)
                continue;
            bool isPeak = true;
            for (int k = std::max (0, i - 3); k <= std::min (frames - 1, i + 3) && isPeak; ++k)
                if (k != i && (novelty[(size_t) k] > v || (novelty[(size_t) k] == v && k < i)))
                    isPeak = false;
            if (! isPeak)
                continue;

            // The flux of frame i compares windows centred hop samples apart: find where the attack starts.
            const int centre = i * env.hop + env.fftSize / 2;
            const int from = std::clamp (centre - (3 * env.hop) / 2 - env.fftSize / 4, 0, length - 1);
            const int to = std::clamp (centre + env.hop / 2, 0, length - 1);
            float peak = 0.0f;
            for (int s = from; s <= to; ++s)
                peak = std::max (peak, std::abs (mono[s]));
            int pos = from;
            if (peak > 0.0f)
            {
                // The loudest point of the window, then back to where the level first passed a fifth of it.
                int loudest = from;
                for (int s = from; s <= to; ++s)
                    if (std::abs (mono[s]) >= peak)
                    {
                        loudest = s;
                        break;
                    }
                pos = loudest;
                const int floorStart = std::max (from, loudest - (int) (0.03 * env.frameRate * env.hop));
                while (pos > floorStart && std::abs (mono[pos - 1]) > 0.2f * peak)
                    --pos;
                // Walk back over the quiet run-up to the last zero crossing.
                while (pos > floorStart && mono[pos - 1] * mono[pos] > 0.0f)
                    --pos;
            }
            pos = std::max (0, pos - (int) (0.001 * env.frameRate * env.hop));   // 1 ms pre-roll
            if (! out.empty() && pos <= out.back().position)
                continue;
            out.push_back ({ pos, v / top });
        }
        return out;
    }

    //==============================================================================
    namespace
    {
        std::vector<float> autocorrelation (const std::vector<float>& flux, double frameRate, int maxLag)
        {
            const int n = (int) flux.size();
            std::vector<float> ac;
            if (n < 8)
                return ac;

            // Remove the slow trend (100 ms moving average) and keep the peaks.
            const int k = std::max (3, (int) std::lround (frameRate * 0.1));
            std::vector<float> o ((size_t) n);
            double running = 0.0;
            std::vector<double> prefix ((size_t) n + 1, 0.0);
            for (int i = 0; i < n; ++i)
                prefix[(size_t) i + 1] = prefix[(size_t) i] + flux[(size_t) i];
            for (int i = 0; i < n; ++i)
            {
                const int a = std::max (0, i - k / 2), b = std::min (n, i - k / 2 + k);
                const double mean = (prefix[(size_t) b] - prefix[(size_t) a]) / (double) k;
                o[(size_t) i] = std::max (0.0f, flux[(size_t) i] - (float) mean);
            }
            juce::ignoreUnused (running);
            const float mean = std::accumulate (o.begin(), o.end(), 0.0f) / (float) n;
            for (auto& v : o)
                v -= mean;

            maxLag = std::min (maxLag, n - 1);
            ac.assign ((size_t) maxLag + 2, 0.0f);
            for (int lag = 0; lag <= maxLag; ++lag)
            {
                double s = 0.0;
                for (int i = 0; i + lag < n; ++i)
                    s += (double) o[(size_t) i] * o[(size_t) (i + lag)];
                ac[(size_t) lag] = (float) s;
            }
            const float zero = ac[0] > 1.0e-12f ? ac[0] : 1.0f;
            for (auto& v : ac)
                v /= zero;
            return ac;
        }

        float acAt (const std::vector<float>& ac, double lag)
        {
            const int i = (int) lag;
            if (i < 1 || i + 1 >= (int) ac.size())
                return 0.0f;
            const float f = (float) (lag - i);
            return ac[(size_t) i] * (1.0f - f) + ac[(size_t) i + 1] * f;
        }

        float beatScore (const std::vector<float>& ac, double frameRate, double bpm)
        {
            const double beat = 60.0 / bpm * frameRate;
            return 0.35f * acAt (ac, beat * 0.5) + 1.0f * acAt (ac, beat) + 0.6f * acAt (ac, beat * 2.0)
                 + 0.4f * acAt (ac, beat * 4.0) + 0.15f * acAt (ac, beat * 0.25);
        }
    }

    double estimateTempo (const OnsetEnvelope& env, double duration)
    {
        if (env.flux.size() < 16 || env.frameRate <= 0.0 || duration < 0.75)
            return 0.0;

        // The first 30 s are enough to find a beat.
        std::vector<float> flux (env.flux.begin(), env.flux.begin() + (long) std::min (env.flux.size(), (size_t) (30.0 * env.frameRate)));
        const int maxLag = (int) std::ceil (4.0 * 60.0 / 70.0 * env.frameRate) + 4;
        const auto ac = autocorrelation (flux, env.frameRate, maxLag);
        if (ac.empty())
            return 0.0;

        double free = 0.0;
        float best = -1.0e9f;
        for (double bpm = 70.0; bpm < 190.0; bpm += 0.25)
        {
            const float s = beatScore (ac, env.frameRate, bpm);
            if (s > best)
            {
                best = s;
                free = bpm;
            }
        }
        if (best <= 0.02f)
            return 0.0;

        double result = free;
        if (duration <= 20.0)
        {
            // Trimmed loops hold a whole power-of-two number of beats: try simple ratios of the free estimate.
            static constexpr double ratios[] = { 1.0, 2.0, 0.5, 1.5, 2.0 / 3.0, 4.0 / 3.0, 0.75 };
            static constexpr float prefs[] = { 1.0f, 0.97f, 0.97f, 0.93f, 0.93f, 0.9f, 0.9f };
            float bestSnap = -1.0e9f;
            for (size_t r = 0; r < std::size (ratios); ++r)
            {
                const double t = free * ratios[r];
                const double beats = duration * t / 60.0;
                for (int p : { 2, 4, 8, 16, 32, 64 })
                {
                    if (std::abs (beats - p) / p >= 0.04)
                        continue;
                    const double snapped = 60.0 * p / duration;
                    if (snapped < 70.0 || snapped > 190.0)
                        continue;
                    const float s = beatScore (ac, env.frameRate, snapped) * prefs[r];
                    if (s > bestSnap)
                    {
                        bestSnap = s;
                        result = snapped;
                    }
                }
            }
        }
        return result;
    }

    double nearestOctave (double bpm, double hostBpm)
    {
        if (bpm <= 0.0 || hostBpm <= 0.0)
            return bpm;
        double best = bpm;
        for (int k = -2; k <= 2; ++k)
        {
            const double c = bpm * std::exp2 ((double) k);
            if (c >= 55.0 && c <= 230.0 && std::abs (std::log2 (c / hostBpm)) < std::abs (std::log2 (best / hostBpm)))
                best = c;
        }
        return best;
    }

    //==============================================================================
    double tempoFromName (const juce::String& name, double duration, bool allowBare)
    {
        const auto lower = name.toLowerCase();

        // Explicit: a number next to "bpm".
        for (int at = lower.indexOf ("bpm"); at >= 0; at = lower.indexOf (at + 3, "bpm"))
        {
            // Before: "174bpm", "174 bpm", "174_bpm", "[174bpm]"
            int end = at;
            while (end > 0 && (lower[end - 1] == ' ' || lower[end - 1] == '_' || lower[end - 1] == '-'))
                --end;
            int start = end;
            while (start > 0 && (juce::CharacterFunctions::isDigit (lower[start - 1]) || lower[start - 1] == '.'))
                --start;
            if (start < end)
            {
                const double v = lower.substring (start, end).getDoubleValue();
                if (v >= 40.0 && v <= 300.0)
                    return v;
            }
            // After: "bpm120", "bpm_120"
            int s = at + 3;
            while (s < lower.length() && (lower[s] == ' ' || lower[s] == '_' || lower[s] == '-'))
                ++s;
            int e = s;
            while (e < lower.length() && juce::CharacterFunctions::isDigit (lower[e]))
                ++e;
            if (e > s)
            {
                const double v = lower.substring (s, e).getDoubleValue();
                if (v >= 40.0 && v <= 300.0)
                    return v;
            }
        }

        if (! allowBare || duration <= 0.5)
            return 0.0;

        // Bare: a separate 2-3 digit number that fits the file's length.
        for (int i = 0; i < lower.length(); ++i)
        {
            if (! juce::CharacterFunctions::isDigit (lower[i]) || (i > 0 && (juce::CharacterFunctions::isDigit (lower[i - 1]) || lower[i - 1] == '.')))
                continue;
            int e = i;
            while (e < lower.length() && juce::CharacterFunctions::isDigit (lower[e]))
                ++e;
            const int len = e - i;
            if (len < 2 || len > 3 || (e < lower.length() && lower[e] == '.'))
            {
                i = e;
                continue;
            }
            const double v = lower.substring (i, e).getDoubleValue();
            if (v >= 60.0 && v <= 200.0)
            {
                const double beats = duration * v / 60.0;
                const double whole = std::round (beats);
                if (whole >= 2.0 && std::abs (beats - whole) / whole < 0.03)
                    return v;
            }
            i = e;
        }
        return 0.0;
    }

    //==============================================================================
    float sensitivityThreshold (float sensitivity) noexcept
    {
        const float s = std::clamp (sensitivity, 0.0f, 1.0f);
        return 0.6f * (1.0f - s) * (1.0f - s) + 0.01f;
    }

    int gridSlices (int length, double beats, int divisionsPerBeat, int* out, int maxOut)
    {
        if (length <= 0 || maxOut <= 0)
            return 0;
        const int n = std::clamp ((int) std::lround (std::max (1.0, beats) * divisionsPerBeat), 1, maxOut);
        for (int i = 0; i < n; ++i)
            out[i] = (int) ((juce::int64) length * i / n);
        return n;
    }

    int onsetSlices (const int* positions, const float* strengths, int count, float sensitivity,
                     int minSpacing, int length, int* out, int maxOut, int* scratch) noexcept
    {
        if (length <= 0 || maxOut <= 0)
            return 0;
        const float threshold = sensitivityThreshold (sensitivity);

        // Strongest first (ties: earlier first), each at least minSpacing from every slice already taken.
        int candidates = 0;
        for (int i = 0; i < count; ++i)
            if (strengths[i] >= threshold && positions[i] > 0 && positions[i] < length)
                scratch[candidates++] = i;
        std::sort (scratch, scratch + candidates, [strengths, positions] (int a, int b)
                   { return strengths[a] != strengths[b] ? strengths[a] > strengths[b] : positions[a] < positions[b]; });

        int n = 0;
        out[n++] = 0;
        for (int c = 0; c < candidates && n < maxOut; ++c)
        {
            const int p = positions[scratch[c]];
            bool clear = true;
            for (int t = 0; t < n && clear; ++t)
                clear = std::abs (out[t] - p) >= minSpacing;
            if (clear)
                out[n++] = p;
        }
        std::sort (out, out + n);
        return n;
    }
}
