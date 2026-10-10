#include "PatternGrid.h"

namespace labx0::ui
{
    namespace
    {
        juce::Colour laneColour (int lane)
        {
            if (lane < laneFirstBreak) return Palette::phosphor;
            if (lane < laneTime)       return Palette::breakTint;
            return lane == laneTime ? Palette::timeTint : Palette::volumeTint;
        }

        bool isEffectLane (int lane) noexcept { return lane >= laneTime; }

        int stepsPerBeat (const Pattern& p)
        {
            return std::max (1, (int) std::lround (1.0 / p.stepQuarters()));
        }

        const SlotInfo& slotInfo (int lane, int slot)
        {
            return lane == laneTime ? timeSlots[std::clamp (slot, 0, (int) Slot::numTime - 1)]
                                    : volumeSlots[std::clamp (slot, 0, (int) Slot::numVolume - 1)];
        }

        int slotCount (int lane) { return lane == laneTime ? (int) Slot::numTime : (int) Slot::numVolume; }

        bool isAudioFile (const juce::String& path)
        {
            return juce::File (path).hasFileExtension ("wav;aif;aiff;flac;ogg;mp3");
        }
    }

    //==============================================================================
    PatternGrid::PatternGrid (EditorContext& context) : ctx (context)
    {
        playStep.fill (-1);
        for (int lane = 0; lane < numLanes; ++lane)
            names.add ({});
        ctx.addChangeListener (this);
        setRepaintsOnMouseActivity (false);
        refresh();
    }

    PatternGrid::~PatternGrid()
    {
        ctx.removeChangeListener (this);
    }

    int PatternGrid::visibleSteps() const noexcept
    {
        int longest = pat.length;
        for (int lane = 0; lane < numLanes; ++lane)
            longest = std::max (longest, pat.laneLength (lane));
        return longest > 32 ? 32 : std::max (1, longest);
    }

    int PatternGrid::laneAt (int y) const noexcept
    {
        if (y < topRow)
            return -1;
        return juce::jlimit (0, numLanes - 1, (int) ((float) (y - topRow) / rowHeight()));
    }

    int PatternGrid::stepAt (int x) const noexcept
    {
        if (x < headerWidth)
            return -1;
        const float w = (float) (getWidth() - headerWidth) / (float) visibleSteps();
        const int s = (int) ((float) (x - headerWidth) / w);
        return s >= 0 && s < visibleSteps() ? firstStep() + s : -1;
    }

    juce::Rectangle<float> PatternGrid::cellBounds (int lane, int step) const noexcept
    {
        const float w = (float) (getWidth() - headerWidth) / (float) visibleSteps();
        const float h = rowHeight();
        return { (float) headerWidth + w * (float) (step - firstStep()), (float) topRow + h * (float) lane, w, h };
    }

    juce::String PatternGrid::laneLabel (int lane) const
    {
        if (lane < numPads)
            return juce::String (lane + 1).paddedLeft ('0', 2) + " " + (names[lane].isNotEmpty() ? names[lane] : juce::String ("----"));
        if (lane < laneTime)
            return "B" + breakLetter (lane - laneFirstBreak) + " " + (names[lane].isNotEmpty() ? names[lane] : juce::String ("----"));
        if (lane == laneTime)
            return "TIME  " + juce::String (timeSlots[ctx.paintTimeSlot].code);
        return "VOLUME  " + juce::String (volumeSlots[ctx.paintVolumeSlot].code);
    }

    void PatternGrid::refresh()
    {
        auto& proc = ctx.processor;
        bool dirty = false;

        const auto version = proc.getPatterns().getVersion();
        if (version != seenVersion || ctx.editPattern() != seenEditPattern)
        {
            seenVersion = version;
            seenEditPattern = ctx.editPattern();
            pat = ctx.pattern();
            dirty = true;
        }

        const int bankVersion = proc.getBank().getVersion();
        if (bankVersion != seenBankVersion)
        {
            seenBankVersion = bankVersion;
            for (int lane = 0; lane < laneTime; ++lane)
                names.set (lane, proc.getSampleRef (lane).displayName());
            dirty = true;
        }

        const bool showPlayhead = proc.meters.running.load() && proc.meters.playingPattern.load() == ctx.editPattern();
        for (int lane = 0; lane < numLanes; ++lane)
        {
            const int s = showPlayhead ? proc.meters.laneStep[(size_t) lane].load() : -1;
            if (s != playStep[(size_t) lane])
            {
                playStep[(size_t) lane] = s;
                dirty = true;
            }
            const float hit = proc.meters.laneFlash[(size_t) lane].exchange (0.0f);
            const float f = std::max (flash[(size_t) lane] * 0.78f, hit);
            if (std::abs (f - flash[(size_t) lane]) > 0.005f)
                dirty = true;
            flash[(size_t) lane] = f < 0.01f ? 0.0f : f;
        }

        if (dirty)
            repaint();
    }

