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

// The main stereo output, then 20 more, one per lane if wanted. They are active by default: FL Studio skips outputs a
// plugin marks inactive unless "Process inactive inputs and outputs" is set, and "Auto map outputs" maps the rest to
// consecutive mixer tracks.
juce::AudioProcessor::BusesProperties LabX0AudioProcessor::makeBuses()
{
    auto buses = BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true);
    for (int k = 0; k < numAuxOutputs; ++k)
        buses = buses.withOutput ("Out " + juce::String (k + 2), juce::AudioChannelSet::stereo(), true);
    return buses;
}

LabX0AudioProcessor::LabX0AudioProcessor()
    : AudioProcessor (makeBuses()),
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
    breakReleasePending.fill (-1);
    sliceScratch.assign (8192, 0);

    applyPreset (0);
    startTimer (500);
}

LabX0AudioProcessor::~LabX0AudioProcessor()
{
    alive->store (false);
    jobs.reset();   // waits for a running job
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
    breakReleasePending.fill (-1);
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
    auxDelayLength = latency;
    for (auto& a : aux)
    {
        a.delayL.assign ((size_t) auxDelayLength + 1, 0.0f);
        a.delayR.assign ((size_t) auxDelayLength + 1, 0.0f);
        a.delayPos = 0;
        a.channel = -1;
    }
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
    for (int k = 1; k < layouts.outputBuses.size(); ++k)
        if (! layouts.outputBuses[k].isDisabled() && layouts.outputBuses[k] != juce::AudioChannelSet::stereo())
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

    // Which of the extra outputs the host gives us this block (any it has not enabled send their lanes to the main one).
    {
        const int busCount = getBusCount (false);
        for (int k = 0; k < numAuxOutputs; ++k)
        {
            auto& a = aux[(size_t) k];
            a.channel = -1;
            if (k + 1 >= busCount || a.delayL.empty())
                continue;
            if (const auto* bus = getBus (false, k + 1); bus != nullptr && bus->isEnabled() && bus->getNumberOfChannels() == 2)
            {
                const int ch = getChannelIndexInProcessBlockBuffer (false, k + 1, 0);
                if (ch >= 0 && ch + 1 < numChannels)
                    a.channel = ch;
            }
        }
    }

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
    // A pattern note switches at the first boundary at or after its own position (half a sample of slack, so a
    // note on the bar line takes that bar); the parameter and the buttons from the block start.
    double queuedFromQ = std::numeric_limits<double>::lowest();
    for (const auto metadata : midi)
    {
        const auto m = metadata.getMessage();
        if (m.isNoteOn() && m.getNoteNumber() >= midiFirstPattern && m.getNoteNumber() < midiFirstPattern + numPatterns)
        {
            queuedPattern = m.getNoteNumber() - midiFirstPattern;
            queuedFromQ = q0 + ((double) metadata.samplePosition - 0.5) * qps;
        }
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
                else if (e.type == 2)
                {
                    const int b = juce::jlimit (0, numBreaks - 1, e.lane);
                    if (const auto* d = blockSamples[(size_t) (slotFirstBreak + b)])
                    {
                        const auto& sc = slicesFor (b, d);
                        const int s = std::max (0, e.slice) % sc.count;
                        triggerBreak (b, std::max (1.0, (double) sc.starts[(size_t) s]), s, e.velocity, 0, false, -1, true);
                    }
                }
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
    const int numEvents = sequencer.collect (localBank, currentPattern, queuedPattern, queuedFromQ, running, q0, qps, numSamples,
                                             swing, events.data(), (int) events.size());

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
        applyPendingReleases();

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
        for (auto& a : aux)
        {
            if (a.channel < 0)
                continue;
            float* outAL = buffer.getWritePointer (a.channel, pos);
            float* outAR = buffer.getWritePointer (a.channel + 1, pos);
            for (int i = 0; i < n; ++i)
            {
                a.delayL[(size_t) a.delayPos] = a.l[(size_t) i];
                a.delayR[(size_t) a.delayPos] = a.r[(size_t) i];
                int readPos = a.delayPos + 1;
                if (readPos > auxDelayLength)
                    readPos = 0;
                outAL[i] = a.delayL[(size_t) readPos];
                outAR[i] = a.delayR[(size_t) readPos];
                a.delayPos = readPos;
            }
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
    for (int lane = 0; lane < numPads + numBreaks; ++lane)
        laneOut[(size_t) lane] = std::clamp ((int) std::lround (v[(size_t) P::out (lane)]), 0, numAuxOutputs);
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
    for (auto& a : aux)
        if (a.channel >= 0)
        {
            std::fill_n (a.l.data(), n, 0.0f);
            std::fill_n (a.r.data(), n, 0.0f);
        }

    // Every voice renders on its own first, so a voice that runs away is stopped alone and named in the log.
    const dsp::VoiceOutput temp { { tempL, tempL, tempL }, { tempR, tempR, tempR } };
    auto addVoice = [&] (int route, int lane)
    {
        const float pk = blockPeak (tempL, tempR, n);
        if (! (pk <= voiceLimit))
        {
            noteFault (stageVoice, pk, true);
            return false;
        }
        float* toL = busL[route];
        float* toR = busR[route];
        if (const int o = laneOut[(size_t) lane]; o > 0 && aux[(size_t) (o - 1)].channel >= 0)
        {
            toL = aux[(size_t) (o - 1)].l.data();
            toR = aux[(size_t) (o - 1)].r.data();
        }
        for (int i = 0; i < n; ++i)
        {
            toL[i] += tempL[i];
            toR[i] += tempR[i];
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
        if (! addVoice (std::clamp (voice.getRoute(), 0, 2), voice.getPad()))
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
            if (! addVoice (std::clamp (voice.getRoute(), 0, 2), numPads + b))
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
   #if LABX0_HARNESS
    if (injectStage == stageOutput && injectAtSample >= samplesRendered - n && injectAtSample < samplesRendered)
        outL[0] = std::numeric_limits<float>::quiet_NaN();
   #endif

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
        fadeTrack[(size_t) i] = switchGain;
    }

    // The per-lane outputs get the same preset fade, guard and safety clip as the main one.
    for (auto& a : aux)
    {
        if (a.channel < 0)
            continue;
        bool bad = runaway;
        for (int i = 0; i < n && ! bad; ++i)
            bad = ! std::isfinite (a.l[(size_t) i]) || ! std::isfinite (a.r[(size_t) i]);
        if (bad)
        {
            std::fill_n (a.l.data(), n, 0.0f);
            std::fill_n (a.r.data(), n, 0.0f);
            fault = fault || ! runaway;
            continue;
        }
        for (int i = 0; i < n; ++i)
        {
            a.l[(size_t) i] = dsp::safetyClip (a.l[(size_t) i]) * fadeTrack[(size_t) i];
            a.r[(size_t) i] = dsp::safetyClip (a.r[(size_t) i]) * fadeTrack[(size_t) i];
        }
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
    if (recordHits != nullptr && e.lane < numPads)   // an offline renderer writing the MIDI file (breaks: triggerBreak)
        recordHits->push_back ({ engineClock, e.lane, -1, e.gateSamples, e.pitch, e.velocity, true });
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
   #if LABX0_HARNESS
    ++padTriggers;
   #endif
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

    // Hand-placed slices win: copied from the editor's list when it changed and is free (never waits).
    auto& copy = customCopy[(size_t) b];
    auto& custom = customSlices[(size_t) b];
    if (const int v = custom.version.load (std::memory_order_acquire); v != copy.version)
    {
        const juce::SpinLock::ScopedTryLockType tl (custom.lock);
        if (tl.isLocked())
        {
            copy.pos = custom.pos;
            copy.count = custom.count;
            copy.version = v;
        }
    }
    if (copy.count > 0)
    {
        if (c.dataId == d->id && c.custom == copy.version)
            return c;
        c.dataId = d->id;
        c.custom = copy.version;
        c.slicing = -1;
        c.count = 0;
        for (int k = 0; k < copy.count; ++k)
            if (copy.pos[(size_t) k] < d->length)
                c.starts[(size_t) c.count++] = copy.pos[(size_t) k];
        if (c.count == 0)
            c.starts[(size_t) c.count++] = 0;
        return c;
    }

    const int slicing = std::clamp (choiceOf (P::brk (b, P::brSlicing)), 0, 3);
    const float sens = rawValue (P::brk (b, P::brSens));
    const double beats = loopBeats (b, d);
    if (c.dataId == d->id && c.custom < 0 && c.slicing == slicing && std::abs (c.sens - sens) < 1.0e-4f && std::abs (c.beats - beats) < 1.0e-6)
        return c;

    c.dataId = d->id;
    c.custom = -1;
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

void LabX0AudioProcessor::triggerBreak (int b, double sourcePos, int slice, float velocity, int pitch, bool midiNote, int note, bool sliceGate) noexcept
{
    const auto* d = blockSamples[(size_t) (slotFirstBreak + b)];
    meters.laneFlash[(size_t) (laneFirstBreak + b)].store (velocity);
    if (d == nullptr)
        return;

    const auto& sc = slicesFor (b, d);
    const int gate = choiceOf (P::brk (b, P::brGate));
    const float decay = rawValue (P::brk (b, P::brDecay));

    if (recordHits != nullptr)
    {
        // The slice note that plays this start back: the slice it starts on, else the nearest one.
        int nearest = 0;
        double distance = std::numeric_limits<double>::max();
        for (int i = 0; i < sc.count; ++i)
        {
            const double gap = std::abs (std::max (1.0, (double) sc.starts[(size_t) i]) - sourcePos);
            if (gap < distance)
            {
                distance = gap;
                nearest = i;
            }
        }
        recordHits->push_back ({ engineClock, laneFirstBreak + b, nearest, 0, pitch, velocity, distance < 0.5 });
    }

    double stopAt = -1.0;
    if (gate == Choice::gateSlice || sliceGate)
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
    ++breakSerial[(size_t) b];
   #if LABX0_HARNESS
    ++breakTriggers;
   #endif
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
        triggerBreak (b, std::max (1.0, (double) sc.starts[(size_t) s]), s, e.velocity, e.pitch, false, -1);
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

void LabX0AudioProcessor::releaseMidiNote (int channel, int note) noexcept
{
    if (note >= midiFirstPad && note < midiFirstPad + numPads)
    {
        const int pad = note - midiFirstPad;
        for (auto& v : padVoices)
            if (v.isSounding() && v.getPad() == pad)
                v.noteOff();
    }
    const int key = midiKey (channel, note);
    for (int b = 0; b < numBreaks; ++b)
        if (breakMidiNote[(size_t) b] == key)
        {
            // The note started at this very sample: this note-off ends the note before it.
            if (breakSerial[(size_t) b] != breakSerialRendered[(size_t) b])
                continue;
            if (choiceOf (P::brk (b, P::brGate)) == Choice::gateLegato)
                breakReleasePending[(size_t) b] = (int64_t) breakSerial[(size_t) b];
            breakMidiNote[(size_t) b] = -1;
        }
}

void LabX0AudioProcessor::releaseAllMidiNotes() noexcept
{
    for (auto& v : padVoices)
        if (v.isSounding())
            v.noteOff();
    for (int b = 0; b < numBreaks; ++b)
        if (breakMidiNote[(size_t) b] >= 0)
        {
            if (choiceOf (P::brk (b, P::brGate)) == Choice::gateLegato)
                breakVoices[(size_t) b][(size_t) breakCurrent[(size_t) b]].release();
            breakMidiNote[(size_t) b] = -1;
        }
}

// Before audio moves on from a sample: a break released there is released, unless it was started again there.
void LabX0AudioProcessor::applyPendingReleases() noexcept
{
    for (int b = 0; b < numBreaks; ++b)
    {
        auto& pending = breakReleasePending[(size_t) b];
        if (pending >= 0 && (uint32_t) pending == breakSerial[(size_t) b])
            breakVoices[(size_t) b][(size_t) breakCurrent[(size_t) b]].release();
        pending = -1;
        breakSerialRendered[(size_t) b] = breakSerial[(size_t) b];
    }
}

void LabX0AudioProcessor::handleMidi (const juce::MidiMessage& m, int) noexcept
{
    ++midiAccumulator;
    if (m.isNoteOn())
    {
        const int note = m.getNoteNumber();
        const int channel = m.getChannel();
        const float vel = m.getFloatVelocity();
        if (note >= midiFirstPad && note < midiFirstPad + numPads)
            triggerPad (note - midiFirstPad, vel, 0, -1);
        else if (note >= midiFirstBreak && note < midiFirstBreak + numBreaks)
            triggerBreak (note - midiFirstBreak, 1.0, 0, vel, 0, true, midiKey (channel, note));
        else if (note >= midiFirstSlice && note < midiFirstSlice + midiSliceCount)
        {
            // Channels 2-5 (FL's note colours 2-5) play the slices of break A-D, any other the SLICE MIDI slot.
            const int b = channel >= midiFirstBreakChannel && channel < midiFirstBreakChannel + numBreaks
                              ? channel - midiFirstBreakChannel
                              : std::clamp (choiceOf (P::sliceMidi), 0, numBreaks - 1);
            triggerBreakSlice (b, note - midiFirstSlice, vel, midiKey (channel, note));
        }
    }
    else if (m.isNoteOff())
        releaseMidiNote (m.getChannel(), m.getNoteNumber());
    else if (m.isAllNotesOff())
        releaseAllMidiNotes();
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
    breakReleasePending.fill (-1);
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
    breakReleasePending.fill (-1);
    transient.reset();
    multiband.reset();
    punch.reset();
    regulator.reset();
    std::fill (dryDelayL.begin(), dryDelayL.end(), 0.0f);
    std::fill (dryDelayR.begin(), dryDelayR.end(), 0.0f);
    clearAuxDelays();
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
    clearAuxDelays();
    dcInL = dcInR = dcOutL = dcOutR = 0.0f;
}

void LabX0AudioProcessor::clearAuxDelays() noexcept
{
    for (auto& a : aux)
    {
        std::fill (a.delayL.begin(), a.delayL.end(), 0.0f);
        std::fill (a.delayR.begin(), a.delayR.end(), 0.0f);
        a.l.fill (0.0f);
        a.r.fill (0.0f);
    }
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
void LabX0AudioProcessor::assignSample (int slot, const SampleRef& ref, double offsetSeconds, double lengthSeconds)
{
    const bool changed = ref != bank.getRef (slot) || std::abs (offsetSeconds - bank.getOffset (slot)) > 1.0e-9
                      || std::abs (lengthSeconds - bank.getLength (slot)) > 1.0e-9;
    bank.assign (slot, ref, offsetSeconds, lengthSeconds);
    if (changed && slot >= slotFirstBreak && slot < previewSlot)
        setCustomSlices (slot - slotFirstBreak, {});
}

void LabX0AudioProcessor::setCustomSlices (int b, const std::vector<int>& positions)
{
    if (! juce::isPositiveAndBelow (b, numBreaks))
        return;
    std::vector<int> p;
    for (const int x : positions)
        if (x >= 0)
            p.push_back (x);
    std::sort (p.begin(), p.end());
    p.erase (std::unique (p.begin(), p.end()), p.end());
    if (! p.empty() && p.front() != 0)
        p.insert (p.begin(), 0);
    if ((int) p.size() > maxSlices)
        p.resize ((size_t) maxSlices);

    auto& cs = customSlices[(size_t) b];
    {
        const juce::SpinLock::ScopedLockType sl (cs.lock);
        cs.count = (int) p.size();
        std::copy (p.begin(), p.end(), cs.pos.begin());
    }
    cs.version.fetch_add (1, std::memory_order_release);
}

std::vector<int> LabX0AudioProcessor::getCustomSlices (int b) const
{
    if (! juce::isPositiveAndBelow (b, numBreaks))
        return {};
    const auto& cs = customSlices[(size_t) b];
    const juce::SpinLock::ScopedLockType sl (cs.lock);
    return std::vector<int> (cs.pos.begin(), cs.pos.begin() + cs.count);
}

int LabX0AudioProcessor::getCustomSlicesVersion (int b) const noexcept
{
    return juce::isPositiveAndBelow (b, numBreaks) ? customSlices[(size_t) b].version.load() : 0;
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
        uiEvents[(size_t) fifoScope.startIndex1] = { 1, 0, 0.0f, -1 };
}

void LabX0AudioProcessor::audition (int lane, float velocity)
{
    const auto fifoScope = uiFifo.write (1);
    if (fifoScope.blockSize1 > 0)
        uiEvents[(size_t) fifoScope.startIndex1] = { 0, lane, velocity, -1 };
}

void LabX0AudioProcessor::auditionSlice (int breakSlot, int slice)
{
    const auto fifoScope = uiFifo.write (1);
    if (fifoScope.blockSize1 > 0)
        uiEvents[(size_t) fifoScope.startIndex1] = { 2, breakSlot, 0.85f, slice };
}

//==============================================================================
LabX0AudioProcessor::ExplodeSource LabX0AudioProcessor::explodeSourceFor (int b) const
{
    ExplodeSource source;
    const int slot = slotFirstBreak + b;
    auto ov = bank.getOverview (slot);
    if (! juce::isPositiveAndBelow (b, numBreaks) || ov == nullptr || bank.getStatus (slot) != SampleBank::Status::ready)
    {
        source.error = "LOAD A BREAK FIRST";
        return source;
    }
    source.ref = bank.getRef (slot);
    source.offset = bank.getOffset (slot);
    source.length = bank.getLength (slot);
    source.beats = loopBeatsFor (b, *ov);
    source.slices.resize ((size_t) maxSlices);
    source.slices.resize ((size_t) computeSlices (b, *ov, source.slices.data(), maxSlices));
    return source;
}

ExplodePlan LabX0AudioProcessor::analyseExplode (const ExplodeSource& source, int maxPads, bool everySlice, double& sampleRate)
{
    // The break exactly as its slot holds it: same file, window, region and normalising, so the slice positions fit.
    // (Tempo analysis is off, so the host tempo does not matter here.)
    auto result = bank.decode (getLibrary().resolve (source.ref), source.offset, false, 120.0, source.length, nullptr);
    if (result.data == nullptr)
    {
        ExplodePlan failed;
        failed.message = result.message.isNotEmpty() ? result.message : juce::String ("COULD NOT READ THE BREAK");
        return failed;
    }
    const auto& d = *result.data;
    std::vector<float> mono ((size_t) d.length);
    for (int i = 0; i < d.length; ++i)
        mono[(size_t) i] = 0.5f * (d.l()[i] + d.r()[i]);
    sampleRate = d.sampleRate;
    return planExplode (mono.data(), d.length, d.sampleRate, source.slices, source.beats, maxPads, everySlice ? 0.0f : mergeDistanceDb);
}

int LabX0AudioProcessor::applyExplode (int b, int firstPad, const ExplodePlan& plan, const ExplodeSource& source, double sampleRate)
{
    // The first empty pattern after the current one (else the current one, rewritten).
    const int current = getEditPattern();
    int target = -1;
    patterns.read ([&target, current] (const PatternBank& pb)
    {
        for (int k = 1; k <= numPatterns && target < 0; ++k)
            if (const int i = (current + k) % numPatterns; pb.patterns[(size_t) i].isEmpty())
                target = i;
    });
    if (target < 0)
        target = current;

    auto set = [this] (int index, float value)
    {
        if (auto* p = apvts.getParameter (paramSpecs()[(size_t) index].id))
            p->setValueNotifyingHost (p->convertTo0to1 (value));
    };
    const int route = choiceOf (P::brk (b, P::brRoute));
    const int pads = std::min ((int) plan.components.size(), numPads - firstPad);
    for (int k = 0; k < pads; ++k)
    {
        const auto& c = plan.components[(size_t) k];
        const auto& h = plan.hits[(size_t) c.representative];
        const int pad = firstPad + k;
        assignSample (pad, source.ref, source.offset + (double) h.start / sampleRate, (double) h.length / sampleRate);
        // A plain pad that plays the whole region at the level the hit had in the break; velocity does the rest.
        set (P::pad (pad, P::padLevel), std::clamp (c.levelDb, -48.0f, 6.0f));
        set (P::pad (pad, P::padTune), 0.0f);
        set (P::pad (pad, P::padStart), 0.0f);
        set (P::pad (pad, P::padAttack), 0.0f);
        set (P::pad (pad, P::padHold), 0.0f);
        set (P::pad (pad, P::padDecay), 10.0f);
        set (P::pad (pad, P::padTone), 0.0f);
        set (P::pad (pad, P::padReso), 0.0f);
        set (P::pad (pad, P::padDrive), 0.0f);
        set (P::pad (pad, P::padVel), 1.0f);
        set (P::pad (pad, P::padReverse), 0.0f);
        set (P::pad (pad, P::padMode), (float) Choice::modeMono);
        set (P::pad (pad, P::padChoke), 0.0f);
        set (P::pad (pad, P::padRoute), (float) route);
        set (P::out (pad), 0.0f);
    }

    patterns.edit (target, [&plan, pads, firstPad] (Pattern& p)
    {
        p.clear();
        p.length = plan.pattern.length;
        p.resolution = plan.pattern.resolution;
        for (int k = 0; k < pads; ++k)
            p.lanes[(size_t) (firstPad + k)] = plan.pattern.lanes[(size_t) k];
    });
    setEditPattern (target);
    return target;
}

ExplodePlan LabX0AudioProcessor::explodeBreakNow (int b, int firstPad, bool everySlice, int* patternIndex)
{
    firstPad = juce::jlimit (0, numPads - 1, firstPad);
    const auto source = explodeSourceFor (b);
    if (source.error.isNotEmpty())
    {
        ExplodePlan failed;
        failed.message = source.error;
        return failed;
    }
    double sampleRate = 48000.0;
    auto plan = analyseExplode (source, numPads - firstPad, everySlice, sampleRate);
    if (plan.ok)
    {
        const int target = applyExplode (b, firstPad, plan, source, sampleRate);
        plan.message << ", PATTERN " << juce::String (target + 1).paddedLeft ('0', 2);
        if (patternIndex != nullptr)
            *patternIndex = target;
    }
    return plan;
}

void LabX0AudioProcessor::explodeBreak (int b, int firstPad, bool everySlice, std::function<void (const juce::String&)> done)
{
    firstPad = juce::jlimit (0, numPads - 1, firstPad);
    auto source = std::make_shared<ExplodeSource> (explodeSourceFor (b));
    if (source->error.isNotEmpty())
    {
        if (done)
            done (source->error);
        return;
    }
    auto token = alive;
    jobs->addJob ([this, token, source, b, firstPad, everySlice, done]
    {
        double sampleRate = 48000.0;
        auto plan = std::make_shared<ExplodePlan> (analyseExplode (*source, numPads - firstPad, everySlice, sampleRate));
        juce::MessageManager::callAsync ([this, token, source, plan, b, firstPad, sampleRate, done]
        {
            if (! token->load())
                return;
            auto text = plan->message;
            if (plan->ok)
                text << ", PATTERN " << juce::String (applyExplode (b, firstPad, *plan, *source, sampleRate) + 1).paddedLeft ('0', 2);
            if (done)
                done (text);
        });
    });
}

//==============================================================================
namespace
{
    // A transport that plays from beat 0 at a fixed tempo, and stops for the tails: the renderer's host.
    struct RenderPlayHead final : public juce::AudioPlayHead
    {
        double bpm = 120.0, sampleRate = 48000.0;
        juce::int64 position = 0;
        bool playing = true;

        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            info.setBpm (bpm);
            info.setTimeInSamples (position);
            info.setPpqPosition ((double) position / sampleRate * bpm / 60.0);
            info.setIsPlaying (playing);
            return info;
        }
    };

    bool writeWav24 (const juce::File& file, const juce::AudioBuffer<float>& buffer, double sampleRate)
    {
        file.deleteFile();
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
        if (stream == nullptr)
            return false;
        juce::WavAudioFormat wav;
        auto options = juce::AudioFormatWriterOptions().withSampleRate (sampleRate).withNumChannels (buffer.getNumChannels()).withBitsPerSample (24);
        if (auto writer = wav.createWriterFor (stream, options))
            return writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
        return false;
    }

    // Everything after the pattern's own length that is quieter than -90 dBFS goes.
    int trimmedLength (const juce::AudioBuffer<float>& buffer, int atLeast)
    {
        int last = atLeast;
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            const float* d = buffer.getReadPointer (ch);
            for (int i = buffer.getNumSamples() - 1; i >= atLeast; --i)
                if (std::abs (d[i]) > 3.2e-5f)
                {
                    last = std::max (last, i + 1);
                    break;
                }
        }
        return std::min (buffer.getNumSamples(), last);
    }
}

LabX0AudioProcessor::RenderRequest LabX0AudioProcessor::resolveRequest (const RenderRequest& request) const
{
    auto r = request;
    r.pattern = juce::jlimit (0, numPatterns - 1, r.pattern);
    r.passes = juce::jlimit (1, 64, r.passes);
    if (r.bpm <= 1.0)
        r.bpm = meters.bpm.load() > 1.0f ? (double) meters.bpm.load() : 120.0;   // the tempo the host last reported
    if (r.sampleRate <= 0.0)
        r.sampleRate = currentSampleRate > 0.0 ? currentSampleRate : 48000.0;
    if (r.folder == juce::File())
        r.folder = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("LAB X-0").getChildFile ("Renders")
                       .getChildFile (juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H%M%S") + " Pattern "
                                      + juce::String (r.pattern + 1).paddedLeft ('0', 2));
    return r;
}

juce::AudioBuffer<float> LabX0AudioProcessor::renderPass (int pattern, double sampleRate, double bpm, int body, int length,
                                                          std::atomic<float>& progress, float from, float to)
{
    RenderPlayHead head;
    head.bpm = bpm;
    head.sampleRate = sampleRate;
    setPlayHead (&head);
    constexpr int block = 512;
    if (auto* p = apvts.getParameter ("pattern"))
        p->setValueNotifyingHost (p->convertTo0to1 ((float) pattern));
    if (auto* p = apvts.getParameter ("run"))
        p->setValueNotifyingHost (p->convertTo0to1 ((float) Choice::runHost));
    setPlayConfigDetails (0, 2, sampleRate, block);
    prepareToPlay (sampleRate, block);
    programChangePending.store (false);
    switchState = SwitchState::idle;
    switchGain = 1.0f;
    sequencer.reset();

    const int latency = getLatencySamples();
    juce::AudioBuffer<float> out (2, length);
    out.clear();
    juce::AudioBuffer<float> chunk (2, block);
    juce::MidiBuffer midi;
    // The pattern plays for `body` samples; then the transport stops (a block ends exactly there, so no hit of the
    // next pass slips in) and voices and effects ring out.
    int written = -latency;
    for (juce::int64 pos = 0; written < length;)
    {
        int step = block;
        if (pos < body && pos + block > body)
            step = (int) (body - pos);
        head.position = pos;
        head.playing = pos < body;
        juce::AudioBuffer<float> view (chunk.getArrayOfWritePointers(), 2, step);
        processBlock (view, midi);
        const int skip = std::max (0, -written);
        const int n = std::min (step - skip, length - std::max (0, written));
        if (n > 0)
            for (int ch = 0; ch < 2; ++ch)
                out.copyFrom (ch, std::max (0, written), view, ch, skip, n);
        pos += step;
        written += step;
        progress.store (from + (to - from) * (float) std::clamp ((double) written / (double) length, 0.0, 1.0));
    }
    setPlayHead (nullptr);
    releaseResources();
    return out;
}

LabX0AudioProcessor::RenderStatus LabX0AudioProcessor::renderAll (const RenderRequest& request, double sampleRate, double bpm,
                                                                  std::atomic<float>& progress)
{
    RenderStatus status;
    status.folder = request.folder;
    if (! bank.waitUntilSettled (120000))
    {
        status.text = "SAMPLES DID NOT LOAD";
        return status;
    }

    Pattern pattern;
    patterns.read ([&pattern, &request] (const PatternBank& pb) { pattern = pb.patterns[(size_t) request.pattern]; });
    const double patternQuarters = pattern.stepQuarters() * pattern.length;
    // Rounded down: the transport stops just before the next pass's downbeat, never a fraction of a sample after it.
    const int body = std::max (1, (int) std::floor (request.passes * patternQuarters * 60.0 / bpm * sampleRate));
    const int length = body + (int) (4.0 * sampleRate);   // room for the tails, trimmed after

    // Jobs: the mix, then stems (lanes with steps and a sample), or one lane.
    std::vector<int> solos { -1 };
    if (request.what == 1)
    {
        for (int lane = 0; lane < laneTime; ++lane)
            if (pattern.laneHasContent (lane) && ! pattern.lanes[(size_t) lane].mute && ! bank.getRef (lane).isEmpty())
                solos.push_back (lane);
    }
    else if (request.what == 2)
        solos = { juce::jlimit (0, laneTime - 1, request.lane) };

    if (! request.folder.createDirectory())
    {
        status.text = "COULD NOT CREATE " + request.folder.getFullPathName();
        return status;
    }
    const auto prefix = "Pattern " + juce::String (request.pattern + 1).paddedLeft ('0', 2);
    std::vector<RecordedHit> hits;
    int common = -1;   // every stem as long as the mix, so they line up in the arrangement
    for (size_t j = 0; j < solos.size(); ++j)
    {
        const int solo = solos[j];
        patterns.edit (request.pattern, [&pattern, solo] (Pattern& p)
        {
            for (int lane = 0; lane < laneTime; ++lane)
                p.lanes[(size_t) lane].mute = (uint8_t) (solo >= 0 ? (lane != solo ? 1 : 0) : pattern.lanes[(size_t) lane].mute);
        });
        recordHits = solo < 0 ? &hits : nullptr;
        hits.clear();
        auto audio = renderPass (request.pattern, sampleRate, bpm, body, length, progress, (float) j / (float) solos.size(),
                                 (float) (j + 1) / (float) solos.size());
        recordHits = nullptr;
        const int keep = common > 0 ? std::min (common, audio.getNumSamples()) : trimmedLength (audio, body);
        if (solo < 0)
            common = keep;
        juce::AudioBuffer<float> trimmed (2, keep);
        for (int ch = 0; ch < 2; ++ch)
            trimmed.copyFrom (ch, 0, audio, ch, 0, keep);

        juce::String name = prefix + " mix";
        if (solo >= 0)
        {
            const auto sample = bank.getRef (solo).displayName();
            name = prefix + " " + (solo < numPads ? "pad " + juce::String (solo + 1).paddedLeft ('0', 2) : "break " + breakLetter (solo - numPads))
                 + (sample.isNotEmpty() ? " " + sample : juce::String());
        }
        const auto file = request.folder.getChildFile (juce::File::createLegalFileName (name) + ".wav");
        if (! writeWav24 (file, trimmed, sampleRate))
        {
            status.text = "COULD NOT WRITE " + file.getFileName();
            return status;
        }
        status.files.add (file);

        if (solo < 0)
        {
            // The hits as the engine played them (swing, nudge, ratchets and probability included), written so the file
            // plays back into LAB X-0 as rendered: pads on channel 1 (notes 36-51) for their gate, breaks A-D on
            // channels 2-5 (FL's note colours 2-5) as slice notes (60-91) held until the break's next start or the end
            // of the last pass, as the step sequencer holds them. A note has no room for step pitch, and a break start
            // between slices (a natural step with FIT on REPITCH or STRETCH) takes the nearest slice: the status line
            // counts both.
            const double ticksPerSample = bpm / 60.0 / sampleRate * 960.0;
            const auto tickOf = [ticksPerSample] (juce::int64 sample) { return (double) std::llround ((double) sample * ticksPerSample); };
            juce::MidiMessageSequence sequence;
            sequence.addEvent (juce::MidiMessage::tempoMetaEvent ((int) std::lround (60000000.0 / bpm)), 0.0);
            sequence.addEvent (juce::MidiMessage::timeSignatureMetaEvent (4, 4), 0.0);
            int pitched = 0, between = 0, unreachable = 0;
            for (size_t i = 0; i < hits.size(); ++i)
            {
                const auto& h = hits[i];
                const double on = tickOf (h.sample);
                double next = -1.0;   // the lane's next hit: a retrigger cuts this one anyway
                for (size_t k = i + 1; k < hits.size(); ++k)
                    if (hits[k].lane == h.lane)
                    {
                        next = tickOf (hits[k].sample);
                        break;
                    }
                if (next >= 0.0 && next <= on)
                    continue;   // replaced within the same tick
                const bool isPad = h.lane < numPads;
                if (! isPad && h.slice >= midiSliceCount)
                {
                    ++unreachable;
                    continue;
                }
                pitched += h.pitch != 0 ? 1 : 0;
                between += h.exact ? 0 : 1;
                const int channel = isPad ? 1 : midiFirstBreakChannel + (h.lane - laneFirstBreak);
                const int note = isPad ? midiFirstPad + h.lane : midiFirstSlice + h.slice;
                double off = std::max (on + 1.0, isPad ? tickOf (h.sample + std::max (1, h.gateSamples)) : tickOf (body));
                if (next >= 0.0)
                    off = isPad ? std::min (off, next) : next;
                sequence.addEvent (juce::MidiMessage::noteOn (channel, note, (juce::uint8) juce::jlimit (1, 127, (int) std::lround (h.velocity * 127.0f))), on);
                sequence.addEvent (juce::MidiMessage::noteOff (channel, note), off);
            }
            sequence.updateMatchedPairs();
            juce::StringArray notes;
            if (between > 0)
                notes.add (juce::String (between) + (between == 1 ? " BREAK HIT STARTS" : " BREAK HITS START") + " BETWEEN SLICES (MOVED TO THE NEAREST)");
            if (unreachable > 0)
                notes.add (juce::String (unreachable) + (unreachable == 1 ? " HIT ON A SLICE" : " HITS ON SLICES") + " PAST 32 LEFT OUT");
            if (pitched > 0)
                notes.add (juce::String (pitched) + (pitched == 1 ? " HIT WITH STEP PITCH PLAYS" : " HITS WITH STEP PITCH PLAY") + " UNPITCHED");
            status.midi = notes.isEmpty() ? juce::String ("MIDI: PADS ON CHANNEL 1, BREAKS A-D ON CHANNELS 2-5 (FL NOTE COLOURS 1-5)")
                                          : "MIDI APPROXIMATE: " + notes.joinIntoString (", ");
            juce::MidiFile midiFile;
            midiFile.setTicksPerQuarterNote (960);
            midiFile.addTrack (sequence);
            const auto midiPath = request.folder.getChildFile (juce::File::createLegalFileName (prefix) + ".mid");
            midiPath.deleteFile();
            juce::FileOutputStream stream (midiPath);
            if (stream.openedOk() && midiFile.writeTo (stream))
            {
                stream.flush();
                status.files.add (midiPath);
            }
        }
    }
    progress.store (1.0f);
    status.text = "RENDERED " + juce::String (status.files.size()) + " FILES AT " + juce::String (bpm, 1) + " BPM";
    return status;
}

LabX0AudioProcessor::RenderStatus LabX0AudioProcessor::renderNow (const RenderRequest& request)
{
    const auto r = resolveRequest (request);
    juce::MemoryBlock state;
    getStateInformation (state);
    auto renderer = std::make_unique<LabX0AudioProcessor>();
    renderer->setStateInformation (state.getData(), (int) state.getSize());
    std::atomic<float> progress { 0.0f };
    return renderer->renderAll (r, r.sampleRate, r.bpm, progress);
}

bool LabX0AudioProcessor::startRender (const RenderRequest& request)
{
    if (renderBusy.exchange (true))
        return false;
    const auto r = resolveRequest (request);
    juce::MemoryBlock state;
    getStateInformation (state);
    // Made here, on the message thread, and destroyed here too; only the rendering runs on the job thread.
    auto renderer = std::make_shared<std::unique_ptr<LabX0AudioProcessor>> (std::make_unique<LabX0AudioProcessor>());
    (*renderer)->setStateInformation (state.getData(), (int) state.getSize());
    {
        const juce::ScopedLock sl (renderLock);
        renderStatus = {};
        renderStatus.running = true;
        renderStatus.folder = r.folder;
        renderStatus.text = "RENDERING PATTERN " + juce::String (r.pattern + 1).paddedLeft ('0', 2) + "...";
    }
    renderProgress.store (0.0f);
    ++renderVersion;
    auto token = alive;
    jobs->addJob ([this, token, renderer, r]
    {
        auto result = std::make_shared<RenderStatus> ((*renderer)->renderAll (r, r.sampleRate, r.bpm, renderProgress));
        juce::MessageManager::callAsync ([this, token, renderer, result]
        {
            renderer->reset();
            if (! token->load())
                return;
            {
                const juce::ScopedLock sl (renderLock);
                renderStatus = *result;
                renderStatus.running = false;
            }
            renderBusy.store (false);
            ++renderVersion;
        });
    });
    return true;
}

LabX0AudioProcessor::RenderStatus LabX0AudioProcessor::getRenderStatus() const
{
    const juce::ScopedLock sl (renderLock);
    auto s = renderStatus;
    s.progress = renderProgress.load();
    return s;
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
    if (const auto custom = getCustomSlices (b); ! custom.empty())
    {
        int n = 0;
        for (const int p : custom)
            if (p < ov.length && n < maxOut)
                out[n++] = p;
        if (n == 0 && maxOut > 0)
            out[n++] = 0;
        return n;
    }

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
    for (int b = 0; b < numBreaks; ++b)
        setCustomSlices (b, {});

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
        if (bank.getLength (slot) > 0.0)
            state.setProperty ("length" + juce::String (slot), bank.getLength (slot), nullptr);
    }
    for (int b = 0; b < numBreaks; ++b)
        if (const auto custom = getCustomSlices (b); ! custom.empty())
        {
            juce::StringArray list;
            for (const int p : custom)
                list.add (juce::String (p));
            state.setProperty ("slices" + juce::String (b), list.joinIntoString (","), nullptr);
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
    std::array<double, previewSlot> offsets {}, lengths {};
    for (int slot = 0; slot < previewSlot; ++slot)
    {
        refs[(size_t) slot] = SampleRef::fromString (tree.getProperty ("slot" + juce::String (slot)).toString());
        offsets[(size_t) slot] = (double) tree.getProperty ("offset" + juce::String (slot), 0.0);
        lengths[(size_t) slot] = (double) tree.getProperty ("length" + juce::String (slot), 0.0);
    }
    std::array<std::vector<int>, numBreaks> slices;
    for (int b = 0; b < numBreaks; ++b)
        for (const auto& token : juce::StringArray::fromTokens (tree.getProperty ("slices" + juce::String (b)).toString(), ",", ""))
            if (token.trim().isNotEmpty())
                slices[(size_t) b].push_back (token.getIntValue());

    juce::MemoryBlock blob;
    const bool havePatterns = blob.fromBase64Encoding (tree.getProperty ("patterns").toString());
    uiSelectedLane.store (juce::jlimit (0, numLanes - 1, (int) tree.getProperty ("uiLane", 0)));
    uiPage.store (juce::jlimit (0, 1, (int) tree.getProperty ("uiPage", 0)));
    uiStripMode.store (juce::jlimit (0, 5, (int) tree.getProperty ("uiStrip", 0)));

    apvts.replaceState (tree);

    for (int slot = 0; slot < previewSlot; ++slot)
        if (refs[(size_t) slot] != bank.getRef (slot) || offsets[(size_t) slot] != bank.getOffset (slot)
            || lengths[(size_t) slot] != bank.getLength (slot))
            bank.assign (slot, refs[(size_t) slot], offsets[(size_t) slot], lengths[(size_t) slot]);
    for (int b = 0; b < numBreaks; ++b)
        setCustomSlices (b, slices[(size_t) b]);

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
