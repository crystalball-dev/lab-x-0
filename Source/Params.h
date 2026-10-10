#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <vector>
#include "Config.h"

namespace labx0
{
    enum class Kind { real, integer, choice };
    enum class Smooth { none, linear, log };

    struct ParamSpec
    {
        juce::String id, name;
        Kind kind;
        float minValue, maxValue, defaultValue, skewCentre;   // skewCentre 0 = linear range
        Smooth smooth;
        juce::String unit;      // "%", "Hz", "s", "ms", "dB", "st", "pan", "tone", "decay", ""
        juce::String choices;   // '|' separated
    };

    // Every parameter is a float or a choice. A bool parameter failed pluginval's state restore in LAB X-3.
    // Pads and break slots repeat one block of fields each; P::pad() and P::brk() give their indices.
    namespace P
    {
        enum Global : int
        {
            pattern, swing, run, sliceMidi, emission, volume, width, ceiling,
            anomalyMix, scratchDepth,
            trAttack, trSustain,
            mbDepth, mbTime, mbUp, mbDown, mbLowX, mbHighX, mbLowGain, mbMidGain, mbHighGain,
            slamBark, slamBite, slamWet, slamSc,
            clipDrive, subSafe,
            numGlobal
        };

        enum PadField : int
        {
            padLevel, padPan, padTune, padStart, padAttack, padHold, padDecay, padTone, padReso, padDrive, padVel,
            padReverse, padMode, padChoke, padRoute,
            numPadFields
        };

        enum BreakField : int
        {
            brLevel, brPan, brTune, brFit, brBeats, brSlicing, brSens, brGate, brDecay, brTone, brRoute,
            numBreakFields
        };

        inline constexpr int padBase   = numGlobal;
        inline constexpr int breakBase = padBase + numPads * numPadFields;
        inline constexpr int outBase   = breakBase + numBreaks * numBreakFields;   // 0.2.0, appended: older indices stay
        inline constexpr int count     = outBase + numPads + numBreaks;

        constexpr int pad (int p, int field) noexcept { return padBase + p * numPadFields + field; }
        constexpr int brk (int b, int field) noexcept { return breakBase + b * numBreakFields + field; }
        constexpr int out (int lane) noexcept         { return outBase + lane; }   // lane 0-15 pads, 16-19 breaks
    }

    // Choice indices that the DSP code relies on.
    namespace Choice
    {
        enum Run     { runHost = 0, runFree };
        enum Mode    { modePoly = 0, modeMono, modeGate };
        enum Route   { routeFull = 0, routePunch, routeDry };
        enum Fit     { fitRepitch = 0, fitSlice, fitStretch };
        enum Slicing { sliceAuto = 0, slice8, slice16, slice32 };
        enum Gate    { gateLegato = 0, gateSlice };
    }

    inline constexpr float breakBeatsChoices[] = { 0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f, 12.0f, 16.0f, 24.0f, 32.0f, 64.0f };
    inline constexpr float subSafeHz[]         = { 0.0f, 40.0f, 60.0f, 80.0f, 100.0f, 120.0f, 150.0f };
    inline constexpr float slamScHz[]          = { 0.0f, 90.0f, 180.0f };
    inline constexpr float decayFull           = 9.99f;   // a decay at or above this plays the whole sample

    inline constexpr const char* routeNames = "Full|Punch|Dry";

    // TONE: one bipolar knob. Below the centre a low-pass falls from 20 kHz to 78 Hz; above it a high-pass rises
    // from 20 Hz to 7.8 kHz. The centre is open.
    inline float toneLowpassHz (float tone) noexcept  { return 20000.0f * std::exp2 (8.0f * std::min (0.0f, tone)); }
    inline float toneHighpassHz (float tone) noexcept { return 20.0f * std::exp2 (8.6f * std::max (0.0f, tone)); }

    inline juce::String padPrefix (int pad)   { return "p" + juce::String (pad + 1).paddedLeft ('0', 2) + "_"; }
    inline juce::String breakPrefix (int b)   { return "b" + juce::String (b + 1) + "_"; }
    inline juce::String breakLetter (int b)   { return juce::String::charToString ((juce::juce_wchar) ('A' + b)); }

