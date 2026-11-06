#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

class AquaSynthEditor : public juce::AudioProcessorEditor
{
public:
    explicit AquaSynthEditor (AquaSynthProcessor&);
    ~AquaSynthEditor() override;
    void paint (juce::Graphics&) override {}
    void resized() override;

    void showPageForTest (int page);
    void importForTest (const juce::File& f);

    static constexpr int baseW = 1120, baseH = 840;

private:
    AquaSynthProcessor& proc;
    std::unique_ptr<juce::Component> panel;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AquaSynthEditor)
};
