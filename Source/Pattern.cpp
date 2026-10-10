#include <algorithm>
#include <numeric>
#include "Pattern.h"

namespace labx0
{
    namespace
    {
        constexpr uint32_t magic = 0x5030584c;   // "LX0P"
        constexpr uint32_t formatVersion = 1;

        Step sanitised (Step s, bool effectLane) noexcept
        {
            s.on = s.on != 0 ? 1 : 0;
            s.velocity = (uint8_t) juce::jlimit (1, 127, (int) s.velocity);
            s.probability = (uint8_t) juce::jlimit (1, 100, (int) s.probability);
            s.ratchet = (uint8_t) juce::jlimit (1, 4, (int) s.ratchet);
            s.nudge = (int8_t) juce::jlimit (-50, 50, (int) s.nudge);
            s.pitch = (int8_t) juce::jlimit (-24, 24, (int) s.pitch);
            s.slice = (int8_t) juce::jlimit (-1, maxSlices - 1, (int) s.slice);
            if (effectLane)
                s.on = 0;
            return s;
        }

        int laneIndexFromName (const juce::String& name)
        {
            const auto n = name.trim().toLowerCase();
            if (n == "t" || n == "time")
                return laneTime;
            if (n == "v" || n == "vol" || n == "volume")
                return laneVolume;
            if (n.startsWith ("p"))
            {
                const int pad = n.substring (1).getIntValue();
                if (n.substring (1).containsOnly ("0123456789") && juce::isPositiveAndNotGreaterThan (pad, numPads) && pad > 0)
                    return pad - 1;
            }
            if (n.startsWith ("b") && n.length() == 2)
            {
                const auto c = n[1];
                if (c >= '1' && c <= '4')
                    return laneFirstBreak + (int) (c - '1');
                if (c >= 'a' && c <= 'd')
                    return laneFirstBreak + (int) (c - 'a');
            }
            return -1;
        }

        int digit36 (juce::juce_wchar c)
        {
            if (c >= '0' && c <= '9') return (int) (c - '0');
            if (c >= 'a' && c <= 'z') return 10 + (int) (c - 'a');
            return -1;
        }

        juce::juce_wchar char36 (int v)
        {
            return v < 10 ? (juce::juce_wchar) ('0' + v) : (juce::juce_wchar) ('a' + (v - 10));
        }

        juce::String laneName (int lane)
        {
            if (lane < numPads)       return "p" + juce::String (lane + 1);
            if (lane < laneTime)      return "b" + juce::String (lane - laneFirstBreak + 1);
            return lane == laneTime ? "t" : "v";
        }

        // Steps written as characters: spaces are ignored, so long rows can be grouped "x... x... x...".
        juce::String compact (const juce::String& s)
        {
            return s.removeCharacters (" \t|");
        }
    }

    //==============================================================================
    bool Pattern::laneHasContent (int lane) const noexcept
    {
        const auto& l = lanes[(size_t) lane];
        const bool effect = lane >= laneTime;
        for (const auto& s : l.steps)
            if (effect ? s.slot != 0 : s.on != 0)
                return true;
        return false;
    }

    bool Pattern::isEmpty() const noexcept
    {
        for (int lane = 0; lane < numLanes; ++lane)
            if (laneHasContent (lane))
                return false;
        return true;
    }

    void Pattern::clear() noexcept
    {
        *this = Pattern();
    }