    inline const std::vector<ParamSpec>& paramSpecs()
    {
        static const std::vector<ParamSpec> specs = []
        {
            std::vector<ParamSpec> s;
            s.reserve ((size_t) P::count);
            auto add = [&s] (juce::String id, juce::String name, Kind kind, float lo, float hi, float def, float skew,
                             Smooth smooth, juce::String unit, juce::String choices = {})
            {
                s.push_back ({ std::move (id), std::move (name), kind, lo, hi, def, skew, smooth, std::move (unit), std::move (choices) });
            };

            juce::StringArray patternNames;
            for (int i = 1; i <= numPatterns; ++i)
                patternNames.add (juce::String (i).paddedLeft ('0', 2));

            // Global, in P::Global order
            add ("pattern",       "Pattern",                Kind::choice, 0.0f, (float) (numPatterns - 1), 0.0f, 0.0f, Smooth::none, "", patternNames.joinIntoString ("|"));
            add ("swing",         "Swing",                  Kind::real,   0.0f, 1.0f,    0.0f,   0.0f,   Smooth::none,   "%");
            add ("run",           "Run",                    Kind::choice, 0.0f, 1.0f,    0.0f,   0.0f,   Smooth::none,   "", "Host|Free");
            add ("slice_midi",    "Slice MIDI Slot",        Kind::choice, 0.0f, 3.0f,    0.0f,   0.0f,   Smooth::none,   "", "A|B|C|D");
            add ("emission",      "EMISSION",               Kind::real,   0.0f, 1.0f,    0.0f,   0.0f,   Smooth::linear, "%");
            add ("master_volume", "Master Volume",          Kind::real,   -48.0f, 6.0f,  -6.0f,  0.0f,   Smooth::linear, "dB");
            add ("width",         "Width",                  Kind::real,   0.0f, 1.5f,    1.0f,   0.0f,   Smooth::linear, "%");
            add ("limit_ceiling", "Limiter Ceiling",        Kind::real,   -12.0f, 0.0f,  -0.3f,  0.0f,   Smooth::linear, "dB");
            add ("anomaly_mix",   "Anomaly Mix",            Kind::real,   0.0f, 1.0f,    1.0f,   0.0f,   Smooth::linear, "%");
            add ("scratch_depth", "Scratch Depth",          Kind::real,   0.0f, 1.0f,    0.6f,   0.0f,   Smooth::none,   "%");
            add ("tr_attack",     "Transient Attack",       Kind::real,   -1.0f, 1.0f,   0.0f,   0.0f,   Smooth::linear, "%");
            add ("tr_sustain",    "Transient Sustain",      Kind::real,   -1.0f, 1.0f,   0.0f,   0.0f,   Smooth::linear, "%");
            add ("mb_depth",      "Multiband Depth",        Kind::real,   0.0f, 1.0f,    0.0f,   0.0f,   Smooth::linear, "%");
            add ("mb_time",       "Multiband Time",         Kind::real,   0.0f, 1.0f,    0.5f,   0.0f,   Smooth::linear, "%");
            add ("mb_up",         "Multiband Upward",       Kind::real,   0.0f, 1.0f,    0.5f,   0.0f,   Smooth::linear, "%");
            add ("mb_down",       "Multiband Downward",     Kind::real,   0.0f, 1.0f,    0.5f,   0.0f,   Smooth::linear, "%");
            add ("mb_low_xover",  "Multiband Low Split",    Kind::real,   40.0f, 400.0f, 120.0f, 120.0f, Smooth::log,    "Hz");
            add ("mb_high_xover", "Multiband High Split",   Kind::real,   1000.0f, 8000.0f, 2500.0f, 2500.0f, Smooth::log, "Hz");
            add ("mb_low_gain",   "Multiband Low Gain",     Kind::real,   -12.0f, 12.0f, 0.0f,   0.0f,   Smooth::linear, "dB");
            add ("mb_mid_gain",   "Multiband Mid Gain",     Kind::real,   -12.0f, 12.0f, 0.0f,   0.0f,   Smooth::linear, "dB");
            add ("mb_high_gain",  "Multiband High Gain",    Kind::real,   -12.0f, 12.0f, 0.0f,   0.0f,   Smooth::linear, "dB");
            add ("slam_bark",     "Slam Bark",              Kind::real,   0.0f, 1.0f,    0.0f,   0.0f,   Smooth::linear, "%");
            add ("slam_bite",     "Slam Bite",              Kind::real,   0.0f, 1.0f,    0.0f,   0.0f,   Smooth::linear, "%");
            add ("slam_wet",      "Slam Wet",               Kind::real,   0.0f, 1.0f,    1.0f,   0.0f,   Smooth::linear, "%");
            add ("slam_sc",       "Slam Sidechain Cut",     Kind::choice, 0.0f, 2.0f,    1.0f,   0.0f,   Smooth::none,   "", "Off|90 Hz|180 Hz");
            add ("clip_drive",    "Clip Drive",             Kind::real,   0.0f, 18.0f,   0.0f,   0.0f,   Smooth::linear, "dB");
            add ("sub_safe",      "Sub Safe",               Kind::choice, 0.0f, 6.0f,    2.0f,   0.0f,   Smooth::none,   "", "Off|40 Hz|60 Hz|80 Hz|100 Hz|120 Hz|150 Hz");
            jassert ((int) s.size() == P::numGlobal);

            for (int p = 0; p < numPads; ++p)
            {
                const auto id = padPrefix (p);
                const auto name = "Pad " + juce::String (p + 1) + " ";
                add (id + "level",   name + "Level",    Kind::real,   -48.0f, 6.0f,  0.0f,   0.0f,   Smooth::linear, "dB");
                add (id + "pan",     name + "Pan",      Kind::real,   -1.0f, 1.0f,   0.0f,   0.0f,   Smooth::linear, "pan");
                add (id + "tune",    name + "Tune",     Kind::real,   -24.0f, 24.0f, 0.0f,   0.0f,   Smooth::linear, "st");
                add (id + "start",   name + "Start",    Kind::real,   0.0f, 1.0f,    0.0f,   0.0f,   Smooth::none,   "%");
                add (id + "attack",  name + "Attack",   Kind::real,   0.0f, 100.0f,  0.0f,   10.0f,  Smooth::none,   "ms");
                add (id + "hold",    name + "Hold",     Kind::real,   0.0f, 2000.0f, 0.0f,   200.0f, Smooth::none,   "ms");
                add (id + "decay",   name + "Decay",    Kind::real,   0.01f, 10.0f,  10.0f,  0.6f,   Smooth::none,   "decay");
                add (id + "tone",    name + "Tone",     Kind::real,   -1.0f, 1.0f,   0.0f,   0.0f,   Smooth::linear, "tone");
                add (id + "reso",    name + "Resonance",Kind::real,   0.0f, 1.0f,    0.0f,   0.0f,   Smooth::linear, "%");
                add (id + "drive",   name + "Drive",    Kind::real,   0.0f, 24.0f,   0.0f,   0.0f,   Smooth::linear, "dB");
                add (id + "vel",     name + "Velocity", Kind::real,   0.0f, 1.0f,    1.0f,   0.0f,   Smooth::none,   "%");
                add (id + "reverse", name + "Reverse",  Kind::choice, 0.0f, 1.0f,    0.0f,   0.0f,   Smooth::none,   "", "Off|On");
                add (id + "mode",    name + "Mode",     Kind::choice, 0.0f, 2.0f,    1.0f,   0.0f,   Smooth::none,   "", "Poly|Mono|Gate");
                add (id + "choke",   name + "Choke",    Kind::choice, 0.0f, 4.0f,    0.0f,   0.0f,   Smooth::none,   "", "None|1|2|3|4");
                add (id + "route",   name + "Route",    Kind::choice, 0.0f, 2.0f,    0.0f,   0.0f,   Smooth::none,   "", routeNames);
            }

            for (int b = 0; b < numBreaks; ++b)
            {
                const auto id = breakPrefix (b);
                const auto name = "Break " + breakLetter (b) + " ";
                add (id + "level",   name + "Level",    Kind::real,   -48.0f, 6.0f,  0.0f,   0.0f,   Smooth::linear, "dB");
                add (id + "pan",     name + "Pan",      Kind::real,   -1.0f, 1.0f,   0.0f,   0.0f,   Smooth::linear, "pan");
                add (id + "tune",    name + "Tune",     Kind::real,   -24.0f, 24.0f, 0.0f,   0.0f,   Smooth::linear, "st");
                add (id + "fit",     name + "Fit",      Kind::choice, 0.0f, 2.0f,    0.0f,   0.0f,   Smooth::none,   "", "Repitch|Slice|Stretch");
                add (id + "beats",   name + "Beats",    Kind::choice, 0.0f, 11.0f,   0.0f,   0.0f,   Smooth::none,   "", "Auto|1|2|3|4|6|8|12|16|24|32|64");
                add (id + "slicing", name + "Slicing",  Kind::choice, 0.0f, 3.0f,    0.0f,   0.0f,   Smooth::none,   "", "Auto|1/8|1/16|1/32");
                add (id + "sens",    name + "Sensitivity", Kind::real, 0.0f, 1.0f,   0.5f,   0.0f,   Smooth::none,   "%");
                add (id + "gate",    name + "Gate",     Kind::choice, 0.0f, 1.0f,    0.0f,   0.0f,   Smooth::none,   "", "Legato|Slice");
                add (id + "decay",   name + "Decay",    Kind::real,   0.01f, 10.0f,  10.0f,  0.6f,   Smooth::none,   "decay");
                add (id + "tone",    name + "Tone",     Kind::real,   -1.0f, 1.0f,   0.0f,   0.0f,   Smooth::linear, "tone");
                add (id + "route",   name + "Route",    Kind::choice, 0.0f, 2.0f,    0.0f,   0.0f,   Smooth::none,   "", routeNames);
            }

            // Output per lane (0.2.0): Main, or a stereo output of its own (2..21) for a mixer track in the host.
            juce::StringArray outputs { "Main" };
            for (int k = 2; k <= numAuxOutputs + 1; ++k)
                outputs.add (juce::String (k));
            for (int lane = 0; lane < numPads + numBreaks; ++lane)
            {
                const auto id = lane < numPads ? padPrefix (lane) : breakPrefix (lane - numPads);
                const auto name = lane < numPads ? "Pad " + juce::String (lane + 1) + " " : "Break " + breakLetter (lane - numPads) + " ";
                add (id + "out", name + "Output", Kind::choice, 0.0f, (float) numAuxOutputs, 0.0f, 0.0f, Smooth::none, "",
                     outputs.joinIntoString ("|"));
            }

            jassert ((int) s.size() == P::count);
            return s;
        }();
        return specs;
    }

