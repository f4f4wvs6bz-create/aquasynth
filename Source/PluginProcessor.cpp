#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace us;

AquaSynthProcessor::AquaSynthProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
    for (auto* prm : getParameters())
        if (auto* wp = dynamic_cast<juce::RangedAudioParameter*> (prm))
            params[wp->getParameterID()] = apvts.getRawParameterValue (wp->getParameterID());
    if (! apvts.state.hasProperty ("theme")) apvts.state.setProperty ("theme", 0, nullptr);
    bank.build();
    uint32_t seed = 12345;
    for (auto& v : voices) { v.rng.s = (seed = seed * 747796405u + 2891336453u); }
}

bool AquaSynthProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void AquaSynthProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sr = sampleRate;
    mods.assign ((size_t) std::max (samplesPerBlock, 16) + 16, ModFrame {});
    bufL.assign (mods.size(), 0.0f);
    bufR.assign (mods.size(), 0.0f);
    chorus.prepare ((float) sr);
    flanger.prepare ((float) sr);
    delay.prepare ((float) sr);
    juce::dsp::ProcessSpec spec { sr, (juce::uint32) samplesPerBlock, 2 };
    reverb.prepare (spec);
    reverb.reset();
    juce::dsp::ProcessSpec mono { sr, (juce::uint32) samplesPerBlock, 1 };
    for (int b = 0; b < 3; ++b) { eqL[b].prepare (mono); eqR[b].prepare (mono); eqCache[b] = 99; }
    for (auto& v : voices) { v.active = false; v.amp.stage = Env::idle; v.fenv.stage = Env::idle; v.amp.v = v.fenv.v = 0; }
}

// ======================================================================= samples
void AquaSynthProcessor::setOscSample (int osc, std::shared_ptr<SampleData> data)
{
    std::atomic_store (&samples[(size_t) osc], data);
    storeSampleInState (osc, data.get());
}

void AquaSynthProcessor::storeSampleInState (int osc, const SampleData* data)
{
    auto list = apvts.state.getOrCreateChildWithName ("SAMPLES", nullptr);
    for (int i = list.getNumChildren(); --i >= 0;)
        if ((int) list.getChild (i).getProperty ("osc") == osc) list.removeChild (i, nullptr);
    if (data == nullptr || data->buffer.getNumSamples() == 0) return;

    juce::MemoryBlock block;
    {
        juce::FlacAudioFormat flac;
        auto* os = new juce::MemoryOutputStream (block, false);
        std::unique_ptr<juce::AudioFormatWriter> w (flac.createWriterFor (os, data->sampleRate, (unsigned int) data->buffer.getNumChannels(), 24, {}, 0));
        if (w == nullptr) { delete os; return; }
        w->writeFromAudioSampleBuffer (data->buffer, 0, data->buffer.getNumSamples());
    }
    juce::ValueTree s ("S");
    s.setProperty ("osc", osc, nullptr);
    s.setProperty ("name", data->name, nullptr);
    s.setProperty ("flac", block.toBase64Encoding(), nullptr);
    list.appendChild (s, nullptr);
}

void AquaSynthProcessor::restoreSamplesFromState()
{
    for (int o = 0; o < numOscs; ++o) std::atomic_store (&samples[(size_t) o], std::shared_ptr<SampleData>());
    auto list = apvts.state.getChildWithName ("SAMPLES");
    for (int i = 0; i < list.getNumChildren(); ++i)
    {
        auto s = list.getChild (i);
        const int osc = (int) s.getProperty ("osc");
        if (osc < 0 || osc >= numOscs) continue;
        juce::MemoryBlock block;
        if (! block.fromBase64Encoding (s.getProperty ("flac").toString())) continue;
        juce::FlacAudioFormat flac;
        std::unique_ptr<juce::AudioFormatReader> r (flac.createReaderFor (new juce::MemoryInputStream (block, false), true));
        if (r == nullptr) continue;
        auto d = std::make_shared<SampleData>();
        d->sampleRate = r->sampleRate;
        d->name = s.getProperty ("name").toString();
        d->buffer.setSize ((int) r->numChannels, (int) r->lengthInSamples);
        r->read (&d->buffer, 0, (int) r->lengthInSamples, 0, true, true);
        std::atomic_store (&samples[(size_t) osc], std::shared_ptr<SampleData> (d));
    }
}

