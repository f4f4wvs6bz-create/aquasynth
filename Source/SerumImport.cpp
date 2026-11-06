#include "SerumImport.h"
#include "Params.h"
#include "../ThirdParty/zstd/zstd.h"

using namespace juce;

namespace
{
// ------------------------------------------------------------ minimal CBOR reader -> juce::var
// Long numeric arrays (embedded audio) are stored as binary blocks of float32 to save memory.
struct Cbor
{
    const uint8_t* p; const uint8_t* end; bool bad = false;

    uint8_t u8() { if (p >= end) { bad = true; return 0; } return *p++; }
    uint64_t be (int n) { uint64_t v = 0; for (int i = 0; i < n; ++i) v = (v << 8) | u8(); return v; }
    uint64_t arg (uint8_t ai)
    {
        if (ai < 24) return ai;
        if (ai == 24) return be (1);
        if (ai == 25) return be (2);
        if (ai == 26) return be (4);
        if (ai == 27) return be (8);
        bad = true; return 0;
    }
    static float half (uint16_t h)
    {
        const int e = (h >> 10) & 0x1f, m = h & 0x3ff;
        float v = e == 0 ? std::ldexp ((float) m, -24) : (e == 31 ? (m ? NAN : INFINITY) : std::ldexp ((float) (m + 1024), e - 25));
        return (h & 0x8000) ? -v : v;
    }
    bool isNumberAhead() const
    {
        if (p >= end) return false;
        const uint8_t mt = *p >> 5, ai = *p & 31;
        return mt == 0 || mt == 1 || (mt == 7 && ai >= 25 && ai <= 27);
    }
    double number()
    {
        const uint8_t b = u8(), mt = b >> 5, ai = b & 31;
        if (mt == 0) return (double) arg (ai);
        if (mt == 1) return -1.0 - (double) arg (ai);
        if (ai == 25) return half ((uint16_t) be (2));
        if (ai == 26) { uint32_t u = (uint32_t) be (4); float f; std::memcpy (&f, &u, 4); return f; }
        uint64_t u = be (8); double d; std::memcpy (&d, &u, 8); return d;
    }
    String text (uint8_t ai)
    {
        if (ai == 31)
        {
            String s;
            while (p < end && *p != 0xff) { const uint8_t b = u8(); s += text (b & 31); }
            u8();
            return s;
        }
        const uint64_t n = arg (ai);
        if (n > (uint64_t) (end - p)) { bad = true; return {}; }
        String s = String::fromUTF8 ((const char*) p, (int) n);
        p += n;
        return s;
    }
    var parse (int depth)
    {
        if (bad || depth > 64 || p >= end) { bad = true; return {}; }
        const uint8_t b = u8(), mt = b >> 5, ai = b & 31;
        switch (mt)
        {
            case 0: return (double) arg (ai);
            case 1: return -1.0 - (double) arg (ai);
            case 2: { if (ai == 31) { while (p < end && *p != 0xff) { const uint8_t c = u8(); text (c & 31); } u8(); return {}; }
                      const uint64_t n = arg (ai); if (n > (uint64_t) (end - p)) { bad = true; return {}; } p += n; return {}; }
            case 3: return text (ai);
            case 4:
            {
                const bool indef = ai == 31;
                const uint64_t n = indef ? 0 : arg (ai);
                if (! indef && n > (uint64_t) (end - p)) { bad = true; return {}; }
                std::vector<float> nums;
                Array<var> items;
                bool numeric = true;
                for (uint64_t i = 0; indef ? (p < end && *p != 0xff) : i < n; ++i)
                {
                    if (bad) break;
                    if (numeric && isNumberAhead()) { nums.push_back ((float) number()); continue; }
                    if (numeric) { for (auto f : nums) items.add ((double) f); nums.clear(); numeric = false; }
                    items.add (parse (depth + 1));
                }
                if (indef) u8();
                if (numeric)
                {
                    if (nums.size() > 64) return var (MemoryBlock (nums.data(), nums.size() * sizeof (float)));
                    for (auto f : nums) items.add ((double) f);
                }
                return var (items);
            }
            case 5:
            {
                const bool indef = ai == 31;
                const uint64_t n = indef ? 0 : arg (ai);
                if (! indef && n > (uint64_t) (end - p)) { bad = true; return {}; }
                auto* obj = new DynamicObject();
                var result (obj);
                for (uint64_t i = 0; indef ? (p < end && *p != 0xff) : i < n; ++i)
                {
                    if (bad) break;
                    const var k = parse (depth + 1);
                    const var v = parse (depth + 1);
                    const String key = k.toString();
                    if (key.isNotEmpty()) obj->setProperty (Identifier (key), v);
                }
                if (indef) u8();
                return result;
            }
            case 6: arg (ai); return parse (depth + 1);
            default:
                if (ai == 20) return false;
                if (ai == 21) return true;
                if (ai == 22 || ai == 23) return {};
                if (ai == 25) return (double) half ((uint16_t) be (2));
                if (ai == 26) { uint32_t u = (uint32_t) be (4); float f; std::memcpy (&f, &u, 4); return (double) f; }
                if (ai == 27) { uint64_t u = be (8); double d; std::memcpy (&d, &u, 8); return d; }
                return {};
        }
    }
};

// ------------------------------------------------------------ helpers
var child (const var& v, const String& key) { return v.getProperty (Identifier (key), var()); }
var plain (const var& module)
{
    auto pp = child (module, "plainParams");
    return pp.isObject() ? pp : var (new DynamicObject());
}
bool has (const var& pp, const char* key) { return pp.getDynamicObject() != nullptr && pp.getDynamicObject()->hasProperty (key); }
double num (const var& pp, const char* key, double def)
{
    auto x = pp.getProperty (key, var());
    return (x.isDouble() || x.isInt() || x.isInt64() || x.isBool()) ? (double) x : def;
}
String str (const var& pp, const char* key, const String& def = {})
{
    auto x = pp.getProperty (key, var());
    return x.isString() ? x.toString() : def;
}

std::shared_ptr<us::SampleData> audioFrom (const var& channels, double sampleRate, const String& name)
{
    if (! channels.isArray() || channels.size() == 0) return nullptr;
    const int numCh = jmin (2, channels.size());
    int len = 0;
    for (int c = 0; c < numCh; ++c)
        if (auto* mb = channels[c].getBinaryData()) len = len == 0 ? (int) (mb->getSize() / sizeof (float)) : jmin (len, (int) (mb->getSize() / sizeof (float)));
    if (len < 16) return nullptr;
    auto d = std::make_shared<us::SampleData>();
    d->sampleRate = sampleRate > 1000 ? sampleRate : 44100.0;
    d->name = name;
    d->buffer.setSize (numCh, len);
    for (int c = 0; c < numCh; ++c)
        if (auto* mb = channels[c].getBinaryData())
            d->buffer.copyFrom (c, 0, static_cast<const float*> (mb->getData()), len);
    return d;
}

String shortName (const String& path)
{
    return File::createFileWithoutCheckingPath (path.replaceCharacter ('\\', '/')).getFileNameWithoutExtension();
}
} // namespace

