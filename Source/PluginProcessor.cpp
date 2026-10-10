#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "FactoryPresets.h"
#include "KitCatalog.h"
#include "AudioAnalysis.h"

using namespace labx0;

namespace
{
    enum Bus { busFull = 0, busPunch, busDry };
    enum Stage { stageVoice = 0, stageAnomaly, stageTransient, stageMultiband, stageSlam, stageOutput, numStages };
}

LabX0AudioProcessor::LabX0AudioProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "LABX0", createParameterLayout())
{
    const auto& specs = paramSpecs();
    for (int i = 0; i < P::count; ++i)
    {
        raw[(size_t) i] = apvts.getRawParameterValue (specs[(size_t) i].id);
        jassert (raw[(size_t) i] != nullptr);
        smoothers[(size_t) i].enabled = specs[(size_t) i].smooth != Smooth::none;
        smoothers[(size_t) i].logDomain = specs[(size_t) i].smooth == Smooth::log;
        values[(size_t) i] = rawValue (i);
    }

    for (auto& b : meters.breakPosition)
        b.store (-1.0);
    breakMidiNote.fill (-1);
    sliceScratch.assign (8192, 0);

    applyPreset (0);
    startTimer (500);
}

LabX0AudioProcessor::~LabX0AudioProcessor()
{
    stopTimer();
    cancelPendingUpdate();
    flushFaultLog();
}

//==============================================================================
const char* LabX0AudioProcessor::stageName (int stage) noexcept
{
    static const char* names[] = { "VOICE", "ANOMALY", "TRANSIENT", "MULTIBAND", "SLAM", "OUTPUT" };
    return juce::isPositiveAndBelow (stage, (int) numStages) ? names[stage] : "?";
}

juce::File LabX0AudioProcessor::getFaultLogFile() const
{
    if (faultLogOverride != juce::File())
        return faultLogOverride;
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile (vendorName).getChildFile (juce::String (pluginName) + " faults.log");
}

int LabX0AudioProcessor::flushFaultLog()
{
    const int written = faultWrite.load (std::memory_order_acquire);
    if (written == faultRead)
        return 0;

    const int size = (int) faultRing.size();
    const int from = std::max (faultRead, written - size);
    const int lost = from - faultRead;
    faultRead = written;

    auto file = getFaultLogFile();
    file.getParentDirectory().createDirectory();
    juce::FileOutputStream out (file);
    if (! out.openedOk())
        return 0;
    out.setPosition (out.getFile().getSize());

    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H:%M:%S");
    if (! faultLogHeaderWritten)
    {
        faultLogHeaderWritten = true;
        out << "\n" << stamp << "  " << pluginName << " " << versionString << "  sample rate " << (int) currentSampleRate
            << "  block " << preparedBlockSize << "\n";
    }
    if (lost > 0)
        out << stamp << "  (" << lost << " entries lost: too many in half a second)\n";

    int count = 0;
    bool wantParams = false;
    for (int i = from; i < written; ++i)
    {
        const auto f = faultRing[(size_t) (i % size)];
        out << stamp << "  " << (f.fault ? "FAULT" : "hot  ") << "  " << juce::String (stageName (f.stage)).paddedRight (' ', 10)
            << " peak " << (f.peakDb > 900.0f ? juce::String ("non-finite") : juce::String (f.peakDb, 1) + " dBFS")
            << "  at " << juce::String (f.seconds, 2) << " s  voices " << f.activeVoices << "  pattern " << (f.pattern + 1)
            << "  bpm " << juce::String (f.bpm, 1) << (f.running ? " running" : " stopped")
            << "  preset " << getProgramName (currentProgram) << "\n";
        wantParams = wantParams || f.fault;
        ++count;
    }

    if (wantParams)
    {
        juce::String line ("    params:");
        for (const auto& spec : paramSpecs())
            line << " " << spec.id << "=" << juce::String (apvts.getRawParameterValue (spec.id)->load(), 3);
        out << line << "\n    samples:";
        for (int s = 0; s < previewSlot; ++s)
            out << " " << s << "=\"" << bank.getRef (s).toString() << "\"";
        out << "\n";
    }
    out.flush();
    return count;
}

//==============================================================================
void LabX0AudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    preparedBlockSize = samplesPerBlock;
    currentSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    const auto sr = (float) currentSampleRate;

    for (int n = 0; n <= controlBlock; ++n)
        smoothCoef[(size_t) n] = 1.0f - std::exp (-(float) std::max (n, 1) / (0.015f * sr));

    for (int i = 0; i < P::count; ++i)
    {
        smoothers[(size_t) i].reset (rawValue (i));
        values[(size_t) i] = rawValue (i);
    }

    for (auto& v : padVoices)
        v.prepare (sr);
    for (auto& pair : breakVoices)
        for (auto& v : pair)
            v.prepare (sr);
    breakCurrent.fill (0);
    breakMidiNote.fill (-1);
    preview.prepare (sr);

    anomaly.prepare (currentSampleRate);
    transient.prepare (sr);
    multiband.prepare (sr);
    punch.prepare (currentSampleRate, controlBlock);
    regulator.prepare (sr);

    dryDelayLength = std::max (0, punch.getLatency());
    dryDelayL.assign ((size_t) dryDelayLength + 1, 0.0f);
    dryDelayR.assign ((size_t) dryDelayLength + 1, 0.0f);
    dryDelayPos = 0;

    const int latency = dryDelayLength + regulator.getLatency();
    setLatencySamples (latency);
    meters.latency.store (latency);

    busStorage.assign ((size_t) controlBlock * 8, 0.0f);

    masterGain = dsp::dbToGain (rawValue (P::volume));
    dcInL = dcInR = dcOutL = dcOutR = 0.0f;
    dcCoef = 1.0f - dsp::twoPi * 4.0f / sr;
    modWheel = modWheelTarget = 0.0f;
    midiAccumulator = meterSamples = 0;

    sequencer.reset();
    freeQ = 0.0;
    wasFree = false;
    hostPlaying = false;
    currentPattern = juce::jlimit (0, numPatterns - 1, choiceOf (P::pattern));
    queuedPattern = -1;
    lastParamPattern = currentPattern;
    for (auto& c : sliceCaches)
        c = SliceCache();

    programChangePending.store (false);
    switchState = SwitchState::idle;
    switchGain = 1.0f;
    switchFadeLength = std::max (1, (int) (0.03 * currentSampleRate));
    switchFadeRemaining = 0;
    switchFadeInStep = 1.0f / (0.005f * sr);
}