// ======================================================================= presets
juce::File AquaSynthProcessor::presetFolder()
{
    auto f = juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("AquaSynth").getChildFile ("Presets");
    f.createDirectory();
    return f;
}

void AquaSynthProcessor::resetToDefaults()
{
    for (auto* prm : getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm))
        {
            rp->beginChangeGesture(); rp->setValueNotifyingHost (rp->getDefaultValue()); rp->endChangeGesture();
        }
    for (int o = 0; o < numOscs; ++o) setOscSample (o, nullptr);
}

void AquaSynthProcessor::applyValues (const std::map<juce::String, float>& plainValues)
{
    for (auto& kv : plainValues)
        if (auto* rp = apvts.getParameter (kv.first))
        {
            rp->beginChangeGesture(); rp->setValueNotifyingHost (rp->convertTo0to1 (kv.second)); rp->endChangeGesture();
        }
}

bool AquaSynthProcessor::savePreset (const juce::File& file, const juce::String& name)
{
    auto state = apvts.copyState();
    state.setProperty ("presetName", name, nullptr);
    state.removeProperty ("theme", nullptr);
    if (auto xml = state.createXml())
    {
        if (xml->writeTo (file)) { currentPresetName = name; return true; }
    }
    return false;
}

bool AquaSynthProcessor::loadPreset (const juce::File& file)
{
    auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType())) return false;
    const auto theme = apvts.state.getProperty ("theme", 0);
    resetToDefaults();
    auto tree = juce::ValueTree::fromXml (*xml);
    apvts.replaceState (tree);
    apvts.state.setProperty ("theme", theme, nullptr);
    restoreSamplesFromState();
    currentPresetName = tree.getProperty ("presetName", file.getFileNameWithoutExtension()).toString();
    return true;
}

juce::StringArray AquaSynthProcessor::starterNames()
{
    return { "muffled keys", "breathy lead", "sine chord", "wide pad", "pluck arp" };
}

void AquaSynthProcessor::loadStarter (int index)
{
    resetToDefaults();
    std::map<juce::String, float> v;
    switch (index)
    {
        case 0: v = { { "oscCount", 3 }, { "o1Type", tTri }, { "o1Lvl", 0.8f }, { "o2Type", tSaw }, { "o2Lvl", 0.45f }, { "o3Type", tWave }, { "o3Lvl", 0.3f },
                      { "cutoff", 700 }, { "reso", 0.15f }, { "fAmt", 0.35f }, { "drift", 0.3f }, { "width", 0.25f }, { "digital", 0.2f }, { "choMix", 0.3f }, { "revMix", 0.2f } }; break;
        case 1: v = { { "oscCount", 3 }, { "o1Type", tSaw }, { "o1Lvl", 0.7f }, { "o2Type", tPulse }, { "o2Lvl", 0.5f }, { "o3Type", tNoise }, { "o3Lvl", 0.12f }, { "o3Pos", 0.8f },
                      { "cutoff", 1500 }, { "aA", 0.12f }, { "drift", 0.7f }, { "width", 0.85f }, { "choMix", 0.5f }, { "revMix", 0.35f }, { "l1Depth", 0.25f } }; break;
        case 2: v = { { "oscCount", 3 }, { "o1Type", tSine }, { "o1Lvl", 0.8f }, { "o2Type", tSine }, { "o2Lvl", 0.3f }, { "o2Oct", 1 }, { "o3Type", tFormant }, { "o3Lvl", 0.15f }, { "o3Pos", 0.3f },
                      { "cutoff", 3000 }, { "drift", 0.15f }, { "revMix", 0.35f } }; break;
        case 3: v = { { "oscCount", 2 }, { "o1Type", tSaw }, { "o1Uni", 5 }, { "o1Det", 0.35f }, { "o1Lvl", 0.7f }, { "o2Type", tSaw }, { "o2Uni", 3 }, { "o2Det", 0.2f }, { "o2Oct", -1 }, { "o2Lvl", 0.4f },
                      { "aA", 0.3f }, { "aR", 1.5f }, { "cutoff", 900 }, { "fAmt", 0.2f }, { "width", 0.95f }, { "revMix", 0.5f }, { "revSize", 0.8f }, { "choMix", 0.45f } }; break;
        default: v = { { "oscCount", 3 }, { "o1Type", tTri }, { "o1Lvl", 0.8f }, { "o2Type", tPulse }, { "o2Lvl", 0.4f }, { "o3Type", tFm }, { "o3Lvl", 0.3f },
                      { "aD", 0.25f }, { "aS", 0.05f }, { "aR", 0.35f }, { "fD", 0.15f }, { "fS", 0.0f }, { "fAmt", 0.7f }, { "cutoff", 600 },
                      { "arpMode", 3 }, { "arpOct", 2 }, { "dlyOn", 1 }, { "dlyMix", 0.25f }, { "digital", 0.35f }, { "revMix", 0.3f } }; break;
    }
    applyValues (v);
    currentPresetName = starterNames()[index];
}