    inline int paramIndexForId (const juce::String& id)
    {
        const auto& specs = paramSpecs();
        for (int i = 0; i < (int) specs.size(); ++i)
            if (id == specs[(size_t) i].id)
                return i;
        return -1;
    }

    inline juce::String padParamId (int pad, int field)   { return paramSpecs()[(size_t) P::pad (pad, field)].id; }
    inline juce::String breakParamId (int b, int field)   { return paramSpecs()[(size_t) P::brk (b, field)].id; }
    inline juce::String outParamId (int lane)             { return paramSpecs()[(size_t) P::out (lane)].id; }

    inline juce::NormalisableRange<float> rangeFor (const ParamSpec& s)
    {
        juce::NormalisableRange<float> range (s.minValue, s.maxValue);
        if (s.kind != Kind::real)
            range.interval = 1.0f;
        else if (s.skewCentre > 0.0f)
            range.setSkewForCentre (s.skewCentre);
        return range;
    }

    inline juce::String formatHz (float v)
    {
        return v >= 1000.0f ? juce::String (v / 1000.0f, v >= 10000.0f ? 1 : 2) + "k"
                            : juce::String (juce::roundToInt (v));
    }

    inline juce::String formatValue (const ParamSpec& s, float v)
    {
        const auto& unit = s.unit;
        if (unit == "%")     return juce::String (juce::roundToInt (v * 100.0f)) + "%";
        if (unit == "Hz")    return v >= 1000.0f ? juce::String (v / 1000.0f, 2) + " kHz" : juce::String (juce::roundToInt (v)) + " Hz";
        if (unit == "s")     return v < 1.0f ? juce::String (juce::roundToInt (v * 1000.0f)) + " ms" : juce::String (v, 2) + " s";
        if (unit == "ms")    return v >= 1000.0f ? juce::String (v / 1000.0f, 2) + " s" : juce::String (v, v < 10.0f ? 1 : 0) + " ms";
        if (unit == "dB")    return juce::String (v, 1) + " dB";
        if (unit == "st")    return (v > 0.04f ? "+" : "") + juce::String (v, 1) + " st";
        if (unit == "pan")   return std::abs (v) < 0.005f ? juce::String ("C")
                                   : (v < 0.0f ? "L" : "R") + juce::String (juce::roundToInt (std::abs (v) * 100.0f));
        if (unit == "tone")
        {
            if (v < -0.01f) return "LP " + formatHz (toneLowpassHz (v));
            if (v > 0.01f)  return "HP " + formatHz (toneHighpassHz (v));
            return "OPEN";
        }
        if (unit == "decay") return v >= decayFull ? juce::String ("FULL")
                                   : (v < 1.0f ? juce::String (juce::roundToInt (v * 1000.0f)) + " ms" : juce::String (v, 2) + " s");
        return juce::String (juce::roundToInt (v));
    }