    //==============================================================================
    juce::MemoryBlock serialisePatterns (const PatternBank& bank)
    {
        juce::MemoryOutputStream raw;
        raw.writeInt ((int) magic);
        raw.writeInt ((int) formatVersion);
        raw.writeInt (numPatterns);
        raw.writeInt (numLanes);
        raw.writeInt (maxSteps);

        for (const auto& p : bank.patterns)
        {
            raw.writeByte ((char) p.length);
            raw.writeByte ((char) p.resolution);
            for (const auto& lane : p.lanes)
            {
                raw.writeByte ((char) lane.length);
                raw.writeByte ((char) lane.mute);
                for (const auto& s : lane.steps)
                {
                    raw.writeByte ((char) s.on);
                    raw.writeByte ((char) s.velocity);
                    raw.writeByte ((char) s.probability);
                    raw.writeByte ((char) s.ratchet);
                    raw.writeByte ((char) s.nudge);
                    raw.writeByte ((char) s.pitch);
                    raw.writeByte ((char) s.slice);
                    raw.writeByte ((char) s.slot);
                }
            }
        }

        juce::MemoryOutputStream packed;
        {
            juce::GZIPCompressorOutputStream gz (packed, 9);
            gz.write (raw.getData(), raw.getDataSize());
            gz.flush();
        }
        return packed.getMemoryBlock();
    }

    bool deserialisePatterns (const void* data, size_t size, PatternBank& out)
    {
        if (data == nullptr || size < 8)
            return false;

        juce::MemoryInputStream packed (data, size, false);
        juce::GZIPDecompressorInputStream gz (packed);
        juce::MemoryBlock rawBlock;
        gz.readIntoMemoryBlock (rawBlock);
        juce::MemoryInputStream in (rawBlock, false);

        if ((uint32_t) in.readInt() != magic)
            return false;
        const auto version = (uint32_t) in.readInt();
        if (version < 1 || version > formatVersion)
            return false;
        const int patterns = in.readInt();
        const int lanes = in.readInt();
        const int steps = in.readInt();
        if (patterns <= 0 || patterns > 256 || lanes <= 0 || lanes > 256 || steps <= 0 || steps > 1024)
            return false;

        PatternBank bank;
        for (int p = 0; p < patterns; ++p)
        {
            Pattern pat;
            pat.length = (uint8_t) juce::jlimit (1, maxSteps, (int) (uint8_t) in.readByte());
            pat.resolution = (uint8_t) juce::jlimit (0, (int) numResolutions - 1, (int) (uint8_t) in.readByte());
            for (int l = 0; l < lanes; ++l)
            {
                Lane lane;
                lane.length = (uint8_t) juce::jlimit (0, maxSteps, (int) (uint8_t) in.readByte());
                lane.mute = (uint8_t) (in.readByte() != 0 ? 1 : 0);
                for (int s = 0; s < steps; ++s)
                {
                    Step st;
                    st.on = (uint8_t) in.readByte();
                    st.velocity = (uint8_t) in.readByte();
                    st.probability = (uint8_t) in.readByte();
                    st.ratchet = (uint8_t) in.readByte();
                    st.nudge = (int8_t) in.readByte();
                    st.pitch = (int8_t) in.readByte();
                    st.slice = (int8_t) in.readByte();
                    st.slot = (uint8_t) in.readByte();
                    if (s < maxSteps)
                        lane.steps[(size_t) s] = sanitised (st, l >= laneTime);
                }
                if (l < numLanes)
                {
                    if (l == laneTime)
                        for (auto& st : lane.steps) st.slot = (uint8_t) std::min ((int) st.slot, (int) Slot::numTime - 1);
                    if (l == laneVolume)
                        for (auto& st : lane.steps) st.slot = (uint8_t) std::min ((int) st.slot, (int) Slot::numVolume - 1);
                    if (l < laneTime)
                        for (auto& st : lane.steps) st.slot = 0;
                    pat.lanes[(size_t) l] = lane;
                }
            }
            if (in.isExhausted() && p < patterns - 1)
                return false;
            if (p < numPatterns)
                bank.patterns[(size_t) p] = pat;
        }

        out = bank;
        return true;
    }

