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
#include "Explode.h"
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

    void assignSample (int slot, const labx0::SampleRef& ref, double offsetSeconds = 0.0, double lengthSeconds = 0.0);

    // Slice points placed by hand (0.2.0); none = the automatic ones from SLICING and SENS. Sample positions in the
    // slot's decoded audio, ascending, the first at 0. Cleared when the slot's sample changes. Message thread.
    void setCustomSlices (int breakSlot, const std::vector<int>& positions);
    std::vector<int> getCustomSlices (int breakSlot) const;
    int getCustomSlicesVersion (int breakSlot) const noexcept;

    // EXPLODE (0.2.0): break slot -> pads firstPad.. (one per component of the break, each holding its loudest hit as
    // a region of the break file) and a pattern that plays the break back on them, written into the first empty
    // pattern after the current one and selected. The break is decoded again on a background thread; `done` gets a
    // summary on the message thread. explodeBreakNow does it all at once (the harness).
    // everySlice: one pad per slice (similar slices share only when there are more slices than pads).
    void explodeBreak (int breakSlot, int firstPad, bool everySlice, std::function<void (const juce::String&)> done);
    labx0::ExplodePlan explodeBreakNow (int breakSlot, int firstPad, bool everySlice = false, int* patternIndex = nullptr);

    // RENDER (0.2.0): WAV files of a pattern as heard, made by a second engine on a background thread, to drag into
    // the host's playlist. What: the mix; the mix and every lane with steps on its own (soloed, through the whole
    // chain); or one lane. A MIDI file of the hits as played goes with the mix.
    struct RenderRequest
    {
        int pattern = 0, passes = 1;
        int what = 0;                  // 0 mix, 1 mix and stems, 2 one lane, 3 stems only
        int lane = 0;                  // for what == 2
        double bpm = 0.0;              // 0 = the host's last tempo
        double sampleRate = 0.0;       // 0 = the current rate
        juce::File folder;             // empty = Documents\LAB X-0\Renders\<date time> Pattern NN
    };
    struct RenderStatus
    {
        bool running = false;
        float progress = 0.0f;
        juce::String text;
        juce::String midi;               // what the MIDI file holds, or what it could not
        juce::Array<juce::File> files;   // WAVs, then the MIDI file
        juce::File folder;
    };
    bool startRender (const RenderRequest& request);    // message thread; false while one is running

    // OUTS menu (message thread): every lane on its own output (pad 1-16 on Out 2-17, break A-D on Out 18-21), or
    // every lane on the main output.
    void setEveryLaneOutput (bool ownOutputs);
    int countLanesOnOwnOutput() const;   // how many lanes are on an output of their own
    RenderStatus getRenderStatus() const;
    int getRenderVersion() const noexcept { return renderVersion.load(); }
    RenderStatus renderNow (const RenderRequest& request);   // all at once on this thread (the harness)
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
    juce::String describeVoices() const;   // what is sounding, for the fuzz session's reports
    int activeVoiceCount() const noexcept
    {
        int n = 0;
        for (const auto& v : padVoices)
            n += v.isActive() ? 1 : 0;
        for (const auto& pair : breakVoices)
            for (const auto& v : pair)
                n += v.isActive() ? 1 : 0;
        return n;
    }
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
        int custom = -1;     // version of the hand-placed slices in use, -1 = automatic
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
    void releaseMidiNote (int channel, int note) noexcept;
    void releaseAllMidiNotes() noexcept;
    void applyPendingReleases() noexcept;
    static int midiKey (int channel, int note) noexcept { return (channel - 1) * 128 + note; }
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
    std::array<int, labx0::numBreaks> breakMidiNote {};           // midiKey of the MIDI note holding the break, -1 none
    // A Legato break's note-off waits until every event at its sample is in: a retrigger at that sample cuts the voice
    // itself, so a note-off and the next note at the same instant (touching notes) play like the step sequencer.
    std::array<uint32_t, labx0::numBreaks> breakSerial {}, breakSerialRendered {};
    std::array<int64_t, labx0::numBreaks> breakReleasePending {};  // the serial a note-off saw, -1 none
    std::array<SliceCache, labx0::numBreaks> sliceCaches {};

    struct CustomSlices
    {
        mutable juce::SpinLock lock;
        std::array<int, labx0::maxSlices> pos {};
        int count = 0;                      // 0 = automatic
        std::atomic<int> version { 0 };
    };
    std::array<CustomSlices, labx0::numBreaks> customSlices;
    struct CustomCopy
    {
        std::array<int, labx0::maxSlices> pos {};
        int count = 0, version = -1;
    };
    std::array<CustomCopy, labx0::numBreaks> customCopy;   // the audio thread's copy
    std::vector<int> sliceScratch;
    labx0::dsp::PreviewVoice preview;
    std::array<labx0::dsp::PadSettings, labx0::numPads> padSettings {};
    std::array<labx0::dsp::BreakSettings, labx0::numBreaks> breakSettings {};
    labx0::dsp::Anomaly anomaly;
    labx0::dsp::Transient transient;
    labx0::dsp::Multiband multiband;
    labx0::dsp::PunchStage punch;
    labx0::dsp::Regulator regulator;

    // Per-lane outputs (0.2.0). A lane set to output k (2..21) renders into aux[k - 2] instead of FULL, PUNCH or
    // DRY: it leaves straight after its voices, before ANOMALY, PUNCH and the master section, and waits the plugin's
    // latency so every output lines up. When the host has not enabled that output, the lane plays on the main one.
    struct AuxOutput
    {
        std::array<float, labx0::controlBlock> l {}, r {};
        std::vector<float> delayL, delayR;
        int delayPos = 0;
        int channel = -1;   // first channel of this output in the current block's buffer, -1 = not available
    };
    std::array<AuxOutput, labx0::numAuxOutputs> aux;
    int auxDelayLength = 0;
    std::array<int, labx0::numPads + labx0::numBreaks> laneOut {};
    std::array<float, labx0::controlBlock> fadeTrack {};
    void clearAuxDelays() noexcept;
    static BusesProperties makeBuses();

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

    struct ExplodeSource
    {
        labx0::SampleRef ref;
        double offset = 0.0, length = 0.0, beats = 4.0;
        std::vector<int> slices;
        juce::String error;
    };
    ExplodeSource explodeSourceFor (int breakSlot) const;
    labx0::ExplodePlan analyseExplode (const ExplodeSource& source, int maxPads, bool everySlice, double& sampleRate);

    // Rendering: these run on a renderer instance (never the one the host plays).
    struct RecordedHit
    {
        juce::int64 sample = 0;
        int lane = 0, slice = -1, gateSamples = 0, pitch = 0;
        float velocity = 1.0f;
        bool exact = true;   // breaks: starts on its slice (else between slices, and the slice is the nearest)
    };
    std::vector<RecordedHit>* recordHits = nullptr;
    RenderStatus renderAll (const RenderRequest& request, double sampleRate, double bpm, std::atomic<float>& progress);
    juce::AudioBuffer<float> renderPass (int pattern, double sampleRate, double bpm, int body, int length, std::atomic<float>& progress,
                                         float from, float to);
    RenderRequest resolveRequest (const RenderRequest& request) const;
    mutable juce::CriticalSection renderLock;
    RenderStatus renderStatus;
    std::atomic<int> renderVersion { 0 };
    std::atomic<float> renderProgress { 0.0f };
    std::atomic<bool> renderBusy { false };
    int applyExplode (int breakSlot, int firstPad, const labx0::ExplodePlan& plan, const ExplodeSource& source, double sampleRate);

    // Background work (EXPLODE, renders). Declared last, so it is destroyed first: a running job finishes before
    // anything it uses goes away; `alive` stops results arriving after that.
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
    std::unique_ptr<juce::ThreadPool> jobs = std::make_unique<juce::ThreadPool> (juce::ThreadPoolOptions().withThreadName ("LAB X-0 jobs")
                                                                                                           .withNumberOfThreads (1));

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LabX0AudioProcessor)
};
