#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "EditorContext.h"
#include "Widgets.h"

namespace labx0::ui
{
    // RENDER // ЗАПИСЬ: the pattern being edited as WAV files, as heard (and a MIDI file of its hits), made in the
    // background by a second engine at the host's tempo. Drag a file - or all of them - out of the plugin onto the
    // host's playlist; stems are as long as the mix, so dropped at one bar they line up.
    class RenderPanel final : public juce::Component, private juce::Timer
    {
    public:
        explicit RenderPanel (EditorContext& context) : ctx (context)
        {
            whatBox.addItem ("MIX", 1);
            whatBox.addItem ("MIX + EVERY LANE (STEMS)", 2);
            whatBox.addItem ("SELECTED LANE", 3);
            whatBox.setSelectedId (1, juce::dontSendNotification);
            whatBox.setTooltip ("Stems: every lane with steps on its own, through ANOMALY and PUNCH, as heard");
            for (const int passes : { 1, 2, 4, 8 })
                passesBox.addItem (juce::String (passes) + (passes == 1 ? " PASS" : " PASSES"), passes);
            passesBox.setSelectedId (1, juce::dontSendNotification);
            passesBox.setTooltip ("How many times the pattern plays; the tails are added after the last pass");
            styleButton (renderButton, "Render the pattern being edited at the host's tempo");
            styleButton (folderButton, "Show the files in Explorer");
            styleButton (closeButton, "Close this panel (renders keep going)");
            renderButton.onClick = [this] { startRender(); };
            folderButton.onClick = [this] { if (folder.isDirectory()) folder.revealToUser(); };
            closeButton.onClick = [this] { setVisible (false); };
            status.setFont (monoFont (10.5f));
            status.setColour (juce::Label::textColourId, Palette::phosphor);
            status.setJustificationType (juce::Justification::centredLeft);
            midiStatus.setFont (monoFont (9.5f));
            midiStatus.setColour (juce::Label::textColourId, Palette::dim);
            midiStatus.setJustificationType (juce::Justification::centredLeft);
            for (auto* c : std::initializer_list<juce::Component*> { &whatBox, &passesBox, &renderButton, &folderButton, &closeButton, &status,
                                                                      &midiStatus, &dragAll, &viewport })
                addAndMakeVisible (c);
            viewport.setViewedComponent (&list, false);
            viewport.setScrollBarsShown (true, false);
            dragAll.panel = this;
        }

        ~RenderPanel() override { stopTimer(); }

        void visibilityChanged() override
        {
            if (isVisible())
            {
                seenVersion = -1;
                startTimerHz (10);
                timerCallback();
            }
            else
                stopTimer();
        }

        void paint (juce::Graphics& g) override
        {
            auto b = getLocalBounds().toFloat();
            g.setColour (Palette::background.withAlpha (0.97f));
            g.fillRect (b);
            g.setColour (Palette::red);
            g.drawRect (b, 1.5f);
            g.setFont (monoFont (12.0f, true));
            g.setColour (Palette::bright);
            const int pattern = ctx.processor.getEditPattern() + 1;
            const float bpm = ctx.processor.meters.bpm.load();
            g.drawText (utf8 ("RENDER  //  ЗАПИСЬ    PATTERN ") + juce::String (pattern).paddedLeft ('0', 2) + "  AT "
                            + juce::String (bpm > 1.0f ? bpm : 120.0f, 1) + " BPM",
                        getLocalBounds().reduced (12, 8).removeFromTop (18), juce::Justification::centredLeft);
            g.setFont (monoFont (9.5f));
            g.setColour (Palette::dim);
            g.drawText ("DRAG A FILE ONTO THE PLAYLIST. STEMS ARE AS LONG AS THE MIX: DROP THEM AT ONE BAR.",
                        getLocalBounds().reduced (12, 6).removeFromBottom (14), juce::Justification::centredLeft);
        }

        void resized() override
        {
            auto b = getLocalBounds().reduced (12, 8);
            b.removeFromTop (22);
            auto row = b.removeFromTop (26);
            whatBox.setBounds (row.removeFromLeft (row.getWidth() / 2).reduced (2, 1));
            renderButton.setBounds (row.removeFromRight (100).reduced (2, 1));
            passesBox.setBounds (row.reduced (2, 1));
            b.removeFromTop (4);
            status.setBounds (b.removeFromTop (20));
            midiStatus.setBounds (b.removeFromTop (16));
            b.removeFromBottom (16);
            auto bottom = b.removeFromBottom (26);
            closeButton.setBounds (bottom.removeFromRight (90).reduced (2, 1));
            folderButton.setBounds (bottom.removeFromRight (120).reduced (2, 1));
            dragAll.setBounds (bottom.reduced (2, 1));
            b.removeFromBottom (4);
            viewport.setBounds (b);
            layoutRows();
        }

    private:
        // One rendered file. Dragging it hands the file to the host (or Explorer).
        class FileRow final : public juce::Component, public juce::SettableTooltipClient
        {
        public:
            explicit FileRow (const juce::File& f) : file (f) { setTooltip ("Drag onto the host: " + file.getFullPathName()); }

