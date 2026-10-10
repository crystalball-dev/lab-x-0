#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "EditorContext.h"
#include "Widgets.h"

namespace labx0::ui
{
    // The lower-left panel follows the selected lane: PAD controls for a pad, BREAK controls for a break slot,
    // the slot palette for the TIME and VOLUME lanes. One set of knobs is rebound to whichever pad or slot is
    // selected.
    class LanePanel final : public juce::Component, private juce::ChangeListener
    {
    public:
        explicit LanePanel (EditorContext& context) : ctx (context)
        {
            auto& state = ctx.processor.apvts;
            const char* padCaptions[] = { "LEVEL", "PAN", "TUNE", "START", "ATTACK", "HOLD", "DECAY", "TONE", "RESO", "DRIVE", "VEL" };
            const int padFields[] = { P::padLevel, P::padPan, P::padTune, P::padStart, P::padAttack, P::padHold, P::padDecay,
                                      P::padTone, P::padReso, P::padDrive, P::padVel };
            for (int i = 0; i < 11; ++i)
            {
                padKnobs[(size_t) i] = std::make_unique<LabeledKnob> (state, padParamId (0, padFields[i]), padCaptions[i]);
                padFieldIds[(size_t) i] = padFields[i];
                addChildComponent (*padKnobs[(size_t) i]);
            }
            padStackA = std::make_unique<ChoiceStack> (state, padParamId (0, P::padReverse), "REVERSE", padParamId (0, P::padMode), "MODE");
            padStackB = std::make_unique<ChoiceStack> (state, padParamId (0, P::padChoke), "CHOKE", padParamId (0, P::padRoute), "ROUTE",
                                                       outParamId (0), "OUT");
            addChildComponent (*padStackA);
            addChildComponent (*padStackB);

            const char* breakCaptions[] = { "LEVEL", "PAN", "TUNE", "SENS", "DECAY", "TONE" };
            const int breakFields[] = { P::brLevel, P::brPan, P::brTune, P::brSens, P::brDecay, P::brTone };
            for (int i = 0; i < 6; ++i)
            {
                breakKnobs[(size_t) i] = std::make_unique<LabeledKnob> (state, breakParamId (0, breakFields[i]), breakCaptions[i]);
                breakFieldIds[(size_t) i] = breakFields[i];
                addChildComponent (*breakKnobs[(size_t) i]);
            }
            breakStackA = std::make_unique<ChoiceStack> (state, breakParamId (0, P::brFit), "FIT", breakParamId (0, P::brBeats), "BEATS");
            breakStackB = std::make_unique<ChoiceStack> (state, breakParamId (0, P::brSlicing), "SLICING", breakParamId (0, P::brGate), "GATE");
            breakStackC = std::make_unique<ChoiceStack> (state, breakParamId (0, P::brRoute), "ROUTE", outParamId (numPads), "OUT");
            addChildComponent (*breakStackA);
            addChildComponent (*breakStackB);
            addChildComponent (*breakStackC);
            styleButton (explodeButton, "Take the break apart into pads (kick, snare, hats...) and a pattern that plays it back on them");
            explodeButton.onClick = [this] { explodeMenu(); };
            addChildComponent (explodeButton);

            name.setFont (monoFont (12.0f, true));
            name.setColour (juce::Label::textColourId, Palette::bright);
            name.setMinimumHorizontalScale (0.6f);
            statusLabel.setFont (monoFont (9.5f));
            statusLabel.setJustificationType (juce::Justification::topLeft);
            statusLabel.setMinimumHorizontalScale (0.6f);
            styleButton (loadButton, "Load a sound file into this lane");
            styleButton (clearButton, "Empty this lane's sample slot");
            loadButton.onClick = [this] { if (onLoadSample) onLoadSample (ctx.selectedLane); };
            clearButton.onClick = [this] { if (ctx.selectedLane < laneTime) ctx.processor.assignSample (ctx.selectedLane, {}); };
            windowBox.setTooltip ("Long files: which part of the file to load (120 s at a time)");
            windowBox.onChange = [this]
            {
                const int lane = ctx.selectedLane;
                const int id = windowBox.getSelectedId();
                if (lane < laneFirstBreak || lane >= laneTime || id <= 0)
                    return;
                const double offset = 60.0 * (id - 1);
                if (std::abs (offset - ctx.processor.getBank().getOffset (lane)) > 0.5)
                    ctx.processor.assignSample (lane, ctx.processor.getSampleRef (lane), offset);
            };
            for (auto* c : std::initializer_list<juce::Component*> { &name, &statusLabel, &loadButton, &clearButton })
                addAndMakeVisible (c);
            addChildComponent (windowBox);

            for (int k = 1; k < Slot::numTime; ++k)
            {
                auto b = std::make_unique<juce::TextButton> (timeSlots[k].code);
                styleButton (*b, timeSlots[k].name);
                b->onClick = [this, k] { ctx.paintTimeSlot = k; ctx.sendChangeMessage(); };
                addChildComponent (*b);
                timeButtons.push_back (std::move (b));
            }
            for (int k = 1; k < Slot::numVolume; ++k)
            {
                auto b = std::make_unique<juce::TextButton> (volumeSlots[k].code);
                styleButton (*b, volumeSlots[k].name);
                b->onClick = [this, k] { ctx.paintVolumeSlot = k; ctx.sendChangeMessage(); };
                addChildComponent (*b);
                volumeButtons.push_back (std::move (b));
            }
            slotInfo.setFont (monoFont (10.5f));
            slotInfo.setColour (juce::Label::textColourId, Palette::dim);
            slotInfo.setJustificationType (juce::Justification::topLeft);
            addChildComponent (slotInfo);

            ctx.addChangeListener (this);
            rebind();
        }

