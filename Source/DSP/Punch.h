#pragma once

#include <array>
#include <memory>
#include <juce_dsp/juce_dsp.h>
#include "Common.h"
#include "Filters.h"

namespace labx0::dsp
{
    //==============================================================================
    // TRANSIENT: the gap between a fast and a slow envelope follower marks attacks (fast above slow) and
    // tails (slow above fast). ATTACK and SUSTAIN turn each gap into gain, up to +-18 dB.
    class Transient
    {
    public:
        void prepare (float sr) noexcept
        {
            fastAttack = onePoleCoef (0.0005f, sr);
            fastRelease = onePoleCoef (0.02f, sr);
            slowAttack = onePoleCoef (0.015f, sr);
            slowRelease = onePoleCoef (0.15f, sr);
            gainCoef = onePoleCoef (0.0003f, sr);
            reset();
        }

        void reset() noexcept { fast = slow = 0.0f; gainDb = 0.0f; meterDb = 0.0f; }

        void process (float* l, float* r, int n, float attack, float sustain) noexcept
        {
            const bool active = std::abs (attack) > 0.005f || std::abs (sustain) > 0.005f;
            float peak = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                const float x = std::max (std::abs (l[i]), std::abs (r[i]));
                fast += (x - fast) * (x > fast ? fastAttack : fastRelease);
                slow += (x - slow) * (x > slow ? slowAttack : slowRelease);
                if (! active)
                    continue;

                const float gap = gainToDb (fast + 1.0e-6f) - gainToDb (slow + 1.0e-6f);
                float target = 0.75f * (attack * std::clamp (gap, 0.0f, 24.0f) + sustain * std::clamp (-gap, 0.0f, 24.0f));
                // Below -60 dBFS there is nothing to shape: stop gain from creeping up the noise floor.
                if (slow < 0.001f)
                    target *= slow * 1000.0f;
                target = std::clamp (target, -18.0f, 18.0f);
                gainDb += (target - gainDb) * gainCoef;
                const float g = dbToGain (gainDb);
                l[i] *= g;
                r[i] *= g;
                peak = std::max (peak, std::abs (gainDb));
            }
            meterDb = active ? peak : 0.0f;
        }

        float getMeterDb() const noexcept { return meterDb; }

