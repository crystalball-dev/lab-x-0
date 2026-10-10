#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "CrtLookAndFeel.h"
#include "../Pattern.h"

namespace labx0::ui
{
    // EMISSION: a Veles-style detector sweep over the playing pattern. The sweep is the pattern position, the
    // blips are the hits of every lane, the glow is the EMISSION macro.
    class EmissionGauge final : public juce::Component
    {
    public:
        void setValues (const Pattern& p, double positionQuarters, bool running, float emission, int playing, int queued,
                        int timeSlot, int volumeSlot)
        {
            pat = p;
            const double patternQ = p.stepQuarters() * p.length;
            sweep = running && patternQ > 0.0 ? (float) (std::fmod (std::max (0.0, positionQuarters), patternQ) / patternQ) : -1.0f;
            glow += (std::clamp (emission, 0.0f, 1.0f) - glow) * 0.3f;
            playingPattern = playing;
            queuedPattern = queued;
            isRunning = running;
            activeTime = timeSlot;
            activeVolume = volumeSlot;
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            const auto b = getLocalBounds().toFloat().reduced (2.0f);
            g.setColour (Palette::background);
            g.fillRect (b);
            const float radius = std::min (b.getWidth(), b.getHeight() - 22.0f) * 0.46f;
            const auto centre = juce::Point<float> (b.getCentreX(), b.getY() + 6.0f + radius);

            // Glow of the emission
            juce::ColourGradient halo (Palette::red.withAlpha (0.08f + 0.45f * glow), centre.x, centre.y,
                                       Palette::red.withAlpha (0.0f), centre.x + radius * 1.3f, centre.y, true);
            g.setGradientFill (halo);
            g.fillEllipse (juce::Rectangle<float> (radius * 2.6f, radius * 2.6f).withCentre (centre));

            // Rings and bars
            for (int ring = 1; ring <= 3; ++ring)
            {
                g.setColour (Palette::faint.brighter (0.25f));
                const float r = radius * (float) ring / 3.0f;
                g.drawEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (centre), 1.0f);
            }
            for (int k = 0; k < 4; ++k)
            {
                const float a = juce::MathConstants<float>::halfPi * (float) k;
                g.drawLine ({ centre, centre.getPointOnCircumference (radius, a) }, 0.7f);
            }

            // Hits: pads on the outer ring, breaks in the middle, TIME and VOLUME on the inner ring.
            const int len = std::max (1, (int) pat.length);
            for (int lane = 0; lane < numLanes; ++lane)
            {
                const auto& l = pat.lanes[(size_t) lane];
                const int laneLen = pat.laneLength (lane);
                const float r = lane < laneFirstBreak ? radius * (0.72f + 0.25f * (float) lane / numPads)
                              : (lane < laneTime ? radius * (0.45f + 0.06f * (float) (lane - laneFirstBreak)) : radius * (lane == laneTime ? 0.22f : 0.3f));
                for (int s = 0; s < std::min (laneLen, len); ++s)
                {
                    const auto& st = l.steps[(size_t) s];
                    const bool hit = lane < laneTime ? st.on != 0 : st.slot != 0;
                    if (! hit || l.mute)
                        continue;
                    const float a = juce::MathConstants<float>::twoPi * (float) s / (float) len;
                    const auto p = centre.getPointOnCircumference (r, a);
                    const juce::Colour c = lane < laneFirstBreak ? Palette::phosphor : (lane < laneTime ? Palette::breakTint
                                         : (lane == laneTime ? Palette::timeTint : Palette::volumeTint));
                    float age = 1.0f;
                    if (sweep >= 0.0f)
                    {
                        float d = sweep - (float) s / (float) len;
                        if (d < 0.0f)
                            d += 1.0f;
                        age = d;
                    }
                    g.setColour (c.withAlpha (0.25f + 0.75f * (1.0f - age)));
                    const float dot = lane < laneTime ? 2.4f : 2.0f;
                    g.fillEllipse (p.x - dot, p.y - dot, dot * 2.0f, dot * 2.0f);
                }
            }

            if (sweep >= 0.0f)
            {
                const float a = juce::MathConstants<float>::twoPi * sweep;
                for (int trail = 0; trail < 10; ++trail)
                {
                    const float t = a - 0.05f * (float) trail;
                    g.setColour (Palette::phosphor.withAlpha (0.45f * (1.0f - (float) trail / 10.0f)));
                    g.drawLine ({ centre, centre.getPointOnCircumference (radius, t) }, trail == 0 ? 1.6f : 1.0f);
                }
            }

