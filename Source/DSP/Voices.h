#pragma once

#include <cstdint>
#include "Common.h"
#include "Filters.h"
#include "SampleData.h"

namespace labx0::dsp
{
    // Control-rate settings for one pad or break slot, shared by all of its voices.
    struct ToneSettings
    {
        bool lowpass = false, highpass = false;
        float g = 0.0f, k = 1.4142f;   // TPT SVF coefficients

        static ToneSettings make (float tone, float reso, float sampleRate) noexcept
        {
            ToneSettings t;
            t.k = 2.0f - 1.85f * std::clamp (reso, 0.0f, 1.0f);   // 0.5 .. 6.7 Q
            if (tone < -0.01f)
            {
                t.lowpass = true;
                t.g = SVF::gFor (20000.0f * std::exp2 (8.0f * tone), sampleRate);
            }
            else if (tone > 0.01f)
            {
                t.highpass = true;
                t.g = SVF::gFor (20.0f * std::exp2 (8.6f * tone), sampleRate);
            }
            return t;
        }
    };

    struct PadSettings
    {
        float gainL = 1.0f, gainR = 1.0f;   // level x pan
        float pitchRatio = 1.0f;            // tune
        ToneSettings tone;
        float drive = 1.0f, driveComp = 1.0f;
        bool driving = false;
    };

    struct VoiceOutput
    {
        float* l[3];   // FULL, PUNCH, DRY buses
        float* r[3];
    };

    // Short memory of a voice cut off when its sample disappeared under it: the last output value fades out
    // over a few milliseconds instead of stepping to zero.
    struct ClickTail
    {
        float l = 0.0f, r = 0.0f;
        bool active() const noexcept { return std::abs (l) + std::abs (r) > 1.0e-5f; }
        void add (float* outL, float* outR, int n, float coef) noexcept
        {
            for (int i = 0; i < n; ++i)
            {
                outL[i] += l;
                outR[i] += r;
                l *= coef;
                r *= coef;
            }
        }
    };

    //==============================================================================
    class PadVoice
    {
    public:
        struct Start
        {
            int pad = 0, route = 0;
            float velocityGain = 1.0f;
            float stepPitch = 0.0f;       // semitones added at the trigger
            float startNorm = 0.0f;       // 0..1 into the sample
            bool reverse = false;
            float attackSeconds = 0.0f, holdSeconds = 0.0f, decaySeconds = 10.0f;
            bool fullLength = true;
            int gateSamples = -1;         // Gate mode: release after this many samples (-1: until noteOff)
            bool gated = false;
            uint64_t stamp = 0;
        };

        void prepare (float sr) noexcept
        {
            hostRate = sr;
            fadeStep = 1.0f / std::max (1.0f, 0.003f * sr);
            releaseCoef = std::exp (std::log (0.001f) / (0.02f * sr));
            tailCoef = std::exp (std::log (0.001f) / (0.004f * sr));
            kill();
        }

        bool isActive() const noexcept   { return active || tail.active(); }
        bool isSounding() const noexcept { return active && ! fading; }
        int getPad() const noexcept      { return pad; }
        int getRoute() const noexcept    { return route; }
        uint64_t getStamp() const noexcept { return stamp; }
        float getLevel() const noexcept  { return env * fade; }

        void kill() noexcept
        {
            active = false;
            fading = false;
            tail = {};
        }

        // Fast fade (choke, mono retrigger, stealing).
        void fadeOut() noexcept
        {
            if (active)
                fading = true;
        }

        void noteOff() noexcept
        {
            if (active && gated)
                releasing = true;
        }

