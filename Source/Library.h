#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_data_structures/juce_data_structures.h>
#include "Config.h"

namespace labx0
{
    //==============================================================================
    // Where samples come from. USER is the configurable sample library (FL Studio's legacy Packs folder by
    // default), ZONE the STALKER SPECIMENS library, PACK FL's own legacy 'stalker sounds' pack.
    enum class Root : int { none = -1, user = 0, zone = 1, pack = 2 };
    inline constexpr int numRoots = 3;

    inline const char* rootTag (Root r) noexcept
    {
        switch (r)
        {
            case Root::user: return "USER";
            case Root::zone: return "ZONE";
            case Root::pack: return "PACK";
            default:         return "FILE";
        }
    }

    // Windows refuses paths of 260 characters or more unless they carry the \\?\ prefix (the user's legacy packs hold
    // 424 such files, all spoken-word recordings). JUCE hands the string straight to the wide Win32 calls.
    inline juce::File longPathSafe (const juce::File& file)
    {
       #if JUCE_WINDOWS
        const auto path = file.getFullPathName();
        if (path.length() >= 260 && ! path.startsWith ("\\\\?\\"))
            return juce::File (path.startsWith ("\\\\") ? "\\\\?\\UNC\\" + path.substring (2) : "\\\\?\\" + path);
       #endif
        return file;
    }

    // A sample reference survives a moved library: it stores the root and the path inside it. Files outside every
    // root keep their absolute path.
    struct SampleRef
    {
        Root root = Root::none;
        juce::String path;   // relative to the root with '/' separators, or absolute for Root::none

        bool isEmpty() const noexcept { return path.isEmpty(); }
        juce::String toString() const;
        static SampleRef fromString (const juce::String& text);
        juce::String fileName() const;
        juce::String displayName() const;   // file name without extension
        bool operator== (const SampleRef& o) const noexcept { return root == o.root && path == o.path; }
        bool operator!= (const SampleRef& o) const noexcept { return ! (*this == o); }
    };

    //==============================================================================
    enum Category : uint8_t { catNone = 0, catKick, catSnare, catHat, catCymbal, catPerc, catBreak, catFx, catVox, numCategories };
    inline constexpr const char* categoryNames[] = { "", "KICK", "SNARE", "HAT", "CYM", "PERC", "BREAK", "FX", "VOX" };

    enum : uint8_t { flagHeader = 1, flagUnsupported = 2, flagLongPath = 4 };

    struct IndexEntry
    {
        uint32_t folder = 0;
        uint32_t nameOffset = 0;
        uint16_t nameLength = 0;
        uint8_t category = catNone;
        uint8_t flags = 0;
        float seconds = -1.0f;
        float bpm = 0.0f;
        uint8_t channels = 0;
        uint8_t ext = 0;
        uint16_t sampleRateK = 0;   // sample rate / 100
    };

    // Folder and file names of one scan. Written once by the scanner, then shared by every published index.
    struct IndexStrings
    {
        std::vector<juce::String> folders;       // relative, '/' separators, "" is the root itself
        std::vector<std::string> foldersLower;   // lowercase UTF-8, for search
        std::vector<char> names;                 // UTF-8 file names with extension, back to back
        std::vector<char> namesLower;
    };

    // One scanned root. Immutable once published; readers hold a shared_ptr. The header pass publishes new
    // copies of `entries` only; the strings are shared.
    struct RootIndex
    {
        Root root = Root::none;
        juce::File folder;
        std::shared_ptr<const IndexStrings> text = std::make_shared<IndexStrings>();
        std::vector<IndexEntry> entries;

        juce::String name (const IndexEntry& e) const
        {
            return juce::String::fromUTF8 (text->names.data() + e.nameOffset, (int) e.nameLength);
        }
        std::string_view nameLower (const IndexEntry& e) const
        {
            return std::string_view (text->namesLower.data() + e.nameOffset, e.nameLength);
        }
        std::string_view nameUtf8 (const IndexEntry& e) const
        {
            return std::string_view (text->names.data() + e.nameOffset, e.nameLength);
        }
        const juce::String& folderOf (const IndexEntry& e) const { return text->folders[(size_t) e.folder]; }
        juce::String relativePath (const IndexEntry& e) const
        {
            const auto& f = folderOf (e);
            return f.isEmpty() ? name (e) : f + "/" + name (e);
        }
        SampleRef ref (const IndexEntry& e) const { return { root, relativePath (e) }; }
    };

