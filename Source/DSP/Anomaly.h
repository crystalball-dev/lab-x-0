#pragma once

#include <vector>
#include "Common.h"
#include "Sequencer.h"

namespace labx0::dsp
{
    // Stereo circular buffer read at a delay measured back from the newest sample. The delay is split into whole
    // and fractional samples before any wrap-around, so both indices are integers in range by construction.
    // (LAB X-3 and LAB X-19 once wrapped a float position, which rounded to the buffer length and read one sample
    // past the end.) One slot past the end holds a sentinel that no correct read touches.
    struct HistoryBuffer
    {
        static constexpr float sentinel = 1.0e6f;
        std::vector<float> left, right;
        int size = 0, write = 0;

        void prepare (int samples)
        {
            size = std::max (16, samples);
            left.assign ((size_t) size + 1, 0.0f);
            right.assign ((size_t) size + 1, 0.0f);
            left[(size_t) size] = right[(size_t) size] = sentinel;
            write = 0;
        }

        void clear() noexcept
        {
            std::fill (left.begin(), left.begin() + size, 0.0f);
            std::fill (right.begin(), right.begin() + size, 0.0f);
            write = 0;
        }

        void push (float l, float r) noexcept
        {
            left[(size_t) write] = l;
            right[(size_t) write] = r;
            if (++write >= size)
                write = 0;
        }

        int maxDelay() const noexcept { return size - 4; }

        int indexAt (int delay) const noexcept   // delay 0 = the newest sample
        {
            int i = write - 1 - delay;
            if (i < 0)
                i += size;
            return i;
        }

        // Hermite between delays whole and whole + 1 (toward the past).
        void read (double delay, float& outL, float& outR) const noexcept
        {
            delay = std::clamp (delay, 0.0, (double) maxDelay());
            const int whole = (int) delay;
            const float f = (float) (delay - (double) whole);
            const int i1 = indexAt (whole);
            const int i0 = whole >= 1 ? indexAt (whole - 1) : i1;
            const int i2 = indexAt (whole + 1);
            const int i3 = indexAt (whole + 2);
            outL = interp (left[(size_t) i0], left[(size_t) i1], left[(size_t) i2], left[(size_t) i3], f);
            outR = interp (right[(size_t) i0], right[(size_t) i1], right[(size_t) i2], right[(size_t) i3], f);
        }

        static float interp (float x0, float x1, float x2, float x3, float f) noexcept
        {
            const float c1 = 0.5f * (x2 - x0);
            const float c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
            const float c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
            return ((c3 * f + c2) * f + c1) * f + x1;
        }
    };

    //==============================================================================
    // ANOMALY: the TIME and VOLUME lanes, Gross Beat style, on the FULL bus. A region (a run of identical slots)
    // takes its anchor when it begins; repeats play the first pass live and then replay it, so every read looks
    // into the past only. Every jump of the read position crossfades over 2.5 ms between two read heads.
    class Anomaly
    {
    public:
        struct Context
        {
            const Pattern* pattern = nullptr;
            const StepSequencer* sequencer = nullptr;
            bool running = false;
            double q0 = 0.0, qps = 0.0;   // quarter position at the block start, quarters per sample
            float mix = 1.0f, scratchDepth = 0.6f;
        };

        void prepare (double sr)
        {
            sampleRate = sr;
            history.prepare ((int) std::ceil (16.0 * sr) + 8);
            xfadeLength = std::max (8, (int) (0.0025 * sr));
            volumeSlew = 1.0f - std::exp (-1.0f / (0.0007f * (float) sr));
            reset();
        }

        void reset() noexcept
        {
            history.clear();
            now = 0;
            timeRegion = volumeRegion = Region();
            timeAnchor = volumeAnchor = 0.0;
            nextBoundaryQ = -1.0e18;
            heads[0] = heads[1] = Head();
            current = 0;
            xfade = 1.0f;
            volumeGain = 1.0f;
            lastTarget = 0.0;
            haveTarget = false;
            activeTime = activeVolume = 0;
        }

        int getActiveTimeSlot() const noexcept   { return activeTime; }
        int getActiveVolumeSlot() const noexcept { return activeVolume; }