    inline float parseValue (const ParamSpec& s, const juce::String& text)
    {
        const auto t = text.trim().toLowerCase();
        float v = t.retainCharacters ("0123456789.-+").getFloatValue();
        const auto& unit = s.unit;
        if (unit == "%")                               v *= 0.01f;
        else if (unit == "Hz" && t.contains ("k"))     v *= 1000.0f;
        else if (unit == "s" && t.contains ("ms"))     v *= 0.001f;
        else if (unit == "ms" && t.endsWith ("s") && ! t.endsWith ("ms")) v *= 1000.0f;
        else if (unit == "pan")
        {
            if (t == "c" || t == "centre" || t == "center") v = 0.0f;
            else v = std::abs (v) * 0.01f * (t.startsWith ("l") || v < 0.0f ? -1.0f : 1.0f);
        }
        else if (unit == "tone")
        {
            if (t.startsWith ("open")) return 0.0f;
            float hz = std::abs (v) * (t.contains ("k") ? 1000.0f : 1.0f);
            if (t.startsWith ("lp"))  v = juce::jlimit (-1.0f, 0.0f, std::log2 (std::max (hz, 1.0f) / 20000.0f) / 8.0f);
            else if (t.startsWith ("hp")) v = juce::jlimit (0.0f, 1.0f, std::log2 (std::max (hz, 1.0f) / 20.0f) / 8.6f);
        }
        else if (unit == "decay")
        {
            if (t.startsWith ("full")) return s.maxValue;
            if (t.contains ("ms")) v *= 0.001f;
        }
        return juce::jlimit (s.minValue, s.maxValue, v);
    }

