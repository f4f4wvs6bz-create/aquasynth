#pragma once
#include <juce_core/juce_core.h>
#include <map>
#include <memory>
#include "Engine.h"

// Reads a Serum 2 .SerumPreset file (XferJson header + Zstandard-compressed CBOR, as documented by
// open reverse-engineering work) and turns its settings into this synth's settings.
// Serum's own factory wavetables and samples are not inside preset files, so those are replaced by
// this synth's own similar waveforms. Recordings that the preset author embedded are used as-is.
struct SerumImportResult
{
    bool ok = false;
    juce::String error, name, author, info;
    std::map<juce::String, float> values;               // plain parameter values
    std::shared_ptr<us::SampleData> samples[us::numOscs]; // embedded recordings for SAMPLE oscillators
    juce::StringArray applied, close, missing;
};

SerumImportResult importSerumPreset (const juce::File& file);
