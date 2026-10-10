#pragma once

#include <vector>
#include "Common.h"

namespace labx0::dsp
{
    // REGULATOR: keeps the level steady and the peaks under a ceiling.
    //
    // 1. A stereo-linked compressor. Its detector holds each peak for 50 ms, longer than one cycle of
    //    the lowest sub note, so a steady bass note sees a steady gain and the compressor never ripples
    //    inside the waveform. Threshold, ratio and makeup all follow one AMOUNT control.
    // 2. WEIGHT: a 60 Hz low shelf.
    // 3. A look-ahead peak limiter: the gain reaches its target before the peak leaves the delay line,
    //    so the output never exceeds the ceiling and the gain never steps.
    class Regulator
    {
    public:
        void prepare (float sr)
        {
            sampleRate = sr;
            lookahead = std::max (2, (int) std::lround (0.0015 * (double) sr));
            delayL.assign ((size_t) lookahead, 0.0f);
            delayR.assign ((size_t) lookahead, 0.0f);
            targets.assign ((size_t) lookahead, 1.0f);
            held.assign ((size_t) lookahead, 1.0f);

            holdSamples = (int) (0.05f * sr);
            detectorRelease = std::exp (-1.0f / (0.08f * sr));
            attackCoef = onePoleCoef (0.005f, sr);
            releaseCoef = onePoleCoef (0.08f, sr);
            limiterRelease = onePoleCoef (0.05f, sr);
            reset();
        }

        // Samples of delay added by the look-ahead.
        int getLatency() const noexcept { return lookahead - 1; }

        void reset() noexcept
        {
            std::fill (delayL.begin(), delayL.end(), 0.0f);
            std::fill (delayR.begin(), delayR.end(), 0.0f);
            std::fill (targets.begin(), targets.end(), 1.0f);
            std::fill (held.begin(), held.end(), 1.0f);
            heldSum = (double) lookahead;
            pos = 0;
            env = 0.0f;
            holdCount = 0;
            gainDb = 0.0f;
            limitGain = 1.0f;
            shelfL = shelfR = {};
            lastWeight = 1000.0f;
            gainReductionDb = 0.0f;
        }

        void process (float* left, float* right, int n, float amount, float ceilingDb, float weightDb) noexcept
        {
            amount = std::clamp (amount, 0.0f, 1.0f);
            const bool compress = amount > 0.001f;
            const float threshold = -30.0f * amount;
            const float ratio = 1.0f + 7.0f * amount;
            const float slope = 1.0f / ratio - 1.0f;
            const float makeup = -threshold * (1.0f - 1.0f / ratio) * 0.5f;
            constexpr float knee = 6.0f;

            if (std::abs (weightDb - lastWeight) > 0.01f)
            {
                lastWeight = weightDb;
                designShelf (weightDb);
            }
            const bool shelving = std::abs (weightDb) > 0.01f;

            const float ceiling = dbToGain (std::clamp (ceilingDb, -24.0f, 0.0f));
            float maxReduction = 0.0f;

            for (int i = 0; i < n; ++i)
            {
                float l = left[i], r = right[i];

                // 1. Compressor
                if (compress)
                {
                    const float x = std::max (std::abs (l), std::abs (r));
                    if (x >= env)
                    {
                        env = x;
                        holdCount = holdSamples;
                    }
                    else if (holdCount > 0)
                        --holdCount;
                    else
                        env *= detectorRelease;

                    const float over = gainToDb (env) - threshold;
                    float reduction = 0.0f;
                    if (over >= 0.5f * knee)
                        reduction = slope * over;
                    else if (over > -0.5f * knee)
                        reduction = slope * (over + 0.5f * knee) * (over + 0.5f * knee) / (2.0f * knee);

                    const float targetDb = reduction + makeup;
                    gainDb += (targetDb - gainDb) * (targetDb < gainDb ? attackCoef : releaseCoef);
                    maxReduction = std::min (maxReduction, reduction);
                }
                else
                {
                    gainDb += (0.0f - gainDb) * releaseCoef;
                }

                if (std::abs (gainDb) > 1.0e-4f)
                {
                    const float g = dbToGain (gainDb);
                    l *= g;
                    r *= g;
                }

                // 2. Weight shelf
                if (shelving)
                {
                    l = shelfL.process (l, coeffs);
                    r = shelfR.process (r, coeffs);
                }

                // 3. Look-ahead limiter: moving minimum of the required gain, then a box average of the
                //    same length, so the gain is fully down when the peak leaves the delay line.
                const float peak = std::max (std::abs (l), std::abs (r));
                targets[(size_t) pos] = peak > ceiling ? ceiling / peak : 1.0f;
                float windowMin = 1.0f;
                for (float t : targets)
                    windowMin = std::min (windowMin, t);

                heldSum += (double) windowMin - (double) held[(size_t) pos];
                held[(size_t) pos] = windowMin;
                const float smooth = std::min (1.0f, (float) (heldSum / (double) lookahead));

                limitGain = smooth < limitGain ? smooth : limitGain + (smooth - limitGain) * limiterRelease;

                const int readPos = (pos + 1) % lookahead;   // oldest sample: lookahead - 1 behind
                const float outL = delayL[(size_t) readPos];
                const float outR = delayR[(size_t) readPos];
                delayL[(size_t) pos] = l;
                delayR[(size_t) pos] = r;
                pos = readPos;

                left[i] = outL * limitGain;
                right[i] = outR * limitGain;
            }

            gainReductionDb = std::min (maxReduction, gainToDb (limitGain));
        }

