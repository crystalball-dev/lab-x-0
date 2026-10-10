#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <vector>
#include <juce_audio_formats/juce_audio_formats.h>
#include "Config.h"
#include "Library.h"
#include "DSP/SampleData.h"

namespace labx0
{
    // Loads the samples of all slots (16 pads, 4 breaks, the browser preview) on one background thread and hands
    // them to the audio thread without locks. The audio thread brackets every block with beginBlock() and
    // endBlock(), which move an epoch counter: odd while a block runs. A replaced buffer is freed once no block
    // that could have read it is still running, at once when the host is idle.
    class SampleBank : private juce::Thread
    {
    public:
        enum class Status { empty, loading, ready, missingRoot, missingFile, failed, unsupported };

        explicit SampleBank (Library& library);
        ~SampleBank() override;

        // Message thread.
        int assign (int slot, const SampleRef& ref, double offsetSeconds = 0.0);   // returns the request generation
        void clearSlot (int slot);
        void reloadAll();
        SampleRef getRef (int slot) const;
        double getOffset (int slot) const;
        Status getStatus (int slot) const noexcept { return status[(size_t) slot].load(); }
        juce::String getStatusText (int slot) const;

        // Waveform and analysis for the editor, copied at publish time (never shared with the audio thread).
        struct Overview
        {
            uint64_t id = 0;
            juce::String name;
            int length = 0;
            double sampleRate = 44100.0;
            double seconds = 0.0, fileSeconds = 0.0, offsetSeconds = 0.0;
            double bpm = 0.0, beats = 0.0;
            int tempoSource = 0;
            std::vector<float> peaks;          // min, max pairs over 512 columns (mono)
            std::vector<int> onsetPos;
            std::vector<float> onsetStrength;
        };
        std::shared_ptr<const Overview> getOverview (int slot) const;
        int getVersion() const noexcept { return version.load(); }

        // The last tempo the host reported, used to pick the octave of an estimated break tempo.
        void setHostBpm (double bpm) noexcept { hostBpm.store (bpm); }

        // Audio thread.
        void beginBlock (std::array<const dsp::SampleData*, numSlots>& out) noexcept;
        void endBlock() noexcept { epoch.fetch_add (1); }

        // Harness and tests.
        bool isSettled() const noexcept;
        bool waitUntilSettled (int timeoutMs) const;

        // Decodes one file the way a slot would (analysis included). Used by the loader and the harness.
        struct LoadResult
        {
            std::unique_ptr<dsp::SampleData> data;
            std::shared_ptr<Overview> overview;
            Status status = Status::failed;
            juce::String message;
        };
        LoadResult decode (const juce::File& file, double offsetSeconds, bool analyse, double hostTempo,
                           const std::function<bool()>& shouldAbort);

    private:
        struct Request
        {
            SampleRef ref;
            double offset = 0.0;
        };

        void run() override;
        void load (int slot, int generation);
        void publish (int slot, std::unique_ptr<dsp::SampleData> next, std::shared_ptr<const Overview> overview);
        void reclaim();
        void setStatus (int slot, Status s, const juce::String& text);

        Library& library;
        juce::AudioFormatManager formats;

        mutable juce::CriticalSection lock;   // requests, status texts, overviews
        std::array<Request, numSlots> requests;
        std::array<juce::String, numSlots> statusText;
        std::array<std::shared_ptr<const Overview>, numSlots> overviews;

        std::array<std::atomic<int>, numSlots> requested {}, handled {};
        std::array<std::atomic<Status>, numSlots> status {};
        std::array<std::atomic<dsp::SampleData*>, numSlots> active {};
        std::array<std::unique_ptr<dsp::SampleData>, numSlots> owners;                      // loader thread
        std::vector<std::pair<std::unique_ptr<dsp::SampleData>, uint64_t>> retired;          // loader thread
        std::atomic<uint64_t> epoch { 0 };
        std::atomic<int> version { 0 };
        std::atomic<double> hostBpm { 120.0 };
        uint64_t nextId = 1;
        int seenRootsVersion = 0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SampleBank)
    };
}
