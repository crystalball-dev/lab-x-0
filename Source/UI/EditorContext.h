#pragma once

#include <deque>
#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"

namespace labx0::ui
{
    // What the editor's parts share: the selected lane, the visible page, the step strip's mode, the slots the
    // TIME and VOLUME lanes paint with, a clipboard and an undo history of whole patterns.
    class EditorContext : public juce::ChangeBroadcaster
    {
    public:
        explicit EditorContext (LabX0AudioProcessor& p) : processor (p)
        {
            selectedLane = juce::jlimit (0, numLanes - 1, p.uiSelectedLane.load());
            page = juce::jlimit (0, 1, p.uiPage.load());
            stripMode = juce::jlimit (0, 5, p.uiStripMode.load());
        }

        LabX0AudioProcessor& processor;

        enum StripMode { stripVelocity = 0, stripProbability, stripRatchet, stripNudge, stripPitch, stripSlice, numStripModes };

        int selectedLane = 0;
        int page = 0;
        int stripMode = stripVelocity;
        int paintTimeSlot = Slot::spring16;
        int paintVolumeSlot = Slot::gate16;

        int editPattern() const { return processor.getEditPattern(); }
        Pattern pattern() const { return processor.getPatterns().copyPattern (editPattern()); }

        void selectLane (int lane)
        {
            lane = juce::jlimit (0, numLanes - 1, lane);
            if (lane == selectedLane)
                return;
            selectedLane = lane;
            processor.uiSelectedLane.store (lane);
            if (lane >= laneFirstBreak && lane < laneTime && stripMode == stripSlice)
                ; // keep
            else if (lane < laneFirstBreak && stripMode == stripSlice)
                stripMode = stripVelocity;
            processor.uiStripMode.store (stripMode);
            sendChangeMessage();
        }

        void setPage (int p)
        {
            page = juce::jlimit (0, 1, p);
            processor.uiPage.store (page);
            sendChangeMessage();
        }

        void setStripMode (int m)
        {
            stripMode = juce::jlimit (0, numStripModes - 1, m);
            processor.uiStripMode.store (stripMode);
            sendChangeMessage();
        }

        // Every edit gesture snapshots the pattern first, so UNDO can step back through it.
        void beginEdit()
        {
            undo.push_back ({ editPattern(), pattern() });
            if (undo.size() > 64)
                undo.pop_front();
        }

        template <typename Fn>
        void edit (Fn&& fn)
        {
            processor.getPatterns().edit (editPattern(), std::forward<Fn> (fn));
        }

        bool canUndo() const { return ! undo.empty(); }

        void undoLast()
        {
            if (undo.empty())
                return;
            const auto snapshot = undo.back();
            undo.pop_back();
            processor.getPatterns().edit (snapshot.first, [&snapshot] (Pattern& p) { p = snapshot.second; });
            sendChangeMessage();
        }

        // Clipboard
        bool hasPatternClip = false, hasLaneClip = false;
        Pattern patternClip;
        Lane laneClip;
        bool laneClipIsEffect = false;

    private:
        std::deque<std::pair<int, Pattern>> undo;
    };
}