    //==============================================================================
    bool parsePatternText (const juce::String& text, Pattern& out, juce::String& error)
    {
        Pattern p;
        auto lines = juce::StringArray::fromTokens (text.replace (";", "\n"), "\n", "");

        for (auto rawLine : lines)
        {
            auto line = rawLine.trim();
            if (line.isEmpty() || line.startsWith ("#"))
                continue;

            // Pattern settings: "len=32 res=1/16"
            if (line.startsWithIgnoreCase ("len=") || line.startsWithIgnoreCase ("res="))
            {
                for (const auto& token : juce::StringArray::fromTokens (line, " ", ""))
                {
                    if (token.startsWithIgnoreCase ("len="))
                        p.length = (uint8_t) juce::jlimit (1, maxSteps, token.substring (4).getIntValue());
                    else if (token.startsWithIgnoreCase ("res="))
                    {
                        const auto r = token.substring (4).toUpperCase();
                        bool found = false;
                        for (int i = 0; i < numResolutions; ++i)
                            if (r == juce::String (resolutionNames[i]).toUpperCase())
                            {
                                p.resolution = (uint8_t) i;
                                found = true;
                            }
                        if (! found)
                        {
                            error = "unknown resolution " + r;
                            return false;
                        }
                    }
                }
                continue;
            }

            // Lane attributes: "p1.len=12", "p1.mute=1"
            if (line.contains ("=") && ! line.contains (":"))
            {
                const auto key = line.upToFirstOccurrenceOf ("=", false, false).trim();
                const auto value = line.fromFirstOccurrenceOf ("=", false, false).trim().getIntValue();
                const int lane = laneIndexFromName (key.upToFirstOccurrenceOf (".", false, false));
                const auto attr = key.fromFirstOccurrenceOf (".", false, false).toLowerCase();
                if (lane < 0)
                {
                    error = "unknown lane in " + line;
                    return false;
                }
                if (attr == "len")
                    p.lanes[(size_t) lane].length = (uint8_t) juce::jlimit (0, maxSteps, value);
                else if (attr == "mute")
                    p.lanes[(size_t) lane].mute = (uint8_t) (value != 0 ? 1 : 0);
                else
                {
                    error = "unknown attribute in " + line;
                    return false;
                }
                continue;
            }

            const auto head = line.upToFirstOccurrenceOf (":", false, false).trim();
            const auto body = line.fromFirstOccurrenceOf (":", false, false);
            const int lane = laneIndexFromName (head.upToFirstOccurrenceOf (".", false, false));
            if (lane < 0)
            {
                error = "unknown lane " + head;
                return false;
            }
            const auto attr = head.contains (".") ? head.fromFirstOccurrenceOf (".", false, false).toLowerCase() : juce::String();
            auto& steps = p.lanes[(size_t) lane].steps;

            if (attr.isEmpty())
            {
                const auto row = compact (body);
                for (int i = 0; i < row.length() && i < maxSteps; ++i)
                {
                    const auto c = row[i];
                    auto& s = steps[(size_t) i];
                    if (lane == laneTime || lane == laneVolume)
                    {
                        s.slot = 0;
                        const auto* table = lane == laneTime ? timeSlots : volumeSlots;
                        const int count = lane == laneTime ? (int) Slot::numTime : (int) Slot::numVolume;
                        bool found = c == '.';
                        for (int k = 1; k < count && ! found; ++k)
                            if (table[k].text == (char) c)
                            {
                                s.slot = (uint8_t) k;
                                found = true;
                            }
                        if (! found)
                        {
                            error = "unknown slot character in " + line;
                            return false;
                        }
                    }
                    else if (lane >= laneFirstBreak)
                    {
                        if (c == '.') { s.on = 0; continue; }
                        s.on = 1;
                        if (c == 'x')      s.slice = -1;
                        else if (c == 'X') { s.slice = -1; s.velocity = 127; }
                        else
                        {
                            const int d = digit36 (c);
                            if (d < 0)
                            {
                                error = "unknown slice character in " + line;
                                return false;
                            }
                            s.slice = (int8_t) d;
                        }
                    }
                    else
                    {
                        switch (c)
                        {
                            case '.': s.on = 0; break;
                            case 'x': s.on = 1; s.velocity = 100; break;
                            case 'X': s.on = 1; s.velocity = 127; break;
                            case 'o': s.on = 1; s.velocity = 50; break;
                            case '2': case '3': case '4': s.on = 1; s.ratchet = (uint8_t) (c - '0'); break;
                            case '?': s.on = 1; s.probability = 50; break;
                            default:
                                error = "unknown step character in " + line;
                                return false;
                        }
                    }
                }
                continue;
            }

            if (attr == "pitch")
            {
                const auto values = juce::StringArray::fromTokens (body.trim(), " ", "");
                for (int i = 0; i < values.size() && i < maxSteps; ++i)
                    if (values[i] != ".")
                        steps[(size_t) i].pitch = (int8_t) juce::jlimit (-24, 24, values[i].getIntValue());
                continue;
            }

            const auto row = compact (body);
            for (int i = 0; i < row.length() && i < maxSteps; ++i)
            {
                const auto c = row[i];
                if (c == '.')
                    continue;
                auto& s = steps[(size_t) i];
                const int d = (c >= '0' && c <= '9') ? (int) (c - '0') : -1;
                if (attr == "vel" && d >= 1)
                    s.velocity = (uint8_t) juce::jlimit (1, 127, juce::roundToInt ((float) d / 9.0f * 127.0f));
                else if (attr == "prob" && d >= 0)
                    s.probability = (uint8_t) (d == 0 ? 100 : d * 10);
                else if (attr == "rat" && d >= 1 && d <= 4)
                    s.ratchet = (uint8_t) d;
                else if (attr == "nudge" && d >= 1 && d <= 5)
                    s.nudge = (int8_t) (d * 10);
                else if (attr == "nudge" && c >= 'a' && c <= 'e')
                    s.nudge = (int8_t) (-10 * (int) (c - 'a' + 1));
                else
                {
                    error = "bad value '" + juce::String::charToString (c) + "' in " + line;
                    return false;
                }
            }
        }

        out = p;
        return true;
    }

