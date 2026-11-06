#pragma once
// Sound engine. Design notes (papers the blocks follow):
//  - Basic oscillators: PolyBLEP band-limited saw / pulse (Välimäki & Huovilainen, IEEE SPM 2007).
//  - WAVETABLE: frames of summed harmonics scanned by POS, mip-mapped per octave
//    ("multiple wavetable synthesis", Horner, Beauchamp & Haken, JAES 1993). All tables are generated here.
//  - FM: 2-operator FM, carrier:modulator 1:2 (Chowning, JAES 1973).
//  - FORMANT: pulse source through two vowel formant band-passes (source–filter idea, cf. Rodet's FOF, CMJ 1984).
//  - Filter: 4-pole ladder solved with zero-delay feedback, tanh on the input (Huovilainen, DAFx 2004;
//    Zavalishin, "The Art of VA Filter Design"); 2-pole TPT state-variable filter for LP12 / HP / BP.
//  - Chorus: Juno-style two-tap modulated delay with a dark wet path (BBD behaviour, Raffel & Smith, DAFx 2010).
#include <cmath>
#include <vector>
#include <memory>
#include <algorithm>
#include <cstdint>
#include <juce_audio_basics/juce_audio_basics.h>
#include "Params.h"

namespace us
{
constexpr float kTwoPi = 6.283185307179586f;
constexpr float kPi    = 3.141592653589793f;
constexpr int maxUnison = 7;

struct Rng
{
    uint32_t s = 0x9E3779B9u;
    float next() { s = s * 1664525u + 1013904223u; return (float) (s >> 8) * (1.0f / 16777216.0f); }
    float bip()  { return next() * 2.0f - 1.0f; }
};

inline float polyBlep (float t, float dt)
{
    if (t < dt)        { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}

inline float fastTanh (float x)
{
    x = std::clamp (x, -3.0f, 3.0f);
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// ---------------------------------------------------------------- generated wavetables (3 tables)
struct WaveBank
{
    static constexpr int N = 2048, F = 16, M = 9, T = 3;
    std::vector<float> data;

    static void harmonics (int table, float pos, int h, float& a)
    {
        const float pi = kPi;
        if (table == 0) // SHAPES: sine -> triangle -> saw -> square
        {
            auto shape = [&] (int s) -> float
            {
                switch (s)
                {
                    case 0: return h == 1 ? 1.0f : 0.0f;
                    case 1: return (h % 2) ? (8.0f / (pi * pi)) * ((((h - 1) / 2) % 2) ? -1.0f : 1.0f) / (float) (h * h) : 0.0f;
                    case 2: return (2.0f / pi) * ((h % 2) ? 1.0f : -1.0f) / (float) h;
                    default: return (h % 2) ? (4.0f / pi) / (float) h : 0.0f;
                }
            };
            const float p = std::clamp (pos, 0.0f, 1.0f) * 3.0f;
            const int s0 = std::min ((int) p, 2);
            const float f = p - (float) s0;
            a = shape (s0) * (1.0f - f) + shape (s0 + 1) * f;
        }
        else if (table == 1) // ANALOG: soft saw morphing to a narrow pulse
        {
            const float saw = (2.0f / pi) / (float) h;
            const float pw = 0.5f - 0.42f * pos;
            const float pulse = (2.0f / (pi * (float) h)) * std::sin (pi * (float) h * pw) * 2.0f;
            a = (saw * (1.0f - pos) + pulse * pos) * std::exp (-(float) h / 60.0f);
        }
        else // DIGITAL: moving formant peak
        {
            const float c = 1.0f + pos * 14.0f, w = 1.5f + pos * 3.0f;
            a = std::pow ((float) h, -1.1f) * (1.0f + 2.5f * std::exp (-((float) h - c) * ((float) h - c) / (2.0f * w * w)));
            if (h % 2 == 0) a *= (1.0f - 0.6f * pos);
        }
    }

    void build()
    {
        if (! data.empty()) return;
        data.assign ((size_t) T * F * M * N, 0.0f);
        std::vector<float> sinTab ((size_t) N);
        for (int i = 0; i < N; ++i) sinTab[(size_t) i] = std::sin (kTwoPi * (float) i / (float) N);
        for (int tb = 0; tb < T; ++tb)
            for (int f = 0; f < F; ++f)
            {
                const float pos = (float) f / (float) (F - 1);
                for (int m = 0; m < M; ++m)
                {
                    const int maxH = std::max (1, 128 >> m);
                    float* t = table (tb, f, m);
                    for (int h = 1; h <= maxH; ++h)
                    {
                        float a = 0; harmonics (tb, pos, h, a);
                        if (std::abs (a) < 1.0e-6f) continue;
                        for (int i = 0; i < N; ++i) t[i] += a * sinTab[(size_t) ((h * i) & (N - 1))];
                    }
                    float peak = 1.0e-6f;
                    for (int i = 0; i < N; ++i) peak = std::max (peak, std::abs (t[i]));
                    for (int i = 0; i < N; ++i) t[i] /= peak;
                }
            }
    }
    float* table (int tb, int f, int m) { return data.data() + (((size_t) tb * F + (size_t) f) * M + (size_t) m) * N; }
    const float* table (int tb, int f, int m) const { return data.data() + (((size_t) tb * F + (size_t) f) * M + (size_t) m) * N; }

    float read (int tb, float phase, float pos, float freq, float sr) const
    {
        const float harmonicsAllowed = 0.5f * sr / std::max (freq, 1.0f);
        int m = 0;
        while (m < M - 1 && (float) (128 >> m) > harmonicsAllowed) ++m;
        const float fp = std::clamp (pos, 0.0f, 1.0f) * (F - 1);
        const int f0 = std::min ((int) fp, F - 2);
        const float ff = fp - (float) f0;
        const float x = phase * (float) N;
        const int i0 = (int) x & (N - 1), i1 = (i0 + 1) & (N - 1);
        const float fr = x - std::floor (x);
        const float* a = table (tb, f0, m);
        const float* b = table (tb, f0 + 1, m);
        const float va = a[i0] + (a[i1] - a[i0]) * fr;
        const float vb = b[i0] + (b[i1] - b[i0]) * fr;
        return va + (vb - va) * ff;
    }
};

// audio clip used by a SAMPLE oscillator
struct SampleData
{
    juce::AudioBuffer<float> buffer;
    double sampleRate = 44100.0;
    juce::String name;
};

// ---------------------------------------------------------------- TPT state-variable filter
struct Svf
{
    float ic1 = 0, ic2 = 0, a1 = 0, a2 = 0, a3 = 0, k = 1;
    float lp = 0, bp = 0, hp = 0;
    void set (float fc, float q, float sr)
    {
        const float g = std::tan (kPi * std::clamp (fc, 20.0f, 0.45f * sr) / sr);
        k = 1.0f / q;
        a1 = 1.0f / (1.0f + g * (g + k)); a2 = g * a1; a3 = g * a2;
    }
    float process (float x)
    {
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1; ic2 = 2.0f * v2 - ic2;
        bp = v1; lp = v2; hp = x - k * v1 - v2;
        return bp;
    }
    void reset() { ic1 = ic2 = 0; }
};

// ---------------------------------------------------------------- 4-pole ladder (ZDF, tanh input)
struct Ladder
{
    float s[4] { 0, 0, 0, 0 };
    float G = 0.1f, g = 0.1f, k = 0;
    void set (float fc, float res, float sr)
    {
        g = std::tan (kPi * std::clamp (fc, 20.0f, 0.46f * sr) / sr);
        G = g / (1.0f + g);
        k = 3.9f * std::clamp (res, 0.0f, 1.0f);
    }
    float process (float x)
    {
        const float G2 = G * G, G3 = G2 * G, G4 = G3 * G, om = 1.0f - G;
        const float sigma = G3 * om * s[0] + G2 * om * s[1] + G * om * s[2] + om * s[3];
        float u = (x * (1.0f + 0.5f * k) - k * sigma) / (1.0f + k * G4);
        u = fastTanh (u);
        float y = u;
        for (int i = 0; i < 4; ++i)
        {
            const float v = (y - s[i]) * G;
            const float o = v + s[i];
            s[i] = o + v;
            y = o;
        }
        return y;
    }
    void reset() { for (auto& v : s) v = 0; }
};

// ---------------------------------------------------------------- envelope
struct Env
{
    enum Stage { idle, attack, decay, release };
    Stage stage = idle;
    float v = 0, aInc = 1, dCoef = 0.99f, rCoef = 0.99f, sus = 1;
    void set (float a, float d, float s, float r, float sr)
    {
        aInc = 1.0f / std::max (1.0f, a * sr);
        dCoef = std::exp (-1.0f / std::max (1.0f, d * sr * 0.3f));
        rCoef = std::exp (-1.0f / std::max (1.0f, r * sr * 0.3f));
        sus = s;
    }
    void on (bool retrigger) { if (retrigger || stage == idle || stage == release) stage = attack; }
    void off() { if (stage != idle) stage = release; }
    float next()
    {
        switch (stage)
        {
            case attack:  v += aInc; if (v >= 1.0f) { v = 1.0f; stage = decay; } break;
            case decay:   v = sus + (v - sus) * dCoef; break;
            case release: v *= rCoef; if (v < 1.0e-5f) { v = 0; stage = idle; } break;
            case idle:    break;
        }
        return v;
    }
};

// ---------------------------------------------------------------- per-block settings shared by voices
struct OscSettings
{
    bool on = false;
    int type = tSaw, table = 2, unison = 1;
    float level = 0, semis = 0, detune = 0.25f, pos = 0.4f;
    bool keyTrack = true;
    std::shared_ptr<SampleData> sample;
};

struct VoiceSettings
{
    OscSettings osc[numOscs];
    int filterType = 0;
    float cutoff = 1400, reso = 0.15f, fAmt = 0.45f, drive = 0, keyTrack = 0.5f;
    float pitchAmt = 0, pitchDecayCoef = 0.999f;
    float drift = 0.3f, width = 0.35f;
    float sr = 44100;
};

// per-sample modulation from the LFOs (filled by the processor)
struct ModFrame
{
    float pitch[numOscs] { 0, 0, 0, 0 };   // semitones per oscillator
    float cutoffOct = 0, reso = 0, pan = 0, width = 0;
};

struct Voice
{
    int note = -1;
    bool active = false;
    float vel = 1.0f, panSeed = 0.0f, unisonCents = 0.0f, staticCents = 0.0f;
    uint32_t age = 0;
    float ph[numOscs][maxUnison] {}, phm[numOscs][maxUnison] {};
    float inc[numOscs][maxUnison] {}, freq[numOscs] {};
    double samplePos[numOscs] {};
    float noiseLp[numOscs] {};
    float driftNow = 0, driftTarget = 0; int driftCount = 0;
    float cutSeed = 0, pitchEnv = 0;
    Env amp, fenv;
    Ladder flt, fltR;
    Svf svf, svfR, form1[numOscs], form2[numOscs];
    Rng rng;

    void start (int n, float velocity, float unison, float panPos, bool retrigger, uint32_t stamp, float drift, const VoiceSettings& vs)
    {
        if (! active || retrigger)
        {
            for (int o = 0; o < numOscs; ++o)
            {
                for (int u = 0; u < maxUnison; ++u) { ph[o][u] = rng.next(); phm[o][u] = 0; }
                const auto& os = vs.osc[o];
                if (os.sample && os.sample->buffer.getNumSamples() > 0)
                    samplePos[o] = (double) os.pos * (double) (os.sample->buffer.getNumSamples() - 1);
            }
            pitchEnv = 1.0f;
        }
        note = n; vel = velocity; unisonCents = unison; panSeed = panPos; age = stamp;
        staticCents = rng.bip() * 4.0f * drift;
        cutSeed = rng.bip();
        active = true;
        amp.on (retrigger); fenv.on (retrigger);
        if (retrigger) { flt.reset(); fltR.reset(); svf.reset(); svfR.reset(); for (auto& f : form1) f.reset(); for (auto& f : form2) f.reset(); }
    }
    void stop() { amp.off(); fenv.off(); }

    static float basic (int type, float& ph, float inc)
    {
        float y = 0;
        switch (type)
        {
            case tSine: y = std::sin (kTwoPi * ph); break;
            case tTri:  y = 1.0f - 4.0f * std::abs (ph - 0.5f); break;
            case tSaw:  y = 2.0f * ph - 1.0f - polyBlep (ph, inc); break;
            default:
            {
                float p2 = ph + 0.5f; if (p2 >= 1.0f) p2 -= 1.0f;
                y = (ph < 0.5f ? 1.0f : -1.0f) + polyBlep (ph, inc) - polyBlep (p2, inc);
                break;
            }
        }
        ph += inc; if (ph >= 1.0f) ph -= 1.0f;
        return y;
    }

    void render (float* L, float* R, int n, const VoiceSettings& vs, const ModFrame* mods, const WaveBank& bank)
    {
        static const float vowels[5][2] { { 730, 1090 }, { 530, 1840 }, { 270, 2290 }, { 570, 840 }, { 300, 870 } };
        const float sr = vs.sr;
        const float width = std::clamp (vs.width, 0.0f, 1.0f);
        for (int i = 0; i < n; ++i)
        {
            const ModFrame& m = mods[i];
            if (--driftCount <= 0)
            {
                driftCount = 2048 + (int) (rng.next() * 2048.0f);
                driftTarget = rng.bip() * vs.drift * 14.0f;
            }
            driftNow += (driftTarget - driftNow) * 0.0004f;
            pitchEnv *= vs.pitchDecayCoef;

            if ((i & 7) == 0)
            {
                const float base = (float) note - 69.0f + (unisonCents + staticCents + driftNow) * 0.01f + vs.pitchAmt * pitchEnv;
                for (int o = 0; o < numOscs; ++o)
                {
                    const auto& os = vs.osc[o];
                    if (! os.on) continue;
                    const float semi = base + os.semis + m.pitch[o];
                    freq[o] = 440.0f * std::exp2 (semi / 12.0f);
                    const int nu = os.unison;
                    for (int u = 0; u < nu; ++u)
                    {
                        const float spread = nu > 1 ? ((float) u / (float) (nu - 1) * 2.0f - 1.0f) : 0.0f;
                        const float cents = spread * os.detune * 50.0f;
                        inc[o][u] = std::min (freq[o] * std::exp2 (cents / 1200.0f) / sr, 0.45f);
                    }
                }
                const float fe = fenv.v;
                const float keyOct = ((float) note - 60.0f) / 12.0f * vs.keyTrack;
                const float oct = fe * vs.fAmt * 6.0f + m.cutoffOct + keyOct + cutSeed * vs.drift * 0.25f;
                const float fc = vs.cutoff * std::exp2 (oct);
                if (vs.filterType == 0) { flt.set (fc, vs.reso + m.reso, sr); fltR.set (fc, vs.reso + m.reso, sr); }
                else
                {
                    const float q = 0.5f + std::clamp (vs.reso + m.reso, 0.0f, 1.0f) * 9.5f;
                    svf.set (fc, q, sr); svfR.set (fc, q, sr);
                }
            }

            float sl = 0, sr2 = 0;
            for (int o = 0; o < numOscs; ++o)
            {
                const auto& os = vs.osc[o];
                if (! os.on || os.level <= 0.0001f) continue;
                if (os.type == tSample || os.type == tNoise)
                {
                    float a = 0, b = 0;
                    if (os.sample && os.sample->buffer.getNumSamples() > 1)
                    {
                        const auto& buf = os.sample->buffer;
                        const int len = buf.getNumSamples();
                        const double ratio = os.sample->sampleRate / sr * (os.keyTrack ? std::exp2 (((double) note - 60.0 + os.semis) / 12.0) : std::exp2 (os.semis / 12.0));
                        const int i0 = juce::jlimit (0, len - 1, (int) samplePos[o]);
                        const int i1 = (i0 + 1) % len;
                        const float fr = (float) (samplePos[o] - (double) i0);
                        const float* c0 = buf.getReadPointer (0);
                        const float* c1 = buf.getReadPointer (buf.getNumChannels() > 1 ? 1 : 0);
                        a = c0[i0] + (c0[i1] - c0[i0]) * fr;
                        b = c1[i0] + (c1[i1] - c1[i0]) * fr;
                        samplePos[o] += ratio;
                        if (samplePos[o] >= (double) len) samplePos[o] -= (double) len;
                    }
                    else
                    {
                        const float w = rng.bip();
                        noiseLp[o] += (w - noiseLp[o]) * (0.05f + 0.95f * os.pos);
                        a = b = noiseLp[o] * 0.8f;
                    }
                    sl += os.level * a; sr2 += os.level * b;
                    continue;
                }
                const int nu = os.unison;
                const float norm = 1.0f / std::sqrt ((float) nu);
                float ol = 0, orr = 0;
                for (int u = 0; u < nu; ++u)
                {
                    float y;
                    if (os.type <= tPulse) y = basic (os.type, ph[o][u], inc[o][u]);
                    else if (os.type == tWave)
                    {
                        y = bank.read (os.table, ph[o][u], os.pos, freq[o], sr);
                        ph[o][u] += inc[o][u]; if (ph[o][u] >= 1.0f) ph[o][u] -= 1.0f;
                    }
                    else if (os.type == tFm)
                    {
                        const float index = 0.2f + os.pos * 3.5f;
                        y = std::sin (kTwoPi * ph[o][u] + index * std::sin (kTwoPi * phm[o][u]));
                        ph[o][u] += inc[o][u]; if (ph[o][u] >= 1.0f) ph[o][u] -= 1.0f;
                        phm[o][u] += 2.0f * inc[o][u]; if (phm[o][u] >= 1.0f) phm[o][u] -= 1.0f;
                    }
                    else y = basic (tSaw, ph[o][u], inc[o][u]);
                    if (nu == 1) { ol += y; orr += y; }
                    else
                    {
                        const float spread = ((float) u / (float) (nu - 1) * 2.0f - 1.0f) * width;
                        ol += y * (1.0f - spread) * 0.7071f;
                        orr += y * (1.0f + spread) * 0.7071f;
                    }
                }
                if (os.type == tFormant)
                {
                    if ((i & 15) == 0)
                    {
                        const float vp = std::clamp (os.pos, 0.0f, 1.0f) * 4.0f;
                        const int v0 = std::min ((int) vp, 3);
                        const float vf = vp - (float) v0;
                        form1[o].set (vowels[v0][0] + (vowels[v0 + 1][0] - vowels[v0][0]) * vf, 7.0f, sr);
                        form2[o].set (vowels[v0][1] + (vowels[v0 + 1][1] - vowels[v0][1]) * vf, 9.0f, sr);
                    }
                    const float mono = 0.5f * (ol + orr);
                    ol = orr = 2.2f * (form1[o].process (mono) + 0.7f * form2[o].process (mono));
                }
                sl += os.level * norm * ol; sr2 += os.level * norm * orr;
            }

            fenv.next();
            const float a = amp.next();
            const float driveGain = 1.0f + vs.drive * 6.0f;
            auto filt = [&] (Ladder& lad, Svf& sv, float x) -> float
            {
                x *= 0.5f;
                if (vs.drive > 0.001f) x = fastTanh (x * driveGain) / std::sqrt (driveGain);
                if (vs.filterType == 0) return lad.process (x);
                sv.process (x);
                return vs.filterType == 1 ? sv.lp : (vs.filterType == 2 ? sv.hp : sv.bp * 1.5f);
            };
            const float yL = filt (flt, svf, sl) * a * vel;
            const float yR = filt (fltR, svfR, sr2) * a * vel;

            const float pan = std::clamp (panSeed * std::clamp (width + m.width, 0.0f, 1.0f) + m.pan, -1.0f, 1.0f);
            const float ang = (pan + 1.0f) * 0.25f * kPi;
            L[i] += yL * std::cos (ang) * 1.414f;
            R[i] += yR * std::sin (ang) * 1.414f;
        }
        if (amp.stage == Env::idle) { active = false; note = -1; }
    }
};

// ---------------------------------------------------------------- LFO
struct Lfo
{
    float phase = 0, prevPhase = 0, value = 0, held = 0, smoothState = 0;
    Rng rng;
    float shapeAt (int shape, float p) const
    {
        switch (shape)
        {
            case 0: return std::sin (kTwoPi * p);
            case 1: return 1.0f - 4.0f * std::abs (p - 0.5f);
            case 2: return p < 0.5f ? 1.0f : -1.0f;
            case 3: return 2.0f * p - 1.0f;
            default: return held;
        }
    }
    float step (float newPhase, int shape, float smoothCoef)
    {
        if (newPhase < prevPhase) held = rng.bip();
        prevPhase = newPhase;
        phase = newPhase;
        const float raw = shapeAt (shape, phase);
        smoothState += (raw - smoothState) * smoothCoef;
        value = smoothState;
        return value;
    }
};

// ---------------------------------------------------------------- effects
struct Crusher
{
    float holdL = 0, holdR = 0, count = 0;
    void process (float* L, float* R, int n, float amount)
    {
        if (amount < 0.005f) return;
        const float hold = 1.0f + amount * amount * 7.0f;
        const float levels = std::exp2 (16.0f - amount * 10.0f);
        for (int i = 0; i < n; ++i)
        {
            count += 1.0f;
            if (count >= hold)
            {
                count -= hold;
                holdL = std::round (L[i] * levels) / levels;
                holdR = std::round (R[i] * levels) / levels;
            }
            L[i] = holdL; R[i] = holdR;
        }
    }
};

struct Distortion
{
    float lpL = 0, lpR = 0;
    void process (float* L, float* R, int n, float drive, float tone, float mix, float sr)
    {
        const float g = 1.0f + drive * drive * 30.0f;
        const float fc = 800.0f * std::pow (25.0f, tone);
        const float c = 1.0f - std::exp (-kTwoPi * fc / sr);
        const float comp = 1.0f / (0.6f + 0.4f * fastTanh (g * 0.3f) * 1.6f);
        for (int i = 0; i < n; ++i)
        {
            float a = fastTanh (L[i] * g + 0.15f * drive) - fastTanh (0.15f * drive);
            float b = fastTanh (R[i] * g + 0.15f * drive) - fastTanh (0.15f * drive);
            lpL += (a - lpL) * c; lpR += (b - lpR) * c;
            L[i] = L[i] * (1.0f - mix) + lpL * comp * mix;
            R[i] = R[i] * (1.0f - mix) + lpR * comp * mix;
        }
    }
};

struct ModDelayLine
{
    std::vector<float> buf;
    int w = 0, mask = 0;
    void prepare (int sizePow2) { buf.assign ((size_t) sizePow2, 0.0f); mask = sizePow2 - 1; w = 0; }
    void push (float x) { buf[(size_t) w] = x; w = (w + 1) & mask; }
    float read (float delaySamples) const
    {
        float rp = (float) w - delaySamples;
        while (rp < 0) rp += (float) buf.size();
        const int i0 = (int) rp & mask, i1 = (i0 + 1) & mask;
        const float fr = rp - std::floor (rp);
        return buf[(size_t) i0] + (buf[(size_t) i1] - buf[(size_t) i0]) * fr;
    }
};

struct Chorus
{
    ModDelayLine dl, dr;
    float ph = 0, lpL = 0, lpR = 0, sr = 44100;
    Rng rng;
    void prepare (float sampleRate) { sr = sampleRate; dl.prepare (8192); dr.prepare (8192); }
    void process (float* L, float* R, int n, float rateHz, float depth, float mix)
    {
        if (mix < 0.002f) { for (int i = 0; i < n; ++i) { dl.push (L[i]); dr.push (R[i]); } return; }
        const float inc = rateHz / sr;
        const float lpc = 1.0f - std::exp (-kTwoPi * 7000.0f / sr);
        const float depthMs = 0.3f + depth * 3.2f;
        for (int i = 0; i < n; ++i)
        {
            const float mono = 0.5f * (L[i] + R[i]) + rng.bip() * 0.00002f;
            dl.push (mono); dr.push (mono);
            const float tri = 1.0f - 4.0f * std::abs (ph - 0.5f);
            ph += inc; if (ph >= 1.0f) ph -= 1.0f;
            const float wl = dl.read ((3.5f + depthMs * tri) * 0.001f * sr);
            const float wr = dr.read ((3.5f - depthMs * tri) * 0.001f * sr);
            lpL += (wl - lpL) * lpc; lpR += (wr - lpR) * lpc;
            L[i] = L[i] * (1.0f - mix * 0.5f) + lpL * mix;
            R[i] = R[i] * (1.0f - mix * 0.5f) + lpR * mix;
        }
    }
};

struct Flanger
{
    ModDelayLine dl, dr;
    float ph = 0, fbL = 0, fbR = 0, sr = 44100;
    void prepare (float sampleRate) { sr = sampleRate; dl.prepare (2048); dr.prepare (2048); }
    void process (float* L, float* R, int n, float rateHz, float fb, float mix)
    {
        const float inc = rateHz / sr;
        const float f = std::clamp (fb, 0.0f, 0.95f) * 0.9f;
        for (int i = 0; i < n; ++i)
        {
            const float s = 0.5f + 0.5f * std::sin (kTwoPi * ph);
            const float s2 = 0.5f + 0.5f * std::sin (kTwoPi * (ph + 0.25f));
            ph += inc; if (ph >= 1.0f) ph -= 1.0f;
            dl.push (L[i] + fbL * f); dr.push (R[i] + fbR * f);
            const float wl = dl.read ((0.4f + 4.5f * s) * 0.001f * sr);
            const float wr = dr.read ((0.4f + 4.5f * s2) * 0.001f * sr);
            fbL = wl; fbR = wr;
            L[i] = L[i] * (1.0f - mix * 0.5f) + wl * mix * 0.7f;
            R[i] = R[i] * (1.0f - mix * 0.5f) + wr * mix * 0.7f;
        }
    }
};

struct Delay
{
    ModDelayLine dl, dr;
    float lpL = 0, lpR = 0, sr = 44100, cur = 0;
    void prepare (float sampleRate) { sr = sampleRate; dl.prepare (262144); dr.prepare (262144); cur = 0; }
    void process (float* L, float* R, int n, float seconds, float fb, float mix)
    {
        const float target = std::clamp (seconds * sr, 1.0f, 262000.0f);
        if (cur <= 0) cur = target;
        const float lpc = 1.0f - std::exp (-kTwoPi * 5500.0f / sr);
        for (int i = 0; i < n; ++i)
        {
            cur += (target - cur) * 0.0005f;
            const float wl = dl.read (cur), wr = dr.read (cur);
            lpL += (wl - lpL) * lpc; lpR += (wr - lpR) * lpc;
            // ping-pong: each side feeds the other
            dl.push (L[i] * 0.7f + lpR * fb);
            dr.push (R[i] * 0.7f + lpL * fb);
            L[i] += lpL * mix;
            R[i] += lpR * mix;
        }
    }
};

struct Compressor
{
    float env = 0;
    void process (float* L, float* R, int n, float amount, float speed, float mix, float sr)
    {
        const float thrDb = -amount * 30.0f, ratio = 4.0f;
        const float att = std::exp (-1.0f / (sr * (0.001f + (1.0f - speed) * 0.03f)));
        const float rel = std::exp (-1.0f / (sr * (0.04f + (1.0f - speed) * 0.4f)));
        const float makeup = juce::Decibels::decibelsToGain (amount * 30.0f * (1.0f - 1.0f / ratio) * 0.5f);
        for (int i = 0; i < n; ++i)
        {
            const float x = std::max (std::abs (L[i]), std::abs (R[i]));
            env = x > env ? att * env + (1.0f - att) * x : rel * env + (1.0f - rel) * x;
            const float db = juce::Decibels::gainToDecibels (env + 1.0e-9f);
            const float over = db - thrDb;
            const float gr = over > 0 ? juce::Decibels::decibelsToGain (-over * (1.0f - 1.0f / ratio)) : 1.0f;
            const float g = gr * makeup;
            L[i] = L[i] * (1.0f - mix) + L[i] * g * mix;
            R[i] = R[i] * (1.0f - mix) + R[i] * g * mix;
        }
    }
};

struct StereoTool
{
    float sideLp = 0, l1 = 0, l2 = 0, r1 = 0, r2 = 0;
    void process (float* L, float* R, int n, float width, float monoHz, float lpfHz, float sr)
    {
        const float w = width * 2.0f;
        const float hc = 1.0f - std::exp (-kTwoPi * monoHz / sr);
        const float lc = 1.0f - std::exp (-kTwoPi * std::min (lpfHz, 0.45f * sr) / sr);
        const bool lpf = lpfHz < 19500.0f;
        for (int i = 0; i < n; ++i)
        {
            const float m = 0.5f * (L[i] + R[i]);
            float s = 0.5f * (L[i] - R[i]);
            sideLp += (s - sideLp) * hc;
            s = (s - (monoHz > 25.0f ? sideLp : 0.0f)) * w;
            float a = m + s, b = m - s;
            if (lpf) { l1 += (a - l1) * lc; l2 += (l1 - l2) * lc; r1 += (b - r1) * lc; r2 += (r1 - r2) * lc; a = l2; b = r2; }
            L[i] = a; R[i] = b;
        }
    }
};
} // namespace us
