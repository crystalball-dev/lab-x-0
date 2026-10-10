#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include "Library.h"

namespace labx0
{
    //==============================================================================
    juce::String SampleRef::toString() const
    {
        if (path.isEmpty())
            return {};
        return juce::String (rootTag (root)) + ":" + path;
    }

    SampleRef SampleRef::fromString (const juce::String& text)
    {
        SampleRef r;
        const auto t = text.trim();
        if (t.isEmpty())
            return r;

        const auto tag = t.upToFirstOccurrenceOf (":", false, false);
        const auto rest = t.fromFirstOccurrenceOf (":", false, false);
        if (tag == "USER")      { r.root = Root::user; r.path = rest; }
        else if (tag == "ZONE") { r.root = Root::zone; r.path = rest; }
        else if (tag == "PACK") { r.root = Root::pack; r.path = rest; }
        else if (tag == "FILE") { r.root = Root::none; r.path = rest; }
        else                    { r.root = Root::none; r.path = t; }   // a bare absolute path such as C:\x.wav
        return r;
    }

    juce::String SampleRef::fileName() const
    {
        return path.fromLastOccurrenceOf ("/", false, false).fromLastOccurrenceOf ("\\", false, false);
    }

    juce::String SampleRef::displayName() const
    {
        const auto name = fileName();
        return name.containsChar ('.') ? name.upToLastOccurrenceOf (".", false, false) : name;
    }

    //==============================================================================
    namespace
    {
        enum Ext : uint8_t { extNone = 0, extWav, extAiff, extFlac, extOgg, extMp3 };

        uint8_t extensionCode (const juce::String& fileName)
        {
            const auto ext = fileName.fromLastOccurrenceOf (".", false, false).toLowerCase();
            if (ext == "wav")                  return extWav;
            if (ext == "aif" || ext == "aiff") return extAiff;
            if (ext == "flac")                 return extFlac;
            if (ext == "ogg")                  return extOgg;
            if (ext == "mp3")                  return extMp3;
            return extNone;
        }

        std::string lowerUtf8 (const juce::String& s)
        {
            return s.toLowerCase().toStdString();
        }

        bool isAlnum (char c) noexcept
        {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        }

        bool has (std::string_view s, std::string_view token) noexcept
        {
            return s.find (token) != std::string_view::npos;
        }

        // A short token that must stand alone: "bd" in "bd_01" or "808 bd", not in "abduct".
        bool hasWord (std::string_view s, std::string_view w) noexcept
        {
            size_t pos = 0;
            while ((pos = s.find (w, pos)) != std::string_view::npos)
            {
                const bool startOk = pos == 0 || ! isAlnum (s[pos - 1]) || (s[pos - 1] >= '0' && s[pos - 1] <= '9');
                const size_t end = pos + w.size();
                const bool endOk = end >= s.size() || ! isAlnum (s[end]) || (s[end] >= '0' && s[end] <= '9');
                if (startOk && endOk)
                    return true;
                ++pos;
            }
            return false;
        }

        template <size_t N>
        bool hasAny (std::string_view s, const std::string_view (&tokens)[N]) noexcept
        {
            for (auto t : tokens)
                if (has (s, t))
                    return true;
            return false;
        }

        template <size_t N>
        bool hasAnyWord (std::string_view s, const std::string_view (&tokens)[N]) noexcept
        {
            for (auto t : tokens)
                if (hasWord (s, t))
                    return true;
            return false;
        }

        // Rules for generic sample packs, applied to a name or a folder path.
        uint8_t categoriseText (std::string_view s) noexcept
        {
            static constexpr std::string_view melodic[] = { "bass loop", "bassloop", "synth", "pad loop", "chord", "keys", "piano",
                                                             "guitar", "melod", "lead", "arp", "string", "horn", "brass", "organ" };
            static constexpr std::string_view breakTokens[] = { "break", "amen", "drum loop", "drumloop", "drum_loop", "beat loop",
                                                                 "groove", "bpm", "loop", "beats" };
            static constexpr std::string_view kick[] = { "kick", "kik", "bassdrum", "bass drum", "bass_drum" };
            static constexpr std::string_view kickWords[] = { "bd", "kck", "kd" };
            static constexpr std::string_view snare[] = { "snare", "snr", "clap", "rimshot" };
            static constexpr std::string_view snareWords[] = { "sd", "sn", "clp", "cp" };
            static constexpr std::string_view hat[] = { "hihat", "hi hat", "hi-hat", "hi_hat", "hat" };
            static constexpr std::string_view hatWords[] = { "hh", "oh", "ch", "ohh", "chh", "hhc", "hho" };
            static constexpr std::string_view cym[] = { "crash", "ride", "cymbal", "splash", "china" };
            static constexpr std::string_view cymWords[] = { "cym", "cy", "rd", "cr" };
            static constexpr std::string_view perc[] = { "perc", "tom", "conga", "bongo", "shaker", "tamb", "cowbell", "clave",
                                                         "rim", "woodblock", "block", "snap", "click", "guiro", "cabasa", "timbale",
                                                         "agogo", "triangle", "bell", "tabla", "djembe", "metal hit", "knock", "stick" };
            static constexpr std::string_view vox[] = { "vox", "vocal", "voice", "acapella", "acappella", "shout", "chant", "speech",
                                                        "spoken", "phrase", "adlib", "ad lib", "ad-lib", "talk", "dialog", "dialogue",
                                                        "scream", "yell", "hey", "choir" };
            static constexpr std::string_view fx[] = { "fx", "sfx", "riser", "uplifter", "downlifter", "impact", "sweep", "noise",
                                                       "whoosh", "swoosh", "scratch", "explosion", "gun", "laser", "zap", "glitch",
                                                       "siren", "atmos", "foley", "boom", "transition", "drop", "reverse", "rewind",
                                                       "vinyl", "tape stop", "stab", "hit", "radio" };

            const bool melodicLoop = hasAny (s, melodic);
            if (hasAny (s, breakTokens))
            {
                if (melodicLoop)
                    return catNone;
                if (has (s, "vocal") || has (s, "vox"))
                    return catVox;
                return catBreak;
            }
            if (hasAny (s, kick) || hasAnyWord (s, kickWords))   return catKick;
            if (hasAny (s, snare) || hasAnyWord (s, snareWords)) return catSnare;
            if (hasAny (s, hat) || hasAnyWord (s, hatWords))     return catHat;
            if (hasAny (s, cym) || hasAnyWord (s, cymWords))     return catCymbal;
            if (hasAny (s, vox))                                 return catVox;
            if (hasAny (s, perc))                                return catPerc;
            if (hasAny (s, fx))                                  return catFx;
            return catNone;
        }