    juce::String patternToText (const Pattern& p)
    {
        juce::StringArray lines;
        lines.add ("len=" + juce::String ((int) p.length) + " res=" + resolutionNames[p.resolution < numResolutions ? p.resolution : 1]);

        for (int lane = 0; lane < numLanes; ++lane)
        {
            if (! p.laneHasContent (lane))
                continue;
            const auto& l = p.lanes[(size_t) lane];
            const int len = p.laneLength (lane);
            juce::String row;
            for (int i = 0; i < len; ++i)
            {
                const auto& s = l.steps[(size_t) i];
                if (lane == laneTime)        row << juce::String::charToString ((juce::juce_wchar) timeSlots[std::min ((int) s.slot, (int) Slot::numTime - 1)].text);
                else if (lane == laneVolume) row << juce::String::charToString ((juce::juce_wchar) volumeSlots[std::min ((int) s.slot, (int) Slot::numVolume - 1)].text);
                else if (! s.on)             row << ".";
                else if (lane >= laneFirstBreak) row << (s.slice < 0 ? juce::String ("x") : juce::String::charToString (char36 (std::min (35, (int) s.slice))));
                else if (s.velocity >= 120)  row << "X";
                else if (s.velocity <= 60)   row << "o";
                else                         row << "x";
            }
            lines.add (laneName (lane) + ": " + row);
            if (l.length > 0)
                lines.add (laneName (lane) + ".len=" + juce::String ((int) l.length));
            if (l.mute)
                lines.add (laneName (lane) + ".mute=1");
        }
        return lines.joinIntoString ("\n");
    }

    //==============================================================================
    namespace PatternTools
    {
        void clearLane (Lane& lane)
        {
            for (auto& s : lane.steps)
                s = Step();
        }

