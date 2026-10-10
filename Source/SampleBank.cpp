#include <algorithm>
#include <cmath>
#include "SampleBank.h"
#include "AudioAnalysis.h"

namespace labx0
{
    namespace
    {
        constexpr float targetPeak = 0.891f;    // -1 dBFS
        constexpr float maxNormaliseGain = 31.6f;   // +30 dB: near-silent files are not blown up into noise

        // Some game files decode with a burst of garbage: Shadow of Chornobyl's psy_blackout_l.ogg starts with
        // 238 samples reaching +71 dBFS. Non-finite samples and anything above +24 dBFS are silenced together
        // with 10 ms either side, in both channels, so one bad burst can neither dominate the normalisation nor
        // reach the output.
        int silenceDecodeBursts (float* l, float* r, int length, double sampleRate)
        {
            const int pad = std::max (1, (int) (sampleRate * 0.010));
            auto bad = [l, r] (int i)
            {
                return ! std::isfinite (l[i]) || ! std::isfinite (r[i]) || std::abs (l[i]) > 16.0f || std::abs (r[i]) > 16.0f;
            };
            // Find the whole burst first (bad samples closer than the margin belong to one), then silence it: zeroing
            // as we go would hide the rest of the burst and cut the margin after it short.
            int bursts = 0, clearedUntil = -1;
            for (int i = 0; i < length; ++i)
            {
                if (! bad (i))
                    continue;
                int last = i;
                for (int k = i + 1; k < length && k <= last + pad; ++k)
                    if (bad (k))
                        last = k;
                ++bursts;
                const int from = std::max (clearedUntil + 1, i - pad);
                const int to = std::min (length - 1, last + pad);
                for (int k = from; k <= to; ++k)
                    l[k] = r[k] = 0.0f;
                clearedUntil = to;
                i = to;
            }
            return bursts;
        }

        juce::String formatSeconds (double s)
        {
            return s < 10.0 ? juce::String (s, 2) + " S" : juce::String (s, 1) + " S";
        }
    }

    //==============================================================================
    SampleBank::SampleBank (Library& lib)
        : juce::Thread ("LAB X-0 sample loader"), library (lib)
    {
        formats.registerBasicFormats();
        for (auto& s : status)
            s.store (Status::empty);
        for (auto& a : active)
            a.store (nullptr);
        seenRootsVersion = library.getRootsVersion();
        startThread (juce::Thread::Priority::low);
    }

    SampleBank::~SampleBank()
    {
        stopThread (5000);
        for (auto& a : active)
            a.store (nullptr);
        for (auto& o : owners)
            o.reset();
        retired.clear();
    }

    int SampleBank::assign (int slot, const SampleRef& ref, double offsetSeconds)
    {
        if (! juce::isPositiveAndBelow (slot, numSlots))
            return 0;
        int generation;
        {
            const juce::ScopedLock sl (lock);
            requests[(size_t) slot] = { ref, std::max (0.0, offsetSeconds) };
            generation = requested[(size_t) slot].fetch_add (1) + 1;
        }
        notify();
        return generation;
    }

    void SampleBank::clearSlot (int slot)
    {
        assign (slot, {}, 0.0);
    }

    void SampleBank::reloadAll()
    {
        for (int s = 0; s < numSlots; ++s)
            requested[(size_t) s].fetch_add (1);
        notify();
    }

    SampleRef SampleBank::getRef (int slot) const
    {
        const juce::ScopedLock sl (lock);
        return requests[(size_t) slot].ref;
    }

    double SampleBank::getOffset (int slot) const
    {
        const juce::ScopedLock sl (lock);
        return requests[(size_t) slot].offset;
    }

    juce::String SampleBank::getStatusText (int slot) const
    {
        const juce::ScopedLock sl (lock);
        return statusText[(size_t) slot];
    }

    std::shared_ptr<const SampleBank::Overview> SampleBank::getOverview (int slot) const
    {
        const juce::ScopedLock sl (lock);
        return overviews[(size_t) slot];
    }

    void SampleBank::setStatus (int slot, Status s, const juce::String& text)
    {
        {
            const juce::ScopedLock sl (lock);
            statusText[(size_t) slot] = text;
        }
        status[(size_t) slot].store (s);
        version.fetch_add (1);
    }