    struct Query
    {
        Root root = Root::user;
        int category = 0;          // 0 = all, catKick.. = that category, -1 = favourites
        juce::String text;
        int sort = 0;              // 0 = folder then name, 1 = name, 2 = length, 3 = tempo
    };

    // Lowercase tokens, AND-matched against file name and folder path.
    std::vector<int> runQuery (const RootIndex& index, const Query& query, const std::vector<juce::String>* favourites);

    // Category from the file name, its folders (deepest first) and its length (-1 when unknown). Name and folder are
    // UTF-8 in their original case: capitals split run-together words ("YChopHat" has a hat, "Phat Phrog" has none).
    uint8_t categorise (std::string_view name, std::string_view folder, Root root, float seconds);

    // Header facts read without decoding: length, channels, rate, ACID tempo. Exact for WAV, AIFF, FLAC and Ogg
    // (including FL's Ogg-in-WAV); MP3 is estimated from its first frame (or exact with a Xing/Info header).
    bool readAudioHeader (const juce::File& file, IndexEntry& entry);

    //==============================================================================
    // One per process (juce::SharedResourcePointer), shared by every LAB X-0 instance: root folders, favourites,
    // settings and the file index. Scans run on one background thread and start only when something asks.
    class Library : private juce::Thread
    {
    public:
        Library();
        ~Library() override;

        static constexpr const char* defaultUserRoot = "E:\\Program Files\\Image-Line\\FL Studio 20\\Data\\Patches\\Packs\\Legacy";

        // Roots (any thread; set from the message thread).
        juce::File getRoot (Root r) const;
        void setRoot (Root r, const juce::File& folder, bool persist = true);
        int getRootsVersion() const noexcept { return rootsVersion.load(); }
        juce::File resolve (const SampleRef& ref) const;
        SampleRef makeRef (const juce::File& file) const;

        static juce::File autodetectUserRoot();
        static juce::File autodetectZoneRoot();
        static juce::File autodetectPackRoot (const juce::File& userRoot);
        static bool looksLikeZone (const juce::File& folder);

        // Favourites (message thread).
        bool isFavourite (const SampleRef& ref) const;
        void setFavourite (const SampleRef& ref, bool on);
        std::vector<juce::String> getFavourites() const;

        // Index
        enum class ScanState { idle, missing, loadingCache, scanning, headers, ready };
        void requestScan (Root r, bool force = false);
        std::shared_ptr<const RootIndex> getIndex (Root r) const;
        int getIndexVersion (Root r) const noexcept { return indexVersion[(size_t) (int) r].load(); }
        ScanState getScanState (Root r) const noexcept { return state[(size_t) (int) r].load(); }
        int getFilesFound (Root r) const noexcept { return filesFound[(size_t) (int) r].load(); }
        int getHeadersDone (Root r) const noexcept { return headersDone[(size_t) (int) r].load(); }
        juce::String getStatusText (Root r) const;

        // Synchronous scan, used by the harness: walk + headers, optional cache file.
        static std::shared_ptr<RootIndex> scanFolder (Root r, const juce::File& folder, const std::shared_ptr<const RootIndex>& previous,
                                                      const std::function<bool()>& shouldExit, std::atomic<int>* found = nullptr,
                                                      std::atomic<int>* headers = nullptr, bool readHeaders = true);
        static bool saveCache (const RootIndex& index, const juce::File& file);
        static std::shared_ptr<RootIndex> loadCache (const juce::File& file, Root r, const juce::File& expectedFolder);
        juce::File cacheFileFor (Root r) const;
        static juce::File settingsFolder();
        static inline juce::File settingsFileOverride;   // tests: use this settings file instead of the user's

        juce::PropertiesFile& getSettings() noexcept { return *settings; }

    private:
        void run() override;
        void scanRoot (Root r);
        void publish (Root r, std::shared_ptr<const RootIndex> index);

        std::unique_ptr<juce::PropertiesFile> settings;
        mutable juce::CriticalSection lock;   // roots, favourites, published indexes
        std::array<juce::File, numRoots> roots;
        std::vector<juce::String> favourites;
        std::array<std::shared_ptr<const RootIndex>, numRoots> indexes;

        std::array<std::atomic<bool>, numRoots> scanRequested {};
        std::array<std::atomic<bool>, numRoots> forceRequested {};
        std::array<std::atomic<ScanState>, numRoots> state {};
        std::array<std::atomic<int>, numRoots> indexVersion {};
        std::array<std::atomic<int>, numRoots> filesFound {};
        std::array<std::atomic<int>, numRoots> headersDone {};
        std::atomic<int> rootsVersion { 0 };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Library)
    };
}
