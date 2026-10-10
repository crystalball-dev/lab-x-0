#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <juce_core/juce_core.h>
#include "Config.h"

namespace labx0
{
    // TIME lane slots (anomalies that bend time) and VOLUME lane slots. A run of identical slots on
    // consecutive steps is one region; repeats replay the start of their region.
    namespace Slot
    {
        enum Time : uint8_t
        {
            timeNone = 0, spring4, spring8, spring16, spring32, spring64, whirl8, whirl16,
            vortex, comet, jelly, burner, bubble, numTime
        };

        enum Volume : uint8_t
        {
            volNone = 0, gate8, gate16, gate32, fadeIn, fadeOut, duck, trem16, mute, numVolume
        };
    }

    struct SlotInfo
    {
        const char* code;   // cell label
        const char* name;
        char text;          // character in the pattern text form
    };

    inline constexpr SlotInfo timeSlots[] = {
        { "",    "None",                     '.' },
        { "S4",  "Springboard 1/4",          'q' },
        { "S8",  "Springboard 1/8",          'e' },
        { "S16", "Springboard 1/16",         's' },
        { "S32", "Springboard 1/32",         't' },
        { "S64", "Springboard 1/64",         'u' },
        { "W8",  "Whirligig scratch 1/8",    'w' },
        { "W16", "Whirligig scratch 1/16",   'W' },
        { "VX",  "Vortex reverse",           'v' },
        { "CM",  "Comet tape stop",          'c' },
        { "JL",  "Jelly half speed",         'j' },
        { "BN",  "Burner accelerating roll", 'b' },
        { "BB",  "Bubble random stutter",    'o' },
    };

    inline constexpr SlotInfo volumeSlots[] = {
        { "",    "None",             '.' },
        { "G8",  "Electro gate 1/8", 'e' },
        { "G16", "Electro gate 1/16",'s' },
        { "G32", "Electro gate 1/32",'t' },
        { "FI",  "Fade in",          'i' },
        { "FO",  "Fade out",         'o' },
        { "DK",  "Duck",             'd' },
        { "TR",  "Tremolo 1/16",     'r' },
        { "MU",  "Mute",             'm' },
    };

    static_assert (std::size (timeSlots) == Slot::numTime);
    static_assert (std::size (volumeSlots) == Slot::numVolume);

    struct Step
    {
        uint8_t on = 0;              // pad and break lanes
        uint8_t velocity = 100;      // 1..127
        uint8_t probability = 100;   // 1..100 %
        uint8_t ratchet = 1;         // 1..4 hits inside the step
        int8_t nudge = 0;            // -50..+50 % of a step
        int8_t pitch = 0;            // -24..+24 semitones
        int8_t slice = -1;           // break lanes: -1 natural (the loop's own position), else slice index
        uint8_t slot = 0;            // TIME and VOLUME lanes
    };
    static_assert (sizeof (Step) == 8);

    struct Lane
    {
        std::array<Step, maxSteps> steps {};
        uint8_t length = 0;   // 0 follows the pattern; 1..64 gives the lane its own cycle (polymeter)
        uint8_t mute = 0;
    };

    enum Resolution : uint8_t { res8 = 0, res16, res32, res8T, res16T, numResolutions };
    inline constexpr double resolutionQuarters[] = { 0.5, 0.25, 0.125, 1.0 / 3.0, 1.0 / 6.0 };
    inline constexpr const char* resolutionNames[] = { "1/8", "1/16", "1/32", "1/8T", "1/16T" };

    struct Pattern
    {
        std::array<Lane, numLanes> lanes {};
        uint8_t length = 16;             // steps, 1..64
        uint8_t resolution = res16;

        double stepQuarters() const noexcept { return resolutionQuarters[resolution < numResolutions ? resolution : res16]; }
        int laneLength (int lane) const noexcept
        {
            const int l = lanes[(size_t) lane].length;
            return l > 0 ? std::min (l, maxSteps) : std::max (1, std::min ((int) length, maxSteps));
        }
        bool laneHasContent (int lane) const noexcept;
        bool isEmpty() const noexcept;
        void clear() noexcept;
    };