        void start (const SampleData* d, const Start& s) noexcept
        {
            if (d == nullptr || d->length < 4)
                return;
            active = true;
            fading = releasing = false;
            fade = 1.0f;
            pad = s.pad;
            route = s.route;
            stamp = s.stamp;
            dataId = d->id;
            reverse = s.reverse;
            velocityGain = s.velocityGain;
            stepRatio = semisToRatio (s.stepPitch);
            gated = s.gated;
            gateLeft = s.gateSamples;

            const double span = (double) d->length - 2.0;
            const double offset = (double) std::clamp (s.startNorm, 0.0f, 0.999f) * span;
            pos = reverse ? span - offset : offset;
            startRamp = (s.startNorm > 0.0005f || reverse) ? 0.0f : 1.0f;   // mid-sample starts need a short fade
            startRampStep = 1.0f / std::max (1.0f, 0.001f * hostRate);

            attackLeft = (int) (s.attackSeconds * hostRate);
            attackStep = attackLeft > 0 ? 1.0f / (float) attackLeft : 1.0f;
            env = attackLeft > 0 ? 0.0f : 1.0f;
            holdLeft = (int) (s.holdSeconds * hostRate);
            fullLength = s.fullLength;
            decayCoef = fullLength ? 1.0f : std::exp (std::log (0.001f) / (std::max (0.005f, s.decaySeconds) * hostRate));
            filterL.reset();
            filterR.reset();
            tail = {};
        }

        // d: the pad's sample for this block. A different id means the sample was replaced: stop.
        void render (const SampleData* d, const PadSettings& p, const VoiceOutput& out, int n) noexcept
        {
            float* outL = out.l[route];
            float* outR = out.r[route];

            if (! active)
            {
                if (tail.active())
                    tail.add (outL, outR, n, tailCoef);
                return;
            }
            if (d == nullptr || d->id != dataId)
            {
                active = false;
                tail.add (outL, outR, n, tailCoef);
                return;
            }

            const float* L = d->l();
            const float* R = d->r();
            const double inc = (double) (p.pitchRatio * stepRatio) * d->sampleRate / (double) hostRate * (reverse ? -1.0 : 1.0);
            const double last = (double) d->length - 2.0;
            const double endFade = 64.0;

            for (int i = 0; i < n; ++i)
            {
                // Envelope: attack, hold, decay; gate release; fade out.
                if (attackLeft > 0)
                {
                    env = std::min (1.0f, env + attackStep);
                    if (--attackLeft == 0)
                        env = 1.0f;
                }
                else if (holdLeft > 0)
                    --holdLeft;
                else if (! fullLength)
                    env *= decayCoef;

                if (gated && gateLeft >= 0 && --gateLeft < 0)
                    releasing = true;
                if (releasing)
                    env *= releaseCoef;
                if (fading)
                {
                    fade -= fadeStep;
                    if (fade <= 0.0f)
                    {
                        active = false;
                        break;
                    }
                }
                if (env < 1.0e-4f && (attackLeft == 0) && (! fullLength || releasing))
                {
                    active = false;
                    break;
                }

                float a = hermite (L, pos);
                float b = hermite (R, pos);

                // Fade the last 64 source samples so a sample that ends mid-wave does not click.
                const double remaining = reverse ? pos : last - pos;
                float edge = remaining < endFade ? (float) std::max (0.0, remaining / endFade) : 1.0f;
                if (startRamp < 1.0f)
                {
                    edge *= startRamp;
                    startRamp = std::min (1.0f, startRamp + startRampStep);
                }

                if (p.tone.lowpass || p.tone.highpass)
                {
                    float lp, bp, hp;
                    filterL.process (a, lp, bp, hp);
                    a = p.tone.lowpass ? lp : hp;
                    filterR.process (b, lp, bp, hp);
                    b = p.tone.lowpass ? lp : hp;
                }
                if (p.driving)
                {
                    a = fastTanh (a * p.drive) * p.driveComp;
                    b = fastTanh (b * p.drive) * p.driveComp;
                }

                const float g = env * fade * edge * velocityGain;
                tail.l = a * g * p.gainL;
                tail.r = b * g * p.gainR;
                outL[i] += tail.l;
                outR[i] += tail.r;

                pos += inc;
                if (pos >= last || pos < 0.0)
                {
                    active = false;
                    break;
                }
            }

            if (! active)
                tail = {};   // ended on its own: faded already
        }