void LabX0AudioProcessor::releaseResources()
{
    for (auto& v : padVoices)
        v.kill();
    for (auto& pair : breakVoices)
        for (auto& v : pair)
            v.kill();
    preview.stop();
}

bool LabX0AudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::stereo() && out != juce::AudioChannelSet::mono())
        return false;
    return layouts.getMainInputChannelSet().isDisabled() || layouts.inputBuses.isEmpty();
}

//==============================================================================
void LabX0AudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    buffer.clear();

    bank.beginBlock (blockSamples);
    struct BlockEnd
    {
        SampleBank& b;
        ~BlockEnd() { b.endBlock(); }
    } blockEnd { bank };

    patterns.sync (localBank, seenPatternVersions, seenPatternTotal);

    if (programChangePending.exchange (false))
    {
        if (switchState != SwitchState::fadingOut)
        {
            switchState = SwitchState::fadingOut;
            switchFadeRemaining = (int) std::ceil (switchGain * (float) switchFadeLength);
        }
    }

    // Host transport: tempo always when reported, position only while playing.
    bool playing = false;
    double ppq = 0.0;
    if (auto* host = getPlayHead())
    {
        if (auto position = host->getPosition())
        {
            if (auto bpm = position->getBpm())
                if (*bpm > 1.0)
                    hostBpm = *bpm;
            if (position->getIsPlaying())
                if (auto p = position->getPpqPosition())
                {
                    playing = true;
                    ppq = *p;
                }
        }
    }
    bank.setHostBpm (hostBpm);

    const bool free = choiceOf (P::run) == Choice::runFree;
    const double qps = hostBpm / 60.0 / currentSampleRate;
    bool running = false;
    double q0 = 0.0;
    if (playing)
    {
        running = true;
        q0 = sequencer.continuePosition (ppq, qps);
        wasFree = false;
    }
    else if (free)
    {
        if (! wasFree)
        {
            freeQ = 0.0;
            sequencer.reset();
        }
        wasFree = true;
        running = true;
        q0 = freeQ;
    }
    else
    {
        wasFree = false;
    }
    hostPlaying = playing;

    // Pattern selection: the parameter and MIDI notes 96..111 queue the next pattern.
    const int paramPattern = juce::jlimit (0, numPatterns - 1, choiceOf (P::pattern));
    if (paramPattern != lastParamPattern)
    {
        lastParamPattern = paramPattern;
        queuedPattern = paramPattern == currentPattern ? -1 : paramPattern;
    }
    for (const auto metadata : midi)
    {
        const auto m = metadata.getMessage();
        if (m.isNoteOn() && m.getNoteNumber() >= midiFirstPattern && m.getNoteNumber() < midiFirstPattern + numPatterns)
            queuedPattern = m.getNoteNumber() - midiFirstPattern;
    }

    // Editor requests: auditions and preview stops.
    {
        const auto fifoScope = uiFifo.read (uiFifo.getNumReady());
        auto handle = [this] (int start, int size)
        {
            for (int i = start; i < start + size; ++i)
            {
                const auto& e = uiEvents[(size_t) i];
                if (e.type == 1)
                    preview.stop();
                else if (e.lane < numPads)
                    triggerPad (e.lane, e.velocity, 0, -1);
                else if (e.lane < laneTime)
                    triggerBreak (e.lane - laneFirstBreak, 1.0, 0, e.velocity, 0, false, -1);
            }
        };
        handle (fifoScope.startIndex1, fifoScope.blockSize1);
        handle (fifoScope.startIndex2, fifoScope.blockSize2);
    }

    // Preview: start once the requested file has been published.
    {
        const int target = previewRequest.load();
        const auto* d = blockSamples[(size_t) previewSlot];
        if (target != previewSeenRequest)
        {
            previewSeenRequest = target;
            previewWanted = true;
            preview.stop();
        }
        if (previewWanted && d != nullptr && d->generation >= target && target > 0)
        {
            preview.start (d);
            previewWanted = false;
        }
    }

    blockQps = qps;
    blockRunning = running;
    const float swing = rawValue (P::swing);
    const int numEvents = sequencer.collect (localBank, currentPattern, queuedPattern, running, q0, qps, numSamples, swing,
                                             events.data(), (int) events.size());

    auto midiIt = midi.cbegin();
    const auto midiEnd = midi.cend();

    if (numSamples == 0)
    {
        for (; midiIt != midiEnd; ++midiIt)
            handleMidi ((*midiIt).getMessage(), 0);
        return;
    }

    std::array<float, controlBlock> blockL {}, blockR {};
    double sumSquares = 0.0;
    float peak = 0.0f;
    int pos = 0, ei = 0;
    int scopePos = scopeWrite.load (std::memory_order_relaxed);

    while (pos < numSamples)
    {
        while (midiIt != midiEnd && (*midiIt).samplePosition <= pos)
        {
            handleMidi ((*midiIt).getMessage(), pos);
            ++midiIt;
        }
        while (ei < numEvents && events[(size_t) ei].offset <= pos)
            handleSeqEvent (events[(size_t) ei++]);

        int next = numSamples;
        if (midiIt != midiEnd)
            next = std::min (next, (*midiIt).samplePosition);
        if (ei < numEvents)
            next = std::min (next, events[(size_t) ei].offset);
        const int n = std::min ({ controlBlock, std::max (1, next - pos), numSamples - pos });

        renderControlBlock (blockL.data(), blockR.data(), n, q0 + (double) pos * qps);

        if (numChannels >= 2)
        {
            buffer.copyFrom (0, pos, blockL.data(), n);
            buffer.copyFrom (1, pos, blockR.data(), n);
        }
        else if (numChannels == 1)
        {
            buffer.copyFrom (0, pos, blockL.data(), n, 0.5f);
            buffer.addFrom (0, pos, blockR.data(), n, 0.5f);
        }

        for (int i = 0; i < n; ++i)
        {
            const float l = blockL[(size_t) i], r = blockR[(size_t) i];
            sumSquares += (double) (l * l + r * r) * 0.5;
            peak = std::max (peak, std::max (std::abs (l), std::abs (r)));
            scope[(size_t) scopePos] = 0.5f * (l + r);
            scopePos = (scopePos + 1) & (scopeSize - 1);
        }
        pos += n;
    }
    scopeWrite.store (scopePos, std::memory_order_release);

    for (; midiIt != midiEnd; ++midiIt)
        handleMidi ((*midiIt).getMessage(), numSamples);

    if (running && ! playing)
        freeQ += (double) numSamples * qps;

    // Readouts
    int active = 0;
    for (const auto& v : padVoices)
        if (v.isSounding())
            ++active;
    for (int b = 0; b < numBreaks; ++b)
    {
        const auto& v = breakVoices[(size_t) b][(size_t) breakCurrent[(size_t) b]];
        if (v.isSounding())
            ++active;
        meters.breakPosition[(size_t) b].store (v.isSounding() ? v.getPosition() : -1.0);
    }

    meterSamples += numSamples;
    if (meterSamples >= (int) (currentSampleRate * 0.1))
    {
        meters.midiPerSecond.store ((float) midiAccumulator * (float) currentSampleRate / (float) meterSamples);
        midiAccumulator = 0;
        meterSamples = 0;
    }

    const float rms = (float) std::sqrt (sumSquares / (double) numSamples);
    meters.rmsDb.store (juce::Decibels::gainToDecibels (rms, -100.0f));
    meters.peakDb.store (juce::Decibels::gainToDecibels (peak, -100.0f));
    meters.activeVoices.store (active);
    meters.emission.store (emissionValue);
    meters.bpm.store ((float) hostBpm);
    meters.synced.store (playing);
    meters.running.store (running);
    meters.playingPattern.store (currentPattern);
    meters.queuedPattern.store (queuedPattern);
    meters.transientDb.store (transient.getMeterDb());
    for (int b = 0; b < 3; ++b)
        meters.bandDb[(size_t) b].store (multiband.getBandMeterDb (b));
    meters.slamDb.store (punch.getSlamMeterDb());
    meters.clipDb.store (punch.getClipMeterDb());
    meters.limitDb.store (-regulator.getGainReductionDb());
    meters.timeSlot.store (anomaly.getActiveTimeSlot());
    meters.volumeSlot.store (anomaly.getActiveVolumeSlot());

    const double qEnd = q0 + (double) numSamples * qps;
    const auto& pat = localBank.patterns[(size_t) currentPattern];
    meters.position.store (running ? qEnd - sequencer.getPatternStartQ() : -1.0);
    if (running)
    {
        const auto stepIndex = sequencer.stepIndexAt (pat, qEnd - 1.0e-9);
        for (int lane = 0; lane < numLanes; ++lane)
            meters.laneStep[(size_t) lane].store (dsp::StepSequencer::laneStepFor (stepIndex, pat.laneLength (lane)));
    }
    else
    {
        for (auto& s : meters.laneStep)
            s.store (-1);
    }
}