    void PatternGrid::paint (juce::Graphics& g)
    {
        g.fillAll (Palette::background);
        const int steps = visibleSteps();
        const int first = firstStep();
        const float cw = (float) (getWidth() - headerWidth) / (float) steps;
        const float rh = rowHeight();
        const int spb = stepsPerBeat (pat);

        // Beat shading and the step numbers
        for (int s = 0; s < steps; ++s)
        {
            const int step = first + s;
            const float x = (float) headerWidth + cw * (float) s;
            if ((step / spb) % 2 == 1)
            {
                g.setColour (Palette::faint.withAlpha (0.45f));
                g.fillRect (x, (float) topRow, cw, (float) getHeight() - (float) topRow);
            }
            if (step % spb == 0)
            {
                g.setColour (step % (spb * 4) == 0 ? Palette::dim : Palette::dim.withAlpha (0.6f));
                g.setFont (monoFont (9.5f));
                g.drawText (juce::String (step + 1), juce::Rectangle<float> (x + 2.0f, 0.0f, cw * (float) spb, (float) topRow),
                            juce::Justification::centredLeft);
            }
        }

        for (int lane = 0; lane < numLanes; ++lane)
        {
            const float y = (float) topRow + rh * (float) lane;
            const auto& l = pat.lanes[(size_t) lane];
            const int len = pat.laneLength (lane);
            const auto tint = laneColour (lane);
            const bool selected = lane == ctx.selectedLane;

            // Header
            auto header = juce::Rectangle<float> (0.0f, y, (float) headerWidth - 2.0f, rh).reduced (0.0f, 0.5f);
            g.setColour (selected ? Palette::faint.brighter (0.35f) : Palette::panel);
            g.fillRect (header);
            if (flash[(size_t) lane] > 0.0f)
            {
                g.setColour (Palette::red.withAlpha (std::min (1.0f, flash[(size_t) lane])));
                g.fillRect (header.withWidth (3.0f));
            }
            g.setColour (lane == dropLane ? Palette::red : (selected ? Palette::bright : tint.withAlpha (l.mute ? 0.4f : 0.85f)));
            g.setFont (monoFont (std::min (11.0f, rh - 2.0f), selected));
            g.drawText (laneLabel (lane), header.withTrimmedLeft (5.0f).withTrimmedRight (16.0f), juce::Justification::centredLeft, true);
            auto mute = header.removeFromRight (14.0f).reduced (1.5f, 2.0f);
            g.setColour (l.mute ? Palette::red : Palette::border);
            g.drawRect (mute, 1.0f);
            g.setFont (monoFont (std::min (9.0f, rh - 3.0f), true));
            g.drawText ("M", mute, juce::Justification::centred);

            // Cells
            for (int s = 0; s < steps; ++s)
            {
                const int step = first + s;
                auto cell = juce::Rectangle<float> ((float) headerWidth + cw * (float) s, y, cw, rh).reduced (1.0f);
                if (step >= len)
                {
                    g.setColour (Palette::background.brighter (0.03f));
                    g.fillRect (cell);
                    continue;
                }
                const auto& st = l.steps[(size_t) step];

                if (isEffectLane (lane))
                {
                    if (st.slot == 0)
                    {
                        g.setColour (Palette::faint);
                        g.drawRect (cell, 0.6f);
                        continue;
                    }
                    const bool joinsNext = step + 1 < len && l.steps[(size_t) step + 1].slot == st.slot && s + 1 < steps;
                    const bool startsRegion = step == 0 || l.steps[(size_t) step - 1].slot != st.slot || s == 0;
                    auto fill = cell;
                    if (joinsNext)
                        fill.setRight (fill.getRight() + 2.0f);
                    g.setColour (tint.withAlpha (l.mute ? 0.25f : 0.55f));
                    g.fillRect (fill);
                    if (startsRegion)
                    {
                        g.setColour (Palette::background);
                        g.setFont (monoFont (std::min (10.0f, rh - 3.0f), true));
                        g.drawText (slotInfo (lane, st.slot).code, cell.withWidth (std::max (cell.getWidth(), 26.0f)), juce::Justification::centredLeft, false);
                    }
                    continue;
                }

                if (! st.on)
                {
                    g.setColour (Palette::faint.brighter (0.15f));
                    g.drawRect (cell, 0.6f);
                    continue;
                }

                const float vel = (float) st.velocity / 127.0f;
                auto body = cell;
                if (st.nudge != 0)
                    body = body.translated ((float) st.nudge * 0.01f * cw * 0.5f, 0.0f);
                g.setColour (tint.withAlpha ((0.30f + 0.70f * vel) * (l.mute ? 0.4f : 1.0f)));
                g.fillRoundedRectangle (body, 2.0f);
                if (st.velocity >= 120)
                {
                    g.setColour (Palette::red);
                    g.drawRoundedRectangle (body, 2.0f, 1.0f);
                }
                if (st.probability < 100)
                {
                    g.setColour (Palette::background);
                    const float dot = std::max (2.0f, rh * 0.18f);
                    g.fillEllipse (body.getRight() - dot - 2.0f, body.getY() + 2.0f, dot, dot);
                }
                if (st.ratchet > 1)
                {
                    g.setColour (Palette::background.withAlpha (0.8f));
                    for (int k = 1; k < st.ratchet; ++k)
                    {
                        const float x = body.getX() + body.getWidth() * (float) k / (float) st.ratchet;
                        g.drawVerticalLine ((int) x, body.getY() + 1.0f, body.getBottom() - 1.0f);
                    }
                }
                if (lane >= laneFirstBreak || st.pitch != 0)
                {
                    juce::String text;
                    if (lane >= laneFirstBreak)
                        text = st.slice < 0 ? "x" : juce::String ((int) st.slice);
                    if (st.pitch != 0)
                        text << (st.pitch > 0 ? "+" : "") << (int) st.pitch;
                    g.setColour (Palette::background);
                    g.setFont (monoFont (std::min (10.0f, rh - 3.0f), true));
                    g.drawText (text, body.reduced (2.0f, 0.0f), juce::Justification::centredLeft, false);
                }
            }

            // Playhead: a light column per lane (lanes with their own length run their own playhead)
            const int ps = playStep[(size_t) lane];
            if (ps >= first && ps < first + steps)
            {
                const auto r = juce::Rectangle<float> ((float) headerWidth + cw * (float) (ps - first), y, cw, rh);
                g.setColour (Palette::bright.withAlpha (0.14f));
                g.fillRect (r);
                const bool hit = isEffectLane (lane) ? l.steps[(size_t) ps].slot != 0 : l.steps[(size_t) ps].on != 0;
                if (hit)
                {
                    g.setColour (Palette::bright.withAlpha (0.9f));
                    g.drawRect (r.reduced (1.0f), 1.0f);
                }
            }
        }

        // Pattern length marker (lanes may run longer or shorter)
        if (pat.length >= first && pat.length < first + steps)
        {
            const float x = (float) headerWidth + cw * (float) (pat.length - first);
            g.setColour (Palette::red.withAlpha (0.6f));
            g.drawVerticalLine ((int) x, (float) topRow, (float) getHeight());
        }

        const float sy = (float) topRow + rh * (float) ctx.selectedLane;
        g.setColour (Palette::phosphor.withAlpha (0.5f));
        g.drawRect (juce::Rectangle<float> (0.0f, sy, (float) getWidth(), rh), 1.0f);
    }

    //==============================================================================
    void PatternGrid::applyCell (int lane, int step, const juce::MouseEvent& e, bool first)
    {
        if (step < 0 || step >= pat.laneLength (lane))
            return;
        const auto st = pat.lanes[(size_t) lane].steps[(size_t) step];

        if (isEffectLane (lane))
        {
            const int paint = lane == laneTime ? ctx.paintTimeSlot : ctx.paintVolumeSlot;
            if (first)
                dragMode = (e.mods.isRightButtonDown() || st.slot == paint) ? 4 : 3;
            const uint8_t slot = dragMode == 3 ? (uint8_t) paint : (uint8_t) 0;
            ctx.edit ([lane, step, slot] (Pattern& p) { p.lanes[(size_t) lane].steps[(size_t) step].slot = slot; });
        }
        else
        {
            if (first)
            {
                dragMode = st.on ? 2 : 1;
                dragVelocity = e.mods.isShiftDown() ? (uint8_t) 127 : ((e.mods.isAltDown() || e.mods.isCtrlDown()) ? (uint8_t) 50 : (uint8_t) 100);
            }
            const bool on = dragMode == 1;
            const auto vel = dragVelocity;
            ctx.edit ([lane, step, on, vel] (Pattern& p)
            {
                auto& s = p.lanes[(size_t) lane].steps[(size_t) step];
                s.on = on ? 1 : 0;
                if (on)
                    s.velocity = vel;
            });
            if (on && lane < laneTime && first)
                ctx.processor.audition (lane, (float) vel / 127.0f);
        }
        lastLane = lane;
        lastStep = step;
        pat = ctx.pattern();
        seenVersion = ctx.processor.getPatterns().getVersion();
        repaint();
    }