        void updateFilter (const ToneSettings& t) noexcept
        {
            if (t.lowpass || t.highpass)
            {
                filterL.set (t.g, t.k);
                filterR.set (t.g, t.k);
            }
        }

    private:
        SVF filterL, filterR;
        ClickTail tail;
        double pos = 0.0;
        uint64_t dataId = 0, stamp = 0;
        float hostRate = 48000.0f, env = 0.0f, attackStep = 1.0f, decayCoef = 1.0f, releaseCoef = 0.99f, tailCoef = 0.99f;
        float fade = 1.0f, fadeStep = 0.01f, velocityGain = 1.0f, stepRatio = 1.0f, startRamp = 1.0f, startRampStep = 0.02f;
        int pad = 0, route = 0, attackLeft = 0, holdLeft = 0, gateLeft = -1;
        bool active = false, fading = false, releasing = false, reverse = false, fullLength = true, gated = false;
    };

    //==============================================================================
    struct BreakSettings
    {
        float gainL = 1.0f, gainR = 1.0f;
        int fit = 0;                 // 0 repitch, 1 slice, 2 stretch
        double rate = 1.0;           // source samples per host sample (repitch, slice)
        double advance = 1.0;        // stretch: timeline speed in source samples per host sample
        double grainRate = 1.0;      // stretch: pitch of the grains
        ToneSettings tone;
    };

    class BreakVoice
    {
    public:
        struct Start
        {
            double sourcePos = 0.0;
            double stopAt = -1.0;          // Slice gate: stop at the end of the slice
            float velocityGain = 1.0f;
            float stepPitchRatio = 1.0f;
            float decaySeconds = 10.0f;
            bool fullLength = true;
            int route = 0;
            int slice = -1;                // slice index started (for "same slice" checks)
        };

        void prepare (float sr) noexcept
        {
            hostRate = sr;
            fadeStep = 1.0f / std::max (1.0f, 0.003f * sr);
            rampStep = 1.0f / std::max (1.0f, 0.0003f * sr);
            grainLength = std::max (64, (int) (0.042f * sr));
            grainHop = grainLength / 2;
            releaseCoef = std::exp (std::log (0.001f) / (0.03f * sr));
            tailCoef = std::exp (std::log (0.001f) / (0.004f * sr));
            kill();
        }

        bool isActive() const noexcept   { return active || tail.active(); }
        bool isSounding() const noexcept { return active && ! fading; }
        double getPosition() const noexcept { return pos; }
        int getSlice() const noexcept     { return slice; }
        int getRoute() const noexcept     { return route; }
        uint64_t getDataId() const noexcept { return dataId; }

        void kill() noexcept { active = false; fading = false; tail = {}; }
        void fadeOut() noexcept { if (active) fading = true; }
        void release() noexcept { if (active) releasing = true; }

        void start (const SampleData* d, const Start& s) noexcept
        {
            if (d == nullptr || d->length < 8)
                return;
            active = true;
            fading = releasing = false;
            fade = 1.0f;
            ramp = 0.0f;
            dataId = d->id;
            pos = std::clamp (s.sourcePos, 0.0, (double) d->length - 3.0);
            stopAt = s.stopAt;
            velocityGain = s.velocityGain;
            stepRatio = s.stepPitchRatio;
            route = s.route;
            slice = s.slice;
            env = 1.0f;
            fullLength = s.fullLength;
            decayCoef = fullLength ? 1.0f : std::exp (std::log (0.001f) / (std::max (0.005f, s.decaySeconds) * hostRate));
            filterL.reset();
            filterR.reset();
            tail = {};

            // Stretch: the first grain after a trigger starts at full level, so the attack is kept.
            timeline = pos;
            grainPhase = 0;
            nextGrain = 0;
            firstGrain = true;
            for (auto& g : grains)
                g.active = false;
        }

