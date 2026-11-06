// Offline check: renders starters and imported Serum presets to WAV, checks levels / NaN / CPU,
// saves and reloads a preset, and writes panel screenshots of every page.
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"
#include "../Source/SerumImport.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <iostream>

using namespace juce;

using Notes = std::vector<std::array<float, 3>>; // note, start s, length s

static int failures = 0;

static void render (AquaSynthProcessor& p, const String& name, const Notes& notes, const File& out, double seconds = 5.5)
{
    const double sr = 44100.0; const int block = 512;
    p.setPlayConfigDetails (0, 2, sr, block);
    p.prepareToPlay (sr, block);
    const int total = (int) (sr * seconds);
    AudioBuffer<float> all (2, total), buf (2, block);
    double maxAbs = 0; bool bad = false;
    const auto t0 = Time::getMillisecondCounterHiRes();
    for (int pos = 0; pos < total; pos += block)
    {
        MidiBuffer midi;
        for (auto& n : notes)
        {
            const int on = (int) (n[1] * sr), off = (int) ((n[1] + n[2]) * sr);
            if (on >= pos && on < pos + block) midi.addEvent (MidiMessage::noteOn (1, (int) n[0], (uint8) 100), on - pos);
            if (off >= pos && off < pos + block) midi.addEvent (MidiMessage::noteOff (1, (int) n[0]), off - pos);
        }
        buf.clear();
        p.processBlock (buf, midi);
        const int len = std::min (block, total - pos);
        for (int c = 0; c < 2; ++c)
        {
            all.copyFrom (c, pos, buf, c, 0, len);
            for (int i = 0; i < len; ++i)
            {
                const float v = buf.getSample (c, i);
                if (! std::isfinite (v)) bad = true;
                maxAbs = std::max (maxAbs, (double) std::abs (v));
            }
        }
    }
    const double ms = Time::getMillisecondCounterHiRes() - t0;
    std::cout << name << ": peak " << maxAbs << " rms " << all.getRMSLevel (0, 0, total)
              << " cpu " << (ms / (seconds * 1000.0) * 100.0) << "% of realtime" << (bad ? " NAN!" : "") << std::endl;
    if (bad || maxAbs < 0.01 || maxAbs > 1.0) ++failures;
    auto f = out.getChildFile (name + ".wav");
    f.deleteFile();
    WavAudioFormat wav;
    std::unique_ptr<AudioFormatWriter> w (wav.createWriterFor (new FileOutputStream (f), sr, 2, 24, {}, 0));
    if (w) w->writeFromAudioSampleBuffer (all, 0, total);
}

static void shot (Component& c, const File& f)
{
    auto img = c.createComponentSnapshot (c.getLocalBounds(), true, 1.0f);
    f.deleteFile();
    FileOutputStream os (f);
    PNGImageFormat().writeImageToStream (img, os);
}

static void pump (int ms)
{
    Timer::callAfterDelay (ms, [] { MessageManager::getInstance()->stopDispatchLoop(); });
    MessageManager::getInstance()->runDispatchLoop();
}

