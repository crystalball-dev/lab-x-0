#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "EditorContext.h"
#include "Widgets.h"

namespace labx0::ui
{
    // The step grid: 16 pad lanes, 4 break lanes, the TIME lane and the VOLUME lane, 32 steps per page.
    //   click        toggle a step (shift: accent 127, alt: ghost 50); drag paints
    //   right click  step menu: velocity, ratchet, probability, nudge, pitch, slice, clear
    //   wheel        velocity on pad lanes, slice on break lanes, slot on TIME / VOLUME
    //   lane header  click selects and auditions; right click lane menu; double click loads a sample;
    //                drop files from Explorer or rows from the browser to load them
    class PatternGrid final : public juce::Component,
                              public juce::FileDragAndDropTarget,
                              public juce::DragAndDropTarget,
                              private juce::ChangeListener
    {
    public:
        explicit PatternGrid (EditorContext& context);
        ~PatternGrid() override;

        void refresh();
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

        bool isInterestedInFileDrag (const juce::StringArray& files) override;
        void fileDragMove (const juce::StringArray&, int x, int y) override;
        void fileDragExit (const juce::StringArray&) override;
        void filesDropped (const juce::StringArray& files, int x, int y) override;
        bool isInterestedInDragSource (const SourceDetails&) override;
        void itemDragMove (const SourceDetails&) override;
        void itemDragExit (const SourceDetails&) override;
        void itemDropped (const SourceDetails&) override;

        std::function<void (int lane)> onLoadSample;
        std::function<void (int lane, juce::Point<int> where)> onLaneMenu;

        static constexpr int headerWidth = 112;
        static constexpr int topRow = 14;

        int visibleSteps() const noexcept;
        int firstStep() const noexcept { return ctx.page * 32; }
        int laneAt (int y) const noexcept;
        int stepAt (int x) const noexcept;
        juce::Rectangle<float> cellBounds (int lane, int step) const noexcept;
        float rowHeight() const noexcept { return (float) (getHeight() - topRow) / (float) numLanes; }

    private:
        void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); repaint(); }
        void applyCell (int lane, int step, const juce::MouseEvent& e, bool first);
        void showStepMenu (int lane, int step);
        int naturalSliceCount (int lane) const;
        juce::String laneLabel (int lane) const;

        EditorContext& ctx;
        Pattern pat;
        uint32_t seenVersion = 0;
        int seenEditPattern = -1, seenBankVersion = -1;
        std::array<float, numLanes> flash {};
        std::array<int, numLanes> playStep {};
        juce::StringArray names;
        int dragMode = 0, dropLane = -1;   // 1 paint on, 2 paint off, 3 paint slot, 4 erase slot
        int lastLane = -1, lastStep = -1;
        uint8_t dragVelocity = 100;
    };

    // Per-step values of the selected lane, drawn as bars under the grid and edited by dragging.
    class StepStrip final : public juce::Component, private juce::ChangeListener
    {
    public:
        StepStrip (EditorContext& context, PatternGrid& grid);
        ~StepStrip() override;
        void refresh();
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;

    private:
        void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
        void setFrom (const juce::MouseEvent&);
        int valueRange (int& lo, int& hi) const;
        static int valueOf (const Step& s, int mode);
        static void setValue (Step& s, int mode, int v);

        EditorContext& ctx;
        PatternGrid& grid;
        Pattern pat;
        uint32_t seenVersion = 0;
    };

    // The selected lane's sample: waveform, slices and beat grid for breaks, the playing position, the status.
    // Clicking a break slice auditions it.
    class WaveView final : public juce::Component, private juce::ChangeListener
    {
    public:
        explicit WaveView (EditorContext& context);
        ~WaveView() override;
        void refresh();
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;

    private:
        void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }

        EditorContext& ctx;
        std::shared_ptr<const SampleBank::Overview> overview;
        std::vector<int> slices;
        double beats = 0.0, position = -1.0;
        juce::String status;
        int lastLane = -1;
        uint64_t lastId = 0;
        juce::String sliceKey;
    };

    // PATTERN // ВЫБРОС: pattern buttons, length, resolution, page, PLAY, lane tools, grid, strip and waveform.
    class PatternPanel final : public juce::Component, private juce::ChangeListener
    {
    public:
        explicit PatternPanel (EditorContext& context);
        ~PatternPanel() override;

        void refresh();
        void resized() override;
        void paint (juce::Graphics&) override;

        PatternGrid grid;
        std::function<void (int lane)> onLoadSample;

    private:
        void changeListenerCallback (juce::ChangeBroadcaster*) override { updateButtons(); }
        void updateButtons();
        void laneMenu (int lane, juce::Point<int> where);
        void patternMenu (int index);
        int naturalSlice (int breakSlot, int step, const Pattern& p) const;
        void breakTool (int which);

        EditorContext& ctx;
        StepStrip strip;
        WaveView wave;

        std::array<juce::TextButton, numPatterns> patternButtons;
        juce::ComboBox lengthBox, resolutionBox;
        juce::TextButton pageA { "1-32" }, pageB { "33-64" }, playButton { "PLAY" };
        juce::TextButton undoButton { "UNDO" }, copyButton { "COPY" }, pasteButton { "PASTE" }, clearButton { "CLEAR" };
        juce::TextButton euclidButton { "EUCLID" }, fillButton { "FILL" }, leftButton { "<" }, rightButton { ">" };
        juce::TextButton shuffleButton { "SHUFFLE" }, reverseButton { "REVERSE" }, rollButton { "ROLL" };
        juce::TextButton halfButton { "HALF" }, doubleButton { "DOUBLE" };
        std::array<juce::TextButton, EditorContext::numStripModes> modeButtons;
        juce::Label laneInfo;

        int shownPattern = -1, shownLength = -1, shownResolution = -1;
        uint32_t seenVersion = 0;
        uint32_t shuffleSeed = 1;
    };
}