        void render (const SampleData* d, const BreakSettings& p, const VoiceOutput& out, int n) noexcept
        {
            float* outL = out.l[route];
            float* outR = out.r[route];
            if (! active)
            {
                if (tail.active())
                    tail.add (outL, outR, n, tailCoef);
                return;
            }
            if (d == nullptr || d->id != dataId)
            {
                active = false;
                tail.add (outL, outR, n, tailCoef);
                return;
            }

            const float* L = d->l();
            const float* R = d->r();
            const double last = (double) d->length - 3.0;

            for (int i = 0; i < n; ++i)
            {
                if (! fullLength)
                    env *= decayCoef;
                if (releasing)
                    env *= releaseCoef;
                if (fading || (stopAt >= 0.0 && pos >= stopAt))
                {
                    fade -= fadeStep;
                    if (fade <= 0.0f)
                    {
                        active = false;
                        break;
                    }
                }
                if (env < 1.0e-4f)
                {
                    active = false;
                    break;
                }

                float a = 0.0f, b = 0.0f;
                if (p.fit == 2)
                {
                    // STRETCH: two Hann grains half a grain apart follow the timeline at the original pitch. Each new
                    // grain starts where the source best continues the fading one (WSOLA), so tones keep their pitch.
                    if (grainPhase == 0)
                    {
                        auto& g = grains[(size_t) nextGrain];
                        const auto& fadingGrain = grains[(size_t) (nextGrain ^ 1)];
                        g.active = true;
                        g.src = (firstGrain || ! fadingGrain.active) ? timeline
                                                                      : alignGrain (*d, timeline, fadingGrain.src, p.grainRate * stepRatio);
                        g.age = 0;
                        g.first = firstGrain;
                        firstGrain = false;
                        nextGrain ^= 1;
                    }
                    for (auto& g : grains)
                    {
                        if (! g.active)
                            continue;
                        float w;
                        if (g.first && g.age < grainHop)
                            w = 1.0f;
                        else
                        {
                            const float s = sin2pi (0.5f * (float) g.age / (float) grainLength);
                            w = s * s;
                        }
                        if (g.src >= 0.0 && g.src < last)
                        {
                            a += hermite (L, g.src) * w;
                            b += hermite (R, g.src) * w;
                        }
                        g.src += p.grainRate * stepRatio;
                        if (++g.age >= grainLength)
                            g.active = false;
                    }
                    timeline += p.advance;
                    pos = timeline;
                    if (++grainPhase >= grainHop)
                        grainPhase = 0;
                }
                else
                {
                    a = hermite (L, pos);
                    b = hermite (R, pos);
                    pos += p.rate * stepRatio;
                }

                if (ramp < 1.0f)
                {
                    a *= ramp;
                    b *= ramp;
                    ramp = std::min (1.0f, ramp + rampStep);
                }

                if (p.tone.lowpass || p.tone.highpass)
                {
                    float lp, bp, hp;
                    filterL.process (a, lp, bp, hp);
                    a = p.tone.lowpass ? lp : hp;
                    filterR.process (b, lp, bp, hp);
                    b = p.tone.lowpass ? lp : hp;
                }

                const float g = env * fade * velocityGain;
                tail.l = a * g * p.gainL;
                tail.r = b * g * p.gainR;
                outL[i] += tail.l;
                outR[i] += tail.r;

                if (pos >= last || pos < 0.0)
                {
                    // End of the file: fade over the tail memory.
                    active = false;
                    tail.add (outL + i + 1, outR + i + 1, n - i - 1, tailCoef);
                    return;
                }
            }

            if (! active)
                tail = {};
        }

        void updateFilter (const ToneSettings& t) noexcept
        {
            if (t.lowpass || t.highpass)
            {
                filterL.set (t.g, t.k);
                filterR.set (t.g, t.k);
            }
        }

    private:
        struct Grain
        {
            double src = 0.0;
            int age = 0;
            bool active = false, first = false;
        };