    struct PatternBank
    {
        std::array<Pattern, numPatterns> patterns {};
    };

    // Versioned, gzip-compressed binary, stored base64 in the plugin state.
    juce::MemoryBlock serialisePatterns (const PatternBank&);
    bool deserialisePatterns (const void* data, size_t size, PatternBank&);

    // Text form, used by the factory presets and the harness. Lines (or ';'-separated):
    //   len=32 res=1/16                       pattern length and resolution
    //   p1: x...x...X...o...                  pad 1: . off, x on, X accent, o ghost, 2-4 ratchets, ? 50 %
    //   b1: 0.1.2.3.x...                      break A: . off, x natural, 0-9 a-z slice index
    //   t: ............ssss                   TIME lane slot characters (see timeSlots)
    //   v: ........eeee....                   VOLUME lane slot characters (see volumeSlots)
    //   p1.vel: 9..5..   p1.prob: 0..5..  p1.rat: ..2.   p1.nudge: .3.c   p1.pitch: 0 0 -5 0   p1.len=12   p1.mute=1
    bool parsePatternText (const juce::String& text, Pattern& out, juce::String& error);
    juce::String patternToText (const Pattern&);

    namespace PatternTools
    {
        void euclid (Lane&, int length, int hits, int rotate);
        void randomFill (Lane&, int length, float density, uint32_t seed);
        void rotate (Lane&, int length, int amount);
        void clearLane (Lane&);

        // Break lanes. materialise() writes the natural slice of each step as a number first.
        void materialise (Lane&, int length, const std::function<int (int step)>& naturalSlice);
        void shuffleSlices (Lane&, int length, uint32_t seed);
        void reverseSlices (Lane&, int length);
        void rollSlices (Lane&, int length);
        void halfTime (Lane&, int length);
        void doubleTime (Lane&, int length);
    }

    // Patterns shared between the editor (writer) and the audio thread (reader). Writes take a spin lock for the
    // few microseconds an edit needs; the audio thread copies changed patterns only when the lock is free, so it
    // never waits.
    class PatternStore
    {
    public:
        template <typename Fn>
        void edit (int index, Fn&& fn)
        {
            if (! juce::isPositiveAndBelow (index, numPatterns))
                return;
            const juce::SpinLock::ScopedLockType sl (lock);
            fn (bank.patterns[(size_t) index]);
            versions[(size_t) index].fetch_add (1);
            total.fetch_add (1);
        }

        template <typename Fn>
        void editAll (Fn&& fn)
        {
            const juce::SpinLock::ScopedLockType sl (lock);
            fn (bank);
            for (auto& v : versions)
                v.fetch_add (1);
            total.fetch_add (1);
        }

        template <typename Fn>
        void read (Fn&& fn) const
        {
            const juce::SpinLock::ScopedLockType sl (lock);
            fn (bank);
        }

        Pattern copyPattern (int index) const
        {
            const juce::SpinLock::ScopedLockType sl (lock);
            return bank.patterns[(size_t) juce::jlimit (0, numPatterns - 1, index)];
        }

        // Audio thread. Returns true when every pattern in `local` is current.
        bool sync (PatternBank& local, std::array<uint32_t, numPatterns>& seen, uint32_t& seenTotal) const noexcept
        {
            const auto t = total.load();
            if (t == seenTotal)
                return true;
            if (! lock.tryEnter())
                return false;
            for (int i = 0; i < numPatterns; ++i)
            {
                const auto v = versions[(size_t) i].load();
                if (v != seen[(size_t) i])
                {
                    local.patterns[(size_t) i] = bank.patterns[(size_t) i];
                    seen[(size_t) i] = v;
                }
            }
            seenTotal = total.load();
            lock.exit();
            return true;
        }

        uint32_t getVersion() const noexcept { return total.load(); }

    private:
        mutable juce::SpinLock lock;
        PatternBank bank;
        std::array<std::atomic<uint32_t>, numPatterns> versions {};
        std::atomic<uint32_t> total { 1 };
    };
}