//==============================================================================
void LabX0AudioProcessor::updateControl (int n) noexcept
{
    const float coef = smoothCoef[(size_t) std::clamp (n, 0, controlBlock)];
    for (int i = 0; i < P::count; ++i)
    {
        const float target = rawValue (i);
        values[(size_t) i] = smoothers[(size_t) i].enabled ? smoothers[(size_t) i].advance (target, coef) : target;
    }
    modWheel += (modWheelTarget - modWheel) * coef;

    const auto& v = values;
    const auto sr = (float) currentSampleRate;
    emissionValue = std::clamp (v[P::emission] + modWheel, 0.0f, 1.0f);

    for (int p = 0; p < numPads; ++p)
    {
        auto& s = padSettings[(size_t) p];
        const float gain = dsp::dbToGain (v[(size_t) P::pad (p, P::padLevel)]);
        float pl = 1.0f, pr = 1.0f;
        dsp::panGains (v[(size_t) P::pad (p, P::padPan)], pl, pr);
        s.gainL = gain * pl;
        s.gainR = gain * pr;
        s.pitchRatio = dsp::semisToRatio (v[(size_t) P::pad (p, P::padTune)]);
        s.tone = dsp::ToneSettings::make (v[(size_t) P::pad (p, P::padTone)], v[(size_t) P::pad (p, P::padReso)], sr);
        const float driveDb = v[(size_t) P::pad (p, P::padDrive)];
        s.driving = driveDb > 0.05f;
        s.drive = dsp::dbToGain (driveDb);
        s.driveComp = 0.5f / dsp::fastTanh (0.5f * std::max (s.drive, 1.0f));
    }

    for (int b = 0; b < numBreaks; ++b)
    {
        auto& s = breakSettings[(size_t) b];
        const float gain = dsp::dbToGain (v[(size_t) P::brk (b, P::brLevel)]);
        float pl = 1.0f, pr = 1.0f;
        dsp::panGains (v[(size_t) P::brk (b, P::brPan)], pl, pr);
        s.gainL = gain * pl;
        s.gainR = gain * pr;
        s.fit = std::clamp ((int) std::lround (v[(size_t) P::brk (b, P::brFit)]), 0, 2);
        s.tone = dsp::ToneSettings::make (v[(size_t) P::brk (b, P::brTone)], 0.0f, sr);

        const auto* d = blockSamples[(size_t) (slotFirstBreak + b)];
        if (d != nullptr && d->length > 0)
        {
            const double base = d->sampleRate / currentSampleRate;
            const double seconds = (double) d->length / d->sampleRate;
            const double loopBpm = loopBeats (b, d) * 60.0 / seconds;
            const double tempo = loopBpm > 1.0 ? hostBpm / loopBpm : 1.0;
            const double tune = (double) dsp::semisToRatio (v[(size_t) P::brk (b, P::brTune)]);
            s.rate = s.fit == Choice::fitSlice ? base * tune : base * tempo * tune;
            s.advance = base * tempo;
            s.grainRate = base * tune;
        }
    }

    for (auto& voice : padVoices)
        if (voice.isActive())
            voice.updateFilter (padSettings[(size_t) voice.getPad()].tone);
    for (int b = 0; b < numBreaks; ++b)
        for (auto& voice : breakVoices[(size_t) b])
            if (voice.isActive())
                voice.updateFilter (breakSettings[(size_t) b].tone);

    widthValue = v[P::width];
}