        // WSOLA: the start within +-6 ms of the timeline whose next 10 ms look most like what the fading grain plays
        // next (normalised cross-correlation of L+R, every 4th sample, candidates 2 samples apart, then refined).
        // Keeps the fading grain's fractional position, so the two grains stay in phase.
        static double alignGrain (const SampleData& d, double target, double continuation, double rate) noexcept
        {
            const float* L = d.l();
            const float* R = d.r();
            const int radius = std::max (8, (int) (0.006 * d.sampleRate));
            const int window = std::max (32, (int) (0.010 * d.sampleRate));
            const int reach = (int) std::ceil ((double) window * rate) + 2;
            const int c0 = (int) continuation;
            if (c0 < 0 || c0 + reach >= d.length)
                return target;

            auto score = [&] (int j)
            {
                const int s0 = c0 + j;
                if (s0 < 0 || s0 + reach >= d.length)
                    return -1.0e30;
                double xy = 0.0, yy = 1.0e-12;
                for (int k = 0; k < window; k += 4)
                {
                    const int step = (int) ((double) k * rate);
                    const float x = L[c0 + step] + R[c0 + step];
                    const float y = L[s0 + step] + R[s0 + step];
                    xy += (double) x * y;
                    yy += (double) y * y;
                }
                return xy / std::sqrt (yy);
            };

            const int lo = (int) std::floor (target - continuation) - radius;
            const int hi = (int) std::ceil (target - continuation) + radius;
            int best = (int) std::lround (target - continuation);
            double bestScore = score (best);
            for (int j = lo; j <= hi; j += 2)
                if (const double s = score (j); s > bestScore)
                {
                    bestScore = s;
                    best = j;
                }
            for (const int j : { best - 1, best + 1 })
                if (const double s = score (j); s > bestScore)
                {
                    bestScore = s;
                    best = j;
                }
            return continuation + (double) best;
        }

        SVF filterL, filterR;
        ClickTail tail;
        std::array<Grain, 2> grains {};
        double pos = 0.0, stopAt = -1.0, timeline = 0.0;
        uint64_t dataId = 0;
        float hostRate = 48000.0f, env = 1.0f, decayCoef = 1.0f, releaseCoef = 0.99f, tailCoef = 0.99f;
        float fade = 1.0f, fadeStep = 0.01f, ramp = 1.0f, rampStep = 0.1f, velocityGain = 1.0f, stepRatio = 1.0f;
        int route = 0, slice = -1, grainLength = 2016, grainHop = 1008, grainPhase = 0, nextGrain = 0;
        bool active = false, fading = false, releasing = false, fullLength = true, firstGrain = true;
    };

    //==============================================================================
    // The browser's audition: one sample, once, at its own pitch.
    class PreviewVoice
    {
    public:
        void prepare (float sr) noexcept { hostRate = sr; active = false; }
        void stop() noexcept             { active = false; }
        bool isActive() const noexcept   { return active; }

        void start (const SampleData* d) noexcept
        {
            active = d != nullptr && d->length > 4;
            if (active)
            {
                dataId = d->id;
                pos = 1.0;
            }
        }

        uint64_t getDataId() const noexcept { return dataId; }

        void render (const SampleData* d, float* outL, float* outR, int n, float gain) noexcept
        {
            if (! active)
                return;
            if (d == nullptr || d->id != dataId)
            {
                active = false;
                return;
            }
            const double inc = d->sampleRate / (double) hostRate;
            const double last = (double) d->length - 3.0;
            for (int i = 0; i < n; ++i)
            {
                const double remaining = last - pos;
                const float edge = remaining < 64.0 ? (float) std::max (0.0, remaining / 64.0) : 1.0f;
                outL[i] += hermite (d->l(), pos) * gain * edge;
                outR[i] += hermite (d->r(), pos) * gain * edge;
                pos += inc;
                if (pos >= last)
                {
                    active = false;
                    return;
                }
            }
        }

    private:
        double pos = 0.0;
        uint64_t dataId = 0;
        float hostRate = 48000.0f;
        bool active = false;
    };
}
