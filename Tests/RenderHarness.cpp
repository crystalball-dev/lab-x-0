// LabX0Render: drives the LAB X-0 processor offline.
//
//   LabX0Render --out <file.wav> [--preset <n|name>] [--clear-patterns] [--pattern "<text>"] [--pattern-index 0]
//               [--pad 1=<ref>] [--break 1=<ref>] [--events "36@0-0.5:127,38@0.5-1"] [--notes 36,38]
//               [--seconds 4] [--hold 2] [--velocity 0.8] [--sr 48000] [--block 512] [--bpm 174] [--free]
//               [--jump "<at s>:<to quarter>"] [--library <dir>] [--specimens <dir>] [--pack <dir>]
//               [--set id=value ...] [--cc "1:127@0.5"] [--program-at "4@2.5"] [--tail-from <s>] [--no-trim]
//               [--png <file.png> [--png-wait <ms>]] [--roundtrip] [--fault-log <file>] [--inject <stage>@<s>]
//               [--multi-out <prefix>]   (all 21 outputs; each extra output with sound goes to <prefix>_outK.wav)
//               [--slices 1=0,11952,23952]   (hand-placed slice starts for break slot A, in samples)
//               [--explode 1[:5]]            (break A's components to pads 1.. (or 5..) and the first empty pattern)
// --pad and --break take <ref>[@offset[,length]] (seconds): a region of the file.
//   LabX0Render --fuzz --seconds 300 [--seed 1] [--vary-blocks]
//   LabX0Render --selftest-buffer
//   LabX0Render --analyze <file> [--estimate-only] [--host-bpm 174] [--sens 0.5] [--slicing 0]
//   LabX0Render --index-test <dir> [--cache <file>]
//   LabX0Render --search-test <entries>
//   LabX0Render --categorise-file <labels.tsv>
//   LabX0Render --check-kit | --check-presets | --settings-test
//   LabX0Render --tempo-batch <list.tsv>          (lines: path<TAB>tagged bpm; prints estimate, octave nearest the tag)
//   LabX0Render --slice-batch <list.txt> [--sens 0.5]   (lines: path; prints sample rate, slice count, slice starts)
//   LabX0Render --index-dump <dir> [--count 300] [--seed 1]
//
// --init resets every parameter to its default and clears the kit and all patterns (after --preset, before the
// rest), so a test starts from a known, neutral state. --pat <index>:<text> may repeat.
//   LabX0Render --list | --list-params | --list-kit
//
// <ref> is "USER:path/inside/library", "ZONE:...", "PACK:...", "KIT:<name>" or an absolute file path.
// --bpm attaches a playing transport at that tempo from beat 0. Output is trimmed by the reported latency (unless
// --no-trim), so an event at t seconds is heard at t seconds in the file. Prints key=value statistics.

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "FactoryPresets.h"
#include "KitCatalog.h"
#include "AudioAnalysis.h"

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

using namespace labx0;

namespace
{
    struct NoteEvent
    {
        int note;
        double start, end;
        float velocity;
    };

    // A host transport at a fixed tempo; optionally jumps once to another quarter position.
    struct TestPlayHead final : public juce::AudioPlayHead
    {
        double bpm = 120.0, sampleRate = 48000.0;
        juce::int64 position = 0;
        bool playing = true;
        double jumpAtSeconds = -1.0, jumpToQuarter = 0.0;