        bool isVoiceFolder (std::string_view folder) noexcept
        {
            return has (folder, "characters_voice") || has (folder, "script_replics") || has (folder, "voiceover")
                || has (folder, "localization");
        }
    }

    uint8_t categorise (std::string_view name, std::string_view folder, Root root, float seconds)
    {
        // STALKER sources: voices are VOX, everything else is an effect unless its name says more.
        if (root == Root::zone || root == Root::pack)
        {
            if (isVoiceFolder (folder) || name.substr (0, 3) == "vo_")
                return catVox;
            if (has (folder, "/music") || has (folder, "music/"))
                return catNone;
            return catFx;
        }

        // The name decides first, then the deepest folder, then the whole path.
        const auto stem = name.substr (0, name.rfind ('.'));
        uint8_t c = categoriseText (stem);

        if (c == catNone)
        {
            const auto slash = folder.rfind ('/');
            const auto last = slash == std::string_view::npos ? folder : folder.substr (slash + 1);
            c = categoriseText (last);
            if (c == catNone)
                c = categoriseText (folder);
        }

        if (seconds > 0.0f)
        {
            // A long file filed with one-shots is a loop; a tiny file called a loop is a hit.
            if (c == catBreak && seconds < 0.6f)
                c = categoriseText (folder) == catBreak ? catPerc : categoriseText (folder);
            else if ((c == catKick || c == catSnare || c == catHat || c == catPerc) && seconds > 4.0f
                     && categoriseText (stem) == catNone)
                c = catBreak;
        }
        return c;
    }

    //==============================================================================
    // Header parsing: reads at most a few kilobytes at each end of the file.
    namespace
    {
        uint32_t le32 (const uint8_t* p) noexcept { return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24); }
        uint16_t le16 (const uint8_t* p) noexcept { return (uint16_t) (p[0] | (p[1] << 8)); }
        uint32_t be32 (const uint8_t* p) noexcept { return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | (uint32_t) p[3]; }
        uint16_t be16 (const uint8_t* p) noexcept { return (uint16_t) ((p[0] << 8) | p[1]); }

        int64_t oggTotalFrames (juce::FileInputStream& in)
        {
            const auto size = in.getTotalLength();
            const auto tail = (int) std::min<juce::int64> (size, 65536);
            std::vector<uint8_t> buf ((size_t) tail);
            if (! in.setPosition (size - tail) || in.read (buf.data(), tail) != tail)
                return -1;
            for (int i = tail - 27; i >= 0; --i)
            {
                if (buf[(size_t) i] == 'O' && buf[(size_t) i + 1] == 'g' && buf[(size_t) i + 2] == 'g' && buf[(size_t) i + 3] == 'S'
                    && buf[(size_t) i + 4] == 0)
                {
                    int64_t g = 0;
                    std::memcpy (&g, buf.data() + i + 6, 8);
                    if (g >= 0)
                        return g;
                }
            }
            return -1;
        }

