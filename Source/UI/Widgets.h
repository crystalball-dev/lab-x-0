#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "CrtLookAndFeel.h"

namespace labx0::ui
{
    using APVTS = juce::AudioProcessorValueTreeState;

    // Rotary control with a caption above and its value below. bind() moves it to another parameter, so one set
    // of knobs serves whichever pad or break slot is selected.
    class LabeledKnob final : public juce::Component
    {
    public:
        LabeledKnob (APVTS& s, const juce::String& paramID, const juce::String& caption, bool accent = false)
            : state (s)
        {
            slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 64, 14);
            slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
            if (accent)
                slider.getProperties().set ("accent", true);

            label.setText (caption, juce::dontSendNotification);
            label.setJustificationType (juce::Justification::centred);
            label.setColour (juce::Label::textColourId, accent ? Palette::red : Palette::dim);
            label.setInterceptsMouseClicks (false, false);
            label.setMinimumHorizontalScale (0.6f);

            addAndMakeVisible (label);
            addAndMakeVisible (slider);
            bind (paramID);
        }

        void bind (const juce::String& paramID)
        {
            if (paramID == boundId)
                return;
            attachment.reset();
            boundId = paramID;
            slider.getProperties().remove ("bipolar");
            if (auto* param = state.getParameter (paramID))
            {
                const auto range = param->getNormalisableRange();
                if (range.start < 0.0f && range.end > 0.0f)
                    slider.getProperties().set ("bipolar", true);
                slider.setTooltip (param->getName (64));
                attachment = std::make_unique<APVTS::SliderAttachment> (state, paramID, slider);
                slider.setDoubleClickReturnValue (true, param->convertFrom0to1 (param->getDefaultValue()));
            }
            repaint();
        }

        void resized() override
        {
            auto b = getLocalBounds();
            label.setBounds (b.removeFromTop (13));
            slider.setBounds (b);
        }

        juce::Slider slider;
        juce::Label label;