        float getGainReductionDb() const noexcept { return gainReductionDb; }

    private:
        struct Biquad
        {
            float z1 = 0.0f, z2 = 0.0f;

            template <typename C>
            float process (float x, const C& c) noexcept
            {
                const float y = c.b0 * x + z1;
                z1 = c.b1 * x - c.a1 * y + z2;
                z2 = c.b2 * x - c.a2 * y;
                return y;
            }
        };

        struct Coeffs { float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f; };

        // RBJ low shelf at 60 Hz, shelf slope 1.
        void designShelf (float db) noexcept
        {
            const double A = std::pow (10.0, (double) db / 40.0);
            const double w0 = 2.0 * 3.14159265358979323846 * 60.0 / (double) sampleRate;
            const double cw = std::cos (w0), sw = std::sin (w0);
            const double alpha = sw / 2.0 * std::sqrt (2.0);
            const double sA = 2.0 * std::sqrt (A) * alpha;

            const double b0 = A * ((A + 1) - (A - 1) * cw + sA);
            const double b1 = 2 * A * ((A - 1) - (A + 1) * cw);
            const double b2 = A * ((A + 1) - (A - 1) * cw - sA);
            const double a0 = (A + 1) + (A - 1) * cw + sA;
            const double a1 = -2 * ((A - 1) + (A + 1) * cw);
            const double a2 = (A + 1) + (A - 1) * cw - sA;

            coeffs.b0 = (float) (b0 / a0);
            coeffs.b1 = (float) (b1 / a0);
            coeffs.b2 = (float) (b2 / a0);
            coeffs.a1 = (float) (a1 / a0);
            coeffs.a2 = (float) (a2 / a0);
        }

        float sampleRate = 48000.0f;
        int lookahead = 72, pos = 0, holdSamples = 2400, holdCount = 0;
        std::vector<float> delayL, delayR, targets, held;
        double heldSum = 0.0;
        float env = 0.0f, gainDb = 0.0f, limitGain = 1.0f;
        float detectorRelease = 0.9997f, attackCoef = 0.004f, releaseCoef = 0.0003f, limiterRelease = 0.0004f;
        Biquad shelfL, shelfR;
        Coeffs coeffs;
        float lastWeight = 1000.0f;
        float gainReductionDb = 0.0f;
    };
}