SerumImportResult importSerumPreset (const File& file)
{
    SerumImportResult r;
    MemoryBlock raw;
    if (file.getSize() > 300 * 1024 * 1024 || ! file.loadFileAsData (raw)) { r.error = "could not read the file"; return r; }
    const auto* b = static_cast<const uint8_t*> (raw.getData());
    const size_t size = raw.getSize();
    if (size < 33 || std::memcmp (b, "XferJson\0", 9) != 0) { r.error = "this is not a Serum 2 preset (.SerumPreset)"; return r; }
    uint64_t jsonLen = 0; std::memcpy (&jsonLen, b + 9, 8);
    if (17 + jsonLen + 8 > size) { r.error = "the preset file is damaged"; return r; }
    auto meta = JSON::parse (String::fromUTF8 ((const char*) b + 17, (int) jsonLen));
    size_t o = 17 + (size_t) jsonLen;
    uint32_t cborSize = 0, fmt = 0;
    std::memcpy (&cborSize, b + o, 4); std::memcpy (&fmt, b + o + 4, 4); o += 8;
    if (cborSize == 0 || cborSize > 1024u * 1024u * 1024u) { r.error = "the preset file is damaged"; return r; }

    std::vector<uint8_t> cbor (cborSize);
    const size_t got = ZSTD_decompress (cbor.data(), cbor.size(), b + o, size - o);
    if (ZSTD_isError (got) || got != cborSize) { r.error = "could not unpack the preset data"; return r; }
    Cbor reader { cbor.data(), cbor.data() + cbor.size() };
    const var tree = reader.parse (0);
    if (reader.bad || ! tree.isObject()) { r.error = "could not read the preset data"; return r; }

    r.name = meta.getProperty ("presetName", file.getFileNameWithoutExtension()).toString();
    r.author = meta.getProperty ("presetAuthor", "").toString();
    auto& v = r.values;

    // ---------------- macros (baked in at their saved positions)
    float macro[8] {};
    StringArray macroNames;
    for (int i = 0; i < 8; ++i)
    {
        auto m = child (tree, "Macro" + String (i));
        macro[i] = (float) num (plain (m), "kParamValue", 0.0) / 100.0f;
        auto nm = child (m, "name");
        if (nm.isString() && nm.toString().isNotEmpty()) macroNames.add (nm.toString());
    }
    struct Mod { int src; String type; int id; String param; float amt; };
    std::vector<Mod> mods;
    for (int i = 0; i < 64; ++i)
    {
        auto s = child (tree, "ModSlot" + String (i));
        if (! has (s, "destModuleTypeString")) continue;
        auto src = child (s, "source");
        mods.push_back ({ src.isArray() && src.size() > 0 ? (int) src[0] : -1, str (s, "destModuleTypeString"),
                          (int) num (s, "destModuleID", -1), str (s, "destModuleParamName"), (float) num (plain (s), "kParamAmount", 0.0) / 100.0f });
    }
    std::vector<bool> used (mods.size(), false);
    auto bake = [&] (const String& type, int id, const String& param) -> float
    {
        float sum = 0;
        for (size_t k = 0; k < mods.size(); ++k)
            if (mods[k].src >= 25 && mods[k].src <= 32 && mods[k].type == type && (id < 0 || mods[k].id == id) && mods[k].param == param)
            { sum += macro[mods[k].src - 25] * mods[k].amt; used[k] = true; }
        return sum;
    };
    auto findEnvMod = [&] (const String& type, const String& param, int& envIndex) -> float
    {
        for (size_t k = 0; k < mods.size(); ++k)
            if (mods[k].src >= 3 && mods[k].src <= 5 && mods[k].type == type && mods[k].param == param)
            { envIndex = mods[k].src - 2; used[k] = true; return mods[k].amt; }
        return 0.0f;
    };

    // ---------------- oscillators
    struct Slot { int type = us::tSaw, table = 2, uni = 1, oct = 0, semi = 0; float fine = 0, lvl = 0.75f, pos = 0.4f, det = 0.25f; bool key = true; std::shared_ptr<us::SampleData> sample; int serum = 0; };
    std::vector<Slot> slots;
    const char* letters[5] { "A", "B", "C", "NOISE", "SUB" };
    for (int i = 0; i < 5; ++i)
    {
        auto osc = child (tree, "Oscillator" + String (i));
        if (! osc.isObject()) continue;
        auto pp = plain (osc);
        const bool enabled = has (pp, "kParamEnable") ? num (pp, "kParamEnable", 0) > 0.5 : (i == 0);
        if (! enabled) continue;
        Slot s; s.serum = i;
        s.lvl = jlimit (0.0f, 1.0f, (float) num (pp, "kParamVolume", 0.75) + bake ("Oscillator", i, "kParamVolume"));
        s.oct = (int) num (pp, "kParamOctave", i == 4 ? -1 : 0);
        s.semi = (int) num (pp, "kParamCoarsePit", 0);
        s.fine = (float) num (pp, "kParamFine", 0);
        s.uni = jlimit (1, us::maxUnison, (int) num (pp, "kParamUnison", 1));
        const String label = String ("OSC ") + letters[i];

        if (i == 3)
        {
            auto n = child (osc, "NoiseOsc3");
            if (auto d = audioFrom (child (n, "embeddedNoiseData"), num (n, "sampleRate", 44100), shortName (str (n, "pathToNoiseSample", "recording"))))
            {
                s.type = us::tSample; s.key = false; s.sample = d; s.pos = 0;
                r.applied.add ("noise layer: the embedded recording \"" + d->name + "\"");
            }
            else
            {
                s.type = us::tNoise; s.pos = 0.6f;
                r.close.add ("noise \"" + shortName (str (n, "relativePathToNoiseSample", "noise")) + "\" -> plain noise (Serum's own file)");
            }
        }
        else if (i == 4)
        {
            s.type = us::tSine;
            r.applied.add ("sub oscillator (sine, one octave down)");
        }
        else
        {
            const String kind = str (pp, "kParamType", "kOsc_WT");
            if (kind == "kOsc_WT" || kind.isEmpty())
            {
                auto wt = child (osc, "WTOsc" + String (i));
                const String table = shortName (str (wt, "relativePathToWT", "Default Shapes"));
                const float tpos = (float) jlimit (0.0, 1.0, num (plain (wt), "kParamTablePos", 0.0) / 256.0);
                const String low = table.toLowerCase();
                String to;
                if (low.contains ("shape")) { s.type = us::tWave; s.table = 0; s.pos = tpos; to = "SHAPES wavetable"; }
                else if (low.contains ("jno") || low.contains ("juno") || low.contains ("saw")) { s.type = us::tSaw; to = "JUNO SAW"; }
                else if (low.contains ("mini") || low.contains ("analog") || low.contains ("moog")) { s.type = us::tWave; s.table = 1; s.pos = tpos; to = "ANALOG wavetable"; }
                else if (low.contains ("square") || low.contains ("pulse")) { s.type = us::tPulse; to = "JUNO PULSE"; }
                else if (low.contains ("sine")) { s.type = us::tSine; to = "SINE"; }
                else if (low.contains ("tri")) { s.type = us::tTri; to = "TRIANGLE"; }
                else { s.type = us::tWave; s.table = 2; s.pos = tpos; to = "DIGITAL wavetable"; }
                r.close.add (label + " \"" + table + "\" -> " + to);
            }
            else if (kind == "kOsc_Granular")
            {
                auto g = child (osc, "GranularOsc" + String (i));
                auto ea = child (g, "embedded_audio");
                if (auto d = audioFrom (child (ea, "data"), num (ea, "sampleRate", num (g, "sampleRate", 44100)), child (ea, "table_name").toString()))
                {
                    s.type = us::tSample; s.key = true; s.sample = d;
                    s.pos = (float) jlimit (0.0, 1.0, num (plain (g), "kParamPosition", 0.0) / 100.0);
                    r.close.add (label + " granular -> plays the embedded recording \"" + d->name + "\" as a loop");
                }
                else { s.type = us::tWave; r.missing.add (label + " granular sound (Serum's own file)"); }
            }
            else if (kind == "kOsc_Sample" || kind == "kOsc_MultiSample")
            {
                auto so = child (osc, (kind == "kOsc_Sample" ? "SampleOsc" : "MultiSampleOsc") + String (i));
                auto ea = child (so, "embedded_audio");
                if (auto d = audioFrom (child (ea, "data"), num (ea, "sampleRate", 44100), shortName (str (so, "samplePath", "sample"))))
                {
                    s.type = us::tSample; s.key = true; s.sample = d;
                    r.applied.add (label + " sample: the embedded recording \"" + d->name + "\"");
                }
                else
                {
                    s.type = us::tFm; s.pos = 0.35f;
                    const String nm = shortName (str (so, "samplePathRelative", kind == "kOsc_Sample" ? "sample" : "multisample"));
                    r.missing.add (label + " " + (kind == "kOsc_Sample" ? "sample \"" + nm + "\"" : String ("multisample")) + " (Serum's own file) -> FM used instead");
                }
            }
            else { s.type = us::tWave; s.table = 2; r.close.add (label + " spectral -> DIGITAL wavetable"); }
            if (s.uni > 1) r.applied.add (label + " unison: " + String (s.uni) + " voices");
        }
        slots.push_back (s);
    }
    while (slots.size() > (size_t) us::numOscs)
    {
        auto it = std::min_element (slots.begin(), slots.end(), [] (const Slot& a, const Slot& b2) { return a.lvl < b2.lvl; });
        const String lab = String ("OSC ") + letters[it->serum];
        for (auto* col : { &r.applied, &r.close })
            for (int k = col->size(); --k >= 0;)
                if ((*col)[k].startsWith (lab + " ")) col->remove (k);
        r.missing.add (lab + " (only 4 oscillators here, the quietest one was left out)");
        slots.erase (it);
    }
    if (slots.empty()) { Slot s; s.lvl = 0.0f; slots.push_back (s); }
    v["oscCount"] = (float) slots.size();
    for (int i = 0; i < (int) slots.size(); ++i)
    {
        auto& s = slots[(size_t) i];
        v[us::oscId (i, "Type")] = (float) s.type; v[us::oscId (i, "Table")] = (float) s.table;
        v[us::oscId (i, "Lvl")] = s.lvl; v[us::oscId (i, "Oct")] = (float) jlimit (-3, 3, s.oct);
        v[us::oscId (i, "Semi")] = (float) jlimit (-12, 12, s.semi); v[us::oscId (i, "Fine")] = jlimit (-100.0f, 100.0f, s.fine);
        v[us::oscId (i, "Uni")] = (float) s.uni; v[us::oscId (i, "Det")] = s.det; v[us::oscId (i, "Pos")] = s.pos;
        v[us::oscId (i, "Key")] = s.key ? 1.0f : 0.0f;
        r.samples[i] = s.sample;
    }
    auto slotOf = [&] (int serumOsc) { for (int i = 0; i < (int) slots.size(); ++i) if (slots[(size_t) i].serum == serumOsc) return i; return -1; };

    // ---------------- amp envelope
    auto envVals = [&] (int idx, float& a, float& d, float& s, float& rl)
    {
        auto pp = plain (child (tree, "Env" + String (idx)));
        a = jmax (0.0005f, (float) num (pp, "kParamAttack", 0.0005));
        d = jmax (0.001f, (float) num (pp, "kParamDecay", 1.0));
        s = jlimit (0.0f, 1.0f, (float) num (pp, "kParamSustain", 1.0));
        rl = jmax (0.001f, (float) num (pp, "kParamRelease", 0.015));
    };
    { float a, d, s, rl; envVals (0, a, d, s, rl); v["aA"] = a; v["aD"] = d; v["aS"] = s; v["aR"] = rl; r.applied.add ("amp envelope"); }

    // ---------------- filter
    {
        auto pp = plain (child (tree, "VoiceFilter0"));
        if (num (pp, "kParamEnable", 0) > 0.5)
        {
            const String type = str (pp, "kParamType", "MgL12");
            int ft = 0;
            if (type.contains ("L24") || type.contains ("Ldr")) ft = 0;
            else if (type.contains ("L12") || type.contains ("L6") || type.contains ("L18")) ft = 1;
            else if (type.contains ("H")) ft = 2;
            else if (type.contains ("B")) ft = 3;
            else r.missing.add ("filter type \"" + type + "\" -> LP24");
            const float f = jlimit (0.0f, 1.0f, (float) num (pp, "kParamFreq", 0.5) + bake ("VoiceFilter", 0, "kParamFreq"));
            v["fType"] = (float) ft;
            v["cutoff"] = jlimit (20.0f, 18000.0f, 8.0f * std::pow (2756.25f, f));
            v["reso"] = jlimit (0.0f, 1.0f, (float) num (pp, "kParamReso", 0) / 100.0f * 0.9f);
            v["fDrive"] = jlimit (0.0f, 1.0f, (float) num (pp, "kParamDrive", 0) / 100.0f);
            const double kt = num (pp, "kParamKeyTrack", 0);
            v["fKey"] = (float) jlimit (0.0, 1.0, kt > 1.0 ? kt / 100.0 : kt);
            r.applied.add ("filter: " + us::filterTypes[ft] + ", cutoff, resonance, drive, key track");
            int env = 0;
            const float amt = findEnvMod ("VoiceFilter", "kParamFreq", env);
            if (env > 0 && amt > 0)
            {
                float a, d, s, rl; envVals (env, a, d, s, rl);
                v["fA"] = a; v["fD"] = d; v["fS"] = s; v["fR"] = rl;
                v["fAmt"] = jlimit (0.0f, 1.0f, amt * 11.4f / 6.0f);
                r.applied.add ("filter envelope (ENV " + String (env + 1) + ")");
            }
            else v["fAmt"] = 0.0f;
        }
        else { v["fType"] = 0; v["cutoff"] = 18000.0f; v["reso"] = 0.0f; v["fAmt"] = 0.0f; v["fKey"] = 0.0f; }
    }

    // ---------------- pitch envelope
    {
        int env = 0;
        const float amt = findEnvMod ("Oscillator", "kParamCoarsePit", env);
        if (env > 0 && std::abs (amt) > 0.001f)
        {
            float a, d, s, rl; envVals (env, a, d, s, rl);
            v["pAmt"] = jlimit (-24.0f, 24.0f, amt * 48.0f);
            v["pD"] = d;
            r.close.add ("pitch envelope applied to all oscillators");
        }
    }

    // ---------------- LFOs
    {
        int next = 0;
        std::map<int, int> lfoSlot;
        for (size_t k = 0; k < mods.size() && next < 3; ++k)
        {
            auto& m = mods[k];
            if (m.src < 6 || m.src > 15 || used[k]) continue;
            int dest = us::dNone; float depth = 0;
            if (m.type == "Oscillator" && (m.param == "kParamCoarsePit" || m.param == "kParamFine"))
            {
                const int s = slotOf (m.id);
                dest = s >= 0 ? us::dOsc1 + s : us::dPitch;
                const float semis = std::abs (m.amt) * (m.param == "kParamCoarsePit" ? 48.0f : 1.0f);
                depth = std::sqrt (jlimit (0.0f, 12.0f, semis) / 12.0f);
            }
            else if (m.type == "VoiceFilter" && m.param == "kParamFreq") { dest = us::dCutoff; depth = jlimit (0.0f, 1.0f, std::abs (m.amt) * 11.4f / 4.0f); }
            else if (m.type == "VoiceFilter" && m.param == "kParamReso") { dest = us::dReso; depth = jlimit (0.0f, 1.0f, std::abs (m.amt) * 2.0f); }
            else if (m.type == "Oscillator" && m.param == "kParamPan") { dest = us::dPan; depth = jlimit (0.0f, 1.0f, std::abs (m.amt)); }
            else if (m.type == "FXReverb" && m.param == "kParamWet") { dest = us::dRev; depth = jlimit (0.0f, 1.0f, std::abs (m.amt)); }
            if (dest == us::dNone) continue;
            used[k] = true;
            int l;
            if (lfoSlot.count (m.src)) l = lfoSlot[m.src];
            else
            {
                l = next++; lfoSlot[m.src] = l;
                auto pp = plain (child (tree, "LFO" + String (m.src - 6)));
                const String t = str (pp, "kParamType", "").toLowerCase();
                v[us::lfoId (l, "Shape")] = t.contains ("random") ? 4.0f : t.contains ("sine") ? 0.0f : t.contains ("saw") ? 3.0f : t.contains ("square") ? 2.0f : 1.0f;
                if (has (pp, "kParamRate") || str (pp, "kParamMode") == "Free")
                { v[us::lfoId (l, "Sync")] = 0.0f; v[us::lfoId (l, "Hz")] = jlimit (0.05f, 20.0f, (float) num (pp, "kParamRate", 1.0)); }
                else { v[us::lfoId (l, "Sync")] = 1.0f; v[us::lfoId (l, "Div")] = 4.0f; }
                v[us::lfoId (l, "Depth")] = depth;
                v[us::lfoId (l, "D1")] = (float) dest;
                v[us::lfoId (l, "D2")] = (float) us::dNone;
                continue;
            }
            v[us::lfoId (l, "D2")] = (float) dest;
            v[us::lfoId (l, "Depth")] = jmax (v[us::lfoId (l, "Depth")], depth);
        }
        for (int l = next; l < 3; ++l) { v[us::lfoId (l, "Depth")] = 0.0f; v[us::lfoId (l, "D1")] = (float) us::dNone; v[us::lfoId (l, "D2")] = (float) us::dNone; }
        if (next > 0) r.close.add (String (next) + " LFO route" + (next > 1 ? "s" : "") + " (custom LFO shapes become simple ones)");
    }

    // ---------------- global
    {
        auto pp = plain (child (tree, "Global0"));
        if (num (pp, "kParamMonoToggle", 0) > 0.5) { v["voice"] = 1.0f; r.applied.add ("mono mode"); }
        v["aLvl"] = jlimit (0.3f, 1.0f, 0.8f * (float) num (pp, "kParamMasterVolume", 0.7) / 0.7f);
        v["drift"] = 0.05f; v["digital"] = 0.0f; v["space"] = 0.0f; v["width"] = 0.5f; v["arpMode"] = 0.0f;
    }

    // ---------------- effects (main rack)
    for (auto* id : { "distOn", "choOn", "flaOn", "dlyOn", "revOn", "eqOn", "cmpOn", "stOn" }) v[id] = 0.0f;
    auto rack = child (child (tree, "FXRack0"), "FX");
    bool choSet = false;
    for (int k = 0; rack.isArray() && k < rack.size(); ++k)
    {
        auto entry = rack[k];
        auto* obj = entry.getDynamicObject();
        if (obj == nullptr) continue;
        String fx; var mod;
        for (auto& prop : obj->getProperties())
            if (prop.name.toString().startsWith ("FX")) { fx = prop.name.toString(); mod = prop.value; }
        if (fx.isEmpty()) continue;
        auto pp = plain (mod);
        auto wet = [&] (const char* param, double def) { return jlimit (0.0f, 1.0f, (float) num (pp, param, def) / 100.0f + bake (fx, k, param)); };
        if (fx == "FXChorus")
        {
            const float w = wet ("kParamWet", 50);
            if (w > 0.005f) { v["choOn"] = 1; v["choMix"] = w; v["choRate"] = jlimit (0.05f, 5.0f, (float) num (pp, "kParamRate", 0.8)); v["choDepth"] = jlimit (0.0f, 1.0f, (float) num (pp, "kParamDepth", 2.0) / 4.0f); choSet = true; r.applied.add ("chorus"); }
        }
        else if (fx == "FXReverb")
        {
            const float w = wet ("kParamWet", 30);
            if (w > 0.005f)
            {
                const String t = str (pp, "kParamType");
                v["revOn"] = 1; v["revMix"] = w * 0.8f;
                v["revSize"] = t == "kAbyss" ? 0.95f : jlimit (0.0f, 1.0f, (float) num (pp, "kParamSize", 50) / 100.0f * 0.8f + 0.2f);
                v["revDamp"] = 0.5f;
                r.applied.add ("reverb");
            }
        }
        else if (fx == "FXDelay")
        {
            const float w = wet ("kParamWet", 25);
            if (w > 0.005f) { v["dlyOn"] = 1; v["dlyMix"] = w * 0.8f; v["dlyFb"] = jlimit (0.0f, 0.95f, (float) num (pp, "kParamFeedback", 40) / 100.0f); v["dlyTime"] = 3; r.close.add ("delay (time set to 1/8)"); }
        }
        else if (fx == "FXDistortion")
        {
            const float w = wet ("kParamWet", 100);
            if (w > 0.005f) { v["distOn"] = 1; v["distMix"] = w; v["distDrive"] = jlimit (0.0f, 1.0f, (float) num (pp, "kParamDrive", 25) / 100.0f + bake (fx, k, "kParamDrive")); v["distTone"] = 0.5f; r.close.add ("distortion (" + str (pp, "kParamMode", "tube").fromFirstOccurrenceOf ("k", false, false).toLowerCase() + " -> tape style)"); }
        }
        else if (fx == "FXFlanger")
        {
            const float w = wet ("kParamWet", 50);
            if (w > 0.005f) { v["flaOn"] = 1; v["flaMix"] = w; v["flaRate"] = jlimit (0.02f, 4.0f, (float) num (pp, "kParamRate", 0.25)); v["flaFb"] = jlimit (0.0f, 1.0f, (float) num (pp, "kParamFeedback", 50) / 100.0f); r.applied.add ("flanger"); }
        }
        else if (fx == "FXComp")
        {
            const float w = wet ("kParamWet", 100);
            const float thr = (float) num (pp, "kParamThresh", 0.3);
            if (w > 0.005f) { v["cmpOn"] = 1; v["cmpMix"] = w; v["cmpAmt"] = jlimit (0.0f, 1.0f, -Decibels::gainToDecibels (jmax (thr, 0.001f)) / 30.0f); v["cmpSpeed"] = 0.6f; if (! r.applied.contains ("compressor")) r.applied.add ("compressor"); }
        }
        else if (fx == "FXEQ")
        {
            v["eqOn"] = 1;
            for (int band = 1; band <= 2; ++band)
            {
                const String gk = "kParamGain" + String (band), fk = "kParamFreq" + String (band);
                const float g = (float) num (pp, gk.toRawUTF8(), 0), f = (float) num (pp, fk.toRawUTF8(), band == 1 ? 200 : 4000);
                v[f < 500 ? "eqLow" : (f > 2000 ? "eqHigh" : "eqMid")] = jlimit (-12.0f, 12.0f, g);
            }
            r.close.add ("EQ (moved to the nearest of low / mid / high)");
        }
        else if (fx == "FXUtils")
        {
            v["stOn"] = 1;
            v["stWidth"] = jlimit (0.0f, 1.0f, (float) num (pp, "kParamWidth", 100) / 200.0f);
            v["stMono"] = num (pp, "kParamLFMono", 0) > 0.5 ? jlimit (20.0f, 400.0f, (float) num (pp, "kParamLFXover", 120)) : 20.0f;
            v["stLpf"] = jlimit (1000.0f, 20000.0f, (float) num (pp, "kParamLPF", 20000));
            r.applied.add ("stereo width / mono lows / top cut");
        }
        else if (fx == "FXHyperD")
        {
            const float w = jmax (wet ("kParamWet", 0), wet ("kParamDimEWet", 0));
            if (w > 0.005f && ! choSet) { v["choOn"] = 1; v["choMix"] = w * 0.8f; v["choDepth"] = 0.7f; v["choRate"] = 0.4f; choSet = true; }
            if (w > 0.005f) r.close.add ("hyper / dimension -> chorus");
        }
        else
        {
            const float w = wet ("kParamWet", 0);
            if (w > 0.005f || fx != "FXBode")
                r.missing.add (fx.fromFirstOccurrenceOf ("FX", false, false).toLowerCase() + " effect");
        }
    }
    for (int bus = 1; bus <= 2; ++bus)
    {
        auto fxs = child (child (tree, "FXRack" + String (bus)), "FX");
        if (fxs.isArray() && fxs.size() > 0) { r.missing.add ("FX bus " + String (bus) + " (parallel effects)"); }
    }
    int unmapped = 0;
    for (size_t k = 0; k < mods.size(); ++k) if (! used[k] && mods[k].src < 25) ++unmapped;
    if (unmapped > 0) r.missing.add (String (unmapped) + " other modulation route" + (unmapped > 1 ? "s" : ""));
    if (macroNames.size() > 0) r.close.add ("macro positions baked in (" + macroNames.joinIntoString (", ").toLowerCase() + ")");

    r.info = (r.author.isNotEmpty() ? "by " + r.author + " · " : String()) + String ((double) size / (1024.0 * 1024.0), 1) + " MB";
    r.ok = true;
    return r;
}