    private:
        float fast = 0.0f, slow = 0.0f, gainDb = 0.0f, meterDb = 0.0f;
        float fastAttack = 0.5f, fastRelease = 0.01f, slowAttack = 0.01f, slowRelease = 0.001f, gainCoef = 0.1f;
    };

    //==============================================================================
    // Linkwitz-Riley 4th order: two Butterworth sections in series. Low and high always sum to an all-pass.
    struct LR4
    {
        SVF first, low2, high2;

        void reset() noexcept { first.reset(); low2.reset(); high2.reset(); }

        void set (float hz, float sampleRate) noexcept
        {
            const float g = SVF::gFor (hz, sampleRate);
            first.set (g, 1.41421356f);
            low2.set (g, 1.41421356f);
            high2.set (g, 1.41421356f);
        }

        void process (float x, float& low, float& high) noexcept
        {
            float lp, bp, hp;
            first.process (x, lp, bp, hp);
            float lp2, bp2, hp2;
            low2.process (lp, lp2, bp2, hp2);
            low = lp2;
            float lp3, bp3, hp3;
            high2.process (hp, lp3, bp3, hp3);
            high = hp3;
        }
    };

    //==============================================================================
    // MULTIBAND: three LR4 bands, each pushed down above a threshold and pulled up below another, OTT style.
    // DEPTH blends inside the band split (dry bands + processed bands), so no comb filtering against the dry path.
    class Multiband
    {
    public:
        void prepare (float sr) noexcept
        {
            sampleRate = sr;
            detectorRelease = std::exp (-1.0f / (0.03f * sr));
            engageStep = 1.0f / (0.02f * sr);
            lastLow = lastHigh = -1.0f;
            reset();
        }

        void reset() noexcept
        {
            for (auto& c : ch)
            {
                c.a.reset();
                c.b.reset();
                c.ap.reset();
            }
            for (auto& b : bands)
                b = {};
            engaged = 0.0f;
        }

        void process (float* l, float* r, int n, float depth, float time, float up, float down,
                      float lowHz, float highHz, const float* outDb) noexcept
        {
            depth = std::clamp (depth, 0.0f, 1.0f);
            if (std::abs (lowHz - lastLow) > 0.5f || std::abs (highHz - lastHigh) > 2.0f)
            {
                lastLow = lowHz;
                lastHigh = highHz;
                for (auto& c : ch)
                {
                    c.a.set (lowHz, sampleRate);
                    c.b.set (highHz, sampleRate);
                    c.ap.set (highHz, sampleRate);
                }
            }

            const float attackMs = 0.3f + 12.0f * time * time;
            const float releaseMs = 20.0f + 330.0f * time;
            const float attackCoef = onePoleCoef (attackMs * 0.001f, sampleRate);
            const float releaseCoef = onePoleCoef (releaseMs * 0.001f, sampleRate);

            constexpr float downThreshold = -18.0f, upThreshold = -42.0f, floorDb = -66.0f;
            const float downSlope = (1.0f - 1.0f / 10.0f) * std::clamp (down, 0.0f, 1.0f);
            const float upSlope = (1.0f - 1.0f / 4.0f) * std::clamp (up, 0.0f, 1.0f);
            const float makeup = 0.45f * -downThreshold * downSlope;
            const bool processing = depth > 0.0005f;

            for (auto& b : bands)
                b.meter = 0.0f;

            for (int i = 0; i < n; ++i)
            {
                // Unused (DEPTH 0), the stage hands back its input untouched: no crossover phase on the drums.
                engaged = std::clamp (engaged + (processing ? engageStep : -engageStep), 0.0f, 1.0f);
                const float dryL = l[i], dryR = r[i];
                float split[2][3];
                float* io[2] = { l + i, r + i };
                for (int c = 0; c < 2; ++c)
                {
                    float low, rest, mid, high, apLow, apHigh;
                    ch[(size_t) c].a.process (*io[c], low, rest);
                    ch[(size_t) c].b.process (rest, mid, high);
                    ch[(size_t) c].ap.process (low, apLow, apHigh);   // the low band through the same phase as the others
                    split[c][0] = apLow + apHigh;
                    split[c][1] = mid;
                    split[c][2] = high;
                }

                for (int b = 0; b < 3; ++b)
                {
                    auto& band = bands[(size_t) b];
                    const float x = std::max (std::abs (split[0][b]), std::abs (split[1][b]));
                    band.env = x > band.env ? x : band.env * detectorRelease;

                    float gain = 1.0f;
                    if (processing)
                    {
                        const float level = gainToDb (band.env + 1.0e-9f);
                        float targetDb = 0.0f;
                        if (level > downThreshold)
                            targetDb = -(level - downThreshold) * downSlope;
                        else if (level < upThreshold)
                        {
                            float lift = (upThreshold - level) * upSlope;
                            if (level < floorDb)
                                lift *= std::max (0.0f, 1.0f - (floorDb - level) / 12.0f);
                            targetDb = std::min (lift, 24.0f);
                        }
                        targetDb += makeup + outDb[b];
                        band.gainDb += (targetDb - band.gainDb) * (targetDb < band.gainDb ? attackCoef : releaseCoef);
                        gain = 1.0f + depth * (dbToGain (band.gainDb) - 1.0f);
                        band.meter = std::max (band.meter, std::abs (band.gainDb - makeup - outDb[b]));
                    }
                    split[0][b] *= gain;
                    split[1][b] *= gain;
                }

                const float wetL = split[0][0] + split[0][1] + split[0][2];
                const float wetR = split[1][0] + split[1][1] + split[1][2];
                *io[0] = dryL + engaged * (wetL - dryL);
                *io[1] = dryR + engaged * (wetR - dryR);
            }
        }

        float getBandMeterDb (int b) const noexcept { return bands[(size_t) b].meter; }

    private:
        struct Channel
        {
            LR4 a, b, ap;
        };
        struct Band
        {
            float env = 0.0f, gainDb = 0.0f, meter = 0.0f;
        };

        std::array<Channel, 2> ch {};
        std::array<Band, 3> bands {};
        float sampleRate = 48000.0f, detectorRelease = 0.999f, lastLow = -1.0f, lastHigh = -1.0f;
        float engaged = 0.0f, engageStep = 0.001f;
    };

    //==============================================================================
    // SLAM, after BeatSkillz Slam Pup: BARK is three asymmetric tube stages behind a pre-emphasis that follows the
    // drive (and the matching de-emphasis after them); BITE is a fast, high-ratio compressor that pumps when
    // pushed, its detector optionally blind to the lowest band. WET blends the slammed signal with the dry one.
    // Runs inside the 2x oversampled stage.
    class Slam
    {
    public:
        void prepare (float sr) noexcept
        {
            sampleRate = sr;
            attackCoef = onePoleCoef (0.002f, sr);
            releaseCoef = onePoleCoef (0.09f, sr);
            lastBark = -1.0f;
            lastSc = -1.0f;
            reset();
        }

        void reset() noexcept
        {
            for (auto& s : shelf)
                s.reset();
            for (auto& s : deshelf)
                s.reset();
            scL.reset();
            scR.reset();
            env = 0.0f;
            gainDb = 0.0f;
            meterDb = 0.0f;
        }

        void process (float* l, float* r, int n, float bark, float bite, float wet, float scHz) noexcept
        {
            bark = std::clamp (bark, 0.0f, 1.0f);
            bite = std::clamp (bite, 0.0f, 1.0f);
            wet = std::clamp (wet, 0.0f, 1.0f);
            meterDb = 0.0f;
            if ((bark < 0.001f && bite < 0.001f) || wet < 0.001f)
            {
                env = 0.0f;
                gainDb = 0.0f;
                return;
            }

            if (std::abs (bark - lastBark) > 0.002f)
            {
                lastBark = bark;
                designShelves (6.0f * bark);
            }
            if (std::abs (scHz - lastSc) > 0.5f)
            {
                lastSc = scHz;
                if (scHz > 0.0f)
                {
                    const float g = SVF::gFor (scHz, sampleRate);
                    scL.set (g, 1.41421356f);
                    scR.set (g, 1.41421356f);
                }
            }

            const float driveDb = 30.0f * bark;
            const float stage = dbToGain (driveDb / 3.0f);
            const float bias = 0.25f * bark;
            const float tb = std::tanh (bias);
            const float norm = 1.0f / (stage * (1.0f - tb * tb));
            const float makeupBark = dbToGain (9.0f * bark);

            const float threshold = -30.0f * bite;
            const float ratio = 4.0f + 8.0f * bite;
            const float slope = 1.0f / ratio - 1.0f;
            const float makeupBite = -threshold * (1.0f - 1.0f / ratio) * 0.5f;
            constexpr float knee = 6.0f;
            float maxReduction = 0.0f;

            for (int i = 0; i < n; ++i)
            {
                const float dryL = l[i], dryR = r[i];
                float a = dryL, b = dryR;

                if (bark > 0.001f)
                {
                    a = shelf[0].process (a, shelfCoeffs);
                    b = shelf[1].process (b, shelfCoeffs);
                    for (int s = 0; s < 3; ++s)
                    {
                        a = (std::tanh (stage * a + bias) - tb) * norm;
                        b = (std::tanh (stage * b + bias) - tb) * norm;
                    }
                    a = deshelf[0].process (a, deshelfCoeffs) * makeupBark;
                    b = deshelf[1].process (b, deshelfCoeffs) * makeupBark;
                }

                if (bite > 0.001f)
                {
                    float da = a, db = b;
                    if (lastSc > 0.0f)
                    {
                        float lp, bp, hp;
                        scL.process (a, lp, bp, hp);
                        da = hp;
                        scR.process (b, lp, bp, hp);
                        db = hp;
                    }
                    const float x = std::max (std::abs (da), std::abs (db));
                    env += (x - env) * (x > env ? attackCoef : releaseCoef);
                    const float over = gainToDb (env + 1.0e-9f) - threshold;
                    float reduction = 0.0f;
                    if (over >= 0.5f * knee)
                        reduction = slope * over;
                    else if (over > -0.5f * knee)
                        reduction = slope * (over + 0.5f * knee) * (over + 0.5f * knee) / (2.0f * knee);
                    gainDb = reduction + makeupBite;
                    maxReduction = std::min (maxReduction, reduction);
                    const float g = dbToGain (gainDb);
                    a *= g;
                    b *= g;
                }

                l[i] = dryL + wet * (a - dryL);
                r[i] = dryR + wet * (b - dryR);
            }
            meterDb = -maxReduction;
        }

        float getMeterDb() const noexcept { return meterDb; }

    private:
        struct Coeffs { float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f; };
        struct Biquad
        {
            float z1 = 0.0f, z2 = 0.0f;
            void reset() noexcept { z1 = z2 = 0.0f; }
            float process (float x, const Coeffs& c) noexcept
            {
                const float y = c.b0 * x + z1;
                z1 = c.b1 * x - c.a1 * y + z2;
                z2 = c.b2 * x - c.a2 * y;
                return y;
            }
        };

        // RBJ high shelf at 1.5 kHz and its exact inverse.
        void designShelves (float db) noexcept
        {
            const double A = std::pow (10.0, (double) db / 40.0);
            const double w0 = 2.0 * 3.14159265358979323846 * 1500.0 / (double) sampleRate;
            const double cw = std::cos (w0), sw = std::sin (w0);
            const double alpha = sw / 2.0 * std::sqrt (2.0);
            const double sA = 2.0 * std::sqrt (A) * alpha;
            const double b0 = A * ((A + 1) + (A - 1) * cw + sA);
            const double b1 = -2 * A * ((A - 1) + (A + 1) * cw);
            const double b2 = A * ((A + 1) + (A - 1) * cw - sA);
            const double a0 = (A + 1) - (A - 1) * cw + sA;
            const double a1 = 2 * ((A - 1) - (A + 1) * cw);
            const double a2 = (A + 1) - (A - 1) * cw - sA;
            shelfCoeffs = { (float) (b0 / a0), (float) (b1 / a0), (float) (b2 / a0), (float) (a1 / a0), (float) (a2 / a0) };
            // Inverse: swap numerator and denominator.
            deshelfCoeffs = { (float) (a0 / b0), (float) (a1 / b0), (float) (a2 / b0), (float) (b1 / b0), (float) (b2 / b0) };
        }

        std::array<Biquad, 2> shelf {}, deshelf {};
        Coeffs shelfCoeffs, deshelfCoeffs;
        SVF scL, scR;
        float sampleRate = 96000.0f, attackCoef = 0.01f, releaseCoef = 0.001f;
        float env = 0.0f, gainDb = 0.0f, meterDb = 0.0f, lastBark = -1.0f, lastSc = -1.0f;
    };

    //==============================================================================
    // CLIP: linear up to 0.75, then a tanh knee that never passes 1.0. Drive 0 is bypassed entirely.
    struct Clipper
    {
        static float shape (float u) noexcept
        {
            constexpr float k = 0.75f;
            const float a = std::abs (u);
            if (a <= k)
                return u;
            const float y = k + (1.0f - k) * std::tanh ((a - k) / (1.0f - k));
            return u < 0.0f ? -y : y;
        }

        float meterDb = 0.0f;

        void process (float* l, float* r, int n, float driveDb) noexcept
        {
            meterDb = 0.0f;
            if (driveDb < 0.05f)
                return;
            const float g = dbToGain (driveDb);
            float most = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                const float a = l[i] * g, b = r[i] * g;
                const float ya = shape (a), yb = shape (b);
                most = std::max (most, std::max (std::abs (a) - std::abs (ya), std::abs (b) - std::abs (yb)));
                l[i] = ya;
                r[i] = yb;
            }
            meterDb = most > 1.0e-4f ? gainToDb (1.0f + most) : 0.0f;
        }
    };

    //==============================================================================
    // The oversampled part of the punch chain: SUB SAFE split, then SLAM and CLIP above the split; the band
    // below it is summed to mono and left untouched. Always runs (also when everything is at zero), so the
    // reported latency never changes.
    class PunchStage
    {
    public:
        void prepare (double sr, int maxBlock)
        {
            oversampler = std::make_unique<juce::dsp::Oversampling<float>> (2, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR,
                                                                           true, true);
            oversampler->initProcessing ((size_t) std::max (1, maxBlock));
            osRate = (float) (sr * 2.0);
            slam.prepare (osRate);
            lastSplit = -1.0f;
            reset();
        }

        int getLatency() const noexcept
        {
            return oversampler != nullptr ? (int) std::lround (oversampler->getLatencyInSamples()) : 0;
        }

        void reset() noexcept
        {
            if (oversampler != nullptr)
                oversampler->reset();
            slam.reset();
            splitL.reset();
            splitR.reset();
            splitMix = 0.0f;
        }

        void process (float* l, float* r, int n, float bark, float bite, float wet, float scHz, float clipDb, float splitHz) noexcept
        {
            if (oversampler == nullptr || n <= 0)
                return;
            float* chans[2] = { l, r };
            juce::dsp::AudioBlock<float> block (chans, 2, (size_t) n);
            auto up = oversampler->processSamplesUp (block);
            float* ul = up.getChannelPointer (0);
            float* ur = up.getChannelPointer (1);
            const int un = (int) up.getNumSamples();

            // While SLAM and CLIP do nothing the stage hands back its input untouched (no crossover phase on the
            // drums); once either works, it blends over 20 ms to the processed path: SLAM and CLIP above the SUB
            // SAFE split, the band below it summed to mono and left clean. The split filters always run, so
            // engaging them never starts from stale state.
            const bool working = ((bark > 0.001f || bite > 0.001f) && wet > 0.001f) || clipDb >= 0.05f;
            if (splitHz > 0.0f && std::abs (splitHz - lastSplit) > 0.5f)
            {
                lastSplit = splitHz;
                splitL.set (splitHz, osRate);
                splitR.set (splitHz, osRate);
            }
            for (int i = 0; i < un; ++i)
            {
                dryL[(size_t) i] = ul[i];
                dryR[(size_t) i] = ur[i];
                if (splitHz > 0.0f)
                {
                    float lowL, highL, lowR, highR;
                    splitL.process (ul[i], lowL, highL);
                    splitR.process (ur[i], lowR, highR);
                    lowBuffer[(size_t) i] = 0.5f * (lowL + lowR);
                    ul[i] = highL;
                    ur[i] = highR;
                }
                else
                    lowBuffer[(size_t) i] = 0.0f;
            }

            slam.process (ul, ur, un, bark, bite, wet, scHz);
            clipper.process (ul, ur, un, clipDb);

            const float step = 1.0f / (0.02f * osRate);
            for (int i = 0; i < un; ++i)
            {
                splitMix = std::clamp (splitMix + (working ? step : -step), 0.0f, 1.0f);
                const float wetL = ul[i] + lowBuffer[(size_t) i];
                const float wetR = ur[i] + lowBuffer[(size_t) i];
                ul[i] = dryL[(size_t) i] + splitMix * (wetL - dryL[(size_t) i]);
                ur[i] = dryR[(size_t) i] + splitMix * (wetR - dryR[(size_t) i]);
            }

            oversampler->processSamplesDown (block);
        }

        float getSlamMeterDb() const noexcept { return slam.getMeterDb(); }
        float getClipMeterDb() const noexcept { return clipper.meterDb; }

    private:
        std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
        Slam slam;
        Clipper clipper;
        LR4 splitL, splitR;
        std::array<float, 2 * 4096> lowBuffer {}, dryL {}, dryR {};
        float osRate = 96000.0f, lastSplit = -1.0f, splitMix = 0.0f;
    };
}