// ======================================================================= voices
Voice* AquaSynthProcessor::findVoice()
{
    Voice* best = nullptr;
    for (auto& v : voices) if (! v.active) return &v;
    for (auto& v : voices) if (v.amp.stage == Env::release && (best == nullptr || v.age < best->age)) best = &v;
    if (best) return best;
    for (auto& v : voices) if (best == nullptr || v.age < best->age) best = &v;
    return best;
}

void AquaSynthProcessor::triggerVoices (int note, float vel)
{
    const int mode = (int) p ("voice");
    const float width = p ("width"), drift = p ("drift");
    if (mode == 1)
    {
        auto& v = voices[0];
        const bool legato = v.active && v.amp.stage != Env::release && v.amp.stage != Env::idle;
        v.start (note, vel, 0.0f, 0.0f, ! legato, ++stamp, drift, vs);
        return;
    }
    if (mode == 2)
    {
        const float spread = 6.0f + width * 20.0f;
        for (int k = -1; k <= 1; ++k)
            if (auto* v = findVoice())
                v->start (note, vel * 0.7f, (float) k * spread, (float) k, true, ++stamp, drift, vs);
        return;
    }
    if (auto* v = findVoice())
        v->start (note, vel, 0.0f, rng.bip(), true, ++stamp, drift, vs);
}

void AquaSynthProcessor::releaseVoices (int note)
{
    for (auto& v : voices)
        if (v.active && v.note == note && v.amp.stage != Env::release) v.stop();
}

void AquaSynthProcessor::noteOn (int note, float vel)
{
    if ((int) p ("arpMode") > 0)
    {
        held.erase (std::remove_if (held.begin(), held.end(), [note] (auto& h) { return h.first == note; }), held.end());
        held.push_back ({ note, vel });
        return;
    }
    if ((int) p ("voice") == 1)
    {
        monoStack.erase (std::remove (monoStack.begin(), monoStack.end(), note), monoStack.end());
        monoStack.push_back (note);
    }
    triggerVoices (note, vel);
}

void AquaSynthProcessor::noteOff (int note)
{
    held.erase (std::remove_if (held.begin(), held.end(), [note] (auto& h) { return h.first == note; }), held.end());
    if ((int) p ("arpMode") > 0) return;
    if ((int) p ("voice") == 1)
    {
        const bool wasTop = ! monoStack.empty() && monoStack.back() == note;
        monoStack.erase (std::remove (monoStack.begin(), monoStack.end(), note), monoStack.end());
        if (wasTop && ! monoStack.empty()) { triggerVoices (monoStack.back(), voices[0].vel); return; }
        if (monoStack.empty()) voices[0].stop();
        return;
    }
    releaseVoices (note);
}

void AquaSynthProcessor::allNotesOff()
{
    held.clear(); monoStack.clear();
    for (auto& v : voices) if (v.active) v.stop();
    arpCurrent = -1;
}