        double quarterAt (juce::int64 samples) const
        {
            const double seconds = (double) samples / sampleRate;
            if (jumpAtSeconds >= 0.0 && seconds >= jumpAtSeconds)
                return jumpToQuarter + (seconds - jumpAtSeconds) * bpm / 60.0;
            return seconds * bpm / 60.0;
        }

        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            info.setBpm (bpm);
            info.setTimeInSamples (position);
            info.setPpqPosition (quarterAt (position));
            info.setIsPlaying (playing);
            return info;
        }
    };

    juce::String option (const juce::StringArray& args, const juce::String& key, const juce::String& fallback = {})
    {
        const int i = args.indexOf (key);
        return (i >= 0 && i + 1 < args.size()) ? args[i + 1] : fallback;
    }

    juce::StringArray allOptions (const juce::StringArray& args, const juce::String& key)
    {
        juce::StringArray out;
        for (int i = 0; i + 1 < args.size(); ++i)
            if (args[i] == key)
                out.add (args[i + 1]);
        return out;
    }

    int presetIndex (const juce::String& text)
    {
        const auto& presets = factoryPresets();
        if (text.containsOnly ("0123456789"))
            return juce::jlimit (0, (int) presets.size() - 1, text.getIntValue());
        for (int i = 0; i < (int) presets.size(); ++i)
            if (presets[(size_t) i].name.equalsIgnoreCase (text))
                return i;
        return -1;
    }

    bool setParam (LabX0AudioProcessor& proc, const juce::String& assignment)
    {
        const auto id = assignment.upToFirstOccurrenceOf ("=", false, false).trim();
        const auto value = assignment.fromFirstOccurrenceOf ("=", false, false).trim().getFloatValue();
        if (auto* p = proc.apvts.getParameter (id))
        {
            p->setValueNotifyingHost (p->convertTo0to1 (value));
            return true;
        }
        std::cerr << "unknown parameter: " << id << std::endl;
        return false;
    }

    std::vector<float> paramSnapshot (LabX0AudioProcessor& proc)
    {
        std::vector<float> v;
        for (const auto& s : paramSpecs())
            v.push_back (proc.apvts.getRawParameterValue (s.id)->load());
        return v;
    }

    SampleRef refFrom (LabX0AudioProcessor& proc, const juce::String& text)
    {
        if (juce::File::isAbsolutePath (text))
            return proc.getLibrary().makeRef (juce::File (text));
        return proc.resolveKitRef (text);
    }

    double ticksToMs (juce::int64 ticks)
    {
        return juce::Time::highResolutionTicksToSeconds (ticks) * 1000.0;
    }

    struct BlockTimer
    {
        std::vector<double> ms, budget;
        void add (double elapsedMs, double budgetMs) { ms.push_back (elapsedMs); budget.push_back (budgetMs); }
        juce::String summary() const
        {
            if (ms.empty())
                return {};
            auto sorted = ms;
            std::sort (sorted.begin(), sorted.end());
            const double p999 = sorted[(size_t) (0.999 * (double) (sorted.size() - 1))];
            int overruns = 0;
            for (size_t i = 0; i < ms.size(); ++i)
                if (ms[i] > 0.5 * budget[i])
                    ++overruns;
            return " block_max_ms=" + juce::String (sorted.back(), 3) + " block_p999_ms=" + juce::String (p999, 3)
                 + " half_budget_overruns=" + juce::String (overruns);
        }
    };

    // Processing time of this thread only, so a busy machine does not distort the real-time factor.
    double threadCpuSeconds()
    {
       #if JUCE_WINDOWS
        FILETIME created, exited, kernel, user;
        if (GetThreadTimes (GetCurrentThread(), &created, &exited, &kernel, &user))
        {
            ULARGE_INTEGER k, u;
            k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
            u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
            return (double) (k.QuadPart + u.QuadPart) * 1.0e-7;
        }
       #endif
        return juce::Time::getMillisecondCounterHiRes() * 0.001;
    }

    bool writeWav (const juce::File& file, const juce::AudioBuffer<float>& buffer, double sampleRate)
    {
        file.getParentDirectory().createDirectory();
        file.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
        auto options = juce::AudioFormatWriterOptions().withSampleRate (sampleRate)
                                                       .withNumChannels (buffer.getNumChannels())
                                                       .withBitsPerSample (32)
                                                       .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
        if (auto writer = wav.createWriterFor (stream, options))
            return writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
        return false;
    }

    void printSlots (LabX0AudioProcessor& proc)
    {
        auto& bank = proc.getBank();
        for (int s = 0; s < previewSlot; ++s)
        {
            if (bank.getRef (s).isEmpty())
                continue;
            std::cout << " slot" << s << "=\"" << bank.getStatusText (s) << "\" slot" << s << "_status=" << (int) bank.getStatus (s);
        }
    }

    //==============================================================================
    int selfTestBuffer()
    {
        // HistoryBuffer reads against a double-precision reference, including delays that cross the wrap-around
        // point. The slot past the end holds a 1e6 sentinel, so any stray read shows up as a huge error.
        dsp::HistoryBuffer h;
        const int size = 1931;
        h.prepare (size);
        juce::Random rnd (0);
        std::vector<double> histL ((size_t) size, 0.0);
        double worst = 0.0;
        long checks = 0;
        for (int step = 0; step < 300000; ++step)
        {
            const float x = rnd.nextFloat() * 2.0f - 1.0f;
            h.push (x, -x);
            histL[(size_t) (step % size)] = x;
            const int written = step + 1;
            const double delays[] = { rnd.nextDouble() * (size - 6), (double) (h.write) + 1.0e-5, (double) (h.write) - 1.0e-5,
                                      (double) h.maxDelay(), 0.0, 0.5, 1.0 };
            for (double d : delays)
            {
                d = std::clamp (d, 0.0, (double) h.maxDelay());
                if (d + 3.0 >= written)
                    continue;
                const int whole = (int) d;
                const double f = d - whole;
                auto at = [&] (int k) { return histL[(size_t) (((written - 1 - k) % size + size) % size)]; };
                const double x0 = whole >= 1 ? at (whole - 1) : at (whole), x1 = at (whole), x2 = at (whole + 1), x3 = at (whole + 2);
                const double c1 = 0.5 * (x2 - x0), c2 = x0 - 2.5 * x1 + 2.0 * x2 - 0.5 * x3, c3 = 0.5 * (x3 - x0) + 1.5 * (x1 - x2);
                const double ref = ((c3 * f + c2) * f + c1) * f + x1;
                float l, r;
                h.read (d, l, r);
                worst = std::max (worst, std::max (std::abs ((double) l - ref), std::abs ((double) r + ref)));
                ++checks;
            }
        }
        std::cout << "buffer_checks=" << checks << " buffer_worst_error=" << worst << std::endl;
        return worst < 1.0e-3 ? 0 : 1;
    }

    int analyseFile (const juce::StringArray& args)
    {
        Library library;
        const auto target = option (args, "--analyze");
        auto file = juce::File::getCurrentWorkingDirectory().getChildFile (target);
        if (! file.existsAsFile() && ! juce::File::isAbsolutePath (target))
        {
            SampleRef ref;
            if (target.startsWith ("KIT:"))
            {
                if (const auto* e = findKitEntry (target.substring (4)))
                    ref = { e->root, e->path };
            }
            else
                ref = SampleRef::fromString (target);
            file = library.resolve (ref);
        }
        SampleBank bank (library);
        const double hostBpm = option (args, "--host-bpm", "120").getDoubleValue();
        auto result = bank.decode (file, option (args, "--offset", "0").getDoubleValue(), true, hostBpm, 0.0, nullptr);
        std::cout << "status=" << (int) result.status << " message=\"" << result.message << "\"";
        if (result.data == nullptr)
        {
            std::cout << std::endl;
            return 1;
        }
        const auto& d = *result.data;
        std::cout << " length=" << d.length << " sr=" << d.sampleRate << " seconds=" << juce::String ((double) d.length / d.sampleRate, 4)
                  << " bpm=" << juce::String (d.bpm, 3) << " beats=" << juce::String (d.beats, 3) << " source=" << d.tempoSource
                  << " onsets=" << (int) d.onsetPos.size() << " gain=" << juce::String (d.normaliseGain, 4);

        std::vector<float> mono ((size_t) d.length);
        for (int i = 0; i < d.length; ++i)
            mono[(size_t) i] = 0.5f * (d.l()[i] + d.r()[i]);
        const auto env = analysis::computeEnvelope (mono.data(), d.length, d.sampleRate);
        const double estimate = analysis::estimateTempo (env, (double) d.length / d.sampleRate);
        std::cout << " estimate=" << juce::String (estimate, 3) << " estimate_octave=" << juce::String (analysis::nearestOctave (estimate, hostBpm), 3);

        const float sens = option (args, "--sens", "0.5").getFloatValue();
        const double beats = d.beats > 0.0 ? d.beats : 4.0;
        const double loopBpm = beats * 60.0 / ((double) d.length / d.sampleRate);
        const int minSpacing = (int) (60.0 / std::max (20.0, loopBpm) / 8.0 * d.sampleRate);
        std::vector<int> slices ((size_t) maxSlices), scratch (d.onsetPos.size() + 1);
        const int count = analysis::onsetSlices (d.onsetPos.data(), d.onsetStrength.data(), (int) d.onsetPos.size(), sens, minSpacing,
                                                 d.length, slices.data(), maxSlices, scratch.data());
        std::cout << " slices=" << count << " slice_pos=";
        for (int i = 0; i < count; ++i)
            std::cout << (i ? "," : "") << slices[(size_t) i];
        std::cout << std::endl;
        return 0;
    }

    // The break slicer's default slice points, as SLICE mode uses them, for many files.
    int sliceBatch (const juce::StringArray& args)
    {
        Library library;
        SampleBank bank (library);
        const float sens = option (args, "--sens", "0.5").getFloatValue();
        for (const auto& line : juce::StringArray::fromLines (juce::File (option (args, "--slice-batch")).loadFileAsString()))
        {
            const auto path = line.trim();
            if (path.isEmpty())
                continue;
            auto result = bank.decode (juce::File (path), 0.0, true, 120.0, 0.0, nullptr);
            if (result.data == nullptr)
            {
                std::cout << "ERR	0		" << path << std::endl;
                continue;
            }
            const auto& d = *result.data;
            const double beats = d.beats > 0.0 ? d.beats : 4.0;
            const double loopBpm = beats * 60.0 / ((double) d.length / d.sampleRate);
            const int minSpacing = (int) (60.0 / std::max (20.0, loopBpm) / 8.0 * d.sampleRate);
            std::vector<int> slices ((size_t) maxSlices), scratch (d.onsetPos.size() + 1);
            const int count = analysis::onsetSlices (d.onsetPos.data(), d.onsetStrength.data(), (int) d.onsetPos.size(), sens, minSpacing,
                                                     d.length, slices.data(), maxSlices, scratch.data());
            std::cout << d.sampleRate << "	" << count << "	";
            for (int i = 0; i < count; ++i)
                std::cout << (i ? "," : "") << slices[(size_t) i];
            std::cout << "	" << path << std::endl;
        }
        return 0;
    }

    int indexTest (const juce::StringArray& args)
    {
        const juce::File dir (option (args, "--index-test"));
        const juce::File cache (option (args, "--cache", juce::File::getSpecialLocation (juce::File::tempDirectory)
                                                             .getChildFile ("labx0_index_test.cache").getFullPathName()));
        std::atomic<int> found { 0 }, headers { 0 };

        auto t0 = juce::Time::getMillisecondCounterHiRes();
        auto pass1 = Library::scanFolder (Root::user, dir, nullptr, nullptr, &found, nullptr, false);
        auto t1 = juce::Time::getMillisecondCounterHiRes();
        auto full = Library::scanFolder (Root::user, dir, nullptr, nullptr, &found, &headers, true);
        auto t2 = juce::Time::getMillisecondCounterHiRes();
        if (full == nullptr)
        {
            std::cout << "index_failed" << std::endl;
            return 1;
        }
        Library::saveCache (*full, cache);
        auto t3 = juce::Time::getMillisecondCounterHiRes();
        auto loaded = Library::loadCache (cache, Root::user, dir);
        auto t4 = juce::Time::getMillisecondCounterHiRes();
        auto rescan = Library::scanFolder (Root::user, dir, loaded, nullptr, &found, &headers, true);
        auto t5 = juce::Time::getMillisecondCounterHiRes();

        std::array<int, numCategories> cats {};
        int withSeconds = 0, unsupported = 0, longPaths = 0, withBpm = 0;
        for (const auto& e : full->entries)
        {
            ++cats[(size_t) std::min ((int) e.category, numCategories - 1)];
            withSeconds += e.seconds > 0.0f ? 1 : 0;
            unsupported += (e.flags & flagUnsupported) ? 1 : 0;
            longPaths += (e.flags & flagLongPath) ? 1 : 0;
            withBpm += e.bpm > 0.0f ? 1 : 0;
        }

        Query q;
        q.text = option (args, "--query", "amen");
        auto t6 = juce::Time::getMillisecondCounterHiRes();
        const auto hits = runQuery (*full, q, nullptr);
        auto t7 = juce::Time::getMillisecondCounterHiRes();

        std::cout << "entries=" << full->entries.size() << " folders=" << full->text->folders.size()
                  << " pass1_ms=" << juce::String (t1 - t0, 1) << " full_ms=" << juce::String (t2 - t1, 1)
                  << " save_ms=" << juce::String (t3 - t2, 1) << " cache_load_ms=" << juce::String (t4 - t3, 1)
                  << " cached_entries=" << (loaded != nullptr ? (int) loaded->entries.size() : -1)
                  << " rescan_with_cache_ms=" << juce::String (t5 - t4, 1)
                  << " rescan_entries=" << (rescan != nullptr ? (int) rescan->entries.size() : -1)
                  << " with_seconds=" << withSeconds << " with_bpm=" << withBpm << " unsupported=" << unsupported
                  << " long_paths=" << longPaths << " query=\"" << q.text << "\" hits=" << hits.size()
                  << " query_ms=" << juce::String (t7 - t6, 2) << " pass1_entries=" << (pass1 != nullptr ? (int) pass1->entries.size() : -1);
        for (int c = 1; c < numCategories; ++c)
            std::cout << " cat_" << juce::String (categoryNames[c]).toLowerCase() << "=" << cats[(size_t) c];
        std::cout << " cat_none=" << cats[0] << std::endl;
        return 0;
    }

    int searchTest (const juce::StringArray& args)
    {
        const int n = option (args, "--search-test", "300000").getIntValue();
        static const char* words[] = { "kick", "snare", "hat", "amen", "break", "loop", "perc", "crash", "ride", "vox", "fx", "riser",
                                       "dnb", "jungle", "174", "deep", "dirty", "tape", "vinyl", "funky", "soul", "drum", "tom", "clap" };
        juce::Random rnd (7);
        auto index = std::make_shared<RootIndex>();
        index->root = Root::user;
        auto text = std::make_shared<IndexStrings>();
        for (int f = 0; f < 4000; ++f)
        {
            const auto folder = juce::String ("Pack ") + words[rnd.nextInt (24)] + " " + juce::String (f) + "/" + words[rnd.nextInt (24)];
            text->folders.push_back (folder);
            text->foldersLower.push_back (folder.toLowerCase().toStdString());
        }
        for (int i = 0; i < n; ++i)
        {
            const auto name = (juce::String (words[rnd.nextInt (24)]) + "_" + words[rnd.nextInt (24)] + "_" + juce::String (i) + ".wav").toStdString();
            IndexEntry e;
            e.folder = (uint32_t) rnd.nextInt (4000);
            e.nameOffset = (uint32_t) text->names.size();
            e.nameLength = (uint16_t) name.size();
            text->names.insert (text->names.end(), name.begin(), name.end());
            text->namesLower.insert (text->namesLower.end(), name.begin(), name.end());
            e.category = (uint8_t) (1 + rnd.nextInt (8));
            index->entries.push_back (e);
        }
        index->text = text;

        double worst = 0.0;
        juce::String detail;
        for (auto query : { "amen", "kick dirty", "174 break jungle", "zzz", "a" })
        {
            Query q;
            q.text = query;
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            const auto hits = runQuery (*index, q, nullptr);
            const auto ms = juce::Time::getMillisecondCounterHiRes() - t0;
            worst = std::max (worst, ms);
            detail << " q_" << juce::String (query).replaceCharacter (' ', '_') << "=" << juce::String (ms, 2) << "ms/" << (int) hits.size();
        }
        Query cat;
        cat.category = catBreak;
        cat.text = "amen";
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        const auto hits = runQuery (*index, cat, nullptr);
        const auto ms = juce::Time::getMillisecondCounterHiRes() - t0;
        worst = std::max (worst, ms);
        std::cout << "entries=" << n << " worst_ms=" << juce::String (worst, 2) << detail << " q_break_amen=" << juce::String (ms, 2)
                  << "ms/" << (int) hits.size() << std::endl;
        return 0;
    }

    int categoriseFile (const juce::StringArray& args)
    {
        const juce::File file (option (args, "--categorise-file"));
        int total = 0, correct = 0;
        juce::StringArray misses;
        for (const auto& line : juce::StringArray::fromLines (file.loadFileAsString()))
        {
            // label<TAB>relative path[<TAB>seconds]
            const auto cols = juce::StringArray::fromTokens (line, "\t", "");
            const auto label = cols[0].trim().toUpperCase();
            const auto path = cols[1].trim();
            if (label.isEmpty() || path.isEmpty() || label.startsWith ("#"))
                continue;
            const auto folder = path.upToLastOccurrenceOf ("/", false, false).toStdString();
            const auto name = path.fromLastOccurrenceOf ("/", false, false).toStdString();
            const float seconds = cols.size() > 2 ? cols[2].getFloatValue() : -1.0f;
            const auto c = categorise (name, folder, Root::user, seconds);
            ++total;
            if (label == categoryNames[c] || (label == "NONE" && c == catNone))
                ++correct;
            else
                misses.add (label + " -> " + (c == catNone ? juce::String ("NONE") : juce::String (categoryNames[c])) + "  " + path);
        }
        std::cout << "labelled=" << total << " correct=" << correct << " accuracy=" << juce::String (total > 0 ? 100.0 * correct / total : 0.0, 1) << std::endl;
        for (int i = 0; i < std::min (40, misses.size()); ++i)
            std::cout << "  miss " << misses[i] << std::endl;
        return 0;
    }

    //==============================================================================
    int runFuzz (LabX0AudioProcessor& proc, TestPlayHead& head, double sampleRate, int baseBlock, double seconds, int seed, bool varyBlocks)
    {
        juce::Random rnd (seed);
        const auto& specs = paramSpecs();
        const int total = (int) (seconds * sampleRate);
        const double cycle = 30.0, activeSpan = 20.0;
        juce::AudioBuffer<float> block (2, 4096);

        std::vector<int> held;
        int events = 0, presetChanges = 0, paramChanges = 0, patternEdits = 0;
        long nonFinite = 0;
        float peak = 0.0f;
        BlockTimer timer;

        const int window = (int) (0.1 * sampleRate);
        double windowSum = 0.0;
        int windowCount = 0, windowIndex = 0, idleStretches = 0, growth = 0;
        bool inIdle = false;
        float minSoFar = 1000.0f, worstGrowth = -1000.0f;
        double nextEvent = 0.0;

        int pos = 0;
        while (pos < total)
        {
            const int n = varyBlocks ? std::min (total - pos, 16 + rnd.nextInt (1009)) : std::min (baseBlock, total - pos);
            const double t0 = (double) pos / sampleRate;
            const bool idleNow = std::fmod (t0, cycle) >= activeSpan;
            juce::MidiBuffer midi;

            if (idleNow && ! inIdle)
            {
                midi.addEvent (juce::MidiMessage::allNotesOff (1), 0);
                held.clear();
                head.playing = false;
                inIdle = true;
                ++idleStretches;
                windowSum = 0.0;
                windowCount = windowIndex = 0;
                minSoFar = 1000.0f;
            }
            else if (! idleNow && inIdle)
            {
                inIdle = false;
                head.playing = true;
            }

            if (idleNow)
                nextEvent = (std::floor (t0 / cycle) + 1.0) * cycle;
            else
            {
                const double blockEnd = t0 + (double) n / sampleRate;
                while (nextEvent < blockEnd)
                {
                    const int offset = juce::jlimit (0, n - 1, (int) ((nextEvent - t0) * sampleRate));
                    const float r = rnd.nextFloat();
                    if (r < 0.35f && held.size() < 12)
                    {
                        const int note = rnd.nextBool() ? midiFirstPad + rnd.nextInt (16) : (rnd.nextBool() ? midiFirstSlice + rnd.nextInt (32) : midiFirstBreak + rnd.nextInt (4));
                        midi.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) (20 + rnd.nextInt (108))), offset);
                        held.push_back (note);
                    }
                    else if (r < 0.55f && ! held.empty())
                    {
                        const int k = rnd.nextInt ((int) held.size());
                        midi.addEvent (juce::MidiMessage::noteOff (1, held[(size_t) k]), offset);
                        held.erase (held.begin() + k);
                    }
                    else if (r < 0.58f)
                    {
                        proc.setCurrentProgram (rnd.nextInt (proc.getNumPrograms()));
                        ++presetChanges;
                    }
                    else if (r < 0.75f)
                    {
                        const auto& s = specs[(size_t) rnd.nextInt ((int) specs.size())];
                        if (s.id != "run")
                            if (auto* p = proc.apvts.getParameter (s.id))
                                p->setValueNotifyingHost (rnd.nextFloat());
                        ++paramChanges;
                    }
                    else if (r < 0.88f)
                    {
                        // Random pattern edits, including the TIME and VOLUME lanes.
                        const int pat = rnd.nextInt (numPatterns);
                        proc.getPatterns().edit (pat, [&rnd] (Pattern& p)
                        {
                            p.length = (uint8_t) (rnd.nextBool() ? 16 : 8 + rnd.nextInt (57));
                            p.resolution = (uint8_t) rnd.nextInt (numResolutions);
                            const int lane = rnd.nextInt (numLanes);
                            auto& st = p.lanes[(size_t) lane].steps[(size_t) rnd.nextInt (maxSteps)];
                            if (lane == laneTime)        st.slot = (uint8_t) rnd.nextInt (Slot::numTime);
                            else if (lane == laneVolume) st.slot = (uint8_t) rnd.nextInt (Slot::numVolume);
                            else
                            {
                                st.on = rnd.nextBool() ? 1 : 0;
                                st.ratchet = (uint8_t) (1 + rnd.nextInt (4));
                                st.nudge = (int8_t) (rnd.nextInt (101) - 50);
                                st.slice = (int8_t) (rnd.nextInt (40) - 1);
                                st.probability = (uint8_t) (1 + rnd.nextInt (100));
                                st.pitch = (int8_t) (rnd.nextInt (49) - 24);
                            }
                            if (rnd.nextFloat() < 0.1f)
                                p.lanes[(size_t) lane].length = (uint8_t) rnd.nextInt (65);
                        });
                        ++patternEdits;
                    }
                    else if (r < 0.93f)
                        midi.addEvent (juce::MidiMessage::controllerEvent (1, 1, rnd.nextInt (128)), offset);
                    else if (r < 0.97f)
                        midi.addEvent (juce::MidiMessage::noteOn (1, midiFirstPattern + rnd.nextInt (16), (juce::uint8) 100), offset);
                    else
                    {
                        midi.addEvent (juce::MidiMessage::allNotesOff (1), offset);
                        held.clear();
                    }
                    ++events;
                    nextEvent += 0.03 + rnd.nextDouble() * 0.4;
                }
            }

            juce::AudioBuffer<float> view (block.getArrayOfWritePointers(), 2, 0, n);
            head.position = pos;
            const auto start = juce::Time::getHighResolutionTicks();
            proc.processBlock (view, midi);
            timer.add (ticksToMs (juce::Time::getHighResolutionTicks() - start), 1000.0 * (double) n / sampleRate);
            if (rnd.nextFloat() < 0.002f)
                proc.flushFaultLog();

            const float* l = view.getReadPointer (0);
            const float* r = view.getReadPointer (1);
            for (int i = 0; i < n; ++i)
            {
                if (! std::isfinite (l[i]) || ! std::isfinite (r[i]))
                {
                    ++nonFinite;
                    continue;
                }
                peak = std::max (peak, std::max (std::abs (l[i]), std::abs (r[i])));
                if (inIdle)
                {
                    windowSum += (double) (l[i] * l[i] + r[i] * r[i]);
                    if (++windowCount >= window)
                    {
                        const float rmsDb = juce::Decibels::gainToDecibels ((float) std::sqrt (windowSum / (2.0 * windowCount)), -240.0f);
                        if (windowIndex >= 40 && rmsDb > -80.0f)   // after 4 s: every tail has had time to die
                        {
                            const float g = rmsDb - minSoFar;
                            worstGrowth = std::max (worstGrowth, g);
                            if (g > 6.0f)
                                ++growth;
                        }
                        minSoFar = std::min (minSoFar, rmsDb);
                        windowSum = 0.0;
                        windowCount = 0;
                        ++windowIndex;
                    }
                }
            }
            pos += n;
        }

        std::cout << "fuzz seed=" << seed << " seconds=" << seconds << " events=" << events << " preset_changes=" << presetChanges
                  << " param_changes=" << paramChanges << " pattern_edits=" << patternEdits << " idle_stretches=" << idleStretches
                  << " growth_idle=" << growth << " worst_growth_db=" << juce::String (worstGrowth, 1) << " non_finite=" << nonFinite
                  << " peak_db=" << juce::String (juce::Decibels::gainToDecibels (peak, -120.0f), 2)
                  << " faults=" << proc.meters.faults.load() << " faults_logged=" << proc.flushFaultLog() << timer.summary() << std::endl;
        return (nonFinite == 0 && growth == 0) ? 0 : 1;
    }
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI gui;

    juce::StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add (juce::String::fromUTF8 (argv[i]));

    if (args.contains ("--selftest-buffer"))
        return selfTestBuffer();
    if (args.contains ("--analyze"))
        return analyseFile (args);
    if (args.contains ("--index-test"))
        return indexTest (args);
    if (args.contains ("--slice-batch"))
        return sliceBatch (args);
    if (args.contains ("--search-test"))
        return searchTest (args);
    if (args.contains ("--categorise-file"))
        return categoriseFile (args);

    if (args.contains ("--list"))
    {
        const auto& presets = factoryPresets();
        for (int i = 0; i < (int) presets.size(); ++i)
            std::cout << i << "\t" << presets[(size_t) i].name << std::endl;
        return 0;
    }
    if (args.contains ("--settings-test"))
    {
        // Roots persist in the settings file and come back in a new Library; a missing root reports itself.
        const auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("labx0_settings_test");
        temp.deleteRecursively();
        temp.createDirectory();
        const auto settingsFile = temp.getChildFile ("test.settings");
        const auto folder = temp.getChildFile ("library");
        folder.createDirectory();
        Library::settingsFileOverride = settingsFile;
        juce::String fails;
        {
            Library a;
            a.setRoot (Root::user, folder, true);
            a.setFavourite ({ Root::user, "Drums/kick.wav" }, true);
        }
        {
            Library b;
            if (b.getRoot (Root::user) != folder)
                fails << " root_not_restored";
            if (! b.isFavourite ({ Root::user, "Drums/kick.wav" }))
                fails << " favourite_not_restored";
            b.setRoot (Root::user, temp.getChildFile ("does_not_exist"), false);
            b.requestScan (Root::user);
            juce::Thread::sleep (400);
            const auto status = b.getStatusText (Root::user);
            if (! status.contains ("NOT FOUND"))
                fails << " missing_status=" << status.replaceCharacter (' ', '_');
            SampleBank bank (b);
            bank.assign (0, { Root::user, "Drums/kick.wav" });
            bank.waitUntilSettled (5000);
            const auto slot = bank.getStatusText (0);
            if (bank.getStatus (0) == SampleBank::Status::ready || ! slot.contains ("USER LIBRARY NOT FOUND"))
                fails << " slot_status=" << slot.replaceCharacter (' ', '_');
            std::cout << "settings_file=" << settingsFile.getFullPathName() << " missing_root_status=\"" << status << "\" slot_status=\"" << slot << "\"";
        }
        Library::settingsFileOverride = juce::File();
        temp.deleteRecursively();
        std::cout << " settings_test=" << (fails.isEmpty() ? "PASS" : "FAIL" + fails) << std::endl;
        return fails.isEmpty() ? 0 : 1;
    }
    if (args.contains ("--index-dump"))
    {
        // A random sample of indexed paths (for labelling the category test).
        auto index = Library::scanFolder (Root::user, juce::File (option (args, "--index-dump")), nullptr, nullptr, nullptr, nullptr, true);
        if (index == nullptr)
            return 1;
        juce::Random rnd (option (args, "--seed", "1").getIntValue());
        const int count = option (args, "--count", "300").getIntValue();
        for (int k = 0; k < count && ! index->entries.empty(); ++k)
        {
            const auto& e = index->entries[(size_t) rnd.nextInt ((int) index->entries.size())];
            std::cout << categoryNames[e.category] << "\t" << index->relativePath (e) << "\t" << juce::String (e.seconds, 2) << std::endl;
        }
        return 0;
    }
    if (args.contains ("--check-kit"))
    {
        // Every ZONE KIT entry resolves, decodes and is loud enough to use.
        Library library;
        SampleBank bank (library);
        int ready = 0, failed = 0;
        for (int i = 0; i < kitCatalogSize; ++i)
        {
            const auto& e = kitCatalog[i];
            const auto file = library.resolve ({ e.root, e.path });
            auto result = bank.decode (file, 0.0, false, 120.0, 0.0, nullptr);
            const bool ok = result.data != nullptr && result.data->length > 0;
            ok ? ++ready : ++failed;
            std::cout << (ok ? "OK	" : "FAIL	") << e.name << "	";
            if (ok)
                std::cout << juce::String ((double) result.data->length / result.data->sampleRate, 3) << "	"
                          << juce::String (juce::Decibels::gainToDecibels (0.891f / std::max (1.0e-9f, result.data->normaliseGain)), 1);
            else
                std::cout << result.message;
            std::cout << std::endl;
        }
        std::cout << "kit_entries=" << kitCatalogSize << " kit_ready=" << ready << " kit_failed=" << failed << std::endl;
        return failed == 0 ? 0 : 1;
    }
    if (args.contains ("--tempo-batch"))
    {
        Library library;
        SampleBank bank (library);
        for (const auto& line : juce::StringArray::fromLines (juce::File (option (args, "--tempo-batch")).loadFileAsString()))
        {
            const auto path = line.upToFirstOccurrenceOf ("	", false, false).trim();
            const double truth = line.fromFirstOccurrenceOf ("	", false, false).getDoubleValue();
            if (path.isEmpty())
                continue;
            auto result = bank.decode (juce::File (path), 0.0, false, truth > 0.0 ? truth : 120.0, 0.0, nullptr);
            if (result.data == nullptr)
            {
                std::cout << "ERR	ERR	" << path << std::endl;
                continue;
            }
            const auto& d = *result.data;
            std::vector<float> mono ((size_t) d.length);
            for (int i = 0; i < d.length; ++i)
                mono[(size_t) i] = 0.5f * (d.l()[i] + d.r()[i]);
            const auto env = analysis::computeEnvelope (mono.data(), d.length, d.sampleRate);
            const double estimate = analysis::estimateTempo (env, (double) d.length / d.sampleRate);
            std::cout << juce::String (estimate, 3) << "	" << juce::String (analysis::nearestOctave (estimate, truth), 3) << "	" << path << std::endl;
        }
        return 0;
    }
    if (args.contains ("--check-presets"))
    {
        // Every preset pattern parses and every sample reference resolves to a file on this machine.
        auto proc = std::make_unique<LabX0AudioProcessor>();
        int problems = 0, samples = 0, missing = 0;
        const auto& presets = factoryPresets();
        for (int i = 0; i < (int) presets.size(); ++i)
        {
            const auto& pr = presets[(size_t) i];
            for (const auto& [index, text] : pr.patterns)
            {
                Pattern pat;
                juce::String error;
                if (! parsePatternText (text, pat, error))
                {
                    std::cout << "PATTERN " << pr.name << " #" << index << ": " << error << std::endl;
                    ++problems;
                }
            }
            for (int s = 0; s < previewSlot; ++s)
            {
                const auto& text = pr.samples[(size_t) s];
                if (text.isEmpty())
                    continue;
                ++samples;
                const auto ref = proc->resolveKitRef (text);
                const auto file = proc->getLibrary().resolve (ref);
                if (ref.isEmpty() || ! file.existsAsFile())
                {
                    std::cout << "MISSING " << pr.name << " slot " << s << ": " << text << std::endl;
                    ++missing;
                }
            }
            for (const auto& [id, value] : pr.values)
                if (paramIndexForId (id) < 0)
                {
                    std::cout << "PARAM " << pr.name << ": unknown id " << id << std::endl;
                    ++problems;
                }
        }
        std::cout << "presets=" << presets.size() << " samples=" << samples << " missing=" << missing << " problems=" << problems << std::endl;
        return (problems == 0 && missing == 0) ? 0 : 1;
    }
    if (args.contains ("--list-kit"))
    {
        for (int i = 0; i < kitCatalogSize; ++i)
        {
            const auto& e = kitCatalog[i];
            std::cout << i << "\t" << e.name << "\t" << kitGroupNames[(int) e.group] << "\t" << rootTag (e.root) << ":" << e.path << std::endl;
        }
        return 0;
    }
    if (args.contains ("--list-params"))
    {
        int count = 0;
        for (const auto& s : paramSpecs())
        {
            std::cout << s.id << "\t" << s.name << std::endl;
            ++count;
        }
        std::cout << "param_count=" << count << std::endl;
        return count == P::count ? 0 : 1;
    }

    const double sampleRate = option (args, "--sr", "48000").getDoubleValue();
    const int blockSize = option (args, "--block", "512").getIntValue();
    const double seconds = option (args, "--seconds", "4").getDoubleValue();
    const double hold = option (args, "--hold", "2").getDoubleValue();
    const float velocity = option (args, "--velocity", "0.8").getFloatValue();
    const juce::File outFile (option (args, "--out"));
    const int prepareBlock = args.contains ("--vary-blocks") ? 4096 : blockSize;

    auto proc = std::make_unique<LabX0AudioProcessor>();
    if (args.contains ("--fault-log"))
        proc->setFaultLogFile (juce::File (option (args, "--fault-log")));
    if (args.contains ("--library"))
        proc->setLibraryOverride (Root::user, juce::File (option (args, "--library")));
    if (args.contains ("--specimens"))
        proc->setLibraryOverride (Root::zone, juce::File (option (args, "--specimens")));
    if (args.contains ("--pack"))
        proc->setLibraryOverride (Root::pack, juce::File (option (args, "--pack")));

    TestPlayHead playHead;
    playHead.sampleRate = sampleRate;
    const bool useTransport = args.contains ("--bpm") || args.contains ("--fuzz");
    if (useTransport)
    {
        playHead.bpm = option (args, "--bpm", "174").getDoubleValue();
        if (args.contains ("--jump"))
        {
            const auto spec = option (args, "--jump");
            playHead.jumpAtSeconds = spec.upToFirstOccurrenceOf (":", false, false).getDoubleValue();
            playHead.jumpToQuarter = spec.fromFirstOccurrenceOf (":", false, false).getDoubleValue();
        }
        proc->setPlayHead (&playHead);
    }

    if (args.contains ("--preset"))
    {
        const int index = presetIndex (option (args, "--preset"));
        if (index < 0)
        {
            std::cerr << "unknown preset" << std::endl;
            return 2;
        }
        proc->setCurrentProgram (index);
    }

    if (args.contains ("--init"))
    {
        for (const auto& spec : paramSpecs())
            if (auto* p = proc->apvts.getParameter (spec.id))
                p->setValueNotifyingHost (p->getDefaultValue());
        proc->getPatterns().editAll ([] (PatternBank& b) { for (auto& p : b.patterns) p.clear(); });
        for (int s = 0; s < previewSlot; ++s)
            proc->assignSample (s, {});
    }

    if (args.contains ("--clear-patterns"))
        proc->getPatterns().editAll ([] (PatternBank& b) { for (auto& p : b.patterns) p.clear(); });
    if (args.contains ("--clear-kit"))
        for (int s = 0; s < previewSlot; ++s)
            proc->assignSample (s, {});

    if (args.contains ("--pattern"))
    {
        Pattern pat;
        juce::String error;
        if (! parsePatternText (option (args, "--pattern"), pat, error))
        {
            std::cerr << "pattern: " << error << std::endl;
            return 2;
        }
        const int index = option (args, "--pattern-index", "0").getIntValue();
        proc->getPatterns().edit (index, [&pat] (Pattern& p) { p = pat; });
    }

    for (const auto& a : allOptions (args, "--pat"))
    {
        Pattern pat;
        juce::String error;
        if (! parsePatternText (a.fromFirstOccurrenceOf (":", false, false), pat, error))
        {
            std::cerr << "pattern: " << error << std::endl;
            return 2;
        }
        proc->getPatterns().edit (a.upToFirstOccurrenceOf (":", false, false).getIntValue(), [&pat] (Pattern& p) { p = pat; });
    }

    // <ref>[@offset[,length]] in seconds: a region of the file.
    auto assignFrom = [&proc] (int slot, const juce::String& value)
    {
        const auto where = value.contains ("@") ? value.fromLastOccurrenceOf ("@", false, false) : juce::String();
        const auto ref = value.contains ("@") ? value.upToLastOccurrenceOf ("@", false, false) : value;
        proc->assignSample (slot, refFrom (*proc, ref), where.upToFirstOccurrenceOf (",", false, false).getDoubleValue(),
                            where.contains (",") ? where.fromFirstOccurrenceOf (",", false, false).getDoubleValue() : 0.0);
    };
    for (const auto& a : allOptions (args, "--pad"))
        assignFrom (a.upToFirstOccurrenceOf ("=", false, false).getIntValue() - 1, a.fromFirstOccurrenceOf ("=", false, false));
    for (const auto& a : allOptions (args, "--break"))
        assignFrom (slotFirstBreak + a.upToFirstOccurrenceOf ("=", false, false).getIntValue() - 1, a.fromFirstOccurrenceOf ("=", false, false));
    for (const auto& a : allOptions (args, "--slices"))
    {
        std::vector<int> positions;
        for (const auto& token : juce::StringArray::fromTokens (a.fromFirstOccurrenceOf ("=", false, false), ",", ""))
            positions.push_back (token.getIntValue());
        proc->setCustomSlices (a.upToFirstOccurrenceOf ("=", false, false).getIntValue() - 1, positions);
    }

    for (const auto& assignment : allOptions (args, "--set"))
        if (! setParam (*proc, assignment))
            return 2;
    if (args.contains ("--free"))
        setParam (*proc, "run=1");

    // --multi-out renders every output bus, as a host that enabled them all; otherwise the main stereo output only,
    // and lanes set to another output fall back to it.
    const bool multiOut = args.contains ("--multi-out");
    if (multiOut)
        proc->setRateAndBufferSizeDetails (sampleRate, prepareBlock);
    else
        proc->setPlayConfigDetails (0, 2, sampleRate, prepareBlock);
    const int outChannels = multiOut ? proc->getTotalNumOutputChannels() : 2;
    proc->prepareToPlay (sampleRate, prepareBlock);

    // One empty block, then wait for every sample and start clean.
    {
        juce::AudioBuffer<float> warm (2, blockSize);
        juce::MidiBuffer none;
        const bool wasPlaying = playHead.playing;
        playHead.playing = false;
        proc->processBlock (warm, none);
        playHead.playing = wasPlaying;
        proc->getBank().waitUntilSettled (120000);
        proc->prepareToPlay (sampleRate, prepareBlock);
    }

    if (args.contains ("--explode"))
    {
        // --explode <break slot>[:<first pad>], both 1-based: the break's components to pads and a pattern.
        const auto spec = option (args, "--explode");
        const int b = spec.upToFirstOccurrenceOf (":", false, false).getIntValue() - 1;
        const int firstPad = spec.contains (":") ? spec.fromFirstOccurrenceOf (":", false, false).getIntValue() - 1 : 0;
        int pattern = -1;
        const auto plan = proc->explodeBreakNow (b, firstPad, args.contains ("--every-slice"), &pattern);
        std::cout << "explode=\"" << plan.message << "\" explode_ok=" << (plan.ok ? 1 : 0) << " explode_pattern=" << (pattern + 1)
                  << " explode_steps=" << plan.steps << " explode_components=";
        for (size_t k = 0; k < plan.components.size(); ++k)
            std::cout << (k ? "," : "") << explodeKindNames[(int) plan.components[k].kind] << "x" << plan.components[k].count;
        std::cout << std::endl;
        proc->getBank().waitUntilSettled (120000);
    }

    if (args.contains ("--fuzz"))
    {
        const int result = runFuzz (*proc, playHead, sampleRate, blockSize, seconds, option (args, "--seed", "1").getIntValue(),
                                    args.contains ("--vary-blocks"));
        proc->setPlayHead (nullptr);
        proc->releaseResources();
        return result;
    }

    std::vector<NoteEvent> events;
    if (args.contains ("--events"))
    {
        for (const auto& token : juce::StringArray::fromTokens (option (args, "--events"), ",", ""))
        {
            const int note = token.upToFirstOccurrenceOf ("@", false, false).getIntValue();
            auto times = token.fromFirstOccurrenceOf ("@", false, false);
            float vel = velocity;
            if (times.contains (":"))
            {
                vel = juce::jlimit (1.0f, 127.0f, times.fromFirstOccurrenceOf (":", false, false).getFloatValue()) / 127.0f;
                times = times.upToFirstOccurrenceOf (":", false, false);
            }
            events.push_back ({ note, times.upToFirstOccurrenceOf ("-", false, false).getDoubleValue(),
                                times.fromFirstOccurrenceOf ("-", false, false).getDoubleValue(), vel });
        }
    }
    else if (args.contains ("--notes"))
    {
        for (const auto& token : juce::StringArray::fromTokens (option (args, "--notes"), ",", ""))
            events.push_back ({ token.getIntValue(), 0.0, hold, velocity });
    }

    struct ProgramChange { int index; double time; };
    std::vector<ProgramChange> programChanges;
    for (const auto& token : juce::StringArray::fromTokens (option (args, "--program-at"), ",", ""))
        programChanges.push_back ({ token.upToFirstOccurrenceOf ("@", false, false).getIntValue(),
                                    token.fromFirstOccurrenceOf ("@", false, false).getDoubleValue() });

    struct CcEvent { int cc, value; double time; };
    std::vector<CcEvent> ccs;
    for (const auto& token : juce::StringArray::fromTokens (option (args, "--cc"), ",", ""))
    {
        const auto head = token.upToFirstOccurrenceOf ("@", false, false);
        ccs.push_back ({ head.upToFirstOccurrenceOf (":", false, false).getIntValue(), head.fromFirstOccurrenceOf (":", false, false).getIntValue(),
                         token.fromFirstOccurrenceOf ("@", false, false).getDoubleValue() });
    }

    if (args.contains ("--inject"))
    {
        const auto spec = option (args, "--inject");
        proc->injectStage = spec.upToFirstOccurrenceOf ("@", false, false).getIntValue();
        proc->injectAtSample = (juce::int64) (spec.fromFirstOccurrenceOf ("@", false, false).getDoubleValue() * sampleRate);
    }

    const int latency = args.contains ("--no-trim") ? 0 : proc->getLatencySamples();
    const int total = (int) (seconds * sampleRate);
    const int rendered = total + latency;
    juce::AudioBuffer<float> raw (outChannels, rendered);
    raw.clear();
    BlockTimer timer;

    const auto cpu0 = threadCpuSeconds();
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    for (int pos = 0; pos < rendered; pos += blockSize)
    {
        const int n = std::min (blockSize, rendered - pos);
        for (const auto& pc : programChanges)
        {
            const int at = (int) (pc.time * sampleRate);
            if (at >= pos && at < pos + n)
                proc->setCurrentProgram (pc.index);
        }

        juce::AudioBuffer<float> view (raw.getArrayOfWritePointers(), outChannels, pos, n);
        juce::MidiBuffer midi;
        for (const auto& e : events)
        {
            const int on = (int) (e.start * sampleRate);
            const int off = (int) (e.end * sampleRate);
            if (on >= pos && on < pos + n)
                midi.addEvent (juce::MidiMessage::noteOn (1, e.note, e.velocity), on - pos);
            if (off >= pos && off < pos + n)
                midi.addEvent (juce::MidiMessage::noteOff (1, e.note), off - pos);
        }
        for (const auto& c : ccs)
        {
            const int at = (int) (c.time * sampleRate);
            if (at >= pos && at < pos + n)
                midi.addEvent (juce::MidiMessage::controllerEvent (1, c.cc, c.value), at - pos);
        }
        playHead.position = pos;
        const auto start = juce::Time::getHighResolutionTicks();
        proc->processBlock (view, midi);
        timer.add (ticksToMs (juce::Time::getHighResolutionTicks() - start), 1000.0 * (double) n / sampleRate);
    }
    const double elapsedMs = juce::Time::getMillisecondCounterHiRes() - t0;
    const double cpuSeconds = threadCpuSeconds() - cpu0;

    juce::AudioBuffer<float> output (2, total);
    for (int ch = 0; ch < 2; ++ch)
        output.copyFrom (ch, 0, raw, ch, latency, total);

    int nonFinite = 0;
    float peak = 0.0f, maxJump = 0.0f;
    double sumSquares = 0.0;
    for (int ch = 0; ch < 2; ++ch)
    {
        const float* d = output.getReadPointer (ch);
        for (int i = 0; i < total; ++i)
        {
            if (! std::isfinite (d[i]))
            {
                ++nonFinite;
                continue;
            }
            peak = std::max (peak, std::abs (d[i]));
            sumSquares += (double) d[i] * d[i];
            if (i > 0)
                maxJump = std::max (maxJump, std::abs (d[i] - d[i - 1]));
        }
    }
    const double rms = std::sqrt (sumSquares / std::max (1, total * 2));
    const double audioSeconds = (double) rendered / sampleRate;

    std::cout << "preset=\"" << proc->getProgramName (proc->getCurrentProgram()) << "\""
              << " latency=" << proc->getLatencySamples()
              << " seconds=" << seconds
              << " peak_db=" << juce::String (juce::Decibels::gainToDecibels (peak, -120.0f), 2)
              << " rms_db=" << juce::String (juce::Decibels::gainToDecibels ((float) rms, -120.0f), 2)
              << " max_jump=" << juce::String (maxJump, 4)
              << " non_finite=" << nonFinite
              << " faults=" << proc->meters.faults.load()
              << " faults_logged=" << proc->flushFaultLog()
              << " pad_triggers=" << proc->padTriggers << " break_triggers=" << proc->breakTriggers
              << " render_ms=" << juce::String (elapsedMs, 1)
              << " realtime_x=" << juce::String (audioSeconds * 1000.0 / std::max (0.001, elapsedMs), 1);
    if (cpuSeconds >= 0.05)
        std::cout << " cpu_realtime_x=" << juce::String (audioSeconds / cpuSeconds, 1);
    std::cout << timer.summary();
    printSlots (*proc);

    if (args.contains ("--tail-from"))
    {
        const int from = juce::jlimit (0, total, (int) (option (args, "--tail-from").getDoubleValue() * sampleRate));
        const int window = (int) (0.1 * sampleRate);
        float tailPeak = 0.0f, minSoFar = 1000.0f, worstRise = -1000.0f;
        int index = 0;
        for (int w = from; w + window <= total; w += window, ++index)
        {
            double sum = 0.0;
            for (int ch = 0; ch < 2; ++ch)
            {
                const float* d = output.getReadPointer (ch);
                for (int i = w; i < w + window; ++i)
                {
                    sum += (double) d[i] * d[i];
                    tailPeak = std::max (tailPeak, std::abs (d[i]));
                }
            }
            const float rmsDb = juce::Decibels::gainToDecibels ((float) std::sqrt (sum / (2.0 * window)), -240.0f);
            if (index >= 5 && rmsDb > -80.0f)
                worstRise = std::max (worstRise, rmsDb - minSoFar);
            minSoFar = std::min (minSoFar, rmsDb);
        }
        std::cout << " tail_peak_db=" << juce::String (juce::Decibels::gainToDecibels (tailPeak, -120.0f), 2)
                  << " tail_rise_db=" << juce::String (worstRise, 1);
    }
    std::cout << std::endl;

    if (outFile != juce::File() && ! writeWav (outFile, output, sampleRate))
    {
        std::cerr << "could not write " << outFile.getFullPathName() << std::endl;
        return 3;
    }

    if (multiOut)
    {
        const auto prefix = option (args, "--multi-out");
        std::cout << "outputs=" << proc->getBusCount (false);
        for (int k = 1; k < proc->getBusCount (false); ++k)
        {
            const int ch = proc->getChannelIndexInProcessBlockBuffer (false, k, 0);
            if (ch < 0 || ch + 1 >= outChannels)
                continue;
            juce::AudioBuffer<float> auxOut (2, total);
            for (int c = 0; c < 2; ++c)
                auxOut.copyFrom (c, 0, raw, ch + c, latency, total);
            const float pk = std::max (auxOut.getMagnitude (0, 0, total), auxOut.getMagnitude (1, 0, total));
            int bad = 0;
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < total; ++i)
                    bad += std::isfinite (auxOut.getSample (c, i)) ? 0 : 1;
            if (pk <= 0.0f && bad == 0)
                continue;
            std::cout << " out" << (k + 1) << "_peak_db=" << juce::String (juce::Decibels::gainToDecibels (pk, -120.0f), 2)
                      << " out" << (k + 1) << "_non_finite=" << bad;
            if (prefix.isNotEmpty() && ! writeWav (juce::File (prefix + "_out" + juce::String (k + 1) + ".wav"), auxOut, sampleRate))
                return 3;
        }
        std::cout << std::endl;
    }

    if (args.contains ("--roundtrip"))
    {
        juce::MemoryBlock state;
        proc->getStateInformation (state);
        const auto before = paramSnapshot (*proc);
        juce::String patternsBefore;
        proc->getPatterns().read ([&patternsBefore] (const PatternBank& b) { for (const auto& p : b.patterns) patternsBefore << patternToText (p) << "\n--\n"; });
        juce::StringArray refsBefore;
        for (int s = 0; s < previewSlot; ++s)
            refsBefore.add (proc->getSampleRef (s).toString() + "@" + juce::String (proc->getBank().getOffset (s), 3)
                            + "," + juce::String (proc->getBank().getLength (s), 3));

        auto other = std::make_unique<LabX0AudioProcessor>();
        other->setCurrentProgram ((proc->getCurrentProgram() + 1) % other->getNumPrograms());
        other->setStateInformation (state.getData(), (int) state.getSize());
        const auto after = paramSnapshot (*other);
        juce::String patternsAfter;
        other->getPatterns().read ([&patternsAfter] (const PatternBank& b) { for (const auto& p : b.patterns) patternsAfter << patternToText (p) << "\n--\n"; });

        int mismatches = 0;
        for (size_t i = 0; i < before.size(); ++i)
            if (std::abs (before[i] - after[i]) > 1.0e-5f)
            {
                ++mismatches;
                std::cout << "roundtrip mismatch: " << paramSpecs()[i].id << " " << before[i] << " vs " << after[i] << std::endl;
            }
        for (int s = 0; s < previewSlot; ++s)
        {
            const auto restored = other->getSampleRef (s).toString() + "@" + juce::String (other->getBank().getOffset (s), 3)
                                + "," + juce::String (other->getBank().getLength (s), 3);
            if (refsBefore[s] != restored)
            {
                ++mismatches;
                std::cout << "roundtrip mismatch: slot " << s << " " << refsBefore[s] << " vs " << restored << std::endl;
            }
        }
        if (patternsBefore != patternsAfter)
        {
            ++mismatches;
            std::cout << "roundtrip mismatch: patterns" << std::endl;
        }
        for (int b = 0; b < numBreaks; ++b)
            if (proc->getCustomSlices (b) != other->getCustomSlices (b))
            {
                ++mismatches;
                std::cout << "roundtrip mismatch: slices of break " << b << std::endl;
            }
        std::cout << "roundtrip=" << (mismatches == 0 ? "PASS" : "FAIL") << " state_bytes=" << (int) state.getSize() << std::endl;
        if (mismatches != 0)
            return 4;
    }

    if (args.contains ("--png"))
    {
        if (args.contains ("--ui-lane"))
            proc->uiSelectedLane.store (juce::jlimit (0, numLanes - 1, option (args, "--ui-lane").getIntValue()));
        std::unique_ptr<juce::AudioProcessorEditor> editor (proc->createEditor());
        if (args.contains ("--png-wait"))
        {
            // Let the browser's timer, the library scan and the search callbacks run, as they would in a host.
            editor->setVisible (true);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (option (args, "--png-wait").getIntValue());
        }
        if (auto* labEditor = dynamic_cast<LabX0AudioProcessorEditor*> (editor.get()))
            for (int i = 0; i < 12; ++i)
                labEditor->refreshFromProcessor();
        const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
        const juce::File pngFile (option (args, "--png"));
        pngFile.deleteFile();
        juce::FileOutputStream stream (pngFile);
        juce::PNGImageFormat png;
        if (! stream.openedOk() || ! png.writeImageToStream (image, stream))
        {
            std::cerr << "could not write " << pngFile.getFullPathName() << std::endl;
            return 5;
        }
        std::cout << "png=" << pngFile.getFullPathName() << " size=" << image.getWidth() << "x" << image.getHeight() << std::endl;
        editor.reset();
    }

    proc->setPlayHead (nullptr);
    proc->releaseResources();
    return nonFinite == 0 ? 0 : 1;
}