        ~LanePanel() override { ctx.removeChangeListener (this); }

        std::function<void (int lane)> onLoadSample;

        void setUnitWidth (float u) { unit = u; resized(); }
        static constexpr float units = 16.3f;

        juce::String title() const
        {
            const int lane = ctx.selectedLane;
            if (lane < numPads)  return utf8 ("PAD ") + juce::String (lane + 1).paddedLeft ('0', 2) + utf8 ("  //  АРСЕНАЛ");
            if (lane < laneTime) return utf8 ("BREAK ") + breakLetter (lane - laneFirstBreak) + utf8 ("  //  АРТЕФАКТЫ");
            if (lane == laneTime) return utf8 ("TIME LANE  //  АНОМАЛИИ");
            return utf8 ("VOLUME LANE  //  ЭЛЕКТРА");
        }

        void refresh()
        {
            const int lane = ctx.selectedLane;
            if (lane >= laneTime)
                return;
            auto& bank = ctx.processor.getBank();
            const auto ref = ctx.processor.getSampleRef (lane);
            const auto n = ref.isEmpty() ? juce::String ("EMPTY") : ref.displayName().toUpperCase();
            if (name.getText() != n)
                name.setText (n, juce::dontSendNotification);
            const auto text = bank.getStatusText (lane) + (ref.isEmpty() ? juce::String() : "\n" + ref.toString());
            const bool showingNotice = juce::Time::getMillisecondCounter() < noticeUntil;
            const auto shown = showingNotice ? notice : text;
            if (statusLabel.getText() != shown)
            {
                statusLabel.setText (shown, juce::dontSendNotification);
                statusLabel.setColour (juce::Label::textColourId, showingNotice || bank.getStatus (lane) == SampleBank::Status::ready
                                                                      ? Palette::phosphor : Palette::red);
            }
            if (lane >= laneFirstBreak)
            {
                const auto ov = bank.getOverview (lane);
                const int minutes = ov != nullptr ? (int) std::ceil (ov->fileSeconds / 60.0) : 0;
                const bool long_ = ov != nullptr && ov->fileSeconds > maxSampleSeconds;
                if (long_ != windowBox.isVisible() || windowBox.getNumItems() != std::max (0, minutes - 1))
                {
                    windowBox.clear (juce::dontSendNotification);
                    for (int m = 0; m + 1 < minutes; ++m)
                        windowBox.addItem ("FROM " + juce::String (m) + ":00", m + 1);
                    windowBox.setVisible (long_);
                }
                if (long_)
                    windowBox.setSelectedId ((int) std::lround (bank.getOffset (lane) / 60.0) + 1, juce::dontSendNotification);
            }
        }

