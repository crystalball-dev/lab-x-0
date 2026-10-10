#include "PluginEditor.h"
#include "FactoryPresets.h"

using namespace labx0;
using namespace labx0::ui;

namespace
{
    constexpr int headerHeight = 58;
    constexpr int footerHeight = 24;
    constexpr int margin = 10;
    constexpr int browserWidth = 292;
    constexpr int sideWidth = 214;
    constexpr int gap = 8;
    constexpr int panelPad = 8;
    constexpr int titleHeight = 18;
    constexpr int rowAHeight = 534;
}

LabX0AudioProcessorEditor::LabX0AudioProcessorEditor (LabX0AudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p), context (p), browser (context), patternPanel (context), lanePanel (context)
{
    setLookAndFeel (&lookAndFeel);
    setWantsKeyboardFocus (true);

    addAndMakeVisible (browser);
    addAndMakeVisible (patternPanel);
    addAndMakeVisible (lanePanel);

    patternPanel.onLoadSample = [this] (int lane) { chooseSampleFor (lane); };
    lanePanel.onLoadSample = [this] (int lane) { chooseSampleFor (lane); };
    browser.onChooseFolder = [this] (int root) { chooseFolderFor (root); };

    // Row B1: the selected lane, the anomaly engine and the groove
    rows.push_back ({
        { lanePanel.title(), { { &lanePanel, LanePanel::units } } },
        { utf8 ("ANOMALY  //  АНОМАЛИЯ"), { { knob ("anomaly_mix", "MIX"), 1.0f }, { knob ("scratch_depth", "SCRATCH"), 1.0f } } },
        { "GROOVE", { { knob ("swing", "SWING"), 1.0f }, { choices ("run", "RUN", "slice_midi", "SLICE MIDI"), 1.35f } } },
    });

    // Row B2: the punch chain and the master section
    rows.push_back ({
        { "TRANSIENT", { { knob ("tr_attack", "ATTACK"), 1.0f }, { knob ("tr_sustain", "SUSTAIN"), 1.0f } } },
        { utf8 ("MULTIBAND  //  OTT"), { { knob ("mb_depth", "DEPTH"), 1.0f }, { knob ("mb_time", "TIME"), 1.0f },
                                         { knob ("mb_up", "UP"), 1.0f }, { knob ("mb_down", "DOWN"), 1.0f },
                                         { knob ("mb_low_xover", "LOW"), 1.0f }, { knob ("mb_high_xover", "HIGH"), 1.0f },
                                         { knob ("mb_low_gain", "L GAIN"), 1.0f }, { knob ("mb_mid_gain", "M GAIN"), 1.0f },
                                         { knob ("mb_high_gain", "H GAIN"), 1.0f } } },
        { utf8 ("SLAM  //  УДАР"), { { knob ("slam_bark", "BARK"), 1.0f }, { knob ("slam_bite", "BITE"), 1.0f },
                                     { knob ("slam_wet", "WET"), 1.0f }, { choices ("slam_sc", "BITE CUT"), 1.35f } } },
        { "CLIP", { { knob ("clip_drive", "DRIVE"), 1.0f }, { choices ("sub_safe", "SUB SAFE"), 1.35f } } },
        { "MASTER", { { knob ("master_volume", "VOLUME"), 1.0f }, { knob ("width", "WIDTH"), 1.0f },
                      { knob ("limit_ceiling", "CEILING"), 1.0f } } },
    });

    emissionKnob = knob ("emission", "EMISSION", true);

    sidePanels.push_back ({ utf8 ("EMISSION  //  ВЫБРОС"), { { &gauge, 1.0f } } });
    sidePanels.push_back ({ utf8 ("MACRO  //  ВЫБРОС"), { { emissionKnob, 1.0f } }, {}, true });
    sidePanels.push_back ({ utf8 ("DOSIMETER  //  ДОЗИМЕТР"), { { &dosimeter, 1.0f } } });
    sidePanels.push_back ({ "MONITOR", { { &scopeView, 1.0f } } });
    sidePanels.push_back ({ "SYSTEM", { { &systemLabel, 1.0f } } });

    addAndMakeVisible (gauge);
    addAndMakeVisible (dosimeter);
    addAndMakeVisible (scopeView);
    systemLabel.setFont (monoFont (10.5f));
    systemLabel.setJustificationType (juce::Justification::topLeft);
    systemLabel.setColour (juce::Label::textColourId, Palette::phosphor);
    addAndMakeVisible (systemLabel);

    const auto& presets = factoryPresets();
    for (int i = 0; i < (int) presets.size(); ++i)
        presetBox.addItem (juce::String (i + 1).paddedLeft ('0', 2) + "  " + presets[(size_t) i].name, i + 1);
    presetBox.setTooltip ("Factory kits with their patterns (loading one replaces the kit and all 16 patterns)");
    presetBox.onChange = [this]
    {
        const int index = presetBox.getSelectedId() - 1;
        if (index >= 0 && index != audioProcessor.getCurrentProgram())
            audioProcessor.setCurrentProgram (index);
        lastProgram = audioProcessor.getCurrentProgram();
    };
    prevButton.onClick = [this] { stepPreset (-1); };
    nextButton.onClick = [this] { stepPreset (1); };
    addAndMakeVisible (presetBox);
    addAndMakeVisible (prevButton);
    addAndMakeVisible (nextButton);
    labx0::ui::styleButton (renderButton, "Render the pattern being edited to WAV files to drag into the playlist");
    renderButton.onClick = [this]
    {
        renderPanel.setVisible (! renderPanel.isVisible());
        if (renderPanel.isVisible())
            renderPanel.toFront (false);
    };
    addAndMakeVisible (renderButton);
    addChildComponent (renderPanel);

    addAndMakeVisible (overlay);
    scopeBuffer.assign ((size_t) LabX0AudioProcessor::scopeSize, 0.0f);

    // Controls were built before they had a parent, so they captured the default theme's colours.
    sendLookAndFeelChange();

    setSize (editorWidth, editorHeight);
    refreshFromProcessor();
    startTimerHz (30);
}