void LabX0AudioProcessor::renderControlBlock (float* outL, float* outR, int n, double q) noexcept
{
    updateControl (n);

    const auto block = (size_t) controlBlock;
    float* bus[6];
    for (int i = 0; i < 6; ++i)
    {
        bus[i] = busStorage.data() + (size_t) i * block;
        std::fill_n (bus[i], n, 0.0f);
    }
    float* tempL = busStorage.data() + 6 * block;
    float* tempR = busStorage.data() + 7 * block;
    float* fullL = bus[0]; float* fullR = bus[1];
    float* punchL = bus[2]; float* punchR = bus[3];
    float* dryL = bus[4]; float* dryR = bus[5];
    float* busL[3] = { fullL, punchL, dryL };
    float* busR[3] = { fullR, punchR, dryR };

    // Every voice renders on its own first, so a voice that runs away is stopped alone and named in the log.
    const dsp::VoiceOutput temp { { tempL, tempL, tempL }, { tempR, tempR, tempR } };
    auto addVoice = [&] (int route)
    {
        const float pk = blockPeak (tempL, tempR, n);
        if (! (pk <= voiceLimit))
        {
            noteFault (stageVoice, pk, true);
            return false;
        }
        for (int i = 0; i < n; ++i)
        {
            busL[route][i] += tempL[i];
            busR[route][i] += tempR[i];
        }
        return true;
    };

    for (auto& voice : padVoices)
    {
        if (! voice.isActive())
            continue;
        std::fill_n (tempL, n, 0.0f);
        std::fill_n (tempR, n, 0.0f);
        voice.render (blockSamples[(size_t) voice.getPad()], padSettings[(size_t) voice.getPad()], temp, n);
       #if LABX0_HARNESS
        if (injectStage == 6 && injectAtSample >= samplesRendered && injectAtSample < samplesRendered + n)
            tempL[0] = std::numeric_limits<float>::infinity();
       #endif
        if (! addVoice (std::clamp (voice.getRoute(), 0, 2)))
            voice.kill();
    }

    for (int b = 0; b < numBreaks; ++b)
        for (auto& voice : breakVoices[(size_t) b])
        {
            if (! voice.isActive())
                continue;
            std::fill_n (tempL, n, 0.0f);
            std::fill_n (tempR, n, 0.0f);
            voice.render (blockSamples[(size_t) (slotFirstBreak + b)], breakSettings[(size_t) b], temp, n);
            if (! addVoice (std::clamp (voice.getRoute(), 0, 2)))
                voice.kill();
        }

    if (preview.isActive())
        preview.render (blockSamples[(size_t) previewSlot], dryL, dryR, n, 0.5f);

    // Runaway guard after every stage: above +18 dBFS logged as hot; non-finite or above +42 dBFS a fault that
    // silences the block and resets every stage.
    bool runaway = false;
    auto check = [&] (int stage, const float* l, const float* r)
    {
        if (runaway)
            return;
       #if LABX0_HARNESS
        if (stage == injectStage && injectAtSample >= samplesRendered && injectAtSample < samplesRendered + n)
            const_cast<float*> (l)[0] = 1.0e9f;
       #endif
        const float pk = blockPeak (l, r, n);
       #if LABX0_HARNESS
        stagePeak[(size_t) stage] = std::max (stagePeak[(size_t) stage], std::isfinite (pk) ? pk : 1.0e30f);
       #endif
        if (! (pk <= stageLimit))
        {
            runaway = true;
            noteFault (stage, pk, true);
        }
        else if (pk > hotLevel)
            noteFault (stage, pk, false);
    };

    check (stageVoice, fullL, fullR);
    check (stageVoice, punchL, punchR);
    check (stageVoice, dryL, dryR);

    // ANOMALY on the FULL bus
    dsp::Anomaly::Context ctx;
    ctx.pattern = &localBank.patterns[(size_t) currentPattern];
    ctx.sequencer = &sequencer;
    ctx.running = blockRunning;
    ctx.q0 = q;
    ctx.qps = blockQps;
    ctx.mix = std::clamp (values[P::anomalyMix] + 0.3f * emissionValue, 0.0f, 1.0f);
    ctx.scratchDepth = std::clamp (values[P::scratchDepth] + 0.4f * emissionValue, 0.0f, 1.0f);
    anomaly.process (fullL, fullR, n, ctx);
    check (stageAnomaly, fullL, fullR);

    // PUNCH: FULL and PUNCH buses together
    for (int i = 0; i < n; ++i)
    {
        fullL[i] += punchL[i];
        fullR[i] += punchR[i];
    }

    const auto& v = values;
    const float e = emissionValue;
    transient.process (fullL, fullR, n, std::clamp (v[P::trAttack] + 0.5f * e, -1.0f, 1.0f), v[P::trSustain]);
    check (stageTransient, fullL, fullR);

    const float bandGains[3] = { v[P::mbLowGain], v[P::mbMidGain], v[P::mbHighGain] };
    multiband.process (fullL, fullR, n, std::clamp (v[P::mbDepth] + 0.35f * e, 0.0f, 1.0f), v[P::mbTime], v[P::mbUp], v[P::mbDown],
                       v[P::mbLowX], v[P::mbHighX], bandGains);
    check (stageMultiband, fullL, fullR);

    const int sc = std::clamp ((int) std::lround (v[P::slamSc]), 0, 2);
    const int split = std::clamp ((int) std::lround (v[P::subSafe]), 0, 6);
    punch.process (fullL, fullR, n, std::clamp (v[P::slamBark] + 0.35f * e, 0.0f, 1.0f), std::clamp (v[P::slamBite] + 0.3f * e, 0.0f, 1.0f),
                   v[P::slamWet], slamScHz[sc], v[P::clipDrive] + 6.0f * e, subSafeHz[split]);
    check (stageSlam, fullL, fullR);

   #if LABX0_HARNESS
    samplesRendered += n;
   #endif

    if (runaway)
    {
        std::fill_n (fullL, n, 0.0f);
        std::fill_n (fullR, n, 0.0f);
        std::fill_n (dryL, n, 0.0f);
        std::fill_n (dryR, n, 0.0f);
        recoverFromFault();
    }

    // DRY waits for the punch chain's latency, then everything meets.
    const float gainTarget = dsp::dbToGain (v[P::volume]);
    for (int i = 0; i < n; ++i)
    {
        float dl = dryL[i], dr = dryR[i];
        if (dryDelayLength > 0)
        {
            dryDelayL[(size_t) dryDelayPos] = dl;
            dryDelayR[(size_t) dryDelayPos] = dr;
            int readPos = dryDelayPos + 1;
            if (readPos > dryDelayLength)
                readPos = 0;
            dl = dryDelayL[(size_t) readPos];
            dr = dryDelayR[(size_t) readPos];
            dryDelayPos = readPos;
        }

        const float a = fullL[i] + dl;
        const float b = fullR[i] + dr;
        masterGain += (gainTarget - masterGain) * 0.002f;
        const float mid = 0.5f * (a + b);
        const float side = 0.5f * (a - b) * widthValue;
        const float l = (mid + side) * masterGain;
        const float r = (mid - side) * masterGain;

        const float ol = l - dcInL + dcCoef * dcOutL;
        const float orr = r - dcInR + dcCoef * dcOutR;
        dcInL = l; dcOutL = ol;
        dcInR = r; dcOutR = orr;
        outL[i] = ol;
        outR[i] = orr;
    }

    regulator.process (outL, outR, n, 0.0f, v[P::ceiling], 0.0f);

    bool fault = false;
    for (int i = 0; i < n; ++i)
    {
        float l = outL[i];
        float r = outR[i];
        if (! std::isfinite (l) || ! std::isfinite (r))
        {
            fault = true;
            l = r = 0.0f;
        }

        if (switchState == SwitchState::fadingOut)
        {
            switchGain = (float) switchFadeRemaining / (float) switchFadeLength;
            if (switchFadeRemaining > 0)
                --switchFadeRemaining;
        }
        else if (switchState == SwitchState::fadingIn)
        {
            switchGain = std::min (1.0f, switchGain + switchFadeInStep);
            if (switchGain >= 1.0f)
                switchState = SwitchState::idle;
        }

        outL[i] = dsp::safetyClip (l) * switchGain;
        outR[i] = dsp::safetyClip (r) * switchGain;
    }

    if (fault)
    {
        noteFault (stageOutput, std::numeric_limits<float>::quiet_NaN(), true);
        recoverFromFault();
    }

    engineClock += n;

    if (switchState == SwitchState::fadingOut && switchFadeRemaining == 0)
    {
        flushForProgramChange();
        switchState = SwitchState::fadingIn;
        switchGain = 0.0f;
    }
}

