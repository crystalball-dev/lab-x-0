#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace labx0::ui
{
    // CRT phosphor in psy-field violet, the fourth terminal after LAB X-3's green, LAB X-10's amber and LAB X-19's
    // cyan. The warm accent (called red in code, as in the siblings) marks EMISSION, accents and warnings.
    struct Palette
    {
        static inline const juce::Colour background { 0xff07050b };
        static inline const juce::Colour panel      { 0xff100b18 };
        static inline const juce::Colour border     { 0xff35234f };
        static inline const juce::Colour faint      { 0xff1c1329 };
        static inline const juce::Colour dim        { 0xff8a6cc0 };
        static inline const juce::Colour phosphor   { 0xffc9a6ff };
        static inline const juce::Colour bright     { 0xffefe2ff };
        static inline const juce::Colour red        { 0xffff8a4d };
        static inline const juce::Colour knobTop    { 0xff22163a };
        static inline const juce::Colour knobBottom { 0xff07050b };
        static inline const juce::Colour breakTint  { 0xff6fd3c4 };   // break lanes: a second phosphor, for contrast
        static inline const juce::Colour timeTint   { 0xffffc46b };   // TIME lane
        static inline const juce::Colour volumeTint { 0xffff6b9a };   // VOLUME lane
    };

    inline juce::Font monoFont (float height, bool bold = false)
    {
        return juce::Font (juce::FontOptions ("Consolas", height, bold ? juce::Font::bold : juce::Font::plain));
    }

    inline juce::String utf8 (const char* text) { return juce::String::fromUTF8 (text); }

    class CrtLookAndFeel final : public juce::LookAndFeel_V4
    {
    public:
        CrtLookAndFeel()
        {
            using P = Palette;
            setColour (juce::ResizableWindow::backgroundColourId, P::background);
            setColour (juce::Label::textColourId, P::dim);
            setColour (juce::Slider::textBoxTextColourId, P::phosphor);
            setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
            setColour (juce::Slider::textBoxHighlightColourId, P::dim.withAlpha (0.5f));
            setColour (juce::ComboBox::backgroundColourId, P::panel);
            setColour (juce::ComboBox::textColourId, P::phosphor);
            setColour (juce::ComboBox::outlineColourId, P::border);
            setColour (juce::ComboBox::arrowColourId, P::phosphor);
            setColour (juce::PopupMenu::backgroundColourId, P::background);
            setColour (juce::PopupMenu::textColourId, P::phosphor);
            setColour (juce::PopupMenu::headerTextColourId, P::red);
            setColour (juce::PopupMenu::highlightedBackgroundColourId, P::dim);
            setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::black);
            setColour (juce::TextButton::buttonColourId, P::panel);
            setColour (juce::TextButton::buttonOnColourId, P::dim);
            setColour (juce::TextButton::textColourOffId, P::phosphor);
            setColour (juce::TextButton::textColourOnId, P::bright);
            setColour (juce::TextEditor::backgroundColourId, P::background);
            setColour (juce::TextEditor::textColourId, P::bright);
            setColour (juce::TextEditor::highlightColourId, P::dim.withAlpha (0.5f));
            setColour (juce::TextEditor::outlineColourId, P::border);
            setColour (juce::TextEditor::focusedOutlineColourId, P::dim);
            setColour (juce::CaretComponent::caretColourId, P::phosphor);
            setColour (juce::ListBox::backgroundColourId, P::background);
            setColour (juce::ListBox::outlineColourId, P::border);
            setColour (juce::ScrollBar::thumbColourId, P::dim.withAlpha (0.6f));
            setColour (juce::ScrollBar::trackColourId, P::faint);
            setColour (juce::TooltipWindow::backgroundColourId, P::background);
            setColour (juce::TooltipWindow::textColourId, P::phosphor);
            setColour (juce::TooltipWindow::outlineColourId, P::border);
        }

        void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float pos,
                               float startAngle, float endAngle, juce::Slider& slider) override
        {
            const bool accent = slider.getProperties().contains ("accent");
            const bool bipolar = slider.getProperties().contains ("bipolar");
            const auto colour = accent ? Palette::red : Palette::phosphor;

            const auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (3.0f);
            const float radius = std::min (bounds.getWidth(), bounds.getHeight()) * 0.5f;
            const auto centre = bounds.getCentre();
            const float angle = startAngle + pos * (endAngle - startAngle);
            const float lineW = std::max (2.0f, radius * 0.1f);
            const float arcR = radius - lineW;

            const juce::PathStrokeType stroke (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

            juce::Path track;
            track.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, startAngle, endAngle, true);
            g.setColour (Palette::faint);
            g.strokePath (track, stroke);

            const float from = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;
            if (std::abs (angle - from) > 0.001f)
            {
                juce::Path value;
                value.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, std::min (from, angle), std::max (from, angle), true);
                g.setColour (colour.withAlpha (0.16f));
                g.strokePath (value, juce::PathStrokeType (lineW * 3.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                g.setColour (colour);
                g.strokePath (value, stroke);
            }

            const float bodyR = arcR - lineW * 1.7f;
            if (bodyR > 2.0f)
            {
                juce::ColourGradient grad (Palette::knobTop, centre.x, centre.y - bodyR, Palette::knobBottom, centre.x, centre.y + bodyR, false);
                g.setGradientFill (grad);
                g.fillEllipse (centre.x - bodyR, centre.y - bodyR, bodyR * 2.0f, bodyR * 2.0f);
                g.setColour (Palette::border);
                g.drawEllipse (centre.x - bodyR, centre.y - bodyR, bodyR * 2.0f, bodyR * 2.0f, 1.0f);

                juce::Path pointer;
                pointer.addRoundedRectangle (-lineW * 0.4f, -bodyR + 1.0f, lineW * 0.8f, bodyR * 0.6f, lineW * 0.3f);
                pointer.applyTransform (juce::AffineTransform::rotation (angle).translated (centre.x, centre.y));
                g.setColour (colour);
                g.fillPath (pointer);
            }
        }

        void drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box) override
        {
            const auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.5f);
            g.setColour (Palette::panel);
            g.fillRoundedRectangle (r, 3.0f);
            g.setColour (box.hasKeyboardFocus (true) ? Palette::dim : Palette::border);
            g.drawRoundedRectangle (r, 3.0f, 1.0f);

            juce::Path arrow;
            const float ax = (float) width - 9.0f, ay = (float) height * 0.5f;
            arrow.addTriangle (ax - 3.5f, ay - 2.0f, ax + 3.5f, ay - 2.0f, ax, ay + 3.0f);
            g.setColour (Palette::phosphor);
            g.fillPath (arrow);
        }

        void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
        {
            label.setBounds (3, 1, box.getWidth() - 14, box.getHeight() - 2);
            label.setFont (getComboBoxFont (box));
            label.setMinimumHorizontalScale (0.6f);
        }

        void drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour&, bool highlighted, bool down) override
        {
            const auto r = button.getLocalBounds().toFloat().reduced (0.5f);
            const bool latched = button.getToggleState();
            const bool accent = button.getProperties().contains ("accent");
            const auto on = accent ? Palette::red : Palette::dim;
            auto fill = down ? on.withAlpha (0.45f) : (highlighted ? Palette::faint.brighter (0.25f) : Palette::panel);
            if (latched)
                fill = on.withAlpha (highlighted ? 0.5f : 0.36f);
            g.setColour (fill);
            g.fillRoundedRectangle (r, 3.0f);
            g.setColour (latched ? (accent ? Palette::red : Palette::phosphor).withAlpha (0.75f) : (highlighted ? Palette::dim : Palette::border));
            g.drawRoundedRectangle (r, 3.0f, 1.0f);
        }

        void drawButtonText (juce::Graphics& g, juce::TextButton& button, bool, bool) override
        {
            g.setFont (getTextButtonFont (button, button.getHeight()));
            const bool latched = button.getToggleState();
            g.setColour (button.findColour (latched ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId)
                             .withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.4f));
            g.drawFittedText (button.getButtonText(), button.getLocalBounds().reduced (2, 0), juce::Justification::centred, 1, 0.7f);
        }

        juce::Font getLabelFont (juce::Label&) override                       { return monoFont (11.0f); }
        juce::Font getComboBoxFont (juce::ComboBox&) override                 { return monoFont (11.5f); }
        juce::Font getPopupMenuFont() override                                { return monoFont (13.0f); }
        juce::Font getTextButtonFont (juce::TextButton&, int h) override      { return monoFont (std::min (10.5f, (float) h * 0.62f), true); }

        juce::Label* createSliderTextBox (juce::Slider& slider) override
        {
            auto* label = juce::LookAndFeel_V4::createSliderTextBox (slider);
            label->setFont (monoFont (10.5f));
            label->setMinimumHorizontalScale (0.7f);
            label->setColour (juce::Label::textColourId, slider.getProperties().contains ("accent") ? Palette::red : Palette::phosphor);
            label->setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
            label->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
            label->setColour (juce::TextEditor::backgroundColourId, Palette::background);
            label->setColour (juce::TextEditor::textColourId, Palette::phosphor);
            return label;
        }
    };
}