            g.setFont (monoFont (10.5f, true));
            g.setColour (isRunning ? Palette::bright : Palette::dim);
            juce::String text = "PATTERN " + juce::String (playingPattern + 1).paddedLeft ('0', 2);
            if (queuedPattern >= 0)
                text << "  NEXT " << juce::String (queuedPattern + 1).paddedLeft ('0', 2);
            g.drawText (text, b.withTop (b.getBottom() - 20.0f), juce::Justification::centredLeft);
            juce::String slots;
            if (activeTime > 0)   slots << timeSlots[std::min (activeTime, (int) Slot::numTime - 1)].code;
            if (activeVolume > 0) slots << (slots.isEmpty() ? "" : " ") << volumeSlots[std::min (activeVolume, (int) Slot::numVolume - 1)].code;
            g.setColour (Palette::timeTint);
            g.drawText (slots, b.withTop (b.getBottom() - 20.0f), juce::Justification::centredRight);
            g.setColour (Palette::border);
            g.drawRect (b, 1.0f);
        }

    private:
        Pattern pat;
        float sweep = -1.0f, glow = 0.0f;
        int playingPattern = 0, queuedPattern = -1, activeTime = 0, activeVolume = 0;
        bool isRunning = false;
    };

    // DOSIMETER: output level and how hard each punch stage works.
    class Dosimeter final : public juce::Component
    {
    public:
        struct Reading
        {
            float outDb = -100.0f, peakDb = -100.0f, transientDb = 0.0f, slamDb = 0.0f, clipDb = 0.0f, limitDb = 0.0f;
            float band[3] {};
        };

        void setValues (const Reading& r)
        {
            auto ease = [] (float& v, float t) { v = t > v ? t : v + (t - v) * 0.15f; };
            ease (shown.outDb, r.outDb);
            ease (shown.peakDb, r.peakDb);
            ease (shown.transientDb, r.transientDb);
            ease (shown.slamDb, r.slamDb);
            ease (shown.clipDb, r.clipDb);
            ease (shown.limitDb, r.limitDb);
            for (int b = 0; b < 3; ++b)
                ease (shown.band[b], r.band[b]);
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            const auto area = getLocalBounds().toFloat().reduced (2.0f);
            g.setColour (Palette::background);
            g.fillRect (area);
            struct Row { const char* name; float value; float range; bool level; };
            const Row rowsList[] = {
                { "OUT",   shown.outDb,       48.0f, true },
                { "TRANS", shown.transientDb, 18.0f, false },
                { "LOW",   shown.band[0],     24.0f, false },
                { "MID",   shown.band[1],     24.0f, false },
                { "HIGH",  shown.band[2],     24.0f, false },
                { "BITE",  shown.slamDb,      24.0f, false },
                { "CLIP",  shown.clipDb,      12.0f, false },
                { "LIMIT", shown.limitDb,     12.0f, false },
            };
            const int count = (int) std::size (rowsList);
            const float h = area.getHeight() / (float) count;
            g.setFont (monoFont (std::min (10.0f, h - 1.0f)));
            for (int i = 0; i < count; ++i)
            {
                const auto& row = rowsList[i];
                auto r = juce::Rectangle<float> (area.getX(), area.getY() + h * (float) i, area.getWidth(), h).reduced (1.0f, 1.0f);
                g.setColour (Palette::dim);
                g.drawText (row.name, r.removeFromLeft (40.0f), juce::Justification::centredLeft);
                const auto valueBox = r.removeFromRight (44.0f);
                const float frac = row.level ? std::clamp ((row.value + row.range) / row.range, 0.0f, 1.0f)
                                             : std::clamp (row.value / row.range, 0.0f, 1.0f);
                g.setColour (Palette::faint);
                g.fillRect (r);
                g.setColour (row.level && shown.peakDb > -1.0f ? Palette::red : Palette::phosphor.withAlpha (0.8f));
                g.fillRect (r.withWidth (r.getWidth() * frac));
                g.setColour (Palette::phosphor);
                g.drawText (row.level ? juce::String (row.value, 1) : juce::String (row.value, 1), valueBox, juce::Justification::centredRight);
            }
            g.setColour (Palette::border);
            g.drawRect (area, 1.0f);
        }

    private:
        Reading shown;
    };
}