    void SampleBank::beginBlock (std::array<const dsp::SampleData*, numSlots>& out) noexcept
    {
        // Sequentially consistent on purpose: pairs with publish() and reclaim().
        epoch.fetch_add (1);
        for (int s = 0; s < numSlots; ++s)
            out[(size_t) s] = active[(size_t) s].load();
    }

    bool SampleBank::isSettled() const noexcept
    {
        for (int s = 0; s < numSlots; ++s)
            if (handled[(size_t) s].load() != requested[(size_t) s].load() || status[(size_t) s].load() == Status::loading)
                return false;
        return true;
    }

    bool SampleBank::waitUntilSettled (int timeoutMs) const
    {
        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
        while (juce::Time::getMillisecondCounter() < deadline)
        {
            if (isSettled())
                return true;
            juce::Thread::sleep (5);
        }
        return false;
    }

    //==============================================================================
    void SampleBank::run()
    {
        while (! threadShouldExit())
        {
            reclaim();

            // A changed library root may bring missing files back.
            const int roots = library.getRootsVersion();
            if (roots != seenRootsVersion)
            {
                seenRootsVersion = roots;
                for (int s = 0; s < numSlots; ++s)
                {
                    const auto st = status[(size_t) s].load();
                    if (st == Status::missingRoot || st == Status::missingFile || st == Status::ready)
                        requested[(size_t) s].fetch_add (1);
                }
            }

            bool worked = false;
            for (int s = 0; s < numSlots && ! threadShouldExit(); ++s)
            {
                const int gen = requested[(size_t) s].load();
                if (gen != handled[(size_t) s].load())
                {
                    load (s, gen);
                    handled[(size_t) s].store (gen);
                    worked = true;
                }
            }

            if (! worked)
                wait (40);
        }
    }

    void SampleBank::load (int slot, int generation)
    {
        Request request;
        {
            const juce::ScopedLock sl (lock);
            request = requests[(size_t) slot];
        }

        auto fail = [this, slot] (Status s, const juce::String& text)
        {
            publish (slot, nullptr, nullptr);
            setStatus (slot, s, text);
        };

        if (request.ref.isEmpty())
            return fail (Status::empty, "EMPTY");

        const auto name = request.ref.fileName();
        const auto file = library.resolve (request.ref);
        // A root that is set but gone (unplugged drive, moved library) names the library, not the file.
        if (file == juce::File() || (request.ref.root != Root::none && ! library.getRoot (request.ref.root).isDirectory()))
            return fail (Status::missingRoot, juce::String (rootTag (request.ref.root)) + " LIBRARY NOT FOUND / SET FOLDER");

        setStatus (slot, Status::loading, "LOADING " + request.ref.displayName().toUpperCase());

        auto shouldAbort = [this, slot, generation] { return threadShouldExit() || requested[(size_t) slot].load() != generation; };
        auto result = decode (file, request.offset, slot >= slotFirstBreak && slot < previewSlot, hostBpm.load(), shouldAbort);
        if (shouldAbort())
            return;

        if (result.data == nullptr)
            return fail (result.status, result.message);

        const auto text = result.message;
        result.data->generation = generation;
        publish (slot, std::move (result.data), result.overview);
        setStatus (slot, Status::ready, text);
    }