        void process (float* l, float* r, int n, const Context& c) noexcept
        {
            const bool lanes = c.running && c.pattern != nullptr && c.sequencer != nullptr && c.qps > 0.0;
            const double spq = lanes ? 1.0 / c.qps : sampleRate;   // samples per quarter
            const float mix = std::clamp (c.mix, 0.0f, 1.0f);

            for (int i = 0; i < n; ++i)
            {
                history.push (l[i], r[i]);
                ++now;
                const double t = (double) now - 1.0;   // absolute time of the newest sample

                // Regions change only on step boundaries.
                if (lanes)
                {
                    const double q = c.q0 + (double) i * c.qps;
                    if (q >= nextBoundaryQ || q < nextBoundaryQ - 2.0 * c.pattern->stepQuarters())
                        updateRegions (c, q, t);
                }
                else if (timeRegion.slot != 0 || volumeRegion.slot != 0)
                {
                    timeRegion = volumeRegion = Region();
                    nextBoundaryQ = -1.0e18;
                }

                // TIME: where in the past to read.
                double target = t;
                float timeGain = 1.0f;
                if (timeRegion.slot != 0)
                {
                    const double e = t - timeAnchor;
                    const double regionLength = std::max (1.0, (timeRegion.endQ - timeRegion.startQ) * spq);
                    double p = e;
                    sourceFor (timeRegion.slot, e, spq, regionLength, c.scratchDepth, timeRegion.id, p, timeGain);
                    target = timeAnchor + p;
                    if (t - target > (double) history.maxDelay() - 64.0)
                        target = t;   // the anchor has left the buffer: back to live
                }

                // Two heads: a jump hands the old head to the fade-out while the new one fades in.
                auto& head = heads[(size_t) current];
                if (! haveTarget)
                {
                    head = { target, 1.0 };
                    haveTarget = true;
                }
                else if (std::abs (target - (head.src + head.speed)) > 1.5)
                {
                    heads[(size_t) (1 - current)] = head;
                    current = 1 - current;
                    heads[(size_t) current] = { target, heads[(size_t) (1 - current)].speed };
                    xfade = 0.0f;
                }
                else
                {
                    auto& h = heads[(size_t) current];
                    h.speed = target - h.src;
                    h.src = target;
                }

                float wl, wr;
                {
                    const auto& h = heads[(size_t) current];
                    history.read (t - h.src, wl, wr);
                }
                if (xfade < 1.0f)
                {
                    auto& old = heads[(size_t) (1 - current)];
                    old.src = std::min (old.src + old.speed, t);
                    float ol, orr;
                    history.read (t - old.src, ol, orr);
                    xfade = std::min (1.0f, xfade + 1.0f / (float) xfadeLength);
                    const float in = xfade, out = 1.0f - xfade;
                    wl = wl * in + ol * out;
                    wr = wr * in + orr * out;
                }

                // VOLUME
                float volumeTarget = 1.0f;
                if (volumeRegion.slot != 0)
                {
                    const double e = t - volumeAnchor;
                    const double regionLength = std::max (1.0, (volumeRegion.endQ - volumeRegion.startQ) * spq);
                    const double stepLength = lanes ? c.pattern->stepQuarters() * spq : regionLength;
                    volumeTarget = volumeFor (volumeRegion.slot, e, spq, regionLength, stepLength);
                }
                volumeGain += (volumeTarget - volumeGain) * volumeSlew;

                const float g = timeGain * volumeGain;
                l[i] = l[i] * (1.0f - mix) + wl * g * mix;
                r[i] = r[i] * (1.0f - mix) + wr * g * mix;
            }

            activeTime = timeRegion.slot;
            activeVolume = volumeRegion.slot;
        }

        const HistoryBuffer& getHistory() const noexcept { return history; }

    private:
        struct Head
        {
            double src = 0.0, speed = 1.0;
        };

        void updateRegions (const Context& c, double q, double t) noexcept
        {
            const auto& pat = *c.pattern;
            const double stepQ = pat.stepQuarters();
            const auto start = c.sequencer->getPatternStartQ();
            nextBoundaryQ = start + (std::floor ((q - start) / stepQ + 1.0e-9) + 1.0) * stepQ;

            auto take = [&] (int lane, Region& region, double& anchor)
            {
                const auto next = c.sequencer->regionAt (pat, lane, q);
                if (next.slot == region.slot && next.id == region.id)
                    return;
                region = next;
                if (region.slot == 0)
                    return;
                // A region entered at its start anchors there exactly; one entered late (a host jump, a pattern
                // switch) anchors where it was entered.
                const double late = (q - region.startQ) / c.qps;
                anchor = late <= 2.0 ? std::round (t - std::max (0.0, late)) : t;
            };
            take (laneTime, timeRegion, timeAnchor);
            take (laneVolume, volumeRegion, volumeAnchor);
        }