    private:
        APVTS& state;
        juce::String boundId;
        std::unique_ptr<APVTS::SliderAttachment> attachment;
    };

    // Choice parameter as a combo box with a caption above.
    class LabeledChoice final : public juce::Component
    {
    public:
        LabeledChoice (APVTS& s, const juce::String& paramID, const juce::String& caption)
            : state (s)
        {
            label.setText (caption, juce::dontSendNotification);
            label.setJustificationType (juce::Justification::centredLeft);
            label.setInterceptsMouseClicks (false, false);
            addAndMakeVisible (label);
            addAndMakeVisible (box);
            bind (paramID);
        }

        void bind (const juce::String& paramID)
        {
            if (paramID == boundId)
                return;
            attachment.reset();
            boundId = paramID;
            box.clear (juce::dontSendNotification);
            if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (state.getParameter (paramID)))
            {
                box.addItemList (choice->choices, 1);
                box.setTooltip (choice->getName (64));
                attachment = std::make_unique<APVTS::ComboBoxAttachment> (state, paramID, box);
            }
        }

        void resized() override
        {
            auto b = getLocalBounds();
            label.setBounds (b.removeFromTop (13));
            b.removeFromTop (2);
            box.setBounds (b.removeFromTop (22));
        }

        static constexpr int naturalHeight = 37;

        juce::ComboBox box;
        juce::Label label;

    private:
        APVTS& state;
        juce::String boundId;
        std::unique_ptr<APVTS::ComboBoxAttachment> attachment;
    };

    // Up to two choices stacked in one column, so a panel with several switches stays narrow.
    class ChoiceStack final : public juce::Component
    {
    public:
        ChoiceStack (APVTS& state, const juce::String& idA, const juce::String& captionA,
                     const juce::String& idB = {}, const juce::String& captionB = {})
            : top (state, idA, captionA)
        {
            addAndMakeVisible (top);
            if (idB.isNotEmpty())
            {
                bottom = std::make_unique<LabeledChoice> (state, idB, captionB);
                addAndMakeVisible (*bottom);
            }
        }

        void bind (const juce::String& idA, const juce::String& idB = {})
        {
            top.bind (idA);
            if (bottom != nullptr && idB.isNotEmpty())
                bottom->bind (idB);
        }

        void resized() override
        {
            auto b = getLocalBounds().reduced (3, 0);
            const int h = LabeledChoice::naturalHeight;
            if (bottom == nullptr)
            {
                top.setBounds (b.withHeight (h).withY (b.getY() + 8));
                return;
            }
            const int spare = std::max (0, b.getHeight() - 2 * h);
            top.setBounds (b.removeFromTop (h + spare / 3).withHeight (h).translated (0, spare / 6));
            bottom->setBounds (b.withHeight (h).translated (0, spare / 6));
        }

    private:
        LabeledChoice top;
        std::unique_ptr<LabeledChoice> bottom;
    };

    // Phosphor oscilloscope with a rising-edge trigger and a short persistence trail.
    class ScopeView final : public juce::Component
    {
    public:
        static constexpr int points = 512;

        void setSamples (const float* source, int numSource)
        {
            int start = 0;
            const int searchEnd = std::max (0, numSource - points * 2);
            for (int i = 1; i < searchEnd; ++i)
                if (source[i - 1] <= 0.0f && source[i] > 0.0f)
                {
                    start = i;
                    break;
                }
            history[2] = history[1];
            history[1] = history[0];
            for (int i = 0; i < points; ++i)
            {
                const int idx = start + i * 2;
                history[0][(size_t) i] = idx < numSource ? source[idx] : 0.0f;
            }
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            const auto b = getLocalBounds().toFloat();
            g.setColour (Palette::background);
            g.fillRect (b);
            g.setColour (Palette::faint);
            for (int i = 1; i < 8; ++i)
                g.drawVerticalLine ((int) (b.getX() + b.getWidth() * (float) i / 8.0f), b.getY(), b.getBottom());
            for (int i = 1; i < 4; ++i)
                g.drawHorizontalLine ((int) (b.getY() + b.getHeight() * (float) i / 4.0f), b.getX(), b.getRight());

            const float alphas[3] = { 1.0f, 0.35f, 0.15f };
            for (int h = 2; h >= 0; --h)
            {
                juce::Path trace;
                for (int i = 0; i < points; ++i)
                {
                    const float x = b.getX() + b.getWidth() * (float) i / (float) (points - 1);
                    const float y = b.getCentreY() - std::clamp (history[(size_t) h][(size_t) i] * 1.4f, -1.0f, 1.0f) * b.getHeight() * 0.45f;
                    if (i == 0)
                        trace.startNewSubPath (x, y);
                    else
                        trace.lineTo (x, y);
                }
                if (h == 0)
                {
                    g.setColour (Palette::phosphor.withAlpha (0.18f));
                    g.strokePath (trace, juce::PathStrokeType (4.0f));
                }
                g.setColour (Palette::phosphor.withAlpha (alphas[h]));
                g.strokePath (trace, juce::PathStrokeType (1.3f));
            }
            g.setColour (Palette::border);
            g.drawRect (b, 1.0f);
        }

    private:
        std::array<std::array<float, points>, 3> history {};
    };

    // Scanlines and a vignette drawn over everything. Never takes mouse input.
    class ScanlineOverlay final : public juce::Component
    {
    public:
        ScanlineOverlay()
        {
            setInterceptsMouseClicks (false, false);
            pattern = juce::Image (juce::Image::ARGB, 4, 3, true);
            for (int x = 0; x < 4; ++x)
                pattern.setPixelAt (x, 2, juce::Colour (0x2a000000));
        }

        void paint (juce::Graphics& g) override
        {
            const auto b = getLocalBounds().toFloat();
            g.setFillType (juce::FillType (pattern, juce::AffineTransform()));
            g.fillRect (b);
            juce::ColourGradient vignette (juce::Colours::transparentBlack, b.getCentreX(), b.getCentreY(),
                                           juce::Colour (0x55000000), b.getX(), b.getY(), true);
            g.setGradientFill (vignette);
            g.fillRect (b);
        }

    private:
        juce::Image pattern;
    };

    // Small latching or momentary button used throughout the pattern and browser bars.
    inline void styleButton (juce::TextButton& b, const juce::String& tooltip, bool toggle = false)
    {
        b.setTooltip (tooltip);
        b.setClickingTogglesState (toggle);
        b.setWantsKeyboardFocus (false);
    }
}