//==============================================================================
void LabX0AudioProcessor::handleSeqEvent (const dsp::SeqEvent& e) noexcept
{
    if (e.lane < numPads)
        triggerPad (e.lane, e.velocity, e.pitch, e.gateSamples);
    else if (e.lane < laneTime)
        triggerBreakStep (e.lane - laneFirstBreak, e);
}

void LabX0AudioProcessor::triggerPad (int pad, float velocity, int pitch, int gateSamples) noexcept
{
    if (! juce::isPositiveAndBelow (pad, numPads))
        return;
    meters.laneFlash[(size_t) pad].store (velocity);
    const auto* d = blockSamples[(size_t) pad];
    if (d == nullptr)
        return;

    const int mode = std::clamp (choiceOf (P::pad (pad, P::padMode)), 0, 2);
    const int choke = choiceOf (P::pad (pad, P::padChoke));
    const int route = std::clamp (choiceOf (P::pad (pad, P::padRoute)), 0, 2);

    // Choke groups silence each other; Mono and Gate pads cut their own previous hit.
    int sameCount = 0;
    const dsp::PadVoice* oldest = nullptr;
    for (auto& v : padVoices)
    {
        if (! v.isSounding())
            continue;
        const int other = v.getPad();
        if (other == pad)
        {
            if (mode != Choice::modePoly)
                v.fadeOut();
            else
            {
                ++sameCount;
                if (oldest == nullptr || v.getStamp() < oldest->getStamp())
                    oldest = &v;
            }
        }
        else if (choke > 0 && choiceOf (P::pad (other, P::padChoke)) == choke)
            v.fadeOut();
    }
    if (mode == Choice::modePoly && sameCount >= maxVoicesPerPad && oldest != nullptr)
        const_cast<dsp::PadVoice*> (oldest)->fadeOut();

    // A free voice, else the quietest fading one, else the oldest.
    dsp::PadVoice* target = nullptr;
    for (auto& v : padVoices)
        if (! v.isActive())
        {
            target = &v;
            break;
        }
    if (target == nullptr)
    {
        float quietest = 2.0f;
        for (auto& v : padVoices)
            if (! v.isSounding() && v.getLevel() < quietest)
            {
                quietest = v.getLevel();
                target = &v;
            }
    }
    if (target == nullptr)
    {
        uint64_t oldestStamp = std::numeric_limits<uint64_t>::max();
        for (auto& v : padVoices)
            if (v.getStamp() < oldestStamp)
            {
                oldestStamp = v.getStamp();
                target = &v;
            }
    }
    if (target == nullptr)
        return;

    const float velAmount = rawValue (P::pad (pad, P::padVel));
    const float decay = rawValue (P::pad (pad, P::padDecay));

    dsp::PadVoice::Start s;
    s.pad = pad;
    s.route = route;
    s.velocityGain = 1.0f - velAmount * (1.0f - std::clamp (velocity, 0.0f, 1.0f));
    s.stepPitch = (float) pitch;
    s.startNorm = rawValue (P::pad (pad, P::padStart));
    s.reverse = choiceOf (P::pad (pad, P::padReverse)) == 1;
    s.attackSeconds = rawValue (P::pad (pad, P::padAttack)) * 0.001f;
    s.holdSeconds = rawValue (P::pad (pad, P::padHold)) * 0.001f;
    s.decaySeconds = decay;
    s.fullLength = decay >= decayFull;
    s.gated = mode == Choice::modeGate;
    s.gateSamples = gateSamples;
    s.stamp = ++voiceStamp;

    target->kill();
    target->start (d, s);
    target->updateFilter (padSettings[(size_t) pad].tone);
}

double LabX0AudioProcessor::loopBeats (int b, const dsp::SampleData* d) const noexcept
{
    const int choice = std::clamp (choiceOf (P::brk (b, P::brBeats)), 0, (int) std::size (breakBeatsChoices) - 1);
    if (breakBeatsChoices[choice] > 0.0f)
        return breakBeatsChoices[choice];
    if (d->beats > 0.0)
        return d->beats;
    const double seconds = (double) d->length / d->sampleRate;
    return std::max (1.0, std::round (seconds * hostBpm / 60.0));
}