int main (int argc, char** argv)
{
    ScopedJuceInitialiser_GUI gui;
    const File out (argc > 1 ? argv[1] : "render_out");
    const File serumDir (argc > 2 ? argv[2] : "serum");
    out.createDirectory();

    const Notes chordNotes[5] {
        { { 58, 0.0f, 0.35f }, { 58, 0.4f, 0.35f }, { 57, 0.8f, 1.2f }, { 46, 0.8f, 1.2f } },
        { { 73, 0.0f, 2.4f }, { 87, 0.3f, 1.0f } },
        { { 67, 0.0f, 3.0f }, { 70, 0.0f, 3.0f }, { 74, 0.0f, 3.0f }, { 75, 0.0f, 3.0f } },
        { { 58, 0.0f, 3.4f }, { 63, 0.0f, 3.4f }, { 67, 0.0f, 3.4f }, { 70, 0.0f, 3.4f }, { 74, 0.0f, 3.4f } },
        { { 62, 0.0f, 3.5f }, { 67, 0.0f, 3.5f }, { 69, 0.0f, 3.5f }, { 71, 0.0f, 3.5f } },
    };
    const auto names = AquaSynthProcessor::starterNames();
    for (int i = 0; i < names.size(); ++i)
    {
        AquaSynthProcessor p;
        p.loadStarter (i);
        render (p, "starter_" + String (i + 1) + "_" + names[i].replaceCharacter (' ', '_'), chordNotes[i], out);
    }

    // all effects on, 4 oscillators with 7-voice unison, 8 notes: worst-case CPU
    {
        AquaSynthProcessor p;
        p.loadStarter (3);
        auto setP = [&] (const String& id, float v) { auto* prm = p.apvts.getParameter (id); prm->setValueNotifyingHost (prm->convertTo0to1 (v)); };
        setP ("oscCount", 4);
        for (int o = 0; o < 4; ++o) setP (us::oscId (o, "Uni"), 7);
        for (auto& fx : us::fxList) setP (String (fx.id) + "On", 1);
        Notes n; for (int k = 0; k < 8; ++k) n.push_back ({ (float) (48 + k * 3), 0.0f, 3.0f });
        render (p, "stress_all_fx_4osc_7uni_8notes", n, out, 4.0);
    }

    // Serum imports
    const Notes serumNotes { { 60, 0.0f, 1.6f }, { 64, 0.0f, 1.6f }, { 67, 0.0f, 1.6f }, { 72, 2.0f, 1.6f } };
    File lastImport;
    for (auto& f : serumDir.findChildFiles (File::findFiles, false, "*.SerumPreset"))
    {
        auto res = importSerumPreset (f);
        std::cout << "\n=== " << f.getFileName() << " ok=" << res.ok << " " << res.error << "\n" << res.name << " | " << res.info << std::endl;
        if (! res.ok) { ++failures; continue; }
        for (auto* col : { &res.applied, &res.close, &res.missing })
        {
            std::cout << (col == &res.applied ? " APPLIED" : col == &res.close ? " CLOSE" : " MISSING") << std::endl;
            for (auto& s : *col) std::cout << "   - " << s << std::endl;
        }
        if (std::getenv ("DUMP")) for (auto& kv : res.values) std::cout << "   " << kv.first << "=" << kv.second << std::endl;
        AquaSynthProcessor p;
        p.resetToDefaults();
        p.applyValues (res.values);
        for (int o = 0; o < us::numOscs; ++o) p.setOscSample (o, res.samples[o]);
        render (p, "serum_" + res.name.toLowerCase().replaceCharacter (' ', '_'), serumNotes, out, 4.5);

        // save -> load round trip keeps the embedded sample and every value
        auto pf = out.getChildFile ("roundtrip.aqpreset");
        p.savePreset (pf, res.name);
        AquaSynthProcessor q;
        const bool ok = q.loadPreset (pf);
        int diff = 0;
        for (auto* prm : p.getParameters())
            if (auto* rp = dynamic_cast<RangedAudioParameter*> (prm))
                if (std::abs (rp->getValue() - q.apvts.getParameter (rp->getParameterID())->getValue()) > 1.0e-4f) ++diff;
        int samples = 0;
        for (int o = 0; o < us::numOscs; ++o) samples += (p.getOscSample (o) != nullptr) == (q.getOscSample (o) != nullptr) ? 0 : 1;
        std::cout << " roundtrip: loaded=" << ok << " differing params=" << diff << " sample mismatches=" << samples
                  << " file " << pf.getSize() / 1024 << " KB" << std::endl;
        if (! ok || diff || samples) ++failures;
        pf.deleteFile();
        lastImport = f;
    }

    // screenshots
    {
        AquaSynthProcessor p;
        p.loadStarter (3);
        const double sr = 44100.0; const int block = 512;
        p.setPlayConfigDetails (0, 2, sr, block);
        p.prepareToPlay (sr, block);
        AudioBuffer<float> buf (2, block);
        MidiBuffer midi;
        for (int n : { 58, 63, 67, 70, 74 }) midi.addEvent (MidiMessage::noteOn (1, n, (uint8) 100), 0);
        for (int i = 0; i < 40; ++i) { p.processBlock (buf, midi); midi.clear(); }
        for (int th = 0; th < 3; ++th)
        {
            p.setTheme (th);
            std::unique_ptr<AudioProcessorEditor> ed (p.createEditor());
            ed->setSize (AquaSynthEditor::baseW, AquaSynthEditor::baseH);
            for (int i = 0; i < 4; ++i) { p.processBlock (buf, midi); pump (60); }
            shot (*ed, out.getChildFile ("page_main_" + String (th) + ".png"));
            if (th == 0)
            {
                auto* e = dynamic_cast<AquaSynthEditor*> (ed.get());
                e->showPageForTest (1); pump (60);
                shot (*ed, out.getChildFile ("page_fx.png"));
                if (lastImport.existsAsFile()) e->importForTest (lastImport);
                else e->showPageForTest (2);
                pump (60);
                shot (*ed, out.getChildFile ("page_presets.png"));
            }
        }
    }
    std::cout << (failures == 0 ? "ALL OK" : "FAILURES: " + std::to_string (failures)) << std::endl;
    return failures;
}