            void paint (juce::Graphics& g) override
            {
                const auto b = getLocalBounds().toFloat().reduced (1.0f);
                g.setColour (isMouseOver() ? Palette::faint.brighter (0.3f) : Palette::faint);
                g.fillRect (b);
                g.setFont (monoFont (10.5f, true));
                g.setColour (file.hasFileExtension ("mid") ? Palette::timeTint : Palette::bright);
                g.drawText (file.getFileName(), b.reduced (8.0f, 0.0f), juce::Justification::centredLeft, true);
                g.setColour (Palette::dim);
                g.drawText (utf8 ("DRAG ⇢"), b.reduced (8.0f, 0.0f), juce::Justification::centredRight, false);
            }

            void mouseEnter (const juce::MouseEvent&) override { repaint(); }
            void mouseExit (const juce::MouseEvent&) override  { repaint(); }
            void mouseDown (const juce::MouseEvent&) override  { dragging = false; }
            void mouseDrag (const juce::MouseEvent& e) override
            {
                if (dragging || e.getDistanceFromDragStart() < 6 || ! file.existsAsFile())
                    return;
                dragging = true;
                juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this);
            }

            juce::File file;

        private:
            bool dragging = false;
        };

        // Drags every rendered file at once.
        class DragAll final : public juce::Component, public juce::SettableTooltipClient
        {
        public:
            DragAll() { setTooltip ("Drag all the files onto the host at once"); }

            void paint (juce::Graphics& g) override
            {
                const auto b = getLocalBounds().toFloat().reduced (1.0f);
                const bool ready = panel != nullptr && ! panel->files.isEmpty();
                g.setColour (ready ? Palette::red.withAlpha (0.25f) : Palette::faint);
                g.fillRect (b);
                g.setColour (ready ? Palette::red : Palette::dim);
                g.drawRect (b, 1.0f);
                g.setFont (monoFont (10.5f, true));
                g.drawText (utf8 ("⇢ DRAG ALL"), b, juce::Justification::centred, false);
            }

            void mouseDown (const juce::MouseEvent&) override { dragging = false; }
            void mouseDrag (const juce::MouseEvent& e) override
            {
                if (dragging || panel == nullptr || panel->files.isEmpty() || e.getDistanceFromDragStart() < 6)
                    return;
                dragging = true;
                juce::StringArray paths;
                for (const auto& f : panel->files)
                    if (f.existsAsFile())
                        paths.add (f.getFullPathName());
                juce::DragAndDropContainer::performExternalDragDropOfFiles (paths, false, this);
            }

            RenderPanel* panel = nullptr;

        private:
            bool dragging = false;
        };

        void startRender()
        {
            LabX0AudioProcessor::RenderRequest request;
            request.pattern = ctx.processor.getEditPattern();
            request.passes = std::max (1, passesBox.getSelectedId());
            request.what = whatBox.getSelectedId() - 1;
            request.lane = std::min (ctx.selectedLane, laneTime - 1);
            if (! ctx.processor.startRender (request))
                status.setText ("A RENDER IS ALREADY RUNNING", juce::dontSendNotification);
            timerCallback();
        }

        void timerCallback() override
        {
            const auto s = ctx.processor.getRenderStatus();
            renderButton.setEnabled (! s.running);
            auto text = s.text;
            if (s.running)
                text << "  " << juce::String ((int) std::lround (s.progress * 100.0f)) << " %";
            if (status.getText() != text)
                status.setText (text, juce::dontSendNotification);
            const auto midiText = s.running ? juce::String() : s.midi;
            if (midiStatus.getText() != midiText)
                midiStatus.setText (midiText, juce::dontSendNotification);
            const int v = ctx.processor.getRenderVersion();
            if (v != seenVersion)
            {
                seenVersion = v;
                files = s.files;
                folder = s.folder;
                rows.clear();
                for (const auto& f : files)
                    list.addAndMakeVisible (rows.add (new FileRow (f)));
                layoutRows();
                dragAll.repaint();
            }
            repaint (getLocalBounds().removeFromTop (30));
        }

        void layoutRows()
        {
            const int rowHeight = 22;
            list.setBounds (0, 0, std::max (1, viewport.getWidth() - viewport.getScrollBarThickness()), std::max (1, rows.size() * rowHeight));
            for (int i = 0; i < rows.size(); ++i)
                rows[i]->setBounds (0, i * rowHeight, list.getWidth(), rowHeight);
        }

        EditorContext& ctx;
        juce::ComboBox whatBox, passesBox;
        juce::TextButton renderButton { "RENDER" }, folderButton { "OPEN FOLDER" }, closeButton { "CLOSE" };
        juce::Label status, midiStatus;
        juce::Viewport viewport;
        juce::Component list;
        juce::OwnedArray<FileRow> rows;
        DragAll dragAll;
        juce::Array<juce::File> files;
        juce::File folder;
        int seenVersion = -1;
    };
}