        bool parseWav (juce::FileInputStream& in, IndexEntry& e, bool rf64)
        {
            const auto size = in.getTotalLength();
            uint16_t format = 0, channels = 0, blockAlign = 0;
            uint32_t rate = 0, factFrames = 0;
            int64_t dataSize = -1, ds64Data = -1;
            float acidTempo = 0.0f;
            in.setPosition (12);

            while (in.getPosition() + 8 <= size)
            {
                uint8_t h[8];
                if (in.read (h, 8) != 8)
                    break;
                const auto pos = in.getPosition();
                const uint32_t len = le32 (h + 4);
                uint8_t b[48] = {};
                const int want = (int) std::min<uint32_t> (len, sizeof (b));

                if (std::memcmp (h, "fmt ", 4) == 0)
                {
                    in.read (b, want);
                    format = le16 (b);
                    channels = le16 (b + 2);
                    rate = le32 (b + 4);
                    blockAlign = le16 (b + 12);
                    if (format == 0xfffe && len >= 40)
                        format = le16 (b + 24);   // the sub-format GUID starts with the real format tag
                }
                else if (std::memcmp (h, "fact", 4) == 0 && len >= 4)
                {
                    in.read (b, 4);
                    factFrames = le32 (b);
                }
                else if (std::memcmp (h, "ds64", 4) == 0 && len >= 16)
                {
                    in.read (b, 16);
                    std::memcpy (&ds64Data, b + 8, 8);
                }
                else if (std::memcmp (h, "data", 4) == 0)
                {
                    dataSize = (rf64 && len == 0xffffffffu) ? ds64Data : (int64_t) len;
                }
                else if (std::memcmp (h, "acid", 4) == 0 && len >= 24)
                {
                    in.read (b, 24);
                    std::memcpy (&acidTempo, b + 20, 4);
                }

                const int64_t chunk = (dataSize >= 0 && std::memcmp (h, "data", 4) == 0) ? dataSize : (int64_t) len;
                const int64_t next = pos + chunk + (chunk & 1);
                if (next <= pos || next >= size)
                    break;
                in.setPosition (next);
            }

            if (channels == 0 || rate == 0)
                return false;

            e.channels = (uint8_t) std::min<int> (channels, 255);
            e.sampleRateK = (uint16_t) std::min<uint32_t> (rate / 100, 65535);
            int64_t frames = -1;
            const bool ogg = format == 0x674f || format == 0x6750 || format == 0x6751 || format == 0x676f || format == 0x6770 || format == 0x6771;

            if ((format == 1 || format == 3) && blockAlign > 0 && dataSize >= 0)
                frames = dataSize / blockAlign;
            else if (ogg)
                frames = oggTotalFrames (in);
            else
            {
                e.flags |= flagUnsupported;   // MS-ADPCM and other compressed WAVs
                frames = factFrames;
            }

            if (frames > 0)
                e.seconds = (float) ((double) frames / (double) rate);
            if (acidTempo >= 40.0f && acidTempo <= 300.0f)
                e.bpm = acidTempo;
            return true;
        }

        double extended80 (const uint8_t* p) noexcept
        {
            const int exponent = ((p[0] & 0x7f) << 8) | p[1];
            uint64_t mantissa = 0;
            for (int i = 0; i < 8; ++i)
                mantissa = (mantissa << 8) | p[2 + i];
            if (exponent == 0 && mantissa == 0)
                return 0.0;
            return std::ldexp ((double) mantissa, exponent - 16383 - 63);
        }

        bool parseAiff (juce::FileInputStream& in, IndexEntry& e)
        {
            const auto size = in.getTotalLength();
            in.setPosition (12);
            while (in.getPosition() + 8 <= size)
            {
                uint8_t h[8];
                if (in.read (h, 8) != 8)
                    break;
                const auto pos = in.getPosition();
                const uint32_t len = be32 (h + 4);
                if (std::memcmp (h, "COMM", 4) == 0 && len >= 18)
                {
                    uint8_t b[18];
                    in.read (b, 18);
                    const int channels = (int) be16 (b);
                    const uint32_t frames = be32 (b + 2);
                    const double rate = extended80 (b + 8);
                    if (channels <= 0 || rate <= 0.0)
                        return false;
                    e.channels = (uint8_t) std::min (channels, 255);
                    e.sampleRateK = (uint16_t) std::min (rate / 100.0, 65535.0);
                    e.seconds = (float) ((double) frames / rate);
                    return true;
                }
                const int64_t next = pos + len + (len & 1);
                if (next <= pos || next >= size)
                    break;
                in.setPosition (next);
            }
            return false;
        }

        int64_t skipId3 (juce::FileInputStream& in)
        {
            uint8_t h[10];
            in.setPosition (0);
            if (in.read (h, 10) == 10 && h[0] == 'I' && h[1] == 'D' && h[2] == '3')
                return 10 + (((int64_t) (h[6] & 0x7f) << 21) | ((h[7] & 0x7f) << 14) | ((h[8] & 0x7f) << 7) | (h[9] & 0x7f));
            return 0;
        }

        bool parseFlac (juce::FileInputStream& in, IndexEntry& e)
        {
            in.setPosition (skipId3 (in));
            uint8_t b[42];
            if (in.read (b, 42) != 42 || std::memcmp (b, "fLaC", 4) != 0 || (b[4] & 0x7f) != 0)
                return false;
            const uint8_t* s = b + 8;   // STREAMINFO body
            const uint32_t rate = ((uint32_t) s[10] << 12) | ((uint32_t) s[11] << 4) | ((uint32_t) s[12] >> 4);
            const int channels = ((s[12] >> 1) & 7) + 1;
            const uint64_t total = ((uint64_t) (s[13] & 0x0f) << 32) | ((uint64_t) s[14] << 24) | ((uint64_t) s[15] << 16)
                                 | ((uint64_t) s[16] << 8) | (uint64_t) s[17];
            if (rate == 0)
                return false;
            e.channels = (uint8_t) channels;
            e.sampleRateK = (uint16_t) (rate / 100);
            if (total > 0)
                e.seconds = (float) ((double) total / (double) rate);
            return true;
        }

        bool parseOgg (juce::FileInputStream& in, IndexEntry& e)
        {
            uint8_t b[512];
            in.setPosition (0);
            const int n = in.read (b, sizeof (b));
            for (int i = 0; i + 16 < n; ++i)
            {
                if (b[i] == 1 && std::memcmp (b + i + 1, "vorbis", 6) == 0)
                {
                    const int channels = b[i + 11];
                    const uint32_t rate = le32 (b + i + 12);
                    if (channels <= 0 || rate == 0)
                        return false;
                    e.channels = (uint8_t) channels;
                    e.sampleRateK = (uint16_t) (rate / 100);
                    const auto frames = oggTotalFrames (in);
                    if (frames > 0)
                        e.seconds = (float) ((double) frames / (double) rate);
                    return true;
                }
            }
            e.flags |= flagUnsupported;   // Opus or something else in an Ogg container
            return false;
        }

