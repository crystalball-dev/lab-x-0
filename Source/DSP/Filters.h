#pragma once

#include "Common.h"

namespace labx0::dsp
{
    // Topology-preserving state-variable filter (Zavalishin). Stable under per-sample modulation.
    struct SVF
    {
        float ic1 = 0.0f, ic2 = 0.0f;
        float g = 0.0f, k = 2.0f, a1 = 1.0f, a2 = 0.0f, a3 = 0.0f;

        void reset() noexcept { ic1 = ic2 = 0.0f; }

        void set (float newG, float newK) noexcept
        {
            g = newG;
            k = newK;
            a1 = 1.0f / (1.0f + g * (g + k));
            a2 = g * a1;
            a3 = g * a2;
        }

        static float gFor (float hz, float sampleRate) noexcept
        {
            const float f = std::clamp (hz, 10.0f, sampleRate * 0.45f);
            return std::tan (pi * f / sampleRate);
        }

        void process (float v0, float& lp, float& bp, float& hp) noexcept
        {
            const float v3 = v0 - ic2;
            const float v1 = a1 * ic1 + a2 * v3;
            const float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;
            lp = v2;
            bp = v1;
            hp = v0 - k * v1 - v2;
        }

        float lowpass (float v0) noexcept
        {
            float lp, bp, hp;
            process (v0, lp, bp, hp);
            return lp;
        }

        // Band-pass whose peak gain is 1 / k.
        float bandpass (float v0) noexcept
        {
            float lp, bp, hp;
            process (v0, lp, bp, hp);
            return bp;
        }
    };

    // Four-pole transistor ladder in zero-delay-feedback form. The feedback loop is solved linearly,
    // then the loop input is saturated, so the filter self-oscillates at full resonance without blowing up.
    // Output makeup keeps the level roughly steady as resonance thins out the bass.
    struct TransistorLadder
    {
        float s1 = 0.0f, s2 = 0.0f, s3 = 0.0f, s4 = 0.0f;
        float G = 0.0f, G2 = 0.0f, G3 = 0.0f, G4 = 0.0f, oneMinusG = 1.0f;

        static constexpr float maxK = 4.15f;   // self-oscillation starts at k = 4

        void reset() noexcept { s1 = s2 = s3 = s4 = 0.0f; }

        // g = tan (pi * fc / fs)
        void setG (float g) noexcept
        {
            G = g / (1.0f + g);
            G2 = G * G;
            G3 = G2 * G;
            G4 = G3 * G;
            oneMinusG = 1.0f - G;
        }

        static float kFor (float resonance) noexcept { return maxK * std::clamp (resonance, 0.0f, 1.0f); }
        static float makeupFor (float k) noexcept    { return 1.0f + 0.6f * k; }

        float process (float x, float k) noexcept
        {
            const float S = oneMinusG * (G3 * s1 + G2 * s2 + G * s3 + s4);
            const float y4Linear = (G4 * x + S) / (1.0f + k * G4);
            const float u = 2.0f * fastTanh (0.5f * (x - k * y4Linear));

            float v = (u - s1) * G;  const float y1 = v + s1; s1 = y1 + v;
            v = (y1 - s2) * G;       const float y2 = v + s2; s2 = y2 + v;
            v = (y2 - s3) * G;       const float y3 = v + s3; s3 = y3 + v;
            v = (y3 - s4) * G;       const float y4 = v + s4; s4 = y4 + v;
            return y4;
        }
    };

    // TB-303 style diode ladder (Zavalishin's analysis in the TPT form popularised by Will Pirkle).
    // The four stages feed back into each other, which gives the softer, squelchier response and the
    // bass loss at high resonance that define acid. The resonant peak sits at about 0.7 of the
    // coefficient frequency, so the requested cutoff is scaled up to put the peak where it is asked for.
    struct DiodeLadder
    {
        float z1 = 0.0f, z2 = 0.0f, z3 = 0.0f, z4 = 0.0f;
        float gamma = 0.0f, sg1 = 0.0f, sg2 = 0.0f, sg3 = 0.0f;
        float alpha = 0.0f, beta1 = 1.0f, beta2 = 1.0f, beta3 = 1.0f, beta4 = 1.0f;
        float gamma1 = 1.0f, gamma2 = 1.0f, gamma3 = 1.0f;
        float delta1 = 0.0f, delta2 = 0.0f, delta3 = 0.0f;
        float eps1 = 0.0f, eps2 = 0.0f, eps3 = 0.0f;

        static constexpr float maxK = 17.2f;          // self-oscillation starts near k = 17
        static constexpr float peakScale = 1.0f / 0.70f;

        void reset() noexcept { z1 = z2 = z3 = z4 = 0.0f; }

        static float kFor (float resonance) noexcept { return maxK * std::clamp (resonance, 0.0f, 1.0f); }
        static float makeupFor (float k) noexcept    { return 1.0f + 0.33f * k; }

        // g = tan (pi * fc / fs) for the already scaled coefficient frequency
        void setG (float g) noexcept
        {
            const float G4 = 0.5f * g / (1.0f + g);
            const float G3 = 0.5f * g / (1.0f + g - 0.5f * g * G4);
            const float G2 = 0.5f * g / (1.0f + g - 0.5f * g * G3);
            const float G1 = g / (1.0f + g - g * G2);

            gamma = G4 * G3 * G2 * G1;
            sg1 = G4 * G3 * G2;
            sg2 = G4 * G3;
            sg3 = G4;

            alpha = g / (1.0f + g);
            beta1 = 1.0f / (1.0f + g - g * G2);
            beta2 = 1.0f / (1.0f + g - 0.5f * g * G3);
            beta3 = 1.0f / (1.0f + g - 0.5f * g * G4);
            beta4 = 1.0f / (1.0f + g);

            gamma1 = 1.0f + G1 * G2;
            gamma2 = 1.0f + G2 * G3;
            gamma3 = 1.0f + G3 * G4;

            delta1 = g;
            delta2 = 0.5f * g;
            delta3 = 0.5f * g;

            eps1 = G2;
            eps2 = G3;
            eps3 = G4;
        }

        float process (float x, float k) noexcept
        {
            // Feedback outputs, each stage seen from the one before it
            const float fo4 = beta4 * z4;
            const float fb3 = fo4;
            const float fo3 = beta3 * (z3 + fb3 * delta3);
            const float fb2 = fo3;
            const float fo2 = beta2 * (z2 + fb2 * delta2);
            const float fb1 = fo2;
            const float fo1 = beta1 * (z1 + fb1 * delta1);

            const float sigma = sg1 * fo1 + sg2 * fo2 + sg3 * fo3 + fo4;
            const float u = 2.0f * fastTanh (0.5f * (x - k * sigma) / (1.0f + k * gamma));

            float in = u * gamma1 + fb1 + eps1 * fo1;
            float v = (in - z1) * alpha;
            const float y1 = v + z1;
            z1 = y1 + v;

            in = y1 * gamma2 + fb2 + eps2 * fo2;
            v = (0.5f * in - z2) * alpha;
            const float y2 = v + z2;
            z2 = y2 + v;

            in = y2 * gamma3 + fb3 + eps3 * fo3;
            v = (0.5f * in - z3) * alpha;
            const float y3 = v + z3;
            z3 = y3 + v;

            v = (0.5f * y3 - z4) * alpha;
            const float y4 = v + z4;
            z4 = y4 + v;
            return y4;
        }
    };
}
