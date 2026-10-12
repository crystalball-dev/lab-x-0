#pragma once

namespace labx0
{
    inline constexpr const char* pluginName    = "LAB X-0";
    inline constexpr const char* vendorName    = "OPERATION FAIRWAY, LLC";
    inline constexpr const char* versionString = "0.2.3";
    inline constexpr const char* projectUrl    = "github.com/crystalball-dev/lab-x-0";

    inline constexpr int numPads     = 16;
    inline constexpr int numBreaks   = 4;
    inline constexpr int numPatterns = 16;
    inline constexpr int maxSteps    = 64;

    // Sequencer lanes: 16 pads, 4 break slots, the TIME lane and the VOLUME lane.
    inline constexpr int laneFirstBreak = numPads;
    inline constexpr int laneTime       = numPads + numBreaks;
    inline constexpr int laneVolume     = laneTime + 1;
    inline constexpr int numLanes       = laneVolume + 1;

    // Sample slots: one per pad, one per break slot, one for the browser preview.
    inline constexpr int slotFirstBreak = numPads;
    inline constexpr int previewSlot    = numPads + numBreaks;
    inline constexpr int numSlots       = previewSlot + 1;

    inline constexpr int numAuxOutputs   = numPads + numBreaks;   // stereo outputs 2..21, one per lane if wanted
    inline constexpr int maxPadVoices    = 48;   // shared by all pads
    inline constexpr int maxVoicesPerPad = 4;    // Poly mode
    inline constexpr int controlBlock    = 16;   // samples per control-rate step
    inline constexpr int maxSlices       = 128;
    inline constexpr double maxSampleSeconds = 120.0;   // longest stretch of a file that is decoded

    // MIDI map
    inline constexpr int midiFirstPad     = 36;   // 36..51  pads 1-16
    inline constexpr int midiFirstBreak   = 52;   // 52..55  break slots A-D from their start
    inline constexpr int midiFirstSlice   = 60;   // 60..91  slices 1-32 of the SLICE MIDI slot
    inline constexpr int midiSliceCount   = 32;
    inline constexpr int midiFirstPattern = 96;   // 96..111 pattern select at the next pattern boundary
    inline constexpr int midiFirstBreakChannel = 2;   // channels 2..5: notes 60..91 play slices of break A-D
}