const LabX0AudioProcessor::SliceCache& LabX0AudioProcessor::slicesFor (int b, const dsp::SampleData* d) noexcept
{
    auto& c = sliceCaches[(size_t) b];
    const int slicing = std::clamp (choiceOf (P::brk (b, P::brSlicing)), 0, 3);
    const float sens = rawValue (P::brk (b, P::brSens));
    const double beats = loopBeats (b, d);
    if (c.dataId == d->id && c.slicing == slicing && std::abs (c.sens - sens) < 1.0e-4f && std::abs (c.beats - beats) < 1.0e-6)
        return c;

    c.dataId = d->id;
    c.slicing = slicing;
    c.sens = sens;
    c.beats = beats;
    if (slicing == Choice::sliceAuto && ! d->onsetPos.empty())
    {
        const double seconds = (double) d->length / d->sampleRate;
        const double loopBpm = beats * 60.0 / seconds;
        const int minSpacing = (int) (60.0 / std::max (20.0, loopBpm) / 8.0 * d->sampleRate);   // a 1/32 note
        const int count = std::min ((int) d->onsetPos.size(), (int) sliceScratch.size());
        c.count = analysis::onsetSlices (d->onsetPos.data(), d->onsetStrength.data(), count, sens, minSpacing, d->length,
                                         c.starts.data(), maxSlices, sliceScratch.data());
    }
    else
    {
        const int divisions = slicing == Choice::slice8 ? 2 : (slicing == Choice::slice32 ? 8 : 4);
        c.count = analysis::gridSlices (d->length, beats, divisions, c.starts.data(), maxSlices);
    }
    c.count = std::max (1, c.count);
    return c;
}

void LabX0AudioProcessor::triggerBreak (int b, double sourcePos, int slice, float velocity, int pitch, bool midiNote, int note) noexcept
{
    const auto* d = blockSamples[(size_t) (slotFirstBreak + b)];
    meters.laneFlash[(size_t) (laneFirstBreak + b)].store (velocity);
    if (d == nullptr)
        return;

    const auto& sc = slicesFor (b, d);
    const int gate = choiceOf (P::brk (b, P::brGate));
    const float decay = rawValue (P::brk (b, P::brDecay));

    double stopAt = -1.0;
    if (gate == Choice::gateSlice)
    {
        // The end of the slice that contains the start position.
        stopAt = (double) d->length;
        for (int i = 0; i < sc.count; ++i)
            if ((double) sc.starts[(size_t) i] > sourcePos + 1.0)
            {
                stopAt = (double) sc.starts[(size_t) i];
                break;
            }
    }

    auto& pair = breakVoices[(size_t) b];
    pair[(size_t) breakCurrent[(size_t) b]].fadeOut();
    breakCurrent[(size_t) b] ^= 1;
    auto& voice = pair[(size_t) breakCurrent[(size_t) b]];

    dsp::BreakVoice::Start s;
    s.sourcePos = sourcePos;
    s.stopAt = stopAt;
    s.velocityGain = 0.4f + 0.6f * std::clamp (velocity, 0.0f, 1.0f);
    s.stepPitchRatio = dsp::semisToRatio ((float) pitch);
    s.decaySeconds = decay;
    s.fullLength = decay >= decayFull;
    s.route = std::clamp (choiceOf (P::brk (b, P::brRoute)), 0, 2);
    s.slice = slice;
    voice.kill();
    voice.start (d, s);
    voice.updateFilter (breakSettings[(size_t) b].tone);
    breakMidiNote[(size_t) b] = midiNote ? note : -1;
}

void LabX0AudioProcessor::triggerBreakStep (int b, const dsp::SeqEvent& e) noexcept
{
    const auto* d = blockSamples[(size_t) (slotFirstBreak + b)];
    if (d == nullptr)
    {
        meters.laneFlash[(size_t) (laneFirstBreak + b)].store (e.velocity);
        return;
    }
    const auto& sc = slicesFor (b, d);
    const int fit = std::clamp (choiceOf (P::brk (b, P::brFit)), 0, 2);

    if (e.slice >= 0)
    {
        const int s = e.slice % sc.count;
        triggerBreak (b, (double) sc.starts[(size_t) s], s, e.velocity, e.pitch, false, -1);
        return;
    }

    // Natural: the position this step holds in the loop, counted from the pattern start.
    const double beats = loopBeats (b, d);
    double frac = std::fmod (e.stepQuarter, beats) / beats;
    if (frac < 0.0)
        frac += 1.0;
    double sourcePos = frac * (double) d->length;
    int slice = 0;
    while (slice + 1 < sc.count && (double) sc.starts[(size_t) slice + 1] <= sourcePos + 0.5)
        ++slice;
    if (fit == Choice::fitSlice)
        sourcePos = (double) sc.starts[(size_t) slice];

    // Already there: a voice playing on through this point is not restarted (ratchet repeats always are).
    const auto& voice = breakVoices[(size_t) b][(size_t) breakCurrent[(size_t) b]];
    if (! e.repeat && voice.isSounding() && voice.getDataId() == d->id)
    {
        if (fit == Choice::fitSlice ? voice.getSlice() == slice
                                    : std::abs (voice.getPosition() - sourcePos) < 0.012 * d->sampleRate)
        {
            meters.laneFlash[(size_t) (laneFirstBreak + b)].store (e.velocity);
            return;
        }
    }
    triggerBreak (b, std::max (1.0, sourcePos), slice, e.velocity, e.pitch, false, -1);
}

void LabX0AudioProcessor::triggerBreakSlice (int b, int slice, float velocity, int note) noexcept
{
    const auto* d = blockSamples[(size_t) (slotFirstBreak + b)];
    if (d == nullptr)
        return;
    const auto& sc = slicesFor (b, d);
    const int s = std::clamp (slice, 0, maxSlices - 1) % sc.count;
    triggerBreak (b, std::max (1.0, (double) sc.starts[(size_t) s]), s, velocity, 0, true, note);
}

void LabX0AudioProcessor::releaseMidiNote (int note) noexcept
{
    if (note >= midiFirstPad && note < midiFirstPad + numPads)
    {
        const int pad = note - midiFirstPad;
        for (auto& v : padVoices)
            if (v.isSounding() && v.getPad() == pad)
                v.noteOff();
    }
    for (int b = 0; b < numBreaks; ++b)
        if (breakMidiNote[(size_t) b] == note)
        {
            if (choiceOf (P::brk (b, P::brGate)) == Choice::gateLegato)
                breakVoices[(size_t) b][(size_t) breakCurrent[(size_t) b]].release();
            breakMidiNote[(size_t) b] = -1;
        }
}