    SampleBank::LoadResult SampleBank::decode (const juce::File& requestedFile, double offsetSeconds, bool analyse, double hostTempo,
                                               const std::function<bool()>& shouldAbort)
    {
        LoadResult result;
        const auto name = requestedFile.getFileName();
        const auto file = longPathSafe (requestedFile);

        if (! file.existsAsFile())
        {
            result.status = Status::missingFile;
            result.message = "FILE NOT FOUND: " + name;
            return result;
        }

        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        if (reader == nullptr)
        {
            // Misnamed files: let every format look at the content.
            if (auto stream = file.createInputStream())
                reader.reset (formats.createReaderFor (std::move (stream)));
        }
        if (reader == nullptr)
        {
            IndexEntry probe;
            readAudioHeader (file, probe);
            result.status = (probe.flags & flagUnsupported) ? Status::unsupported : Status::failed;
            result.message = ((probe.flags & flagUnsupported) ? "UNSUPPORTED FORMAT: " : "UNREADABLE: ") + name;
            return result;
        }
        if (reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0 || reader->numChannels == 0)
        {
            result.status = Status::failed;
            result.message = "EMPTY OR BROKEN: " + name;
            return result;
        }

        const double sr = reader->sampleRate;
        const juce::int64 total = reader->lengthInSamples;
        const auto offset = std::clamp ((juce::int64) (offsetSeconds * sr), (juce::int64) 0, std::max ((juce::int64) 0, total - 1));
        const int length = (int) std::min (total - offset, (juce::int64) (maxSampleSeconds * sr));
        const int channels = (int) reader->numChannels;
        if (length <= 0)
        {
            result.status = Status::failed;
            result.message = "EMPTY OR BROKEN: " + name;
            return result;
        }

        auto data = std::make_unique<dsp::SampleData>();
        data->left.assign ((size_t) length + 2 * dsp::SampleData::guard, 0.0f);
        data->right.assign ((size_t) length + 2 * dsp::SampleData::guard, 0.0f);
        data->length = length;
        data->sampleRate = sr;
        float* l = data->left.data() + dsp::SampleData::guard;
        float* r = data->right.data() + dsp::SampleData::guard;

        // Decode in chunks so a newer request or shutdown interrupts a long file.
        constexpr int chunk = 65536;
        juce::AudioBuffer<float> buffer (channels, chunk);
        for (int pos = 0; pos < length; pos += chunk)
        {
            if (shouldAbort != nullptr && shouldAbort())
                return result;
            const int n = std::min (chunk, length - pos);
            buffer.clear();
            reader->read (buffer.getArrayOfWritePointers(), channels, offset + pos, n);

            if (channels == 1)
            {
                std::copy_n (buffer.getReadPointer (0), n, l + pos);
                std::copy_n (buffer.getReadPointer (0), n, r + pos);
            }
            else if (channels == 2)
            {
                std::copy_n (buffer.getReadPointer (0), n, l + pos);
                std::copy_n (buffer.getReadPointer (1), n, r + pos);
            }
            else
            {
                // More than two channels: even channels to the left, odd to the right.
                const int evens = (channels + 1) / 2, odds = channels / 2;
                for (int i = 0; i < n; ++i)
                {
                    float a = 0.0f, b = 0.0f;
                    for (int ch = 0; ch < channels; ++ch)
                        (ch % 2 == 0 ? a : b) += buffer.getSample (ch, i);
                    l[pos + i] = a / (float) evens;
                    r[pos + i] = b / (float) odds;
                }
            }
        }

        silenceDecodeBursts (l, r, length, sr);

        float peak = 0.0f;
        for (int i = 0; i < length; ++i)
            peak = std::max (peak, std::max (std::abs (l[i]), std::abs (r[i])));
        if (peak > 1.0e-6f)
        {
            const float gain = std::min (targetPeak / peak, maxNormaliseGain);
            for (int i = 0; i < length; ++i)
            {
                l[i] *= gain;
                r[i] *= gain;
            }
            data->normaliseGain = gain;
        }

        auto overview = std::make_shared<Overview>();
        overview->name = file.getFileNameWithoutExtension();
        overview->length = length;
        overview->sampleRate = sr;
        overview->seconds = (double) length / sr;
        overview->fileSeconds = (double) total / sr;
        overview->offsetSeconds = (double) offset / sr;

        std::vector<float> mono ((size_t) length);
        for (int i = 0; i < length; ++i)
            mono[(size_t) i] = 0.5f * (l[i] + r[i]);

        constexpr int columns = 512;
        overview->peaks.assign ((size_t) columns * 2, 0.0f);
        for (int c = 0; c < columns; ++c)
        {
            const int a = (int) ((juce::int64) length * c / columns);
            const int b = std::max (a + 1, (int) ((juce::int64) length * (c + 1) / columns));
            float lo = 0.0f, hi = 0.0f;
            for (int i = a; i < std::min (b, length); ++i)
            {
                lo = std::min (lo, mono[(size_t) i]);
                hi = std::max (hi, mono[(size_t) i]);
            }
            overview->peaks[(size_t) c * 2] = lo;
            overview->peaks[(size_t) c * 2 + 1] = hi;
        }

        const double duration = (double) length / sr;
        juce::String detail;

        if (analyse)
        {
            const auto env = analysis::computeEnvelope (mono.data(), length, sr);
            if (shouldAbort != nullptr && shouldAbort())
                return result;
            for (const auto& o : analysis::detectOnsets (env, mono.data(), length))
            {
                data->onsetPos.push_back (o.position);
                data->onsetStrength.push_back (o.strength);
            }

            // Tempo: ACID chunk, then the file name, then the folder names, then the estimate.
            double bpm = 0.0;
            int source = 0;
            const auto acid = reader->metadataValues.getValue (juce::WavAudioFormat::acidTempo, {});
            if (acid.isNotEmpty())
            {
                const double v = acid.getDoubleValue();
                if (v >= 40.0 && v <= 300.0)
                {
                    bpm = v;
                    source = 1;
                }
            }
            if (bpm <= 0.0)
            {
                bpm = analysis::tempoFromName (file.getFileNameWithoutExtension(), duration, offset == 0);
                if (bpm > 0.0)
                    source = 2;
            }
            if (bpm <= 0.0)
            {
                // "Percussion 100 BPM Loops", "Synth Loops 100 BPM": the two folders above the file.
                auto folder = file.getParentDirectory();
                for (int depth = 0; depth < 2 && bpm <= 0.0; ++depth)
                {
                    bpm = analysis::tempoFromName (folder.getFileName(), duration, false);
                    if (bpm > 0.0)
                        source = 3;
                    folder = folder.getParentDirectory();
                }
            }
            if (bpm <= 0.0)
            {
                const double estimate = analysis::estimateTempo (env, duration);
                if (estimate > 0.0)
                {
                    bpm = analysis::nearestOctave (estimate, hostTempo > 0.0 ? hostTempo : 120.0);
                    source = 4;
                }
            }

            if (bpm > 0.0)
            {
                double beats = duration * bpm / 60.0;
                const double whole = std::round (beats);
                if (whole >= 1.0 && std::abs (beats - whole) / whole < 0.03)
                {
                    beats = whole;
                    if (source == 4)
                        bpm = 60.0 * beats / duration;   // a trimmed loop: exact tempo from its length
                }
                data->bpm = bpm;
                data->beats = beats;
                data->tempoSource = source;
                detail = "  " + juce::String (bpm, 1) + " BPM " + dsp::tempoSourceName (source) + "  "
                       + juce::String (beats, std::abs (beats - std::round (beats)) < 1.0e-6 ? 0 : 1) + " BEATS";
            }
            else
            {
                detail = "  TEMPO UNKNOWN";
            }

            overview->bpm = data->bpm;
            overview->beats = data->beats;
            overview->tempoSource = data->tempoSource;
            overview->onsetPos = data->onsetPos;
            overview->onsetStrength = data->onsetStrength;
        }

        result.message = file.getFileNameWithoutExtension().toUpperCase() + "  " + formatSeconds (duration)
                       + (total > length + offset || offset > 0 ? " OF " + formatSeconds ((double) total / sr) : juce::String())
                       + detail + (peak <= 1.0e-6f ? "  SILENT" : "");
        result.status = Status::ready;
        result.overview = overview;
        result.data = std::move (data);
        return result;
    }

    void SampleBank::publish (int slot, std::unique_ptr<dsp::SampleData> next, std::shared_ptr<const Overview> overview)
    {
        if (next != nullptr)
        {
            next->id = nextId++;
            if (overview != nullptr)
            {
                auto copy = std::make_shared<Overview> (*overview);
                copy->id = next->id;
                overview = copy;
            }
        }

        active[(size_t) slot].exchange (next.get());
        const auto e = epoch.load();
        if (owners[(size_t) slot] != nullptr)
            retired.emplace_back (std::move (owners[(size_t) slot]), e);
        owners[(size_t) slot] = std::move (next);

        {
            const juce::ScopedLock sl (lock);
            overviews[(size_t) slot] = std::move (overview);
        }
        version.fetch_add (1);
        reclaim();
    }

    void SampleBank::reclaim()
    {
        // Retired at epoch e: if e was even no block was running, so nothing can still hold the buffer; if odd,
        // the block running then has finished once the epoch has moved past e.
        const auto now = epoch.load();
        retired.erase (std::remove_if (retired.begin(), retired.end(),
                                       [now] (const auto& r) { return (r.second % 2) == 0 || now > r.second; }),
                       retired.end());
    }
}