LabX0AudioProcessorEditor::~LabX0AudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

LabeledKnob* LabX0AudioProcessorEditor::knob (const char* id, const char* caption, bool accent)
{
    knobs.push_back (std::make_unique<LabeledKnob> (audioProcessor.apvts, id, caption, accent));
    addAndMakeVisible (*knobs.back());
    return knobs.back().get();
}

ChoiceStack* LabX0AudioProcessorEditor::choices (const char* idA, const char* captionA, const char* idB, const char* captionB)
{
    stacks.push_back (std::make_unique<ChoiceStack> (audioProcessor.apvts, idA, captionA, idB, captionB));
    addAndMakeVisible (*stacks.back());
    return stacks.back().get();
}

//==============================================================================
void LabX0AudioProcessorEditor::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds();
    g.fillAll (Palette::background);

    g.setColour (Palette::faint.withAlpha (0.35f));
    for (int x = 0; x < bounds.getWidth(); x += 24)
        g.drawVerticalLine (x, 0.0f, (float) bounds.getHeight());
    for (int y = 0; y < bounds.getHeight(); y += 24)
        g.drawHorizontalLine (y, 0.0f, (float) bounds.getWidth());

    auto header = bounds.withHeight (headerHeight).reduced (margin + 6, 6);
    g.setFont (monoFont (30.0f, true));
    g.setColour (Palette::phosphor.withAlpha (0.18f));
    g.drawText ("LAB X-0", header.translated (0, 1).expanded (1, 0), juce::Justification::topLeft);
    g.setColour (Palette::phosphor);
    g.drawText ("LAB X-0", header, juce::Justification::topLeft);
    g.setFont (monoFont (11.0f));
    g.setColour (Palette::dim);
    g.drawText (utf8 ("ЛАБОРАТОРИЯ X-0  //  НУЛЕВАЯ ТОЧКА  //  BREAKS, CHOPS AND ORDNANCE"), header.withTrimmedTop (32), juce::Justification::topLeft);
    g.drawText (juce::String (vendorName) + "  //  v" + versionString, header.withTrimmedTop (32), juce::Justification::topRight);
    g.setColour (Palette::border);
    g.drawHorizontalLine (headerHeight - 2, (float) margin, (float) (bounds.getWidth() - margin));

    auto drawFrame = [&g] (juce::Rectangle<int> frame, const juce::String& title, bool accent)
    {
        const auto r = frame.toFloat();
        g.setColour (Palette::panel.withAlpha (0.92f));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (accent ? Palette::red.withAlpha (0.45f) : Palette::border);
        g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
        auto t = frame.withHeight (titleHeight).reduced (panelPad, 0);
        g.setFont (monoFont (10.5f, true));
        g.setColour (accent ? Palette::red : Palette::phosphor.withAlpha (0.85f));
        g.drawText (title, t.withTrimmedTop (3), juce::Justification::centredLeft);
        g.setColour (accent ? Palette::red.withAlpha (0.3f) : Palette::faint);
        g.drawHorizontalLine (frame.getY() + titleHeight, r.getX() + 4.0f, r.getRight() - 4.0f);
    };

    drawFrame (browserFrame, utf8 ("LIBRARY  //  СХРОН"), false);
    drawFrame (patternFrame, utf8 ("PATTERN  //  ВЫБРОС"), false);
    for (auto& row : rows)
        for (auto& panel : row)
            drawFrame (panel.bounds, panel.items.size() == 1 && panel.items[0].component == &lanePanel ? lanePanel.title() : panel.title, panel.accent);
    for (const auto& panel : sidePanels)
        drawFrame (panel.bounds, panel.title, panel.accent);

    auto footer = bounds.withTop (bounds.getHeight() - footerHeight).reduced (margin + 6, 4);
    g.setFont (monoFont (10.0f));
    g.setColour (Palette::dim);
    g.drawText (juce::String ("OPERATION FAIRWAY  //  AGPL-3.0  //  ") + projectUrl, footer, juce::Justification::centredLeft);
    g.drawText ("SAMPLES READ FROM YOUR OWN DISK  //  NO AUDIO INCLUDED", footer, juce::Justification::centredRight);
}