        // Source position p (relative to the anchor, never later than e) for each TIME slot.
        static void sourceFor (int slot, double e, double spq, double regionLength, float depth, int64_t id,
                               double& p, float& gain) noexcept
        {
            const double l4 = spq, l8 = 0.5 * spq, l16 = 0.25 * spq, l32 = 0.125 * spq, l64 = 0.0625 * spq;
            gain = 1.0f;
            switch (slot)
            {
                case Slot::spring4:  p = std::fmod (e, l4); break;
                case Slot::spring8:  p = std::fmod (e, l8); break;
                case Slot::spring16: p = std::fmod (e, l16); break;
                case Slot::spring32: p = std::fmod (e, l32); break;
                case Slot::spring64: p = std::fmod (e, l64); break;

                case Slot::whirl8:
                case Slot::whirl16:
                {
                    // Baby scratch: back and forth behind the anchor, peak speed up to twice normal.
                    const double period = slot == Slot::whirl8 ? l8 : l16;
                    const double amplitude = (0.15 + 0.85 * (double) std::clamp (depth, 0.0f, 1.0f)) * 2.0 * period / 3.14159265358979;
                    p = -0.5 * amplitude * (1.0 - std::cos (6.28318530717959 * e / period));
                    break;
                }

                case Slot::vortex:
                    p = -std::fmod (e, std::max (l4, std::min (regionLength, 4.0 * l4)));
                    break;

                case Slot::comet:
                {
                    // Tape stop: the speed falls from 1 to 0 across the region, then a 5 ms fade.
                    const double d = regionLength;
                    const double x = std::min (e, d);
                    p = x - x * x / (2.0 * d);
                    const double fadeStart = std::max (0.0, d - 0.005 * spq * 2.0);
                    if (e > fadeStart)
                        gain = (float) std::max (0.0, 1.0 - (e - fadeStart) / std::max (1.0, d - fadeStart));
                    break;
                }

                case Slot::jelly:
                    p = 0.5 * e;
                    break;

                case Slot::burner:
                {
                    // The first 1/8 live, then repeats of its start at 1/16, 1/32, then 1/64 for good.
                    if (e < l8)
                        p = e;
                    else
                    {
                        double x = e - l8;
                        if (x < l16)            p = x;
                        else if ((x -= l16) < l32) p = x;
                        else                    p = std::fmod (x - l32, l64);
                    }
                    break;
                }

                case Slot::bubble:
                {
                    // Random stutter over 1/32 cells: live, repeat, 1/64 stutter or reverse of the previous cell.
                    const double cell = l32;
                    const auto k = (int64_t) std::floor (e / cell);
                    const double within = e - (double) k * cell;
                    const float roll = StepSequencer::chance (id * 131 + k, 77, 0);
                    if (k < 1 || roll < 0.40f)      p = e;
                    else if (roll < 0.65f)          p = (double) (k - 1) * cell + within;
                    else if (roll < 0.90f)          p = (double) (k - 1) * cell + std::fmod (within, 0.5 * cell);
                    else                            p = (double) k * cell - within;
                    break;
                }

                default:
                    p = e;
                    break;
            }
            p = std::min (p, e);
        }

        static float volumeFor (int slot, double e, double spq, double regionLength, double stepLength) noexcept
        {
            auto gate = [e] (double cycle) { return std::fmod (e, cycle) < 0.5 * cycle ? 1.0f : 0.0f; };
            switch (slot)
            {
                case Slot::gate8:   return gate (0.5 * spq);
                case Slot::gate16:  return gate (0.25 * spq);
                case Slot::gate32:  return gate (0.125 * spq);
                case Slot::fadeIn:  { const float x = (float) std::clamp (e / regionLength, 0.0, 1.0); return x * x; }
                case Slot::fadeOut: { const float x = (float) std::clamp (1.0 - e / regionLength, 0.0, 1.0); return x * x; }
                case Slot::duck:
                {
                    const float x = (float) std::clamp (std::fmod (e, stepLength) / (0.7 * stepLength), 0.0, 1.0);
                    return x * x * (3.0f - 2.0f * x);
                }
                case Slot::trem16:  return 0.5f + 0.5f * std::cos (6.2831853f * (float) (e / (0.25 * spq)));
                case Slot::mute:    return 0.0f;
                default:            return 1.0f;
            }
        }

        HistoryBuffer history;
        double sampleRate = 48000.0;
        int64_t now = 0;
        Region timeRegion, volumeRegion;
        double timeAnchor = 0.0, volumeAnchor = 0.0, nextBoundaryQ = -1.0e18, lastTarget = 0.0;
        std::array<Head, 2> heads {};
        int current = 0, xfadeLength = 120, activeTime = 0, activeVolume = 0;
        float xfade = 1.0f, volumeGain = 1.0f, volumeSlew = 0.03f;
        bool haveTarget = false;
    };
}