void LabX0AudioProcessor::handleMidi (const juce::MidiMessage& m, int) noexcept
{
    ++midiAccumulator;
    if (m.isNoteOn())
    {
        const int note = m.getNoteNumber();
        const float vel = m.getFloatVelocity();
        if (note >= midiFirstPad && note < midiFirstPad + numPads)
            triggerPad (note - midiFirstPad, vel, 0, -1);
        else if (note >= midiFirstBreak && note < midiFirstBreak + numBreaks)
            triggerBreak (note - midiFirstBreak, 1.0, 0, vel, 0, true, note);
        else if (note >= midiFirstSlice && note < midiFirstSlice + midiSliceCount)
            triggerBreakSlice (std::clamp (choiceOf (P::sliceMidi), 0, numBreaks - 1), note - midiFirstSlice, vel, note);
    }
    else if (m.isNoteOff())
        releaseMidiNote (m.getNoteNumber());
    else if (m.isAllNotesOff())
    {
        for (int note = 0; note < 128; ++note)
            releaseMidiNote (note);
    }
    else if (m.isAllSoundOff())
        allSoundOff();
    else if (m.isController() && m.getControllerNumber() == 1)
        modWheelTarget = (float) m.getControllerValue() / 127.0f;
}

void LabX0AudioProcessor::allSoundOff() noexcept
{
    for (auto& v : padVoices)
        v.kill();
    for (auto& pair : breakVoices)
        for (auto& v : pair)
            v.kill();
    preview.stop();
    anomaly.reset();
}

void LabX0AudioProcessor::flushForProgramChange() noexcept
{
    for (auto& v : padVoices)
        v.kill();
    for (auto& pair : breakVoices)
        for (auto& v : pair)
            v.kill();
    preview.stop();
    anomaly.reset();
    transient.reset();
    multiband.reset();
    punch.reset();
    regulator.reset();
    std::fill (dryDelayL.begin(), dryDelayL.end(), 0.0f);
    std::fill (dryDelayR.begin(), dryDelayR.end(), 0.0f);
    dcInL = dcInR = dcOutL = dcOutR = 0.0f;
    for (int i = 0; i < P::count; ++i)
        smoothers[(size_t) i].reset (rawValue (i));
    masterGain = dsp::dbToGain (rawValue (P::volume));
    currentPattern = juce::jlimit (0, numPatterns - 1, choiceOf (P::pattern));
    lastParamPattern = currentPattern;
    queuedPattern = -1;
}

void LabX0AudioProcessor::recoverFromFault() noexcept
{
    for (auto& v : padVoices)
        v.kill();
    for (auto& pair : breakVoices)
        for (auto& v : pair)
            v.kill();
    anomaly.reset();
    transient.reset();
    multiband.reset();
    punch.reset();
    regulator.reset();
    std::fill (dryDelayL.begin(), dryDelayL.end(), 0.0f);
    std::fill (dryDelayR.begin(), dryDelayR.end(), 0.0f);
    dcInL = dcInR = dcOutL = dcOutR = 0.0f;
}

float LabX0AudioProcessor::blockPeak (const float* l, const float* r, int n) noexcept
{
    float pk = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float a = std::abs (l[i]), b = std::abs (r[i]);
        if (! (a < 3.0e38f) || ! (b < 3.0e38f))
            return std::numeric_limits<float>::quiet_NaN();
        pk = std::max (pk, std::max (a, b));
    }
    return pk;
}

void LabX0AudioProcessor::noteFault (int stage, float peak, bool fault) noexcept
{
    const int64_t now = engineClock;
    if (! fault)
    {
        auto& last = lastHotAt[(size_t) stage];
        if (last >= 0 && now - last < (int64_t) (2.0 * currentSampleRate))
            return;
        last = now;
    }

    const int index = faultWrite.load (std::memory_order_relaxed);
    auto& f = faultRing[(size_t) (index % (int) faultRing.size())];
    f.stage = stage;
    f.peakDb = std::isfinite (peak) ? juce::Decibels::gainToDecibels (peak, -120.0f) : 999.0f;
    f.fault = fault;
    f.seconds = (double) now / currentSampleRate;
    f.activeVoices = meters.activeVoices.load (std::memory_order_relaxed);
    f.pattern = currentPattern;
    f.bpm = hostBpm;
    f.running = blockRunning;
    faultWrite.store (index + 1, std::memory_order_release);

    if (fault)
    {
        meters.faults.fetch_add (1);
        meters.lastFaultStage.store (stage);
    }
}

//==============================================================================
void LabX0AudioProcessor::assignSample (int slot, const SampleRef& ref, double offsetSeconds)
{
    bank.assign (slot, ref, offsetSeconds);
}

SampleRef LabX0AudioProcessor::resolveKitRef (const juce::String& text) const
{
    if (text.startsWith ("KIT:"))
    {
        if (const auto* e = findKitEntry (text.substring (4)))
            return { e->root, e->path };
        return {};
    }
    return SampleRef::fromString (text);
}

void LabX0AudioProcessor::previewSample (const SampleRef& ref)
{
    const int generation = bank.assign (previewSlot, ref);
    previewRequest.store (generation);
}

void LabX0AudioProcessor::stopPreview()
{
    const auto fifoScope = uiFifo.write (1);
    if (fifoScope.blockSize1 > 0)
        uiEvents[(size_t) fifoScope.startIndex1] = { 1, 0, 0.0f };
}

void LabX0AudioProcessor::audition (int lane, float velocity)
{
    const auto fifoScope = uiFifo.write (1);
    if (fifoScope.blockSize1 > 0)
        uiEvents[(size_t) fifoScope.startIndex1] = { 0, lane, velocity };
}

int LabX0AudioProcessor::getEditPattern() const noexcept
{
    return juce::jlimit (0, numPatterns - 1, choiceOf (P::pattern));
}

void LabX0AudioProcessor::setEditPattern (int index)
{
    if (auto* param = apvts.getParameter ("pattern"))
        param->setValueNotifyingHost (param->convertTo0to1 ((float) juce::jlimit (0, numPatterns - 1, index)));
}

double LabX0AudioProcessor::loopBeatsFor (int b, const SampleBank::Overview& ov) const
{
    const int choice = std::clamp (choiceOf (P::brk (b, P::brBeats)), 0, (int) std::size (breakBeatsChoices) - 1);
    if (breakBeatsChoices[choice] > 0.0f)
        return breakBeatsChoices[choice];
    if (ov.beats > 0.0)
        return ov.beats;
    return std::max (1.0, std::round (ov.seconds * meters.bpm.load() / 60.0));
}