void LabX0AudioProcessorEditor::layoutRow (std::vector<Panel>& row, juce::Rectangle<int> area, float unitWidth)
{
    float naturalTotal = 0.0f;
    for (const auto& panel : row)
    {
        float units = 0.0f;
        for (const auto& item : panel.items)
            units += item.units;
        naturalTotal += units * unitWidth + (float) (2 * panelPad);
    }
    const float spare = std::max (0.0f, (float) area.getWidth() - naturalTotal - (float) (gap * ((int) row.size() - 1)));
    const float extraPerPanel = spare / (float) row.size();

    float x = (float) area.getX();
    for (auto& panel : row)
    {
        float units = 0.0f;
        for (const auto& item : panel.items)
            units += item.units;
        const float width = units * unitWidth + (float) (2 * panelPad) + extraPerPanel;
        panel.bounds = juce::Rectangle<int> ((int) std::round (x), area.getY(), (int) std::round (width), area.getHeight());

        auto content = panel.bounds.withTrimmedTop (titleHeight + 4).reduced (panelPad, 0).withTrimmedBottom (4);
        const float contentNatural = units * unitWidth;
        float cx = (float) content.getX() + ((float) content.getWidth() - contentNatural) * 0.5f;
        for (const auto& item : panel.items)
        {
            const float w = item.units * unitWidth;
            item.component->setBounds (juce::Rectangle<int> ((int) std::round (cx), content.getY(), (int) std::round (w), content.getHeight()));
            cx += w;
        }
        x += width + (float) gap;
    }
}

