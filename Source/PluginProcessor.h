#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <map>
#include <vector>
#include "Params.h"
#include "Engine.h"

class AquaSynthProcessor : public juce::AudioProcessor
{
public:
    AquaSynthProcessor();
    ~AquaSynthProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    using AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "AquaSynth"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 4.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    int getTheme() const { return (int) apvts.state.getProperty ("theme", 0); }
    void setTheme (int t) { apvts.state.setProperty ("theme", t, nullptr); }

    // ---- samples used by SAMPLE / NOISE oscillators (message thread)
    void setOscSample (int osc, std::shared_ptr<us::SampleData> data);
    std::shared_ptr<us::SampleData> getOscSample (int osc) const { return std::atomic_load (&samples[(size_t) osc]); }

    // ---- presets (message thread)
    void resetToDefaults();
    void applyValues (const std::map<juce::String, float>& plainValues);
    bool savePreset (const juce::File& file, const juce::String& name);
    bool loadPreset (const juce::File& file);
    void loadStarter (int index);
    static juce::StringArray starterNames();
    static juce::File presetFolder();
    juce::String currentPresetName;

    // read by the editor for the spread display (single writer, lossy reads are fine)
    static constexpr int visSize = 8192;
    std::array<float, visSize> visL {}, visR {};
    std::atomic<int> visWrite { 0 };
    std::atomic<float> hostBpm { 120.0f };
    std::atomic<bool> hostSynced { false };

private:
    float p (const juce::String& id) const { return params.at (id)->load(); }
    void noteOn (int note, float vel);
    void noteOff (int note);
    void allNotesOff();
    void handleArp (int chunk, double ppqAtChunk, bool usePpq, double bpm);
    void triggerVoices (int note, float vel);
    void releaseVoices (int note);
    us::Voice* findVoice();
    void restoreSamplesFromState();
    void storeSampleInState (int osc, const us::SampleData* data);

    std::map<juce::String, std::atomic<float>*> params;
    std::array<us::Voice, 16> voices;
    std::array<std::shared_ptr<us::SampleData>, us::numOscs> samples;
    us::WaveBank bank;
    us::VoiceSettings vs;
    us::Lfo lfos[3];
    double freePhase[3] { 0, 0, 0 };
    us::Crusher crusher;
    us::Distortion dist;
    us::Chorus chorus;
    us::Flanger flanger;
    us::Delay delay;
    juce::dsp::Reverb reverb;
    juce::dsp::IIR::Filter<float> eqL[3], eqR[3];
    float eqCache[3] { 99, 99, 99 };
    us::Compressor comp;
    us::StereoTool stereo;
    std::vector<us::ModFrame> mods;
    std::vector<float> bufL, bufR;
    us::Rng rng;
    uint32_t stamp = 0;
    double sr = 44100.0;

    std::vector<int> monoStack;
    std::vector<std::pair<int, float>> held;
    int arpCurrent = -1, arpIndex = -1, arpDir = 1;
    long long lastArpStep = -1;
    double arpClock = 0, arpGateLeft = 0;
    bool arpWasOn = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AquaSynthProcessor)
};