    void PatternGrid::mouseDown (const juce::MouseEvent& e)
    {
        const int lane = laneAt (e.y);
        if (lane < 0)
            return;

        if (e.x < headerWidth)
        {
            if (e.mods.isPopupMenu())
            {
                ctx.selectLane (lane);
                if (onLaneMenu)
                    onLaneMenu (lane, e.getScreenPosition());
                return;
            }
            if (e.x > headerWidth - 18)
            {
                ctx.beginEdit();
                ctx.edit ([lane] (Pattern& p) { p.lanes[(size_t) lane].mute ^= 1; });
                refresh();
                return;
            }
            ctx.selectLane (lane);
            if (lane < laneTime)
                ctx.processor.audition (lane, 0.85f);
            repaint();
            return;
        }

        const int step = stepAt (e.x);
        if (step < 0)
            return;
        ctx.selectLane (lane);

        if (e.mods.isPopupMenu() && ! isEffectLane (lane))
        {
            showStepMenu (lane, step);
            return;
        }
        if (e.mods.isPopupMenu() && isEffectLane (lane) && e.mods.isShiftDown())
        {
            showStepMenu (lane, step);
            return;
        }
        ctx.beginEdit();
        applyCell (lane, step, e, true);
    }

    void PatternGrid::mouseDrag (const juce::MouseEvent& e)
    {
        if (dragMode == 0 || lastLane < 0)
            return;
        const int step = stepAt (juce::jlimit (headerWidth, getWidth() - 1, e.x));
        if (step >= 0 && step != lastStep)
        {
            // Fill every cell between the last one and this one, so fast drags leave no gaps.
            const int from = std::min (lastStep, step), to = std::max (lastStep, step);
            for (int s = from; s <= to; ++s)
                if (s != lastStep)
                    applyCell (lastLane, s, e, false);
            lastStep = step;
        }
    }

    void PatternGrid::mouseUp (const juce::MouseEvent&)
    {
        dragMode = 0;
        lastLane = lastStep = -1;
    }

    void PatternGrid::mouseDoubleClick (const juce::MouseEvent& e)
    {
        const int lane = laneAt (e.y);
        if (lane >= 0 && lane < laneTime && e.x < headerWidth - 18 && onLoadSample)
            onLoadSample (lane);
    }

    int PatternGrid::naturalSliceCount (int lane) const
    {
        if (lane < laneFirstBreak || lane >= laneTime)
            return 32;
        if (auto ov = ctx.processor.getBank().getOverview (lane))
        {
            std::array<int, maxSlices> s {};
            return ctx.processor.computeSlices (lane - laneFirstBreak, *ov, s.data(), maxSlices);
        }
        return 16;
    }

    void PatternGrid::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
    {
        const int lane = laneAt (e.y);
        const int step = stepAt (e.x);
        if (lane < 0 || step < 0 || step >= pat.laneLength (lane))
            return;
        const int dir = wheel.deltaY > 0 ? 1 : (wheel.deltaY < 0 ? -1 : 0);
        if (dir == 0)
            return;
        const auto& st = pat.lanes[(size_t) lane].steps[(size_t) step];

        ctx.beginEdit();
        if (isEffectLane (lane))
        {
            const int n = slotCount (lane);
            const int slot = (st.slot + dir + n) % n;
            ctx.edit ([lane, step, slot] (Pattern& p) { p.lanes[(size_t) lane].steps[(size_t) step].slot = (uint8_t) slot; });
        }
        else if (st.on && lane >= laneFirstBreak)
        {
            const int n = std::max (1, naturalSliceCount (lane));
            int slice = st.slice + dir;
            if (slice < -1) slice = n - 1;
            if (slice >= n) slice = -1;
            ctx.edit ([lane, step, slice] (Pattern& p) { p.lanes[(size_t) lane].steps[(size_t) step].slice = (int8_t) slice; });
            if (slice >= 0)
                ctx.processor.auditionSlice (lane - laneFirstBreak, slice);
        }
        else if (st.on)
        {
            const int vel = juce::jlimit (1, 127, (int) st.velocity + dir * 6);
            ctx.edit ([lane, step, vel] (Pattern& p) { p.lanes[(size_t) lane].steps[(size_t) step].velocity = (uint8_t) vel; });
        }
        refresh();
    }