    inline juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
    {
        juce::AudioProcessorValueTreeState::ParameterLayout layout;

        for (const auto& s : paramSpecs())
        {
            const juce::ParameterID pid { s.id, 1 };
            const auto* spec = &s;

            switch (s.kind)
            {
                case Kind::choice:
                    layout.add (std::make_unique<juce::AudioParameterChoice> (pid, s.name, juce::StringArray::fromTokens (s.choices, "|", ""),
                                                                              (int) s.defaultValue));
                    break;

                case Kind::integer:
                    layout.add (std::make_unique<juce::AudioParameterInt> (
                        pid, s.name, (int) s.minValue, (int) s.maxValue, (int) s.defaultValue,
                        juce::AudioParameterIntAttributes()
                            .withStringFromValueFunction ([spec] (int v, int) { return formatValue (*spec, (float) v); })));
                    break;

                case Kind::real:
                    layout.add (std::make_unique<juce::AudioParameterFloat> (
                        pid, s.name, rangeFor (s), s.defaultValue,
                        juce::AudioParameterFloatAttributes()
                            .withStringFromValueFunction ([spec] (float v, int) { return formatValue (*spec, v); })
                            .withValueFromStringFunction ([spec] (const juce::String& text) { return parseValue (*spec, text); })));
                    break;
            }
        }

        return layout;
    }
}