void LabX0AudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds();
    overlay.setBounds (bounds);

    auto header = bounds.withHeight (headerHeight).reduced (margin, 14);
    auto presetArea = header.withSizeKeepingCentre (360, 26).translated (90, -4);
    renderButton.setBounds (presetArea.withX (presetArea.getRight() + 14).withWidth (96));
    prevButton.setBounds (presetArea.removeFromLeft (26));
    nextButton.setBounds (presetArea.removeFromRight (26));
    presetBox.setBounds (presetArea.reduced (4, 0));

    auto body = bounds.withTrimmedTop (headerHeight + 4).withTrimmedBottom (footerHeight).reduced (margin, 0);
    auto rowA = body.removeFromTop (rowAHeight);
    body.removeFromTop (6);

    // Row A: browser | pattern editor | side column
    browserFrame = rowA.removeFromLeft (browserWidth);
    rowA.removeFromLeft (gap);
    auto side = rowA.removeFromRight (sideWidth);
    rowA.removeFromRight (gap);
    patternFrame = rowA;
    renderPanel.setBounds (patternFrame.withSizeKeepingCentre (std::min (640, patternFrame.getWidth() - 40),
                                                               std::min (360, patternFrame.getHeight() - 40)));
    browser.setBounds (browserFrame.withTrimmedTop (titleHeight + 4).reduced (panelPad - 2, 0).withTrimmedBottom (6));
    patternPanel.setBounds (patternFrame.withTrimmedTop (titleHeight + 4).reduced (panelPad - 2, 0).withTrimmedBottom (6));

    const int heights[] = { 150, 96, 104, 56, 0 };
    auto column = side;
    for (size_t i = 0; i < sidePanels.size(); ++i)
    {
        auto& panel = sidePanels[i];
        panel.bounds = heights[i] > 0 ? column.removeFromTop (heights[i]) : column;
        column.removeFromTop (gap);
        auto content = panel.bounds.withTrimmedTop (titleHeight + 4).reduced (panelPad - 2, 4);
        for (auto& item : panel.items)
            item.component->setBounds (content);
    }
    if (emissionKnob != nullptr)
        emissionKnob->setBounds (sidePanels[1].bounds.withTrimmedTop (titleHeight + 2).reduced (60, 2));

    // Row B: one unit width for both rows so every knob is the same size
    const int rowCount = (int) rows.size();
    const int rowHeight = (body.getHeight() - gap * (rowCount - 1)) / rowCount;
    float unitWidth = 66.0f;
    for (const auto& row : rows)
    {
        float units = 0.0f;
        for (const auto& panel : row)
            for (const auto& item : panel.items)
                units += item.units;
        const float overhead = (float) (row.size() * 2 * panelPad + (row.size() - 1) * gap);
        unitWidth = std::min (unitWidth, ((float) body.getWidth() - overhead) / units);
    }
    lanePanel.setUnitWidth (unitWidth);
    for (int r = 0; r < rowCount; ++r)
        layoutRow (rows[(size_t) r], body.withTrimmedTop (r * (rowHeight + gap)).withHeight (rowHeight), unitWidth);

    overlay.toFront (false);
}