        void resized() override
        {
            const int lane = ctx.selectedLane;
            auto b = getLocalBounds();
            if (lane >= laneTime)
            {
                const auto& buttons = lane == laneTime ? timeButtons : volumeButtons;
                const int perRow = lane == laneTime ? 6 : 4;
                auto grid = b.removeFromLeft ((int) (unit * 7.5f)).reduced (2, 4);
                const int bw = grid.getWidth() / perRow, bh = std::min (28, grid.getHeight() / 2);
                for (size_t k = 0; k < buttons.size(); ++k)
                    buttons[k]->setBounds (grid.getX() + (int) (k % (size_t) perRow) * bw, grid.getY() + (int) (k / (size_t) perRow) * (bh + 4),
                                           bw - 3, bh);
                slotInfo.setBounds (b.reduced (8, 4));
                return;
            }

            // Source block: name, status, LOAD / CLEAR (and the window for long breaks)
            auto source = b.removeFromLeft ((int) (unit * 2.6f)).reduced (3, 0);
            name.setBounds (source.removeFromTop (16));
            auto buttons = source.removeFromTop (20);
            loadButton.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2).reduced (1, 1));
            clearButton.setBounds (buttons.reduced (1, 1));
            if (windowBox.isVisible())
                windowBox.setBounds (source.removeFromBottom (20).reduced (1, 1));
            source.removeFromTop (2);
            statusLabel.setBounds (source);