        bool parseMp3 (juce::FileInputStream& in, IndexEntry& e)
        {
            static const int bitratesV1[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
            static const int bitratesV2[16] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 };
            static const int ratesV1[4] = { 44100, 48000, 32000, 0 };

            const auto size = in.getTotalLength();
            const auto start = skipId3 (in);
            std::vector<uint8_t> buf (65536);
            in.setPosition (start);
            const int n = in.read (buf.data(), (int) buf.size());

            for (int i = 0; i + 4 < n; ++i)
            {
                if (buf[(size_t) i] != 0xff || (buf[(size_t) i + 1] & 0xe0) != 0xe0)
                    continue;
                const int version = (buf[(size_t) i + 1] >> 3) & 3;    // 3 = MPEG1, 2 = MPEG2, 0 = MPEG2.5
                const int layer = (buf[(size_t) i + 1] >> 1) & 3;      // 1 = layer III
                const int brIndex = buf[(size_t) i + 2] >> 4;
                const int srIndex = (buf[(size_t) i + 2] >> 2) & 3;
                const int mode = buf[(size_t) i + 3] >> 6;
                if (version == 1 || layer != 1 || brIndex == 0 || brIndex == 15 || srIndex == 3)
                    continue;

                const bool v1 = version == 3;
                const int rate = ratesV1[srIndex] / (v1 ? 1 : (version == 2 ? 2 : 4));
                const int bitrate = (v1 ? bitratesV1 : bitratesV2)[brIndex];
                const int samplesPerFrame = v1 ? 1152 : 576;
                e.channels = (uint8_t) (mode == 3 ? 1 : 2);
                e.sampleRateK = (uint16_t) (rate / 100);

                // Xing / Info header: exact frame count for VBR files.
                const int side = v1 ? (mode == 3 ? 17 : 32) : (mode == 3 ? 9 : 17);
                const int x = i + 4 + side;
                if (x + 12 < n && (std::memcmp (buf.data() + x, "Xing", 4) == 0 || std::memcmp (buf.data() + x, "Info", 4) == 0)
                    && (be32 (buf.data() + x + 4) & 1))
                {
                    const uint32_t frames = be32 (buf.data() + x + 8);
                    e.seconds = (float) ((double) frames * samplesPerFrame / rate);
                    return true;
                }
                if (bitrate > 0)
                    e.seconds = (float) ((double) (size - start - i) * 8.0 / (bitrate * 1000.0));
                return true;
            }
            return false;
        }
    }

    bool readAudioHeader (const juce::File& file, IndexEntry& e)
    {
        e.flags |= flagHeader;
        juce::FileInputStream in (file);
        if (! in.openedOk())
            return false;

        uint8_t magic[12] = {};
        in.read (magic, 12);
        if (std::memcmp (magic, "RIFF", 4) == 0 && std::memcmp (magic + 8, "WAVE", 4) == 0) return parseWav (in, e, false);
        if (std::memcmp (magic, "RF64", 4) == 0 && std::memcmp (magic + 8, "WAVE", 4) == 0) return parseWav (in, e, true);
        if (std::memcmp (magic, "FORM", 4) == 0)                                              return parseAiff (in, e);
        if (std::memcmp (magic, "fLaC", 4) == 0)                                              return parseFlac (in, e);
        if (std::memcmp (magic, "OggS", 4) == 0)                                              return parseOgg (in, e);
        if (e.ext == extFlac)                                                                 return parseFlac (in, e);
        if (e.ext == extMp3 || std::memcmp (magic, "ID3", 3) == 0 || (magic[0] == 0xff && (magic[1] & 0xe0) == 0xe0))
            return parseMp3 (in, e);
        return false;   // not audio at all: reported as unreadable, not as an unsupported codec
    }

    //==============================================================================
    std::vector<int> runQuery (const RootIndex& index, const Query& query, const std::vector<juce::String>* favourites)
    {
        std::vector<std::string> tokens;
        for (const auto& t : juce::StringArray::fromTokens (query.text.toLowerCase(), " ", "\""))
            if (t.isNotEmpty())
                tokens.push_back (t.toStdString());

        const auto& text = *index.text;
        const size_t numFolders = text.folders.size();
        std::vector<std::vector<uint8_t>> folderHit (tokens.size());
        for (size_t t = 0; t < tokens.size(); ++t)
        {
            folderHit[t].resize (numFolders);
            for (size_t f = 0; f < numFolders; ++f)
                folderHit[t][f] = text.foldersLower[f].find (tokens[t]) != std::string::npos ? 1 : 0;
        }

        std::unordered_set<std::string> favSet;
        const bool favouritesOnly = query.category < 0;
        if (favouritesOnly && favourites != nullptr)
        {
            const auto prefix = juce::String (rootTag (index.root)) + ":";
            for (const auto& f : *favourites)
                if (f.startsWith (prefix))
                    favSet.insert (f.substring (prefix.length()).toStdString());
        }

        std::vector<int> out;
        out.reserve (std::min<size_t> (index.entries.size(), 65536));
        for (int i = 0; i < (int) index.entries.size(); ++i)
        {
            const auto& e = index.entries[(size_t) i];
            if (query.category > 0 && e.category != query.category)
                continue;
            if (favouritesOnly && favSet.find (index.relativePath (e).toStdString()) == favSet.end())
                continue;

            const auto name = index.nameLower (e);
            bool ok = true;
            for (size_t t = 0; t < tokens.size() && ok; ++t)
                ok = name.find (tokens[t]) != std::string_view::npos || folderHit[t][e.folder] != 0;
            if (ok)
                out.push_back (i);
        }

        const auto& entries = index.entries;
        if (query.sort == 1)
            std::stable_sort (out.begin(), out.end(), [&index, &entries] (int a, int b)
                              { return index.nameLower (entries[(size_t) a]) < index.nameLower (entries[(size_t) b]); });
        else if (query.sort == 2)
            std::stable_sort (out.begin(), out.end(), [&entries] (int a, int b)
                              { return entries[(size_t) a].seconds < entries[(size_t) b].seconds; });
        else if (query.sort == 3)
            std::stable_sort (out.begin(), out.end(), [&entries] (int a, int b)
                              { return entries[(size_t) a].bpm < entries[(size_t) b].bpm; });
        return out;
    }

    //==============================================================================
    namespace
    {
        juce::PropertiesFile::Options settingsOptions()
        {
            juce::PropertiesFile::Options o;
            o.applicationName = pluginName;
            o.filenameSuffix = ".settings";
            o.folderName = vendorName;
            o.osxLibrarySubFolder = "Application Support";
            o.commonToAllUsers = false;
            return o;
        }

        const char* rootKey (Root r)
        {
            switch (r)
            {
                case Root::user: return "userRoot";
                case Root::zone: return "zoneRoot";
                case Root::pack: return "packRoot";
                default:         return "";
            }
        }

        bool isAudioFileName (const juce::String& name)
        {
            return ! name.startsWith ("._") && extensionCode (name) != extNone;
        }

        // Folders the index skips. ZONE keeps the English and Russian voices, drops the other languages, the
        // games' music and cutscene mixes, and the library's own working folders.
        bool skipFolder (Root root, const juce::String& parentRel, const juce::String& name)
        {
            if (name.startsWith (".") || name == "__MACOSX")
                return true;
            const auto lower = name.toLowerCase();
            const auto parentLower = parentRel.toLowerCase();

            if (root == Root::zone)
            {
                if (parentRel.isEmpty() && (lower == "_tools" || lower == "_reports"))
                    return true;
                if (parentLower == "heart_of_chornobyl" && (lower == "wem" || lower == "_work"))
                    return true;
                if (lower == "music" || lower == "cutscenes")
                    return true;
                if (parentLower.endsWith ("/localization"))
                    return ! (lower == "eng" || lower == "rus");
            }
            else if (root == Root::pack)
            {
                if (parentRel.isEmpty() && lower == "music")
                    return true;
            }
            else if (root == Root::user)
            {
                if (parentLower == "stalker sounds" && lower == "music")   // commercial tracks by other artists
                    return true;
            }
            return false;
        }
    }

    //==============================================================================
    Library::Library()
        : juce::Thread ("LAB X-0 library")
    {
        settings = std::make_unique<juce::PropertiesFile> (settingsOptions());

        auto saved = [this] (Root r) { return juce::File (settings->getValue (rootKey (r))); };
        const auto user = saved (Root::user);
        roots[0] = user.isDirectory() ? user : autodetectUserRoot();
        const auto zone = saved (Root::zone);
        roots[1] = looksLikeZone (zone) ? zone : autodetectZoneRoot();
        const auto pack = saved (Root::pack);
        roots[2] = pack.isDirectory() ? pack : autodetectPackRoot (roots[0]);

        for (const auto& line : juce::StringArray::fromLines (settings->getValue ("favourites")))
            if (line.trim().isNotEmpty())
                favourites.push_back (line.trim());

        for (auto& s : state)
            s.store (ScanState::idle);

        startThread (juce::Thread::Priority::low);
    }

    Library::~Library()
    {
        stopThread (10000);
    }

    juce::File Library::settingsFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile (vendorName).getChildFile (pluginName);
    }

    juce::File Library::cacheFileFor (Root r) const
    {
        return settingsFolder().getChildFile ("index").getChildFile (juce::String (rootTag (r)).toLowerCase() + ".cache");
    }

    bool Library::looksLikeZone (const juce::File& folder)
    {
        if (! folder.isDirectory())
            return false;
        for (auto game : { "shadow_of_chornobyl", "clear_sky", "call_of_prypiat", "heart_of_chornobyl" })
            if (folder.getChildFile (game).getChildFile ("sounds").isDirectory())
                return true;
        return false;
    }

    juce::File Library::autodetectUserRoot()
    {
        const juce::File preferred (defaultUserRoot);
        if (preferred.isDirectory())
            return preferred;

        for (auto drive : { "C", "D", "E", "F", "G", "H" })
            for (auto programFiles : { ":\\Program Files\\Image-Line", ":\\Program Files (x86)\\Image-Line" })
            {
                juce::File base (juce::String (drive) + programFiles);
                if (! base.isDirectory())
                    continue;
                for (const auto& install : base.findChildFiles (juce::File::findDirectories, false, "FL Studio*"))
                {
                    const auto legacy = install.getChildFile ("Data/Patches/Packs/Legacy");
                    if (legacy.isDirectory())
                        return legacy;
                }
            }
        return {};
    }

    juce::File Library::autodetectZoneRoot()
    {
        for (auto drive : { "C", "D", "E", "F", "G", "H" })
        {
            const juce::File candidate (juce::String (drive) + ":\\_AUDIO\\STALKER SPECIMENS");
            if (looksLikeZone (candidate))
                return candidate;
        }
        return {};
    }

    juce::File Library::autodetectPackRoot (const juce::File& userRoot)
    {
        auto looksLikePack = [] (const juce::File& f)
        {
            return f.isDirectory() && (f.getChildFile ("weapons").isDirectory() || f.getChildFile ("characters_voice").isDirectory());
        };

        if (userRoot.isDirectory() && looksLikePack (userRoot.getChildFile ("stalker sounds")))
            return userRoot.getChildFile ("stalker sounds");

        for (auto drive : { "C", "D", "E", "F", "G", "H" })
            for (auto programFiles : { ":\\Program Files\\Image-Line", ":\\Program Files (x86)\\Image-Line" })
            {
                juce::File base (juce::String (drive) + programFiles);
                if (! base.isDirectory())
                    continue;
                for (const auto& install : base.findChildFiles (juce::File::findDirectories, false, "FL Studio*"))
                {
                    const auto pack = install.getChildFile ("Data/Patches/Packs/Legacy/stalker sounds");
                    if (looksLikePack (pack))
                        return pack;
                }
            }
        return {};
    }

    juce::File Library::getRoot (Root r) const
    {
        if (r == Root::none)
            return {};
        const juce::ScopedLock sl (lock);
        return roots[(size_t) (int) r];
    }

    void Library::setRoot (Root r, const juce::File& folder, bool persist)
    {
        if (r == Root::none)
            return;
        {
            const juce::ScopedLock sl (lock);
            if (roots[(size_t) (int) r] == folder)
                return;
            roots[(size_t) (int) r] = folder;
            indexes[(size_t) (int) r].reset();
        }
        if (persist && settings != nullptr)
        {
            settings->setValue (rootKey (r), folder.getFullPathName());
            settings->saveIfNeeded();
        }
        state[(size_t) (int) r].store (ScanState::idle);
        indexVersion[(size_t) (int) r].fetch_add (1);
        rootsVersion.fetch_add (1);
    }

    juce::File Library::resolve (const SampleRef& ref) const
    {
        if (ref.isEmpty())
            return {};
        if (ref.root == Root::none)
            return juce::File::isAbsolutePath (ref.path) ? juce::File (ref.path) : juce::File();
        const auto base = getRoot (ref.root);
        if (base == juce::File())
            return {};
        return base.getChildFile (ref.path.replace ("/", juce::File::getSeparatorString()));
    }

    SampleRef Library::makeRef (const juce::File& file) const
    {
        // The most specific root wins: PACK lives inside USER by default.
        for (auto r : { Root::pack, Root::zone, Root::user })
        {
            const auto base = getRoot (r);
            if (base != juce::File() && file.isAChildOf (base))
                return { r, file.getRelativePathFrom (base).replace ("\\", "/") };
        }
        return { Root::none, file.getFullPathName() };
    }

    bool Library::isFavourite (const SampleRef& ref) const
    {
        const auto key = ref.toString();
        const juce::ScopedLock sl (lock);
        return std::find (favourites.begin(), favourites.end(), key) != favourites.end();
    }

    void Library::setFavourite (const SampleRef& ref, bool on)
    {
        const auto key = ref.toString();
        if (key.isEmpty())
            return;
        juce::StringArray lines;
        {
            const juce::ScopedLock sl (lock);
            auto it = std::find (favourites.begin(), favourites.end(), key);
            if (on && it == favourites.end())
                favourites.push_back (key);
            else if (! on && it != favourites.end())
                favourites.erase (it);
            for (const auto& f : favourites)
                lines.add (f);
        }
        settings->setValue ("favourites", lines.joinIntoString ("\n"));
        settings->saveIfNeeded();
    }

    std::vector<juce::String> Library::getFavourites() const
    {
        const juce::ScopedLock sl (lock);
        return favourites;
    }

    //==============================================================================
    void Library::requestScan (Root r, bool force)
    {
        if (r == Root::none)
            return;
        const auto i = (size_t) (int) r;
        const auto s = state[i].load();
        if (! force && s != ScanState::idle && s != ScanState::missing)
            return;
        if (! force && s == ScanState::missing && ! getRoot (r).isDirectory())
            return;
        forceRequested[i].store (force);
        scanRequested[i].store (true);
        notify();
    }

    std::shared_ptr<const RootIndex> Library::getIndex (Root r) const
    {
        if (r == Root::none)
            return {};
        const juce::ScopedLock sl (lock);
        return indexes[(size_t) (int) r];
    }

    void Library::publish (Root r, std::shared_ptr<const RootIndex> index)
    {
        {
            const juce::ScopedLock sl (lock);
            indexes[(size_t) (int) r] = std::move (index);
        }
        indexVersion[(size_t) (int) r].fetch_add (1);
    }

    juce::String Library::getStatusText (Root r) const
    {
        const auto i = (size_t) (int) r;
        switch (state[i].load())
        {
            case ScanState::missing:      return "NOT FOUND / SET FOLDER";
            case ScanState::idle:         return getRoot (r).isDirectory() ? juce::String ("NOT SCANNED") : juce::String ("NOT FOUND / SET FOLDER");
            case ScanState::loadingCache: return "LOADING INDEX";
            case ScanState::scanning:     return "SCANNING  " + juce::String (filesFound[i].load()) + " FILES";
            case ScanState::headers:
            {
                const auto index = getIndex (r);
                const int total = index != nullptr ? (int) index->entries.size() : 0;
                return juce::String (total) + " FILES  /  READING " + juce::String (headersDone[i].load()) + "/" + juce::String (total);
            }
            case ScanState::ready:
            {
                const auto index = getIndex (r);
                return juce::String (index != nullptr ? (int) index->entries.size() : 0) + " FILES";
            }
        }
        return {};
    }

    void Library::run()
    {
        while (! threadShouldExit())
        {
            bool worked = false;
            for (int r = 0; r < numRoots && ! threadShouldExit(); ++r)
                if (scanRequested[(size_t) r].exchange (false))
                {
                    scanRoot ((Root) r);
                    worked = true;
                }
            if (! worked)
                wait (250);
        }
    }

    void Library::scanRoot (Root r)
    {
        const auto i = (size_t) (int) r;
        const auto folder = getRoot (r);
        filesFound[i].store (0);
        headersDone[i].store (0);

        if (! folder.isDirectory())
        {
            state[i].store (ScanState::missing);
            publish (r, nullptr);
            return;
        }

        auto stopScan = [this, r, folder] { return threadShouldExit() || getRoot (r) != folder; };

        std::shared_ptr<const RootIndex> previous;
        if (! forceRequested[i].load())
        {
            state[i].store (ScanState::loadingCache);
            previous = loadCache (cacheFileFor (r), r, folder);
            if (previous != nullptr)
                publish (r, previous);
        }

        state[i].store (ScanState::scanning);
        auto index = scanFolder (r, folder, previous, stopScan, &filesFound[i], nullptr, false);
        if (index == nullptr || stopScan())
            return;
        publish (r, std::make_shared<const RootIndex> (*index));

        state[i].store (ScanState::headers);
        auto lastPublish = juce::Time::getMillisecondCounter();
        int done = 0, sincePublish = 0;
        for (auto& e : index->entries)
        {
            if (stopScan())
                return;
            if ((e.flags & flagHeader) == 0)
            {
                readAudioHeader (folder.getChildFile (index->relativePath (e).replace ("/", "\\")), e);
                e.category = categorise (index->nameLower (e), index->text->foldersLower[(size_t) e.folder], r, e.seconds);
                ++sincePublish;
            }
            headersDone[i].store (++done);
            const auto now = juce::Time::getMillisecondCounter();
            if (sincePublish > 0 && (sincePublish >= 4000 || now - lastPublish > 1500))
            {
                publish (r, std::make_shared<const RootIndex> (*index));
                lastPublish = now;
                sincePublish = 0;
            }
        }

        publish (r, std::make_shared<const RootIndex> (*index));
        saveCache (*index, cacheFileFor (r));
        state[i].store (ScanState::ready);
    }

    std::shared_ptr<RootIndex> Library::scanFolder (Root r, const juce::File& folder, const std::shared_ptr<const RootIndex>& previous,
                                                    const std::function<bool()>& shouldExit, std::atomic<int>* found,
                                                    std::atomic<int>* headers, bool readHeaders)
    {
        auto index = std::make_shared<RootIndex>();
        index->root = r;
        index->folder = folder;
        auto text = std::make_shared<IndexStrings>();

        // Header facts already known from the cache, by relative path.
        std::unordered_map<std::string, const IndexEntry*> known;
        if (previous != nullptr)
        {
            known.reserve (previous->entries.size());
            for (const auto& e : previous->entries)
                if (e.flags & flagHeader)
                    known.emplace (previous->relativePath (e).toStdString(), &e);
        }

        struct Pending { juce::File dir; juce::String rel; };
        std::vector<Pending> stack { { folder, {} } };
        int count = 0;

        while (! stack.empty())
        {
            if (shouldExit != nullptr && shouldExit())
                return nullptr;

            const auto current = stack.back();
            stack.pop_back();

            std::vector<juce::String> files, dirs;
            for (const auto& entry : juce::RangedDirectoryIterator (current.dir, false, "*", juce::File::findFilesAndDirectories))
            {
                const auto name = entry.getFile().getFileName();
                if (entry.isDirectory())
                {
                    if (! skipFolder (r, current.rel, name))
                        dirs.push_back (name);
                }
                else if (isAudioFileName (name))
                    files.push_back (name);
            }

            auto byName = [] (const juce::String& a, const juce::String& b) { return a.compareNatural (b) < 0; };
            std::sort (files.begin(), files.end(), byName);
            std::sort (dirs.begin(), dirs.end(), byName);

            if (! files.empty())
            {
                const auto folderIndex = (uint32_t) text->folders.size();
                text->folders.push_back (current.rel);
                text->foldersLower.push_back (lowerUtf8 (current.rel));
                const auto& folderLower = text->foldersLower.back();

                for (const auto& name : files)
                {
                    IndexEntry e;
                    e.folder = folderIndex;
                    const auto utf8 = name.toStdString();
                    const auto lower = lowerUtf8 (name);
                    e.nameOffset = (uint32_t) text->names.size();
                    e.nameLength = (uint16_t) std::min<size_t> (utf8.size(), lower.size());
                    text->names.insert (text->names.end(), utf8.begin(), utf8.begin() + e.nameLength);
                    text->namesLower.insert (text->namesLower.end(), lower.begin(), lower.begin() + e.nameLength);
                    e.ext = extensionCode (name);

                    const auto rel = current.rel.isEmpty() ? name : current.rel + "/" + name;
                    if (folder.getFullPathName().length() + rel.length() + 1 > 259)
                        e.flags |= flagLongPath;

                    if (auto it = known.find (rel.toStdString()); it != known.end())
                    {
                        const auto* k = it->second;
                        e.flags = (uint8_t) (k->flags | (e.flags & flagLongPath));
                        e.seconds = k->seconds;
                        e.bpm = k->bpm;
                        e.channels = k->channels;
                        e.sampleRateK = k->sampleRateK;
                    }

                    e.category = categorise (std::string_view (text->namesLower.data() + e.nameOffset, e.nameLength), folderLower, r, e.seconds);
                    index->entries.push_back (e);
                    if (found != nullptr)
                        found->store (++count);
                }
            }

            // Depth first, alphabetical: push in reverse.
            for (auto it = dirs.rbegin(); it != dirs.rend(); ++it)
                stack.push_back ({ current.dir.getChildFile (*it), current.rel.isEmpty() ? *it : current.rel + "/" + *it });
        }

        index->text = text;

        if (readHeaders)
        {
            int done = 0;
            for (auto& e : index->entries)
            {
                if (shouldExit != nullptr && shouldExit())
                    return nullptr;
                if ((e.flags & flagHeader) == 0)
                {
                    readAudioHeader (folder.getChildFile (index->relativePath (e).replace ("/", "\\")), e);
                    e.category = categorise (index->nameLower (e), text->foldersLower[(size_t) e.folder], r, e.seconds);
                }
                if (headers != nullptr)
                    headers->store (++done);
            }
        }
        return index;
    }

    //==============================================================================
    namespace
    {
        constexpr uint32_t cacheMagic = 0x4930584c;   // "LX0I"
        constexpr int cacheVersion = 1;

        void writeString (juce::OutputStream& out, const juce::String& s)
        {
            const auto utf8 = s.toStdString();
            out.writeCompressedInt ((int) utf8.size());
            out.write (utf8.data(), utf8.size());
        }

        juce::String readString (juce::InputStream& in)
        {
            const int n = in.readCompressedInt();
            if (n <= 0 || n > 4096)
                return {};
            std::string s ((size_t) n, '\0');
            in.read (s.data(), n);
            return juce::String::fromUTF8 (s.data(), n);
        }
    }

    bool Library::saveCache (const RootIndex& index, const juce::File& file)
    {
        file.getParentDirectory().createDirectory();
        const auto temp = file.getSiblingFile (file.getFileName() + ".tmp");
        {
            juce::FileOutputStream raw (temp);
            if (! raw.openedOk())
                return false;
            raw.setPosition (0);
            raw.truncate();
            juce::GZIPCompressorOutputStream out (raw, 6);
            out.writeInt ((int) cacheMagic);
            out.writeInt (cacheVersion);
            out.writeInt ((int) index.root);
            writeString (out, index.folder.getFullPathName());

            const auto& text = *index.text;
            out.writeInt ((int) text.folders.size());
            for (const auto& f : text.folders)
                writeString (out, f);

            out.writeInt ((int) index.entries.size());
            for (const auto& e : index.entries)
            {
                out.writeInt ((int) e.folder);
                writeString (out, index.name (e));
                out.writeByte ((char) e.category);
                out.writeByte ((char) e.flags);
                out.writeFloat (e.seconds);
                out.writeFloat (e.bpm);
                out.writeByte ((char) e.channels);
                out.writeByte ((char) e.ext);
                out.writeShort ((short) e.sampleRateK);
            }
            out.flush();
        }
        return temp.moveFileTo (file);
    }

    std::shared_ptr<RootIndex> Library::loadCache (const juce::File& file, Root r, const juce::File& expectedFolder)
    {
        if (! file.existsAsFile())
            return nullptr;
        juce::FileInputStream raw (file);
        if (! raw.openedOk())
            return nullptr;
        juce::GZIPDecompressorInputStream in (raw);

        if ((uint32_t) in.readInt() != cacheMagic || in.readInt() != cacheVersion || in.readInt() != (int) r)
            return nullptr;
        if (readString (in) != expectedFolder.getFullPathName())
            return nullptr;

        auto index = std::make_shared<RootIndex>();
        index->root = r;
        index->folder = expectedFolder;
        auto text = std::make_shared<IndexStrings>();

        const int numFolders = in.readInt();
        if (numFolders < 0 || numFolders > 10'000'000)
            return nullptr;
        text->folders.reserve ((size_t) numFolders);
        for (int f = 0; f < numFolders; ++f)
        {
            text->folders.push_back (readString (in));
            text->foldersLower.push_back (lowerUtf8 (text->folders.back()));
        }

        const int numEntries = in.readInt();
        if (numEntries < 0 || numEntries > 20'000'000)
            return nullptr;
        index->entries.reserve ((size_t) numEntries);
        for (int i = 0; i < numEntries; ++i)
        {
            IndexEntry e;
            e.folder = (uint32_t) in.readInt();
            const auto name = readString (in);
            e.category = (uint8_t) in.readByte();
            e.flags = (uint8_t) in.readByte();
            e.seconds = in.readFloat();
            e.bpm = in.readFloat();
            e.channels = (uint8_t) in.readByte();
            e.ext = (uint8_t) in.readByte();
            e.sampleRateK = (uint16_t) in.readShort();
            if (e.folder >= (uint32_t) numFolders || name.isEmpty())
                return nullptr;

            const auto utf8 = name.toStdString();
            const auto lower = lowerUtf8 (name);
            e.nameOffset = (uint32_t) text->names.size();
            e.nameLength = (uint16_t) std::min<size_t> (utf8.size(), lower.size());
            text->names.insert (text->names.end(), utf8.begin(), utf8.begin() + e.nameLength);
            text->namesLower.insert (text->namesLower.end(), lower.begin(), lower.begin() + e.nameLength);
            index->entries.push_back (e);
        }
        if (in.isExhausted() && numEntries > 0 && index->entries.size() != (size_t) numEntries)
            return nullptr;

        index->text = text;
        return index;
    }
}