//==============================================================================
void LabX0AudioProcessorEditor::refreshFromProcessor()
{
    auto& m = audioProcessor.meters;
    patternPanel.refresh();
    lanePanel.refresh();

    const auto title = lanePanel.title();
    if (title != lastLaneTitle)
    {
        lastLaneTitle = title;
        repaint (rows[0][0].bounds.withHeight (titleHeight + 2));
    }

    const int playing = m.playingPattern.load();
    gauge.setValues (audioProcessor.getPatterns().copyPattern (playing), m.position.load(), m.running.load(), m.emission.load(),
                     playing, m.queuedPattern.load(), m.timeSlot.load(), m.volumeSlot.load());

    Dosimeter::Reading reading;
    reading.outDb = m.rmsDb.load();
    reading.peakDb = m.peakDb.load();
    reading.transientDb = m.transientDb.load();
    reading.slamDb = m.slamDb.load();
    reading.clipDb = m.clipDb.load();
    reading.limitDb = m.limitDb.load();
    for (int b = 0; b < 3; ++b)
        reading.band[b] = m.bandDb[(size_t) b].load();
    dosimeter.setValues (reading);

    audioProcessor.copyScope (scopeBuffer.data(), (int) scopeBuffer.size());
    scopeView.setSamples (scopeBuffer.data(), (int) scopeBuffer.size());

    const int program = audioProcessor.getCurrentProgram();
    if (program != lastProgram)
    {
        presetBox.setSelectedId (program + 1, juce::dontSendNotification);
        lastProgram = program;
    }

    auto& library = audioProcessor.getLibrary();
    auto rootState = [&library] (Root r) { return library.getRoot (r).isDirectory() ? juce::String ("OK") : juce::String ("NOT FOUND"); };
    const bool free = audioProcessor.apvts.getRawParameterValue ("run")->load() > 0.5f;
    juce::String text;
    text << "VOICES " << m.activeVoices.load() << "/" << maxPadVoices << "   MIDI " << juce::String (m.midiPerSecond.load(), 1) << "/s\n"
         << "BPM " << juce::String (m.bpm.load(), 1) << " " << (m.synced.load() ? "HOST" : (free ? "FREE" : "STOP"))
         << "   LATENCY " << m.latency.load() << "\n"
         << "PEAK " << juce::String (m.peakDb.load(), 1) << " dBFS   FAULTS " << m.faults.load()
         << (m.lastFaultStage.load() >= 0 ? juce::String (" ") + LabX0AudioProcessor::stageName (m.lastFaultStage.load()) : juce::String()) << "\n"
         << "USER " << rootState (Root::user) << "   ZONE " << rootState (Root::zone) << "\n"
         << "PACK " << rootState (Root::pack);
    systemLabel.setText (text, juce::dontSendNotification);
}

bool LabX0AudioProcessorEditor::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress ('z', juce::ModifierKeys::ctrlModifier, 0))
    {
        context.undoLast();
        return true;
    }
    return false;
}

void LabX0AudioProcessorEditor::stepPreset (int delta)
{
    const int count = audioProcessor.getNumPrograms();
    const int next = (audioProcessor.getCurrentProgram() + delta + count) % count;
    audioProcessor.setCurrentProgram (next);
    presetBox.setSelectedId (next + 1, juce::dontSendNotification);
    lastProgram = next;
}

void LabX0AudioProcessorEditor::chooseSampleFor (int lane)
{
    if (lane < 0 || lane >= laneTime)
        return;
    auto& library = audioProcessor.getLibrary();
    const auto current = library.resolve (audioProcessor.getSampleRef (lane));
    const auto start = current.existsAsFile() ? current : library.getRoot (Root::user);
    chooser = std::make_unique<juce::FileChooser> ("Load a sound into lane " + juce::String (lane + 1), start, "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this, lane] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();
                              if (file.existsAsFile())
                                  audioProcessor.assignSample (lane, audioProcessor.getLibrary().makeRef (file));
                          });
}

void LabX0AudioProcessorEditor::chooseFolderFor (int root)
{
    auto& library = audioProcessor.getLibrary();
    const auto r = (Root) root;
    const char* titles[] = { "Choose your sample library folder", "Choose your STALKER SPECIMENS folder", "Choose FL Studio's 'stalker sounds' folder" };
    chooser = std::make_unique<juce::FileChooser> (titles[juce::jlimit (0, 2, root)], library.getRoot (r), "*");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this, r] (const juce::FileChooser& fc)
                          {
                              const auto dir = fc.getResult();
                              if (dir.isDirectory())
                              {
                                  audioProcessor.getLibrary().setRoot (r, dir, true);
                                  audioProcessor.getLibrary().requestScan (r, true);
                              }
                          });
}