void AquaSynthProcessor::handleArp (int chunk, double ppqAtChunk, bool usePpq, double bpm)
{
    const int mode = (int) p ("arpMode");
    if (mode == 0)
    {
        if (arpWasOn) { if (arpCurrent >= 0) releaseVoices (arpCurrent); arpCurrent = -1; held.clear(); arpWasOn = false; }
        return;
    }
    arpWasOn = true;
    const double stepSamples = sr * 60.0 / bpm / 4.0;

    if (arpCurrent >= 0)
    {
        arpGateLeft -= chunk;
        if (arpGateLeft <= 0) { releaseVoices (arpCurrent); arpCurrent = -1; }
    }

    bool fire = false;
    if (usePpq)
    {
        const long long step = (long long) std::floor (ppqAtChunk * 4.0);
        if (step != lastArpStep) { lastArpStep = step; fire = true; }
    }
    else
    {
        arpClock -= chunk;
        if (arpClock <= 0) { arpClock += stepSamples; fire = true; }
    }
    if (! fire) return;
    if (held.empty()) { arpIndex = -1; return; }

    std::vector<std::pair<int, float>> sorted = held;
    std::sort (sorted.begin(), sorted.end());
    std::vector<std::pair<int, float>> pattern;
    const int octs = juce::jlimit (1, 3, (int) p ("arpOct"));
    for (int o = 0; o < octs; ++o)
        for (auto& h : sorted) pattern.push_back ({ h.first + 12 * o, h.second });
    const int n = (int) pattern.size();

    if (mode == 1) arpIndex = (arpIndex + 1) % n;
    else if (mode == 2) arpIndex = arpIndex <= 0 ? n - 1 : arpIndex - 1;
    else if (mode == 3)
    {
        if (n == 1) arpIndex = 0;
        else if (arpIndex < 0) { arpIndex = 0; arpDir = 1; }
        else
        {
            arpIndex += arpDir;
            if (arpIndex >= n) { arpIndex = n - 2; arpDir = -1; }
            if (arpIndex < 0)  { arpIndex = 1; arpDir = 1; }
        }
    }
    else arpIndex = (int) (rng.next() * (float) n) % n;
    arpIndex = juce::jlimit (0, n - 1, arpIndex);

    if (arpCurrent >= 0) releaseVoices (arpCurrent);
    arpCurrent = pattern[(size_t) arpIndex].first;
    triggerVoices (arpCurrent, pattern[(size_t) arpIndex].second);
    arpGateLeft = stepSamples * 0.8;
}

