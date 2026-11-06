#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

namespace us
{
constexpr int numOscs = 4;
inline const juce::StringArray oscTypes   { "SINE", "TRIANGLE", "JUNO SAW", "JUNO PULSE", "WAVETABLE", "FM 2-OP", "FORMANT", "NOISE", "SAMPLE" };
inline const juce::StringArray oscShort   { "SINE", "TRI", "SAW", "PULSE", "WAVE", "FM", "FORMANT", "NOISE", "SAMPLE" };
enum OscType { tSine, tTri, tSaw, tPulse, tWave, tFm, tFormant, tNoise, tSample, numOscTypes };
inline const juce::StringArray wtTables   { "SHAPES", "ANALOG", "DIGITAL" };
inline const juce::StringArray filterTypes{ "LP24", "LP12", "HP", "BP" };
inline const juce::StringArray lfoShapes  { "SINE", "TRI", "SQUARE", "SAW", "RANDOM" };
inline const juce::StringArray lfoDivs    { "4/1", "2/1", "1/1", "1/2", "1/4", "1/8", "1/16", "1/32" };
inline const float lfoDivBeats[] { 16.0f, 8.0f, 4.0f, 2.0f, 1.0f, 0.5f, 0.25f, 0.125f };
inline const juce::StringArray delayDivs  { "1/32", "1/16", "1/8T", "1/8", "1/8D", "1/4", "1/4D", "1/2" };
inline const float delayBeats[] { 0.125f, 0.25f, 1.0f / 3.0f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f };
inline const juce::StringArray lfoDests   { "OSC1", "OSC2", "OSC3", "OSC4", "CUTOFF", "RESO", "PITCH", "PAN", "REV", "WIDTH", "NONE" };
inline const juce::StringArray arpModes   { "OFF", "UP", "DOWN", "UP-DN", "RAND" };
inline const juce::StringArray voiceModes { "POLY", "MONO", "UNISON" };

enum Dest { dOsc1, dOsc2, dOsc3, dOsc4, dCutoff, dReso, dPitch, dPan, dRev, dWidth, dNone, numDests };

inline juce::String lfoId (int lfo, const char* name) { return "l" + juce::String (lfo + 1) + name; }
inline juce::String oscId (int osc, const char* name) { return "o" + juce::String (osc + 1) + name; }

// effects, in signal order: id prefix, display name
struct FxInfo { const char* id; const char* name; };
inline const FxInfo fxList[8] { { "dist", "DISTORTION" }, { "cho", "CHORUS" }, { "fla", "FLANGER" }, { "dly", "DELAY" },
                                { "rev", "REVERB" }, { "eq", "EQ" }, { "cmp", "COMP" }, { "st", "STEREO" } };

inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    auto pct = [] (const String& id, const String& name, float def)
    {
        return std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, NormalisableRange<float> (0.0f, 1.0f), def,
            AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v * 100.0f)) + "%"; }));
    };
    auto time = [] (const String& id, const String& name, float def)
    {
        NormalisableRange<float> r (0.0005f, 8.0f);
        r.setSkewForCentre (0.35f);
        return std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, r, def,
            AudioParameterFloatAttributes().withLabel ("s").withStringFromValueFunction ([] (float v, int)
            { return v < 1.0f ? String (v * 1000.0f, v < 0.01f ? 1 : 0) + "ms" : String (v, 2) + "s"; }));
    };
    auto hzParam = [] (const String& id, const String& name, float lo, float hi, float centre, float def)
    {
        NormalisableRange<float> r (lo, hi);
        r.setSkewForCentre (centre);
        return std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, r, def,
            AudioParameterFloatAttributes().withLabel ("Hz").withStringFromValueFunction ([] (float v, int)
            { return v >= 1000.0f ? String (v / 1000.0f, 1) + "kHz" : (v < 10.0f ? String (v, 2) : String (roundToInt (v))) + "Hz"; }));
    };
    auto db = [] (const String& id, const String& name)
    {
        return std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, NormalisableRange<float> (-12.0f, 12.0f), 0.0f,
            AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return (v > 0.05f ? "+" : "") + String (v, 1) + "dB"; }));
    };
    auto onOff = [] (const String& id, const String& name, bool def)
    { return std::make_unique<AudioParameterBool> (ParameterID { id, 1 }, name, def); };

    layout.add (std::make_unique<AudioParameterInt> (ParameterID { "oscCount", 1 }, "Oscillators", 1, numOscs, 3));
    const int   typeDef[] { tTri, tSaw, tWave, tNoise };
    const float lvlDef[]  { 0.8f, 0.45f, 0.3f, 0.2f };
    for (int i = 0; i < numOscs; ++i)
    {
        auto n = "OSC" + String (i + 1) + " ";
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { oscId (i, "Type"), 1 }, n + "Type", oscTypes, typeDef[i]));
        layout.add (pct (oscId (i, "Lvl"), n + "Level", lvlDef[i]));
        layout.add (std::make_unique<AudioParameterInt> (ParameterID { oscId (i, "Oct"), 1 }, n + "Octave", -3, 3, 0));
        layout.add (std::make_unique<AudioParameterInt> (ParameterID { oscId (i, "Semi"), 1 }, n + "Semitone", -12, 12, 0,
            AudioParameterIntAttributes().withStringFromValueFunction ([] (int v, int) { return (v > 0 ? "+" : "") + String (v) + "st"; })));
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { oscId (i, "Fine"), 1 }, n + "Fine", NormalisableRange<float> (-100.0f, 100.0f), 0.0f,
            AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return (v > 0.5f ? "+" : "") + String (roundToInt (v)) + "ct"; })));
        layout.add (std::make_unique<AudioParameterInt> (ParameterID { oscId (i, "Uni"), 1 }, n + "Unison", 1, 7, 1));
        layout.add (pct (oscId (i, "Det"), n + "Detune", 0.25f));
        layout.add (pct (oscId (i, "Pos"), n + "Position", 0.4f));
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { oscId (i, "Table"), 1 }, n + "Table", wtTables, 2));
        layout.add (onOff (oscId (i, "Key"), n + "Key Track", true));
    }

    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "fType", 1 }, "Filter Type", filterTypes, 0));
    layout.add (hzParam ("cutoff", "Cutoff", 20.0f, 18000.0f, 900.0f, 1400.0f));
    layout.add (pct ("reso", "Resonance", 0.15f));
    layout.add (pct ("fDrive", "Filter Drive", 0.0f));
    layout.add (pct ("fKey", "Filter Key Track", 0.5f));
    layout.add (time ("fA", "Filter Attack", 0.005f));
    layout.add (time ("fD", "Filter Decay", 0.45f));
    layout.add (pct ("fS", "Filter Sustain", 0.35f));
    layout.add (time ("fR", "Filter Release", 0.4f));
    layout.add (pct ("fAmt", "Filter Env Amount", 0.45f));
    layout.add (time ("aA", "Amp Attack", 0.005f));
    layout.add (time ("aD", "Amp Decay", 0.6f));
    layout.add (pct ("aS", "Amp Sustain", 0.6f));
    layout.add (time ("aR", "Amp Release", 0.5f));
    layout.add (pct ("aLvl", "Level", 0.8f));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "pAmt", 1 }, "Pitch Env Amount", NormalisableRange<float> (-24.0f, 24.0f), 0.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return (v > 0.05f ? "+" : "") + String (v, 1) + "st"; })));
    layout.add (time ("pD", "Pitch Env Decay", 0.08f));

    layout.add (pct ("drift", "Drift", 0.3f));
    layout.add (pct ("width", "Width", 0.35f));
    layout.add (pct ("digital", "90s", 0.15f));
    layout.add (pct ("space", "Space", 0.0f));

    const int   shapeDef[] { 0, 1, 4 };
    const bool  syncDef[]  { true, false, false };
    const float hzDef[]    { 1.0f, 0.35f, 0.2f };
    const float depthDef[] { 0.15f, 0.2f, 0.0f };
    const int   d1Def[]    { dOsc1, dOsc2, dNone };
    const int   d2Def[]    { dRev, dPan, dNone };
    for (int l = 0; l < 3; ++l)
    {
        auto n = "LFO" + String (l + 1) + " ";
        NormalisableRange<float> hz (0.05f, 20.0f);
        hz.setSkewForCentre (1.0f);
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { lfoId (l, "Shape"), 1 }, n + "Shape", lfoShapes, shapeDef[l]));
        layout.add (onOff (lfoId (l, "Sync"), n + "Sync", syncDef[l]));
        layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { lfoId (l, "Hz"), 1 }, n + "Rate Hz", hz, hzDef[l],
            AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, 2) + "Hz"; })));
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { lfoId (l, "Div"), 1 }, n + "Rate Sync", lfoDivs, 4));
        layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { lfoId (l, "Depth"), 1 }, n + "Depth", NormalisableRange<float> (0.0f, 1.0f), depthDef[l],
            AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v * 100.0f)) + "%"; })));
        layout.add (pct (lfoId (l, "Smooth"), n + "Smooth", 0.3f));
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { lfoId (l, "D1"), 1 }, n + "Target 1", lfoDests, d1Def[l]));
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { lfoId (l, "D2"), 1 }, n + "Target 2", lfoDests, d2Def[l]));
    }

    // effects page
    layout.add (onOff ("distOn", "Distortion On", false));
    layout.add (pct ("distDrive", "Distortion Drive", 0.25f));
    layout.add (pct ("distTone", "Distortion Tone", 0.5f));
    layout.add (pct ("distMix", "Distortion Mix", 0.5f));
    layout.add (onOff ("choOn", "Chorus On", true));
    layout.add (hzParam ("choRate", "Chorus Rate", 0.05f, 5.0f, 0.6f, 0.55f));
    layout.add (pct ("choDepth", "Chorus Depth", 0.5f));
    layout.add (pct ("choMix", "Chorus Mix", 0.35f));
    layout.add (onOff ("flaOn", "Flanger On", false));
    layout.add (hzParam ("flaRate", "Flanger Rate", 0.02f, 4.0f, 0.3f, 0.2f));
    layout.add (pct ("flaFb", "Flanger Feedback", 0.4f));
    layout.add (pct ("flaMix", "Flanger Mix", 0.4f));
    layout.add (onOff ("dlyOn", "Delay On", false));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "dlyTime", 1 }, "Delay Time", delayDivs, 4));
    layout.add (pct ("dlyFb", "Delay Feedback", 0.35f));
    layout.add (pct ("dlyMix", "Delay Mix", 0.2f));
    layout.add (onOff ("revOn", "Reverb On", true));
    layout.add (pct ("revSize", "Reverb Size", 0.6f));
    layout.add (pct ("revDamp", "Reverb Damp", 0.5f));
    layout.add (pct ("revMix", "Reverb Mix", 0.25f));
    layout.add (onOff ("eqOn", "EQ On", false));
    layout.add (db ("eqLow", "EQ Low"));
    layout.add (db ("eqMid", "EQ Mid"));
    layout.add (db ("eqHigh", "EQ High"));
    layout.add (onOff ("cmpOn", "Comp On", false));
    layout.add (pct ("cmpAmt", "Comp Amount", 0.3f));
    layout.add (pct ("cmpSpeed", "Comp Speed", 0.5f));
    layout.add (pct ("cmpMix", "Comp Mix", 1.0f));
    layout.add (onOff ("stOn", "Stereo On", false));
    layout.add (pct ("stWidth", "Stereo Width", 0.5f));
    layout.add (hzParam ("stMono", "Mono Below", 20.0f, 400.0f, 120.0f, 20.0f));
    layout.add (hzParam ("stLpf", "Top Cut", 1000.0f, 20000.0f, 6000.0f, 20000.0f));

    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "arpMode", 1 }, "Arp Mode", arpModes, 0));
    layout.add (std::make_unique<AudioParameterInt>    (ParameterID { "arpOct", 1 }, "Arp Octaves", 1, 3, 1));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "voice", 1 }, "Voice Mode", voiceModes, 0));
    return layout;
}
} // namespace us