            if (lane < numPads)
            {
                for (auto& k : padKnobs)
                    k->setBounds (b.removeFromLeft ((int) unit));
                padStackA->setBounds (b.removeFromLeft ((int) (unit * 1.35f)));
                padStackB->setBounds (b.removeFromLeft ((int) (unit * 1.35f)));
            }
            else
            {
                for (auto& k : breakKnobs)
                    k->setBounds (b.removeFromLeft ((int) unit));
                breakStackA->setBounds (b.removeFromLeft ((int) (unit * 1.35f)));
                breakStackB->setBounds (b.removeFromLeft ((int) (unit * 1.35f)));
                breakStackC->setBounds (b.removeFromLeft ((int) (unit * 1.35f)));
                explodeButton.setBounds (b.removeFromLeft ((int) (unit * 1.4f)).reduced (6, 0).withSizeKeepingCentre ((int) (unit * 1.4f) - 12, 26));
            }
        }

    private:
        void changeListenerCallback (juce::ChangeBroadcaster*) override { rebind(); }

        void showNotice (const juce::String& text, int milliseconds = 8000)
        {
            notice = text;
            noticeUntil = juce::Time::getMillisecondCounter() + (juce::uint32) milliseconds;
            refresh();
        }

        // EXPLODE: the break's components to pads and a pattern that plays it back on them.
        void explodeMenu()
        {
            const int lane = ctx.selectedLane;
            if (lane < laneFirstBreak || lane >= laneTime)
                return;
            const int b = lane - laneFirstBreak;
            juce::PopupMenu menu;
            menu.addSectionHeader ("EXPLODE BREAK " + breakLetter (b) + " INTO PADS + A NEW PATTERN");
            menu.addItem (1, "By sound (alike hits share a pad): pads 1-16");
            menu.addItem (2, "By sound: pads 9-16");
            menu.addItem (3, "Every slice its own pad: pads 1-16");
            menu.addItem (4, "Every slice its own pad: pads 9-16");
            juce::Component::SafePointer<LanePanel> safe (this);
            menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&explodeButton), [safe, b] (int result)
            {
                if (safe == nullptr || result == 0)
                    return;
                const int first = (result == 2 || result == 4) ? 8 : 0;
                const bool everySlice = result >= 3;
                auto run = [safe, b, first, everySlice]
                {
                    if (safe == nullptr)
                        return;
                    safe->showNotice ("EXPLODING BREAK " + breakLetter (b) + "...", 60000);
                    safe->ctx.processor.explodeBreak (b, first, everySlice, [safe] (const juce::String& text)
                    {
                        if (safe == nullptr)
                            return;
                        safe->showNotice ("EXPLODED: " + text);
                        safe->ctx.sendChangeMessage();
                    });
                };
                bool used = false;
                for (int p = first; p < numPads; ++p)
                    used = used || ! safe->ctx.processor.getSampleRef (p).isEmpty();
                if (! used)
                {
                    run();
                    return;
                }
                juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "EXPLODE BREAK " + breakLetter (b),
                                                    "Pads " + juce::String (first + 1) + "-16 hold samples. The pads the break needs "
                                                    "will be replaced; the pattern goes into the first empty pattern slot.",
                                                    "EXPLODE", "CANCEL", safe.getComponent(),
                                                    juce::ModalCallbackFunction::create ([run] (int ok) { if (ok == 1) run(); }));
            });
        }

        void rebind()
        {
            const int lane = ctx.selectedLane;
            const bool pad = lane < numPads, brk = lane >= laneFirstBreak && lane < laneTime;
            for (int i = 0; i < 11; ++i)
            {
                if (pad)
                    padKnobs[(size_t) i]->bind (padParamId (lane, padFieldIds[(size_t) i]));
                padKnobs[(size_t) i]->setVisible (pad);
            }
            if (pad)
            {
                padStackA->bind (padParamId (lane, P::padReverse), padParamId (lane, P::padMode));
                padStackB->bind (padParamId (lane, P::padChoke), padParamId (lane, P::padRoute), outParamId (lane));
            }
            padStackA->setVisible (pad);
            padStackB->setVisible (pad);

            const int b = lane - laneFirstBreak;
            for (int i = 0; i < 6; ++i)
            {
                if (brk)
                    breakKnobs[(size_t) i]->bind (breakParamId (b, breakFieldIds[(size_t) i]));
                breakKnobs[(size_t) i]->setVisible (brk);
            }
            if (brk)
            {
                breakStackA->bind (breakParamId (b, P::brFit), breakParamId (b, P::brBeats));
                breakStackB->bind (breakParamId (b, P::brSlicing), breakParamId (b, P::brGate));
                breakStackC->bind (breakParamId (b, P::brRoute), outParamId (numPads + b));
            }
            breakStackA->setVisible (brk);
            breakStackB->setVisible (brk);
            breakStackC->setVisible (brk);
            explodeButton.setVisible (brk);
            if (! brk)
                windowBox.setVisible (false);

            const bool source = lane < laneTime;
            for (auto* c : std::initializer_list<juce::Component*> { &name, &statusLabel, &loadButton, &clearButton })
                c->setVisible (source);
            for (size_t k = 0; k < timeButtons.size(); ++k)
            {
                timeButtons[k]->setVisible (lane == laneTime);
                timeButtons[k]->setToggleState ((int) k + 1 == ctx.paintTimeSlot, juce::dontSendNotification);
            }
            for (size_t k = 0; k < volumeButtons.size(); ++k)
            {
                volumeButtons[k]->setVisible (lane == laneVolume);
                volumeButtons[k]->setToggleState ((int) k + 1 == ctx.paintVolumeSlot, juce::dontSendNotification);
            }
            slotInfo.setVisible (lane >= laneTime);
            if (lane == laneTime)
                slotInfo.setText ("PAINT: " + juce::String (timeSlots[ctx.paintTimeSlot].name)
                                  + "\nA run of the same slot is one region: repeats play its first pass live, then replay it."
                                  + "\nClick a painted step with the same slot to erase it; right click erases.", juce::dontSendNotification);
            else if (lane == laneVolume)
                slotInfo.setText ("PAINT: " + juce::String (volumeSlots[ctx.paintVolumeSlot].name)
                                  + "\nGates, fades, ducks and mutes on the FULL bus, region by region.", juce::dontSendNotification);
            refresh();
            resized();
            if (auto* parent = getParentComponent())
                parent->repaint();
        }

        EditorContext& ctx;
        float unit = 60.0f;
        std::array<std::unique_ptr<LabeledKnob>, 11> padKnobs;
        std::array<int, 11> padFieldIds {};
        std::unique_ptr<ChoiceStack> padStackA, padStackB;
        std::array<std::unique_ptr<LabeledKnob>, 6> breakKnobs;
        std::array<int, 6> breakFieldIds {};
        std::unique_ptr<ChoiceStack> breakStackA, breakStackB, breakStackC;
        juce::Label name, statusLabel, slotInfo;
        juce::TextButton loadButton { "LOAD" }, clearButton { "CLEAR" };
        juce::ComboBox windowBox;
        juce::TextButton explodeButton { "EXPLODE" };
        juce::String notice;
        juce::uint32 noticeUntil = 0;
        std::vector<std::unique_ptr<juce::TextButton>> timeButtons, volumeButtons;
    };
}
