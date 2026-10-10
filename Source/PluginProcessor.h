#pragma once

#include <array>
#include <atomic>
#include <limits>
#include <vector>
#include <juce_audio_processors/juce_audio_processors.h>
#include "Config.h"
#include "Params.h"
#include "Pattern.h"
#include "Library.h"
#include "SampleBank.h"
#include "DSP/Anomaly.h"
#include "DSP/Punch.h"
#include "DSP/Regulator.h"
#include "DSP/Sequencer.h"
#include "DSP/Voices.h"

class LabX0AudioProcessor final : public juce::AudioProcessor,
                                  private juce::Timer,
                                  private juce::AsyncUpdater
{
public:
    LabX0AudioProcessor();
    ~LabX0AudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return labx0::pluginName; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 4.0; }

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    // Editor API (message thread)
    labx0::Library& getLibrary() noexcept           { return *library; }
    labx0::SampleBank& getBank() noexcept           { return bank; }
    labx0::PatternStore& getPatterns() noexcept     { return patterns; }

    void assignSample (int slot, const labx0::SampleRef& ref, double offsetSeconds = 0.0);
    labx0::SampleRef getSampleRef (int slot) const  { return bank.getRef (slot); }
    labx0::SampleRef resolveKitRef (const juce::String& text) const;
    void previewSample (const labx0::SampleRef& ref);
    void stopPreview();
    void audition (int lane, float velocity);
    void auditionSlice (int breakSlot, int slice);

    int getEditPattern() const noexcept;
    void setEditPattern (int index);

    // Slices of a break slot exactly as the audio thread computes them, from the editor's overview copy.
    int computeSlices (int breakSlot, const labx0::SampleBank::Overview& overview, int* out, int maxOut) const;
    double loopBeatsFor (int breakSlot, const labx0::SampleBank::Overview& overview) const;

    // Remembered with the project: what the editor showed.
    std::atomic<int> uiSelectedLane { 0 }, uiPage { 0 }, uiStripMode { 0 };

    //==============================================================================
    struct Meters
    {
        std::atomic<float> rmsDb { -100.0f }, peakDb { -100.0f }, emission { 0.0f }, midiPerSecond { 0.0f };
        std::atomic<float> bpm { 120.0f }, transientDb { 0.0f }, slamDb { 0.0f }, clipDb { 0.0f }, limitDb { 0.0f };
        std::array<std::atomic<float>, 3> bandDb {};
        std::atomic<int> activeVoices { 0 }, faults { 0 }, latency { 0 }, lastFaultStage { -1 };
        std::atomic<int> playingPattern { 0 }, queuedPattern { -1 }, timeSlot { 0 }, volumeSlot { 0 };
        std::atomic<bool> running { false }, synced { false };
        std::atomic<double> position { 0.0 };          // quarters since the pattern start
        std::array<std::atomic<int>, labx0::numLanes> laneStep {};
        std::array<std::atomic<float>, labx0::numLanes> laneFlash {};   // last hit velocity (editor decays it)
        std::array<std::atomic<double>, labx0::numBreaks> breakPosition {};   // source sample of each break voice, -1 idle
    };
    Meters meters;

    static constexpr int scopeSize = 2048;
    void copyScope (float* dest, int numSamples) const noexcept;

    // Runaway guard log, as in the siblings: %APPDATA%\OPERATION FAIRWAY, LLC\LAB X-0 faults.log
    static const char* stageName (int stage) noexcept;
    juce::File getFaultLogFile() const;
    void setFaultLogFile (const juce::File& f) { faultLogOverride = f; }
    int flushFaultLog();

    juce::AudioProcessorValueTreeState apvts;

   #if LABX0_HARNESS
    std::array<float, 6> stagePeak {};
    int64_t samplesRendered = 0;
    int injectStage = -1;
    int64_t injectAtSample = -1;
    int padTriggers = 0, breakTriggers = 0;
    void setLibraryOverride (labx0::Root r, const juce::File& f) { library->setRoot (r, f, false); }
   #endif