// ======================================================================= audio
void AquaSynthProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    buffer.clear();
    if ((int) mods.size() < numSamples + 16)
    {
        mods.assign ((size_t) numSamples + 16, ModFrame {});
        bufL.assign (mods.size(), 0.0f); bufR.assign (mods.size(), 0.0f);
    }

    double bpm = 120.0, ppq = 0.0;
    bool playing = false, havePpq = false;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            if (auto b = pos->getBpm()) bpm = *b;
            if (auto q = pos->getPpqPosition()) { ppq = *q; havePpq = true; }
            playing = pos->getIsPlaying();
        }
    bpm = juce::jlimit (20.0, 400.0, bpm);
    hostBpm.store ((float) bpm);
    const bool usePpq = playing && havePpq;
    hostSynced.store (usePpq);
    const double ppqPerSample = bpm / 60.0 / sr;

    // settings
    vs.sr = (float) sr;
    const int count = juce::jlimit (1, numOscs, (int) p ("oscCount"));
    for (int o = 0; o < numOscs; ++o)
    {
        auto& os = vs.osc[o];
        os.on = o < count;
        os.type = (int) p (oscId (o, "Type"));
        os.level = p (oscId (o, "Lvl"));
        os.semis = p (oscId (o, "Oct")) * 12.0f + p (oscId (o, "Semi")) + p (oscId (o, "Fine")) * 0.01f;
        os.unison = juce::jlimit (1, maxUnison, (int) p (oscId (o, "Uni")));
        if (os.type == tSample || os.type == tNoise) os.unison = 1;
        os.detune = p (oscId (o, "Det"));
        os.pos = p (oscId (o, "Pos"));
        os.table = (int) p (oscId (o, "Table"));
        os.keyTrack = p (oscId (o, "Key")) > 0.5f;
        os.sample = (os.type == tSample || os.type == tNoise) ? getOscSample (o) : nullptr;
    }
    vs.filterType = (int) p ("fType");
    vs.cutoff = p ("cutoff"); vs.reso = p ("reso"); vs.fAmt = p ("fAmt");
    vs.drive = p ("fDrive"); vs.keyTrack = p ("fKey");
    vs.pitchAmt = p ("pAmt");
    vs.pitchDecayCoef = std::exp (-1.0f / std::max (1.0f, p ("pD") * (float) sr * 0.3f));
    vs.drift = p ("drift"); vs.width = p ("width");
    for (auto& v : voices)
    {
        v.amp.set (p ("aA"), p ("aD"), p ("aS"), p ("aR"), (float) sr);
        v.fenv.set (p ("fA"), p ("fD"), p ("fS"), p ("fR"), (float) sr);
    }

    // LFOs
    float revMod = 0.0f;
    std::fill (mods.begin(), mods.begin() + numSamples, ModFrame {});
    for (int l = 0; l < 3; ++l)
    {
        const int shape = (int) p (lfoId (l, "Shape"));
        const bool sync = p (lfoId (l, "Sync")) > 0.5f;
        const float hz = p (lfoId (l, "Hz"));
        const float beats = lfoDivBeats[juce::jlimit (0, 7, (int) p (lfoId (l, "Div")))];
        const float depth = p (lfoId (l, "Depth"));
        const float smooth = p (lfoId (l, "Smooth"));
        const int d1 = (int) p (lfoId (l, "D1")), d2 = (int) p (lfoId (l, "D2"));
        const float coef = smooth < 0.001f ? 1.0f : 1.0f - std::exp (-1.0f / ((float) sr * (0.001f + smooth * 0.08f)));
        const double rateHz = sync ? bpm / 60.0 / beats : hz;
        for (int i = 0; i < numSamples; ++i)
        {
            double ph;
            if (sync && usePpq)
            {
                const double q = (ppq + i * ppqPerSample) / beats;
                ph = q - std::floor (q);
                freePhase[l] = ph;
            }
            else
            {
                freePhase[l] += rateHz / sr;
                if (freePhase[l] >= 1.0) freePhase[l] -= 1.0;
                ph = freePhase[l];
            }
            const float raw = lfos[l].step ((float) ph, shape, coef);
            if (depth <= 0.0001f) continue;
            auto& m = mods[(size_t) i];
            for (int dsel : { d1, d2 })
            {
                switch (dsel)
                {
                    case dOsc1: case dOsc2: case dOsc3: case dOsc4: m.pitch[dsel - dOsc1] += raw * depth * depth * 12.0f; break;
                    case dPitch:  for (auto& pv : m.pitch) pv += raw * depth * depth * 12.0f; break;
                    case dCutoff: m.cutoffOct += raw * depth * 4.0f; break;
                    case dReso:   m.reso += raw * depth * 0.5f; break;
                    case dPan:    m.pan += raw * depth; break;
                    case dWidth:  m.width += raw * depth * 0.6f; break;
                    case dRev:    if (i == 0) revMod += (raw * 0.5f + 0.5f) * depth * 0.6f; break;
                    default: break;
                }
            }
        }
    }

    // MIDI + arp + voices, in chunks of 16 samples
    auto midiIt = midi.begin();
    for (int start = 0; start < numSamples; start += 16)
    {
        const int len = std::min (16, numSamples - start);
        for (; midiIt != midi.end(); ++midiIt)
        {
            const auto meta = *midiIt;
            if (meta.samplePosition >= start + len) break;
            const auto msg = meta.getMessage();
            if (msg.isNoteOn()) noteOn (msg.getNoteNumber(), 0.35f + 0.65f * msg.getFloatVelocity());
            else if (msg.isNoteOff()) noteOff (msg.getNoteNumber());
            else if (msg.isAllNotesOff() || msg.isAllSoundOff()) allNotesOff();
        }
        handleArp (len, ppq + start * ppqPerSample, usePpq, bpm);

        std::fill (bufL.begin(), bufL.begin() + len, 0.0f);
        std::fill (bufR.begin(), bufR.begin() + len, 0.0f);
        for (auto& v : voices)
            if (v.active) v.render (bufL.data(), bufR.data(), len, vs, mods.data() + start, bank);
        buffer.copyFrom (0, start, bufL.data(), len);
        buffer.copyFrom (1, start, bufR.data(), len);
    }

    // effects, in the order shown on the FX page
    float* L = buffer.getWritePointer (0);
    float* R = buffer.getWritePointer (1);
    const float fsr = (float) sr;
    const float space = p ("space"), width = p ("width");
    crusher.process (L, R, numSamples, p ("digital"));
    if (p ("distOn") > 0.5f) dist.process (L, R, numSamples, p ("distDrive"), p ("distTone"), p ("distMix"), fsr);
    const float choMix = std::min (1.0f, (p ("choOn") > 0.5f ? p ("choMix") : 0.0f) + space * 0.4f);
    chorus.process (L, R, numSamples, p ("choRate"), p ("choDepth"), choMix);
    if (p ("flaOn") > 0.5f) flanger.process (L, R, numSamples, p ("flaRate"), p ("flaFb"), p ("flaMix"));
    if (p ("dlyOn") > 0.5f)
        delay.process (L, R, numSamples, (float) (60.0 / bpm) * delayBeats[juce::jlimit (0, 7, (int) p ("dlyTime"))], p ("dlyFb"), p ("dlyMix"));

    const float revMix = juce::jlimit (0.0f, 1.0f, (p ("revOn") > 0.5f ? p ("revMix") : 0.0f) + space * 0.5f + revMod);
    if (revMix > 0.001f)
    {
        juce::dsp::Reverb::Parameters rp;
        rp.roomSize = 0.3f + 0.69f * p ("revSize");
        rp.damping = p ("revDamp");
        rp.wetLevel = revMix * 0.9f;
        rp.dryLevel = 1.0f - revMix * 0.4f;
        rp.width = 0.6f + 0.4f * width;
        reverb.setParameters (rp);
        juce::dsp::AudioBlock<float> block (buffer);
        juce::dsp::ProcessContextReplacing<float> ctx (block);
        reverb.process (ctx);
    }
    if (p ("eqOn") > 0.5f)
    {
        const float g[3] { p ("eqLow"), p ("eqMid"), p ("eqHigh") };
        for (int b = 0; b < 3; ++b)
            if (g[b] != eqCache[b])
            {
                eqCache[b] = g[b];
                const float gain = juce::Decibels::decibelsToGain (g[b]);
                auto c = b == 0 ? juce::dsp::IIR::Coefficients<float>::makeLowShelf (sr, 200.0, 0.7, gain)
                       : b == 1 ? juce::dsp::IIR::Coefficients<float>::makePeakFilter (sr, 1000.0, 0.7, gain)
                                : juce::dsp::IIR::Coefficients<float>::makeHighShelf (sr, 5000.0, 0.7, gain);
                *eqL[b].coefficients = *c; *eqR[b].coefficients = *c;
            }
        for (int i = 0; i < numSamples; ++i)
            for (int b = 0; b < 3; ++b) { L[i] = eqL[b].processSample (L[i]); R[i] = eqR[b].processSample (R[i]); }
    }
    if (p ("cmpOn") > 0.5f) comp.process (L, R, numSamples, p ("cmpAmt"), p ("cmpSpeed"), p ("cmpMix"), fsr);
    if (p ("stOn") > 0.5f) stereo.process (L, R, numSamples, p ("stWidth"), p ("stMono"), p ("stLpf"), fsr);

    const float gain = p ("aLvl") * 0.6f;
    int w = visWrite.load (std::memory_order_relaxed);
    for (int i = 0; i < numSamples; ++i)
    {
        L[i] = fastTanh (L[i] * gain);
        R[i] = fastTanh (R[i] * gain);
        if (! std::isfinite (L[i]) || ! std::isfinite (R[i])) { L[i] = R[i] = 0.0f; }
        visL[(size_t) w] = L[i]; visR[(size_t) w] = R[i];
        w = (w + 1) & (visSize - 1);
    }
    visWrite.store (w, std::memory_order_relaxed);
}

void AquaSynthProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("presetName", currentPresetName, nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary (*xml, destData);
}

void AquaSynthProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto tree = juce::ValueTree::fromXml (*xml);
            apvts.replaceState (tree);
            currentPresetName = tree.getProperty ("presetName", "").toString();
            restoreSamplesFromState();
        }
}

juce::AudioProcessorEditor* AquaSynthProcessor::createEditor() { return new AquaSynthEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new AquaSynthProcessor(); }