        // Bjorklund's even distribution of `hits` over `length` steps.
        void euclid (Lane& lane, int length, int hits, int rotateBy)
        {
            length = juce::jlimit (1, maxSteps, length);
            hits = juce::jlimit (0, length, hits);
            for (int i = 0; i < length; ++i)
            {
                const int k = ((i - rotateBy) % length + length) % length;
                const bool on = hits > 0 && ((k * hits) % length) < hits;
                auto& s = lane.steps[(size_t) i];
                s.on = on ? 1 : 0;
                if (on)
                {
                    s.velocity = 100;
                    s.probability = 100;
                    s.ratchet = 1;
                }
            }
        }

        void randomFill (Lane& lane, int length, float density, uint32_t seed)
        {
            juce::Random rnd ((juce::int64) seed);
            length = juce::jlimit (1, maxSteps, length);
            for (int i = 0; i < length; ++i)
            {
                auto& s = lane.steps[(size_t) i];
                s.on = rnd.nextFloat() < density ? 1 : 0;
                if (s.on)
                {
                    s.velocity = (uint8_t) (70 + rnd.nextInt (58));
                    s.probability = 100;
                    s.ratchet = rnd.nextFloat() < 0.08f ? 2 : 1;
                }
            }
        }

        void rotate (Lane& lane, int length, int amount)
        {
            length = juce::jlimit (1, maxSteps, length);
            amount = ((amount % length) + length) % length;
            std::rotate (lane.steps.begin(), lane.steps.begin() + (length - amount), lane.steps.begin() + length);
        }

        void materialise (Lane& lane, int length, const std::function<int (int)>& naturalSlice)
        {
            for (int i = 0; i < std::min (length, maxSteps); ++i)
            {
                auto& s = lane.steps[(size_t) i];
                if (s.on && s.slice < 0)
                    s.slice = (int8_t) juce::jlimit (0, maxSlices - 1, naturalSlice (i));
            }
        }

        static std::vector<int> onSteps (const Lane& lane, int length)
        {
            std::vector<int> idx;
            for (int i = 0; i < std::min (length, maxSteps); ++i)
                if (lane.steps[(size_t) i].on)
                    idx.push_back (i);
            return idx;
        }

        void shuffleSlices (Lane& lane, int length, uint32_t seed)
        {
            auto idx = onSteps (lane, length);
            std::vector<int8_t> slices;
            for (int i : idx)
                slices.push_back (lane.steps[(size_t) i].slice);
            juce::Random rnd ((juce::int64) seed);
            for (int i = (int) slices.size() - 1; i > 0; --i)
                std::swap (slices[(size_t) i], slices[(size_t) rnd.nextInt (i + 1)]);
            for (size_t k = 0; k < idx.size(); ++k)
                lane.steps[(size_t) idx[k]].slice = slices[k];
        }

        void reverseSlices (Lane& lane, int length)
        {
            auto idx = onSteps (lane, length);
            for (size_t a = 0, b = idx.size(); a + 1 < b; ++a, --b)
                std::swap (lane.steps[(size_t) idx[a]].slice, lane.steps[(size_t) idx[b - 1]].slice);
        }

        // Every fourth hit repeats the hit before it as a double, the classic jungle roll.
        void rollSlices (Lane& lane, int length)
        {
            auto idx = onSteps (lane, length);
            for (size_t k = 3; k < idx.size(); k += 4)
            {
                auto& s = lane.steps[(size_t) idx[k]];
                s.slice = lane.steps[(size_t) idx[k - 1]].slice;
                s.ratchet = 2;
            }
        }

        // Half time: the first half of the lane spreads over the whole lane.
        void halfTime (Lane& lane, int length)
        {
            length = juce::jlimit (1, maxSteps, length);
            const auto old = lane.steps;
            for (int i = 0; i < length; ++i)
                lane.steps[(size_t) i] = (i % 2 == 0) ? old[(size_t) (i / 2)] : Step();
        }

        // Double time: the lane plays twice as fast, twice over.
        void doubleTime (Lane& lane, int length)
        {
            length = juce::jlimit (1, maxSteps, length);
            const auto old = lane.steps;
            for (int i = 0; i < length; ++i)
                lane.steps[(size_t) i] = old[(size_t) ((2 * i) % length)];
        }
    }
}