private:
    struct Smoother
    {
        bool enabled = false, logDomain = false;
        float current = 0.0f;
        void reset (float v) noexcept { current = logDomain ? std::log2 (std::max (v, 1.0e-4f)) : v; }
        float advance (float target, float coef) noexcept
        {
            const float t = logDomain ? std::log2 (std::max (target, 1.0e-4f)) : target;
            current += (t - current) * coef;
            return logDomain ? std::exp2 (current) : current;
        }
    };

    struct UiEvent
    {
        int type = 0;        // 0 audition, 1 stop preview, 2 slice audition
        int lane = 0;
        float velocity = 1.0f;
        int slice = -1;
    };

    struct SliceCache
    {
        uint64_t dataId = 0;
        int slicing = -1;
        float sens = -1.0f;
        double beats = -1.0;
        int count = 0;
        std::array<int, labx0::maxSlices> starts {};
    };

    void timerCallback() override { flushFaultLog(); }
    void handleAsyncUpdate() override;

    void renderControlBlock (float* outL, float* outR, int n, double q) noexcept;
    void updateControl (int n) noexcept;
    void handleMidi (const juce::MidiMessage& m, int offsetInBlock) noexcept;
    void handleSeqEvent (const labx0::dsp::SeqEvent& e) noexcept;
    void triggerPad (int pad, float velocity, int pitch, int gateSamples) noexcept;
    void triggerBreak (int b, double sourcePos, int slice, float velocity, int pitch, bool midiNote, int note, bool sliceGate = false) noexcept;
    void triggerBreakStep (int b, const labx0::dsp::SeqEvent& e) noexcept;
    void triggerBreakSlice (int b, int slice, float velocity, int note) noexcept;
    const SliceCache& slicesFor (int b, const labx0::dsp::SampleData* d) noexcept;
    double loopBeats (int b, const labx0::dsp::SampleData* d) const noexcept;
    void releaseMidiNote (int note) noexcept;
    void allSoundOff() noexcept;
    void flushForProgramChange() noexcept;
    void recoverFromFault() noexcept;
    static float blockPeak (const float* l, const float* r, int n) noexcept;
    void noteFault (int stage, float peak, bool fault) noexcept;

    void applyPreset (int index);
    float rawValue (int index) const noexcept { return raw[(size_t) index]->load (std::memory_order_relaxed); }
    int choiceOf (int index) const noexcept   { return (int) std::lround (rawValue (index)); }

    juce::SharedResourcePointer<labx0::Library> library;
    labx0::SampleBank bank { *library };
    labx0::PatternStore patterns;

    std::array<std::atomic<float>*, labx0::P::count> raw {};
    std::array<Smoother, labx0::P::count> smoothers {};
    std::array<float, labx0::P::count> values {};
    std::array<float, labx0::controlBlock + 1> smoothCoef {};

    // Audio thread state
    std::array<const labx0::dsp::SampleData*, labx0::numSlots> blockSamples {};
    labx0::PatternBank localBank;
    std::array<uint32_t, labx0::numPatterns> seenPatternVersions {};
    uint32_t seenPatternTotal = 0;
    labx0::dsp::StepSequencer sequencer;
    std::array<labx0::dsp::SeqEvent, labx0::dsp::StepSequencer::maxEvents> events {};
    std::array<labx0::dsp::PadVoice, labx0::maxPadVoices> padVoices;
    std::array<std::array<labx0::dsp::BreakVoice, 2>, labx0::numBreaks> breakVoices;
    std::array<int, labx0::numBreaks> breakCurrent {};
    std::array<int, labx0::numBreaks> breakMidiNote {};
    std::array<SliceCache, labx0::numBreaks> sliceCaches {};
    std::vector<int> sliceScratch;
    labx0::dsp::PreviewVoice preview;
    std::array<labx0::dsp::PadSettings, labx0::numPads> padSettings {};
    std::array<labx0::dsp::BreakSettings, labx0::numBreaks> breakSettings {};
    labx0::dsp::Anomaly anomaly;
    labx0::dsp::Transient transient;
    labx0::dsp::Multiband multiband;
    labx0::dsp::PunchStage punch;
    labx0::dsp::Regulator regulator;

    std::vector<float> busStorage;   // FULL, PUNCH, DRY x 2 channels x controlBlock
    std::vector<float> dryDelayL, dryDelayR;
    int dryDelayLength = 0, dryDelayPos = 0;

    double currentSampleRate = 48000.0, hostBpm = 120.0, freeQ = 0.0;
    bool hostPlaying = false, wasFree = false;
    int currentPattern = 0, queuedPattern = -1, lastParamPattern = -1;
    float modWheel = 0.0f, modWheelTarget = 0.0f;
    uint64_t voiceStamp = 0;
    double blockQps = 0.0;
    bool blockRunning = false;

    // Effective control values for the current control block
    float emissionValue = 0.0f, widthValue = 1.0f, masterGain = 0.5f;
    float dcInL = 0.0f, dcInR = 0.0f, dcOutL = 0.0f, dcOutR = 0.0f, dcCoef = 0.9995f;

    // Preview and auditions from the editor
    juce::AbstractFifo uiFifo { 64 };
    std::array<UiEvent, 64> uiEvents {};
    std::atomic<int> previewRequest { 0 };
    int previewSeenRequest = 0;
    uint64_t previewIdAtRequest = 0;
    bool previewWanted = false;

    // Preset changes: fade out, switch, fade back in.
    enum class SwitchState { idle, fadingOut, fadingIn };
    std::atomic<bool> programChangePending { false };
    SwitchState switchState = SwitchState::idle;
    int switchFadeRemaining = 0, switchFadeLength = 1440;
    float switchGain = 1.0f, switchFadeInStep = 0.005f;
    int currentProgram = 0;
    std::atomic<int> pendingProgram { -1 };

    // Metering
    std::array<float, scopeSize> scope {};
    std::atomic<int> scopeWrite { 0 };
    int midiAccumulator = 0, meterSamples = 0;

    // Runaway guard
    static constexpr float voiceLimit = 64.0f;    // +36 dBFS for one voice
    static constexpr float stageLimit = 128.0f;   // +42 dBFS anywhere in the chain
    static constexpr float hotLevel = 8.0f;       // +18 dBFS: logged, not reset
    struct FaultEntry
    {
        int stage = 0, activeVoices = 0, pattern = 0;
        float peakDb = 0.0f;
        bool fault = false, running = false;
        double seconds = 0.0, bpm = 120.0;
    };
    std::array<FaultEntry, 64> faultRing {};
    std::atomic<int> faultWrite { 0 };
    int faultRead = 0;
    std::array<int64_t, 7> lastHotAt { -1, -1, -1, -1, -1, -1, -1 };
    int64_t engineClock = 0;
    juce::File faultLogOverride;
    bool faultLogHeaderWritten = false;
    int preparedBlockSize = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LabX0AudioProcessor)
};