    void PatternGrid::showStepMenu (int lane, int step)
    {
        const auto st = pat.lanes[(size_t) lane].steps[(size_t) step];
        juce::PopupMenu menu;
        menu.addSectionHeader ("STEP " + juce::String (step + 1) + "  /  " + laneLabel (lane));

        if (isEffectLane (lane))
        {
            for (int k = 0; k < slotCount (lane); ++k)
                menu.addItem (700 + k, slotInfo (lane, k).name, true, st.slot == k);
        }
        else
        {
            menu.addItem (2, st.on ? "Off" : "On");
            juce::PopupMenu vel, rat, prob, nudge, pitch;
            for (int v : { 127, 110, 100, 80, 64, 50, 32, 16 })
                vel.addItem (100 + v, juce::String (v) + (v == 127 ? "  accent" : (v == 50 ? "  ghost" : "")), true, st.velocity == v);
            for (int r = 1; r <= 4; ++r)
                rat.addItem (200 + r, r == 1 ? juce::String ("1 (single)") : juce::String (r) + " hits", true, st.ratchet == r);
            for (int p : { 100, 90, 75, 50, 33, 25, 10 })
                prob.addItem (300 + p, juce::String (p) + "%", true, st.probability == p);
            for (int n : { -50, -33, -25, -10, 0, 10, 25, 33, 50 })
                nudge.addItem (400 + n + 50, (n > 0 ? "+" : "") + juce::String (n) + "% of a step", true, st.nudge == n);
            for (int t : { -24, -12, -7, -5, -2, -1, 0, 1, 2, 5, 7, 12, 24 })
                pitch.addItem (500 + t + 24, (t > 0 ? "+" : "") + juce::String (t) + " st", true, st.pitch == t);
            menu.addSubMenu ("Velocity", vel);
            menu.addSubMenu ("Ratchet", rat);
            menu.addSubMenu ("Probability", prob);
            menu.addSubMenu ("Nudge", nudge);
            menu.addSubMenu ("Pitch", pitch);
            if (lane >= laneFirstBreak)
            {
                juce::PopupMenu slice;
                slice.addItem (600, "Natural (the loop's own position)", true, st.slice < 0);
                const int n = std::min (naturalSliceCount (lane), 64);
                for (int k = 0; k < n; ++k)
                    slice.addItem (601 + k, "Slice " + juce::String (k), true, st.slice == k);
                menu.addSubMenu ("Slice", slice);
            }
            menu.addSeparator();
            menu.addItem (1, "Clear step");
        }

        juce::Component::SafePointer<PatternGrid> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [safe, lane, step] (int r)
        {
            if (safe == nullptr || r == 0)
                return;
            auto& c = safe->ctx;
            c.beginEdit();
            c.edit ([r, lane, step] (Pattern& p)
            {
                auto& s = p.lanes[(size_t) lane].steps[(size_t) step];
                if (r == 1)               s = Step();
                else if (r == 2)          s.on ^= 1;
                else if (r >= 700)        s.slot = (uint8_t) (r - 700);
                else if (r >= 600)        { s.slice = (int8_t) (r - 601); s.on = 1; }
                else if (r >= 500)        { s.pitch = (int8_t) (r - 500 - 24); s.on = 1; }
                else if (r >= 400)        { s.nudge = (int8_t) (r - 400 - 50); s.on = 1; }
                else if (r >= 300)        { s.probability = (uint8_t) (r - 300); s.on = 1; }
                else if (r >= 200)        { s.ratchet = (uint8_t) (r - 200); s.on = 1; }
                else if (r >= 100)        { s.velocity = (uint8_t) (r - 100); s.on = 1; }
            });
            safe->refresh();
        });
    }

    //==============================================================================
    bool PatternGrid::isInterestedInFileDrag (const juce::StringArray& files)
    {
        for (const auto& f : files)
            if (isAudioFile (f))
                return true;
        return false;
    }

    void PatternGrid::fileDragMove (const juce::StringArray&, int, int y)
    {
        const int lane = laneAt (y);
        dropLane = lane >= 0 && lane < laneTime ? lane : -1;
        repaint();
    }

    void PatternGrid::fileDragExit (const juce::StringArray&)
    {
        dropLane = -1;
        repaint();
    }

    void PatternGrid::filesDropped (const juce::StringArray& files, int, int y)
    {
        dropLane = -1;
        int lane = laneAt (y);
        if (lane < 0 || lane >= laneTime)
            return;
        // Several files dropped on a pad fill the pads from there on (a kit in one gesture).
        const int last = lane < numPads ? numPads : laneTime;
        for (const auto& f : files)
        {
            if (! isAudioFile (f) || lane >= last)
                continue;
            ctx.processor.assignSample (lane, ctx.processor.getLibrary().makeRef (juce::File (f)));
            ++lane;
        }
        ctx.selectLane (juce::jlimit (0, laneTime - 1, laneAt (y)));
        repaint();
    }

    bool PatternGrid::isInterestedInDragSource (const SourceDetails& d)
    {
        return d.description.toString().startsWith ("labx0ref:");
    }

    void PatternGrid::itemDragMove (const SourceDetails& d)
    {
        const int lane = laneAt (d.localPosition.y);
        dropLane = lane >= 0 && lane < laneTime ? lane : -1;
        repaint();
    }

    void PatternGrid::itemDragExit (const SourceDetails&)
    {
        dropLane = -1;
        repaint();
    }

    void PatternGrid::itemDropped (const SourceDetails& d)
    {
        dropLane = -1;
        const int lane = laneAt (d.localPosition.y);
        if (lane >= 0 && lane < laneTime)
        {
            ctx.processor.assignSample (lane, SampleRef::fromString (d.description.toString().fromFirstOccurrenceOf ("labx0ref:", false, false)));
            ctx.selectLane (lane);
        }
        repaint();
    }

    //==============================================================================
    StepStrip::StepStrip (EditorContext& context, PatternGrid& g) : ctx (context), grid (g)
    {
        ctx.addChangeListener (this);
    }

    StepStrip::~StepStrip()
    {
        ctx.removeChangeListener (this);
    }

    void StepStrip::refresh()
    {
        const auto v = ctx.processor.getPatterns().getVersion();
        if (v != seenVersion)
        {
            seenVersion = v;
            pat = ctx.pattern();
        }
        repaint();
    }

    int StepStrip::valueRange (int& lo, int& hi) const
    {
        switch (ctx.stripMode)
        {
            case EditorContext::stripVelocity:    lo = 1;   hi = 127; break;
            case EditorContext::stripProbability: lo = 1;   hi = 100; break;
            case EditorContext::stripRatchet:     lo = 1;   hi = 4;   break;
            case EditorContext::stripNudge:       lo = -50; hi = 50;  break;
            case EditorContext::stripPitch:       lo = -24; hi = 24;  break;
            default:                              lo = -1;  hi = 31;  break;
        }
        return hi - lo;
    }

    int StepStrip::valueOf (const Step& s, int mode)
    {
        switch (mode)
        {
            case EditorContext::stripVelocity:    return s.velocity;
            case EditorContext::stripProbability: return s.probability;
            case EditorContext::stripRatchet:     return s.ratchet;
            case EditorContext::stripNudge:       return s.nudge;
            case EditorContext::stripPitch:       return s.pitch;
            default:                              return s.slice;
        }
    }

    void StepStrip::setValue (Step& s, int mode, int v)
    {
        switch (mode)
        {
            case EditorContext::stripVelocity:    s.velocity = (uint8_t) juce::jlimit (1, 127, v); break;
            case EditorContext::stripProbability: s.probability = (uint8_t) juce::jlimit (1, 100, v); break;
            case EditorContext::stripRatchet:     s.ratchet = (uint8_t) juce::jlimit (1, 4, v); break;
            case EditorContext::stripNudge:       s.nudge = (int8_t) juce::jlimit (-50, 50, v); break;
            case EditorContext::stripPitch:       s.pitch = (int8_t) juce::jlimit (-24, 24, v); break;
            default:                              s.slice = (int8_t) juce::jlimit (-1, maxSlices - 1, v); break;
        }
    }

    void StepStrip::paint (juce::Graphics& g)
    {
        g.fillAll (Palette::background);
        const int lane = ctx.selectedLane;
        const auto area = getLocalBounds().toFloat().withTrimmedLeft ((float) PatternGrid::headerWidth);
        g.setColour (Palette::border);
        g.drawRect (getLocalBounds().toFloat(), 1.0f);

        if (lane >= laneTime)
        {
            g.setColour (Palette::dim);
            g.setFont (monoFont (11.0f));
            g.drawText ("TIME AND VOLUME LANES: PICK A SLOT IN THE LANE PANEL, PAINT IT IN THE GRID",
                        area.reduced (8.0f, 0.0f), juce::Justification::centredLeft);
            return;
        }

        int lo, hi;
        valueRange (lo, hi);
        const bool bipolar = lo < 0 && ctx.stripMode != EditorContext::stripSlice;
        const int steps = grid.visibleSteps();
        const int first = grid.firstStep();
        const float cw = area.getWidth() / (float) steps;
        const auto tint = lane < laneFirstBreak ? Palette::phosphor : Palette::breakTint;
        const auto& l = pat.lanes[(size_t) lane];
        const float zeroY = bipolar ? area.getCentreY() : area.getBottom() - 2.0f;

        if (bipolar)
        {
            g.setColour (Palette::faint.brighter (0.2f));
            g.drawHorizontalLine ((int) zeroY, area.getX(), area.getRight());
        }

        for (int s = 0; s < steps; ++s)
        {
            const int step = first + s;
            if (step >= pat.laneLength (lane))
                break;
            const auto& st = l.steps[(size_t) step];
            if (! st.on)
                continue;
            const int v = valueOf (st, ctx.stripMode);
            const float x = area.getX() + cw * (float) s;
            float top, bottom;
            if (ctx.stripMode == EditorContext::stripSlice)
            {
                const float h = v < 0 ? 0.25f : 0.25f + 0.75f * (float) v / (float) hi;
                top = area.getBottom() - 2.0f - h * (area.getHeight() - 14.0f);
                bottom = area.getBottom() - 2.0f;
            }
            else if (bipolar)
            {
                const float y = area.getCentreY() - (float) v / (float) hi * (area.getHeight() * 0.5f - 6.0f);
                top = std::min (y, zeroY);
                bottom = std::max (y, zeroY);
                if (v == 0)
                    bottom = top + 1.5f;
            }
            else
            {
                const float h = (float) (v - lo + 1) / (float) (hi - lo + 1);
                top = area.getBottom() - 2.0f - h * (area.getHeight() - 14.0f);
                bottom = area.getBottom() - 2.0f;
            }
            const auto bar = juce::Rectangle<float> (x + 2.0f, top, cw - 4.0f, std::max (1.5f, bottom - top));
            g.setColour (tint.withAlpha (0.75f));
            g.fillRect (bar);
            if (ctx.stripMode != EditorContext::stripVelocity && ctx.stripMode != EditorContext::stripProbability)
            {
                g.setColour (Palette::bright);
                g.setFont (monoFont (9.5f));
                const auto text = ctx.stripMode == EditorContext::stripSlice && v < 0 ? juce::String ("x") : juce::String (v);
                g.drawText (text, juce::Rectangle<float> (x, area.getY() + 1.0f, cw, 11.0f), juce::Justification::centred);
            }
        }
    }

    void StepStrip::setFrom (const juce::MouseEvent& e)
    {
        const int lane = ctx.selectedLane;
        if (lane >= laneTime)
            return;
        const int step = grid.stepAt (e.x);
        if (step < 0 || step >= pat.laneLength (lane) || ! pat.lanes[(size_t) lane].steps[(size_t) step].on)
            return;
        int lo, hi;
        valueRange (lo, hi);
        const auto area = getLocalBounds().toFloat().withTrimmedLeft ((float) PatternGrid::headerWidth);
        const bool bipolar = lo < 0 && ctx.stripMode != EditorContext::stripSlice;
        int v;
        if (bipolar)
            v = juce::roundToInt ((area.getCentreY() - (float) e.y) / (area.getHeight() * 0.5f - 6.0f) * (float) hi);
        else
        {
            const float frac = juce::jlimit (0.0f, 1.0f, (area.getBottom() - 2.0f - (float) e.y) / (area.getHeight() - 14.0f));
            v = lo + juce::roundToInt (frac * (float) (hi - lo));
        }
        v = juce::jlimit (lo, hi, v);
        const int mode = ctx.stripMode;
        ctx.edit ([lane, step, mode, v] (Pattern& p) { setValue (p.lanes[(size_t) lane].steps[(size_t) step], mode, v); });
        pat = ctx.pattern();
        seenVersion = ctx.processor.getPatterns().getVersion();
        repaint();
        grid.refresh();
    }

    void StepStrip::mouseDown (const juce::MouseEvent& e)
    {
        ctx.beginEdit();
        setFrom (e);
    }

    void StepStrip::mouseDrag (const juce::MouseEvent& e)
    {
        setFrom (e);
    }

    //==============================================================================
    WaveView::WaveView (EditorContext& context) : ctx (context)
    {
        ctx.addChangeListener (this);
    }

    WaveView::~WaveView()
    {
        ctx.removeChangeListener (this);
    }

    void WaveView::refresh()
    {
        auto& proc = ctx.processor;
        const int lane = ctx.selectedLane;
        bool dirty = lane != lastLane;
        lastLane = lane;

        if (lane < laneTime)
        {
            auto ov = proc.getBank().getOverview (lane);
            const auto text = proc.getBank().getStatusText (lane);
            if (ov != overview || text != status)
            {
                overview = ov;
                status = text;
                dirty = true;
            }
            if (lane >= laneFirstBreak && overview != nullptr)
            {
                const int b = lane - laneFirstBreak;
                const auto key = juce::String ((juce::int64) overview->id) + "/" + juce::String (proc.apvts.getRawParameterValue (breakParamId (b, P::brSlicing))->load())
                               + "/" + juce::String (proc.apvts.getRawParameterValue (breakParamId (b, P::brSens))->load(), 3)
                               + "/" + juce::String (proc.apvts.getRawParameterValue (breakParamId (b, P::brBeats))->load());
                if (key != sliceKey)
                {
                    sliceKey = key;
                    slices.assign ((size_t) maxSlices, 0);
                    const int n = proc.computeSlices (b, *overview, slices.data(), maxSlices);
                    slices.resize ((size_t) n);
                    beats = proc.loopBeatsFor (b, *overview);
                    dirty = true;
                }
                const double p = proc.meters.breakPosition[(size_t) b].load();
                if (std::abs (p - position) > 1.0)
                {
                    position = p;
                    dirty = true;
                }
            }
            else
            {
                slices.clear();
                position = -1.0;
            }
        }
        else
        {
            overview = nullptr;
            slices.clear();
        }

        if (dirty)
            repaint();
    }

    void WaveView::paint (juce::Graphics& g)
    {
        const auto b = getLocalBounds().toFloat();
        g.fillAll (Palette::background);
        g.setColour (Palette::border);
        g.drawRect (b, 1.0f);
        const int lane = ctx.selectedLane;

        if (lane >= laneTime)
        {
            g.setFont (monoFont (10.5f));
            g.setColour (Palette::dim);
            juce::String legend;
            if (lane == laneTime)
                for (int k = 1; k < Slot::numTime; ++k)
                    legend << timeSlots[k].code << " " << timeSlots[k].name << "   ";
            else
                for (int k = 1; k < Slot::numVolume; ++k)
                    legend << volumeSlots[k].code << " " << volumeSlots[k].name << "   ";
            g.drawFittedText (legend, b.reduced (8.0f, 4.0f).toNearestInt(), juce::Justification::centredLeft, 3, 0.8f);
            return;
        }

        const auto tint = lane < laneFirstBreak ? Palette::phosphor : Palette::breakTint;
        const auto area = b.reduced (2.0f);
        if (overview != nullptr && ! overview->peaks.empty())
        {
            const int columns = (int) overview->peaks.size() / 2;
            const float mid = area.getCentreY(), half = area.getHeight() * 0.45f;

            // Beat grid
            if (beats > 0.0 && beats < 256.0)
            {
                for (int k = 0; k <= (int) std::ceil (beats); ++k)
                {
                    const float x = area.getX() + area.getWidth() * (float) ((double) k / beats);
                    if (x > area.getRight())
                        break;
                    g.setColour (k % 4 == 0 ? Palette::dim.withAlpha (0.5f) : Palette::faint.brighter (0.2f));
                    g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
                }
            }

            g.setColour (tint.withAlpha (0.8f));
            for (int c = 0; c < columns; ++c)
            {
                const float x = area.getX() + area.getWidth() * (float) c / (float) columns;
                const float lo = overview->peaks[(size_t) c * 2], hi = overview->peaks[(size_t) c * 2 + 1];
                g.drawVerticalLine ((int) x, mid - hi * half, mid - lo * half + 1.0f);
            }

            // Slices
            g.setFont (monoFont (9.0f, true));
            for (size_t k = 0; k < slices.size(); ++k)
            {
                const float x = area.getX() + area.getWidth() * (float) slices[k] / (float) std::max (1, overview->length);
                g.setColour (Palette::red.withAlpha (0.75f));
                g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
                g.drawText (juce::String ((int) k), juce::Rectangle<float> (x + 1.0f, area.getBottom() - 11.0f, 22.0f, 10.0f),
                            juce::Justification::centredLeft);
            }

            // Pad start marker
            if (lane < numPads)
            {
                const float start = ctx.processor.apvts.getRawParameterValue (padParamId (lane, P::padStart))->load();
                if (start > 0.0005f)
                {
                    const float x = area.getX() + area.getWidth() * start;
                    g.setColour (Palette::red);
                    g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
                }
            }

            if (position >= 0.0 && overview->length > 0)
            {
                const float x = area.getX() + area.getWidth() * (float) (position / (double) overview->length);
                g.setColour (Palette::bright);
                g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
            }
        }

        g.setFont (monoFont (10.5f, true));
        g.setColour (ctx.processor.getBank().getStatus (lane) == SampleBank::Status::ready ? Palette::bright : Palette::red);
        g.drawText (status.isNotEmpty() ? status : juce::String ("EMPTY  /  DOUBLE CLICK THE LANE NAME OR DROP A FILE"),
                    area.reduced (6.0f, 2.0f), juce::Justification::topLeft, true);
    }

    void WaveView::mouseDown (const juce::MouseEvent& e)
    {
        const int lane = ctx.selectedLane;
        if (lane < laneFirstBreak || lane >= laneTime || overview == nullptr || slices.empty())
        {
            if (lane < laneTime)
                ctx.processor.audition (lane, 0.85f);
            return;
        }
        const auto area = getLocalBounds().toFloat().reduced (2.0f);
        const double pos = (double) (e.x - area.getX()) / (double) area.getWidth() * (double) overview->length;
        int slice = 0;
        while (slice + 1 < (int) slices.size() && (double) slices[(size_t) slice + 1] <= pos)
            ++slice;
        ctx.processor.auditionSlice (lane - laneFirstBreak, slice);
    }

    //==============================================================================
    PatternPanel::PatternPanel (EditorContext& context)
        : grid (context), ctx (context), strip (context, grid), wave (context)
    {
        addAndMakeVisible (grid);
        addAndMakeVisible (strip);
        addAndMakeVisible (wave);

        for (int i = 0; i < numPatterns; ++i)
        {
            auto& b = patternButtons[(size_t) i];
            b.setButtonText (juce::String (i + 1).paddedLeft ('0', 2));
            styleButton (b, "Edit pattern " + juce::String (i + 1) + " (it plays from the next pattern boundary)");
            b.onClick = [this, i] { ctx.processor.setEditPattern (i); updateButtons(); };
            addAndMakeVisible (b);
        }

        for (int i = 1; i <= maxSteps; ++i)
            lengthBox.addItem (juce::String (i), i);
        lengthBox.setTooltip ("Pattern length in steps");
        lengthBox.onChange = [this]
        {
            const int len = lengthBox.getSelectedId();
            if (len <= 0 || len == shownLength)
                return;
            ctx.beginEdit();
            ctx.edit ([len] (Pattern& p) { p.length = (uint8_t) len; });
            if (len <= 32 && ctx.page == 1)
                ctx.setPage (0);
        };
        addAndMakeVisible (lengthBox);

        for (int i = 0; i < numResolutions; ++i)
            resolutionBox.addItem (resolutionNames[i], i + 1);
        resolutionBox.setTooltip ("Step resolution");
        resolutionBox.onChange = [this]
        {
            const int r = resolutionBox.getSelectedId() - 1;
            if (r < 0 || r == shownResolution)
                return;
            ctx.beginEdit();
            ctx.edit ([r] (Pattern& p) { p.resolution = (uint8_t) r; });
        };
        addAndMakeVisible (resolutionBox);

        styleButton (pageA, "Steps 1-32");
        styleButton (pageB, "Steps 33-64");
        pageA.onClick = [this] { ctx.setPage (0); };
        pageB.onClick = [this] { ctx.setPage (1); };
        styleButton (playButton, "Run the patterns on the internal clock (for the standalone app, or with the host stopped)", true);
        playButton.onClick = [this]
        {
            if (auto* p = ctx.processor.apvts.getParameter ("run"))
                p->setValueNotifyingHost (playButton.getToggleState() ? 1.0f : 0.0f);
        };
        for (auto* b : { &pageA, &pageB, &playButton })
            addAndMakeVisible (*b);

        styleButton (undoButton, "Undo the last edit");
        undoButton.onClick = [this] { ctx.undoLast(); grid.refresh(); };
        styleButton (copyButton, "Copy this pattern");
        copyButton.onClick = [this] { ctx.patternClip = ctx.pattern(); ctx.hasPatternClip = true; updateButtons(); };
        styleButton (pasteButton, "Paste the copied pattern here");
        pasteButton.onClick = [this]
        {
            if (! ctx.hasPatternClip)
                return;
            ctx.beginEdit();
            const auto clip = ctx.patternClip;
            ctx.edit ([clip] (Pattern& p) { p = clip; });
        };
        styleButton (clearButton, "Clear this pattern");
        clearButton.onClick = [this] { ctx.beginEdit(); ctx.edit ([] (Pattern& p) { const auto len = p.length; const auto res = p.resolution; p.clear(); p.length = len; p.resolution = res; }); };

        styleButton (euclidButton, "Spread a number of hits evenly over the selected lane");
        euclidButton.onClick = [this]
        {
            const int lane = ctx.selectedLane;
            if (lane >= laneTime)
                return;
            juce::PopupMenu m;
            const int len = ctx.pattern().laneLength (lane);
            for (int k = 1; k <= std::min (len, 24); ++k)
                m.addItem (k, juce::String (k) + " of " + juce::String (len));
            juce::Component::SafePointer<PatternPanel> safe (this);
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&euclidButton), [safe, lane, len] (int hits)
            {
                if (safe == nullptr || hits <= 0)
                    return;
                safe->ctx.beginEdit();
                safe->ctx.edit ([lane, len, hits] (Pattern& p) { PatternTools::euclid (p.lanes[(size_t) lane], len, hits, 0); });
            });
        };
        styleButton (fillButton, "Random hits on the selected lane");
        fillButton.onClick = [this]
        {
            const int lane = ctx.selectedLane;
            if (lane >= laneTime)
                return;
            juce::PopupMenu m;
            for (int d : { 12, 25, 40, 60, 80 })
                m.addItem (d, juce::String (d) + "% of the steps");
            juce::Component::SafePointer<PatternPanel> safe (this);
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&fillButton), [safe, lane] (int d)
            {
                if (safe == nullptr || d <= 0)
                    return;
                const auto seed = (uint32_t) juce::Time::getMillisecondCounter();
                safe->ctx.beginEdit();
                safe->ctx.edit ([lane, d, seed] (Pattern& p) { PatternTools::randomFill (p.lanes[(size_t) lane], p.laneLength (lane), (float) d / 100.0f, seed); });
            });
        };
        styleButton (leftButton, "Shift the selected lane one step earlier");
        styleButton (rightButton, "Shift the selected lane one step later");
        leftButton.onClick = [this] { const int lane = ctx.selectedLane; ctx.beginEdit(); ctx.edit ([lane] (Pattern& p) { PatternTools::rotate (p.lanes[(size_t) lane], p.laneLength (lane), -1); }); };
        rightButton.onClick = [this] { const int lane = ctx.selectedLane; ctx.beginEdit(); ctx.edit ([lane] (Pattern& p) { PatternTools::rotate (p.lanes[(size_t) lane], p.laneLength (lane), 1); }); };

        styleButton (shuffleButton, "Break lane: shuffle the order of the slices (press again for another order)");
        styleButton (reverseButton, "Break lane: play the slices in reverse order");
        styleButton (rollButton, "Break lane: every fourth hit repeats the one before as a double (jungle roll)");
        styleButton (halfButton, "Half time: the first half of the lane spreads over all of it");
        styleButton (doubleButton, "Double time: the lane plays twice as fast, twice");
        shuffleButton.onClick = [this] { breakTool (0); };
        reverseButton.onClick = [this] { breakTool (1); };
        rollButton.onClick = [this] { breakTool (2); };
        halfButton.onClick = [this] { breakTool (3); };
        doubleButton.onClick = [this] { breakTool (4); };

        for (auto* b : { &undoButton, &copyButton, &pasteButton, &clearButton, &euclidButton, &fillButton, &leftButton, &rightButton,
                         &shuffleButton, &reverseButton, &rollButton, &halfButton, &doubleButton })
            addAndMakeVisible (*b);

        const char* modeNames[] = { "VEL", "PROB", "RATCH", "NUDGE", "PITCH", "SLICE" };
        const char* modeTips[] = { "Velocity of each step", "Chance that each step plays", "Hits inside each step (1-4)",
                                   "Push each step early or late", "Pitch of each step in semitones", "Break lanes: the slice each step plays (x = natural)" };
        for (int m = 0; m < EditorContext::numStripModes; ++m)
        {
            auto& b = modeButtons[(size_t) m];
            b.setButtonText (modeNames[m]);
            styleButton (b, modeTips[m]);
            b.onClick = [this, m] { ctx.setStripMode (m); };
            addAndMakeVisible (b);
        }

        laneInfo.setFont (monoFont (10.5f));
        laneInfo.setColour (juce::Label::textColourId, Palette::dim);
        laneInfo.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (laneInfo);

        grid.onLoadSample = [this] (int lane) { if (onLoadSample) onLoadSample (lane); };
        grid.onLaneMenu = [this] (int lane, juce::Point<int> where) { laneMenu (lane, where); };

        ctx.addChangeListener (this);
        updateButtons();
    }

    PatternPanel::~PatternPanel()
    {
        ctx.removeChangeListener (this);
    }

    void PatternPanel::paint (juce::Graphics&) {}

    void PatternPanel::resized()
    {
        auto b = getLocalBounds();
        auto row1 = b.removeFromTop (24);
        const int pw = 25;
        for (auto& pb : patternButtons)
        {
            pb.setBounds (row1.removeFromLeft (pw).reduced (1, 1));
        }
        row1.removeFromLeft (8);
        lengthBox.setBounds (row1.removeFromLeft (56).reduced (1, 1));
        row1.removeFromLeft (4);
        resolutionBox.setBounds (row1.removeFromLeft (64).reduced (1, 1));
        row1.removeFromLeft (6);
        playButton.setBounds (row1.removeFromRight (56).reduced (1, 1));
        row1.removeFromRight (6);
        pageB.setBounds (row1.removeFromRight (52).reduced (1, 1));
        pageA.setBounds (row1.removeFromRight (46).reduced (1, 1));

        b.removeFromTop (3);
        auto row2 = b.removeFromTop (22);
        auto place = [&row2] (juce::Component& c, int w) { c.setBounds (row2.removeFromLeft (w).reduced (1, 1)); };
        place (undoButton, 46); place (copyButton, 46); place (pasteButton, 50); place (clearButton, 50);
        row2.removeFromLeft (8);
        place (euclidButton, 56); place (fillButton, 42); place (leftButton, 24); place (rightButton, 24);
        row2.removeFromLeft (8);
        place (shuffleButton, 62); place (reverseButton, 62); place (rollButton, 44); place (halfButton, 44); place (doubleButton, 56);
        laneInfo.setBounds (row2.reduced (4, 0));

        b.removeFromTop (4);
        auto waveArea = b.removeFromBottom (54);
        b.removeFromBottom (4);
        auto stripArea = b.removeFromBottom (56);
        b.removeFromBottom (4);
        grid.setBounds (b);
        strip.setBounds (stripArea);
        wave.setBounds (waveArea);

        // Strip mode buttons sit in the strip's header column, two rows of three.
        auto modes = stripArea.withWidth (PatternGrid::headerWidth).reduced (3, 4);
        const int bw = modes.getWidth() / 3, bh = modes.getHeight() / 2;
        for (int m = 0; m < EditorContext::numStripModes; ++m)
            modeButtons[(size_t) m].setBounds (modes.getX() + (m % 3) * bw, modes.getY() + (m / 3) * bh, bw - 1, bh - 1);
        modeButtons[0].toFront (false);
        for (auto& mb : modeButtons)
            mb.toFront (false);
    }

    void PatternPanel::refresh()
    {
        grid.refresh();
        strip.refresh();
        wave.refresh();
        const auto v = ctx.processor.getPatterns().getVersion();
        if (v != seenVersion || ctx.editPattern() != shownPattern)
        {
            seenVersion = v;
            updateButtons();
        }
        // Playing and queued pattern markers
        const int playing = ctx.processor.meters.playingPattern.load();
        const int queued = ctx.processor.meters.queuedPattern.load();
        const bool running = ctx.processor.meters.running.load();
        for (int i = 0; i < numPatterns; ++i)
        {
            auto text = juce::String (i + 1).paddedLeft ('0', 2);
            if (running && i == playing)
                text = ">" + text;
            else if (i == queued)
                text = "*" + text;
            if (patternButtons[(size_t) i].getButtonText() != text)
                patternButtons[(size_t) i].setButtonText (text);
        }
        const bool free = ctx.processor.apvts.getRawParameterValue ("run")->load() > 0.5f;
        if (playButton.getToggleState() != free)
            playButton.setToggleState (free, juce::dontSendNotification);
    }

    void PatternPanel::updateButtons()
    {
        const auto p = ctx.pattern();
        shownPattern = ctx.editPattern();
        std::array<bool, numPatterns> filled {};
        ctx.processor.getPatterns().read ([&filled] (const PatternBank& b)
        {
            for (int i = 0; i < numPatterns; ++i)
                filled[(size_t) i] = ! b.patterns[(size_t) i].isEmpty();
        });
        for (int i = 0; i < numPatterns; ++i)
        {
            auto& b = patternButtons[(size_t) i];
            b.setToggleState (i == shownPattern, juce::dontSendNotification);
            b.setColour (juce::TextButton::textColourOffId, filled[(size_t) i] ? Palette::bright : Palette::dim.withAlpha (0.7f));
        }
        shownLength = p.length;
        shownResolution = p.resolution;
        lengthBox.setSelectedId (p.length, juce::dontSendNotification);
        resolutionBox.setSelectedId (p.resolution + 1, juce::dontSendNotification);

        int longest = p.length;
        for (int lane = 0; lane < numLanes; ++lane)
            longest = std::max (longest, p.laneLength (lane));
        pageB.setEnabled (longest > 32);
        pageA.setToggleState (ctx.page == 0, juce::dontSendNotification);
        pageB.setToggleState (ctx.page == 1, juce::dontSendNotification);
        pasteButton.setEnabled (ctx.hasPatternClip);

        const int lane = ctx.selectedLane;
        const bool isBreak = lane >= laneFirstBreak && lane < laneTime;
        for (auto* b : { &shuffleButton, &reverseButton, &rollButton })
            b->setEnabled (isBreak);
        for (auto* b : { &euclidButton, &fillButton, &leftButton, &rightButton, &halfButton, &doubleButton })
            b->setEnabled (lane < laneTime || b == &leftButton || b == &rightButton || b == &halfButton || b == &doubleButton);
        modeButtons[(size_t) EditorContext::stripSlice].setEnabled (isBreak);
        for (int m = 0; m < EditorContext::numStripModes; ++m)
            modeButtons[(size_t) m].setToggleState (m == ctx.stripMode, juce::dontSendNotification);

        int on = 0;
        const int len = p.laneLength (lane);
        for (int s = 0; s < len; ++s)
            on += lane >= laneTime ? (p.lanes[(size_t) lane].steps[(size_t) s].slot != 0 ? 1 : 0) : p.lanes[(size_t) lane].steps[(size_t) s].on;
        laneInfo.setText ("LEN " + juce::String (len) + (p.lanes[(size_t) lane].length > 0 ? "*" : "") + "  HITS " + juce::String (on),
                          juce::dontSendNotification);
        grid.repaint();
        strip.repaint();
    }

    int PatternPanel::naturalSlice (int breakSlot, int step, const Pattern& p) const
    {
        auto ov = ctx.processor.getBank().getOverview (slotFirstBreak + breakSlot);
        if (ov == nullptr || ov->length <= 0)
            return step % 16;
        std::array<int, maxSlices> starts {};
        const int n = ctx.processor.computeSlices (breakSlot, *ov, starts.data(), maxSlices);
        const double beats = ctx.processor.loopBeatsFor (breakSlot, *ov);
        double frac = std::fmod ((double) step * p.stepQuarters(), beats) / beats;
        if (frac < 0.0)
            frac += 1.0;
        const double pos = frac * (double) ov->length;
        int slice = 0;
        while (slice + 1 < n && (double) starts[(size_t) slice + 1] <= pos + 0.5)
            ++slice;
        return slice;
    }

    void PatternPanel::breakTool (int which)
    {
        const int lane = ctx.selectedLane;
        if (lane >= laneTime)
            return;
        const auto current = ctx.pattern();
        const bool isBreak = lane >= laneFirstBreak;
        std::vector<int> natural ((size_t) maxSteps, 0);
        if (isBreak)
            for (int s = 0; s < maxSteps; ++s)
                natural[(size_t) s] = naturalSlice (lane - laneFirstBreak, s, current);
        const uint32_t seed = shuffleSeed++;

        ctx.beginEdit();
        ctx.edit ([=] (Pattern& p)
        {
            auto& l = p.lanes[(size_t) lane];
            const int len = p.laneLength (lane);
            if (isBreak && which <= 2)
                PatternTools::materialise (l, len, [&natural] (int s) { return natural[(size_t) s]; });
            switch (which)
            {
                case 0: if (isBreak) PatternTools::shuffleSlices (l, len, seed * 2654435761u); break;
                case 1: if (isBreak) PatternTools::reverseSlices (l, len); break;
                case 2: if (isBreak) PatternTools::rollSlices (l, len); break;
                case 3: PatternTools::halfTime (l, len); break;
                case 4: PatternTools::doubleTime (l, len); break;
                default: break;
            }
        });
    }

    void PatternPanel::laneMenu (int lane, juce::Point<int>)
    {
        const auto p = ctx.pattern();
        const auto& l = p.lanes[(size_t) lane];
        juce::PopupMenu m;
        m.addSectionHeader ("LANE  " + juce::String (lane + 1));
        m.addItem (1, "Clear lane");
        m.addItem (2, "Copy lane");
        m.addItem (3, "Paste lane", ctx.hasLaneClip && (ctx.laneClipIsEffect == (lane >= laneTime)));
        m.addItem (4, l.mute ? "Unmute" : "Mute");
        juce::PopupMenu len;
        len.addItem (100, "Follow the pattern (" + juce::String ((int) p.length) + ")", true, l.length == 0);
        for (int k : { 3, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15, 16, 20, 24, 28, 32, 48, 64 })
            len.addItem (100 + k, juce::String (k) + " steps", true, l.length == k);
        m.addSubMenu ("Lane length (polymeter)", len);
        if (lane < laneTime)
        {
            m.addSeparator();
            m.addItem (5, "Load sample...");
            m.addItem (6, "Clear sample");
        }

        juce::Component::SafePointer<PatternPanel> safe (this);
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [safe, lane] (int r)
        {
            if (safe == nullptr || r == 0)
                return;
            auto& c = safe->ctx;
            if (r == 2)
            {
                c.laneClip = c.pattern().lanes[(size_t) lane];
                c.laneClipIsEffect = lane >= laneTime;
                c.hasLaneClip = true;
                return;
            }
            if (r == 5)
            {
                if (safe->onLoadSample)
                    safe->onLoadSample (lane);
                return;
            }
            if (r == 6)
            {
                c.processor.assignSample (lane, {});
                return;
            }
            c.beginEdit();
            const auto clip = c.laneClip;
            c.edit ([r, lane, clip] (Pattern& p)
            {
                auto& l = p.lanes[(size_t) lane];
                if (r == 1)        PatternTools::clearLane (l);
                else if (r == 3)   l = clip;
                else if (r == 4)   l.mute ^= 1;
                else if (r >= 100) l.length = (uint8_t) (r - 100);
            });
        });
    }

    void PatternPanel::patternMenu (int) {}
}