int LabX0AudioProcessor::computeSlices (int b, const SampleBank::Overview& ov, int* out, int maxOut) const
{
    const int slicing = std::clamp (choiceOf (P::brk (b, P::brSlicing)), 0, 3);
    const float sens = rawValue (P::brk (b, P::brSens));
    const double beats = loopBeatsFor (b, ov);
    int count;
    if (slicing == Choice::sliceAuto && ! ov.onsetPos.empty())
    {
        const double loopBpm = beats * 60.0 / std::max (0.01, ov.seconds);
        const int minSpacing = (int) (60.0 / std::max (20.0, loopBpm) / 8.0 * ov.sampleRate);
        std::vector<int> scratch (ov.onsetPos.size());
        count = analysis::onsetSlices (ov.onsetPos.data(), ov.onsetStrength.data(), (int) ov.onsetPos.size(), sens, minSpacing,
                                       ov.length, out, maxOut, scratch.data());
    }
    else
    {
        const int divisions = slicing == Choice::slice8 ? 2 : (slicing == Choice::slice32 ? 8 : 4);
        count = analysis::gridSlices (ov.length, beats, divisions, out, maxOut);
    }
    return std::max (1, count);
}

void LabX0AudioProcessor::copyScope (float* dest, int numSamples) const noexcept
{
    numSamples = std::min (numSamples, scopeSize);
    int r = (scopeWrite.load (std::memory_order_acquire) - numSamples) & (scopeSize - 1);
    for (int i = 0; i < numSamples; ++i)
    {
        dest[i] = scope[(size_t) r];
        r = (r + 1) & (scopeSize - 1);
    }
}

//==============================================================================
int LabX0AudioProcessor::getNumPrograms()
{
    return (int) factoryPresets().size();
}

int LabX0AudioProcessor::getCurrentProgram()
{
    return currentProgram;
}

void LabX0AudioProcessor::setCurrentProgram (int index)
{
    if (juce::isPositiveAndBelow (index, getNumPrograms()))
        applyPreset (index);
}

const juce::String LabX0AudioProcessor::getProgramName (int index)
{
    const auto& presets = factoryPresets();
    return juce::isPositiveAndBelow (index, (int) presets.size()) ? presets[(size_t) index].name : juce::String();
}

void LabX0AudioProcessor::handleAsyncUpdate()
{
    const int index = pendingProgram.exchange (-1);
    if (index >= 0)
        applyPreset (index);
}

void LabX0AudioProcessor::applyPreset (int index)
{
    const auto& presets = factoryPresets();
    if (! juce::isPositiveAndBelow (index, (int) presets.size()))
        return;

    // Presets load samples and patterns: do that on the message thread.
    if (auto* mm = juce::MessageManager::getInstanceWithoutCreating())
        if (! mm->isThisTheMessageThread())
        {
            pendingProgram.store (index);
            triggerAsyncUpdate();
            return;
        }

    // Every parameter is set once, straight to its final value.
    const auto& preset = presets[(size_t) index];
    for (const auto& spec : paramSpecs())
    {
        auto* param = apvts.getParameter (spec.id);
        if (param == nullptr)
            continue;
        float target = param->convertFrom0to1 (param->getDefaultValue());
        for (const auto& [id, value] : preset.values)
            if (id == spec.id)
            {
                target = value;
                break;
            }
        param->setValueNotifyingHost (param->convertTo0to1 (target));
    }

    for (int slot = 0; slot < previewSlot; ++slot)
        bank.assign (slot, resolveKitRef (preset.samples[(size_t) slot]));

    patterns.editAll ([&preset] (PatternBank& bank)
    {
        for (auto& p : bank.patterns)
            p.clear();
        for (const auto& [i, text] : preset.patterns)
        {
            juce::String error;
            if (juce::isPositiveAndBelow (i, numPatterns))
            {
                const bool ok = parsePatternText (text, bank.patterns[(size_t) i], error);
                jassert (ok);
                juce::ignoreUnused (ok);
            }
        }
    });

    currentProgram = index;
    programChangePending.store (true);
}

//==============================================================================
void LabX0AudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("version", versionString, nullptr);
    state.setProperty ("program", currentProgram, nullptr);
    for (int slot = 0; slot < previewSlot; ++slot)
    {
        state.setProperty ("slot" + juce::String (slot), bank.getRef (slot).toString(), nullptr);
        state.setProperty ("offset" + juce::String (slot), bank.getOffset (slot), nullptr);
    }
    juce::MemoryBlock blob;
    patterns.read ([&blob] (const PatternBank& bank) { blob = serialisePatterns (bank); });
    state.setProperty ("patterns", blob.toBase64Encoding(), nullptr);
    state.setProperty ("uiLane", uiSelectedLane.load(), nullptr);
    state.setProperty ("uiPage", uiPage.load(), nullptr);
    state.setProperty ("uiStrip", uiStripMode.load(), nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void LabX0AudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    auto tree = juce::ValueTree::fromXml (*xml);
    currentProgram = juce::jlimit (0, getNumPrograms() - 1, (int) tree.getProperty ("program", 0));

    std::array<SampleRef, previewSlot> refs;
    std::array<double, previewSlot> offsets {};
    for (int slot = 0; slot < previewSlot; ++slot)
    {
        refs[(size_t) slot] = SampleRef::fromString (tree.getProperty ("slot" + juce::String (slot)).toString());
        offsets[(size_t) slot] = (double) tree.getProperty ("offset" + juce::String (slot), 0.0);
    }

    juce::MemoryBlock blob;
    const bool havePatterns = blob.fromBase64Encoding (tree.getProperty ("patterns").toString());
    uiSelectedLane.store (juce::jlimit (0, numLanes - 1, (int) tree.getProperty ("uiLane", 0)));
    uiPage.store (juce::jlimit (0, 1, (int) tree.getProperty ("uiPage", 0)));
    uiStripMode.store (juce::jlimit (0, 5, (int) tree.getProperty ("uiStrip", 0)));

    apvts.replaceState (tree);

    for (int slot = 0; slot < previewSlot; ++slot)
        if (refs[(size_t) slot] != bank.getRef (slot) || offsets[(size_t) slot] != bank.getOffset (slot))
            bank.assign (slot, refs[(size_t) slot], offsets[(size_t) slot]);

    if (havePatterns)
    {
        PatternBank restored;
        if (deserialisePatterns (blob.getData(), blob.getSize(), restored))
            patterns.editAll ([&restored] (PatternBank& b) { b = restored; });
    }
}

//==============================================================================
juce::AudioProcessorEditor* LabX0AudioProcessor::createEditor()
{
    return new LabX0AudioProcessorEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new LabX0AudioProcessor();
}
